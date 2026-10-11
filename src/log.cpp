#include "common.h"

#include <stdarg.h>
#include <stdio.h>

namespace vu {

volatile LONG g_logLevel = 1;

static CRITICAL_SECTION g_logLock;
static bool g_logLockInit = false;
static FILE *g_logFile = nullptr;

void LogOpen(const std::wstring &path)
{
    if (!g_logLockInit) {
        InitializeCriticalSection(&g_logLock);
        g_logLockInit = true;
    }
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    // Start fresh each session if the previous log got large.
    const wchar_t *mode = L"a";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ULONGLONG size = (ULONGLONG(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > 4ull * 1024 * 1024) mode = L"w";
    }
    g_logFile = _wfopen(path.c_str(), mode);
    LeaveCriticalSection(&g_logLock);
}

void LogWrite(int level, const char *fmt, ...)
{
    if (!g_logLockInit || !g_logFile) return;
    static const char *kTags[] = { "ERR ", "INFO", "VERB" };
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u [%s] [t%lu] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     kTags[level < 0 ? 0 : (level > 2 ? 2 : level)], GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && n < (int)sizeof(buf)) vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    ControlNoteEvent(level, buf);
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fputs(buf, g_logFile);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
}

void LogFlush()
{
    if (!g_logLockInit) return;
    EnterCriticalSection(&g_logLock);
    if (g_logFile) fflush(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

const char *FormatName(DXGI_FORMAT f)
{
    switch (f) {
#define F(x) case DXGI_FORMAT_##x: return #x;
        F(UNKNOWN)
        F(R32G32B32A32_TYPELESS) F(R32G32B32A32_FLOAT)
        F(R16G16B16A16_TYPELESS) F(R16G16B16A16_FLOAT) F(R16G16B16A16_UNORM)
        F(R10G10B10A2_TYPELESS) F(R10G10B10A2_UNORM) F(R10G10B10A2_UINT)
        F(R11G11B10_FLOAT)
        F(R8G8B8A8_TYPELESS) F(R8G8B8A8_UNORM) F(R8G8B8A8_UNORM_SRGB) F(R8G8B8A8_UINT)
        F(B8G8R8A8_TYPELESS) F(B8G8R8A8_UNORM) F(B8G8R8A8_UNORM_SRGB)
        F(B8G8R8X8_TYPELESS) F(B8G8R8X8_UNORM) F(B8G8R8X8_UNORM_SRGB)
        F(R32_TYPELESS) F(R32_FLOAT) F(D32_FLOAT) F(R24G8_TYPELESS) F(D24_UNORM_S8_UINT)
        F(R32G8X24_TYPELESS) F(D32_FLOAT_S8X24_UINT) F(R16_TYPELESS) F(D16_UNORM)
        F(NV12) F(R8_UNORM) F(R8G8_UNORM) F(R16_FLOAT) F(R16G16_FLOAT)
#undef F
        default: {
            static char unk[32];
            snprintf(unk, sizeof(unk), "fmt%d", (int)f);
            return unk;
        }
    }
}

} // namespace vu
