#include "common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

namespace vu {

Config g_cfg;
volatile LONG g_cfgGen = 1;
std::wstring g_dllDir;
std::wstring g_iniPath;          // switched (ConfigFileChanged) under g_iniLock once other threads run
std::wstring g_iniPreferred;
std::wstring g_logPath;

static SRWLOCK g_iniLock = SRWLOCK_INIT;
static volatile LONG64 g_iniTime = 0;   // last-write time of the file as last read (FILETIME as 64 bits)

std::wstring IniPath()
{
    AcquireSRWLockShared(&g_iniLock);
    std::wstring p = g_iniPath;
    ReleaseSRWLockShared(&g_iniLock);
    return p;
}

static std::wstring ReadStr(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, const wchar_t *def)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec, key, def, buf, 1024, ini);
    // strip trailing comments ("; ...") and whitespace
    std::wstring s(buf);
    size_t sc = s.find_first_of(L";#");
    if (sc != std::wstring::npos) s.resize(sc);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && iswspace(s[b])) ++b;
    return s.substr(b);
}

static bool ReadBool(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, bool def)
{
    std::wstring s = ReadStr(ini, sec, key, def ? L"1" : L"0");
    if (s.empty()) return def;
    wchar_t c = towlower(s[0]);
    return c == L'1' || c == L't' || c == L'y' || (c == L'o' && s.size() > 1 && towlower(s[1]) == L'n');
}

static int ReadInt(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, int def)
{
    std::wstring s = ReadStr(ini, sec, key, L"");
    if (s.empty()) return def;
    return (int)wcstol(s.c_str(), nullptr, 10);
}

static float ReadFloat(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, float def)
{
    std::wstring s = ReadStr(ini, sec, key, L"");
    if (s.empty()) return def;
    return (float)wcstod(s.c_str(), nullptr);
}

static std::vector<Size2> ReadSizes(const wchar_t *ini, const wchar_t *sec, const wchar_t *key)
{
    std::vector<Size2> out;
    std::wstring s = ReadStr(ini, sec, key, L"");
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(L',', i);
        if (j == std::wstring::npos) j = s.size();
        std::wstring item = s.substr(i, j - i);
        int w = 0, h = 0;
        if (swscanf(item.c_str(), L" %dx%d", &w, &h) == 2 || swscanf(item.c_str(), L" %dX%d", &w, &h) == 2) {
            if (w > 0 && h > 0) out.push_back({ w, h });
        }
        i = j + 1;
    }
    return out;
}

static bool GetIniTime(const wchar_t *ini, LONG64 &t)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(ini, GetFileExInfoStandard, &fad)) return false;
    t = (LONG64)(((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime);
    return true;
}

bool LoadConfig(Config &c, std::wstring *pathOut)
{
    Config d; // defaults
    const std::wstring path = IniPath();
    const wchar_t *ini = path.c_str();
    if (pathOut) *pathOut = path;
    LONG64 t = 0;
    bool exists = GetIniTime(ini, t);
    if (exists) InterlockedExchange64(&g_iniTime, t);
    if (!exists) {
        c = d;
        return false;
    }

    c.active = ReadBool(ini, L"general", L"active", d.active);
    c.logLevel = ReadInt(ini, L"general", L"logLevel", d.logLevel);
    c.statsIntervalSec = ReadInt(ini, L"general", L"statsIntervalSec", d.statsIntervalSec);
    c.hotkeys = ReadBool(ini, L"general", L"hotkeys", d.hotkeys);
    c.gpuTiming = ReadBool(ini, L"general", L"gpuTiming", d.gpuTiming);

    c.enabled = ReadBool(ini, L"upscale", L"enabled", d.enabled);
    std::wstring mode = ReadStr(ini, L"upscale", L"mode", L"temporal");
    wchar_t m0 = mode.empty() ? L't' : towlower(mode[0]);
    c.mode = m0 == L'b' ? Mode::Bilinear : (m0 == L'f' ? Mode::Fsr : Mode::Temporal);
    c.upscaleFactor = ReadFloat(ini, L"upscale", L"upscaleFactor", d.upscaleFactor);
    c.sharpness = ReadFloat(ini, L"upscale", L"sharpness", d.sharpness);
    {
        const std::wstring f = ReadStr(ini, L"upscale", L"fxaa", L"off");
        const wchar_t f0 = f.empty() ? L'o' : towlower(f[0]);
        c.fxaa = (f0 == L'b' || f0 == L'1') ? kFxaaBefore : ((f0 == L'a' || f0 == L'2') ? kFxaaAfter : kFxaaOff);
    }
    c.dedupeMs = ReadFloat(ini, L"upscale", L"dedupeMs", d.dedupeMs);
    c.tint = ReadBool(ini, L"upscale", L"debugTint", d.tint);

    c.maxWeight = ReadFloat(ini, L"temporal", L"maxWeight", d.maxWeight);
    c.clipGamma = ReadFloat(ini, L"temporal", L"clipGamma", d.clipGamma);
    c.residualTol = ReadFloat(ini, L"temporal", L"residualTol", d.residualTol);
    c.sampleSigma = ReadFloat(ini, L"temporal", L"sampleSigma", d.sampleSigma);
    c.minNewWeight = ReadFloat(ini, L"temporal", L"minNewWeight", d.minNewWeight);
    c.lkEps = ReadFloat(ini, L"temporal", L"lkEps", d.lkEps);
    c.searchLambda = ReadFloat(ini, L"temporal", L"searchLambda", d.searchLambda);
    c.clipReject = ReadFloat(ini, L"temporal", L"clipReject", d.clipReject);
    c.localTrust = ReadFloat(ini, L"temporal", L"localTrust", d.localTrust);
    c.localGate = ReadFloat(ini, L"temporal", L"localGate", d.localGate);
    c.localRatio = ReadFloat(ini, L"temporal", L"localRatio", d.localRatio);
    c.sharpHistory = ReadBool(ini, L"temporal", L"sharpHistory", d.sharpHistory);
    c.localCheck = ReadInt(ini, L"temporal", L"localCheck", d.localCheck);
    c.localAgree = ReadFloat(ini, L"temporal", L"localAgree", d.localAgree);
    c.localAperture = ReadFloat(ini, L"temporal", L"localAperture", d.localAperture);
    c.workMargin = ReadInt(ini, L"temporal", L"workMargin", d.workMargin);
    c.commandLists = ReadBool(ini, L"temporal", L"commandLists", d.commandLists);
    c.bilinearEdges = ReadBool(ini, L"temporal", L"bilinearEdges", d.bilinearEdges);
    {
        const std::wstring of = ReadStr(ini, L"temporal", L"opticalFlow", L"default");
        c.fsrFlow = !of.empty() && towlower(of[0]) == L'f';   // fsr3
    }

    c.gazeFollow = ReadBool(ini, L"eyes", L"followGaze", d.gazeFollow);
    c.gazeShow = ReadBool(ini, L"eyes", L"showGaze", d.gazeShow);
    c.gazeOffset[0] = ReadFloat(ini, L"eyes", L"offsetX", d.gazeOffset[0]);
    c.gazeOffset[1] = ReadFloat(ini, L"eyes", L"offsetY", d.gazeOffset[1]);
    c.gazeDeadzone = ReadFloat(ini, L"eyes", L"deadzone", d.gazeDeadzone);
    {   // "l r t b l r t b": what SteamVR reports for each eye (the app writes it)
        std::wstring pr = ReadStr(ini, L"eyes", L"projection", L"");
        float v[8] = {};
        if (swscanf(pr.c_str(), L"%f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8)
            memcpy(c.eyeProj, v, sizeof(v));
    }
    c.temporalArea = ReadFloat(ini, L"temporal", L"temporalArea", d.temporalArea);
    c.gazeArea = ReadFloat(ini, L"temporal", L"gazeArea", d.gazeArea);
    c.sharpArea = ReadFloat(ini, L"upscale", L"sharpnessArea", d.sharpArea);
    c.gazeSharpArea = ReadFloat(ini, L"upscale", L"gazeSharpnessArea", d.gazeSharpArea);
    c.maxStreams = ReadInt(ini, L"temporal", L"maxStreams", d.maxStreams);
    c.linkPools = ReadBool(ini, L"temporal", L"linkPools", d.linkPools);
    c.linkGapMs = ReadFloat(ini, L"temporal", L"linkGapMs", d.linkGapMs);
    c.detectContent = ReadBool(ini, L"temporal", L"detectContent", d.detectContent);
    c.probeWindowMs = ReadInt(ini, L"temporal", L"probeWindowMs", d.probeWindowMs);

    c.minWidth = ReadInt(ini, L"match", L"minWidth", d.minWidth);
    c.minHeight = ReadInt(ini, L"match", L"minHeight", d.minHeight);
    c.minAspect = ReadFloat(ini, L"match", L"minAspect", d.minAspect);
    c.maxAspect = ReadFloat(ini, L"match", L"maxAspect", d.maxAspect);
    c.onlySizes = ReadSizes(ini, L"match", L"onlySizes");
    c.excludeSizes = ReadSizes(ini, L"match", L"excludeSizes");
    c.requireShared = ReadBool(ini, L"match", L"requireShared", d.requireShared);
    c.followCopies = ReadBool(ini, L"match", L"followCopies", d.followCopies);
    c.maxArraySize = ReadInt(ini, L"match", L"maxArraySize", d.maxArraySize);

    SanitizeConfig(c);
    return true;
}

void SanitizeConfig(Config &c)
{
    // Non-numbers fall back to defaults; sizes are kept to the 0.01 steps the app writes, so a
    // hand-edited value doesn't re-plan again once the app saves it rounded.
    const Config d;
    auto fin = [](float &v, float def) { if (!(v == v) || v > 1e30f || v < -1e30f) v = def; };
    fin(c.upscaleFactor, d.upscaleFactor); fin(c.sharpness, d.sharpness); fin(c.dedupeMs, d.dedupeMs);
    fin(c.minAspect, d.minAspect); fin(c.maxAspect, d.maxAspect); fin(c.maxWeight, d.maxWeight);
    fin(c.clipGamma, d.clipGamma); fin(c.residualTol, d.residualTol); fin(c.sampleSigma, d.sampleSigma);
    fin(c.minNewWeight, d.minNewWeight); fin(c.lkEps, d.lkEps); fin(c.searchLambda, d.searchLambda);
    fin(c.clipReject, d.clipReject); fin(c.localTrust, d.localTrust); fin(c.localGate, d.localGate);
    fin(c.localRatio, d.localRatio); fin(c.temporalArea, d.temporalArea); fin(c.gazeArea, d.gazeArea);
    fin(c.sharpArea, d.sharpArea); fin(c.gazeSharpArea, d.gazeSharpArea); fin(c.linkGapMs, d.linkGapMs);
    c.upscaleFactor = roundf(c.upscaleFactor * 100.0f) / 100.0f;
    c.temporalArea = roundf(c.temporalArea * 100.0f) / 100.0f;
    c.gazeArea = roundf(c.gazeArea * 100.0f) / 100.0f;
    c.sharpArea = roundf(c.sharpArea * 100.0f) / 100.0f;
    c.gazeSharpArea = roundf(c.gazeSharpArea * 100.0f) / 100.0f;
    if (c.upscaleFactor < 1.0f) c.upscaleFactor = 1.0f;
    if (c.upscaleFactor > 3.0f) c.upscaleFactor = 3.0f;
    if (c.sharpness < 0.0f) c.sharpness = 0.0f;
    if (c.sharpness > 1.0f) c.sharpness = 1.0f;
    if (c.dedupeMs < 0.0f) c.dedupeMs = 0.0f;
    if (c.maxArraySize < 1) c.maxArraySize = 1;
    if (c.statsIntervalSec < 0) c.statsIntervalSec = 0;
    if (c.minAspect <= 0.0f) c.minAspect = 0.01f;
    if (c.maxWeight < 0.1f) c.maxWeight = 0.1f;
    if (c.maxWeight > 64.0f) c.maxWeight = 64.0f;
    if (c.clipGamma < 0.25f) c.clipGamma = 0.25f;
    if (c.residualTol < 0.05f) c.residualTol = 0.05f;
    if (c.sampleSigma < 0.05f) c.sampleSigma = 0.05f;
    if (c.minNewWeight < 0.001f) c.minNewWeight = 0.001f;
    if (c.minNewWeight > 1.0f) c.minNewWeight = 1.0f;
    if (c.lkEps < 1e-6f) c.lkEps = 1e-6f;
    if (c.searchLambda < 0.0f) c.searchLambda = 0.0f;
    if (c.clipReject < 0.0f) c.clipReject = 0.0f;
    c.localTrust = c.localTrust < 0.0f ? 0.0f : (c.localTrust > 1.0f ? 1.0f : c.localTrust);
    for (float &o : c.gazeOffset) {
        fin(o, 0.0f);
        o = o < -1.0f ? -1.0f : (o > 1.0f ? 1.0f : o);
    }
    for (float &p : c.eyeProj) fin(p, 0.0f);
    fin(c.gazeDeadzone, d.gazeDeadzone);
    c.gazeDeadzone = c.gazeDeadzone < 0.0f ? 0.0f : (c.gazeDeadzone > 0.3f ? 0.3f : c.gazeDeadzone);
    if (c.localGate < 0.0f) c.localGate = 0.0f;
    c.localRatio = c.localRatio < 0.1f ? 0.1f : (c.localRatio > 1.0f ? 1.0f : c.localRatio);
    c.temporalArea = c.temporalArea < 0.3f ? 0.3f : (c.temporalArea > 1.0f ? 1.0f : c.temporalArea);
    c.gazeArea = c.gazeArea < 0.3f ? 0.3f : (c.gazeArea > 1.0f ? 1.0f : c.gazeArea);
    c.sharpArea = c.sharpArea < 0.3f ? 0.3f : (c.sharpArea > 1.0f ? 1.0f : c.sharpArea);   // (less than the area: the area)
    c.gazeSharpArea = c.gazeSharpArea < 0.3f ? 0.3f : (c.gazeSharpArea > 1.0f ? 1.0f : c.gazeSharpArea);
    if (c.maxStreams < 0) c.maxStreams = 0;
    if (c.maxStreams > 16) c.maxStreams = 16;
    if (c.linkGapMs < 0.0f) c.linkGapMs = 0.0f;
    c.probeWindowMs = c.probeWindowMs < 50 ? 50 : (c.probeWindowMs > 5000 ? 5000 : c.probeWindowMs);
    c.workMargin = c.workMargin < 64 ? 64 : (c.workMargin > 65536 ? 65536 : c.workMargin);
    if (c.maxAspect < c.minAspect) c.maxAspect = c.minAspect;
}

void PrintConfig(const Config &c)
{
    LOGI("config: active=%d enabled=%d mode=%s upscaleFactor=%.3f sharpness=%.2f sharpnessArea=%.2f gazeSharpnessArea=%.2f "
         "fxaa=%s dedupeMs=%.1f tint=%d sharpHistory=%d bilinearEdges=%d followGaze=%d showGaze=%d gazeOffset=%.3f,%.3f",
         c.active, c.enabled, ModeName(c.mode), c.upscaleFactor, c.sharpness, c.sharpArea, c.gazeSharpArea, FxaaName(c.fxaa), c.dedupeMs, c.tint,
         c.sharpHistory, c.bilinearEdges, c.gazeFollow, c.gazeShow, c.gazeOffset[0], c.gazeOffset[1]);
    LOGI("config: temporal maxWeight=%.1f clipGamma=%.2f residualTol=%.3f sampleSigma=%.2f minNewWeight=%.3f lkEps=%.4f "
         "searchLambda=%.4f clipReject=%.2f localTrust=%.2f localGate=%.2f localRatio=%.2f temporalArea=%.2f gazeArea=%.2f maxStreams=%d "
         "linkPools=%d linkGapMs=%.1f detectContent=%d probeWindowMs=%d commandLists=%d opticalFlow=%s",
         c.maxWeight, c.clipGamma, c.residualTol, c.sampleSigma, c.minNewWeight, c.lkEps, c.searchLambda, c.clipReject,
         c.localTrust, c.localGate, c.localRatio, c.temporalArea, c.gazeArea, c.maxStreams, c.linkPools, c.linkGapMs, c.detectContent,
         c.probeWindowMs, c.commandLists, c.fsrFlow ? "fsr3" : "default");
    LOGI("config: match min=%dx%d aspect=[%.2f, %.2f] requireShared=%d followCopies=%d maxArraySize=%d onlySizes=%zu excludeSizes=%zu",
         c.minWidth, c.minHeight, c.minAspect, c.maxAspect, c.requireShared, c.followCopies, c.maxArraySize,
         c.onlySizes.size(), c.excludeSizes.size());
    for (auto &s : c.onlySizes) LOGI("config:   onlySize %dx%d", s.w, s.h);
    for (auto &s : c.excludeSizes) LOGI("config:   excludeSize %dx%d", s.w, s.h);
    LOGI("config: logLevel=%d statsIntervalSec=%d hotkeys=%d gpuTiming=%d", c.logLevel, c.statsIntervalSec, c.hotkeys, c.gpuTiming);
}

const char *FxaaName(int f) { return f == kFxaaBefore ? "before" : (f == kFxaaAfter ? "after" : "off"); }

const char *ModeName(Mode m)
{
    return m == Mode::Temporal ? "temporal" : (m == Mode::Fsr ? "fsr" : "bilinear");
}

void ConfigRecheck() { InterlockedExchange64(&g_iniTime, 0); }

bool ConfigFileChanged(bool *switched)
{
    if (switched) *switched = false;
    const std::wstring path = IniPath();
    // The app's settings file appeared after we started on a fallback: switch to it.
    if (!g_iniPreferred.empty() && path != g_iniPreferred &&
        GetFileAttributesW(g_iniPreferred.c_str()) != INVALID_FILE_ATTRIBUTES) {
        AcquireSRWLockExclusive(&g_iniLock);
        g_iniPath = g_iniPreferred;
        ReleaseSRWLockExclusive(&g_iniLock);
        InterlockedExchange64(&g_iniTime, 0);
        if (switched) *switched = true;
        return true;
    }
    LONG64 t;
    if (!GetIniTime(path.c_str(), t)) return false;
    return t != InterlockedCompareExchange64(&g_iniTime, 0, 0);
}

} // namespace vu
