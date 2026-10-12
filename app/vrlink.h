// The app's connection to SteamVR: OpenVR (openvr_api.dll next to this exe, initialised as a
// background app, which never starts SteamVR), and the app's place among SteamVR's startup apps.
//
// Every use of OpenVR goes through a VrSession: SteamVR's settings (gameres.cpp), the eyes'
// projection, the startup registration. While "Start with SteamVR" is on, the app also keeps one
// lasting connection open for as long as SteamVR runs (VrLinkOpen), so that SteamVR's request to
// quit reaches it (VrLinkPollQuit); sessions then borrow that connection instead of opening their own
// (openvr_api.dll holds one connection per process).
#pragma once

#include <string>

namespace app {

// Holds OpenVR for one use (other threads wait meanwhile).
class VrSession {
public:
    VrSession();
    ~VrSession();
    VrSession(const VrSession &) = delete;
    VrSession &operator=(const VrSession &) = delete;
    bool Open(std::wstring &error);          // connects, or borrows the lasting connection
    void *Interface(const char *version);    // e.g. "FnTable:IVRSettings_003"; nullptr if not offered
    int initError = 0;                       // VR_InitInternal2's error when Open failed

private:
    bool own_ = false, open_ = false;
};

// The lasting connection (SteamVR must be running). Open fails if this SteamVR can't deliver its
// quit request (no IVRSystem_026).
bool VrLinkOpen(std::wstring &error);
bool VrLinkIsOpen();
void VrLinkClose();
// Takes SteamVR's pending events: true if SteamVR asked this process to quit (VREvent_Quit), which is
// then acknowledged (the process is about to exit).
bool VrLinkPollQuit();

// SteamVR starts the app with it: a manifest (vrupscale.vrmanifest, next to the exe, else in the
// settings folder) registered through IVRApplications as a dashboard overlay app, with SteamVR's
// auto-launch switched on. SteamVR lists it under its startup overlay apps, where the user can switch
// it off too.
constexpr const char *kVrAppKey = "zakhazerd.vrupscale";
constexpr const wchar_t *kLaunchArg = L"--from-steamvr";   // the manifest's arguments

struct AutoLaunchJob {
    bool want = true;        // the app's setting
    bool applied = false;    // SteamVR was last told "on" (so "off" there now means the user switched it off)
};
struct AutoLaunchResult {
    bool ok = false;
    bool want = true;        // the setting after the sync: off if the user switched vrupscale off in SteamVR
    bool userOff = false;    // ... which happened
    bool registered = false; // the manifest was added (or moved to this exe) now
    bool notReady = false;   // SteamVR didn't take the connection (starting or closing): try again later
    std::wstring error;
};
AutoLaunchResult SyncAutoLaunch(const AutoLaunchJob &job);

// The manifest for this exe: binaryPath relative to the manifest's folder, or absolute.
std::string ManifestText(const std::wstring &binaryPath);

// This process was started by SteamVR (the manifest's argument, or a SteamVR process as parent).
bool StartedBySteamVR(bool hasLaunchArg);

int VrLinkSelfTest(std::wstring &report);

} // namespace app
