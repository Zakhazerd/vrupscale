// vrupscale compute shaders. Compiled at runtime with D3DCompile (cs_5_0).
//
//   PASS 0  EASU      FSR1 edge-adaptive upscale       source  -> scratch (or proxy if RCAS is off)
//   PASS 1  RCAS      FSR1 robust contrast-adaptive sharpening  scratch -> proxy
//   PASS 2  BILINEAR  plain bilinear upscale (A/B baseline)     source  -> proxy
//
// Every texture is addressed as a Texture2DArray so the same code handles plain eye textures and
// Unity single-pass-instanced arrays (one dispatch, z = array slice).
//
// FSR wants perceptual input. The source can reach us in three encodings (see XFORM):
//   raw          UNORM bytes as stored (gamma-encoded already, or whatever the app wrote)  -> identity
//   srgbDecoded  typed *_SRGB texture read through an SRGB view, i.e. linear values        -> sRGB encode
//   float        linear HDR float                                                          -> sqrt(x/(1+x))
// The proxy is always written "raw": UNORM UAV over a TYPELESS proxy (the compositor's own SRV
// format decodes it exactly as it would have decoded the original), or FLOAT for float sources
// (inverse transform applied on the final pass).

#ifndef PASS
#define PASS 0
#endif
// XFORM selects the input encoding at compile time (fxc flattens uniform branches, so runtime
// flags would make every pass pay for the sRGB pow and the float sqrt):
//   0 raw, 1 srgbDecoded, 2 float
#ifndef XFORM
#define XFORM 0
#endif

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer Params : register(b0)
{
    uint4 Con0;
    uint4 Con1;
    uint4 Con2;
    uint4 Con3;
    uint2 OutSize;   // pixels written by this pass
    uint2 InSize;    // size of the texture bound at t0 (used to clamp RCAS loads)
    uint  Flags;
    uint  FirstSlice;  // absolute slice of the first array slice this dispatch covers
    uint2 _pad;
};

#define FLAG_FINAL     4u   // this pass writes the proxy: apply the output transform
#define FLAG_TINT      8u   // debug: tint processed output magenta

SamplerState            LinearClamp : register(s0);
Texture2DArray<float4>  Src         : register(t0);
RWTexture2DArray<float4> Dst        : register(u0);

static float gSlice;

float4 SrgbEncode4(float4 x)
{
    x = saturate(x);
    return (x <= 0.0031308) ? (x * 12.92) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float4 FloatFwd4(float4 x)
{
    x = max(x, 0.0);
    return sqrt(x / (1.0 + x));
}

float3 FloatInv3(float3 y)
{
    y = clamp(y, 0.0, 0.9999);
    float3 y2 = y * y;
    return y2 / (1.0 - y2);
}

// Input transform for samples read from the source (EASU and bilinear passes only; the RCAS pass
// reads the scratch texture, which already holds perceptual values).
float4 InXform4(float4 v)
{
#if XFORM == 1
    return SrgbEncode4(v);
#elif XFORM == 2
    return FloatFwd4(v);
#else
    return v;
#endif
}

float3 InXform3(float3 c)
{
    return InXform4(float4(c, 0.0)).rgb;
}

void Emit(uint2 pos, float3 c, float a)
{
    if (Flags & FLAG_TINT)
        c *= float3(1.0, 0.72, 1.0);
#if XFORM == 2
    if (Flags & FLAG_FINAL)
        c = FloatInv3(c);
#endif
    Dst[uint3(pos, (uint)gSlice)] = float4(c, a);
}

#if PASS == 0
  AF4 FsrEasuRF(AF2 p) { return InXform4(Src.GatherRed  (LinearClamp, float3(p, gSlice))); }
  AF4 FsrEasuGF(AF2 p) { return InXform4(Src.GatherGreen(LinearClamp, float3(p, gSlice))); }
  AF4 FsrEasuBF(AF2 p) { return InXform4(Src.GatherBlue (LinearClamp, float3(p, gSlice))); }
  #define FSR_EASU_F 1
#elif PASS == 1
  AF4 FsrRcasLoadF(ASU2 p)
  {
      p = clamp(p, ASU2(0, 0), ASU2(InSize) - ASU2(1, 1));
      return Src.Load(int4(p, (int)gSlice, 0));
  }
  void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
  #define FSR_RCAS_F 1
#endif

#include "ffx_fsr1.h"

void Process(uint2 pos)
{
    if (any(pos >= OutSize))
        return;

#if PASS == 0
    AF3 c;
    FsrEasuF(c, pos, Con0, Con1, Con2, Con3);
    float2 uv = (float2(pos) + 0.5) / float2(OutSize);
    float a = Src.SampleLevel(LinearClamp, float3(uv, gSlice), 0).a;
    Emit(pos, c, a);
#elif PASS == 1
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, pos, Con0);
    float a = Src.Load(int4(int2(min(pos, InSize - 1)), (int)gSlice, 0)).a;
    Emit(pos, c, a);
#else
    float2 uv = (float2(pos) + 0.5) / float2(OutSize);
    float4 s = Src.SampleLevel(LinearClamp, float3(uv, gSlice), 0);
    Emit(pos, InXform3(s.rgb), s.a);
#endif
}

[numthreads(64, 1, 1)]
void main(uint3 lt : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    gSlice = (float)(gid.z + FirstSlice);
    // 64 threads cover a 16x16 tile as four 8x8 quads (AMD's PS-like swizzle).
    AU2 gxy = ARmp8x8(lt.x) + AU2(gid.x << 4u, gid.y << 4u);
    Process(gxy);
    gxy.x += 8u;
    Process(gxy);
    gxy.y += 8u;
    Process(gxy);
    gxy.x -= 8u;
    Process(gxy);
}
