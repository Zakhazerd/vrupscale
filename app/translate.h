// Translation of the text the user sees in VR, shown over the original.
//
// While it is on, vrupscale's DLL in SteamVR's compositor hands over a part of each eye's picture
// around where the user looks, a few times a second (control_shared.h: VuCapture). A worker thread
// here reads the left eye's part with Windows' OCR (ocr.h), groups the words into blocks of text
// (trcore.h), finds how far away each block is from where it sits in the right eye's part, and asks
// DeepL (deepl.h, on a thread of its own) for the translation of blocks it has not seen before. Each
// translated block becomes a small SteamVR overlay: a rectangle in the room, placed over the text at
// its distance, in the text's own colours. SteamVR keeps it in place between captures; every capture
// re-reads the text and moves, changes or removes the labels.
//
// Nothing leaves the computer except the texts sent to DeepL with the user's own key, and nothing is
// sent without a key. Settings: [translate] in the settings file (enabled, source, target, deeplKey,
// distance, charsPerMinute; the DLL reads captureHz and captureSize there).
#pragma once

#include <stdint.h>
#include <string>

namespace app {

struct TranslateStatus {
    enum State { Off, NoSteamVR, NoPictures, NoOcr, NoKey, Running, Trouble };
    State state = Off;
    std::wstring line;        // for the page: what it is doing, or what it needs
    std::wstring ocrCommand;  // NoOcr: the PowerShell line (as administrator) that adds Windows' OCR for the language
    std::wstring languages;   // "Japanese to English"
    bool hasKey = false;
    int showing = 0;          // translations in view
    uint64_t sent = 0;        // characters sent to DeepL in this run
};

void TranslateStart();              // its threads (once; they idle while it is off)
void TranslateStop();               // before the app exits: its labels go
void TranslateSet(bool on);         // the switch
void TranslateReload();             // [translate] in the settings file changed
TranslateStatus TranslateGetStatus();
// Keeps the DeepL key in the settings file. False if the text does not look like a key.
bool TranslateSetKey(const std::wstring &key);
bool TranslateWantsLink();          // it is on: the app keeps its connection to SteamVR open

int TranslateSelfTest(std::wstring &report);   // with trcore's and deepl's

} // namespace app
