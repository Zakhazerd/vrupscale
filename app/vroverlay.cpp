// The app's page in SteamVR's dashboard (see vroverlay.h).
#include "vroverlay.h"
#include "core.h"
#include "vrlink.h"

#include <d3d11.h>
#include <dxgi.h>
#include <string.h>

namespace app {

namespace {

// ---- OpenVR's C interface (openvr_capi.h) ---------------------------------------------------------

typedef uint64_t Handle;                                     // VROverlayHandle_t
struct Vec2 { float v[2]; };                                 // HmdVector2_t
struct Texture { void *handle; int type; int colorSpace; };  // Texture_t
// VREvent_t (64-bit Windows): eventType, trackedDeviceIndex, eventAgeSeconds, then the data union
// at 16; 64 bytes in all. Mouse data: x, y, button; scroll data: xdelta, ydelta.
constexpr uint32_t kEventSize = 64;

// IVROverlay_028's function-table slots (VR_IVROverlay_FnTable)
enum { kGetErrorName = 8, kSetFlag = 11, kSetWidth = 22, kIsVisible = 45, kPollEvent = 48, kSetInputMethod = 50,
       kSetMouseScale = 52, kSetTexture = 60, kCreateDashboard = 67 };
typedef const char *(__stdcall *PFN_ErrName)(int err);
typedef int(__stdcall *PFN_SetFlag)(Handle, int flag, bool on);
typedef int(__stdcall *PFN_SetWidth)(Handle, float meters);
typedef bool(__stdcall *PFN_IsVisible)(Handle);
typedef bool(__stdcall *PFN_PollEvent)(Handle, void *event, uint32_t size);
typedef int(__stdcall *PFN_SetInputMethod)(Handle, int method);
typedef int(__stdcall *PFN_SetMouseScale)(Handle, Vec2 *scale);
typedef int(__stdcall *PFN_SetTexture)(Handle, Texture *tex);
typedef int(__stdcall *PFN_CreateDashboard)(const char *key, const char *name, Handle *main, Handle *thumbnail);
// IVRSystem_026 slot 8
typedef void(__stdcall *PFN_GetDXGIOutputInfo)(int32_t *adapterIndex);

enum {
    kFlagDiscreteScroll = 1 << 6,   // VROverlayFlags_SendVRDiscreteScrollEvents
    kInputMouse = 1,                // VROverlayInputMethod_Mouse
    kTexDirectX = 0,                // ETextureType
    kColorGamma = 1,                // EColorSpace
    kEvMouseMove = 300, kEvMouseDown = 301, kEvMouseUp = 302, kEvFocusLeave = 304, kEvScrollDiscrete = 305,
    kMouseLeft = 1,                 // EVRMouseButton
};

// ---- state (the window's thread only) -------------------------------------------------------------

struct Overlay {
    unsigned gen = 0;       // the lasting connection it belongs to
    bool tried = false;     // made, or refused, on that connection
    void **ov = nullptr;    // IVROverlay_028
    Handle main = 0, thumb = 0;
    bool visible = false;
};
Overlay g_o;

// Direct3D 11 on the headset's adapter, made once
ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
bool g_devTried = false;
ID3D11Texture2D *g_pageTex = nullptr, *g_thumbTex = nullptr;
int g_pageW = 0, g_pageH = 0, g_thumbTexW = 0, g_thumbTexH = 0;
std::vector<uint32_t> g_thumb;
int g_thumbW = 0, g_thumbH = 0;

std::wstring ErrName(void **ov, int e)
{
    const char *n = ov ? ((PFN_ErrName)ov[kGetErrorName])(e) : nullptr;
    return n ? Widen(n) : Fmt(L"error %d", e);
}

bool EnsureDevice(VrLinkUse &link)
{
    if (g_dev || g_devTried) return g_dev != nullptr;
    g_devTried = true;
    // from System32 (with what they load), never a dxgi.dll that happens to sit next to the exe
    HMODULE d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D11_CREATE_DEVICE create = d3d ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
    if (!create) return false;
    IDXGIAdapter1 *adapter = nullptr;   // the one the headset is on (SteamVR reads the page there)
    int32_t index = -1;
    if (void **sys = (void **)link.Interface("FnTable:IVRSystem_026")) ((PFN_GetDXGIOutputInfo)sys[8])(&index);
    if (index >= 0) {
        typedef HRESULT(WINAPI * PFN_CreateFactory1)(REFIID, void **);
        HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        PFN_CreateFactory1 cf = dxgi ? (PFN_CreateFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
        IDXGIFactory1 *f = nullptr;
        if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void **)&f))) {
            if (FAILED(f->EnumAdapters1((UINT)index, &adapter))) adapter = nullptr;
            f->Release();
        }
    }
    const HRESULT hr = create(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (adapter) adapter->Release();
    if (FAILED(hr)) g_dev = nullptr, g_ctx = nullptr;
    return g_dev != nullptr;
}

// A texture of w x h holding pixels (made or remade as needed).
bool Upload(ID3D11Texture2D *&tex, int &tw, int &th, const uint32_t *px, int w, int h)
{
    if (tex && (tw != w || th != h)) {
        tex->Release();
        tex = nullptr;
    }
    if (!tex) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &tex))) {
            tex = nullptr;
            return false;
        }
        tw = w;
        th = h;
    }
    g_ctx->UpdateSubresource(tex, 0, nullptr, px, (UINT)w * 4, 0);
    g_ctx->Flush();
    return true;
}

int Send(Handle h, ID3D11Texture2D *tex)
{
    Texture t = { tex, kTexDirectX, kColorGamma };
    return ((PFN_SetTexture)g_o.ov[kSetTexture])(h, &t);
}

// The overlay on this connection; an error text if SteamVR refused it.
std::wstring Create(VrLinkUse &link, int pageW, int pageH)
{
    void **ov = (void **)link.Interface("FnTable:IVROverlay_028");
    if (!ov) return L"this SteamVR doesn't offer IVROverlay_028";
    Handle m = 0, th = 0;
    const int e = ((PFN_CreateDashboard)ov[kCreateDashboard])(kOverlayKey, "vrupscale", &m, &th);
    if (e != 0 || !m) return L"SteamVR refused it (" + ErrName(ov, e) + L")";
    g_o.ov = ov;
    g_o.main = m;
    g_o.thumb = th;
    ((PFN_SetWidth)ov[kSetWidth])(m, kOverlayWidthM);
    ((PFN_SetInputMethod)ov[kSetInputMethod])(m, kInputMouse);
    Vec2 scale = { { (float)pageW, (float)pageH } };
    ((PFN_SetMouseScale)ov[kSetMouseScale])(m, &scale);
    ((PFN_SetFlag)ov[kSetFlag])(m, kFlagDiscreteScroll, true);
    if (th && !g_thumb.empty() && EnsureDevice(link) && Upload(g_thumbTex, g_thumbTexW, g_thumbTexH, g_thumb.data(), g_thumbW, g_thumbH))
        Send(th, g_thumbTex);
    return std::wstring();
}

} // namespace

OverlayTick OverlayUpdate(int pageW, int pageH, std::vector<OverlayInput> &input)
{
    OverlayTick t;
    VrLinkUse link;
    if (link.busy()) {   // another thread is using the connection: as it was
        t.exists = g_o.main != 0;
        t.visible = g_o.visible;
        return t;
    }
    if (!link.ok()) {   // closed: its overlays went with it
        g_o = Overlay();
        return t;
    }
    if (link.generation() != g_o.gen) {
        g_o = Overlay();
        g_o.gen = link.generation();
    }
    if (!g_o.tried) {
        g_o.tried = true;
        t.error = Create(link, pageW, pageH);
        t.created = g_o.main != 0;
    }
    if (!g_o.main) return t;
    t.exists = true;

    PFN_PollEvent poll = (PFN_PollEvent)g_o.ov[kPollEvent];
    alignas(8) uint8_t ev[kEventSize];
    for (int i = 0; i < 256 && poll(g_o.main, ev, kEventSize); ++i) {
        uint32_t type, button;
        float x, y;
        memcpy(&type, ev, 4);
        memcpy(&x, ev + 16, 4);
        memcpy(&y, ev + 20, 4);
        memcpy(&button, ev + 24, 4);
        OverlayInput in;
        in.x = x;
        in.y = (float)pageH - y;   // OpenVR's mouse y runs up from the bottom
        switch (type) {
        case kEvMouseMove: in.kind = OverlayInput::Move; break;
        case kEvMouseDown:
            if (button != kMouseLeft) continue;
            in.kind = OverlayInput::Down;
            break;
        case kEvMouseUp:
            if (button != kMouseLeft) continue;
            in.kind = OverlayInput::Up;
            break;
        case kEvFocusLeave: in.kind = OverlayInput::Leave; break;
        case kEvScrollDiscrete:   // (xdelta, ydelta)
            in.kind = OverlayInput::Scroll;
            in.notches = y;
            break;
        default: continue;
        }
        input.push_back(in);
    }
    g_o.visible = ((PFN_IsVisible)g_o.ov[kIsVisible])(g_o.main);
    t.visible = g_o.visible;
    return t;
}

bool OverlaySetPage(const uint32_t *pixels, int w, int h, std::wstring &error)
{
    VrLinkUse link;
    if (!link.ok() || link.generation() != g_o.gen || !g_o.main) return false;
    if (!EnsureDevice(link)) {
        error = L"Direct3D 11 isn't available for it";
        return true;
    }
    if (!Upload(g_pageTex, g_pageW, g_pageH, pixels, w, h)) {
        error = L"its picture couldn't be made";
        return true;
    }
    const int e = Send(g_o.main, g_pageTex);
    if (e != 0) error = L"SteamVR refused its picture (" + ErrName(g_o.ov, e) + L")";
    return true;
}

void OverlaySetThumbnail(const uint32_t *pixels, int w, int h)
{
    g_thumb.assign(pixels, pixels + (size_t)w * h);
    g_thumbW = w;
    g_thumbH = h;
}

} // namespace app
