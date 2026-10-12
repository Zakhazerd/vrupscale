// The app's connection to SteamVR (see vrlink.h).
#include "vrlink.h"
#include "core.h"
#include "json.h"

#include <stdint.h>
#include <string.h>
#include <tlhelp32.h>
#include <vector>

namespace app {

namespace {

// ---- openvr_api.dll's C interface ----------------------------------------------------------------

typedef uint32_t(__cdecl *PFN_InitInternal2)(int *err, int appType, const char *startupInfo);
typedef void(__cdecl *PFN_ShutdownInternal)();
typedef void *(__cdecl *PFN_GetGenericInterface)(const char *version, int *err);

enum {
    kAppBackground = 3,                  // EVRApplicationType: never starts SteamVR
    kInitNoServerForBackgroundApp = 121,
    kEventQuit = 700,                    // EVREventType VREvent_Quit (data: VREvent_Process_t)
    kPropBinaryPath = 13,                // EVRApplicationProperty BinaryPath_String
};

// VREvent_t (openvr.h, 64-bit Windows): eventType, trackedDeviceIndex, eventAgeSeconds, then the
// data union at 16; 64 bytes in all. PollNextEvent checks the size.
constexpr uint32_t kEventSize = 64;

// Function-table slots (openvr_capi.h). IVRSystem_026: 2 GetProjectionRaw, 30 PollNextEvent,
// 47 AcknowledgeQuit_Exiting. IVRApplications_008 (_007 has the same first 19): 0 Add/1 Remove
// ApplicationManifest, 2 IsApplicationInstalled, 13 GetApplicationsErrorNameFromEnum,
// 14 GetApplicationPropertyString, 17 Set/18 GetApplicationAutoLaunch.
typedef bool(__stdcall *PFN_PollNextEvent)(void *event, uint32_t size);
typedef void(__stdcall *PFN_AcknowledgeQuit)();
typedef int(__stdcall *PFN_AddManifest)(const char *path, bool temporary);
typedef int(__stdcall *PFN_RemoveManifest)(const char *path);
typedef bool(__stdcall *PFN_IsInstalled)(const char *key);
typedef const char *(__stdcall *PFN_AppErrName)(int err);
typedef uint32_t(__stdcall *PFN_GetPropString)(const char *key, int prop, char *buf, uint32_t len, int *err);
typedef int(__stdcall *PFN_SetAutoLaunch)(const char *key, bool on);
typedef bool(__stdcall *PFN_GetAutoLaunch)(const char *key);

struct Api {
    PFN_InitInternal2 init = nullptr;
    PFN_ShutdownInternal shutdown = nullptr;
    PFN_GetGenericInterface get = nullptr;
};

std::wstring EnvW(const wchar_t *name)
{
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return (n > 0 && n < 1024) ? std::wstring(buf, n) : std::wstring();
}

std::string Utf8(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

SRWLOCK g_lock = SRWLOCK_INIT;   // one use of OpenVR at a time (openvr_api.dll has one connection per process)
bool g_linked = false;           // the lasting connection is open
void **g_linkSystem = nullptr;   // its IVRSystem_026

// Loaded once and kept (OpenVR may leave threads behind); tried again while it is missing. Under g_lock.
const Api &OpenVr()
{
    static Api api;
    if (!api.init) {
        std::wstring path = EnvW(L"VRUPSCALE_TEST_OPENVR");   // self-test: a stand-in
        if (path.empty()) path = ExeDir() + L"\\openvr_api.dll";
        if (HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            Api a;
            a.init = (PFN_InitInternal2)GetProcAddress(dll, "VR_InitInternal2");
            a.shutdown = (PFN_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal");
            a.get = (PFN_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface");
            if (a.init && a.shutdown && a.get) api = a;
        }
    }
    return api;
}

std::wstring InitErrorText(int err)
{
    return err == kInitNoServerForBackgroundApp ? std::wstring(L"SteamVR is starting or closing, try again in a moment")
                                                 : Fmt(L"SteamVR didn't answer (error %d)", err);
}

const wchar_t *kMissingDll = L"openvr_api.dll is missing next to vrupscale.exe";

std::wstring Normal(std::wstring p)
{
    for (auto &c : p)
        if (c == L'/') c = L'\\';
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, full, nullptr);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(full, n) : p;
}

bool IsAbsolute(const std::wstring &p) { return p.size() > 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\') || (p[0] == L'/' && p[1] == L'/')); }

std::wstring DirOf(const std::wstring &p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring SelfExe()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

const wchar_t *kManifestName = L"vrupscale.vrmanifest";

bool WriteText(const std::wstring &path, const std::string &text)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wr, nullptr) && wr == text.size();
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Next to the exe (the binary named relative to it, as SteamVR's own examples do); if that folder
// can't be written, in the settings folder with the exe's full path.
bool WriteManifest(const std::wstring &exe, std::wstring &path, std::wstring &error)
{
    const std::wstring dir = DirOf(exe), name = exe.substr(dir.size() + 1);
    path = dir + L"\\" + kManifestName;
    if (WriteText(path, ManifestText(name))) return true;
    path = DataDir() + L"\\" + kManifestName;
    if (WriteText(path, ManifestText(exe))) return true;
    error = L"its manifest couldn't be written";
    return false;
}

} // namespace

// ---- sessions and the lasting connection --------------------------------------------------------

VrSession::VrSession() { AcquireSRWLockExclusive(&g_lock); }

VrSession::~VrSession()
{
    if (own_) OpenVr().shutdown();
    ReleaseSRWLockExclusive(&g_lock);
}

bool VrSession::Open(std::wstring &error)
{
    if (open_) return true;
    if (g_linked) return open_ = true;
    const Api &a = OpenVr();
    if (!a.init) {
        error = kMissingDll;
        return false;
    }
    int err = 0;
    a.init(&err, kAppBackground, nullptr);
    if (err != 0) {   // (openvr_api.dll cleans up after a failed init itself)
        initError = err;
        error = InitErrorText(err);
        return false;
    }
    return own_ = open_ = true;
}

void *VrSession::Interface(const char *version)
{
    if (!open_) return nullptr;
    int err = 0;
    void *p = OpenVr().get(version, &err);
    return err == 0 ? p : nullptr;
}

bool VrLinkOpen(std::wstring &error)
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_linked) {
        const Api &a = OpenVr();
        int err = 0;
        if (!a.init) {
            error = kMissingDll;
        } else if (a.init(&err, kAppBackground, nullptr), err != 0) {
            error = InitErrorText(err);
        } else {
            g_linkSystem = (void **)a.get("FnTable:IVRSystem_026", &err);
            if (g_linkSystem && err == 0) {
                g_linked = true;
            } else {   // without events the connection would only keep SteamVR waiting when it quits
                g_linkSystem = nullptr;
                a.shutdown();
                error = L"this SteamVR is too old to tell other programs it is closing";
            }
        }
    }
    const bool ok = g_linked;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

bool VrLinkIsOpen()
{
    AcquireSRWLockShared(&g_lock);
    const bool ok = g_linked;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void VrLinkClose()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_linked) OpenVr().shutdown();
    g_linked = false;
    g_linkSystem = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}

bool VrLinkPollQuit()
{
    bool quit = false;
    AcquireSRWLockExclusive(&g_lock);
    if (g_linked && g_linkSystem) {
        PFN_PollNextEvent poll = (PFN_PollNextEvent)g_linkSystem[30];
        alignas(8) uint8_t ev[kEventSize];
        for (int i = 0; i < 1000 && poll(ev, kEventSize); ++i) {   // (SteamVR sends Quit only to the process it asks)
            uint32_t type;
            memcpy(&type, ev, 4);
            if (type == kEventQuit) quit = true;
        }
        if (quit) ((PFN_AcknowledgeQuit)g_linkSystem[47])();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return quit;
}

// ---- starting with SteamVR ------------------------------------------------------------------------

std::string ManifestText(const std::wstring &binaryPath)
{
    std::string bin;
    for (char c : Utf8(binaryPath)) {
        if (c == '\\' || c == '"') bin += '\\';
        bin += c;
    }
    return "{\n"
           "   \"source\" : \"builtin\",\n"
           "   \"applications\" : [\n"
           "      {\n"
           "         \"app_key\" : \"" + std::string(kVrAppKey) + "\",\n"
           "         \"launch_type\" : \"binary\",\n"
           "         \"binary_path_windows\" : \"" + bin + "\",\n"
           "         \"arguments\" : \"" + Utf8(kLaunchArg) + "\",\n"
           "         \"is_dashboard_overlay\" : true,\n"
           "         \"strings\" : {\n"
           "            \"en_us\" : {\n"
           "               \"name\" : \"vrupscale\",\n"
           "               \"description\" : \"Upscaling in SteamVR's compositor: starts the vrupscale app with SteamVR\"\n"
           "            }\n"
           "         }\n"
           "      }\n"
           "   ]\n"
           "}\n";
}

AutoLaunchResult SyncAutoLaunch(const AutoLaunchJob &job)
{
    AutoLaunchResult r;
    r.want = job.want;
    VrSession s;
    if (!s.Open(r.error)) {
        r.notReady = s.initError != 0;
        return r;
    }
    void **apps = (void **)s.Interface("FnTable:IVRApplications_008");
    if (!apps) apps = (void **)s.Interface("FnTable:IVRApplications_007");
    if (!apps) {
        r.error = L"this SteamVR doesn't let other programs add startup apps";
        return r;
    }
    PFN_AddManifest add = (PFN_AddManifest)apps[0];
    PFN_RemoveManifest remove = (PFN_RemoveManifest)apps[1];
    PFN_IsInstalled isInstalled = (PFN_IsInstalled)apps[2];
    PFN_AppErrName errName = (PFN_AppErrName)apps[13];
    PFN_GetPropString getProp = (PFN_GetPropString)apps[14];
    PFN_SetAutoLaunch setAuto = (PFN_SetAutoLaunch)apps[17];
    PFN_GetAutoLaunch getAuto = (PFN_GetAutoLaunch)apps[18];
    auto refused = [&](const wchar_t *what, int e) {
        const char *n = errName ? errName(e) : nullptr;
        r.error = Fmt(L"SteamVR refused %s (%s)", what, n ? Widen(n).c_str() : Fmt(L"error %d", e).c_str());
    };

    const std::wstring exe = Normal(SelfExe());
    const bool installed = isInstalled(kVrAppKey);
    std::wstring registered;   // the exe SteamVR would start
    if (installed) {
        char buf[2048] = {};
        int e = 0;
        getProp(kVrAppKey, kPropBinaryPath, buf, sizeof(buf), &e);
        if (e == 0 && buf[0]) registered = Widen(buf);
    }
    // (a path SteamVR reports relative, or not at all, can't be compared: taken as this exe)
    const bool same = registered.empty() || !IsAbsolute(registered) || _wcsicmp(Normal(registered).c_str(), exe.c_str()) == 0;
    const bool autoOn = installed && getAuto(kVrAppKey);
    auto removeOld = [&]() {   // wherever a manifest of the app may be
        std::vector<std::wstring> paths = { DirOf(exe) + L"\\" + kManifestName, DataDir() + L"\\" + kManifestName };
        if (!registered.empty() && IsAbsolute(registered)) paths.push_back(DirOf(Normal(registered)) + L"\\" + kManifestName);
        for (size_t i = 0; i < paths.size(); ++i)
            if (i < 2 || (_wcsicmp(paths[i].c_str(), paths[0].c_str()) != 0 && _wcsicmp(paths[i].c_str(), paths[1].c_str()) != 0))
                remove(Utf8(paths[i]).c_str());
    };

    if (job.want && job.applied && installed && same && !autoOn) {   // switched off in SteamVR since: follow it
        r.want = false;
        r.userOff = true;
        r.ok = true;
        return r;
    }
    if (job.want) {
        if (!installed || !same) {
            if (installed) removeOld();   // an older place of the app
            std::wstring manifest;
            if (!WriteManifest(exe, manifest, r.error)) return r;
            const int e = add(Utf8(manifest).c_str(), false);
            if (e != 0) {
                refused(L"vrupscale's manifest", e);
                return r;
            }
            r.registered = true;
        }
        if (!getAuto(kVrAppKey)) {
            const int e = setAuto(kVrAppKey, true);
            if (e != 0) {
                refused(L"to start vrupscale", e);
                return r;
            }
        }
    } else if (installed) {
        if (autoOn) setAuto(kVrAppKey, false);
        removeOld();
    }
    r.ok = true;
    return r;
}

bool StartedBySteamVR(bool hasLaunchArg)
{
    if (hasLaunchArg) return true;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD parent = 0;
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == self) parent = pe.th32ParentProcessID;
    bool vr = false;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = parent && Process32FirstW(snap, &pe); ok && !vr; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == parent)
            for (const wchar_t *n : { L"vrserver.exe", L"vrmonitor.exe", L"vrstartup.exe", L"vrdashboard.exe" })
                vr = vr || _wcsicmp(pe.szExeFile, n) == 0;
    CloseHandle(snap);
    return vr;
}

int VrLinkSelfTest(std::wstring &report)
{
    int fails = 0;
    auto check = [&](bool ok, const wchar_t *what) {
        report += Fmt(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
        if (!ok) ++fails;
    };
    for (const wchar_t *bin : { L"vrupscale.exe", L"C:\\Games\\vr \"tools\"\\vrupscale.exe" }) {
        JVal root;
        const bool parsed = Parse(ManifestText(bin), root) && root.kind == JVal::Obj;
        JVal *apps = parsed ? root.Find("applications") : nullptr;
        JVal *a = apps && apps->kind == JVal::Arr && apps->items.size() == 1 ? &apps->items[0] : nullptr;
        JVal *key = a ? a->Find("app_key") : nullptr, *path = a ? a->Find("binary_path_windows") : nullptr;
        JVal *args = a ? a->Find("arguments") : nullptr, *overlay = a ? a->Find("is_dashboard_overlay") : nullptr;
        check(key && Unescape(key->raw) == kVrAppKey && path && Widen(Unescape(path->raw).c_str()) == bin, L"the manifest names the app and its exe");
        check(args && Widen(Unescape(args->raw).c_str()) == kLaunchArg && overlay && overlay->raw == "true",
              L"... with its argument, as a dashboard overlay (SteamVR auto-launches only those)");
    }
    check(IsAbsolute(L"C:\\x\\y.exe") && IsAbsolute(L"\\\\srv\\x") && !IsAbsolute(L"vrupscale.exe"), L"absolute paths are told apart");
    check(DirOf(L"C:\\a\\b.exe") == L"C:\\a", L"the folder of a path");
    return fails;
}

} // namespace app
