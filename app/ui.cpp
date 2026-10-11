// The app's look: see ui.h. All drawing is original; the style (grey sky, kraft ground, ink and
// Steam blue, heavy rounded type, flat ink-outlined illustrations) follows Valve's hardware pages.
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <unordered_map>

namespace gp = Gdiplus;

namespace ui {

namespace {

ULONG_PTR g_gdipToken = 0;
bool g_fontsLoaded = false;
gp::Bitmap *g_paper = nullptr;   // kraft texture tile
float g_scale = 1.0f;

struct FontSpec { int weight; float px; };
const FontSpec kSpec[F_COUNT] = {
    { 800, 46 },   // F_DISPLAY: the hero's headline
    { 800, 22 },   // F_BRAND
    { 800, 19 },   // F_H2: card titles
    { 400, 15 },   // F_BODY
    { 500, 14 },   // F_LABEL
    { 700, 14 },   // F_VALUE
    { 400, 13 },   // F_SMALL
    { 700, 13 },   // F_BUTTON
    { 700, 11 },   // F_TINY
    { 400, 12 },   // F_MONO (Consolas)
    { 700, 15 },   // F_BODYB: bold in body text (the change log)
    { 400, 14 },   // F_CODE (Consolas): code in body text
};

struct FontData {
    HFONT h = nullptr;
    int ascent = 0;
    std::unordered_map<wchar_t, int> adv;
    std::unordered_map<uint32_t, int> kern;
};
FontData g_f[F_COUNT];
HDC g_mdc = nullptr;   // measuring
HGDIOBJ g_mdcFont = nullptr;

void SelectMeasure(FontId f)
{
    if (g_mdcFont != g_f[f].h) {
        SelectObject(g_mdc, g_f[f].h);
        g_mdcFont = g_f[f].h;
    }
}

int Adv(FontId f, wchar_t ch)
{
    FontData &fd = g_f[f];
    auto it = fd.adv.find(ch);
    if (it != fd.adv.end()) return it->second;
    SelectMeasure(f);
    INT w = 0;
    if (!GetCharWidth32W(g_mdc, ch, ch, &w)) w = 0;
    fd.adv[ch] = w;
    return w;
}

int Kern(FontId f, wchar_t a, wchar_t b)
{
    const FontData &fd = g_f[f];
    if (fd.kern.empty()) return 0;
    auto it = fd.kern.find(((uint32_t)a << 16) | b);
    return it == fd.kern.end() ? 0 : it->second;
}

struct Run {
    std::wstring s;
    std::vector<INT> dx;
    int w = 0;   // device pixels
};

Run Layout(FontId f, const std::wstring &text, int maxW)
{
    Run r;
    r.s = text;
    const size_t n = text.size();
    r.dx.resize(n);
    for (size_t i = 0; i < n; ++i) {
        r.dx[i] = Adv(f, text[i]) + (i + 1 < n ? Kern(f, text[i], text[i + 1]) : 0);
        r.w += r.dx[i];
    }
    if (maxW > 0 && r.w > maxW) {
        const int ell = Adv(f, L'\x2026');
        while (!r.s.empty() && (r.w + ell > maxW || r.s.back() == L' ')) {
            r.w -= r.dx.back();
            r.s.pop_back();
            r.dx.pop_back();
        }
        if (!r.s.empty()) {   // the last advance included kerning with a character now gone
            r.w -= r.dx.back();
            r.dx.back() = Adv(f, r.s.back());
            r.w += r.dx.back();
        }
        r.s += L'\x2026';
        r.dx.push_back(ell);
        r.w += ell;
    }
    return r;
}

HRSRC FindFont(HINSTANCE inst, int id) { return FindResourceW(inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */); }

bool LoadFonts(HINSTANCE inst)
{
    int ok = 0;
    for (int id = 201; id <= 204; ++id) {
        HRSRC rs = FindFont(inst, id);
        HGLOBAL g = rs ? LoadResource(inst, rs) : nullptr;
        void *data = g ? LockResource(g) : nullptr;
        DWORD n = 0;
        if (data && AddFontMemResourceEx(data, SizeofResource(inst, rs), nullptr, &n) && n > 0) ++ok;
    }
    return ok == 4;
}

// A tile of kraft cardboard: soft mottling, grain, and short fibres, seamless.
gp::Bitmap *MakePaper()
{
    const int N = 256, G = 16;
    uint32_t seed = 0x5eed1234u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (float)((seed >> 8) & 0xffff) / 65535.0f;
    };
    float grid[G][G];
    for (auto &row : grid)
        for (float &v : row) v = rnd() * 2.0f - 1.0f;
    auto *bmp = new gp::Bitmap(N, N, PixelFormat32bppRGB);
    gp::BitmapData bd;
    gp::Rect rc(0, 0, N, N);
    if (bmp->LockBits(&rc, gp::ImageLockModeWrite, PixelFormat32bppRGB, &bd) == gp::Ok) {
        const float cell = (float)N / G;
        for (int y = 0; y < N; ++y) {
            uint32_t *row = (uint32_t *)((uint8_t *)bd.Scan0 + y * bd.Stride);
            for (int x = 0; x < N; ++x) {
                const float fx = x / cell, fy = y / cell;
                const int ix = (int)fx, iy = (int)fy;
                float tx = fx - ix, ty = fy - iy;
                tx = tx * tx * (3 - 2 * tx);
                ty = ty * ty * (3 - 2 * ty);
                const float a = grid[iy % G][ix % G], b = grid[iy % G][(ix + 1) % G];
                const float c = grid[(iy + 1) % G][ix % G], d = grid[(iy + 1) % G][(ix + 1) % G];
                const float v = (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
                const float l = v * 4.5f + (rnd() - 0.5f) * 7.0f;
                auto ch = [&](int base, float k) { return (uint32_t)std::max(0, std::min(255, (int)lroundf(base + l * k))); };
                row[x] = 0xff000000u | (ch(GetRValue(col::kraft), 1.0f) << 16) | (ch(GetGValue(col::kraft), 0.92f) << 8) |
                         ch(GetBValue(col::kraft), 0.78f);
            }
        }
        bmp->UnlockBits(&bd);
    }
    gp::Graphics g(bmp);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    for (int i = 0; i < 1100; ++i) {
        const float x = rnd() * N, y = rnd() * N, len = 2.5f + rnd() * 9.0f, ang = rnd() * 6.2832f;
        const bool dark = rnd() < 0.6f;
        gp::Pen pen(dark ? gp::Color(34, 150, 118, 84) : gp::Color(52, 240, 222, 196), 0.7f + rnd() * 0.6f);
        for (int ox = -1; ox <= 1; ++ox)
            for (int oy = -1; oy <= 1; ++oy) {
                const float x0 = x + ox * N, y0 = y + oy * N;
                g.DrawLine(&pen, x0, y0, x0 + cosf(ang) * len, y0 + sinf(ang) * len);
            }
    }
    return bmp;
}

void RoundPath(gp::GraphicsPath &p, const RectF &r, float rad)
{
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    if (rad <= 0.01f) {
        p.AddRectangle(gp::RectF(r.x, r.y, r.w, r.h));
        return;
    }
    const float d = rad * 2;
    p.AddArc(r.x, r.y, d, d, 180, 90);
    p.AddArc(r.r() - d, r.y, d, d, 270, 90);
    p.AddArc(r.r() - d, r.b() - d, d, d, 0, 90);
    p.AddArc(r.x, r.b() - d, d, d, 90, 90);
    p.CloseFigure();
}

} // namespace

COLORREF Mix(COLORREF a, COLORREF b, float t)
{
    auto m = [&](int x, int y) { return (int)lroundf(x + (y - x) * t); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

gp::Color C(COLORREF c, BYTE alpha) { return gp::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

void Startup(HINSTANCE inst)
{
    gp::GdiplusStartupInput in;
    gp::GdiplusStartup(&g_gdipToken, &in, nullptr);
    g_fontsLoaded = LoadFonts(inst);
    g_mdc = CreateCompatibleDC(nullptr);
    g_paper = MakePaper();
}

void Shutdown()
{
    delete g_paper;
    g_paper = nullptr;
    for (auto &f : g_f)
        if (f.h) DeleteObject(f.h);
    if (g_mdc) DeleteDC(g_mdc);
    gp::GdiplusShutdown(g_gdipToken);
}

void MakeFonts(UINT dpi)
{
    g_scale = dpi / 96.0f;
    if (g_mdc) SelectObject(g_mdc, GetStockObject(SYSTEM_FONT));
    g_mdcFont = nullptr;
    for (int i = 0; i < F_COUNT; ++i) {
        FontData &fd = g_f[i];
        if (fd.h) DeleteObject(fd.h);
        fd = FontData();
        LOGFONTW lf = {};
        lf.lfHeight = -(LONG)lroundf(kSpec[i].px * g_scale);
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfCharSet = DEFAULT_CHARSET;
        if (i == F_MONO || i == F_CODE) {
            lstrcpyW(lf.lfFaceName, L"Consolas");
            lf.lfWeight = FW_NORMAL;
        } else if (g_fontsLoaded) {
            swprintf(lf.lfFaceName, LF_FACESIZE, L"vrupscale Rubik %d", kSpec[i].weight);
            lf.lfWeight = FW_NORMAL;   // each weight is its own family (tools/fonts.py)
        } else {
            lstrcpyW(lf.lfFaceName, L"Segoe UI");
            lf.lfWeight = kSpec[i].weight >= 700 ? FW_BOLD : (kSpec[i].weight >= 500 ? FW_SEMIBOLD : FW_NORMAL);
        }
        fd.h = CreateFontIndirectW(&lf);
        SelectMeasure((FontId)i);
        TEXTMETRICW tm;
        GetTextMetricsW(g_mdc, &tm);
        fd.ascent = tm.tmAscent;
        const DWORD n = GetKerningPairsW(g_mdc, 0, nullptr);
        if (n > 0 && n < 100000) {
            std::vector<KERNINGPAIR> kp(n);
            GetKerningPairsW(g_mdc, n, kp.data());
            for (auto &k : kp)
                if (k.iKernAmount) fd.kern[((uint32_t)k.wFirst << 16) | k.wSecond] = k.iKernAmount;
        }
    }
}

HFONT Font(FontId f) { return g_f[f].h; }
float FontPx(FontId f) { return kSpec[f].px; }
float Mid(FontId f, float cy) { return cy + kSpec[f].px * 0.34f; }

float Measure(FontId f, const std::wstring &s) { return Layout(f, s, 0).w / g_scale; }

std::vector<std::wstring> WrapLines(FontId f, const std::wstring &s, float width)
{
    std::vector<std::wstring> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(L'\n', p);
        std::wstring para = s.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
        if (!para.empty() && para.back() == L'\r') para.pop_back();
        std::wstring cur;
        size_t q = 0;
        while (q < para.size()) {
            size_t sp = para.find(L' ', q);
            std::wstring word = para.substr(q, sp == std::wstring::npos ? std::wstring::npos : sp - q);
            q = sp == std::wstring::npos ? para.size() : sp + 1;
            if (word.empty()) continue;
            std::wstring cand = cur.empty() ? word : cur + L" " + word;
            if (cur.empty() || Measure(f, cand) <= width) cur = cand;
            else {
                out.push_back(cur);
                cur = word;
            }
        }
        out.push_back(cur);
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

// ---- canvas -------------------------------------------------------------------------------------

Canvas::Canvas(HDC dc, float scale, float offY) : dc_(dc), scale_(scale), offY_(offY)
{
    g_ = new gp::Graphics(dc);
    g_->SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g_->SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g_->ScaleTransform(scale, scale);
    g_->TranslateTransform(0, -offY);
}

Canvas::~Canvas() { delete g_; }

HDC Canvas::Begin(FontId f, COLORREF c)
{
    g_->Flush(gp::FlushIntentionSync);
    hdc_ = g_->GetHDC();
    oldFont_ = SelectObject(hdc_, g_f[f].h);
    SetTextColor(hdc_, c);
    SetBkMode(hdc_, TRANSPARENT);
    SetTextAlign(hdc_, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    return hdc_;
}

void Canvas::End()
{
    SelectObject(hdc_, oldFont_);
    g_->ReleaseHDC(hdc_);
    hdc_ = nullptr;
}

void Canvas::Fill(const RectF &r, COLORREF c, BYTE a)
{
    gp::SolidBrush b(C(c, a));
    g_->FillRectangle(&b, r.x, r.y, r.w, r.h);
}

void Canvas::FillRound(const RectF &r, float rad, COLORREF c, BYTE a)
{
    gp::GraphicsPath p;
    RoundPath(p, r, rad);
    gp::SolidBrush b(C(c, a));
    g_->FillPath(&b, &p);
}

void Canvas::StrokeRound(const RectF &r, float rad, COLORREF c, float width, BYTE a)
{
    gp::GraphicsPath p;
    RoundPath(p, r, rad);
    gp::Pen pen(C(c, a), width);
    pen.SetLineJoin(gp::LineJoinRound);
    g_->DrawPath(&pen, &p);
}

void Canvas::FillCircle(float cx, float cy, float rad, COLORREF c, BYTE a)
{
    gp::SolidBrush b(C(c, a));
    g_->FillEllipse(&b, cx - rad, cy - rad, rad * 2, rad * 2);
}

void Canvas::StrokeCircle(float cx, float cy, float rad, COLORREF c, float width)
{
    gp::Pen pen(C(c), width);
    g_->DrawEllipse(&pen, cx - rad, cy - rad, rad * 2, rad * 2);
}

void Canvas::Line(float x0, float y0, float x1, float y1, COLORREF c, float width, BYTE a)
{
    gp::Pen pen(C(c, a), width);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    g_->DrawLine(&pen, x0, y0, x1, y1);
}

float Canvas::Text(FontId f, const std::wstring &s, float x, float base, COLORREF c, int align, float maxW)
{
    if (s.empty()) return 0;
    Run r = Layout(f, s, maxW > 0 ? (int)(maxW * scale_) : 0);
    int X = (int)lroundf(x * scale_);
    const int Y = (int)lroundf((base - offY_) * scale_);
    if (align == 1) X -= r.w / 2;
    else if (align == 2) X -= r.w;
    HDC dc = Begin(f, c);
    ExtTextOutW(dc, X, Y, 0, nullptr, r.s.c_str(), (UINT)r.s.size(), r.dx.data());
    End();
    return r.w / scale_;
}

int Canvas::Wrap(FontId f, const std::wstring &s, const RectF &box, float lineH, COLORREF c, int maxLines)
{
    std::vector<std::wstring> lines = WrapLines(f, s, box.w);
    const int n = std::min((int)lines.size(), maxLines);
    for (int i = 0; i < n; ++i) {
        std::wstring t = lines[i];
        if (i == n - 1 && (int)lines.size() > n) t += L" \x2026\x2026";   // forces the ellipsis
        Text(f, t, box.x, Mid(f, box.y + lineH * (i + 0.5f)), c, 0, box.w);
    }
    return n;
}

void Canvas::PathText(FontId f, const std::wstring &s, float x, float base, float w, COLORREF c)
{
    HDC dc = Begin(f, c);
    SetTextAlign(dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    const int Y = (int)lroundf((base - offY_) * scale_) - g_f[f].ascent;
    RECT rc = { (int)lroundf(x * scale_), Y, (int)lroundf((x + w) * scale_), Y + g_f[f].ascent * 2 };
    std::wstring t = s;
    DrawTextW(dc, &t[0], (int)t.size(), &rc, DT_PATH_ELLIPSIS | DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_TOP);
    End();
}

// ---- page -------------------------------------------------------------------------------------

float GroundY(float x, float width)
{
    // like the hardware pages: a cut that dips, rises gently, and falls away to the right
    static const float px[] = { 0.0f, 0.17f, 0.56f, 0.74f, 1.0f };
    static const float py[] = { 176, 189, 178, 170, 194 };
    const float t = std::max(0.0f, std::min(1.0f, x / width));
    for (int i = 0; i < 4; ++i)
        if (t <= px[i + 1]) {
            const float u = (t - px[i]) / (px[i + 1] - px[i]);
            return py[i] + (py[i + 1] - py[i]) * u;
        }
    return py[4];
}

void DrawGround(Canvas &c, float width, float height, float shift)
{
    c.Fill({ 0, 0, width, height }, col::sky);
    static const float px[] = { 0.0f, 0.17f, 0.56f, 0.74f, 1.0f };
    gp::PointF pts[7];
    for (int i = 0; i < 5; ++i) pts[i] = gp::PointF(px[i] * width, GroundY(px[i] * width, width) + shift);
    pts[0].X = -2;
    pts[4].X = width + 2;
    pts[5] = gp::PointF(width + 2, height + 2);
    pts[6] = gp::PointF(-2, height + 2);
    if (g_paper) {
        gp::TextureBrush tb(g_paper, gp::WrapModeTile);
        tb.ScaleTransform(1.0f / c.Scale(), 1.0f / c.Scale());   // texture at device resolution
        c.G().FillPolygon(&tb, pts, 7);
    } else {
        gp::SolidBrush b(C(col::kraft));
        c.G().FillPolygon(&b, pts, 7);
    }
}

void DrawCard(Canvas &c, const RectF &r, const wchar_t *title)
{
    c.FillRound({ r.x, r.y + 5, r.w, r.h }, 18, col::kraftShadow, 150);
    c.FillRound(r, 18, col::card);
    if (title && *title) c.Text(F_H2, title, r.x + 20, r.y + 34, col::ink);
}

void DrawWell(Canvas &c, const RectF &r) { c.FillRound(r, 12, col::well); }

// ---- the scene --------------------------------------------------------------------------------

namespace {

void Stroke(gp::Graphics &g, const gp::PointF *p, int n, COLORREF c, float w)
{
    gp::Pen pen(C(c), w);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    g.DrawLines(&pen, p, n);
}

// A leg from the hip to the ground, with a foot pointing right (the walking direction).
void Leg(Canvas &c, float hipX, float hipY, float footX, float groundY, float w)
{
    const gp::PointF leg[3] = { gp::PointF(hipX, hipY), gp::PointF((hipX + footX) / 2 - 2, (hipY + groundY) / 2 + 1),
                                gp::PointF(footX, groundY - w * 0.9f) };
    Stroke(c.G(), leg, 3, col::blue, w);
    const gp::PointF foot[2] = { gp::PointF(footX - w * 0.5f, groundY - w * 0.55f), gp::PointF(footX + w * 1.6f, groundY - w * 0.55f) };
    Stroke(c.G(), foot, 2, col::blue, w * 1.15f);
}

void Eye(Canvas &c, float x, float y, float r, bool closed, COLORREF lid = RGB(150, 154, 160))
{
    if (closed) {
        gp::Pen pen(C(lid), std::max(1.6f, r * 0.34f));
        pen.SetStartCap(gp::LineCapRound);
        pen.SetEndCap(gp::LineCapRound);
        c.G().DrawArc(&pen, x - r * 0.8f, y - r * 0.75f, r * 1.6f, r * 1.1f, 20, 140);
        return;
    }
    c.FillCircle(x, y, r, col::mid);
    c.StrokeCircle(x, y, r, col::dark, r * 0.32f);
    c.FillCircle(x - r * 0.32f, y - r * 0.32f, r * 0.28f, col::sky);
}

void Star(Canvas &c, float x, float y, float s)
{
    gp::PointF p[8];
    for (int i = 0; i < 8; ++i) {
        const float a = i * 3.14159265f / 4 - 3.14159265f / 2, rr = (i % 2) ? s * 0.3f : s;
        p[i] = gp::PointF(x + cosf(a) * rr, y + sinf(a) * rr);
    }
    gp::SolidBrush b(C(col::blue));
    c.G().FillPolygon(&b, p, 8);
    gp::Pen pen(C(col::dark), std::max(1.2f, s * 0.14f));
    pen.SetLineJoin(gp::LineJoinRound);
    c.G().DrawPolygon(&pen, p, 8);
}

// A tiny picture (sky, hills) as the characters' faces: pixelated for the input, smooth for the output.
const COLORREF kS0 = RGB(160, 220, 255), kS1 = RGB(118, 203, 255), kS2 = RGB(196, 234, 255);

void PixelFrame(Canvas &c, float cx, float width, Mood mood)
{
    const float size = 50, gl = GroundY(cx - 8, width), gr = GroundY(cx + 14, width);
    const float B = std::min(gl, gr) - 26;
    const RectF r = { cx - size / 2, B - size, size, size };
    Leg(c, cx - 10, B - 4, cx - 12, gl, 6);
    Leg(c, cx + 10, B - 4, cx + 13, gr, 6);
    static const COLORREF px[5][5] = {
        { kS2, kS0, kS2, kS0, kS1 }, { kS0, kS1, kS0, kS2, kS0 }, { kS1, col::mid, kS0, kS1, col::mid },
        { col::mid, col::body, col::mid, col::mid, col::body }, { col::body, col::dark, col::body, col::dark, col::dark } };
    gp::GraphicsPath clip;
    RoundPath(clip, r, 8);
    c.G().SetClip(&clip);
    const float cell = size / 5;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) c.Fill({ r.x + x * cell - 0.3f, r.y + y * cell - 0.3f, cell + 0.6f, cell + 0.6f }, px[y][x]);
    c.G().ResetClip();
    c.StrokeRound(r.Inflate(-1.5f), 7, col::dark, 3.5f);
    Eye(c, cx - 9, r.y + 17, 4.2f, mood == MoodSleep, col::dark);
    Eye(c, cx + 9, r.y + 17, 4.2f, mood == MoodSleep, col::dark);
}

void SharpFrame(Canvas &c, float cx, float width, Mood mood)
{
    const float size = 66, gl = GroundY(cx - 12, width), gr = GroundY(cx + 16, width);
    const float B = std::min(gl, gr) - 30;
    const RectF r = { cx - size / 2, B - size, size, size };
    Leg(c, cx - 13, B - 4, cx - 15, gl, 6.5f);
    Leg(c, cx + 13, B - 4, cx + 16, gr, 6.5f);
    gp::GraphicsPath clip;
    RoundPath(clip, r, 10);
    gp::LinearGradientBrush sky(gp::PointF(0, r.y), gp::PointF(0, r.b()), C(kS2), C(kS1));
    c.G().FillPath(&sky, &clip);
    c.G().SetClip(&clip);
    const float x0 = r.x, y0 = r.y, k = size / 66.0f;
    c.FillCircle(x0 + 50 * k, y0 + 16 * k, 6 * k, col::paper);
    const gp::PointF hills[] = { gp::PointF(x0 - 2, y0 + 48 * k), gp::PointF(x0 + 20 * k, y0 + 27 * k), gp::PointF(x0 + 33 * k, y0 + 39 * k),
                                 gp::PointF(x0 + 47 * k, y0 + 24 * k), gp::PointF(x0 + 68 * k, y0 + 46 * k), gp::PointF(x0 + 68 * k, y0 + 70 * k),
                                 gp::PointF(x0 - 2, y0 + 70 * k) };
    gp::SolidBrush hb(C(col::mid));
    c.G().FillPolygon(&hb, hills, 7);
    const gp::PointF front[] = { gp::PointF(x0 - 2, y0 + 58 * k), gp::PointF(x0 + 30 * k, y0 + 47 * k), gp::PointF(x0 + 68 * k, y0 + 59 * k),
                                 gp::PointF(x0 + 68 * k, y0 + 70 * k), gp::PointF(x0 - 2, y0 + 70 * k) };
    gp::SolidBrush fb(C(col::body));
    c.G().FillPolygon(&fb, front, 5);
    c.G().ResetClip();
    c.StrokeRound(r.Inflate(-1.75f), 9, col::dark, 3.5f);
    Eye(c, cx - 11, r.y + 21, 4.8f, mood == MoodSleep, col::dark);
    Eye(c, cx + 11, r.y + 21, 4.8f, mood == MoodSleep, col::dark);
    if (mood == MoodWork) Star(c, r.r() + 6, r.y - 4, 7);
}

void Headset(Canvas &c, float hx, float width, Mood mood)
{
    gp::Graphics &g = c.G();
    const float gl = GroundY(hx - 34, width), gr = GroundY(hx + 40, width);
    const float B = std::min(gl, gr) - 34;   // body bottom
    const float L = hx - 75, R = hx + 75, T = B - 80;
    // head strap behind: side arms and a band over the top
    for (float sx : { L - 15, R - 15 }) {
        c.FillRound({ sx, T + 27, 30, 20 }, 8, col::mid);
        c.StrokeRound({ sx, T + 27, 30, 20 }, 8, col::dark, 3.5f);
    }
    {
        gp::Pen outer(C(col::dark), 15), inner(C(col::mid), 8);
        for (gp::Pen *p : { &outer, &inner }) {
            p->SetStartCap(gp::LineCapRound);
            p->SetEndCap(gp::LineCapRound);
            g.DrawBezier(p, L + 26, T + 8, L + 46, T - 30, R - 46, T - 30, R - 26, T + 8);
        }
    }
    Leg(c, hx - 30, B - 6, hx - 34, gl, 7);
    Leg(c, hx + 30, B - 6, hx + 40, gr, 7);
    // the body, with a cut-out for the nose
    gp::GraphicsPath body;
    const float rr = 26, d = rr * 2, nw = 19, nh = 17;
    body.AddArc(L, T, d, d, 180, 90);
    body.AddArc(R - d, T, d, d, 270, 90);
    body.AddArc(R - d, B - d, d, d, 0, 90);
    body.AddLine(R - rr, B, hx + nw + 6, B);
    body.AddBezier(hx + nw + 6, B, hx + nw - 2, B, hx + nw * 0.7f, B - nh, hx, B - nh);
    body.AddBezier(hx, B - nh, hx - nw * 0.7f, B - nh, hx - nw + 2, B, hx - nw - 6, B);
    body.AddLine(hx - nw - 6, B, L + rr, B);
    body.AddArc(L, B - d, d, d, 90, 90);
    body.CloseFigure();
    gp::SolidBrush bb(C(col::body));
    g.FillPath(&bb, &body);
    // the visor, with a glare
    const RectF fp = { L + 13, T + 11, 124, 47 };
    gp::GraphicsPath visor;
    RoundPath(visor, fp, 17);
    gp::SolidBrush vb(C(col::dark));
    g.FillPath(&vb, &visor);
    g.SetClip(&visor);
    c.Line(fp.x + 92, fp.y - 10, fp.x + 64, fp.b() + 10, col::mid, 13);
    c.Line(fp.x + 112, fp.y - 10, fp.x + 84, fp.b() + 10, col::mid, 5);
    g.ResetClip();
    gp::Pen outline(C(col::dark), 4.5f);
    outline.SetLineJoin(gp::LineJoinRound);
    g.DrawPath(&outline, &body);
    const bool closed = mood == MoodSleep;
    Eye(c, hx - 31, fp.y + 23, 9.5f, closed);
    Eye(c, hx + 31, fp.y + 23, 9.5f, closed);
    if (mood == MoodWork) {
        Star(c, R + 22, T - 4, 11);
        Star(c, R + 40, T + 20, 7);
        Star(c, L - 16, T - 6, 7);
    } else if (mood == MoodSleep) {
        c.Text(F_VALUE, L"z", R + 10, T + 8, col::inkFaint);
        c.Text(F_H2, L"z", R + 22, T - 8, col::inkFaint);
        c.Text(F_BRAND, L"Z", R + 38, T - 26, col::inkFaint);
    } else if (mood == MoodWarn) {
        c.FillCircle(R + 20, T - 6, 12, col::orange);
        c.StrokeCircle(R + 20, T - 6, 12, col::dark, 3);
        c.Text(F_VALUE, L"!", R + 20, Mid(F_VALUE, T - 6), col::dark, 1);
    }
}

} // namespace

void DrawScene(Canvas &c, float cx, float width, Mood mood)
{
    PixelFrame(c, cx - 158, width, mood);
    Headset(c, cx, width, mood);
    SharpFrame(c, cx + 150, width, mood);
}

// ---- widgets ----------------------------------------------------------------------------------

RectF SliderTrack(const Widget &w) { return { w.r.x + w.labelW, w.r.cy() - 4, w.r.w - w.labelW - 58, 8 }; }

int SliderPosAt(const Widget &w, float x)
{
    const RectF t = SliderTrack(w);
    const float u = std::max(0.0f, std::min(1.0f, (x - t.x) / t.w));
    return w.lo + (int)lroundf(u * (w.hi - w.lo));
}

int SegmentAt(const Widget &w, float x)
{
    const int n = (int)w.items.size();
    if (n == 0) return 0;
    return std::max(0, std::min(n - 1, (int)((x - w.r.x) / (w.r.w / n))));
}

RectF SwitchRect(const Widget &w) { return { w.r.x, w.r.cy() - 12, 44, 24 }; }

void DrawFocus(Canvas &c, const RectF &r, float rad) { c.StrokeRound(r.Inflate(3.5f), rad + 3.5f, col::blueDeep, 2.2f); }

void DrawCheck(Canvas &c, const RectF &box, bool on, bool enabled, bool hot)
{
    const BYTE a = enabled ? 255 : 110;
    if (on) {
        c.FillRound(box, 5, hot ? Mix(col::blue, RGB(255, 255, 255), 0.18f) : col::blue, a);
        c.StrokeRound(box.Inflate(-1), 4, col::ink, 2, a);
        gp::PointF p[3] = { gp::PointF(box.x + 4.6f, box.y + 9.4f), gp::PointF(box.x + 7.8f, box.y + 12.6f),
                            gp::PointF(box.x + 13.4f, box.y + 5.6f) };
        gp::Pen pen(C(col::ink, a), 2.4f);
        pen.SetStartCap(gp::LineCapRound);
        pen.SetEndCap(gp::LineCapRound);
        pen.SetLineJoin(gp::LineJoinRound);
        c.G().DrawLines(&pen, p, 3);
    } else {
        c.FillRound(box, 5, hot && enabled ? Mix(col::paper, col::blue, 0.2f) : col::paper, a);
        c.StrokeRound(box.Inflate(-1), 4, enabled ? col::ink : col::inkFaint, 2, a);
    }
}

void DrawWidget(Canvas &c, const Widget &w, const Interact &st)
{
    if (!w.visible) return;
    const bool hot = w.enabled && st.hover == w.id;
    const bool down = hot && st.pressed == w.id;
    const bool focus = st.focusVisible && st.focus == w.id;
    const COLORREF text = w.enabled ? col::ink : col::inkFaint;
    const BYTE a = w.enabled ? 255 : 110;
    switch (w.kind) {
    case Kind::Button: {
        RectF r = w.r;
        if (down) r.y += 1;
        const float rad = r.h / 2;
        if (w.style == BtnPrimary) c.FillRound(r, rad, hot ? Mix(col::blue, RGB(255, 255, 255), 0.2f) : col::blue, a);
        else if (hot) c.FillRound(r, rad, col::ink, 20);
        c.StrokeRound(r.Inflate(-1), rad - 1, col::ink, 2, w.enabled ? 255 : 90);
        c.Text(F_BUTTON, w.text, r.cx(), Mid(F_BUTTON, r.cy()), text, 1, r.w - 16);
        if (focus) DrawFocus(c, w.r, rad);
        break;
    }
    case Kind::Toggle: {
        const RectF s = SwitchRect(w);
        c.FillRound(s, 12, w.on ? (hot ? Mix(col::blue, RGB(255, 255, 255), 0.2f) : col::blue) : (hot ? Mix(col::control, col::ink, 0.1f) : col::control), a);
        c.StrokeRound(s.Inflate(-1), 11, col::ink, 2, w.enabled ? 255 : 90);
        const float kx = w.on ? s.r() - 12 : s.x + 12;
        c.FillCircle(kx, s.cy(), 7, col::paper, a);
        gp::Pen pen(C(col::ink, w.enabled ? 255 : 90), 2);
        c.G().DrawEllipse(&pen, kx - 7, s.cy() - 7, 14.0f, 14.0f);
        if (!w.text.empty()) {
            const float x = s.r() + 11;
            const float tw = c.Text(F_LABEL, w.text, x, Mid(F_LABEL, w.r.cy()), text, 0, w.r.r() - x);
            if (!w.note.empty())
                c.Text(F_SMALL, w.note, x + tw + 8, Mid(F_SMALL, w.r.cy()), col::inkFaint, 0, w.r.r() - x - tw - 8);
        }
        if (focus) DrawFocus(c, s, 12);
        break;
    }
    case Kind::Slider: {
        c.Text(F_LABEL, w.text, w.r.x, Mid(F_LABEL, w.r.cy()), text, 0, w.labelW - 8);
        const RectF t = SliderTrack(w);
        const float u = w.hi > w.lo ? (float)(w.pos - w.lo) / (float)(w.hi - w.lo) : 0;
        const float tx = t.x + u * t.w;
        c.FillRound(t, 4, col::control);
        if (tx > t.x + 1) c.FillRound({ t.x, t.y, tx - t.x, t.h }, 4, w.enabled ? col::blue : Mix(col::control, col::blue, 0.35f));
        const float rad = (hot || st.drag == w.id) ? 10.5f : 9.5f;
        c.FillCircle(tx, t.cy(), rad, col::paper, a);
        gp::Pen pen(C(col::ink, w.enabled ? 255 : 90), 2.5f);
        c.G().DrawEllipse(&pen, tx - rad, t.cy() - rad, rad * 2, rad * 2);
        c.Text(F_VALUE, w.value, w.r.r(), Mid(F_VALUE, w.r.cy()), text, 2);
        if (focus) c.StrokeCircle(tx, t.cy(), rad + 4, col::blueDeep, 2.2f);
        break;
    }
    case Kind::Segment: {
        const RectF r = w.r;
        c.FillRound(r, r.h / 2, col::control);
        const int n = (int)w.items.size();
        const float iw = r.w / std::max(1, n);
        for (int i = 0; i < n; ++i) {
            const RectF s = { r.x + i * iw + 3, r.y + 3, iw - 6, r.h - 6 };
            if (i == w.sel) {
                c.FillRound(s, s.h / 2, w.enabled ? col::blue : Mix(col::control, col::blue, 0.35f));
                c.StrokeRound(s.Inflate(-1), s.h / 2 - 1, col::ink, 2, a);
            } else if (hot && st.hoverPart == i) {
                c.FillRound(s, s.h / 2, col::ink, 20);
            }
            c.Text(F_BUTTON, w.items[i], s.cx(), Mid(F_BUTTON, s.cy()), text, 1, s.w - 8);
        }
        if (focus) DrawFocus(c, r, r.h / 2);
        break;
    }
    case Kind::Custom: break;
    }
}

} // namespace ui
