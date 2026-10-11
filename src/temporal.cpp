// Temporal path: per-eye "streams" that hold history, a luma pyramid and the motion field.
//
// Which history does a texture belong to?
//   - A texture slice range is linked to one stream for its whole life.
//   - A texture seen for the first time continues an existing stream only if exactly one stream
//     of the same size/format already renders to the same place (render-target description +
//     viewport), and that stream was not bound moments ago (that would be the other eye of the
//     same compositor frame). This lets a rotating pool of per-eye textures share one history,
//     while two eyes that render through the same viewport never get mixed: they stay separate.
//   - Anything ambiguous gets its own stream. Worst case is extra memory, never cross-eye ghosting.
//   - "Same size" means the same picture size: a stream works on a texture's content rectangle
//     (ContentProbe), so after a SteamVR resolution change one eye's textures of different sizes,
//     which hold the same picture in a corner, still share one history.
//     When one of an eye's textures takes a new content rectangle, the others whose latest changes
//     show the same picture take it in the same moment (ProbeAlignPool), so a resolution change
//     moves the whole eye to one new history at once.
//   - If an eye's textures end up on different histories anyway (or some on none), all of them are
//     upscaled spatially until that resolves (PoolMixed), so the eye never alternates in quality.
//
// Frame identity is decided on the GPU (fingerprint ring in temporal.hlsl), so re-sampling an
// older or the same app frame never advances the history, and the CPU never waits on the GPU.
#include "internal.h"

#include <math.h>
#include <string.h>
#include <algorithm>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace vu {

const char *const kTPassNames[kTPasses] = { "luma+decide", "pyramid", "search", "lucas-kanade", "global fit",
                                            "refine",      "accumulate", "output",  "copy" };

void PollPassQueries(DevState *ds, ID3D11DeviceContext *ctx, bool wait)
{
    for (auto &q : ds->pq) {
        if (!q.busy) continue;
        const UINT flags = wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        HRESULT hr;
        int spins = 0;
        while ((hr = ctx->GetData(q.dj.Get(), &dj, sizeof(dj), flags)) == S_FALSE && wait && ++spins < 200000) Sleep(0);
        if (hr != S_OK) continue;
        UINT64 t[kTPasses + 1] = {};
        bool ok = !dj.Disjoint && dj.Frequency;
        for (int i = 0; i <= kTPasses && ok; ++i)
            ok = ctx->GetData(q.t[i].Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        for (int i = 0; i < kTPasses && ok; ++i) ok = t[i + 1] >= t[i];
        if (ok) {
            for (int i = 0; i < kTPasses; ++i) ds->passMs[i] += double(t[i + 1] - t[i]) * 1000.0 / double(dj.Frequency);
            ds->passRuns++;
        }
        q.busy = false;
    }
}

namespace {

struct TParams {   // cbuffer TParams in temporal.hlsl
    uint32_t size01[4];
    uint32_t size23[4];
    uint32_t size4out[4];
    float scaleInfo[4];
    uint32_t rcasCon[4];
    float tune0[4];
    float tune1[4];
    uint32_t misc[4];
    float debugFlow[4];
    float tune2[4];
    uint32_t region[4];      // output region with history: x0, y0, w, h
    uint32_t lkCells[4];     // L3 cells: first x, y, count x, y (strided)
    uint32_t refCells[4];    // L2 cells: first x, y, count x, y
    uint32_t searchBox[4];   // L4 pixels: x0, y0, w, h
    uint32_t strides[4];     // LK cell step, global-fit step (in LK cells), 0, 0
    uint32_t srcRect[4];     // content origin in the source x, y | source texture w, h
    uint32_t dstRect[4];     // content origin in the proxy x, y | proxy w, h
    uint32_t marks[4];       // gaze ring x, y | calibration cross x, y (content output px; ~0u = none)
    float tune3[4];          // localCheck (bits), localAgree (input px), localAperture, 0
    uint32_t work[4];        // L4 pixels the pyramid is built over (x0, y0, x1, y1): the region and a margin
};
static_assert(sizeof(TParams) == 320, "cbuffer layout must match temporal.hlsl");
} // namespace
const UINT kTParamsBytes = sizeof(TParams);
namespace {

float g_debugFlow[3] = {};   // test hook (VrUpscaleDebugForceFlow)

PassQueries *AcquirePassQueries(DevState *ds, ID3D11DeviceContext *ctx)
{
    PollPassQueries(ds, ctx, false);
    for (auto &q : ds->pq) {
        if (q.busy) continue;
        if (!q.dj) {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
            bool ok = SUCCEEDED(ds->dev->CreateQuery(&qd, q.dj.Put()));
            for (auto &t : q.t) ok = ok && SUCCEEDED(ds->dev->CreateQuery(&qt, t.Put()));
            if (!ok) { q.dj.Reset(); return nullptr; }
        }
        return &q;
    }
    return nullptr;
}

enum { A_DOWN2, A_DOWN3, A_DOWN4, A_SEARCH, A_LK, A_ACC01, A_ACC10, A_OUT1, A_OUT0, A_COPY, A_GLOBAL, A_REFINE, A_OUTER,
       A_LUMA, A_MARGIN, A_COUNT };
constexpr UINT TF_TINT = 1, TF_FORCE = 2, TF_RESET = 4, TF_PROBE = 8, TF_PRIME = 16, TF_BILINEAR = 32, TF_MARGIN = 64;
constexpr UINT kStateBytes = 64;

struct Tex {
    Ref<ID3D11Texture2D> tex;
    Ref<ID3D11ShaderResourceView> srv;
    Ref<ID3D11UnorderedAccessView> uav;
    void Drop()
    {
        DeferRelease(uav.Detach());
        DeferRelease(srv.Detach());
        DeferRelease(tex.Detach());
    }
};

struct Buf {
    Ref<ID3D11Buffer> buf;
    Ref<ID3D11ShaderResourceView> srv;
    Ref<ID3D11UnorderedAccessView> uav;
    void Drop()
    {
        DeferRelease(uav.Detach());
        DeferRelease(srv.Detach());
        DeferRelease(buf.Detach());
    }
};

volatile LONG g_streamIds = 0;
volatile LONG g_streamsFreed = 0;   // bumped when a stream loses its last texture (a slot under maxStreams opens)
volatile LONG g_resetGen = 1;
volatile LONG g_capLogged = 0;

} // namespace

class Stream final : public RefObj {
public:
    Stream()
    {
        id = (int)InterlockedIncrement(&g_streamIds);
        InterlockedIncrement(&g_stats.liveStreams);
    }
    int id = 0;
    ID3D11Device *dev = nullptr;   // comparison only (our resources keep the device alive)
    LONG cfgGen = 0;
    LONG resetGen = 0;
    UINT w = 0, h = 0, slices = 0, outW = 0, outH = 0;   // the picture (content rectangle) and its output size
    double scaleX = 1, scaleY = 1;
    int xform = kRaw;
    UINT lw[5] = {}, lh[5] = {};
    // Foveation: the output region with history (the rest is upscaled spatially) and the band
    // inside its edge over which the two are blended. Its size is fixed for the stream's life; its
    // position is centred, or follows the gaze (the GPU keeps track of where the history it holds
    // was made, see DECIDE).
    UINT rx = 0, ry = 0, rw = 0, rh = 0, band = 0;
    bool foveated = false;
    int side = -1;                 // which eye it shows (StreamSide), as of its last run
    OutSig sig;
    Tex cur[5], prev[5], pack[4], flow4, flowLoc, flow2, flowPrev, hist[2];   // cur/prev [0] unused
    Buf fp, state, ctrl, args, model;
    Ref<ID3D11Buffer> staging, debugStaging;
    bool stagingPending = false;
    bool haveCounters = false;                       // gpuNew.. valid (TemporalPollCounters)
    uint32_t gpuNew = 0, gpuCalls = 0, gpuForced = 0;   // GPU totals: new app frames, checks, forced repaints
    bool haveBaseline = false;
    uint32_t baseNew = 0, baseCalls = 0, baseForced = 0;
    uint64_t lastTexSerial = 0, lastBindSerial = 0;
    LONGLONG lastRunQpc = 0, lastBindQpc = 0;
    volatile LONG textures = 0;
    uint64_t runs = 0, runsInterval = 0;
    bool needReset = true;
    bool ok = false;
    size_t bytes = 0;

protected:
    ~Stream() override
    {
        for (auto &t : cur) t.Drop();
        for (auto &t : prev) t.Drop();
        for (auto &t : pack) t.Drop();
        flow4.Drop();
        flowLoc.Drop();
        flow2.Drop();
        flowPrev.Drop();
        model.Drop();
        hist[0].Drop();
        hist[1].Drop();
        fp.Drop();
        state.Drop();
        ctrl.Drop();
        args.Drop();
        DeferRelease(staging.Detach());
        DeferRelease(debugStaging.Detach());
        InterlockedDecrement(&g_stats.liveStreams);
    }
};

namespace {

// Guards the stream list (a second D3D11 device could draw on another thread).
SRWLOCK g_streamLock = SRWLOCK_INIT;

// Heap-allocated and never freed (hooks may run during process teardown). Guarded by g_streamLock.
std::vector<Stream *> &Streams()
{
    static std::vector<Stream *> *v = new std::vector<Stream *>();
    return *v;
}

bool CreateTex(ID3D11Device *dev, Tex &t, UINT w, UINT h, UINT n, DXGI_FORMAT f, UINT bpp, size_t &bytes, bool zero = false)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = n;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    // zero: textures the passes write only in part but read further out (D3D11 leaves new contents undefined)
    std::vector<uint8_t> zeros;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    if (zero) {
        zeros.assign((size_t)w * h * bpp, 0);
        init.assign(n, D3D11_SUBRESOURCE_DATA{ zeros.data(), w * bpp, w * h * bpp });
    }
    if (FAILED(dev->CreateTexture2D(&d, zero ? init.data() : nullptr, t.tex.Put()))) return false;
    UINT32 ours = 1;
    t.tex->SetPrivateData(kGuidOurs, sizeof(ours), &ours);
    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = f;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    s.Texture2DArray.MipLevels = 1;
    s.Texture2DArray.ArraySize = n;
    if (FAILED(dev->CreateShaderResourceView(t.tex.Get(), &s, t.srv.Put()))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = f;
    u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    u.Texture2DArray.ArraySize = n;
    if (FAILED(dev->CreateUnorderedAccessView(t.tex.Get(), &u, t.uav.Put()))) return false;
    bytes += (size_t)w * h * n * bpp;
    return true;
}

bool CreateRaw(ID3D11Device *dev, Buf &b, UINT size, bool srv, bool indirect, uint8_t fill = 0)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | (indirect ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0);
    std::vector<uint8_t> zeros(size, fill);
    D3D11_SUBRESOURCE_DATA init = { zeros.data(), 0, 0 };
    if (FAILED(dev->CreateBuffer(&d, &init, b.buf.Put()))) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = DXGI_FORMAT_R32_TYPELESS;
        s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        s.BufferEx.NumElements = size / 4;
        s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(dev->CreateShaderResourceView(b.buf.Get(), &s, b.srv.Put()))) return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = size / 4;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(dev->CreateUnorderedAccessView(b.buf.Get(), &u, b.uav.Put()));
}

bool CreateStaging(ID3D11Device *dev, Ref<ID3D11Buffer> &out, UINT bytes = kStateBytes)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(dev->CreateBuffer(&d, nullptr, out.Put()));
}

// Can a texture whose picture is at geo continue stream s? The picture must be the same size: to
// within a few pixels when it is a measured part of the texture (the measurement is only that
// exact), and s's rectangle must fit in the texture at the corner the picture is anchored to.
// *use = the geometry this texture then has on s.
bool Compatible(const Stream *s, const DevState *ds, const TexState *ts, UINT count, const LinkGeo &geo, LinkGeo *use)
{
    if (!(s->ok && s->dev == ds->dev.Get() && s->cfgGen == g_cfgGen && s->slices == count && s->xform == ts->plan.xform))
        return false;
    if (s->w == geo.cw && s->h == geo.ch && s->outW == geo.outW && s->outH == geo.outH) {
        if (use) *use = geo;
        return true;
    }
    if (s->scaleX != ts->plan.scaleX || s->scaleY != ts->plan.scaleY) return false;
    auto off = [](UINT a, UINT b) { return a > b ? a - b : b - a; };
    const UINT tw = ts->src.Width, th = ts->src.Height;
    if (off(s->w, geo.cw) > 24 || off(s->h, geo.ch) > 24 || s->w > tw + 8 || s->h > th + 8) return false;
    LinkGeo g = geo;
    if (geo.sx > 0) g.sx = tw > s->w ? tw - s->w : 0;   // anchored right / bottom
    if (geo.sy > 0) g.sy = th > s->h ? th - s->h : 0;
    g.cw = s->w;
    g.ch = s->h;
    g.outW = s->outW;
    g.outH = s->outH;
    g.ox = (UINT)std::min(floor(g.sx * s->scaleX + 0.5), (double)ts->plan.outW);
    g.oy = (UINT)std::min(floor(g.sy * s->scaleY + 0.5), (double)ts->plan.outH);
    g.full = g.sx == 0 && g.sy == 0 && g.cw == tw && g.ch == th && g.outW == ts->plan.outW && g.outH == ts->plan.outH;
    if (use) *use = g;
    return true;
}

// Streams in use (one only the list still holds is freed at the next sweep).
int CurrentStreams()
{
    int n = 0;
    for (Stream *s : Streams())
        if (s->cfgGen == g_cfgGen && s->RefCount() > 1) ++n;
    return n;
}

// The centred output rectangle that keeps history: temporalArea of the width and height, in whole
// 16-pixel accumulation tiles; nearly everything becomes everything (no thin spatial rim).
void PlanRegion(Stream *s, float area)
{
    auto side = [&](UINT out) -> UINT {
        if (area >= 0.999f) return out;
        UINT r = ((UINT)ceilf((float)out * area / 16.0f)) * 16;
        return r + 32 >= out ? out : r;
    };
    s->rw = side(s->outW);
    s->rh = side(s->outH);
    s->rx = (s->outW - s->rw) / 2;
    s->ry = (s->outH - s->rh) / 2;
    s->foveated = s->rw < s->outW || s->rh < s->outH;
    s->band = s->foveated ? std::max(16u, (UINT)lroundf(0.02f * (float)std::min(s->outW, s->outH))) : 0u;
}

// Returns a stream holding two references (one for the list, one for the caller's link).
Stream *CreateStream(DevState *ds, const TexState *ts, UINT count, const OutSig &sig, const LinkGeo &geo)
{
    if (CurrentStreams() >= g_cfg.maxStreams) {
        if (InterlockedExchange(&g_capLogged, 1) == 0)
            LOGI("temporal: maxStreams (%d) reached; further textures use FSR 1. Raise [temporal] maxStreams "
                 "if the log shows more than two eyes' worth of textures.", g_cfg.maxStreams);
        return nullptr;
    }
    ID3D11Device *dev = ds->dev.Get();
    Stream *s = new Stream();
    s->dev = dev;
    s->cfgGen = g_cfgGen;
    s->w = geo.cw;
    s->h = geo.ch;
    s->slices = count;
    s->outW = geo.outW;
    s->outH = geo.outH;
    s->scaleX = ts->plan.scaleX;
    s->scaleY = ts->plan.scaleY;
    s->xform = ts->plan.xform;
    s->sig = sig;
    PlanRegion(s, g_cfg.temporalArea);
    s->lw[0] = s->w;
    s->lh[0] = s->h;
    for (int l = 1; l < 5; ++l) {
        s->lw[l] = (s->lw[l - 1] + 1) / 2;
        s->lh[l] = (s->lh[l - 1] + 1) / 2;
    }
    size_t &b = s->bytes;
    bool ok = true;
    for (int l = 1; l < 5 && ok; ++l)   // no full-resolution level: LUMA writes L1 directly
        ok = CreateTex(dev, s->cur[l], s->lw[l], s->lh[l], count, DXGI_FORMAT_R16_UNORM, 2, b) &&
             CreateTex(dev, s->prev[l], s->lw[l], s->lh[l], count, DXGI_FORMAT_R16_UNORM, 2, b);
    for (int l = 1; l <= 3 && ok; ++l)
        ok = CreateTex(dev, s->pack[l], s->lw[l], s->lh[l], count, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, b);
    ok = ok && CreateTex(dev, s->flow4, s->lw[4], s->lh[4], count, DXGI_FORMAT_R16G16_FLOAT, 4, b) &&
         CreateTex(dev, s->flowLoc, s->lw[3], s->lh[3], count, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, b) &&
         CreateTex(dev, s->flow2, s->lw[2], s->lh[2], count, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, b, true) &&
         CreateTex(dev, s->flowPrev, s->lw[2], s->lh[2], count, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, b, true) &&
         CreateRaw(dev, s->model, 64 * count, true, false) &&
         CreateTex(dev, s->hist[0], s->rw, s->rh, count, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, b) &&
         CreateTex(dev, s->hist[1], s->rw, s->rh, count, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, b) &&
         CreateRaw(dev, s->fp, 16, true, false) && CreateRaw(dev, s->state, kStateBytes, false, false) &&
         CreateRaw(dev, s->ctrl, 16, true, false) && CreateRaw(dev, s->args, A_COUNT * 12, false, true) &&
         CreateStaging(dev, s->staging) && CreateStaging(dev, s->debugStaging);
    if (!ok) {
        LOGE("temporal: could not allocate a %ux%u x%u -> %ux%u history (out of video memory?); using FSR 1",
             s->w, s->h, count, s->outW, s->outH);
        s->Release();
        return nullptr;
    }
    s->ok = true;
    s->AddRef();   // list reference
    Streams().push_back(s);
    char fov[96] = "history covers the whole image";
    if (s->foveated)
        snprintf(fov, sizeof(fov), "history in the centre %ux%u at (%u,%u), blend band %u px", s->rw, s->rh, s->rx, s->ry, s->band);
    char part[96] = "";
    if (!geo.full)
        snprintf(part, sizeof(part), " (the part at %u,%u of a %ux%u texture)", geo.sx, geo.sy, ts->src.Width, ts->src.Height);
    LOGI("temporal: stream #%d created for %ux%u%s x%u (%s) -> %ux%u, %s, %.0f MB, output rt %ux%u vp (%d,%d %dx%d)%s", s->id,
         s->w, s->h, part, count, XformName(s->xform), s->outW, s->outH, fov, s->bytes / (1024.0 * 1024.0), sig.rtW, sig.rtH,
         sig.vx, sig.vy, sig.vw, sig.vh, sig.valid ? "" : " (no render target)");
    return s;
}

void ReadCounters(const uint32_t *st, uint32_t &newFrames, uint32_t &calls, uint32_t &forced)
{
    newFrames = st[12];
    calls = st[13];
    forced = st[14];
}

} // namespace

OutSig CaptureOutSig(ID3D11DeviceContext *ctx)
{
    OutSig sig;
    ID3D11RenderTargetView *rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        Ref<ID3D11Resource> r;
        rtv->GetResource(r.Put());
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (r) r->GetType(&dim);
        Ref<ID3D11Texture2D> t;
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D &&
            SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)t.Put()))) {
            D3D11_TEXTURE2D_DESC d;
            t->GetDesc(&d);
            sig.rtW = d.Width;
            sig.rtH = d.Height;
            sig.rtFmt = d.Format;
        }
        rtv->Release();
    }
    UINT n = 1;
    D3D11_VIEWPORT vp = {};
    ctx->RSGetViewports(&n, &vp);
    if (n >= 1) {
        sig.vx = (int)vp.TopLeftX;
        sig.vy = (int)vp.TopLeftY;
        sig.vw = (int)vp.Width;
        sig.vh = (int)vp.Height;
    }
    sig.valid = sig.rtW > 0;
    return sig;
}

StreamLink *FindLink(TexState *ts, UINT first, UINT count)
{
    for (int i = 0; i < ts->nlinks; ++i)
        if (ts->links[i].first == first && ts->links[i].count == count) return &ts->links[i];
    return nullptr;
}

namespace {

bool CreateLinkBuffer(ID3D11Device *dev, StreamLink &l)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = 16;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const uint32_t zeros[4] = {};
    D3D11_SUBRESOURCE_DATA init = { zeros, 0, 0 };
    if (FAILED(dev->CreateBuffer(&d, &init, &l.linkBuf))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = 4;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(l.linkBuf, &u, &l.linkUAV))) {
        l.linkBuf->Release();
        l.linkBuf = nullptr;
        return false;
    }
    return true;
}

// ---- content rectangles -------------------------------------------------------------------------

// Probes on the render thread's list (references held), guarded by g_workLock.
std::vector<ContentProbe *> &Probes()
{
    static std::vector<ContentProbe *> *v = new std::vector<ContentProbe *>();
    return *v;
}

volatile LONG g_probeFailLogged = 0;
volatile LONG g_contentLogBudget = 64;

ContentProbe *EnsureProbe(DevState *ds, TexState *ts)
{
    if (ts->probe) return ts->probe->mem ? ts->probe.Get() : nullptr;   // a failed allocation stays failed
    if (!g_cfg.detectContent || ts->gen != g_cfgGen) return nullptr;
    Ref<ContentProbe> p(new ContentProbe(), false);
    p->dev = ds->dev.Get();
    p->texW = ts->src.Width;
    p->texH = ts->src.Height;
    p->slices = ts->src.ArraySize;
    p->rw = p->texW;
    p->rh = p->texH;
    ts->probe = p;
    const UINT blocks = ((p->texW + 7) / 8) * ((p->texH + 7) / 8) * p->slices;
    Buf mem, box;
    Ref<ID3D11Buffer> staging;
    if (!CreateRaw(ds->dev.Get(), mem, blocks * 4, false, false) || !CreateRaw(ds->dev.Get(), box, 16, false, false, 0xFF) ||
        !CreateStaging(ds->dev.Get(), staging, 16)) {
        if (InterlockedExchange(&g_probeFailLogged, 1) == 0)
            LOGE("content: could not allocate change detection for a %ux%u texture; its render size stays unknown", p->texW, p->texH);
        return nullptr;
    }
    p->mem = mem.buf;
    p->memUAV = mem.uav;
    p->box = box.buf;
    p->boxUAV = box.uav;
    p->staging = staging;
    p->AddRef();   // the list's reference
    Probes().push_back(p.Get());
    return p.Get();
}

// Until then (QPC), after the app's notice of a resolution change: short windows, short memory.
LONGLONG g_probeFastUntil = 0;   // guarded by g_workLock

typedef ContentProbe::Box Box;

bool BoxDiffers(const Box &a, const Box &c, UINT tol)
{
    auto off = [](UINT x, UINT y) { return x > y ? x - y : y - x; };
    return off(a.x0, c.x0) > tol || off(a.y0, c.y0) > tol || off(a.x1, c.x1) > tol || off(a.y1, c.y1) > tol;
}

Box ProbeRect(const ContentProbe *p) { return { p->rx, p->ry, p->rx + p->rw, p->ry + p->rh }; }

// Another texture of the same eye, drawn there lately.
bool PoolPeer(const ContentProbe *p, const ContentProbe *q, LONGLONG now)
{
    return q != p && !q->dead && q->dev == p->dev && p->pool >= 0 && q->pool == p->pool && q->poolEpoch == p->poolEpoch &&
           now - q->drawQpc < g_qpf.QuadPart;
}

// Does q's own latest evidence show a picture of r's size? Its latest changes lie in r and reach
// r's far edges. (The game renders into the top-left corner; r must fit the texture.)
bool ProbeFits(const ContentProbe *q, const Box &r, LONGLONG now)
{
    if (r.x0 != 0 || r.y0 != 0 || r.x1 > q->texW || r.y1 > q->texH) return false;
    if (q->newestQpc == 0 || now - q->newestQpc > 2 * g_qpf.QuadPart) return false;   // nothing seen lately
    const Box &n = q->newest;
    return n.x0 < 24 && n.y0 < 24 && n.x1 <= r.x1 + 8 && n.y1 <= r.y1 + 8 && n.x1 + 24 >= r.x1 && n.y1 + 24 >= r.y1;
}

void ProbeAccept(ContentProbe *p, const Box &r, LONGLONG now, const char *how)
{
    const UINT W = p->texW, H = p->texH;
    p->rx = r.x0;
    p->ry = r.y0;
    p->rw = r.x1 - r.x0;
    p->rh = r.y1 - r.y0;
    p->sub = p->rw < W || p->rh < H;
    p->gen++;
    p->candN = 0;
    p->acceptQpc = now;
    // windows from before the change (larger than the rectangle now) no longer count
    for (int k = 0; k < 8; ++k) {
        Box &e = p->ring[k];
        if (e.x1 != 0 && (e.x1 > r.x1 + 8 || e.y1 > r.y1 + 8 || e.x0 + 8 < r.x0 || e.y0 + 8 < r.y0)) e = Box{};
    }
    if (InterlockedDecrement(&g_contentLogBudget) >= 0) {
        if (p->sub)
            LOGI("content: a %ux%u texture holds a %ux%u picture at (%u,%u) (the app renders into part of it)%s", W, H, p->rw,
                 p->rh, p->rx, p->ry, how);
        else
            LOGI("content: a %ux%u texture is used whole%s", W, H, how);
    }
}

// p just took a new rectangle: the other textures of its eye whose latest changes show the same
// picture take it too, in the same moment, so the eye's textures never sit on two histories.
void ProbeAlignPool(ContentProbe *p, LONGLONG now)
{
    if (p->pool < 0) return;
    const Box r = ProbeRect(p);
    for (ContentProbe *q : Probes())
        if (PoolPeer(p, q, now) && BoxDiffers(r, ProbeRect(q), 0) && ProbeFits(q, r, now))
            ProbeAccept(q, r, now, " (with the other textures of its eye)");
}

// p's latest window fits the rectangle another texture of its eye took last: take it as well (a
// texture that saw the change later, or a new one).
bool ProbeJoinPool(ContentProbe *p, LONGLONG now)
{
    if (p->pool < 0) return false;
    const ContentProbe *best = nullptr;
    for (const ContentProbe *q : Probes())
        if (PoolPeer(p, q, now) && q->acceptQpc != 0 && (!best || q->acceptQpc > best->acceptQpc)) best = q;
    if (!best) return false;
    const Box r = ProbeRect(best);
    if (!BoxDiffers(r, ProbeRect(p), 0) || !ProbeFits(p, r, now)) return false;
    ProbeAccept(p, r, now, " (like the other textures of its eye)");
    return true;
}

// One observation window is over: b = min x, min y, ~max x, ~max y of the samples that changed
// (all ones: none). Updates the accepted rectangle when the evidence is clear.
void ProbeWindow(ContentProbe *p, const uint32_t *b, LONGLONG now)
{
    const UINT W = p->texW, H = p->texH;
    Box w = {};
    if (b[0] != ~0u && b[0] < W && b[1] < H) {
        // the outermost changed samples: each block samples one pixel at its own spot, so along an
        // edge of the picture (hundreds of blocks) some block samples its last row or column
        w.x0 = b[0];
        w.y0 = b[1];
        w.x1 = std::min(W, (~b[2]) + 1);
        w.y1 = std::min(H, (~b[3]) + 1);
        p->active++;
        p->newest = w;
        p->newestQpc = now;
    }
    p->ring[p->ringHead] = w;
    p->ringHead = (p->ringHead + 1) % 8;
    if (p->ringN < 8) p->ringN++;
    if (w.x1 != 0 && ProbeJoinPool(p, now)) return;
    // the newest windows with changes: all eight kept, or after a resolution change the last two
    // (what came before the change says nothing about the picture now)
    const int keep = now < g_probeFastUntil ? 2 : 8;
    Box u = { ~0u, ~0u, 0, 0 };
    int n = 0;
    for (int i = 0; i < p->ringN && n < keep; ++i) {
        const Box &r = p->ring[(p->ringHead + 7 - i) % 8];
        if (r.x1 == 0) continue;
        u.x0 = std::min(u.x0, r.x0);
        u.y0 = std::min(u.y0, r.y0);
        u.x1 = std::max(u.x1, r.x1);
        u.y1 = std::max(u.y1, r.y1);
        ++n;
    }
    if (n == 0 || p->active < 3) return;   // too little to go on (yet)
    if (u.x0 < 8) u.x0 = 0;                // within a block of an edge reaches it
    if (u.y0 < 8) u.y0 = 0;
    if (u.x1 + 8 > W) u.x1 = W;
    if (u.y1 + 8 > H) u.y1 = H;
    // A viewport scaled down from one corner keeps the texture's shape. Changes of any other shape
    // (say a still scene where only an animated part changes) say nothing about the rectangle,
    // unless they reach outside it: then the whole texture, to be safe.
    auto differs = BoxDiffers;
    const Box cur = ProbeRect(p);
    const UINT cw = u.x1 - u.x0, ch = u.y1 - u.y0;
    const bool corner = (u.x0 == 0 || u.x1 == W) && (u.y0 == 0 || u.y1 == H);
    const double shape = ((double)cw / (double)ch) / ((double)W / (double)H);
    Box r = u;
    if (!(corner && cw >= W / 4 && ch >= H / 4 && fabs(shape - 1.0) < 0.03)) {
        const bool outside = u.x0 + 16 < cur.x0 || u.y0 + 16 < cur.y0 || u.x1 > cur.x1 + 16 || u.y1 > cur.y1 + 16;
        if (!outside) {
            p->candN = 0;
            return;
        }
        r = { 0, 0, W, H };
    }
    if (!differs(r, cur, 16)) {
        p->candN = 0;
        return;
    }
    // The game drawing outside the rectangle is taken at once; a smaller one must hold for two
    // windows (it only shows once the older windows have left the ring).
    const bool grows = r.x0 < cur.x0 || r.y0 < cur.y0 || r.x1 > cur.x1 || r.y1 > cur.y1;
    if (p->candN > 0 && !differs(r, p->cand, 8)) p->candN++;
    else {
        p->cand = r;
        p->candN = 1;
    }
    if (!grows && p->candN < 2) return;
    // within a few pixels of what another texture of the eye renders: the same picture (the
    // changed samples' extent varies by a pixel or two between textures), so exactly the same
    // rectangle, or the two would be on different histories
    if (p->pool >= 0)
        for (const ContentProbe *q : Probes()) {
            const Box qr = ProbeRect(q);
            if (PoolPeer(p, q, now) && qr.x0 == 0 && qr.y0 == 0 && qr.x1 <= W && qr.y1 <= H && !differs(r, qr, 8)) {
                r = qr;
                break;
            }
        }
    if (!differs(r, cur, 0)) {
        p->candN = 0;
        return;
    }
    ProbeAccept(p, r, now, "");
    ProbeAlignPool(p, now);
}

// ---- eye pools -------------------------------------------------------------------------------

// The last draws into one eye viewport. What matters is whether textures on different histories
// take turns there (the compositor shows one, then the other): one texture simply replacing
// another (a new swapchain, a re-plan) is not that.
struct PoolDraw {
    uint64_t serial;
    int streamId;          // 0 = spatial path
    DXGI_FORMAT fmt;
    UINT texW, texH;
    LONGLONG qpc;
};
struct Pool {
    OutSig sig;
    PoolDraw d[16];        // ring, oldest first from head
    int head = 0, n = 0;
    LONGLONG lastQpc = 0, mixedSince = 0, holdUntil = 0;
    LONGLONG relinkQpc = 0;   // a texture of this eye last left its history for a new content rectangle
    uint32_t epoch = 0;       // tells a slot's pools apart when it is reused for another viewport
};
Pool g_pools[4];   // guarded by g_workLock
int g_poolN = 0;
uint32_t g_poolEpochs = 0;

bool SameShape(UINT w0, UINT h0, UINT w1, UINT h1)
{
    const double a = ((double)w0 / (double)h0) / ((double)w1 / (double)h1);
    return fabs(a - 1.0) < 0.03;
}

bool PoolHeld(int pool, LONGLONG now) { return pool >= 0 && pool < g_poolN && g_pools[pool].holdUntil > now; }

// Interleaved: among the recent draws of textures like (fmt, w x h), a history comes back after
// another one was drawn (X .. Y .. X). Each draw counts with the history its texture is on now (its
// latest draw), so once an eye's textures have all moved to a new history the eye is no longer
// mixed, however their earlier draws went; and a texture not drawn since one of the eye's textures
// moved to a new rectangle does not count yet (it moves too when it is drawn next, if it shows the
// same picture: its rectangle changed in the same moment).
bool PoolInterleaved(const Pool &P, DXGI_FORMAT fmt, UINT w, UINT h, LONGLONG now)
{
    int prev = -1, closed[16], nclosed = 0;
    for (int i = 0; i < P.n; ++i) {
        const PoolDraw &d = P.d[(P.head + i) % 16];
        if (now - d.qpc > g_qpf.QuadPart || d.fmt != fmt || !SameShape(d.texW, d.texH, w, h)) continue;
        int id = d.streamId;
        LONGLONG latest = d.qpc;
        for (int j = i + 1; j < P.n; ++j) {
            const PoolDraw &e = P.d[(P.head + j) % 16];
            if (e.serial == d.serial) {
                id = e.streamId;
                latest = e.qpc;
            }
        }
        if (latest < P.relinkQpc) continue;
        if (prev >= 0 && id != prev && nclosed < 16) closed[nclosed++] = prev;
        for (int c = 0; c < nclosed; ++c)
            if (closed[c] == id) return true;
        prev = id;
    }
    return false;
}

volatile LONG g_pausedDraws = 0;   // draws sent to the spatial path because their eye was mixed (tests)

// Picks an existing stream to continue (rotating pool) or creates one. Caller holds g_streamLock.
Stream *AttachStream(DevState *ds, ID3D11DeviceContext *ctx, TexState *ts, ViewEntry *ve, LONGLONG now, LinkGeo &geo)
{
    OutSig sig = CaptureOutSig(ctx);
    Stream *s = nullptr;
    // When each eye has its own viewport in a larger render target, the viewport says which eye a
    // texture is, so textures drawn back to back there (the compositor draws every texture of
    // the app's swapchain each frame) are the same eye. Otherwise two textures bound moments
    // apart may be the two eyes, and only a later texture can continue a stream.
    const bool eyeByViewport = sig.valid && (sig.vx != 0 || sig.vy != 0 || sig.vw < (int)sig.rtW || sig.vh < (int)sig.rtH);
    if (g_cfg.linkPools && sig.valid) {
        Stream *cand = nullptr;
        LinkGeo candGeo;
        int same = 0;
        for (Stream *x : Streams()) {
            LinkGeo g;
            if (x->RefCount() <= 1 || !(x->sig == sig) || !Compatible(x, ds, ts, ve->count, geo, &g)) continue;
            ++same;
            // several (an eye viewport after textures moved between histories): the busiest, then the latest
            if (!cand || x->textures > cand->textures || (x->textures == cand->textures && x->lastBindQpc > cand->lastBindQpc)) {
                cand = x;
                candGeo = g;
            }
        }
        const LONGLONG gap = (LONGLONG)(g_cfg.linkGapMs * (double)g_qpf.QuadPart / 1000.0);
        // (in an eye's own viewport this texture can't be the other eye, whatever was bound last)
        if (cand && (eyeByViewport ? same >= 1 : (same == 1 && cand->lastBindSerial != ts->serial && now - cand->lastBindQpc > gap))) {
            s = cand;
            s->AddRef();
            geo = candGeo;
            LOGI("temporal: texture #%llu (%ux%u, slices %u..%u) continues stream #%d: same eye (%s)", (unsigned long long)ts->serial,
                 ts->src.Width, ts->src.Height, ve->first, ve->first + ve->count - 1, s->id,
                 eyeByViewport ? "same viewport): another texture of the app's swapchain" : "next texture of a rotating pool)");
        } else if (same > 1) {
            LOGV("temporal: %d streams render to the same place; texture #%llu gets its own history",
                 same, (unsigned long long)ts->serial);
        }
    }
    if (!s) s = CreateStream(ds, ts, ve->count, sig, geo);
    if (s) InterlockedIncrement(&s->textures);
    return s;
}

volatile LONG g_relinkLogBudget = 64;

} // namespace

StreamLink *ResolveStream(DevState *ds, ID3D11DeviceContext *ctx, ViewEntry *ve, LONGLONG now)
{
    TexState *ts = ve->tex.Get();
    const LONG gen = ts->probe ? ts->probe->gen : 0;
    if (StreamLink *l = FindLink(ts, ve->first, ve->count)) {
        const bool hold = PoolHeld(l->pool, now);
        if (l->stream && (hold || gen != l->geoGen)) {
            // The content rectangle moved (the game renders into another part of the texture), or
            // this eye is held on the spatial path: leave the history.
            if (!hold && InterlockedDecrement(&g_relinkLogBudget) >= 0)
                LOGI("temporal: texture #%llu now renders %ux%u at (%u,%u); leaving stream #%d", (unsigned long long)ts->serial,
                     ts->probe->rw, ts->probe->rh, ts->probe->rx, ts->probe->ry, l->stream->id);
            if (!hold && l->pool >= 0 && l->pool < g_poolN) g_pools[l->pool].relinkQpc = now;
            AcquireSRWLockExclusive(&g_streamLock);
            ReleaseStreamRef(l->stream);
            l->stream = nullptr;
            ReleaseSRWLockExclusive(&g_streamLock);
            l->retryQpc = hold ? now : 0;
            l->written = false;
        } else if (!l->stream && gen != l->geoGen) {
            l->retryQpc = 0;   // on the spatial path: try again at once with the new rectangle
        }
        // A range on the spatial path tries for a history again now and then (an allocation may
        // succeed now), at once after a content change, and when another history was let go.
        if (!l->stream && !hold && g_cfg.maxStreams > 0 && l->linkUAV &&
            (l->retryQpc == 0 || now - l->retryQpc > 2 * g_qpf.QuadPart || l->freedSeen != g_streamsFreed)) {
            l->retryQpc = now;
            l->freedSeen = g_streamsFreed;
            l->geo = ContentGeo(ts);
            l->geoGen = gen;
            AcquireSRWLockExclusive(&g_streamLock);
            l->stream = AttachStream(ds, ctx, ts, ve, now, l->geo);
            l->written = false;
            ReleaseSRWLockExclusive(&g_streamLock);
        }
        return l;
    }
    if (ts->nlinks >= 2) return nullptr;

    StreamLink &l = ts->links[ts->nlinks++];
    l.first = ve->first;
    l.count = ve->count;
    l.written = false;
    l.retryQpc = now;
    l.stream = nullptr;
    l.pool = -1;
    l.geo = ContentGeo(ts);
    l.geoGen = gen;
    l.freedSeen = g_streamsFreed;
    if (g_cfg.maxStreams > 0 && CreateLinkBuffer(ds->dev.Get(), l)) {
        AcquireSRWLockExclusive(&g_streamLock);
        l.stream = AttachStream(ds, ctx, ts, ve, now, l.geo);   // null: this slice range stays on the spatial path
        ReleaseSRWLockExclusive(&g_streamLock);
    }
    return &l;
}

void ReleaseLink(StreamLink &l)
{
    if (l.stream) ReleaseStreamRef(l.stream);
    l.stream = nullptr;
    DeferRelease(l.linkUAV);
    DeferRelease(l.linkBuf);
    l.linkUAV = nullptr;
    l.linkBuf = nullptr;
}

void ReleaseStreamRef(Stream *s)
{
    if (InterlockedDecrement(&s->textures) == 0) InterlockedIncrement(&g_streamsFreed);
    s->Release();
}

bool StreamWantsRun(StreamLink *link, TexState *ts, LONGLONG now, LONGLONG dedupeTicks)
{
    Stream *s = link->stream;
    if (!link->written) return true;
    return !(s->lastTexSerial == ts->serial && s->lastRunQpc != 0 && now - s->lastRunQpc < dedupeTicks);
}

void StreamTouch(StreamLink *link, TexState *ts, LONGLONG now)
{
    link->stream->lastBindQpc = now;
    link->stream->lastBindSerial = ts->serial;
}

namespace {

// Which eye a stream shows: the viewport it is drawn into (left or right half of the compositor's
// target), else the slice of a two-slice texture; -1 when neither says.
int StreamSide(const Stream *s, const StreamLink *link, const TexState *ts)
{
    const OutSig &g = s->sig;
    if (g.valid && g.rtW > 0 && g.vw > 0 && (UINT)g.vw < g.rtW) return (g.vx + g.vw / 2) < (int)g.rtW / 2 ? 0 : 1;
    if (link->count == 1 && ts->src.ArraySize == 2) return (int)link->first;
    return -1;
}

// Where the region with history is this frame: centred, or around the gaze. It moves only once
// the gaze leaves a small zone around its centre (fixational eye movements leave it alone; every
// move costs the history in the strip it uncovers). Also places the gaze ring and the calibration
// cross for the output passes.
void PlaceRegion(Stream *s, int side, TParams &p)
{
    p.marks[0] = p.marks[1] = p.marks[2] = p.marks[3] = ~0u;
    float u = 0, v = 0;
    const bool live = GazePoint(side, &u, &v);
    if (s->foveated) {
        if (!g_cfg.gazeFollow) {
            s->rx = (s->outW - s->rw) / 2;
            s->ry = (s->outH - s->rh) / 2;
        } else if (live) {
            const float cx = u * (float)s->outW, cy = v * (float)s->outH;
            const float dead = g_cfg.gazeDeadzone * (float)s->outW;
            if (fabsf(cx - (s->rx + 0.5f * s->rw)) > dead || fabsf(cy - (s->ry + 0.5f * s->rh)) > dead) {
                const long x = lroundf(cx - 0.5f * s->rw), y = lroundf(cy - 0.5f * s->rh);
                s->rx = (UINT)std::max(0L, std::min(x, (long)(s->outW - s->rw)));
                s->ry = (UINT)std::max(0L, std::min(y, (long)(s->outH - s->rh)));
            }
        }   // tracking lost: stay where it is
    }
    if (live && GazeShowRing()) {
        p.marks[0] = (uint32_t)lroundf(u * s->outW);
        p.marks[1] = (uint32_t)lroundf(v * s->outH);
    }
    if (GazeCalibrating()) {
        GazeTarget(side, &u, &v);
        p.marks[2] = (uint32_t)lroundf(u * s->outW);
        p.marks[3] = (uint32_t)lroundf(v * s->outH);
    }
}

// Which grid cells the motion passes cover: the input footprint of the output region plus a
// margin for their windows and for the flow interpolation in ACCUM.
void FillRegionParams(const Stream *s, bool localSearch, TParams &p)
{
    const float sx = (float)s->w / (float)s->outW, sy = (float)s->h / (float)s->outH;
    const float ix0 = s->rx * sx, ix1 = (s->rx + s->rw) * sx, iy0 = s->ry * sy, iy1 = (s->ry + s->rh) * sy;
    auto lo = [](float v, float cell, int margin) { return std::max(0, (int)floorf(v / cell) - margin); };
    auto hi = [](float v, float cell, int margin, UINT n) { return std::min((int)n, (int)ceilf(v / cell) + margin); };
    p.region[0] = s->rx; p.region[1] = s->ry; p.region[2] = s->rw; p.region[3] = s->rh;

    // refinement: L2 cells (4 px), 2 cells of margin for ACCUM's bilinear flow lookup
    const int r0x = lo(ix0, 4.0f, 2), r1x = hi(ix1, 4.0f, 2, s->lw[2]);
    const int r0y = lo(iy0, 4.0f, 2), r1y = hi(iy1, 4.0f, 2, s->lh[2]);
    p.refCells[0] = r0x; p.refCells[1] = r0y; p.refCells[2] = r1x - r0x; p.refCells[3] = r1y - r0y;

    // Lucas-Kanade: L3 cells (8 px). With the local search, REFINE reads the cell under each of
    // its cells and the four neighbours, so every cell there; otherwise only the global fit reads
    // them and a sample of at most ~12k is plenty.
    int l0x, l1x, l0y, l1y;
    if (localSearch) {
        l0x = std::max(0, r0x / 2 - 1); l1x = std::min((int)s->lw[3], (r1x - 1) / 2 + 2);
        l0y = std::max(0, r0y / 2 - 1); l1y = std::min((int)s->lh[3], (r1y - 1) / 2 + 2);
    } else {
        l0x = lo(ix0, 8.0f, 0); l1x = hi(ix1, 8.0f, 0, s->lw[3]);
        l0y = lo(iy0, 8.0f, 0); l1y = hi(iy1, 8.0f, 0, s->lh[3]);
    }
    const double cells = (double)(l1x - l0x) * (double)(l1y - l0y);
    const UINT thin = cells > 12000.0 ? (UINT)ceil(sqrt(cells / 12000.0)) : 1u;
    const UINT lkStep = localSearch ? 1u : thin;
    p.lkCells[0] = l0x; p.lkCells[1] = l0y;
    p.lkCells[2] = (UINT)(l1x - l0x + lkStep - 1) / lkStep;
    p.lkCells[3] = (UINT)(l1y - l0y + lkStep - 1) / lkStep;
    p.strides[0] = lkStep;
    p.strides[1] = localSearch ? thin : 1u;

    // block search: L4 pixels (16 px), 2 of margin around the LK cells' predictors
    const int s0x = lo(ix0, 16.0f, 2), s1x = hi(ix1, 16.0f, 2, s->lw[4]);
    const int s0y = lo(iy0, 16.0f, 2), s1y = hi(iy1, 16.0f, 2, s->lh[4]);
    p.searchBox[0] = s0x; p.searchBox[1] = s0y; p.searchBox[2] = s1x - s0x; p.searchBox[3] = s1y - s0y;

    // The pyramid (luma, levels, gradient packs) and its copy for the next frame: only where the
    // motion passes read, the region and [temporal] workMargin input px around it (their windows,
    // the block search's reach and the motion of a frame); the rest of the image never needs it.
    const int m4 = (g_cfg.workMargin + 15) / 16;
    p.strides[3] = (UINT)(m4 * 16);
    p.work[0] = (UINT)lo(ix0, 16.0f, m4); p.work[1] = (UINT)lo(iy0, 16.0f, m4);
    p.work[2] = (UINT)hi(ix1, 16.0f, m4, s->lw[4]); p.work[3] = (UINT)hi(iy1, 16.0f, m4, s->lh[4]);
}

} // namespace

void RunStream(ID3D11DeviceContext *ctx, DevState *ds, StreamLink *link, ViewEntry *ve, LONGLONG now)
{
    Stream *s = link->stream;
    TexState *ts = ve->tex.Get();
    const int x = s->xform;
    const UINT n = link->count;
    const bool reset = s->needReset || s->resetGen != g_resetGen;

    TParams p = {};
    p.size01[0] = s->lw[0]; p.size01[1] = s->lh[0]; p.size01[2] = s->lw[1]; p.size01[3] = s->lh[1];
    p.size23[0] = s->lw[2]; p.size23[1] = s->lh[2]; p.size23[2] = s->lw[3]; p.size23[3] = s->lh[3];
    p.size4out[0] = s->lw[4]; p.size4out[1] = s->lh[4]; p.size4out[2] = s->outW; p.size4out[3] = s->outH;
    p.scaleInfo[0] = (float)s->outW / (float)s->w;
    p.scaleInfo[1] = (float)s->outH / (float)s->h;
    p.scaleInfo[2] = (float)s->w / (float)s->outW;
    p.scaleInfo[3] = (float)s->h / (float)s->outH;
    const float sharp = g_cfg.sharpness;
    FsrRcasCon(p.rcasCon, 2.0f - 2.0f * sharp);
    p.tune0[0] = g_cfg.maxWeight;
    p.tune0[1] = g_cfg.clipGamma;
    p.tune0[2] = g_cfg.residualTol;
    p.tune0[3] = g_cfg.sampleSigma;
    p.tune1[0] = g_cfg.minNewWeight;
    p.tune1[1] = g_cfg.lkEps;
    p.tune1[2] = g_cfg.searchLambda;
    p.tune1[3] = g_cfg.clipReject;
    p.misc[0] = link->first;
    const bool repaint = !link->written || link->outGen != g_outGen;   // never written, or settings changed
    p.misc[1] = (g_cfg.tint ? TF_TINT : 0u) | (repaint ? TF_FORCE : 0u) | (reset ? TF_RESET : 0u) |
                (g_cfg.bilinearEdges ? TF_BILINEAR : 0u);
    p.misc[2] = n;
    p.debugFlow[0] = g_debugFlow[0];
    p.debugFlow[1] = g_debugFlow[1];
    p.debugFlow[2] = g_debugFlow[2];
    p.tune2[0] = g_cfg.localTrust;
    p.tune2[1] = g_cfg.localGate;
    p.tune2[2] = g_cfg.localRatio;
    p.tune2[3] = g_cfg.sharpHistory ? 1.0f : 0.0f;
    p.tune3[0] = (float)g_cfg.localCheck;
    p.tune3[1] = g_cfg.localAgree;
    p.tune3[2] = g_cfg.localAperture;
    p.misc[3] = s->band;
    s->side = StreamSide(s, link, ts);
    PlaceRegion(s, s->side, p);
    FillRegionParams(s, g_cfg.localTrust > 0.0f, p);
    const LinkGeo &geo = link->geo;
    const UINT tw = ts->src.Width, th = ts->src.Height;
    p.srcRect[0] = geo.sx; p.srcRect[1] = geo.sy; p.srcRect[2] = tw; p.srcRect[3] = th;
    p.dstRect[0] = geo.ox; p.dstRect[1] = geo.oy; p.dstRect[2] = ts->plan.outW; p.dstRect[3] = ts->plan.outH;
    // OUTER also refreshes this band outside the rectangle: the picture may reach a little further
    p.strides[2] = geo.full ? 0u : (UINT)ceil(48.0 * std::max(ts->plan.scaleX, ts->plan.scaleY));
    // For a few seconds after the app's notice of a resolution change, the rest of the proxy is
    // painted with every new frame as well: a picture that grew shows its new part at once (from
    // this frame alone), before the larger rectangle is confirmed.
    if (!geo.full && now < g_probeFastUntil) p.misc[1] |= TF_MARGIN;
    ContentProbe *probe = EnsureProbe(ds, ts);
    const uint32_t sliceMask = ((1u << n) - 1u) << link->first;
    if (probe) p.misc[1] |= (probe->primed & sliceMask) == sliceMask ? TF_PROBE : TF_PRIME;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(ds->tcb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(ds->tcb.Get(), 0);
    ID3D11Buffer *cb = ds->tcb.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);

    PassQueries *pq = (g_cfg.passTiming && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) ? AcquirePassQueries(ds, ctx)
                                                                                           : nullptr;
    int mark = 0;
    auto Mark = [&]() {
        if (pq && mark <= kTPasses) ctx->End(pq->t[mark++].Get());
    };
    if (pq) ctx->Begin(pq->dj.Get());
    Mark();

    ID3D11Buffer *args = s->args.buf.Get();
    auto Indirect = [&](int shader, int entry) {
        ctx->CSSetShader(ds->cs[shader].Get(), nullptr, 0);
        ctx->DispatchIndirect(args, entry * 12);
    };
    ID3D11ShaderResourceView *src = ve->srcRead.Get();

    // 1. sparse fingerprint of the whole texture and decide (always: the only work for a frame
    //    already processed), then luma straight to L1 (new frames only)
    const UINT zero[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(s->fp.uav.Get(), zero);
    ctx->CSSetShader(ds->cs[SH_T_FP_X0 + x].Get(), nullptr, 0);
    BindCS(ctx, { src }, { s->fp.uav.Get(), probe ? probe->memUAV.Get() : nullptr, probe ? probe->boxUAV.Get() : nullptr });
    ctx->Dispatch((tw + 63) / 64, (th + 63) / 64, n);   // one sample per 8x8 block
    if (probe) probe->primed |= sliceMask;
    ctx->CSSetShader(ds->cs[SH_T_DECIDE].Get(), nullptr, 0);
    BindCS(ctx, { s->fp.srv.Get() }, { s->state.uav.Get(), s->args.uav.Get(), s->ctrl.uav.Get(), link->linkUAV });
    ctx->Dispatch(1, 1, 1);
    BindCS(ctx, { src }, { s->cur[1].uav.Get() });   // (the args buffer must not stay bound as a UAV)
    Indirect(SH_T_LUMA_X0 + x, A_LUMA);
    Mark();

    // 3. pyramid (+ gradient packs); LUMA already wrote L1
    BindCS(ctx, { s->cur[1].srv.Get() }, { s->cur[2].uav.Get(), s->pack[1].uav.Get() });
    Indirect(SH_T_DOWN_L2, A_DOWN2);
    BindCS(ctx, { s->cur[2].srv.Get() }, { s->cur[3].uav.Get(), s->pack[2].uav.Get() });
    Indirect(SH_T_DOWN_L3, A_DOWN3);
    BindCS(ctx, { s->cur[3].srv.Get() }, { s->cur[4].uav.Get(), s->pack[3].uav.Get() });
    Indirect(SH_T_DOWN_L4, A_DOWN4);
    Mark();

    // 4. motion: coarse search, pyramidal Lucas-Kanade, robust global model, per-cell refinement
    BindCS(ctx, { s->cur[4].srv.Get(), s->prev[4].srv.Get(), s->flowPrev.srv.Get(), s->ctrl.srv.Get() }, { s->flow4.uav.Get() });
    Indirect(SH_T_SEARCH, A_SEARCH);
    Mark();
    BindCS(ctx, { s->pack[1].srv.Get(), s->pack[2].srv.Get(), s->pack[3].srv.Get(), s->prev[1].srv.Get(),
                  s->prev[2].srv.Get(), s->prev[3].srv.Get(), s->flow4.srv.Get(), s->ctrl.srv.Get() },
           { s->flowLoc.uav.Get() });
    Indirect(SH_T_LK, A_LK);
    Mark();
    BindCS(ctx, { s->flowLoc.srv.Get(), s->ctrl.srv.Get() }, { s->model.uav.Get() });
    Indirect(SH_T_GLOBAL, A_GLOBAL);
    Mark();
    BindCS(ctx, { s->flowLoc.srv.Get(), s->model.srv.Get(), s->flowPrev.srv.Get(), s->pack[1].srv.Get(), s->prev[1].srv.Get(),
                  s->ctrl.srv.Get() },
           { s->flow2.uav.Get() });
    Indirect(g_cfg.localTrust > 0.0f ? SH_T_REFINE : SH_T_REFINE_G, A_REFINE);
    Mark();

    // 5. accumulate into whichever history is not current (the GPU knows which)
    BindCS(ctx, { src, s->flow2.srv.Get(), s->hist[0].srv.Get(), s->ctrl.srv.Get() }, { s->hist[1].uav.Get() });
    Indirect(SH_T_ACCUM_X0 + x, A_ACC01);
    BindCS(ctx, { src, s->flow2.srv.Get(), s->hist[1].srv.Get(), s->ctrl.srv.Get() }, { s->hist[0].uav.Get() });
    Indirect(SH_T_ACCUM_X0 + x, A_ACC10);
    Mark();

    // 6. output into this texture's proxy (RCAS or copy)
    const int outShader = (sharp > 0.0f ? SH_T_OUTRCAS_X0 : SH_T_OUTCOPY_X0) + x;
    BindCS(ctx, { s->hist[1].srv.Get(), src, s->ctrl.srv.Get() }, { ts->proxyUAV.Get() });
    Indirect(outShader, A_OUT1);
    BindCS(ctx, { s->hist[0].srv.Get(), src, s->ctrl.srv.Get() }, { ts->proxyUAV.Get() });
    Indirect(outShader, A_OUT0);
    if (s->foveated || !geo.full) {   // outside the region (and around the content rectangle): this frame alone
        BindCS(ctx, { nullptr, src, s->ctrl.srv.Get() }, { ts->proxyUAV.Get() });
        Indirect(SH_T_OUTER_X0 + x, A_OUTER);
    }
    if (!geo.full) {     // outside the content rectangle (the compositor does not sample it): repaints only
        BindCS(ctx, { nullptr, src }, { ts->proxyUAV.Get() });
        Indirect(SH_T_MARGIN_X0 + x, A_MARGIN);
    }
    Mark();

    // 7. this frame's pyramid becomes next frame's reference
    BindCS(ctx, { s->cur[1].srv.Get(), s->cur[2].srv.Get(), s->cur[3].srv.Get(), s->cur[4].srv.Get(), s->flow2.srv.Get() },
           { s->prev[1].uav.Get(), s->prev[2].uav.Get(), s->prev[3].uav.Get(), s->prev[4].uav.Get(), s->flowPrev.uav.Get() });
    Indirect(SH_T_COPY, A_COPY);
    Mark();
    if (pq) {
        ctx->End(pq->dj.Get());
        pq->busy = mark == kTPasses + 1;
    }

    if (s->runs == 0)
        LOGI("temporal: stream #%d first run (texture #%llu, slices %u..%u)", s->id, (unsigned long long)ts->serial,
             link->first, link->first + n - 1);
    s->needReset = false;
    s->resetGen = g_resetGen;
    link->written = true;
    link->outGen = g_outGen;
    s->lastTexSerial = ts->serial;
    s->lastRunQpc = now;
    s->runs++;
    s->runsInterval++;
    InterlockedIncrement(&g_stats.temporalRuns);
    InterlockedIncrement(&g_stats.upscales);
}

// ---- content check for the spatial path ------------------------------------------------------

bool CreateGateResources(DevState *ds)
{
    Buf fp, args;
    if (!CreateRaw(ds->dev.Get(), fp, 16, true, false) || !CreateRaw(ds->dev.Get(), args, 32, false, true)) return false;
    ds->gateFp = fp.buf;
    ds->gateFpSRV = fp.srv;
    ds->gateFpUAV = fp.uav;
    ds->gateArgs = args.buf;
    ds->gateArgsUAV = args.uav;
    return true;
}

bool EnsureGate(DevState *ds, ContentGate &g)
{
    if (g.uav) return true;
    Buf b;
    if (!CreateRaw(ds->dev.Get(), b, 32, false, false)) return false;
    g.buf = b.buf.Detach();
    g.uav = b.uav.Detach();
    return true;
}

void ReleaseGate(ContentGate &g)
{
    DeferRelease(g.uav);
    DeferRelease(g.buf);
    g.uav = nullptr;
    g.buf = nullptr;
}

bool RunContentGate(ID3D11DeviceContext *ctx, DevState *ds, TexState *ts, ID3D11ShaderResourceView *src, UINT first,
                    UINT count, int xform, ContentGate &g, bool force, UINT gx, UINT gy, UINT gz)
{
    if (!ds->gateFpUAV || !EnsureGate(ds, g)) return false;
    const UINT w = ts->src.Width, h = ts->src.Height;
    ContentProbe *probe = EnsureProbe(ds, ts);
    const uint32_t sliceMask = ((1u << count) - 1u) << first;
    TParams p = {};
    p.size01[0] = w; p.size01[1] = h;
    p.srcRect[2] = w; p.srcRect[3] = h;
    p.misc[0] = first;
    p.misc[1] = force ? TF_FORCE : 0u;
    if (probe) p.misc[1] |= (probe->primed & sliceMask) == sliceMask ? TF_PROBE : TF_PRIME;
    p.misc[2] = count;
    p.strides[0] = gx; p.strides[1] = gy; p.strides[2] = gz;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(ds->tcb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(ds->tcb.Get(), 0);
    ID3D11Buffer *cb = ds->tcb.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    const UINT zero[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(ds->gateFpUAV.Get(), zero);
    ctx->CSSetShader(ds->cs[SH_T_FP_X0 + xform].Get(), nullptr, 0);
    BindCS(ctx, { src }, { ds->gateFpUAV.Get(), probe ? probe->memUAV.Get() : nullptr, probe ? probe->boxUAV.Get() : nullptr });
    ctx->Dispatch((w + 63) / 64, (h + 63) / 64, count);
    if (probe) probe->primed |= sliceMask;
    ctx->CSSetShader(ds->cs[SH_T_SDECIDE].Get(), nullptr, 0);
    BindCS(ctx, { ds->gateFpSRV.Get() }, { g.uav, ds->gateArgsUAV.Get() });
    ctx->Dispatch(1, 1, 1);
    BindCS(ctx, {}, {});   // the args buffer must not stay bound as a UAV
    g.outGen = g_outGen;
    return true;
}

void TemporalSweep()
{
    if (!TryAcquireSRWLockExclusive(&g_streamLock)) return;
    auto &v = Streams();
    for (size_t i = 0; i < v.size();) {
        Stream *s = v[i];
        if (s->RefCount() == 1) {   // only the list still references it: its textures are gone
            LOGI("temporal: stream #%d released (%llu runs)", s->id, (unsigned long long)s->runs);
            v.erase(v.begin() + i);
            s->Release();
        } else {
            ++i;
        }
    }
    ReleaseSRWLockExclusive(&g_streamLock);
}

int TemporalDescribe(StreamDesc *out, int max)
{
    int n = 0;
    AcquireSRWLockShared(&g_streamLock);
    for (Stream *s : Streams()) {
        if (n >= max) break;
        // new app frames as counted on the GPU (CPU runs count every draw of every texture)
        out[n++] = { (uint32_t)s->id, s->w, s->h, s->outW, s->outH, (uint32_t)s->textures, (uint64_t)s->gpuNew };
    }
    ReleaseSRWLockShared(&g_streamLock);
    return n;
}

void TemporalResetAll(const char *why)
{
    InterlockedIncrement(&g_resetGen);
    if (!Streams().empty()) LOGI("temporal: history reset (%s)", why);
}

void StreamPause(StreamLink *link)
{
    // the eye is on the spatial path for now: start over when it comes back, and repaint
    if (link->stream) link->stream->needReset = true;
    link->written = false;
}

// ---- content rectangles ----------------------------------------------------------------------

ContentProbe::ContentProbe() {}

ContentProbe::~ContentProbe()
{
    DeferRelease(memUAV.Detach());
    DeferRelease(boxUAV.Detach());
    DeferRelease(mem.Detach());
    DeferRelease(box.Detach());
    DeferRelease(staging.Detach());
}

LinkGeo ContentGeo(const TexState *ts)
{
    LinkGeo g;
    const UINT tw = ts->src.Width, th = ts->src.Height;
    const ContentProbe *p = ts->probe.Get();
    if (!p || !p->sub || !g_cfg.detectContent) {
        g.cw = tw;
        g.ch = th;
        g.outW = ts->plan.outW;
        g.outH = ts->plan.outH;
        return g;
    }
    g.full = false;
    g.sx = p->rx;
    g.sy = p->ry;
    g.cw = p->rw;
    g.ch = p->rh;
    g.ox = (UINT)std::min(floor(g.sx * ts->plan.scaleX + 0.5), (double)ts->plan.outW - 1);
    g.oy = (UINT)std::min(floor(g.sy * ts->plan.scaleY + 0.5), (double)ts->plan.outH - 1);
    g.outW = (UINT)std::max(1.0, std::min(floor(g.cw * ts->plan.scaleX + 0.5), (double)(ts->plan.outW - g.ox)));
    g.outH = (UINT)std::max(1.0, std::min(floor(g.ch * ts->plan.scaleY + 0.5), (double)(ts->plan.outH - g.oy)));
    return g;
}

// Render thread (immediate context), holding g_workLock: reads the windows that ended and starts
// new ones, every probeWindowMs per texture.
void PollProbes(ID3D11DeviceContext *ctx, ID3D11Device *dev)
{
    auto &v = Probes();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int ms = now.QuadPart < g_probeFastUntil ? std::min(g_cfg.probeWindowMs, 100) : g_cfg.probeWindowMs;
    const LONGLONG window = (LONGLONG)ms * g_qpf.QuadPart / 1000;
    for (size_t i = 0; i < v.size();) {
        ContentProbe *p = v[i];
        if (p->dead) {
            v.erase(v.begin() + i);
            p->Release();
            continue;
        }
        ++i;
        if (p->dev != dev) continue;   // copies and maps must stay on the probe's own device
        if (p->pending) {
            D3D11_MAPPED_SUBRESOURCE m;
            HRESULT hr = ctx->Map(p->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (SUCCEEDED(hr)) {
                uint32_t b[4];
                memcpy(b, m.pData, sizeof(b));
                ctx->Unmap(p->staging.Get(), 0);
                ProbeWindow(p, b, now.QuadPart);
            }
            p->pending = false;
        }
        // every texture's windows end in the same call: an eye's textures judge the same span of
        // time, so they see a change of the picture in the same window
        if (now.QuadPart / window != p->windowSlot) {
            ctx->CopyResource(p->staging.Get(), p->box.Get());
            const UINT ones[4] = { ~0u, ~0u, ~0u, ~0u };
            ctx->ClearUnorderedAccessViewUint(p->boxUAV.Get(), ones);
            p->pending = true;
            p->windowSlot = now.QuadPart / window;
        }
    }
}

void ContentHint(const char *who)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const bool was = now.QuadPart < g_probeFastUntil;
    // the app sends this as it starts the change and again when SteamVR took it; the game follows
    // within a frame or a few
    g_probeFastUntil = now.QuadPart + 5 * g_qpf.QuadPart;
    if (!was)
        LOGI("content: %s is changing the game's resolution; looking for the new picture size every %d ms for a few seconds", who,
             std::min(g_cfg.probeWindowMs, 100));
}

// ---- eye pools -------------------------------------------------------------------------------

int NotePool(const OutSig &sig, TexState *ts, StreamLink *link, LONGLONG now)
{
    const bool eye = sig.valid && (sig.vx != 0 || sig.vy != 0 || sig.vw < (int)sig.rtW || sig.vh < (int)sig.rtH);
    if (!eye) {
        if (link) link->pool = -1;
        return -1;
    }
    int pi = -1;
    for (int i = 0; i < g_poolN && pi < 0; ++i)
        if (g_pools[i].sig == sig) pi = i;
    if (pi < 0) {
        if (g_poolN < 4) pi = g_poolN++;
        else {
            pi = 0;
            for (int i = 1; i < g_poolN; ++i)
                if (g_pools[i].lastQpc < g_pools[pi].lastQpc) pi = i;
        }
        g_pools[pi] = Pool();
        g_pools[pi].sig = sig;
        g_pools[pi].epoch = ++g_poolEpochs;
    }
    Pool &P = g_pools[pi];
    P.lastQpc = now;
    if (ContentProbe *p = ts->probe.Get()) {
        p->pool = pi;
        p->poolEpoch = P.epoch;
        p->drawQpc = now;
    }
    const int k = (P.head + P.n) % 16;
    if (P.n < 16) P.n++;
    else P.head = (P.head + 1) % 16;
    P.d[k] = { ts->serial, link && link->stream ? link->stream->id : 0, ts->src.Format, ts->src.Width, ts->src.Height, now };
    if (link) link->pool = pi;
    return pi;
}

bool PoolMixed(const StreamLink *link, const TexState *ts, LONGLONG now)
{
    if (!link || link->pool < 0 || link->pool >= g_poolN) return false;
    Pool &P = g_pools[link->pool];
    if (P.holdUntil > now) {
        InterlockedIncrement(&g_pausedDraws);
        return true;
    }
    const bool mixed = PoolInterleaved(P, ts->src.Format, ts->src.Width, ts->src.Height, now);
    if (!mixed) {
        P.mixedSince = 0;
        return false;
    }
    InterlockedIncrement(&g_pausedDraws);
    if (P.mixedSince == 0) {
        P.mixedSince = now;
        LOGI("temporal: the textures drawn into the eye viewport (%d,%d %dx%d) are on different histories; FSR 1 for all "
             "of them until they share one", P.sig.vx, P.sig.vy, P.sig.vw, P.sig.vh);
    } else if (now - P.mixedSince > 10 * g_qpf.QuadPart) {
        // Not resolving (say the game renders each of them at a different size): stop holding the
        // histories for a while, then try again.
        P.holdUntil = now + 30 * g_qpf.QuadPart;
        P.mixedSince = 0;
        LOGI("temporal: the eye viewport (%d,%d %dx%d) still has textures of different picture sizes after 10 s; FSR 1 there "
             "for 30 s, then trying again (restarting the game after a resolution change avoids this)",
             P.sig.vx, P.sig.vy, P.sig.vw, P.sig.vh);
    }
    return true;
}

bool PoolEyeTexture(int pool, const TexState *ts, LONGLONG now)
{
    // several textures like this one drawn in turn into the same eye viewport
    if (pool < 0 || pool >= g_poolN) return false;
    const Pool &P = g_pools[pool];
    for (int i = 0; i < P.n; ++i) {
        const PoolDraw &d = P.d[(P.head + i) % 16];
        if (d.serial != ts->serial && now - d.qpc <= g_qpf.QuadPart && d.fmt == ts->src.Format &&
            SameShape(d.texW, d.texH, ts->src.Width, ts->src.Height))
            return true;
    }
    return false;
}

bool PoolHasUpscaledLike(const OutSig &sig, DXGI_FORMAT fmt, UINT w, UINT h, LONGLONG now, uint64_t notSerial, LONGLONG since)
{
    const bool eye = sig.valid && (sig.vx != 0 || sig.vy != 0 || sig.vw < (int)sig.rtW || sig.vh < (int)sig.rtH);
    if (!eye) return false;
    for (int i = 0; i < g_poolN; ++i) {
        const Pool &P = g_pools[i];
        if (!(P.sig == sig)) continue;
        for (int k = 0; k < P.n; ++k) {
            const PoolDraw &d = P.d[(P.head + k) % 16];
            if (now - d.qpc <= g_qpf.QuadPart && d.qpc > since && d.serial != notSerial && d.fmt == fmt &&
                SameShape(d.texW, d.texH, w, h))
                return true;
        }
    }
    return false;
}

void PoolStatus(int *pools, int *mixed)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int n = 0, mx = 0;
    for (int i = 0; i < g_poolN; ++i) {
        const Pool &P = g_pools[i];
        if (now.QuadPart - P.lastQpc > 2 * g_qpf.QuadPart) continue;
        ++n;
        const PoolDraw &last = P.d[(P.head + P.n - 1) % 16];
        // a moment on the spatial path while an eye's textures move to a new history together is
        // not worth reporting; one that lasts is
        if (P.holdUntil > now.QuadPart || (P.n > 0 && P.mixedSince != 0 && now.QuadPart - P.mixedSince > g_qpf.QuadPart &&
                                           PoolInterleaved(P, last.fmt, last.texW, last.texH, now.QuadPart)))
            ++mx;
    }
    *pools = n;
    *mixed = mx;
}

// Reads each stream's GPU counters (new app frames, checks, forced repaints) without waiting: a
// copy is requested now and picked up on a later call. Render thread, immediate context.
void TemporalPollCounters(ID3D11DeviceContext *ctx, ID3D11Device *dev)
{
    AcquireSRWLockShared(&g_streamLock);
    for (Stream *s : Streams()) {
        if (s->dev != dev) continue;   // copies and maps must stay on the stream's own device
        if (s->stagingPending) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(s->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                ReadCounters((const uint32_t *)m.pData, s->gpuNew, s->gpuCalls, s->gpuForced);
                ctx->Unmap(s->staging.Get(), 0);
                s->stagingPending = false;
                s->haveCounters = true;
            }
        }
        if (!s->stagingPending) {
            ctx->CopyResource(s->staging.Get(), s->state.buf.Get());
            s->stagingPending = true;
        }
    }
    ReleaseSRWLockShared(&g_streamLock);
}

void TemporalLogStats(ID3D11DeviceContext *ctx, ID3D11Device *dev, double seconds)
{
    TemporalPollCounters(ctx, dev);
    AcquireSRWLockShared(&g_streamLock);
    for (Stream *s : Streams()) {
        if (s->dev != dev || !s->haveCounters) continue;
        const uint32_t nf = s->gpuNew, calls = s->gpuCalls, forced = s->gpuForced;
        if (s->haveBaseline) {
            LOGI("stats:   temporal stream #%d %ux%u x%u -> %ux%u: textures=%ld draws=%llu new app frames=%u (%.1f/s) "
                 "repeats=%u forced=%u",
                 s->id, s->w, s->h, s->slices, s->outW, s->outH, (long)s->textures, (unsigned long long)s->runsInterval,
                 nf - s->baseNew, seconds > 0 ? (nf - s->baseNew) / seconds : 0.0, (calls - s->baseCalls) - (nf - s->baseNew),
                 forced - s->baseForced);
        }
        s->baseNew = nf;
        s->baseCalls = calls;
        s->baseForced = forced;
        s->haveBaseline = true;
        s->runsInterval = 0;
    }
    ReleaseSRWLockShared(&g_streamLock);
}

} // namespace vu

// ---------------------------------------------------------------------------------------------
// Test hook: blocking read of a stream's GPU-side counters. Never called in normal operation.
// out[0..15] = state words, out[16] = stream id, out[17] = linked textures, out[18] = CPU runs,
// out[19..22] = region x, y, w, h, out[23..24] = output size. out must hold 32 words.
extern "C" __declspec(dllexport) int VrUpscaleDebugStream(int index, uint32_t *out)
{
    auto &v = vu::Streams();   // test-only: the harness drives one thread
    if (index < 0 || index >= (int)v.size()) return (int)v.size();
    vu::Stream *s = v[index];
    vu::Ref<ID3D11Device> dev;
    s->state.buf->GetDevice(dev.Put());
    vu::Ref<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.Put());
    ctx->CopyResource(s->debugStaging.Get(), s->state.buf.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(s->debugStaging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, vu::kStateBytes);
        ctx->Unmap(s->debugStaging.Get(), 0);
    }
    out[16] = (uint32_t)s->id;
    out[17] = (uint32_t)s->textures;
    out[18] = (uint32_t)s->runs;
    out[19] = s->rx; out[20] = s->ry; out[21] = s->rw; out[22] = s->rh;   // the region (CPU side, this frame)
    out[23] = s->outW; out[24] = s->outH;
    out[25] = (uint32_t)(s->side + 1);   // 0 unknown, 1 left, 2 right
    return (int)v.size();
}

// Test hook: blocking read of a stream's current flow field (L2 grid, RGBA16F: dx, dy in input px,
// residual, valid). Writes up to maxFloats floats as [slice][y][x][4]; returns the float count needed.
// out2[0..1] = grid width/height. Never called in normal operation.
extern "C" __declspec(dllexport) int VrUpscaleDebugFlow(int index, float *out, int maxFloats, int *dims)
{
    auto &v = vu::Streams();
    if (index < 0 || index >= (int)v.size()) return 0;
    vu::Stream *s = v[index];
    const UINT gw = s->lw[2], gh = s->lh[2], n = s->slices;
    if (dims) { dims[0] = (int)gw; dims[1] = (int)gh; dims[2] = (int)n; }
    const int need = (int)(gw * gh * n * 4);
    if (!out || maxFloats < need) return need;
    vu::Ref<ID3D11Device> dev;
    s->flow2.tex->GetDevice(dev.Put());
    vu::Ref<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.Put());
    D3D11_TEXTURE2D_DESC d;
    s->flow2.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    vu::Ref<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, st.Put()))) return -1;
    ctx->CopyResource(st.Get(), s->flow2.tex.Get());
    for (UINT z = 0; z < n; ++z) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(st.Get(), z, D3D11_MAP_READ, 0, &m))) return -1;
        for (UINT y = 0; y < gh; ++y) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            for (UINT x = 0; x < gw * 4; ++x) {
                uint16_t hbits = row[x];
                uint32_t sign = (hbits & 0x8000u) << 16, e = (hbits >> 10) & 0x1f, mant = hbits & 0x3ff, f;
                if (e == 0) f = sign;
                else if (e == 31) f = sign | 0x7f800000u | (mant << 13);
                else f = sign | ((e - 15 + 127) << 23) | (mant << 13);
                memcpy(&out[((size_t)z * gh + y) * gw * 4 + x], &f, 4);
            }
        }
        ctx->Unmap(st.Get(), z);
    }
    return need;
}

// Test hook: force a uniform flow (input px, cur -> prev) for every stream; enable = 0 restores
// normal motion estimation. Used to measure the accumulation with perfect registration.
extern "C" __declspec(dllexport) void VrUpscaleDebugForceFlow(float dx, float dy, int enable)
{
    vu::g_debugFlow[0] = dx;
    vu::g_debugFlow[1] = dy;
    vu::g_debugFlow[2] = enable ? 1.0f : 0.0f;
}

// Test hook: a texture's content rectangle. out[0..3] = x, y, w, h; out[4] = part of the texture;
// out[5] = changes of the rectangle; out[6] = observation windows with changes; out[7] = probe exists.
// Returns 0 if the texture has no upscaler state. Never called in normal operation.
extern "C" __declspec(dllexport) int VrUpscaleDebugContent(ID3D11Texture2D *tex, uint32_t *out)
{
    memset(out, 0, 8 * sizeof(uint32_t));
    IUnknown *unk = nullptr;
    UINT sz = sizeof(unk);
    if (!tex || FAILED(tex->GetPrivateData(vu::kGuidTexState, &sz, &unk)) || !unk) return 0;
    vu::TexState *ts = static_cast<vu::TexState *>(unk);
    if (vu::ContentProbe *p = ts->probe.Get()) {
        out[0] = p->rx; out[1] = p->ry; out[2] = p->rw; out[3] = p->rh;
        out[4] = p->sub ? 1u : 0u;
        out[5] = (uint32_t)p->gen;
        out[6] = (uint32_t)p->active;
        out[7] = 1;
    }
    unk->Release();
    return 1;
}

// Test hook: draws that went to the spatial path because their eye's textures were on different
// histories (or the eye was held there), since the DLL loaded.
extern "C" __declspec(dllexport) int VrUpscaleDebugPaused()
{
    return (int)vu::g_pausedDraws;
}

// Test hook: eye pools seen in the last 2 s, and how many of them are on the spatial path because
// their textures are on different histories (for more than a second, as the app is told).
extern "C" __declspec(dllexport) int VrUpscaleDebugPools(int *mixed)
{
    int n = 0, mx = 0;
    vu::PoolStatus(&n, &mx);
    if (mixed) *mixed = mx;
    return n;
}
