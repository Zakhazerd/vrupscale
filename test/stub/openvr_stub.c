// Stand-in for openvr_api.dll (self-test of the app's VRChat-resolution, start-with-SteamVR and
// dashboard features): records the calls vrupscale.exe makes. Env: STUB_LOG (file), STUB_INIT_ERR (VR_InitInternal2's error),
// STUB_VALUE (the value SteamVR "has"; absent = not set), STUB_VALUE_FILE (a value changed meanwhile).
// Build: gcc -O1 -shared -o openvr_api.dll test/stub/openvr_stub.c -static-libgcc
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static int Env(const char *n, int def);
static void Log(const char *fmt, ...)   // STUB_LOG_TIME=1: each line starts with GetTickCount
{
    char path[512];
    if (!GetEnvironmentVariableA("STUB_LOG", path, sizeof(path))) return;
    FILE *f = fopen(path, "ab");
    if (!f) return;
    if (Env("STUB_LOG_TIME", 0)) fprintf(f, "%lu ", (unsigned long)GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static int Env(const char *n, int def)
{
    char b[64];
    return GetEnvironmentVariableA(n, b, sizeof(b)) ? atoi(b) : def;
}
static int g_init = 0, g_has = -1, g_value = 0;

static const char *__stdcall ErrName(int e) { return e == 2 ? "VRSettingsError_WriteFailed" : "VRSettingsError_?"; }
static void __stdcall SetInt32(const char *sec, const char *key, int32_t v, int *err)
{
    Log("SetInt32 %s %s %d", sec, key, v);
    int fail = Env("STUB_SET_ERR", 0);
    if (err) *err = fail;
    if (!fail) { g_has = 1; g_value = v; }
}
static int32_t __stdcall GetInt32(const char *sec, const char *key, int *err)
{
    if (g_has < 0) { g_has = Env("STUB_VALUE", -1) >= 0; g_value = Env("STUB_VALUE", 0); }
    {   // STUB_VALUE_FILE: the value the user sets meanwhile (a number in that file), if it exists
        char path[512];
        if (GetEnvironmentVariableA("STUB_VALUE_FILE", path, sizeof(path))) {
            FILE *f = fopen(path, "rb");
            int v;
            if (f) {
                if (fscanf(f, "%d", &v) == 1) { g_has = 1; g_value = v; }
                fclose(f);
            }
        }
    }
    Log("GetInt32 %s %s -> %s %d", sec, key, g_has ? "value" : "unset", g_value);
    if (err) *err = g_has ? 0 : 5;
    return g_has ? g_value : 0;
}
static void __stdcall RemoveKey(const char *sec, const char *key, int *err)
{
    Log("RemoveKeyInSection %s %s", sec, key);
    if (err) *err = 0;
    g_has = 0;
}
// SteamVR's global resolution ("steamvr" section): STUB_SCALE (default 1.0), STUB_MANUAL (default 1)
static int g_globInit = 0, g_manual = 1;
static float g_scale = 1.0f;
static void GlobInit(void)
{
    char b[64];
    if (g_globInit) return;
    g_globInit = 1;
    if (GetEnvironmentVariableA("STUB_SCALE", b, sizeof(b))) g_scale = (float)atof(b);
    g_manual = Env("STUB_MANUAL", 1);
}
static void __stdcall SetBool(const char *sec, const char *key, bool v, int *err)
{
    GlobInit();
    Log("SetBool %s %s %d", sec, key, (int)v);
    if (!strcmp(key, "supersampleManualOverride")) g_manual = v;
    if (err) *err = 0;
}
static bool __stdcall GetBool(const char *sec, const char *key, int *err)
{
    GlobInit();
    Log("GetBool %s %s -> %d", sec, key, g_manual);
    if (err) *err = 0;
    return g_manual != 0;
}
static void __stdcall SetFloat(const char *sec, const char *key, float v, int *err)
{
    GlobInit();
    Log("SetFloat %s %s %.4f", sec, key, v);
    if (!strcmp(key, "supersampleScale")) g_scale = v;
    if (err) *err = 0;
}
static float __stdcall GetFloat(const char *sec, const char *key, int *err)
{
    GlobInit();
    Log("GetFloat %s %s -> %.4f", sec, key, g_scale);
    if (err) *err = 0;
    return g_scale;
}
static void *g_table[11] = { (void *)ErrName, (void *)SetBool, (void *)SetInt32, (void *)SetFloat, 0, (void *)GetBool, (void *)GetInt32,
                             (void *)GetFloat, 0, 0, (void *)RemoveKey };
static void __stdcall ProjectionRaw(int eye, float *l, float *r, float *t, float *b)
{
    Log("GetProjectionRaw eye %d", eye);
    *l = eye ? -1.1f : -1.3f; *r = eye ? 1.3f : 1.1f; *t = -1.25f; *b = 1.2f;
}
// IVRSystem_026's events: STUB_QUIT_FILE, once it exists, delivers one VREvent_Quit (700) for
// STUB_QUIT_PID (default: the calling process).
static int g_quitSent = 0;
static bool __stdcall PollNextEvent(void *ev, uint32_t size)
{
    char path[512];
    if (size != 64) Log("PollNextEvent with size %u", size);
    if (g_quitSent || !GetEnvironmentVariableA("STUB_QUIT_FILE", path, sizeof(path)) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return false;
    g_quitSent = 1;
    memset(ev, 0, size);
    uint32_t type = 700, pid = (uint32_t)Env("STUB_QUIT_PID", (int)GetCurrentProcessId());
    memcpy(ev, &type, 4);
    memcpy((char *)ev + 16, &pid, 4);
    Log("PollNextEvent -> VREvent_Quit pid %s", pid == GetCurrentProcessId() ? "self" : "other");
    return true;
}
static void __stdcall AcknowledgeQuit(void) { Log("AcknowledgeQuit_Exiting"); }
static void *g_system[48] = { 0, 0, (void *)ProjectionRaw };

// IVRApplications_008: one application (the app's), state in STUB_APP_FILE ("installed auto binary")
// so it lasts between runs; STUB_APP_FILE missing = not installed.
static char g_appBin[1024];
static int g_appInstalled = 0, g_appAuto = 0, g_appLoaded = 0;
static void AppLoad(void)
{
    char path[512];
    if (g_appLoaded) return;
    g_appLoaded = 1;
    if (!GetEnvironmentVariableA("STUB_APP_FILE", path, sizeof(path))) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fscanf(f, "%d %d ", &g_appInstalled, &g_appAuto) == 2 && fgets(g_appBin, sizeof(g_appBin), f)) {
        size_t n = strlen(g_appBin);
        while (n && (g_appBin[n - 1] == '\n' || g_appBin[n - 1] == '\r')) g_appBin[--n] = 0;
    }
    fclose(f);
}
static void AppSave(void)
{
    char path[512];
    if (!GetEnvironmentVariableA("STUB_APP_FILE", path, sizeof(path))) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%d %d %s\n", g_appInstalled, g_appAuto, g_appBin);
    fclose(f);
}
static int __stdcall AddManifest(const char *path, bool temporary)
{
    AppLoad();
    char key[128] = "", bin[1024] = "";
    FILE *f = fopen(path, "rb");
    if (f) {   // log the manifest's key, binary and arguments
        char text[8192];
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        text[n] = 0;
        fclose(f);
        const char *k = strstr(text, "\"app_key\" : \""), *b = strstr(text, "\"binary_path_windows\" : \"");
        if (k) sscanf(k + 13, "%127[^\"]", key);
        if (b) sscanf(b + 25, "%1023[^\"]", bin);
        Log("AddApplicationManifest %s temporary=%d key=%s binary=%s args=%s", path, (int)temporary, key, bin,
            strstr(text, "\"arguments\" : \"--from-steamvr\"") ? "--from-steamvr" : "?");
    } else {
        Log("AddApplicationManifest %s (unreadable)", path);
        return 101;
    }
    // binary: relative to the manifest's folder, or absolute (unescape the JSON's backslashes)
    char abs[1024] = "", plain[1024];
    size_t j = 0;
    for (size_t i = 0; bin[i] && j + 1 < sizeof(plain); ++i) {
        if (bin[i] == '\\' && bin[i + 1]) ++i;
        plain[j++] = bin[i];
    }
    plain[j] = 0;
    if (plain[1] == ':') snprintf(abs, sizeof(abs), "%s", plain);
    else {
        snprintf(abs, sizeof(abs), "%s", path);
        char *slash = strrchr(abs, '\\');
        if (slash) snprintf(slash + 1, sizeof(abs) - (slash + 1 - abs), "%s", plain);
    }
    g_appInstalled = 1;
    snprintf(g_appBin, sizeof(g_appBin), "%s", abs);
    AppSave();
    return 0;
}
static int __stdcall RemoveManifest(const char *path)
{
    AppLoad();
    Log("RemoveApplicationManifest %s", path);
    // removed if it is the one that names the registered binary's folder
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", g_appBin);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    if (g_appInstalled && strncmp(path, dir, strlen(dir)) == 0) {
        g_appInstalled = 0;
        g_appAuto = 0;
        g_appBin[0] = 0;
        AppSave();
        return 0;
    }
    return 100;   // VRApplicationError_AppKeyAlreadyExists... (any error: not that manifest)
}
static bool __stdcall IsInstalled(const char *key)
{
    AppLoad();
    Log("IsApplicationInstalled %s -> %d", key, g_appInstalled);
    return g_appInstalled != 0;
}
static const char *__stdcall AppErrName(int e) { return e == 0 ? "VRApplicationError_None" : "VRApplicationError_Stub"; }
static uint32_t __stdcall GetPropString(const char *key, int prop, char *buf, uint32_t len, int *err)
{
    AppLoad();
    Log("GetApplicationPropertyString %s %d -> %s", key, prop, g_appInstalled ? g_appBin : "(none)");
    if (!g_appInstalled || prop != 13) {
        if (err) *err = 100;
        if (len) buf[0] = 0;
        return 0;
    }
    snprintf(buf, len, "%s", g_appBin);
    if (err) *err = 0;
    return (uint32_t)strlen(g_appBin) + 1;
}
static int __stdcall SetAutoLaunch(const char *key, bool on)
{
    AppLoad();
    Log("SetApplicationAutoLaunch %s %d", key, (int)on);
    if (!g_appInstalled) return 100;
    g_appAuto = on;
    AppSave();
    return 0;
}
static bool __stdcall GetAutoLaunch(const char *key)
{
    AppLoad();
    Log("GetApplicationAutoLaunch %s -> %d", key, g_appAuto);
    return g_appInstalled && g_appAuto;
}
static void *g_apps[40] = { (void *)AddManifest, (void *)RemoveManifest, (void *)IsInstalled };

// IVROverlay_028: the app's dashboard overlay. STUB_OVERLAY_ERR: CreateDashboardOverlay's error.
// STUB_OVERLAY_HIDDEN=1: the page isn't showing. STUB_DUMP_DIR: every picture the app sends is saved
// there (thumb.bmp, page_NNN.bmp). STUB_OVERLAY_EVENTS: a script of the laser's input in page pixels
// from the top left ("move x y", "down x y", "up x y", "leave", "scroll dy", "wait n": n polls
// without input), turned into OpenVR's events (y up from the bottom of the mouse scale).
typedef uint64_t Handle;
static const Handle kMain = 0x1001, kThumb = 0x1002;
static float g_mouseW = 1, g_mouseH = 1;
static int g_pages = 0;
static const char *__stdcall OvErrName(int e) { return e == 12 ? "VROverlayError_PermissionDenied" : "VROverlayError_Stub"; }
static int __stdcall CreateDashboard(const char *key, const char *name, Handle *m, Handle *t)
{
    int e = Env("STUB_OVERLAY_ERR", 0);
    Log("CreateDashboardOverlay key=%s name=%s -> %d", key, name, e);
    if (e) return e;
    *m = kMain;
    *t = kThumb;
    return 0;
}
static int __stdcall SetWidth(Handle h, float w) { Log("SetOverlayWidthInMeters %s %.2f", h == kMain ? "main" : "?", w); return 0; }
static int __stdcall SetInputMethod(Handle h, int m) { Log("SetOverlayInputMethod %s %d", h == kMain ? "main" : "?", m); return 0; }
static int __stdcall SetMouseScale(Handle h, float *v)
{
    g_mouseW = v[0];
    g_mouseH = v[1];
    Log("SetOverlayMouseScale %s %.0f x %.0f", h == kMain ? "main" : "?", v[0], v[1]);
    return 0;
}
static int __stdcall SetFlag(Handle h, int flag, bool on) { Log("SetOverlayFlag %s %d %d", h == kMain ? "main" : "?", flag, (int)on); return 0; }
static bool __stdcall IsVisible(Handle h) { return h == kMain && !Env("STUB_OVERLAY_HIDDEN", 0); }
static void DumpBmp(ID3D11Texture2D *tex, const char *path)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *st = NULL;
    D3D11_TEXTURE2D_DESC d, s;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D_GetDevice(tex, &dev);
    ID3D11Device_GetImmediateContext(dev, &ctx);
    ID3D11Texture2D_GetDesc(tex, &d);
    s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &s, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE *f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = { 0 };
                BITMAPINFOHEADER ih = { 0 };
                fh.bfType = 0x4d42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                ih.biSize = sizeof(ih);
                ih.biWidth = (LONG)d.Width;
                ih.biHeight = -(LONG)d.Height;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (UINT y = 0; y < d.Height; ++y) {
                    uint32_t *row = (uint32_t *)((char *)m.pData + y * m.RowPitch);
                    for (UINT x = 0; x < d.Width; ++x) {   // the BMP is BGRA
                        uint32_t p = row[x];
                        if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) p = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
                        fwrite(&p, 4, 1, f);
                    }
                }
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}
static int __stdcall SetTexture(Handle h, void **tex)   // Texture_t: handle, type, color space
{
    ID3D11Texture2D *t = (ID3D11Texture2D *)tex[0];
    int type = ((int *)tex)[2], space = ((int *)tex)[3];
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    Log("SetOverlayTexture %s %ux%u format %d mips %u type %d colorspace %d", h == kMain ? "main" : h == kThumb ? "thumbnail" : "?",
        d.Width, d.Height, (int)d.Format, d.MipLevels, type, space);
    char dir[400], path[512];
    if (GetEnvironmentVariableA("STUB_DUMP_DIR", dir, sizeof(dir))) {
        if (h == kThumb) snprintf(path, sizeof(path), "%s\\thumb.bmp", dir);
        else snprintf(path, sizeof(path), "%s\\page_%03d.bmp", dir, g_pages++);
        DumpBmp(t, path);
    }
    return 0;
}
static FILE *g_script = NULL;
static int g_scriptDone = 0, g_wait = 0;
static bool __stdcall PollOverlayEvent(Handle h, void *ev, uint32_t size)
{
    char path[512], line[128];
    float x = 0, y = 0;
    if (size != 64) Log("PollNextOverlayEvent with size %u", size);
    if (Env("STUB_LOG_POLLS", 0)) Log("poll (wait %d)", g_wait);
    if (h != kMain || g_scriptDone) return false;
    if (!g_script) {
        if (!GetEnvironmentVariableA("STUB_OVERLAY_EVENTS", path, sizeof(path)) || !(g_script = fopen(path, "rb"))) {
            g_scriptDone = 1;
            return false;
        }
    }
    if (g_wait > 0) {
        --g_wait;
        return false;
    }
    for (;;) {
        if (!fgets(line, sizeof(line), g_script)) {
            g_scriptDone = 1;
            Log("events: end of script");
            return false;
        }
        uint32_t type = 0, button = 0;
        if (sscanf(line, "wait %d", &g_wait) == 1) return false;
        if (sscanf(line, "move %f %f", &x, &y) == 2) type = 300;
        else if (sscanf(line, "down %f %f", &x, &y) == 2) type = 301, button = 1;
        else if (sscanf(line, "up %f %f", &x, &y) == 2) type = 302, button = 1;
        else if (strncmp(line, "leave", 5) == 0) type = 304;
        else if (sscanf(line, "scroll %f", &y) == 1) type = 305;
        else continue;
        memset(ev, 0, size);
        memcpy(ev, &type, 4);
        if (type == 305) {   // VREvent_Scroll_t: xdelta, ydelta
            memcpy((char *)ev + 20, &y, 4);
        } else {             // VREvent_Mouse_t: x, y (up from the bottom), button
            float vy = g_mouseH - y;
            memcpy((char *)ev + 16, &x, 4);
            memcpy((char *)ev + 20, &vy, 4);
            memcpy((char *)ev + 24, &button, 4);
        }
        Log("events: %.*s", (int)strcspn(line, "\r\n"), line);
        return true;
    }
}
static void *g_overlay[82];
static void __stdcall DXGIOutputInfo(int32_t *adapter) { *adapter = Env("STUB_ADAPTER", 0); Log("GetDXGIOutputInfo -> %d", *adapter); }

__declspec(dllexport) uint32_t VR_InitInternal2(int *err, int type, const char *info)
{
    g_system[8] = (void *)DXGIOutputInfo;
    g_system[30] = (void *)PollNextEvent;
    g_system[47] = (void *)AcknowledgeQuit;
    g_overlay[8] = (void *)OvErrName;
    g_overlay[11] = (void *)SetFlag;
    g_overlay[22] = (void *)SetWidth;
    g_overlay[45] = (void *)IsVisible;
    g_overlay[48] = (void *)PollOverlayEvent;
    g_overlay[50] = (void *)SetInputMethod;
    g_overlay[52] = (void *)SetMouseScale;
    g_overlay[60] = (void *)SetTexture;
    g_overlay[67] = (void *)CreateDashboard;
    g_apps[13] = (void *)AppErrName;
    g_apps[14] = (void *)GetPropString;
    g_apps[17] = (void *)SetAutoLaunch;
    g_apps[18] = (void *)GetAutoLaunch;
    int e = Env("STUB_INIT_ERR", 0);
    Log("VR_InitInternal2 type=%d -> %d", type, e);
    if (err) *err = e;
    if (e) return 0;
    g_init = 1;
    return 1;
}
__declspec(dllexport) void VR_ShutdownInternal(void) { Log("VR_ShutdownInternal (init %d)", g_init); g_init = 0; }
__declspec(dllexport) void *VR_GetGenericInterface(const char *ver, int *err)
{
    Log("VR_GetGenericInterface %s", ver);
    if (err) *err = 0;
    if (!g_init) return NULL;
    if (strcmp(ver, "FnTable:IVRSettings_003") == 0) return (void *)g_table;
    if (strcmp(ver, "FnTable:IVRSystem_026") == 0) return (void *)g_system;
    if (strcmp(ver, "FnTable:IVRApplications_008") == 0) return (void *)g_apps;
    if (strcmp(ver, "FnTable:IVROverlay_028") == 0) return (void *)g_overlay;
    if (err) *err = 105;   // VRInitError_Init_InterfaceNotFound
    return NULL;
}
