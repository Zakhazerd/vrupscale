// Stand-in for openvr_api.dll (self-test of the app's VRChat-resolution and start-with-SteamVR
// features): records the calls vrupscale.exe makes. Env: STUB_LOG (file), STUB_INIT_ERR (VR_InitInternal2's error),
// STUB_VALUE (the value SteamVR "has"; absent = not set), STUB_VALUE_FILE (a value changed meanwhile).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static void Log(const char *fmt, ...)
{
    char path[512];
    if (!GetEnvironmentVariableA("STUB_LOG", path, sizeof(path))) return;
    FILE *f = fopen(path, "ab");
    if (!f) return;
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

__declspec(dllexport) uint32_t VR_InitInternal2(int *err, int type, const char *info)
{
    g_system[30] = (void *)PollNextEvent;
    g_system[47] = (void *)AcknowledgeQuit;
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
    if (err) *err = 105;   // VRInitError_Init_InterfaceNotFound
    return NULL;
}
