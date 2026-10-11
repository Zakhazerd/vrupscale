// Eye tracking: where the user looks, so the temporal area can follow the gaze.
//
// Source: Bigscreen Beyond 2e. Bigscreen's eye tracking client publishes each new gaze sample in a
// named shared memory block, "VRCFTMemmapData" (its "VRCFT" output, the one Bigscreen's own
// VRCFaceTracking module reads: github.com/BigscreenVR/VRCFT-Beyond). Any number of readers can
// open it; nothing is sent back.
//
// The gaze direction becomes a point in each eye's picture through that eye's projection, which
// the app reads from SteamVR (IVRSystem::GetProjectionRaw) and passes on in the settings.
// Calibration: the user looks at a cross drawn straight ahead (tangent 0,0 in both eyes, so it
// fuses at infinity) for a few seconds; the median gaze measured then becomes the offset that is
// subtracted from every later sample.
#include "internal.h"
#include "control_shared.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>

namespace vu {

namespace {

#pragma pack(push, 8)
struct BeyondGaze {   // SharedGazeData of VRCFT-Beyond (C#, sequential layout)
    float lx, ly, lz, rx, ry, rz, cx, cy, cz;   // gaze directions: left, right, combined (x right, y up)
    float confidence;
    int64_t timestamp;
    int32_t valid;
    float leftClosed, rightClosed;              // 0 open .. 1 closed
};
#pragma pack(pop)
static_assert(sizeof(BeyondGaze) == 64, "layout of Bigscreen's shared gaze data");

const wchar_t *const kMapName = L"VRCFTMemmapData";
const double kStaleSec = 0.5;        // no new sample for this long: tracking is not live
const double kCalSettle = 1.0, kCalEnd = 3.0, kCalShowDone = 4.0;

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_map = nullptr;
const volatile BeyondGaze *g_view = nullptr;
DWORD g_lastOpenTry = 0;
bool g_loggedOpen = false;

BeyondGaze g_last = {};
bool g_have = false;                 // a usable sample has arrived
float g_x = 0, g_y = 0;              // its direction as tangents (raw, before calibration)
LONGLONG g_sampleQpc = 0;            // when it arrived
int g_rateCount = 0;
DWORD g_rateTick = 0;
float g_rate = 0;

int g_calState = kVuCalIdle;
LONGLONG g_calStart = 0, g_calDone = 0;
std::vector<float> g_calX, g_calY;
char g_calMsg[96] = "";

double Seconds(LONGLONG a, LONGLONG b) { return double(b - a) / double(g_qpf.QuadPart ? g_qpf.QuadPart : 1); }

LONGLONG Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void CloseMap()
{
    if (g_view) UnmapViewOfFile((LPCVOID)g_view);
    if (g_map) CloseHandle(g_map);
    g_view = nullptr;
    g_map = nullptr;
}

bool OpenMap()
{
    if (g_view) return true;
    const DWORD t = GetTickCount();
    if (g_lastOpenTry && t - g_lastOpenTry < 1000) return false;   // look again once a second
    g_lastOpenTry = t;
    g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, kMapName);
    if (!g_map) return false;
    g_view = (const volatile BeyondGaze *)MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!g_view || !VirtualQuery((LPCVOID)g_view, &mbi, sizeof(mbi)) || mbi.RegionSize < sizeof(BeyondGaze)) {
        CloseMap();
        return false;
    }
    if (!g_loggedOpen) {
        LOGI("eyes: reading Bigscreen Beyond eye tracking (%ls)", kMapName);
        g_loggedOpen = true;
    }
    return true;
}

// a gaze direction as tangents; false if it isn't usable
bool Tangents(float x, float y, float z, float *tx, float *ty)
{
    const float len = sqrtf(x * x + y * y + z * z);
    if (!(len > 1e-4f)) return false;
    const float fz = fabsf(z) / len;   // forward, whichever sign the tracker uses for it
    if (fz < 0.3f) return false;       // more than ~70 degrees off: not a real gaze
    *tx = (x / len) / fz;
    *ty = (y / len) / fz;
    return true;
}

void Poll(LONGLONG now)
{
    if (!OpenMap()) return;
    BeyondGaze s;
    memcpy(&s, (const void *)g_view, sizeof(s));
    if (memcmp(&s, &g_last, sizeof(s)) == 0) return;   // nothing new
    g_last = s;
    if (s.valid != 1) return;
    if (s.leftClosed > 0.7f && s.rightClosed > 0.7f) return;   // a blink
    float tx, ty;
    bool ok = Tangents(s.cx, s.cy, s.cz, &tx, &ty);
    if (!ok) {   // no combined gaze: the two eyes' average
        float lx, ly, rx, ry;
        bool l = Tangents(s.lx, s.ly, s.lz, &lx, &ly), r = Tangents(s.rx, s.ry, s.rz, &rx, &ry);
        if (l && r) { tx = 0.5f * (lx + rx); ty = 0.5f * (ly + ry); ok = true; }
        else if (l) { tx = lx; ty = ly; ok = true; }
        else if (r) { tx = rx; ty = ry; ok = true; }
    }
    if (!ok) return;
    g_x = tx;
    g_y = ty;
    g_have = true;
    g_sampleQpc = now;
    ++g_rateCount;
    if (g_calState == kVuCalRunning) {
        const double t = Seconds(g_calStart, now);
        if (t >= kCalSettle && t < kCalEnd) {
            g_calX.push_back(tx);
            g_calY.push_back(ty);
        }
    }
}

bool Live(LONGLONG now) { return g_have && Seconds(g_sampleQpc, now) < kStaleSec; }

float Median(std::vector<float> v)
{
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Finishes a calibration run; returns true (and the offset) when it succeeded.
bool FinishCalibration(float *ox, float *oy)
{
    const size_t n = g_calX.size();
    if (n < 12) {
        snprintf(g_calMsg, sizeof(g_calMsg), "Calibration failed: too few gaze samples (%u). Is eye tracking running?", (unsigned)n);
        return false;
    }
    // A steady fixation: most samples within ~2 degrees of the median (blinks and small
    // saccades may leave a few outside). The offset is the median of those.
    float mx = Median(g_calX), my = Median(g_calY);
    std::vector<float> ix, iy;
    for (size_t i = 0; i < n; ++i)
        if (fabsf(g_calX[i] - mx) < 0.035f && fabsf(g_calY[i] - my) < 0.035f) {
            ix.push_back(g_calX[i]);
            iy.push_back(g_calY[i]);
        }
    if (ix.size() < (n * 7 + 9) / 10) {
        snprintf(g_calMsg, sizeof(g_calMsg), "Calibration failed: the gaze moved. Keep looking at the cross until it goes.");
        return false;
    }
    mx = Median(ix);
    my = Median(iy);
    if (fabsf(mx) > 0.5f || fabsf(my) > 0.5f) {   // ~27 degrees: not someone looking straight ahead
        snprintf(g_calMsg, sizeof(g_calMsg), "Calibration failed: the gaze was %.0f degrees off straight ahead.",
                 atanf(std::max(fabsf(mx), fabsf(my))) * 57.2958f);
        return false;
    }
    *ox = mx;
    *oy = my;
    snprintf(g_calMsg, sizeof(g_calMsg), "Calibrated: the tracker read %.1f\xC2\xB0 %s and %.1f\xC2\xB0 %s while you looked straight ahead.",
             fabsf(atanf(mx)) * 57.2958f, mx >= 0 ? "right" : "left", fabsf(atanf(my)) * 57.2958f, my >= 0 ? "up" : "down");
    return true;
}

// The eye's projection as tangents with y down (left < right, top < bottom).
void Proj(int side, float &l, float &r, float &t, float &b)
{
    const float *p = g_cfg.eyeProj;
    bool known = false;
    for (int i = 0; i < 8; ++i) known = known || p[i] != 0.0f;
    if (!known) {   // until the app has asked SteamVR: a typical ~100 degree field of view
        l = -1.2f; r = 1.2f; t = -1.2f; b = 1.2f;
        return;
    }
    if (side == 0 || side == 1) {
        const float *q = p + side * 4;
        l = q[0]; r = q[1]; t = q[2]; b = q[3];
    } else {   // eye not known: a symmetric average of the two
        const float hw = 0.25f * ((p[1] - p[0]) + (p[5] - p[4]));
        const float tc = 0.25f * (p[2] + p[3] + p[6] + p[7]), ht = 0.25f * ((p[3] - p[2]) + (p[7] - p[6]));
        l = -hw; r = hw; t = tc - fabsf(ht); b = tc + fabsf(ht);
    }
    if (l > r) std::swap(l, r);
    if (t > b) { t = -t; b = -b; }   // reported with y up: SteamVR's are y down (top negative)
}

void ToPicture(int side, float tx, float ty, float *u, float *v)
{
    float l, r, t, b;
    Proj(side, l, r, t, b);
    *u = std::min(1.0f, std::max(0.0f, (tx - l) / (r - l)));
    *v = std::min(1.0f, std::max(0.0f, (-ty - t) / (b - t)));
}

} // namespace

void GazeTick()
{
    const LONGLONG now = Now();
    bool finished = false;
    float ox = 0, oy = 0;
    AcquireSRWLockExclusive(&g_lock);
    Poll(now);
    const DWORD t = GetTickCount();
    if (t - g_rateTick >= 1000) {
        g_rate = g_rateTick ? g_rateCount * 1000.0f / (float)(t - g_rateTick) : 0.0f;
        g_rateCount = 0;
        g_rateTick = t;
    }
    if (g_calState == kVuCalRunning && Seconds(g_calStart, now) >= kCalEnd) {
        finished = FinishCalibration(&ox, &oy);
        g_calState = finished ? kVuCalDone : kVuCalFailed;
        g_calDone = now;
        LOGI("eyes: %s", g_calMsg);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (finished) {
        AcquireSRWLockShared(&g_cfgLock);
        Config nc = g_cfg;
        ReleaseSRWLockShared(&g_cfgLock);
        nc.gazeOffset[0] = ox;
        nc.gazeOffset[1] = oy;
        ApplyConfig(nc, "gaze calibration");   // the app mirrors it and saves it
    }
}

bool GazeStartCalibration(const char *by)
{
    const LONGLONG now = Now();
    AcquireSRWLockExclusive(&g_lock);
    bool ok = false;
    if (g_cfg.mode != Mode::Temporal || !g_cfg.enabled)
        snprintf(g_calMsg, sizeof(g_calMsg), "Calibration needs upscaling on, in Temporal mode (the cross is drawn there).");
    else if (!g_view)
        snprintf(g_calMsg, sizeof(g_calMsg), "Calibration needs eye tracking: no data from the Beyond eye tracking client.");
    else if (!Live(now))
        snprintf(g_calMsg, sizeof(g_calMsg), "Calibration needs eye tracking: no recent gaze data (headset on?).");
    else
        ok = true;
    if (ok) {
        g_calState = kVuCalRunning;
        g_calStart = now;
        g_calX.clear();
        g_calY.clear();
        snprintf(g_calMsg, sizeof(g_calMsg), "Look at the cross straight ahead until it goes.");
    } else {
        g_calState = kVuCalFailed;
        g_calDone = now;
    }
    LOGI("eyes: calibration (%s): %s", by, g_calMsg);
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

bool GazePoint(int side, float *u, float *v)
{
    const LONGLONG now = Now();
    AcquireSRWLockShared(&g_lock);
    const bool live = Live(now);
    const float tx = g_x - g_cfg.gazeOffset[0], ty = g_y - g_cfg.gazeOffset[1];
    ReleaseSRWLockShared(&g_lock);
    if (live) ToPicture(side, tx, ty, u, v);
    return live;
}

void GazeTarget(int side, float *u, float *v) { ToPicture(side, 0.0f, 0.0f, u, v); }

bool GazeCalibrating()
{
    AcquireSRWLockShared(&g_lock);
    const bool r = g_calState == kVuCalRunning;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

bool GazeShowRing()
{
    if (g_cfg.gazeShow) return true;
    const LONGLONG now = Now();
    AcquireSRWLockShared(&g_lock);   // just calibrated: show where the gaze lands now
    const bool r = g_calState == kVuCalDone && Seconds(g_calDone, now) < kCalShowDone;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

void GazeFillStatus(VuGazeInfo &g)
{
    const LONGLONG now = Now();
    AcquireSRWLockShared(&g_lock);
    memset(&g, 0, sizeof(g));
    g.source = g_view ? 1 : 0;
    g.live = Live(now) ? 1 : 0;
    g.rate = g.live ? g_rate : 0.0f;
    g.x = g_x;
    g.y = g_y;
    g.following = g.live && g_cfg.gazeFollow && g_cfg.mode == Mode::Temporal && g_cfg.temporalArea < 0.995f;
    g.calState = g_calState;
    g.calProgress = g_calState == kVuCalRunning ? (float)std::min(1.0, Seconds(g_calStart, now) / kCalEnd) : 0.0f;
    snprintf(g.calMessage, sizeof(g.calMessage), "%s", g_calMsg);
    ReleaseSRWLockShared(&g_lock);
}

void GazeShutdown()
{
    AcquireSRWLockExclusive(&g_lock);
    CloseMap();
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace vu
