// Translation of the text in view, shown over it (see translate.h).
#include "translate.h"
#include "core.h"
#include "deepl.h"
#include "ocr.h"
#include "trcore.h"
#include "vrlink.h"

#include <d3d11.h>
#include <dxgi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace app {

namespace {

// ---- what the threads share -------------------------------------------------------------------------

struct Config {
    bool on = false;
    std::wstring key, source = L"ja", target = L"en-US";
    float distance = 3.0f;      // metres, for a text whose distance can't be told from the two eyes
    int charsPerMinute = 3000;  // at most this much goes to DeepL in a minute
    float poseMs = 1e9f;        // the head's pose this long from now is the one a captured frame was drawn for
                                // (negative: before now); 1e9 = half the game's frame time ago
    bool log = false;           // what was read and done, in translate.log next to the settings
    unsigned gen = 0;           // counts changes of the key and the languages
};

CRITICAL_SECTION g_cs;
bool g_started = false;
Config g_cfg;
TranslateStatus g_status;
std::map<std::wstring, std::wstring> g_cache;   // a text -> its translation (this run's)
std::deque<std::wstring> g_queue;               // texts DeepL has yet to be asked for
std::set<std::wstring> g_asked;                 // ... and those, with the ones being asked right now
std::wstring g_trouble;                         // the last request's failure ("" after one that worked)
bool g_fatal = false;                           // ... which asking again won't mend (until the key changes)
bool g_limited = false;                         // charsPerMinute is holding texts back
uint64_t g_sent = 0;
bool g_usageKnown = false;
uint64_t g_usageCount = 0, g_usageLimit = 0;

HANDLE g_worker = nullptr, g_http = nullptr, g_wake = nullptr, g_httpWake = nullptr;
volatile LONG g_stop = 0;

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

std::wstring Trimmed(const std::wstring &s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

void ReadConfig(Config &c)
{
    const Config d;
    c.key = Trimmed(ReadIniString(L"translate", L"deeplKey"));
    c.source = Trimmed(ReadIniString(L"translate", L"source"));
    c.target = Trimmed(ReadIniString(L"translate", L"target"));
    if (c.source.empty()) c.source = d.source;
    if (c.target.empty()) c.target = d.target;
    const std::wstring dist = ReadIniString(L"translate", L"distance"), cpm = ReadIniString(L"translate", L"charsPerMinute"),
                       pose = Trimmed(ReadIniString(L"translate", L"poseMs")), log = ReadIniString(L"translate", L"log");
    c.distance = dist.empty() ? d.distance : (float)wcstod(dist.c_str(), nullptr);
    if (!(c.distance >= 0.3f && c.distance <= 100.0f)) c.distance = d.distance;
    c.charsPerMinute = cpm.empty() ? d.charsPerMinute : _wtoi(cpm.c_str());
    if (c.charsPerMinute < 100) c.charsPerMinute = 100;
    c.poseMs = pose.empty() ? d.poseMs : (float)wcstod(pose.c_str(), nullptr);
    c.log = !log.empty() && log[0] == L'1';
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"VRUPSCALE_TRANSLATE_LOG", env, 8)) c.log = true;   // (tests)
}

// ---- the log ([translate] log=1): what was read and what was done with it ---------------------------

FILE *g_log = nullptr;

void LogLine(const wchar_t *fmt, ...)
{
    if (!g_log) return;
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    char u8[6200];
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
    if (n <= 0) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, u8);
    fflush(g_log);
}

void OpenLog(bool on)
{
    if (on == (g_log != nullptr)) return;
    if (!on) {
        fclose(g_log);
        g_log = nullptr;
        return;
    }
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"VRUPSCALE_TRANSLATE_LOG", path, MAX_PATH * 2);
    const std::wstring p = n > 1 && n < MAX_PATH * 2 ? std::wstring(path, n) : DataDir() + L"\\translate.log";
    g_log = _wfopen(p.c_str(), L"wb");
}

std::wstring OneLine(std::wstring s)
{
    for (wchar_t &c : s)
        if (c == L'\n' || c == L'\r') c = L'/';
    return s;
}

// ---- DeepL, on a thread of its own ----------------------------------------------------------------

DWORD WINAPI HttpThread(LPVOID)
{
    std::deque<std::pair<DWORD, int>> window;   // what went out in the last minute: when, characters
    DWORD backoff = 0, retryAt = 0, usageAt = 0;
    unsigned usageGen = ~0u;
    while (!g_stop) {
        WaitForSingleObject(g_httpWake, 250);
        if (g_stop) break;
        std::vector<std::wstring> batch;
        Config cfg;
        int chars = 0;
        const DWORD now = GetTickCount();
        bool usageDue = false;
        {
            Lock l;
            cfg = g_cfg;
            if (!cfg.on || !DeepLKeyPlausible(cfg.key)) continue;
            if (g_fatal) continue;
            if (retryAt && (int)(now - retryAt) < 0) continue;
            while (!window.empty() && now - window.front().first > 60000) window.pop_front();
            int used = 0;
            for (auto &w : window) used += w.second;
            g_limited = false;
            while (!g_queue.empty() && batch.size() < 16) {
                const int n = (int)g_queue.front().size();
                if (used + chars + n > cfg.charsPerMinute && used + chars > 0) {
                    g_limited = true;
                    break;
                }
                chars += n;
                batch.push_back(g_queue.front());
                g_queue.pop_front();
            }
            usageDue = batch.empty() && (usageGen != cfg.gen || now - usageAt > 600000);
        }
        if (usageDue) {   // how much of the month's allowance is used (the key alone goes out)
            usageGen = cfg.gen;
            usageAt = now;
            const DeepLUsage u = DeepLGetUsage(cfg.key);
            Lock l;
            g_usageKnown = u.ok;
            g_usageCount = u.count;
            g_usageLimit = u.limit;
            continue;
        }
        if (batch.empty()) continue;
        const DeepLResult r = DeepLTranslate(cfg.key, batch, cfg.source, cfg.target);
        LogLine(L"deepl: %d texts, %d characters: %ls after %lu ms", (int)batch.size(), chars, r.ok ? L"translated" : r.error.c_str(),
                (unsigned long)(GetTickCount() - now));
        {
            Lock l;
            const bool current = cfg.gen == g_cfg.gen;   // (else the languages or the key changed meanwhile)
            if (r.ok) {
                for (size_t i = 0; i < batch.size(); ++i) {
                    if (current) g_cache[batch[i]] = r.texts[i];
                    g_asked.erase(batch[i]);
                }
                g_sent += (uint64_t)chars;
                g_usageCount += (uint64_t)chars;
                window.push_back({ now, chars });
                g_trouble.clear();
                backoff = 0;
                retryAt = 0;
            } else {
                if (current)
                    for (size_t i = batch.size(); i-- > 0;) g_queue.push_front(batch[i]);   // asked again later
                else
                    for (auto &t : batch) g_asked.erase(t);
                g_trouble = r.error;
                g_fatal = r.fatal && current;
                backoff = backoff ? std::min<DWORD>(backoff * 2, 30000) : 3000;
                retryAt = (GetTickCount() + backoff) | 1;
            }
        }
        SetEvent(g_wake);
    }
    return 0;
}

// ---- OpenVR's C interface, by slot (as vroverlay.cpp) -------------------------------------------------

typedef uint64_t Handle;
struct RawM34 { float m[3][4]; };
struct RawPose {   // TrackedDevicePose_t
    RawM34 m;
    float velocity[3], angular[3];
    int32_t result;
    bool valid, connected;
};
static_assert(sizeof(RawPose) == 80, "TrackedDevicePose_t");
struct RawTexture { void *handle; int type; int colorSpace; };
enum { kOvCreate = 1, kOvDestroy = 3, kOvErrName = 8, kOvSetWidth = 22, kOvSetTransformAbsolute = 33, kOvShow = 43, kOvHide = 44, kOvSetTexture = 60 };
enum { kSysProjectionRaw = 2, kSysEyeToHead = 5, kSysPoses = 12 };
enum { kStanding = 1, kTexDirectX = 0, kColorGamma = 1 };
typedef int(__stdcall *PFN_OvCreate)(const char *key, const char *name, Handle *h);
typedef int(__stdcall *PFN_OvHandle)(Handle);
typedef const char *(__stdcall *PFN_OvErrName)(int);
typedef int(__stdcall *PFN_OvSetWidth)(Handle, float);
typedef int(__stdcall *PFN_OvSetTransform)(Handle, int origin, RawM34 *m);
typedef int(__stdcall *PFN_OvSetTexture)(Handle, RawTexture *t);
typedef void(__stdcall *PFN_SysProjection)(int eye, float *l, float *r, float *t, float *b);
typedef RawM34(__stdcall *PFN_SysEyeToHead)(int eye);
typedef void(__stdcall *PFN_SysPoses)(int origin, float seconds, RawPose *poses, uint32_t count);

M34 FromRaw(const RawM34 &r)
{
    M34 m;
    memcpy(m.m, r.m, sizeof(m.m));
    return m;
}

// ---- pictures: the DLL's shared textures, read on a device of ours ---------------------------------

uint8_t g_gamma[4097];   // linear 0..1 (in 4096 steps) -> display-encoded 8 bits
void InitGamma()
{
    for (int i = 0; i <= 4096; ++i) {
        const float c = i / 4096.0f, s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
        g_gamma[i] = (uint8_t)lroundf(s * 255.0f);
    }
}
inline uint8_t Encode(float linear) { return g_gamma[linear <= 0 ? 0 : (linear >= 1 ? 4096 : (int)(linear * 4096.0f))]; }

float HalfToFloat(uint16_t h)
{
    const int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : (e == 31 ? 65504.0f : ldexpf((float)(m + 1024), e - 25));
    return (h & 0x8000) ? -v : v;
}
float SmallFloat(uint32_t bits, int mantissaBits)   // the 11- and 10-bit floats of R11G11B10 (no sign)
{
    const int e = (int)(bits >> mantissaBits), m = (int)(bits & ((1u << mantissaBits) - 1));
    if (e == 0) return ldexpf((float)m, -14 - mantissaBits);
    if (e == 31) return 65504.0f;
    return ldexpf((float)(m + (1 << mantissaBits)), e - 15 - mantissaBits);
}

// One row of a texture as 8-bit red, green and blue. False: a format this doesn't read.
bool RowToRgb(uint32_t format, const uint8_t *src, int w, uint8_t *rgb)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (int x = 0; x < w; ++x, src += 4, rgb += 3) { rgb[0] = src[0]; rgb[1] = src[1]; rgb[2] = src[2]; }
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        for (int x = 0; x < w; ++x, src += 4, rgb += 3) { rgb[0] = src[2]; rgb[1] = src[1]; rgb[2] = src[0]; }
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        for (int x = 0; x < w; ++x, src += 4, rgb += 3) {
            uint32_t v;
            memcpy(&v, src, 4);
            rgb[0] = (uint8_t)((v & 1023) >> 2); rgb[1] = (uint8_t)(((v >> 10) & 1023) >> 2); rgb[2] = (uint8_t)(((v >> 20) & 1023) >> 2);
        }
        return true;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        for (int x = 0; x < w; ++x, src += 8, rgb += 3) { rgb[0] = src[1]; rgb[1] = src[3]; rgb[2] = src[5]; }
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int x = 0; x < w; ++x, src += 8, rgb += 3) {
            uint16_t h[3];
            memcpy(h, src, 6);
            for (int c = 0; c < 3; ++c) rgb[c] = Encode(HalfToFloat(h[c]));
        }
        return true;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        for (int x = 0; x < w; ++x, src += 16, rgb += 3) {
            float f[3];
            memcpy(f, src, 12);
            for (int c = 0; c < 3; ++c) rgb[c] = Encode(f[c]);
        }
        return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        for (int x = 0; x < w; ++x, src += 4, rgb += 3) {
            uint32_t v;
            memcpy(&v, src, 4);
            rgb[0] = Encode(SmallFloat(v & 2047, 6)); rgb[1] = Encode(SmallFloat((v >> 11) & 2047, 6)); rgb[2] = Encode(SmallFloat(v >> 22, 5));
        }
        return true;
    }
    return false;
}

struct Picture {
    Gray gray;                    // placed in its eye's picture (x0, y0)
    std::vector<uint32_t> bgra;   // the same in colour (the left eye's only)
};

// ---- labels ---------------------------------------------------------------------------------------

constexpr int kMaxLabels = 24;

struct Label {
    bool used = false;
    std::wstring src, text;        // as read (what DeepL is asked), and its translation ("" until it is there)
    std::wstring alt;              // another reading of it seen since ...
    int altSightings = 0;          // ... that many captures in a row
    int sightings = 0, misses = 0;
    DWORD lastSeen = 0;
    // where the text is, in SteamVR's standing space: its own box
    V3 centre, right, up;
    float width = 0, height = 0, depth = 0;
    bool depthKnown = false;
    V3 seenFrom;                   // where the head was
    // how it looks: the box in the picture's pixels, a line's height, its colours
    float boxW = 0, boxH = 0, lineH = 0;
    uint32_t bg = 0, ink = 0;
    // its overlay
    Handle overlay = 0;
    DWORD overlayRefused = 0;      // when SteamVR last refused to make it (tried again a few seconds later)
    ID3D11Texture2D *tex = nullptr;
    int texW = 0, texH = 0;
    float grow = 1;
    bool drawn = false;            // tex holds `text`
    bool placed = false;           // the overlay is where the label is
    bool showing = false;
};

bool WantShown(const Label &l)
{
    return l.used && !l.text.empty() && l.text != l.src && (l.sightings >= 2 || l.src.size() >= 6);
}

// ---- the worker -------------------------------------------------------------------------------------

struct EyeSource {
    uint64_t handle = 0;
    ID3D11Texture2D *shared = nullptr, *staging = nullptr;
    UINT w = 0, h = 0;
    uint32_t format = 0;
};

template <class T> void SafeRelease(T *&p)
{
    if (p) p->Release();
    p = nullptr;
}

struct Worker {
    // the DLL's channel
    HANDLE map = nullptr, event = nullptr;
    VuShared *sh = nullptr;
    DWORD channelTry = 0, statusAt = 0, lastCapture = 0, openedAt = 0;
    uint32_t round = 0;
    bool upscaling = true;
    float gameFps = 0;
    // Direct3D: a device on the adapter the compositor's textures are on
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    EyeSource eye[2];
    uint64_t failedHandle = 0;
    std::wstring pictureError;
    // text recognition
    Ocr ocr;
    std::wstring ocrLang, ocrError;
    bool ocrMissing = false;
    DWORD ocrTry = 0;
    // SteamVR
    unsigned vrGen = 0;
    void **vrOverlay = nullptr, **vrSystem = nullptr;   // this connection's IVROverlay_028 and IVRSystem_026
    EyeView view[2];
    bool viewsKnown = false;
    Label labels[kMaxLabels];
    unsigned cfgGen = ~0u;
    bool lowPriority = false;
    double ocrMs = 0;

    // ---- channel
    void CloseChannel()
    {
        if (sh) UnmapViewOfFile(sh);
        if (map) CloseHandle(map);
        if (event) CloseHandle(event);
        sh = nullptr;
        map = event = nullptr;
        for (EyeSource &s : eye) ReleaseEye(s);   // (its textures went with it)
    }
    bool OpenChannel()
    {
        const DWORD now = GetTickCount();
        if (sh) {
            if (now - statusAt < 1000) return true;
            statusAt = now;
            // the DLL is still there, and what it is doing
            static VuStatus st;   // (large; this thread's only)
            bool ok = false;
            for (int tries = 0; tries < 2000 && !ok; ++tries) {
                const int32_t a = sh->statusSeq;
                if (a & 1) continue;
                MemoryBarrier();
                memcpy(&st, (const void *)&sh->status, sizeof(st));
                MemoryBarrier();
                ok = sh->statusSeq == a;
            }
            if (ok && StatusIsLive(st)) {
                upscaling = st.current.enabled != 0;
                gameFps = 0;
                for (uint32_t i = 0; i < st.streamCount && i < kVuMaxStreams; ++i) gameFps = std::max(gameFps, st.streams[i].runsPerSec);
                return true;
            }
            CloseChannel();
            return false;
        }
        if (channelTry && now - channelTry < 1000) return false;
        channelTry = now | 1;
        map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_channelName.c_str());
        sh = map ? (VuShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VuShared)) : nullptr;
        if (!sh || sh->magic != kVuMagic || sh->layout != kVuLayout) {
            CloseChannel();
            return false;
        }
        event = OpenEventW(SYNCHRONIZE, FALSE, (g_channelName + VU_CAPTURE_EVENT_SUFFIX).c_str());
        VuCapture c;
        round = ReadCapture(c) ? c.round : 0;   // (what is there now is old)
        statusAt = 0;
        openedAt = lastCapture = now;
        return true;
    }
    bool ReadCapture(VuCapture &c)
    {
        for (int tries = 0; sh && tries < 2000; ++tries) {
            const int32_t a = sh->captureSeq;
            if (a & 1) continue;
            MemoryBarrier();
            memcpy(&c, (const void *)&sh->capture, sizeof(c));
            MemoryBarrier();
            if (sh->captureSeq == a) return true;
        }
        return false;
    }

    // ---- Direct3D
    void ReleaseEye(EyeSource &s)
    {
        SafeRelease(s.shared);
        SafeRelease(s.staging);
        s = EyeSource();
    }
    void ReleaseDevice()
    {
        for (EyeSource &s : eye) ReleaseEye(s);
        for (Label &l : labels) {
            SafeRelease(l.tex);
            l.drawn = false;
        }
        SafeRelease(ctx);
        SafeRelease(dev);
    }
    // The shared texture opened on the adapter that has it (a device there replaces ours).
    ID3D11Texture2D *OpenOnItsAdapter(uint64_t handle)
    {
        // from System32, never a dxgi.dll that sits next to the exe
        HMODULE d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        typedef HRESULT(WINAPI * PFN_CreateFactory1)(REFIID, void **);
        PFN_D3D11_CREATE_DEVICE create = d3d ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
        PFN_CreateFactory1 cf = dxgi ? (PFN_CreateFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
        IDXGIFactory1 *f = nullptr;
        if (!create || !cf || FAILED(cf(__uuidof(IDXGIFactory1), (void **)&f)) || !f) return nullptr;
        ID3D11Texture2D *t = nullptr;
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; !t && f->EnumAdapters1(i, &a) == S_OK; ++i) {
            ID3D11Device *d = nullptr;
            ID3D11DeviceContext *c = nullptr;
            if (SUCCEEDED(create(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)) && d) {
                if (SUCCEEDED(d->OpenSharedResource((HANDLE)(uintptr_t)handle, __uuidof(ID3D11Texture2D), (void **)&t)) && t) {
                    ReleaseDevice();
                    dev = d;
                    ctx = c;
                    d = nullptr;
                    c = nullptr;
                }
            }
            SafeRelease(c);
            SafeRelease(d);
            a->Release();
            a = nullptr;
        }
        f->Release();
        return t;
    }
    bool OpenEye(int i, const VuCaptureEye &e)
    {
        EyeSource &s = eye[i];
        if (s.shared && s.handle == e.handle && s.w == e.w && s.h == e.h && s.format == e.format) return true;
        ReleaseEye(s);
        if (!e.handle || e.handle == failedHandle) return false;
        ID3D11Texture2D *t = nullptr;
        if (dev && FAILED(dev->OpenSharedResource((HANDLE)(uintptr_t)e.handle, __uuidof(ID3D11Texture2D), (void **)&t))) t = nullptr;
        if (!t) t = OpenOnItsAdapter(e.handle);
        if (!t) {
            failedHandle = e.handle;
            pictureError = L"the picture from SteamVR's compositor can't be opened";
            return false;
        }
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        s.shared = t;
        s.handle = e.handle;
        s.w = e.w;
        s.h = e.h;
        s.format = e.format;
        const bool same = d.Width == e.w && d.Height == e.h && (uint32_t)d.Format == e.format;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!same || FAILED(dev->CreateTexture2D(&d, nullptr, &s.staging)) || !s.staging) {
            ReleaseEye(s);
            failedHandle = e.handle;
            pictureError = L"the picture from SteamVR's compositor can't be read";
            return false;
        }
        return true;
    }
    bool ReadEye(int i, const VuCaptureEye &e, Picture &p, bool colour)
    {
        if (!OpenEye(i, e)) return false;
        EyeSource &s = eye[i];
        ctx->CopyResource(s.staging, s.shared);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m))) {
            pictureError = L"the picture from SteamVR's compositor can't be read";
            return false;
        }
        const int w = (int)e.w, h = (int)e.h;
        p.gray.w = w;
        p.gray.h = h;
        p.gray.x0 = (int)e.cropX - (int)e.picX;
        p.gray.y0 = (int)e.cropY - (int)e.picY;
        p.gray.px.resize((size_t)w * h);
        if (colour) p.bgra.resize((size_t)w * h);
        std::vector<uint8_t> rgb((size_t)w * 3);
        bool ok = true;
        for (int y = 0; y < h && ok; ++y) {
            ok = RowToRgb(e.format, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, w, rgb.data());
            uint8_t *g = &p.gray.px[(size_t)y * w];
            for (int x = 0; x < w; ++x) g[x] = (uint8_t)((rgb[x * 3] * 77 + rgb[x * 3 + 1] * 150 + rgb[x * 3 + 2] * 29) >> 8);
            if (colour) {
                uint32_t *c = &p.bgra[(size_t)y * w];
                for (int x = 0; x < w; ++x) c[x] = 0xff000000u | ((uint32_t)rgb[x * 3] << 16) | ((uint32_t)rgb[x * 3 + 1] << 8) | rgb[x * 3 + 2];
            }
        }
        ctx->Unmap(s.staging, 0);
        if (!ok) pictureError = Fmt(L"the game's picture is in a format this can't read (%u)", e.format);
        return ok;
    }

    // ---- SteamVR: waits up to waitMs for the connection (another thread may be using it for a moment).
    // 1: f ran; 0: the connection stayed busy; -1: there is none.
    template <class F> int WithVr(int waitMs, F f)
    {
        const DWORD t0 = GetTickCount();
        for (;;) {
            {
                VrLinkUse link;
                if (!link.busy()) {
                    if (!link.ok()) return -1;
                    if (link.generation() != vrGen) {   // a new connection: its overlays are not ours any more
                        vrGen = link.generation();
                        vrOverlay = (void **)link.Interface("FnTable:IVROverlay_028");
                        vrSystem = (void **)link.Interface("FnTable:IVRSystem_026");
                        viewsKnown = false;
                        for (Label &l : labels) {
                            l.overlay = 0;
                            l.placed = l.showing = false;
                        }
                    }
                    void **ov = vrOverlay, **sys = vrSystem;
                    if (!ov || !sys) return -1;
                    if (!viewsKnown) {
                        for (int e = 0; e < 2; ++e) {
                            ((PFN_SysProjection)sys[kSysProjectionRaw])(e, &view[e].l, &view[e].r, &view[e].t, &view[e].b);
                            view[e].toHead = FromRaw(((PFN_SysEyeToHead)sys[kSysEyeToHead])(e));
                        }
                        viewsKnown = view[0].r - view[0].l > 0.1f && fabsf(view[0].b - view[0].t) > 0.1f;
                        LogLine(L"eyes: left %.4f %.4f %.4f %.4f at x %.4f, right %.4f %.4f %.4f %.4f at x %.4f", view[0].l, view[0].r, view[0].t,
                                view[0].b, view[0].toHead.m[0][3], view[1].l, view[1].r, view[1].t, view[1].b, view[1].toHead.m[0][3]);
                    }
                    f(ov, sys);
                    return 1;
                }
            }
            if ((int)(GetTickCount() - t0) >= waitMs) return 0;
            Sleep(1);
        }
    }

    // ---- labels
    void DropLabel(Label &l)
    {
        // (its overlay stays, hidden, for the next label in this place of the table)
        const Handle ov = l.overlay;
        ID3D11Texture2D *tex = l.tex;
        const int tw = l.texW, th = l.texH;
        const bool showing = l.showing;
        const DWORD refused = l.overlayRefused;
        l = Label();
        l.overlayRefused = refused;
        l.overlay = ov;
        l.tex = tex;
        l.texW = tw;
        l.texH = th;
        l.showing = showing;   // hidden by the next SyncLabels
    }
    void DropAllLabels()
    {
        for (Label &l : labels)
            if (l.used) DropLabel(l);
    }
    // Shows, moves, redraws and hides the overlays to match the labels.
    void SyncLabels(void **ov)
    {
        for (int i = 0; i < kMaxLabels; ++i) {
            Label &l = labels[i];
            const bool want = WantShown(l) && dev;
            if (!want) {
                if (l.showing && l.overlay) ((PFN_OvHandle)ov[kOvHide])(l.overlay);
                l.showing = false;
                continue;
            }
            if (!l.overlay) {
                if (l.overlayRefused && GetTickCount() - l.overlayRefused < 5000) continue;
                char key[64];
                snprintf(key, sizeof(key), "zakhazerd.vrupscale.label.%d", i);
                Handle h = 0;
                const int e = ((PFN_OvCreate)ov[kOvCreate])(key, "vrupscale translation", &h);
                if (e != 0 || !h) {
                    const char *n = ((PFN_OvErrName)ov[kOvErrName])(e);
                    LogLine(L"overlay %d: SteamVR refused it (%ls)", i, n ? Widen(n).c_str() : L"?");
                    l.overlayRefused = GetTickCount() | 1;
                    continue;
                }
                l.overlay = h;
                l.overlayRefused = 0;
                l.placed = l.drawn = false;
            }
            if (!l.drawn) {
                const LabelPicture pic = DrawLabel(l.text, l.boxW, l.boxH, l.lineH, l.bg, l.ink);
                if (pic.px.empty()) continue;
                if (l.tex && (l.texW != pic.w || l.texH != pic.h)) SafeRelease(l.tex);
                if (!l.tex) {
                    D3D11_TEXTURE2D_DESC d = {};
                    d.Width = (UINT)pic.w;
                    d.Height = (UINT)pic.h;
                    d.MipLevels = 1;
                    d.ArraySize = 1;
                    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    d.SampleDesc.Count = 1;
                    d.Usage = D3D11_USAGE_DEFAULT;
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (FAILED(dev->CreateTexture2D(&d, nullptr, &l.tex)) || !l.tex) continue;
                    l.texW = pic.w;
                    l.texH = pic.h;
                }
                ctx->UpdateSubresource(l.tex, 0, nullptr, pic.px.data(), (UINT)pic.w * 4, 0);
                ctx->Flush();
                RawTexture t = { l.tex, kTexDirectX, kColorGamma };
                ((PFN_OvSetTexture)ov[kOvSetTexture])(l.overlay, &t);
                l.grow = pic.grow;
                l.drawn = true;
                l.placed = false;
            }
            if (!l.placed) {
                const V3 back = { l.right.y * l.up.z - l.right.z * l.up.y, l.right.z * l.up.x - l.right.x * l.up.z,
                                  l.right.x * l.up.y - l.right.y * l.up.x };   // towards the viewer
                RawM34 m = { { { l.right.x, l.up.x, back.x, l.centre.x }, { l.right.y, l.up.y, back.y, l.centre.y }, { l.right.z, l.up.z, back.z, l.centre.z } } };
                ((PFN_OvSetWidth)ov[kOvSetWidth])(l.overlay, l.width * l.grow);
                ((PFN_OvSetTransform)ov[kOvSetTransformAbsolute])(l.overlay, kStanding, &m);
                l.placed = true;
            }
            if (!l.showing) {
                ((PFN_OvHandle)ov[kOvShow])(l.overlay);
                l.showing = true;
            }
        }
    }
    void DestroyOverlays()
    {
        bool any = false;
        for (Label &l : labels) any = any || l.overlay;
        if (any)
            WithVr(200, [&](void **ov, void **) {
                for (Label &l : labels)
                    if (l.overlay) ((PFN_OvHandle)ov[kOvDestroy])(l.overlay);
            });
        for (Label &l : labels) {
            SafeRelease(l.tex);
            l = Label();
        }
    }

    // What this capture shows: the labels found again, new ones, and those that are gone.
    // L: the left eye's part (or the only eye's), le that eye's view; R, re: the right eye's, if it was captured too.
    void Process(const VuCapture &c, const Config &cfg, Picture &L, const VuCaptureEye &ce, EyeView le, Picture *R, EyeView re,
                 const M34 &head)
    {
        le.picW = (float)ce.picW;
        le.picH = (float)ce.picH;
        if (R) {
            re.picW = (float)c.eye[1].picW;
            re.picH = (float)c.eye[1].picH;
        }
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        OcrPage page;
        std::wstring err;
        if (!ocr.Recognize(L.gray.px.data(), L.gray.w, L.gray.h, L.gray.w, page, err)) {
            ocrError = err;
            return;
        }
        QueryPerformanceCounter(&t1);
        ocrMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        ocrError.clear();
        const std::vector<TextBlock> blocks = GroupText(page, L.gray.w, L.gray.h, cfg.source);

        struct Found {
            const TextBlock *b;
            std::wstring text;
            float px, py, depth;
            bool depthKnown;
            int label;   // the label it is a new reading of (-1: none)
        };
        std::vector<Found> found;
        std::vector<float> depths;
        for (const TextBlock &b : blocks) {
            if (b.text.size() > 500 || !WorthTranslating(b.text, cfg.source)) continue;
            Found fd = { &b, b.text, b.cx + (float)L.gray.x0, b.cy + (float)L.gray.y0, 0, false, -1 };
            if (R) fd.depthKnown = StereoDepth(L.gray, le, R->gray, re, fd.px, fd.py, b.w, b.h, &fd.depth);
            if (fd.depthKnown) depths.push_back(fd.depth);
            found.push_back(fd);
        }
        // a text whose distance can't be told: as far as the others in view, else the setting
        std::sort(depths.begin(), depths.end());
        const float guess = depths.empty() ? cfg.distance : depths[depths.size() / 2];
        const V3 headPos = { head.m[0][3], head.m[1][3], head.m[2][3] };
        const DWORD now = GetTickCount();
        bool seen[kMaxLabels] = {};
        QueryPerformanceCounter(&t0);
        LogLine(L"capture %u: %d x %d, %s, read in %.0f ms: %d lines, %d blocks, %d worth translating (distances in %.0f ms)", c.round,
                L.gray.w, L.gray.h, R ? L"both eyes" : L"one eye", ocrMs, (int)page.lines.size(), (int)blocks.size(), (int)found.size(),
                (double)(t0.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart);

        // Where each label is in this picture (its centre; its size is the box it was made for).
        M34 toHead;   // the standing space -> the head's
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) toHead.m[r][k] = head.m[k][r];
            toHead.m[r][3] = -(head.m[0][r] * headPos.x + head.m[1][r] * headPos.y + head.m[2][r] * headPos.z);
        }
        struct Spot { bool ahead; float px, py; } spot[kMaxLabels] = {};
        for (int i = 0; i < kMaxLabels; ++i)
            if (labels[i].used) spot[i].ahead = ProjectToPixel(le, Apply(toHead, labels[i].centre), &spot[i].px, &spot[i].py);
        // First the blocks that are a label's text again: the same text (or nearly: a reading differs
        // by a character now and then) about where the label is. "About" allows for the label having
        // been placed with the head's pose a moment off.
        for (Found &fd : found) {
            float nearest = 1e9f;
            for (int i = 0; i < kMaxLabels; ++i) {
                const Label &l = labels[i];
                if (!l.used || seen[i] || !spot[i].ahead) continue;
                if (std::max(TextSimilarity(fd.text, l.src), l.alt.empty() ? 0.0f : TextSimilarity(fd.text, l.alt)) < 0.7f) continue;
                const float d = hypotf(fd.px - spot[i].px, fd.py - spot[i].py);
                if (d < 0.5f * hypotf(l.boxW, l.boxH) + 0.03f * le.picW && d < nearest) {
                    nearest = d;
                    fd.label = i;
                }
            }
            if (fd.label >= 0) seen[fd.label] = true;
        }

        for (Found &fd : found) {
            const TextBlock &b = *fd.b;
            if (!fd.depthKnown) fd.depth = guess;
            // the label is a little larger than the text's own box, so that it covers it
            const float boxW = b.w + 0.5f * b.lineH, boxH = b.h + 0.4f * b.lineH;
            const Placement p = PlaceBlock(le, fd.px, fd.py, boxW, boxH, b.angle, fd.depth);
            const V3 centre = Apply(head, p.centre), right = Rotate(head, p.right), up = Rotate(head, p.up);
            const int best = fd.label;
            if (best < 0) {
                // a text that is no label's: a label that was not read again and has this text's
                // middle inside it showed what was written there before
                for (int i = 0; i < kMaxLabels; ++i) {
                    Label &l = labels[i];
                    if (!l.used || seen[i] || !spot[i].ahead) continue;
                    if (fabsf(fd.px - spot[i].px) < 0.5f * l.boxW && fabsf(fd.py - spot[i].py) < 0.5f * l.boxH) {
                        LogLine(L"  label %d \"%ls\" is now \"%ls\"", i, OneLine(l.src).c_str(), OneLine(fd.text).c_str());
                        DropLabel(l);
                    }
                }
                int slot = -1;
                for (int i = 0; i < kMaxLabels && slot < 0; ++i)
                    if (!labels[i].used) slot = i;
                if (slot < 0) {   // all taken: of those not read this time, the one not seen for longest goes
                    for (int i = 0; i < kMaxLabels; ++i)
                        if (!seen[i] && (slot < 0 || (int)(labels[i].lastSeen - labels[slot].lastSeen) < 0)) slot = i;
                    if (slot < 0) continue;
                    DropLabel(labels[slot]);
                }
                Label &l = labels[slot];
                l.used = true;
                l.src = fd.text;
                l.sightings = 1;
                l.lastSeen = now;
                l.centre = centre; l.right = right; l.up = up;
                l.width = p.width; l.height = p.height; l.depth = fd.depth;
                l.depthKnown = fd.depthKnown;
                l.seenFrom = headPos;
                l.boxW = boxW; l.boxH = boxH; l.lineH = b.lineH;
                TextColours(L.bgra.data(), L.gray.w, L.gray.h, L.gray.w, b.cx, b.cy, b.w, b.h, &l.bg, &l.ink);
                l.drawn = l.placed = false;
                seen[slot] = true;
                LogLine(L"  new %d \"%ls\" at %.2f m%s, %.0f x %.0f px, lines of %.0f px, turned %.1f", slot, OneLine(l.src).c_str(), l.depth,
                        l.depthKnown ? L"" : L" (guessed)", b.w, b.h, b.lineH, b.angle);
                continue;
            }
            Label &l = labels[best];
            ++l.sightings;
            l.misses = 0;
            l.lastSeen = now;
            l.seenFrom = headPos;
            // another reading of the same text is taken once it has been read that way three times running
            if (fd.text == l.src) {
                l.alt.clear();
                l.altSightings = 0;
            } else if (fd.text == l.alt) {
                if (++l.altSightings >= 3) {
                    LogLine(L"  label %d \"%ls\" is read \"%ls\" now", best, OneLine(l.src).c_str(), OneLine(l.alt).c_str());
                    l.src = l.alt;
                    l.text.clear();
                    l.alt.clear();
                    l.altSightings = 0;
                    l.drawn = false;
                }
            } else {
                l.alt = fd.text;
                l.altSightings = 1;
            }
            // moved only when it is off by more than the reading's own jitter, so that a label on a still text stands still
            const float off = Length(Sub(centre, l.centre)) / std::max(0.3f, fd.depth);   // radians, about
            const bool knowBetter = fd.depthKnown && !l.depthKnown;
            const bool resized = fabsf(p.width / l.width - 1) > 0.1f || fabsf(p.height / l.height - 1) > 0.15f;
            if (off > 0.005f || resized || knowBetter || (fd.depthKnown && fabsf(fd.depth / l.depth - 1) > 0.15f)) {
                l.centre = centre; l.right = right; l.up = up;
                l.width = p.width; l.height = p.height; l.depth = fd.depth;
                l.depthKnown = fd.depthKnown || l.depthKnown;
                l.placed = false;
                if (resized) {
                    l.boxW = boxW; l.boxH = boxH; l.lineH = b.lineH;
                    l.drawn = false;
                }
            }
        }

        // labels not found this time: one that should have been in the picture goes after two captures
        // without it; one out of the picture stays for ten seconds, unless the head has moved away
        for (int i = 0; i < kMaxLabels; ++i) {
            Label &l = labels[i];
            if (!l.used || seen[i]) continue;
            const bool ahead = spot[i].ahead;
            const float px = spot[i].px, py = spot[i].py;
            const float mx = 0.1f * L.gray.w, my = 0.1f * L.gray.h;
            const bool inPicture = ahead && px > L.gray.x0 + mx && px < L.gray.x0 + L.gray.w - mx && py > L.gray.y0 + my && py < L.gray.y0 + L.gray.h - my;
            bool gone = false;
            if (inPicture) gone = ++l.misses >= 2;
            else gone = now - l.lastSeen > 10000 || Length(Sub(headPos, l.seenFrom)) > 0.35f;
            if (gone) {
                LogLine(L"  gone %d \"%ls\"", i, OneLine(l.src).c_str());
                DropLabel(l);
            }
        }
    }

    // Translations that have arrived go onto their labels; texts not asked for yet are queued.
    void Translations(const Config &cfg)
    {
        bool wake = false;
        Lock lock;
        for (Label &l : labels) {
            if (!l.used || !l.text.empty()) continue;
            auto it = g_cache.find(l.src);
            if (it != g_cache.end()) {
                l.text = it->second.empty() ? l.src : it->second;
                l.drawn = false;
                LogLine(L"  translated \"%ls\" -> \"%ls\"", OneLine(l.src).c_str(), OneLine(l.text).c_str());
                continue;
            }
            if (!(l.sightings >= 2 || l.src.size() >= 6) || g_asked.count(l.src) || g_queue.size() >= 64) continue;
            g_asked.insert(l.src);
            g_queue.push_back(l.src);
            wake = true;
        }
        (void)cfg;
        if (wake) SetEvent(g_httpWake);
    }

    void SetStatus(TranslateStatus::State state, const std::wstring &line, const std::wstring &command = std::wstring())
    {
        int showing = 0;
        for (const Label &l : labels) showing += l.showing ? 1 : 0;
        Lock lock;
        g_status.state = state;
        g_status.line = line;
        g_status.ocrCommand = command;
        g_status.showing = showing;
        g_status.sent = g_sent;
        g_status.hasKey = DeepLKeyPlausible(g_cfg.key);
    }

    void Idle()   // off, or nothing to read: no labels
    {
        bool any = false;
        for (Label &l : labels) any = any || l.used || l.showing;
        if (!any) return;
        DropAllLabels();
        WithVr(50, [&](void **ov, void **) { SyncLabels(ov); });
    }

    // One turn. Returns how long to wait for the next (ms).
    DWORD Step()
    {
        Config cfg;
        {
            Lock lock;
            cfg = g_cfg;
        }
        OpenLog(cfg.on && cfg.log);
        if (cfg.on != lowPriority) {   // reading text must not take CPU time from the game
            lowPriority = cfg.on;
            SetPriorityClass(GetCurrentProcess(), cfg.on ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
        }
        if (!cfg.on) {
            Idle();
            DestroyOverlays();
            ocr.Close();
            ocrLang.clear();
            CloseChannel();
            ReleaseDevice();
            SetStatus(TranslateStatus::Off, std::wstring());
            return 1000;
        }
        if (cfg.gen != cfgGen) {   // another language or key: what is shown was made with the old ones
            cfgGen = cfg.gen;
            DropAllLabels();
        }
        const std::wstring pair = LanguageName(cfg.source) + L" to " + LanguageName(cfg.target);
        if (ocrLang != cfg.source || (!ocr.IsOpen() && GetTickCount() - ocrTry > 5000)) {
            ocrLang = cfg.source;
            ocrTry = GetTickCount();
            ocrMissing = false;
            if (!ocr.Open(cfg.source, ocrError, &ocrMissing)) LogLine(L"ocr: %ls", ocrError.c_str());
            else LogLine(L"ocr: reading %ls", cfg.source.c_str());
        }
        if (!ocr.IsOpen()) {
            Idle();
            if (ocrMissing)
                SetStatus(TranslateStatus::NoOcr, L"Windows has no " + LanguageName(cfg.source) + L" text recognition yet. Run the command in an "
                          L"administrator PowerShell (Copy command), then it starts by itself.", OcrInstallCommand(cfg.source));
            else
                SetStatus(TranslateStatus::Trouble, L"Text recognition didn't start: " + ocrError + L".");
            return 1000;
        }
        if (!DeepLKeyPlausible(cfg.key)) {
            Idle();
            SetStatus(TranslateStatus::NoKey, L"Needs your DeepL API key: copy it at deepl.com (Account, API keys) and press Paste key. Texts it "
                      L"reads go to DeepL from then on.");
            return 1000;
        }
        if (!OpenChannel()) {
            Idle();
            SetStatus(TranslateStatus::NoSteamVR, pair + L". Starts when SteamVR runs with vrupscale installed.");
            return 500;
        }

        // ---- a new capture?
        VuCapture c;
        const DWORD now = GetTickCount();
        if (ReadCapture(c) && c.round != round) {
            round = c.round;
            if (!c.eyes) {   // the DLL's textures are gone (Translate went off there)
                for (EyeSource &s : eye) ReleaseEye(s);
            } else {
                // the head now is about where it was for the frame captured (see Config::poseMs)
                float lead = cfg.poseMs < 1e8f ? cfg.poseMs * 0.001f : (gameFps > 5 ? -0.5f / gameFps : 0.0f);
                lead = std::max(-0.1f, std::min(0.05f, lead));
                RawPose pose = {};
                const bool vr = WithVr(4, [&](void **, void **sys) { ((PFN_SysPoses)sys[kSysPoses])(kStanding, lead, &pose, 1); }) == 1;
                sh->captureAck = c.round;
                lastCapture = now;
                // the copy runs with the compositor's frame: a frame or two until it is there
                LARGE_INTEGER f, t;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t);
                const LONGLONG settle = c.qpc + f.QuadPart * kVuCaptureSettleMs / 1000 - t.QuadPart;
                if (settle > 0) Sleep((DWORD)std::min<LONGLONG>(100, settle * 1000 / f.QuadPart + 1));
                Picture L, R;
                const int first = (c.eyes & 1) ? 0 : 1;   // the left eye, or the only one
                const bool both = c.eyes == 3;
                pictureError.clear();
                bool ok = ReadEye(first, c.eye[first], L, true);
                const bool right = ok && both && ReadEye(1, c.eye[1], R, false);
                VuCapture again;
                const bool overwritten = ok && ReadCapture(again) && again.round != c.round;   // meanwhile: the next one
                LARGE_INTEGER t2;
                QueryPerformanceCounter(&t2);
                if (!ok) {
                    LogLine(L"capture %u: not read: %ls", c.round, pictureError.c_str());
                } else if (overwritten) {
                    LogLine(L"capture %u: overwritten while it was read (seen %.0f ms after it was taken, read for %.0f ms)", c.round,
                            (double)(t.QuadPart - c.qpc) * 1000.0 / (double)f.QuadPart, (double)(t2.QuadPart - t.QuadPart) * 1000.0 / (double)f.QuadPart);
                } else if (vr && pose.valid && viewsKnown) {
                    LogLine(L"capture %u: seen %.0f ms after it was taken, in memory %.0f ms later", c.round,
                            (double)(t.QuadPart - c.qpc) * 1000.0 / (double)f.QuadPart, (double)(t2.QuadPart - t.QuadPart) * 1000.0 / (double)f.QuadPart);
                    Process(c, cfg, L, c.eye[first], view[first], right ? &R : nullptr, view[1], FromRaw(pose.m));
                } else {
                    LogLine(L"capture %u: no head pose from SteamVR (%d %d %d)", c.round, vr ? 1 : 0, pose.valid ? 1 : 0, viewsKnown ? 1 : 0);
                }
            }
        }
        Translations(cfg);
        const int synced = WithVr(2, [&](void **ov, void **) { SyncLabels(ov); });

        // ---- what the page says
        std::wstring trouble;
        bool fatal = false, limited = false, usage = false;
        uint64_t sent = 0, used = 0, limit = 0;
        {
            Lock lock;
            trouble = g_trouble;
            fatal = g_fatal;
            limited = g_limited;
            sent = g_sent;
            usage = g_usageKnown;
            used = g_usageCount;
            limit = g_usageLimit;
        }
        int showing = 0;
        for (const Label &l : labels) showing += l.showing ? 1 : 0;
        if (!pictureError.empty()) {
            SetStatus(TranslateStatus::Trouble, pair + L": " + pictureError + L".");
        } else if (!ocrError.empty()) {
            SetStatus(TranslateStatus::Trouble, pair + L": " + ocrError + L".");
        } else if (!trouble.empty()) {
            SetStatus(TranslateStatus::Trouble, pair + L": " + trouble + (fatal ? L"." : L". Trying again."));
        } else if (now - lastCapture > 3000) {
            SetStatus(TranslateStatus::NoPictures, pair + (upscaling ? L". Waiting for a VR game's picture." : L". Needs Upscaling switched on: the pictures "
                                                                                                              L"it reads come from the upscaler."));
        } else {
            std::wstring line = pair + Fmt(L" with DeepL. %d in view, %llu characters sent", showing, (unsigned long long)sent);
            if (usage && limit) line += Fmt(L" (%llu of %llu used this month)", (unsigned long long)used, (unsigned long long)limit);
            line += limited ? Fmt(L". Holding texts back: %d characters a minute is the limit.", cfg.charsPerMinute) : L".";
            SetStatus(TranslateStatus::Running, line);
        }
        return synced == 0 ? 5 : 50;   // (the connection was busy: again in a moment)
    }

    void Shutdown()
    {
        DestroyOverlays();
        ocr.Close();
        CloseChannel();
        ReleaseDevice();
        OpenLog(false);
        if (lowPriority) SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    }
};

DWORD WINAPI WorkerThread(LPVOID)
{
    Worker *w = new Worker();
    while (!g_stop) {
        const DWORD wait = w->Step();
        if (g_stop) break;
        HANDLE hs[2] = { g_wake, w->event };
        WaitForMultipleObjects(w->event ? 2 : 1, hs, FALSE, wait);
    }
    w->Shutdown();
    delete w;
    return 0;
}

} // namespace

void TranslateStart()
{
    if (g_started) return;
    g_started = true;
    InitializeCriticalSection(&g_cs);
    InitGamma();
    ReadConfig(g_cfg);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_httpWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    g_http = CreateThread(nullptr, 0, HttpThread, nullptr, 0, nullptr);
}

void TranslateStop()
{
    if (!g_started) return;
    g_stop = 1;
    SetEvent(g_wake);
    SetEvent(g_httpWake);
    for (HANDLE *t : { &g_worker, &g_http }) {
        if (!*t) continue;
        WaitForSingleObject(*t, 12000);   // (a request to DeepL may be under way)
        CloseHandle(*t);
        *t = nullptr;
    }
}

void TranslateSet(bool on)
{
    if (!g_started) return;
    {
        Lock lock;
        if (g_cfg.on == on) return;
        g_cfg.on = on;
        if (on) {   // what failed before is tried again
            g_fatal = false;
            g_trouble.clear();
        }
    }
    SetEvent(g_wake);
    SetEvent(g_httpWake);
}

void TranslateReload()
{
    if (!g_started) return;
    Config c;
    ReadConfig(c);
    {
        Lock lock;
        const bool changed = c.key != g_cfg.key || c.source != g_cfg.source || c.target != g_cfg.target;
        c.on = g_cfg.on;
        c.gen = g_cfg.gen + (changed ? 1 : 0);
        g_cfg = c;
        if (changed) {
            g_cache.clear();
            g_queue.clear();
            g_asked.clear();
            g_fatal = false;
            g_trouble.clear();
            g_usageKnown = false;
        }
    }
    SetEvent(g_wake);
    SetEvent(g_httpWake);
}

TranslateStatus TranslateGetStatus()
{
    if (!g_started) return TranslateStatus();
    Lock lock;
    TranslateStatus s = g_status;
    s.hasKey = DeepLKeyPlausible(g_cfg.key);
    s.languages = LanguageName(g_cfg.source) + L" to " + LanguageName(g_cfg.target);
    return s;
}

bool TranslateSetKey(const std::wstring &key0)
{
    const std::wstring key = Trimmed(key0);
    if (!DeepLKeyPlausible(key)) return false;
    WriteIniString(L"translate", L"deeplKey", key);
    TranslateReload();
    return true;
}

bool TranslateWantsLink()
{
    if (!g_started) return false;
    Lock lock;
    return g_cfg.on;
}

// ---- self-test -------------------------------------------------------------------------------------

namespace {

// Text drawn with GDI+ for the recogniser: light on dark blue, turned about (cx, cy).
void DrawTestText(std::vector<uint32_t> &px, int W, int H, const wchar_t *text, float cx, float cy, float fontPx, float angle)
{
    Gdiplus::Bitmap bmp(W, H, W * 4, PixelFormat32bppARGB, (BYTE *)px.data());
    Gdiplus::Graphics g(&bmp);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.TranslateTransform(cx, cy);
    g.RotateTransform(angle);
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 240, 236, 228));
    g.DrawString(text, -1, &font, Gdiplus::PointF(0, 0), &fmt, &brush);
    g.Flush();
}

} // namespace

int TranslateSelfTest(std::wstring &report)
{
    int fails = TranslateCoreSelfTest(report) + DeepLSelfTest(report);
    auto check = [&](bool ok, const std::wstring &what) {
        report += Fmt(L"%s %s\n", ok ? L"ok  " : L"FAIL", what.c_str());
        if (!ok) ++fails;
    };
    InitGamma();
    {
        // one pixel of each format the compositor's textures come in: a mid orange
        uint8_t rgb[3];
        const uint8_t rgba[4] = { 200, 120, 40, 255 }, bgra[4] = { 40, 120, 200, 255 };
        bool ok = RowToRgb(DXGI_FORMAT_R8G8B8A8_UNORM, rgba, 1, rgb) && rgb[0] == 200 && rgb[1] == 120 && rgb[2] == 40;
        ok = ok && RowToRgb(DXGI_FORMAT_B8G8R8A8_UNORM, bgra, 1, rgb) && rgb[0] == 200 && rgb[1] == 120 && rgb[2] == 40;
        const uint32_t ten = (800u) | (480u << 10) | (160u << 20) | (3u << 30);
        ok = ok && RowToRgb(DXGI_FORMAT_R10G10B10A2_UNORM, (const uint8_t *)&ten, 1, rgb) && rgb[0] == 200 && rgb[1] == 120 && rgb[2] == 40;
        const uint16_t half[4] = { 0x3C00 /* 1.0 */, 0x3800 /* 0.5 */, 0x0000, 0x3C00 };
        ok = ok && RowToRgb(DXGI_FORMAT_R16G16B16A16_FLOAT, (const uint8_t *)half, 1, rgb) && rgb[0] == 255 && rgb[1] == 188 && rgb[2] == 0;
        const float fl[4] = { 1.0f, 0.5f, 0.0f, 1.0f };
        ok = ok && RowToRgb(DXGI_FORMAT_R32G32B32A32_FLOAT, (const uint8_t *)fl, 1, rgb) && rgb[0] == 255 && rgb[1] == 188 && rgb[2] == 0;
        const uint32_t small = (0x3C0u) | (0x380u << 11) | (0u << 22);   // 1.0, 0.5, 0 as 11-, 11- and 10-bit floats
        ok = ok && RowToRgb(DXGI_FORMAT_R11G11B10_FLOAT, (const uint8_t *)&small, 1, rgb) && rgb[0] == 255 && rgb[1] == 188 && rgb[2] == 0;
        ok = ok && !RowToRgb(DXGI_FORMAT_BC1_UNORM, rgba, 1, rgb);
        check(ok, L"pictures: every format an eye texture comes in is read (float ones display-encoded)");
    }
    {
        // Windows' own recogniser on text drawn here, if it has English
        Ocr ocr;
        std::wstring err;
        bool missing = false;
        if (!ocr.Open(L"en-US", err, &missing)) {
            report += L"skip ocr: " + err + L"\n";
        } else {
            const int W = 1536, H = 1024;
            std::vector<uint32_t> px((size_t)W * H, 0xff26426cu);
            DrawTestText(px, W, H, L"Welcome to the quiet world", 520, 300, 40, 0);
            DrawTestText(px, W, H, L"Please do not feed\nthe sleeping avatars", 1000, 700, 34, 0);
            std::vector<uint8_t> gray((size_t)W * H);
            for (size_t i = 0; i < px.size(); ++i) gray[i] = (uint8_t)((((px[i] >> 16) & 255) * 77 + ((px[i] >> 8) & 255) * 150 + (px[i] & 255) * 29) >> 8);
            OcrPage page;
            const bool read = ocr.Recognize(gray.data(), W, H, W, page, err);
            const std::vector<TextBlock> blocks = GroupText(page, W, H, L"en-US");
            const TextBlock *a = nullptr, *b = nullptr;
            for (auto &t : blocks) {
                if (t.text == L"Welcome to the quiet world") a = &t;
                if (t.text == L"Please do not feed the sleeping avatars") b = &t;
            }
            check(read && a && b && fabsf(a->cx - 520) < 8 && fabsf(a->cy - 300) < 8 && fabsf(b->cx - 1000) < 8 && fabsf(b->cy - 700) < 10 && b->lines == 2 &&
                      a->lineH > 24 && a->lineH < 52,
                  Fmt(L"ocr: text drawn here is read and grouped, where it is (%d blocks)", (int)blocks.size()));
            if (a) {
                uint32_t bg = 0, ink = 0;
                TextColours(px.data(), W, H, W, a->cx, a->cy, a->w, a->h, &bg, &ink);
                check((bg & 0xffffffu) == 0x26426cu && ((ink >> 16) & 255) > 200, L"ocr: the label takes the text's colours");
            }
            // the same sign turned 8 degrees clockwise
            std::fill(px.begin(), px.end(), 0xff26426cu);
            DrawTestText(px, W, H, L"Welcome to the quiet world", 900, 420, 40, 8);
            for (size_t i = 0; i < px.size(); ++i) gray[i] = (uint8_t)((((px[i] >> 16) & 255) * 77 + ((px[i] >> 8) & 255) * 150 + (px[i] & 255) * 29) >> 8);
            const bool read2 = ocr.Recognize(gray.data(), W, H, W, page, err);
            const std::vector<TextBlock> turned = GroupText(page, W, H, L"en-US");
            const TextBlock *t = nullptr;
            for (auto &k : turned)
                if (k.text == L"Welcome to the quiet world") t = &k;
            check(read2 && t && fabsf(t->cx - 900) < 10 && fabsf(t->cy - 420) < 10 && fabsf(t->angle - 8) < 2.5f,
                  Fmt(L"ocr: turned text is placed where it is (at %.0f, %.0f, turned %.1f)", t ? t->cx : 0.0f, t ? t->cy : 0.0f, t ? t->angle : 0.0f));
        }
    }
    return fails;
}

} // namespace app
