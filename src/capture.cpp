// Pictures of what the user sees, for the app's translation (control_shared.h: VuCapture).
//
// While [translate] is on, captureHz times a second: when the compositor draws an eye's upscaled
// texture, a part of it (captureSize pixels a side, centred on the gaze, or on straight ahead without
// live eye tracking) is copied into a texture of ours that the app opens by its shared handle. Both
// eyes are taken in the same compositor frame, so they show the same frame of the game and the app
// can tell how far away a text is from where it sits in each. One CopySubresourceRegion per eye and
// capture is all that happens here: nothing is read back, and the picture the compositor shows is not
// touched.
#include "internal.h"
#include "control_shared.h"

#include <math.h>

namespace vu {

namespace {

struct EyeTex {
    Ref<ID3D11Texture2D> tex;
    ID3D11Device *dev = nullptr;   // key only
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint64_t handle = 0;
    bool failed = false;           // this device, size and format could not be shared (not tried again)
};

// all of it under g_workLock
EyeTex g_eye[2];
VuCapture g_round = {};        // the capture being taken
bool g_open = false;
LONGLONG g_openQpc = 0;        // when it was started
LONGLONG g_lastQpc = 0;        // ... the same, kept once it is done (0: none since Translate went on)
uint32_t g_rounds = 0;
bool g_loggedOn = false, g_loggedFail = false, g_loggedIdle = false;
// Which eye the compositor draws first in a frame: the one that follows the longer pause. A capture
// starts with that eye, so that its two eyes are of one frame.
LONGLONG g_drawQpc[2] = {};    // each eye's last draw
LONGLONG g_pause[2] = {};      // how long after the other eye's last draw each eye's turn started
int g_firstEye = 0;

void DropEye(EyeTex &e) { e = EyeTex(); }

// The texture eye i's part goes into: w x h of fmt on dev, shared. Null if that can't be made.
ID3D11Texture2D *EnsureEye(int i, ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    EyeTex &e = g_eye[i];
    if (e.dev == dev && e.w == w && e.h == h && e.fmt == fmt) return e.failed ? nullptr : e.tex.Get();
    DropEye(e);
    e.dev = dev;
    e.w = w;
    e.h = h;
    e.fmt = fmt;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HANDLE shared = nullptr;
    Ref<IDXGIResource> res;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, e.tex.Put());
    if (SUCCEEDED(hr)) hr = e.tex->QueryInterface(__uuidof(IDXGIResource), (void **)res.Put());
    if (SUCCEEDED(hr)) hr = res->GetSharedHandle(&shared);
    if (FAILED(hr) || !shared) {
        e.tex.Reset();
        e.failed = true;
        if (!g_loggedFail) {
            g_loggedFail = true;
            LOGE("translate: a %ux%u %s texture can't be shared with the app (0x%08lx): nothing to read text from", w, h,
                 FormatName(fmt), (unsigned long)hr);
        }
        return nullptr;
    }
    UINT32 ours = 1;
    e.tex->SetPrivateData(kGuidOurs, sizeof(ours), &ours);
    e.handle = (uint64_t)(uintptr_t)shared;
    return e.tex.Get();
}

void Publish()
{
    g_open = false;
    if (g_round.eyes) ControlPublishCapture(g_round);
}

// Straight ahead in an eye's picture (0..1 from the top left), from the projection SteamVR reports
// for it: its raw "top" and "bottom" are the tangents at the picture's bottom and top edge, counted
// upwards (its own projection matrix puts "bottom" at the top), so the larger of the two is the top
// edge. The middle while the projection is not known.
void Ahead(int side, float *u, float *v)
{
    *u = *v = 0.5f;
    if (side != 0 && side != 1) return;
    const float *p = g_cfg.eyeProj + side * 4;
    const float l = p[0] < p[1] ? p[0] : p[1], r = p[0] < p[1] ? p[1] : p[0];
    const float lo = p[2] < p[3] ? p[2] : p[3], hi = p[2] < p[3] ? p[3] : p[2];
    if (!(r - l > 0.1f) || !(hi - lo > 0.1f) || !(l < 0 && r > 0 && lo < 0 && hi > 0)) return;
    *u = -l / (r - l);
    *v = hi / (hi - lo);
}

// Where a part of `size` pixels starts so that it is centred on t (0..1) of the picture, inside it.
UINT Place(float t, UINT origin, UINT pic, UINT size)
{
    const long c = (long)origin + lroundf(t * (float)pic) - (long)(size / 2);
    const long lo = (long)origin, hi = (long)(origin + pic - size);
    return (UINT)(c < lo ? lo : (c > hi ? hi : c));
}

void TakeEye(ID3D11DeviceContext *ctx, TexState *ts, int eye, UINT slice, int side)
{
    if (g_round.eyes & (1u << eye)) return;
    ID3D11Texture2D *proxy = ts->proxy.Get();
    if (!proxy || slice >= ts->src.ArraySize) return;
    const LinkGeo geo = ContentGeo(ts);
    if (geo.outW < 64 || geo.outH < 64) return;
    const UINT size = (UINT)g_cfg.captureSize;
    const UINT w = geo.outW < size ? geo.outW : size, h = geo.outH < size ? geo.outH : size;
    Ref<ID3D11Device> dev;
    ctx->GetDevice(dev.Put());
    ID3D11Texture2D *dst = dev ? EnsureEye(eye, dev.Get(), w, h, ts->plan.uavFmt) : nullptr;
    if (!dst) return;
    float u = 0.5f, v = 0.5f;
    const bool live = GazePoint(side, &u, &v);
    if (!live) Ahead(side, &u, &v);
    const UINT x = Place(u, geo.ox, geo.outW, w), y = Place(v, geo.oy, geo.outH, h);
    const D3D11_BOX box = { x, y, 0, x + w, y + h, 1 };
    ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, proxy, slice, &box);   // (the proxy has one mip level)

    VuCaptureEye &e = g_round.eye[eye];
    e.handle = g_eye[eye].handle;
    e.w = w;
    e.h = h;
    e.format = (uint32_t)ts->plan.uavFmt;
    e.cropX = x;
    e.cropY = y;
    e.picX = geo.ox;
    e.picY = geo.oy;
    e.picW = geo.outW;
    e.picH = geo.outH;
    e.side = side;
    g_round.eyes |= 1u << eye;
    if (live) g_round.gazeLive = 1;
}

} // namespace

void CaptureAtDraw(ID3D11DeviceContext *ctx, TexState *ts, int side, LONGLONG now)
{
    if (!g_cfg.translate || !ts || ts->failed) return;
    const LONGLONG f = g_qpf.QuadPart;
    const bool both = ts->src.ArraySize == 2;   // an array of the two eyes
    const int eye = side == 1 ? 1 : 0;
    const LONGLONG firstSeen = g_drawQpc[g_firstEye];   // the first eye's last draw before this one
    if (side >= 0) {
        if (g_drawQpc[1 - eye] > g_drawQpc[eye]) {   // the other eye was drawn since: this eye's turn starts
            g_pause[eye] = now - g_drawQpc[1 - eye];
            if (g_pause[0] && g_pause[1]) g_firstEye = g_pause[0] >= g_pause[1] ? 0 : 1;
        }
        g_drawQpc[eye] = now;
    }
    // A capture that has waited for its other eye: the same eye again a while later is the
    // compositor's next frame, so what it got (one eye) goes out as it is.
    if (g_open && now - g_openQpc > ((g_round.eyes & (1u << eye)) ? f / 250 : f / 50)) Publish();
    if (!g_open) {
        // While the app takes no captures (it is closed, or busy), one every two seconds does.
        const bool idle = g_rounds - ControlCaptureAck() >= 4;
        const double period = idle ? 2.0 : 1.0 / (double)g_cfg.captureHz;
        if (g_lastQpc != 0 && (double)(now - g_lastQpc) < period * (double)f) return;
        // (the other eye only starts one if the first has not been drawn for a while: one eye alone)
        if (side >= 0 && eye != g_firstEye && firstSeen != 0 && now - firstSeen < f / 10) return;
        if (!g_loggedOn) {
            g_loggedOn = true;
            LOGI("translate: handing the app %d px of each eye's picture, %.1f times a second", g_cfg.captureSize,
                 g_cfg.captureHz);
        }
        if (idle != g_loggedIdle) {
            g_loggedIdle = idle;
            LOGI("translate: %s", idle ? "the app isn't taking the pictures: one every two seconds until it does"
                                       : "the app is taking the pictures");
        }
        g_open = true;
        g_openQpc = g_lastQpc = now;
        g_round = VuCapture();
        g_round.round = ++g_rounds;
        g_round.qpc = now;
    }
    if (both && side < 0) {
        TakeEye(ctx, ts, 0, 0, 0);
        TakeEye(ctx, ts, 1, 1, 1);
    } else {
        TakeEye(ctx, ts, eye, both ? (UINT)eye : 0u, side);
    }
    if (g_round.eyes == 3) Publish();
}

void CaptureTick(LONGLONG now)
{
    if (g_open && now - g_openQpc > g_qpf.QuadPart / 50) Publish();
    if (!g_cfg.translate && (g_eye[0].dev || g_eye[1].dev || g_loggedOn)) {
        g_open = false;
        DropEye(g_eye[0]);
        DropEye(g_eye[1]);
        g_lastQpc = 0;
        g_drawQpc[0] = g_drawQpc[1] = g_pause[0] = g_pause[1] = 0;
        g_firstEye = 0;
        if (g_loggedOn) LOGI("translate: off");
        g_loggedOn = g_loggedFail = g_loggedIdle = false;
        VuCapture none = {};
        none.round = ++g_rounds;   // (tells the app the textures are gone)
        ControlPublishCapture(none);
    }
}

} // namespace vu
