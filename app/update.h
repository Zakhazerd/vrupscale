// Updates from GitHub: the app asks GitHub for vrupscale's latest release, and when it is newer
// offers it; one click downloads the release zip, checks it against the SHA-256 published with
// it, unpacks it and replaces the app's files (the running exe and DLLs are renamed aside, which
// Windows allows, and deleted at the next start). SteamVR's copy of the DLL is updated by the
// restarted app, as soon as SteamVR is closed.
//
// The release zip is stored without compression (tools/package.sh), so unpacking needs no
// decompressor; each file's CRC-32 is checked as well.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace app {

constexpr const wchar_t *kUpdateRepo = L"Zakhazerd/vrupscale";   // github.com/<this>/releases

struct UpdateInfo {
    bool ok = false;            // GitHub answered
    bool newer = false;         // ... with a release newer than this app
    std::wstring version;       // "0.6.1"
    std::wstring title;         // the release's name
    std::wstring notes;         // its description (Markdown)
    std::wstring page;          // its page on GitHub
    std::wstring zipUrl, zipName;
    uint64_t zipSize = 0;
    std::string sha256;         // lower-case hex, from GitHub's digest of the asset ("" = not given)
    std::wstring shaUrl;        // or the release's <zip>.sha256 asset
    std::wstring error;
};

// Asks GitHub (blocking: call off the UI thread). currentVersion: "0.6.0".
UpdateInfo CheckForUpdate(const std::wstring &currentVersion);

// a < b: < 0, equal: 0, a > b: > 0 ("0.6.10" > "0.6.9"; a leading "v" is ignored)
int CompareVersions(const std::wstring &a, const std::wstring &b);

// Downloads the zip, checks its SHA-256 and unpacks it into DataDir()\update\<version>.
// progress(done, total) is called from this thread; cancel: set to stop.
struct StagedUpdate {
    bool ok = false;
    std::wstring dir;           // the unpacked release (vrupscale.exe, payload\dxgi.dll, ...)
    std::wstring version;
    std::wstring error;
};
StagedUpdate DownloadUpdate(const UpdateInfo &u, void (*progress)(uint64_t done, uint64_t total, void *ctx), void *ctx,
                            volatile long *cancel);

// Replaces the files in the app's folder with the staged ones. In-use files (the exe, openvr_api.dll)
// are renamed to <name>.old first; on failure everything is put back. False + error on failure.
bool ApplyUpdate(const std::wstring &stagedDir, std::wstring &error);

// At start: deletes what an earlier update renamed aside, and old staging folders.
void CleanupAfterUpdate();

// Self-test of the zip reader, the hash and the version comparison (no network): 0 = pass.
int UpdateSelfTest(std::wstring &report);

} // namespace app
