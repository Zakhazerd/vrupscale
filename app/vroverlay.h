// The app's page in SteamVR's dashboard: a dashboard overlay. Its icon sits in the dashboard's bottom
// bar with SteamVR's own; picked, it shows the same page as the window, used with the controller's
// laser like a mouse. It lives on the lasting connection (vrlink.h): made once that connection is
// open, again after it reopens, gone with it. Everything here runs on the window's thread and never
// waits for OpenVR (a tick while another thread uses the connection does nothing).
//
// The page is drawn at 96 dpi, 1120 x 900 pixels: under the 1000 pixels a side vrupscale's DLL wants
// for an eye texture, so the compositor never upscales the app's own page. It goes to SteamVR as a
// D3D11 texture on the headset's adapter.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace app {

constexpr const char *kOverlayKey = "zakhazerd.vrupscale";   // the same as the app's key in its manifest
constexpr float kOverlayWidthM = 2.0f;                       // the page's width in the dashboard

struct OverlayInput {
    enum Kind { Move, Down, Up, Leave, Scroll } kind = Move;
    float x = 0, y = 0;   // page pixels from the top left (Move, Down, Up)
    float notches = 0;    // Scroll: positive away from the user, like a mouse wheel
};

struct OverlayTick {
    bool exists = false;   // the overlay is in the dashboard
    bool created = false;  // ... since the last tick: the page has to be sent again
    bool visible = false;  // the page is showing: worth drawing
    std::wstring error;    // SteamVR refused the overlay (once per connection)
};

// pageW x pageH: the page in pixels (mouse coordinates are scaled to it).
OverlayTick OverlayUpdate(int pageW, int pageH, std::vector<OverlayInput> &input);
// The page: 32-bit BGRA rows from the top, alpha 255. False if it couldn't go now (try again).
bool OverlaySetPage(const uint32_t *pixels, int w, int h, std::wstring &error);
// The icon in the dashboard's bar (straight alpha), sent with every new overlay.
void OverlaySetThumbnail(const uint32_t *pixels, int w, int h);

} // namespace app
