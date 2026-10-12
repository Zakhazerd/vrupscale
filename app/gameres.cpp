// Sets a game's render resolution in SteamVR (see gameres.h).
#include "gameres.h"
#include "core.h"
#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <utility>
#include <vector>
#include <tlhelp32.h>

namespace app {

namespace {

const char *const kKey = "resolutionScale";

// SteamVR's own layout: three-space indent, "key" : value.
void Write(const JVal &v, int indent, std::string &out)
{
    const std::string pad(indent + 3, ' ');
    switch (v.kind) {
    case JVal::Obj:
        if (v.members.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t i = 0; i < v.members.size(); ++i) {
            out += pad + "\"" + v.members[i].first + "\" : ";
            Write(v.members[i].second, indent + 3, out);
            out += i + 1 < v.members.size() ? ",\n" : "\n";
        }
        out += std::string(indent, ' ') + "}";
        break;
    case JVal::Arr:
        if (v.items.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t i = 0; i < v.items.size(); ++i) {
            out += pad;
            Write(v.items[i], indent + 3, out);
            out += i + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += std::string(indent, ' ') + "]";
        break;
    case JVal::Str: out += "\"" + v.raw + "\""; break;
    default: out += v.raw; break;
    }
}

bool ReadAll(const std::wstring &path, std::string &out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart < (64 << 20);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = out.empty() || (ReadFile(f, &out[0], (DWORD)out.size(), &rd, nullptr) && rd == out.size());
    }
    CloseHandle(f);
    return ok;
}

std::wstring EnvW(const wchar_t *name)
{
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return (n > 0 && n < 1024) ? std::wstring(buf, n) : std::wstring();
}

int ValueIn(JVal &root, const char *appKey)
{
    JVal *sec = root.kind == JVal::Obj ? root.Find(appKey) : nullptr;
    JVal *v = sec && sec->kind == JVal::Obj ? sec->Find(kKey) : nullptr;
    if (!v || v->kind != JVal::Num) return kResUnset;
    return (int)lround(atof(v->raw.c_str()));
}

// Changes appKey's value in the JSON text; false if the text isn't SteamVR's settings object.
bool EditText(std::string &text, const char *appKey, int value, int *before)
{
    JVal root;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) text = "{}";
    if (!Parse(text, root) || root.kind != JVal::Obj) return false;
    if (before) *before = ValueIn(root, appKey);
    JVal *sec = root.Find(appKey);
    if (sec && sec->kind != JVal::Obj) return false;
    if (value == kResUnset) {
        if (sec)
            for (size_t i = 0; i < sec->members.size(); ++i)
                if (sec->members[i].first == kKey) {
                    sec->members.erase(sec->members.begin() + i);
                    break;
                }
    } else {
        if (!sec) {
            JVal obj;
            obj.kind = JVal::Obj;
            root.members.push_back({ appKey, obj });
            sec = &root.members.back().second;
        }
        JVal *v = sec->Find(kKey);
        if (!v) {
            sec->members.push_back({ kKey, JVal() });
            v = &sec->members.back().second;
        }
        v->kind = JVal::Num;
        v->raw = std::to_string(value);
    }
    text.clear();
    Write(root, 0, text);
    text += "\n";
    return true;
}

// Sets keys of one section (raw JSON values: a number, true or false) in the settings text.
bool EditSection(std::string &text, const char *section, const std::vector<std::pair<std::string, std::string>> &kv)
{
    JVal root;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) text = "{}";
    if (!Parse(text, root) || root.kind != JVal::Obj) return false;
    JVal *sec = root.Find(section);
    if (sec && sec->kind != JVal::Obj) return false;
    if (!sec) {
        JVal obj;
        obj.kind = JVal::Obj;
        root.members.push_back({ section, obj });
        sec = &root.members.back().second;
    }
    for (auto &e : kv) {
        JVal *v = sec->Find(e.first.c_str());
        if (!v) {
            sec->members.push_back({ e.first, JVal() });
            v = &sec->members.back().second;
        }
        v->raw = e.second;
        v->kind = (e.second == "true" || e.second == "false") ? JVal::Lit : JVal::Num;
    }
    text.clear();
    Write(root, 0, text);
    text += "\n";
    return true;
}

bool WriteSettingsText(const std::wstring &file, const std::string &text, bool existed, std::wstring &error)
{
    const std::wstring backup = file + L".vrupscale-backup";
    if (existed && GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileW(file.c_str(), backup.c_str(), TRUE);
    const std::wstring tmp = file + L".vrupscale-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), (DWORD)text.size(), &wr, nullptr) && wr == text.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        error = L"SteamVR's settings file couldn't be written";
    }
    return ok;
}

bool SetViaFile(const std::wstring &file, const char *appKey, const GameResJob &job, GameResResult &r)
{
    if (file.empty()) {
        r.error = L"SteamVR's settings file wasn't found";
        return false;
    }
    std::string text;
    const bool exists = GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists && !ReadAll(file, text)) {
        r.error = L"SteamVR's settings file couldn't be read";
        return false;
    }
    int before = kResUnset;
    if (!EditText(text, appKey, job.value, &before)) {
        r.error = L"SteamVR's settings file isn't in the expected format";
        return false;
    }
    r.before = before;
    const std::wstring backup = file + L".vrupscale-backup";
    if (exists && GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileW(file.c_str(), backup.c_str(), TRUE);
    const std::wstring tmp = file + L".vrupscale-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), (DWORD)text.size(), &wr, nullptr) && wr == text.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        r.error = L"SteamVR's settings file couldn't be written";
        return false;
    }
    return true;
}

// ---- OpenVR (openvr_api.dll's C interface: VR_InitInternal2 + the IVRSettings function table) ----

typedef uint32_t(__cdecl *PFN_InitInternal2)(int *err, int appType, const char *startupInfo);
typedef void(__cdecl *PFN_ShutdownInternal)();
typedef void *(__cdecl *PFN_GetGenericInterface)(const char *version, int *err);
typedef const char *(__cdecl *PFN_InitErrorText)(int err);

struct SettingsFnTable {   // VR_IVRSettings_FnTable, IVRSettings_003 (openvr_capi.h)
    const char *(__stdcall *GetSettingsErrorNameFromEnum)(int err);
    void(__stdcall *SetBool)(const char *section, const char *key, bool v, int *err);
    void(__stdcall *SetInt32)(const char *section, const char *key, int32_t v, int *err);
    void(__stdcall *SetFloat)(const char *section, const char *key, float v, int *err);
    void(__stdcall *SetString)(const char *section, const char *key, const char *v, int *err);
    bool(__stdcall *GetBool)(const char *section, const char *key, int *err);
    int32_t(__stdcall *GetInt32)(const char *section, const char *key, int *err);
    float(__stdcall *GetFloat)(const char *section, const char *key, int *err);
    void(__stdcall *GetString)(const char *section, const char *key, char *buf, uint32_t len, int *err);
    void(__stdcall *RemoveSection)(const char *section, int *err);
    void(__stdcall *RemoveKeyInSection)(const char *section, const char *key, int *err);
};

enum { kAppBackground = 3, kInitNoServerForBackgroundApp = 121, kSettingsIpcFailed = 1 };

// OpenVR is initialised and shut down around each use; two threads (the resolution setter and the
// projection lookup) must not overlap, or one's shutdown pulls the interfaces from under the other.
SRWLOCK g_ovrLock = SRWLOCK_INIT;
struct OvrGuard {
    OvrGuard() { AcquireSRWLockExclusive(&g_ovrLock); }
    ~OvrGuard() { ReleaseSRWLockExclusive(&g_ovrLock); }
};

HMODULE OpenVrDll()
{
    static HMODULE dll = nullptr;   // loaded once, kept (OpenVR may leave threads behind)
    if (!dll) {
        std::wstring path = EnvW(L"VRUPSCALE_TEST_OPENVR");   // self-test: a stand-in
        if (path.empty()) path = ExeDir() + L"\\openvr_api.dll";
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    return dll;
}

bool SetViaApi(const char *appKey, const GameResJob &job, GameResResult &r)
{
    OvrGuard guard;
    HMODULE dll = OpenVrDll();
    PFN_InitInternal2 init = dll ? (PFN_InitInternal2)GetProcAddress(dll, "VR_InitInternal2") : nullptr;
    PFN_ShutdownInternal shutdown = dll ? (PFN_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal") : nullptr;
    PFN_GetGenericInterface get = dll ? (PFN_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface") : nullptr;
    if (!init || !shutdown || !get) {
        r.error = L"openvr_api.dll is missing next to vrupscale.exe";
        return false;
    }
    int err = 0;
    init(&err, kAppBackground, nullptr);   // never starts SteamVR
    if (err != 0) {
        r.error = err == kInitNoServerForBackgroundApp ? L"SteamVR is starting or closing, try again in a moment"
                                                         : Fmt(L"SteamVR didn't answer (error %d)", err);
        return false;
    }
    SettingsFnTable *s = (SettingsFnTable *)get("FnTable:IVRSettings_003", &err);
    if (!s || err != 0) {
        shutdown();
        r.error = L"this SteamVR doesn't offer its settings to other programs";
        return false;
    }
    if (job.readBefore) {
        int e = 0;
        int32_t v = s->GetInt32(appKey, kKey, &e);
        r.before = e == 0 ? (int)v : (e == kSettingsIpcFailed ? kResUnknown : kResUnset);
    }
    int e = 0;
    if (job.value == kResUnset) s->RemoveKeyInSection(appKey, kKey, &e);
    else s->SetInt32(appKey, kKey, job.value, &e);
    bool ok = e == 0 || (job.value == kResUnset && e != kSettingsIpcFailed);   // removing what isn't there is fine
    if (!ok) {
        const char *name = s->GetSettingsErrorNameFromEnum ? s->GetSettingsErrorNameFromEnum(e) : nullptr;
        r.error = Fmt(L"SteamVR refused the change (%s)", name ? Widen(name).c_str() : Fmt(L"error %d", e).c_str());
    }
    shutdown();
    return ok;
}

} // namespace

bool GetEyeProjection(float out[8], std::wstring &error)
{
    OvrGuard guard;
    HMODULE dll = OpenVrDll();
    PFN_InitInternal2 init = dll ? (PFN_InitInternal2)GetProcAddress(dll, "VR_InitInternal2") : nullptr;
    PFN_ShutdownInternal shutdown = dll ? (PFN_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal") : nullptr;
    PFN_GetGenericInterface get = dll ? (PFN_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface") : nullptr;
    if (!init || !shutdown || !get) {
        error = L"openvr_api.dll is missing next to vrupscale.exe";
        return false;
    }
    int err = 0;
    init(&err, kAppBackground, nullptr);
    if (err != 0) {
        error = Fmt(L"SteamVR didn't answer (error %d)", err);
        return false;
    }
    // VR_IVRSystem_FnTable (openvr_capi.h): GetRecommendedRenderTargetSize, GetProjectionMatrix,
    // GetProjectionRaw, ... (the first entries have kept their place in every version)
    typedef void(__stdcall * PFN_ProjectionRaw)(int eye, float *l, float *r, float *t, float *b);
    void **sys = nullptr;
    for (const char *v : { "FnTable:IVRSystem_026", "FnTable:IVRSystem_023", "FnTable:IVRSystem_022", "FnTable:IVRSystem_021",
                           "FnTable:IVRSystem_020", "FnTable:IVRSystem_019" }) {
        err = 0;
        sys = (void **)get(v, &err);
        if (sys && err == 0) break;
    }
    bool ok = sys && err == 0;
    if (ok) {
        PFN_ProjectionRaw raw = (PFN_ProjectionRaw)sys[2];
        for (int eye = 0; eye < 2; ++eye) raw(eye, &out[eye * 4], &out[eye * 4 + 1], &out[eye * 4 + 2], &out[eye * 4 + 3]);
        for (int i = 0; i < 8; ++i) ok = ok && out[i] == out[i] && fabsf(out[i]) < 100.0f;
        ok = ok && out[1] > out[0] && out[5] > out[4] && out[2] != out[3];
        if (!ok) error = L"SteamVR reported no usable projection";
    } else {
        error = L"this SteamVR doesn't offer IVRSystem to other programs";
    }
    shutdown();
    return ok;
}

bool EyeTrackerPresent()
{
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, L"VRCFTMemmapData");
    if (h) CloseHandle(h);
    return h != nullptr;
}

std::wstring SteamVrSettingsFile()
{
    std::wstring t = EnvW(L"VRUPSCALE_TEST_VRSETTINGS");   // self-test
    if (!t.empty()) return t;
    // OpenVR's registry of paths names Steam's config folder
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::string text;
    JVal root;
    if (n > 0 && n < MAX_PATH && ReadAll(std::wstring(base, n) + L"\\openvr\\openvrpaths.vrpath", text) && Parse(text, root) &&
        root.kind == JVal::Obj) {
        JVal *cfg = root.Find("config");
        if (cfg && cfg->kind == JVal::Arr)
            for (auto &it : cfg->items)
                if (it.kind == JVal::Str && !it.raw.empty()) return Widen(Unescape(it.raw).c_str()) + L"\\steamvr.vrsettings";
    }
    // else Steam's folder from the registry
    wchar_t steam[MAX_PATH];
    DWORD sz = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &sz) == ERROR_SUCCESS) {
        std::wstring p = steam;
        for (auto &c : p)
            if (c == L'/') c = L'\\';
        return p + L"\\config\\steamvr.vrsettings";
    }
    return L"";
}

int ReadGameResolutionFile(const std::wstring &file, const char *appKey)
{
    std::string text;
    JVal root;
    if (file.empty() || !ReadAll(file, text) || !Parse(text, root)) return kResUnknown;
    return ValueIn(root, appKey);
}

// SteamVR's global render resolution ("steamvr" section): the scale applies live to the running
// game, which the per-application value does not (SteamVR stores that one, but applies it when the
// game starts, or while its settings window is open).
GlobalResResult SetGlobalResolution(const char *appKey, const GlobalResJob &job)
{
    GlobalResResult r;
    r.job = job;
    r.base = job.base;
    const bool running = IsSteamVRRunning() || EnvW(L"VRUPSCALE_TEST_STEAMVR_RUNNING") == L"1";
    r.viaApi = running;
    if (!running) {
        if (!job.restore) {
            r.error = L"SteamVR isn't running";
            return r;
        }
        // put it back in the settings file, for the next time SteamVR starts
        const std::wstring file = SteamVrSettingsFile();
        std::string text;
        const bool exists = !file.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (file.empty() || (exists && !ReadAll(file, text))) {
            r.error = L"SteamVR's settings file wasn't found";
            return r;
        }
        char scale[32];
        snprintf(scale, sizeof(scale), "%.4f", job.base.scale);
        if (!EditSection(text, "steamvr", { { "supersampleScale", scale }, { "supersampleManualOverride", job.base.manual ? "true" : "false" } })) {
            r.error = L"SteamVR's settings file isn't in the expected format";
            return r;
        }
        r.ok = WriteSettingsText(file, text, exists, r.error);
        return r;
    }
    OvrGuard guard;
    HMODULE dll = OpenVrDll();
    PFN_InitInternal2 init = dll ? (PFN_InitInternal2)GetProcAddress(dll, "VR_InitInternal2") : nullptr;
    PFN_ShutdownInternal shutdown = dll ? (PFN_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal") : nullptr;
    PFN_GetGenericInterface get = dll ? (PFN_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface") : nullptr;
    if (!init || !shutdown || !get) {
        r.error = L"openvr_api.dll is missing next to vrupscale.exe";
        return r;
    }
    int err = 0;
    init(&err, kAppBackground, nullptr);
    if (err != 0) {
        r.error = err == kInitNoServerForBackgroundApp ? L"SteamVR is starting or closing, try again in a moment"
                                                         : Fmt(L"SteamVR didn't answer (error %d)", err);
        return r;
    }
    SettingsFnTable *s = (SettingsFnTable *)get("FnTable:IVRSettings_003", &err);
    if (!s || err != 0) {
        shutdown();
        r.error = L"this SteamVR doesn't offer its settings to other programs";
        return r;
    }
    int e = 0;
    if (job.restore) {
        s->SetFloat("steamvr", "supersampleScale", job.base.scale, &e);
        int e2 = 0;
        s->SetBool("steamvr", "supersampleManualOverride", job.base.manual, &e2);
        r.ok = e == 0 && e2 == 0;
    } else {
        if (!job.haveBase) {   // what the user had: put back when vrupscale stops managing it
            int e1 = 0, e2 = 0;
            const float sc = s->GetFloat("steamvr", "supersampleScale", &e1);
            const bool manual = s->GetBool("steamvr", "supersampleManualOverride", &e2);
            r.base.scale = (e1 == 0 && sc > 0.05f && sc < 10.0f) ? sc : 1.0f;
            r.base.manual = e2 == 0 && manual;
        }
        int ea = 0;
        const int32_t app = s->GetInt32(appKey, kKey, &ea);
        r.perApp = (ea == 0 && app > 0) ? (int)app : 100;
        // The game's resolution = global x its own per-application value, and what it would get
        // with the user's global value (Auto counts as 100%) is the target: lowering the global
        // value by `target` makes the game render that share of it, whatever its own value is
        // (read for the record only; changing it changes the target).
        const float base = r.base.manual ? r.base.scale : 1.0f;
        r.applied = std::max(0.2f, std::min(5.0f, base * job.target));
        s->SetBool("steamvr", "supersampleManualOverride", true, &e);
        int e2 = 0;
        s->SetFloat("steamvr", "supersampleScale", r.applied, &e2);
        r.ok = e == 0 && e2 == 0;
    }
    if (!r.ok) {
        const char *name = s->GetSettingsErrorNameFromEnum ? s->GetSettingsErrorNameFromEnum(e) : nullptr;
        r.error = Fmt(L"SteamVR refused the change (%s)", name ? Widen(name).c_str() : Fmt(L"error %d", e).c_str());
    }
    shutdown();
    return r;
}

bool VRChatRunning()
{
    const std::wstring t = EnvW(L"VRUPSCALE_TEST_VRCHAT_FLAG");   // self-test: a file that stands for the process
    if (!t.empty()) return GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool running = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !running; ok = Process32NextW(snap, &pe))
        running = _wcsicmp(pe.szExeFile, L"VRChat.exe") == 0;
    CloseHandle(snap);
    return running;
}

GameResResult SetGameResolution(const char *appKey, const GameResJob &job)
{
    GameResResult r;
    r.job = job;
    const bool running = IsSteamVRRunning() || EnvW(L"VRUPSCALE_TEST_STEAMVR_RUNNING") == L"1";
    r.viaApi = running;
    r.ok = running ? SetViaApi(appKey, job, r) : SetViaFile(SteamVrSettingsFile(), appKey, job, r);
    return r;
}

int GameResSelfTest(std::wstring &report)
{
    int fails = 0;
    auto check = [&](bool ok, const wchar_t *what) {
        report += Fmt(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
        if (!ok) ++fails;
    };
    const char *sample =
        "\xEF\xBB\xBF{\n   \"steamvr\" : {\n      \"supersampleScale\" : 1.5,\n      \"supersampleManualOverride\" : true,\n"
        "      \"lastVersion\" : \"2.7.4\"\n   },\n   \"openvr.tool.steamvr_environments\" : {\n      \"motionSmoothingOverride\" : 0,\n"
        "      \"resolutionScale\" : 100\n   },\n   \"steam.app.438100\" : {\n      \"motionSmoothingOverride\" : 1,\n"
        "      \"resolutionScale\" : 59\n   },\n   \"list\" : [ 1, \"a\\\"b\", { \"x\" : null } ],\n   \"empty\" : {}\n}\n";
    std::string t = sample;
    int before = 0;
    check(EditText(t, kVrchatAppKey, 44, &before) && before == 59, L"reads VRChat's 59% and sets 44%");
    JVal root;
    check(Parse(t, root) && ValueIn(root, kVrchatAppKey) == 44, L"the result parses, with 44%");
    check(ValueIn(root, "openvr.tool.steamvr_environments") == 100, L"other apps' values are kept");
    JVal *ss = root.Find("steamvr");
    JVal *sv = ss ? ss->Find("supersampleScale") : nullptr;
    JVal *lv = ss ? ss->Find("lastVersion") : nullptr;
    check(sv && sv->raw == "1.5" && lv && lv->raw == "2.7.4", L"the global settings are kept as they were");
    JVal *ls = root.Find("list");
    check(ls && ls->kind == JVal::Arr && ls->items.size() == 3 && ls->items[1].raw == "a\\\"b", L"arrays and escapes survive");
    JVal *app = root.Find(kVrchatAppKey);
    JVal *ms = app ? app->Find("motionSmoothingOverride") : nullptr;
    check(ms && ms->raw == "1", L"VRChat's other per-application settings are kept");
    check(EditText(t, kVrchatAppKey, kResUnset, &before) && before == 44, L"removing it reads 44%");
    JVal r2;
    check(Parse(t, r2) && ValueIn(r2, kVrchatAppKey) == kResUnset && r2.Find(kVrchatAppKey), L"removed: VRChat back to 100%");
    std::string fresh = "{\n   \"steamvr\" : {}\n}\n";
    check(EditText(fresh, kVrchatAppKey, 25, &before) && before == kResUnset, L"a file without VRChat's section gets one");
    JVal r3;
    check(Parse(fresh, r3) && ValueIn(r3, kVrchatAppKey) == 25, L"... with 25%");
    std::string bad = "{ \"steamvr\" : { ";
    check(!EditText(bad, kVrchatAppKey, 59, nullptr), L"a broken file is left alone");
    return fails;
}

} // namespace app
