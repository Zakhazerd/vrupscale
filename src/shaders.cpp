// Precompiled shader bytecode, generated offline by tools/shaders.sh (Microsoft d3dcompiler_47,
// cs_5_0) into src/gen/shaders.bin and embedded by src/shader_pack.S. Embedding DXBC avoids any
// runtime dependency on the HLSL compiler and any start-up delay.
//
// The pack: "VUSH", uint32 count, then per shader (permutations.h order) uint32 offset and size
// from the pack's start, then the bytecode.
#include "common.h"
#include "permutations.h"

#include <string.h>

extern "C" const unsigned char vu_shader_pack[];
extern "C" const unsigned char vu_shader_pack_end[];

namespace vu {

namespace {
uint32_t Read32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
} // namespace

BlobRef GetShaderBlob(int id)
{
    const unsigned char *p = vu_shader_pack;
    const size_t n = (size_t)(vu_shader_pack_end - vu_shader_pack);
    if (id < 0 || id >= SH_COUNT || n < 8 || memcmp(p, "VUSH", 4) != 0) return { nullptr, 0 };
    const uint32_t count = Read32(p + 4);
    if (count != (uint32_t)SH_COUNT || n < 8 + 8 * (size_t)count) {
        static bool logged = false;
        if (!logged) LOGE("the embedded shaders (%u) don't match this build (%d): run tools/shaders.sh", count, (int)SH_COUNT);
        logged = true;
        return { nullptr, 0 };
    }
    const uint32_t off = Read32(p + 8 + 8 * id), size = Read32(p + 12 + 8 * id);
    if ((size_t)off + size > n) return { nullptr, 0 };
    return { p + off, size };
}

} // namespace vu
