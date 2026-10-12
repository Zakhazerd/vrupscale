// Live control channel to the vrupscale app (see control_shared.h): applies settings the app
// sends, runs its commands, and publishes what the upscaler is doing.
#include "internal.h"
#include "control_shared.h"

#include <stdio.h>
#include <string.h>

namespace vu {

namespace {

HANDLE g_map = nullptr;
VuShared *g_sh = nullptr;
int32_t g_lastSettingsSeq = 0;
int32_t g_lastCmdSeq = 0;
DWORD g_lastPublish = 0;

SRWLOCK g_ctlLock = SRWLOCK_INIT;   // texture table and event ring
VuTexInfo g_texTab[kVuMaxTex];
uint32_t g_texGen[kVuMaxTex];
uint32_t g_texN = 0;
char g_events[kVuEvents][160];
uint32_t g_eventN = 0;

struct RateRec { uint32_t id; uint64_t runs; };
RateRec g_rates[kVuMaxStreams];
int g_rateN = 0;
DWORD g_rateTick = 0;
LONG g_upsAtRate = 0;
float g_upsPerSec = 0;
LONG g_batchesAtRate = 0;
float g_batchesPerSec = 0;

void SeqWrite(volatile int32_t *seq, void *dst, const void *src, size_t n)
{
    InterlockedIncrement((volatile LONG *)seq);
    MemoryBarrier();
    memcpy(dst, src, n);
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)seq);
}

bool SeqRead(volatile int32_t *seq, void *dst, const void *src, size_t n, int32_t *seen)
{
    for (int tries = 0; tries < 256; ++tries) {
        int32_t a = *seq;
        if (a & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        memcpy(dst, src, n);
        MemoryBarrier();
        if (*seq == a) {
            if (seen) *seen = a;
            return true;
        }
    }
    return false;
}

void CopyW(wchar_t *dst, size_t n, const std::wstring &s)
{
    size_t k = s.size() < n - 1 ? s.size() : n - 1;
    memcpy(dst, s.data(), k * sizeof(wchar_t));
    dst[k] = 0;
}

int32_t ModeToVu(Mode m) { return m == Mode::Fsr ? kVuModeFsr : (m == Mode::Bilinear ? kVuModeBilinear : kVuModeTemporal); }
Mode ModeFromVu(int32_t m) { return m == kVuModeFsr ? Mode::Fsr : (m == kVuModeBilinear ? Mode::Bilinear : Mode::Temporal); }

void ConfigToSettings(const Config &c, VuSettings &s)
{
    memset(&s, 0, sizeof(s));
    s.enabled = c.enabled;
    s.mode = ModeToVu(c.mode);
    s.upscaleFactor = c.upscaleFactor;
    s.fastHistory = c.sharpHistory ? 0 : 1;
    s.bilinearEdges = c.bilinearEdges;
    s.gazeFollow = c.gazeFollow;
    s.gazeShow = c.gazeShow;
    s.gazeOffset[0] = c.gazeOffset[0];
    s.gazeOffset[1] = c.gazeOffset[1];
    memcpy(s.eyeProj, c.eyeProj, sizeof(s.eyeProj));
    s.sharpness = c.sharpness;
    s.tint = c.tint;
    s.hotkeys = c.hotkeys;
    s.maxWeight = c.maxWeight;
    s.localTrust = c.localTrust;
    s.residualTol = c.residualTol;
    s.clipGamma = c.clipGamma;
    s.maxStreams = c.maxStreams;
    s.temporalArea = c.temporalArea;
    s.gazeArea = c.gazeArea;
    s.sharpArea = c.sharpArea;
    s.gazeSharpArea = c.gazeSharpArea;
    s.requireShared = c.requireShared;
    s.minWidth = c.minWidth;
    s.minHeight = c.minHeight;
    s.minAspect = c.minAspect;
    s.maxAspect = c.maxAspect;
    for (auto &e : c.excludeSizes) {
        if (s.excludeCount >= (int32_t)kVuMaxExclude) break;
        s.exclude[s.excludeCount++] = ((uint32_t)e.w << 16) | ((uint32_t)e.h & 0xffff);
    }
}

void SettingsToConfig(const VuSettings &s, Config &c)
{
    c.enabled = s.enabled != 0;
    c.mode = ModeFromVu(s.mode);
    c.upscaleFactor = s.upscaleFactor;
    c.sharpHistory = s.fastHistory == 0;
    c.bilinearEdges = s.bilinearEdges != 0;
    c.gazeFollow = s.gazeFollow != 0;
    c.gazeShow = s.gazeShow != 0;
    c.gazeOffset[0] = s.gazeOffset[0];
    c.gazeOffset[1] = s.gazeOffset[1];
    memcpy(c.eyeProj, s.eyeProj, sizeof(c.eyeProj));
    c.sharpness = s.sharpness;
    c.tint = s.tint != 0;
    c.hotkeys = s.hotkeys != 0;
    c.maxWeight = s.maxWeight;
    c.localTrust = s.localTrust;
    c.residualTol = s.residualTol;
    c.clipGamma = s.clipGamma;
    c.maxStreams = s.maxStreams;
    c.temporalArea = s.temporalArea;
    if (s.gazeArea > 0.0f) c.gazeArea = s.gazeArea;   // (0: an app from before 0.6.1)
    if (s.sharpArea > 0.0f) c.sharpArea = s.sharpArea;   // (0: an app from before 0.6.4)
    if (s.gazeSharpArea > 0.0f) c.gazeSharpArea = s.gazeSharpArea;
    c.requireShared = s.requireShared != 0;
    c.minWidth = s.minWidth;
    c.minHeight = s.minHeight;
    c.minAspect = s.minAspect;
    c.maxAspect = s.maxAspect;
    c.excludeSizes.clear();
    int n = s.excludeCount < 0 ? 0 : (s.excludeCount > (int32_t)kVuMaxExclude ? (int32_t)kVuMaxExclude : s.excludeCount);
    for (int i = 0; i < n; ++i) c.excludeSizes.push_back({ (int)(s.exclude[i] >> 16), (int)(s.exclude[i] & 0xffff) });
}

void Publish()
{
    static VuStatus st;   // only ever touched under g_workLock (render tick or worker fallback)
    memset(&st, 0, sizeof(st));
    const DWORD now = GetTickCount();
    st.pid = GetCurrentProcessId();
    st.tick = now;
    snprintf(st.version, sizeof(st.version), "%s", VU_VERSION_STR);
    st.hooksInstalled = g_stats.hooksInstalled;
    st.commandLists = ControlCommandLists();
    st.upscalesTotal = (uint32_t)g_stats.upscales;
    if (g_rateTick == 0 || now - g_rateTick >= 1000) {
        const float secs = g_rateTick ? (now - g_rateTick) / 1000.0f : 0.0f;
        LONG ups = g_stats.upscales;
        g_upsPerSec = secs > 0 ? (ups - g_upsAtRate) / secs : 0.0f;
        g_upsAtRate = ups;
        LONG batches = g_jobBatches;
        g_batchesPerSec = secs > 0 ? (batches - g_batchesAtRate) / secs : 0.0f;
        g_batchesAtRate = batches;
        // per-stream update rates
        StreamDesc sd[kVuMaxStreams];
        int n = TemporalDescribe(sd, kVuMaxStreams);
        RateRec next[kVuMaxStreams];
        int nn = 0;
        for (int i = 0; i < n; ++i) {
            float rate = 0;
            for (int k = 0; k < g_rateN; ++k)
                if (g_rates[k].id == sd[i].id && secs > 0) rate = (float)(sd[i].runs - g_rates[k].runs) / secs;
            next[nn++] = { sd[i].id, sd[i].runs };
            st.streams[i] = { sd[i].id, sd[i].w, sd[i].h, sd[i].outW, sd[i].outH, sd[i].textures, rate };
        }
        st.streamCount = (uint32_t)n;
        memcpy(g_rates, next, sizeof(RateRec) * nn);
        g_rateN = nn;
        g_rateTick = now;
    } else if (g_sh) {
        st.streamCount = g_sh->status.streamCount;   // keep the last rates between refreshes
        memcpy(st.streams, (const void *)g_sh->status.streams, sizeof(st.streams));
    }
    st.upscalesPerSec = g_upsPerSec;
    st.gpuMs = GpuMsRecent();
    st.gpuMsPerSec = st.gpuMs >= 0 ? st.gpuMs * g_batchesPerSec : -1.0f;
    PoolStatus(&st.eyePools, &st.mixedEyes);
    st.appliedSeq = (uint32_t)g_lastSettingsSeq;
    AcquireSRWLockShared(&g_cfgLock);
    ConfigToSettings(g_cfg, st.current);
    ReleaseSRWLockShared(&g_cfgLock);
    AcquireSRWLockShared(&g_ctlLock);
    st.texCount = g_texN;
    memcpy(st.tex, g_texTab, sizeof(st.tex));
    st.eventCount = g_eventN;
    memcpy(st.events, g_events, sizeof(st.events));
    ReleaseSRWLockShared(&g_ctlLock);
    CopyW(st.iniPath, 260, g_iniPath);
    CopyW(st.logPath, 260, g_logPath);
    GazeFillStatus(st.gaze);
    SeqWrite(&g_sh->statusSeq, &g_sh->status, &st, sizeof(st));
}

volatile LONG g_commandListsSeen = 0;

} // namespace

std::wstring ControlChannelName(bool realCompositor)
{
    wchar_t buf[256];
    DWORD n = GetEnvironmentVariableW(L"VRUPSCALE_CONTROL", buf, 256);
    if (n > 0 && n < 256) return std::wstring(buf, n);
    if (realCompositor) return VU_CONTROL_NAME;
    // a forced (test) process must not take over the channel of a running SteamVR
    return std::wstring(VU_CONTROL_NAME) + L".test." + std::to_wstring(GetCurrentProcessId());
}

void ControlInit(bool realCompositor)
{
    std::wstring name = ControlChannelName(realCompositor);
    const DWORD size = (DWORD)((sizeof(VuShared) + 4095) & ~4095u);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name.c_str());
    const bool existed = g_map && GetLastError() == ERROR_ALREADY_EXISTS;
    if (!g_map) {
        LOGE("control: could not create the channel for the vrupscale app (%lu)", GetLastError());
        return;
    }
    g_sh = (VuShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!g_sh) {
        LOGE("control: could not map the channel (%lu)", GetLastError());
        CloseHandle(g_map);
        g_map = nullptr;
        return;
    }
    if (!existed || g_sh->magic != kVuMagic || g_sh->layout != kVuLayout) {
        if (existed) LOGI("control: replacing a channel with an older layout (update the app)");
        memset((void *)g_sh, 0, sizeof(VuShared));
    }
    // Settings and commands already sitting in the channel are from an earlier session: the
    // settings file holds the current ones.
    g_lastSettingsSeq = g_sh->settingsSeq;
    g_lastCmdSeq = g_sh->commandSeq;
    g_sh->size = sizeof(VuShared);
    g_sh->layout = kVuLayout;
    MemoryBarrier();
    g_sh->magic = kVuMagic;
    LOGI("control: channel for the vrupscale app ready%s", existed ? " (the app is already running)" : "");
}

void ControlNoteCommandLists() { InterlockedExchange(&g_commandListsSeen, 1); }
int ControlCommandLists() { return (int)g_commandListsSeen; }

int ControlNoteTexture(const D3D11_TEXTURE2D_DESC &td, bool upscaled, UINT outW, UINT outH, const char *reason)
{
    const DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&g_ctlLock);
    uint32_t i = 0;
    for (; i < g_texN; ++i) {
        VuTexInfo &t = g_texTab[i];
        if (t.w == td.Width && t.h == td.Height && t.slices == td.ArraySize && t.format == (uint32_t)td.Format) break;
    }
    if (i == g_texN) {
        if (g_texN < kVuMaxTex) {
            i = g_texN++;
        } else {   // replace the one seen least recently
            i = 0;
            for (uint32_t k = 1; k < g_texN; ++k)
                if ((int32_t)(g_texTab[k].lastSeenTick - g_texTab[i].lastSeenTick) < 0) i = k;
        }
        g_texGen[i]++;
    }
    VuTexInfo &t = g_texTab[i];
    t.w = td.Width;
    t.h = td.Height;
    t.slices = td.ArraySize;
    t.format = (uint32_t)td.Format;
    t.upscaled = upscaled ? 1 : 0;
    t.outW = upscaled ? outW : 0;
    t.outH = upscaled ? outH : 0;
    t.lastSeenTick = now;
    snprintf(t.reason, sizeof(t.reason), "%s", reason ? reason : "");
    if (!upscaled) {
        t.eye = 0;
        t.contentW = t.contentH = t.contentOutW = t.contentOutH = 0;
    }
    int token = (int)((g_texGen[i] << 8) | i);
    ReleaseSRWLockExclusive(&g_ctlLock);
    return token;
}

void ControlTouchTexture(int token)
{
    if (token < 0) return;
    uint32_t i = (uint32_t)token & 0xff;
    if (i >= kVuMaxTex || g_texGen[i] != ((uint32_t)token >> 8)) return;
    g_texTab[i].lastSeenTick = GetTickCount();   // single aligned store; races are harmless
}

// What the render thread learned about an upscaled texture while drawing it (single aligned stores:
// a reader may briefly see a mix of old and new values, which is harmless for a status display).
void ControlNoteTextureUse(int token, bool eye, UINT contentW, UINT contentH, UINT contentOutW, UINT contentOutH)
{
    if (token < 0) return;
    uint32_t i = (uint32_t)token & 0xff;
    if (i >= kVuMaxTex || g_texGen[i] != ((uint32_t)token >> 8)) return;
    VuTexInfo &t = g_texTab[i];
    t.eye = eye ? 1 : 0;
    t.contentW = contentW;
    t.contentH = contentH;
    t.contentOutW = contentOutW;
    t.contentOutH = contentOutH;
}

void ControlNoteEvent(int level, const char *line)
{
    if (level > 1 || !line) return;
    // the periodic stats lines would push everything else out of the ring
    const char *msg = strstr(line, "] ");
    msg = msg ? strstr(msg + 2, "] ") : nullptr;
    msg = msg ? msg + 2 : line;
    if (strncmp(msg, "stats", 5) == 0 || strncmp(msg, "diag:", 5) == 0 || strncmp(msg, "config: ", 8) == 0) return;
    AcquireSRWLockExclusive(&g_ctlLock);
    snprintf(g_events[g_eventN % kVuEvents], sizeof(g_events[0]), "%s", line);
    g_eventN++;
    ReleaseSRWLockExclusive(&g_ctlLock);
}

// Called with g_workLock held: from the render-thread tick, or from the worker thread while the
// compositor is not passing through the hooks.
void ControlTick()
{
    if (!g_sh) return;
    int32_t seq = g_sh->settingsSeq;
    if (!(seq & 1) && seq != g_lastSettingsSeq) {
        VuSettings s;
        int32_t seen = 0;
        if (SeqRead(&g_sh->settingsSeq, &s, &g_sh->settings, sizeof(s), &seen)) {
            g_lastSettingsSeq = seen;
            Config nc;
            AcquireSRWLockShared(&g_cfgLock);
            nc = g_cfg;
            ReleaseSRWLockShared(&g_cfgLock);
            SettingsToConfig(s, nc);
            SanitizeConfig(nc);
            ApplyConfig(nc, "the vrupscale app");
        }
    }
    int32_t cs = g_sh->commandSeq;
    if (cs != g_lastCmdSeq) {
        g_lastCmdSeq = cs;
        uint32_t cmd = g_sh->command;
        if (cmd == kVuCmdResetHistory) TemporalResetAll("requested by the vrupscale app");
        else if (cmd == kVuCmdReloadIni) InterlockedExchange(&g_reloadRequested, 1);
        else if (cmd == kVuCmdCalibrateGaze) GazeStartCalibration("the vrupscale app");
        else if (cmd == kVuCmdResolutionChanging) ContentHint("the vrupscale app");
    }
    const DWORD now = GetTickCount();
    if (g_lastPublish == 0 || now - g_lastPublish >= 250) {
        g_lastPublish = now;
        Publish();
    }
}

} // namespace vu
