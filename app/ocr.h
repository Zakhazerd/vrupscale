// Text recognition: Windows' own OCR (Windows.Media.Ocr, part of Windows 10 and 11; it runs on the
// CPU and offline). One language at a time, and only languages whose OCR pack Windows has: English
// comes with an English Windows, others are added as an optional feature (an admin PowerShell:
// Add-WindowsCapability -Online -Name "Language.OCR~~~ja-JP~0.0.1.0").
//
// MinGW-w64 has no header for it, so its WinRT interfaces are called by vtable slot (ocr.cpp), as
// vroverlay.cpp does with OpenVR. An Ocr belongs to the thread that opened it.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace app {

struct OcrWord {
    std::wstring text;
    float x = 0, y = 0, w = 0, h = 0;   // pixels, in the picture turned upright (OcrPage::angle)
};
struct OcrLine {
    std::vector<OcrWord> words;
};
struct OcrPage {
    std::vector<OcrLine> lines;
    float angle = 0;   // degrees clockwise the text is turned in the picture, about its centre
};

class Ocr {
public:
    Ocr() = default;
    ~Ocr();
    Ocr(const Ocr &) = delete;
    Ocr &operator=(const Ocr &) = delete;
    // lang: a BCP-47 tag ("ja", "en-US"). False with missing = true: Windows has no OCR for it.
    bool Open(const std::wstring &lang, std::wstring &error, bool *missing = nullptr);
    bool IsOpen() const { return engine_ != nullptr; }
    void Close();
    // gray: 8 bits a pixel, rows from the top, `stride` bytes apart. At most MaxDimension() a side.
    bool Recognize(const uint8_t *gray, int w, int h, int stride, OcrPage &out, std::wstring &error);
    int MaxDimension() const { return maxDim_; }

private:
    void *engine_ = nullptr;      // IOcrEngine
    void *bitmaps_ = nullptr;     // ISoftwareBitmapFactory
    int maxDim_ = 0;
    bool inited_ = false;         // this thread's RoInitialize
};

// The PowerShell line (run as administrator) that adds Windows' OCR for a language.
std::wstring OcrInstallCommand(const std::wstring &lang);

} // namespace app
