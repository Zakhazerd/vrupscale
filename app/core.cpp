#include "core.h"

#include <shlobj.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

namespace app {

std::wstring g_settingsPath;
std::wstring g_channelName = VU_CONTROL_NAME;

std::wstring Widen(const char *s)
{
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

std::wstring Fmt(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}

static bool FileExists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

std::wstring DataDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    std::wstring dir = (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) + L"\\vrupscale" : ExeDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring DefaultSettingsPath() { return DataDir() + L"\\vrupscale.ini"; }
std::wstring DefaultLogPath() { return DataDir() + L"\\vrupscale.log"; }

std::wstring PayloadPath()
{
    std::wstring p = ExeDir() + L"\\payload\\dxgi.dll";
    if (FileExists(p)) return p;
    return ExeDir() + L"\\dxgi.dll";
}

// ---- settings ---------------------------------------------------------------------------------

VuSettings DefaultSettings()
{
    VuSettings s = {};
    s.enabled = 1;
    s.mode = kVuModeTemporal;
    s.upscaleFactor = kVuScaleFactor[kVuScaleDefault];   // Ultra Quality
    s.sharpness = 1.0f;
    s.tint = 0;
    s.hotkeys = 1;
    s.maxWeight = 1.0f;
    s.localTrust = 1.0f;
    s.residualTol = 1.5f;
    s.fastHistory = 1;   // Sharp history off
    s.bilinearEdges = 0;
    s.clipGamma = 1.25f;
    s.maxStreams = 4;
    s.temporalArea = 0.6f;
    s.requireShared = 1;
    s.minWidth = 1000;
    s.minHeight = 1000;
    s.minAspect = 0.6f;
    s.maxAspect = 1.6f;
    return s;
}

static std::wstring IniStr(const std::wstring &path, const wchar_t *sec, const wchar_t *key)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec, key, L"", buf, 1024, path.c_str());
    std::wstring s(buf);
    size_t sc = s.find_first_of(L";#");
    if (sc != std::wstring::npos) s.resize(sc);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && iswspace(s[b])) ++b;
    return s.substr(b);
}

static void IniFloat(const std::wstring &p, const wchar_t *sec, const wchar_t *key, float &v)
{
    std::wstring s = IniStr(p, sec, key);
    if (!s.empty()) v = (float)wcstod(s.c_str(), nullptr);
}

static void IniInt(const std::wstring &p, const wchar_t *sec, const wchar_t *key, int32_t &v)
{
    std::wstring s = IniStr(p, sec, key);
    if (!s.empty()) v = (int32_t)wcstol(s.c_str(), nullptr, 10);
}

static void IniBool(const std::wstring &p, const wchar_t *sec, const wchar_t *key, int32_t &v)
{
    std::wstring s = IniStr(p, sec, key);
    if (s.empty()) return;
    wchar_t c = towlower(s[0]);
    v = (c == L'1' || c == L't' || c == L'y' || (c == L'o' && s.size() > 1 && towlower(s[1]) == L'n')) ? 1 : 0;
}

static void Clamp(VuSettings &s)
{
    auto cl = [](float v, float lo, float hi) { return !(v == v) ? lo : (v < lo ? lo : (v > hi ? hi : v)); };
    s.upscaleFactor = cl(s.upscaleFactor, 1.0f, 3.0f);
    s.fastHistory = s.fastHistory ? 1 : 0;
    s.bilinearEdges = s.bilinearEdges ? 1 : 0;
    s.gazeFollow = s.gazeFollow ? 1 : 0;
    s.gazeShow = s.gazeShow ? 1 : 0;
    for (float &o : s.gazeOffset) o = cl(o, -1.0f, 1.0f);
    for (float &p : s.eyeProj)
        if (!(p == p) || p > 100.0f || p < -100.0f) p = 0.0f;
    s.sharpness = cl(s.sharpness, 0.0f, 1.0f);
    s.maxWeight = cl(s.maxWeight, 0.1f, 64.0f);
    s.localTrust = cl(s.localTrust, 0.0f, 1.0f);
    s.residualTol = cl(s.residualTol, 0.05f, 10.0f);
    s.temporalArea = roundf(cl(s.temporalArea, 0.3f, 1.0f) * 100.0f) / 100.0f;
    s.upscaleFactor = roundf(s.upscaleFactor * 100.0f) / 100.0f;
    if (s.mode < 0 || s.mode > 2) s.mode = kVuModeTemporal;
    if (s.excludeCount < 0) s.excludeCount = 0;
    if (s.excludeCount > (int32_t)kVuMaxExclude) s.excludeCount = kVuMaxExclude;
}

bool LoadSettings(const std::wstring &p, VuSettings &s, bool *adjusted)
{
    if (adjusted) *adjusted = false;
    s = DefaultSettings();
    if (!FileExists(p)) return false;
    IniBool(p, L"upscale", L"enabled", s.enabled);
    std::wstring mode = IniStr(p, L"upscale", L"mode");
    if (!mode.empty()) {
        wchar_t m = towlower(mode[0]);
        s.mode = m == L'b' ? kVuModeBilinear : (m == L'f' ? kVuModeFsr : kVuModeTemporal);
    }
    IniFloat(p, L"upscale", L"upscaleFactor", s.upscaleFactor);
    IniFloat(p, L"upscale", L"sharpness", s.sharpness);
    IniBool(p, L"upscale", L"debugTint", s.tint);
    IniBool(p, L"general", L"hotkeys", s.hotkeys);
    IniFloat(p, L"temporal", L"maxWeight", s.maxWeight);
    IniFloat(p, L"temporal", L"localTrust", s.localTrust);
    IniFloat(p, L"temporal", L"residualTol", s.residualTol);
    IniFloat(p, L"temporal", L"clipGamma", s.clipGamma);
    int32_t sharpHistory = 0;
    IniBool(p, L"temporal", L"sharpHistory", sharpHistory);
    s.fastHistory = sharpHistory ? 0 : 1;
    IniBool(p, L"temporal", L"bilinearEdges", s.bilinearEdges);
    IniBool(p, L"eyes", L"followGaze", s.gazeFollow);
    IniBool(p, L"eyes", L"showGaze", s.gazeShow);
    IniFloat(p, L"eyes", L"offsetX", s.gazeOffset[0]);
    IniFloat(p, L"eyes", L"offsetY", s.gazeOffset[1]);
    {
        std::wstring pr = IniStr(p, L"eyes", L"projection");
        float v[8] = {};
        if (swscanf(pr.c_str(), L"%f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8)
            memcpy(s.eyeProj, v, sizeof(v));
    }
    IniInt(p, L"temporal", L"maxStreams", s.maxStreams);
    IniFloat(p, L"temporal", L"temporalArea", s.temporalArea);
    IniBool(p, L"match", L"requireShared", s.requireShared);
    IniInt(p, L"match", L"minWidth", s.minWidth);
    IniInt(p, L"match", L"minHeight", s.minHeight);
    IniFloat(p, L"match", L"minAspect", s.minAspect);
    IniFloat(p, L"match", L"maxAspect", s.maxAspect);
    std::wstring ex = IniStr(p, L"match", L"excludeSizes");
    s.excludeCount = 0;
    size_t i = 0;
    while (i < ex.size() && s.excludeCount < (int32_t)kVuMaxExclude) {
        size_t j = ex.find(L',', i);
        if (j == std::wstring::npos) j = ex.size();
        int w = 0, h = 0;
        std::wstring item = ex.substr(i, j - i);
        if ((swscanf(item.c_str(), L" %dx%d", &w, &h) == 2 || swscanf(item.c_str(), L" %dX%d", &w, &h) == 2) && w > 0 &&
            h > 0)
            s.exclude[s.excludeCount++] = ((uint32_t)w << 16) | (uint32_t)h;
        i = j + 1;
    }
    Clamp(s);
    // Ultra Performance (3x) was dropped in 0.4.7: the nearest preset left is Performance.
    if (fabsf(s.upscaleFactor - 3.0f) < 0.005f) {
        s.upscaleFactor = kVuScaleFactor[kVuScaleCount - 1];
        if (adjusted) *adjusted = true;
    }
    // The factor slider (before 0.4.5) left values like 1.29 (its old default, for SteamVR at
    // 60%): put those on the preset they were meant as. Factors further from any preset stay.
    if (VuScalePreset(s.upscaleFactor) < 0)
        for (int k = 0; k < kVuScaleCount; ++k)
            if (fabsf(s.upscaleFactor - kVuScaleFactor[k]) < 0.021f) {
                s.upscaleFactor = kVuScaleFactor[k];
                if (adjusted) *adjusted = true;
            }
    return true;
}

bool SaveSettings(const std::wstring &p, const VuSettings &s)
{
    if (!FileExists(p)) {
        // a header for anyone opening the file; the keys follow
        HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char hdr[] = "; vrupscale settings. Managed by vrupscale.exe, which also changes them live while\r\n"
                               "; SteamVR runs. Hand edits are fine too: SteamVR picks them up within a second.\r\n\r\n";
            DWORD wr = 0;
            WriteFile(f, hdr, sizeof(hdr) - 1, &wr, nullptr);
            CloseHandle(f);
        }
    }
    bool ok = true;
    auto put = [&](const wchar_t *sec, const wchar_t *key, const std::wstring &v) {
        ok = WritePrivateProfileStringW(sec, key, v.c_str(), p.c_str()) && ok;
    };
    put(L"upscale", L"enabled", s.enabled ? L"1" : L"0");
    put(L"upscale", L"mode", s.mode == kVuModeFsr ? L"fsr" : (s.mode == kVuModeBilinear ? L"bilinear" : L"temporal"));
    put(L"upscale", L"upscaleFactor", Fmt(L"%.2f", s.upscaleFactor));
    WritePrivateProfileStringW(L"upscale", L"targetHeight", nullptr, p.c_str());   // removed in 0.4.4
    put(L"upscale", L"sharpness", Fmt(L"%.2f", s.sharpness));
    put(L"upscale", L"debugTint", s.tint ? L"1" : L"0");
    put(L"general", L"hotkeys", s.hotkeys ? L"1" : L"0");
    put(L"temporal", L"maxWeight", Fmt(L"%.2f", s.maxWeight));
    put(L"temporal", L"localTrust", Fmt(L"%.2f", s.localTrust));
    put(L"temporal", L"residualTol", Fmt(L"%.2f", s.residualTol));
    put(L"temporal", L"temporalArea", Fmt(L"%.2f", s.temporalArea));
    put(L"temporal", L"sharpHistory", s.fastHistory ? L"0" : L"1");
    put(L"temporal", L"bilinearEdges", s.bilinearEdges ? L"1" : L"0");
    put(L"eyes", L"followGaze", s.gazeFollow ? L"1" : L"0");
    put(L"eyes", L"showGaze", s.gazeShow ? L"1" : L"0");
    put(L"eyes", L"offsetX", Fmt(L"%.4f", s.gazeOffset[0]));
    put(L"eyes", L"offsetY", Fmt(L"%.4f", s.gazeOffset[1]));
    {
        bool known = false;
        for (float v : s.eyeProj) known = known || v != 0.0f;
        if (known) {
            const float *v = s.eyeProj;
            put(L"eyes", L"projection", Fmt(L"%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]));
        }
    }
    std::wstring ex;
    for (int i = 0; i < s.excludeCount; ++i)
        ex += Fmt(L"%s%ux%u", i ? L"," : L"", s.exclude[i] >> 16, s.exclude[i] & 0xffff);
    put(L"match", L"excludeSizes", ex);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, p.c_str());   // flush the profile cache
    return ok;
}

bool SameSettings(const VuSettings &a, const VuSettings &b)
{
    auto feq = [](float x, float y) { return fabsf(x - y) < 0.004f; };
    if (a.enabled != b.enabled || a.mode != b.mode || !feq(a.upscaleFactor, b.upscaleFactor) ||
        a.fastHistory != b.fastHistory || a.bilinearEdges != b.bilinearEdges || a.gazeFollow != b.gazeFollow ||
        a.gazeShow != b.gazeShow || fabsf(a.gazeOffset[0] - b.gazeOffset[0]) > 0.0002f ||
        fabsf(a.gazeOffset[1] - b.gazeOffset[1]) > 0.0002f ||
        !feq(a.sharpness, b.sharpness) ||
        a.tint != b.tint || a.hotkeys != b.hotkeys ||
        !feq(a.maxWeight, b.maxWeight) || !feq(a.localTrust, b.localTrust) || !feq(a.residualTol, b.residualTol) ||
        !feq(a.temporalArea, b.temporalArea) || a.excludeCount != b.excludeCount)
        return false;
    for (int i = 0; i < a.excludeCount; ++i)
        if (a.exclude[i] != b.exclude[i]) return false;
    for (int i = 0; i < 8; ++i)   // (saved to 5 decimals)
        if (fabsf(a.eyeProj[i] - b.eyeProj[i]) > 0.0001f) return false;
    return true;
}

const wchar_t *ModeLabel(int mode)
{
    return mode == kVuModeFsr ? L"fsr" : (mode == kVuModeBilinear ? L"bilinear" : L"temporal");
}

bool SetSettingByName(VuSettings &s, const std::wstring &key, const std::wstring &v)
{
    const float f = (float)wcstod(v.c_str(), nullptr);
    const bool b = !v.empty() && (v[0] == L'1' || towlower(v[0]) == L't' || towlower(v[0]) == L'y' ||
                                  (towlower(v[0]) == L'o' && v.size() > 1 && towlower(v[1]) == L'n'));
    if (key == L"enabled") s.enabled = b;
    else if (key == L"mode") {
        wchar_t m = v.empty() ? L't' : towlower(v[0]);
        s.mode = m == L'b' ? kVuModeBilinear : (m == L'f' ? kVuModeFsr : kVuModeTemporal);
    } else if (key == L"upscaleFactor") s.upscaleFactor = f;
    else if (key == L"sharpHistory") s.fastHistory = b ? 0 : 1;
    else if (key == L"bilinearEdges") s.bilinearEdges = b;
    else if (key == L"followGaze") s.gazeFollow = b;
    else if (key == L"showGaze") s.gazeShow = b;
    else if (key == L"offsetX") s.gazeOffset[0] = f;
    else if (key == L"offsetY") s.gazeOffset[1] = f;
    else if (key == L"sharpness") s.sharpness = f;
    else if (key == L"debugTint" || key == L"tint") s.tint = b;
    else if (key == L"hotkeys") s.hotkeys = b;
    else if (key == L"maxWeight") s.maxWeight = f;
    else if (key == L"localTrust") s.localTrust = f;
    else if (key == L"residualTol") s.residualTol = f;
    else if (key == L"temporalArea") s.temporalArea = f;
    else if (key == L"excludeSizes") {
        s.excludeCount = 0;
        size_t a = 0;
        while (a < v.size() && s.excludeCount < (int32_t)kVuMaxExclude) {
            size_t z = v.find(L',', a);
            if (z == std::wstring::npos) z = v.size();
            int w = 0, h = 0;
            if (swscanf(v.substr(a, z - a).c_str(), L" %dx%d", &w, &h) == 2 && w > 0 && h > 0)
                s.exclude[s.excludeCount++] = ((uint32_t)w << 16) | (uint32_t)h;
            a = z + 1;
        }
    } else return false;
    Clamp(s);
    return true;
}

std::wstring ReadAppString(const wchar_t *key)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"app", key, L"", buf, 1024, g_settingsPath.c_str());
    return buf;
}

void WriteAppString(const wchar_t *key, const std::wstring &v)
{
    WritePrivateProfileStringW(L"app", key, v.empty() ? nullptr : v.c_str(), g_settingsPath.c_str());
}

// ---- SteamVR ----------------------------------------------------------------------------------

bool IsSteamVRRoot(const std::wstring &root)
{
    return !root.empty() && FileExists(root + L"\\bin\\win64\\vrcompositor.exe");
}

static std::string ReadFileA(const std::wstring &p)
{
    std::string out;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr);
    if (size != INVALID_FILE_SIZE && size < (8u << 20)) {
        out.resize(size);
        DWORD rd = 0;
        if (!ReadFile(f, &out[0], size, &rd, nullptr)) rd = 0;
        out.resize(rd);
    }
    CloseHandle(f);
    return out;
}

// Next JSON / VDF string literal starting at or after pos; handles \\ and \" escapes.
static bool NextQuoted(const std::string &s, size_t &pos, std::string &out)
{
    size_t a = s.find('"', pos);
    if (a == std::string::npos) return false;
    out.clear();
    size_t i = a + 1;
    for (; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == '/' ? '/' : s[i];
        } else {
            out += s[i];
        }
    }
    pos = i + 1;
    return i < s.size();
}

std::wstring FindSteamVR()
{
    std::wstring saved = ReadAppString(L"steamvrPath");
    if (IsSteamVRRoot(saved)) return saved;

    // 1. the runtime OpenVR itself uses
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::string j = ReadFileA(std::wstring(buf, n) + L"\\openvr\\openvrpaths.vrpath");
        size_t r = j.find("\"runtime\"");
        size_t a = r == std::string::npos ? r : j.find('[', r);
        size_t z = a == std::string::npos ? a : j.find(']', a);
        if (z != std::string::npos) {
            std::string list = j.substr(a + 1, z - a - 1);
            size_t pos = 0;
            std::string item;
            while (NextQuoted(list, pos, item)) {
                std::wstring w = Widen(item.c_str());
                for (auto &c : w) if (c == L'/') c = L'\\';
                if (IsSteamVRRoot(w)) return w;
            }
        }
    }
    // 2. Steam libraries
    std::vector<std::wstring> libs;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t v[MAX_PATH * 2];
        DWORD sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"SteamPath", nullptr, &type, (BYTE *)v, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring p(v);
            for (auto &c : p) if (c == L'/') c = L'\\';
            libs.push_back(p);
        }
        RegCloseKey(k);
    }
    n = GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) libs.push_back(std::wstring(buf, n) + L"\\Steam");
    const size_t base = libs.size();
    for (size_t i = 0; i < base; ++i) {
        std::string vdf = ReadFileA(libs[i] + L"\\steamapps\\libraryfolders.vdf");
        size_t pos = 0;
        while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
            pos += 6;
            std::string item;
            if (!NextQuoted(vdf, pos, item)) break;
            libs.push_back(Widen(item.c_str()));
        }
    }
    for (auto &lib : libs) {
        std::wstring cand = lib + L"\\steamapps\\common\\SteamVR";
        if (IsSteamVRRoot(cand)) return cand;
    }
    return L"";
}

bool IsSteamVRRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool running = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"vrcompositor.exe") == 0 || _wcsicmp(pe.szExeFile, L"vrserver.exe") == 0 ||
            _wcsicmp(pe.szExeFile, L"vrmonitor.exe") == 0) {
            running = true;
            break;
        }
    }
    CloseHandle(snap);
    return running;
}

// Reads a string from a PE file's version resource by scanning for its UTF-16 key (the
// resource stores "Key\0", padding, then "Value\0"). Works the same everywhere, whatever the
// loader does with a file called dxgi.dll.
static std::wstring RawVersionString(const std::string &file, const wchar_t *key)
{
    std::string pat;
    for (const wchar_t *k = key; *k; ++k) {
        pat += (char)(*k & 0xff);
        pat += (char)(*k >> 8);
    }
    pat += std::string(2, '\0');
    size_t at = file.find(pat);
    if (at == std::string::npos) return L"";
    size_t i = at + pat.size();
    while (i + 1 < file.size() && file[i] == 0 && file[i + 1] == 0) i += 2;
    std::wstring v;
    for (; i + 1 < file.size() && v.size() < 64; i += 2) {
        wchar_t c = (wchar_t)((unsigned char)file[i] | ((unsigned char)file[i + 1] << 8));
        if (!c) break;
        v += c;
    }
    return v;
}

DllInfo GetDllInfo(const std::wstring &path)
{
    DllInfo info;
    info.present = FileExists(path);
    if (!info.present) return info;
    {
        std::string raw = ReadFileA(path);
        std::wstring product = RawVersionString(raw, L"ProductName");
        if (product == L"vrupscale") {
            info.product = product;
            info.version = RawVersionString(raw, L"ProductVersion");
            info.ours = true;
            return info;
        }
    }
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return info;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return info;
    struct Lang { WORD lang, cp; } *langs = nullptr;
    UINT len = 0;
    std::wstring block = L"\\StringFileInfo\\040904b0\\";
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &len) && len >= sizeof(Lang))
        block = Fmt(L"\\StringFileInfo\\%04x%04x\\", langs[0].lang, langs[0].cp);
    wchar_t *v = nullptr;
    if (VerQueryValueW(data.data(), (block + L"ProductName").c_str(), (void **)&v, &len) && v) info.product = v;
    if (VerQueryValueW(data.data(), (block + L"ProductVersion").c_str(), (void **)&v, &len) && v) info.version = v;
    info.ours = info.product == L"vrupscale";
    return info;
}

std::wstring InstalledDllPath(const std::wstring &root) { return root + L"\\bin\\win64\\dxgi.dll"; }
std::wstring LegacyIniPath(const std::wstring &root) { return root + L"\\bin\\win64\\vrupscale.ini"; }

bool MigrateLegacySettings(const std::wstring &root)
{
    std::wstring legacy = LegacyIniPath(root);
    if (root.empty() || !FileExists(legacy) || FileExists(g_settingsPath)) return false;
    return CopyFileW(legacy.c_str(), g_settingsPath.c_str(), TRUE) != 0;
}

InstallResult Install(const std::wstring &root, bool force, std::wstring &msg)
{
    if (IsSteamVRRunning()) {
        msg = L"SteamVR is running. Close it completely (all its windows), then try again.";
        return InstallResult::SteamVRRunning;
    }
    std::wstring src = PayloadPath();
    if (!FileExists(src)) {
        msg = L"payload\\dxgi.dll is missing next to vrupscale.exe. Extract the whole zip and run it from there.";
        return InstallResult::NoPayload;
    }
    std::wstring dst = InstalledDllPath(root);
    DllInfo cur = GetDllInfo(dst);
    if (cur.present && !cur.ours) {
        if (!force) {
            msg = Fmt(L"SteamVR already has a dxgi.dll from another tool (%s).",
                      cur.product.empty() ? L"unknown" : cur.product.c_str());
            return InstallResult::ForeignDll;
        }
        std::wstring bak = dst + L".vrupscale-backup";
        if (!CopyFileW(dst.c_str(), bak.c_str(), FALSE)) {
            DWORD e = GetLastError();
            msg = Fmt(L"Could not back up the existing dxgi.dll (error %lu).", e);
            return e == ERROR_ACCESS_DENIED ? InstallResult::AccessDenied : InstallResult::Failed;
        }
    }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        DWORD e = GetLastError();
        msg = Fmt(L"Could not write %s (error %lu).", dst.c_str(), e);
        return e == ERROR_ACCESS_DENIED ? InstallResult::AccessDenied : InstallResult::Failed;
    }
    // Settings now live with the app; an old file next to the DLL would only confuse.
    std::wstring legacy = LegacyIniPath(root);
    if (FileExists(legacy)) DeleteFileW(legacy.c_str());
    DllInfo now = GetDllInfo(dst);
    msg = Fmt(L"Installed vrupscale %s into SteamVR. Start SteamVR to use it.", now.version.c_str());
    return InstallResult::Ok;
}

InstallResult Uninstall(const std::wstring &root, std::wstring &msg)
{
    if (IsSteamVRRunning()) {
        msg = L"SteamVR is running. Close it completely (all its windows), then try again.";
        return InstallResult::SteamVRRunning;
    }
    std::wstring dst = InstalledDllPath(root);
    std::wstring bak = dst + L".vrupscale-backup";
    DllInfo cur = GetDllInfo(dst);
    if (cur.present && !cur.ours) {
        msg = L"The dxgi.dll in SteamVR is not vrupscale's; left it alone.";
        return InstallResult::ForeignDll;
    }
    if (cur.present && !DeleteFileW(dst.c_str())) {
        DWORD e = GetLastError();
        msg = Fmt(L"Could not remove %s (error %lu).", dst.c_str(), e);
        return e == ERROR_ACCESS_DENIED ? InstallResult::AccessDenied : InstallResult::Failed;
    }
    bool restored = false;
    if (FileExists(bak)) restored = MoveFileW(bak.c_str(), dst.c_str()) != 0;
    std::wstring legacy = LegacyIniPath(root);
    if (FileExists(legacy)) DeleteFileW(legacy.c_str());
    msg = restored ? L"Removed vrupscale and put back the dxgi.dll it had replaced." : L"Removed vrupscale from SteamVR.";
    return InstallResult::Ok;
}

// ---- channel ----------------------------------------------------------------------------------

bool Channel::Open()
{
    if (sh_) return true;
    mismatch_ = false;
    map_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_channelName.c_str());
    if (!map_) return false;
    sh_ = (VuShared *)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VuShared));
    if (!sh_ || sh_->magic != kVuMagic || sh_->layout != kVuLayout) {
        mismatch_ = sh_ != nullptr && sh_->magic == kVuMagic;
        Close();
        return false;
    }
    return true;
}

void Channel::Close()
{
    if (sh_) UnmapViewOfFile(sh_);
    if (map_) CloseHandle(map_);
    sh_ = nullptr;
    map_ = nullptr;
}

bool Channel::ReadStatus(VuStatus &st) const
{
    if (!sh_) return false;
    for (int tries = 0; tries < 2000; ++tries) {
        int32_t a = sh_->statusSeq;
        if (a & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        memcpy(&st, (const void *)&sh_->status, sizeof(st));
        MemoryBarrier();
        if (sh_->statusSeq == a) return st.pid != 0;
    }
    return false;
}

int32_t Channel::SendSettings(const VuSettings &s)
{
    if (!sh_) return 0;
    InterlockedIncrement((volatile LONG *)&sh_->settingsSeq);
    MemoryBarrier();
    memcpy((void *)&sh_->settings, &s, sizeof(s));
    MemoryBarrier();
    return (int32_t)InterlockedIncrement((volatile LONG *)&sh_->settingsSeq);
}

void Channel::SendCommand(uint32_t cmd)
{
    if (!sh_) return;
    sh_->command = cmd;
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)&sh_->commandSeq);
}

bool StatusIsLive(const VuStatus &st)
{
    if (!st.pid) return false;
    HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, st.pid);
    if (!p) return false;
    bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive && (GetTickCount() - st.tick) < 5000;
}

const wchar_t *FormatLabel(uint32_t f)
{
    switch (f) {
    case 2: return L"RGBA32F";
    case 9: return L"RGBA16 typeless";
    case 10: return L"RGBA16F (HDR)";
    case 24: return L"RGB10A2";
    case 26: return L"R11G11B10F";
    case 27: return L"RGBA8 typeless";
    case 28: return L"RGBA8";
    case 29: return L"RGBA8 sRGB";
    case 87: return L"BGRA8";
    case 88: return L"BGRX8";
    case 90: return L"BGRA8 typeless";
    case 91: return L"BGRA8 sRGB";
    default: return L"other";
    }
}

} // namespace app
