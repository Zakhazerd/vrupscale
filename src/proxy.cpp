// Resolution of the real System32\dxgi.dll for the forwarding thunks in exports.S.
#include "common.h"

namespace {

const char *const kDxgiExportNames[] = {
    "CreateDXGIFactory",
    "CreateDXGIFactory1",
    "CreateDXGIFactory2",
    "DXGIGetDebugInterface1",
    "DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization",
    "DXGID3D10CreateDevice",
    "DXGID3D10CreateLayeredDevice",
    "DXGID3D10ETWRundown",
    "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers",
    "DXGIDumpJournal",
    "DXGIReportAdapterConfiguration",
    "CompatString",
    "CompatValue",
    "ApplyCompatResolutionQuirking",
    "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus",
    "PIXBeginCapture",
    "PIXEndCapture",
    "PIXGetCaptureState",
};
constexpr int kDxgiExportCount = sizeof(kDxgiExportNames) / sizeof(kDxgiExportNames[0]);

INIT_ONCE g_resolveOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_realDxgi = nullptr;

} // namespace

extern "C" {

void *vu_dxgi_real[kDxgiExportCount] = {};

// Returned for exports the installed dxgi.dll does not have. Only reachable if something imports
// a name this Windows build lacks, which the loader would already have refused for the real DLL.
HRESULT vu_dxgi_missing_export() { return E_NOTIMPL; }

static BOOL CALLBACK ResolveAll(PINIT_ONCE, PVOID, PVOID *)
{
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 16) {
        std::wstring path(sys);
        path += L"\\dxgi.dll";
        g_realDxgi = LoadLibraryW(path.c_str());
    }
    for (int i = 0; i < kDxgiExportCount; ++i) {
        void *p = g_realDxgi ? (void *)GetProcAddress(g_realDxgi, kDxgiExportNames[i]) : nullptr;
        vu_dxgi_real[i] = p ? p : (void *)&vu_dxgi_missing_export;
    }
    return TRUE;
}

void *vu_dxgi_resolve(int idx)
{
    InitOnceExecuteOnce(&g_resolveOnce, ResolveAll, nullptr, nullptr);
    if (idx < 0 || idx >= kDxgiExportCount) return (void *)&vu_dxgi_missing_export;
    return vu_dxgi_real[idx];
}

} // extern "C"
