// Sets a game's render resolution in SteamVR: its per-application "resolutionScale", the
// "Custom resolution multiplier" of SteamVR's per-application video settings, a percentage of
// SteamVR's global resolution. Section = the app's key (VRChat: steam.app.438100).
//
// While SteamVR runs, SteamVR owns its settings file, so the change goes through OpenVR's settings
// interface (openvr_api.dll next to this exe, initialised as a background app, which never starts
// SteamVR); SteamVR applies it at once and saves it. Otherwise the settings file is edited
// directly (a backup of the original is kept once, as steamvr.vrsettings.vrupscale-backup).
#pragma once

#include <string>

namespace app {

constexpr const char *kVrchatAppKey = "steam.app.438100";
constexpr int kResUnset = -1;     // no per-application value: SteamVR uses 100%
constexpr int kResUnknown = -2;

struct GameResJob {
    int value = 100;          // percent to set, or kResUnset to remove the per-application value
    bool readBefore = false;  // also report the value found before the change
};

struct GameResResult {
    GameResJob job;
    bool ok = false;
    bool viaApi = false;      // SteamVR was running: set through OpenVR (else: its settings file)
    int before = kResUnknown;
    std::wstring error;       // short, for the app's hint
};

// Runs synchronously (may take a moment while SteamVR answers): call it off the UI thread.
GameResResult SetGameResolution(const char *appKey, const GameResJob &job);

// SteamVR's global render resolution: this one SteamVR applies to the running game at once (the
// per-application value only when the game starts, or while SteamVR's settings window is open).
// vrupscale drives it while VRChat runs: global = what the user had x target / VRChat's own
// per-application value, so VRChat renders `target` of the resolution it would otherwise.
struct GlobalRes {
    float scale = 1.0f;       // supersampleScale
    bool manual = true;       // supersampleManualOverride ("Custom"; false = "Auto")
};
struct GlobalResJob {
    bool restore = false;     // put `base` back (through OpenVR, or the settings file with SteamVR closed)
    float target = 1.0f;      // apply: the game's resolution, a fraction (pixels) of what it is with `base`
    bool haveBase = false;    // apply: base is known; else it is read now and returned
    GlobalRes base;
};
struct GlobalResResult {
    GlobalResJob job;
    bool ok = false;
    bool viaApi = false;
    GlobalRes base;           // what the user had
    float applied = 0;        // the global scale set
    int perApp = 100;         // the game's own per-application percentage
    std::wstring error;
};
GlobalResResult SetGlobalResolution(const char *appKey, const GlobalResJob &job);
bool VRChatRunning();

// steamvr.vrsettings, from OpenVR's own registry of paths (%LOCALAPPDATA%\openvr\openvrpaths.vrpath),
// else Steam's folder; empty if neither is found.
std::wstring SteamVrSettingsFile();

// The settings file's value for appKey: percent, kResUnset or kResUnknown (no file / unreadable).
int ReadGameResolutionFile(const std::wstring &file, const char *appKey);

// Each eye's projection as SteamVR reports it (IVRSystem::GetProjectionRaw: tangents left, right,
// top, bottom; left eye then right), through OpenVR as a background app. SteamVR must be running.
bool GetEyeProjection(float out[8], std::wstring &error);

// Bigscreen's eye tracking client is publishing gaze data (its "VRCFT" output).
bool EyeTrackerPresent();

// Self-test of the JSON handling (no SteamVR involved): 0 = pass.
int GameResSelfTest(std::wstring &report);

} // namespace app
