// Types shared between the spatial path (upscaler.cpp) and the temporal path (temporal.cpp).
#pragma once

#include "common.h"
#include "permutations.h"

#include <initializer_list>

struct VuGazeInfo;   // control_shared.h

namespace vu {

constexpr UINT kSharedMask = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                             D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
constexpr int kSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT kCsSrvSlots = 8;   // compute slots our passes touch (saved and restored)
constexpr UINT kCsUavSlots = 8;

enum Xform { kRaw = 0, kSrgb = 1, kFloat = 2 };
const char *XformName(int x);

extern LARGE_INTEGER g_qpf;
extern SRWLOCK g_cfgLock;            // exclusive: reload/hotkeys; shared: readers off the render thread
extern const GUID kGuidOurs;         // marks textures we created

// Release on the render thread later, never inside D3D's own object teardown.
void DeferRelease(IUnknown *p);

class RefObj : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown))) {
            *pp = static_cast<IUnknown *>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    LONG RefCount() const { return refs_; }
protected:
    virtual ~RefObj() = default;
private:
    volatile LONG refs_ = 1;
};

struct Plan {
    UINT outW = 0, outH = 0;
    double scaleX = 1, scaleY = 1;   // output / input (the same for any part of the texture)
    int xform = kRaw;
    DXGI_FORMAT readFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT proxyFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uavFmt = DXGI_FORMAT_UNKNOWN;
};

class Stream;   // temporal.cpp

// Where a stream's picture is: the content rectangle in the source texture (sx, sy, cw x ch) and
// in the proxy (ox, oy, outW x outH). The whole texture unless the game renders into part of it.
struct LinkGeo {
    UINT sx = 0, sy = 0, cw = 0, ch = 0;
    UINT ox = 0, oy = 0, outW = 0, outH = 0;
    bool full = true;
};

// A texture's slice range bound to a temporal history. Owns one reference to the stream.
struct StreamLink {
    UINT first = 0, count = 0;
    Stream *stream = nullptr;
    LinkGeo geo;              // the stream's content rectangle in this texture
    LONG geoGen = -1;         // ContentProbe::gen it was taken from
    int pool = -1;            // eye pool (where the compositor draws it), -1 = none known
    LONG freedSeen = 0;       // g_streamsFreed when this range last tried for a history
    bool written = false;     // the proxy slices hold temporal output at least once
    ID3D11Buffer *linkBuf = nullptr;               // GPU: which frame this proxy range shows
    ID3D11UnorderedAccessView *linkUAV = nullptr;
    LONGLONG spatialQpc = 0;  // dedupe when this range falls back to the spatial path
    LONGLONG retryQpc = 0;    // last attempt to get a stream for a fallback range
    LONG outGen = -1;         // g_outGen when the proxy range was last written (settings change -> repaint)
};

// Which part of a texture the game renders into. SteamVR keeps an app's larger eye textures after
// the resolution is lowered, and the game then renders into a corner of them; a slider drag can
// leave one eye with textures of several sizes holding the same picture. Found by watching which
// pixels change (FP pass, temporal.hlsl): one sample per 8x8 block is compared with the last one
// seen, and the changed samples' bounding box is read back every probeWindowMs.
// Lives in a list on the render thread (guarded by g_workLock); its texture's TexState holds a
// reference and marks it dead when the texture goes.
class ContentProbe final : public RefObj {
public:
    ContentProbe();
    ID3D11Device *dev = nullptr;              // key only
    UINT texW = 0, texH = 0, slices = 0;
    Ref<ID3D11Buffer> mem, box, staging;
    Ref<ID3D11UnorderedAccessView> memUAV, boxUAV;
    uint32_t primed = 0;                      // slices whose block memory holds samples
    volatile LONG dead = 0;
    bool pending = false;                     // a copy of the box waits in staging
    LONGLONG windowSlot = -1;                 // windows end together for every texture (now / window length)
    struct Box { UINT x0, y0, x1, y1; };      // pixels, exclusive; x1 == 0: nothing changed
    Box ring[8] = {};
    int ringN = 0, ringHead = 0, active = 0;  // windows kept, next slot, windows with changes so far
    Box cand = {};
    int candN = 0;
    Box newest = {};                          // the latest window with changes, and when it ended
    LONGLONG newestQpc = 0;
    // the eye pool the texture was last drawn into (NotePool): a pool's textures hold the same
    // picture, so they take a new rectangle together (no window where the eye is on two histories)
    int pool = -1;
    uint32_t poolEpoch = 0;
    LONGLONG drawQpc = 0;
    // accepted content rectangle (the whole texture until something else is established)
    UINT rx = 0, ry = 0, rw = 0, rh = 0;
    bool sub = false;                         // rx..rh is less than the whole texture
    LONG gen = 0;                             // bumped when rx..rh change
    LONGLONG acceptQpc = 0;                   // when rx..rh were accepted (0: never, the default)
protected:
    ~ContentProbe() override;
};

// Spatial path: which content a proxy range shows, kept on the GPU (see SDECIDE in temporal.hlsl).
struct ContentGate {
    ID3D11Buffer *buf = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    LONG outGen = -1;         // g_outGen when last written
};

class TexState final : public RefObj {
public:
    TexState();
    LONG gen = 0;
    uint64_t serial = 0;      // unique per TexState, never reused (unlike addresses)
    bool failed = false;
    D3D11_TEXTURE2D_DESC src = {};
    Plan plan;
    Ref<ID3D11Texture2D> proxy;
    Ref<ID3D11UnorderedAccessView> proxyUAV;
    LONGLONG lastQpc = 0;     // spatial path dedupe
    uint64_t upscales = 0;
    StreamLink links[2];
    int nlinks = 0;
    ContentGate gates[3];     // spatial path: [0] the whole texture, [1 + i] links[i]'s slices
    Ref<ContentProbe> probe;  // created on first use
protected:
    ~TexState() override;
};

class ViewEntry final : public RefObj {
public:
    ViewEntry();
    LONG gen = 0;
    int ctlToken = -1;                       // row in the app's texture list
    UINT first = 0, count = 1;               // slices the compositor's view covers
    Ref<TexState> tex;
    Ref<ID3D11ShaderResourceView> srcRead;   // all slices of the source, typed for reading
    Ref<ID3D11ShaderResourceView> proxySRV;  // same desc as the compositor's view, on the proxy
protected:
    ~ViewEntry() override;
};

struct QuerySet {
    Ref<ID3D11Query> dj, t0, t1;
    bool busy = false;
};

// Per-pass timestamps of one temporal run ([general] passTiming=1).
constexpr int kTPasses = 9;
struct PassQueries {
    Ref<ID3D11Query> dj, t[kTPasses + 1];
    bool busy = false;
};

struct DevState {
    Ref<ID3D11Device> dev;
    Ref<ID3D11ComputeShader> cs[SH_COUNT];
    Ref<ID3D11Buffer> cb;          // spatial-path parameters
    Ref<ID3D11Buffer> tcb;         // temporal-path parameters
    Ref<ID3D11SamplerState> samp;  // linear clamp
    Ref<ID3D11Texture2D> scratch;
    Ref<ID3D11ShaderResourceView> scratchSRV;
    Ref<ID3D11UnorderedAccessView> scratchUAV;
    UINT sw = 0, sh = 0, ss = 0;
    QuerySet q[8];
    double gpuMsSum = 0, gpuMsMax = 0;
    int gpuSamples = 0;
    double gpuEma = -1;            // for the app's status (not reset by the stats line)
    DWORD gpuEmaTick = 0;
    PassQueries pq[8];
    // content check of the spatial path: fingerprint and the indirect args it leaves (used at once)
    Ref<ID3D11Buffer> gateFp, gateArgs;
    Ref<ID3D11ShaderResourceView> gateFpSRV;
    Ref<ID3D11UnorderedAccessView> gateFpUAV, gateArgsUAV;
    double passMs[kTPasses] = {};    // sums since the last stats line / debug read
    int passRuns = 0;
    bool ok = false;
    struct FmtOk { DXGI_FORMAT f; bool ok; };
    std::vector<FmtOk> uavFmts;
};

bool UavStoreSupported(DevState *ds, DXGI_FORMAT f);

// Sets compute SRV slots 0..7 and UAV slots 0..7 (unlisted slots become null). UAVs are cleared
// first so a texture written by the previous pass can be bound for reading by this one.
void BindCS(ID3D11DeviceContext *ctx, std::initializer_list<ID3D11ShaderResourceView *> srvs,
            std::initializer_list<ID3D11UnorderedAccessView *> uavs);

// ---- temporal.cpp ---------------------------------------------------------------------------

// Identifies where a draw renders to (render target description + viewport). Used only to
// decide whether a new texture continues an existing eye history; robust to swap-chain rotation.
struct OutSig {
    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFmt = DXGI_FORMAT_UNKNOWN;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    bool valid = false;
    bool operator==(const OutSig &o) const
    {
        return valid && o.valid && rtW == o.rtW && rtH == o.rtH && rtFmt == o.rtFmt && vx == o.vx && vy == o.vy &&
               vw == o.vw && vh == o.vh;
    }
};

OutSig CaptureOutSig(ID3D11DeviceContext *ctx);
StreamLink *FindLink(TexState *ts, UINT first, UINT count);
// Finds or creates the link (and history) for this view's slices. Returns nullptr if no link
// could be made; a link whose stream is null means "spatial path for this slice range".
StreamLink *ResolveStream(DevState *ds, ID3D11DeviceContext *ctx, ViewEntry *ve, LONGLONG now);
void ReleaseLink(StreamLink &l);
bool StreamWantsRun(StreamLink *link, TexState *ts, LONGLONG now, LONGLONG dedupeTicks);
void StreamTouch(StreamLink *link, TexState *ts, LONGLONG now);
void RunStream(ID3D11DeviceContext *ctx, DevState *ds, StreamLink *link, ViewEntry *ve, LONGLONG now);
void ReleaseStreamRef(Stream *s);
void TemporalSweep();
void TemporalResetAll(const char *why);
// [general] passTiming: names of the timed temporal pass groups, and collection of finished query
// sets into ds->passMs (wait = block until every pending set is readable; tests only).
extern const char *const kTPassNames[kTPasses];
extern const UINT kTParamsBytes;   // size of the temporal passes' constant buffer
// Bumped by every settings change: proxies are repainted even if their source did not change.
extern volatile LONG g_outGen;
bool CreateGateResources(DevState *ds);
bool EnsureGate(DevState *ds, ContentGate &g);
void ReleaseGate(ContentGate &g);
// Fingerprints the source slices and compares them with what the gate's proxy shows. Leaves the
// indirect args (2 entries of gx, gy, gz, or zeros when unchanged) in ds->gateArgs. Binds the
// temporal constant buffer. False: could not run (dispatch directly instead).
bool RunContentGate(ID3D11DeviceContext *ctx, DevState *ds, TexState *ts, ID3D11ShaderResourceView *src, UINT first,
                    UINT count, int xform, ContentGate &g, bool force, UINT gx, UINT gy, UINT gz);
// Content rectangles: reads finished observation windows and starts new ones (render tick).
void PollProbes(ID3D11DeviceContext *ctx, ID3D11Device *dev);
// The vrupscale app changed the game's render resolution (SteamVR's resolution) just now: for a few
// seconds, observation windows are short and only the latest ones count, so the new picture size
// is found within a fraction of a second instead of seconds. Caller holds g_workLock.
void ContentHint(const char *who);
// The texture's content rectangle as the temporal path uses it (whole texture if unknown).
LinkGeo ContentGeo(const TexState *ts);
// Eye pools: textures the compositor draws into the same eye viewport. Called at each draw with
// what it renders to; returns the pool (-1 when the viewport does not identify an eye).
int NotePool(const OutSig &sig, TexState *ts, StreamLink *link, LONGLONG now);
// True if the textures of link's eye pool are not all on one history right now: then every one
// of them is upscaled spatially, so the eye does not alternate between temporal and spatial.
bool PoolMixed(const StreamLink *link, const TexState *ts, LONGLONG now);
// Eye textures in the app's list: is ts one of a pool of at least two similar textures?
bool PoolEyeTexture(int pool, const TexState *ts, LONGLONG now);
void PoolStatus(int *pools, int *mixed);   // for the app: eye pools seen lately, and how many are mixed
// Was another upscaled texture (not notSerial) of this kind drawn into that eye viewport in the last
// second, and after since?
bool PoolHasUpscaledLike(const OutSig &sig, DXGI_FORMAT fmt, UINT w, UINT h, LONGLONG now, uint64_t notSerial, LONGLONG since);
void StreamPause(StreamLink *link);       // link's eye is on the spatial path for now
void PollPassQueries(DevState *ds, ID3D11DeviceContext *ctx, bool wait);
void TemporalLogStats(ID3D11DeviceContext *ctx, ID3D11Device *dev, double seconds);
void TemporalPollCounters(ID3D11DeviceContext *ctx, ID3D11Device *dev);   // every ~0.5 s from the render tick
struct StreamDesc { uint32_t id, w, h, outW, outH, textures; uint64_t runs; };
int TemporalDescribe(StreamDesc *out, int max);

// ---- upscaler.cpp ---------------------------------------------------------------------------
// Installs a new configuration: live values take effect at once; anything that changes which
// textures are upscaled or how large (factor, size rules) re-plans them.
void ApplyConfig(const Config &nc, const char *source);

// Eye tracking (gaze.cpp). side: 0 left eye, 1 right, -1 not known. Points are in the eye's
// picture, 0..1 from the top left.
void GazeTick();                                     // render thread: new samples, calibration
bool GazeStartCalibration(const char *by);           // false (and a message in the status) if it can't run
bool GazePoint(int side, float *u, float *v);        // calibrated gaze; false = no live tracking
void GazeTarget(int side, float *u, float *v);       // straight ahead (the calibration cross)
bool GazeCalibrating();
bool GazeShowRing();                                 // draw the gaze ring (Show gaze, or just calibrated)
void GazeFillStatus(VuGazeInfo &g);
void GazeShutdown();
float GpuMsRecent();                 // recent GPU ms per batch of upscale work, < 0 if not measured
extern volatile LONG g_jobBatches;   // batches of upscale work issued on an immediate context (timed ones)
void ControlTickFromWorker();        // the app channel when the render thread is not ticking

// ---- control.cpp (channel to the vrupscale app) -------------------------------------------
void ControlInit(bool realCompositor);
void ControlTick();                  // caller holds g_workLock
void ControlNoteCommandLists();
int ControlCommandLists();
int ControlNoteTexture(const D3D11_TEXTURE2D_DESC &td, bool upscaled, UINT outW, UINT outH, const char *reason);
void ControlTouchTexture(int token);
void ControlNoteTextureUse(int token, bool eye, UINT contentW, UINT contentH, UINT contentOutW, UINT contentOutH);

} // namespace vu
