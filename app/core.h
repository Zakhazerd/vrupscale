// vrupscale app: settings file, SteamVR install, and the live channel to the DLL.
// No UI here; main.cpp builds the window on top of these.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>

#include "../src/control_shared.h"

namespace app {

// ---- paths ------------------------------------------------------------------------------------

std::wstring ExeDir();
std::wstring DataDir();             // %LOCALAPPDATA%\vrupscale (created)
std::wstring DefaultSettingsPath(); // DataDir()\vrupscale.ini
std::wstring DefaultLogPath();      // DataDir()\vrupscale.log
std::wstring PayloadPath();         // the dxgi.dll this app installs

extern std::wstring g_settingsPath; // overridable (--settings)
extern std::wstring g_channelName;  // overridable (--channel)

// ---- settings (same keys and meaning as the DLL reads) -------------------------------------------

VuSettings DefaultSettings();
// false: file missing (defaults). *adjusted: a value was changed on loading (an upscale factor
// close to a scaling preset is moved onto it), so the caller should save them back.
bool LoadSettings(const std::wstring &path, VuSettings &s, bool *adjusted = nullptr);
bool SaveSettings(const std::wstring &path, const VuSettings &s);
bool SameSettings(const VuSettings &a, const VuSettings &b);  // fields the app manages
const wchar_t *ModeLabel(int mode);
bool SetSettingByName(VuSettings &s, const std::wstring &key, const std::wstring &value);   // --set

std::wstring ReadAppString(const wchar_t *key);               // [app] section of the settings file
void WriteAppString(const wchar_t *key, const std::wstring &v);

// ---- SteamVR ----------------------------------------------------------------------------------

std::wstring FindSteamVR();          // root folder, or empty
bool IsSteamVRRoot(const std::wstring &root);
bool IsSteamVRRunning();
struct DllInfo {
    bool present = false;
    bool ours = false;
    std::wstring product, version;
};
DllInfo GetDllInfo(const std::wstring &path);
std::wstring InstalledDllPath(const std::wstring &root);   // root\bin\win64\dxgi.dll
std::wstring LegacyIniPath(const std::wstring &root);      // root\bin\win64\vrupscale.ini

enum class InstallResult { Ok, SteamVRRunning, NoPayload, ForeignDll, AccessDenied, Failed };
// force: back up and replace a dxgi.dll from another tool.
InstallResult Install(const std::wstring &root, bool force, std::wstring &message);
InstallResult Uninstall(const std::wstring &root, std::wstring &message);
// Copies an old settings file from the SteamVR folder to the app's location if that has none.
bool MigrateLegacySettings(const std::wstring &root);

// ---- live channel to the DLL ------------------------------------------------------------------

class Channel {
public:
    ~Channel() { Close(); }
    bool Open();                         // true if the DLL's channel exists (and is our layout)
    void Close();
    bool IsOpen() const { return sh_ != nullptr; }
    bool LayoutMismatch() const { return mismatch_; }
    bool ReadStatus(VuStatus &st) const;
    int32_t SendSettings(const VuSettings &s);   // returns the sequence the DLL will echo
    void SendCommand(uint32_t cmd);
private:
    HANDLE map_ = nullptr;
    VuShared *sh_ = nullptr;
    bool mismatch_ = false;
};

// Status is live if the DLL's process exists and published recently.
bool StatusIsLive(const VuStatus &st);
const wchar_t *FormatLabel(uint32_t dxgiFormat);

std::wstring Widen(const char *s);
std::wstring Fmt(const wchar_t *fmt, ...);

} // namespace app
