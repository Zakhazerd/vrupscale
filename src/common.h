#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <stdint.h>
#include <string>
#include <vector>
#include <utility>

#define VU_VERSION_STR "0.6.6"

namespace vu {

// ---------------------------------------------------------------------------------------------
// Minimal intrusive COM smart pointer (avoids depending on WRL under MinGW).
template <class T>
class Ref {
public:
    Ref() = default;
    Ref(std::nullptr_t) {}
    explicit Ref(T *p, bool addRef) : p_(p) { if (p_ && addRef) p_->AddRef(); }
    Ref(const Ref &o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Ref(Ref &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ref() { Reset(); }
    Ref &operator=(const Ref &o) { if (this != &o) { Reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    Ref &operator=(Ref &&o) noexcept { if (this != &o) { Reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    void Reset() { if (p_) { T *t = p_; p_ = nullptr; t->Release(); } }
    T *Get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T **Put() { Reset(); return &p_; }          // for out-params
    T *const *AddressOf() const { return &p_; }  // for array-of-one inputs
    T *Detach() { T *t = p_; p_ = nullptr; return t; }
private:
    T *p_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Logging
extern volatile LONG g_logLevel;   // 0 = errors, 1 = info, 2 = verbose
void LogOpen(const std::wstring &path);
void ControlNoteEvent(int level, const char *line);   // control.cpp: recent events for the app
void LogWrite(int level, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
void LogFlush();

#define LOGE(...) ::vu::LogWrite(0, __VA_ARGS__)
#define LOGI(...) do { if (::vu::g_logLevel >= 1) ::vu::LogWrite(1, __VA_ARGS__); } while (0)
#define LOGV(...) do { if (::vu::g_logLevel >= 2) ::vu::LogWrite(2, __VA_ARGS__); } while (0)

const char *FormatName(DXGI_FORMAT f);

// ---------------------------------------------------------------------------------------------
// Configuration (vrupscale.ini next to the DLL). Read/written only on the compositor render
// thread after startup, except g_logLevel which is atomic.
enum class Mode : int { Temporal = 0, Fsr = 1, Bilinear = 2 };
const char *ModeName(Mode m);

struct Size2 { int w, h; };

struct Config {
    bool active = true;              // [general] active: master switch, read once at load
    bool enabled = true;             // runtime toggle (Ctrl+F1)
    Mode mode = Mode::Temporal;      // temporal | fsr | bilinear
    float upscaleFactor = 1.3f;      // per-axis output/input ratio
    float sharpness = 1.0f;          // 0 = RCAS off, 1 = max
    int minWidth = 1000;
    int minHeight = 1000;
    float minAspect = 0.6f;          // width / height, per array slice
    float maxAspect = 1.6f;
    std::vector<Size2> onlySizes;    // if non-empty, only these source sizes are processed
    std::vector<Size2> excludeSizes;
    bool requireShared = true;       // only textures shared from another process (or copies of them)
    bool followCopies = true;        // mark compositor-internal copies of shared textures as eligible
    int maxArraySize = 2;
    float dedupeMs = 3.0f;           // re-upscale a texture at most once per this window
    bool tint = false;               // debug tint of processed output
    int logLevel = 1;
    int statsIntervalSec = 10;
    bool hotkeys = true;
    bool gpuTiming = true;           // timestamp queries around our dispatches: the app's GPU figure and the
                                     // stats line's per-pass times of the temporal path

    // [temporal]
    float maxWeight = 1.0f;          // history weight cap (current sample = 1); more = steadier but softer
    float clipGamma = 1.25f;         // variance-clip box size in standard deviations
    float residualTol = 1.5f;        // contrast-normalised flow residual at which history is dropped
    float sampleSigma = 0.35f;       // input-pixel radius over which a new sample counts as "direct"
    float minNewWeight = 0.05f;      // floor on the current frame's contribution
    float lkEps = 0.001f;            // Lucas-Kanade regulariser (flat areas -> no motion)
    float searchLambda = 0.002f;     // coarse search bias towards small / predicted motion
    float clipReject = 1.0f;         // how fast history far outside the clip box is dropped
    float localTrust = 1.0f;         // history trust where motion differs from the global model (0 = spatial there)
    float localGate = 0.6f;          // normalised global-model residual above which local motion is searched
    float localRatio = 0.7f;         // a local candidate must fit this much better than the global model
    int localCheck = 47;             // checks on local motion before history is trusted on it (bits): 1 well-determined
                                     // (not an edge or stripes), 2 neighbours agree, 4 last frame agrees; where no local
                                     // motion fits: 8 no history; a doubted one: 16 no history, 32 the global model
                                     // only where the cell followed it last frame (else no history)
    float localAgree = 1.0f;         // input px within which neighbouring / last frame's motion counts as agreeing
    float localAperture = 0.3f;      // smallest/largest structure-tensor eigenvalue below which motion is not determined
    int workMargin = 128;            // input px around the region where the luma pyramid is built (it is needed only there)
    bool sharpHistory = false;       // resample the history with Lanczos-3 (else Catmull-Rom: cheaper, a little softer)
    bool bilinearEdges = false;      // outside temporalArea: bilinear instead of Catmull-Rom (cheaper, softer)

    // [eyes] eye tracking (Bigscreen Beyond 2e)
    bool gazeFollow = false;         // the temporal area follows the gaze
    bool gazeShow = false;           // draw a ring at the gaze
    float gazeOffset[2] = {};        // calibration (tangents, x right, y up), subtracted from the gaze
    float eyeProj[8] = {};           // per-eye projection tangents l, r, t, b (left, right eye), from SteamVR via the app
    float gazeDeadzone = 0.02f;      // the area moves once the gaze is this far (fraction of the width) from its centre
    float temporalArea = 0.6f;       // width and height of the part with history / FSR 1 (1 = everything) ...
    float gazeArea = 0.4f;           // ... and the same while it follows the gaze (it is where you look: smaller does)
    float sharpArea = 0.6f;          // [upscale] width and height of the part RCAS sharpens (at least the area above) ...
    float gazeSharpArea = 0.4f;      // ... and the same while the area follows the gaze
    int maxStreams = 4;              // temporal histories (one per eye texture lineage)
    bool linkPools = true;           // let rotating swap-chain textures of one eye share history
    float linkGapMs = 2.0f;          // never link textures bound closer together than this
    int probeWindowMs = 500;         // content (render size) detection: length of one observation window
    bool detectContent = true;       // find the part of each eye texture the game renders into
};

// The area that gets the full treatment (history in Temporal mode, FSR 1 in FSR mode): its own
// value while it follows the gaze.
inline float RegionArea(const Config &c) { return c.gazeFollow ? c.gazeArea : c.temporalArea; }

// The part RCAS sharpens (the Sharpness area), centred on that area: at least the area itself.
// Outside the area it sharpens the spatial upscale.
inline float SharpArea(const Config &c)
{
    const float s = c.gazeFollow ? c.gazeSharpArea : c.sharpArea, r = RegionArea(c);
    return s > r ? s : r;
}

extern Config g_cfg;
extern volatile LONG g_cfgGen;       // bumped on every (re)load; invalidates cached decisions
extern std::wstring g_dllDir;
extern std::wstring g_iniPreferred;   // where the app keeps the settings; switched to once it exists
extern std::wstring g_logPath;
extern volatile LONG g_reloadRequested;
extern std::wstring g_iniPath;

bool LoadConfig(Config &out);        // returns false if the file is missing (defaults kept)
void SanitizeConfig(Config &c);
void PrintConfig(const Config &c);
bool ConfigFileChanged(bool *switched = nullptr);   // cheap mtime poll (switched: now another file)

// ---------------------------------------------------------------------------------------------
// Hooks / runtime
extern volatile LONG g_shuttingDown;   // set at DLL_PROCESS_DETACH: detours become pass-through
bool HookDeviceCreation();           // hooks D3D11CreateDevice* so method hooks come from the real device
bool InstallD3D11Hooks();            // fallback: creates a throwaway device to locate the method bodies
bool HooksInstalled();
void ProbeContextVariants();         // worker thread, once: hooks other context configurations' bodies
void FormatHookVariantHits(char *out, size_t n);   // empty unless several bodies of PS/Draw are hooked
extern volatile LONGLONG g_lastTickQpc;  // last render-thread housekeeping tick (QPC), 0 = never
void StartWorker();

// Precompiled compute-shader bytecode (src/gen/shaders.bin, indexed by ShaderId in permutations.h)
struct BlobRef { const void *data; size_t size; };
BlobRef GetShaderBlob(int id);

// Upscaler entry points called from hooks (implemented in upscaler.cpp)
void OnPSSetShaderResources(ID3D11DeviceContext *ctx, UINT start, UINT num,
                            ID3D11ShaderResourceView *const *views,
                            ID3D11ShaderResourceView **outViews, bool *replaced);
void OnBeforeDraw(ID3D11DeviceContext *ctx);
void OnClearState(ID3D11DeviceContext *ctx);
void OnFinishCommandList(ID3D11DeviceContext *ctx, bool restore, ID3D11CommandList *cl);   // deferred context
bool CommandListHooksLive(ID3D11DeviceContext *deferred);   // hooks.cpp
void OnBeforeExecuteCommandList(ID3D11DeviceContext *ctx, ID3D11CommandList *cl);          // immediate context
void OnAfterExecuteCommandList(ID3D11DeviceContext *ctx, bool restore);
void OnCopy(ID3D11DeviceContext *ctx, ID3D11Resource *dst, ID3D11Resource *src);
void OnSharedOpened(ID3D11Device *dev, IUnknown *obj, REFIID riid, const char *how);
void OnCSSetShaderResources(ID3D11DeviceContext *ctx, UINT num, ID3D11ShaderResourceView *const *views);

// Original (trampoline) functions, filled in by hooks.cpp
typedef void (STDMETHODCALLTYPE *PFN_SetSRVs)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
extern PFN_SetSRVs g_origPSSetShaderResources;
extern PFN_SetSRVs g_origCSSetShaderResources;

// Private-data GUIDs
extern const GUID kGuidViewEntry;   // on compositor SRVs: our ViewEntry (interface)
extern const GUID kGuidViewReject;  // on compositor SRVs: UINT32 config generation of a "no" decision
extern const GUID kGuidTexState;    // on source textures: our TexState (interface)
extern const GUID kGuidTexMark;     // on textures: UINT32 how it became a candidate (1 = opened shared, 2 = copy)
extern const GUID kGuidTexLogged;   // on textures: UINT32 config gen of last rejection log

// Debug counters exported for the test harness
struct DebugStats {
    LONG hooksInstalled;
    LONG shadersReady;
    LONG liveTexStates;
    LONG liveViewEntries;
    LONG upscales;
    LONG psHookCalls;
    LONG substitutions;
    LONG rejects;
    LONG sharedOpened;
    LONG copiesMarked;
    LONG deferredPending;
    LONG liveStreams;
    LONG temporalRuns;
    LONG csHookCalls;
    LONG drawHookCalls;
    LONG copyHookCalls;
    LONG extraBodies;     // context-method bodies hooked after the first installation
};
extern DebugStats g_stats;

} // namespace vu
