// Precompiled shader bytecode, generated offline by tools/shaders.sh (Microsoft d3dcompiler_47,
// cs_5_0). Embedding DXBC avoids any runtime dependency on the HLSL compiler and any start-up delay.
#include "common.h"
#include "permutations.h"
#include "gen/shader_blobs.h"

namespace vu {

static_assert(sizeof(kShaderBlobTable) / sizeof(kShaderBlobTable[0]) == SH_COUNT,
              "src/gen/shader_blobs.h is out of date: run tools/shaders.sh");

BlobRef GetShaderBlob(int id)
{
    if (id < 0 || id >= SH_COUNT) return { nullptr, 0 };
    return { kShaderBlobTable[id].data, kShaderBlobTable[id].size };
}

} // namespace vu
