// The change log window: CHANGELOG.md (embedded in the exe when it is built) drawn in the app's
// style, one card per version, newest first. Opened by the Change log button by the version.
#pragma once

#include <windows.h>
#include <string>

namespace app {

struct ChangeLogInfo {
    std::wstring current;       // this app's version: marked "this version"
    std::wstring updatedFrom;   // started by an update from that version: the ones after it are marked "new"
    std::wstring updVersion;    // an update is available: its notes (Markdown) come first, marked "available"
    std::wstring updNotes;
};

// Opens the window over owner, or refreshes it (and brings it forward, if bringForward) when it is open.
// dpi: what the fonts are made for.
void ShowChangeLog(HWND owner, HINSTANCE inst, UINT dpi, const ChangeLogInfo &info, bool bringForward = true);
// The owner remade the fonts for a new dpi (ui::MakeFonts): relayout.
void ChangeLogDpiChanged(UINT dpi);
bool ChangeLogOpen();

// The embedded CHANGELOG.md ("" if missing).
std::wstring ChangeLogText();

// Self-test of the Markdown reading and the layout (needs ui::Startup and ui::MakeFonts): 0 = pass.
int ChangeLogSelfTest(const std::wstring &current, std::wstring &report);

} // namespace app
