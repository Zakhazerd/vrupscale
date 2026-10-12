// Temporal-path tests for fake_compositor.cpp (included into it; uses its helpers).
//
// The core test renders a procedural scene the way a VR app would: an 8-bit low-resolution eye
// buffer per frame, with sub-pixel "head" motion (random walk + drift) and an object moving on its
// own. Ground truth is the same scene rendered at the output resolution. FSR 1 and the temporal
// upscaler are both run over the sequence and compared against ground truth per region:
//   background  steady scene content: where accumulation should win
//   trail       area the moving object just uncovered: where ghosting would show up
//   object      the moving object's interior: tracking of independent motion
//   grating     a pattern above the input Nyquist limit: detail a single frame cannot hold

typedef int (*PFN_DebugStream)(int, uint32_t *);
static PFN_DebugStream g_debugStream;
typedef int (*PFN_DebugFlow)(int, float *, int, int *);
static PFN_DebugFlow g_debugFlow;
typedef void (*PFN_ForceFlow)(float, float, int);
static PFN_ForceFlow g_forceFlow;
typedef int (*PFN_PassTimes)(double *, const char **, int, int);
static PFN_PassTimes g_passTimes;
typedef void (*PFN_PassRuns)(int *, int *, double *);
static PFN_PassRuns g_passRuns;
typedef int (*PFN_DebugGate)(ID3D11Texture2D *, int, uint32_t *);
static PFN_DebugGate g_debugGate;

static const char *EnvStr(const char *name)
{
    static char buf[4][512];
    static int k = 0;
    char *b = buf[k++ & 3];
    DWORD n = GetEnvironmentVariableA(name, b, 512);
    return (n > 0 && n < 512) ? b : "";
}

struct V3 { float r, g, b; };

// ---------------------------------------------------------------------------------------------
// Procedural world (stored at kWorldScale world px per input px)

static const float kWorldScale = 3.0f;

struct World {
    int w = 0, h = 0;
    std::vector<float> px;   // RGB
    V3 At(float x, float y) const
    {
        x -= 0.5f; y -= 0.5f;
        int x0 = (int)floorf(x), y0 = (int)floorf(y);
        float fx = x - x0, fy = y - y0;
        auto P = [&](int xx, int yy) -> const float * {
            xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
            yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
            return &px[(size_t(yy) * w + xx) * 3];
        };
        const float *a = P(x0, y0), *b = P(x0 + 1, y0), *c = P(x0, y0 + 1), *d = P(x0 + 1, y0 + 1);
        V3 o;
        o.r = (a[0] * (1 - fx) + b[0] * fx) * (1 - fy) + (c[0] * (1 - fx) + d[0] * fx) * fy;
        o.g = (a[1] * (1 - fx) + b[1] * fx) * (1 - fy) + (c[1] * (1 - fx) + d[1] * fx) * fy;
        o.b = (a[2] * (1 - fx) + b[2] * fx) * (1 - fy) + (c[2] * (1 - fx) + d[2] * fx) * fy;
        return o;
    }
};

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float U() { return (Next() >> 8) * (1.0f / 16777216.0f); }
    float N() { float a = U() + 1e-7f, b = U(); return sqrtf(-2.0f * logf(a)) * cosf(6.2831853f * b); }
};

struct RectF { float x0, y0, x1, y1; };
static const RectF kGratingWorld = { 750, 850, 1140, 1290 };    // world px

static World MakeWorld(int w, int h)
{
    World W;
    W.w = w; W.h = h;
    W.px.resize(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = &W.px[(size_t(y) * w + x) * 3];
            p[0] = 0.55f + 0.18f * sinf(x * 0.0071f + 1.3f) * cosf(y * 0.0053f);
            p[1] = 0.50f + 0.15f * sinf(x * 0.0043f - y * 0.0061f);
            p[2] = 0.45f + 0.20f * cosf(y * 0.0079f + 0.4f);
        }
    auto Blend = [&](int x, int y, float a, V3 c) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        float *p = &W.px[(size_t(y) * w + x) * 3];
        p[0] += (c.r - p[0]) * a; p[1] += (c.g - p[1]) * a; p[2] += (c.b - p[2]) * a;
    };
    Rng rng(12345);
    if (EnvStr("VRUPSCALE_TEST_SMOOTH")[0] == '1') {   // diagnostics: smooth blobs only
        for (int i = 0; i < 400; ++i) {
            float cx = rng.U() * w, cy = rng.U() * h, rad = 15 + rng.U() * 40;
            V3 c = { rng.U(), rng.U(), rng.U() };
            for (int y = (int)(cy - 3 * rad); y < (int)(cy + 3 * rad); ++y)
                for (int x = (int)(cx - 3 * rad); x < (int)(cx + 3 * rad); ++x) {
                    float dx = x - cx, dy = y - cy;
                    Blend(x, y, 0.8f * expf(-(dx * dx + dy * dy) / (2 * rad * rad)), c);
                }
        }
        return W;
    }
    // thin lines at random angles (0.4 .. 1.3 input px wide)
    for (int i = 0; i < 260; ++i) {
        float x0 = rng.U() * w, y0 = rng.U() * h, ang = rng.U() * 3.14159f, len = 30 + rng.U() * 260;
        float x1 = x0 + cosf(ang) * len, y1 = y0 + sinf(ang) * len, wd = 1.2f + rng.U() * 2.8f;
        bool dark = rng.U() < 0.5f;
        V3 c = dark ? V3{ 0.05f + 0.1f * rng.U(), 0.05f + 0.1f * rng.U(), 0.1f * rng.U() }
                    : V3{ 0.85f + 0.15f * rng.U(), 0.8f + 0.2f * rng.U(), 0.7f + 0.3f * rng.U() };
        int bx0 = (int)floorf(fminf(x0, x1) - wd), bx1 = (int)ceilf(fmaxf(x0, x1) + wd);
        int by0 = (int)floorf(fminf(y0, y1) - wd), by1 = (int)ceilf(fmaxf(y0, y1) + wd);
        float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                float px = x + 0.5f - x0, py = y + 0.5f - y0;
                float t = fmaxf(0.0f, fminf(1.0f, (px * dx + py * dy) / l2));
                float ex = px - t * dx, ey = py - t * dy;
                float d = sqrtf(ex * ex + ey * ey);
                Blend(x, y, fmaxf(0.0f, fminf(1.0f, 0.5f * wd + 0.5f - d)), c);
            }
    }
    // tiny "text": 3x5 glyphs, cell 2 world px (0.67 input px) and 4 world px (1.33 input px)
    for (int pass = 0; pass < 2; ++pass) {
        int cell = pass ? 4 : 2, gx0 = pass ? 150 : 150, gy0 = pass ? 700 : 150;
        int cols = pass ? 40 : 70, rows = pass ? 10 : 22;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                uint32_t bits = rng.Next();
                for (int gy = 0; gy < 5; ++gy)
                    for (int gx = 0; gx < 3; ++gx) {
                        if (!((bits >> (gy * 3 + gx)) & 1)) continue;
                        int x = gx0 + c * cell * 4 + gx * cell, y = gy0 + r * cell * 7 + gy * cell;
                        for (int yy = 0; yy < cell; ++yy)
                            for (int xx = 0; xx < cell; ++xx) Blend(x + xx, y + yy, 1.0f, V3{ 0.08f, 0.08f, 0.12f });
                    }
            }
    }
    // zone plate (chirp up to 0.28 cycles / world px = 0.84 cycles / input px)
    {
        const float cx = 1100, cy = 520, R = 260, k = 0.28f / R;
        for (int y = (int)(cy - R); y < (int)(cy + R); ++y)
            for (int x = (int)(cx - R); x < (int)(cx + R); ++x) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, r2 = dx * dx + dy * dy;
                if (r2 > R * R) continue;
                float v = 0.5f + 0.45f * cosf(3.14159265f * k * r2);
                Blend(x, y, 1.0f, V3{ v, v, v });
            }
    }
    // grating at 0.55 cycles / input px (above the input Nyquist limit of 0.5, below the output's 0.65)
    {
        const float f = 0.55f / kWorldScale, ca = cosf(0.5236f), sa = sinf(0.5236f);
        for (int y = (int)kGratingWorld.y0; y < (int)kGratingWorld.y1; ++y)
            for (int x = (int)kGratingWorld.x0; x < (int)kGratingWorld.x1; ++x) {
                float v = 0.5f + 0.4f * sinf(6.2831853f * f * ((x + 0.5f) * ca + (y + 0.5f) * sa));
                Blend(x, y, 1.0f, V3{ v, 0.9f * v + 0.05f, 0.8f * v + 0.1f });
            }
    }
    return W;
}

// ---------------------------------------------------------------------------------------------
// Scene: camera offset (input px) + an independently moving textured square, two eyes.

struct SceneFrame {
    float ox, oy;       // camera offset, input px
    float objX, objY;   // object top-left, input px (screen space)
};
static const float kObjSize = 64.0f;
static const float kDisparityBg = 6.4f, kDisparityObj = 9.1f;   // eye 1 offsets, input px

static V3 ObjectColor(float u, float v)   // object-local, input px
{
    bool chk = (((int)floorf(u / 3.0f) + (int)floorf(v / 3.0f)) & 1) != 0;
    V3 c = chk ? V3{ 0.9f, 0.25f, 0.2f } : V3{ 0.2f, 0.3f, 0.85f };
    float s = fmodf(u + v * 0.5f, 7.0f);
    if (s < 1.2f) c = V3{ 0.95f, 0.95f, 0.3f };
    if (u < 2 || v < 2 || u > kObjSize - 2 || v > kObjSize - 2) c = V3{ 0.05f, 0.05f, 0.05f };
    return c;
}

static V3 SceneAt(const World &Wd, const SceneFrame &f, int eye, float sx, float sy)
{
    float ox = eye ? kDisparityObj : 0.0f;
    float u = sx - (f.objX + ox), v = sy - f.objY;
    if (u >= 0 && v >= 0 && u < kObjSize && v < kObjSize) return ObjectColor(u, v);
    float bx = eye ? kDisparityBg : 0.0f;
    return Wd.At((sx + f.ox + bx) * kWorldScale, (sy + f.oy) * kWorldScale);
}

// Renders w x h pixels whose footprint is `scale` input px each (1 = input buffer), 4x4 supersampled.
static Image RenderScene(const World &Wd, const SceneFrame &f, int eye, int w, int h, float scale)
{
    Image im;
    im.w = w; im.h = h;
    im.px.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    V3 c = SceneAt(Wd, f, eye, (x + (i + 0.5f) / 4.0f) * scale, (y + (j + 0.5f) / 4.0f) * scale);
                    r += c.r; g += c.g; b += c.b;
                }
            float *p = im.at(x, y);
            p[0] = r / 16; p[1] = g / 16; p[2] = b / 16; p[3] = 1.0f;
        }
    return Quantize8(im);
}

// ---------------------------------------------------------------------------------------------
// Side-by-side "backbuffer": each eye is drawn into its own viewport, as a compositor would.

struct EyeTarget {
    int w = 0, h = 0;
    ID3D11Texture2D *rt = nullptr, *staging = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11Query *dj = nullptr, *t0 = nullptr, *t1 = nullptr;
    void Release() { SafeRelease(rt); SafeRelease(staging); SafeRelease(rtv); SafeRelease(dj); SafeRelease(t0); SafeRelease(t1); w = h = 0; }
};

static void EnsureEyeTarget(Gfx &g, EyeTarget &t, int w, int h)
{
    if (t.rt && t.w == w && t.h == h) return;
    t.Release();
    t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 2 * w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    g.dev->CreateTexture2D(&td, nullptr, &t.rt);
    g.dev->CreateRenderTargetView(t.rt, nullptr, &t.rtv);
    td.Width = w; td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g.dev->CreateTexture2D(&td, nullptr, &t.staging);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
    g.dev->CreateQuery(&qd, &t.dj);
    g.dev->CreateQuery(&qt, &t.t0);
    g.dev->CreateQuery(&qt, &t.t1);
}

// Binds `view` to PS t0 and draws eye `eye` into its half of the backbuffer. Returns that eye's
// pixels (point sampled, so they are exactly the proxy contents when the sizes match).
// uMax, vMax: the part of the texture the compositor samples (OpenVR texture bounds), from the top left.
static Image DrawEye(Gfx &g, EyeTarget &t, ID3D11ShaderResourceView *view, int eye, double *gpuMs = nullptr,
                     bool readback = true, float uMax = 1.0f, float vMax = 1.0f)
{
    ID3D11DeviceContext *rc = g.rc;
    float cbData[8] = { (float)t.w, (float)t.h, (float)(eye * t.w), 0.0f, 0.0f, 0.0f, uMax, vMax };
    rc->UpdateSubresource(g.cb, 0, nullptr, cbData, 0, 0);
    rc->OMSetRenderTargets(1, &t.rtv, nullptr);
    D3D11_VIEWPORT vp = { (float)(eye * t.w), 0, (float)t.w, (float)t.h, 0, 1 };
    rc->RSSetViewports(1, &vp);
    rc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    rc->IASetInputLayout(nullptr);
    rc->VSSetShader(g.vs, nullptr, 0);
    rc->PSSetShader(g.ps, nullptr, 0);
    rc->PSSetSamplers(0, 1, &g.point);
    rc->PSSetConstantBuffers(0, 1, &g.cb);
    // Timestamps go on the immediate context: around the draw, or around the command list.
    if (gpuMs && !g.deferred) { g.ctx->Begin(t.dj); g.ctx->End(t.t0); }
    rc->PSSetShaderResources(0, 1, &view);
    rc->Draw(3, 0);
    if (gpuMs && !g.deferred) { g.ctx->End(t.t1); g.ctx->End(t.dj); }
    ID3D11ShaderResourceView *nullView = nullptr;
    rc->PSSetShaderResources(0, 1, &nullView);
    ID3D11RenderTargetView *nullRtv = nullptr;
    rc->OMSetRenderTargets(1, &nullRtv, nullptr);
    if (gpuMs && g.deferred) { g.ctx->Begin(t.dj); g.ctx->End(t.t0); }
    Submit(g);
    if (gpuMs && g.deferred) { g.ctx->End(t.t1); g.ctx->End(t.dj); }

    Image im;
    if (readback) {
        D3D11_BOX box = { (UINT)(eye * t.w), 0, 0, (UINT)(eye * t.w + t.w), (UINT)t.h, 1 };
        g.ctx->CopySubresourceRegion(t.staging, 0, 0, 0, 0, t.rt, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        im.w = t.w; im.h = t.h;
        im.px.resize(size_t(t.w) * t.h * 4);
        if (SUCCEEDED(g.ctx->Map(t.staging, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < t.h; ++y) {
                const uint8_t *row = (const uint8_t *)m.pData + size_t(y) * m.RowPitch;
                for (int x = 0; x < t.w * 4; ++x) im.px[size_t(y) * t.w * 4 + x] = row[x] / 255.0f;
            }
            g.ctx->Unmap(t.staging, 0);
        }
    }
    if (gpuMs) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 a = 0, b = 0;
        while (g.ctx->GetData(t.dj, &dj, sizeof(dj), 0) == S_FALSE) Sleep(0);
        while (g.ctx->GetData(t.t0, &a, sizeof(a), 0) == S_FALSE) Sleep(0);
        while (g.ctx->GetData(t.t1, &b, sizeof(b), 0) == S_FALSE) Sleep(0);
        *gpuMs = (!dj.Disjoint && dj.Frequency && b >= a) ? double(b - a) * 1000.0 / double(dj.Frequency) : -1.0;
    }
    return im;
}

static void UploadSlices(Gfx &g, ID3D11Texture2D *t, const Image &s0, const Image &s1)
{
    const Image *sl[2] = { &s0, &s1 };
    std::vector<uint8_t> bytes(size_t(s0.w) * s0.h * 4);
    for (int s = 0; s < 2; ++s) {
        for (size_t i = 0; i < bytes.size(); ++i) {
            float v = sl[s]->px[i];
            bytes[i] = (uint8_t)lroundf((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f);
        }
        g.ctx->UpdateSubresource(t, D3D11CalcSubresource(0, s, 1), nullptr, bytes.data(), s0.w * 4, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Region metrics

struct IRect { int x0, y0, x1, y1; };
static IRect Grow(IRect r, int m) { return { r.x0 - m, r.y0 - m, r.x1 + m, r.y1 + m }; }
static bool In(const IRect &r, int x, int y) { return x >= r.x0 && y >= r.y0 && x < r.x1 && y < r.y1; }

struct Acc {
    double se = 0;
    size_t n = 0;
    void Add(const float *a, const float *b)
    {
        for (int c = 0; c < 3; ++c) { double e = double(a[c]) - b[c]; se += e * e; }
        n += 3;
    }
    double Psnr() const { return n ? (se > 0 ? 10.0 * log10(double(n) / se) : 99.0) : 0.0; }
};

struct FlowStats {
    double gErr = 0, lErr = 0, bgConf = 0, objConf = 0, objGlobal = 0;
    size_t gN = 0, lN = 0;
    double bgErrSum = 0, objErrSum = 0;
    size_t bgN = 0, objN = 0, bgLt01 = 0, bgLt025 = 0, bgLt05 = 0, objLt05 = 0;
    std::vector<double> bgErrs;
};
struct SeqResult {
    Acc bg, trail, obj, grating, all;
    Acc flicker;   // background: how much the error changes from frame to frame (shimmer), motion-compensated
    Acc fovCore, fovBand, fovOuter;   // foveation regions (when g_fovBand > 0)
    double gpuMs = 0;
    int gpuN = 0;
    Image lastOut, lastGt;
    FlowStats flow;
};

static int g_fovBand = 0;
static int g_fovRect[4] = {};   // x0, y0, x1, y1

// The output region the DLL keeps history for at a given temporalArea (mirrors PlanRegion).
static void SetFovRegion(int ow, int oh, float area)
{
    auto side = [&](int out) {
        if (area >= 0.999f) return out;
        int r = (int)ceilf((float)out * area / 16.0f) * 16;
        return r + 32 >= out ? out : r;
    };
    int rw = side(ow), rh = side(oh);
    g_fovRect[0] = (ow - rw) / 2; g_fovRect[1] = (oh - rh) / 2;
    g_fovRect[2] = g_fovRect[0] + rw; g_fovRect[3] = g_fovRect[1] + rh;
    g_fovBand = std::max(16, (int)lroundf(0.02f * (float)std::min(ow, oh)));
}

static IRect ObjRectOut(const SceneFrame &f, int eye, float s)
{
    float ox = eye ? kDisparityObj : 0.0f;
    return { (int)floorf((f.objX + ox) * s), (int)floorf(f.objY * s), (int)ceilf((f.objX + ox + kObjSize) * s),
             (int)ceilf((f.objY + kObjSize) * s) };
}

static IRect GratingRectOut(const SceneFrame &f, int eye, float s)
{
    float bx = eye ? kDisparityBg : 0.0f;
    return { (int)ceilf((kGratingWorld.x0 / kWorldScale - f.ox - bx) * s) + 3, (int)ceilf((kGratingWorld.y0 / kWorldScale - f.oy) * s) + 3,
             (int)floorf((kGratingWorld.x1 / kWorldScale - f.ox - bx) * s) - 3, (int)floorf((kGratingWorld.y1 / kWorldScale - f.oy) * s) - 3 };
}

struct Sequence {
    int w = 0, h = 0, ow = 0, oh = 0, frames = 0;
    std::vector<SceneFrame> f;
    std::vector<Image> in[2], gt[2];
};

static Sequence MakeSequence(const World &Wd, int w, int h, int ow, int oh, int frames, bool moving, uint32_t seed)
{
    Sequence q;
    q.w = w; q.h = h; q.ow = ow; q.oh = oh; q.frames = frames;
    Rng rng(seed);
    SceneFrame cur = { 40.0f, 30.0f, 60.0f, 250.0f };
    for (int i = 0; i < frames; ++i) {
        q.f.push_back(cur);
        if (moving) {
            static const float k = EnvStr("VRUPSCALE_TEST_MOTION")[0] ? (float)atof(EnvStr("VRUPSCALE_TEST_MOTION")) : 1.0f;
            cur.ox += (0.37f + 0.45f * rng.N()) * k;   // drift + head micro-motion (sub-pixel)
            cur.oy += (0.13f + 0.45f * rng.N()) * k;
            cur.objX += 3.3f;                    // object moves on its own
            cur.objY -= 1.7f;
        }
    }
    const float r = (float)w / (float)ow;
    for (int i = 0; i < frames; ++i)
        for (int e = 0; e < 2; ++e) {
            q.in[e].push_back(RenderScene(Wd, q.f[i], e, w, h, 1.0f));
            q.gt[e].push_back(RenderScene(Wd, q.f[i], e, ow, oh, r));
        }
    return q;
}

// Runs the sequence through the compositor path; both eyes per frame, metrics from frame `warm` on.
static void MeasureFlow(const Sequence &q, int i, FlowStats &fs)
{
    if (!g_debugFlow || i == 0) return;
    int dims[3] = {};
    int need = g_debugFlow(0, nullptr, 0, dims);
    if (need <= 0) return;
    std::vector<float> fl(need);
    if (g_debugFlow(0, fl.data(), need, dims) != need) return;
    const int gw = dims[0], gh = dims[1];
    const SceneFrame &a = q.f[i], &b = q.f[i - 1];
    const float tbx = a.ox - b.ox, tby = a.oy - b.oy;                     // background: prev = cur + (o_t - o_t-1)
    const float tox = -(a.objX - b.objX), toy = -(a.objY - b.objY);       // object: prev = cur - its motion
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const float *v = &fl[((size_t)y * gw + x) * 4];
            float cx = (x + 0.5f) * 4.0f - 0.5f, cy = (y + 0.5f) * 4.0f - 0.5f;
            if (cx < 12 || cy < 12 || cx > q.w - 12 || cy > q.h - 12) continue;
            bool inObj = cx > a.objX + 6 && cy > a.objY + 6 && cx < a.objX + kObjSize - 6 && cy < a.objY + kObjSize - 6;
            bool nearObj = cx > a.objX - 10 && cy > a.objY - 10 && cx < a.objX + kObjSize + 10 && cy < a.objY + kObjSize + 10;
            bool nearPrev = cx > b.objX - 10 && cy > b.objY - 10 && cx < b.objX + kObjSize + 10 && cy < b.objY + kObjSize + 10;
            if (inObj) {
                double e = hypot(v[0] - tox, v[1] - toy);
                fs.objErrSum += e; fs.objN++; if (e < 0.5) fs.objLt05++;
                fs.objConf += v[2]; fs.objGlobal += v[3];
            } else if (!nearObj && !nearPrev) {
                double e = hypot(v[0] - tbx, v[1] - tby);
                fs.bgConf += v[2];
                if (v[3] > 0.5) { fs.gErr += e; fs.gN++; } else { fs.lErr += e; fs.lN++; }
                fs.bgErrSum += e; fs.bgN++;
                if (e < 0.1) fs.bgLt01++;
                if (e < 0.25) fs.bgLt025++;
                if (e < 0.5) fs.bgLt05++;
                fs.bgErrs.push_back(e);
            }
        }
}

static void PrintFlow(const char *tag, FlowStats &fs)
{
    if (!fs.bgN) return;
    std::sort(fs.bgErrs.begin(), fs.bgErrs.end());
    printf("         flow (%s): background mean err %.3f px, median %.3f, <0.1px %.0f%%, <0.25px %.0f%%, <0.5px %.0f%% | "
           "object mean err %.3f px, <0.5px %.0f%%\n", tag, fs.bgErrSum / fs.bgN, fs.bgErrs[fs.bgErrs.size() / 2],
           100.0 * fs.bgLt01 / fs.bgN, 100.0 * fs.bgLt025 / fs.bgN, 100.0 * fs.bgLt05 / fs.bgN,
           fs.objN ? fs.objErrSum / fs.objN : 0.0, fs.objN ? 100.0 * fs.objLt05 / fs.objN : 0.0);
    printf("         flow split: background %.0f%% global (err %.3f px) / local (err %.3f px), mean conf %.2f | object conf %.2f, global %.0f%%\n",
           100.0 * fs.gN / fs.bgN, fs.gN ? fs.gErr / fs.gN : 0.0, fs.lN ? fs.lErr / fs.lN : 0.0, fs.bgConf / fs.bgN,
           fs.objN ? fs.objConf / fs.objN : 0.0, fs.objN ? 100.0 * fs.objGlobal / fs.objN : 0.0);
}

static void (*g_frameHook)(int frame) = nullptr;   // called before each frame of RunSequence (gaze test)

static SeqResult RunSequence(Gfx &g, EyeTarget &tgt, const Sequence &q, ID3D11Texture2D *app,
                             ID3D11ShaderResourceView *views[2], int warm, bool flowDiag = false)
{
    SeqResult res;
    const float s = (float)q.ow / (float)q.w;
    Image prevOut, prevGt;   // eye 0, the frame before
    const bool trueFlow = flowDiag && g_forceFlow && EnvStr("VRUPSCALE_TEST_TRUEFLOW")[0] == '1';
    for (int i = 0; i < q.frames; ++i) {
        if (trueFlow && i > 0) g_forceFlow(q.f[i].ox - q.f[i - 1].ox, q.f[i].oy - q.f[i - 1].oy, 1);
        if (g_frameHook) g_frameHook(i);
        UploadSlices(g, app, q.in[0][i], q.in[1][i]);
        for (int e = 0; e < 2; ++e) {
            double ms = 0;
            Image out = DrawEye(g, tgt, views[e], e, &ms);
            if (ms > 0) { res.gpuMs += ms; res.gpuN++; }
            if (i < warm) continue;
            const Image &gt = q.gt[e][i];
            IRect obj = ObjRectOut(q.f[i], e, s), objCore = Grow(obj, -4), objNear = Grow(obj, 6);
            IRect trail = objNear;
            for (int k = (i >= 8 ? i - 8 : 0); k < i; ++k) {
                IRect o = Grow(ObjRectOut(q.f[k], e, s), 6);
                trail = { std::min(trail.x0, o.x0), std::min(trail.y0, o.y0), std::max(trail.x1, o.x1), std::max(trail.y1, o.y1) };
            }
            IRect grat = GratingRectOut(q.f[i], e, s);
            for (int y = 4; y < q.oh - 4; ++y)
                for (int x = 4; x < q.ow - 4; ++x) {
                    const float *a = out.at(x, y), *b = gt.at(x, y);
                    res.all.Add(a, b);
                    if (g_fovBand > 0) {
                        const IRect reg = { g_fovRect[0], g_fovRect[1], g_fovRect[2], g_fovRect[3] };
                        if (In(Grow(reg, -g_fovBand - 2), x, y)) res.fovCore.Add(a, b);
                        else if (In(reg, x, y)) res.fovBand.Add(a, b);
                        else if (!In(Grow(reg, 2), x, y)) res.fovOuter.Add(a, b);
                    }
                    if (In(objCore, x, y)) res.obj.Add(a, b);
                    else if (In(objNear, x, y)) {}
                    else if (In(trail, x, y)) res.trail.Add(a, b);
                    else {
                        res.bg.Add(a, b);
                        if (In(grat, x, y)) res.grating.Add(a, b);
                    }
                }
            // Shimmer: the same scene point's error now and one frame ago (the background moved by the
            // head offset; the previous error is sampled there, bilinearly).
            if (e == 0 && i > warm && prevOut.w == out.w) {
                const float dx = (q.f[i].ox - q.f[i - 1].ox) * s, dy = (q.f[i].oy - q.f[i - 1].oy) * s;
                IRect obj = Grow(ObjRectOut(q.f[i], e, s), 10), objPrev = Grow(ObjRectOut(q.f[i - 1], e, s), 10);
                for (int y = 8; y < q.oh - 8; ++y)
                    for (int x = 8; x < q.ow - 8; ++x) {
                        if (In(obj, x, y) || In(objPrev, x, y)) continue;
                        const float px = x + dx, py = y + dy;
                        const int x0 = (int)floorf(px), y0 = (int)floorf(py);
                        if (x0 < 1 || y0 < 1 || x0 >= q.ow - 2 || y0 >= q.oh - 2) continue;
                        const float fx = px - x0, fy = py - y0;
                        float e1[3], zero[3] = { 0, 0, 0 };
                        for (int c = 0; c < 3; ++c) {
                            auto E = [&](int xx, int yy) { return prevOut.at(xx, yy)[c] - prevGt.at(xx, yy)[c]; };
                            const float pe = (E(x0, y0) * (1 - fx) + E(x0 + 1, y0) * fx) * (1 - fy) + (E(x0, y0 + 1) * (1 - fx) + E(x0 + 1, y0 + 1) * fx) * fy;
                            e1[c] = (out.at(x, y)[c] - gt.at(x, y)[c]) - pe;
                        }
                        res.flicker.Add(e1, zero);
                    }
            }
            if (e == 0) { prevOut = out; prevGt = gt; }
            if (i == q.frames - 1 && e == 0) { res.lastOut = out; res.lastGt = gt; }
        }
        if (flowDiag) MeasureFlow(q, i, res.flow);
        Sleep(4);   // > dedupeMs: the next draw of a texture is a new compositor frame
    }
    return res;
}

static bool AllFinite(const Image &im)
{
    for (float v : im.px)
        if (!(v >= 0.0f && v <= 1.0f)) return false;
    return true;
}

// Releases are deferred twice: D3D11 may destroy objects only after a flush, and the DLL frees its
// per-texture state on its next housekeeping tick. Run both so the next test starts clean.
static void Settle(Gfx &g)
{
    ClearAll(g);
    for (int r = 0; r < 3; ++r) {
        g.ctx->Flush();
        for (int i = 0; i < 3; ++i) Pump(g);
    }
}

static int StreamCount() { uint32_t tmp[32] = {}; return g_debugStream ? g_debugStream(-1, tmp) : -1; }

// ---------------------------------------------------------------------------------------------

typedef void (*PFN_Hotkey)(int);
static PFN_Hotkey g_hotkey;

static void LoadDebugExports()
{
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    g_hotkey = (PFN_Hotkey)GetProcAddress(dxgi, "VrUpscaleDebugHotkey");
    g_debugStream = (PFN_DebugStream)GetProcAddress(dxgi, "VrUpscaleDebugStream");
    g_debugFlow = (PFN_DebugFlow)GetProcAddress(dxgi, "VrUpscaleDebugFlow");
    g_forceFlow = (PFN_ForceFlow)GetProcAddress(dxgi, "VrUpscaleDebugForceFlow");
    g_passTimes = (PFN_PassTimes)GetProcAddress(dxgi, "VrUpscaleDebugPassTimes");
    g_passRuns = (PFN_PassRuns)GetProcAddress(dxgi, "VrUpscaleDebugPassRuns");
    g_debugGate = (PFN_DebugGate)GetProcAddress(dxgi, "VrUpscaleDebugGate");
}

// Foveated temporal (the default): history only in a centred rectangle, this frame alone outside.
// Its centre must match whole-image temporal, its periphery must be about as good as FSR 1, and the
// band where the two meet must not be worse than either side (no seam).
static void RunFoveationTest(Gfx &g, EyeTarget &tgt, const Sequence &mov, const char *iniBase, const char *reqShared, float F,
                             UINT misc, float area, const SeqResult &rf, const SeqResult &rt, const char *outDir)
{
    printf("[temporal: foveated (temporalArea=%.1f: history in the centre %dx%d, blend band %d px)]\n", area,
           g_fovRect[2] - g_fovRect[0], g_fovRect[3] - g_fovRect[1], g_fovBand);
    std::vector<Image> blank = { mov.in[0][0], mov.in[1][0] };
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, blank, misc);
    ID3D11ShaderResourceView *views[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                           MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    char ini[2048];
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=%.2f\n",
             iniBase, reqShared, F, area);
    WriteIni(ini);
    Pump(g);
    SeqResult rv = RunSequence(g, tgt, mov, app, views, 8);
    printf("         region        FSR 1   temporal   foveated   (PSNR vs ground truth, dB)\n");
    printf("         centre       %6.2f    %6.2f     %6.2f\n", rf.fovCore.Psnr(), rt.fovCore.Psnr(), rv.fovCore.Psnr());
    printf("         blend band   %6.2f    %6.2f     %6.2f\n", rf.fovBand.Psnr(), rt.fovBand.Psnr(), rv.fovBand.Psnr());
    printf("         periphery    %6.2f    %6.2f     %6.2f\n", rf.fovOuter.Psnr(), rt.fovOuter.Psnr(), rv.fovOuter.Psnr());
    CHECK(AllFinite(rv.lastOut), "foveated output is finite and in range");
    CHECK(rv.fovCore.Psnr() > rt.fovCore.Psnr() - 0.3, "centre as good as whole-image temporal (%+.2f dB)",
          rv.fovCore.Psnr() - rt.fovCore.Psnr());
    CHECK(rv.fovOuter.Psnr() > rf.fovOuter.Psnr() - 1.0, "periphery close to FSR 1 (%+.2f dB)", rv.fovOuter.Psnr() - rf.fovOuter.Psnr());
    // the band's content is its own: compare it with the same pixels under FSR 1 and whole-image temporal
    CHECK(rv.fovBand.Psnr() > std::min(rf.fovBand.Psnr(), rt.fovBand.Psnr()) - 0.3,
          "no seam: blend band %.2f dB (FSR 1 %.2f, whole-image temporal %.2f there)", rv.fovBand.Psnr(), rf.fovBand.Psnr(),
          rt.fovBand.Psnr());
    // Bilinear edges: the same centre, and outside it what a plain bilinear upscale gives
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=bilinear\nupscaleFactor=%.2f\nsharpness=0\n[temporal]\ntemporalArea=1\n",
             iniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    SeqResult rb = RunSequence(g, tgt, mov, app, views, 8);
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=%.2f\nbilinearEdges=1\n",
             iniBase, reqShared, F, area);
    WriteIni(ini);
    Pump(g);
    SeqResult rvb = RunSequence(g, tgt, mov, app, views, 8);
    printf("         Bilinear edges: centre %.2f dB, blend band %.2f dB, periphery %.2f dB (plain bilinear there: %.2f)\n",
           rvb.fovCore.Psnr(), rvb.fovBand.Psnr(), rvb.fovOuter.Psnr(), rb.fovOuter.Psnr());
    CHECK(AllFinite(rvb.lastOut) && fabs(rvb.fovCore.Psnr() - rv.fovCore.Psnr()) < 0.05,
          "Bilinear edges: the centre is unchanged (%+.3f dB)", rvb.fovCore.Psnr() - rv.fovCore.Psnr());
    CHECK(fabs(rvb.fovOuter.Psnr() - rb.fovOuter.Psnr()) < 0.15,
          "Bilinear edges: outside the area it is a bilinear upscale (%.2f dB, bilinear mode %.2f dB)", rvb.fovOuter.Psnr(),
          rb.fovOuter.Psnr());
    CHECK(rvb.fovBand.Psnr() > std::min(rb.fovBand.Psnr(), rv.fovBand.Psnr()) - 0.3,
          "Bilinear edges: no seam (blend band %.2f dB; bilinear %.2f, Catmull-Rom edges %.2f there)", rvb.fovBand.Psnr(),
          rb.fovBand.Psnr(), rv.fovBand.Psnr());
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\t_foveated.ppm", outDir); SavePpm(rv.lastOut, path);
    snprintf(path, sizeof(path), "%s\\t_foveated_bilinear.ppm", outDir); SavePpm(rvb.lastOut, path);
    // one tinted frame for a look at the layout (magenta centre, blue periphery)
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\ndebugTint=1\n[temporal]\ntemporalArea=%.2f\n",
             iniBase, reqShared, F, area);
    WriteIni(ini);
    Pump(g);
    UploadSlices(g, app, mov.in[0][3], mov.in[1][3]);
    Image tinted = DrawEye(g, tgt, views[0], 0, nullptr, true);
    snprintf(path, sizeof(path), "%s\\t_foveated_tint.ppm", outDir); SavePpm(tinted, path);
    ClearAll(g);
    SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
    Settle(g);
    g_fovBand = 0;
}

// SteamVR's compositor draws every texture of the app's swapchain (3 per eye) every frame, into
// that eye's viewport of a side-by-side target, while new app frames arrive more slowly. Each
// texture must be upscaled only when its content is new, and an eye's textures share one history.
// ---------------------------------------------------------------------------------------------
// FSR 1 mode uses the same area: FSR 1 inside it, the cheap spatial upscale outside, the same band.

static bool InRect(int x, int y, const int r[4], int grow) { return x >= r[0] - grow && y >= r[1] - grow && x < r[2] + grow && y < r[3] + grow; }

static void RunFoveatedFsrTest(Gfx &g, EyeTarget &tgt, const Sequence &mov, const char *iniBase, const char *reqShared, float F,
                               UINT misc, float area, const SeqResult &rf, const char *outDir)
{
    SetFovRegion(mov.ow, mov.oh, area);
    printf("[FSR 1 in the area only (area %.1f: FSR 1 in the centre %dx%d, blend band %d px)]\n", area, g_fovRect[2] - g_fovRect[0],
           g_fovRect[3] - g_fovRect[1], g_fovBand);
    std::vector<Image> blank = { mov.in[0][0], mov.in[1][0] };
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, blank, misc);
    ID3D11ShaderResourceView *views[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                           MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    char ini[2048];
    auto Ini = [&](const char *mode, float a, const char *upscaleExtra, const char *temporalExtra) {
        snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=%s\nupscaleFactor=%.2f\nsharpness=0.5\n%s[temporal]\ntemporalArea=%.2f\n%s",
                 iniBase, reqShared, mode, F, upscaleExtra, a, temporalExtra);
        WriteIni(ini);
        Pump(g);
    };
    Ini("fsr", area, "", "");
    SeqResult rv = RunSequence(g, tgt, mov, app, views, 8);
    printf("         region        FSR 1 (whole)   FSR 1 in the area   (PSNR vs ground truth, dB)\n");
    printf("         centre        %6.2f          %6.2f\n", rf.fovCore.Psnr(), rv.fovCore.Psnr());
    printf("         blend band    %6.2f          %6.2f\n", rf.fovBand.Psnr(), rv.fovBand.Psnr());
    printf("         periphery     %6.2f          %6.2f\n", rf.fovOuter.Psnr(), rv.fovOuter.Psnr());
    CHECK(AllFinite(rv.lastOut), "FSR 1 in the area: output finite and in range");
    CHECK(fabs(rv.fovCore.Psnr() - rf.fovCore.Psnr()) < 0.02, "the centre is FSR 1 as over the whole image (%+.3f dB)",
          rv.fovCore.Psnr() - rf.fovCore.Psnr());
    CHECK(rv.fovOuter.Psnr() > rf.fovOuter.Psnr() - 1.0, "periphery close to FSR 1 (%+.2f dB)", rv.fovOuter.Psnr() - rf.fovOuter.Psnr());
    CHECK(rv.fovBand.Psnr() > std::min(rf.fovBand.Psnr(), rv.fovOuter.Psnr()) - 0.3, "no seam: blend band %.2f dB", rv.fovBand.Psnr());

    // one frame three ways: inside the area exactly FSR 1, outside exactly temporal's spatial edges
    auto Shot = [&](const char *mode, float a, const char *ux, const char *tx) {
        Ini(mode, a, ux, tx);
        UploadSlices(g, app, mov.in[0][5], mov.in[1][5]);
        DrawEye(g, tgt, views[1], 1, nullptr, false);
        return DrawEye(g, tgt, views[0], 0, nullptr, true);
    };
    const Image whole = Shot("fsr", 1.0f, "", ""), fov = Shot("fsr", area, "", ""), temporal = Shot("temporal", area, "", "");
    double inMax = 0, outMax = 0;
    for (int y = 0; y < mov.oh; ++y)
        for (int x = 0; x < mov.ow; ++x) {
            const float *a = fov.at(x, y), *b = whole.at(x, y), *t = temporal.at(x, y);
            const int core[4] = { g_fovRect[0] + g_fovBand + 2, g_fovRect[1] + g_fovBand + 2, g_fovRect[2] - g_fovBand - 2, g_fovRect[3] - g_fovBand - 2 };
            for (int c = 0; c < 3; ++c) {
                if (InRect(x, y, core, 0)) inMax = std::max(inMax, (double)fabsf(a[c] - b[c]));
                if (!InRect(x, y, g_fovRect, 2)) outMax = std::max(outMax, (double)fabsf(a[c] - t[c]));
            }
        }
    CHECK(inMax <= 1.5 / 255.0, "inside the area, exactly what FSR 1 gives over the whole image (largest difference %.1f/255)", inMax * 255.0);
    CHECK(outMax <= 1.5 / 255.0, "outside it, exactly Temporal's spatial upscale of the edges (largest difference %.1f/255)", outMax * 255.0);

    // Bilinear edges: outside the area what a plain bilinear upscale gives
    Ini("bilinear", 1.0f, "", "");
    SeqResult rb = RunSequence(g, tgt, mov, app, views, 8);
    Ini("fsr", area, "", "bilinearEdges=1\n");
    SeqResult rvb = RunSequence(g, tgt, mov, app, views, 8);
    printf("         Bilinear edges: centre %.2f dB, blend band %.2f dB, periphery %.2f dB (plain bilinear there: %.2f)\n",
           rvb.fovCore.Psnr(), rvb.fovBand.Psnr(), rvb.fovOuter.Psnr(), rb.fovOuter.Psnr());
    CHECK(fabs(rvb.fovCore.Psnr() - rv.fovCore.Psnr()) < 0.02 && fabs(rvb.fovOuter.Psnr() - rb.fovOuter.Psnr()) < 0.15,
          "Bilinear edges: the same centre, bilinear outside (%.2f dB, bilinear mode %.2f dB)", rvb.fovOuter.Psnr(), rb.fovOuter.Psnr());

    // debug tint: magenta in the area, blue outside it
    const Image plain = Shot("fsr", area, "", ""), tinted = Shot("fsr", area, "debugTint=1\n", "");
    auto Ratio = [&](const Image &im, int x0, int y0, int x1, int y1, int a, int b) {
        double sa = 0, sb = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) { sa += im.at(x, y)[a]; sb += im.at(x, y)[b]; }
        return sa / std::max(sb, 1e-6);
    };
    const int cx = (g_fovRect[0] + g_fovRect[2]) / 2, cy = (g_fovRect[1] + g_fovRect[3]) / 2;
    const double gc0 = Ratio(plain, cx - 20, cy - 20, cx + 20, cy + 20, 1, 0), gc1 = Ratio(tinted, cx - 20, cy - 20, cx + 20, cy + 20, 1, 0);
    const double ge0 = Ratio(plain, 2, 2, 30, 30, 0, 2), ge1 = Ratio(tinted, 2, 2, 30, 30, 0, 2);
    CHECK(gc1 < gc0 * 0.8 && ge1 < ge0 * 0.8, "debug tint: magenta in the area (G/R %.2f -> %.2f), blue outside (R/B %.2f -> %.2f)", gc0, gc1, ge0,
          ge1);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\fsr_area.ppm", outDir); SavePpm(fov, path);
    snprintf(path, sizeof(path), "%s\\fsr_area_tint.ppm", outDir); SavePpm(tinted, path);
    ClearAll(g);
    SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
    Settle(g);
    g_fovBand = 0;
}

static void Upload1(Gfx &g, ID3D11Texture2D *t, const Image &im)
{
    std::vector<uint8_t> bytes(size_t(im.w) * im.h * 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        float v = im.px[i];
        bytes[i] = (uint8_t)lroundf((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f);
    }
    g.ctx->UpdateSubresource(t, 0, nullptr, bytes.data(), im.w * 4, 0);
}

static void RunSwapchainPoolTest(Gfx &g, const Sequence &mov, const char *iniBase, const char *reqShared, float F, UINT misc)
{
    printf("[swapchain drawn every frame, like SteamVR: 3 textures per eye, each app frame shown twice]\n");
    char ini[2048];
    // with per-pass timing: the averages must be per new frame, not per draw
    std::string timedBase(iniBase);
    const size_t at = timedBase.find("[match]");
    if (at != std::string::npos) timedBase.insert(at, "passTiming=1\n");
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n",
             timedBase.c_str(), reqShared, F);
    WriteIni(ini);
    Pump(g);
    if (g_passTimes) g_passTimes(nullptr, nullptr, 0, 1);   // nothing older
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, mov.ow, mov.oh);
    ID3D11Texture2D *tex[2][3] = {};
    ID3D11ShaderResourceView *v[2][3] = {};
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            tex[e][k] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { mov.in[e][0] }, misc);
            v[e][k] = MakeView(g, tex[e][k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
        }
    // app frames f = 1..n go to texture f % 3; the compositor draws all three, newest last
    auto Show = [&](int f, int reps) {
        const int k = f % 3;
        for (int c = 0; c < reps; ++c) {
            for (int e = 0; e < 2; ++e)
                for (int j = 1; j <= 3; ++j) DrawEye(g, tgt, v[e][(k + j) % 3], e, nullptr, false);
            Sleep(4);
        }
    };
    const int n = 12;
    for (int f = 1; f <= n; ++f) {
        Upload1(g, tex[0][f % 3], mov.in[0][f]);
        Upload1(g, tex[1][f % 3], mov.in[1][f]);
        Show(f, 2);
    }
    // the two newest streams (earlier tests' textures may still hold older ones)
    const int all = StreamCount();
    uint32_t s0[32] = {}, s1[32] = {}, tmp[32];
    int ns = 0;
    for (int i = 0; i < all; ++i) {
        g_debugStream(i, tmp);
        if (tmp[16] > s1[16]) {
            if (tmp[16] > s0[16]) { memcpy(s1, s0, sizeof(s1)); memcpy(s0, tmp, sizeof(s0)); }
            else memcpy(s1, tmp, sizeof(s1));
        }
        if (tmp[17] > 0) ++ns;
    }
    (void)ns;
    CHECK(s0[17] == 3 && s1[17] == 3, "an eye's 3 textures share one history (newest streams: %u and %u textures)", s0[17], s1[17]);
    CHECK(s0[12] == (uint32_t)n + 1 && s1[12] == (uint32_t)n + 1,
          "history updates = new app frames, not draws (%u and %u updates for %d frames; %u draws each)", s0[12], s1[12], n + 1,
          s0[13]);
    if (g_passRuns && g_passTimes) {
        int upd = 0, chk = 0;
        double chkMs = 0, pm[16] = {}, tot = 0;
        const char *pn[16] = {};
        g_passRuns(&upd, &chk, &chkMs);
        const int np = g_passTimes(pm, pn, 16, 1);
        for (int i = 0; i < np; ++i) tot += pm[i];
        CHECK(np > 0 && upd >= 1 && upd <= 2 * (n + 1) && chk >= 1 && chkMs < 0.5 * tot,
              "pass timing: new frames' updates averaged apart from the checks of frames already processed "
              "(%d updates timed, %.3f ms each; %d checks, %.3f ms each)", upd, tot, chk, chkMs);
    }
    // what the eye shows is the newest frame
    Image out = DrawEye(g, tgt, v[0][n % 3], 0, nullptr, true);
    const double now = Compare(out, mov.gt[0][n]).psnr, before = Compare(out, mov.gt[0][n - 1]).psnr;
    CHECK(now > before + 1.0, "the newest texture shows the newest frame (%.2f dB vs %.2f dB against the previous frame)", now, before);

    // the same with FSR 1: each texture is upscaled when its content changes, not on every draw
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n",
             iniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    const int m = 6;
    for (int f = n + 1; f <= n + m; ++f) {
        Upload1(g, tex[0][f % 3], mov.in[0][f]);
        Upload1(g, tex[1][f % 3], mov.in[1][f]);
        Show(f, 2);
    }
    uint32_t ups = 0, checks = 0;
    bool gates = g_debugGate != nullptr;
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            uint32_t c[2] = {};
            if (g_debugGate && g_debugGate(tex[e][k], 0, c)) { ups += c[0]; checks += c[1]; }
            else gates = false;
        }
    // every texture once on the switch to FSR (first use), then once per new frame
    CHECK(gates && checks == (uint32_t)m * 2 * 6 && ups >= (uint32_t)m * 2 && ups <= (uint32_t)m * 2 + 6,
          "FSR 1 upscales only changed textures (%u upscales for %d new eye frames, %u draws)", ups, m * 2, checks);
    Image out2 = DrawEye(g, tgt, v[1][(n + m) % 3], 1, nullptr, true);
    const double now2 = Compare(out2, mov.gt[1][n + m]).psnr, before2 = Compare(out2, mov.gt[1][n + m - 1]).psnr;
    CHECK(now2 > before2 + 1.0, "FSR 1: the newest texture shows the newest frame (%.2f dB vs %.2f dB)", now2, before2);

    ClearAll(g);
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) { SafeRelease(v[e][k]); SafeRelease(tex[e][k]); }
    tgt.Release();
    Settle(g);
}

// ---------------------------------------------------------------------------------------------
// After SteamVR's resolution is lowered, the game keeps its (larger) eye textures and renders into
// their top-left corner; the compositor samples only that part. A slider drag can leave each eye
// with three textures of different sizes that hold the same picture. They must share one history
// (no frame-to-frame alternation between temporal and FSR 1), the picture's size must be found
// (and reported to the app), and a later change of it followed. If the textures really hold
// pictures of different sizes, the eye stays on FSR 1 for every frame rather than alternating.

static void UploadCorner(Gfx &g, ID3D11Texture2D *t, const Image &im)
{
    std::vector<uint8_t> bytes(size_t(im.w) * im.h * 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        float v = im.px[i];
        bytes[i] = (uint8_t)lroundf((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f);
    }
    D3D11_BOX box = { 0, 0, 0, (UINT)im.w, (UINT)im.h, 1 };
    g.ctx->UpdateSubresource(t, 0, &box, bytes.data(), im.w * 4, 0);
}

struct StreamSummary { int live = 0, minTex = 99, maxTex = 0; uint64_t newFrames = 0; uint32_t maxId = 0; };

// Streams created after firstId that hold textures (others belong to earlier tests or wait to be freed).
static StreamSummary Summarize(uint32_t firstId = 0)
{
    StreamSummary r;
    const int n = StreamCount();
    for (int i = 0; i < n; ++i) {
        uint32_t st[32] = {};
        g_debugStream(i, st);
        r.maxId = std::max(r.maxId, st[16]);
        if (st[17] == 0 || st[16] <= firstId) continue;
        r.live++;
        r.minTex = std::min(r.minTex, (int)st[17]);
        r.maxTex = std::max(r.maxTex, (int)st[17]);
        r.newFrames += st[12];
    }
    return r;
}

static void PrintStreams()
{
    const int n = StreamCount();
    printf("         histories:");
    for (int i = 0; i < n; ++i) {
        uint32_t st[32] = {};
        g_debugStream(i, st);
        printf(" #%u:%u", st[16], st[17]);
    }
    printf("\n");
}

static void RunResizedPoolTest(Gfx &g, const World &world, const char *iniBase, const char *reqShared, float F, UINT misc)
{
    printf("[resolution lowered: 3 textures per eye of different sizes, the game renders into their top-left corner]\n");
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    auto debugContent = (int (*)(ID3D11Texture2D *, uint32_t *))GetProcAddress(dxgi, "VrUpscaleDebugContent");
    auto debugPools = (int (*)(int *))GetProcAddress(dxgi, "VrUpscaleDebugPools");
    CHECK(debugContent && debugPools, "content and eye-pool debug exports present");
    if (!debugContent || !debugPools) return;

    const int W = 400, H = 440;
    const int sz[3][2] = { { W, H }, { 432, 475 }, { 464, 510 } };   // the same shape
    const int OW = (int)lroundf(W * F), OH = (int)lroundf(H * F);
    const int N1 = 30, N2 = 12;
    Sequence seq = MakeSequence(world, W, H, OW, OH, N1 + N2, true, 4242);
    char ini[2048];
    auto Mode = [&](const char *mode) {
        snprintf(ini, sizeof(ini),
                 "%srequireShared=%s\n[upscale]\nmode=%s\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\nprobeWindowMs=100\n",
                 iniBase, reqShared, mode, F);
        WriteIni(ini);
        Pump(g);
    };
    Mode("temporal");
    const uint32_t firstId = Summarize().maxId;
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, OW, OH);
    ID3D11Texture2D *tex[2][3] = {};
    ID3D11ShaderResourceView *v[2][3] = {};
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            // what the texture held before: a larger picture, stale outside the corner now in use
            SceneFrame old = seq.f[0];
            old.ox += 37.0f;
            old.oy += 11.0f;
            Image stale = RenderScene(world, old, e, sz[k][0], sz[k][1], (float)W / (float)sz[k][0]);
            tex[e][k] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { stale }, misc);
            v[e][k] = MakeView(g, tex[e][k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
        }
    int cw = W, ch = H;   // the picture the game renders now
    // app frame f goes to texture f % 3 of each eye; the compositor draws all three (newest last),
    // sampling only the picture's part of each
    auto Frame = [&](const Sequence &q, int i, int f, Image *out0) {
        const int k = f % 3;
        UploadCorner(g, tex[0][k], q.in[0][i]);
        UploadCorner(g, tex[1][k], q.in[1][i]);
        for (int e = 0; e < 2; ++e)
            for (int j = 1; j <= 3; ++j) {
                const int kk = (k + j) % 3;
                const bool read = out0 && e == 0 && j == 3;
                Image im = DrawEye(g, tgt, v[e][kk], e, nullptr, read, (float)cw / sz[kk][0], (float)ch / sz[kk][1]);
                if (read) *out0 = im;
            }
        Sleep(25);
    };
    auto ContentIs = [&](int k, int x, int y, int w, int h, bool part) {
        bool ok = true;
        for (int e = 0; e < 2; ++e) {
            uint32_t c[8] = {};
            ok = ok && debugContent(tex[e][k], c) && c[7] == 1 && (c[4] != 0) == part &&
                 (!part || (c[0] == (uint32_t)x && c[1] == (uint32_t)y && c[2] == (uint32_t)w && c[3] == (uint32_t)h));
            if (!ok) printf("         texture %dx%d eye %d: rect %u,%u %ux%u part=%u changes=%u windows=%u\n", sz[k][0], sz[k][1], e, c[0], c[1],
                            c[2], c[3], c[4], c[5], c[6]);
        }
        return ok;
    };

    // 1. the picture is found and the textures share one history per eye: how many frames it takes
    //    (probeWindowMs=100 here; at the default 500 ms it is five times as long)
    auto Shared = [&](int need) {
        StreamSummary x = Summarize(firstId);
        return x.live == 2 && x.minTex == need && x.maxTex == need;
    };
    int f1 = 0, settled = -1;
    for (; f1 < N1; ++f1) {
        Frame(seq, f1, f1, nullptr);
        if (settled < 0 && Shared(3)) settled = f1;
        if (settled >= 0 && f1 >= settled + 8) break;   // and a few frames more, so no eye still counts as mixed
    }
    printf("         one history per eye after %d frames (%d ms windows)\n", settled + 1, 100);
    CHECK(ContentIs(1, 0, 0, W, H, true) && ContentIs(2, 0, 0, W, H, true) && ContentIs(0, 0, 0, W, H, false),
          "the %dx%d picture is found in the corner of the larger textures (the %dx%d one is used whole)", W, H, W, H);
    StreamSummary s1 = Summarize(firstId);
    int mixed = -1;
    const int pools = debugPools(&mixed);
    if (!(s1.live == 2 && s1.minTex == 3 && s1.maxTex == 3)) PrintStreams();
    CHECK(settled >= 0 && settled < 20, "that takes few frames (%d)", settled + 1);
    CHECK(s1.live == 2 && s1.minTex == 3 && s1.maxTex == 3, "each eye's 3 textures of different sizes share one history (%d histories, %d..%d textures)",
          s1.live, s1.minTex, s1.maxTex);
    CHECK(pools == 2 && mixed == 0, "both eyes temporal, none held on FSR 1 (%d eyes, %d mixed)", pools, mixed);
    {
        HANDLE map = nullptr;
        VuShared *sh = OpenTestChannel(&map);
        VuStatus st = {};
        const VuTexInfo *t = nullptr;
        for (int i = 0; i < 40 && sh; ++i) {   // published every 250 ms from the render tick
            Frame(seq, N1 - 1, N1 - 1, nullptr);
            if (ReadStatus(sh, st) && (t = FindTex(st, sz[2][0], sz[2][1])) && t->contentW == (uint32_t)W) break;
        }
        CHECK(t && t->eye && t->contentW == (uint32_t)W && t->contentH == (uint32_t)H && t->contentOutW == (uint32_t)OW &&
              t->contentOutH == (uint32_t)OH && st.eyePools == 2 && st.mixedEyes == 0,
              "the app is told: eye texture %dx%d renders %ux%u -> %ux%u (%d eyes, %d mixed)", sz[2][0], sz[2][1], t ? t->contentW : 0,
              t ? t->contentH : 0, t ? t->contentOutW : 0, t ? t->contentOutH : 0, st.eyePools, st.mixedEyes);
        if (sh) UnmapViewOfFile(sh);
        if (map) CloseHandle(map);
    }

    // 2. every new frame updates the shared history once, and every displayed frame is temporal:
    //    none of them equals what FSR 1 shows for the same frame
    std::vector<Image> outT(N2), outF(N2);
    StreamSummary a = Summarize(firstId);
    for (int i = 0; i < N2; ++i) Frame(seq, N1 + i, N1 + i, &outT[i]);
    StreamSummary b = Summarize(firstId);
    CHECK(b.newFrames - a.newFrames == (uint64_t)(2 * N2), "one history update per new frame per eye (%llu for %d frames)",
          (unsigned long long)(b.newFrames - a.newFrames), N2);
    Mode("fsr");
    for (int i = 0; i < N2; ++i) Frame(seq, N1 + i, N1 + i, &outF[i]);
    Acc accT, accF;
    double closest = 0;
    for (int i = 0; i < N2; ++i) {
        for (int y = 4; y < OH - 4; ++y)
            for (int x = 4; x < OW - 4; ++x) {
                accT.Add(outT[i].at(x, y), seq.gt[0][N1 + i].at(x, y));
                accF.Add(outF[i].at(x, y), seq.gt[0][N1 + i].at(x, y));
            }
        closest = std::max(closest, Compare(outT[i], outF[i]).psnr);
    }
    printf("         displayed frames vs ground truth: temporal %.2f dB, FSR 1 %.2f dB; closest temporal frame to FSR 1: %.1f dB\n",
           accT.Psnr(), accF.Psnr(), closest);
    CHECK(closest < 40.0, "no displayed frame fell back to FSR 1 (closest %.1f dB)", closest);
    CHECK(accT.Psnr() > accF.Psnr() + 0.2, "temporal beats FSR 1 through the resized pool (%+.2f dB)", accT.Psnr() - accF.Psnr());

    // 3. the resolution is lowered again: the game renders a smaller picture into the same textures
    Mode("temporal");
    const int W3 = 360, H3 = 396, OW3 = (int)lroundf(W3 * F), OH3 = (int)lroundf(H3 * F), N3 = 40;
    Sequence seq3 = MakeSequence(world, W3, H3, OW3, OH3, N3, true, 777);
    EnsureEyeTarget(g, tgt, OW3, OH3);
    cw = W3;
    ch = H3;
    int f3 = 0, settled3 = -1;
    for (; f3 < N3 - 1; ++f3) {
        Frame(seq3, f3, f3, nullptr);
        bool all = true;
        for (int k = 0; k < 3 && all; ++k)
            for (int e = 0; e < 2 && all; ++e) {
                uint32_t c[8] = {};
                all = debugContent(tex[e][k], c) && c[4] && c[2] < (uint32_t)W;
            }
        if (settled3 < 0 && all && Shared(3)) settled3 = f3;
        if (settled3 >= 0 && f3 >= settled3 + 8) break;
    }
    printf("         smaller picture followed after %d frames\n", settled3 + 1);
    Image last;
    Frame(seq3, f3, f3, &last);
    CHECK(ContentIs(0, 0, 0, W3, H3, true) && ContentIs(1, 0, 0, W3, H3, true) && ContentIs(2, 0, 0, W3, H3, true),
          "a smaller picture (%dx%d) is followed in all three textures", W3, H3);
    StreamSummary s3 = Summarize(firstId);
    debugPools(&mixed);
    CHECK(s3.live == 2 && s3.minTex == 3 && s3.maxTex == 3 && mixed == 0, "... and they share one history per eye again (%d histories, %d..%d textures, %d mixed)",
          s3.live, s3.minTex, s3.maxTex, mixed);
    const double now3 = Compare(last, seq3.gt[0][f3]).psnr, prev3 = Compare(last, seq3.gt[0][f3 - 1]).psnr;
    CHECK(AllFinite(last) && now3 > prev3 + 1.0 && now3 > 18.0, "the newest frame is shown at the new size (%.2f dB vs %.2f dB against the previous)",
          now3, prev3);

    // 3b. the head and the scene stand still; only a small thing moves: that says nothing about the
    //     picture's size, which must stay as it is (no history reset)
    {
        uint32_t before[2][3][8] = {}, after[2][3][8] = {};
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 3; ++k) debugContent(tex[e][k], before[e][k]);
        StreamSummary sb = Summarize(firstId);
        for (int f = 0; f < 24; ++f) {
            Sequence still;
            still.in[0].push_back(seq3.in[0][f3]);
            still.in[1].push_back(seq3.in[1][f3]);
            for (int e = 0; e < 2; ++e)
                for (int y = 150; y < 190; ++y)
                    for (int x = 160; x < 200; ++x) {
                        float *px = still.in[e][0].at(x, y);
                        px[0] = (float)((f * 37) % 256) / 255.0f;
                        px[1] = (float)((f * 91 + 40) % 256) / 255.0f;
                    }
            Frame(still, 0, f3 + 1 + f, nullptr);
        }
        bool same = true;
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 3; ++k) {
                debugContent(tex[e][k], after[e][k]);
                same = same && memcmp(before[e][k], after[e][k], 6 * sizeof(uint32_t)) == 0;
            }
        StreamSummary sa = Summarize(firstId);
        CHECK(same && sa.live == 2 && sa.minTex == 3 && sa.maxTex == 3, "a still scene with one small moving thing keeps the picture size "
              "(%ux%u, %u changes before, %u after)", after[0][1][2], after[0][1][3], before[0][1][5], after[0][1][5]);
        (void)sb;
    }

    // 3c. one of the sizes unticked in the app (some did that to work around the flicker of 0.4.1):
    //     SteamVR shows it in turn with the upscaled ones, so it is upscaled with them anyway
    {
        snprintf(ini, sizeof(ini),
                 "%srequireShared=%s\nexcludeSizes=%dx%d\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n"
                 "[temporal]\ntemporalArea=1\nprobeWindowMs=100\n", iniBase, reqShared, sz[1][0], sz[1][1], F);
        WriteIni(ini);
        Pump(g);
        int f = 0, ok3 = -1;
        for (; f < 40; ++f) {
            Frame(seq3, (f3 + f) % N3, f3 + 30 + f, nullptr);
            if (ok3 < 0 && Shared(3)) ok3 = f;
            if (ok3 >= 0 && f >= ok3 + 8) break;
        }
        int mx = -1;
        debugPools(&mx);
        CHECK(ok3 >= 0 && mx == 0, "an unticked size that SteamVR shows in turn with the upscaled ones is upscaled too "
              "(one history per eye after %d frames, %d mixed)", ok3 + 1, mx);
        Mode("temporal");
    }

    // 3d. every size the eye uses unticked (or one texture alone): left alone, and it stays that way;
    //     its own draws from before the change must not count as "shown in turn with upscaled ones"
    {
        snprintf(ini, sizeof(ini),
                 "%srequireShared=%s\nexcludeSizes=%dx%d,%dx%d,%dx%d\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n"
                 "[temporal]\ntemporalArea=1\nprobeWindowMs=100\n", iniBase, reqShared, sz[0][0], sz[0][1], sz[1][0], sz[1][1],
                 sz[2][0], sz[2][1], F);
        WriteIni(ini);
        Pump(g);
        HANDLE map = nullptr;
        VuShared *sh = OpenTestChannel(&map);
        auto AllAlone = [&]() {
            VuStatus st = {};
            if (!sh || !ReadStatus(sh, st)) return false;
            for (int k = 0; k < 3; ++k) {   // the entries drawn just now (the table keeps older ones)
                bool seen = false;
                for (uint32_t i = 0; i < st.texCount && i < kVuMaxTex; ++i) {
                    const VuTexInfo &t = st.tex[i];
                    if (t.w != (uint32_t)sz[k][0] || t.h != (uint32_t)sz[k][1] || st.tick - t.lastSeenTick > 1000) continue;
                    if (t.upscaled) return false;
                    seen = true;
                }
                if (!seen) return false;
            }
            return true;
        };
        int f = 0, alone = -1;
        for (; f < 40 && alone < 0; ++f) {
            Frame(seq3, (f3 + f) % N3, f3 + 80 + f, nullptr);
            if (AllAlone()) alone = f;
        }
        for (int k = 0; k < 16; ++k) Frame(seq3, (f3 + f + k) % N3, f3 + 120 + f + k, nullptr);
        const bool still = AllAlone();
        if (!still && sh) {
            VuStatus st = {};
            ReadStatus(sh, st);
            for (uint32_t i = 0; i < st.texCount && i < kVuMaxTex; ++i)
                printf("         status: %ux%u upscaled=%d eye=%d seen %ums ago: %s\n", st.tex[i].w, st.tex[i].h, st.tex[i].upscaled, st.tex[i].eye,
                       st.tick - st.tex[i].lastSeenTick, st.tex[i].reason);
        }
        CHECK(alone >= 0 && still, "unticking every size the eye uses leaves them alone, and they stay that way (after %d frames: %s)",
              alone + 1, still ? "still left alone" : "upscaled again");
        if (sh) UnmapViewOfFile(sh);
        if (map) CloseHandle(map);
        Mode("temporal");
    }

    ClearAll(g);
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) { SafeRelease(v[e][k]); SafeRelease(tex[e][k]); }
    Settle(g);

    // 4. pictures of really different sizes (each texture used whole): the eye can't keep one
    //    history, so it stays on FSR 1 for every frame instead of alternating
    printf("[3 textures per eye holding pictures of different sizes]\n");
    EnsureEyeTarget(g, tgt, OW, OH);
    std::vector<Image> frames[2][3];
    const int N4 = 16;
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < N4; i += 1)
                if (i % 3 == k) frames[e][k].push_back(RenderScene(world, seq.f[i], e, sz[k][0], sz[k][1], (float)W / (float)sz[k][0]));
            tex[e][k] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { frames[e][k][0] }, misc);
            v[e][k] = MakeView(g, tex[e][k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
        }
    auto Frame4 = [&](int f) {
        const int k = f % 3;
        Upload1(g, tex[0][k], frames[0][k][f / 3]);
        Upload1(g, tex[1][k], frames[1][k][f / 3]);
        for (int e = 0; e < 2; ++e)
            for (int j = 1; j <= 3; ++j) DrawEye(g, tgt, v[e][(k + j) % 3], e, nullptr, false);
        Sleep(25);
    };
    for (int f = 0; f < 6; ++f) Frame4(f);
    StreamSummary c0 = Summarize(firstId);
    uint32_t ups0 = 0, ups1 = 0;
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            uint32_t c[2] = {};
            if (g_debugGate && g_debugGate(tex[e][k], 0, c)) ups0 += c[0];
        }
    const DWORD t4 = GetTickCount();
    for (int f = 6; f < N4; ++f) Frame4(f);
    // the app is told about an eye held on FSR 1 once that lasts more than a second
    for (int f = N4; GetTickCount() - t4 < 1500 && f < N4 + 200; ++f) Frame4(6 + (f - 6) % (N4 - 6));
    StreamSummary c1 = Summarize(firstId);
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            uint32_t c[2] = {};
            if (g_debugGate && g_debugGate(tex[e][k], 0, c)) ups1 += c[0];
        }
    debugPools(&mixed);
    CHECK(mixed == 2, "both eyes are held on FSR 1 (%d)", mixed);
    CHECK(c1.newFrames == c0.newFrames && ups1 - ups0 >= (uint32_t)(2 * (N4 - 6)),
          "every frame is upscaled spatially, none temporally (%u spatial upscales, %llu history updates for %d frames)", ups1 - ups0,
          (unsigned long long)(c1.newFrames - c0.newFrames), N4 - 6);

    ClearAll(g);
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) { SafeRelease(v[e][k]); SafeRelease(tex[e][k]); }
    tgt.Release();
    Settle(g);
}


// ---------------------------------------------------------------------------------------------
// A preset switch with "Set VRChat's resolution" on: the app changes SteamVR's resolution and the
// game renders a picture of another size into the same textures (here the pool a slider drag
// leaves: three textures of different sizes per eye). At the default observation window (500 ms).
// Every texture of an eye must take the new size in the same moment, so the eye is never on two
// histories ("Temporal paused"), and with the app's notice (kVuCmdResolutionChanging) the new size
// must be found within a fraction of the time.

struct SwitchPhase { int frames = 0, followMs = -1, pausedDraws = 0, reported = 0; double early = 0, later = 0; };

// ---------------------------------------------------------------------------------------------
// The spatial path when the game renders into part of a larger texture (SteamVR's resolution
// lowered while it runs, as Set VRChat's resolution does): only the picture and a band around it
// are upscaled, so the result is what a texture holding just the picture gives, and so is the GPU
// time (before 0.6.2 the passes outside the area covered the whole proxy: at 2x, 4 times the picture).

static void RunSpatialCornerTest(Gfx &g, const World &world, const char *iniBase, const char *reqShared, float F, UINT misc)
{
    printf("[FSR 1 and bilinear when the game renders into the corner of a larger texture]\n");
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    auto debugContent = (int (*)(ID3D11Texture2D *, uint32_t *))GetProcAddress(dxgi, "VrUpscaleDebugContent");
    CHECK(debugContent != nullptr, "content debug export present");
    if (!debugContent) return;
    const int W = 400, H = 440, TW = 2 * W, TH = 2 * H;   // the picture: a quarter of the texture
    const int OW = (int)lroundf(W * F), OH = (int)lroundf(H * F), N = 40;
    Sequence seq = MakeSequence(world, W, H, OW, OH, N, true, 3131);
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, OW, OH);
    ID3D11Texture2D *exact[2] = {}, *big[2] = {};
    ID3D11ShaderResourceView *ve[2] = {}, *vb[2] = {};
    for (int e = 0; e < 2; ++e) {
        exact[e] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { seq.in[e][0] }, misc);
        ve[e] = MakeView(g, exact[e], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
        SceneFrame old = seq.f[0];   // what the texture held before: a larger picture, stale outside the corner now
        old.ox += 23.0f;
        Image stale = RenderScene(world, old, e, TW, TH, 0.5f);
        big[e] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { stale }, misc);
        vb[e] = MakeView(g, big[e], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
    }
    char ini[2048];
    auto Mode = [&](const char *mode, float area) {
        snprintf(ini, sizeof(ini),
                 "%srequireShared=%s\n[upscale]\nmode=%s\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=%.2f\nprobeWindowMs=100\n",
                 iniBase, reqShared, mode, F, area);
        WriteIni(ini);
        Pump(g);
    };
    // a new frame into both; each eye draws the exact texture, then the corner of the big one
    auto Frame = [&](int i, Image *outE, Image *outB) {
        for (int e = 0; e < 2; ++e) {
            Upload1(g, exact[e], seq.in[e][i]);
            UploadCorner(g, big[e], seq.in[e][i]);
        }
        for (int e = 0; e < 2; ++e) {
            Image x = DrawEye(g, tgt, ve[e], e, nullptr, outE && e == 0);
            Image y = DrawEye(g, tgt, vb[e], e, nullptr, outB && e == 0, 0.5f, 0.5f);
            if (outE && e == 0) *outE = x;
            if (outB && e == 0) *outB = y;
        }
        Sleep(25);
    };
    Mode("fsr", 0.6f);
    int f = 0;
    bool found = false;
    for (; f < N - 12 && !found; ++f) {
        Frame(f, nullptr, nullptr);
        found = true;
        for (int e = 0; e < 2; ++e) {
            uint32_t c[8] = {};
            found = found && debugContent(big[e], c) && c[4] && c[0] == 0 && c[1] == 0 && c[2] == (uint32_t)W && c[3] == (uint32_t)H;
        }
    }
    CHECK(found, "the %dx%d picture is found in the corner of the %dx%d textures (%d frames)", W, H, TW, TH, f);

    // the corner of the big texture shows what the exact one does (away from the picture's right
    // and bottom edges, where the filters read the stale pixels beyond it instead of clamping)
    auto Same = [&](const char *what) {
        Image a, b;
        Frame(f++, &a, &b);
        double worst = 0;
        for (int y = 0; y < OH - 10; ++y)
            for (int x = 0; x < OW - 10; ++x)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, (double)fabsf(a.at(x, y)[c] - b.at(x, y)[c]));
        CHECK(AllFinite(b) && worst <= 1.5 / 255.0, "%s: the picture in the corner is upscaled as on its own (largest difference %.1f/255)",
              what, worst * 255.0);
    };
    Same("FSR 1 in the area");
    Mode("fsr", 1.0f);
    Frame(f++, nullptr, nullptr);
    Same("FSR 1 over the whole picture");
    Mode("bilinear", 1.0f);
    Frame(f++, nullptr, nullptr);
    Same("bilinear");

    // GPU time per new frame and eye, one kind of texture drawn at a time (temporal too, for comparison)
    auto Time = [&](const char *mode, bool corner) {
        Mode(mode, 0.6f);
        double ms = 0;
        const int warm = 6, timed = 5;   // (a mode's first frames set up its histories)
        for (int k = 0; k < warm + timed; ++k) {
            const int i = f++ % N;
            for (int e = 0; e < 2; ++e) {
                if (corner) UploadCorner(g, big[e], seq.in[e][i]);
                else Upload1(g, exact[e], seq.in[e][i]);
            }
            for (int e = 0; e < 2; ++e) {
                double t = 0;
                DrawEye(g, tgt, corner ? vb[e] : ve[e], e, &t, false, corner ? 0.5f : 1.0f, corner ? 0.5f : 1.0f);
                if (k >= warm) ms += t;
            }
            Sleep(25);
        }
        return ms / (2 * timed);
    };
    const bool noT = EnvStr("VRUPSCALE_TEST_NOTEMPORAL")[0] == '1';
    const double fE = Time("fsr", false), fB = Time("fsr", true), tE = noT ? 0 : Time("temporal", false), tB = noT ? 0 : Time("temporal", true);
    printf("         GPU ms per new frame and eye (area 0.6): FSR 1 %.3f for the picture alone, %.3f in the corner; temporal %.3f, %.3f\n",
           fE, fB, tE, tB);
    CHECK(fB < 1.5 * fE, "FSR 1 costs about the picture's size, not the texture's (%.2fx)", fE > 0 ? fB / fE : 0.0);

    ClearAll(g);
    for (int e = 0; e < 2; ++e) { SafeRelease(ve[e]); SafeRelease(vb[e]); SafeRelease(exact[e]); SafeRelease(big[e]); }
    tgt.Release();
    Settle(g);
}

static void RunResolutionSwitchTest(Gfx &g, const World &world, const char *iniBase, const char *reqShared, float F, UINT misc)
{
    printf("[preset switch: the game's picture changes size in the same textures (default 500 ms windows)]\n");
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    auto debugContent = (int (*)(ID3D11Texture2D *, uint32_t *))GetProcAddress(dxgi, "VrUpscaleDebugContent");
    auto debugPools = (int (*)(int *))GetProcAddress(dxgi, "VrUpscaleDebugPools");
    auto debugPaused = (int (*)())GetProcAddress(dxgi, "VrUpscaleDebugPaused");
    HANDLE map = nullptr;
    VuShared *sh = OpenTestChannel(&map);
    CHECK(debugContent && debugPools && debugPaused && sh, "content and eye-pool debug exports, and the app's channel, present");
    if (!debugContent || !debugPools || !debugPaused || !sh) {
        if (sh) UnmapViewOfFile(sh);
        if (map) CloseHandle(map);
        return;
    }
    const int sz[3][2] = { { 400, 440 }, { 432, 475 }, { 464, 510 } };
    // the picture at 1.3x and at 1.5x of the same field of view (VRChat at 59% and 44%)
    const int pw[2] = { 400, 348 }, ph[2] = { 440, 383 };
    const int OW = (int)lroundf(pw[0] * F), OH = (int)lroundf(ph[0] * F);
    const int N = 24;
    std::vector<Image> frames[2][2], gt;   // [picture][eye]; what eye 0 should show
    {
        Rng rng(4711);
        SceneFrame cur = { 40.0f, 30.0f, 60.0f, 250.0f };
        for (int i = 0; i < N; ++i) {
            for (int p = 0; p < 2; ++p)
                for (int e = 0; e < 2; ++e) frames[p][e].push_back(RenderScene(world, cur, e, pw[p], ph[p], (float)pw[0] / (float)pw[p]));
            gt.push_back(RenderScene(world, cur, 0, OW, OH, (float)pw[0] / (float)OW));
            cur.ox += 0.37f + 0.45f * rng.N();
            cur.oy += 0.13f + 0.45f * rng.N();
            cur.objX += 3.3f;
            cur.objY -= 1.7f;
        }
    }
    char ini[2048];
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n",
             iniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    const uint32_t firstId = Summarize().maxId;
    EyeTarget tgt;   // the compositor's eye targets keep their size whatever the game renders
    EnsureEyeTarget(g, tgt, OW, OH);
    ID3D11Texture2D *tex[2][3] = {};
    ID3D11ShaderResourceView *v[2][3] = {};
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) {
            tex[e][k] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { RenderScene(world, { 77, 41, 0, 0 }, e, sz[k][0], sz[k][1], 1.0f) }, misc);
            v[e][k] = MakeView(g, tex[e][k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
        }
    int fc = 0;
    // returns what eye 0 shows against the ground truth (dB) when `measure`
    auto Frame = [&](int pic, bool measure = false) {
        const int k = fc % 3, i = fc % (2 * N - 2), idx = i < N ? i : 2 * N - 2 - i;   // back and forth through the frames
        ++fc;
        UploadCorner(g, tex[0][k], frames[pic][0][idx]);
        UploadCorner(g, tex[1][k], frames[pic][1][idx]);
        double psnr = 0;
        for (int e = 0; e < 2; ++e)
            for (int j = 1; j <= 3; ++j) {
                const int kk = (k + j) % 3;
                const bool read = measure && e == 0 && j == 3;
                Image im = DrawEye(g, tgt, v[e][kk], e, nullptr, read, (float)pw[pic] / sz[kk][0], (float)ph[pic] / sz[kk][1]);
                if (read) psnr = Compare(im, gt[idx]).psnr;
            }
        Sleep(25);
        return psnr;
    };
    auto Hint = [&]() {
        sh->command = kVuCmdResolutionChanging;
        MemoryBarrier();
        InterlockedIncrement((volatile LONG *)&sh->commandSeq);
    };
    auto Followed = [&](int pic) {
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 3; ++k) {
                uint32_t c[8] = {};
                if (!debugContent(tex[e][k], c) || !c[7] || c[0] != 0 || c[1] != 0 || c[2] != (uint32_t)pw[pic] || c[3] != (uint32_t)ph[pic])
                    return false;
            }
        StreamSummary s = Summarize(firstId);
        return s.live == 2 && s.minTex == 3 && s.maxTex == 3;
    };
    // the game switches to picture `pic` (after the app's notice `lead` ms earlier, if `hint`); runs
    // until every texture shows the new size on one history per eye, and that held for a second
    auto Phase = [&](int pic, bool hint, int lead, int maxMs) {
        SwitchPhase r;
        if (hint) {
            Hint();
            const DWORD h0 = GetTickCount();
            while (GetTickCount() - h0 < (DWORD)lead) Frame(1 - pic);
        }
        const DWORD t0 = GetTickCount();
        const int paused0 = debugPaused();
        for (;;) {
            const bool early = r.frames < 8, later = r.frames >= 16 && r.frames < 24;
            const double q = Frame(pic, early || later);
            if (early) r.early += q / 8;
            if (later) r.later += q / 8;
            r.frames++;
            int mixed = 0;
            debugPools(&mixed);   // what the app is told ("Temporal paused")
            if (mixed) r.reported++;
            r.pausedDraws = debugPaused() - paused0;
            const int t = (int)(GetTickCount() - t0);
            if (Followed(pic)) {
                if (r.followMs < 0) r.followMs = t;
            } else {
                r.followMs = -1;
            }
            if ((r.followMs >= 0 && t > r.followMs + 1000 && r.frames >= 24) || t > maxMs) break;
        }
        return r;
    };
    SwitchPhase p0 = Phase(0, false, 0, 15000);
    printf("         start: %dx%d found in all six textures after %d ms (%d frames)\n", pw[0], ph[0], p0.followMs, p0.frames);
    CHECK(p0.followMs >= 0, "the %dx%d picture is found in all three textures of each eye", pw[0], ph[0]);
    struct { int pic; bool hint; int lead; const char *what; } steps[4] = {
        { 1, false, 0, "smaller, no notice" }, { 0, true, 0, "larger, with the app's notice" },
        { 1, true, 300, "smaller, notice 300 ms ahead" }, { 0, false, 0, "larger, no notice" } };
    SwitchPhase res[4];
    for (int s = 0; s < 4; ++s) {
        res[s] = Phase(steps[s].pic, steps[s].hint, steps[s].lead, 12000);
        printf("         %-30s %dx%d followed after %5d ms (%3d frames); %2d draws on FSR 1, reported %d times; eye 0 shows "
               "%.2f dB in the first 8 frames, %.2f dB 16 frames later\n", steps[s].what, pw[steps[s].pic], ph[steps[s].pic],
               res[s].followMs, res[s].frames, res[s].pausedDraws, res[s].reported, res[s].early, res[s].later);
    }
    bool allFollowed = true;
    int reported = 0;
    for (int s = 0; s < 4; ++s) {
        allFollowed = allFollowed && res[s].followMs >= 0;
        reported += res[s].reported;
    }
    CHECK(allFollowed, "every switch is followed in all six textures, one history per eye");
    CHECK(res[0].pausedDraws == 0 && res[2].pausedDraws == 0,
          "a smaller picture: each eye's textures take it together, no draw on FSR 1 (%d and %d draws)", res[0].pausedDraws,
          res[2].pausedDraws);
    // a larger one is taken by a texture as soon as its own frames show it (it may not have had a
    // new frame yet when the first one did); this test's frames come ~10x slower than a headset's,
    // so here that can be a few frames, in a headset a few milliseconds
    CHECK(res[1].pausedDraws <= 6 * 6 && res[3].pausedDraws <= 6 * 6, "a larger picture: FSR 1 at most until each texture had a frame of it "
          "(%d and %d draws)", res[1].pausedDraws, res[3].pausedDraws);
    CHECK(reported == 0, "the app is never told \"Temporal paused\" during the switches (%d)", reported);
    CHECK(res[2].followMs >= 0 && res[0].followMs >= 0 && res[2].followMs * 2 < res[0].followMs,
          "with the app's notice a smaller picture is found much sooner (%d ms vs %d ms)", res[2].followMs, res[0].followMs);
    CHECK(res[1].followMs >= 0 && res[1].followMs < 1500, "with the app's notice a larger picture is taken at once (%d ms)", res[1].followMs);

    ClearAll(g);
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) { SafeRelease(v[e][k]); SafeRelease(tex[e][k]); }
    tgt.Release();
    UnmapViewOfFile(sh);
    CloseHandle(map);
    Settle(g);
}

// ---- eye tracking: the temporal area follows the gaze (Bigscreen Beyond's shared gaze data) ----

#pragma pack(push, 8)
struct FakeGaze {   // SharedGazeData of Bigscreen's VRCFT module
    float lx, ly, lz, rx, ry, rz, cx, cy, cz, confidence;
    int64_t timestamp;
    int32_t valid;
    float leftClosed, rightClosed;
};
#pragma pack(pop)
static volatile FakeGaze *g_fakeGaze = nullptr;
static float g_gazeTx = 0, g_gazeTy = 0;

// The eye tracking client's side: a new sample looking at tangents (tx, ty) (x right, y up)
static void WriteGaze(float tx, float ty)
{
    if (!g_fakeGaze) return;
    const float n = 1.0f / sqrtf(1.0f + tx * tx + ty * ty);
    FakeGaze s = {};
    s.lx = s.rx = s.cx = tx * n;
    s.ly = s.ry = s.cy = ty * n;
    s.lz = s.rz = s.cz = n;
    s.confidence = 1.0f;
    s.timestamp = g_fakeGaze->timestamp + 1;
    s.valid = 1;
    memcpy((void *)g_fakeGaze, &s, sizeof(s));
}

struct GazeRegion { int side; uint32_t rx, ry, rw, rh, ow, oh; };
static std::vector<GazeRegion> GazeRegions()
{
    std::vector<GazeRegion> out;
    uint32_t st[32] = {};
    const int n = StreamCount();
    for (int i = 0; i < n; ++i) {
        memset(st, 0, sizeof(st));
        g_debugStream(i, st);
        if (st[17] == 0) continue;
        out.push_back({ (int)st[25] - 1, st[19], st[20], st[21], st[22], st[23], st[24] });
    }
    return out;
}

// Keeps the gaze live (new samples) while drawing the same frame for `ms`.
static Image HoldGaze(Gfx &g, EyeTarget &tgt, ID3D11ShaderResourceView *view, int ms)
{
    Image out;
    for (int t = 0; t < ms; t += 30) {
        WriteGaze(g_gazeTx, g_gazeTy);
        Pump(g);
        out = DrawEye(g, tgt, view, 0, nullptr, true);
        Sleep(30);
    }
    return out;
}

static void RunGazeTest(Gfx &g, const Sequence &mov, const char *iniBase, const char *reqShared, float F, UINT misc)
{
    printf("[eye tracking: the temporal area follows the gaze]\n");
    const int OW = mov.ow, OH = mov.oh;
    std::vector<Image> blank = { mov.in[0][0], mov.in[1][0] };
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, blank, misc);
    ID3D11ShaderResourceView *views[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                           MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, OW, OH);
    HANDLE chMap = nullptr;
    VuShared *sh = OpenTestChannel(&chMap);
    VuStatus st;
    // projection +-1 both ways: tangent t lands at (t + 1) / 2 of the picture
    char ini[1536];
    auto Ini = [&](int follow, int show, float ox, float oy, float sharp, float area = 0.6f, int margin = 0) {
        char wm[48] = "";
        if (margin) snprintf(wm, sizeof(wm), "workMargin=%d\n", margin);
        snprintf(ini, sizeof(ini),
                 "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=%.2f\n[temporal]\ntemporalArea=%.2f\n%s"
                 "[eyes]\nfollowGaze=%d\nshowGaze=%d\noffsetX=%.4f\noffsetY=%.4f\nprojection=-1 1 -1 1 -1 1 -1 1\n",
                 iniBase, reqShared, F, sharp, area, wm, follow, show, ox, oy);
        WriteIni(ini);
        Pump(g);
    };
    auto Frames = [&](int from, int n) {
        for (int i = from; i < from + n; ++i) {
            WriteGaze(g_gazeTx, g_gazeTy);
            UploadSlices(g, app, mov.in[0][i % mov.frames], mov.in[1][i % mov.frames]);
            DrawEye(g, tgt, views[0], 0, nullptr, false);
            DrawEye(g, tgt, views[1], 1, nullptr, false);
            Sleep(4);
        }
    };
    auto Status = [&]() { return sh && ReadStatus(sh, st); };

    // 1. no eye tracker: centred, and the status says so
    Ini(1, 0, 0, 0, 0.5f);
    Frames(0, 3);
    Sleep(300);
    Pump(g);
    std::vector<GazeRegion> r = GazeRegions();
    bool centred = r.size() == 2;
    for (auto &x : r) centred = centred && x.rx == (x.ow - x.rw) / 2 && x.ry == (x.oh - x.rh) / 2;
    CHECK(centred && Status() && st.gaze.source == 0, "no eye tracker: the area stays centred (status: no source)");

    // 2. the eye tracking client starts publishing
    HANDLE gm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FakeGaze), L"VRCFTMemmapData");
    g_fakeGaze = gm ? (volatile FakeGaze *)MapViewOfFile(gm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FakeGaze)) : nullptr;
    CHECK(g_fakeGaze != nullptr, "test: Bigscreen's shared gaze data stand-in created");
    if (!g_fakeGaze) return;
    g_gazeTx = 0; g_gazeTy = 0;
    for (int t = 0; t < 1300; t += 50) { WriteGaze(0, 0); Pump(g); Sleep(50); }   // the DLL looks once a second
    Frames(0, 3);
    Sleep(300);
    WriteGaze(0, 0);
    Pump(g);
    bool ok = Status() && st.gaze.source == 1 && st.gaze.live == 1;
    CHECK(ok, "the eye tracking data is found and live (%.0f samples/s)", ok ? st.gaze.rate : 0.0f);

    // 3. looking right and up: the area moves there, in both eyes
    g_gazeTx = 0.25f; g_gazeTy = 0.2f;   // -> (0.625, 0.4) of the picture
    Frames(3, 2);
    r = GazeRegions();
    ok = r.size() == 2;
    for (auto &x : r) {
        const long ex = std::max(0L, std::min(lroundf(0.625f * x.ow - 0.5f * x.rw), (long)(x.ow - x.rw)));
        const long ey = std::max(0L, std::min(lroundf(0.4f * x.oh - 0.5f * x.rh), (long)(x.oh - x.rh)));
        ok = ok && labs((long)x.rx - ex) <= 1 && labs((long)x.ry - ey) <= 1;
        printf("         eye %d: area %ux%u at (%u,%u), expected (%ld,%ld)\n", x.side, x.rw, x.rh, x.rx, x.ry, ex, ey);
    }
    CHECK(ok && r[0].side >= 0 && r[1].side >= 0 && r[0].side != r[1].side, "looking right and up moves the area there, in both eyes");
    const uint32_t keepX = r.empty() ? 0 : r[0].rx;
    g_gazeTx = 0.26f;   // a small movement (inside the dead zone) leaves it where it is
    Frames(5, 2);
    r = GazeRegions();
    CHECK(!r.empty() && r[0].rx == keepX, "a small eye movement leaves the area alone (dead zone)");

    // 4. a sequence with the gaze jumping left and right: the history moves with the area
    Ini(0, 0, 0, 0, 0.5f);
    Settle(g);
    g_frameHook = nullptr;
    SeqResult fixed = RunSequence(g, tgt, mov, app, views, 8);
    Ini(1, 0, 0, 0, 0.5f);
    g_frameHook = [](int i) { g_gazeTx = ((i / 4) & 1) ? 0.25f : -0.25f; g_gazeTy = 0; WriteGaze(g_gazeTx, g_gazeTy); };
    SeqResult jump = RunSequence(g, tgt, mov, app, views, 8);
    g_frameHook = nullptr;
    printf("         whole frame: fixed centre %.2f dB, gaze jumping +-0.25 every 4 frames %.2f dB\n", fixed.all.Psnr(), jump.all.Psnr());
    CHECK(AllFinite(jump.lastOut) && jump.all.Psnr() > fixed.all.Psnr() - 0.6,
          "with the area jumping after the gaze, the picture stays right (%+.2f dB vs a fixed area)", jump.all.Psnr() - fixed.all.Psnr());

    // 4b. the luma pyramid is built only around the area: the same picture as with the pyramid over
    //     the whole image, with the area fixed and with it jumping (where it moved on, the previous
    //     frame's pyramid is missing; the motion fit leaves that part out). On this small image the
    //     default margin (128 px) around even the smallest area reaches the edges, so with half of it.
    {
        SeqResult r[2][2];   // [whole pyramid][jumping]
        for (int wp = 0; wp < 2; ++wp)
            for (int j = 0; j < 2; ++j) {
                Ini(j, 0, 0, 0, 0.5f, 0.3f, wp ? 65536 : 64);
                Settle(g);
                g_frameHook = nullptr;
                if (j) g_frameHook = [](int i) { g_gazeTx = ((i / 4) & 1) ? 0.25f : -0.25f; g_gazeTy = 0; WriteGaze(g_gazeTx, g_gazeTy); };
                r[wp][j] = RunSequence(g, tgt, mov, app, views, 8);
                g_frameHook = nullptr;
            }
        const double same = Compare(r[0][0].lastOut, r[1][0].lastOut).psnr;
        printf("         area 0.3, pyramid around the area / over the whole image: fixed %.2f / %.2f dB (last frames %.1f dB apart), "
               "jumping %.2f / %.2f dB\n", r[0][0].all.Psnr(), r[1][0].all.Psnr(), same, r[0][1].all.Psnr(), r[1][1].all.Psnr());
        CHECK(fabs(r[0][0].all.Psnr() - r[1][0].all.Psnr()) < 0.05 && same > 40.0,
              "building the pyramid only around a fixed area changes nothing (%+.3f dB)", r[0][0].all.Psnr() - r[1][0].all.Psnr());
        CHECK(AllFinite(r[0][1].lastOut) && r[0][1].all.Psnr() > r[1][1].all.Psnr() - 0.15,
              "... and little with the area jumping after the gaze (%+.3f dB)", r[0][1].all.Psnr() - r[1][1].all.Psnr());
        Ini(1, 0, 0, 0, 0.5f);
    }

    // 4c. FSR 1 mode follows the gaze too, with the area's own value while following (gazeArea)
    {
        auto FsrShot = [&](int follow, float area) {
            snprintf(ini, sizeof(ini),
                     "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=%.2f\n"
                     "gazeArea=0.40\n[eyes]\nfollowGaze=%d\nprojection=-1 1 -1 1 -1 1 -1 1\n",
                     iniBase, reqShared, F, area, follow);
            WriteIni(ini);
            Pump(g);
            g_gazeTx = 0.25f; g_gazeTy = 0.2f;   // -> (0.625, 0.4) of the picture
            Frames(7, 2);
            return DrawEye(g, tgt, views[0], 0, nullptr, true);
        };
        const Image whole = FsrShot(0, 1.0f), follow = FsrShot(1, 0.6f);
        auto side = [](int out, float a) { int r = (int)ceilf(out * a / 16.0f) * 16; return r + 32 >= out ? out : r; };
        const int rw = side(OW, 0.4f), rh = side(OH, 0.4f), band = std::max(16, (int)lroundf(0.02f * std::min(OW, OH)));
        const int rx = std::max(0, std::min((int)lroundf(0.625f * OW - 0.5f * rw), OW - rw));
        const int ry = std::max(0, std::min((int)lroundf(0.4f * OH - 0.5f * rh), OH - rh));
        double inMax = 0, strip = 0;
        int n = 0;
        for (int y = ry + band + 2; y < ry + rh - band - 2; ++y)
            for (int x = 0; x < OW; ++x) {
                const float *a = follow.at(x, y), *b = whole.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    if (x >= rx + band + 2 && x < rx + rw - band - 2) inMax = std::max(inMax, (double)fabsf(a[c] - b[c]));
                    if (x >= rx - 30 && x < rx - 2) { strip += fabsf(a[c] - b[c]); ++n; }   // inside a 60% area, outside 40%
                }
            }
        strip /= std::max(n, 1);
        CHECK(inMax <= 1.5 / 255.0 && strip > 0.002,
              "FSR 1 mode: the area (40%% while following, %dx%d) sits around the gaze (FSR 1 there: largest difference %.1f/255; "
              "the cheap upscale just beside it: %.4f mean difference)", rw, rh, inMax * 255.0, strip);
    }

    // 5. the gaze moves while the game repeats a frame, and a setting forces a repaint: the area
    //    stays where its history was made until the next new frame
    Image before = DrawEye(g, tgt, views[0], 0, nullptr, true);
    g_gazeTx = -0.3f;
    WriteGaze(g_gazeTx, 0);
    Ini(1, 0, 0, 0, 0.55f);   // a new sharpness: every proxy is repainted
    Image after = HoldGaze(g, tgt, views[0], 120);
    double se = 0;
    for (size_t k = 0; k < before.px.size(); ++k) se += (before.px[k] - after.px[k]) * (before.px[k] - after.px[k]);
    const double psnr = 10.0 * log10(1.0 / std::max(se / before.px.size(), 1e-12));
    CHECK(psnr > 32.0, "a repaint of a repeated frame keeps the area where its history is (%.1f dB vs before)", psnr);

    // 6. calibration: the tracker reads 7 degrees right and 4.6 down while the user looks straight ahead
    Ini(1, 0, 0, 0, 0.5f);
    g_gazeTx = 0.12f; g_gazeTy = -0.08f;
    Frames(0, 2);
    if (g_hotkey) g_hotkey(7);   // Ctrl+F8
    Image cal;   // the game keeps delivering frames: the cross straight ahead (the centre here)
    for (int t = 0, i = 0; t < 600; t += 40, ++i) {
        WriteGaze(g_gazeTx, g_gazeTy);
        UploadSlices(g, app, mov.in[0][i % mov.frames], mov.in[1][i % mov.frames]);
        cal = DrawEye(g, tgt, views[0], 0, nullptr, true);
        DrawEye(g, tgt, views[1], 1, nullptr, false);
        Sleep(40);
    }
    SavePpm(cal, "t_gaze_cross.ppm");
    const float *c = cal.at(OW / 2, OH / 2);
    CHECK(c[0] > 0.85f && c[1] > 0.85f, "calibrating: the cross is drawn straight ahead (%.2f %.2f %.2f)", c[0], c[1], c[2]);
    HoldGaze(g, tgt, views[0], 2900);
    ok = Status() && st.gaze.calState == kVuCalDone;
    printf("         %s\n", Status() ? st.gaze.calMessage : "?");
    CHECK(ok && fabsf(st.current.gazeOffset[0] - 0.12f) < 0.005f && fabsf(st.current.gazeOffset[1] + 0.08f) < 0.005f,
          "calibration measures the offset (%.3f, %.3f) and publishes it for the app", st.current.gazeOffset[0], st.current.gazeOffset[1]);
    Frames(2, 2);
    r = GazeRegions();
    centred = !r.empty();
    for (auto &x : r) centred = centred && labs((long)x.rx - (long)(x.ow - x.rw) / 2) <= 12 && labs((long)x.ry - (long)(x.oh - x.rh) / 2) <= 12;
    CHECK(centred, "calibrated: looking straight ahead puts the area in the centre");
    // just after a calibration the gaze ring shows where the gaze lands (straight ahead now)
    Image ring = HoldGaze(g, tgt, views[0], 60);
    SavePpm(ring, "t_gaze_ring.ppm");
    const int R = std::max(9, (int)(0.013f * OW));
    const float *rg = ring.at(OW / 2 + R, OH / 2);
    CHECK(rg[1] > 0.7f && rg[0] < 0.75f, "the gaze ring is drawn where the gaze is (%.2f %.2f %.2f)", rg[0], rg[1], rg[2]);
    // a restless gaze fails the calibration and keeps the old offset
    if (g_hotkey) g_hotkey(7);
    for (int t = 0; t < 3300; t += 30) {
        g_gazeTx = 0.12f + ((t / 30) & 1 ? 0.15f : -0.15f);
        WriteGaze(g_gazeTx, g_gazeTy);
        Pump(g);
        DrawEye(g, tgt, views[0], 0, nullptr, false);
        Sleep(30);
    }
    ok = Status() && st.gaze.calState == kVuCalFailed && fabsf(st.current.gazeOffset[0] - 0.12f) < 0.005f;
    CHECK(ok, "a calibration with a restless gaze fails and keeps the offset (\"%s\")", Status() ? st.gaze.calMessage : "?");

    UnmapViewOfFile((void *)g_fakeGaze);
    g_fakeGaze = nullptr;
    CloseHandle(gm);
    if (sh) UnmapViewOfFile(sh);
    if (chMap) CloseHandle(chMap);
    Ini(0, 0, 0, 0, 0.5f);
    ClearAll(g);
    SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
    tgt.Release();
    Settle(g);
}

// Native (1x): the eye textures are processed at their own size; temporal then works as
// anti-aliasing (head micro-motion jitters the samples), FSR 1 only sharpens.
static void RunNativeTest(Gfx &g, const World &world, const char *iniBase, const char *reqShared, UINT misc)
{
    const int W = 400, H = 440;
    printf("[temporal: Native (1x): %dx%d processed at its own size]\n", W, H);
    Sequence q = MakeSequence(world, W, H, W, H, 16, true, 777);
    std::vector<Image> blank = { q.in[0][0], q.in[1][0] };
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, blank, misc);
    ID3D11ShaderResourceView *views[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                           MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, W, H);
    char ini[1024];
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=1.00\nsharpness=0.5\n[temporal]\ntemporalArea=1\n", iniBase,
             reqShared);
    WriteIni(ini);
    Pump(g);
    SeqResult rf = RunSequence(g, tgt, q, app, views, 6);
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=1.00\nsharpness=0.5\n[temporal]\ntemporalArea=1\n",
             iniBase, reqShared);
    WriteIni(ini);
    Pump(g);
    SeqResult rt = RunSequence(g, tgt, q, app, views, 6);
    printf("         region        FSR 1    temporal   (PSNR vs a 4x4-supersampled render, dB)\n");
    printf("         background   %6.2f    %6.2f\n", rf.bg.Psnr(), rt.bg.Psnr());
    printf("         grating      %6.2f    %6.2f\n", rf.grating.Psnr(), rt.grating.Psnr());
    printf("         steadiness   %6.2f    %6.2f\n", rf.flicker.Psnr(), rt.flicker.Psnr());
    ID3D11ShaderResourceView *bound = nullptr;
    UploadSlices(g, app, q.in[0][0], q.in[1][0]);
    CompositorSample(g, views[0], W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &bound);
    UINT bw = 0, bh = 0;
    if (bound) ViewSize(bound, &bw, &bh);
    CHECK(bound && bound != views[0] && (int)bw == W && (int)bh == H, "at 1x the eye texture is still processed, at its own size (%ux%u)",
          bw, bh);
    SafeRelease(bound);
    CHECK(AllFinite(rt.lastOut), "1x temporal output is finite and in range");
    CHECK(rt.bg.Psnr() > rf.bg.Psnr() + 1.0 && rt.grating.Psnr() > rf.grating.Psnr() + 1.0,
          "1x temporal anti-aliases: background %+.2f dB, fine detail %+.2f dB over FSR 1 at 1x", rt.bg.Psnr() - rf.bg.Psnr(),
          rt.grating.Psnr() - rf.grating.Psnr());
    CHECK(rt.flicker.Psnr() > rf.flicker.Psnr() + 0.5, "1x temporal is steadier (%+.2f dB)", rt.flicker.Psnr() - rf.flicker.Psnr());
    ClearAll(g);
    SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
    tgt.Release();
    Settle(g);
}

static void RunTemporalTests(Gfx &g, const char *outDir, const char *reqShared, bool sharedOk, bool qualityOnly)
{
    LoadDebugExports();
    // e.g. "maxWeight=4\nclipGamma=1.0" (test tuning); copied: EnvStr's buffer is reused by the next call
    const std::string extraStr = EnvStr("VRUPSCALE_TEST_EXTRA");
    const char *extra = extraStr.c_str();
    CHECK(g_debugStream != nullptr, "temporal debug export present");
    if (!g_debugStream) return;

    const int W = 400, H = 440;
    // VRUPSCALE_TEST_FACTOR: the quality table at another scaling preset (with VRUPSCALE_TEST_TUNE_ONLY)
    const float F = EnvStr("VRUPSCALE_TEST_FACTOR")[0] ? (float)atof(EnvStr("VRUPSCALE_TEST_FACTOR")) : 1.3f;
    const int OW = (int)lroundf(W * F), OH = (int)lroundf(H * F);
    const UINT misc = sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0;
    char ini[2048];
    const char *tIniBase =
        "[general]\nlogLevel=1\nstatsIntervalSec=0\nhotkeys=0\n"
        "[match]\nminWidth=300\nminHeight=300\nminAspect=0.6\nmaxAspect=1.6\nfollowCopies=1\n";

    printf("[temporal: building test scene]\n");
    World world = MakeWorld(2200, 2000);
    if (strcmp(EnvStr("VRUPSCALE_TEST_ONLY"), "switch") == 0) {   // one test alone (while working on it)
        RunResolutionSwitchTest(g, world, tIniBase, reqShared, F, misc);
        return;
    }
    if (strcmp(EnvStr("VRUPSCALE_TEST_ONLY"), "corner") == 0 || strcmp(EnvStr("VRUPSCALE_TEST_ONLY"), "cornerswitch") == 0) {
        RunSpatialCornerTest(g, world, tIniBase, reqShared, F, misc);
        if (strcmp(EnvStr("VRUPSCALE_TEST_ONLY"), "cornerswitch") == 0) RunResolutionSwitchTest(g, world, tIniBase, reqShared, F, misc);
        return;
    }
    if (strcmp(EnvStr("VRUPSCALE_TEST_ONLY"), "rcs") == 0) {
        RunResizedPoolTest(g, world, tIniBase, reqShared, F, misc);
        if (EnvStr("VRUPSCALE_TEST_NOCORNER")[0] != '1') RunSpatialCornerTest(g, world, tIniBase, reqShared, F, misc);
        RunResolutionSwitchTest(g, world, tIniBase, reqShared, F, misc);
        return;
    }
    const int seqFrames = EnvStr("VRUPSCALE_TEST_FRAMES")[0] ? std::max(12, atoi(EnvStr("VRUPSCALE_TEST_FRAMES"))) : 24;
    Sequence mov = MakeSequence(world, W, H, OW, OH, seqFrames, true, 777);
    Sequence still = MakeSequence(world, W, H, OW, OH, 4, false, 1);
    if (strcmp(EnvStr("VRUPSCALE_TEST_ONLY"), "gaze") == 0) {
        RunGazeTest(g, mov, tIniBase, reqShared, F, misc);
        return;
    }

    std::vector<Image> blank = { mov.in[0][0], mov.in[1][0] };
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, blank, misc);
    ID3D11ShaderResourceView *views[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                           MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, OW, OH);

    // ---- quality: FSR 1 vs temporal on the moving sequence
    printf("[temporal: quality vs FSR 1 on a moving sequence (%dx%d -> %dx%d, %d frames, 2 eyes)]\n", W, H, OW, OH, seqFrames);
    const float kFovArea = 0.6f;   // the default temporalArea
    SetFovRegion(OW, OH, kFovArea);   // also measure the regions the foveation test compares
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n", tIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    SeqResult rf;
    if (EnvStr("VRUPSCALE_TEST_SKIP_FSR")[0] != '1') rf = RunSequence(g, tgt, mov, app, views, 8);
    // the quality comparisons run temporal over the whole image (foveation has its own test)
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n%s\n",
             tIniBase, reqShared, F, extra);
    WriteIni(ini);
    Pump(g);
    SeqResult rt = RunSequence(g, tgt, mov, app, views, 8, true);
    PrintFlow("temporal", rt.flow);
    printf("         region        FSR 1    temporal   (PSNR vs ground truth, dB)\n");
    printf("         background   %6.2f    %6.2f\n", rf.bg.Psnr(), rt.bg.Psnr());
    printf("         grating      %6.2f    %6.2f\n", rf.grating.Psnr(), rt.grating.Psnr());
    printf("         trail        %6.2f    %6.2f\n", rf.trail.Psnr(), rt.trail.Psnr());
    printf("         object       %6.2f    %6.2f\n", rf.obj.Psnr(), rt.obj.Psnr());
    printf("         whole frame  %6.2f    %6.2f\n", rf.all.Psnr(), rt.all.Psnr());
    printf("         steadiness   %6.2f    %6.2f   (frame-to-frame error change, higher = less shimmer)\n", rf.flicker.Psnr(), rt.flicker.Psnr());
    if (EnvStr("VRUPSCALE_TEST_TUNE_ONLY")[0] == '1') {   // tuning sweeps: the table is all that is wanted
        ClearAll(g);
        SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
        tgt.Release();
        return;
    }
    // The defaults (0.4.5: Moving objects 1, Sharp history off) favour a steady image over the last
    // fraction of a dB: on this scene they gain about +0.5 dB on the background, +0.6 dB on the
    // grating and +2.9 dB in steadiness. The bars below catch a broken accumulation (no gain at
    // all); the stricter ones run with Moving objects off further down.
    CHECK(AllFinite(rt.lastOut), "temporal output is finite and in range");
    CHECK(rt.bg.Psnr() > rf.bg.Psnr() + 0.3, "temporal beats FSR 1 on steady background (+%.2f dB)", rt.bg.Psnr() - rf.bg.Psnr());
    CHECK(rt.grating.Psnr() > rf.grating.Psnr() + 0.3, "temporal recovers detail above the input Nyquist limit (+%.2f dB)",
          rt.grating.Psnr() - rf.grating.Psnr());
    CHECK(rt.flicker.Psnr() > rf.flicker.Psnr() + 1.0, "temporal is steadier than FSR 1 (+%.2f dB less shimmer)",
          rt.flicker.Psnr() - rf.flicker.Psnr());
    // the default keeps history on moving things too (Moving objects = 1): some smearing on the object
    // and a little in the area it uncovers is the price, but neither may fall apart
    CHECK(rt.trail.Psnr() > rf.trail.Psnr() - 2.0, "little ghosting behind the moving object with Moving objects on (%+.2f dB)",
          rt.trail.Psnr() - rf.trail.Psnr());
    CHECK(rt.obj.Psnr() > rf.obj.Psnr() - 5.0, "moving object stays recognisable with Moving objects on (%+.2f dB)",
          rt.obj.Psnr() - rf.obj.Psnr());
    if (extraStr.empty()) {
        // Moving objects off: no history where motion differs from the head's, so no smearing at all
        snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\nlocalTrust=0\n",
                 tIniBase, reqShared, F);
        WriteIni(ini);
        Pump(g);
        SeqResult r0 = RunSequence(g, tgt, mov, app, views, 8);
        printf("         Moving objects off: object %.2f dB, trail %.2f dB, background %.2f dB\n", r0.obj.Psnr(), r0.trail.Psnr(),
               r0.bg.Psnr());
        CHECK(r0.obj.Psnr() > rf.obj.Psnr() - 1.5, "Moving objects off: the moving object stays intact (%+.2f dB)",
              r0.obj.Psnr() - rf.obj.Psnr());
        CHECK(r0.trail.Psnr() > rf.trail.Psnr() - 0.5, "Moving objects off: no ghosting behind it (%+.2f dB)", r0.trail.Psnr() - rf.trail.Psnr());
        CHECK(r0.bg.Psnr() > rf.bg.Psnr() + 0.5, "Moving objects off: clearly beats FSR 1 on steady background (+%.2f dB)",
              r0.bg.Psnr() - rf.bg.Psnr());
        // the sharper history filter (off by default)
        snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\nsharpHistory=1\n",
                 tIniBase, reqShared, F);
        WriteIni(ini);
        Pump(g);
        SeqResult rc = RunSequence(g, tgt, mov, app, views, 8);
        printf("         Sharp history on: background %.2f dB, grating %.2f dB, steadiness %.2f dB (off: %.2f, %.2f, %.2f)\n", rc.bg.Psnr(),
               rc.grating.Psnr(), rc.flicker.Psnr(), rt.bg.Psnr(), rt.grating.Psnr(), rt.flicker.Psnr());
        CHECK(AllFinite(rc.lastOut) && rc.grating.Psnr() > rt.grating.Psnr() + 0.3,
              "Sharp history on keeps more fine detail (+%.2f dB on the grating)", rc.grating.Psnr() - rt.grating.Psnr());
        snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n",
                 tIniBase, reqShared, F);
        WriteIni(ini);
        Pump(g);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\t_gt.ppm", outDir); SavePpm(rt.lastGt, path);
    snprintf(path, sizeof(path), "%s\\t_temporal.ppm", outDir); SavePpm(rt.lastOut, path);
    snprintf(path, sizeof(path), "%s\\t_fsr.ppm", outDir); SavePpm(rf.lastOut, path);
    snprintf(path, sizeof(path), "%s\\t_input.ppm", outDir); SavePpm(mov.in[0][mov.frames - 1], path);
    if (qualityOnly) {
        RunFoveationTest(g, tgt, mov, tIniBase, reqShared, F, misc, kFovArea, rf, rt, outDir);
        RunFoveatedFsrTest(g, tgt, mov, tIniBase, reqShared, F, misc, kFovArea, rf, outDir);
        RunSwapchainPoolTest(g, mov, tIniBase, reqShared, F, misc);
        RunResizedPoolTest(g, world, tIniBase, reqShared, F, misc);
        RunSpatialCornerTest(g, world, tIniBase, reqShared, F, misc);
        RunResolutionSwitchTest(g, world, tIniBase, reqShared, F, misc);
        ClearAll(g);
        SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
        tgt.Release();
        RunGazeTest(g, mov, tIniBase, reqShared, F, misc);
        RunNativeTest(g, world, tIniBase, reqShared, misc);
        return;
    }

    // scene cut (teleport / world change): the first frame after it must not show the old scene
    {
        Sequence cut = MakeSequence(world, W, H, OW, OH, 1, false, 3);
        cut.f[0].ox += 170.0f; cut.f[0].oy += 95.0f;
        cut.in[0][0] = RenderScene(world, cut.f[0], 0, W, H, 1.0f);
        cut.in[1][0] = RenderScene(world, cut.f[0], 1, W, H, 1.0f);
        cut.gt[0][0] = RenderScene(world, cut.f[0], 0, OW, OH, (float)W / OW);
        cut.gt[1][0] = RenderScene(world, cut.f[0], 1, OW, OH, (float)W / OW);
        SeqResult c = RunSequence(g, tgt, cut, app, views, 0);
        double fsrRef = rf.all.Psnr();
        printf("         scene cut: first frame after the cut %.2f dB (sequence FSR 1 average %.2f dB)\n", c.all.Psnr(), fsrRef);
        CHECK(c.all.Psnr() > fsrRef - 1.5 || fsrRef == 0.0, "no ghost of the previous scene after a cut (%.2f dB)", c.all.Psnr());
    }

    int ns = StreamCount();
    CHECK(ns == 2, "one temporal stream per eye (%d)", ns);
    {
        uint32_t st[32] = {};
        uint32_t newFrames = 0;
        for (int i = 0; i < ns; ++i) {
            g_debugStream(i, st);
            newFrames += st[12];
        }
        // (the two comparison runs above, Moving objects off and Sharp history on, continue the same histories)
        const int expect = 2 * (mov.frames + 1) + (extraStr.empty() ? 2 * 2 * mov.frames : 0);
        CHECK(newFrames == (uint32_t)expect, "every new app frame advanced the history exactly once (%u of %d)", newFrames, expect);
    }

    RunFoveationTest(g, tgt, mov, tIniBase, reqShared, F, misc, kFovArea, rf, rt, outDir);
    RunFoveatedFsrTest(g, tgt, mov, tIniBase, reqShared, F, misc, kFovArea, rf, outDir);

    // ---- still scene: no extra information, must not be worse than FSR 1 by much
    printf("[temporal: still scene]\n");
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n", tIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    SeqResult sf = RunSequence(g, tgt, still, app, views, 1);
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n[temporal]\ntemporalArea=1\n",
             tIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    SeqResult st = RunSequence(g, tgt, still, app, views, 1);
    printf("         still scene  FSR 1 %.2f dB, temporal %.2f dB\n", sf.all.Psnr(), st.all.Psnr());
    CHECK(st.all.Psnr() > sf.all.Psnr() - 0.75, "with no motion, temporal is close to FSR 1 (%+.2f dB)", st.all.Psnr() - sf.all.Psnr());

    // ---- repeated frames: re-sampling the same app frame must not advance the history
    printf("[temporal: repeated and older frames]\n");
    {
        uint32_t a[32] = {}, b[32] = {};
        int idx = 0;   // the eye-0 stream is the first one listed
        UploadSlices(g, app, mov.in[0][3], mov.in[1][3]);
        DrawEye(g, tgt, views[0], 0, nullptr, false);
        Sleep(5);
        g_debugStream(idx, a);
        DrawEye(g, tgt, views[0], 0, nullptr, false);
        Sleep(5);
        DrawEye(g, tgt, views[0], 0, nullptr, false);
        g_debugStream(idx, b);
        CHECK(b[12] == a[12] && b[13] == a[13] + 2, "two re-samples of one app frame: 0 history updates, 2 checks (%u, %u)",
              b[12] - a[12], b[13] - a[13]);
        // older frame comes back (the compositor re-reading the previous app frame)
        Sleep(5);
        UploadSlices(g, app, mov.in[0][4], mov.in[1][4]);
        DrawEye(g, tgt, views[0], 0, nullptr, false);
        Sleep(5);
        UploadSlices(g, app, mov.in[0][3], mov.in[1][3]);
        g_debugStream(idx, a);
        DrawEye(g, tgt, views[0], 0, nullptr, false);
        g_debugStream(idx, b);
        CHECK(b[12] == a[12], "an older app frame shown again does not advance the history");
    }

    // ---- input formats through the temporal path
    printf("[temporal: typed sRGB and HDR float sources]\n");
    {
        Sequence q = MakeSequence(world, W, H, OW, OH, 3, true, 99);
        // typed sRGB vs typeless: same bytes, same views; must give the same result
        ID3D11Texture2D *tr = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { q.in[0][0], q.in[1][0] }, misc);
        ID3D11ShaderResourceView *vr[2] = { MakeView(g, tr, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0), MakeView(g, tr, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
        SeqResult refRes = RunSequence(g, tgt, q, tr, vr, 1);
        ClearAll(g);
        SafeRelease(vr[0]); SafeRelease(vr[1]); SafeRelease(tr);
        Settle(g);
        ID3D11Texture2D *ts = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, { q.in[0][0], q.in[1][0] }, misc);
        ID3D11ShaderResourceView *vs[2] = { MakeView(g, ts, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0), MakeView(g, ts, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
        SeqResult a = RunSequence(g, tgt, q, ts, vs, 1);
        CHECK(AllFinite(a.lastOut) && fabs(a.all.Psnr() - refRes.all.Psnr()) < 0.6,   // a wrong transform is off by several dB
              "typed sRGB source matches the typeless path (%.2f vs %.2f dB)", a.all.Psnr(), refRes.all.Psnr());
        ClearAll(g);
        SafeRelease(vs[0]); SafeRelease(vs[1]); SafeRelease(ts);
        Settle(g);

        // float: upload the 8-bit frames as linear HDR (x3) and compare in the same encoding
        std::vector<Image> fl = { q.in[0][0], q.in[1][0] };
        for (auto &im : fl) for (auto &v : im.px) v = HalfToFloat(FloatToHalf(SrgbToLinear(v) * 3.0f));
        ID3D11Texture2D *tf = MakeTexture(g, DXGI_FORMAT_R16G16B16A16_FLOAT, fl, misc);
        ID3D11ShaderResourceView *vf = MakeView(g, tf, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        LONG tr0 = Stats().temporalRuns;
        Image out = CompositorSample(g, vf, OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT);
        CHECK(Stats().temporalRuns > tr0, "HDR float source went through the temporal path");
        bool finite = true;
        double sum = 0, ref = 0;
        for (size_t i = 0; i < out.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) {
                if (!(out.px[i + c] >= 0.0f && out.px[i + c] < 65504.0f)) finite = false;
                sum += out.px[i + c];
            }
        for (size_t i = 0; i < fl[0].px.size(); i += 4)
            for (int c = 0; c < 3; ++c) ref += fl[0].px[i + c];
        double meanOut = sum / (out.px.size() / 4 * 3), meanIn = ref / (fl[0].px.size() / 4 * 3);
        CHECK(finite && fabs(meanOut / meanIn - 1.0) < 0.03, "HDR float source: finite, brightness preserved (%.4f vs %.4f)",
              meanOut, meanIn);
        ClearAll(g);
        SafeRelease(vf); SafeRelease(tf);
        Settle(g);
    }

    // ---- compute / pixel state is restored around the (many) temporal passes
    printf("[temporal: state preservation]\n");
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D *t1 = nullptr, *t2 = nullptr, *t3 = nullptr;
        g.dev->CreateTexture2D(&td, nullptr, &t1);
        g.dev->CreateTexture2D(&td, nullptr, &t2);
        g.dev->CreateTexture2D(&td, nullptr, &t3);
        ID3D11ShaderResourceView *srv = nullptr, *ps2 = nullptr;
        ID3D11UnorderedAccessView *uav = nullptr;
        g.dev->CreateShaderResourceView(t1, nullptr, &srv);
        g.dev->CreateUnorderedAccessView(t2, nullptr, &uav);
        g.dev->CreateShaderResourceView(t3, nullptr, &ps2);
        g.keepState = true;   // the bindings must survive the command list being finished and executed
        g.rc->CSSetShaderResources(5, 1, &srv);
        UINT keep = (UINT)-1;
        g.rc->CSSetUnorderedAccessViews(3, 1, &uav, &keep);
        g.rc->PSSetShaderResources(2, 1, &ps2);
        if (g.deferred) {   // with command lists our passes run on the immediate context: its state too
            g.ctx->CSSetShaderResources(5, 1, &srv);
            g.ctx->CSSetUnorderedAccessViews(3, 1, &uav, &keep);
        }
        UploadSlices(g, app, mov.in[0][10], mov.in[1][10]);
        LONG r0 = Stats().temporalRuns;
        // draw with PS t2 still bound, through the same path as DrawEye
        float cbData[8] = { (float)tgt.w, (float)tgt.h, 0, 0, 0, 0, 1, 1 };
        g.rc->UpdateSubresource(g.cb, 0, nullptr, cbData, 0, 0);
        g.rc->OMSetRenderTargets(1, &tgt.rtv, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)tgt.w, (float)tgt.h, 0, 1 };
        g.rc->RSSetViewports(1, &vp);
        g.rc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.rc->VSSetShader(g.vs, nullptr, 0);
        g.rc->PSSetShader(g.ps, nullptr, 0);
        g.rc->PSSetConstantBuffers(0, 1, &g.cb);
        g.rc->PSSetSamplers(0, 1, &g.point);
        g.rc->PSSetShaderResources(0, 1, &views[0]);
        g.rc->Draw(3, 0);
        Submit(g);   // command lists: the passes run as the list executes
        CHECK(Stats().temporalRuns > r0, "a temporal run happened in the middle of this state");
        if (g.deferred) {
            ID3D11ShaderResourceView *is = nullptr;
            ID3D11UnorderedAccessView *iu = nullptr;
            g.ctx->CSGetShaderResources(5, 1, &is);
            g.ctx->CSGetUnorderedAccessViews(3, 1, &iu);
            CHECK(is == srv && iu == uav, "immediate context: CS t5 / u3 restored after the passes run before the command list");
            SafeRelease(is); SafeRelease(iu);
        }
        ID3D11ShaderResourceView *gs = nullptr, *gp = nullptr, *g0 = nullptr;
        ID3D11UnorderedAccessView *gu = nullptr;
        g.rc->CSGetShaderResources(5, 1, &gs);
        g.rc->CSGetUnorderedAccessViews(3, 1, &gu);
        g.rc->PSGetShaderResources(2, 1, &gp);
        g.rc->PSGetShaderResources(0, 1, &g0);
        UINT bw = 0, bh = 0;
        if (g0) ViewSize(g0, &bw, &bh);
        CHECK(gs == srv && gu == uav, "CS t5 / u3 restored after the temporal passes");
        CHECK(gp == ps2, "unrelated PS t2 binding untouched");
        CHECK(g0 && (int)bw == OW, "PS t0 still holds the proxy");
        SafeRelease(gs); SafeRelease(gu); SafeRelease(gp); SafeRelease(g0);
        g.keepState = false;
        ClearAll(g);
        SafeRelease(srv); SafeRelease(uav); SafeRelease(ps2); SafeRelease(t1); SafeRelease(t2); SafeRelease(t3);
    }

    // ---- a rotating pool of eye textures shares one history per eye
    printf("[temporal: rotating texture pool]\n");
    ClearAll(g);
    SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
    Settle(g);   // the old eye texture must really be gone, or the pool links to its stream
    {
        ID3D11Texture2D *pool[3] = {};
        ID3D11ShaderResourceView *pv[3][2] = {};
        for (int k = 0; k < 3; ++k) {
            pool[k] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, blank, misc);
            pv[k][0] = MakeView(g, pool[k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0);
            pv[k][1] = MakeView(g, pool[k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1);
        }
        for (int i = 0; i < 9; ++i) {
            int k = i % 3;
            UploadSlices(g, pool[k], mov.in[0][i], mov.in[1][i]);
            DrawEye(g, tgt, pv[k][0], 0, nullptr, false);
            DrawEye(g, tgt, pv[k][1], 1, nullptr, false);
            Sleep(4);
        }
        int n = StreamCount();
        uint32_t s0[32] = {}, s1[32] = {};
        if (n >= 1) g_debugStream(0, s0);
        if (n >= 2) g_debugStream(1, s1);
        CHECK(n == 2 && s0[17] == 3 && s1[17] == 3, "3 pool textures per eye -> 2 streams of 3 textures (%d streams: %u, %u)",
              n, s0[17], s1[17]);
        CHECK(s0[12] == 9 && s1[12] == 9, "pool: each eye's history saw all 9 frames (%u, %u)", s0[12], s1[12]);
        // the app shows one identical frame for a while (loading screen): every pool texture's
        // proxy must show that frame, not whatever it held last
        Image same = mov.in[0][20];
        Image ref0 = DrawEye(g, tgt, pv[0][0], 0, nullptr, true);   // warm the readback path
        (void)ref0;
        double worst = 99;
        Image first;
        for (int i = 0; i < 6; ++i) {
            int k = (9 + i) % 3;
            UploadSlices(g, pool[k], same, mov.in[1][20]);
            Image out = DrawEye(g, tgt, pv[k][0], 0, nullptr, true);
            if (i == 0) first = out;
            else worst = std::min(worst, Compare(out, first).psnr);
            Sleep(4);
        }
        CHECK(worst > 45.0, "pool re-filled with one identical frame: every texture shows it (worst %.1f dB vs the first)", worst);
        ClearAll(g);
        for (int k = 0; k < 3; ++k) { SafeRelease(pv[k][0]); SafeRelease(pv[k][1]); SafeRelease(pool[k]); }
    }
    Settle(g);
    RunSwapchainPoolTest(g, mov, tIniBase, reqShared, F, misc);
    RunResizedPoolTest(g, world, tIniBase, reqShared, F, misc);
    RunSpatialCornerTest(g, world, tIniBase, reqShared, F, misc);
    RunResolutionSwitchTest(g, world, tIniBase, reqShared, F, misc);
    RunGazeTest(g, mov, tIniBase, reqShared, F, misc);
    RunNativeTest(g, world, tIniBase, reqShared, misc);
    CHECK(Stats().liveStreams == 0, "temporal streams released with their textures (%ld left)", (long)Stats().liveStreams);
    tgt.Release();
}

// ---------------------------------------------------------------------------------------------
// GPU cost at realistic VRChat sizes (Valve Index at 60%: 1561x1735 x2 -> 2014x2238, and a
// high-resolution headset at 50%: 2756x2756 x2 -> 3555x3555), measured the way it happens in VR: every frame the "game" keeps the GPU busy for ~5 ms (a calibrated pixel-
// shader load, so the GPU runs at its working clocks), a new app frame is copied into the eye
// texture, and the compositor draws both eyes; at most 2 frames are in flight. One timestamp pair
// per frame brackets both eyes' draws; "off" is the compositor's own draws with no upscaler.

static uint64_t SumNewFrames()
{
    uint64_t sum = 0;
    int n = StreamCount();
    for (int i = 0; i < n; ++i) {
        uint32_t st[32] = {};
        if (g_debugStream(i, st)) sum += st[12];
    }
    return sum;
}

static void RunPerfTestAt(Gfx &g, const char *reqShared, bool sharedOk, int frames, int W, int H, int OW, int OH);

static void RunPerfTest(Gfx &g, const char *reqShared, bool sharedOk, int frames)
{
    if (frames <= 0) return;
    RunPerfTestAt(g, reqShared, sharedOk, frames, 1561, 1735, 2014, 2238);
    if (EnvStr("VRUPSCALE_PERF_LARGE")[0] != '0') RunPerfTestAt(g, reqShared, sharedOk, frames, 2756, 2756, 3555, 3555);
}

static void RunPerfTestAt(Gfx &g, const char *reqShared, bool sharedOk, int frames, int W, int H, int OW, int OH)
{
    const double warmupMs = 1500.0;
    printf("[performance: %dx%d x2 -> %dx%d, sustained load, %d frames per mode after %.1f s warm-up]\n", W, H, OW, OH,
           frames, warmupMs / 1000.0);
    World world = MakeWorld(2200, 2000);
    Rng rng(5);
    const int distinct = 4;
    const UINT misc = sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0;
    ID3D11Texture2D *src[distinct] = {};
    std::vector<Image> firstImgs;
    for (int i = 0; i < distinct; ++i) {
        SceneFrame f = { 10.0f + 0.4f * i + 0.3f * rng.N(), 5.0f + 0.2f * i + 0.3f * rng.N(), 300.0f + 3.0f * i, 600.0f };
        std::vector<Image> eyesImg;
        for (int e = 0; e < 2; ++e) {
            Image im;
            im.w = W; im.h = H;
            im.px.resize(size_t(W) * H * 4);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    V3 c = SceneAt(world, f, e, x + 0.5f, y + 0.5f);
                    float *p = im.at(x, y);
                    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 1;
                }
            eyesImg.push_back(Quantize8(im));
        }
        src[i] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, eyesImg, 0);   // never sampled: not an eye texture
        if (i == 0) firstImgs = eyesImg;
    }
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, firstImgs, misc);
    ID3D11ShaderResourceView *views[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                           MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, OW, OH);

    std::vector<ID3D11Query *> ts(size_t(frames) * 2, nullptr);
    std::vector<ID3D11Query *> djf(size_t(frames), nullptr);   // one disjoint query per timed frame
    ID3D11Query *fence[3] = {};
    D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 }, qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qe = { D3D11_QUERY_EVENT, 0 };
    for (auto &q : ts) g.dev->CreateQuery(&qt, &q);
    for (auto &q : djf) g.dev->CreateQuery(&qd, &q);
    for (auto &q : fence) g.dev->CreateQuery(&qe, &q);

    // the "game": a 1024x1024 pixel-shader load, calibrated during warm-up to ~5 ms of GPU time
    const double loadTargetMs = 5.0;
    ID3D11Texture2D *loadTex = nullptr;
    ID3D11RenderTargetView *loadRtv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1024; td.Height = 1024; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        g.dev->CreateTexture2D(&td, nullptr, &loadTex);
        if (loadTex) g.dev->CreateRenderTargetView(loadTex, nullptr, &loadRtv);
    }
    ID3D11Query *loadQ[3][3] = {};   // per in-flight frame: disjoint, begin, end
    for (auto &f3 : loadQ) {
        g.dev->CreateQuery(&qd, &f3[0]);
        g.dev->CreateQuery(&qt, &f3[1]);
        g.dev->CreateQuery(&qt, &f3[2]);
    }
    double loadIters = 64;
    auto DrawLoad = [&](int slot, bool timed) {   // timed only during warm-up: no nested disjoint queries
        if (!loadRtv || !g.load) return;
        float cbData[8] = { (float)(int)loadIters, 0, 0, 0, 0, 0, 1, 1 };
        g.ctx->UpdateSubresource(g.cb, 0, nullptr, cbData, 0, 0);
        g.ctx->OMSetRenderTargets(1, &loadRtv, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, 1024, 1024, 0, 1 };
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs, nullptr, 0);
        g.ctx->PSSetShader(g.load, nullptr, 0);
        g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
        if (timed) { g.ctx->Begin(loadQ[slot][0]); g.ctx->End(loadQ[slot][1]); }
        g.ctx->Draw(3, 0);
        if (timed) { g.ctx->End(loadQ[slot][2]); g.ctx->End(loadQ[slot][0]); }
        ID3D11RenderTargetView *nullRtv = nullptr;
        g.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    };
    auto Calibrate = [&](int slot) {   // the slot's frame has completed (its fence was waited on)
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d = {};
        UINT64 a = 0, b = 0;
        if (g.ctx->GetData(loadQ[slot][0], &d, sizeof(d), 0) != S_OK || d.Disjoint || !d.Frequency) return;
        if (g.ctx->GetData(loadQ[slot][1], &a, sizeof(a), 0) != S_OK || g.ctx->GetData(loadQ[slot][2], &b, sizeof(b), 0) != S_OK || b <= a)
            return;
        double ms = double(b - a) * 1000.0 / double(d.Frequency);
        double k = loadTargetMs / std::max(ms, 0.01);
        k = std::min(std::max(k, 0.25), 4.0);
        loadIters = std::min(std::max(loadIters * (1.0 + 0.7 * (k - 1.0)), 1.0), 65536.0);
    };

    // the last run repeats foveated temporal with per-pass timestamps (kept out of the table: they add a little)
    enum { kOff, kBilinear, kFsr, kFsrFov, kTemporalFull, kTemporalFov, kTemporalFovBil, kPerPass, kRuns };
    const char *names[kRuns] = { "off", "bilinear", "fsr, whole image", "fsr, area 0.6", "temporal, whole image", "temporal, area 0.6",
                                 "  + bilinear edges", "per-pass timestamps" };
    const char *modeIni[kRuns] = { "enabled=0\n", "mode=bilinear\n", "mode=fsr\n", "mode=fsr\n", "mode=temporal\n", "mode=temporal\n",
                                   "mode=temporal\n", "mode=temporal\n" };
    const char *areaIni[kRuns] = { "1", "1", "1", "0.6", "1", "0.6", "0.6\nbilinearEdges=1", "0.6" };
    double mean[kRuns] = {}, med[kRuns] = {};
    uint64_t newFrames[kRuns] = {};
    uint32_t serial = 1;
    char ini[1024];
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    // (With command lists the passes run on the immediate context too, so they can be timed.)
    const int runs = g_passTimes ? kRuns : kRuns - 1;
    for (int m = 0; m < runs; ++m) {
        const bool perPass = m == kPerPass;
        snprintf(ini, sizeof(ini),
                 "[general]\nlogLevel=1\nstatsIntervalSec=0\nhotkeys=0\ngpuTiming=0\npassTiming=%d\n[match]\nrequireShared=%s\n"
                 "[upscale]\n%supscaleFactor=%.4f\nsharpness=0.5\n[temporal]\ntemporalArea=%s\n", perPass ? 1 : 0, reqShared,
                 modeIni[m], (double)OH / (double)H, areaIni[m]);
        WriteIni(ini);
        Pump(g);
        if (perPass) g_passTimes(nullptr, nullptr, 0, 1);   // discard anything older
        uint64_t nf0 = 0;
        LARGE_INTEGER t0, t, lastDraw[2] = {};
        QueryPerformanceCounter(&t0);
        bool measuring = false;
        int mi = 0;   // measured frame index
        for (int f = 0;; ++f) {
            if (!measuring) {
                QueryPerformanceCounter(&t);
                if (double(t.QuadPart - t0.QuadPart) * 1000.0 / double(qf.QuadPart) >= warmupMs) {
                    measuring = true;
                    nf0 = SumNewFrames();
                    if (perPass) g_passTimes(nullptr, nullptr, 0, 1);   // only the measured frames
                }
            }
            if (measuring && mi == frames) break;
            DrawLoad(f % 3, !measuring);
            // new app frame: GPU copy + one unique 16x16 block per eye
            g.ctx->CopyResource(app, src[f % distinct]);
            for (UINT e = 0; e < 2; ++e) {
                // mid-grey values that change every frame, over 16x16 pixels: the frame fingerprint
                // samples one pixel per 8x8 block
                uint32_t v = serial++;
                uint32_t px[256];
                for (auto &p : px) p = 0xff000000u | (200u << 16) | ((128u + ((v >> 7) & 127u)) << 8) | (128u + (v & 127u));
                D3D11_BOX box = { 0, 0, 0, 16, 16, 1 };
                g.ctx->UpdateSubresource(app, D3D11CalcSubresource(0, e, 1), &box, px, 64, 0);
            }
            const bool timeIt = measuring && !perPass;   // the per-pass run has its own (DLL-side) queries
            if (timeIt) { g.ctx->Begin(djf[mi]); g.ctx->End(ts[size_t(mi) * 2]); }
            for (int e = 0; e < 2; ++e) {
                // at least 4 ms between draws of the same eye, so the DLL's dedupe window (3 ms by
                // default; a real compositor draws each eye once per 7+ ms) never merges two frames
                for (;;) {
                    QueryPerformanceCounter(&t);
                    if (lastDraw[e].QuadPart == 0 || double(t.QuadPart - lastDraw[e].QuadPart) * 1000.0 / double(qf.QuadPart) >= 4.0)
                        break;
                    Sleep(0);
                }
                lastDraw[e] = t;
                DrawEye(g, tgt, views[e], e, nullptr, false);
            }
            if (timeIt) { g.ctx->End(ts[size_t(mi) * 2 + 1]); g.ctx->End(djf[mi]); }
            if (measuring) ++mi;
            g.ctx->End(fence[f % 3]);
            g.ctx->Flush();
            if (f >= 2) {   // at most 2 frames queued, like a compositor waiting on its swap chain
                while (g.ctx->GetData(fence[(f - 2) % 3], nullptr, 0, 0) == S_FALSE) Sleep(0);
                if (!measuring) Calibrate((f - 2) % 3);   // frame f-2 was a timed warm-up frame
            }
            if (f > 100000) break;   // safety
        }
        if (perPass) {   // drain the GPU so every pass timestamp is readable
            for (auto &q : fence) while (g.ctx->GetData(q, nullptr, 0, 0) == S_FALSE) Sleep(0);
            continue;
        }
        // Frames whose disjoint query reports a clock discontinuity are skipped. Some drivers (DXVK)
        // flag every one now and then; if all are flagged, use them anyway and say so.
        std::vector<double> ms, msFlagged;
        for (int i = 0; i < mi; ++i) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d = {};
            UINT64 a = 0, b = 0;
            while (g.ctx->GetData(djf[i], &d, sizeof(d), 0) == S_FALSE) Sleep(0);
            while (g.ctx->GetData(ts[size_t(i) * 2], &a, sizeof(a), 0) == S_FALSE) Sleep(0);
            while (g.ctx->GetData(ts[size_t(i) * 2 + 1], &b, sizeof(b), 0) == S_FALSE) Sleep(0);
            if (!d.Frequency || b < a) continue;
            (d.Disjoint ? msFlagged : ms).push_back(double(b - a) * 1000.0 / double(d.Frequency));
        }
        newFrames[m] = SumNewFrames() - nf0;
        if (ms.empty() && !msFlagged.empty()) {
            printf("         note: the GPU timer flagged every %s frame as disjoint; using them anyway\n", names[m]);
            ms.swap(msFlagged);
        }
        if (ms.empty()) { mean[m] = med[m] = -1; continue; }
        double sum = 0;
        for (double v : ms) sum += v;
        mean[m] = sum / ms.size();
        std::sort(ms.begin(), ms.end());
        med[m] = ms[ms.size() / 2];
    }
    printf("         (game load per frame: %d iterations, calibrated to ~%.0f ms)\n", (int)loadIters, loadTargetMs);
    printf("         mode                    GPU ms per frame (both eyes)   added by the upscaler (median)\n");
    for (int m = 0; m < kPerPass; ++m) {
        if (m == 0)
            printf("         %-22s  mean %6.3f  median %6.3f    (compositor draws only)\n", names[m], mean[m], med[m]);
        else
            printf("         %-22s  mean %6.3f  median %6.3f    %+.3f ms\n", names[m], mean[m], med[m], med[m] - med[0]);
    }
    if (runs == kRuns) {
        double pm[16] = {};
        const char *pn[16] = {};
        int np = g_passTimes(pm, pn, 16, 1);
        double tot = 0;
        for (int i = 0; i < np; ++i) tot += pm[i];
        printf("         temporal passes at area 0.6, GPU ms per eye (sum %.3f):\n", tot);
        for (int i = 0; i < np; ++i) printf("           %-13s %6.3f  %4.1f%%\n", pn[i], pm[i], tot > 0 ? 100.0 * pm[i] / tot : 0.0);
        CHECK(np > 0 && tot > 0, "per-pass GPU timing collected (%d pass groups)", np);
    }
    CHECK(med[kTemporalFull] > 0 && med[kTemporalFov] > 0, "temporal path timed (%.3f / %.3f ms per frame, both eyes)",
          med[kTemporalFull], med[kTemporalFov]);
    CHECK(newFrames[kTemporalFull] >= (uint64_t)frames * 2 && newFrames[kTemporalFov] >= (uint64_t)frames * 2,
          "every timed frame was a full temporal update on both eyes (%llu and %llu of %d)",
          (unsigned long long)newFrames[kTemporalFull], (unsigned long long)newFrames[kTemporalFov], frames * 2);
    {
        const double full = med[kTemporalFull] - med[kOff], fov = med[kTemporalFov] - med[kOff];
        printf("         foveation (area 0.6) costs %.0f%% of whole-image temporal\n", full > 0 ? 100.0 * fov / full : 0.0);
        const double bil = med[kTemporalFovBil] - med[kOff];
        printf("         bilinear edges (area 0.6): %+.3f ms per frame (%+.0f%%)\n", bil - fov, fov > 0 ? 100.0 * (bil - fov) / fov : 0.0);
        CHECK(fov < full, "foveated temporal is cheaper than whole-image temporal (%.3f vs %.3f ms)", fov, full);
        const double fsrFull = med[kFsr] - med[kOff], fsrFov = med[kFsrFov] - med[kOff];
        printf("         FSR 1 in the area (0.6) costs %.0f%% of whole-image FSR 1\n", fsrFull > 0 ? 100.0 * fsrFov / fsrFull : 0.0);
        CHECK(fsrFov < fsrFull, "FSR 1 in the area only is cheaper than over the whole image (%.3f vs %.3f ms)", fsrFov, fsrFull);
    }
    for (auto &q : ts) SafeRelease(q);
    for (auto &q : djf) SafeRelease(q);
    for (auto &q : fence) SafeRelease(q);
    for (auto &f3 : loadQ) for (auto &q : f3) SafeRelease(q);
    SafeRelease(loadRtv); SafeRelease(loadTex);
    ClearAll(g);
    SafeRelease(views[0]); SafeRelease(views[1]); SafeRelease(app);
    for (auto &t2 : src) SafeRelease(t2);
    tgt.Release();
    Settle(g);
}
