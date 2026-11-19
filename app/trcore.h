// The parts of the translation feature (translate.h) that are plain computation: recognised words
// into blocks of text worth translating, where a block is in space from where it sits in each eye's
// picture, the colours to draw it in, and the picture of a translation. No SteamVR, no Direct3D, no
// network here, so TranslateCoreSelfTest covers all of it.
#pragma once

#include "ocr.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace app {

// ---- text ---------------------------------------------------------------------------------------

// A block of text as it was read: one or more lines that belong together.
struct TextBlock {
    std::wstring text;      // what goes to the translator (lines of one sentence joined, others on lines of their own)
    float cx = 0, cy = 0;   // centre, in the picture's pixels
    float w = 0, h = 0;     // size along and across the text's direction
    float angle = 0;        // degrees clockwise the text is turned
    float lineH = 0;        // one line's height
    int lines = 0;
};
// Words into lines and lines into blocks. lang: the language read ("ja", "en-US"); it decides whether
// words are joined with spaces.
std::vector<TextBlock> GroupText(const OcrPage &page, int picW, int picH, const std::wstring &lang);
// Is this text of that language, and enough of it to be worth a translation? (The recogniser also
// "reads" a few characters into railings, tiles and fur.)
bool WorthTranslating(const std::wstring &text, const std::wstring &lang);
// How alike two readings of a text are: 1 the same, 0 nothing in common (the recogniser's reading of
// the same sign changes by a character now and then).
float TextSimilarity(const std::wstring &a, const std::wstring &b);
std::wstring LanguageName(const std::wstring &tag);   // "ja" -> "Japanese" (the tag itself if not known)

// ---- geometry -----------------------------------------------------------------------------------

struct V3 {
    float x = 0, y = 0, z = 0;
};
struct M34 {
    float m[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };   // OpenVR's HmdMatrix34_t: rotation and position
};
V3 Apply(const M34 &m, const V3 &p);       // a point
V3 Rotate(const M34 &m, const V3 &d);      // a direction
M34 Mul(const M34 &a, const M34 &b);       // a after b
V3 Sub(const V3 &a, const V3 &b);
V3 Add(const V3 &a, const V3 &b);
V3 Scale(const V3 &a, float s);
float Dot(const V3 &a, const V3 &b);
float Length(const V3 &a);
V3 Normalized(const V3 &a);

// One eye's picture: its projection as SteamVR reports it (IVRSystem::GetProjectionRaw: left, right,
// top, bottom, where "bottom" is the tangent at the picture's top edge and "top" the one at its
// bottom edge, counted upwards), where the eye is in the head, and the picture's size in pixels.
struct EyeView {
    float l = -1, r = 1, t = -1, b = 1;
    M34 toHead;
    float picW = 1, picH = 1;
};
// A pixel of the eye's picture (from its top left) as a direction in the head's space (x right, y up,
// -z forward; not normalised: its length along the eye's forward axis is 1), and back.
V3 PixelRay(const EyeView &e, float px, float py);
bool ProjectToPixel(const EyeView &e, const V3 &headPoint, float *px, float *py);
V3 EyePosition(const EyeView &e);

// A grey picture: part of an eye's picture, starting at (x0, y0) of it.
struct Gray {
    int w = 0, h = 0;
    int x0 = 0, y0 = 0;
    std::vector<uint8_t> px;
    uint8_t at(int x, int y) const { return px[(size_t)y * w + x]; }
};
// How far away the patch (cx, cy, w, h; pixels of the left eye's picture) is, from where the same
// patch shows in the right eye's picture: the distance along the left eye's forward axis in metres.
// False if it can't be told (no clear match, or the patch is not in the right picture).
bool StereoDepth(const Gray &left, const EyeView &le, const Gray &right, const EyeView &re, float cx, float cy, float w, float h,
                 float *depth, float *score = nullptr);

// Where a label goes: a flat rectangle in the head's space.
struct Placement {
    V3 centre, right, up;   // unit axes; it faces along right x up
    float width = 0, height = 0;   // metres
    float depth = 0;
};
// The rectangle that covers a block of the left eye's picture (centre, size, angle as TextBlock) at
// that depth, parallel to the picture.
Placement PlaceBlock(const EyeView &le, float cx, float cy, float w, float h, float angleDeg, float depth);

// ---- colours and the label's picture ------------------------------------------------------------

// The colours a text is written in: its background and its ink, from the picture (32-bit BGRA rows,
// `stride` pixels apart) inside the box.
void TextColours(const uint32_t *bgra, int picW, int picH, int stride, float cx, float cy, float w, float h, uint32_t *background,
                 uint32_t *ink);

struct LabelPicture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // BGRA, opaque
    float grow = 1;             // how much larger than the text's own box it had to be, per axis
    float fontPx = 0;
};
// The translation drawn to cover a box of boxW x boxH pixels (lineH: a line's height there): on the
// background colour, wrapped, as large as fits up to the original's size. A translation that does
// not fit at a readable size gets a larger picture (grow). Needs GDI+ started (ui::Startup).
LabelPicture DrawLabel(const std::wstring &text, float boxW, float boxH, float lineH, uint32_t background, uint32_t ink);

int TranslateCoreSelfTest(std::wstring &report);

} // namespace app
