// The app's look, after Valve's Steam hardware pages: a grey sky, a kraft-cardboard ground with
// characters standing on its edge, heavy rounded headlines (Rubik, embedded), ink text, and Steam
// blue for whatever is on. Shapes are drawn with GDI+, text with GDI (ClearType) in kerned runs.
//
// Everything here works in logical pixels (96 per inch); a Canvas maps them to the device.
#pragma once

#include "core.h"

#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>

namespace ui {

namespace col {
constexpr COLORREF sky = RGB(224, 224, 224);      // the page's grey
constexpr COLORREF ink = RGB(33, 37, 44);         // headlines and text
constexpr COLORREF inkSoft = RGB(80, 85, 94);     // secondary text
constexpr COLORREF inkFaint = RGB(128, 132, 140); // hints on grey, disabled
constexpr COLORREF kraft = RGB(217, 189, 154);    // cardboard
constexpr COLORREF kraftShadow = RGB(176, 145, 108);
constexpr COLORREF card = RGB(232, 232, 232);
constexpr COLORREF well = RGB(218, 218, 219);     // list and log backgrounds inside cards
constexpr COLORREF control = RGB(204, 206, 209);  // switch and slider tracks, segment background
constexpr COLORREF blue = RGB(29, 173, 255);      // Steam blue
constexpr COLORREF blueDeep = RGB(0, 116, 196);   // focus rings, links
constexpr COLORREF orange = RGB(232, 128, 30);
constexpr COLORREF red = RGB(214, 69, 65);
constexpr COLORREF paper = RGB(246, 246, 246);    // switch knobs, slider thumbs, text fields
constexpr COLORREF dark = RGB(38, 38, 38);        // illustration
constexpr COLORREF body = RGB(56, 58, 57);
constexpr COLORREF mid = RGB(79, 79, 79);
} // namespace col

COLORREF Mix(COLORREF a, COLORREF b, float t);   // a .. b
Gdiplus::Color C(COLORREF c, BYTE alpha = 255);

enum FontId { F_DISPLAY, F_BRAND, F_H2, F_BODY, F_LABEL, F_VALUE, F_SMALL, F_BUTTON, F_TINY, F_MONO, F_BODYB, F_CODE, F_COUNT };

void Startup(HINSTANCE inst);   // GDI+, the embedded fonts, the kraft texture
void Shutdown();
void MakeFonts(UINT dpi);
HFONT Font(FontId f);
float FontPx(FontId f);
float Mid(FontId f, float cy);   // baseline that centres a line of text on cy

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    RectF Inflate(float d) const { return { x - d, y - d, w + 2 * d, h + 2 * d }; }
};

float Measure(FontId f, const std::wstring &s);
// Greedy word wrap (explicit \n too); lines are returned without trailing spaces.
std::vector<std::wstring> WrapLines(FontId f, const std::wstring &s, float width);

class Canvas {
public:
    Canvas(HDC dc, float scale, float offY);   // offY: the logical y shown at the top of dc
    ~Canvas();
    Gdiplus::Graphics &G() { return *g_; }
    float Scale() const { return scale_; }

    void Fill(const RectF &r, COLORREF c, BYTE a = 255);
    void FillRound(const RectF &r, float rad, COLORREF c, BYTE a = 255);
    void StrokeRound(const RectF &r, float rad, COLORREF c, float width, BYTE a = 255);
    void FillCircle(float cx, float cy, float rad, COLORREF c, BYTE a = 255);
    void StrokeCircle(float cx, float cy, float rad, COLORREF c, float width);
    void Line(float x0, float y0, float x1, float y1, COLORREF c, float width, BYTE a = 255);

    // One line of text on a baseline: kerned; align 0 left, 1 centre, 2 right; ellipsis past maxW.
    // Returns the width drawn.
    float Text(FontId f, const std::wstring &s, float x, float base, COLORREF c, int align = 0, float maxW = 0);
    // Wrapped text from the top of box, at most maxLines (the last one ellipsized). Returns lines drawn.
    int Wrap(FontId f, const std::wstring &s, const RectF &box, float lineH, COLORREF c, int maxLines = 99);
    void PathText(FontId f, const std::wstring &s, float x, float base, float w, COLORREF c);   // ...\middle\ elided

private:
    HDC Begin(FontId f, COLORREF c);
    void End();
    HDC dc_;
    HDC hdc_ = nullptr;
    HGDIOBJ oldFont_ = nullptr;
    Gdiplus::Graphics *g_;
    float scale_, offY_;
};

// ---- page -------------------------------------------------------------------------------------

float GroundY(float x, float width);   // the kraft's top edge
void DrawGround(Canvas &c, float width, float height, float shift = 0);   // shift: moves the ground's edge down (< 0: up)
void DrawCard(Canvas &c, const RectF &r, const wchar_t *title);
void DrawWell(Canvas &c, const RectF &r);

enum Mood { MoodSleep, MoodAwake, MoodWork, MoodWarn };
// The characters standing on the ground's edge: a low-resolution frame walking into a headset, a
// sharp one coming out. cx: the headset's centre.
void DrawScene(Canvas &c, float cx, float width, Mood mood);

// ---- widgets ----------------------------------------------------------------------------------

enum class Kind { Button, Toggle, Slider, Segment, Custom };
enum ButtonStyle { BtnPrimary, BtnOutline };

struct Widget {
    int id = 0;
    Kind kind = Kind::Button;
    RectF r;
    std::wstring text;    // label, button text
    std::wstring note;    // toggle: quieter text after the label
    int style = BtnOutline;
    bool enabled = true;
    bool visible = true;  // false: not drawn, not hit, not focused
    bool on = false;      // toggle
    int lo = 0, hi = 100, pos = 0, page = 5;   // slider
    float labelW = 116;   // slider: label column
    std::wstring value;   // slider: value text
    std::vector<std::wstring> items;   // segment
    int sel = 0;
};

struct Interact {
    int hover = 0, hoverPart = -1, pressed = 0, focus = 0, drag = 0;
    bool focusVisible = false;
};

void DrawWidget(Canvas &c, const Widget &w, const Interact &st);
void DrawFocus(Canvas &c, const RectF &r, float rad);
void DrawCheck(Canvas &c, const RectF &box, bool on, bool enabled, bool hot);
RectF SliderTrack(const Widget &w);
int SliderPosAt(const Widget &w, float x);
int SegmentAt(const Widget &w, float x);
RectF SwitchRect(const Widget &w);

} // namespace ui
