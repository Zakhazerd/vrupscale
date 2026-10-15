#include "common.h"

#include <stdarg.h>
#include <stdio.h>

#include <string>
#include <vector>

namespace vu {

volatile LONG g_logLevel = 1;

// Lines are written by a thread of their own: the compositor's threads (which log from inside its
// frame) only queue them, so a slow disk or a virus scanner looking at the file can't hold up a frame.
// g_logLock guards the queue only (held for moments); g_fileLock the file (held while writing). Order:
// g_fileLock, then g_logLock.
static CRITICAL_SECTION g_logLock, g_fileLock;
static bool g_logLockInit = false;
static FILE *volatile g_logFile = nullptr;
static std::vector<std::string> g_logQueue;   // lines not written yet
static LONG g_logDropped = 0;                 // lines dropped because the writer fell far behind
static HANDLE g_logWake = nullptr;
static constexpr size_t kLogQueueMax = 4096;

// Takes what is queued and writes it (caller holds g_fileLock, not g_logLock).
static void WriteQueued(std::vector<std::string> &batch)
{
    EnterCriticalSection(&g_logLock);
    batch.swap(g_logQueue);
    const LONG dropped = g_logDropped;
    g_logDropped = 0;
    LeaveCriticalSection(&g_logLock);
    FILE *f = g_logFile;
    if (f) {
        if (dropped > 0) fprintf(f, "(%ld log lines dropped: the log could not be written fast enough)\n", (long)dropped);
        for (auto &l : batch) {
            fputs(l.c_str(), f);
            fputc('\n', f);
        }
        if (!batch.empty() || dropped > 0) fflush(f);
    }
    batch.clear();
}

static DWORD WINAPI LogWriter(LPVOID)
{
    std::vector<std::string> batch;
    for (;;) {
        WaitForSingleObject(g_logWake, 250);
        EnterCriticalSection(&g_fileLock);
        WriteQueued(batch);
        LeaveCriticalSection(&g_fileLock);
    }
    return 0;
}

void LogOpen(const std::wstring &path)
{
    if (!g_logLockInit) {
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_fileLock);
        g_logLockInit = true;
    }
    EnterCriticalSection(&g_fileLock);
    if (g_logFile) {
        std::vector<std::string> batch;
        WriteQueued(batch);
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
    if (!g_logWake) {
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = wake ? CreateThread(nullptr, 0, LogWriter, nullptr, CREATE_SUSPENDED, nullptr) : nullptr;
        if (t) {
            g_logWake = wake;
            ResumeThread(t);
            CloseHandle(t);
        } else if (wake) {   // no writer thread: lines are written on the caller's thread, as before
            CloseHandle(wake);
        }
    }
    LeaveCriticalSection(&g_fileLock);
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
    if (g_logQueue.size() < kLogQueueMax) g_logQueue.emplace_back(buf);
    else ++g_logDropped;
    LeaveCriticalSection(&g_logLock);
    if (g_logWake) SetEvent(g_logWake);
    else LogFlush();
}

// At process exit the writer thread may have been ended while it held a lock: only write when the
// locks are free.
void LogFlushAtExit()
{
    if (!g_logLockInit || !TryEnterCriticalSection(&g_fileLock)) return;
    if (TryEnterCriticalSection(&g_logLock)) {
        LeaveCriticalSection(&g_logLock);
        std::vector<std::string> batch;
        WriteQueued(batch);
    }
    LeaveCriticalSection(&g_fileLock);
}

// Writes everything queued, on the caller's thread (wherever the log must be complete).
void LogFlush()
{
    if (!g_logLockInit) return;
    EnterCriticalSection(&g_fileLock);
    std::vector<std::string> batch;
    WriteQueued(batch);
    LeaveCriticalSection(&g_fileLock);
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
