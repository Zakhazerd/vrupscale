// Every compute-shader permutation the DLL uses. Compiled offline by tools/compile_shaders.cpp
// (Microsoft d3dcompiler_47, cs_5_0) into src/gen/shaders.bin, which is embedded in the DLL.
// X(enum name, source file, "DEFINE=VALUE;...")
#pragma once

#define VU_SHADER_LIST(X)                                                     \
    /* FSR 1 / bilinear path (shaders/upscale.hlsl), + XFORM within each group */ \
    X(FSR_EASU_X0,     "upscale.hlsl",  "PASS=0;XFORM=0")                     \
    X(FSR_EASU_X1,     "upscale.hlsl",  "PASS=0;XFORM=1")                     \
    X(FSR_EASU_X2,     "upscale.hlsl",  "PASS=0;XFORM=2")                     \
    X(FSR_RCAS_X0,     "upscale.hlsl",  "PASS=1;XFORM=0")                     \
    X(FSR_RCAS_X1,     "upscale.hlsl",  "PASS=1;XFORM=1")                     \
    X(FSR_RCAS_X2,     "upscale.hlsl",  "PASS=1;XFORM=2")                     \
    X(FSR_BILIN_X0,    "upscale.hlsl",  "PASS=2;XFORM=0")                     \
    X(FSR_BILIN_X1,    "upscale.hlsl",  "PASS=2;XFORM=1")                     \
    X(FSR_BILIN_X2,    "upscale.hlsl",  "PASS=2;XFORM=2")                     \
    X(FSR_OUTER_X0,    "upscale.hlsl",  "PASS=3;XFORM=0")                     \
    X(FSR_OUTER_X1,    "upscale.hlsl",  "PASS=3;XFORM=1")                     \
    X(FSR_OUTER_X2,    "upscale.hlsl",  "PASS=3;XFORM=2")                     \
    X(FSR_OUTSH_X0,    "upscale.hlsl",  "PASS=3;XFORM=0;OSHARP=1")            \
    X(FSR_OUTSH_X1,    "upscale.hlsl",  "PASS=3;XFORM=1;OSHARP=1")            \
    X(FSR_OUTSH_X2,    "upscale.hlsl",  "PASS=3;XFORM=2;OSHARP=1")            \
    /* temporal path (shaders/temporal.hlsl) */                               \
    X(T_LUMA_X0,       "temporal.hlsl", "TPASS=0;XFORM=0")                    \
    X(T_LUMA_X1,       "temporal.hlsl", "TPASS=0;XFORM=1")                    \
    X(T_LUMA_X2,       "temporal.hlsl", "TPASS=0;XFORM=2")                    \
    X(T_DOWN_L2,       "temporal.hlsl", "TPASS=2;LEVEL=2")                    \
    X(T_DOWN_L3,       "temporal.hlsl", "TPASS=2;LEVEL=3")                    \
    X(T_DOWN_L4,       "temporal.hlsl", "TPASS=2;LEVEL=4")                    \
    X(T_SEARCH,        "temporal.hlsl", "TPASS=3")                            \
    X(T_LK,            "temporal.hlsl", "TPASS=4")                            \
    X(T_ACCUM_X0,      "temporal.hlsl", "TPASS=5;XFORM=0")                    \
    X(T_ACCUM_X1,      "temporal.hlsl", "TPASS=5;XFORM=1")                    \
    X(T_ACCUM_X2,      "temporal.hlsl", "TPASS=5;XFORM=2")                    \
    X(T_OUTCOPY_X0,    "temporal.hlsl", "TPASS=6;XFORM=0;SHARPEN=0")          \
    X(T_OUTCOPY_X1,    "temporal.hlsl", "TPASS=6;XFORM=1;SHARPEN=0")          \
    X(T_OUTCOPY_X2,    "temporal.hlsl", "TPASS=6;XFORM=2;SHARPEN=0")          \
    X(T_OUTRCAS_X0,    "temporal.hlsl", "TPASS=6;XFORM=0;SHARPEN=1")          \
    X(T_OUTRCAS_X1,    "temporal.hlsl", "TPASS=6;XFORM=1;SHARPEN=1")          \
    X(T_OUTRCAS_X2,    "temporal.hlsl", "TPASS=6;XFORM=2;SHARPEN=1")          \
    X(T_COPY,          "temporal.hlsl", "TPASS=7")                            \
    X(T_GLOBAL,        "temporal.hlsl", "TPASS=8")                            \
    X(T_REFINE,        "temporal.hlsl", "TPASS=9;LOCAL=1")                    \
    X(T_REFINE_G,      "temporal.hlsl", "TPASS=9;LOCAL=0")                    \
    X(T_OUTER_X0,      "temporal.hlsl", "TPASS=10;XFORM=0;SHARPEN=0")         \
    X(T_OUTER_X1,      "temporal.hlsl", "TPASS=10;XFORM=1;SHARPEN=0")         \
    X(T_OUTER_X2,      "temporal.hlsl", "TPASS=10;XFORM=2;SHARPEN=0")         \
    X(T_OUTERSH_X0,    "temporal.hlsl", "TPASS=10;XFORM=0;SHARPEN=1")         \
    X(T_OUTERSH_X1,    "temporal.hlsl", "TPASS=10;XFORM=1;SHARPEN=1")         \
    X(T_OUTERSH_X2,    "temporal.hlsl", "TPASS=10;XFORM=2;SHARPEN=1")         \
    /* fingerprint, then its last group decides: temporal (FPD) or spatial (FPS) */ \
    X(T_FPD_X0,        "temporal.hlsl", "TPASS=11;FPMODE=1;XFORM=0")          \
    X(T_FPD_X1,        "temporal.hlsl", "TPASS=11;FPMODE=1;XFORM=1")          \
    X(T_FPD_X2,        "temporal.hlsl", "TPASS=11;FPMODE=1;XFORM=2")          \
    X(T_FPS_X0,        "temporal.hlsl", "TPASS=11;FPMODE=2;XFORM=0")          \
    X(T_FPS_X1,        "temporal.hlsl", "TPASS=11;FPMODE=2;XFORM=1")          \
    X(T_FPS_X2,        "temporal.hlsl", "TPASS=11;FPMODE=2;XFORM=2")          \
    X(T_MARGIN_X0,     "temporal.hlsl", "TPASS=13;XFORM=0")                   \
    X(T_MARGIN_X1,     "temporal.hlsl", "TPASS=13;XFORM=1")                   \
    X(T_MARGIN_X2,     "temporal.hlsl", "TPASS=13;XFORM=2")

#ifdef __cplusplus
namespace vu {
enum ShaderId : int {
#define VU_ENUM_ENTRY(name, file, defs) SH_##name,
    VU_SHADER_LIST(VU_ENUM_ENTRY)
#undef VU_ENUM_ENTRY
    SH_COUNT
};
}
#endif
