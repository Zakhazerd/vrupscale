// Core of the compositor-side upscaler.
//
// Flow on the compositor's immediate context:
//   PSSetShaderResources(views)  -> each view is classified once (cached as private data on the
//                                   view). Eligible views are swapped for a view of an upscaled
//                                   "proxy" texture of the same format family and slice count.
//   Draw*()                      -> before a draw that samples a proxy, the upscaler runs in the
//                                   same context (so it is ordered after any GPU wait the
//                                   compositor queued): the temporal path (temporal.cpp) or, as a
//                                   fallback / alternative mode, FSR 1 EASU(+RCAS) or bilinear.
// Distortion shaders sample with normalized UVs, so the larger proxy is transparent to them.
//
// Lifetime: our per-view (ViewEntry) and per-texture (TexState) objects are COM objects attached
// with SetPrivateDataInterface, so they die with the compositor's objects. Anything they own is
// released later on the render thread (deferred), never inside D3D's own teardown.
#include "internal.h"
#include "control_shared.h"

#include <d3d11_4.h>

#include <math.h>
#include <string.h>
#include <algorithm>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace vu {

const GUID kGuidViewEntry  = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x01 } };
const GUID kGuidViewReject = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x02 } };
const GUID kGuidTexState   = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x03 } };
const GUID kGuidTexMark    = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x04 } };
const GUID kGuidTexLogged  = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x05 } };
static const GUID kGuidCsLogged = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x06 } };
const GUID kGuidOurs            = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x07 } };

DebugStats g_stats = {};
volatile LONG g_reloadRequested = 0;
LARGE_INTEGER g_qpf = {};
SRWLOCK g_cfgLock = SRWLOCK_INIT;

const char *XformName(int x) { return x == kSrgb ? "srgb->perceptual" : (x == kFloat ? "hdr-float" : "raw"); }

namespace {

constexpr UINT FLAG_FINAL = 4, FLAG_TINT = 8;

struct Params {
    uint32_t con0[4], con1[4], con2[4], con3[4];
    uint32_t outSize[2];
    uint32_t inSize[2];
    uint32_t flags;
    uint32_t pad[3];
    uint32_t fov[4];        // band | FOV_* | source w, h
    uint32_t content[4];    // the picture in the output: x0, y0, x1, y1
    uint32_t region[2][4];  // per dispatched slice: the area x, y, w, h
    uint32_t marks[2][4];   // per dispatched slice: gaze ring x, y | calibration cross x, y (~0u = none)
};
static_assert(sizeof(Params) == 192, "cbuffer layout must match upscale.hlsl");
constexpr uint32_t FOV_ON = 1, FOV_BILINEAR = 2;

struct RejectRecord { LONG cfgGen; LONG markGen; int token; int unticked; };   // token: row in the app's texture list
// on textures: UINT32 = g_cfgGen when it was found to be one of the game's eye textures: upscaled even
// if its size was unticked, until the next settings change (then it has to show that again)
const GUID kGuidTexEye     = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x0a } };
const char *const kUntickedReason = "unticked in the app (excludeSizes)";

volatile LONG g_markGen = 1;
LONGLONG g_replanQpc = 0;   // when the textures were last re-planned (a settings change)
volatile LONG g_texSerial = 0;

// Interval counters for the periodic stats line
struct Interval { volatile LONG ps, subs, upscales, draws; } g_iv;

// What was upscaled during the current stats interval (render thread only)
struct SizeTally { UINT w, h, slices, ow, oh; DXGI_FORMAT fmt; LONG count; };
SizeTally g_tally[8];
int g_tallyCount = 0;


// ---------------------------------------------------------------------------------------------
// Deferred release

SRWLOCK g_deferLock = SRWLOCK_INIT;
// Heap-allocated and never freed: hooks can still fire while the process is tearing down, after
// static destructors of this module would have run.
std::vector<IUnknown *> &Deferred()
{
    static std::vector<IUnknown *> *v = new std::vector<IUnknown *>();
    return *v;
}
volatile LONG g_deferredCount = 0;

void DrainDeferred()
{
    // Releasing can cascade (texture dies -> its TexState dies -> defers more), so loop a bit.
    for (int round = 0; round < 4 && g_deferredCount > 0; ++round) {
        std::vector<IUnknown *> local;
        AcquireSRWLockExclusive(&g_deferLock);
        local.swap(Deferred());
        ReleaseSRWLockExclusive(&g_deferLock);
        for (IUnknown *p : local) p->Release();
        InterlockedExchangeAdd(&g_deferredCount, -(LONG)local.size());
        InterlockedExchangeAdd(&g_stats.deferredPending, -(LONG)local.size());
    }
}

// ---------------------------------------------------------------------------------------------
// Per-device resources

SRWLOCK g_devLock = SRWLOCK_INIT;
DevState *g_devs[8] = {};
int g_devCount = 0;

std::string Narrow(const wchar_t *w)
{
    char buf[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

void LogDeviceInfo(ID3D11Device *dev)
{
    std::string name = "?";
    Ref<IDXGIDevice> dxgiDev;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)dxgiDev.Put()))) {
        Ref<IDXGIAdapter> ad;
        if (SUCCEEDED(dxgiDev->GetAdapter(ad.Put()))) {
            DXGI_ADAPTER_DESC d;
            if (SUCCEEDED(ad->GetDesc(&d))) name = Narrow(d.Description);
        }
    }
    LOGI("device %p: adapter \"%s\", feature level 0x%x, creation flags 0x%x", (void *)dev, name.c_str(),
         (unsigned)dev->GetFeatureLevel(), dev->GetCreationFlags());
}

DevState *CreateDevState(ID3D11Device *dev)
{
    DevState *ds = new DevState();
    ds->dev = Ref<ID3D11Device>(dev, true);
    LogDeviceInfo(dev);

    if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        LOGE("device %p: feature level below 11_0, compute upscaling unavailable", (void *)dev);
        return ds;
    }
    HRESULT hr = S_OK;
    for (int i = 0; i < SH_COUNT && SUCCEEDED(hr); ++i) {
        BlobRef b = GetShaderBlob(i);
        hr = dev->CreateComputeShader(b.data, b.size, nullptr, ds->cs[i].Put());
        if (FAILED(hr)) LOGE("device %p: CreateComputeShader(#%d) failed (0x%08lx)", (void *)dev, i, (unsigned long)hr);
    }
    if (FAILED(hr)) return ds;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = dev->CreateBuffer(&bd, nullptr, ds->cb.Put());
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = kTParamsBytes;   // TParams in temporal.hlsl
        hr = dev->CreateBuffer(&bd, nullptr, ds->tcb.Put());
    }
    if (SUCCEEDED(hr) && !CreateGateResources(ds))
        LOGE("device %p: content-check buffers failed; the spatial path upscales on every draw", (void *)dev);
    if (FAILED(hr)) {
        LOGE("device %p: CreateBuffer failed (0x%08lx)", (void *)dev, (unsigned long)hr);
        return ds;
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dev->CreateSamplerState(&sd, ds->samp.Put());
    if (FAILED(hr)) {
        LOGE("device %p: CreateSamplerState failed (0x%08lx)", (void *)dev, (unsigned long)hr);
        return ds;
    }
    for (auto &q : ds->q) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
        if (FAILED(dev->CreateQuery(&qd, q.dj.Put())) || FAILED(dev->CreateQuery(&qt, q.t0.Put())) ||
            FAILED(dev->CreateQuery(&qt, q.t1.Put()))) {
            q.dj.Reset(); q.t0.Reset(); q.t1.Reset();
        }
    }
    ds->ok = true;
    LOGI("device %p: upscaler resources ready", (void *)dev);
    return ds;
}

DevState *GetDevState(ID3D11Device *dev)
{
    AcquireSRWLockShared(&g_devLock);
    for (int i = 0; i < g_devCount; ++i) {
        if (g_devs[i]->dev.Get() == dev) {
            DevState *d = g_devs[i];
            ReleaseSRWLockShared(&g_devLock);
            return d;
        }
    }
    ReleaseSRWLockShared(&g_devLock);

    AcquireSRWLockExclusive(&g_devLock);
    DevState *found = nullptr;
    for (int i = 0; i < g_devCount; ++i)
        if (g_devs[i]->dev.Get() == dev) found = g_devs[i];
    if (!found && g_devCount < 8) {
        found = CreateDevState(dev);
        g_devs[g_devCount++] = found;
    }
    ReleaseSRWLockExclusive(&g_devLock);
    return found;
}

} // namespace

void DeferRelease(IUnknown *p)
{
    if (!p) return;
    AcquireSRWLockExclusive(&g_deferLock);
    Deferred().push_back(p);
    ReleaseSRWLockExclusive(&g_deferLock);
    InterlockedIncrement(&g_deferredCount);
    InterlockedIncrement(&g_stats.deferredPending);
}

TexState::TexState() { InterlockedIncrement(&g_stats.liveTexStates); }

TexState::~TexState()
{
    if (probe) InterlockedExchange(&probe->dead, 1);   // the render thread drops it from its list
    for (int i = 0; i < nlinks; ++i) ReleaseLink(links[i]);
    for (auto &g : gates) ReleaseGate(g);
    DeferRelease(proxyUAV.Detach());
    DeferRelease(proxy.Detach());
    InterlockedDecrement(&g_stats.liveTexStates);
}

ViewEntry::ViewEntry() { InterlockedIncrement(&g_stats.liveViewEntries); }

ViewEntry::~ViewEntry()
{
    DeferRelease(proxySRV.Detach());
    DeferRelease(srcRead.Detach());
    DeferRelease(static_cast<IUnknown *>(tex.Detach()));
    InterlockedDecrement(&g_stats.liveViewEntries);
}

void BindCS(ID3D11DeviceContext *ctx, std::initializer_list<ID3D11ShaderResourceView *> srvs,
            std::initializer_list<ID3D11UnorderedAccessView *> uavs)
{
    ID3D11UnorderedAccessView *u[kCsUavSlots] = {};
    ID3D11ShaderResourceView *t[kCsSrvSlots] = {};
    UINT keep[kCsUavSlots];
    for (UINT i = 0; i < kCsUavSlots; ++i) keep[i] = (UINT)-1;
    ctx->CSSetUnorderedAccessViews(0, kCsUavSlots, u, keep);
    UINT i = 0;
    for (auto *v : srvs) if (i < kCsSrvSlots) t[i++] = v;
    if (g_origCSSetShaderResources) g_origCSSetShaderResources(ctx, 0, kCsSrvSlots, t);
    else ctx->CSSetShaderResources(0, kCsSrvSlots, t);
    i = 0;
    for (auto *v : uavs) if (i < kCsUavSlots) u[i++] = v;
    ctx->CSSetUnorderedAccessViews(0, kCsUavSlots, u, keep);
}

bool UavStoreSupported(DevState *ds, DXGI_FORMAT f)
{
    for (auto &e : ds->uavFmts)
        if (e.f == f) return e.ok;
    UINT support = 0;
    bool ok = SUCCEEDED(ds->dev->CheckFormatSupport(f, &support)) &&
              (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) &&
              (support & D3D11_FORMAT_SUPPORT_TEXTURE2D);
    if (ok) {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2 = { f, 0 };
        if (SUCCEEDED(ds->dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))))
            ok = (s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    }
    ds->uavFmts.push_back({ f, ok });
    LOGI("device %p: typed UAV store for %s: %s", (void *)ds->dev.Get(), FormatName(f), ok ? "yes" : "NO");
    return ok;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Per-context state

// Upscale work a draw needs. On a deferred context it is only noted while the command list is
// recorded, and runs on the immediate context just before that command list executes.
struct QJob {
    ViewEntry *ve;        // owned reference while queued
    StreamLink *link;     // temporal mode: the slices' history link (its stream may be null)
    UINT first, count;    // slices
    int side;             // the eye, if the viewport it is drawn into says (0 left, 1 right; -1 not known)
};

struct CtxState {
    ~CtxState() { for (auto &j : pending) j.ve->Release(); }
    std::vector<QJob> pending;            // deferred context: work for the command list being recorded
    ID3D11Resource *written[16] = {};     // deferred context: copy destinations recorded so far (keys only)
    int nWritten = 0;                     // > 16 entries: treat every texture as written
    ID3D11DeviceContext *ctx = nullptr;   // key only
    bool immediate = false;               // else a deferred context (command-list recording)
    Ref<ID3D11Device> dev;
    DevState *ds = nullptr;
    ViewEntry *bound[kSlots] = {};        // PS slots currently holding one of our proxies (owned refs)
    ID3D11ShaderResourceView *unticked[kSlots] = {};   // PS slots holding a view left alone only because its size
    int untickedCount = 0;                             // was unticked (keys; the context holds them while bound)
    int boundCount = 0;
    int hi = -1;
    Ref<ID3D11Multithread> mt;            // the device's lock (from its immediate context), if protection is on
    ID3D11DeviceContext1 *ctx1 = nullptr; // for constant-buffer offset save/restore (same object as ctx: not
                                          // a reference, which would keep a deferred context alive forever)
};

void ClearBound(CtxState *cs);

// Holds the device's multithread lock (if protection is on) for the lifetime of the object, so our
// snapshot -> dispatch -> restore sequence on the immediate context cannot interleave with another
// thread's calls. Deferred contexts belong to one thread and never take it.
struct MtGuard {
    ID3D11Multithread *mt = nullptr;
    explicit MtGuard(CtxState *cs)
    {
        if (cs->mt && cs->mt->GetMultithreadProtected()) {
            mt = cs->mt.Get();
            mt->Enter();
        }
    }
    ~MtGuard() { if (mt) mt->Leave(); }
};

// Serialises our shared state (histories, per-device resources, classification, stats) between
// the threads the compositor renders from: a deferred context records on its own thread while the
// immediate context executes command lists and runs housekeeping.
//
// Lock order: g_workLock, then the device lock (MtGuard). Deferred-context work waits for
// g_workLock but never holds the device lock; immediate-context work only *tries* g_workLock and
// skips this one call's work if another thread has it. So no thread ever waits for g_workLock
// while holding the device lock, whatever the compositor or the runtime holds around our hooks.
CRITICAL_SECTION g_workLock;
struct WorkLock {
    bool held;
    explicit WorkLock(bool wait) : held(wait ? (EnterCriticalSection(&g_workLock), true) : TryEnterCriticalSection(&g_workLock) != 0) {}
    // keeps trying for up to spinUs microseconds (never blocks indefinitely)
    explicit WorkLock(int spinUs) : held(TryEnterCriticalSection(&g_workLock) != 0)
    {
        if (held || spinUs <= 0) return;
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const LONGLONG limit = f.QuadPart * spinUs / 1000000;
        int n = 0;
        do {
            if (++n < 64) YieldProcessor();
            else SwitchToThread();   // past a few microseconds: let the holder run
            if ((held = TryEnterCriticalSection(&g_workLock) != 0)) return;
            QueryPerformanceCounter(&t);
        } while (t.QuadPart - t0.QuadPart < limit);
    }
    ~WorkLock() { if (held) LeaveCriticalSection(&g_workLock); }
};

SRWLOCK g_ctxLock = SRWLOCK_INIT;
CtxState *g_ctxs[16] = {};
volatile LONG g_ctxCount = 0;

CtxState *FindImmediate(ID3D11DeviceContext *ctx)
{
    LONG n = g_ctxCount;
    MemoryBarrier();
    for (LONG i = 0; i < n; ++i)
        if (g_ctxs[i]->ctx == ctx) return g_ctxs[i];
    return nullptr;
}

// A deferred context's state lives in its private data, so it dies with the context (deferred
// contexts can come and go; their addresses get reused).
const GUID kGuidCtxState = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x08 } };

class CtxHolder final : public RefObj {
public:
    CtxState *cs = nullptr;
protected:
    ~CtxHolder() override
    {
        if (cs) {
            ClearBound(cs);
            delete cs;
        }
    }
};

CtxState *FindDeferred(ID3D11DeviceContext *ctx)
{
    IUnknown *unk = nullptr;
    UINT sz = sizeof(unk);
    if (FAILED(ctx->GetPrivateData(kGuidCtxState, &sz, &unk)) || !unk) return nullptr;
    CtxState *cs = static_cast<CtxHolder *>(unk)->cs;
    unk->Release();   // the context keeps its own reference
    return cs;
}

CtxState *FindCtx(ID3D11DeviceContext *ctx)
{
    if (CtxState *c = FindImmediate(ctx)) return c;
    if (ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) return nullptr;
    return FindDeferred(ctx);
}

void InitCtxState(CtxState *c, ID3D11DeviceContext *ctx, bool immediate)
{
    c->ctx = ctx;
    c->immediate = immediate;
    ctx->GetDevice(c->dev.Put());
    Ref<ID3D11DeviceContext> imm;
    if (immediate) imm = Ref<ID3D11DeviceContext>(ctx, true);
    if (imm) imm->QueryInterface(__uuidof(ID3D11Multithread), (void **)c->mt.Put());
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c->ctx1)) && c->ctx1) c->ctx1->Release();
}

CtxState *GetCtx(ID3D11DeviceContext *ctx, const char *firstCall)
{
    if (CtxState *c = FindImmediate(ctx)) return c;
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        if (CtxState *c = FindDeferred(ctx)) return c;
        CtxHolder *h = new CtxHolder();
        h->cs = new CtxState();
        InitCtxState(h->cs, ctx, false);
        CtxState *c = h->cs;
        if (FAILED(ctx->SetPrivateDataInterface(kGuidCtxState, static_cast<IUnknown *>(h)))) {
            h->Release();
            return nullptr;
        }
        h->Release();
        ControlNoteCommandLists();
        LOGI("context %p: %s on a deferred context of device %p (command-list rendering)", (void *)ctx, firstCall,
             (void *)c->dev.Get());
        return c;
    }
    AcquireSRWLockExclusive(&g_ctxLock);
    CtxState *c = FindImmediate(ctx);
    if (!c && g_ctxCount < 16) {
        // Immediate contexts live as long as their device, which this entry keeps a reference to,
        // so a cached entry can never be confused with a later context at a recycled address.
        c = new CtxState();
        InitCtxState(c, ctx, true);
        LOGI("context %p: %s on the immediate context of device %p", (void *)ctx, firstCall, (void *)c->dev.Get());
        g_ctxs[g_ctxCount] = c;
        MemoryBarrier();
        InterlockedIncrement(&g_ctxCount);
    }
    ReleaseSRWLockExclusive(&g_ctxLock);
    return c;
}

void ClearBound(CtxState *cs)
{
    for (int s = 0; s <= cs->hi; ++s) {
        if (cs->bound[s]) {
            cs->bound[s]->Release();
            cs->bound[s] = nullptr;
        }
    }
    cs->boundCount = 0;
    cs->hi = -1;
    if (cs->untickedCount) {
        memset(cs->unticked, 0, sizeof(cs->unticked));
        cs->untickedCount = 0;
    }
}

bool HasEyeMark(ID3D11Texture2D *t)
{
    UINT32 m = 0;
    UINT sz = sizeof(m);
    return SUCCEEDED(t->GetPrivateData(kGuidTexEye, &sz, &m)) && sz == sizeof(m) && m == (UINT32)g_cfgGen;
}

// ---------------------------------------------------------------------------------------------
// Classification

bool HasMark(ID3D11Texture2D *t)
{
    UINT32 m = 0;
    UINT sz = sizeof(m);
    return SUCCEEDED(t->GetPrivateData(kGuidTexMark, &sz, &m)) && sz == sizeof(m) && m != 0;
}

bool IsSrgbTyped(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// Decide how to read the source, what to allocate for the proxy and how to write it, based on
// the format the compositor's own view interprets the texture as.
bool MapFormats(DXGI_FORMAT texFmt, DXGI_FORMAT viewFmt, Plan &p)
{
    switch (viewFmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        p.proxyFmt = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        p.uavFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        p.readFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        p.proxyFmt = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        p.uavFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        p.readFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        p.proxyFmt = DXGI_FORMAT_B8G8R8X8_TYPELESS;
        p.uavFmt = DXGI_FORMAT_B8G8R8X8_UNORM;
        p.readFmt = DXGI_FORMAT_B8G8R8X8_UNORM;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        p.proxyFmt = DXGI_FORMAT_R10G10B10A2_TYPELESS;
        p.uavFmt = p.readFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
        break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        p.proxyFmt = DXGI_FORMAT_R16G16B16A16_TYPELESS;
        p.uavFmt = p.readFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        p.proxyFmt = DXGI_FORMAT_R16G16B16A16_TYPELESS;
        p.uavFmt = p.readFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        p.xform = kFloat;
        break;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        p.proxyFmt = p.uavFmt = p.readFmt = DXGI_FORMAT_R11G11B10_FLOAT;
        p.xform = kFloat;
        break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        p.proxyFmt = DXGI_FORMAT_R32G32B32A32_TYPELESS;
        p.uavFmt = p.readFmt = DXGI_FORMAT_R32G32B32A32_FLOAT;
        p.xform = kFloat;
        break;
    default:
        return false;
    }
    // A fully typed sRGB texture can only be viewed as sRGB: we get linear values and must
    // re-encode them before FSR. Typeless or UNORM textures are read raw.
    if (IsSrgbTyped(texFmt)) {
        p.readFmt = texFmt;
        p.xform = kSrgb;
    }
    return true;
}

bool SizeIn(const std::vector<Size2> &list, UINT w, UINT h)
{
    for (auto &s : list)
        if ((UINT)s.w == w && (UINT)s.h == h) return true;
    return false;
}

const char *CheckRules(const D3D11_TEXTURE2D_DESC &td, const D3D11_SHADER_RESOURCE_VIEW_DESC &sd, const Config &cfg, Plan &plan,
                       bool eye)
{
    if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D && sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
        return "view is not Texture2D/Texture2DArray";
    UINT mip = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? sd.Texture2D.MostDetailedMip : sd.Texture2DArray.MostDetailedMip;
    if (mip != 0) return "view starts below mip 0";
    if (td.SampleDesc.Count != 1) return "multisampled";
    if (td.MipLevels != 1) return "has mipmaps (not an eye texture)";
    if ((int)td.ArraySize > cfg.maxArraySize) return "texture array too large";
    if (td.Usage != D3D11_USAGE_DEFAULT || td.CPUAccessFlags != 0) return "not a GPU-only texture";
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return "no shader-resource binding";
    if (!cfg.onlySizes.empty()) {
        if (!SizeIn(cfg.onlySizes, td.Width, td.Height)) return "not in onlySizes";
    } else {
        if ((int)td.Width < cfg.minWidth || (int)td.Height < cfg.minHeight) return "too small for an eye texture";
        float aspect = (float)td.Width / (float)td.Height;
        if (aspect < cfg.minAspect || aspect > cfg.maxAspect) return "too wide or tall for an eye texture";
    }
    if (!eye && SizeIn(cfg.excludeSizes, td.Width, td.Height)) return kUntickedReason;
    if (!MapFormats(td.Format, sd.Format, plan)) return "unsupported format";

    double ow, oh;
    plan.scaleX = plan.scaleY = (double)std::max(1.0f, cfg.upscaleFactor);   // 1: same size (Native)
    ow = (double)td.Width * plan.scaleX;
    oh = (double)td.Height * plan.scaleY;
    plan.outW = (UINT)std::min(16384.0, floor(ow + 0.5));
    plan.outH = (UINT)std::min(16384.0, floor(oh + 0.5));
    if (ow > 16384.0) plan.scaleX = (double)plan.outW / (double)td.Width;
    if (oh > 16384.0) plan.scaleY = (double)plan.outH / (double)td.Height;
    if (plan.outW < td.Width || plan.outH < td.Height) return "output would be smaller";
    return nullptr;
}

void RecordReject(ID3D11ShaderResourceView *v, int token = -1, bool unticked = false)
{
    RejectRecord r = { g_cfgGen, g_markGen, token, unticked ? 1 : 0 };
    v->SetPrivateData(kGuidViewReject, sizeof(r), &r);
    InterlockedIncrement(&g_stats.rejects);
}

void LogRejectOnce(ID3D11Texture2D *tex, const D3D11_TEXTURE2D_DESC &td, const D3D11_SHADER_RESOURCE_VIEW_DESC &sd,
                   bool interesting, const char *why)
{
    if (!interesting && g_logLevel < 2) return;
    LONG gen = g_cfgGen;
    LONG prev = 0;
    UINT sz = sizeof(prev);
    if (tex && SUCCEEDED(tex->GetPrivateData(kGuidTexLogged, &sz, &prev)) && prev == gen) return;
    if (tex) tex->SetPrivateData(kGuidTexLogged, sizeof(gen), &gen);
    LogWrite(interesting ? 1 : 2, "skip: %ux%u x%u %s (view %s) misc=0x%x bind=0x%x%s: %s", td.Width, td.Height,
             td.ArraySize, FormatName(td.Format), FormatName(sd.Format), td.MiscFlags, td.BindFlags,
             interesting ? " [shared]" : "", why);
}

Ref<TexState> GetOrCreateTexState(DevState *ds, ID3D11Texture2D *tex, const D3D11_TEXTURE2D_DESC &td, const Plan &plan,
                                  const char **why)
{
    Ref<TexState> ts;
    IUnknown *unk = nullptr;
    UINT sz = sizeof(unk);
    if (SUCCEEDED(tex->GetPrivateData(kGuidTexState, &sz, &unk)) && unk)
        ts = Ref<TexState>(static_cast<TexState *>(unk), false);
    if (ts && ts->gen == g_cfgGen && ts->plan.outW == plan.outW && ts->plan.outH == plan.outH &&
        ts->plan.proxyFmt == plan.proxyFmt && ts->plan.uavFmt == plan.uavFmt) {
        if (ts->failed) {
            *why = "proxy allocation failed earlier";
            return Ref<TexState>();
        }
        return ts;
    }

    Ref<TexState> fresh(new TexState(), false);
    fresh->gen = g_cfgGen;
    fresh->serial = (uint64_t)InterlockedIncrement(&g_texSerial);
    fresh->src = td;
    fresh->plan = plan;
    // what is known about the texture's content carries over to its new plan (same texture)
    if (ts && ts->probe && ts->probe->dev == ds->dev.Get()) {
        fresh->probe = ts->probe;
        ts->probe.Reset();
    }

    D3D11_TEXTURE2D_DESC pd = {};
    pd.Width = plan.outW;
    pd.Height = plan.outH;
    pd.MipLevels = 1;
    pd.ArraySize = td.ArraySize;
    pd.Format = plan.proxyFmt;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = ds->dev->CreateTexture2D(&pd, nullptr, fresh->proxy.Put());
    if (SUCCEEDED(hr)) {
        UINT32 ours = 1;
        fresh->proxy->SetPrivateData(kGuidOurs, sizeof(ours), &ours);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = plan.uavFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.MipSlice = 0;
        ud.Texture2DArray.FirstArraySlice = 0;
        ud.Texture2DArray.ArraySize = td.ArraySize;
        hr = ds->dev->CreateUnorderedAccessView(fresh->proxy.Get(), &ud, fresh->proxyUAV.Put());
    }
    if (FAILED(hr)) {
        LOGE("proxy %ux%u x%u %s allocation failed (0x%08lx)", plan.outW, plan.outH, td.ArraySize,
             FormatName(plan.proxyFmt), (unsigned long)hr);
        fresh->failed = true;
        fresh->proxy.Reset();
        fresh->proxyUAV.Reset();
        *why = "proxy allocation failed";
    }
    tex->SetPrivateDataInterface(kGuidTexState, static_cast<IUnknown *>(fresh.Get()));
    if (fresh->failed) return Ref<TexState>();
    LOGI("upscale: source %ux%u x%u %s -> proxy %ux%u %s, input %s", td.Width, td.Height, td.ArraySize,
         FormatName(td.Format), plan.outW, plan.outH, FormatName(plan.proxyFmt), XformName(plan.xform));
    return fresh;
}

ViewEntry *Classify(CtxState *cs, DevState *ds, ID3D11ShaderResourceView *v)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    v->GetDesc(&sd);
    Ref<ID3D11Resource> res;
    v->GetResource(res.Put());
    if (!res) {
        RecordReject(v);
        return nullptr;
    }
    D3D11_RESOURCE_DIMENSION rd = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&rd);
    if (rd != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        RecordReject(v);
        return nullptr;
    }
    Ref<ID3D11Texture2D> tex;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)tex.Put())) || !tex) {
        RecordReject(v);
        return nullptr;
    }
    UINT32 ours = 0;
    UINT osz = sizeof(ours);
    if (SUCCEEDED(tex->GetPrivateData(kGuidOurs, &osz, &ours)) && ours) {
        RecordReject(v);   // one of our own proxies came back (e.g. via PSGetShaderResources)
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    const bool interesting = (td.MiscFlags & kSharedMask) != 0 || HasMark(tex.Get());

    Plan plan;
    const char *why = nullptr;
    AcquireSRWLockShared(&g_cfgLock);
    if (g_cfg.requireShared && !interesting) why = "SteamVR's own texture (not from an app)";
    else why = CheckRules(td, sd, g_cfg, plan, HasEyeMark(tex.Get()));
    ReleaseSRWLockShared(&g_cfgLock);

    if (!why && !UavStoreSupported(ds, plan.uavFmt)) why = "GPU cannot store this format from a compute shader";

    Ref<TexState> ts;
    if (!why) ts = GetOrCreateTexState(ds, tex.Get(), td, plan, &why);

    Ref<ViewEntry> ve;
    if (!why) {
        ve = Ref<ViewEntry>(new ViewEntry(), false);
        ve->gen = g_cfgGen;
        ve->tex = ts;
        ve->first = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY ? sd.Texture2DArray.FirstArraySlice : 0;
        ve->count = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY ? sd.Texture2DArray.ArraySize : 1;
        if (ve->first + ve->count > td.ArraySize) ve->count = td.ArraySize > ve->first ? td.ArraySize - ve->first : 0;
        if (ve->count == 0) why = "view covers no slices";
        D3D11_SHADER_RESOURCE_VIEW_DESC rdsc = {};
        rdsc.Format = plan.readFmt;
        rdsc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        rdsc.Texture2DArray.MostDetailedMip = 0;
        rdsc.Texture2DArray.MipLevels = 1;
        rdsc.Texture2DArray.FirstArraySlice = 0;
        rdsc.Texture2DArray.ArraySize = td.ArraySize;
        HRESULT hr = why ? E_FAIL : ds->dev->CreateShaderResourceView(tex.Get(), &rdsc, ve->srcRead.Put());
        if (why) {
        } else if (FAILED(hr)) {
            LOGE("could not create a %s read view of the source (0x%08lx)", FormatName(plan.readFmt), (unsigned long)hr);
            why = "read view creation failed";
        } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC pdsc = sd;
            if (pdsc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
                pdsc.Texture2D.MostDetailedMip = 0;
                pdsc.Texture2D.MipLevels = 1;
            } else {
                pdsc.Texture2DArray.MostDetailedMip = 0;
                pdsc.Texture2DArray.MipLevels = 1;
            }
            hr = ds->dev->CreateShaderResourceView(ts->proxy.Get(), &pdsc, ve->proxySRV.Put());
            if (FAILED(hr)) {
                LOGE("could not create the proxy view (%s, 0x%08lx)", FormatName(pdsc.Format), (unsigned long)hr);
                why = "proxy view creation failed";
            }
        }
    }

    if (why) {
        // shared textures of a plausible size are listed in the app, with the reason
        const int token = (interesting && td.Width >= 256 && td.Height >= 256) ? ControlNoteTexture(td, false, 0, 0, why) : -1;
        RecordReject(v, token, why == kUntickedReason);
        LogRejectOnce(tex.Get(), td, sd, interesting, why);
        return nullptr;
    }
    ve->ctlToken = ControlNoteTexture(td, true, plan.outW, plan.outH, "");

    v->SetPrivateDataInterface(kGuidViewEntry, static_cast<IUnknown *>(ve.Get()));
    UINT first = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY ? sd.Texture2DArray.FirstArraySlice : 0;
    UINT count = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY ? sd.Texture2DArray.ArraySize : 1;
    LOGI("upscale: view %p (%s, %s slices %u..%u) now samples the %ux%u proxy", (void *)v, FormatName(sd.Format),
         sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? "2D" : "2DArray", first, first + count - 1,
         plan.outW, plan.outH);
    (void)cs;
    return ve.Detach();
}

DevState *EnsureDevState(CtxState *cs)
{
    if (!cs->ds) cs->ds = GetDevState(cs->dev.Get());
    return cs->ds;
}

// Returns an owned reference, or nullptr if the view is not (or not yet) eligible (*unticked: only
// because its size was unticked in the app).
ViewEntry *Lookup(CtxState *cs, ID3D11ShaderResourceView *v, bool *unticked)
{
    IUnknown *unk = nullptr;
    UINT sz = sizeof(unk);
    if (SUCCEEDED(v->GetPrivateData(kGuidViewEntry, &sz, &unk)) && unk) {
        ViewEntry *ve = static_cast<ViewEntry *>(unk);
        if (ve->gen == g_cfgGen && !ve->tex->failed) {
            ControlTouchTexture(ve->ctlToken);
            return ve;
        }
        ve->Release();
        v->SetPrivateDataInterface(kGuidViewEntry, nullptr);
    }
    RejectRecord r = {};
    sz = sizeof(r);
    if (SUCCEEDED(v->GetPrivateData(kGuidViewReject, &sz, &r)) && sz == sizeof(r) && r.cfgGen == g_cfgGen &&
        r.markGen == g_markGen) {
        ControlTouchTexture(r.token);
        *unticked = r.unticked != 0;
        return nullptr;
    }
    DevState *ds = EnsureDevState(cs);
    if (!ds || !ds->ok) return nullptr;   // not ready yet: decide later, cache nothing
    ViewEntry *ve = Classify(cs, ds, v);
    if (!ve) {
        sz = sizeof(r);
        *unticked = SUCCEEDED(v->GetPrivateData(kGuidViewReject, &sz, &r)) && sz == sizeof(r) && r.unticked != 0;
    }
    return ve;
}

// ---------------------------------------------------------------------------------------------
// Upscale dispatch

bool WriteParams(ID3D11DeviceContext *ctx, DevState *ds, const Params &p)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(ds->cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(ds->cb.Get(), 0);
    return true;
}

bool EnsureScratch(DevState *ds, UINT w, UINT h, UINT slices)
{
    if (ds->scratch && ds->sw >= w && ds->sh >= h && ds->ss >= slices) return true;
    UINT nw = std::max(w, ds->sw), nh = std::max(h, ds->sh), ns = std::max(slices, ds->ss);
    DeferRelease(ds->scratchUAV.Detach());
    DeferRelease(ds->scratchSRV.Detach());
    DeferRelease(ds->scratch.Detach());
    ds->sw = ds->sh = ds->ss = 0;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = nw;
    d.Height = nh;
    d.MipLevels = 1;
    d.ArraySize = ns;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = ds->dev->CreateTexture2D(&d, nullptr, ds->scratch.Put());
    if (SUCCEEDED(hr)) {
        UINT32 ours = 1;
        ds->scratch->SetPrivateData(kGuidOurs, sizeof(ours), &ours);
        D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = d.Format;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        s.Texture2DArray.MipLevels = 1;
        s.Texture2DArray.ArraySize = ns;
        hr = ds->dev->CreateShaderResourceView(ds->scratch.Get(), &s, ds->scratchSRV.Put());
    }
    if (SUCCEEDED(hr)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = d.Format;
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.ArraySize = ns;
        hr = ds->dev->CreateUnorderedAccessView(ds->scratch.Get(), &u, ds->scratchUAV.Put());
    }
    if (FAILED(hr)) {
        LOGE("scratch %ux%u x%u allocation failed (0x%08lx)", nw, nh, ns, (unsigned long)hr);
        ds->scratch.Reset(); ds->scratchSRV.Reset(); ds->scratchUAV.Reset();
        return false;
    }
    ds->sw = nw; ds->sh = nh; ds->ss = ns;
    LOGV("scratch resized to %ux%u x%u", nw, nh, ns);
    return true;
}

void PollQueries(DevState *ds, ID3D11DeviceContext *ctx)
{
    for (auto &q : ds->q) {
        if (!q.busy) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ctx->GetData(q.dj.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 a = 0, b = 0;
        bool okA = ctx->GetData(q.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        bool okB = ctx->GetData(q.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (okA && okB && !dj.Disjoint && dj.Frequency && b >= a) {
            double ms = double(b - a) * 1000.0 / double(dj.Frequency);
            ds->gpuMsSum += ms;
            ds->gpuMsMax = std::max(ds->gpuMsMax, ms);
            ds->gpuSamples++;
            ds->gpuEma = ds->gpuEma < 0 ? ms : 0.9 * ds->gpuEma + 0.1 * ms;
            ds->gpuEmaTick = GetTickCount();
        }
        q.busy = false;
    }
}

QuerySet *AcquireQuerySet(DevState *ds, ID3D11DeviceContext *ctx)
{
    PollQueries(ds, ctx);
    for (auto &q : ds->q)
        if (!q.busy && q.dj) return &q;
    return nullptr;
}

struct TJob {
    StreamLink *link;
    ViewEntry *ve;
};

struct SJob {
    ViewEntry *ve;
    UINT first, count;   // array slices to process
    int side;            // QJob::side
};

void SpatialUpscale(ID3D11DeviceContext *ctx, DevState *ds, const SJob &job, LONGLONG now)
{
    ViewEntry *ve = job.ve;
    TexState *ts = ve->tex.Get();
    const UINT w = ts->src.Width, h = ts->src.Height, slices = ts->src.ArraySize;
    const UINT first = job.first, count = job.count;
    const UINT ow = ts->plan.outW, oh = ts->plan.outH;
    const UINT gx = (ow + 15) / 16, gy = (oh + 15) / 16;
    const int x = ts->plan.xform;
    const float sharp = g_cfg.sharpness;
    const bool fsr = g_cfg.mode != Mode::Bilinear;   // temporal mode falls back to FSR 1
    UINT finalFlags = FLAG_FINAL | (g_cfg.tint ? FLAG_TINT : 0u);
    // FSR 1 in the area only (as Temporal: centred, or around the gaze), a cheap upscale outside
    SpatialArea area;
    const bool fov = fsr && PlaceSpatialArea(ts, first, count, job.side, area);
    const UINT ax = fov ? (area.region[0][2] + 15) / 16 : gx, ay = fov ? (area.region[0][3] + 15) / 16 : gy;
    auto Common = [&](Params &q) {
        q.fov[0] = area.band;
        q.fov[1] = fov ? FOV_ON | (g_cfg.bilinearEdges ? FOV_BILINEAR : 0u) : 0u;
        q.fov[2] = w; q.fov[3] = h;
        memcpy(q.content, area.content, sizeof(q.content));
        memcpy(q.region, area.region, sizeof(q.region));
        memcpy(q.marks, area.marks, sizeof(q.marks));   // (all ~0 unless FSR 1 shows the gaze)
    };

    // Upscale only if the content changed since this proxy range was written (the compositor draws
    // every texture of the app's swapchain each frame, and every app frame for several display
    // frames). Decided on the GPU; the passes below run indirectly.
    int slot = (first == 0 && count == slices) ? 0 : -1;
    for (int i = 0; slot < 0 && i < ts->nlinks; ++i)
        if (ts->links[i].first == first && ts->links[i].count == count) slot = 1 + i;
    ID3D11Buffer *gateArgs = nullptr;
    if (slot >= 0) {
        ContentGate &g = ts->gates[slot];
        if (RunContentGate(ctx, ds, ts, ve->srcRead.Get(), first, count, x, g, g.outGen != g_outGen, ax, ay, count, gx, gy))
            gateArgs = ds->gateArgs.Get();
    }
    // entries 0 and 1: EASU and RCAS (over the area when foveated), 2: OUTER (the whole output)
    auto Run = [&](UINT argOffset) {
        if (gateArgs) ctx->DispatchIndirect(gateArgs, argOffset);
        else if (argOffset == 24) ctx->Dispatch(gx, gy, count);
        else ctx->Dispatch(ax, ay, count);
    };
    ID3D11Buffer *cb = ds->cb.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);

    bool rcas = fsr && sharp > 0.0f;
    if (rcas && !EnsureScratch(ds, ow, oh, slices)) rcas = false;

    Params p = {};
    p.outSize[0] = ow; p.outSize[1] = oh;
    p.inSize[0] = w; p.inSize[1] = h;
    p.pad[0] = first;
    Common(p);
    if (fsr) {
        FsrEasuCon(p.con0, p.con1, p.con2, p.con3, (AF1)w, (AF1)h, (AF1)w, (AF1)h, (AF1)ow, (AF1)oh);
        p.flags = rcas ? 0u : finalFlags;
        if (!WriteParams(ctx, ds, p)) return;
        ctx->CSSetShader(ds->cs[SH_FSR_EASU_X0 + x].Get(), nullptr, 0);
    } else {
        p.flags = finalFlags;
        if (!WriteParams(ctx, ds, p)) return;
        ctx->CSSetShader(ds->cs[SH_FSR_BILIN_X0 + x].Get(), nullptr, 0);
    }
    BindCS(ctx, { ve->srcRead.Get(), ve->srcRead.Get() }, { rcas ? ds->scratchUAV.Get() : ts->proxyUAV.Get() });
    Run(0);

    if (rcas) {
        Params r = {};
        FsrRcasCon(r.con0, 2.0f - 2.0f * sharp);
        r.outSize[0] = ow; r.outSize[1] = oh;
        r.inSize[0] = ow; r.inSize[1] = oh;
        r.flags = finalFlags;   // scratch already holds perceptual values; only the output transform applies
        r.pad[0] = first;
        Common(r);
        if (WriteParams(ctx, ds, r)) {
            ctx->CSSetShader(ds->cs[SH_FSR_RCAS_X0 + x].Get(), nullptr, 0);
            BindCS(ctx, { ds->scratchSRV.Get(), ve->srcRead.Get() }, { ts->proxyUAV.Get() });
            Run(12);
        }
    }

    if (fov) {   // outside the area: this frame alone, Catmull-Rom (or bilinear)
        Params o = {};
        o.outSize[0] = ow; o.outSize[1] = oh;
        o.inSize[0] = w; o.inSize[1] = h;
        o.flags = finalFlags;
        o.pad[0] = first;
        Common(o);
        if (WriteParams(ctx, ds, o)) {
            ctx->CSSetShader(ds->cs[SH_FSR_OUTER_X0 + x].Get(), nullptr, 0);
            BindCS(ctx, { ve->srcRead.Get(), ve->srcRead.Get() }, { ts->proxyUAV.Get() });
            Run(24);
        }
    }

    if (ts->upscales == 0)
        LOGI("upscale: first spatial dispatch %ux%u slices %u..%u -> %ux%u (%s%s%s)", w, h, first, first + count - 1, ow, oh,
             fsr ? "EASU" : "bilinear", rcas ? "+RCAS" : "", fov ? ", in the area only" : "");
    (void)now;
    ts->upscales++;
    InterlockedIncrement(&g_stats.upscales);
    InterlockedIncrement(&g_iv.upscales);
    int k = 0;
    for (; k < g_tallyCount; ++k)
        if (g_tally[k].w == w && g_tally[k].h == h && g_tally[k].slices == slices && g_tally[k].fmt == ts->src.Format) break;
    if (k == g_tallyCount && g_tallyCount < 8) g_tally[g_tallyCount++] = { w, h, slices, ow, oh, ts->src.Format, 0 };
    if (k < g_tallyCount) g_tally[k].count++;
}

void RunJobs(CtxState *cs, SJob *spatial, int ns, TJob *tj, int nt, LONGLONG now)
{
    DevState *ds = cs->ds;
    ID3D11DeviceContext *ctx = cs->ctx;

    // Snapshot every compute-stage binding our passes touch.
    Ref<ID3D11ComputeShader> oldCS;
    ctx->CSGetShader(oldCS.Put(), nullptr, nullptr);
    ID3D11ShaderResourceView *oldSRV[kCsSrvSlots] = {};
    ctx->CSGetShaderResources(0, kCsSrvSlots, oldSRV);
    ID3D11UnorderedAccessView *oldUAV[kCsUavSlots] = {};
    ctx->CSGetUnorderedAccessViews(0, kCsUavSlots, oldUAV);
    ID3D11Buffer *oldCB = nullptr;
    UINT cbFirst = 0, cbNum = 0;
    if (cs->ctx1) cs->ctx1->CSGetConstantBuffers1(0, 1, &oldCB, &cbFirst, &cbNum);
    else ctx->CSGetConstantBuffers(0, 1, &oldCB);
    ID3D11SamplerState *oldSamp = nullptr;
    ctx->CSGetSamplers(0, 1, &oldSamp);
    // Binding a proxy as a UAV makes the runtime silently unbind it from PS slots; remember them.
    const UINT nps = (UINT)(cs->hi + 1);
    ID3D11ShaderResourceView *oldPS[kSlots] = {};
    if (nps > 0) ctx->PSGetShaderResources(0, nps, oldPS);

    // With passTiming the temporal passes carry their own disjoint query; don't nest another.
    // Deferred contexts cannot read query results, so only immediate-context work is timed.
    QuerySet *qs = (cs->immediate && g_cfg.gpuTiming && !g_cfg.passTiming) ? AcquireQuerySet(ds, ctx) : nullptr;
    if (cs->immediate) InterlockedIncrement(&g_jobBatches);
    if (qs) {
        ctx->Begin(qs->dj.Get());
        ctx->End(qs->t0.Get());
    }
    ID3D11SamplerState *samp = ds->samp.Get();
    ctx->CSSetSamplers(0, 1, &samp);

    for (int i = 0; i < ns; ++i) SpatialUpscale(ctx, ds, spatial[i], now);
    for (int i = 0; i < nt; ++i) RunStream(ctx, ds, tj[i].link, tj[i].ve, now);

    if (qs) {
        ctx->End(qs->t1.Get());
        ctx->End(qs->dj.Get());
        qs->busy = true;
    }

    // Restore compute state exactly as it was (ours unbound first, so nothing conflicts).
    BindCS(ctx, {}, {});
    ctx->CSSetShader(oldCS.Get(), nullptr, 0);
    if (g_origCSSetShaderResources) g_origCSSetShaderResources(ctx, 0, kCsSrvSlots, oldSRV);
    else ctx->CSSetShaderResources(0, kCsSrvSlots, oldSRV);
    UINT keep[kCsUavSlots];
    for (UINT i = 0; i < kCsUavSlots; ++i) keep[i] = (UINT)-1;
    ctx->CSSetUnorderedAccessViews(0, kCsUavSlots, oldUAV, keep);
    if (cs->ctx1 && oldCB && (cbFirst != 0 || cbNum != D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT))
        cs->ctx1->CSSetConstantBuffers1(0, 1, &oldCB, &cbFirst, &cbNum);   // keep D3D11.1 offsets
    else
        ctx->CSSetConstantBuffers(0, 1, &oldCB);
    ctx->CSSetSamplers(0, 1, &oldSamp);
    for (UINT i = 0; i < kCsSrvSlots; ++i) if (oldSRV[i]) oldSRV[i]->Release();
    for (UINT i = 0; i < kCsUavSlots; ++i) if (oldUAV[i]) oldUAV[i]->Release();
    if (oldCB) oldCB->Release();
    if (oldSamp) oldSamp->Release();

    // Re-bind the pixel-shader views (including the proxies the UAV binding just evicted).
    if (nps > 0) {
        g_origPSSetShaderResources(ctx, 0, nps, oldPS);
        for (UINT s = 0; s < nps; ++s)
            if (oldPS[s]) oldPS[s]->Release();
    }
}

// ---------------------------------------------------------------------------------------------
// Render-thread housekeeping: deferred releases, hotkeys, config reload, stats.

typedef SHORT (WINAPI *PFN_GetAsyncKeyState)(int);
PFN_GetAsyncKeyState g_getKey = nullptr;
bool g_keyWasDown[8] = {};
volatile LONG g_testHotkey = -1;   // a shortcut the self-test asks for (VrUpscaleDebugHotkey)
SRWLOCK g_tickLock = SRWLOCK_INIT;
LONGLONG g_lastHotkey = 0, g_lastCfgPoll = 0, g_lastStats = 0, g_lastCounters = 0, g_lastProbePoll = 0;

// Ctrl+F1..F8. Goes through ApplyConfig like any other change, so a new factor re-plans the
// textures; the app mirrors the result and saves it.
void ApplyHotkey(int idx)
{
    AcquireSRWLockShared(&g_cfgLock);
    Config nc = g_cfg;
    ReleaseSRWLockShared(&g_cfgLock);
    switch (idx) {
    case 0:
        nc.enabled = !nc.enabled;
        LOGI("hotkey: upscaling %s", nc.enabled ? "ON" : "OFF");
        break;
    case 1:
        nc.mode = nc.mode == Mode::Temporal ? Mode::Fsr : (nc.mode == Mode::Fsr ? Mode::Bilinear : Mode::Temporal);
        LOGI("hotkey: mode %s", ModeName(nc.mode));
        break;
    case 2:
        nc.tint = !nc.tint;
        LOGI("hotkey: debug tint %s", nc.tint ? "ON" : "OFF");
        break;
    case 3: {
        float s = nc.sharpness + 0.25f;
        nc.sharpness = s > 1.001f ? 0.0f : s;
        LOGI("hotkey: sharpness %.2f%s", nc.sharpness, nc.sharpness == 0.0f ? " (RCAS off)" : "");
        break;
    }
    case 4: {
        const int p = VuScaleNext(nc.upscaleFactor);
        LOGI("hotkey: scaling %s (%.2fx, was %.2fx)", kVuScaleName[p], kVuScaleFactor[p], nc.upscaleFactor);
        nc.upscaleFactor = kVuScaleFactor[p];
        break;
    }
    case 5:
        nc.sharpHistory = !nc.sharpHistory;
        LOGI("hotkey: sharp history %s", nc.sharpHistory ? "ON" : "OFF");
        break;
    case 6:
        nc.bilinearEdges = !nc.bilinearEdges;
        LOGI("hotkey: bilinear edges %s", nc.bilinearEdges ? "ON" : "OFF");
        break;
    case 7:   // eye tracking: calibrate (look straight ahead at the cross)
        GazeStartCalibration("Ctrl+F8");
        return;
    default: return;
    }
    ApplyConfig(nc, "a shortcut");
}

void PollHotkeys()
{
    if (!g_getKey) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (!u) u = LoadLibraryW(L"user32.dll");
        if (u) g_getKey = (PFN_GetAsyncKeyState)GetProcAddress(u, "GetAsyncKeyState");
        if (!g_getKey) return;
    }
    bool ctrl = (g_getKey(VK_CONTROL) & 0x8000) != 0;
    for (int i = 0; i < 8; ++i) {
        bool down = (g_getKey(VK_F1 + i) & 0x8000) != 0;
        if (ctrl && down && !g_keyWasDown[i]) ApplyHotkey(i);
        g_keyWasDown[i] = down;
    }
}

void ReloadConfig()
{
    Config nc;
    bool found = LoadConfig(nc);
    LOGI("config: settings file %ls%s", g_iniPath.c_str(), found ? "" : " (missing: defaults)");
    ApplyConfig(nc, "the settings file");
    PrintConfig(nc);
}

void LogStats(ID3D11DeviceContext *ctx, double seconds)
{
    LONG ps = InterlockedExchange(&g_iv.ps, 0);
    LONG subs = InterlockedExchange(&g_iv.subs, 0);
    LONG ups = InterlockedExchange(&g_iv.upscales, 0);
    double gpuSum = 0, gpuMax = 0;
    int gpuN = 0;
    AcquireSRWLockShared(&g_devLock);
    for (int i = 0; i < g_devCount; ++i) {
        gpuSum += g_devs[i]->gpuMsSum;
        gpuMax = std::max(gpuMax, g_devs[i]->gpuMsMax);
        gpuN += g_devs[i]->gpuSamples;
        g_devs[i]->gpuMsSum = 0;
        g_devs[i]->gpuMsMax = 0;
        g_devs[i]->gpuSamples = 0;
    }
    ReleaseSRWLockShared(&g_devLock);
    char gpu[64];
    if (gpuN) snprintf(gpu, sizeof(gpu), "%.3f ms avg, %.3f ms max", gpuSum / gpuN, gpuMax);
    else snprintf(gpu, sizeof(gpu), "%s", ups ? "not timed yet" : "-");
    LOGI("stats %.0fs: binds=%ld substituted=%ld upscales=%ld (%.1f/s) gpu=%s | live textures=%ld views=%ld | "
         "totals: opened=%ld copied=%ld skipped=%ld | %s",
         seconds, (long)ps, (long)subs, (long)ups, seconds > 0 ? ups / seconds : 0.0, gpu,
         (long)g_stats.liveTexStates, (long)g_stats.liveViewEntries, (long)g_stats.sharedOpened,
         (long)g_stats.copiesMarked, (long)g_stats.rejects, g_cfg.enabled ? "ON" : "OFF");
    {
        char hits[256];
        FormatHookVariantHits(hits, sizeof(hits));
        if (hits[0]) LOGI("stats:   %s", hits);
    }
    for (int k = 0; k < g_tallyCount; ++k)
        LOGI("stats:   spatial %ux%u x%u %s -> %ux%u: %ld draws checked (upscaled when the content changed)", g_tally[k].w,
             g_tally[k].h, g_tally[k].slices,
             FormatName(g_tally[k].fmt), g_tally[k].ow, g_tally[k].oh, (long)g_tally[k].count);
    g_tallyCount = 0;
    Ref<ID3D11Device> dev;
    ctx->GetDevice(dev.Put());
    TemporalLogStats(ctx, dev.Get(), seconds);
    if (DevState *ds = GetDevState(dev.Get())) {
        PollPassQueries(ds, ctx, false);
        if (ds->passRuns > 0) {
            char line[512];
            int len = 0;
            double total = 0;
            for (int i = 0; i < kTPasses; ++i) total += ds->passMs[i];
            for (int i = 0; i < kTPasses && len < (int)sizeof(line) - 40; ++i)
                len += snprintf(line + len, sizeof(line) - len, "%s%s %.3f", i ? ", " : "", kTPassNames[i],
                                ds->passMs[i] / ds->passRuns);
            LOGI("stats:   temporal passes, GPU ms per eye (%d runs, total %.3f): %s", ds->passRuns, total / ds->passRuns, line);
            for (auto &v : ds->passMs) v = 0;
            ds->passRuns = 0;
        }
    }
}

// Immediate context only (it reads query results and maps staging copies). Caller holds g_workLock
// and the device lock (MtGuard).
void RenderTick(ID3D11DeviceContext *ctx)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_lastTickQpc = t.QuadPart;
    TemporalSweep();
    DrainDeferred();
    if (!TryAcquireSRWLockExclusive(&g_tickLock)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG f = g_qpf.QuadPart;
    if (g_cfg.hotkeys && now.QuadPart - g_lastHotkey > f / 20) {
        g_lastHotkey = now.QuadPart;
        PollHotkeys();
    }
    const LONG testKey = InterlockedExchange(&g_testHotkey, -1);
    if (testKey >= 0) ApplyHotkey((int)testKey);
    GazeTick();
    if (g_reloadRequested || now.QuadPart - g_lastCfgPoll > f) {
        g_lastCfgPoll = now.QuadPart;
        if (InterlockedExchange(&g_reloadRequested, 0) || ConfigFileChanged()) ReloadConfig();
    }
    if (now.QuadPart - g_lastProbePoll > f / 20) {   // content rectangles (render size)
        g_lastProbePoll = now.QuadPart;
        Ref<ID3D11Device> dev;
        ctx->GetDevice(dev.Put());
        PollProbes(ctx, dev.Get());
    }
    if (now.QuadPart - g_lastCounters > f / 2) {   // GPU counters for the app's frame rate
        g_lastCounters = now.QuadPart;
        Ref<ID3D11Device> dev;
        ctx->GetDevice(dev.Put());
        TemporalPollCounters(ctx, dev.Get());
    }
    if (g_lastStats == 0) g_lastStats = now.QuadPart;
    if (g_cfg.statsIntervalSec > 0 && now.QuadPart - g_lastStats > f * g_cfg.statsIntervalSec) {
        double secs = double(now.QuadPart - g_lastStats) / double(f);
        g_lastStats = now.QuadPart;
        LogStats(ctx, secs);
    }
    ReleaseSRWLockExclusive(&g_tickLock);
    ControlTick();
}

volatile LONG g_csDiagBudget = 20000;
volatile LONG g_openLogBudget = 256;
volatile LONG g_copyLogBudget = 32;

} // namespace

volatile LONGLONG g_lastTickQpc = 0;
volatile LONG g_outGen = 0;
volatile LONG g_jobBatches = 0;

void UpscalerInit()
{
    QueryPerformanceFrequency(&g_qpf);
    InitializeCriticalSection(&g_workLock);
}

static bool SameSizes(const std::vector<Size2> &a, const std::vector<Size2> &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].w != b[i].w || a[i].h != b[i].h) return false;
    return true;
}

void ApplyConfig(const Config &nc, const char *source)
{
    Config old;
    AcquireSRWLockShared(&g_cfgLock);
    old = g_cfg;
    ReleaseSRWLockShared(&g_cfgLock);
    // Anything that changes which textures are upscaled, or to what size, re-plans them (new
    // proxies and histories). Everything else applies to the next frame as it is.
    const bool replan = old.upscaleFactor != nc.upscaleFactor ||
                        old.minWidth != nc.minWidth || old.minHeight != nc.minHeight || old.minAspect != nc.minAspect ||
                        old.maxAspect != nc.maxAspect || !SameSizes(old.onlySizes, nc.onlySizes) ||
                        !SameSizes(old.excludeSizes, nc.excludeSizes) || old.requireShared != nc.requireShared ||
                        old.followCopies != nc.followCopies || old.maxArraySize != nc.maxArraySize ||
                        old.maxStreams != nc.maxStreams || old.linkPools != nc.linkPools || old.linkGapMs != nc.linkGapMs ||
                        RegionArea(old) != RegionArea(nc) || old.detectContent != nc.detectContent;
    AcquireSRWLockExclusive(&g_cfgLock);
    g_cfg = nc;
    g_cfg.active = old.active;   // read once at startup
    InterlockedExchange(&g_logLevel, nc.logLevel);
    if (replan) {
        InterlockedIncrement(&g_cfgGen);
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_replanQpc = q.QuadPart;
    }
    InterlockedIncrement(&g_outGen);   // repaint every proxy once with the new settings
    ReleaseSRWLockExclusive(&g_cfgLock);
    if (nc.enabled && !old.enabled) TemporalResetAll("re-enabled");
    else if (nc.mode == Mode::Temporal && old.mode != Mode::Temporal) TemporalResetAll("mode switch");
    char excl[160] = "";
    size_t len = 0;
    for (auto &e : nc.excludeSizes)
        if (len < sizeof(excl) - 16) len += snprintf(excl + len, sizeof(excl) - len, " %dx%d", e.w, e.h);
    LOGI("settings from %s: %s, mode=%s, factor=%.2f, sharpness=%.2f, tint=%d, area=%.2f (following the gaze %.2f), history=%.2f, "
         "moving=%.2f, tolerance=%.2f, %s history, %s edges, gaze %s%s (offset %.3f, %.3f), excluded:%s%s",
         source, nc.enabled ? "on" : "OFF", ModeName(nc.mode), nc.upscaleFactor, nc.sharpness, nc.tint, nc.temporalArea,
         nc.gazeArea, nc.maxWeight, nc.localTrust, nc.residualTol, nc.sharpHistory ? "sharp" : "fast",
         nc.bilinearEdges ? "bilinear" : "Catmull-Rom", nc.gazeFollow ? "followed" : "not followed",
         nc.gazeShow ? ", ring shown" : "", nc.gazeOffset[0], nc.gazeOffset[1], len ? excl : " none",
         replan ? " (re-planning textures)" : "");
}

float GpuMsRecent()
{
    const DWORD now = GetTickCount();
    float best = -1.0f;
    AcquireSRWLockShared(&g_devLock);
    for (int i = 0; i < g_devCount; ++i) {
        DevState *d = g_devs[i];
        if (d->gpuEma >= 0 && now - d->gpuEmaTick < 3000) best = std::max(best, (float)d->gpuEma);
    }
    ReleaseSRWLockShared(&g_devLock);
    return best;
}

void ControlTickFromWorker()
{
    WorkLock lock(false);
    if (lock.held) ControlTick();
}

// ---------------------------------------------------------------------------------------------
// Hook entry points

void OnPSSetShaderResources(ID3D11DeviceContext *ctx, UINT start, UINT num, ID3D11ShaderResourceView *const *views,
                            ID3D11ShaderResourceView **out, bool *replaced)
{
    InterlockedIncrement(&g_stats.psHookCalls);
    InterlockedIncrement(&g_iv.ps);
    CtxState *cs = GetCtx(ctx, "first PSSetShaderResources");
    if (!cs) return;
    WorkLock lock(!cs->immediate);
    MtGuard guard(cs);
    if (lock.held && cs->immediate) RenderTick(ctx);

    const bool enabled = g_cfg.enabled && lock.held;
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        if (ViewEntry *old = cs->bound[slot]) {
            cs->bound[slot] = nullptr;
            cs->boundCount--;
            old->Release();
        }
        if (cs->unticked[slot]) {
            cs->unticked[slot] = nullptr;
            cs->untickedCount--;
        }
        ID3D11ShaderResourceView *v = views[i];
        out[i] = v;
        if (!v || !enabled) continue;
        bool unticked = false;
        ViewEntry *ve = Lookup(cs, v, &unticked);
        if (!ve) {
            if (unticked) {
                cs->unticked[slot] = v;
                cs->untickedCount++;
                if ((int)slot > cs->hi) cs->hi = (int)slot;
            }
            continue;
        }
        out[i] = ve->proxySRV.Get();
        cs->bound[slot] = ve;   // keeps the reference returned by Lookup
        cs->boundCount++;
        if ((int)slot > cs->hi) cs->hi = (int)slot;
        *replaced = true;
        InterlockedIncrement(&g_stats.substitutions);
        InterlockedIncrement(&g_iv.subs);
    }
    if (cs->boundCount == 0 && cs->untickedCount == 0) cs->hi = -1;
}

namespace {

// What the views bound to cs need (one candidate per texture slice range). Resolving the history
// link has to happen here, at the draw: it looks at the render target and viewport.
int CollectJobs(CtxState *cs, ID3D11DeviceContext *ctx, LONGLONG now, QJob *out, int max)
{
    const bool temporal = g_cfg.mode == Mode::Temporal;
    const OutSig sig = CaptureOutSig(ctx);   // which eye (viewport) this draw renders
    const int side = (sig.valid && sig.rtW > 0 && sig.vw > 0 && (UINT)sig.vw < sig.rtW)
                         ? ((sig.vx + sig.vw / 2) < (int)sig.rtW / 2 ? 0 : 1) : -1;
    int n = 0;
    for (int s = 0; s <= cs->hi && n < max; ++s) {
        ViewEntry *ve = cs->bound[s];
        if (!ve) continue;
        TexState *ts = ve->tex.Get();
        if (ts->failed) continue;
        QJob j = { ve, nullptr, 0, ts->src.ArraySize, side };
        StreamLink *link = temporal ? ResolveStream(cs->ds, ctx, ve, now) : nullptr;
        if (link) {
            j.link = link;
            j.first = link->first;
            j.count = link->count;
            if (link->stream) StreamTouch(link, ts, now);
        }
        const int pool = NotePool(sig, ts, link, now);
        {
            const LinkGeo cg = ContentGeo(ts);
            ControlNoteTextureUse(ve->ctlToken, PoolEyeTexture(pool, ts, now), cg.full ? 0u : cg.cw, cg.full ? 0u : cg.ch,
                                  cg.full ? 0u : cg.outW, cg.full ? 0u : cg.outH);
        }
        bool dup = false;
        for (int k = 0; k < n; ++k)
            if (out[k].ve->tex.Get() == ts && out[k].link == j.link && out[k].first == j.first && out[k].count == j.count) dup = true;
        if (!dup) out[n++] = j;
    }
    return n;
}

// Of the candidates, what runs now: each history at most once per new frame, and each spatial
// upscale at most once per dedupe window. Then runs it on cs's context.
void SelectAndRun(CtxState *cs, ID3D11DeviceContext *ctx, const QJob *cand, int nc, LONGLONG now)
{
    const LONGLONG window = (LONGLONG)(g_cfg.dedupeMs * (double)g_qpf.QuadPart / 1000.0);
    const bool temporal = g_cfg.mode == Mode::Temporal;
    SJob spatial[16];
    TJob tj[16];
    int ns = 0, nt = 0;
    for (int i = 0; i < nc && ns < 16 && nt < 16; ++i) {
        ViewEntry *ve = cand[i].ve;
        TexState *ts = ve->tex.Get();
        if (ts->failed) continue;
        // Spatial work is either the whole texture (spatial modes) or just this view's slices
        // (a temporal-mode range without a history), each with its own dedupe stamp.
        LONGLONG *stamp = &ts->lastQpc;
        SJob job = { ve, 0, ts->src.ArraySize, cand[i].side };
        if (temporal && cand[i].link) {
            StreamLink *link = cand[i].link;
            if (link->stream) {
                if (!PoolMixed(link, ts, now)) {
                    bool dup = false;
                    for (int k = 0; k < nt; ++k)
                        if (tj[k].link == link) dup = true;
                    if (!dup && StreamWantsRun(link, ts, now, window)) tj[nt++] = { link, ve };
                    continue;
                }
                // this eye's textures are not all on one history: all of them spatial for now, so
                // the eye does not alternate between temporal and spatial frames
                StreamPause(link);
            }
            stamp = &link->spatialQpc;
            job = { ve, link->first, link->count, cand[i].side };
        }
        if (*stamp != 0 && now - *stamp < window) continue;
        bool dup = false;
        for (int k = 0; k < ns; ++k)
            if (spatial[k].ve->tex.Get() == ts && spatial[k].first == job.first && spatial[k].count == job.count) dup = true;
        if (dup) continue;
        *stamp = now;
        spatial[ns++] = job;
    }
    (void)ctx;
    if (ns + nt > 0) RunJobs(cs, spatial, ns, tj, nt, now);
}

// Work recorded into a command list, attached to it as private data.
const GUID kGuidJobs = { 0x7a3f2c10, 0x51d4, 0x4b8e, { 0x9c, 0x2e, 0x61, 0x0f, 0x3a, 0x77, 0xd1, 0x09 } };

class JobList final : public RefObj {
public:
    std::vector<QJob> jobs;
    ID3D11Resource *written[16] = {};   // what the command list copied into (keys only)
    int nWritten = 0;
protected:
    ~JobList() override
    {
        for (auto &j : jobs) j.ve->Release();
    }
};

volatile LONG g_untickLogBudget = 16;

// A texture left alone only because its size was unticked, drawn into an eye viewport in turn with
// upscaled textures of the same kind: one of the game's swapchain textures (the size was unticked
// to work around the flicker of earlier versions, or the game reallocated). Left alone it would
// make that eye flicker between upscaled and not, so it is marked and upscaled with the others.
// Only other textures count, upscaled since the settings changed: a texture's own draws from before
// it was unticked, or textures of a size unticked as a whole, say nothing.
void NoteUnticked(CtxState *cs, ID3D11DeviceContext *ctx, LONGLONG now)
{
    const OutSig sig = CaptureOutSig(ctx);
    for (int s = 0; s <= cs->hi; ++s) {
        ID3D11ShaderResourceView *v = cs->unticked[s];
        if (!v) continue;
        Ref<ID3D11Resource> res;
        v->GetResource(res.Put());
        Ref<ID3D11Texture2D> tex;
        if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)tex.Put())) || !tex || HasEyeMark(tex.Get()))
            continue;
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        uint64_t self = 0;
        IUnknown *unk = nullptr;
        UINT sz = sizeof(unk);
        if (SUCCEEDED(tex->GetPrivateData(kGuidTexState, &sz, &unk)) && unk) {
            self = static_cast<TexState *>(unk)->serial;
            unk->Release();
        }
        if (!PoolHasUpscaledLike(sig, td.Format, td.Width, td.Height, now, self, g_replanQpc)) continue;
        UINT32 mark = (UINT32)g_cfgGen;
        tex->SetPrivateData(kGuidTexEye, sizeof(mark), &mark);
        InterlockedIncrement(&g_markGen);   // decide again at its next bind
        if (InterlockedDecrement(&g_untickLogBudget) >= 0)
            LOGI("eye texture %ux%u was unticked in the app, but SteamVR shows it in turn with upscaled ones (leaving it alone "
                 "would flicker): upscaling it too", td.Width, td.Height);
    }
}

volatile LONG g_jobSkipLogged = 0;
volatile LONG g_noQueueLogged = 0;

} // namespace

void OnBeforeDraw(ID3D11DeviceContext *ctx)
{
    if (g_stats.liveViewEntries == 0) return;
    CtxState *cs = FindCtx(ctx);
    if (!cs || (cs->boundCount <= 0 && cs->untickedCount <= 0) || !g_cfg.enabled) return;
    if (!cs->ds || !cs->ds->ok) return;
    WorkLock lock(!cs->immediate);
    if (!lock.held) return;
    MtGuard guard(cs);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (cs->untickedCount > 0) NoteUnticked(cs, ctx, now.QuadPart);
    if (cs->boundCount <= 0) return;
    QJob cand[16];
    const int nc = CollectJobs(cs, ctx, now.QuadPart, cand, 16);
    if (nc == 0) return;
    if (cs->immediate) {
        SelectAndRun(cs, ctx, cand, nc, now.QuadPart);
        return;
    }
    // Recording a command list: note the work; it runs on the immediate context right before the
    // command list executes (where it can be timed). Unless this command list itself wrote the
    // texture earlier (a copy recorded before this draw): then the content only exists at this point
    // of the list, so the work is recorded here, in order. The same if the hooks that would carry
    // the work to the immediate context are not in place.
    QJob inList[16];
    int ni = 0;
    if (!CommandListHooksLive(ctx)) {
        if (InterlockedExchange(&g_noQueueLogged, 1) == 0)
            LOGI("command list: FinishCommandList / ExecuteCommandList are not hooked here; upscales are recorded into the "
                 "command list (not timed)");
        SelectAndRun(cs, ctx, cand, nc, now.QuadPart);
        return;
    }
    for (int i = 0; i < nc; ++i) {
        if (cs->nWritten > 0) {
            bool written = cs->nWritten > 16;
            Ref<ID3D11Resource> r;
            if (!written) cand[i].ve->srcRead->GetResource(r.Put());
            for (int k = 0; k < cs->nWritten && k < 16 && !written; ++k) written = cs->written[k] == r.Get();
            if (written) {
                inList[ni++] = cand[i];
                continue;
            }
        }
        bool dup = false;
        for (auto &p : cs->pending)
            if (p.ve->tex.Get() == cand[i].ve->tex.Get() && p.link == cand[i].link && p.first == cand[i].first &&
                p.count == cand[i].count)
                dup = true;
        if (dup || cs->pending.size() >= 16) continue;
        cand[i].ve->AddRef();
        cs->pending.push_back(cand[i]);
    }
    if (ni > 0) SelectAndRun(cs, ctx, inList, ni, now.QuadPart);
}

void OnClearState(ID3D11DeviceContext *ctx)
{
    if (CtxState *cs = FindCtx(ctx)) ClearBound(cs);
}

void OnFinishCommandList(ID3D11DeviceContext *ctx, bool restore, ID3D11CommandList *cl)
{
    CtxState *cs = FindCtx(ctx);
    if (!cs) return;
    if (!cs->pending.empty() || cs->nWritten > 0) {
        JobList *jl = new JobList();
        jl->jobs.swap(cs->pending);
        jl->nWritten = cs->nWritten;
        memcpy(jl->written, cs->written, sizeof(jl->written));
        if (cl) cl->SetPrivateDataInterface(kGuidJobs, static_cast<IUnknown *>(jl));
        jl->Release();   // the command list holds it (or, if there is none, the jobs are dropped)
    }
    cs->nWritten = 0;
    if (!restore) ClearBound(cs);   // otherwise the deferred context keeps its state, and so do we
}

void OnBeforeExecuteCommandList(ID3D11DeviceContext *ctx, ID3D11CommandList *cl)
{
    // A compositor that records into deferred contexts may never bind anything on its immediate
    // context: run the housekeeping (stats, hotkeys, config reload, releases) here, and the
    // upscales the command list's draws need.
    CtxState *cs = GetCtx(ctx, "first ExecuteCommandList");
    if (!cs) return;
    Ref<JobList> jl;
    if (cl) {
        IUnknown *unk = nullptr;
        UINT sz = sizeof(unk);
        if (SUCCEEDED(cl->GetPrivateData(kGuidJobs, &sz, &unk)) && unk) jl = Ref<JobList>(static_cast<JobList *>(unk), false);
    }
    if (!cs->immediate) {
        // A command list executed inside another one being recorded: its work goes with the outer one,
        // and what it wrote counts as written by the outer one.
        if (!jl) return;
        WorkLock lock(true);
        for (auto &j : jl->jobs) {
            bool dup = false;
            for (auto &p : cs->pending)
                if (p.ve->tex.Get() == j.ve->tex.Get() && p.link == j.link && p.first == j.first && p.count == j.count) dup = true;
            if (dup || cs->pending.size() >= 16) continue;
            j.ve->AddRef();
            cs->pending.push_back(j);
        }
        for (int k = 0; k < jl->nWritten; ++k) {
            ID3D11Resource *r = k < 16 ? jl->written[k] : nullptr;   // overflowed: treat everything as written
            bool known = false;
            for (int m = 0; m < cs->nWritten && m < 16 && r; ++m) known = known || cs->written[m] == r;
            if (known) continue;
            if (r && cs->nWritten < 16) cs->written[cs->nWritten] = r;
            cs->nWritten = r ? cs->nWritten + 1 : 17;
        }
        return;
    }
    // With work to do, wait for a recording thread that is inside our hooks. That is normally
    // microseconds; it can be longer while new textures and histories are being created (first use,
    // or a new size or area), which is a hitch either way. Bounded, so a stuck thread can't hang us.
    WorkLock lock(jl && !jl->jobs.empty() ? 100000 : 0);
    if (!lock.held) {
        if (jl && !jl->jobs.empty() && InterlockedIncrement(&g_jobSkipLogged) <= 5)
            LOGE("command list: upscale work skipped (another thread held our lock for 100 ms)");
        return;
    }
    MtGuard guard(cs);
    RenderTick(ctx);
    if (!jl || jl->jobs.empty()) return;
    DevState *ds = EnsureDevState(cs);
    if (!ds || !ds->ok) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    SelectAndRun(cs, ctx, jl->jobs.data(), (int)jl->jobs.size(), now.QuadPart);
}

void OnAfterExecuteCommandList(ID3D11DeviceContext *ctx, bool restore)
{
    if (restore) return;   // otherwise the context's state was reset to defaults
    if (CtxState *cs = FindCtx(ctx)) ClearBound(cs);
}

void OnCopy(ID3D11DeviceContext *ctx, ID3D11Resource *dst, ID3D11Resource *src)
{
    if (dst && ctx && ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
        // remember what this command list writes (see OnBeforeDraw)
        if (CtxState *cs = GetCtx(ctx, "first copy")) {
            bool known = false;
            for (int k = 0; k < cs->nWritten && k < 16; ++k) known = known || cs->written[k] == dst;
            if (!known) {
                if (cs->nWritten < 16) cs->written[cs->nWritten] = dst;
                cs->nWritten++;
            }
        }
    }
    if (!g_cfg.followCopies || !dst || !src || dst == src) return;
    D3D11_RESOURCE_DIMENSION a = D3D11_RESOURCE_DIMENSION_UNKNOWN, b = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    src->GetType(&a);
    dst->GetType(&b);
    if (a != D3D11_RESOURCE_DIMENSION_TEXTURE2D || b != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    Ref<ID3D11Texture2D> s, d;
    if (FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), (void **)s.Put()))) return;
    D3D11_TEXTURE2D_DESC sd, dd;
    s->GetDesc(&sd);
    if (!((sd.MiscFlags & kSharedMask) || HasMark(s.Get()))) return;
    if (FAILED(dst->QueryInterface(__uuidof(ID3D11Texture2D), (void **)d.Put()))) return;
    d->GetDesc(&dd);
    if ((dd.MiscFlags & kSharedMask) || HasMark(d.Get())) return;
    if (sd.Width != dd.Width || sd.Height != dd.Height || !(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return;
    UINT32 mark = 2;
    d->SetPrivateData(kGuidTexMark, sizeof(mark), &mark);
    InterlockedIncrement(&g_markGen);
    InterlockedIncrement(&g_stats.copiesMarked);
    if (InterlockedDecrement(&g_copyLogBudget) >= 0)
        LOGI("copy: shared %ux%u x%u %s copied into internal %ux%u x%u %s; following the copy", sd.Width, sd.Height,
             sd.ArraySize, FormatName(sd.Format), dd.Width, dd.Height, dd.ArraySize, FormatName(dd.Format));
}

void OnSharedOpened(ID3D11Device *, IUnknown *obj, REFIID, const char *how)
{
    Ref<ID3D11Texture2D> t;
    if (FAILED(obj->QueryInterface(__uuidof(ID3D11Texture2D), (void **)t.Put())) || !t) return;
    UINT32 mark = 1;
    t->SetPrivateData(kGuidTexMark, sizeof(mark), &mark);
    InterlockedIncrement(&g_stats.sharedOpened);
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    int level = InterlockedDecrement(&g_openLogBudget) >= 0 ? 1 : 2;
    if (g_logLevel >= level)
        LogWrite(level, "open: %s -> %ux%u x%u %s mips=%u msaa=%u bind=0x%x misc=0x%x", how, d.Width, d.Height,
                 d.ArraySize, FormatName(d.Format), d.MipLevels, d.SampleDesc.Count, d.BindFlags, d.MiscFlags);
}

void OnCSSetShaderResources(ID3D11DeviceContext *, UINT num, ID3D11ShaderResourceView *const *views)
{
    // Diagnostic only: tells us if the compositor reads shared textures from compute shaders
    // (in which case pixel-shader substitution would never see them).
    if (g_logLevel < 1 || g_csDiagBudget <= 0) return;
    InterlockedDecrement(&g_csDiagBudget);
    for (UINT i = 0; i < num; ++i) {
        ID3D11ShaderResourceView *v = views[i];
        if (!v) continue;
        Ref<ID3D11Resource> r;
        v->GetResource(r.Put());
        Ref<ID3D11Texture2D> t;
        if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)t.Put())) || !t) continue;
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        if (!((d.MiscFlags & kSharedMask) || HasMark(t.Get()))) continue;
        UINT32 seen = 0;
        UINT sz = sizeof(seen);
        if (SUCCEEDED(t->GetPrivateData(kGuidCsLogged, &sz, &seen))) continue;
        seen = 1;
        t->SetPrivateData(kGuidCsLogged, sizeof(seen), &seen);
        LOGI("diag: shared %ux%u x%u %s is read by a compute shader (not substituted)", d.Width, d.Height,
             d.ArraySize, FormatName(d.Format));
    }
}

} // namespace vu

// ---------------------------------------------------------------------------------------------
// Test / diagnostics hook: mean GPU time per temporal run of each pass group, collected with
// [general] passTiming=1 since the last reset. Blocks until pending queries are readable, so call
// it from the thread that owns the immediate context. Returns the number of pass groups (and
// fills names/ms up to max), or 0 when nothing was collected.
// Self-test: acts as if Ctrl+F1+idx were pressed (applied on the next render-thread tick, like a
// real key press), whether or not shortcuts are on.
extern "C" __declspec(dllexport) void VrUpscaleDebugHotkey(int idx)
{
    InterlockedExchange(&vu::g_testHotkey, idx);
}

extern "C" __declspec(dllexport) int VrUpscaleDebugPassTimes(double *ms, const char **names, int max, int reset)
{
    using namespace vu;
    double sum[kTPasses] = {};
    int runs = 0;
    AcquireSRWLockShared(&g_devLock);
    for (int d = 0; d < g_devCount; ++d) {
        DevState *ds = g_devs[d];
        Ref<ID3D11DeviceContext> ctx;
        ds->dev->GetImmediateContext(ctx.Put());
        PollPassQueries(ds, ctx.Get(), true);
        for (int i = 0; i < kTPasses; ++i) sum[i] += ds->passMs[i];
        runs += ds->passRuns;
        if (reset) {
            for (auto &v : ds->passMs) v = 0;
            ds->passRuns = 0;
        }
    }
    ReleaseSRWLockShared(&g_devLock);
    for (int i = 0; i < kTPasses && i < max; ++i) {
        if (ms) ms[i] = runs ? sum[i] / runs : 0.0;
        if (names) names[i] = kTPassNames[i];
    }
    return runs ? kTPasses : 0;
}

// Test hook: blocking read of a texture's spatial content-check counters. out[0] = upscales the
// GPU actually ran for that slice range, out[1] = checks. slot 0 = the whole texture. Returns 1 if
// the texture has that gate.
extern "C" __declspec(dllexport) int VrUpscaleDebugGate(ID3D11Texture2D *tex, int slot, uint32_t *out)
{
    using namespace vu;
    if (!tex || slot < 0 || slot > 2 || !out) return 0;
    IUnknown *unk = nullptr;
    UINT sz = sizeof(unk);
    if (FAILED(tex->GetPrivateData(kGuidTexState, &sz, &unk)) || !unk) return 0;
    Ref<TexState> ts(static_cast<TexState *>(unk), false);
    ID3D11Buffer *b = ts->gates[slot].buf;
    if (!b) return 0;
    Ref<ID3D11Device> dev;
    b->GetDevice(dev.Put());
    Ref<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.Put());
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = 32;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Ref<ID3D11Buffer> st;
    if (FAILED(dev->CreateBuffer(&d, nullptr, st.Put()))) return 0;
    ctx->CopyResource(st.Get(), b);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return 0;
    const uint32_t *w = (const uint32_t *)m.pData;
    out[0] = w[3];
    out[1] = w[4];
    ctx->Unmap(st.Get(), 0);
    return 1;
}
