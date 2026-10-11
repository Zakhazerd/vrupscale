// Functional test for vrupscale's dxgi.dll: acts like a compositor that receives app eye textures
// and samples them in a pixel shader. Runs against a real D3D11 implementation (DXVK under Wine in
// CI). Expects to be started with VRUPSCALE_FORCE=1 and our dxgi.dll next to the exe.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <algorithm>
#include <stdlib.h>
#include "../src/control_shared.h"

struct DebugStats {
    LONG hooksInstalled, shadersReady, liveTexStates, liveViewEntries, upscales, psHookCalls, substitutions,
        rejects, sharedOpened, copiesMarked, deferredPending, liveStreams, temporalRuns, csHookCalls, drawHookCalls,
        copyHookCalls, extraBodies;
};
typedef int (*PFN_GetStats)(void *, int);
typedef void (*PFN_Reload)();

static PFN_GetStats g_getStats;
static PFN_Reload g_reload;
static int g_fail = 0, g_pass = 0;
static std::string g_iniPath;

#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("  ok   "); } else { g_fail++; printf("  FAIL "); } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static DebugStats Stats() { DebugStats s = {}; g_getStats(&s, (int)sizeof(s)); return s; }

template <class T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------------
static const char *kShader = R"(
Texture2DArray<float4> T : register(t0);
SamplerState S : register(s0);
cbuffer C : register(b0) { float2 rtSize; float2 origin; float4 bounds; };   // bounds: uv offset, uv scale
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target {
    return T.SampleLevel(S, float3(bounds.xy + (pos.xy - origin) / rtSize * bounds.zw, 0), 0);
}
// stand-in for the game's GPU work in the performance test (rtSize.x = iterations per pixel)
float4 LOAD(float4 pos : SV_Position) : SV_Target {
    float x = frac(pos.x * 0.0137 + pos.y * 0.0071);
    uint n = (uint)rtSize.x;
    [loop] for (uint i = 0; i < n; ++i) x = frac(x * 1.618 + 0.271);
    return float4(x, x, x, 1);
}
)";

struct Gfx {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11PixelShader *load = nullptr;
    ID3D11SamplerState *point = nullptr;
    ID3D11Buffer *cb = nullptr;
    // The "compositor" records its draws here: the immediate context, or a deferred context whose
    // command lists are executed on the immediate context (VRUPSCALE_TEST_DEFERRED=1, which is how
    // SteamVR's compositor renders).
    ID3D11DeviceContext *rc = nullptr;
    bool deferred = false;
    bool keepState = false;   // FinishCommandList(RestoreDeferredContextState)
};

// Executes what the compositor recorded (deferred mode); a no-op on the immediate context.
static void Submit(Gfx &g)
{
    if (!g.deferred) return;
    ID3D11CommandList *cl = nullptr;
    if (SUCCEEDED(g.rc->FinishCommandList(g.keepState ? TRUE : FALSE, &cl)) && cl) {
        g.ctx->ExecuteCommandList(cl, g.keepState ? TRUE : FALSE);
        cl->Release();
    }
}

static void ClearAll(Gfx &g)
{
    if (g.rc && g.rc != g.ctx) g.rc->ClearState();
    g.ctx->ClearState();
}

static bool Compile(pD3DCompile compile, const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = nullptr;
    HRESULT hr = compile(kShader, strlen(kShader), "test", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (err) { printf("%.*s\n", (int)err->GetBufferSize(), (char *)err->GetBufferPointer()); err->Release(); }
    return SUCCEEDED(hr);
}

static bool InitGfx(Gfx &g)
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr); return false; }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { printf("no d3dcompiler\n"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *ldb = nullptr;
    if (!Compile(compile, "VS", "vs_5_0", &vsb) || !Compile(compile, "PS", "ps_5_0", &psb) ||
        !Compile(compile, "LOAD", "ps_5_0", &ldb))
        return false;
    g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    g.dev->CreatePixelShader(ldb->GetBufferPointer(), ldb->GetBufferSize(), nullptr, &g.load);
    vsb->Release(); psb->Release(); ldb->Release();
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.point);
    D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    {
        IDXGIDevice *xd = nullptr;
        IDXGIAdapter *ad = nullptr;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)))
            ad->GetDesc(&desc);
        char name[128] = "?";
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        printf("GPU: %s, %llu MB VRAM, feature level 0x%x\n", name, (unsigned long long)(desc.DedicatedVideoMemory >> 20), (unsigned)fl);
        if (ad) ad->Release();
        if (xd) xd->Release();
    }
    return g.vs && g.ps && g.point && g.cb;
}

// ---------------------------------------------------------------------------------------------
// Test images

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;   // RGBA, stored values (raw bytes / 255, or linear floats)
    float *at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    const float *at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

static Image MakePattern(int w, int h, int variant, float scale)
{
    Image im;
    im.w = w; im.h = h;
    im.px.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = im.at(x, y);
            float fx = (x + 0.5f) / w, fy = (y + 0.5f) / h;
            float r, g, b;
            if (variant == 0) {
                // diagonal hard edge + concentric rings + small rectangles ("text")
                float edge = (fx * 0.7f + fy * 0.3f) > 0.5f ? 0.85f : 0.15f;
                float d = sqrtf((fx - 0.3f) * (fx - 0.3f) + (fy - 0.6f) * (fy - 0.6f));
                float ring = 0.5f + 0.5f * sinf(d * 120.0f);
                bool box = ((x / 6) % 3 == 0) && ((y / 9) % 2 == 0) && fx > 0.6f && fy < 0.4f;
                r = box ? 0.05f : edge;
                g = box ? 0.05f : 0.5f * edge + 0.5f * ring;
                b = box ? 0.95f : ring;
            } else {
                // vertical bars of increasing width + horizontal gradient
                int bw = 1 + (x * 8) / w;
                bool bar = ((x / bw) % 2) == 0;
                r = bar ? 0.9f : 0.1f;
                g = fy;
                b = 1.0f - fx;
            }
            p[0] = r * scale; p[1] = g * scale; p[2] = b * scale; p[3] = 1.0f;
        }
    return im;
}

static float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float LinearToSrgb(float c) { c = c < 0 ? 0 : (c > 1 ? 1 : c); return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

static Image Quantize8(const Image &a)
{
    Image o = a;
    for (auto &v : o.px) v = roundf((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f) / 255.0f;
    return o;
}

// Bilinear upscale with clamp addressing; mode 0: filter stored values, 1: decode sRGB -> filter -> encode
static Image Bilinear(const Image &src, int ow, int oh, int mode)
{
    Image o;
    o.w = ow; o.h = oh;
    o.px.resize(size_t(ow) * oh * 4);
    auto fetch = [&](int x, int y, int c) {
        x = x < 0 ? 0 : (x >= src.w ? src.w - 1 : x);
        y = y < 0 ? 0 : (y >= src.h ? src.h - 1 : y);
        float v = src.at(x, y)[c];
        return (mode == 1 && c < 3) ? SrgbToLinear(v) : v;
    };
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            float sx = (x + 0.5f) / ow * src.w - 0.5f, sy = (y + 0.5f) / oh * src.h - 0.5f;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - x0, fy = sy - y0;
            for (int c = 0; c < 4; ++c) {
                float v = (fetch(x0, y0, c) * (1 - fx) + fetch(x0 + 1, y0, c) * fx) * (1 - fy) +
                          (fetch(x0, y0 + 1, c) * (1 - fx) + fetch(x0 + 1, y0 + 1, c) * fx) * fy;
                o.at(x, y)[c] = (mode == 1 && c < 3) ? LinearToSrgb(v) : v;
            }
        }
    return o;
}

struct Diff { double maxAbs, meanAbs, psnr, p999; };
// p999: 99.9th percentile of the absolute error (1/4096 resolution). GPUs filter with limited
// subtexel weight precision (NVIDIA: 8 bits), so a handful of pixels on hard edges can be off by a
// few LSB from an exact CPU reference; the percentile ignores those, the PSNR still bounds them.
static Diff Compare(const Image &a, const Image &b, int channels = 3)
{
    Diff d = { 0, 0, 0, 0 };
    double se = 0;
    size_t n = 0;
    std::vector<uint32_t> hist(4096, 0);
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < channels; ++c) {
                double e = fabs(double(a.at(x, y)[c]) - double(b.at(x, y)[c]));
                d.maxAbs = e > d.maxAbs ? e : d.maxAbs;
                d.meanAbs += e;
                se += e * e;
                int bin = (int)(e * 4096.0);
                hist[bin < 0 ? 0 : (bin > 4095 ? 4095 : bin)]++;
                ++n;
            }
    size_t acc = 0, want = (size_t)(n * 0.999);
    for (int i = 0; i < 4096; ++i) {
        acc += hist[i];
        if (acc >= want) { d.p999 = (i + 1) / 4096.0; break; }
    }
    d.meanAbs /= n;
    double mse = se / n;
    d.psnr = mse > 0 ? 10.0 * log10(1.0 / mse) : 99.0;
    return d;
}

// Mean horizontal gradient inside a sub-rectangle given in normalized coordinates.
static double GradientEnergy(const Image &a, float x0 = 0, float y0 = 0, float x1 = 1, float y1 = 1)
{
    int ax = (int)(x0 * a.w) + 1, bx = (int)(x1 * a.w), ay = (int)(y0 * a.h), by = (int)(y1 * a.h);
    double e = 0;
    for (int y = ay; y < by; ++y)
        for (int x = ax; x < bx; ++x)
            for (int c = 0; c < 3; ++c) e += fabs(a.at(x, y)[c] - a.at(x - 1, y)[c]);
    return e / (double(bx - ax) * (by - ay) * 3);
}

static double ChannelMean(const Image &a, int c)
{
    double s = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) s += a.at(x, y)[c];
    return s / (double(a.w) * a.h);
}

// ---------------------------------------------------------------------------------------------
// Texture helpers

static uint16_t FloatToHalf(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, mant = x & 0x7fffff;
    int exp = int((x >> 23) & 0xff) - 127 + 15;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | (exp << 10) | (mant >> 13));
}

static float HalfToFloat(uint16_t h)
{
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
    if (exp == 0) x = sign;
    else if (exp == 31) x = sign | 0x7f800000 | (mant << 13);
    else x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

static ID3D11Texture2D *MakeTexture(Gfx &g, DXGI_FORMAT fmt, const std::vector<Image> &slices, UINT misc, HRESULT *phr = nullptr)
{
    const int w = slices[0].w, h = slices[0].h;
    const bool half = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    const int bpp = half ? 8 : 4;
    std::vector<std::vector<uint8_t>> data(slices.size());
    std::vector<D3D11_SUBRESOURCE_DATA> init(slices.size());
    for (size_t s = 0; s < slices.size(); ++s) {
        data[s].resize(size_t(w) * h * bpp);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float *p = slices[s].at(x, y);
                uint8_t *d = &data[s][(size_t(y) * w + x) * bpp];
                for (int c = 0; c < 4; ++c) {
                    if (half) {
                        uint16_t hv = FloatToHalf(p[c]);
                        memcpy(d + c * 2, &hv, 2);
                    } else {
                        float v = p[c] < 0 ? 0 : (p[c] > 1 ? 1 : p[c]);
                        d[c] = (uint8_t)lroundf(v * 255.0f);
                    }
                }
            }
        init[s] = { data[s].data(), UINT(w * bpp), 0 };
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = (UINT)slices.size();
    td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = misc;
    ID3D11Texture2D *t = nullptr;
    HRESULT hr = g.dev->CreateTexture2D(&td, init.data(), &t);
    if (phr) *phr = hr;
    return SUCCEEDED(hr) ? t : nullptr;
}

static ID3D11ShaderResourceView *MakeView(Gfx &g, ID3D11Texture2D *t, DXGI_FORMAT fmt, int slice)
{
    D3D11_TEXTURE2D_DESC td;
    t->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = fmt;
    if (td.ArraySize == 1 && slice < 0) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
    } else {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.FirstArraySlice = slice < 0 ? 0 : slice;
        sd.Texture2DArray.ArraySize = 1;
    }
    ID3D11ShaderResourceView *v = nullptr;
    HRESULT hr = g.dev->CreateShaderResourceView(t, &sd, &v);
    if (FAILED(hr)) printf("CreateShaderResourceView failed 0x%08lx\n", (unsigned long)hr);
    return v;
}

static void ViewSize(ID3D11ShaderResourceView *v, UINT *w, UINT *h)
{
    ID3D11Resource *r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D *t = nullptr;
    r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    *w = d.Width; *h = d.Height;
    t->Release(); r->Release();
}

// The "compositor" pass: bind the app's view to PS t0, draw a full-screen triangle into an RT of
// the given size with point sampling, read the RT back. Returns what PS t0 held at draw time.
static Image CompositorSample(Gfx &g, ID3D11ShaderResourceView *view, int rw, int rh, DXGI_FORMAT rtFmt,
                              ID3D11ShaderResourceView **boundAtDraw = nullptr, int draws = 1)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = rw; td.Height = rh; td.MipLevels = 1; td.ArraySize = 1; td.Format = rtFmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr;
    g.dev->CreateTexture2D(&td, nullptr, &rt);
    ID3D11RenderTargetView *rtv = nullptr;
    g.dev->CreateRenderTargetView(rt, nullptr, &rtv);
    float cbData[8] = { (float)rw, (float)rh, 0, 0, 0, 0, 1, 1 };
    g.rc->UpdateSubresource(g.cb, 0, nullptr, cbData, 0, 0);

    g.rc->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)rw, (float)rh, 0, 1 };
    g.rc->RSSetViewports(1, &vp);
    g.rc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.rc->IASetInputLayout(nullptr);
    g.rc->VSSetShader(g.vs, nullptr, 0);
    g.rc->PSSetShader(g.ps, nullptr, 0);
    g.rc->PSSetSamplers(0, 1, &g.point);
    g.rc->PSSetConstantBuffers(0, 1, &g.cb);
    g.rc->PSSetShaderResources(0, 1, &view);
    for (int i = 0; i < draws; ++i) g.rc->Draw(3, 0);
    if (boundAtDraw) g.rc->PSGetShaderResources(0, 1, boundAtDraw);
    ID3D11ShaderResourceView *nullView = nullptr;
    g.rc->PSSetShaderResources(0, 1, &nullView);
    ID3D11RenderTargetView *nullRtv = nullptr;
    g.rc->OMSetRenderTargets(1, &nullRtv, nullptr);
    Submit(g);

    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    g.dev->CreateTexture2D(&td, nullptr, &st);
    g.ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    Image im;
    im.w = rw; im.h = rh;
    im.px.resize(size_t(rw) * rh * 4);
    if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const bool half = rtFmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const uint8_t *row = (const uint8_t *)m.pData + size_t(y) * m.RowPitch;
                    if (half) {
                        uint16_t hv;
                        memcpy(&hv, row + (size_t(x) * 4 + c) * 2, 2);
                        im.at(x, y)[c] = HalfToFloat(hv);
                    } else {
                        im.at(x, y)[c] = row[size_t(x) * 4 + c] / 255.0f;
                    }
                }
        g.ctx->Unmap(st, 0);
    }
    st->Release(); rtv->Release(); rt->Release();
    return im;
}

static void WriteIni(const char *body)
{
    FILE *f = fopen(g_iniPath.c_str(), "wb");
    fputs(body, f);
    fclose(f);
    g_reload();
}

static void Pump(Gfx &g)
{
    // Any immediate-context PSSetShaderResources runs the DLL's render-thread housekeeping
    // (config reload, deferred releases).
    ID3D11ShaderResourceView *nullView = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &nullView);
}

static void SavePpm(const Image &im, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float v = im.at(x, y)[c];
                fputc((int)lroundf((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255), f);
            }
    fclose(f);
}

// ---------------------------------------------------------------------------------------------

static const char *kIniBase =
    "[general]\nlogLevel=2\nstatsIntervalSec=1\nhotkeys=0\n"
    "[match]\nminWidth=1000\nminHeight=1000\nminAspect=0.6\nmaxAspect=1.6\nfollowCopies=1\n";

// ---- the vrupscale app's channel (control_shared.h), as the app sees it
static bool ReadStatus(VuShared *sh, VuStatus &st)
{
    for (int tries = 0; tries < 1000; ++tries) {
        int32_t a = sh->statusSeq;
        if (a & 1) { Sleep(0); continue; }
        MemoryBarrier();
        memcpy(&st, (const void *)&sh->status, sizeof(st));
        MemoryBarrier();
        if (sh->statusSeq == a) return true;
    }
    return false;
}

static int32_t WriteSettings(VuShared *sh, const VuSettings &s)
{
    InterlockedIncrement((volatile LONG *)&sh->settingsSeq);
    MemoryBarrier();
    memcpy((void *)&sh->settings, &s, sizeof(s));
    MemoryBarrier();
    return (int32_t)InterlockedIncrement((volatile LONG *)&sh->settingsSeq);
}

static bool WaitStatus(Gfx &g, VuShared *sh, VuStatus &st, bool (*ok)(const VuStatus &, const void *), const void *arg)
{
    for (int i = 0; i < 80; ++i) {
        Pump(g);
        if (ReadStatus(sh, st) && ok(st, arg)) return true;
        Sleep(50);
    }
    return false;
}

static const VuTexInfo *FindTex(const VuStatus &st, UINT w, UINT h)
{
    for (uint32_t i = 0; i < st.texCount && i < kVuMaxTex; ++i)
        if (st.tex[i].w == w && st.tex[i].h == h) return &st.tex[i];
    return nullptr;
}

// The channel as the app would open it (this test process's own name).
static VuShared *OpenTestChannel(HANDLE *map)
{
    wchar_t name[128];
    DWORD en = GetEnvironmentVariableW(L"VRUPSCALE_CONTROL", name, 128);
    if (!(en > 0 && en < 128)) swprintf(name, 128, L"%ls.test.%lu", VU_CONTROL_NAME, GetCurrentProcessId());
    *map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    return *map ? (VuShared *)MapViewOfFile(*map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VuShared)) : nullptr;
}

#include "temporal_tests.inc"

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    g_iniPath = std::string(dir) + "\\vrupscale.ini";
    const char *outDir = argc > 1 ? argv[1] : dir;

    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    g_getStats = dxgi ? (PFN_GetStats)GetProcAddress(dxgi, "VrUpscaleGetStats") : nullptr;
    g_reload = dxgi ? (PFN_Reload)GetProcAddress(dxgi, "VrUpscaleRequestReload") : nullptr;
    printf("[setup]\n");
    CHECK(g_getStats && g_reload, "our dxgi.dll is the one loaded in the process (proxy chain works)");
    if (!g_getStats) return 1;

    Gfx g;
    CHECK(InitGfx(g), "D3D11 device + test shaders");
    if (!g.dev) return 1;

    for (int i = 0; i < 400 && !(Stats().hooksInstalled && Stats().shadersReady); ++i) Sleep(50);
    DebugStats s0 = Stats();
    CHECK(s0.hooksInstalled, "hooks installed by the DLL's worker thread");
    CHECK(s0.shadersReady, "upscale shaders compiled by the DLL");
    if (!s0.hooksInstalled || !s0.shadersReady) return 1;

    g.rc = g.ctx;
    if (EnvStr("VRUPSCALE_TEST_DEFERRED")[0] == '1') {
        HRESULT dhr = g.dev->CreateDeferredContext(0, &g.rc);
        g.deferred = SUCCEEDED(dhr) && g.rc;
        if (!g.deferred) g.rc = g.ctx;
        CHECK(g.deferred, "compositor draws recorded into a deferred context (command lists), like SteamVR's");
    }

    // ---- a context that runs through other method bodies than the first device's immediate
    //      context (here: a deferred context) is found and hooked, and each call is seen once
    printf("[hook variants]\n");
    {
        ID3D11DeviceContext *dc = nullptr;
        HRESULT dhr = g.dev->CreateDeferredContext(0, &dc);
        if (SUCCEEDED(dhr) && dc) {
            // found either when it was created or by the DLL's own probe, whichever ran first
            DebugStats a = Stats();
            CHECK(a.extraBodies > 0, "a deferred context's method bodies were found and hooked (%ld beyond the first device's)",
                  (long)a.extraBodies);
            ID3D11ShaderResourceView *nv = nullptr;
            dc->PSSetShaderResources(0, 1, &nv);
            dc->Draw(3, 0);
            DebugStats b = Stats();
            CHECK(b.psHookCalls - a.psHookCalls == 1 && b.drawHookCalls - a.drawHookCalls == 1,
                  "calls on it reach the hooks exactly once (PS %ld, draws %ld)", (long)(b.psHookCalls - a.psHookCalls),
                  (long)(b.drawHookCalls - a.drawHookCalls));
            Pump(g);
            DebugStats c = Stats();
            CHECK(c.psHookCalls - b.psHookCalls == 1, "immediate-context calls still reach the hooks exactly once (%ld)",
                  (long)(c.psHookCalls - b.psHookCalls));
            dc->Release();
        } else {
            printf("  note: no deferred contexts here (0x%08lx); variant check skipped\n", (unsigned long)dhr);
        }
    }

    const int W = 1200, H = 1300;
    const float F = 1.3f;
    const int OW = (int)lroundf(W * F), OH = (int)lroundf(H * F);
    char ini[1024];

    // ---- shared texture support in this D3D11 implementation?
    std::vector<Image> eyes = { Quantize8(MakePattern(W, H, 0, 1.0f)), Quantize8(MakePattern(W, H, 1, 1.0f)) };
    HRESULT hr;
    ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, eyes, D3D11_RESOURCE_MISC_SHARED, &hr);
    bool sharedOk = app != nullptr;
    if (!app) {
        printf("  note: MISC_SHARED textures unsupported here (0x%08lx); using requireShared=0\n", (unsigned long)hr);
        app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, eyes, 0);
    }
    const char *reqShared = sharedOk ? "1" : "0";

    if (EnvStr("VRUPSCALE_TEST_PERF_ONLY")[0] == '1') {
        LoadDebugExports();
        char buf[16];
        DWORD n = GetEnvironmentVariableA("VRUPSCALE_PERF_FRAMES", buf, sizeof(buf));
        RunPerfTest(g, reqShared, sharedOk, (n > 0 && n < sizeof(buf)) ? atoi(buf) : 60);
        printf("RESULT: %d passed, %d failed (performance only)\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (EnvStr("VRUPSCALE_TEST_SERVE")[0]) {
        // Behave like a running compositor for a while (for trying the vrupscale app against it):
        // two eyes of new frames in temporal mode, plus a wide overlay that is left alone.
        const int seconds = atoi(EnvStr("VRUPSCALE_TEST_SERVE"));
        snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=temporal\nupscaleFactor=%.2f\nsharpness=0.5\n", kIniBase,
                 reqShared, F);
        WriteIni(ini);
        ID3D11ShaderResourceView *ev[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                            MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
        std::vector<Image> wide = { Quantize8(MakePattern(1600, 400, 0, 1.0f)) };
        ID3D11Texture2D *ov = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM, wide, sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0);
        ID3D11ShaderResourceView *ovv = MakeView(g, ov, DXGI_FORMAT_R8G8B8A8_UNORM, -1);
        EyeTarget tgt;
        EnsureEyeTarget(g, tgt, OW, OH);
        printf("serving for %d s\n", seconds);
        fflush(stdout);
        const DWORD end = GetTickCount() + seconds * 1000u;
        uint32_t serial = 0;
        if (EnvStr("VRUPSCALE_TEST_SERVE_POOL")[0] == '1') {
            // like VRChat after SteamVR's resolution was lowered during a slider drag: per eye three
            // textures of different sizes, the game rendering W x H into their top-left corner
            const int sz[3][2] = { { W, H }, { W + 96, H + 104 }, { W + 192, H + 208 } };
            ID3D11Texture2D *pt[2][3] = {};
            ID3D11ShaderResourceView *pv[2][3] = {};
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 3; ++k) {
                    pt[e][k] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, { Quantize8(MakePattern(sz[k][0], sz[k][1], e, 1.0f)) },
                                           sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0);
                    pv[e][k] = MakeView(g, pt[e][k], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
                }
            std::vector<uint32_t> strip((size_t)std::max(W, H) * 16);
            for (int f = 0; (int32_t)(GetTickCount() - end) < 0; ++f) {
                const int k = f % 3;
                for (int e = 0; e < 2; ++e) {   // new content along the picture's edges (the rest stays)
                    for (auto &p : strip) p = 0xff000000u | (((uint32_t)f * 2654435761u + (uint32_t)(&p - strip.data()) * 40503u) >> 8);
                    const D3D11_BOX boxes[4] = { { 0, 0, 0, (UINT)W, 16, 1 }, { 0, (UINT)H - 16, 0, (UINT)W, (UINT)H, 1 },
                                                 { 0, 0, 0, 16, (UINT)H, 1 }, { (UINT)W - 16, 0, 0, (UINT)W, (UINT)H, 1 } };
                    for (int b = 0; b < 4; ++b)
                        g.ctx->UpdateSubresource(pt[e][k], 0, &boxes[b], strip.data(), (boxes[b].right - boxes[b].left) * 4, 0);
                }
                for (int e = 0; e < 2; ++e)
                    for (int j = 1; j <= 3; ++j) {
                        const int kk = (k + j) % 3;
                        DrawEye(g, tgt, pv[e][kk], e, nullptr, false, (float)W / sz[kk][0], (float)H / sz[kk][1]);
                    }
                CompositorSample(g, ovv, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
                g.ctx->Flush();
                Sleep(20);
            }
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 3; ++k) { SafeRelease(pv[e][k]); SafeRelease(pt[e][k]); }
        }
        while ((int32_t)(GetTickCount() - end) < 0) {
            for (UINT e = 0; e < 2; ++e) {   // a 16x16 block: the frame fingerprint samples one pixel per 8x8
                uint32_t px[256];
                for (auto &p : px) p = 0xff000000u | (200u << 16) | ((128u + ((serial >> 7) & 127u)) << 8) | (128u + (serial & 127u));
                ++serial;
                D3D11_BOX box = { 0, 0, 0, 16, 16, 1 };
                g.ctx->UpdateSubresource(app, D3D11CalcSubresource(0, e, 1), &box, px, 64, 0);
            }
            DrawEye(g, tgt, ev[0], 0, nullptr, false);
            DrawEye(g, tgt, ev[1], 1, nullptr, false);
            CompositorSample(g, ovv, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
            g.ctx->Flush();
            Sleep(20);
        }
        ClearAll(g);
        SafeRelease(ev[0]); SafeRelease(ev[1]); SafeRelease(ovv); SafeRelease(ov);
        tgt.Release();
        printf("served\n");
        return 0;
    }
    if (EnvStr("VRUPSCALE_TEST_QUALITY_ONLY")[0] == '1' || EnvStr("VRUPSCALE_TEST_ONLY")[0]) {
        RunTemporalTests(g, outDir, reqShared, sharedOk, true);
        printf("RESULT: %d passed, %d failed (quality only)\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    // ================= 1. bilinear mode: exact geometry / slice / format check
    printf("[bilinear mode, R8G8B8A8_TYPELESS array x2, sRGB views]\n");
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=bilinear\nupscaleFactor=%.2f\nsharpness=0\ndedupeMs=3\n", kIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    ID3D11ShaderResourceView *eyeView[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0),
                                             MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
    for (int e = 0; e < 2; ++e) {
        ID3D11ShaderResourceView *bound = nullptr;
        Image out = CompositorSample(g, eyeView[e], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &bound);
        UINT bw = 0, bh = 0;
        if (bound) ViewSize(bound, &bw, &bh);
        CHECK(bound && bound != eyeView[e] && (int)bw == OW && (int)bh == OH,
              "eye %d: PS t0 holds a %ux%u proxy at draw time (expected %dx%d)", e, bw, bh, OW, OH);
        SafeRelease(bound);
        Image ref = Bilinear(eyes[e], OW, OH, 0);
        Diff d = Compare(out, ref);
        CHECK(d.maxAbs <= 3.0 / 255 && d.psnr > 45, "eye %d: matches CPU bilinear of the right slice (max %.1f LSB, %.1f dB)", e, d.maxAbs * 255, d.psnr);
        Diff wrong = Compare(out, Bilinear(eyes[1 - e], OW, OH, 0));
        CHECK(wrong.psnr < d.psnr - 10, "eye %d: clearly differs from the other slice (%.1f dB)", e, wrong.psnr);
        double alpha = ChannelMean(out, 3);
        CHECK(fabs(alpha - 1.0) < 0.01, "eye %d: alpha preserved (%.3f)", e, alpha);
    }

    // ================= 2. FSR mode
    printf("[fsr mode]\n");
    // (FSR 1 over the whole image here: the area has its own test)
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=%.2f\nsharpness=0.5\ndedupeMs=3\n[temporal]\ntemporalArea=1\n",
             kIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    LONG upBefore = Stats().upscales;
    Image fsr[2];
    for (int e = 0; e < 2; ++e) {
        fsr[e] = CompositorSample(g, eyeView[e], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        Image ref = Bilinear(eyes[e], OW, OH, 0);
        Diff d = Compare(fsr[e], ref);
        // high-contrast region: eye 0 = the small dark boxes, eye 1 = the bars
        float rx0 = e == 0 ? 0.62f : 0.0f, ry1 = e == 0 ? 0.38f : 1.0f;
        double ge = GradientEnergy(fsr[e], rx0, 0.02f, 0.98f, ry1), gb = GradientEnergy(ref, rx0, 0.02f, 0.98f, ry1);
        CHECK(d.psnr > 22 && d.psnr < 60, "eye %d: FSR output tracks the bilinear reference (%.1f dB, not identical)", e, d.psnr);
        CHECK(ge > gb * 1.05, "eye %d: FSR is sharper than bilinear on edges (gradient energy %.4f vs %.4f)", e, ge, gb);
        Diff wrong = Compare(fsr[e], Bilinear(eyes[1 - e], OW, OH, 0));
        CHECK(wrong.psnr < d.psnr - 8, "eye %d: FSR uses the right slice (%.1f dB vs other slice %.1f dB)", e, d.psnr, wrong.psnr);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\fsr_eye0.ppm", outDir); SavePpm(fsr[0], path);
    snprintf(path, sizeof(path), "%s\\bilinear_eye0.ppm", outDir); SavePpm(Bilinear(eyes[0], OW, OH, 0), path);
    snprintf(path, sizeof(path), "%s\\source_eye0.ppm", outDir); SavePpm(eyes[0], path);
    LONG upAfter = Stats().upscales;
    CHECK(upAfter - upBefore >= 1, "upscale dispatches happened (%ld)", (long)(upAfter - upBefore));

    // ================= 3. dedupe window
    printf("[dedupe]\n");
    Sleep(20);
    LONG u0 = Stats().upscales;
    CompositorSample(g, eyeView[0], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, nullptr, 3);
    LONG u1 = Stats().upscales;
    CHECK(u1 - u0 == 1, "3 back-to-back draws -> 1 upscale (%ld)", (long)(u1 - u0));
    Sleep(20);
    CompositorSample(g, eyeView[1], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(Stats().upscales - u1 == 1, "after the window expires the texture is refreshed");

    // ================= 3b. the vrupscale app's live channel
    printf("[app channel]\n");
    {
        wchar_t name[128];
        DWORD en = GetEnvironmentVariableW(L"VRUPSCALE_CONTROL", name, 128);
        if (!(en > 0 && en < 128)) swprintf(name, 128, L"%ls.test.%lu", VU_CONTROL_NAME, GetCurrentProcessId());
        HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        VuShared *sh = map ? (VuShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VuShared)) : nullptr;
        CHECK(sh && sh->magic == kVuMagic && sh->layout == kVuLayout, "the DLL opened the app channel");
        if (sh) {
            VuStatus st;
            struct Arg { UINT w, h; DWORD pid; };
            Arg a0 = { (UINT)W, (UINT)H, GetCurrentProcessId() };
            bool ok = WaitStatus(g, sh, st, [](const VuStatus &s, const void *p) {
                const Arg *a = (const Arg *)p;
                const VuTexInfo *t = FindTex(s, a->w, a->h);
                return s.pid == a->pid && t && t->upscaled;
            }, &a0);
            const VuTexInfo *t = FindTex(st, W, H);
            CHECK(ok && st.hooksInstalled && t && (int)t->outW == OW && (int)t->outH == OH && strcmp(st.version, "") != 0,
                  "status: this process, version %s, eye texture %dx%d listed as upscaled to %ux%u", st.version, W, H,
                  t ? t->outW : 0, t ? t->outH : 0);
            CHECK(st.iniPath[0] != 0 && st.logPath[0] != 0, "status names the settings file and the log");

            // a live change (sharpness, tint) keeps the proxies: no re-plan
            ID3D11ShaderResourceView *b0 = nullptr, *b1 = nullptr;
            CompositorSample(g, eyeView[0], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b0);
            VuSettings s = st.current;
            s.sharpness = 0.8f;
            s.tint = 1;
            int32_t seq = WriteSettings(sh, s);
            ok = WaitStatus(g, sh, st, [](const VuStatus &x, const void *p) {
                return (int32_t)x.appliedSeq == *(const int32_t *)p && x.current.sharpness > 0.79f && x.current.tint;
            }, &seq);
            Sleep(20);
            CompositorSample(g, eyeView[0], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b1);
            CHECK(ok && b0 && b0 == b1, "sharpness/tint from the app apply live, same proxy (applied seq %u)", st.appliedSeq);
            SafeRelease(b0); SafeRelease(b1);

            // a new factor re-plans: the bound proxy changes size
            s = st.current;
            s.tint = 0;
            s.upscaleFactor = 1.5f;
            s.temporalArea = 0.8f;
            seq = WriteSettings(sh, s);
            ok = WaitStatus(g, sh, st, [](const VuStatus &x, const void *p) { return (int32_t)x.appliedSeq == *(const int32_t *)p; }, &seq);
            Sleep(20);
            CompositorSample(g, eyeView[0], W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b1);
            UINT bw = 0, bh = 0;
            if (b1) ViewSize(b1, &bw, &bh);
            CHECK(ok && (int)bw == (int)lroundf(W * 1.5f) && (int)bh == (int)lroundf(H * 1.5f),
                  "a new upscale factor from the app re-plans the proxy (%ux%u)", bw, bh);
            CHECK(fabsf(st.current.temporalArea - 0.8f) < 0.001f, "temporal area from the app is in effect (%.2f)", st.current.temporalArea);
            SafeRelease(b1);

            // the shortcuts (as if pressed: VrUpscaleDebugHotkey). Ctrl+F5: the next scaling preset,
            // re-planned like a change from the app, and published so the app follows along
            typedef void (*PFN_Hotkey)(int);
            PFN_Hotkey hotkey = dxgi ? (PFN_Hotkey)GetProcAddress(dxgi, "VrUpscaleDebugHotkey") : nullptr;
            CHECK(hotkey != nullptr, "shortcut test export present");
            if (hotkey) {
                struct Before { float f; int32_t fast, bil; };
                auto press = [&](int idx) {   // until the status shows the change
                    const Before before = { st.current.upscaleFactor, st.current.fastHistory, st.current.bilinearEdges };
                    hotkey(idx);
                    WaitStatus(g, sh, st, [](const VuStatus &x, const void *p) {
                        const Before *b = (const Before *)p;
                        return fabsf(x.current.upscaleFactor - b->f) > 0.001f || x.current.fastHistory != b->fast ||
                               x.current.bilinearEdges != b->bil;
                    }, &before);
                };
                auto factorNow = [&]() { return st.current.upscaleFactor; };
                press(4);   // from Quality (1.5, set above)
                Sleep(20);
                CompositorSample(g, eyeView[0], W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b1);
                bw = bh = 0;
                if (b1) ViewSize(b1, &bw, &bh);
                CHECK(fabsf(factorNow() - 1.72f) < 0.001f && (int)bw == (int)lroundf(W * 1.72f) && (int)bh == (int)lroundf(H * 1.72f),
                      "Ctrl+F5: Quality -> Balanced (factor %.2f), the proxy re-planned to %ux%u", factorNow(), bw, bh);
                SafeRelease(b1);
                float seen[5];
                UINT nw = 0, nh = 0;
                for (int k = 0; k < 5; ++k) {   // Performance, then around to Native, Ultra Quality ...
                    press(4);
                    seen[k] = factorNow();
                    if (k == 1) {   // Native: processed at its own size
                        Sleep(20);
                        CompositorSample(g, eyeView[0], W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b1);
                        if (b1 && b1 != eyeView[0]) ViewSize(b1, &nw, &nh);
                        SafeRelease(b1);
                    }
                }
                CHECK(fabsf(seen[0] - 2.0f) < 0.001f && fabsf(seen[1] - 1.0f) < 0.001f && fabsf(seen[2] - 1.3f) < 0.001f &&
                          fabsf(seen[3] - 1.5f) < 0.001f && fabsf(seen[4] - 1.72f) < 0.001f,
                      "Ctrl+F5 cycles the presets and wraps around to Native (%.2f %.2f %.2f %.2f %.2f)", seen[0], seen[1], seen[2], seen[3], seen[4]);
                CHECK((int)nw == W && (int)nh == H, "at Native (1x) the eye texture gets a same-size proxy (%ux%u)", nw, nh);
                s = st.current;
                s.upscaleFactor = 1.41f;   // between presets (a hand edit)
                seq = WriteSettings(sh, s);
                WaitStatus(g, sh, st, [](const VuStatus &x, const void *p) { return (int32_t)x.appliedSeq == *(const int32_t *)p; }, &seq);
                press(4);
                CHECK(fabsf(factorNow() - 1.5f) < 0.001f, "Ctrl+F5 from a factor between presets (1.41) goes to the next one up (%.2f)",
                      factorNow());
                const int32_t fast0 = st.current.fastHistory;
                press(5);
                const int32_t fast1 = st.current.fastHistory;
                press(5);
                CHECK(fast0 == 1 && fast1 == 0 && st.current.fastHistory == 1,
                      "Ctrl+F6 switches Sharp history on and off again (off by default: %d %d %d)", fast0, fast1, st.current.fastHistory);
                const int32_t bil0 = st.current.bilinearEdges;
                press(6);
                const int32_t bil1 = st.current.bilinearEdges;
                press(6);
                CHECK(bil0 == 0 && bil1 == 1 && st.current.bilinearEdges == 0,
                      "Ctrl+F7 switches Bilinear edges on and off again (off by default: %d %d %d)", bil0, bil1, st.current.bilinearEdges);
            }

            // excluding the eye texture's size leaves it alone, and the list says why
            s = st.current;
            s.excludeCount = 1;
            s.exclude[0] = ((uint32_t)W << 16) | (uint32_t)H;
            seq = WriteSettings(sh, s);
            ok = WaitStatus(g, sh, st, [](const VuStatus &x, const void *p) { return (int32_t)x.appliedSeq == *(const int32_t *)p; }, &seq);
            Sleep(20);
            CompositorSample(g, eyeView[0], W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b1);
            Arg a1 = { (UINT)W, (UINT)H, 0 };
            WaitStatus(g, sh, st, [](const VuStatus &x, const void *p) {
                const Arg *a = (const Arg *)p;
                const VuTexInfo *t = FindTex(x, a->w, a->h);
                return t && !t->upscaled;
            }, &a1);
            t = FindTex(st, W, H);
            CHECK(ok && b1 == eyeView[0] && t && !t->upscaled && t->reason[0], "an excluded size is left alone (\"%s\")",
                  t ? t->reason : "?");
            SafeRelease(b1);

            // reset command, then hand the settings back to the test's ini
            sh->command = kVuCmdResetHistory;
            MemoryBarrier();
            InterlockedIncrement((volatile LONG *)&sh->commandSeq);
            Pump(g);
            CHECK(true, "history reset command accepted");
            UnmapViewOfFile(sh);
        }
        if (map) CloseHandle(map);
        WriteIni(ini);   // back to this section's settings (bilinear/fsr tests wrote it)
        Pump(g);
    }

    // ================= 4. state preservation around the injected dispatch
    printf("[state preservation]\n");
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D *t1 = nullptr, *t2 = nullptr, *t3 = nullptr;
        g.dev->CreateTexture2D(&td, nullptr, &t1);
        g.dev->CreateTexture2D(&td, nullptr, &t2);
        g.dev->CreateTexture2D(&td, nullptr, &t3);
        ID3D11ShaderResourceView *csSrv = nullptr, *psSrv1 = nullptr;
        ID3D11UnorderedAccessView *csUav = nullptr;
        g.dev->CreateShaderResourceView(t1, nullptr, &csSrv);
        g.dev->CreateUnorderedAccessView(t2, nullptr, &csUav);
        g.dev->CreateShaderResourceView(t3, nullptr, &psSrv1);
        D3D11_BUFFER_DESC bd = { 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        ID3D11Buffer *csCb = nullptr;
        g.dev->CreateBuffer(&bd, nullptr, &csCb);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 4;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = 3;
        ID3D11SamplerState *csSamp = nullptr;
        g.dev->CreateSamplerState(&sd, &csSamp);

        g.keepState = true;   // the bindings below must survive the command list being finished
        g.rc->CSSetShaderResources(0, 1, &csSrv);
        UINT keep = (UINT)-1;
        g.rc->CSSetUnorderedAccessViews(0, 1, &csUav, &keep);
        g.rc->CSSetConstantBuffers(0, 1, &csCb);
        g.rc->CSSetSamplers(0, 1, &csSamp);
        g.rc->PSSetShaderResources(1, 1, &psSrv1);

        Sleep(20);
        LONG ub = Stats().upscales;
        ID3D11ShaderResourceView *bound = nullptr;
        CompositorSample(g, eyeView[0], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &bound);
        CHECK(Stats().upscales - ub == 1, "an upscale ran in the middle of this state");
        UINT bw = 0, bh = 0;
        if (bound) ViewSize(bound, &bw, &bh);
        CHECK(bound && (int)bw == OW, "PS t0 still holds the proxy after the injected UAV pass");
        SafeRelease(bound);

        ID3D11ShaderResourceView *gSrv = nullptr, *gPs1 = nullptr;
        ID3D11UnorderedAccessView *gUav = nullptr;
        ID3D11Buffer *gCb = nullptr;
        ID3D11SamplerState *gSamp = nullptr;
        g.rc->CSGetShaderResources(0, 1, &gSrv);
        g.rc->CSGetUnorderedAccessViews(0, 1, &gUav);
        g.rc->CSGetConstantBuffers(0, 1, &gCb);
        g.rc->CSGetSamplers(0, 1, &gSamp);
        g.rc->PSGetShaderResources(1, 1, &gPs1);
        CHECK(gSrv == csSrv, "CS t0 restored");
        CHECK(gUav == csUav, "CS u0 restored");
        CHECK(gCb == csCb, "CS b0 restored");
        CHECK(gSamp == csSamp, "CS s0 restored");
        CHECK(gPs1 == psSrv1, "unrelated PS t1 binding untouched");
        SafeRelease(gSrv); SafeRelease(gUav); SafeRelease(gCb); SafeRelease(gSamp); SafeRelease(gPs1);
        ClearAll(g);
        g.keepState = false;
        SafeRelease(csSrv); SafeRelease(csUav); SafeRelease(psSrv1); SafeRelease(csCb); SafeRelease(csSamp);
        SafeRelease(t1); SafeRelease(t2); SafeRelease(t3);
    }

    // ================= 5. things that must NOT be substituted
    printf("[rejections]\n");
    {
        std::vector<Image> small = { Quantize8(MakePattern(512, 512, 0, 1.0f)) };
        ID3D11Texture2D *ts = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM, small, sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0);
        ID3D11ShaderResourceView *vs = MakeView(g, ts, DXGI_FORMAT_R8G8B8A8_UNORM, -1);
        ID3D11ShaderResourceView *b = nullptr;
        CompositorSample(g, vs, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, &b);
        CHECK(b == vs, "small (overlay-sized) texture is left alone");
        SafeRelease(b); SafeRelease(vs); SafeRelease(ts);

        std::vector<Image> wide = { Quantize8(MakePattern(2400, 1100, 0, 1.0f)) };
        ID3D11Texture2D *tw = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM, wide, sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0);
        ID3D11ShaderResourceView *vw = MakeView(g, tw, DXGI_FORMAT_R8G8B8A8_UNORM, -1);
        CompositorSample(g, vw, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, &b);
        CHECK(b == vw, "wide (desktop-overlay aspect) texture is left alone");
        SafeRelease(b); SafeRelease(vw); SafeRelease(tw);

        if (sharedOk) {
            ID3D11Texture2D *tn = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM, { eyes[0] }, 0);
            ID3D11ShaderResourceView *vn = MakeView(g, tn, DXGI_FORMAT_R8G8B8A8_UNORM, -1);
            CompositorSample(g, vn, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, &b);
            CHECK(b == vn, "compositor-internal (non-shared) texture is left alone");
            SafeRelease(b);

            // ...unless it is a copy of a shared eye texture
            ID3D11Texture2D *tsrc = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM, { eyes[1] }, D3D11_RESOURCE_MISC_SHARED);
            g.rc->CopyResource(tn, tsrc);
            Image out = CompositorSample(g, vn, OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, &b);
            UINT bw = 0, bh = 0;
            if (b) ViewSize(b, &bw, &bh);
            CHECK(b && b != vn && (int)bw == OW, "internal copy of a shared texture is followed and upscaled");
            Diff d = Compare(out, Bilinear(eyes[1], OW, OH, 0));
            CHECK(d.psnr > 22, "followed copy shows the copied content (%.1f dB)", d.psnr);
            SafeRelease(b); SafeRelease(vn); SafeRelease(tn); SafeRelease(tsrc);
        }
        DebugStats st = Stats();
        CHECK(st.rejects > 0, "rejections were recorded (%ld)", (long)st.rejects);
    }

    // ================= 6. typed sRGB source (decode -> FSR in perceptual space -> encode)
    printf("[typed sRGB source, bilinear mode]\n");
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=bilinear\nupscaleFactor=%.2f\nsharpness=0\n", kIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    {
        ID3D11Texture2D *t = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, { eyes[0] }, sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0);
        ID3D11ShaderResourceView *v = MakeView(g, t, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
        ID3D11ShaderResourceView *b = nullptr;
        Image out = CompositorSample(g, v, OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b);
        CHECK(b && b != v, "typed sRGB texture substituted");
        // Hardware decodes before filtering (bilinear pass), then the shader re-encodes.
        // Judge the error in linear light, where the filtering happens: a GPU's limited subtexel
        // weight precision (NVIDIA: 8 bits) costs up to ~0.002 there, which the sRGB curve blows up
        // to several LSB next to black. A wrong or doubled encode is off by 0.05+ almost everywhere.
        Image ref = Bilinear(eyes[0], OW, OH, 1);
        Diff d = Compare(out, ref);
        double linMax = 0;
        for (size_t i = 0; i < out.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) {
                double e = fabs((double)SrgbToLinear(out.px[i + c]) - (double)SrgbToLinear(ref.px[i + c]));
                if (!(e == e)) e = 1e9;
                linMax = e > linMax ? e : linMax;
            }
        CHECK(linMax < 0.015 && d.psnr > 45, "sRGB re-encode round trip correct (%.1f dB, max %.4f linear, 99.9%% within %.1f LSB)",
              d.psnr, linMax, d.p999 * 255);
        SafeRelease(b); SafeRelease(v); SafeRelease(t);
    }

    // ================= 7. HDR float source
    printf("[R16G16B16A16_FLOAT source, bilinear mode]\n");
    {
        Image hdr = MakePattern(W, H, 0, 3.0f);   // values up to ~2.9
        for (auto &p : hdr.px) p = HalfToFloat(FloatToHalf(p));
        ID3D11Texture2D *t = MakeTexture(g, DXGI_FORMAT_R16G16B16A16_FLOAT, { hdr }, sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0);
        ID3D11ShaderResourceView *v = MakeView(g, t, DXGI_FORMAT_R16G16B16A16_FLOAT, -1);
        ID3D11ShaderResourceView *b = nullptr;
        Image out = CompositorSample(g, v, OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, &b);
        CHECK(b && b != v, "float texture substituted");
        Image ref = Bilinear(hdr, OW, OH, 0);
        // Bulk: relative error (the transform must be invertible across the whole range). Outliers:
        // absolute error bounded by the GPU's subtexel weight precision on a 0..3 edge (3/512) plus
        // fp16 rounding at both ends; a broken transform misses this by far on the bright parts.
        double maxAbs = 0;
        std::vector<float> rels;
        rels.reserve(out.px.size() / 4 * 3);
        for (size_t i = 0; i < out.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) {
                double rv = ref.px[i + c], ov = out.px[i + c];
                double e = fabs(ov - rv);
                if (!(e == e)) e = 1e9;   // NaN counts as a failure
                maxAbs = e > maxAbs ? e : maxAbs;
                rels.push_back((float)(e / (fabs(rv) + 0.05)));
            }
        size_t k = (size_t)(rels.size() * 0.999);
        if (k >= rels.size()) k = rels.size() ? rels.size() - 1 : 0;
        double p999 = 1e9;
        if (!rels.empty()) { std::nth_element(rels.begin(), rels.begin() + k, rels.end()); p999 = rels[k]; }
        CHECK(p999 < 0.02 && maxAbs < 0.02, "HDR values survive the forward/inverse transform (99.9%% rel err < %.4f, max abs %.4f)",
              p999, maxAbs);
        SafeRelease(b); SafeRelease(v); SafeRelease(t);
    }

    // ================= 8. tint + disable
    printf("[debug tint, enabled=0]\n");
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nmode=fsr\nupscaleFactor=%.2f\nsharpness=0.5\ndebugTint=1\n[temporal]\ntemporalArea=1\n",
             kIniBase, reqShared, F);
    WriteIni(ini);
    Pump(g);
    Sleep(10);
    {
        Image t = CompositorSample(g, eyeView[0], OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        double rg0 = ChannelMean(fsr[0], 1) / ChannelMean(fsr[0], 0);
        double rg1 = ChannelMean(t, 1) / ChannelMean(t, 0);
        CHECK(rg1 < rg0 * 0.9, "tint visibly lowers green (G/R %.3f -> %.3f)", rg0, rg1);
    }
    snprintf(ini, sizeof(ini), "%srequireShared=%s\n[upscale]\nenabled=0\n", kIniBase, reqShared);
    WriteIni(ini);
    Pump(g);
    {
        ID3D11ShaderResourceView *b = nullptr;
        CompositorSample(g, eyeView[0], W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &b);
        CHECK(b == eyeView[0], "enabled=0 leaves the original view bound");
        SafeRelease(b);
    }

    // ================= temporal path
    RunTemporalTests(g, outDir, reqShared, sharedOk, false);
    {
        char buf[16];
        DWORD n = GetEnvironmentVariableA("VRUPSCALE_PERF_FRAMES", buf, sizeof(buf));
        int perfFrames = (n > 0 && n < sizeof(buf)) ? atoi(buf) : 60;
        RunPerfTest(g, reqShared, sharedOk, perfFrames);
    }

    // ================= 9. lifetime: everything we allocated goes away with the app's objects
    printf("[lifetime]\n");
    ClearAll(g);
    SafeRelease(eyeView[0]);
    SafeRelease(eyeView[1]);
    SafeRelease(app);
    Settle(g);
    DebugStats st = Stats();
    CHECK(st.liveViewEntries == 0, "no live view entries after the app's views are released (%ld)", (long)st.liveViewEntries);
    CHECK(st.liveTexStates == 0, "no live proxy textures after the app's textures are released (%ld)", (long)st.liveTexStates);
    CHECK(st.deferredPending == 0, "deferred release queue drained (%ld)", (long)st.deferredPending);
    CHECK(st.liveStreams == 0, "no live temporal histories (%ld)", (long)st.liveStreams);

    printf("\nstats: ps=%ld subs=%ld upscales=%ld rejects=%ld copiesMarked=%ld\n", (long)st.psHookCalls,
           (long)st.substitutions, (long)st.upscales, (long)st.rejects, (long)st.copiesMarked);
    printf("RESULT: %d passed, %d failed%s\n", g_pass, g_fail, sharedOk ? "" : " (shared-texture checks skipped)");
    return g_fail ? 1 : 0;
}
