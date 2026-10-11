// Shared-memory control channel between the DLL (inside vrcompositor.exe) and the control app
// (vrupscale.exe). Plain C layout, shared verbatim by both sides; bump kVuLayout on any change.
//
// The DLL creates the mapping and publishes its status about four times a second; the app writes
// settings and commands. Each direction is a seqlock: the writer makes the sequence odd, writes,
// then makes it even again; a reader retries until it sees the same even value before and after.
#pragma once

#include <stdint.h>

#define VU_CONTROL_NAME L"Local\\vrupscale.control.v1"

enum : uint32_t { kVuMagic = 0x31505556u /* "VUP1" */, kVuLayout = 4, kVuMaxTex = 16, kVuMaxStreams = 6,
                  kVuMaxExclude = 16, kVuEvents = 8 };

enum : int32_t { kVuModeTemporal = 0, kVuModeFsr = 1, kVuModeBilinear = 2 };
// kVuCmdResolutionChanging (since 0.5.0; an older DLL ignores it): the app is changing the game's
// render resolution (SteamVR's resolution) right now, so the DLL looks for the new picture size
// quickly for a few seconds. Sent as the change starts and again when SteamVR took it.
enum : uint32_t { kVuCmdNone = 0, kVuCmdResetHistory = 1, kVuCmdReloadIni = 2, kVuCmdCalibrateGaze = 3,
                  kVuCmdResolutionChanging = 4 };
// Gaze calibration (VuGazeInfo.calState)
enum : int32_t { kVuCalIdle = 0, kVuCalRunning = 1, kVuCalDone = 2, kVuCalFailed = 3 };

// Scaling presets, after DLSS's: the output size per axis relative to the size the game renders.
// Native (1x) processes the eye textures at their own size (temporal anti-aliasing, or FSR 1's
// sharpening). The factor itself is what both sides store, so a factor between presets (from a
// hand edit) works too. (Ultra Performance, 3x, was dropped in 0.4.7: SteamVR's per-game slider
// does not go as low as the 11% it needs.)
enum : int32_t { kVuScaleCount = 5, kVuScaleDefault = 1 };
static const float kVuScaleFactor[kVuScaleCount] = { 1.00f, 1.30f, 1.50f, 1.72f, 2.00f };
static const char *const kVuScaleName[kVuScaleCount] = { "Native", "Ultra Quality", "Quality", "Balanced", "Performance" };

// The preset a factor is (factors are kept to 0.01), or -1.
static inline int VuScalePreset(float f)
{
    for (int i = 0; i < kVuScaleCount; ++i) {
        const float d = f - kVuScaleFactor[i];
        if (d > -0.005f && d < 0.005f) return i;
    }
    return -1;
}

// The preset after this factor, wrapping from the last to the first; a factor between presets
// goes to the next larger one.
static inline int VuScaleNext(float f)
{
    const int i = VuScalePreset(f);
    if (i >= 0) return (i + 1) % kVuScaleCount;
    for (int k = 0; k < kVuScaleCount; ++k)
        if (kVuScaleFactor[k] > f) return k;
    return 0;
}

// The preset before this factor (no wrap; -1 below the first); a factor between presets goes to
// the next smaller one.
static inline int VuScalePrev(float f)
{
    const int i = VuScalePreset(f);
    if (i >= 0) return i - 1;
    for (int k = kVuScaleCount - 1; k >= 0; --k)
        if (kVuScaleFactor[k] < f) return k;
    return -1;
}

#pragma pack(push, 4)

typedef struct VuSettings {
    int32_t enabled;
    int32_t mode;                 // kVuMode*
    float upscaleFactor;          // per axis
    int32_t unused0;              // was the fixed output height (removed in 0.4.4)
    float sharpness;              // 0..1 (0 = RCAS off)
    int32_t tint;
    int32_t hotkeys;
    float maxWeight;              // [temporal]
    float localTrust;
    float residualTol;
    float clipGamma;
    int32_t maxStreams;
    int32_t requireShared;        // [match]
    int32_t minWidth, minHeight;
    float minAspect, maxAspect;
    int32_t excludeCount;
    uint32_t exclude[kVuMaxExclude];   // (width << 16) | height
    float temporalArea;           // [temporal] centre part with history, fraction of width and height
    int32_t fastHistory;          // 1 = Catmull-Rom history resampling (cheaper); 0 = Lanczos-3 (since 0.4.4; zero
                                  // from an older app means the default)
    int32_t bilinearEdges;        // outside temporalArea: 1 = bilinear, 0 = Catmull-Rom (since 0.4.6)
    int32_t gazeFollow;           // [eyes] 1 = the temporal area follows the gaze (eye tracking, since 0.4.8)
    int32_t gazeShow;             // 1 = draw a ring where vrupscale thinks you look
    float gazeOffset[2];          // calibration: subtracted from the gaze direction's tangents (x right, y up)
    float eyeProj[8];             // each eye's projection as SteamVR reports it (tangents left, right, top, bottom;
                                  // left eye, then right); all 0 = not known yet
    int32_t reserved[16];
} VuSettings;

typedef struct VuTexInfo {
    uint32_t w, h, slices, format;    // DXGI_FORMAT
    uint32_t outW, outH;              // when upscaled
    int32_t upscaled;                 // 1 = upscaled, 0 = left alone
    uint32_t lastSeenTick;            // GetTickCount() of the last bind the DLL saw
    char reason[72];                  // why it was left alone
    int32_t eye;                      // one of the textures an eye's frames rotate through (the game's swapchain)
    uint32_t contentW, contentH;      // the part the game renders into, when less than the texture (else 0)
    uint32_t contentOutW, contentOutH;   // ... and its upscaled size
} VuTexInfo;

typedef struct VuStreamInfo {
    uint32_t id, w, h, outW, outH, textures;
    float runsPerSec;                 // temporal updates per second (one per new app frame)
} VuStreamInfo;

typedef struct VuGazeInfo {
    int32_t source;                   // 0 none, 1 Bigscreen Beyond (its eye tracking client's VRCFT output)
    int32_t live;                     // valid samples arriving now
    float rate;                       // samples per second
    float x, y;                       // latest gaze direction, tangents (x right, y up), before calibration
    int32_t following;                // the temporal area is following it right now
    int32_t calState;                 // kVuCal*
    float calProgress;                // 0..1 while running
    char calMessage[96];
} VuGazeInfo;

typedef struct VuStatus {
    uint32_t pid;
    uint32_t tick;                    // GetTickCount() when published
    char version[16];
    int32_t hooksInstalled;
    int32_t commandLists;             // the compositor records into deferred contexts
    uint32_t upscalesTotal;
    float upscalesPerSec;
    float gpuMs;                      // average GPU time per batch of upscale work; < 0 = not measured
    float gpuMsPerSec;                // GPU time spent upscaling per second of wall time; < 0 = not measured
    int32_t eyePools;                 // eyes seen (textures drawn into a per-eye viewport)
    int32_t mixedEyes;                // of those, eyes upscaled spatially because their textures are on different histories
    uint32_t appliedSeq;              // last settings sequence applied
    uint32_t texCount;
    VuTexInfo tex[kVuMaxTex];
    uint32_t streamCount;
    VuStreamInfo streams[kVuMaxStreams];
    VuSettings current;               // what the DLL is running with right now
    uint32_t eventCount;              // total events ever (the last kVuEvents are in the ring)
    char events[kVuEvents][160];      // ring: index = n % kVuEvents
    wchar_t iniPath[260];
    wchar_t logPath[260];
    VuGazeInfo gaze;
} VuStatus;

typedef struct VuShared {
    uint32_t magic, layout, size, reserved0;
    volatile int32_t statusSeq;
    VuStatus status;
    volatile int32_t settingsSeq;
    VuSettings settings;
    volatile int32_t commandSeq;
    uint32_t command;
} VuShared;

#pragma pack(pop)
