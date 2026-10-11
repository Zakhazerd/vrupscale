// Stand-in for Bigscreen's eye tracking client (self-test of the app and DLL): publishes gaze
// samples in "VRCFTMemmapData" like its VRCFT output, 120 per second, the gaze drifting in a slow
// circle. Usage: fakegaze.exe <seconds>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 8)
typedef struct {
    float lx, ly, lz, rx, ry, rz, cx, cy, cz, confidence;
    int64_t timestamp;
    int32_t valid;
    float leftClosed, rightClosed;
} Gaze;
#pragma pack(pop)

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Gaze), L"VRCFTMemmapData");
    Gaze *g = m ? (Gaze *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Gaze)) : NULL;
    if (!g) return 1;
    for (int i = 0; i < secs * 120; ++i) {
        float a = i / 120.0f * 0.8f, tx = 0.25f * cosf(a), ty = 0.15f * sinf(a);
        float n = 1.0f / sqrtf(1 + tx * tx + ty * ty);
        Gaze s;
        memset(&s, 0, sizeof(s));
        s.lx = s.rx = s.cx = tx * n; s.ly = s.ry = s.cy = ty * n; s.lz = s.rz = s.cz = n;
        s.confidence = 1; s.timestamp = i; s.valid = 1;
        memcpy(g, &s, sizeof(s));
        Sleep(8);
    }
    return 0;
}
