// Signs with text, for the end-to-end test of the app's translation (tools/ab/translate.sh):
// VRUPSCALE_TEST_SERVE=<seconds> with VRUPSCALE_TEST_SERVE_TEXT=1 shows them like a running
// compositor, each eye in a texture of its own, placed in the two eyes as signs at their distances
// would be for the eyes test/stub/openvr_stub.c reports (so the app can tell the distances).
// Included by fake_compositor.cpp. Windows only: the text is drawn with GDI.
//
// Five phases, each starting at a time in seconds (VRUPSCALE_TEST_TEXT_TIMES="t1 t2 t3 t4", default
// "12 18 23 29"): two signs; the second sign reads something else; no signs; the signs of the second
// phase again; and from t4, with those signs still up, Ctrl+F11 has been "pressed" (through the DLL's
// test export), which switches the translation off.

struct TextSign {
    const wchar_t *text;
    int cx, cy;          // its centre in the left eye's picture (of kTextW x kTextH)
    float depth;         // metres
    int fontPx;
    COLORREF bg, ink;
};
static const int kTextW = 1400, kTextH = 1400;
// the stub's eyes (ProjectionRaw, EyeToHead)
static const float kTextL[2] = { -1.3f, -1.1f }, kTextR[2] = { 1.1f, 1.3f }, kTextEyeX[2] = { -0.032f, 0.032f };

static const TextSign kSignWelcome = { L"Welcome to the quiet world", 620, 480, 2.0f, 34, RGB(28, 60, 110), RGB(240, 236, 228) };
static const TextSign kSignFeed = { L"Please do not feed\nthe sleeping avatars", 800, 860, 4.0f, 30, RGB(236, 232, 220), RGB(40, 36, 32) };
static const TextSign kSignExit = { L"Exit this way", 800, 860, 4.0f, 40, RGB(140, 30, 30), RGB(250, 250, 250) };

// Where a point of the left eye's picture at that depth is in the right eye's (the same row).
static float TextRightX(float leftX, float depth)
{
    const float t = kTextL[0] + leftX / kTextW * (kTextR[0] - kTextL[0]);
    const float x = kTextEyeX[0] + t * depth;                 // in the head
    const float tr = (x - kTextEyeX[1]) / depth;
    return (tr - kTextL[1]) / (kTextR[1] - kTextL[1]) * kTextW;
}

// One eye's picture: a dark wall with the signs. 32-bit rows from the top, as R8G8B8A8.
static std::vector<uint32_t> DrawTextScene(const TextSign *const *signs, int n, int eye)
{
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kTextW;
    bi.bmiHeader.biHeight = -kTextH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all = { 0, 0, kTextW, kTextH };
    HBRUSH wall = CreateSolidBrush(RGB(58, 62, 70));
    FillRect(dc, &all, wall);
    DeleteObject(wall);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < n; ++i) {
        const TextSign &s = *signs[i];
        const int cx = eye == 0 ? s.cx : (int)lroundf(TextRightX((float)s.cx, s.depth));
        HFONT font = CreateFontW(-s.fontPx, 0, 0, 0, 500, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT r = { 0, 0, 1200, 0 };
        DrawTextW(dc, s.text, -1, &r, DT_CALCRECT | DT_CENTER | DT_NOPREFIX);
        const int w = r.right - r.left, h = r.bottom - r.top, pad = s.fontPx;
        RECT box = { cx - w / 2 - pad, s.cy - h / 2 - pad / 2, cx + (w + 1) / 2 + pad, s.cy + (h + 1) / 2 + pad / 2 };
        HBRUSH bg = CreateSolidBrush(s.bg);
        FillRect(dc, &box, bg);
        DeleteObject(bg);
        RECT tr = { cx - w / 2, s.cy - h / 2, cx + (w + 1) / 2, s.cy + (h + 1) / 2 };
        SetTextColor(dc, s.ink);
        DrawTextW(dc, s.text, -1, &tr, DT_CENTER | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    GdiFlush();
    std::vector<uint32_t> px((size_t)kTextW * kTextH);
    const uint32_t *p = (const uint32_t *)bits;
    for (size_t i = 0; i < px.size(); ++i) px[i] = 0xff000000u | ((p[i] & 0xffu) << 16) | (p[i] & 0xff00u) | ((p[i] >> 16) & 0xffu);   // BGRA -> RGBA
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return px;
}

static void ServeText(Gfx &g, int seconds, bool sharedOk)
{
    int at[4] = { 12, 18, 23, 29 };
    if (EnvStr("VRUPSCALE_TEST_TEXT_TIMES")[0]) sscanf(EnvStr("VRUPSCALE_TEST_TEXT_TIMES"), "%d %d %d %d", &at[0], &at[1], &at[2], &at[3]);
    typedef void (*PFN_Hotkey)(int);
    HMODULE dxgiMod = GetModuleHandleA("dxgi.dll");
    PFN_Hotkey hotkey = dxgiMod ? (PFN_Hotkey)GetProcAddress(dxgiMod, "VrUpscaleDebugHotkey") : nullptr;
    ID3D11Texture2D *tex[2] = {};
    ID3D11ShaderResourceView *view[2] = {};
    for (int e = 0; e < 2; ++e) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kTextW; td.Height = kTextH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = sharedOk ? D3D11_RESOURCE_MISC_SHARED : 0;
        g.dev->CreateTexture2D(&td, nullptr, &tex[e]);
        view[e] = MakeView(g, tex[e], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
    }
    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, kTextW, kTextH);
    printf("serving text for %d s (phases from %d, %d, %d and %d s)\n", seconds, at[0], at[1], at[2], at[3]);
    fflush(stdout);
    const DWORD start = GetTickCount();
    int shown = -1;
    uint32_t serial = 0;
    while ((int)(GetTickCount() - start) < seconds * 1000) {
        const int age = (int)(GetTickCount() - start) / 1000;
        int phase = 0;
        while (phase < 4 && age >= at[phase]) ++phase;
        if (phase != shown) {
            shown = phase;
            const TextSign *first[2] = { &kSignWelcome, &kSignFeed }, *second[2] = { &kSignWelcome, &kSignExit };
            for (int e = 0; e < 2; ++e) {
                const std::vector<uint32_t> px = DrawTextScene(phase == 0 ? first : second, phase == 2 ? 0 : 2, e);
                g.ctx->UpdateSubresource(tex[e], 0, nullptr, px.data(), kTextW * 4, 0);
            }
            if (phase == 4 && hotkey) hotkey(10);   // Ctrl+F11
            printf("phase %d at %lu\n", phase, (unsigned long)GetTickCount());
            fflush(stdout);
        }
        for (UINT e = 0; e < 2; ++e) {   // a corner that changes: every frame is a new one
            uint32_t px[256];
            for (auto &p : px) p = 0xff000000u | (60u << 16) | ((64u + ((serial >> 7) & 31u)) << 8) | (64u + (serial & 31u));
            ++serial;
            const D3D11_BOX box = { 0, 0, 0, 16, 16, 1 };
            g.ctx->UpdateSubresource(tex[e], 0, &box, px, 64, 0);
        }
        DrawEye(g, tgt, view[0], 0, nullptr, false);
        DrawEye(g, tgt, view[1], 1, nullptr, false);
        g.ctx->Flush();
        Sleep(12);
    }
    ClearAll(g);
    for (int e = 0; e < 2; ++e) { SafeRelease(view[e]); SafeRelease(tex[e]); }
    tgt.Release();
    printf("served\n");
}
