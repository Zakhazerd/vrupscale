// The translation feature's plain computation (see trcore.h).
#include "trcore.h"
#include "core.h"

#include <objidl.h>
#include <gdiplus.h>
#include <math.h>
#include <algorithm>

namespace gp = Gdiplus;

namespace app {

// ---- text ---------------------------------------------------------------------------------------

namespace {

bool IsKana(wchar_t c) { return c >= 0x3041 && c <= 0x30FA; }   // hiragana and katakana (not the long-vowel mark or the dot)
bool IsHan(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF); }
bool IsHangul(wchar_t c) { return c >= 0xAC00 && c <= 0xD7AF; }
// written without spaces between words: Chinese characters, kana, their punctuation, full-width forms
bool NoSpaces(wchar_t c) { return IsHan(c) || (c >= 0x3000 && c <= 0x30FF) || (c >= 0xFF00 && c <= 0xFFEF); }
bool EndsSentence(wchar_t c) { return c == L'.' || c == L'!' || c == L'?' || c == 0x3002 || c == 0xFF01 || c == 0xFF1F; }

// What the recogniser makes of lines, frames and tiles: characters that are a few strokes or a box
// (one, two, three, ten, mouth, day, field, eye, work, river, and the katakana and hiragana alike).
bool Noise(wchar_t c)
{
    static const wchar_t kNoise[] = L"\x4E00\x4E8C\x4E09\x5341\x53E3\x65E5\x7530\x76EE\x5DE5\x5DDD\x4E28"
                                    L"\x30CF\x30CB\x30ED\x30A8\x30C8\x30A4\x30CE\x30D8\x304F\x3078\x3044\x308A\x3057\x3064\x3007\x25CB\x30FC\x30FB";
    for (const wchar_t *p = kNoise; *p; ++p)
        if (*p == c) return true;
    return false;
}

struct Line {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::wstring text;
    float H() const { return y1 - y0; }
};

void AppendJoined(std::wstring &to, const std::wstring &more)
{
    if (!to.empty() && !more.empty() && !NoSpaces(to.back()) && !NoSpaces(more.front()) && to.back() != L' ') to += L' ';
    to += more;
}

} // namespace

std::vector<TextBlock> GroupText(const OcrPage &page, int picW, int picH, const std::wstring &)
{
    std::vector<Line> lines;
    for (const OcrLine &ol : page.lines) {
        Line l;
        bool first = true;
        for (const OcrWord &w : ol.words) {
            if (w.w <= 0 || w.h <= 0) continue;
            if (first) {
                l.x0 = w.x; l.y0 = w.y; l.x1 = w.x + w.w; l.y1 = w.y + w.h;
                first = false;
            } else {
                l.x0 = std::min(l.x0, w.x); l.y0 = std::min(l.y0, w.y);
                l.x1 = std::max(l.x1, w.x + w.w); l.y1 = std::max(l.y1, w.y + w.h);
            }
            AppendJoined(l.text, w.text);
        }
        if (!first && l.H() >= 4) lines.push_back(l);
    }
    std::sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) { return a.y0 != b.y0 ? a.y0 < b.y0 : a.x0 < b.x0; });

    struct Blk {
        std::vector<int> ix;
        float x0, y0, x1, y1;
        std::wstring text;
    };
    std::vector<Blk> blocks;
    for (int i = 0; i < (int)lines.size(); ++i) {
        const Line &l = lines[i];
        int best = -1;
        float bestGap = 1e9f;
        for (int k = 0; k < (int)blocks.size(); ++k) {
            const Line &p = lines[blocks[k].ix.back()];
            const float h = 0.5f * (p.H() + l.H()), gap = l.y0 - p.y1, ratio = l.H() / p.H();
            if (gap < -0.4f * h || gap > 0.75f * h || ratio < 0.65f || ratio > 1.55f) continue;
            const float overlap = std::min(l.x1, p.x1) - std::max(l.x0, p.x0);
            if (!(overlap >= 0.5f * std::min(l.x1 - l.x0, p.x1 - p.x0) || fabsf(l.x0 - p.x0) < h)) continue;
            if (gap < bestGap) {
                bestGap = gap;
                best = k;
            }
        }
        if (best < 0) {
            blocks.push_back({ { i }, l.x0, l.y0, l.x1, l.y1, l.text });
            continue;
        }
        Blk &b = blocks[best];
        const Line &p = lines[b.ix.back()];
        // the line before runs to the block's right edge and doesn't end a sentence: the sentence goes on
        const bool goesOn = p.x1 >= b.x1 - 1.5f * p.H() && !(p.text.empty() || EndsSentence(p.text.back()));
        if (goesOn) AppendJoined(b.text, l.text);
        else b.text += L"\n" + l.text;
        b.ix.push_back(i);
        b.x0 = std::min(b.x0, l.x0); b.y0 = std::min(b.y0, l.y0);
        b.x1 = std::max(b.x1, l.x1); b.y1 = std::max(b.y1, l.y1);
    }

    std::vector<TextBlock> out;
    const float a = page.angle * 0.01745329252f, ca = cosf(a), sa = sinf(a);
    for (const Blk &b : blocks) {
        TextBlock t;
        t.text = b.text;
        // the recogniser's boxes are in the picture turned upright about its centre: turn the centre back
        const float dx = 0.5f * (b.x0 + b.x1) - 0.5f * picW, dy = 0.5f * (b.y0 + b.y1) - 0.5f * picH;
        t.cx = 0.5f * picW + dx * ca - dy * sa;
        t.cy = 0.5f * picH + dx * sa + dy * ca;
        t.w = b.x1 - b.x0;
        t.h = b.y1 - b.y0;
        t.angle = page.angle;
        std::vector<float> hs;
        for (int i : b.ix) hs.push_back(lines[i].H());
        std::sort(hs.begin(), hs.end());
        t.lineH = hs[hs.size() / 2];
        t.lines = (int)b.ix.size();
        out.push_back(t);
    }
    return out;
}

bool WorthTranslating(const std::wstring &text, const std::wstring &lang)
{
    int kana = 0, han = 0, hangul = 0, letters = 0, solid = 0, run = 0, longest = 0;
    for (wchar_t c : text) {
        const bool cjk = IsKana(c) || IsHan(c);
        if (IsKana(c)) ++kana;
        if (IsHan(c)) ++han;
        if (IsHangul(c)) ++hangul;
        if (cjk && !Noise(c)) ++solid;
        if (iswalpha(c) && !cjk && !IsHangul(c)) {
            ++letters;
            longest = std::max(longest, ++run);
        } else {
            run = 0;
        }
    }
    const wchar_t l0 = lang.empty() ? L'e' : towlower(lang[0]), l1 = lang.size() > 1 ? towlower(lang[1]) : 0;
    // (at least one character that is more than strokes: "entrance" is written with "mouth")
    if (l0 == L'j' && l1 == L'a') return kana + han >= 2 && solid >= 1;
    if (l0 == L'z' && l1 == L'h') return han >= 2 && solid >= 1;
    if (l0 == L'k' && l1 == L'o') return hangul >= 2;
    return letters >= 3 && longest >= 2;
}

float TextSimilarity(const std::wstring &a0, const std::wstring &b0)
{
    const std::wstring a = a0.substr(0, 200), b = b0.substr(0, 200);
    if (a == b) return 1.0f;
    const size_t n = a.size(), m = b.size();
    if (!n || !m) return 0.0f;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j)
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
        prev.swap(cur);
    }
    return 1.0f - (float)prev[m] / (float)std::max(n, m);
}

std::wstring LanguageName(const std::wstring &tag)
{
    static const wchar_t *const names[][2] = {
        { L"ja", L"Japanese" }, { L"en", L"English" }, { L"ko", L"Korean" }, { L"zh", L"Chinese" }, { L"de", L"German" },
        { L"fr", L"French" },   { L"es", L"Spanish" }, { L"it", L"Italian" }, { L"pt", L"Portuguese" }, { L"ru", L"Russian" },
        { L"nl", L"Dutch" },    { L"pl", L"Polish" },  { L"sv", L"Swedish" }, { L"tr", L"Turkish" }, { L"uk", L"Ukrainian" },
    };
    for (auto &n : names)
        if (tag.size() >= 2 && towlower(tag[0]) == n[0][0] && towlower(tag[1]) == n[0][1] && (tag.size() == 2 || tag[2] == L'-')) return n[1];
    return tag;
}

// ---- geometry -----------------------------------------------------------------------------------

V3 Apply(const M34 &m, const V3 &p)
{
    return { m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3], m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3],
             m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3] };
}
V3 Rotate(const M34 &m, const V3 &d)
{
    return { m.m[0][0] * d.x + m.m[0][1] * d.y + m.m[0][2] * d.z, m.m[1][0] * d.x + m.m[1][1] * d.y + m.m[1][2] * d.z,
             m.m[2][0] * d.x + m.m[2][1] * d.y + m.m[2][2] * d.z };
}
M34 Mul(const M34 &a, const M34 &b)
{
    M34 o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
        o.m[r][3] += a.m[r][3];
    }
    return o;
}
V3 Sub(const V3 &a, const V3 &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 Add(const V3 &a, const V3 &b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 Scale(const V3 &a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const V3 &a) { return sqrtf(Dot(a, a)); }
V3 Normalized(const V3 &a)
{
    const float n = Length(a);
    return n > 1e-12f ? Scale(a, 1.0f / n) : V3{ 0, 0, -1 };
}

namespace {

// The tangents at the picture's edges, counted rightwards and upwards. SteamVR's raw "top" is the
// negative one: the tangent at the bottom edge (its projection matrix puts raw bottom at clip y = +1).
struct Edges { float l, r, top, bottom; };
Edges EdgesOf(const EyeView &e)
{
    return { std::min(e.l, e.r), std::max(e.l, e.r), std::max(e.t, e.b), std::min(e.t, e.b) };
}

// A point of the head's space in the eye's own (the inverse of a rotation and a position).
V3 ToEye(const EyeView &e, const V3 &p)
{
    const V3 d = { p.x - e.toHead.m[0][3], p.y - e.toHead.m[1][3], p.z - e.toHead.m[2][3] };
    return { e.toHead.m[0][0] * d.x + e.toHead.m[1][0] * d.y + e.toHead.m[2][0] * d.z,
             e.toHead.m[0][1] * d.x + e.toHead.m[1][1] * d.y + e.toHead.m[2][1] * d.z,
             e.toHead.m[0][2] * d.x + e.toHead.m[1][2] * d.y + e.toHead.m[2][2] * d.z };
}

} // namespace

V3 PixelRay(const EyeView &e, float px, float py)
{
    const Edges g = EdgesOf(e);
    const float tx = g.l + (px / e.picW) * (g.r - g.l), ty = g.top - (py / e.picH) * (g.top - g.bottom);
    return Rotate(e.toHead, { tx, ty, -1.0f });
}

bool ProjectToPixel(const EyeView &e, const V3 &headPoint, float *px, float *py)
{
    const Edges g = EdgesOf(e);
    const V3 p = ToEye(e, headPoint);
    if (!(p.z < -1e-4f) || !(g.r - g.l > 1e-4f) || !(g.top - g.bottom > 1e-4f)) return false;
    *px = (p.x / -p.z - g.l) / (g.r - g.l) * e.picW;
    *py = (g.top - p.y / -p.z) / (g.top - g.bottom) * e.picH;
    return true;
}

V3 EyePosition(const EyeView &e) { return { e.toHead.m[0][3], e.toHead.m[1][3], e.toHead.m[2][3] }; }

bool StereoDepth(const Gray &left, const EyeView &le, const Gray &right, const EyeView &re, float cx, float cy, float w, float h,
                 float *depth, float *score)
{
    if (score) *score = 0;
    if (left.w <= 0 || right.w <= 0 || w < 8 || h < 6) return false;
    // the patch's samples in the left picture: a grid of at most about 160 x 48
    const int step = std::max(1, (int)lroundf(std::max(w / 160.0f, h / 48.0f)));
    const int x0 = std::max((int)floorf(cx - 0.5f * w), left.x0), x1 = std::min((int)ceilf(cx + 0.5f * w), left.x0 + left.w);
    const int y0 = std::max((int)floorf(cy - 0.5f * h), left.y0), y1 = std::min((int)ceilf(cy + 0.5f * h), left.y0 + left.h);
    struct Sample { int x, y; float v; };
    std::vector<Sample> s;
    for (int y = y0; y < y1; y += step)
        for (int x = x0; x < x1; x += step) s.push_back({ x, y, (float)left.at(x - left.x0, y - left.y0) });
    if (s.size() < 48) return false;
    {
        double m = 0, v = 0;
        for (auto &q : s) m += q.v;
        m /= s.size();
        for (auto &q : s) v += (q.v - m) * (q.v - m);
        if (v / s.size() < 9.0) return false;   // nothing to match on
    }
    // Where the patch is in the right picture if it is at 1/q metres along the left eye's forward axis:
    // the same for q = 0 (far away) and moving by a fixed step per step of q.
    const float kNearest = 0.4f, qMax = 1.0f / kNearest;
    const V3 ray = PixelRay(le, cx, cy), eye = EyePosition(le);
    float ax, ay, bx, by;
    if (!ProjectToPixel(re, Add(eye, Scale(ray, 1e4f)), &ax, &ay) || !ProjectToPixel(re, Add(eye, Scale(ray, kNearest)), &bx, &by)) return false;
    const float dx0 = ax - cx, dy0 = ay - cy, dx1 = bx - cx, dy1 = by - cy;
    // The patch is tried at whole pixels along that line (a: the shift along the axis it mostly runs
    // along, b: across it), and the best place is then told between pixels from its neighbours' scores.
    const bool alongX = fabsf(dx1 - dx0) >= fabsf(dy1 - dy0);
    const float a0 = alongX ? dx0 : dy0, a1 = alongX ? dx1 : dy1, b0 = alongX ? dy0 : dx0, b1 = alongX ? dy1 : dx1;
    if (fabsf(a1 - a0) < 1.0f) return false;
    const int lo = (int)floorf(std::min(a0, a1)), hi = (int)ceilf(std::max(a0, a1));
    auto match = [&](int a) -> float {   // zero-mean normalised correlation with the patch shifted by a; -2: not in the right picture
        const int b = (int)lroundf(b0 + (b1 - b0) * ((float)a - a0) / (a1 - a0));
        const int dx = alongX ? a : b, dy = alongX ? b : a;
        double sl = 0, sr = 0, sll = 0, srr = 0, slr = 0;
        int n = 0;
        for (const Sample &p : s) {
            const int rx = p.x + dx - right.x0, ry = p.y + dy - right.y0;
            if (rx < 0 || ry < 0 || rx >= right.w || ry >= right.h) continue;
            const double r = right.at(rx, ry);
            sl += p.v; sr += r; sll += p.v * p.v; srr += r * r; slr += p.v * r;
            ++n;
        }
        if (n < (int)s.size() * 3 / 4) return -2.0f;
        const double vl = sll - sl * sl / n, vr = srr - sr * sr / n;
        if (vl < 1e-6 || vr < 1e-6) return -1.0f;
        return (float)((slr - sl * sr / n) / sqrt(vl * vr));
    };
    // coarse: every `step` pixels; then every pixel around the best
    int bestA = lo;
    float best = -3;
    for (int a = lo; a <= hi; a += step) {
        const float m = match(a);
        if (m > best) {
            best = m;
            bestA = a;
        }
    }
    if (best < 0.3f) return false;
    const int centre = bestA;
    for (int a = std::max(lo, centre - step); a <= std::min(hi, centre + step); ++a) {
        const float m = match(a);
        if (m > best) {
            best = m;
            bestA = a;
        }
    }
    if (score) *score = best;
    if (best < 0.6f) return false;
    float a = (float)bestA;
    const float mPrev = match(bestA - 1), mNext = match(bestA + 1);
    if (mPrev > -1.5f && mNext > -1.5f) {   // between pixels: the top of the parabola through the three
        const float den = mPrev - 2 * best + mNext;
        if (den < -1e-6f) a += std::max(-0.5f, std::min(0.5f, 0.5f * (mPrev - mNext) / den));
    }
    const float q = qMax * std::max(0.0f, std::min(1.0f, (a - a0) / (a1 - a0)));
    *depth = q > 1.0f / 200.0f ? 1.0f / q : 200.0f;
    return true;
}

Placement PlaceBlock(const EyeView &le, float cx, float cy, float w, float h, float angleDeg, float depth)
{
    Placement p;
    const Edges g = EdgesOf(le);
    p.depth = depth;
    p.centre = Add(EyePosition(le), Scale(PixelRay(le, cx, cy), depth));
    p.width = depth * w * (g.r - g.l) / le.picW;
    p.height = depth * h * (g.top - g.bottom) / le.picH;
    const float a = angleDeg * 0.01745329252f, ca = cosf(a), sa = sinf(a);
    p.right = Normalized(Rotate(le.toHead, { ca, -sa, 0 }));   // (the picture's y runs down, the eye's up)
    p.up = Normalized(Rotate(le.toHead, { sa, ca, 0 }));
    return p;
}

// ---- colours and the label's picture ------------------------------------------------------------

namespace {

int Lum(uint32_t c) { return (int)((((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8); }

} // namespace

void TextColours(const uint32_t *bgra, int picW, int picH, int stride, float cx, float cy, float w, float h, uint32_t *background,
                 uint32_t *ink)
{
    *background = 0xff202428u;
    *ink = 0xfff2f2f2u;
    const int x0 = std::max(0, (int)(cx - 0.5f * w)), x1 = std::min(picW, (int)(cx + 0.5f * w));
    const int y0 = std::max(0, (int)(cy - 0.5f * h)), y1 = std::min(picH, (int)(cy + 0.5f * h));
    if (x1 - x0 < 4 || y1 - y0 < 4) return;
    const int sx = std::max(1, (x1 - x0) / 48), sy = std::max(1, (y1 - y0) / 16);
    std::vector<uint32_t> px;
    for (int y = y0; y < y1; y += sy)
        for (int x = x0; x < x1; x += sx) px.push_back(bgra[(size_t)y * stride + x]);
    // the background is most of the box: each channel's median
    uint32_t bg = 0xff000000u;
    std::vector<uint8_t> ch(px.size());
    for (int c = 0; c < 3; ++c) {
        for (size_t i = 0; i < px.size(); ++i) ch[i] = (uint8_t)(px[i] >> (8 * c));
        std::nth_element(ch.begin(), ch.begin() + ch.size() / 2, ch.end());
        bg |= (uint32_t)ch[ch.size() / 2] << (8 * c);
    }
    // the ink: the pixels furthest from it
    auto dist = [&](uint32_t p) {
        int d = 0;
        for (int c = 0; c < 3; ++c) {
            const int e = (int)((p >> (8 * c)) & 255) - (int)((bg >> (8 * c)) & 255);
            d += e * e;
        }
        return d;
    };
    std::vector<int> ds(px.size());
    for (size_t i = 0; i < px.size(); ++i) ds[i] = dist(px[i]);
    std::vector<int> sorted = ds;
    const size_t cut = sorted.size() - std::max<size_t>(1, sorted.size() / 8);
    std::nth_element(sorted.begin(), sorted.begin() + cut, sorted.end());
    const int limit = std::max(sorted[cut], 30 * 30);
    uint64_t sum[3] = {}, n = 0;
    for (size_t i = 0; i < px.size(); ++i) {
        if (ds[i] < limit) continue;
        for (int c = 0; c < 3; ++c) sum[c] += (px[i] >> (8 * c)) & 255;
        ++n;
    }
    uint32_t fg = Lum(bg) > 128 ? 0xff181818u : 0xfff2f2f2u;
    if (n) {
        const uint32_t seen = 0xff000000u | (uint32_t)(sum[0] / n) | ((uint32_t)(sum[1] / n) << 8) | ((uint32_t)(sum[2] / n) << 16);
        if (abs(Lum(seen) - Lum(bg)) >= 70) fg = seen;   // (else it would be hard to read: black or white)
    }
    *background = bg;
    *ink = fg;
}

LabelPicture DrawLabel(const std::wstring &text, float boxW, float boxH, float lineH, uint32_t background, uint32_t ink)
{
    LabelPicture out;
    if (text.empty() || boxW < 4 || boxH < 4) return out;
    lineH = std::max(8.0f, std::min(lineH, boxH));
    const float kTexels = 1.5f;        // texels per pixel of the eye's picture: the compositor resamples it
    const int kMaxSide = 990;          // (under the 1000 pixels vrupscale's DLL takes for an eye texture)
    const float maxFont = 0.85f * lineH, minFont = std::max(9.0f, 0.42f * lineH), pad = std::max(2.0f, 0.08f * lineH);

    gp::FontFamily segoe(L"Segoe UI");
    const gp::FontFamily *family = segoe.IsAvailable() ? &segoe : gp::FontFamily::GenericSansSerif();
    gp::StringFormat fmt;
    fmt.SetAlignment(gp::StringAlignmentCenter);
    fmt.SetLineAlignment(gp::StringAlignmentCenter);
    fmt.SetTrimming(gp::StringTrimmingEllipsisWord);
    gp::Bitmap probe(8, 8, PixelFormat32bppARGB);
    gp::Graphics pg(&probe);
    pg.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    auto fits = [&](float fontPx, float w, float h) {
        gp::Font font(family, fontPx, gp::FontStyleRegular, gp::UnitPixel);
        gp::RectF bounds;
        INT chars = 0, linesFilled = 0;
        pg.MeasureString(text.c_str(), -1, &font, gp::RectF(0, 0, w, 100000.0f), &fmt, &bounds, &chars, &linesFilled);
        return bounds.Height <= h && bounds.Width <= w + 0.5f;
    };
    // as large as fits in the text's own box; a box grown step by step for a translation that needs more room
    float grow = 1, font = minFont;
    bool found = false;
    for (int g = 0; g < 7 && !found; ++g) {
        grow = powf(1.2f, (float)g);
        const float w = boxW * grow - 2 * pad, h = boxH * grow - 2 * pad;
        if (w < 8 || h < 8) continue;
        for (float f = maxFont; f >= minFont - 0.01f; f *= 0.92f)
            if (fits(f, w, h)) {
                font = f;
                found = true;
                break;
            }
    }
    float scale = kTexels;
    const float w = boxW * grow, h = boxH * grow;
    scale = std::min(scale, std::min(kMaxSide / w, kMaxSide / h));
    out.w = std::max(16, (int)lroundf(w * scale));
    out.h = std::max(8, (int)lroundf(h * scale));
    out.grow = grow;
    out.fontPx = font;
    out.px.assign((size_t)out.w * out.h, 0xff000000u | background);
    {
        gp::Bitmap bmp(out.w, out.h, out.w * 4, PixelFormat32bppARGB, (BYTE *)out.px.data());
        gp::Graphics g(&bmp);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::Font f(family, font * scale, gp::FontStyleRegular, gp::UnitPixel);
        gp::SolidBrush brush(gp::Color(255, (BYTE)(ink >> 16), (BYTE)(ink >> 8), (BYTE)ink));
        g.DrawString(text.c_str(), -1, &f, gp::RectF(pad * scale, pad * scale, out.w - 2 * pad * scale, out.h - 2 * pad * scale), &fmt, &brush);
        g.Flush();
    }
    for (uint32_t &p : out.px) p |= 0xff000000u;
    return out;
}

// ---- self-test ----------------------------------------------------------------------------------

namespace {

OcrLine MakeLine(std::initializer_list<OcrWord> words)
{
    OcrLine l;
    l.words = words;
    return l;
}

OcrWord Word(const wchar_t *t, float x, float y, float w, float h)
{
    OcrWord o;
    o.text = t;
    o.x = x; o.y = y; o.w = w; o.h = h;
    return o;
}

// A patch of noise-like "writing" drawn into a grey picture at (cx, cy) of the eye's picture.
void DrawPattern(Gray &g, float cx, float cy, int w, int h, uint32_t seed)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int px = (int)lroundf(cx - w / 2.0f) + x - g.x0, py = (int)lroundf(cy - h / 2.0f) + y - g.y0;
            if (px < 0 || py < 0 || px >= g.w || py >= g.h) continue;
            uint32_t v = seed + (uint32_t)(x / 3) * 7919u + (uint32_t)(y / 3) * 104729u;   // 3 x 3 blocks, as strokes
            v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15;
            g.px[(size_t)py * g.w + px] = (v & 1) ? 230 : 40;
        }
}

} // namespace

int TranslateCoreSelfTest(std::wstring &report)
{
    int fails = 0;
    auto check = [&](bool ok, const std::wstring &what) {
        report += Fmt(L"%s %s\n", ok ? L"ok  " : L"FAIL", what.c_str());
        if (!ok) ++fails;
    };

    // ---- text
    {
        OcrPage p;
        // a two-line English sentence, a separate short line under it, and another column far right
        p.lines.push_back(MakeLine({ Word(L"Please", 100, 100, 80, 24), Word(L"do", 190, 100, 30, 24), Word(L"not", 230, 100, 40, 24),
                                     Word(L"feed", 280, 100, 60, 24), Word(L"the", 350, 100, 40, 24) }));
        p.lines.push_back(MakeLine({ Word(L"avatars", 100, 132, 100, 24), Word(L"today.", 210, 132, 80, 24) }));
        p.lines.push_back(MakeLine({ Word(L"Exit", 100, 164, 50, 24) }));
        p.lines.push_back(MakeLine({ Word(L"Shop", 900, 102, 70, 24) }));
        p.lines.push_back(MakeLine({ Word(L"Big", 100, 400, 120, 70), Word(L"title", 240, 400, 160, 70) }));
        const std::vector<TextBlock> b = GroupText(p, 1536, 1536, L"en");
        bool ok = b.size() == 3;
        const TextBlock *sentence = nullptr, *shop = nullptr, *title = nullptr;
        for (auto &t : b) {
            if (t.text.rfind(L"Please", 0) == 0) sentence = &t;
            if (t.text == L"Shop") shop = &t;
            if (t.text == L"Big title") title = &t;
        }
        ok = ok && sentence && shop && title && sentence->text == L"Please do not feed the avatars today.\nExit" && sentence->lines == 3 &&
             fabsf(sentence->cx - 245) < 1 && fabsf(sentence->cy - 144) < 1 && fabsf(sentence->w - 290) < 1 && fabsf(sentence->h - 88) < 1 &&
             fabsf(sentence->lineH - 24) < 1;
        check(ok, L"text: lines of one sentence are joined, a line after its end stays its own, columns and sizes stay apart");
    }
    {
        OcrPage p;   // Japanese: one character a "word"; a Latin word inside
        p.lines.push_back(MakeLine({ Word(L"VRChat", 50, 50, 120, 30), Word(L"\x3078", 175, 50, 30, 30), Word(L"\x3088", 207, 50, 30, 30),
                                     Word(L"\x3046", 239, 50, 30, 30), Word(L"\x3053", 271, 50, 30, 30), Word(L"\x305D", 303, 50, 30, 30) }));
        p.lines.push_back(MakeLine({ Word(L"\x5165", 50, 88, 30, 30), Word(L"\x53E3", 82, 88, 30, 30) }));
        const std::vector<TextBlock> b = GroupText(p, 800, 600, L"ja");
        check(b.size() == 1 && b[0].text == L"VRChat\x3078\x3088\x3046\x3053\x305D\x5165\x53E3" && b[0].lines == 2,
              L"text: Japanese is joined without spaces, across a wrapped line too");
    }
    {
        OcrPage p;   // turned 90 degrees clockwise: a box right of the centre in the upright picture is below it in the real one
        p.angle = 90;
        p.lines.push_back(MakeLine({ Word(L"Hello", 600, 290, 100, 20) }));
        const std::vector<TextBlock> b = GroupText(p, 1000, 600, L"en");
        check(b.size() == 1 && fabsf(b[0].cx - 500) < 1 && fabsf(b[0].cy - 450) < 1 && b[0].angle == 90,
              L"text: turned text is placed where it is in the picture");
    }
    check(WorthTranslating(L"\x3053\x3093\x306B\x3061\x306F", L"ja") && WorthTranslating(L"\x5165\x53E3", L"ja") &&
              WorthTranslating(L"\x30B2\x30FC\x30E0\x3092\x59CB\x3081\x308B", L"ja") && !WorthTranslating(L"Hello world", L"ja") &&
              WorthTranslating(L"\x30C8\x30A4\x30EC", L"ja") && !WorthTranslating(L"\x4E00\x4E8C", L"ja") &&
              !WorthTranslating(L"\x30FC\x30FC\x30FB", L"ja") && !WorthTranslating(L"\x53E3", L"ja") && !WorthTranslating(L"\x65E5\x76EE", L"ja") &&
              !WorthTranslating(L"12:30", L"ja"),
          L"text: Japanese is worth translating; Latin text, single characters and strokes read as characters are not");
    check(WorthTranslating(L"Exit here", L"en") && !WorthTranslating(L"I", L"en") && !WorthTranslating(L"1 2 3", L"en") &&
              WorthTranslating(L"\xC548\xB155\xD558\xC138\xC694", L"ko") && WorthTranslating(L"\x4F60\x597D\x4E16\x754C", L"zh"),
          L"text: other languages by their letters");
    check(TextSimilarity(L"abcdef", L"abcdef") == 1.0f && fabsf(TextSimilarity(L"abcdef", L"abcxef") - 5.0f / 6.0f) < 1e-4f &&
              TextSimilarity(L"abc", L"xyz") == 0.0f && TextSimilarity(L"", L"abc") == 0.0f,
          L"text: readings are compared by the characters that differ");

    // ---- geometry: a headset like the Bigscreen Beyond 2e (its eyes see further down than up, and outwards than inwards)
    EyeView le, re;
    le.l = -1.41254f; le.r = 0.89592f; le.t = -1.31704f; le.b = 0.95767f;
    re.l = -0.92093f; re.r = 1.38990f; re.t = -1.36232f; re.b = 0.93908f;
    le.picW = re.picW = 4572; le.picH = re.picH = 4572;
    le.toHead.m[0][3] = -0.0315f;
    re.toHead.m[0][3] = 0.0315f;
    {
        float px = 0, py = 0;
        const bool ahead = ProjectToPixel(le, { -0.0315f, 0, -5 }, &px, &py);   // straight ahead of the left eye
        const bool ok1 = ahead && fabsf(px - 4572 * 1.41254f / 2.30846f) < 0.5f && fabsf(py - 4572 * 0.95767f / 2.27471f) < 0.5f;
        const V3 ray = PixelRay(le, 1000, 3000);
        float bx = 0, by = 0;
        const bool back = ProjectToPixel(le, Add(EyePosition(le), Scale(ray, 3.7f)), &bx, &by);
        const V3 up = PixelRay(le, 2798, 0), down = PixelRay(le, 2798, 4572);
        check(ok1 && back && fabsf(bx - 1000) < 0.01f && fabsf(by - 3000) < 0.01f && fabsf(up.y - 0.95767f) < 1e-4f && fabsf(down.y + 1.31704f) < 1e-4f,
              L"geometry: pixels and directions agree both ways; straight ahead is above the picture's middle (SteamVR's \"bottom\" is the top edge)");
    }
    {
        // a patch of writing at 0.8, 2 and 6 metres: found again in the right eye, and its distance told
        bool all = true;
        std::wstring got;
        for (float z : { 0.8f, 2.0f, 6.0f }) {
            Gray L, R;
            L.w = L.h = R.w = R.h = 1536;
            L.x0 = 2030; L.y0 = 1157;   // (parts of the pictures around where the eyes look)
            R.x0 = 1054; R.y0 = 1100;
            L.px.assign((size_t)L.w * L.h, 128);
            R.px.assign((size_t)R.w * R.h, 128);
            const float cx = 2700, cy = 1900;
            DrawPattern(L, cx, cy, 300, 60, 11);
            float rx = 0, ry = 0;
            ProjectToPixel(re, Add(EyePosition(le), Scale(PixelRay(le, cx, cy), z)), &rx, &ry);
            DrawPattern(R, rx, ry, 300, 60, 11);
            DrawPattern(L, cx - 500, cy + 300, 200, 40, 5);   // something else in view
            DrawPattern(R, rx - 520, ry + 300, 200, 40, 5);
            float d = 0, sc = 0;
            const bool ok = StereoDepth(L, le, R, re, cx, cy, 300, 60, &d, &sc);
            got += Fmt(L" %.1f m -> %.2f m (%.2f)", z, ok ? d : 0.0f, sc);
            all = all && ok && fabsf(d - z) / z < 0.02f;
            if (z == 2.0f) {
                // the label for it covers the same pixels in both eyes
                const Placement p = PlaceBlock(le, cx, cy, 300, 60, 0, d);
                const V3 corner = Add(Add(p.centre, Scale(p.right, 0.5f * p.width)), Scale(p.up, 0.5f * p.height));
                float lx = 0, ly = 0, qx = 0, qy = 0;
                ProjectToPixel(le, corner, &lx, &ly);
                ProjectToPixel(re, corner, &qx, &qy);
                all = all && fabsf(lx - (cx + 150)) < 0.5f && fabsf(ly - (cy - 30)) < 0.5f && fabsf(qx - (rx + 150)) < 2.5f && fabsf(qy - (ry - 30)) < 1.0f;
            }
        }
        check(all, L"geometry: a text's distance from where it sits in each eye, and the label covers it in both:" + got);
        Gray L, R;
        L.w = L.h = R.w = R.h = 256;
        L.x0 = R.x0 = 2000; L.y0 = R.y0 = 2000;
        L.px.assign(65536, 128);
        R.px.assign(65536, 128);
        float d = 0;
        check(!StereoDepth(L, le, R, re, 2128, 2128, 120, 40, &d), L"geometry: a plain patch has no distance");
        DrawPattern(L, 2128, 2128, 120, 40, 3);   // in the left eye only
        check(!StereoDepth(L, le, R, re, 2128, 2128, 120, 40, &d), L"geometry: a patch one eye alone sees has no distance");
    }

    // ---- colours and labels
    {
        const int W = 200, H = 60;
        std::vector<uint32_t> px((size_t)W * H, 0xff284c9cu);   // blue
        for (int y = 20; y < 40; ++y)
            for (int x = 20; x < 180; x += 7)
                for (int k = 0; k < 2; ++k) px[(size_t)y * W + x + k] = 0xfff0e8e0u;   // light strokes
        uint32_t bg = 0, ink = 0;
        TextColours(px.data(), W, H, W, 100, 30, 180, 50, &bg, &ink);
        check((bg & 0xffffffu) == 0x284c9cu && Lum(ink) > 200, L"colours: the background and the ink are taken from the picture");
        std::vector<uint32_t> flat((size_t)W * H, 0xffe0e0e0u);
        TextColours(flat.data(), W, H, W, 100, 30, 180, 50, &bg, &ink);
        check((bg & 0xffffffu) == 0xe0e0e0u && Lum(ink) < 60, L"colours: ink that can't be told from the background becomes black or white");
    }
    {
        const LabelPicture a = DrawLabel(L"Entrance", 240, 48, 40, 0x284c9cu, 0xf0f0f0u);
        size_t inkPx = 0, bgPx = 0;
        for (uint32_t p : a.px) {
            if ((p & 0xffffffu) == 0x284c9cu) ++bgPx;
            else if (Lum(p) > 150) ++inkPx;
        }
        const bool ok1 = a.w == 360 && a.h == 72 && a.grow == 1.0f && bgPx > a.px.size() / 2 && inkPx > 200 && (a.px[0] & 0xffffffu) == 0x284c9cu &&
                         (a.px[0] >> 24) == 255;
        const LabelPicture b = DrawLabel(L"Please do not feed the avatars on the way to the exit, thank you very much", 120, 30, 26, 0x202020u, 0xf0f0f0u);
        const LabelPicture c = DrawLabel(L"Wide", 3000, 400, 300, 0, 0xffffffu);
        check(ok1 && b.grow > 1.0f && b.fontPx >= 10.9f && b.w > 180 && c.w <= 990 && c.h <= 990,
              Fmt(L"labels: a translation is drawn on the text's background; a long one gets a larger label (%.2fx at %.0f px), none a "
                  L"texture of 1000 pixels", b.grow, b.fontPx));
    }
    return fails;
}

} // namespace app
