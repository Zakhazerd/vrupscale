// Stand-in for openvr_api.dll (self-test of the app's VRChat-resolution feature): records the
// calls vrupscale.exe makes. Env: STUB_LOG (file), STUB_INIT_ERR (VR_InitInternal2's error),
// STUB_VALUE (the value SteamVR "has"; absent = not set).
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
static void *g_system[3] = { 0, 0, (void *)ProjectionRaw };

__declspec(dllexport) uint32_t VR_InitInternal2(int *err, int type, const char *info)
{
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
    if (err) *err = 105;   // VRInitError_Init_InterfaceNotFound
    return NULL;
}
