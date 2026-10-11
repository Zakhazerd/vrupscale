// Compiles the embedded upscale shaders with the real d3dcompiler_47.dll and prints
// errors/warnings plus a short disassembly summary (temps, instruction counts).
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "shader_sources.h"

struct Inc : ID3DInclude {
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR n, LPCVOID, LPCVOID *d, UINT *b) override {
        if (!strcmp(n, "ffx_a.h")) { *d = kFfxA; *b = sizeof(kFfxA) - 1; return S_OK; }
        if (!strcmp(n, "ffx_fsr1.h")) { *d = kFfxFsr1; *b = sizeof(kFfxFsr1) - 1; return S_OK; }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

int main(int argc, char **argv) {
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    if (!m) { printf("cannot load d3dcompiler_47.dll\n"); return 2; }
    auto compile = (pD3DCompile)GetProcAddress(m, "D3DCompile");
    auto disasm = (pD3DDisassemble)GetProcAddress(m, "D3DDisassemble");
    int fails = 0;
    const char *passes[] = { "0", "0", "0", "1", "1", "2", "2", "2" };
    const char *xforms[] = { "0", "1", "2", "0", "2", "0", "1", "2" };
    const char *names[] = { "EASU_raw", "EASU_srgb", "EASU_float", "RCAS_raw", "RCAS_float", "BILINEAR_raw", "BILINEAR_srgb", "BILINEAR_float" };
    for (int i = 0; i < 8; ++i) {
        D3D_SHADER_MACRO defs[] = { { "PASS", passes[i] }, { "XFORM", xforms[i] }, { nullptr, nullptr } };
        Inc inc;
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compile(kShaderMain, sizeof(kShaderMain) - 1, "upscale.hlsl", defs, &inc, "main", "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        printf("== %s: hr=0x%08lx size=%zu\n", names[i], (unsigned long)hr, code ? (size_t)code->GetBufferSize() : 0);
        if (err) { printf("%.*s\n", (int)err->GetBufferSize(), (const char *)err->GetBufferPointer()); err->Release(); }
        if (FAILED(hr)) { fails++; continue; }
        ID3DBlob *text = nullptr;
        if (disasm && SUCCEEDED(disasm(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &text))) {
            std::string s((const char *)text->GetBufferPointer(), text->GetBufferSize());
            size_t p = 0;
            while ((p = s.find('\n', p)) != std::string::npos) {
                size_t e = s.find('\n', p + 1);
                std::string line = s.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
                if (line.find("dcl_temps") != std::string::npos || line.find("dcl_indexableTemp") != std::string::npos ||
                    line.find("dcl_thread_group") != std::string::npos || line.find("dcl_resource") != std::string::npos ||
                    line.find("dcl_uav") != std::string::npos || line.find("instruction slots") != std::string::npos ||
                    line.find("// Approximately") != std::string::npos)
                    printf("   %s\n", line.c_str());
                p = p + 1;
            }
            if (argc > 1) {
                std::string fn = std::string(argv[1]) + "_" + names[i] + ".asm";
                FILE *f = fopen(fn.c_str(), "wb"); if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
            }
            text->Release();
        }
        code->Release();
    }
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails;
}
