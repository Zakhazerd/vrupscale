// Offline shader compiler: compiles every permutation in src/permutations.h with Microsoft's
// d3dcompiler_47.dll (cs_5_0) and writes them into one binary file, src/gen/shaders.bin, which the
// DLL embeds (src/shader_pack.S). Layout, little-endian: "VUSH", uint32 count, then per shader
// (in permutations.h order) uint32 offset and size from the file's start, then the bytecode.
// Runs natively on Windows or under Wine:  compile_shaders.exe <shader dir> <ffx dir> <out file> [asm dir]
#include <windows.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "permutations.h"

struct Entry { const char *name, *file, *defs; };
static const Entry kEntries[] = {
#define X_ENTRY(name, file, defs) { #name, file, defs },
    VU_SHADER_LIST(X_ENTRY)
#undef X_ENTRY
};

static std::string g_dirs[2];

static bool ReadFile(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    bool ok = fread(&out[0], 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

struct Inc : ID3DInclude {
    std::vector<std::string *> open;
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override
    {
        for (auto &d : g_dirs) {
            std::string *s = new std::string();
            if (ReadFile(d + "/" + name, *s)) {
                *data = s->data();
                *bytes = (UINT)s->size();
                open.push_back(s);
                return S_OK;
            }
            delete s;
        }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID p) override
    {
        for (size_t i = 0; i < open.size(); ++i)
            if (open[i]->data() == p) { delete open[i]; open.erase(open.begin() + i); break; }
        return S_OK;
    }
};

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <shader dir> <ffx dir> <out file> [asm dir]\n", argv[0]);
        return 2;
    }
    g_dirs[0] = argv[1];
    g_dirs[1] = argv[2];
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
    auto disasm = m ? (pD3DDisassemble)GetProcAddress(m, "D3DDisassemble") : nullptr;
    if (!compile) {
        fprintf(stderr, "d3dcompiler_47.dll not found\n");
        return 2;
    }

    std::vector<std::string> blobs;
    int fails = 0;
    for (const Entry &e : kEntries) {
        std::string src;
        if (!ReadFile(g_dirs[0] + "/" + e.file, src)) {
            fprintf(stderr, "cannot read %s\n", e.file);
            return 2;
        }
        // "A=1;B=2" -> macro list
        std::vector<std::string> parts;
        std::string defs = e.defs;
        size_t i = 0;
        while (i < defs.size()) {
            size_t j = defs.find(';', i);
            if (j == std::string::npos) j = defs.size();
            parts.push_back(defs.substr(i, j - i));
            i = j + 1;
        }
        std::vector<std::string> names, values;
        for (auto &p : parts) {
            size_t eq = p.find('=');
            names.push_back(p.substr(0, eq));
            values.push_back(eq == std::string::npos ? "1" : p.substr(eq + 1));
        }
        std::vector<D3D_SHADER_MACRO> macros;
        for (size_t k = 0; k < names.size(); ++k) macros.push_back({ names[k].c_str(), values[k].c_str() });
        macros.push_back({ nullptr, nullptr });

        Inc inc;
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compile(src.data(), src.size(), e.file, macros.data(), &inc, "main", "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (err) {
            if (err->GetBufferSize() > 1)
                fprintf(stderr, "%s:\n%.*s\n", e.name, (int)err->GetBufferSize(), (const char *)err->GetBufferPointer());
            err->Release();
        }
        if (FAILED(hr) || !code) {
            fprintf(stderr, "FAILED %s (0x%08lx)\n", e.name, (unsigned long)hr);
            ++fails;
            continue;
        }
        const unsigned char *p = (const unsigned char *)code->GetBufferPointer();
        size_t n = code->GetBufferSize();
        blobs.push_back(std::string((const char *)p, n));

        // summary: temps, groupshared, instruction count
        std::string summary;
        ID3DBlob *text = nullptr;
        if (disasm && SUCCEEDED(disasm(p, n, 0, nullptr, &text))) {
            std::string s((const char *)text->GetBufferPointer(), text->GetBufferSize());
            const char *keys[] = { "dcl_temps", "dcl_indexableTemp", "dcl_tgsm", "instruction slots" };
            size_t pos = 0;
            while (pos < s.size()) {
                size_t eol = s.find('\n', pos);
                if (eol == std::string::npos) eol = s.size();
                std::string ln = s.substr(pos, eol - pos);
                for (const char *k : keys)
                    if (ln.find(k) != std::string::npos) {
                        size_t b = ln.find_first_not_of(" /");
                        summary += " | " + ln.substr(b == std::string::npos ? 0 : b);
                    }
                pos = eol + 1;
            }
            if (argc > 4) {
                FILE *f = fopen((std::string(argv[4]) + "/" + e.name + ".asm").c_str(), "wb");
                if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
            }
            text->Release();
        }
        printf("%-16s %6zu B%s\n", e.name, n, summary.c_str());
        code->Release();
    }
    if (fails) {
        fprintf(stderr, "%d permutation(s) failed\n", fails);
        return 1;
    }
    // the pack: header, table, then each blob at a 4-byte boundary
    std::string out;
    auto Put32 = [&](std::string &s, size_t at, uint32_t v) {
        for (int b = 0; b < 4; ++b) s[at + b] = (char)((v >> (8 * b)) & 0xff);
    };
    const size_t count = blobs.size(), head = 8 + 8 * count;
    out.assign(head, '\0');
    memcpy(&out[0], "VUSH", 4);
    Put32(out, 4, (uint32_t)count);
    for (size_t k = 0; k < count; ++k) {
        while (out.size() % 4) out += '\0';
        Put32(out, 8 + 8 * k, (uint32_t)out.size());
        Put32(out, 12 + 8 * k, (uint32_t)blobs[k].size());
        out += blobs[k];
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 2;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    printf("wrote %s (%zu permutations, %zu bytes)\n", argv[3], count, out.size());
    return 0;
}
