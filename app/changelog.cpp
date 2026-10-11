// The change log window (changelog.h). CHANGELOG.md is read with the little Markdown it uses:
// "## <version>" starts a version, "- " a bullet ("  - " one level deeper), other lines are
// paragraphs; inside them **bold**, `code` and [text](link) (shown as its text).
#include "changelog.h"
#include "core.h"
#include "ui.h"
#include "update.h"

#include <windowsx.h>
#include <math.h>
#include <algorithm>
#include <vector>

namespace app {
namespace {

using ui::RectF;
namespace col = ui::col;

constexpr int kResChangeLog = 205;     // app.rc: CHANGELOG.md
constexpr float kMargin = 24;          // the cards' distance from the window's sides
constexpr float kPad = 20;             // text inside a card
constexpr float kLineH = 22;           // a line of body text
constexpr float kBlockGap = 6;         // between bullets
constexpr float kCardGap = 14;
constexpr float kGroundShift = -40;    // the kraft starts higher than on the main page
constexpr float kFirstCard = 170;
constexpr float kDefaultW = 700, kDefaultH = 860, kMinW = 440, kMinH = 320;

// ---- Markdown -----------------------------------------------------------------------------------

enum SpanStyle { SNormal, SBold, SCode };
struct Span {
    std::wstring t;
    int style = SNormal;
};
struct Block {
    int level = -1;   // -1: a paragraph; 0, 1, ...: a bullet at that depth
    std::vector<Span> spans;
};
enum Tag { TagThis = 1, TagNew = 2, TagAvailable = 4 };
struct Section {
    std::wstring title;     // the heading ("0.6.2")
    std::wstring version;   // its first word
    int tags = 0;
    std::vector<Block> blocks;
};

std::vector<Span> Inline(const std::wstring &s)
{
    std::vector<Span> out;
    std::wstring cur;
    bool bold = false;
    auto flush = [&](int style) {
        if (cur.empty()) return;
        if (!out.empty() && out.back().style == style) out.back().t += cur;
        else out.push_back({ cur, style });
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t ch = s[i];
        if (ch == L'\\' && i + 1 < s.size()) {
            cur += s[++i];
            continue;
        }
        if (ch == L'`') {
            const size_t e = s.find(L'`', i + 1);
            if (e != std::wstring::npos) {
                flush(bold ? SBold : SNormal);
                cur = s.substr(i + 1, e - i - 1);
                flush(SCode);
                i = e;
                continue;
            }
        }
        if (ch == L'*' && i + 1 < s.size() && s[i + 1] == L'*') {
            flush(bold ? SBold : SNormal);
            bold = !bold;
            ++i;
            continue;
        }
        if (ch == L'[') {   // [text](link): the text
            const size_t mid = s.find(L"](", i + 1);
            const size_t end = mid == std::wstring::npos ? mid : s.find(L')', mid + 2);
            if (end != std::wstring::npos) {
                cur += s.substr(i + 1, mid - i - 1);
                i = end;
                continue;
            }
        }
        cur += ch;
    }
    flush(bold ? SBold : SNormal);
    return out;
}

std::vector<std::wstring> Lines(const std::wstring &text)
{
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t e = text.find(L'\n', a);
        if (e == std::wstring::npos) e = text.size();
        std::wstring l = text.substr(a, e - a);
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ' || l.back() == L'\t')) l.pop_back();
        out.push_back(l);
        a = e + 1;
    }
    return out;
}

std::vector<Block> Blocks(const std::vector<std::wstring> &lines)
{
    std::vector<Block> out;
    std::wstring text;
    int level = -2;   // -2: nothing open
    auto flush = [&] {
        if (level != -2 && !text.empty()) out.push_back({ level, Inline(text) });
        text.clear();
        level = -2;
    };
    for (const std::wstring &l : lines) {
        size_t ind = 0;
        while (ind < l.size() && (l[ind] == L' ' || l[ind] == L'\t')) ind += 1;
        const std::wstring t = l.substr(ind);
        if (t.empty()) {
            flush();
            continue;
        }
        if (t.size() > 1 && (t[0] == L'-' || t[0] == L'*') && t[1] == L' ') {
            flush();
            level = std::min(3, (int)ind / 2);
            text = t.substr(2);
            continue;
        }
        if (t[0] == L'#') {   // a heading inside a version: a bold line
            flush();
            size_t k = 0;
            while (k < t.size() && (t[k] == L'#' || t[k] == L' ')) ++k;
            out.push_back({ -1, { { t.substr(k), SBold } } });
            continue;
        }
        if (level == -2) {
            level = -1;
            text = t;
        } else {
            text += L" " + t;   // a wrapped line goes on with its paragraph or bullet
        }
    }
    flush();
    return out;
}

std::wstring FirstWord(const std::wstring &s)
{
    const size_t e = s.find_first_of(L" \t");
    return e == std::wstring::npos ? s : s.substr(0, e);
}

std::vector<Section> Sections(const std::wstring &md)
{
    std::vector<Section> out;
    std::vector<std::wstring> body;
    for (const std::wstring &l : Lines(md)) {
        if (l.compare(0, 3, L"## ") == 0) {
            if (!out.empty()) out.back().blocks = Blocks(body);
            body.clear();
            Section s;
            s.title = l.substr(3);
            while (!s.title.empty() && s.title[0] == L' ') s.title.erase(0, 1);
            s.version = FirstWord(s.title);
            out.push_back(s);
        } else if (!out.empty()) {
            body.push_back(l);   // (before the first version: the file's title and introduction)
        }
    }
    if (!out.empty()) out.back().blocks = Blocks(body);
    return out;
}

std::vector<Section> Build(const ChangeLogInfo &info)
{
    std::vector<Section> secs = Sections(ChangeLogText());
    for (Section &s : secs) {
        if (!info.current.empty() && CompareVersions(s.version, info.current) == 0) s.tags |= TagThis;
        if (!info.updatedFrom.empty() && CompareVersions(s.version, info.updatedFrom) > 0 &&
            (info.current.empty() || CompareVersions(s.version, info.current) <= 0))
            s.tags |= TagNew;
    }
    if (!info.updVersion.empty() && (info.current.empty() || CompareVersions(info.updVersion, info.current) > 0) &&
        (secs.empty() || CompareVersions(info.updVersion, secs[0].version) > 0)) {
        Section s;
        s.title = s.version = info.updVersion;
        s.tags = TagAvailable;
        s.blocks = Blocks(Lines(info.updNotes));
        if (s.blocks.empty()) s.blocks.push_back({ -1, { { L"No notes were published with it.", SNormal } } });
        secs.insert(secs.begin(), s);
    }
    return secs;
}

// ---- layout -------------------------------------------------------------------------------------

struct Op {
    enum Kind { Card, Text, Dot, Ring, Pill } kind = Text;
    RectF r;                  // Card, Pill; Dot and Ring: centre (x, y) and radius (w)
    std::wstring t;           // Card: its title
    ui::FontId f = ui::F_BODY;
    COLORREF c = col::ink;    // Text; Pill: its fill
    float x = 0, base = 0, w = 0;   // Text
};
struct Layout {
    float width = -1;
    float height = 0;
    std::vector<Op> ops;
};

ui::FontId FontOf(int style) { return style == SBold ? ui::F_BODYB : style == SCode ? ui::F_CODE : ui::F_BODY; }

// One block's text, wrapped to maxW from x0; y: the top of its first line, then of the line after it.
void LayoutText(const Block &b, float x0, float maxW, std::vector<Op> &ops, float &y)
{
    struct Atom {
        std::wstring t;
        int style;
        bool space;   // a space before it
    };
    std::vector<Atom> atoms;
    bool sp = false;
    for (const Span &s : b.spans) {
        std::wstring w;
        bool wsp = false;
        for (wchar_t ch : s.t) {
            if (ch == L' ') {
                if (!w.empty()) atoms.push_back({ w, s.style, wsp });
                w.clear();
                sp = true;
            } else {
                if (w.empty()) {
                    wsp = sp;
                    sp = false;
                }
                w += ch;
            }
        }
        if (!w.empty()) atoms.push_back({ w, s.style, wsp });
    }
    struct Piece {
        std::wstring t;
        int style;
        float x, w;
    };
    std::vector<Piece> line;
    const float spaceW = ui::Measure(ui::F_BODY, L" ");
    auto emit = [&] {
        const float base = ui::Mid(ui::F_BODY, y + kLineH / 2);
        for (const Piece &p : line) {
            Op o;
            o.kind = Op::Text;
            o.t = p.t;
            o.f = FontOf(p.style);
            o.x = x0 + p.x;
            o.base = base;
            o.w = p.w;
            ops.push_back(o);
        }
        line.clear();
        y += kLineH;
    };
    size_t i = 0;
    while (i < atoms.size()) {
        size_t j = i + 1;   // the atoms up to the next space stay together
        while (j < atoms.size() && !atoms[j].space) ++j;
        float gw = 0;
        for (size_t k = i; k < j; ++k) gw += ui::Measure(FontOf(atoms[k].style), atoms[k].t);
        const float cur = line.empty() ? 0 : line.back().x + line.back().w;
        bool lead = !line.empty() && atoms[i].space;
        if (!line.empty() && cur + (lead ? spaceW : 0) + gw > maxW) {
            emit();
            lead = false;
        }
        for (size_t k = i; k < j; ++k) {
            const Atom &a = atoms[k];
            const bool s = k == i && lead;
            if (!line.empty() && line.back().style == a.style) {
                Piece &p = line.back();
                p.t += (s ? L" " : L"") + a.t;
                p.w = ui::Measure(FontOf(p.style), p.t);
            } else {
                const float x = line.empty() ? 0 : line.back().x + line.back().w + (s ? spaceW : 0);
                line.push_back({ a.t, a.style, x, ui::Measure(FontOf(a.style), a.t) });
            }
        }
        i = j;
    }
    if (!line.empty()) emit();
}

const wchar_t *TagLabel(int tag) { return tag == TagThis ? L"this version" : tag == TagNew ? L"new" : L"available"; }

Layout DoLayout(const std::vector<Section> &secs, float width)
{
    Layout L;
    L.width = width;
    const float cw = std::max(200.0f, width - 2 * kMargin);
    float y = kFirstCard;
    for (const Section &s : secs) {
        const size_t card = L.ops.size();
        Op c;
        c.kind = Op::Card;
        c.t = s.title;
        L.ops.push_back(c);
        float px = kMargin + 20 + ui::Measure(ui::F_H2, s.title) + 10;
        for (int tag : { TagAvailable, TagThis, TagNew }) {
            if (!(s.tags & tag)) continue;
            Op p;
            p.kind = Op::Pill;
            p.t = TagLabel(tag);
            p.r = { px, y + 19.5f, ui::Measure(ui::F_TINY, p.t) + 14, 16 };
            p.c = tag == TagNew ? col::orange : tag == TagThis ? col::blue : col::paper;
            L.ops.push_back(p);
            px = p.r.r() + 6;
        }
        float ty = y + 50;
        bool first = true;
        for (const Block &b : s.blocks) {
            if (!first) ty += kBlockGap;
            first = false;
            float x0 = kMargin + kPad;
            if (b.level >= 0) {
                x0 += 16 + b.level * 20;
                Op d;
                d.kind = b.level == 0 ? Op::Dot : Op::Ring;
                d.r = { x0 - 10, ty + kLineH / 2 + 0.5f, b.level == 0 ? 2.8f : 2.4f, 0 };
                L.ops.push_back(d);
            }
            LayoutText(b, x0, kMargin + cw - kPad - x0, L.ops, ty);
        }
        const float h = std::max(70.0f, ty - y + 16);
        L.ops[card].r = { kMargin, y, cw, h };
        y += h + kCardGap;
    }
    L.height = y - kCardGap + 28;
    return L;
}

// ---- the window ---------------------------------------------------------------------------------

struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    void Ensure(int W, int H)
    {
        if (dc && w == W && h == H) return;
        Free();
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old = SelectObject(dc, bmp);
        ReleaseDC(nullptr, screen);
        w = W;
        h = H;
    }
    void Free()
    {
        if (dc) {
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
        }
        dc = nullptr;
        bmp = nullptr;
        w = h = 0;
    }
};

struct State {
    HWND wnd = nullptr;
    UINT dpi = 96;
    ChangeLogInfo info;
    std::vector<Section> secs;
    Layout lay;
    float scroll = 0;
    bool dragging = false, thumbHot = false;
    float grab = 0;   // where on the thumb it was grabbed
    Surface back;
} g;

float Scale() { return g.dpi / 96.0f; }
float L(int px) { return px / Scale(); }

RectF View()
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    return { 0, 0, L(rc.right), L(rc.bottom) };
}

void EnsureLayout()
{
    const float w = View().w;
    if (fabsf(w - g.lay.width) > 0.25f) g.lay = DoLayout(g.secs, w);
}

float MaxScroll()
{
    EnsureLayout();
    return std::max(0.0f, g.lay.height - View().h);
}

void SetScroll(float v)
{
    v = std::max(0.0f, std::min(MaxScroll(), v));
    if (v != g.scroll) {
        g.scroll = v;
        InvalidateRect(g.wnd, nullptr, FALSE);
    }
}

// The scroll thumb, in window coordinates (h = 0: nothing to scroll).
RectF Thumb()
{
    const RectF v = View();
    const float ms = MaxScroll();
    if (ms <= 0) return { v.w - 9, 0, 5, 0 };
    const float th = v.h - 16, h = std::max(30.0f, th * v.h / g.lay.height);
    return { v.w - 9, 8 + (th - h) * g.scroll / ms, 5, h };
}

std::wstring Subtitle()
{
    std::wstring s = L"What changed in each version of vrupscale, newest first.";
    if (!g.info.updatedFrom.empty() && !g.info.current.empty())
        s += L" Updated from " + g.info.updatedFrom + L" to " + g.info.current + L": what you didn't have is marked new.";
    else if (!g.info.current.empty())
        s += L" You have " + g.info.current + L".";
    if (!g.info.updVersion.empty() && !g.secs.empty() && (g.secs[0].tags & TagAvailable))
        s += L" " + g.info.updVersion + L" is available (Update, in the main window).";
    return s;
}

void Paint(HDC target)
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    EnsureLayout();
    g.scroll = std::max(0.0f, std::min(MaxScroll(), g.scroll));
    g.back.Ensure(rc.right, rc.bottom);
    {
        ui::Canvas c(g.back.dc, Scale(), g.scroll);
        const RectF v = View();
        const float top = g.scroll, bottom = g.scroll + v.h;
        ui::DrawGround(c, v.w, std::max(g.lay.height, bottom), kGroundShift);
        if (top < kFirstCard) {
            c.Text(ui::F_DISPLAY, L"Change log", 34, 72, col::ink);
            c.Wrap(ui::F_BODY, Subtitle(), { 36, 86, v.w - 72, 44 }, 20, col::ink, 2);
        }
        for (const Op &o : g.lay.ops) {
            switch (o.kind) {
            case Op::Card:
                if (o.r.b() + 8 >= top && o.r.y <= bottom) ui::DrawCard(c, o.r, o.t.c_str());
                break;
            case Op::Pill:
                if (o.r.b() >= top && o.r.y <= bottom) {
                    c.FillRound(o.r, 8, o.c);
                    c.StrokeRound(o.r.Inflate(-0.75f), 7.25f, col::ink, 1.5f);
                    c.Text(ui::F_TINY, o.t, o.r.cx(), ui::Mid(ui::F_TINY, o.r.cy()), col::ink, 1);
                }
                break;
            case Op::Dot:
            case Op::Ring:
                if (o.r.y + 10 >= top && o.r.y - 10 <= bottom) {
                    if (o.kind == Op::Dot) c.FillCircle(o.r.x, o.r.y, o.r.w, col::ink);
                    else c.StrokeCircle(o.r.x, o.r.y, o.r.w, col::ink, 1.4f);
                }
                break;
            case Op::Text:
                if (o.base + 8 >= top && o.base - kLineH <= bottom) c.Text(o.f, o.t, o.x, o.base, col::ink);
                break;
            }
        }
        const RectF t = Thumb();
        if (t.h > 0)
            c.FillRound({ t.x, t.y + g.scroll, t.w, t.h }, 2.5f, col::ink, g.dragging ? 170 : g.thumbHot ? 140 : 110);
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, g.back.dc, 0, 0, SRCCOPY);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        EnsureLayout();
        SetScroll(g.scroll);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        RECT r = { 0, 0, (LONG)lroundf(kMinW * Scale()), (LONG)lroundf(kMinH * Scale()) };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        static int acc = 0;
        acc += GET_WHEEL_DELTA_WPARAM(wp);
        const int notches = acc / WHEEL_DELTA;
        acc -= notches * WHEEL_DELTA;
        if (notches) SetScroll(g.scroll - notches * 66.0f);
        return 0;
    }
    case WM_KEYDOWN: {
        const float page = std::max(40.0f, View().h - 60);
        switch (wp) {
        case VK_UP: SetScroll(g.scroll - 44); break;
        case VK_DOWN: SetScroll(g.scroll + 44); break;
        case VK_PRIOR: SetScroll(g.scroll - page); break;
        case VK_NEXT: case VK_SPACE: SetScroll(g.scroll + (GetKeyState(VK_SHIFT) < 0 && wp == VK_SPACE ? -page : page)); break;
        case VK_HOME: SetScroll(0); break;
        case VK_END: SetScroll(MaxScroll()); break;
        case VK_ESCAPE: DestroyWindow(h); break;
        default: return DefWindowProcW(h, msg, wp, lp);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float x = L(GET_X_LPARAM(lp)), y = L(GET_Y_LPARAM(lp));
        const RectF t = Thumb();
        if (t.h > 0 && x >= t.x - 8) {
            if (y >= t.y && y < t.b()) {
                g.dragging = true;
                g.grab = y - t.y;
                SetCapture(h);
            } else {
                SetScroll(g.scroll + (y < t.y ? -1 : 1) * std::max(40.0f, View().h - 60));
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = L(GET_X_LPARAM(lp)), y = L(GET_Y_LPARAM(lp));
        if (g.dragging) {
            const RectF v = View(), t = Thumb();
            const float range = v.h - 16 - t.h;
            if (range > 0) SetScroll((y - g.grab - 8) / range * MaxScroll());
            return 0;
        }
        const RectF t = Thumb();
        const bool hot = t.h > 0 && x >= t.x - 8 && y >= t.y && y < t.b();
        if (hot != g.thumbHot) {
            g.thumbHot = hot;
            if (hot) {
                TRACKMOUSEEVENT tm = { sizeof(tm), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tm);
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.thumbHot) {
            g.thumbHot = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g.dragging) {
            g.dragging = false;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        g.dragging = false;
        return 0;
    case WM_DPICHANGED: {   // (drawn with the owner's fonts; the window takes the size Windows suggests)
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        g.back.Free();
        g.wnd = nullptr;
        g.dragging = false;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

std::wstring ChangeLogText()
{
    HMODULE m = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(kResChangeLog), (LPCWSTR)RT_RCDATA);
    HGLOBAL hg = r ? LoadResource(m, r) : nullptr;
    const char *p = hg ? (const char *)LockResource(hg) : nullptr;
    int n = r ? (int)SizeofResource(m, r) : 0;
    if (!p || n <= 0) return L"";
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3, n -= 3;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, p, n, nullptr, 0);
    std::wstring w(wn > 0 ? wn : 0, L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, p, n, &w[0], wn);
    return w;
}

bool ChangeLogOpen() { return g.wnd != nullptr; }

void ChangeLogDpiChanged(UINT dpi)
{
    g.dpi = dpi;
    if (!g.wnd) return;
    g.lay = Layout();
    InvalidateRect(g.wnd, nullptr, FALSE);
}

void ShowChangeLog(HWND owner, HINSTANCE inst, UINT dpi, const ChangeLogInfo &info, bool bringForward)
{
    g.dpi = dpi;
    g.info = info;
    g.secs = Build(info);
    g.lay = Layout();
    if (g.wnd) {
        InvalidateRect(g.wnd, nullptr, FALSE);
        if (bringForward) {
            if (IsIconic(g.wnd)) ShowWindow(g.wnd, SW_RESTORE);
            SetForegroundWindow(g.wnd);
        }
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"vrupscale.changelog";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
        registered = RegisterClassExW(&wc) != 0;
    }
    // over the main window, on its screen
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    RECT r = { 0, 0, (LONG)lroundf(kDefaultW * Scale()), (LONG)lroundf(kDefaultH * Scale()) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int ww = std::min((int)(r.right - r.left), (int)(wa.right - wa.left));
    const int wh = std::min((int)(r.bottom - r.top), (int)(wa.bottom - wa.top));
    RECT o;
    GetWindowRect(owner, &o);
    int x = (o.left + o.right) / 2 - ww / 2, y = (o.top + o.bottom) / 2 - wh / 2;
    x = std::max((int)wa.left, std::min(x, (int)wa.right - ww));
    y = std::max((int)wa.top, std::min(y, (int)wa.bottom - wh));
    g.scroll = 0;
    g.wnd = CreateWindowExW(0, L"vrupscale.changelog", L"vrupscale change log", WS_OVERLAPPEDWINDOW, x, y, ww, wh, owner, nullptr,
                            inst, nullptr);
    if (!g.wnd) return;
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {   // Windows 11: the title bar in the page's grey
        typedef HRESULT(WINAPI * PFN)(HWND, DWORD, LPCVOID, DWORD);
        if (PFN set = (PFN)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            const COLORREF caption = col::sky, text = col::ink;
            set(g.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
            set(g.wnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
        }
    }
    ShowWindow(g.wnd, SW_SHOWNORMAL);
    UpdateWindow(g.wnd);
}

int ChangeLogSelfTest(const std::wstring &current, std::wstring &report)
{
    int fails = 0;
    auto check = [&](bool ok, const std::wstring &what) {
        report += (ok ? L"  ok   " : L"  FAIL ") + what + L"\n";
        if (!ok) ++fails;
    };
    // Markdown
    {
        const std::vector<Span> s = Inline(L"a **b c** `d e` [f](https://x.y/z) g\\*");
        std::wstring all;
        for (auto &p : s) all += p.t;
        check(all == L"a b c d e f g*" && s.size() == 5 && s[1].t == L"b c" && s[1].style == SBold && s[3].t == L"d e" &&
                  s[3].style == SCode && s[4].style == SNormal,
              L"inline Markdown: bold, code, a link's text, an escape (" + all + L")");
        const std::vector<Block> b = Blocks(Lines(L"- one\n  - two\n  more\n\npara\nline\n### Sub"));
        check(b.size() == 4 && b[0].level == 0 && b[1].level == 1 && b[1].spans[0].t == L"two more" && b[2].level == -1 &&
                  b[2].spans[0].t == L"para line" && b[3].spans[0].style == SBold,
              Fmt(L"blocks: bullets, a nested bullet with its wrapped line, a paragraph, a heading (%d blocks)", (int)b.size()));
    }
    // the embedded change log
    const std::wstring text = ChangeLogText();
    check(text.size() > 1000, Fmt(L"CHANGELOG.md is embedded (%d characters)", (int)text.size()));
    std::vector<Section> secs = Sections(text);
    bool ordered = true, filled = true;
    for (size_t i = 0; i < secs.size(); ++i) {
        if (secs[i].blocks.empty()) filled = false;
        if (i && CompareVersions(secs[i - 1].version, secs[i].version) <= 0) ordered = false;
    }
    check(secs.size() >= 15 && filled && ordered,
          Fmt(L"%d versions, newest first, each with notes (%s ... %s)", (int)secs.size(), secs.empty() ? L"" : secs[0].version.c_str(),
              secs.empty() ? L"" : secs.back().version.c_str()));
    check(!secs.empty() && (current == L"dev" || secs[0].version == current),
          L"the newest entry is this version (" + current + L")");
    // marks
    if (secs.size() >= 3) {
        ChangeLogInfo info;
        info.current = secs[0].version;
        info.updatedFrom = secs[2].version;
        std::vector<Section> m = Build(info);
        check(m.size() == secs.size() && m[0].tags == (TagThis | TagNew) && m[1].tags == TagNew && m[2].tags == 0,
              L"after an update from " + info.updatedFrom + L": the versions since are marked new");
        info.updatedFrom.clear();
        info.updVersion = L"99.0.0";
        info.updNotes = L"- **Big.** news\n- more";
        m = Build(info);
        check(m.size() == secs.size() + 1 && m[0].version == L"99.0.0" && m[0].tags == TagAvailable && m[0].blocks.size() == 2 &&
                  m[1].tags == TagThis,
              L"an available update's notes come first");
    }
    // layout: no text past its card, at the default and the smallest width
    for (float w : { kDefaultW, kMinW }) {
        const Layout lay = DoLayout(secs, w);
        int texts = 0, over = 0, cards = 0;
        float maxR = 0;
        std::wstring widest;
        const float limit = w - kMargin - kPad + 0.5f;
        for (const Op &o : lay.ops) {
            if (o.kind == Op::Card) ++cards;
            if (o.kind != Op::Text) continue;
            ++texts;
            if (o.x + o.w > maxR) widest = o.t;
            maxR = std::max(maxR, o.x + o.w);
            if (o.x + o.w > limit) ++over;
        }
        check(cards == (int)secs.size() && texts > 40 && over == 0 && lay.height > 1000,
              Fmt(L"layout at %.0f px: %d cards, %d text runs, %.0f px tall, rightmost text at %.1f (limit %.1f: \"%s\")", w, cards,
                  texts, lay.height, maxR, limit, widest.c_str()));
    }
    return fails;
}

} // namespace app
