// vrupscale compute shaders. Compiled at runtime with D3DCompile (cs_5_0).
//
//   PASS 0  EASU      FSR1 edge-adaptive upscale       source  -> scratch (or proxy if RCAS is off)
//   PASS 1  RCAS      FSR1 robust contrast-adaptive sharpening  scratch -> proxy
//   PASS 2  BILINEAR  plain bilinear upscale (A/B baseline)     source  -> proxy
//   PASS 3  OUTER     outside the area: Catmull-Rom (or bilinear) from the source -> proxy
//                     (OSHARP: and RCAS over that in the sharpening area)
//   PASS 4  FXAA      [upscale] fxaa, over Span: before the upscale, the source -> a copy of it
//                     (in its own encoding) the passes above read instead; after it, what they
//                     wrote (into a copy of the proxy) -> the proxy (XFORM: the proxy's encoding)
//
// The area (as in temporal.hlsl): with FOV_ON, EASU and RCAS cover only an area of each eye's
// picture (Region[z], centred or around the gaze), blending into OUTER's cheap upscale across a
// band inside the area's edge; OUTER does the rest. Both are dispatched over their own extent.
//
// The sharpening area (Sharp[z], the Sharpness area setting) holds the area and may reach further.
// With FOV_SHARP it does: OUTER sharpens its upscale with RCAS there (fading out across a band
// inside the sharpening area's edge), and the area's band blends into that sharpened upscale.
//
// Every other pass covers Span: the game's picture and a band around it. When the game renders
// into part of a larger texture (SteamVR's resolution lowered while it runs), the rest of the
// proxy is never sampled by the compositor, so it is not written either.
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
#ifndef OSHARP       // OUTER: 1 = RCAS over the spatial upscale inside the sharpening area
#define OSHARP 0
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
    uint4 Fov;         // band (output px) | FOV_* flags | source w, h
    uint4 Content;     // the picture in the output: x0, y0, x1, y1 (the band is only where the area ends inside it)
    uint4 Region[2];   // per dispatched slice (z 0, 1): the area, x, y, w, h (output px)
    uint4 Marks[2];    // per dispatched slice: gaze ring x, y | calibration cross x, y (output px; ~0 = none)
    uint4 Span;        // what the passes over the picture cover (output px): x, y, w, h
    uint4 Sharp[2];    // per dispatched slice: the sharpening area, x, y, w, h (output px; holds Region)
};

#define FLAG_FINAL     4u   // this pass writes the proxy: apply the output transform
#define FLAG_TINT      8u   // debug: tint processed output magenta (the area) and blue (outside it)
#define FOV_ON         1u   // EASU / RCAS cover Region only, OUTER the rest
#define FOV_BILINEAR   2u   // OUTER and the band: bilinear instead of Catmull-Rom
#define FOV_SHARP      4u   // the sharpening area reaches past the area (Con0: RCAS's constants in every pass)

SamplerState            LinearClamp : register(s0);
Texture2DArray<float4>  Src         : register(t0);
Texture2DArray<float4>  Orig        : register(t1);   // with FOV_ON: the source, for the spatial upscale
RWTexture2DArray<float4> Dst        : register(u0);

static float gSlice;
static uint gZ;   // the slice within this dispatch

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

// InXform's inverse: perceptual back to the source's own encoding (FXAA's output)
float3 InvXform3(float3 c)
{
#if XFORM == 1
    c = saturate(c);
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
#elif XFORM == 2
    return FloatInv3(c);
#else
    return c;
#endif
}

#define TINT_CENTRE float3(1.0, 0.72, 1.0)
#define TINT_OUTER  float3(0.72, 0.86, 1.0)

// Eye tracking markers over the picture (perceptual colour), as temporal.hlsl draws them: a green
// ring where the gaze is, and the calibration cross straight ahead.
float3 Overlay(uint2 g, float3 c)
{
    const uint4 mk = Marks[min(gZ, 1u)];
    const float R = max(9.0, 0.013 * float(Content.z - Content.x));
    const float2 pc = float2(g) + 0.5;
    if (mk.x != 0xffffffffu) {
        float e = abs(length(pc - float2(mk.xy)) - R);
        c = lerp(c, 0.0, 0.7 * (1.0 - saturate(e - 2.5)));
        c = lerp(c, float3(0.25, 1.0, 0.35), 1.0 - saturate(e - 1.0));
    }
    if (mk.z != 0xffffffffu) {
        float2 v = pc - float2(mk.zw);
        float d = length(v);
        float arm = (d < 2.2 * R && d > 0.45 * R) ? min(abs(v.x), abs(v.y)) : 1e3;
        float ring = abs(d - 1.5 * R);
        float e = min(min(arm, ring), d - 1.5);
        c = lerp(c, 0.0, 0.8 * (1.0 - saturate(e - 2.5)));
        c = lerp(c, float3(1.0, 1.0, 0.85), 1.0 - saturate(e - 1.0));
    }
    return c;
}

// t: 1 in the area, 0 outside, between across the band (tint and marks only when foveated)
void EmitW(uint2 pos, float3 c, float a, float t)
{
    if (Flags & FLAG_TINT)
        c *= (Fov.y & FOV_ON) ? lerp(TINT_OUTER, TINT_CENTRE, t) : TINT_CENTRE;
    if (Flags & FLAG_FINAL)
        c = Overlay(pos, c);   // (Marks all ~0 when there is nothing to show)
#if XFORM == 2
    if (Flags & FLAG_FINAL)
        c = FloatInv3(c);
#endif
    Dst[uint3(pos, (uint)gSlice)] = float4(c, a);
}

void Emit(uint2 pos, float3 c, float a) { EmitW(pos, c, a, 1.0); }

uint4 RegionZ() { return Region[min(gZ, 1u)]; }
uint4 SharpZ() { return Sharp[min(gZ, 1u)]; }

// 1 inside the area r, falling smoothly to 0 across the band at its inner edges (edges on the
// picture's border need no blending); 0 outside it.
float CentreWeight(uint2 g, uint4 r)
{
    const float band = float(Fov.x);
    if (band <= 0.0) return 1.0;
    const int2 lo = int2(r.xy), hi = int2(r.xy + r.zw) - 1;
    float d = 1e6;
    if (r.x > Content.x) d = min(d, float(int(g.x) - lo.x));
    if (r.x + r.z < Content.z) d = min(d, float(hi.x - int(g.x)));
    if (r.y > Content.y) d = min(d, float(int(g.y) - lo.y));
    if (r.y + r.w < Content.w) d = min(d, float(hi.y - int(g.y)));
    return smoothstep(0.0, 1.0, (d + 0.5) / band);
}

// This frame alone at output pixel g, from the whole source: Catmull-Rom (5 bilinear taps), or
// one bilinear tap with FOV_BILINEAR; perceptual colour + alpha (temporal.hlsl's SpatialAt).
float4 Spatial(uint2 g)
{
    const float2 size = float2(Fov.zw), inv = 1.0 / size;
    const float2 t = (float2(g) + 0.5) * size / float2(OutSize) - 0.5;
    if (Fov.y & FOV_BILINEAR) {
        float4 b = Orig.SampleLevel(LinearClamp, float3((clamp(t + 0.5, 0.5, size - 0.5)) * inv, gSlice), 0);
        return float4(saturate(InXform3(max(b.rgb, 0.0))), b.a);
    }
    float2 tc = floor(t);
    float2 f = t - tc;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 lo = 0.5, hi = size - 0.5;
    float2 p0 = clamp(tc - 0.5, lo, hi) * inv;
    float2 p3 = clamp(tc + 2.5, lo, hi) * inv;
    float2 p12 = clamp(tc + 0.5 + w2 / w12, lo, hi) * inv;
    float4 ctr = Orig.SampleLevel(LinearClamp, float3(p12, gSlice), 0);
    float3 c = ctr.rgb * (w12.x * w12.y);
    float ws = w12.x * w12.y;
    c += Orig.SampleLevel(LinearClamp, float3(p12.x, p0.y, gSlice), 0).rgb * (w12.x * w0.y); ws += w12.x * w0.y;
    c += Orig.SampleLevel(LinearClamp, float3(p0.x, p12.y, gSlice), 0).rgb * (w0.x * w12.y); ws += w0.x * w12.y;
    c += Orig.SampleLevel(LinearClamp, float3(p3.x, p12.y, gSlice), 0).rgb * (w3.x * w12.y); ws += w3.x * w12.y;
    c += Orig.SampleLevel(LinearClamp, float3(p12.x, p3.y, gSlice), 0).rgb * (w12.x * w3.y); ws += w12.x * w3.y;
    return float4(saturate(InXform3(max(c / ws, 0.0))), ctr.a);
}

#if PASS == 0
  AF4 FsrEasuRF(AF2 p) { return InXform4(Src.GatherRed  (LinearClamp, float3(p, gSlice))); }
  AF4 FsrEasuGF(AF2 p) { return InXform4(Src.GatherGreen(LinearClamp, float3(p, gSlice))); }
  AF4 FsrEasuBF(AF2 p) { return InXform4(Src.GatherBlue (LinearClamp, float3(p, gSlice))); }
  #define FSR_EASU_F 1
#elif PASS == 1
  static bool gRcasSpatial = false;   // RCAS over the spatial upscale (computed here) instead of the scratch
  AF4 FsrRcasLoadF(ASU2 p)
  {
      if (gRcasSpatial)
          return Spatial(uint2(clamp(p, ASU2(Content.xy), ASU2(Content.zw) - ASU2(1, 1))));
      // foveated: the scratch holds EASU's output in the area only
      ASU2 lo = ASU2(Span.xy), hi = ASU2(Span.xy + Span.zw) - ASU2(1, 1);
      if (Fov.y & FOV_ON) {
          const uint4 r = RegionZ();
          lo = ASU2(r.xy);
          hi = ASU2(r.xy + r.zw) - ASU2(1, 1);
      }
      p = clamp(p, lo, hi);
      return Src.Load(int4(p, (int)gSlice, 0));
  }
  void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
  #define FSR_RCAS_F 1
#elif PASS == 3 && OSHARP
  // OUTER's 16x16 tile of the spatial upscale with a 1-pixel apron (filled once per group), for RCAS
  groupshared float4 gsT[18 * 18];
  static int2 gsOrg;   // the output pixel at gsT[0]
  AF4 FsrRcasLoadF(ASU2 p)
  {
      const int2 q = clamp(int2(p) - gsOrg, 0, 17);
      return gsT[q.y * 18 + q.x];
  }
  void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
  #define FSR_RCAS_F 1
#endif

#include "ffx_fsr1.h"

// The area's result at pos, blended into the spatial upscale across the band (sharpened where the
// sharpening area reaches past the area, as OUTER does next to it); then written.
void EmitArea(uint2 pos, float3 c, float a)
{
    float t = 1.0;
    if ((Fov.y & FOV_ON) && (Flags & FLAG_FINAL)) {
        t = CentreWeight(pos, RegionZ());
        if (t < 1.0) {
            float4 sp = Spatial(pos);
#if PASS == 1
            const float ws = (Fov.y & FOV_SHARP) ? CentreWeight(pos, SharpZ()) : 0.0;
            if (ws > 0.0) {
                AF3 s3;
                gRcasSpatial = true;
                FsrRcasF(s3.r, s3.g, s3.b, pos, Con0);
                gRcasSpatial = false;
                sp.rgb = lerp(sp.rgb, s3, ws);
            }
#endif
            c = lerp(sp.rgb, c, t);
            a = sp.a;
        }
    }
    EmitW(pos, c, a, t);
}

void Process(uint2 pos)
{
    bool inArea = false;
#if PASS == 0 || PASS == 1
    if (Fov.y & FOV_ON) {   // dispatched over the area: pos counts from its corner
        const uint4 r = RegionZ();
        if (any(pos >= r.zw)) return;
        pos += r.xy;
        inArea = true;
    }
#endif
    if (!inArea) {          // dispatched over the span
        if (any(pos >= Span.zw)) return;
        pos += Span.xy;
    }
    if (any(pos >= OutSize))
        return;

#if PASS == 0
    AF3 c;
    FsrEasuF(c, pos, Con0, Con1, Con2, Con3);
    float2 uv = (float2(pos) + 0.5) / float2(OutSize);
    float a = Src.SampleLevel(LinearClamp, float3(uv, gSlice), 0).a;
    EmitArea(pos, c, a);
#elif PASS == 1
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, pos, Con0);
    float a = Src.Load(int4(int2(min(pos, InSize - 1)), (int)gSlice, 0)).a;
    EmitArea(pos, c, a);
#elif PASS == 3
    const uint4 r = RegionZ();
    if (all(pos >= r.xy) && all(pos < r.xy + r.zw)) return;   // EASU / RCAS write the area
    float4 sp = Spatial(pos);
    EmitW(pos, sp.rgb, sp.a, 0.0);
#else
    float2 uv = (float2(pos) + 0.5) / float2(OutSize);
    float4 s = Src.SampleLevel(LinearClamp, float3(uv, gSlice), 0);
    Emit(pos, InXform3(s.rgb), s.a);
#endif
}

#if PASS == 3 && OSHARP
// OUTER with the sharpening area: the spatial upscale, and in the sharpening area RCAS over it
// (blended out across the band inside its edges). Outside the picture (the band around it in the
// span) the plain upscale, as always.
void OuterSharp(uint2 pos, bool tile)
{
    if (any(pos >= Span.zw)) return;
    pos += Span.xy;
    if (any(pos >= OutSize)) return;
    const uint4 r = RegionZ();
    if (all(pos >= r.xy) && all(pos < r.xy + r.zw)) return;   // EASU / RCAS write the area
    float4 sp;
    if (tile && all(pos >= Content.xy) && all(pos < Content.zw)) {
        const int2 q = int2(pos) - gsOrg;
        sp = gsT[q.y * 18 + q.x];
        const float ws = CentreWeight(pos, SharpZ());
        if (ws > 0.0) {
            AF3 s3;
            FsrRcasF(s3.r, s3.g, s3.b, pos, Con0);
            sp.rgb = lerp(sp.rgb, s3, ws);
        }
    } else {
        sp = Spatial(pos);
    }
    EmitW(pos, sp.rgb, sp.a, 0.0);
}
#endif

#if PASS == 4
float3 FxColor(int2 p) { return saturate(InXform3(Src.Load(int4(p, gZ + FirstSlice, 0)).rgb)); }
#include "fxaa.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    gZ = id.z;
    if (any(id.xy >= Span.zw)) return;
    const int2 p = int2(Span.xy + id.xy);
    gFxLo = int2(Span.xy);
    gFxHi = int2(Span.xy + Span.zw) - 1;
    int2 q;
    const float w = FxaaBlend(p, q);
    const float4 c = Src.Load(int4(p, gZ + FirstSlice, 0));
#if XFORM == 2
    const float3 o = lerp(c.rgb, Src.Load(int4(q, gZ + FirstSlice, 0)).rgb, w);
#else
    const float3 o = w > 0.0 ? InvXform3(lerp(FxColor(p), FxColor(q), w)) : c.rgb;
#endif
    Dst[uint3(p, gZ + FirstSlice)] = float4(o, c.a);
}
#else
[numthreads(64, 1, 1)]
void main(uint3 lt : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    gSlice = (float)(gid.z + FirstSlice);
    gZ = gid.z;
    // 64 threads cover a 16x16 tile as four 8x8 quads (AMD's PS-like swizzle).
    AU2 gxy = ARmp8x8(lt.x) + AU2(gid.x << 4u, gid.y << 4u);
#if PASS == 3 && OSHARP
    // a tile next to or in the sharpening area: its upscale (and the apron, clamped to the picture)
    // goes to groupshared memory first, so that RCAS reads each pixel once computed
    const int2 T = int2(Span.xy) + int2(gid.xy) * 16;
    gsOrg = T - 1;
    const int4 s = int4(SharpZ());
    const bool tile = T.x - 1 < s.x + s.z && T.x + 17 > s.x && T.y - 1 < s.y + s.w && T.y + 17 > s.y;
    if (tile)
        for (uint i = lt.x; i < 18u * 18u; i += 64u) {
            const int2 q = clamp(gsOrg + int2(i % 18u, i / 18u), int2(Content.xy), int2(Content.zw) - 1);
            gsT[i] = Spatial(uint2(q));
        }
    GroupMemoryBarrierWithGroupSync();
    OuterSharp(gxy, tile);
    gxy.x += 8u;
    OuterSharp(gxy, tile);
    gxy.y += 8u;
    OuterSharp(gxy, tile);
    gxy.x -= 8u;
    OuterSharp(gxy, tile);
#else
    Process(gxy);
    gxy.x += 8u;
    Process(gxy);
    gxy.y += 8u;
    Process(gxy);
    gxy.x -= 8u;
    Process(gxy);
#endif
}
#endif
