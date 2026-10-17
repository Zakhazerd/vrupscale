// vrupscale.exe: install vrupscale into SteamVR, see what it is doing, change its settings live.
// The window is drawn by hand (ui.cpp) in the style of Valve's Steam hardware pages; the only
// native control is the fixed-height field.
#include "changelog.h"
#include "core.h"
#include "gameres.h"
#include "ui.h"
#include "update.h"
#include "vrlink.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <map>

#ifndef VU_APP_VERSION
#define VU_APP_VERSION "dev"
#endif

using namespace app;
using ui::RectF;
using ui::Mix;
namespace col = ui::col;

namespace {

// ---- controls ---------------------------------------------------------------------------------

enum : int {
    IDC_STEAMPATH = 100, IDC_INSTALLSTATE, IDC_INSTALL, IDC_UNINSTALL, IDC_BROWSE, IDC_INSTALLMSG,
    IDC_ENABLED, IDC_MODE, IDC_SCALE, IDC_FACTORVAL, IDC_FACTORHINT, IDC_SHARP, IDC_SHARPVAL,
    IDC_TINT, IDC_HOTKEYS, IDC_AREA, IDC_AREAVAL, IDC_AREAHINT, IDC_HIST, IDC_HISTVAL, IDC_MOVING, IDC_MOVINGVAL, IDC_TOL, IDC_TOLVAL,
    IDC_DEFAULTS, IDC_SAVED, IDC_OPENLOG, IDC_OPENFOLDER, IDC_STATUS, IDC_DETAIL, IDC_TEXLIST, IDC_TEXHINT, IDC_EVENTS,
    IDC_TICKALL, IDC_SHARPHIST, IDC_BILEDGES, IDC_GAMERES, IDC_GAZEFOLLOW, IDC_GAZESHOW, IDC_CALIBRATE, IDC_CALCLEAR,
    IDC_GAZESTATUS, IDC_CALSTATUS, IDC_UPDATE, IDC_CHANGELOG, IDC_WITHVR, IDC_SHARPAREA, IDC_SHARPAREAVAL, IDC_FXAA,
};
enum : UINT_PTR { TIMER_REFRESH = 1, TIMER_SAVE, TIMER_PUSH, TIMER_INSTALL, TIMER_REPLAN, TIMER_TIP };
const UINT WM_APP_SYNCLIST = WM_APP + 1, WM_APP_GAMERES = WM_APP + 2, WM_APP_PROJ = WM_APP + 3, WM_APP_UPDCHECK = WM_APP + 4,
           WM_APP_UPDPROGRESS = WM_APP + 5, WM_APP_UPDDONE = WM_APP + 6, WM_APP_VRQUIT = WM_APP + 7, WM_APP_AUTOLAUNCH = WM_APP + 8;

HWND g_wnd = nullptr;
UINT g_dpi = 96;
HINSTANCE g_inst = nullptr;

VuSettings g_s;            // what the app shows and saves
Channel g_ch;
VuStatus g_st = {};
bool g_live = false;
int32_t g_sentSeq = 0;
DWORD g_lastEdit = 0;
bool g_pushPending = false;
bool g_pushOnConnect = false;   // settings were adjusted on loading: send them once SteamVR is there

// VRChat's resolution: the resolution SteamVR gives VRChat (its global value x VRChat's own
// per-application one) is what the user wants to see. While VRChat runs in SteamVR and this app is
// open, the global value is lowered to the preset's share (1 / factor^2 of the pixels), so VRChat
// renders that share and vrupscale upscales it back (gameres.h, SetGlobalResolution; SteamVR applies
// a global change live). When VRChat or the app closes, the user's global value is put back.
// [app] manageResolution=0 turns this off; globalResBefore = "scale manual" while vrupscale has
// the global value changed (what to put back).
bool g_gameRes = true;
std::wstring g_gameResStatus;        // for the hint under the presets
float g_resApplied = 0;              // the factor the global resolution is set for (0: not set by this run)
bool g_resBusy = false;              // a request is running (one at a time)
volatile LONG g_resWorkers = 0;
DWORD g_resCheck = 0, g_resRetry = 0;
bool g_resVrchat = false;            // VRChat is running
bool g_resMigrated = false;          // 0.4.7/0.4.8's per-application value put back
bool g_dllOurs = true;               // vrupscale is installed in SteamVR (else nothing would upscale VRChat)

// Start with SteamVR (vrlink.h): SteamVR starts the app (minimized) when it starts, and the app closes
// when SteamVR does. While on, a thread keeps a connection to SteamVR for as long as it runs, so that
// SteamVR's request to quit arrives: the resolution is put back while SteamVR still answers, then the
// app closes (or, if SteamVR went without asking, once its processes are gone). The thread also keeps
// the app's entry among SteamVR's startup apps in line with the switch. [app] withSteamVR=0 turns it
// off; withSteamVRApplied = what SteamVR was last told (1 or 0).
bool g_withVR = true;
bool g_fromVR = false;               // this run was started by SteamVR
HANDLE g_linkThread = nullptr, g_linkWake = nullptr;
volatile LONG g_linkStop = 0;
volatile LONG g_linkWant = 1;        // the thread keeps the lasting connection
volatile LONG g_linkApplied = 0;     // SteamVR was last told "on"
volatile LONG g_linkSync = 1;        // the thread brings SteamVR's startup entry in line (again)
bool g_vrSeen = false;               // SteamVR ran during this run
bool g_vrQuitting = false;           // SteamVR asked the app to quit; its processes are still closing
bool g_closingWithVR = false;
DWORD g_vrGoneAt = 0, g_vrCheck = 0;

// Eye tracking: each eye's projection is asked from SteamVR once per SteamVR session (so a gaze
// direction can be placed in each eye's picture); calibration messages from the DLL are shown
// until the user clears the calibration.
uint32_t g_projPid = 0;
DWORD g_projTry = 0;
bool g_calMsgHidden = false;
bool g_ui = false;         // true while the app itself is updating controls

// Updates (update.h): GitHub is asked at start and every 6 hours ([app] checkUpdates=0: never); a
// newer release shows an Update button by the version, which downloads and installs it.
enum UpdState { UpdNone, UpdAvailable, UpdDownloading, UpdFailed };
UpdState g_updState = UpdNone;
app::UpdateInfo g_upd;               // the last answer that offered a release
std::wstring g_updError;             // why the last try failed
float g_updProgress = -1;            // 0..1 while downloading (-1: size unknown)
bool g_updChecking = false;
DWORD g_updLastCheck = 0, g_updStart = 0;
volatile long g_updCancel = 0;
bool g_updRestarting = false;        // closing to start the new version
std::wstring g_updatedFrom;          // this run was started by an update from that version
std::wstring g_root;       // SteamVR folder
// A texture list row: (w << 16) | h, the DLL's entry for it (-1: an unticked size not in use now),
// and what is shown.
struct Row {
    uint32_t wh = 0;
    int tex = -1;
    std::wstring size, format, result, seen;
    bool eye = false, on = false, canChange = false;
};
std::vector<Row> g_rows;
bool g_replanPending = false;   // a size or area change waits for the slider to settle
std::vector<std::wstring> g_events;
COLORREF g_statusColor = col::inkFaint;
ui::Mood g_mood = ui::MoodSleep;
DWORD g_lastInstallCheck = 0;

// ---- layout (logical pixels) ------------------------------------------------------------------

constexpr float kW = 1120, kH = 900;   // the page
constexpr float kColW = 348;
const RectF kCardUp = { 24, 214, kColW, 444 };
const RectF kCardEye = { 24, 672, kColW, 210 };
const RectF kCardTemp = { 386, 214, kColW, 302 };
const RectF kCardVR = { 386, 530, kColW, 352 };   // SteamVR, and the settings file
const RectF kCardTex = { 748, 214, kColW, 262 };
const RectF kCardEv = { 748, 490, kColW, 392 };
const RectF kList = { kCardTex.x + 14, kCardTex.y + 52, kColW - 28, 160 };
const RectF kLog = { kCardEv.x + 14, kCardEv.y + 52, kColW - 28, kCardEv.h - 66 };
constexpr float kRowH = 40, kLogLineH = 15;
constexpr int kRowsShown = 4;
constexpr float kHeadsetX = 908;

// ---- widgets ------------------------------------------------------------------------------------

std::vector<ui::Widget> g_w;          // in tab order
ui::Interact g_in;
std::map<int, std::wstring> g_text;   // texts drawn on the page, by id
float g_scroll = 0;                   // logical pixels the page is scrolled (when the window is short)
int g_listScroll = 0;                 // first texture row shown
int g_logScroll = 0;                  // event lines scrolled back from the newest
int g_hoverRow = -1, g_focusRow = 0;
bool g_mouseTracked = false;
bool g_bgDirty = true;                // the cached background: sky, ground, cards, characters

float Scale() { return g_dpi / 96.0f; }
int S(float v) { return (int)lroundf(v * Scale()); }
float L(int px) { return px / Scale(); }

ui::Widget *W(int id)
{
    for (auto &w : g_w)
        if (w.id == id) return &w;
    return nullptr;
}

void Invalidate()
{
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

void SetText(int id, const std::wstring &t)
{
    if (ui::Widget *w = W(id)) {
        if (w->text != t) {
            w->text = t;
            Invalidate();
        }
        return;
    }
    std::wstring &cur = g_text[id];
    if (cur != t) {
        cur = t;
        Invalidate();
    }
}

const std::wstring &TextOf(int id) { return g_text[id]; }

// ---- settings <-> controls --------------------------------------------------------------------

int Pos(int id)
{
    const ui::Widget *w = W(id);
    return w ? w->pos : 0;
}

void SetPos(int id, int v)
{
    if (ui::Widget *w = W(id)) {
        v = std::max(w->lo, std::min(w->hi, v));
        if (w->pos != v) {
            w->pos = v;
            Invalidate();
        }
    }
}

void SetCheck(int id, bool on)
{
    if (ui::Widget *w = W(id))
        if (w->on != on) {
            w->on = on;
            Invalidate();
        }
}

void SetSel(int id, int sel)
{
    if (ui::Widget *w = W(id))
        if (w->sel != sel) {
            w->sel = sel;
            Invalidate();
        }
}

void Enable(int id, bool on)
{
    if (ui::Widget *w = W(id))
        if (w->enabled != on) {
            w->enabled = on;
            Invalidate();
        }
}

bool IsExcluded(uint32_t wh)
{
    for (int i = 0; i < g_s.excludeCount; ++i)
        if (g_s.exclude[i] == wh) return true;
    return false;
}

void UpdateValueLabels()
{
    const float f = g_s.upscaleFactor;
    const int preset = VuScalePreset(f);
    SetText(IDC_FACTORVAL, preset >= 0 ? Widen(kVuScaleName[preset]) : Fmt(L"Custom %.2f×", f));
    const int pct = (int)lroundf(100.0f / (f * f));   // SteamVR's percentages count pixels
    if (f < 1.005f)
        SetText(IDC_FACTORHINT, L"Full resolution, no upscaling. SteamVR's resolution is left as you set it.");
    else if (!g_gameRes)
        SetText(IDC_FACTORHINT, Fmt(L"Upscales %d%% → 100%% per axis. In SteamVR, set VRChat's resolution to about %d%% of what "
                                    L"you want to see.", (int)lroundf(100.0f / f), pct));
    else if (g_gameResStatus.rfind(L"Couldn't", 0) == 0)
        SetText(IDC_FACTORHINT, g_gameResStatus);
    else if (!g_gameResStatus.empty())
        SetText(IDC_FACTORHINT, g_gameResStatus);
    else
        SetText(IDC_FACTORHINT, Fmt(L"While VRChat runs, it renders %d%% of the resolution SteamVR gives it, upscaled back to "
                                    L"that%s.", pct, g_withVR ? L"" : L" (keep this app open)"));
    SetText(IDC_SHARPVAL, Pos(IDC_SHARP) == 0 ? std::wstring(L"off") : Fmt(L"%.2f", Pos(IDC_SHARP) / 100.0f));
    SetText(IDC_AREAVAL, Pos(IDC_AREA) >= 100 ? std::wstring(L"all") : Fmt(L"%d%%", Pos(IDC_AREA)));
    Enable(IDC_BILEDGES, g_s.mode != kVuModeBilinear && Pos(IDC_AREA) < 100);   // area "all": there are no edges
    // the Sharpness area holds the area: shown at least at its size (also while the area slider moves)
    if (Pos(IDC_SHARPAREA) < Pos(IDC_AREA)) SetPos(IDC_SHARPAREA, Pos(IDC_AREA));
    SetText(IDC_SHARPAREAVAL, Pos(IDC_SHARPAREA) >= 100 ? std::wstring(L"all") : Fmt(L"%d%%", Pos(IDC_SHARPAREA)));
    Enable(IDC_SHARPAREA, g_s.mode != kVuModeBilinear && Pos(IDC_SHARP) > 0 && Pos(IDC_AREA) < 100);
    SetText(IDC_HISTVAL, Fmt(L"%.2f", Pos(IDC_HIST) / 100.0f));
    SetText(IDC_MOVINGVAL, Pos(IDC_MOVING) == 0 ? std::wstring(L"off") : Fmt(L"%.2f", Pos(IDC_MOVING) / 100.0f));
    SetText(IDC_TOLVAL, Fmt(L"%.2f", Pos(IDC_TOL) / 100.0f));
}

void SyncListChecks();

// The area the slider shows and edits: its own value while following the gaze.
float ActiveArea() { return g_s.gazeFollow ? g_s.gazeArea : g_s.temporalArea; }
float &ActiveSharpArea() { return g_s.gazeFollow ? g_s.gazeSharpArea : g_s.sharpArea; }

// The Eye tracking card: what the tracker is doing, and the calibration.
void UpdateGazeUi()
{
    const bool areaModes = g_s.mode != kVuModeBilinear;   // Temporal and FSR 1 have the area
    const VuGazeInfo &gz = g_st.gaze;
    std::wstring st;
    if (g_live) {
        if (gz.source == 0)
            st = L"No eye tracking data. In Bigscreen's Beyond Eyetracking client, switch on VRCFT.";
        else if (!gz.live)
            st = L"Bigscreen eye tracking found, but no gaze right now (headset off, or its client paused).";
        else if (!g_s.gazeFollow)
            st = Fmt(L"Bigscreen eye tracking, %.0f updates/s. Switch on Follow gaze to use it.", gz.rate);
        else if (!areaModes)
            st = L"Following the gaze works in Temporal and FSR 1 mode.";
        else if (g_s.gazeArea >= 0.995f)
            st = L"The area is \"all\": nothing to move. Lower it to follow the gaze.";
        else
            st = Fmt(L"The area follows your gaze (Bigscreen eye tracking, %.0f updates/s).", gz.rate);
    } else {
        st = app::EyeTrackerPresent() ? L"Bigscreen eye tracking is running. It is used once SteamVR runs."
                                      : L"For Bigscreen Beyond 2e: start its Eyetracking client and switch on VRCFT there.";
    }
    SetText(IDC_GAZESTATUS, st);

    const float ox = g_s.gazeOffset[0], oy = g_s.gazeOffset[1];
    const bool calibrated = ox != 0.0f || oy != 0.0f;
    std::wstring cal;
    if (g_live && gz.calState == kVuCalRunning)
        cal = Fmt(L"Look at the cross straight ahead until it goes… %d%%", (int)(gz.calProgress * 100.0f));
    else if (g_live && gz.calState == kVuCalFailed && !g_calMsgHidden)
        cal = Widen(gz.calMessage);
    else if (calibrated)
        cal = Fmt(L"Calibrated: corrects %.1f° %s and %.1f° %s.", fabsf(atanf(ox)) * 57.2958f, ox >= 0 ? L"right" : L"left",
                  fabsf(atanf(oy)) * 57.2958f, oy >= 0 ? L"up" : L"down");
    else
        cal = L"Not calibrated. Calibrate, then look at the cross in VR (or press Ctrl+F8 there).";
    SetText(IDC_CALSTATUS, cal);
    Enable(IDC_GAZEFOLLOW, areaModes);
    Enable(IDC_GAZESHOW, areaModes);
    Enable(IDC_CALIBRATE, g_live && areaModes && g_st.current.enabled && gz.live && gz.calState != kVuCalRunning);
    Enable(IDC_CALCLEAR, calibrated);
}

void ModelToUi()
{
    g_ui = true;
    SetCheck(IDC_ENABLED, g_s.enabled != 0);
    SetSel(IDC_MODE, g_s.mode);
    SetSel(IDC_SCALE, VuScalePreset(g_s.upscaleFactor));
    SetSel(IDC_FXAA, g_s.fxaa);
    SetPos(IDC_SHARP, (int)lroundf(g_s.sharpness * 100));
    SetCheck(IDC_TINT, g_s.tint != 0);
    SetCheck(IDC_HOTKEYS, g_s.hotkeys != 0);
    SetPos(IDC_AREA, (int)lroundf(ActiveArea() * 100));
    SetPos(IDC_SHARPAREA, (int)lroundf(std::max(ActiveSharpArea(), ActiveArea()) * 100));
    SetPos(IDC_HIST, (int)lroundf(g_s.maxWeight * 100));
    SetPos(IDC_MOVING, (int)lroundf(g_s.localTrust * 100));
    SetPos(IDC_TOL, (int)lroundf(g_s.residualTol * 100));
    SetCheck(IDC_SHARPHIST, g_s.fastHistory == 0);
    SetCheck(IDC_BILEDGES, g_s.bilinearEdges != 0);
    SetCheck(IDC_GAZEFOLLOW, g_s.gazeFollow != 0);
    SetCheck(IDC_GAZESHOW, g_s.gazeShow != 0);
    SetCheck(IDC_WITHVR, g_withVR);
    const bool temporal = g_s.mode == kVuModeTemporal;
    for (int id : { IDC_HIST, IDC_MOVING, IDC_TOL, IDC_SHARPHIST }) Enable(id, temporal);
    Enable(IDC_AREA, g_s.mode != kVuModeBilinear);
    {
        const wchar_t *what = temporal ? L"temporal detail" : L"FSR 1", *rest = g_s.bilinearEdges ? L"a bilinear upscale" : L"a fast spatial upscale";
        SetText(IDC_AREAHINT, g_s.mode == kVuModeBilinear ? std::wstring(L"Used in Temporal and FSR 1 mode.")
                              : g_s.gazeFollow ? Fmt(L"Following your gaze (its own value): where you look gets %s, the rest %s.", what, rest)
                                               : Fmt(L"Centre gets %s, edges %s.", what, rest));
    }
    UpdateValueLabels();
    UpdateGazeUi();
    g_ui = false;
    SyncListChecks();
}

void Save()
{
    if (SaveSettings(g_settingsPath, g_s))
        SetText(IDC_SAVED, L"");   // (only a failure is shown)
    else
        SetText(IDC_SAVED, Fmt(L"Could not save the settings to %s", g_settingsPath.c_str()));
}

void Push()
{
    g_pushPending = false;
    if (g_ch.IsOpen() && g_live) g_sentSeq = g_ch.SendSettings(g_s);
}

// A user change: live values are sent at most ten times a second while a slider moves; anything
// that re-plans textures (size, exclusions) is sent once, when the change is final.
// ---- VRChat's resolution (gameres.h), on a worker thread: SteamVR may take a moment.

int GameResTarget(float f) { return (int)lroundf(100.0f / (f * f)); }

std::wstring BeforeText(const app::GlobalRes &b) { return Fmt(L"%.4f %d", b.scale, b.manual ? 1 : 0); }

bool ParseBefore(const std::wstring &t, app::GlobalRes &b)
{
    int manual = 1;
    float sc = 0;
    if (t.empty() || swscanf(t.c_str(), L"%f %d", &sc, &manual) != 2 || !(sc > 0.05f && sc < 10.0f)) return false;
    b.scale = sc;
    b.manual = manual != 0;
    return true;
}

struct ResRequest { app::GlobalResJob job; float factor; };
struct ResDone { app::GlobalResResult r; float factor; };

// The worker keeps the settings file's record of what to put back itself, so it holds even if the
// app closes before the result arrives.
DWORD WINAPI ResThread(LPVOID p)
{
    ResRequest *q = (ResRequest *)p;
    ResDone *d = new ResDone{ app::SetGlobalResolution(app::kVrchatAppKey, q->job), q->factor };
    if (d->r.ok && q->job.restore) WriteAppString(L"globalResBefore", L"");
    else if (d->r.ok && !q->job.haveBase) WriteAppString(L"globalResBefore", BeforeText(d->r.base));
    if (!PostMessageW(g_wnd, WM_APP_GAMERES, 0, (LPARAM)d)) delete d;
    delete q;
    InterlockedDecrement(&g_resWorkers);
    return 0;
}

void StartRes(const app::GlobalResJob &job, float factor)
{
    g_resBusy = true;
    InterlockedIncrement(&g_resWorkers);
    HANDLE t = CreateThread(nullptr, 0, ResThread, new ResRequest{ job, factor }, 0, nullptr);
    if (t) CloseHandle(t);
    else {
        g_resBusy = false;
        InterlockedDecrement(&g_resWorkers);
    }
}

DWORD WINAPI MigrateThread(LPVOID p)
{
    app::GameResJob job;
    job.value = (int)(intptr_t)p;
    app::GameResResult r = app::SetGameResolution(app::kVrchatAppKey, job);
    if (r.ok) WriteAppString(L"gameResolutionBefore", L"");
    return 0;
}

// Once a second (and at once after a change): while VRChat runs in SteamVR, SteamVR's global
// resolution is the user's one lowered by the preset's share (Native: left alone); otherwise what
// the user had is put back.
void GameResTick(bool now)
{
    if (!now && GetTickCount() - g_resCheck < 1000) return;
    g_resCheck = GetTickCount();
    if (g_resBusy) return;   // the running request ends with another tick
    // (SteamVR asked the app to quit: its resolution was put back, and stays so while it closes)
    const bool steamvr = (IsSteamVRRunning() || _wgetenv(L"VRUPSCALE_TEST_STEAMVR_RUNNING") != nullptr) && !g_vrQuitting;
    g_resVrchat = app::VRChatRunning();
    app::GlobalRes base;
    const bool applied = ParseBefore(ReadAppString(L"globalResBefore"), base);
    const bool upscaling = g_s.upscaleFactor >= 1.005f;   // (at Native the user's resolution stays as it is)
    const bool dll = g_live || g_dllOurs;
    if (g_gameRes && steamvr && g_resVrchat && upscaling && dll) {
        if (!applied || fabsf(g_resApplied - g_s.upscaleFactor) > 0.004f) {
            if (!now && g_resRetry && GetTickCount() - g_resRetry < 10000) return;   // failed a moment ago
            app::GlobalResJob job;
            job.target = 1.0f / (g_s.upscaleFactor * g_s.upscaleFactor);
            job.haveBase = applied;
            job.base = base;
            g_gameResStatus = Fmt(L"Lowering SteamVR's resolution so VRChat renders %d%%…", GameResTarget(g_s.upscaleFactor));
            // the DLL looks for the new picture size quickly for a few seconds (again when SteamVR took it)
            g_ch.SendCommand(kVuCmdResolutionChanging);
            StartRes(job, g_s.upscaleFactor);
        }
    } else if (applied) {
        if (!now && g_resRetry && GetTickCount() - g_resRetry < 10000) return;
        app::GlobalResJob job;
        job.restore = true;
        job.base = base;
        if (steamvr) g_ch.SendCommand(kVuCmdResolutionChanging);
        StartRes(job, 0);
    } else if (!g_resMigrated && !g_resVrchat) {
        // 0.4.7 and 0.4.8 changed VRChat's per-application value instead (SteamVR applies that one
        // only when VRChat starts): put back what it was, while VRChat isn't running
        g_resMigrated = true;
        const std::wstring old = ReadAppString(L"gameResolutionBefore");
        if (!old.empty()) {
            const int v = old == L"none" ? app::kResUnset : _wtoi(old.c_str());
            HANDLE t = CreateThread(nullptr, 0, MigrateThread, (LPVOID)(intptr_t)v, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    UpdateValueLabels();
}

void OnResDone(ResDone *d)
{
    g_resBusy = false;
    const app::GlobalResResult &r = d->r;
    if (r.ok && r.viaApi) g_ch.SendCommand(kVuCmdResolutionChanging);   // SteamVR has it: the game follows now
    if (r.job.restore) {
        if (r.ok) {
            g_resApplied = 0;
            g_resRetry = 0;
            g_gameResStatus.clear();
        } else {
            g_gameResStatus = L"Couldn't put SteamVR's resolution back: " + r.error + L".";
            g_resRetry = GetTickCount();
        }
    } else if (r.ok) {
        g_resApplied = d->factor;
        g_resRetry = 0;
        g_gameResStatus = Fmt(L"Upscales %d%% → 100%% per axis. VRChat renders %d%% of its resolution: SteamVR is at %d%% while "
                              L"it runs (yours: %d%%).", (int)lroundf(100.0f / d->factor), GameResTarget(d->factor),
                              (int)lroundf(r.applied * 100.0f), (int)lroundf((r.base.manual ? r.base.scale : 1.0f) * 100.0f));
    } else {
        g_gameResStatus = L"Couldn't lower SteamVR's resolution for VRChat: " + r.error + L".";
        g_resRetry = GetTickCount();
    }
    delete d;
    GameResTick(true);   // anything that changed meanwhile (the preset, VRChat closing)
    Invalidate();
}

// After any change to the factor
void SyncGameRes()
{
    if (g_gameRes && fabsf(g_s.upscaleFactor - g_resApplied) > 0.004f) GameResTick(true);
}

// The app closes: SteamVR gets its own resolution back (waits for a request in flight first).
void RestoreGameResOnExit()
{
    for (int i = 0; i < 100 && g_resWorkers > 0; ++i) Sleep(50);
    app::GlobalRes base;
    if (!ParseBefore(ReadAppString(L"globalResBefore"), base)) return;
    app::GlobalResJob job;
    job.restore = true;
    job.base = base;
    if (app::SetGlobalResolution(app::kVrchatAppKey, job).ok) WriteAppString(L"globalResBefore", L"");
}

void Changed(bool now)
{
    g_lastEdit = GetTickCount();
    if (now) Push();
    else g_pushPending = true;
    SetTimer(g_wnd, TIMER_SAVE, 600, nullptr);
    SyncGameRes();
}

// ---- texture list -----------------------------------------------------------------------------

const VuTexInfo *RowTex(const Row &r) { return (g_live && r.tex >= 0 && r.tex < (int)kVuMaxTex) ? &g_st.tex[r.tex] : nullptr; }

void SyncListChecks() { Invalidate(); }   // the rows' ticks follow the model when drawn

std::wstring Ago(uint32_t now, uint32_t then)
{
    uint32_t d = now - then;
    if (d < 2500) return L"now";
    if (d < 60000) return Fmt(L"%us ago", d / 1000);
    if (d < 3600000) return Fmt(L"%um ago", d / 60000);
    return L"earlier";
}

bool SameRow(const Row &a, const Row &b)
{
    return a.wh == b.wh && a.tex == b.tex && a.size == b.size && a.format == b.format && a.result == b.result && a.seen == b.seen &&
           a.eye == b.eye && a.on == b.on && a.canChange == b.canChange;
}

// Textures seen in the last minute, plus every unticked size (so it can be ticked again even when
// the game does not use it right now). A texture SteamVR stopped drawing drops off the list.
void RefreshList()
{
    std::vector<Row> rows;
    if (g_live) {
        for (uint32_t k = 0; k < g_st.texCount && k < kVuMaxTex; ++k) {
            const VuTexInfo &t = g_st.tex[k];
            const uint32_t wh = (t.w << 16) | t.h;
            if (g_st.tick - t.lastSeenTick > 60000 && !IsExcluded(wh)) continue;
            Row r;
            r.wh = wh;
            r.tex = (int)k;
            rows.push_back(r);
        }
    }
    for (int i = 0; i < g_s.excludeCount && i < (int)kVuMaxExclude; ++i) {
        bool listed = false;
        for (auto &r : rows) listed = listed || r.wh == g_s.exclude[i];
        if (listed) continue;
        Row r;
        r.wh = g_s.exclude[i];
        rows.push_back(r);
    }
    for (Row &r : rows) {
        const VuTexInfo *t = RowTex(r);
        const uint32_t w = r.wh >> 16, h = r.wh & 0xffff;
        const bool excluded = IsExcluded(r.wh);
        if (t) {
            r.size = t->slices > 1 ? Fmt(L"%u×%u ×%u", w, h, t->slices) : Fmt(L"%u×%u", w, h);
            r.eye = t->eye != 0;
            r.format = FormatLabel(t->format);
            if (excluded) r.result = L"unticked: left alone";
            else if (!t->upscaled) r.result = L"left alone: " + Widen(t->reason);
            else if (t->contentW) r.result = Fmt(L"uses %u×%u → %u×%u", t->contentW, t->contentH, t->contentOutW, t->contentOutH);
            else if (t->outW == t->w && t->outH == t->h) r.result = L"processed at its own size (1×)";
            else r.result = Fmt(L"upscaled to %u×%u", t->outW, t->outH);
            r.seen = Ago(g_st.tick, t->lastSeenTick);
            // ticked: upscaled, or about to be (left alone only because it was unticked a moment ago)
            r.on = !excluded && (t->upscaled || strstr(t->reason, "unticked") != nullptr);
            r.canChange = excluded || r.on;
        } else {
            r.size = Fmt(L"%u×%u", w, h);
            r.result = L"unticked (not in use right now)";
            r.canChange = true;
        }
    }
    bool same = rows.size() == g_rows.size();
    for (size_t i = 0; same && i < rows.size(); ++i) same = SameRow(rows[i], g_rows[i]);
    if (!same) {
        g_rows = rows;
        Invalidate();
    }
    g_listScroll = std::max(0, std::min(g_listScroll, (int)g_rows.size() - kRowsShown));
    if (g_hoverRow >= (int)g_rows.size()) g_hoverRow = -1;
    // An eye texture whose size is still unticked (a workaround for the flicker of earlier versions):
    // vrupscale upscales it anyway, as one of the game's swapchain textures; drop the stale entry.
    for (auto &r : g_rows) {
        const VuTexInfo *t = RowTex(r);
        if (!t || !t->upscaled || !t->eye || !IsExcluded(r.wh) || g_pushPending || g_replanPending) continue;
        int n = 0;
        for (int i = 0; i < g_s.excludeCount; ++i)
            if (g_s.exclude[i] != r.wh) g_s.exclude[n++] = g_s.exclude[i];
        g_s.excludeCount = n;
        SetText(IDC_TEXHINT, Fmt(L"%u×%u is one of the game's eye textures, so it is upscaled with the others "
                                 L"(ticked again).", r.wh >> 16, r.wh & 0xffff));
        Changed(true);
    }
    Enable(IDC_TICKALL, g_s.excludeCount > 0);
}

void OnListCheck(int row, bool checked)
{
    if (row < 0 || row >= (int)g_rows.size()) return;
    const uint32_t wh = g_rows[row].wh;
    const VuTexInfo *info = RowTex(g_rows[row]);
    if (checked) {
        if (IsExcluded(wh)) {
            int n = 0;
            for (int i = 0; i < g_s.excludeCount; ++i)
                if (g_s.exclude[i] != wh) g_s.exclude[n++] = g_s.exclude[i];
            g_s.excludeCount = n;
            SetText(IDC_TEXHINT, Fmt(L"%u×%u will be upscaled again.", wh >> 16, wh & 0xffff));
            Changed(true);
        } else {
            SetText(IDC_TEXHINT, Fmt(L"%u×%u can't be turned on here: %s", wh >> 16, wh & 0xffff,
                                     info ? Widen(info->reason).c_str() : L"it does not look like an eye texture"));
            PostMessageW(g_wnd, WM_APP_SYNCLIST, 0, 0);
        }
    } else if (!IsExcluded(wh)) {
        if (info && !info->upscaled) {   // nothing to turn off
            PostMessageW(g_wnd, WM_APP_SYNCLIST, 0, 0);
            return;
        }
        if (info && info->eye) {
            // the game shows its frames from several such textures in turn: leaving one alone flickers
            SetText(IDC_TEXHINT, Fmt(L"%u×%u is one of the game's eye textures; they stay upscaled together. "
                                     L"Switch upscaling off at the top of Upscaling instead.", wh >> 16, wh & 0xffff));
            PostMessageW(g_wnd, WM_APP_SYNCLIST, 0, 0);
            return;
        }
        if (g_s.excludeCount < (int32_t)kVuMaxExclude) g_s.exclude[g_s.excludeCount++] = wh;
        SetText(IDC_TEXHINT, Fmt(L"%u×%u is no longer upscaled (tick it to undo).", wh >> 16, wh & 0xffff));
        Changed(true);
    }
}

// ---- status -----------------------------------------------------------------------------------

void RefreshEvents()
{
    std::vector<std::wstring> lines;
    if (g_live) {
        uint32_t n = g_st.eventCount < kVuEvents ? g_st.eventCount : kVuEvents;
        for (uint32_t i = g_st.eventCount - n; i < g_st.eventCount; ++i) {
            std::wstring line = Widen(g_st.events[i % kVuEvents]);
            // "HH:MM:SS.mmm [INFO] [t123] message" -> "HH:MM:SS  message"
            size_t b1 = line.find(L"] ");
            size_t b2 = b1 == std::wstring::npos ? b1 : line.find(L"] ", b1 + 2);
            if (line.size() > 8 && b2 != std::wstring::npos) {
                const bool err = line.find(L"[ERR") != std::wstring::npos;
                line = line.substr(0, 8) + (err ? L"  ERROR " : L"  ") + line.substr(b2 + 2);
            }
            lines.push_back(line);
        }
    }
    if (lines != g_events) {
        g_events = lines;
        Invalidate();
    }
}

void SetMood(ui::Mood m)
{
    if (m != g_mood) {
        g_mood = m;
        g_bgDirty = true;
        Invalidate();
    }
}

void SetStatusColor(COLORREF c)
{
    if (c != g_statusColor) {
        g_statusColor = c;
        Invalidate();
    }
}

// After an update by the Update button, SteamVR's copy (its dxgi.dll) is updated once SteamVR is
// closed. [app] updateDll=1 while that is pending.
enum class DllUpd { None, Waiting, Done, Failed };
DllUpd FinishDllUpdate(const std::wstring &root, std::wstring &msg)
{
    if (ReadAppString(L"updateDll") != L"1") return DllUpd::None;
    const bool found = IsSteamVRRoot(root);
    const DllInfo payload = GetDllInfo(PayloadPath());
    const DllInfo cur = found ? GetDllInfo(InstalledDllPath(root)) : DllInfo();
    if (!found || !cur.present || !cur.ours || !payload.present || cur.version == payload.version) {
        WriteAppString(L"updateDll", L"");
        return DllUpd::None;
    }
    const std::wstring waiting = Fmt(L"SteamVR's copy is updated to %s as soon as SteamVR closes.", payload.version.c_str());
    if (IsSteamVRRunning()) {
        msg = waiting;
        return DllUpd::Waiting;
    }
    const InstallResult r = Install(root, false, msg);
    if (r == InstallResult::Ok) {
        WriteAppString(L"updateDll", L"");
        msg = Fmt(L"Updated SteamVR's copy to %s.", payload.version.c_str());
        return DllUpd::Done;
    }
    if (r == InstallResult::SteamVRRunning) {   // (closing just now: next time)
        msg = waiting;
        return DllUpd::Waiting;
    }
    WriteAppString(L"updateDll", L"");
    msg = r == InstallResult::AccessDenied ? std::wstring(L"Click Update to finish: Windows needs administrator rights for the SteamVR folder.")
                                           : L"Click Update to finish: " + msg;
    return DllUpd::Failed;
}

const wchar_t *kDllHelperMutex = L"Local\\vrupscale.dllupdate";

bool DllUpdateHelperRunning()
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kDllHelperMutex);
    if (m) CloseHandle(m);
    return m != nullptr;
}

void RefreshInstall(bool force)
{
    DWORD now = GetTickCount();
    if (!force && now - g_lastInstallCheck < 2000) return;
    g_lastInstallCheck = now;
    if (!IsSteamVRRoot(g_root)) g_root = FindSteamVR();
    const bool found = IsSteamVRRoot(g_root);
    SetText(IDC_STEAMPATH, found ? g_root : std::wstring(L"SteamVR was not found: choose its folder."));
    DllInfo payload = GetDllInfo(PayloadPath());
    DllInfo cur = found ? GetDllInfo(InstalledDllPath(g_root)) : DllInfo();
    const bool hasBackup = found && GetFileAttributesW((InstalledDllPath(g_root) + L".vrupscale-backup").c_str()) !=
                                        INVALID_FILE_ATTRIBUTES;
    // After an update by the Update button, SteamVR's copy follows as soon as SteamVR is closed
    // (unless a helper this app started on closing with SteamVR is doing that right now).
    if (!DllUpdateHelperRunning()) {
        std::wstring m;
        const DllUpd u = FinishDllUpdate(g_root, m);
        if (u != DllUpd::None) SetText(IDC_INSTALLMSG, m);
        if (u == DllUpd::Done) cur = GetDllInfo(InstalledDllPath(g_root));
    }
    g_dllOurs = found && cur.present && cur.ours;
    std::wstring state, button = L"Install";
    if (!found) state = L"";
    else if (!cur.present) state = L"vrupscale is not installed in SteamVR.";
    else if (!cur.ours) state = Fmt(L"SteamVR has a dxgi.dll from another tool (%s).", cur.product.empty() ? L"unknown" : cur.product.c_str());
    else if (cur.version == payload.version) {
        state = Fmt(L"Installed: vrupscale %s (this version).", cur.version.c_str());
        button = L"Reinstall";
    } else {
        state = Fmt(L"Installed: vrupscale %s. This app brings %s.", cur.version.c_str(), payload.version.c_str());
        button = L"Update";
    }
    if (!payload.present) state += L" (payload\\dxgi.dll is missing next to the app)";
    SetText(IDC_INSTALLSTATE, state);
    SetText(IDC_INSTALL, button);
    if (ui::Widget *w = W(IDC_INSTALL)) w->style = button == L"Reinstall" ? ui::BtnOutline : ui::BtnPrimary;
    Enable(IDC_INSTALL, found && payload.present);
    Enable(IDC_UNINSTALL, found && ((cur.present && cur.ours) || hasBackup));
}

struct ProjResult {
    uint32_t pid = 0;
    bool ok = false;
    float p[8] = {};
    std::wstring error;
};

DWORD WINAPI ProjThread(LPVOID pid)
{
    ProjResult *r = new ProjResult();
    r->pid = (uint32_t)(uintptr_t)pid;
    r->ok = app::GetEyeProjection(r->p, r->error);
    if (!PostMessageW(g_wnd, WM_APP_PROJ, 0, (LPARAM)r)) delete r;
    return 0;
}

void OnProjection(ProjResult *r)
{
    if (r->ok) {
        g_projPid = r->pid;
        bool same = true;
        for (int i = 0; i < 8; ++i) same = same && fabsf(r->p[i] - g_s.eyeProj[i]) < 0.0001f;
        if (!same) {
            memcpy(g_s.eyeProj, r->p, sizeof(r->p));
            Changed(true);
        }
    }   // else: asked again in a while
    delete r;
}

// The hero: a short headline (like a hardware page's) and a sentence or two under it.
void RefreshStatus()
{
    if (!g_ch.IsOpen()) g_ch.Open();
    g_live = g_ch.IsOpen() && g_ch.ReadStatus(g_st) && StatusIsLive(g_st);
    if (g_live && g_pushOnConnect) {
        g_pushOnConnect = false;
        g_lastEdit = GetTickCount();
        Push();
    }
    std::wstring status, detail;
    COLORREF color = col::inkFaint;
    ui::Mood mood = ui::MoodSleep;
    if (!g_live) {
        const bool running = IsSteamVRRunning();
        const DllInfo cur = IsSteamVRRoot(g_root) ? GetDllInfo(InstalledDllPath(g_root)) : DllInfo();
        if (g_ch.LayoutMismatch()) {
            color = col::orange;
            mood = ui::MoodWarn;
            status = L"Restart SteamVR.";
            detail = L"SteamVR runs a different version of vrupscale. Restart it so it loads the one installed now.";
        } else if (running) {
            color = col::orange;
            mood = ui::MoodWarn;
            status = cur.ours ? L"Restart SteamVR." : L"Install vrupscale.";
            detail = cur.ours ? L"SteamVR is running without vrupscale. It is installed now: restart SteamVR to load it. "
                                L"(Versions before 0.3 can't talk to this app.)"
                              : L"SteamVR is running without vrupscale. Install it below, then restart SteamVR.";
        } else {
            status = cur.ours ? L"Ready when SteamVR is." : L"Install vrupscale.";
            detail = cur.ours ? L"SteamVR is not running. vrupscale is installed, and your settings apply when it starts."
                              : L"SteamVR is not running. Install vrupscale below, then start SteamVR.";
        }
        if (g_ch.IsOpen() && !g_live) g_ch.Close();   // reopen fresh when SteamVR comes back
    } else {
        color = col::blue;
        mood = ui::MoodAwake;
        status = L"Connected to SteamVR.";
        const std::wstring ver = Widen(g_st.version);
        int n = 0;
        std::wstring sizes;
        for (uint32_t k = 0; k < g_st.texCount && k < kVuMaxTex; ++k) {
            const VuTexInfo &t = g_st.tex[k];
            if (t.upscaled && g_st.tick - t.lastSeenTick < 3000) {
                ++n;
                // what the game renders: part of a larger texture after SteamVR's resolution was lowered
                const uint32_t iw = t.contentW ? t.contentW : t.w, ih = t.contentW ? t.contentH : t.h;
                const uint32_t ow = t.contentW ? t.contentOutW : t.outW, oh = t.contentW ? t.contentOutH : t.outH;
                const std::wstring one = ow == iw && oh == ih ? Fmt(L"%u×%u at 1×", iw, ih) : Fmt(L"%u×%u → %u×%u", iw, ih, ow, oh);
                if (sizes.find(one) == std::wstring::npos) sizes += (sizes.empty() ? L"" : L", ") + one;
            }
        }
        float rate = 0;
        for (uint32_t i = 0; i < g_st.streamCount && i < kVuMaxStreams; ++i) rate = std::max(rate, g_st.streams[i].runsPerSec);
        const wchar_t *mode = g_st.current.mode == kVuModeFsr ? L"FSR 1" : (g_st.current.mode == kVuModeBilinear ? L"bilinear" : L"temporal");
        if (!g_st.hooksInstalled) {
            color = col::red;
            mood = ui::MoodWarn;
            status = L"Not hooked in.";
            detail = Fmt(L"vrupscale %s is loaded in SteamVR, but its D3D11 hooks are not installed. Open the log for details.", ver.c_str());
        } else if (!g_st.current.enabled) {
            color = col::orange;
            status = L"Upscaling is off.";
            detail = Fmt(L"Connected to SteamVR (vrupscale %s). Switch it on under Upscaling.", ver.c_str());
        } else if (n == 0) {
            bool any = false;
            for (uint32_t k = 0; k < g_st.texCount && k < kVuMaxTex; ++k)
                if (g_st.tick - g_st.tex[k].lastSeenTick < 3000 && g_st.tex[k].w >= 1000 && g_st.tex[k].h >= 1000) any = true;
            detail = any ? Fmt(L"vrupscale %s. Nothing is being upscaled right now: the texture list says why.", ver.c_str())
                         : Fmt(L"vrupscale %s is waiting for a VR app's frames: start VRChat (or any VR app).", ver.c_str());
        } else if (g_st.current.mode == kVuModeTemporal && g_st.mixedEyes > 0) {
            // an eye's textures hold pictures of different sizes (a resolution change mid-game)
            color = col::orange;
            status = L"Temporal paused.";
            detail = Fmt(L"Upscaling %s with FSR 1 for now: the game's textures differ in size. If that lasts, restart the game.",
                         sizes.c_str());
        } else {
            // GPU cost per displayed frame: GPU time per second over the compositor's frame rate (each
            // upscaled texture is checked once per compositor frame), both eyes together.
            mood = ui::MoodWork;
            const bool temporal = g_st.current.mode == kVuModeTemporal && rate > 0;
            const float dispFps = n > 0 ? g_st.upscalesPerSec / (float)n : 0.0f;
            std::wstring gpu;
            if (g_st.gpuMsPerSec >= 0 && dispFps >= 1.0f)
                gpu = Fmt(L"GPU %.2f ms per displayed frame (%.1f%%)", g_st.gpuMsPerSec / dispFps, g_st.gpuMsPerSec / 10.0f);
            else if (g_st.gpuMsPerSec >= 0)
                gpu = Fmt(L"GPU %.1f%% busy upscaling", g_st.gpuMsPerSec / 10.0f);
            else
                gpu = L"GPU time: no frames timed in the last seconds";
            detail = Fmt(L"Upscaling %s, %s.\nvrupscale %s · ", sizes.c_str(), mode, ver.c_str());
            if (temporal) detail += Fmt(L"%.0f new frames/s · ", rate);
            detail += gpu;
        }
        // Mirror changes made in VR (keyboard shortcuts) once our own changes have landed.
        if ((g_sentSeq == 0 || (int32_t)g_st.appliedSeq == g_sentSeq) && GetTickCount() - g_lastEdit > 1500 &&
            !g_pushPending && !g_replanPending && !SameSettings(g_st.current, g_s)) {
            VuSettings m = g_st.current;
            if (m.sharpArea <= 0.0f) {   // a DLL from before 0.6.4 (an update waiting for SteamVR to close)
                m.sharpArea = g_s.sharpArea;
                m.gazeSharpArea = g_s.gazeSharpArea;
            }
            g_s = m;
            ModelToUi();
            SetTimer(g_wnd, TIMER_SAVE, 600, nullptr);
            SyncGameRes();   // Ctrl+F5 changes VRChat's resolution too
        }
    }
    GameResTick(false);
    // each eye's projection, once per SteamVR session (for placing the gaze in each eye's picture)
    if (g_live && g_st.pid != g_projPid && GetTickCount() - g_projTry > 15000) {
        g_projTry = GetTickCount();
        HANDLE t = CreateThread(nullptr, 0, ProjThread, (LPVOID)(uintptr_t)g_st.pid, 0, nullptr);
        if (t) CloseHandle(t);
    }
    UpdateGazeUi();
    SetText(IDC_STATUS, status);
    SetText(IDC_DETAIL, detail);
    SetStatusColor(color);
    SetMood(mood);
    RefreshList();
    RefreshEvents();
}

// ---- actions ----------------------------------------------------------------------------------

std::wstring SelfPath()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

// Runs this exe elevated with the given arguments and waits; returns its exit code (or -1).
int RunElevated(const std::wstring &args)
{
    std::wstring self = SelfPath();
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_wnd;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

void OnWithVrToggle(bool on);

void DoInstall(bool uninstall)
{
    if (!IsSteamVRRoot(g_root)) return;
    std::wstring msg;
    InstallResult r;
    bool force = false;
    if (!uninstall && MigrateLegacySettings(g_root)) {
        LoadSettings(g_settingsPath, g_s);
        ModelToUi();
    }
    for (;;) {
        r = uninstall ? Uninstall(g_root, msg) : Install(g_root, force, msg);
        if (r == InstallResult::ForeignDll && !uninstall && !force) {
            if (MessageBoxW(g_wnd, (msg + L"\n\nBack it up and replace it with vrupscale? Uninstalling vrupscale later puts it back.").c_str(),
                            L"vrupscale", MB_YESNO | MB_ICONWARNING) != IDYES)
                return;
            force = true;
            continue;
        }
        break;
    }
    if (r == InstallResult::AccessDenied) {
        if (MessageBoxW(g_wnd, (msg + L"\n\nWindows needs administrator rights to change the SteamVR folder. Continue?").c_str(),
                        L"vrupscale", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return;
        std::wstring args = Fmt(L"%s%s --steamvr \"%s\" --settings \"%s\"", uninstall ? L"--uninstall" : L"--install",
                                force ? L" --force" : L"", g_root.c_str(), g_settingsPath.c_str());
        int code = RunElevated(args);
        msg = code == 0 ? (uninstall ? L"Removed vrupscale from SteamVR." : L"Installed vrupscale into SteamVR. Start SteamVR to use it.")
                        : Fmt(L"That did not work (code %d).", code);
        r = code == 0 ? InstallResult::Ok : InstallResult::Failed;
    }
    if (uninstall && r == InstallResult::Ok && g_withVR) {   // gone from SteamVR: the app leaves its startup apps too
        SetCheck(IDC_WITHVR, false);
        OnWithVrToggle(false);
        msg += L" Start with SteamVR is off now.";
    }
    SetText(IDC_INSTALLMSG, msg);
    if (r != InstallResult::Ok && r != InstallResult::ForeignDll)
        MessageBoxW(g_wnd, msg.c_str(), L"vrupscale", MB_OK | MB_ICONWARNING);
    RefreshInstall(true);
    RefreshStatus();
}

// ---- starting and closing with SteamVR (vrlink.h) -------------------------------------------------

struct AutoLaunchDone {
    app::AutoLaunchJob job;
    app::AutoLaunchResult r;
};

// The link thread: while SteamVR runs, the lasting connection (if wanted) and SteamVR's quit request;
// once per SteamVR session, and after the switch moved, SteamVR's startup entry.
DWORD WINAPI LinkThread(LPVOID)
{
    DWORD lastOpen = 0, lastSync = 0;
    bool synced = false, quitSeen = false;
    while (!g_linkStop) {
        WaitForSingleObject(g_linkWake, 500);
        if (g_linkStop) break;
        if (!IsSteamVRRunning()) {
            if (app::VrLinkIsOpen()) app::VrLinkClose();
            synced = quitSeen = false;
            continue;
        }
        if (quitSeen) continue;   // SteamVR is closing: nothing more until it has
        const bool want = g_linkWant != 0;
        if (want && !app::VrLinkIsOpen() && GetTickCount() - lastOpen > 3000) {
            lastOpen = GetTickCount();
            std::wstring err;
            app::VrLinkOpen(err);   // (fails while SteamVR is starting: tried again)
        }
        if (app::VrLinkIsOpen() && app::VrLinkPollQuit()) {
            quitSeen = true;   // the window puts the resolution back through the link, then closes it
            if (!PostMessageW(g_wnd, WM_APP_VRQUIT, 0, 0)) app::VrLinkClose();
            continue;
        }
        if (!want && app::VrLinkIsOpen()) app::VrLinkClose();
        if (InterlockedExchange(&g_linkSync, 0)) synced = false;
        if (!synced && !want && !g_linkApplied) synced = true;   // off, and SteamVR was told so (or switched it off itself)
        if (!synced && GetTickCount() - lastSync > 3000) {
            lastSync = GetTickCount();
            AutoLaunchDone *d = new AutoLaunchDone();
            d->job.want = want;
            d->job.applied = g_linkApplied != 0;
            d->r = app::SyncAutoLaunch(d->job);
            if (d->r.notReady) {   // SteamVR is starting: again in a moment
                delete d;
                continue;
            }
            synced = true;
            if (!PostMessageW(g_wnd, WM_APP_AUTOLAUNCH, 0, (LPARAM)d)) delete d;
        }
    }
    app::VrLinkClose();
    return 0;
}

void StartLink()
{
    const std::wstring applied = ReadAppString(L"withSteamVRApplied");
    if (g_fromVR && !g_withVR && applied == L"0") {   // switched back on in SteamVR's own list since
        g_withVR = true;
        WriteAppString(L"withSteamVR", L"1");
        SetCheck(IDC_WITHVR, true);
        UpdateValueLabels();
    }
    g_linkWant = g_withVR ? 1 : 0;
    g_linkApplied = applied == L"1" ? 1 : 0;
    g_linkWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_linkThread = CreateThread(nullptr, 0, LinkThread, nullptr, 0, nullptr);
}

void StopLink()
{
    if (!g_linkThread) return;
    g_linkStop = 1;
    SetEvent(g_linkWake);
    WaitForSingleObject(g_linkThread, 5000);
    CloseHandle(g_linkThread);
    g_linkThread = nullptr;
}

void OnAutoLaunch(AutoLaunchDone *d)
{
    const app::AutoLaunchResult &r = d->r;
    const bool current = d->job.want == (g_linkWant != 0);   // (the switch didn't move meanwhile)
    if (r.ok) {
        g_linkApplied = r.want ? 1 : 0;
        WriteAppString(L"withSteamVRApplied", r.want ? L"1" : L"0");
    }
    if (current) {
        std::wstring msg;
        if (r.ok && r.userOff) {   // switched off in SteamVR's own list since: the app follows
            g_withVR = false;
            g_linkWant = 0;
            WriteAppString(L"withSteamVR", L"0");
            SetCheck(IDC_WITHVR, false);
            UpdateValueLabels();
            msg = L"Start with SteamVR is off: it was switched off in SteamVR's startup overlay apps.";
        } else if (r.ok && r.registered) {
            msg = L"SteamVR starts this app from now on, and the app closes with SteamVR.";
        } else if (!r.ok) {
            msg = (d->job.want ? L"Couldn't add vrupscale to SteamVR's startup apps: " : L"Couldn't take vrupscale off SteamVR's startup apps: ") +
                  r.error + L".";
        }
        if (!msg.empty()) SetText(IDC_INSTALLMSG, msg);
    }
    delete d;
    Invalidate();
}

// Started by the app as it closes with SteamVR: SteamVR's copy (pending after an update) is replaced
// once SteamVR is gone, from a process of its own (SteamVR may end what it started when it closes).
void StartDllUpdateHelper()
{
    std::wstring cmd = L"\"" + SelfPath() + L"\" --finish-dll-update --settings \"" + g_settingsPath + L"\"";
    if (IsSteamVRRoot(g_root)) cmd += L" --steamvr \"" + g_root + L"\"";
    for (DWORD flags : { (DWORD)CREATE_BREAKAWAY_FROM_JOB, (DWORD)0 }) {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::wstring line = cmd;
        if (CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
}

// SteamVR is closing (it asked) or has closed: so does the app.
void CloseWithSteamVR()
{
    if (g_closingWithVR) return;
    g_closingWithVR = true;
    if (ReadAppString(L"updateDll") == L"1" && !DllUpdateHelperRunning()) {
        if (IsSteamVRRunning()) StartDllUpdateHelper();
        else RefreshInstall(true);   // SteamVR is gone already: now
    }
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

// SteamVR asked the app to quit (it is closing): its resolution goes back while SteamVR still answers.
void OnSteamVrQuit()
{
    g_vrQuitting = true;
    RestoreGameResOnExit();
    g_resApplied = 0;
    app::VrLinkClose();
    if (g_withVR) CloseWithSteamVR();
    else UpdateValueLabels();
}

// Twice a second: SteamVR gone (without asking, or not connected) closes the app too, once it ran
// during this run.
void WithVrTick()
{
    const DWORD now = GetTickCount();
    if (now - g_vrCheck < 500) return;
    g_vrCheck = now;
    if (IsSteamVRRunning()) {
        if (!g_vrQuitting) g_vrSeen = true;
        g_vrGoneAt = 0;
        return;
    }
    g_vrQuitting = false;
    if (!g_vrGoneAt) g_vrGoneAt = now | 1;
    if (g_withVR && g_vrSeen && now - g_vrGoneAt >= 1500) CloseWithSteamVR();
}

void OnWithVrToggle(bool on)
{
    g_withVR = on;
    g_linkWant = on ? 1 : 0;
    g_linkSync = 1;
    SetEvent(g_linkWake);
    WriteAppString(L"withSteamVR", on ? L"1" : L"0");
    UpdateValueLabels();
    if (!IsSteamVRRunning())
        SetText(IDC_INSTALLMSG, on ? L"SteamVR starts this app once it has run while the app is open."
                                   : L"Takes effect the next time SteamVR runs while this app is open.");
    else
        SetText(IDC_INSTALLMSG, on ? L"SteamVR starts this app from now on, and the app closes with SteamVR."
                                   : L"SteamVR no longer starts this app, and it stays open when SteamVR closes.");
}

// ---- updates ------------------------------------------------------------------------------------

float VersionPillRight()   // where the version pill by the name ends (DrawBackground draws it)
{
    return 36 + ui::Measure(ui::F_BRAND, L"vrupscale") + 10 + ui::Measure(ui::F_TINY, Widen(VU_APP_VERSION)) + 16;
}

// The Change log button sits by the version pill, the Update button (when there is one) after it.
RectF ChangeLogButtonRect() { return { VersionPillRight() + 12, 26, ui::Measure(ui::F_BUTTON, L"Change log") + 34, 29 }; }

void OpenChangeLog(bool bringForward = true)
{
    app::ChangeLogInfo info;
    info.current = Widen(VU_APP_VERSION);
    info.updatedFrom = g_updatedFrom;
    if (g_updState != UpdNone) {
        info.updVersion = g_upd.version;
        info.updNotes = g_upd.notes;
    }
    app::ShowChangeLog(g_wnd, g_inst, g_dpi, info, bringForward);
}

void RefreshUpdateUi()
{
    if (ui::Widget *cl = W(IDC_CHANGELOG)) cl->r = ChangeLogButtonRect();
    ui::Widget *w = W(IDC_UPDATE);
    if (!w) return;
    std::wstring t;
    bool en = true;
    switch (g_updState) {
    case UpdNone: break;
    case UpdAvailable: t = L"Update to " + g_upd.version; break;
    case UpdDownloading:
        t = g_updProgress >= 0 ? Fmt(L"Downloading… %d%%", (int)lroundf(g_updProgress * 100)) : std::wstring(L"Downloading…");
        en = false;
        break;
    case UpdFailed: t = L"Update failed: try again"; break;
    }
    const float bw = std::max(ui::Measure(ui::F_BUTTON, L"Downloading… 100%"), ui::Measure(ui::F_BUTTON, t)) + 34;
    w->r = { ChangeLogButtonRect().r() + 10, 26, bw, 29 };
    w->text = t;
    w->enabled = en;
    w->visible = g_updState != UpdNone;
    if (!w->visible && g_in.focus == IDC_UPDATE) g_in.focus = 0;
    Invalidate();
}

DWORD WINAPI UpdateCheckThread(LPVOID)
{
    app::UpdateInfo *u = new app::UpdateInfo(app::CheckForUpdate(Widen(VU_APP_VERSION)));
    if (!PostMessageW(g_wnd, WM_APP_UPDCHECK, 0, (LPARAM)u)) delete u;
    return 0;
}

// From the refresh timer: the first check a few seconds after start, then every 6 hours.
void UpdateTick()
{
    const DWORD now = GetTickCount();
    if (g_updChecking || g_updState == UpdDownloading || ReadAppString(L"checkUpdates") == L"0") return;
    if (g_updLastCheck ? now - g_updLastCheck < 6u * 3600u * 1000u : now - g_updStart < 4000) return;
    g_updChecking = true;
    g_updLastCheck = now;
    HANDLE t = CreateThread(nullptr, 0, UpdateCheckThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else g_updChecking = false;
}

void OnUpdateCheck(app::UpdateInfo *u)
{
    g_updChecking = false;
    g_updLastCheck = GetTickCount();
    if (u->ok && g_updState != UpdDownloading) {
        if (u->newer) {
            if (g_updState != UpdFailed || u->version != g_upd.version) g_updState = UpdAvailable;
            g_upd = *u;
        } else {
            g_updState = UpdNone;   // (a release can be withdrawn)
        }
        RefreshUpdateUi();
        if (app::ChangeLogOpen()) OpenChangeLog(false);   // (an available update's notes show there too)
    }
    delete u;
}

struct DownloadJob { app::UpdateInfo u; };

void DownloadProgress(uint64_t done, uint64_t total, void *)
{
    static int last = -1;
    const int pm = total ? (int)(done * 1000 / total) : -1;
    if (pm == last) return;
    last = pm;
    PostMessageW(g_wnd, WM_APP_UPDPROGRESS, (WPARAM)(pm + 1), 0);
}

DWORD WINAPI UpdateDownloadThread(LPVOID p)
{
    DownloadJob *j = (DownloadJob *)p;
    app::StagedUpdate *r = new app::StagedUpdate(app::DownloadUpdate(j->u, DownloadProgress, nullptr, &g_updCancel));
    delete j;
    if (!PostMessageW(g_wnd, WM_APP_UPDDONE, 0, (LPARAM)r)) delete r;
    return 0;
}

void StartUpdate()
{
    if (g_updState != UpdAvailable && g_updState != UpdFailed) return;
    g_updState = UpdDownloading;
    g_updProgress = -1;
    g_updCancel = 0;
    RefreshUpdateUi();
    HANDLE t = CreateThread(nullptr, 0, UpdateDownloadThread, new DownloadJob{ g_upd }, 0, nullptr);
    if (t) CloseHandle(t);
    else {
        g_updState = UpdFailed;
        g_updError = L"could not start the download";
        RefreshUpdateUi();
    }
}

// The new version is unpacked: put it in place and start it; this one closes.
void OnUpdateDone(app::StagedUpdate *r)
{
    std::wstring err = r->error;
    bool ok = r->ok && app::ApplyUpdate(r->dir, err);
    if (ok) {
        // SteamVR's copy of the DLL: the new app updates it as soon as SteamVR is closed
        if (IsSteamVRRoot(g_root) && GetDllInfo(InstalledDllPath(g_root)).ours) WriteAppString(L"updateDll", L"1");
        std::wstring cmd = L"\"" + SelfPath() + L"\" --updated-from " + Widen(VU_APP_VERSION);
        if (_wcsicmp(g_settingsPath.c_str(), DefaultSettingsPath().c_str()) != 0) cmd += L" --settings \"" + g_settingsPath + L"\"";
        if (g_channelName != VU_CONTROL_NAME) cmd += L" --channel \"" + g_channelName + L"\"";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            g_updRestarting = true;
            PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        } else {
            MessageBoxW(g_wnd, (L"vrupscale " + r->version + L" is installed. Start vrupscale again to use it.").c_str(), L"vrupscale",
                        MB_OK | MB_ICONINFORMATION);
            g_updRestarting = true;
            PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        }
    } else {
        g_updState = UpdFailed;
        g_updError = err;
        RefreshUpdateUi();
        MessageBoxW(g_wnd, (L"The update to " + g_upd.version + L" didn't work: " + err + L".\n\nNothing was changed. You can try again, or "
                            L"download it from the release page on GitHub.").c_str(),
                    L"vrupscale", MB_OK | MB_ICONWARNING);
    }
    delete r;
}

// The release's notes for a tip: Markdown marks dropped, at most a few lines.
std::wstring NotesForTip(const std::wstring &md)
{
    std::wstring out;
    int lines = 0;
    size_t a = 0;
    while (a < md.size() && lines < 12) {
        size_t e = md.find(L'\n', a);
        if (e == std::wstring::npos) e = md.size();
        std::wstring l = md.substr(a, e - a);
        a = e + 1;
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ')) l.pop_back();
        size_t k = 0;
        while (k < l.size() && (l[k] == L'#' || l[k] == L' ')) ++k;
        l = l.substr(k);
        if (l.empty()) continue;
        if (l.compare(0, 2, L"- ") == 0 || l.compare(0, 2, L"* ") == 0) l = L"• " + l.substr(2);
        std::wstring c;
        for (wchar_t ch : l)
            if (ch != L'*' && ch != L'`') c += ch;
        out += (out.empty() ? L"" : L"\n") + c;
        ++lines;
    }
    if (out.size() > 900) out = out.substr(0, 897) + L"…";
    return out;
}

void BrowseSteamVR()
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Choose the SteamVR folder (it contains bin\\win64\\vrcompositor.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH * 2] = L"";
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!IsSteamVRRoot(path)) {
        MessageBoxW(g_wnd, L"That folder does not contain bin\\win64\\vrcompositor.exe.", L"vrupscale", MB_OK | MB_ICONWARNING);
        return;
    }
    g_root = path;
    WriteAppString(L"steamvrPath", g_root);
    RefreshInstall(true);
}

void OpenPath(const std::wstring &p, bool select)
{
    if (select) {
        std::wstring args = L"/select,\"" + p + L"\"";
        ShellExecuteW(g_wnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_wnd, (L"There is no log yet:\n" + p + L"\n\nIt is created when SteamVR starts with vrupscale installed.").c_str(),
                    L"vrupscale", MB_OK | MB_ICONINFORMATION);
    } else {
        ShellExecuteW(g_wnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- window -----------------------------------------------------------------------------------

const wchar_t *const kModeCaption[3] = {
    L"Best quality: adds detail from earlier frames in the centre of your view.",
    L"Spatial only: each frame is upscaled and sharpened on its own. Cheaper.",
    L"A plain upscale, to compare the others against.",
};

void AddWidget(int id, ui::Kind k, const RectF &r, const wchar_t *text, int style = ui::BtnOutline, const wchar_t *note = L"")
{
    ui::Widget w;
    w.id = id;
    w.kind = k;
    w.r = r;
    w.text = text;
    w.style = style;
    w.note = note;
    g_w.push_back(w);
}

void AddSlider(int id, const RectF &r, const wchar_t *label, int lo, int hi, float labelW)
{
    ui::Widget w;
    w.id = id;
    w.kind = ui::Kind::Slider;
    w.r = r;
    w.text = label;
    w.lo = lo;
    w.hi = hi;
    w.pos = lo;
    w.labelW = labelW;
    g_w.push_back(w);
}

void CreateControls()
{
    using ui::Kind;
    const RectF U = kCardUp, T = kCardTemp, V = kCardVR, X = kCardTex, E = kCardEv;
    AddWidget(IDC_CHANGELOG, Kind::Button, ChangeLogButtonRect(), L"Change log");
    AddWidget(IDC_UPDATE, Kind::Button, { 0, 0, 0, 0 }, L"Update", ui::BtnPrimary);   // placed by RefreshUpdateUi
    W(IDC_UPDATE)->visible = false;
    AddWidget(IDC_ENABLED, Kind::Toggle, { U.r() - 64, U.y + 15, 44, 26 }, L"");
    {
        ui::Widget w;
        w.id = IDC_MODE;
        w.kind = Kind::Segment;
        w.r = { U.x + 20, U.y + 50, U.w - 40, 34 };
        w.items = { L"Temporal", L"FSR 1", L"Bilinear" };
        g_w.push_back(w);
    }
    {
        ui::Widget w;   // the scaling presets (kVuScaleFactor), by factor; the name shows above
        w.id = IDC_SCALE;
        w.kind = Kind::Segment;
        w.r = { U.x + 20, U.y + 158, U.w - 40, 34 };
        w.items = { L"1×", L"1.3×", L"1.5×", L"1.72×", L"2×" };
        g_w.push_back(w);
    }
    AddSlider(IDC_SHARP, { U.x + 20, U.y + 270, U.w - 40, 30 }, L"Sharpness", 0, 100, 128);
    AddSlider(IDC_SHARPAREA, { U.x + 20, U.y + 304, U.w - 40, 30 }, L"Sharpness area", 30, 100, 128);
    {
        ui::Widget w;   // FXAA: off, before the upscale, after it (the label is drawn beside it)
        w.id = IDC_FXAA;
        w.kind = Kind::Segment;
        w.r = { U.x + 148, U.y + 338, U.w - 168, 30 };
        w.items = { L"Off", L"Before", L"After" };
        g_w.push_back(w);
    }
    AddWidget(IDC_TINT, Kind::Toggle, { U.x + 20, U.y + 376, U.w - 40, 26 }, L"Debug tint", ui::BtnOutline,
              L"magenta = upscaled");
    AddWidget(IDC_HOTKEYS, Kind::Toggle, { U.x + 20, U.y + 406, U.w - 40, 26 }, L"Shortcuts in VR", ui::BtnOutline,
              L"Ctrl+F1 to F9");
    AddSlider(IDC_AREA, { T.x + 20, T.y + 50, T.w - 40, 30 }, L"Temporal area", 30, 100, 128);
    AddSlider(IDC_HIST, { T.x + 20, T.y + 122, T.w - 40, 30 }, L"History strength", 10, 200, 128);
    AddSlider(IDC_MOVING, { T.x + 20, T.y + 156, T.w - 40, 30 }, L"Moving objects", 0, 100, 128);
    AddSlider(IDC_TOL, { T.x + 20, T.y + 190, T.w - 40, 30 }, L"Motion tolerance", 30, 300, 128);
    AddWidget(IDC_SHARPHIST, Kind::Toggle, { T.x + 20, T.y + 228, T.w - 40, 26 }, L"Sharp history", ui::BtnOutline,
              L"slightly finer, more GPU");
    AddWidget(IDC_BILEDGES, Kind::Toggle, { T.x + 20, T.y + 258, T.w - 40, 26 }, L"Bilinear edges", ui::BtnOutline,
              L"cheaper, softer");
    const RectF Y = kCardEye;
    AddWidget(IDC_GAZEFOLLOW, Kind::Toggle, { Y.r() - 64, Y.y + 15, 44, 26 }, L"");
    AddWidget(IDC_GAZESHOW, Kind::Toggle, { Y.x + 20, Y.y + 92, Y.w - 40, 26 }, L"Show gaze", ui::BtnOutline, L"a green ring in VR");
    AddWidget(IDC_CALIBRATE, Kind::Button, { Y.x + 20, Y.y + 126, 112, 32 }, L"Calibrate", ui::BtnPrimary);
    AddWidget(IDC_CALCLEAR, Kind::Button, { Y.x + 142, Y.y + 126, 150, 32 }, L"Clear calibration");
    AddWidget(IDC_WITHVR, Kind::Toggle, { V.x + 20, V.y + 260, V.w - 40, 26 }, L"Start with SteamVR", ui::BtnOutline,
              L"and close with it");
    AddWidget(IDC_DEFAULTS, Kind::Button, { V.x + 20, V.y + 300, 150, 32 }, L"Restore defaults");
    AddWidget(IDC_OPENFOLDER, Kind::Button, { V.x + 180, V.y + 300, 120, 32 }, L"Settings file");
    AddWidget(IDC_BROWSE, Kind::Button, { V.r() - 140, V.y + 14, 120, 30 }, L"Choose folder…");
    AddWidget(IDC_INSTALL, Kind::Button, { V.x + 20, V.y + 112, 120, 34 }, L"Install", ui::BtnPrimary);
    AddWidget(IDC_UNINSTALL, Kind::Button, { V.x + 150, V.y + 112, 112, 34 }, L"Uninstall");
    AddWidget(IDC_TICKALL, Kind::Button, { X.r() - 106, X.y + 14, 86, 30 }, L"Tick all");
    AddWidget(IDC_TEXLIST, Kind::Custom, kList, L"");
    AddWidget(IDC_OPENLOG, Kind::Button, { E.r() - 112, E.y + 14, 92, 30 }, L"Open log");
    AddWidget(IDC_EVENTS, Kind::Custom, kLog, L"");
}

// ---- drawing ------------------------------------------------------------------------------------

struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    void Ensure(int W, int H)
    {
        if (dc && w == W && h == H) return;
        Free();
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old = SelectObject(dc, bmp);
        ReleaseDC(nullptr, screen);
        w = W;
        h = H;
    }
    void Free()
    {
        if (dc) {
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
        }
        dc = nullptr;
        bmp = nullptr;
        w = h = 0;
    }
};
Surface g_back, g_bg;

int ClientH()
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    return rc.bottom;
}

float MaxScroll() { return std::max(0.0f, kH - L(ClientH())); }

void SetScroll(float v)
{
    v = std::max(0.0f, std::min(MaxScroll(), v));
    if (v != g_scroll) {
        g_scroll = v;
        Invalidate();
    }
}

void ScrollIntoView(const RectF &r)
{
    const float view = L(ClientH());
    if (r.y - 10 < g_scroll) SetScroll(r.y - 10);
    else if (r.b() + 10 > g_scroll + view) SetScroll(r.b() + 10 - view);
}

// The parts that change rarely: sky, ground, characters, cards. Redrawn when the mood or size changes.
void DrawBackground(int W, int H)
{
    g_bg.Ensure(W, H);
    ui::Canvas c(g_bg.dc, Scale(), 0);
    const float pw = L(W);
    ui::DrawGround(c, pw, L(H));
    const float bw = c.Text(ui::F_BRAND, L"vrupscale", 36, 46, col::ink);
    const std::wstring ver = Widen(VU_APP_VERSION);
    const RectF pill = { 36 + bw + 10, 30.5f, ui::Measure(ui::F_TINY, ver) + 16, 20 };
    c.StrokeRound(pill.Inflate(-1), 9, col::ink, 2);
    c.Text(ui::F_TINY, ver, pill.cx(), ui::Mid(ui::F_TINY, pill.cy()), col::ink, 1);
    ui::DrawScene(c, kHeadsetX, pw, g_mood);
    ui::DrawCard(c, kCardUp, L"Upscaling");
    ui::DrawCard(c, kCardEye, L"Eye tracking");
    ui::DrawCard(c, kCardTemp, L"Temporal");
    ui::DrawCard(c, kCardVR, L"SteamVR");
    ui::DrawCard(c, kCardTex, L"Textures");
    ui::DrawCard(c, kCardEv, L"Recent events");
    ui::DrawWell(c, kList);
    ui::DrawWell(c, kLog);
    g_bgDirty = false;
}

std::wstring RowDetail(const Row &r)
{
    std::wstring s = r.size;
    if (r.eye) s += L" (one of the game's eye textures)";
    if (!r.format.empty()) s += L" · " + r.format;
    s += L" · " + r.result;
    if (!r.seen.empty()) s += L" · seen " + r.seen;
    return s;
}

void DrawList(ui::Canvas &c)
{
    const RectF &l = kList;
    if (g_rows.empty()) {
        c.Wrap(ui::F_SMALL, g_live ? L"No eye-sized textures yet: they show up once a VR app runs."
                                   : L"The textures SteamVR draws with show up here while it runs.",
               { l.x + 18, l.cy() - 17, l.w - 36, 34 }, 17, col::inkFaint, 2);
        return;
    }
    for (int v = 0; v < kRowsShown; ++v) {
        const int i = g_listScroll + v;
        if (i >= (int)g_rows.size()) break;
        const Row &r = g_rows[i];
        const float y = l.y + v * kRowH;
        if (i == g_hoverRow) c.FillRound({ l.x + 4, y + 3, l.w - 8, kRowH - 6 }, 9, col::paper, 170);
        else if (v > 0 && i - 1 != g_hoverRow) c.Line(l.x + 14, y, l.r() - 14, y, col::ink, 1, 30);
        const RectF box = { l.x + 12, y + 11, 18, 18 };
        const bool hot = g_in.hover == IDC_TEXLIST && g_in.hoverPart == i;
        ui::DrawCheck(c, box, r.on, r.canChange, hot);
        if (g_in.focusVisible && g_in.focus == IDC_TEXLIST && g_focusRow == i) ui::DrawFocus(c, box, 5);
        const float sx = l.x + 40;
        const COLORREF ink = r.canChange ? col::ink : col::inkSoft;
        const float sw = c.Text(ui::F_LABEL, r.size, sx, y + 18, ink, 0, l.w - 110);
        if (r.eye) {
            const RectF p = { sx + sw + 8, y + 6.5f, ui::Measure(ui::F_TINY, L"eye") + 14, 16 };
            c.FillRound(p, 8, col::blue);
            c.StrokeRound(p.Inflate(-0.75f), 7.25f, col::ink, 1.5f);
            c.Text(ui::F_TINY, L"eye", p.cx(), ui::Mid(ui::F_TINY, p.cy()), col::ink, 1);
        }
        c.Text(ui::F_SMALL, r.seen, l.r() - 12, y + 18, col::inkFaint, 2);
        c.Text(ui::F_SMALL, r.result, sx, y + 33, col::inkSoft, 0, l.r() - 12 - sx);
    }
    const int n = (int)g_rows.size();
    if (n > kRowsShown) {   // where the rows shown are among all of them
        const float th = l.h - 16, h = th * kRowsShown / n, y = l.y + 8 + (th - h) * g_listScroll / (n - kRowsShown);
        c.FillRound({ l.r() - 6, y, 3.5f, h }, 1.75f, col::ink, 90);
    }
}

void DrawLog(ui::Canvas &c)
{
    const RectF &l = kLog;
    if (g_events.empty()) {
        c.Wrap(ui::F_SMALL, L"Events from SteamVR's compositor show up here while it runs.", { l.x + 14, l.y + 8, l.w - 28, 34 }, 17,
               col::inkFaint, 2);
        return;
    }
    struct Vis { std::wstring s; bool first, err; };
    std::vector<Vis> vis;
    for (auto &e : g_events) {
        const bool err = e.find(L"  ERROR ") != std::wstring::npos;
        std::vector<std::wstring> ls = ui::WrapLines(ui::F_MONO, e, l.w - 24);
        for (size_t k = 0; k < ls.size(); ++k) vis.push_back({ ls[k], k == 0, err });
    }
    const int shown = (int)((l.h - 10) / kLogLineH), total = (int)vis.size();
    g_logScroll = std::max(0, std::min(g_logScroll, total - shown));
    const int first = std::max(0, total - shown - g_logScroll);
    for (int k = first; k < std::min(total, first + shown); ++k) {
        const Vis &v = vis[k];
        const float base = ui::Mid(ui::F_MONO, l.y + 5 + (k - first + 0.5f) * kLogLineH);
        float x = l.x + 12;
        std::wstring s = v.s;
        if (v.first && s.size() > 10 && s[2] == L':' && s[5] == L':') {   // the time, quieter
            x += c.Text(ui::F_MONO, s.substr(0, 10), x, base, col::inkFaint);
            s = s.substr(10);
        }
        c.Text(ui::F_MONO, s, x, base, v.err ? col::red : col::ink, 0, l.r() - 12 - x);
    }
    if (total > shown) {
        const float th = l.h - 16, h = std::max(12.0f, th * shown / total);
        const float y = l.y + 8 + (th - h) * (1.0f - (float)g_logScroll / (float)(total - shown));
        c.FillRound({ l.r() - 6, y, 3.5f, h }, 1.75f, col::ink, 90);
    }
}

// ---- tips: the longer description of a control, after the pointer rests on it a moment ---------

constexpr UINT kTipDelayMs = 650;      // first tip
constexpr UINT kTipReshowMs = 120;     // the next one, while moving from tip to tip
constexpr float kTipW = 316;

int g_tipId = 0, g_tipPart = -1;       // what the pointer rests on (part: a segment's item; -1 the whole)
bool g_tipShown = false;
int g_tipSuppress = 0;                 // clicked: no tip for it until the pointer leaves it
DWORD g_tipHiddenAt = 0;

// The control's tip, and its title; empty: none.
std::wstring TipText(int id, int part, std::wstring &title)
{
    static const wchar_t *const kModeTip[3][2] = {
        { L"Temporal",
          L"The best quality. vrupscale estimates the motion between frames itself (the game sends only colour) and keeps "
          L"the earlier frames' samples, so the centre of your view gets finer and steadier than any single frame. Costs the "
          L"most GPU time; the Temporal card tunes it. Ctrl+F2 in VR switches modes." },
        { L"FSR 1",
          L"AMD FidelityFX Super Resolution 1: each frame is upscaled and sharpened on its own (an edge-aware upscale, then "
          L"RCAS), inside the same area Temporal uses; the rest gets a fast spatial upscale. Nothing from earlier frames, "
          L"so nothing can ghost, but no added detail and more shimmer than Temporal. Cheaper." },
        { L"Bilinear", L"A plain bilinear upscale with no sharpening: to compare the other modes against." },
    };
    static const wchar_t *const kScaleTip[kVuScaleCount][2] = {
        { L"Native (1×)",
          L"No upscaling: the eye textures are processed at their own size. In Temporal mode that is anti-aliasing from "
          L"your head's micro-motion (like DLAA); FSR 1 only sharpens. VRChat renders at 100%." },
        { L"Ultra Quality (1.3×), the default",
          L"The output is 1.3 times the game's render size per axis: VRChat renders 77% per axis, 59% of the pixels SteamVR "
          L"would otherwise ask for." },
        { L"Quality (1.5×)", L"1.5 times per axis: VRChat renders 66.7% per axis, 44% of the pixels." },
        { L"Balanced (1.72×)", L"1.72 times per axis: VRChat renders 58% per axis, 34% of the pixels." },
        { L"Performance (2×)",
          L"Twice the size per axis: VRChat renders 50% per axis, a quarter of the pixels. The most GPU time saved in the "
          L"game, the least detail." },
    };
    switch (id) {
    case IDC_UPDATE: {
        title = g_upd.title.empty() ? L"vrupscale " + g_upd.version : g_upd.title;
        std::wstring t = NotesForTip(g_upd.notes);
        if (g_updState == UpdFailed && !g_updError.empty()) t = L"The last try failed: " + g_updError + L".\n\n" + t;
        const std::wstring size = g_upd.zipSize ? Fmt(L" (%.1f MB)", g_upd.zipSize / 1048576.0) : std::wstring();
        return t + (t.empty() ? L"" : L"\n\n") + L"Click to download it" + size +
               L" and install it: the app restarts in the new version. SteamVR's copy is updated at once if SteamVR is closed, "
               L"otherwise as soon as it is. Your settings stay.";
    }
    case IDC_CHANGELOG:
        title = L"Change log";
        return L"What changed in each version of vrupscale, newest first, in a window of its own (the same as CHANGELOG.md "
               L"next to the app)." +
               std::wstring(!g_updatedFrom.empty() ? L" The versions since " + g_updatedFrom + L", which you just updated from, are marked new."
                            : L"") +
               std::wstring(g_updState != UpdNone ? L" The available update's notes come first." : L"");
    case IDC_ENABLED:
        title = L"Upscaling on or off";
        return L"Turns vrupscale on or off inside SteamVR, at once. Off, SteamVR shows what the game renders as it is (at "
               L"its lower resolution, if it renders lower). Ctrl+F1 in VR.";
    case IDC_MODE:
        if (part >= 0 && part < 3) {
            title = kModeTip[part][0];
            return kModeTip[part][1];
        }
        title = L"Mode";
        return L"How the game's frames are upscaled: Temporal, FSR 1 or Bilinear. Ctrl+F2 in VR switches.";
    case IDC_SCALE: {
        std::wstring tail = L"\n\nChanging the preset rebuilds vrupscale's textures (a brief hitch). Ctrl+F5 in VR picks the next one.";
        if (part >= 0 && part < kVuScaleCount) {
            title = kScaleTip[part][0];
            return std::wstring(kScaleTip[part][1]) + tail;
        }
        title = L"Scaling";
        return L"How much larger the output is than what the game renders, per axis, after DLSS's presets.\n\nSet the resolution "
               L"you want to see in SteamVR as usual (VRChat's per-application video setting, or the global one). While VRChat "
               L"runs and this app is open, the app lowers SteamVR's global resolution to the preset's share, so VRChat renders "
               L"that share and vrupscale upscales it back to your resolution. Your global value is put back when VRChat or "
               L"the app closes." + tail;
    }
    case IDC_SHARP:
        title = L"Sharpness";
        return L"RCAS sharpening (from FSR 1) after upscaling, over the Sharpness area, in Temporal and FSR 1 mode. 1.00 is the "
               L"most RCAS does; 0 turns it off. Default 1.00. Ctrl+F4 in VR steps through 0, 0.25, 0.5, 0.75 and 1.";
    case IDC_SHARPAREA:
        title = g_s.gazeFollow ? L"Sharpness area, following your gaze" : L"Sharpness area";
        return L"The width and height of the part of each eye's picture that is sharpened, centred on the Temporal area (and "
               L"moving with it when it follows your gaze). It is never smaller than the Temporal area: at that size, only the "
               L"area is sharpened, as before. Larger also sharpens the fast spatial upscale around the area, fading out at the "
               L"edge, for a little GPU time; all sharpens everything.\n\nLike the area, it has two values: 60% by default, "
               L"and 40% while Follow gaze is on. Raising the Temporal area past it raises it too. Greyed out in Bilinear "
               L"mode, with Sharpness off, or when the Temporal area is all.";
    case IDC_TINT:
        title = L"Debug tint";
        return L"Tints everything vrupscale processes: magenta where it upscales, and in Temporal and FSR 1 mode blue on "
               L"the edges outside the area, which get the fast spatial upscale. Use it to check that only the world is upscaled, not SteamVR's menus or "
               L"overlays (untick those in the Textures list). Ctrl+F3 in VR.";
    case IDC_HOTKEYS:
        title = L"Shortcuts in VR";
        return L"Function-key shortcuts that work while you are in VR: Ctrl+F1 on/off, F2 mode, F3 debug tint, F4 sharpness, "
               L"F5 next scaling preset, F6 Sharp history, F7 Bilinear edges, F8 calibrate the eye tracking, F9 FXAA (off, "
               L"before, after). The app's controls follow along and save the change.";
    case IDC_FXAA:
        title = L"FXAA";
        return L"FXAA anti-aliasing, before the upscale (on the game's picture, so the upscale works from it) or after it (on "
               L"the upscaled picture), in every mode. It smooths stair-stepped edges and costs some fine detail. In testing it "
               L"made Temporal softer either way; it steadied a game without anti-aliasing of its own, most in FSR 1 mode. "
               L"Default off. Ctrl+F9 in VR steps through off, before and after.";
    case IDC_AREA:
        title = g_s.gazeFollow ? L"Temporal area, following your gaze" : L"Temporal area";
        return L"The width and height of the part of each eye's picture that gets the full treatment: history and motion "
               L"estimation in Temporal mode, FSR 1 in FSR 1 mode. The rest gets a fast spatial upscale, and a soft band "
               L"blends the two. The main cost control: smaller is cheaper; all turns this off.\n\nIt has two values: "
               L"60% by default, centred (about a third of the pixels, the lens's sharp centre), and 40% while Follow gaze "
               L"is on, since the area then sits wherever you look. The slider shows and sets the one for the Follow gaze "
               L"switch's state. Changing it rebuilds the histories (a brief hitch). The Sharpness area is never smaller: "
               L"raising this past it raises it too.";
    case IDC_HIST:
        title = L"History strength";
        return L"How much the earlier frames may count. Each output pixel mixes this frame with the history, and the "
               L"history may weigh as much as this many new samples landing right on the pixel. At 1, a pixel a new sample "
               L"lands on squarely is half new, half history; pixels between samples lean on history, which is where the "
               L"added detail comes from.\n\nHigher: steadier, but softer and more ghosting. Lower: sharper, quicker to "
               L"react, more shimmer. Default 1.00; on the test scene steadiness peaks around 1.";
    case IDC_MOVING:
        title = L"Moving objects";
        return L"How far history is trusted on things that move differently from your head: avatars, hands, near objects "
               L"while you walk. Their own motion is searched for and checked first.\n\n1: they get temporal smoothing too, "
               L"with a little smearing on fast movement. 0: they get the spatial upscale, with no smearing and no temporal "
               L"gain there. Default 1.00.";
    case IDC_TOL:
        title = L"Motion tolerance";
        return L"How wrong the motion may look before history is dropped. vrupscale compares this frame with the previous "
               L"one moved by the motion it found, relative to the local contrast: that comes out around 0.3 to 0.6 where the "
               L"motion is right and about 1.4 where it is not. History counts fully below half this value and not at all "
               L"at it.\n\nHigher: history survives doubtful motion (more ghosting). Lower: dropped sooner (shimmer comes "
               L"back). Default 1.50.";
    case IDC_SHARPHIST:
        title = L"Sharp history";
        return L"Resamples the history with a Lanczos-3 filter instead of Catmull-Rom. The history is resampled every frame, "
               L"and Catmull-Rom's slight softening adds up; Lanczos keeps more of the finest detail (on the test scene "
               L"+1.4 dB on patterns finer than the game renders), though in the headset it is hard to see. Costs GPU time: "
               L"36 texture reads per pixel instead of 5. Default off. Ctrl+F6 in VR.";
    case IDC_BILEDGES:
        title = L"Bilinear edges";
        return L"Outside the area (Temporal and FSR 1 mode), a plain bilinear upscale instead of Catmull-Rom: softer at the "
               L"edges, where the lenses blur the most anyway, and cheaper. The centre is untouched, and the blend band still hides the border. "
               L"Greyed out when Temporal area is all (there are no edges). Default off. Ctrl+F7 in VR.";
    case IDC_GAZEFOLLOW:
        title = L"Follow gaze";
        return L"Bigscreen Beyond 2e eye tracking: the area (temporal detail, or FSR 1) moves to where you look, so the "
               L"sharp part of the picture is wherever your eyes are. It moves once your gaze leaves a small zone around the "
               L"area's centre (2% of the width); in Temporal mode the strip it uncovers catches up within a few frames. "
               L"While following, the area has its own size, 40% by default (the Temporal area slider sets it). Needs "
               L"Bigscreen's eye tracking client with its VRCFT output on.";
    case IDC_GAZESHOW:
        title = L"Show gaze";
        return L"Draws a green ring in VR where vrupscale thinks you look. Use it to check the calibration: the ring should "
               L"sit on whatever you look at.";
    case IDC_CALIBRATE:
        title = L"Calibrate";
        return L"A cross appears straight ahead in both eyes: look at its centre until it goes (about 3 seconds). The "
               L"tracker's reading then becomes the offset taken off every later reading, so that straight ahead lines up. "
               L"If your gaze wandered it fails and keeps the old calibration. Needs Temporal or FSR 1 mode and live eye tracking. "
               L"Ctrl+F8 in VR.";
    case IDC_CALCLEAR:
        title = L"Clear calibration";
        return L"Removes the calibration offset: the tracker's own reading is used as it is.";
    case IDC_WITHVR:
        title = L"Start with SteamVR";
        return L"SteamVR starts this app when it starts (minimized), and the app closes when SteamVR does, putting SteamVR's "
               L"resolution back first. So VRChat's resolution is handled every time you play without opening the app.\n\n"
               L"The app adds itself to SteamVR's startup overlay apps (SteamVR settings, Startup / Shutdown) the next time "
               L"SteamVR runs while it is open. Switching it off there switches it off here too. Default on.";
    case IDC_DEFAULTS:
        title = L"Restore defaults";
        return L"Puts every setting back to its default. Your texture choices, the eye tracking calibration and SteamVR's "
               L"field of view are kept.";
    case IDC_OPENFOLDER:
        title = L"Settings file";
        return L"Opens the folder with vrupscale.ini. Settings are saved there as you change them, and apply live.";
    case IDC_INSTALL:
        title = L"Install / Update";
        return L"Puts vrupscale (its dxgi.dll) into SteamVR's bin\\win64 folder, or updates it there. SteamVR must be "
               L"closed. Another tool's dxgi.dll there is kept aside and put back by Uninstall.";
    case IDC_UNINSTALL:
        title = L"Uninstall";
        return L"Removes vrupscale from SteamVR, and puts back another tool's dxgi.dll if Install had to set one aside "
               L"(after asking). SteamVR must be closed.";
    case IDC_TICKALL:
        title = L"Tick all";
        return L"Ticks every texture size again, so whatever looks like an eye texture is upscaled. Unticked sizes stay "
               L"in the list, even when not in use, until then.";
    }
    return L"";
}

// Where the pointer rests for a tip (disabled controls too: a tip says why one is greyed out).
bool TipAt(float x, float y, int &id, int &part, RectF &anchor)
{
    for (auto &w : g_w) {
        RectF r = w.r;
        int pt = -1;
        if (w.kind == ui::Kind::Custom || !w.visible) continue;
        if (w.kind == ui::Kind::Toggle && w.text.empty()) {   // a card's title switch: with its label on the left
            const float lw = ui::Measure(ui::F_SMALL, w.id == IDC_GAZEFOLLOW ? L"Follow gaze" : L"Off");
            r = { r.x - 10 - lw, r.y, r.w + 10 + lw, r.h };
        }
        if (w.id == IDC_SCALE) r = { r.x, r.y - 30, r.w, r.h + 30 };   // with "Scaling" and the preset's name above
        if (!r.Contains(x, y)) continue;
        if (w.kind == ui::Kind::Segment && w.r.Contains(x, y)) pt = ui::SegmentAt(w, x);
        std::wstring t;
        if (TipText(w.id, pt, t).empty()) return false;
        id = w.id;
        part = pt;
        anchor = r;
        return true;
    }
    return false;
}

void HideTip()
{
    KillTimer(g_wnd, TIMER_TIP);
    if (g_tipShown) {
        g_tipShown = false;
        g_tipHiddenAt = GetTickCount();
        Invalidate();
    }
}

// The pointer moved (page coordinates): a new target starts the delay, leaving one hides its tip.
void TipTrack(float x, float y)
{
    int id = 0, part = -1;
    RectF a;
    if (g_in.drag || !TipAt(x, y, id, part, a)) id = 0, part = -1;
    if (id != g_tipSuppress) g_tipSuppress = 0;
    if (id == g_tipId && part == g_tipPart) return;
    const bool wasShown = g_tipShown;
    HideTip();
    g_tipId = id;
    g_tipPart = part;
    if (id && id != g_tipSuppress) {
        const bool quick = wasShown || GetTickCount() - g_tipHiddenAt < 400;
        SetTimer(g_wnd, TIMER_TIP, quick ? kTipReshowMs : kTipDelayMs, nullptr);
    }
}

void TipForget()   // a click, a key, a scroll: no tip until the pointer moves to something else
{
    HideTip();
    g_tipSuppress = g_tipId;
}

void DrawTip(ui::Canvas &c)
{
    if (!g_tipShown || !g_tipId) return;
    int id = 0, part = -1;
    RectF a;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g_wnd, &pt);
    if (!TipAt(L(pt.x), L(pt.y) + g_scroll, id, part, a) || id != g_tipId || part != g_tipPart) return;
    std::wstring title;
    const std::wstring body = TipText(id, part, title);
    const float pad = 14, lineH = 17, titleH = title.empty() ? 0 : 24;
    const std::vector<std::wstring> lines = ui::WrapLines(ui::F_SMALL, body, kTipW - 2 * pad);
    const float h = pad + titleH + lines.size() * lineH + pad;
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const float top = g_scroll + 6, bottom = g_scroll + L(rc.bottom) - 6, right = std::min(kW, L(rc.right)) - 8;
    float x = std::max(8.0f, std::min(a.x, right - kTipW));
    float y = a.b() + 8;
    if (y + h > bottom) y = a.y - 8 - h;   // no room below: above
    y = std::max(top, std::min(y, bottom - h));
    const RectF box = { x, y, kTipW, h };
    c.FillRound({ box.x + 1, box.y + 4, box.w, box.h }, 12, col::ink, 45);
    c.FillRound(box, 12, col::paper);
    c.StrokeRound(box.Inflate(-1), 11, col::ink, 2);
    float base = box.y + pad;
    if (!title.empty()) {
        c.Text(ui::F_LABEL, title, box.x + pad, ui::Mid(ui::F_LABEL, base + 8), col::ink, 0, kTipW - 2 * pad);
        base += titleH;
    }
    c.Wrap(ui::F_SMALL, body, { box.x + pad, base, kTipW - 2 * pad, h }, lineH, col::ink);
}

void DrawPage(ui::Canvas &c)
{
    if (g_updState == UpdNone && !g_updatedFrom.empty())
        c.Text(ui::F_SMALL, L"Updated from " + g_updatedFrom, ChangeLogButtonRect().r() + 12, ui::Mid(ui::F_SMALL, 40.5f), col::inkSoft);
    // hero: a headline and a sentence or two, like a hardware page
    c.FillCircle(45, 89, 9, g_statusColor);
    c.StrokeCircle(45, 89, 9, col::ink, 2.5f);
    c.Text(ui::F_DISPLAY, TextOf(IDC_STATUS), 65, 105, col::ink, 0, 660);
    c.Wrap(ui::F_BODY, TextOf(IDC_DETAIL), { 36, 117, 680, 44 }, 21, col::ink, 2);

    static const int vals[][2] = { { IDC_SHARP, IDC_SHARPVAL }, { IDC_SHARPAREA, IDC_SHARPAREAVAL }, { IDC_AREA, IDC_AREAVAL },
                                   { IDC_HIST, IDC_HISTVAL }, { IDC_MOVING, IDC_MOVINGVAL }, { IDC_TOL, IDC_TOLVAL } };
    for (auto &v : vals)
        if (ui::Widget *w = W(v[0])) w->value = TextOf(v[1]);
    for (auto &w : g_w) ui::DrawWidget(c, w, g_in);

    // Upscaling
    const RectF U = kCardUp;
    const ui::Widget *en = W(IDC_ENABLED);
    c.Text(ui::F_SMALL, en && en->on ? L"On" : L"Off", en->r.x - 10, ui::Mid(ui::F_SMALL, en->r.cy()), col::inkSoft, 2);
    c.Wrap(ui::F_SMALL, kModeCaption[std::max(0, std::min(2, (int)g_s.mode))], { U.x + 22, U.y + 90, U.w - 44, 34 }, 17, col::inkSoft, 2);
    c.Text(ui::F_LABEL, L"Scaling", U.x + 20, ui::Mid(ui::F_LABEL, U.y + 143), col::ink);
    c.Text(ui::F_LABEL, L"FXAA", U.x + 20, ui::Mid(ui::F_LABEL, U.y + 353), col::ink);
    c.Text(ui::F_VALUE, TextOf(IDC_FACTORVAL), U.r() - 20, ui::Mid(ui::F_VALUE, U.y + 143), col::ink, 2);
    c.Wrap(ui::F_SMALL, TextOf(IDC_FACTORHINT), { U.x + 22, U.y + 198, U.w - 44, 51 }, 17, col::inkSoft, 3);
    // Eye tracking
    const RectF Y = kCardEye;
    const ui::Widget *gf = W(IDC_GAZEFOLLOW);
    c.Text(ui::F_SMALL, L"Follow gaze", gf->r.x - 10, ui::Mid(ui::F_SMALL, gf->r.cy()), col::inkSoft, 2);
    c.Wrap(ui::F_SMALL, TextOf(IDC_GAZESTATUS), { Y.x + 20, Y.y + 52, Y.w - 40, 34 }, 17, col::inkSoft, 2);
    c.Wrap(ui::F_SMALL, TextOf(IDC_CALSTATUS), { Y.x + 20, Y.y + 166, Y.w - 40, 34 }, 17, col::inkSoft, 2);

    // Temporal
    const RectF T = kCardTemp;
    c.Wrap(ui::F_SMALL, TextOf(IDC_AREAHINT), { T.x + 20, T.y + 84, T.w - 40, 34 }, 17, col::inkSoft, 2);

    // SteamVR
    const RectF V = kCardVR;
    c.PathText(ui::F_SMALL, TextOf(IDC_STEAMPATH), V.x + 20, V.y + 60, V.w - 40, col::inkSoft);
    c.Wrap(ui::F_LABEL, TextOf(IDC_INSTALLSTATE), { V.x + 20, V.y + 68, V.w - 40, 40 }, 19, col::ink, 2);
    const std::wstring saveError = TextOf(IDC_SAVED);   // (settings that couldn't be saved come first)
    c.Wrap(ui::F_SMALL, saveError.empty() ? TextOf(IDC_INSTALLMSG) : saveError, { V.x + 20, V.y + 154, V.w - 40, 84 }, 16,
           saveError.empty() ? col::inkSoft : col::red, 3);
    c.Fill({ V.x + 20, V.y + 248, V.w - 40, 1.5f }, col::inkFaint);

    // Textures
    const RectF X = kCardTex;
    DrawList(c);
    const bool rowHint = g_hoverRow >= 0 && g_hoverRow < (int)g_rows.size();
    c.Wrap(ui::F_SMALL, rowHint ? RowDetail(g_rows[g_hoverRow]) : TextOf(IDC_TEXHINT), { X.x + 20, X.y + 218, X.w - 40, 36 }, 17,
           rowHint ? col::ink : col::inkSoft, 2);

    // Events
    DrawLog(c);

    // the page's scroll position, when the window is shorter than the page
    const float view = L(ClientH());
    if (MaxScroll() > 0) {
        const float th = view - 16, h = std::max(30.0f, th * view / kH), y = g_scroll + 8 + (th - h) * g_scroll / MaxScroll();
        c.FillRound({ kW - 9, y, 5, h }, 2.5f, col::ink, 110);
    }

    DrawTip(c);   // over everything
}

void Paint(HDC target)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    const int bgW = std::max((int)rc.right, S(kW)), bgH = std::max((int)rc.bottom, S(kH));
    if (g_bgDirty || g_bg.w != bgW || g_bg.h != bgH) DrawBackground(bgW, bgH);
    g_back.Ensure(rc.right, rc.bottom);
    BitBlt(g_back.dc, 0, 0, rc.right, rc.bottom, g_bg.dc, 0, S(g_scroll), SRCCOPY);
    {
        ui::Canvas c(g_back.dc, Scale(), g_scroll);
        DrawPage(c);
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, g_back.dc, 0, 0, SRCCOPY);
}

// ---- interaction --------------------------------------------------------------------------------

struct Hit {
    int id = 0;
    int part = -1;      // segment: item; list: row
    bool click = false; // something happens on a click here
};

Hit HitTest(float x, float y)
{
    for (auto &w : g_w) {
        if (!w.visible) continue;
        switch (w.kind) {
        case ui::Kind::Button:
            if (w.enabled && w.r.Contains(x, y)) return { w.id, -1, true };
            break;
        case ui::Kind::Toggle: {
            RectF r = ui::SwitchRect(w);
            if (!w.text.empty()) r.w += 11 + ui::Measure(ui::F_LABEL, w.text) + 4;
            r.y = std::min(r.y, w.r.y);
            r.h = std::max(r.h, w.r.h);
            if (w.enabled && r.Contains(x, y)) return { w.id, -1, true };
            break;
        }
        case ui::Kind::Slider: {
            const RectF t = ui::SliderTrack(w);
            if (w.enabled && RectF{ t.x - 12, w.r.y, t.w + 24, w.r.h }.Contains(x, y)) return { w.id, -1, true };
            break;
        }
        case ui::Kind::Segment:
            if (w.enabled && w.r.Contains(x, y)) return { w.id, ui::SegmentAt(w, x), true };
            break;
        case ui::Kind::Custom:
            if (w.id == IDC_TEXLIST && w.r.Contains(x, y)) {
                const int i = g_listScroll + (int)((y - kList.y) / kRowH);
                if (i >= 0 && i < (int)g_rows.size()) return { IDC_TEXLIST, i, x < kList.x + 36 };
                return { IDC_TEXLIST, -1, false };
            }
            break;
        }
    }
    return {};
}

bool Focusable(const ui::Widget &w)
{
    if (!w.visible) return false;
    if (w.kind != ui::Kind::Custom) return w.enabled;
    if (w.id == IDC_TEXLIST) return !g_rows.empty();
    return false;
}

void FocusWidget(int id, bool visible)
{
    g_in.focus = id;
    g_in.focusVisible = visible;
    if (GetFocus() != g_wnd) SetFocus(g_wnd);
    if (id == IDC_TEXLIST) g_focusRow = std::max(0, std::min(g_focusRow, (int)g_rows.size() - 1));
    if (const ui::Widget *w = W(id)) ScrollIntoView(w->r);
    Invalidate();
}

void MoveFocus(int dir)
{
    std::vector<int> order;
    for (auto &w : g_w)
        if (Focusable(w)) order.push_back(w.id);
    if (order.empty()) return;
    const int n = (int)order.size();
    int at = -1;
    for (int i = 0; i < n; ++i)
        if (order[i] == g_in.focus) at = i;
    const int next = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
    FocusWidget(order[next], true);
}

void OnSlider(int id, bool tracking)
{
    UpdateValueLabels();
    if (id == IDC_SHARP) {
        g_s.sharpness = Pos(IDC_SHARP) / 100.0f;
        Changed(false);
    } else if (id == IDC_SHARPAREA) {
        // never below the area (the knob stops there); the value for the current Follow gaze state
        if (Pos(IDC_SHARPAREA) < Pos(IDC_AREA)) {
            SetPos(IDC_SHARPAREA, Pos(IDC_AREA));
            UpdateValueLabels();
        }
        ActiveSharpArea() = Pos(IDC_SHARPAREA) / 100.0f;
        Changed(false);
    } else if (id == IDC_AREA) {
        // A new area re-plans the histories: sent once the slider is let go and has settled
        // (keys move it in steps). It is the value for the current Follow gaze state.
        if (tracking) {
            const int kept = (int)lroundf(std::max(ActiveSharpArea(), ActiveArea()) * 100);
            SetPos(IDC_SHARPAREA, std::max(kept, Pos(IDC_AREA)));   // (pushed up meanwhile)
            UpdateValueLabels();
            return;
        }
        float a = Pos(IDC_AREA) / 100.0f;
        float &area = g_s.gazeFollow ? g_s.gazeArea : g_s.temporalArea;
        if (fabsf(a - area) > 0.004f) {
            area = a;
            ActiveSharpArea() = std::max(ActiveSharpArea(), a);   // the Sharpness area holds the area
            g_lastEdit = GetTickCount();
            g_replanPending = true;
            SetTimer(g_wnd, TIMER_REPLAN, 400, nullptr);
        }
    } else if (id == IDC_HIST) {
        g_s.maxWeight = Pos(IDC_HIST) / 100.0f;
        Changed(false);
    } else if (id == IDC_MOVING) {
        g_s.localTrust = Pos(IDC_MOVING) / 100.0f;
        Changed(false);
    } else if (id == IDC_TOL) {
        g_s.residualTol = Pos(IDC_TOL) / 100.0f;
        Changed(false);
    }
}

void DragTo(float x)
{
    ui::Widget *w = W(g_in.drag);
    if (!w) return;
    const int p = ui::SliderPosAt(*w, x);
    if (p != w->pos) {
        w->pos = p;
        Invalidate();
        OnSlider(w->id, true);
    }
}

void OnMode(int sel)
{
    if (sel < 0 || sel > 2 || sel == g_s.mode) return;
    g_s.mode = sel;
    ModelToUi();
    Changed(true);
}

// A scaling preset (index into kVuScaleFactor). A new size re-plans every texture, once.
void OnScale(int preset)
{
    if (preset < 0 || preset >= kVuScaleCount || VuScalePreset(g_s.upscaleFactor) == preset) return;
    g_s.upscaleFactor = kVuScaleFactor[preset];
    ModelToUi();
    Changed(true);
}

void OnSegment(int id, int part)
{
    if (id == IDC_MODE) OnMode(part);
    else if (id == IDC_SCALE) OnScale(part);
    else if (id == IDC_FXAA && part >= 0 && part <= 2 && part != g_s.fxaa) {
        g_s.fxaa = part;
        ModelToUi();
        Changed(true);
    }
}

// Arrow keys on a segment control: one step, no wrapping. The scaling presets step from a
// factor between them (a hand edit) to the neighbouring one.
void OnSegmentKey(const ui::Widget &w, int dir)
{
    if (w.id == IDC_SCALE) {
        const float f = g_s.upscaleFactor;
        const int cur = VuScalePreset(f);
        if (dir < 0) OnScale(VuScalePrev(f));
        else OnScale(cur >= 0 ? (cur + 1 < kVuScaleCount ? cur + 1 : -1) : VuScaleNext(f));
    } else {
        OnSegment(w.id, w.sel + dir);
    }
}

void OnToggle(int id)
{
    ui::Widget *w = W(id);
    if (!w || !w->enabled) return;
    w->on = !w->on;
    Invalidate();
    switch (id) {
    case IDC_ENABLED: g_s.enabled = w->on; Changed(true); break;
    case IDC_TINT: g_s.tint = w->on; Changed(true); break;
    case IDC_HOTKEYS: g_s.hotkeys = w->on; Changed(true); break;
    case IDC_SHARPHIST: g_s.fastHistory = w->on ? 0 : 1; Changed(true); break;
    case IDC_BILEDGES: g_s.bilinearEdges = w->on; ModelToUi(); Changed(true); break;
    case IDC_GAZEFOLLOW: g_s.gazeFollow = w->on; ModelToUi(); Changed(true); break;   // (the area shows its value for this state)
    case IDC_GAZESHOW: g_s.gazeShow = w->on; Changed(true); break;
    case IDC_WITHVR: OnWithVrToggle(w->on); break;
    }
}

void OnButton(int id)
{
    switch (id) {
    case IDC_TICKALL:
        if (g_s.excludeCount > 0) {
            SetText(IDC_TEXHINT, Fmt(L"%d unticked size%s will be upscaled again.", g_s.excludeCount, g_s.excludeCount == 1 ? L"" : L"s"));
            g_s.excludeCount = 0;
            memset(g_s.exclude, 0, sizeof(g_s.exclude));
            Changed(true);
            RefreshList();
        }
        break;
    case IDC_DEFAULTS: {
        VuSettings d = DefaultSettings();
        memcpy(d.exclude, g_s.exclude, sizeof(d.exclude));   // keep the texture choices
        d.excludeCount = g_s.excludeCount;
        memcpy(d.gazeOffset, g_s.gazeOffset, sizeof(d.gazeOffset));   // and what was measured: the calibration
        memcpy(d.eyeProj, g_s.eyeProj, sizeof(d.eyeProj));            // and SteamVR's projection
        g_s = d;
        ModelToUi();
        Changed(true);
        break;
    }
    case IDC_INSTALL: DoInstall(false); break;
    case IDC_UPDATE: StartUpdate(); break;
    case IDC_CHANGELOG: OpenChangeLog(); break;
    case IDC_UNINSTALL:
        if (MessageBoxW(g_wnd, L"Remove vrupscale from SteamVR? Your settings are kept.", L"vrupscale", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            DoInstall(true);
        break;
    case IDC_BROWSE: BrowseSteamVR(); break;
    case IDC_OPENLOG: OpenPath(g_live && g_st.logPath[0] ? std::wstring(g_st.logPath) : DefaultLogPath(), false); break;
    case IDC_OPENFOLDER: OpenPath(g_settingsPath, true); break;
    case IDC_CALIBRATE:
        if (g_live) {
            g_ch.SendCommand(kVuCmdCalibrateGaze);
            g_calMsgHidden = false;
        }
        break;
    case IDC_CALCLEAR:
        g_s.gazeOffset[0] = g_s.gazeOffset[1] = 0.0f;
        g_calMsgHidden = true;
        ModelToUi();
        Changed(true);
        break;
    }
}

void OnListToggle(int row)
{
    if (row < 0 || row >= (int)g_rows.size()) return;
    g_focusRow = row;
    OnListCheck(row, !g_rows[row].on);
    RefreshList();
    Invalidate();
}

bool OnKey(UINT vk)
{
    if (vk == VK_TAB) {
        MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        return true;
    }
    ui::Widget *w = W(g_in.focus);
    if (!w || !Focusable(*w)) return false;
    g_in.focusVisible = true;
    Invalidate();
    const bool act = vk == VK_SPACE || vk == VK_RETURN;
    switch (w->kind) {
    case ui::Kind::Button:
        if (act) OnButton(w->id);
        return act;
    case ui::Kind::Toggle:
        if (act) OnToggle(w->id);
        return act;
    case ui::Kind::Segment:
        if (vk == VK_LEFT || vk == VK_UP) OnSegmentKey(*w, -1);
        else if (vk == VK_RIGHT || vk == VK_DOWN) OnSegmentKey(*w, 1);
        else return false;
        return true;
    case ui::Kind::Slider: {
        int p = w->pos;
        switch (vk) {
        case VK_LEFT: case VK_DOWN: p -= 1; break;
        case VK_RIGHT: case VK_UP: p += 1; break;
        case VK_PRIOR: p += w->page; break;
        case VK_NEXT: p -= w->page; break;
        case VK_HOME: p = w->lo; break;
        case VK_END: p = w->hi; break;
        default: return false;
        }
        p = std::max(w->lo, std::min(w->hi, p));
        if (p != w->pos) {
            w->pos = p;
            OnSlider(w->id, false);
        }
        return true;
    }
    case ui::Kind::Custom:
        if (w->id != IDC_TEXLIST || g_rows.empty()) return false;
        if (vk == VK_UP) g_focusRow = std::max(0, g_focusRow - 1);
        else if (vk == VK_DOWN) g_focusRow = std::min((int)g_rows.size() - 1, g_focusRow + 1);
        else if (act) OnListToggle(g_focusRow);
        else return false;
        if (g_focusRow < g_listScroll) g_listScroll = g_focusRow;
        if (g_focusRow >= g_listScroll + kRowsShown) g_listScroll = g_focusRow - kRowsShown + 1;
        return true;
    }
    return false;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        g_updStart = GetTickCount();
        g_gameRes = ReadAppString(L"manageResolution") != L"0";
        g_withVR = ReadAppString(L"withSteamVR") != L"0";
        CreateControls();
        SetText(IDC_TEXHINT, L"Untick a size to leave it alone, say an overlay that got tinted. Eye textures stay upscaled together.");
        ModelToUi();
        RefreshInstall(true);
        RefreshStatus();
        Save();
        SyncGameRes();   // in case the preset changed while the app was closed
        SetTimer(h, TIMER_REFRESH, 250, nullptr);
        SetTimer(h, TIMER_PUSH, 100, nullptr);
        RefreshUpdateUi();
        StartLink();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            RefreshStatus();
            RefreshInstall(false);
            UpdateTick();
            WithVrTick();
        } else if (wp == TIMER_PUSH) {
            if (g_pushPending) Push();
        } else if (wp == TIMER_SAVE) {
            KillTimer(h, TIMER_SAVE);
            Save();
        } else if (wp == TIMER_TIP) {
            KillTimer(h, TIMER_TIP);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            int id = 0, part = -1;
            RectF a;
            if (g_tipId && !g_in.drag && TipAt(L(pt.x), L(pt.y) + g_scroll, id, part, a) &&
                id == g_tipId && part == g_tipPart) {
                g_tipShown = true;
                Invalidate();
            }
        } else if (wp == TIMER_REPLAN) {
            KillTimer(h, TIMER_REPLAN);
            if (g_replanPending) {
                g_replanPending = false;
                Changed(true);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        g_bgDirty = true;
        SetScroll(g_scroll);
        Invalidate();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == h) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            const Hit ht = HitTest(L(pt.x), L(pt.y) + g_scroll);
            SetCursor(LoadCursor(nullptr, g_in.drag || ht.click ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        const float x = L(GET_X_LPARAM(lp)), y = L(GET_Y_LPARAM(lp)) + g_scroll;
        if (!g_mouseTracked) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            g_mouseTracked = true;
        }
        if (g_in.drag) {
            DragTo(x);
            return 0;
        }
        TipTrack(x, y);
        const Hit ht = HitTest(x, y);
        const int row = ht.id == IDC_TEXLIST ? ht.part : -1;
        const int part = ht.id == IDC_TEXLIST ? (ht.click ? ht.part : -1) : ht.part;
        if (ht.id != g_in.hover || part != g_in.hoverPart || row != g_hoverRow) {
            g_in.hover = ht.id;
            g_in.hoverPart = part;
            g_hoverRow = row;
            Invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouseTracked = false;
        HideTip();
        g_tipId = 0;
        g_tipPart = -1;
        g_tipSuppress = 0;
        if (g_in.hover || g_hoverRow >= 0) {
            g_in.hover = 0;
            g_in.hoverPart = -1;
            g_hoverRow = -1;
            Invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const float x = L(GET_X_LPARAM(lp)), y = L(GET_Y_LPARAM(lp)) + g_scroll;
        const Hit ht = HitTest(x, y);
        TipForget();
        SetFocus(h);
        g_in.focusVisible = false;
        g_in.pressed = ht.click ? ht.id : 0;
        if (ht.click) {
            g_in.focus = ht.id;
            SetCapture(h);
            ui::Widget *w = W(ht.id);
            if (w && w->kind == ui::Kind::Slider) {
                g_in.drag = w->id;
                DragTo(x);
            } else if (w && w->kind == ui::Kind::Segment) {
                OnSegment(w->id, ht.part);
            }
            if (ht.id == IDC_TEXLIST) g_focusRow = ht.part;
        }
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x = L(GET_X_LPARAM(lp)), y = L(GET_Y_LPARAM(lp)) + g_scroll;
        const int pressed = g_in.pressed, drag = g_in.drag;
        g_in.pressed = 0;
        g_in.drag = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (drag) {
            OnSlider(drag, false);
        } else if (pressed) {
            const Hit ht = HitTest(x, y);
            if (ht.id == pressed && ht.click) {
                const ui::Widget *w = W(pressed);
                if (pressed == IDC_TEXLIST) OnListToggle(ht.part);
                else if (w && w->kind == ui::Kind::Button) OnButton(pressed);
                else if (w && w->kind == ui::Kind::Toggle) OnToggle(pressed);
            }
        }
        Invalidate();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_in.drag) {
            const int d = g_in.drag;
            g_in.drag = 0;
            OnSlider(d, false);
        }
        g_in.pressed = 0;
        Invalidate();
        break;
    case WM_MOUSEWHEEL: {
        static int acc = 0;
        acc += GET_WHEEL_DELTA_WPARAM(wp);
        const int notches = acc / WHEEL_DELTA;
        acc -= notches * WHEEL_DELTA;
        if (!notches) return 0;
        TipForget();
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        const float x = L(pt.x), y = L(pt.y) + g_scroll;
        if (kList.Contains(x, y) && (int)g_rows.size() > kRowsShown) {
            g_listScroll = std::max(0, std::min((int)g_rows.size() - kRowsShown, g_listScroll - notches));
            g_hoverRow = -1;
            Invalidate();
        } else if (kLog.Contains(x, y) && !g_events.empty()) {
            g_logScroll = std::max(0, g_logScroll + notches * 3);
            Invalidate();
        } else if (MaxScroll() > 0) {
            SetScroll(g_scroll - notches * 48.0f);
        }
        return 0;
    }
    case WM_KEYDOWN:
        TipForget();
        if (OnKey((UINT)wp)) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) HideTip();
        break;
    case WM_APP_SYNCLIST:
        RefreshList();
        Invalidate();
        return 0;
    case WM_APP_GAMERES:
        OnResDone((ResDone *)lp);
        return 0;
    case WM_APP_PROJ:
        OnProjection((ProjResult *)lp);
        return 0;
    case WM_APP_UPDCHECK:
        OnUpdateCheck((app::UpdateInfo *)lp);
        return 0;
    case WM_APP_UPDPROGRESS:
        if (g_updState == UpdDownloading) {
            g_updProgress = wp ? (float)(wp - 1) / 1000.0f : -1.0f;
            RefreshUpdateUi();
        }
        return 0;
    case WM_APP_UPDDONE:
        OnUpdateDone((app::StagedUpdate *)lp);
        return 0;
    case WM_APP_VRQUIT:
        OnSteamVrQuit();
        return 0;
    case WM_APP_AUTOLAUNCH:
        OnAutoLaunch((AutoLaunchDone *)lp);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        ui::MakeFonts(g_dpi);
        app::ChangeLogDpiChanged(g_dpi);
        RefreshUpdateUi();
        g_bgDirty = true;
        RECT *r = (RECT *)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetScroll(g_scroll);
        Invalidate();
        return 0;
    }
    case WM_CLOSE:
        g_updCancel = 1;
        // restarting into a new version: VRChat's resolution stays as it is (the new app takes over)
        if (g_updRestarting)
            for (int i = 0; i < 100 && g_resWorkers > 0; ++i) Sleep(50);
        else
            RestoreGameResOnExit();
        StopLink();   // (after the restore, which may use its connection)
        KillTimer(h, TIMER_SAVE);
        KillTimer(h, TIMER_REPLAN);
        if (g_replanPending) {
            g_replanPending = false;
            Changed(true);
        }
        Save();
        if (g_pushPending) Push();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_back.Free();
        g_bg.Free();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

UINT SystemDpi()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI * PFN)(void);
    PFN f = u ? (PFN)GetProcAddress(u, "GetDpiForSystem") : nullptr;
    if (f) return f();
    HDC dc = GetDC(nullptr);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

void EnableDpiAwareness()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * PFN)(HANDLE);
    PFN f = u ? (PFN)GetProcAddress(u, "SetProcessDpiAwarenessContext") : nullptr;
    if (!f || !f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

// ---- command line -----------------------------------------------------------------------------

std::string Narrow(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

int WriteStatusFile(const std::wstring &path, int waitMs)
{
    Channel ch;
    VuStatus st = {};
    bool live = false;
    for (int t = 0; t <= waitMs; t += 100) {
        if (ch.Open() && ch.ReadStatus(st) && StatusIsLive(st)) {
            live = true;
            break;
        }
        Sleep(100);
    }
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "connected=%d\n", live ? 1 : 0);
    out += line;
    if (live) {
        snprintf(line, sizeof(line), "version=%s\npid=%u\nhooks=%d\ncommandLists=%d\nupscales=%u\nappliedSeq=%u\neyes=%d\nmixedEyes=%d\n",
                 st.version, st.pid, st.hooksInstalled, st.commandLists, st.upscalesTotal, st.appliedSeq, st.eyePools, st.mixedEyes);
        out += line;
        const VuSettings &c = st.current;
        snprintf(line, sizeof(line),
                 "enabled=%d\nmode=%s\nupscaleFactor=%.2f\nsharpHistory=%d\nbilinearEdges=%d\nsharpness=%.2f\ntint=%d\nhotkeys=%d\n"
                 "maxWeight=%.2f\nlocalTrust=%.2f\nresidualTol=%.2f\ntemporalArea=%.2f\nsharpnessArea=%.2f\nfxaa=%d\nexcluded=%d\n"
                 "gpuMsPerSec=%.3f\n",
                 c.enabled, Narrow(ModeLabel(c.mode)).c_str(), c.upscaleFactor, c.fastHistory ? 0 : 1, c.bilinearEdges, c.sharpness, c.tint,
                 c.hotkeys, c.maxWeight, c.localTrust, c.residualTol, c.temporalArea, c.sharpArea, c.fxaa, c.excludeCount, st.gpuMsPerSec);
        out += line;
        for (uint32_t k = 0; k < st.texCount && k < kVuMaxTex; ++k) {
            const VuTexInfo &t = st.tex[k];
            snprintf(line, sizeof(line), "texture=%ux%u x%u fmt%u %s %ux%u eye=%d content=%ux%u->%ux%u %s\n", t.w, t.h, t.slices,
                     t.format, t.upscaled ? "upscaled" : "skipped", t.outW, t.outH, t.eye, t.contentW, t.contentH, t.contentOutW,
                     t.contentOutH, t.reason);
            out += line;
        }
        for (uint32_t i = 0; i < st.streamCount && i < kVuMaxStreams; ++i) {
            const VuStreamInfo &s = st.streams[i];
            snprintf(line, sizeof(line), "stream=#%u %ux%u->%ux%u textures=%u rate=%.1f\n", s.id, s.w, s.h, s.outW, s.outH,
                     s.textures, s.runsPerSec);
            out += line;
        }
        snprintf(line, sizeof(line), "ini=%s\nlog=%s\n", Narrow(st.iniPath).c_str(), Narrow(st.logPath).c_str());
        out += line;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
        CloseHandle(f);
    }
    return live ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g_inst = inst;
    g_settingsPath = DefaultSettingsPath();
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool doInstall = false, doUninstall = false, force = false, headless = false;
    std::wstring statusFile, steamvr, gameRes, report;
    bool gameResTest = false, updateTest = false, checkUpdate = false, changeLogTest = false, showChangeLog = false;
    bool launchArg = false, finishDll = false, linkTest = false;
    std::vector<std::wstring> sets;
    int waitMs = 3000;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--install") doInstall = headless = true;
        else if (a == L"--uninstall") doUninstall = headless = true;
        else if (a == L"--force") force = true;
        else if (a == L"--steamvr") steamvr = next();
        else if (a == L"--settings") g_settingsPath = next();
        else if (a == L"--channel") g_channelName = next();
        else if (a == L"--status-file") { statusFile = next(); headless = true; }
        else if (a == L"--set") { sets.push_back(next()); headless = true; }
        else if (a == L"--wait-ms") waitMs = _wtoi(next().c_str());
        // self-test of the VRChat-resolution feature: set it (a percent or "none") / check the JSON handling
        else if (a == L"--game-resolution") { gameRes = next(); headless = true; }
        else if (a == L"--test-game-resolution") gameResTest = headless = true;
        else if (a == L"--report") report = next();
        else if (a == L"--updated-from") g_updatedFrom = next();
        else if (a == L"--test-update") updateTest = headless = true;   // self-test of the updater's parts (no network)
        else if (a == L"--test-changelog") changeLogTest = headless = true;   // self-test of the change log's reading and layout
        else if (a == L"--changelog") showChangeLog = true;   // open the change log at start
        else if (a == L"--check-update") checkUpdate = headless = true; // ask GitHub (or VRUPSCALE_UPDATE_URL) and report
        else if (a == app::kLaunchArg) launchArg = true;               // started by SteamVR (its manifest's argument)
        else if (a == L"--finish-dll-update") finishDll = headless = true;   // update SteamVR's copy once SteamVR is gone
        else if (a == L"--test-steamvr-link") linkTest = headless = true;   // self-test of the startup manifest
    }
    LocalFree(argv);

    if (headless) {
        int code = 0;
        std::wstring out;
        if (gameResTest) code = app::GameResSelfTest(out) ? 5 : 0;
        if (linkTest) code = app::VrLinkSelfTest(out) ? 11 : code;
        if (updateTest) code = app::UpdateSelfTest(out) ? 7 : code;
        if (changeLogTest) {
            ui::Startup(inst);
            ui::MakeFonts(96);
            code = app::ChangeLogSelfTest(Widen(VU_APP_VERSION), out) ? 9 : code;
            ui::Shutdown();
        }
        if (checkUpdate) {
            app::UpdateInfo u = app::CheckForUpdate(Widen(VU_APP_VERSION));
            out += Fmt(L"check: %s%s, latest %s, newer %d, zip %s (%llu bytes), sha256 %s, sha asset %s\n", u.ok ? L"ok" : L"FAILED: ",
                       u.error.c_str(), u.version.c_str(), u.newer ? 1 : 0, u.zipUrl.c_str(), (unsigned long long)u.zipSize,
                       Widen(u.sha256.c_str()).c_str(), u.shaUrl.c_str());
            if (!u.ok) code = 8;
        }
        if (!gameRes.empty()) {
            app::GameResJob job;
            job.value = gameRes == L"none" ? app::kResUnset : _wtoi(gameRes.c_str());
            job.readBefore = true;
            app::GameResResult r = app::SetGameResolution(app::kVrchatAppKey, job);
            out += Fmt(L"%s via %s, before %d, file %s%s%s\n", r.ok ? L"ok" : L"FAILED", r.viaApi ? L"OpenVR" : L"the settings file", r.before,
                       app::SteamVrSettingsFile().c_str(), r.error.empty() ? L"" : L": ", r.error.c_str());
            if (!r.ok) code = 6;
        }
        if (!report.empty()) {
            FILE *f = _wfopen(report.c_str(), L"wb");
            if (f) {
                std::string u;
                int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), nullptr, 0, nullptr, nullptr);
                u.resize(n);
                if (n) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), &u[0], n, nullptr, nullptr);
                fwrite(u.data(), 1, u.size(), f);
                fclose(f);
            }
        }
        if (doInstall || doUninstall) {
            std::wstring root = steamvr.empty() ? FindSteamVR() : steamvr;
            if (!IsSteamVRRoot(root)) return 3;
            std::wstring msg;
            if (doInstall) MigrateLegacySettings(root);
            InstallResult r = doUninstall ? Uninstall(root, msg) : Install(root, force, msg);
            code = r == InstallResult::Ok ? 0 : 10 + (int)r;
        }
        if (!sets.empty()) {
            VuSettings s;
            LoadSettings(g_settingsPath, s);
            for (auto &kv : sets) {
                size_t eq = kv.find(L'=');
                if (eq == std::wstring::npos || !SetSettingByName(s, kv.substr(0, eq), kv.substr(eq + 1))) return 4;
            }
            SaveSettings(g_settingsPath, s);
            Channel ch;
            VuStatus st;
            code = 2;   // saved, nobody to apply it live
            if (ch.Open() && ch.ReadStatus(st) && StatusIsLive(st)) {
                int32_t seq = ch.SendSettings(s);
                for (int t = 0; t <= waitMs; t += 50) {
                    if (ch.ReadStatus(st) && (int32_t)st.appliedSeq == seq) {
                        code = 0;
                        break;
                    }
                    Sleep(50);
                }
            }
        }
        if (!statusFile.empty()) code = WriteStatusFile(statusFile, waitMs) || code;
        if (finishDll) {   // started by CloseWithSteamVR: waits up to two minutes for SteamVR to be gone
            HANDLE busy = CreateMutexW(nullptr, FALSE, kDllHelperMutex);
            for (int i = 0; i < 240 && IsSteamVRRunning(); ++i) Sleep(500);
            std::wstring msg;
            const DllUpd u = FinishDllUpdate(IsSteamVRRoot(steamvr) ? steamvr : FindSteamVR(), msg);
            code = u == DllUpd::Failed ? 12 : u == DllUpd::Waiting ? 13 : code;
            if (busy) CloseHandle(busy);
        }
        return code;
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\vrupscale.app.single");
    bool another = single && GetLastError() == ERROR_ALREADY_EXISTS;
    if (another && !g_updatedFrom.empty()) {   // started by an update: the old version is closing; wait for it
        const DWORD w = WaitForSingleObject(single, 30000);
        another = !(w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
    }
    g_fromVR = app::StartedBySteamVR(launchArg);
    if (another && g_fromVR) return 0;   // SteamVR starting it while it is open already: nothing to show
    if (another) {
        HWND other = FindWindowW(L"vrupscale.app", nullptr);
        if (other) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    app::CleanupAfterUpdate();   // what an update renamed aside (the old exe has exited by now)
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    bool adjusted = false;
    LoadSettings(g_settingsPath, g_s, &adjusted);
    g_root = IsSteamVRRoot(steamvr) ? steamvr : FindSteamVR();
    // first run: take over the settings an older version kept in the SteamVR folder
    if (MigrateLegacySettings(g_root)) LoadSettings(g_settingsPath, g_s, &adjusted);
    if (adjusted) {   // e.g. a factor from the old slider, moved onto its preset: SteamVR gets it too
        SaveSettings(g_settingsPath, g_s);
        g_lastEdit = GetTickCount();
        g_pushOnConnect = true;
    }
    if (IsSteamVRRoot(steamvr)) WriteAppString(L"steamvrPath", steamvr);
    g_dpi = SystemDpi();
    ui::Startup(inst);
    ui::MakeFonts(g_dpi);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"vrupscale.app";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    RegisterClassExW(&wc);

    // the page's size, or the screen's height if that is less (the page then scrolls)
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(kW), S(kH) };
    AdjustWindowRect(&r, style, FALSE);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int ww = r.right - r.left, wh = std::min((int)(r.bottom - r.top), (int)(wa.bottom - wa.top));
    const int x = wa.left + std::max(0, (int)(wa.right - wa.left - ww) / 2), y = wa.top + std::max(0, (int)(wa.bottom - wa.top - wh) / 2);
    std::wstring title = L"vrupscale " + Widen(VU_APP_VERSION);
    HWND w = CreateWindowExW(0, L"vrupscale.app", title.c_str(), style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    // Windows 11: a title bar in the page's grey, so the window reads as one piece (ignored elsewhere)
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT(WINAPI * PFN)(HWND, DWORD, LPCVOID, DWORD);
        if (PFN set = (PFN)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            const COLORREF caption = col::sky, text = col::ink;
            set(w, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
            set(w, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
        }
    }
    ShowWindow(w, g_fromVR ? SW_SHOWMINNOACTIVE : show);   // started by SteamVR: out of the way
    UpdateWindow(w);
    if (showChangeLog) OpenChangeLog();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ui::Shutdown();
    return 0;
}
