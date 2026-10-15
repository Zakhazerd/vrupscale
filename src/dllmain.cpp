#include "internal.h"

#include <wchar.h>
#include <string.h>

namespace vu {
void UpscalerInit();
extern volatile LONG g_reloadRequested;
}

namespace {

HMODULE g_self = nullptr;
bool g_isTarget = false;

std::wstring ModuleDir(HMODULE m)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return L".";
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring(L".") : s.substr(0, p);
}

std::wstring ExeName()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return L"";
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? s : s.substr(p + 1);
}

bool EnvIsSet(const wchar_t *name)
{
    wchar_t v[8];
    DWORD n = GetEnvironmentVariableW(name, v, 8);
    return n > 0 && n < 8 && v[0] == L'1';
}

std::wstring LogPath()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"VRUPSCALE_LOG", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::wstring dir = std::wstring(buf, n) + L"\\vrupscale";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\vrupscale.log";
    }
    return vu::g_dllDir + L"\\vrupscale.log";
}

// Settings: %LOCALAPPDATA%\vrupscale\vrupscale.ini, which the vrupscale app manages. A file next
// to the DLL (older versions) is used until that one exists. Test runs (VRUPSCALE_FORCE) use the
// file next to the DLL; VRUPSCALE_INI overrides both.
std::wstring SettingsPath(bool realCompositor)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"VRUPSCALE_INI", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);
    const std::wstring legacy = vu::g_dllDir + L"\\vrupscale.ini";
    if (!realCompositor) return legacy;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return legacy;
    const std::wstring local = std::wstring(buf, n) + L"\\vrupscale\\vrupscale.ini";
    vu::g_iniPreferred = local;
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) return legacy;
    return local;
}

void LogModules(bool force)
{
    static const wchar_t *kWatch[] = {
        L"d3d11.dll", L"d3d12.dll", L"vulkan-1.dll", L"opengl32.dll", L"nvapi64.dll", L"nvwgf2umx.dll",
        L"nvoglv64.dll", L"amdxx64.dll", L"atidxx64.dll", L"amdvlk64.dll", L"igd10iumd64.dll", L"d3dcompiler_47.dll",
    };
    static bool seen[sizeof(kWatch) / sizeof(kWatch[0])] = {};
    for (size_t i = 0; i < sizeof(kWatch) / sizeof(kWatch[0]); ++i) {
        bool present = GetModuleHandleW(kWatch[i]) != nullptr;
        if (present && (!seen[i] || force)) {
            char name[64];
            WideCharToMultiByte(CP_UTF8, 0, kWatch[i], -1, name, sizeof(name), nullptr, nullptr);
            LOGI("module loaded: %s", name);
        }
        seen[i] = seen[i] || present;
    }
}

bool g_staticLoad = false;

DWORD WINAPI Worker(LPVOID)
{
    const bool probeOnly = EnvIsSet(L"VRUPSCALE_TEST_PROBE");
    // 1. Wait for the D3D11 runtime.
    for (int i = 0; !GetModuleHandleW(L"d3d11.dll"); ++i) {
        if (i == 200) LOGI("waiting for d3d11.dll to load...");
        Sleep(50);
        if (i > 20 * 60 * 30) {
            LOGE("d3d11.dll never loaded in 30 minutes; giving up (compositor may not use D3D11)");
            LogModules(true);
            return 0;
        }
    }
    LogModules(false);

    // 2. Watch device creation so the method hooks come from the compositor's real device.
    if (!probeOnly) vu::HookDeviceCreation();

    // 3. Shaders are embedded as precompiled bytecode (nothing to do).
    InterlockedExchange(&vu::g_stats.shadersReady, 1);

    // 4. Fallback: if the compositor's device was created before we were watching (late
    //    injection, dynamic d3d11 load), take the method bodies from a probe device instead.
    int waitMs = (g_staticLoad && !probeOnly) ? 60000 : 0;
    for (int t = 0; t < waitMs && !vu::HooksInstalled(); t += 100) Sleep(100);
    if (!vu::HooksInstalled()) {
        if (!probeOnly) LOGI("hooks: compositor device creation not observed; using a probe device");
        if (!vu::InstallD3D11Hooks()) LOGE("D3D11 hooks could not be installed; the compositor runs unmodified");
    }

    // 5. Hook the method bodies of other context configurations up front.
    vu::ProbeContextVariants();

    // 6. Diagnostics: which graphics stacks show up over the first two minutes, then a heartbeat
    //    whenever the render thread stops passing through the hooks (no stats line, no hotkeys).
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    auto fallbackTick = [&]() {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const LONGLONG lt = vu::g_lastTickQpc;
        if (!lt || q.QuadPart - lt > qpf.QuadPart) vu::ControlTickFromWorker();
    };
    for (int i = 0; i < 240; ++i) {
        Sleep(500);
        LogModules(false);
        vu::ConfigWatchTick();   // (the settings file is read here, never on SteamVR's thread)
        fallbackTick();
        if (i == 59 && vu::HooksInstalled() && vu::g_stats.psHookCalls == 0)
            LOGI("diag: no PSSetShaderResources calls through the hooked runtime after 30 s "
                 "(normal if no VR app has started yet)");
    }
    vu::LogFlush();
    int budget = 60;
    LONG lastPs = -1, lastDraw = -1;
    for (int slice = 0;; ++slice) {
        // the app channel keeps answering even when the compositor is not passing through the
        // hooks (status, settings); the render-thread tick takes over as soon as it runs
        Sleep(500);
        if (vu::g_shuttingDown) break;
        vu::ConfigWatchTick();
        fallbackTick();
        if (slice % 20 != 19) continue;
        if (!vu::HooksInstalled() || budget <= 0) continue;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONGLONG last = vu::g_lastTickQpc;
        const double idle = last ? double(now.QuadPart - last) / double(qpf.QuadPart) : -1.0;
        const LONG ps = vu::g_stats.psHookCalls, draws = vu::g_stats.drawHookCalls;
        if ((idle < 0 || idle > 10.0) && (ps != lastPs || draws != lastDraw || budget == 60)) {
            --budget;
            LOGI("diag: no render-thread tick %s; hook calls so far: PS binds %ld, CS binds %ld, draws %ld, copies %ld, "
                 "shared opens %ld, extra method bodies %ld",
                 idle < 0 ? "yet" : "for over 10 s", (long)ps, (long)vu::g_stats.csHookCalls, (long)draws,
                 (long)vu::g_stats.copyHookCalls, (long)vu::g_stats.sharedOpened, (long)vu::g_stats.extraBodies);
            char hits[256];
            vu::FormatHookVariantHits(hits, sizeof(hits));
            if (hits[0]) LOGI("diag:   %s", hits);
            vu::LogFlush();
        }
        lastPs = ps;
        lastDraw = draws;
    }
    return 0;
}

void Init(HMODULE self, bool dynamicLoad, bool realCompositor)
{
    // Pin ourselves: hooks point into this module, so it must never be unloaded.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)(void *)&Init, &pinned);
    vu::g_dllDir = ModuleDir(self);
    vu::g_iniPath = SettingsPath(realCompositor);
    bool iniFound = vu::LoadConfig(vu::g_cfg);
    if (!vu::g_cfg.active) return;   // master switch off: behave as a plain dxgi forwarder

    vu::g_logLevel = vu::g_cfg.logLevel;
    vu::g_logPath = LogPath();
    vu::LogOpen(vu::g_logPath);
    char exe[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, ExeName().c_str(), -1, exe, sizeof(exe), nullptr, nullptr);
    LOGI("==== vrupscale %s in %s (pid %lu, %s load) ====", VU_VERSION_STR, exe, GetCurrentProcessId(),
         dynamicLoad ? "dynamic" : "static");
    LOGI("config: settings file %ls%s", vu::g_iniPath.c_str(), iniFound ? "" : " (missing: defaults)");
    vu::PrintConfig(vu::g_cfg);
    vu::UpscalerInit();
    vu::ControlInit(realCompositor);
    vu::ControlFileRead(vu::g_cfg);
    vu::ControlNoteState(vu::g_cfg);

    // Static load (we are dxgi.dll in the compositor's folder): d3d11.dll is already mapped, so
    // start watching device creation now, before the compositor's main() can create its device.
    g_staticLoad = !dynamicLoad;
    if (!EnvIsSet(L"VRUPSCALE_TEST_PROBE") && GetModuleHandleW(L"d3d11.dll")) vu::HookDeviceCreation();

    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else LOGE("could not start worker thread (%lu)", GetLastError());
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        std::wstring exe = ExeName();
        const bool real = _wcsicmp(exe.c_str(), L"vrcompositor.exe") == 0;
        g_isTarget = real || EnvIsSet(L"VRUPSCALE_FORCE");
        if (g_isTarget) Init(inst, reserved == nullptr, real);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_isTarget) {
            InterlockedExchange(&vu::g_shuttingDown, 1);
            vu::LogFlushAtExit();
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------------
// Diagnostic exports (used by the test harness; harmless otherwise)

extern "C" __declspec(dllexport) const char *VrUpscaleVersion()
{
    return VU_VERSION_STR;
}

// Copies min(bytes, sizeof(DebugStats)) bytes; returns sizeof(DebugStats). Size-checked so a test
// built against an older struct can never overrun its buffer.
extern "C" __declspec(dllexport) int VrUpscaleGetStats(void *out, int bytes)
{
    if (out && bytes > 0) {
        vu::DebugStats snap = vu::g_stats;
        memcpy(out, &snap, (size_t)bytes < sizeof(snap) ? (size_t)bytes : sizeof(snap));
    }
    return (int)sizeof(vu::DebugStats);
}

extern "C" __declspec(dllexport) void VrUpscaleRequestReload()
{
    InterlockedExchange(&vu::g_reloadRequested, 1);
}
