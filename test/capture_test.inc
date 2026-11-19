// [translate]: the pictures the DLL hands the app (control_shared.h: VuCapture; src/capture.cpp).
// Included by temporal_tests.inc. The test reads them the way the app does: the shared texture is
// opened on a second device, copied and read there.

static bool ReadCapture(VuShared *sh, VuCapture &c)
{
    for (int tries = 0; tries < 1000; ++tries) {
        int32_t a = sh->captureSeq;
        if (a & 1) { Sleep(0); continue; }
        MemoryBarrier();
        memcpy(&c, (const void *)&sh->capture, sizeof(c));
        MemoryBarrier();
        if (sh->captureSeq == a) return true;
    }
    return false;
}

struct CaptureReader {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = S_OK;
    bool Init(Gfx &g)
    {
        IDXGIDevice *dx = nullptr;
        IDXGIAdapter *ad = nullptr;
        if (FAILED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dx)) || !dx) return false;
        dx->GetAdapter(&ad);
        dx->Release();
        if (!ad) return false;
        hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        ad->Release();
        return SUCCEEDED(hr) && dev;
    }
    // The shared texture's bytes (R8G8B8A8, w x h). False if it can't be opened or read.
    bool Read(const VuCaptureEye &e, std::vector<uint8_t> &out)
    {
        ID3D11Texture2D *t = nullptr, *st = nullptr;
        hr = dev->OpenSharedResource((HANDLE)(uintptr_t)e.handle, __uuidof(ID3D11Texture2D), (void **)&t);
        if (FAILED(hr) || !t) return false;
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        bool ok = d.Width == e.w && d.Height == e.h && (uint32_t)d.Format == e.format;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (ok) ok = SUCCEEDED(hr = dev->CreateTexture2D(&d, nullptr, &st)) && st;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ok) {
            ctx->CopyResource(st, t);
            ok = SUCCEEDED(hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
        }
        if (ok) {
            out.resize((size_t)e.w * e.h * 4);
            for (UINT y = 0; y < e.h; ++y) memcpy(&out[(size_t)y * e.w * 4], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)e.w * 4);
            ctx->Unmap(st, 0);
        }
        if (st) st->Release();
        t->Release();
        return ok;
    }
    void Release()
    {
        SafeRelease(ctx);
        SafeRelease(dev);
    }
};

// The largest difference (in 8-bit steps) between a capture and the same part of what the compositor drew.
static int CaptureDiff(const std::vector<uint8_t> &cap, const VuCaptureEye &e, const Image &drawn)
{
    int worst = 0;
    for (UINT y = 0; y < e.h; ++y)
        for (UINT x = 0; x < e.w; ++x) {
            const float *p = drawn.px.data() + ((size_t)(e.cropY + y) * drawn.w + e.cropX + x) * 4;
            const uint8_t *c = &cap[((size_t)y * e.w + x) * 4];
            for (int k = 0; k < 3; ++k) worst = std::max(worst, abs((int)c[k] - (int)lroundf(p[k] * 255.0f)));
        }
    return worst;
}

static void RunCaptureTest(Gfx &g, const Sequence &mov, const char *iniBase, const char *reqShared, float F, UINT misc)
{
    printf("[translate: pictures of what the compositor shows, for the app]\n");
    const int OW = mov.ow, OH = mov.oh;
    const UINT kSize = 256;   // (the smallest the DLL takes)
    HANDLE chMap = nullptr;
    VuShared *sh = OpenTestChannel(&chMap);
    CHECK(sh != nullptr, "test: the app's channel opens");
    if (!sh) return;
    wchar_t evName[192];
    {
        wchar_t name[128];
        DWORD en = GetEnvironmentVariableW(L"VRUPSCALE_CONTROL", name, 128);
        if (!(en > 0 && en < 128)) swprintf(name, 128, L"%ls.test.%lu", VU_CONTROL_NAME, GetCurrentProcessId());
        swprintf(evName, 192, L"%ls%ls", name, VU_CAPTURE_EVENT_SUFFIX);
    }
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, evName);
    CaptureReader rd;
    const bool second = rd.Init(g);
    CHECK(second, "test: a second Direct3D device, as the app's (0x%08lx)", (unsigned long)rd.hr);

    EyeTarget tgt;
    EnsureEyeTarget(g, tgt, OW, OH);
    char ini[1536];
    auto Ini = [&](const char *mode, int on, float hz) {
        snprintf(ini, sizeof(ini),
                 "%srequireShared=%s\n[upscale]\nmode=%s\nupscaleFactor=%.2f\nsharpness=0.5\n[translate]\nenabled=%d\ncaptureHz=%.1f\n"
                 "captureSize=%u\n", iniBase, reqShared, mode, F, on, hz, kSize);
        WriteIni(ini);
        Pump(g);
    };
    auto Bytes = [](const Image &im) {
        std::vector<uint8_t> b(im.px.size());
        for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)lroundf((im.px[i] < 0 ? 0 : (im.px[i] > 1 ? 1 : im.px[i])) * 255.0f);
        return b;
    };

    // off (the default): nothing is captured
    Ini("temporal", 0, 15.0f);
    VuCapture c0 = {};
    ReadCapture(sh, c0);
    {
        std::vector<Image> one = { mov.in[0][0], mov.in[1][0] };
        ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, one, misc);
        ID3D11ShaderResourceView *v[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0), MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
        for (int i = 0; i < 6; ++i) {
            DrawEye(g, tgt, v[0], 0, nullptr, false);
            DrawEye(g, tgt, v[1], 1, nullptr, false);
            Sleep(20);
        }
        VuCapture c = {};
        ReadCapture(sh, c);
        CHECK(c.round == c0.round && c.eyes == 0, "Translate off: no captures");
        ClearAll(g);
        SafeRelease(v[0]); SafeRelease(v[1]); SafeRelease(app);
    }

    // on, in both ways a game hands over its eyes (0: one array of two, 1: a texture per eye) and in
    // Temporal and FSR 1 mode: each capture is what the compositor drew, where the capture says
    bool sharedOk = true;
    for (int layout = 0; layout < 2 && sharedOk; ++layout) {
        for (int m = 0; m < 2 && sharedOk; ++m) {
            Ini(m == 0 ? "temporal" : "fsr", 1, 15.0f);
            ID3D11Texture2D *tex[2] = {};
            ID3D11ShaderResourceView *v[2] = {};
            if (layout == 0) {
                std::vector<Image> both = { mov.in[0][0], mov.in[1][0] };
                tex[0] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, both, misc);
                for (int e = 0; e < 2; ++e) v[e] = MakeView(g, tex[0], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, e);
            } else {
                for (int e = 0; e < 2; ++e) {
                    std::vector<Image> one = { mov.in[e][0] };
                    tex[e] = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, one, misc);
                    v[e] = MakeView(g, tex[e], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, -1);
                }
            }
            int rounds = 0, stereo = 0, worst = 0, placed = 0, read = 0, signalled = 0;
            uint32_t last = 0;
            {
                VuCapture c = {};
                ReadCapture(sh, c);
                last = c.round;
            }
            if (ev) WaitForSingleObject(ev, 0);   // (clear)
            const DWORD t0 = GetTickCount();
            int frames = 0;
            for (int i = 0; GetTickCount() - t0 < 1000; ++i, ++frames) {
                const int f = i % mov.frames;
                if (layout == 0) {
                    UploadSlices(g, tex[0], mov.in[0][f], mov.in[1][f]);
                } else {
                    for (int e = 0; e < 2; ++e) {
                        const std::vector<uint8_t> b = Bytes(mov.in[e][f]);
                        g.ctx->UpdateSubresource(tex[e], 0, nullptr, b.data(), mov.in[e][f].w * 4, 0);
                    }
                }
                Image drawn[2] = { DrawEye(g, tgt, v[0], 0), DrawEye(g, tgt, v[1], 1) };
                VuCapture c = {};
                if (!ReadCapture(sh, c) || c.round == last) {
                    Sleep(4);
                    continue;
                }
                last = c.round;
                ++rounds;
                if (ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) ++signalled;
                if (c.eyes == 3) ++stereo;
                bool where = c.eyes == 3;
                for (int e = 0; e < 2 && where; ++e) {
                    const VuCaptureEye &ce = c.eye[e];
                    where = ce.handle && ce.side == e && ce.w == std::min(kSize, (UINT)OW) && ce.h == std::min(kSize, (UINT)OH) &&
                            ce.picX == 0 && ce.picY == 0 && ce.picW == (UINT)OW && ce.picH == (UINT)OH && ce.cropX + ce.w <= (UINT)OW &&
                            ce.cropY + ce.h <= (UINT)OH && ce.format == (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM;
                    // without live eye tracking: centred on straight ahead (the projection is not known here: the middle)
                    if (where && !c.gazeLive) where = ce.cropX == (OW - ce.w) / 2 && ce.cropY == (OH - ce.h) / 2;
                }
                if (where) ++placed;
                for (int e = 0; e < 2 && where && second && sharedOk; ++e) {
                    std::vector<uint8_t> px;
                    if (!rd.Read(c.eye[e], px)) {
                        sharedOk = false;
                        break;
                    }
                    ++read;
                    worst = std::max(worst, CaptureDiff(px, c.eye[e], drawn[e]));
                }
                sh->captureAck = c.round;   // as the app does
                Sleep(4);
            }
            const char *what = layout == 0 ? "an array of the two eyes" : "a texture per eye";
            const char *mode = m == 0 ? "Temporal" : "FSR 1";
            CHECK(rounds >= 9 && rounds <= 16, "%s, %s: 15 captures a second asked for, %d taken in a second of %d frames", what, mode, rounds,
                  frames);
            CHECK(stereo == rounds && placed == rounds, "%s, %s: every capture has both eyes of one frame, %u px around the middle (%d of %d)",
                  what, mode, kSize, placed, rounds);
            CHECK(!ev || signalled == rounds, "%s, %s: the app is signalled for each (%d of %d)", what, mode, signalled, rounds);
            if (second && sharedOk)
                CHECK(read == 2 * rounds && worst <= 1, "%s, %s: each is the part of the picture the compositor drew (largest difference %d/255)",
                      what, mode, worst);
            ClearAll(g);
            for (int e = 0; e < 2; ++e) { SafeRelease(v[e]); SafeRelease(tex[e]); }
            Settle(g);
        }
    }
    if (second && !sharedOk)
        printf("  skip the shared texture can't be opened from a second device here (0x%08lx): its pixels were not checked\n",
               (unsigned long)rd.hr);

    // the app stops taking them: one every two seconds; then off: the app is told, once
    {
        Ini("fsr", 1, 15.0f);
        std::vector<Image> both = { mov.in[0][0], mov.in[1][0] };
        ID3D11Texture2D *app = MakeTexture(g, DXGI_FORMAT_R8G8B8A8_TYPELESS, both, misc);
        ID3D11ShaderResourceView *v[2] = { MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0), MakeView(g, app, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1) };
        VuCapture c = {};
        ReadCapture(sh, c);
        const uint32_t before = c.round;
        const DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 1500) {
            DrawEye(g, tgt, v[0], 0, nullptr, false);
            DrawEye(g, tgt, v[1], 1, nullptr, false);
            Sleep(4);
        }
        ReadCapture(sh, c);
        const int unacked = (int)(c.round - before);
        CHECK(unacked >= 3 && unacked <= 6, "the app takes none: after four the captures slow to one every two seconds (%d in 1.5 s)", unacked);
        const uint32_t on = c.round;
        Ini("fsr", 0, 15.0f);
        for (int i = 0; i < 6; ++i) {
            DrawEye(g, tgt, v[0], 0, nullptr, false);
            DrawEye(g, tgt, v[1], 1, nullptr, false);
            Sleep(20);
        }
        ReadCapture(sh, c);
        CHECK(c.round == on + 1 && c.eyes == 0 && c.eye[0].handle == 0, "Translate off again: the app is told the pictures are gone");
        sh->captureAck = c.round;
        ClearAll(g);
        SafeRelease(v[0]); SafeRelease(v[1]); SafeRelease(app);
    }
    if (ev) CloseHandle(ev);
    rd.Release();
    tgt.Release();
    UnmapViewOfFile(sh);
    CloseHandle(chMap);
    Settle(g);
}
