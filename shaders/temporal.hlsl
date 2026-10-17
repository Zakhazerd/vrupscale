// vrupscale temporal upscaler (TAAU without engine motion vectors).
//
// The compositor only ever sees colour, so motion is estimated here: a luma pyramid, a coarse
// block search at 1/16 resolution, pyramidal Lucas-Kanade down to 1/2 resolution, then a robust
// global (affine) model refined per cell, producing a flow field on a 1/4-resolution grid. Natural head micro-motion moves
// the scene by fractions of a pixel every frame; with accurate registration those offsets act as
// the sampling jitter a game would add on purpose, so successive frames fill in detail between
// the input pixels.
//
// Passes (TPASS), in dispatch order for one eye stream:
//   FP      1/64 input  sparse frame fingerprint (one sample per 8x8 block); then its last group
//                       DECIDEs: new frame? -> indirect-dispatch args for everything below
//   LUMA    input res   perceptual luma, straight to the L1 level of the pyramid
//   DOWN    L2..L4      2x2 pyramid, plus (I, dI/dx, dI/dy) packs for L1..L3
//   SEARCH  L4          integer block search (+ last frame's flow as predictor)
//   LK      L3 grid     pyramidal Lucas-Kanade L3 -> L2 -> L1 (local flow + texture strength)
//   GLOBAL  1 group     robust affine fit of the local flow (head rotation is ~affine)
//   REFINE  L2 grid     global model unless a local candidate fits clearly better; confidence
//   ACCUM   region      Lanczos reconstruction + reprojected Catmull-Rom history, YCoCg clip
//   OUT     region      RCAS (or copy) -> the compositor-visible proxy, fading to OUTER at the edge
//   OUTER   output res  outside the region: Catmull-Rom (or bilinear) from this frame alone, and
//                       RCAS over that inside the sharpening area (SHARPEN)
//   MARGIN  proxy res   outside the content rectangle (repaints only; see below)
//   COPY    L1..L4      cur pyramid -> prev pyramid
//   FXAA    texture     [upscale] fxaa: before the upscale (after FP) the source -> a copy the
//                       passes after read; after it (last) a copy of the proxy -> the proxy
// Every pass after FP is launched with DispatchIndirect; for a repeated frame (the compositor
// re-sampling the same app frame, or drawing the app's other swapchain textures) the arguments are
// zero and nothing runs. The spatial path uses FP too, deciding with SDECIDE, so it also upscales
// a texture only when its content changed. History is ping-ponged
// between two textures whose roles are tracked on the GPU, so the CPU never needs a readback.
//
// Foveation: history, accumulation and motion estimation cover only Region, a centred rectangle
// of the output (the part of the eye image seen through the sharp centre of the lens). The rest
// is upscaled spatially, which costs a fraction as much; a band inside the region's edge blends
// the two so there is no seam. Region = the whole output turns this off.
//
// Sharpening area (the Sharpness area setting): RCAS sharpens the region's output; the sharpening
// area, centred on the region and at least as large, may reach further. Then (TF_SHARPOUT) OUTER
// sharpens its spatial upscale there too, fading out across a band inside the area's edge, and
// OUT's band blends into that sharpened upscale instead of the plain one.
//
// Content rectangle: after SteamVR's resolution is lowered, the game keeps its larger eye textures
// and renders into a corner of them (the compositor samples only that part). A stream then works
// on that rectangle (SrcRect: its origin in the texture; DstRect: its origin in the proxy), so
// textures of different sizes that hold the same picture share one history. The rest of the proxy
// (MARGIN) is never sampled; it is painted spatially when the proxy is repainted (and with every
// frame for a few seconds after the app changed the game's resolution: TF_MARGIN), except for a band
// along the content's edge (Strides.z) that OUTER refreshes with every frame, in case the picture
// reaches a little further than the rectangle that was measured. FP watches the
// whole texture so the rectangle can be found (and its change noticed): each 8x8 block's sample is
// compared with the one seen last time, and the changed samples' bounding box is accumulated.
//
// Colour is processed in a perceptual space (see XFORM), exactly like the FSR 1 path.

#define TP_LUMA   0
#define TP_DOWN   2
#define TP_SEARCH 3
#define TP_LK     4
#define TP_ACCUM  5
#define TP_OUT    6
#define TP_COPY   7
#define TP_GLOBAL 8
#define TP_REFINE 9
#define TP_OUTER  10
#define TP_FP     11
#define TP_MARGIN 13
#define TP_FXAA   14

#ifndef TPASS
#define TPASS TP_LUMA
#endif
#ifndef XFORM        // 0 raw (already encoded), 1 linear from an sRGB view, 2 linear HDR float
#define XFORM 0
#endif
#ifndef LEVEL        // DOWN: destination pyramid level 2..4 (LUMA writes L1)
#define LEVEL 2
#endif
#ifndef SHARPEN      // OUT: 1 = RCAS, 0 = plain copy; OUTER: 1 = RCAS inside the sharpening area
#define SHARPEN 1
#endif
#ifndef LOCAL        // REFINE: 1 = search local motion where the global model fails (localTrust > 0)
#define LOCAL 1
#endif
#ifndef FPMODE       // FP: what its last group decides: 1 = DECIDE (temporal), 2 = SDECIDE (spatial)
#define FPMODE 1
#endif
#ifndef FXSTAGE      // FXAA: 0 = before the upscale (the source), 1 = after it (the proxy)
#define FXSTAGE 0
#endif

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer TParams : register(b0)
{
    uint4  Size01;     // L0 (input) w,h | L1 w,h
    uint4  Size23;     // L2 w,h | L3 w,h
    uint4  Size4Out;   // L4 w,h | output W,H
    float4 ScaleInfo;  // out/in x,y | in/out x,y
    uint4  RcasCon;    // FsrRcasCon()
    float4 Tune0;      // maxWeight, clipGamma, residualTol, sampleSigma
    float4 Tune1;      // minNewWeight, lkEps, searchLambda, clipReject
    uint4  Misc;       // first source slice, flags, slice count, blend band (output px, 0 = none)
    float4 DebugFlow;  // test hook: (dx, dy, enable, 0) forces a uniform flow
    float4 Tune2;      // localTrust, localGate, localRatio, sharpHistory (1 = Lanczos-3)
    uint4  Region;     // output pixels with history: x0, y0, w, h (the rest is spatial)
    uint4  LkCells;    // L3 grid (8 px) cells with a Lucas-Kanade estimate: first x, y, count x, y
    uint4  RefCells;   // L2 grid (4 px) cells the refinement covers: first x, y, count x, y
    uint4  SearchBox;  // L4 pixels the block search covers: x0, y0, w, h
    uint4  Strides;    // LK cell step (L3 cells), global-fit step (LK cells), OUTER band around the content (proxy px), work margin (input px)
    uint4  SrcRect;    // content origin in the source texture x, y | source texture w, h
    uint4  DstRect;    // content origin in the proxy x, y | proxy w, h
    uint4  Marks;      // gaze ring x, y | calibration cross x, y (content output px; 0xffffffff = none)
    float4 Tune3;      // localCheck (bits: 1 determined, 2 neighbours, 4 last frame), localAgree (L0 px), localAperture, 0
    uint4  Work;       // L4 pixels the pyramid is built over: x0, y0, x1, y1 (the region and Strides.w around it)
    uint4  Sharp;      // the sharpening area: w, h (output px, centred on the region, at least its size), 0, 0
};

// The motion passes read the pyramid only in and around the region, so LUMA, DOWN and COPY work
// there alone: the region (input px) and a margin (Strides.w, [temporal] workMargin) that covers
// their windows, the block search's reach and a frame's motion.

// The region's position can change from frame to frame (it follows the gaze). DECIDE keeps, on
// the GPU, where the history it holds was made, and tells the later passes through Ctrl:
//   Ctrl 8: origin of the history before this frame (ACCUM reads history from there)
//   Ctrl 12: origin of the region the passes of this frame use (Region.xy for a new frame; for a
//            repeated frame that is only repainted, where the history already is)
// (x | y << 16, output pixels)
uint  PackOrg(uint2 o) { return o.x | (o.y << 16); }
uint2 UnpackOrg(uint v) { return uint2(v & 0xffffu, v >> 16); }

#define TF_TINT  1u
#define TF_FORCE 2u
#define TF_RESET 4u
#define TF_PROBE 8u    // FP: compare with the block memory, record changed samples
#define TF_PRIME 16u   // FP: fill the block memory only
#define TF_BILINEAR 32u   // OUT / OUTER / MARGIN: the spatial upscale outside the region is bilinear
#define TF_MARGIN 64u     // DECIDE: MARGIN runs with every new frame, not only on repaints (after a resolution change)
#define TF_SHARPOUT 128u  // OUT / OUTER: the sharpening area reaches past the region (RcasCon set)

SamplerState LinearClamp : register(s0);

uint2 LevelSize(uint l)
{
    if (l == 0) return Size01.xy;
    if (l == 1) return Size01.zw;
    if (l == 2) return Size23.xy;
    if (l == 3) return Size23.zw;
    return Size4Out.xy;
}

// The part of pyramid level l (1..4) that is built: [WorkLo, WorkHi)
uint2 WorkLo(uint l) { return Work.xy << (4u - l); }
uint2 WorkHi(uint l) { return min(Work.zw << (4u - l), LevelSize(l)); }

// OUTER covers the content's output rectangle grown by the band (Strides.z) on every side, in
// 16x16 tiles, except the tiles wholly inside the region (OUT writes those): a list of tiles
// without the hole, the same in DECIDE (how many) and OUTER (which).
struct OuterTiles { uint tx, ty, hx0, hx1, hy0, hy1; };

OuterTiles GetOuterTiles(uint2 org)
{
    OuterTiles t;
    uint2 grid = Size4Out.zw + 2u * Strides.z;
    t.tx = (grid.x + 15u) / 16u;
    t.ty = (grid.y + 15u) / 16u;
    uint2 lo = org + Strides.z, hi = lo + Region.zw;     // the region, in grid pixels
    uint2 h0 = (lo + 15u) / 16u, h1 = max(hi / 16u, h0);
    t.hx0 = h0.x; t.hx1 = h1.x; t.hy0 = h0.y; t.hy1 = h1.y;
    return t;
}

uint OuterCount(OuterTiles t) { return t.tx * t.ty - (t.hx1 - t.hx0) * (t.hy1 - t.hy0); }

uint2 OuterTile(OuterTiles t, uint i)
{
    uint top = t.hy0 * t.tx;
    if (i < top) return uint2(i % t.tx, i / t.tx);
    i -= top;
    uint w = t.tx - (t.hx1 - t.hx0), mid = (t.hy1 - t.hy0) * w;   // rows beside the hole
    if (i < mid) {
        uint c = i % w;
        return uint2(c < t.hx0 ? c : c + (t.hx1 - t.hx0), t.hy0 + i / w);
    }
    i -= mid;
    return uint2(i % t.tx, t.hy1 + i / t.tx);
}

float3 SrgbEncode3(float3 x)
{
    x = saturate(x);
    return (x <= 0.0031308) ? (x * 12.92) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float3 InX(float3 c)
{
#if XFORM == 1
    return SrgbEncode3(c);
#elif XFORM == 2
    c = max(c, 0.0);
    return sqrt(c / (1.0 + c));
#else
    return c;
#endif
}

float3 OutX(float3 c)
{
#if XFORM == 2
    c = clamp(c, 0.0, 0.9999);
    float3 c2 = c * c;
    return c2 / (1.0 - c2);
#else
    return c;
#endif
}

// InX's inverse: perceptual back to the source's own encoding (FXAA before the upscale writes its
// copy of the source so, and the passes after read the copy as they would the source)
float3 InvX(float3 c)
{
#if XFORM == 1
    c = saturate(c);
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
#elif XFORM == 2
    return OutX(c);
#else
    return c;
#endif
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// =============================================================================================
#if TPASS == TP_LUMA

Texture2DArray<float4>    Src : register(t0);
RWTexture2DArray<float>   CurL1 : register(u0);

#define LT 18                      // 16x16 input pixels plus a 1-pixel apron for the L1 filter
groupshared float sL[LT * LT];

// A group converts 16x16 input pixels (plus apron) to luma in shared memory and filters them
// straight down to its 8x8 L1 pixels: the full-resolution luma never goes to memory.
[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    uint z = gid.z;
    uint2 tile = WorkLo(1) / 8u + gid.xy;            // 8x8 L1 pixels from 16x16 input pixels
    int2 base = int2(tile) * 16 - 1;
    int2 inMax = int2(Size01.xy) - 1;
    for (uint k = gi; k < LT * LT; k += 64) {
        int2 pc = clamp(base + int2(k % LT, k / LT), 0, inMax);
        sL[k] = Luma(saturate(InX(Src.Load(int4(min(pc + int2(SrcRect.xy), int2(SrcRect.zw) - 1), z + Misc.x, 0)).rgb)));
    }
    GroupMemoryBarrierWithGroupSync();
    // 4x4 binomial ([1 3 3 1] / 8 per axis), as in DOWN: L1 pixel x reads input 2x-1 .. 2x+2
    uint2 o = tile * 8 + gt.xy;
    if (all(o < Size01.zw)) {
        const float kw[4] = { 0.125, 0.375, 0.375, 0.125 };
        int2 s0 = int2(gt.xy) * 2;
        float acc = 0;
        [unroll] for (int y = 0; y < 4; ++y)
            [unroll] for (int x = 0; x < 4; ++x) acc += kw[y] * kw[x] * sL[(s0.y + y) * LT + s0.x + x];
        CurL1[uint3(o, z)] = acc;
    }
}

// =============================================================================================
#elif TPASS == TP_FP

// Frame fingerprint from one pixel per 8x8 block (at a fixed pseudo-random spot in each block):
// cheap enough to run every time the compositor draws a texture, and any real new frame (head
// motion moves the whole image; animation moves whole regions) changes it. The group that finishes
// last (a counter in Fp) then decides, in the same dispatch: DECIDE (temporal) or SDECIDE (spatial).
Texture2DArray<float4>               Src : register(t0);
globallycoherent RWByteAddressBuffer Fp : register(u0);         // fingerprint x, y; groups done
RWByteAddressBuffer                  ProbeMem : register(u1);   // last sample of each 8x8 block (per slice), bit 31 set
RWByteAddressBuffer                  ProbeBox : register(u2);   // changed samples: min x, min y, ~max x, ~max y (min only)

#if FPMODE == 1
// ---- DECIDE: new frame? Indirect-dispatch args for the passes after FP, and where the region is.
RWByteAddressBuffer State : register(u3);
RWByteAddressBuffer Args : register(u4);
RWByteAddressBuffer Ctrl : register(u5);
RWByteAddressBuffer Link : register(u6);   // this texture's proxy: fingerprint it shows (x, y), valid

// State (bytes): 0 cur history, 4 valid, 8 ring count, 12 ring head, 16..47 ring (4 x uint2),
//                48 new frames, 52 calls, 56 forced outputs, 60 origin of the history (packed)
#define RING 4
// Indirect-argument entries (12 bytes each); must match temporal.cpp
#define A_DOWN2  0
#define A_DOWN3  1
#define A_DOWN4  2
#define A_SEARCH 3
#define A_LK     4
#define A_ACC01  5
#define A_ACC10  6
#define A_OUT1   7
#define A_OUT0   8
#define A_COPY   9
#define A_GLOBAL 10
#define A_REFINE 11
#define A_OUTER  12
#define A_LUMA   13
#define A_MARGIN 14
#define A_FXPRE  15
#define A_FXPOST 16

void SetArgs(uint e, uint3 g) { Args.Store3(e * 12, g); }
uint3 G8(uint2 s)  { return uint3((s + 7) / 8, Misc.z); }
uint3 G16(uint2 s) { return uint3((s + 15) / 16, Misc.z); }

void Decide(uint2 fp)
{
    uint cur = State.Load(0);
    uint valid = State.Load(4);
    uint count = State.Load(8);
    uint head = State.Load(12);
    if (Misc.y & TF_RESET) { valid = 0; count = 0; head = 0; cur = 0; }

    bool seen = false;
    [unroll] for (uint i = 0; i < RING; ++i) {
        uint2 e = State.Load2(16 + i * 8);
        if (i < count && e.x == fp.x && e.y == fp.y) seen = true;
    }
    bool isNew = !seen;
    bool first = valid == 0;
    if (isNew) {
        State.Store2(16 + head * 8, fp);
        head = (head + 1) % RING;
        count = min(count + 1, RING);
    }

    uint3 z0 = uint3(0, 0, 0);
    uint want = PackOrg(Region.xy);
    uint held = first ? want : State.Load(60);    // no history yet: nothing to carry over
    uint use = isNew ? want : held;
    SetArgs(A_LUMA,   isNew ? G8(WorkHi(1) - WorkLo(1)) : z0);   // a group: 8x8 L1 pixels
    SetArgs(A_DOWN2,  isNew ? G8(WorkHi(2) - WorkLo(2)) : z0);
    SetArgs(A_DOWN3,  isNew ? G8(WorkHi(3) - WorkLo(3)) : z0);
    SetArgs(A_DOWN4,  isNew ? G8(WorkHi(4) - WorkLo(4)) : z0);
    SetArgs(A_SEARCH, isNew ? G8(SearchBox.zw) : z0);
    SetArgs(A_LK,     isNew ? G8(LkCells.zw) : z0);
    SetArgs(A_ACC01,  (isNew && cur == 0) ? G16(Region.zw) : z0);
    SetArgs(A_ACC10,  (isNew && cur != 0) ? G16(Region.zw) : z0);
    uint ncur = isNew ? (1 - cur) : cur;
    // The history always holds the stream's newest frame. Repaint this texture's proxy when this
    // frame is new, when the proxy was never written, or when the texture now holds the newest
    // frame but its proxy shows something else (a rotating pool re-filled with an identical frame).
    // An older frame shown again (e.g. by motion smoothing) keeps the proxy it already has.
    uint2 newest = State.Load2(16 + ((head + RING - 1) % RING) * 8);
    uint3 lk = Link.Load3(0);
    bool stale = lk.z == 0 || lk.x != fp.x || lk.y != fp.y;
    bool writeOut = isNew || (Misc.y & TF_FORCE) != 0 || (stale && count > 0 && newest.x == fp.x && newest.y == fp.y);
    if (writeOut) Link.Store3(0, uint3(fp, 1u));
    SetArgs(A_OUT1,   (writeOut && ncur == 1) ? G16(Region.zw) : z0);
    SetArgs(A_OUT0,   (writeOut && ncur == 0) ? G16(Region.zw) : z0);
    OuterTiles ot = GetOuterTiles(UnpackOrg(use));
    uint on = OuterCount(ot);
    SetArgs(A_OUTER,  (writeOut && on > 0u) ? uint3(ot.tx, (on + ot.tx - 1u) / ot.tx, Misc.z) : z0);
    SetArgs(A_MARGIN, ((Misc.y & TF_FORCE) != 0 || ((Misc.y & TF_MARGIN) != 0 && writeOut)) ? G16(DstRect.zw) : z0);
    SetArgs(A_COPY,   isNew ? G8(WorkHi(1) - WorkLo(1)) : z0);
    SetArgs(A_GLOBAL, isNew ? uint3(1, 1, Misc.z) : z0);
    SetArgs(A_REFINE, isNew ? G8(RefCells.zw) : z0);
    // FXAA (when the run binds it): whenever the proxy is written, over the whole texture
    SetArgs(A_FXPRE,  writeOut ? G8(SrcRect.zw) : z0);
    SetArgs(A_FXPOST, writeOut ? G8(DstRect.zw) : z0);

    State.Store(0, ncur);
    State.Store(4, isNew ? 1u : valid);
    State.Store(8, count);
    State.Store(12, head);
    State.Store(48, State.Load(48) + (isNew ? 1u : 0u));
    State.Store(52, State.Load(52) + 1u);
    State.Store(56, State.Load(56) + ((!isNew && writeOut) ? 1u : 0u));
    State.Store(60, use);
    Ctrl.Store4(0, uint4(first ? 1u : 0u, isNew ? 1u : 0u, held, use));
}
#else
// ---- SDECIDE (spatial path): upscale only if this texture range's content changed since its proxy
// was written (or a setting changed: TF_FORCE). Strides.xyz = the group counts of the passes over the
// area (EASU, RCAS: entries 0 and 1), Size23.xy those of the pass outside it (OUTER: entry 2),
// Size4Out.xy those of FXAA before or after the upscale (entry 3; 0: no FXAA).
RWByteAddressBuffer Gate : register(u3);   // 0: fingerprint the proxy shows (x, y), 8: valid, 12: updates, 16: checks
RWByteAddressBuffer Args : register(u4);   // three indirect-dispatch entries

void Decide(uint2 fp)
{
    uint2 shown = Gate.Load2(0);
    bool isNew = (Misc.y & TF_FORCE) != 0 || Gate.Load(8) == 0 || shown.x != fp.x || shown.y != fp.y;
    uint3 g = isNew ? Strides.xyz : uint3(0, 0, 0);
    Args.Store3(0, g);
    Args.Store3(12, g);
    Args.Store3(24, isNew ? uint3(Size23.xy, Strides.z) : uint3(0, 0, 0));
    Args.Store3(36, isNew ? uint3(Size4Out.xy, Strides.z) : uint3(0, 0, 0));
    Gate.Store2(0, fp);
    Gate.Store(8, 1u);
    Gate.Store(12, Gate.Load(12) + (isNew ? 1u : 0u));
    Gate.Store(16, Gate.Load(16) + 1u);
}
#endif

groupshared uint2 gsH[64];
groupshared uint gsBox[4];

uint Mix(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    uint z = gid.z;
    uint2 blk = gid.xy * 8 + gt.xy;
    uint2 tex = SrcRect.zw;                       // the whole texture, not just the content
    uint h1 = 0, h2 = 0;
    bool changed = false;
    uint2 p = uint2(0, 0);
    if (gi < 4) gsBox[gi] = 0xFFFFFFFFu;
    if (all(blk * 8 < tex)) {
        uint r = Mix(blk.x * 0x9E3779B1u + blk.y * 0x85EBCA77u + 0x632BE5ABu);
        p = min(blk * 8 + uint2(r & 7u, (r >> 3) & 7u), tex - 1);
        float3 c = saturate(InX(Src.Load(int4(p, z + Misc.x, 0)).rgb));
        // Order-independent: any changed sample changes h1 (odd multiplier).
        uint3 q = (uint3)(c * 1023.0 + 0.5);
        uint key = (q.r << 20) | (q.g << 10) | q.b;
        uint pos = Mix(p.x * 0x9E3779B1u + p.y * 0x85EBCA77u + z * 0xC2B2AE3Du + 1u);
        h1 = (key + 1u) * (pos | 1u);
        h2 = Mix(key ^ pos);
        if (Misc.y & (TF_PROBE | TF_PRIME)) {
            uint bw = (tex.x + 7u) / 8u, bh = (tex.y + 7u) / 8u;
            uint addr = (((z + Misc.x) * bh + blk.y) * bw + blk.x) * 4u;
            uint v = key | 0x80000000u;
            if (ProbeMem.Load(addr) != v) {
                ProbeMem.Store(addr, v);
                changed = (Misc.y & TF_PRIME) == 0;
            }
        }
    }
    gsH[gi] = uint2(h1, h2);
    GroupMemoryBarrierWithGroupSync();
    if (changed) {
        uint d;
        InterlockedMin(gsBox[0], p.x, d);
        InterlockedMin(gsBox[1], p.y, d);
        InterlockedMin(gsBox[2], ~p.x, d);
        InterlockedMin(gsBox[3], ~p.y, d);
    }
    [unroll] for (uint s = 32; s > 0; s >>= 1) {
        if (gi < s) gsH[gi] += gsH[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) {
        uint dummy;
        Fp.InterlockedAdd(0, gsH[0].x, dummy);
        Fp.InterlockedAdd(4, gsH[0].y, dummy);
        if (gsBox[0] != 0xFFFFFFFFu) {
            ProbeBox.InterlockedMin(0, gsBox[0], dummy);
            ProbeBox.InterlockedMin(4, gsBox[1], dummy);
            ProbeBox.InterlockedMin(8, gsBox[2], dummy);
            ProbeBox.InterlockedMin(12, gsBox[3], dummy);
        }
        // The group that counts itself done last finds every group's share in Fp (each added its
        // share before counting itself): it decides.
        DeviceMemoryBarrier();
        uint done;
        Fp.InterlockedAdd(8, 1u, done);
        const uint groups = ((tex.x + 63u) / 64u) * ((tex.y + 63u) / 64u) * Misc.z;   // (the dispatch's size)
        if (done == groups - 1u) {
            DeviceMemoryBarrier();
            uint fx, fy;
            Fp.InterlockedAdd(0, 0u, fx);
            Fp.InterlockedAdd(4, 0u, fy);
            Decide(uint2(fx, fy));
        }
    }
}

// =============================================================================================
#elif TPASS == TP_DOWN

Texture2DArray<float>    Fine : register(t0);
RWTexture2DArray<float>  Coarse : register(u0);
#if LEVEL >= 2
RWTexture2DArray<float4> Pack : register(u1);   // (I, dI/dx, dI/dy) of the fine level
#endif

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 cs = WorkHi(LEVEL);
    int2 fs = int2(LevelSize(LEVEL - 1));
    id.xy += WorkLo(LEVEL);
    if (any(id.xy >= cs)) return;
    int2 b = int2(id.xy) * 2;
    float v[4][4];
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            v[y][x] = Fine.Load(int4(clamp(b + int2(x - 1, y - 1), 0, fs - 1), id.z, 0));
    // 4x4 binomial ([1 3 3 1] / 8 per axis): far less aliasing than a 2x2 box, so the coarse
    // levels stay faithful to the motion of fine texture.
    const float k[4] = { 0.125, 0.375, 0.375, 0.125 };
    float acc = 0;
    [unroll] for (int y2 = 0; y2 < 4; ++y2)
        [unroll] for (int x2 = 0; x2 < 4; ++x2) acc += k[y2] * k[x2] * v[y2][x2];
    Coarse[id] = acc;
#if LEVEL >= 2
    [unroll] for (int py = 1; py <= 2; ++py)
        [unroll] for (int px = 1; px <= 2; ++px) {
            int2 p = b + int2(px - 1, py - 1);
            if (all(p < fs))
                Pack[uint3(p, id.z)] = float4(v[py][px], 0.5 * (v[py][px + 1] - v[py][px - 1]),
                                              0.5 * (v[py + 1][px] - v[py - 1][px]), 0.0);
        }
#endif
}

// =============================================================================================
#elif TPASS == TP_SEARCH

Texture2DArray<float>   Cur4 : register(t0);
Texture2DArray<float>   Prev4 : register(t1);
Texture2DArray<float4>  FlowPrev : register(t2);   // last frame's final flow (L2 grid, L0 px)
ByteAddressBuffer       Ctrl : register(t3);
RWTexture2DArray<float2> Flow4 : register(u0);     // integer flow in L4 px

#define SR 3                         // search radius (L4 px) = +-48 input px
#define WR 2                         // 5x5 window
#define CT (8 + 2 * WR)
#define PT (8 + 2 * WR + 2 * SR)
groupshared float sCur[CT * CT];
groupshared float sPrev[PT * PT];

float Sad(int2 p, int2 d, uint z, int2 sz)
{
    float s = 0;
    [unroll] for (int oy = -WR; oy <= WR; ++oy)
        [unroll] for (int ox = -WR; ox <= WR; ++ox) {
            int2 q = p + int2(ox, oy);
            s += abs(Cur4.Load(int4(clamp(q, 0, sz - 1), z, 0)) - Prev4.Load(int4(clamp(q + d, 0, sz - 1), z, 0)));
        }
    return s;
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    int2 sz = int2(Size4Out.xy);
    int2 base = int2(SearchBox.xy) + int2(gid.xy) * 8;
    uint z = gid.z;
    for (uint k = gi; k < CT * CT; k += 64) {
        int2 p = base - WR + int2(k % CT, k / CT);
        sCur[k] = Cur4.Load(int4(clamp(p, 0, sz - 1), z, 0));
    }
    for (uint k2 = gi; k2 < PT * PT; k2 += 64) {
        int2 p = base - WR - SR + int2(k2 % PT, k2 / PT);
        sPrev[k2] = Prev4.Load(int4(clamp(p, 0, sz - 1), z, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    int2 p = base + int2(gt.xy);
    if (any(p >= sz) || any(p >= int2(SearchBox.xy + SearchBox.zw))) return;
    if (Ctrl.Load(0) != 0) { Flow4[uint3(p, z)] = 0; return; }

    // Temporal predictor: where this block moved last frame (motion is coherent over time).
    float2 c0 = (float2(p) + 0.5) * 16.0 - 0.5;
    float2 uv2 = ((c0 + 0.5) * 0.25) / float2(Size23.xy);
    float2 fp = FlowPrev.SampleLevel(LinearClamp, float3(uv2, z), 0).xy / 16.0;
    int2 pred = int2(round(clamp(fp, -8.0, 8.0)));                 // finite and bounded, whatever the texture holds

    const float lambda = Tune1.z;
    float best = 1e9;
    int2 bestD = 0;
    [loop] for (int dy = -SR; dy <= SR; ++dy)
        [loop] for (int dx = -SR; dx <= SR; ++dx) {
            float sad = 0;
            [unroll] for (int oy = -WR; oy <= WR; ++oy)
                [unroll] for (int ox = -WR; ox <= WR; ++ox) {
                    int2 ci = int2(gt.xy) + WR + int2(ox, oy);
                    int2 pi = ci + SR + int2(dx, dy);
                    sad += abs(sCur[ci.y * CT + ci.x] - sPrev[pi.y * PT + pi.x]);
                }
            int2 d = int2(dx, dy);
            float cost = sad * (1.0 / 25.0) + lambda * min(abs(d.x) + abs(d.y), abs(d.x - pred.x) + abs(d.y - pred.y));
            if (cost < best) { best = cost; bestD = d; }
        }
    if (any(abs(pred) > SR - 1)) {
        [loop] for (int py = -1; py <= 1; ++py)
            [loop] for (int px = -1; px <= 1; ++px) {
                int2 d = pred + int2(px, py);
                float cost = Sad(p, d, z, sz) * (1.0 / 25.0) +
                             lambda * min(abs(d.x) + abs(d.y), abs(px) + abs(py));
                if (cost < best) { best = cost; bestD = d; }
            }
    }
    Flow4[uint3(p, z)] = float2(bestD);
}

// =============================================================================================
#elif TPASS == TP_LK

Texture2DArray<float4>  Pack1 : register(t0);
Texture2DArray<float4>  Pack2 : register(t1);
Texture2DArray<float4>  Pack3 : register(t2);
Texture2DArray<float>   Prev1 : register(t3);
Texture2DArray<float>   Prev2 : register(t4);
Texture2DArray<float>   Prev3 : register(t5);
Texture2DArray<float2>  Flow4 : register(t6);
ByteAddressBuffer       Ctrl : register(t7);
RWTexture2DArray<float4> FlowLoc : register(u0);   // local flow (L0 px), texture weight, 0 -- L3 grid (8 px)

#define LKR 3                          // 7x7 window

// Inverse-compositional Lucas-Kanade at one level: find d with Prev(x + d) ~= Cur(x).
// G returns the structure tensor (xx, xy, yy) of the window.
float2 LkLevel(Texture2DArray<float4> pack, Texture2DArray<float> prev, int2 sz, float2 c, float2 d, uint z,
               uint iters, out float3 G)
{
    int2 ctr = int2(floor(c + 0.5));
    float2 inv = 1.0 / float2(sz);
    G = 0;
    [loop] for (uint it = 0; it < iters; ++it) {
        G = 0;
        float2 b = 0;
        [loop] for (int oy = -LKR; oy <= LKR; ++oy)
            [unroll] for (int ox = -LKR; ox <= LKR; ++ox) {
                int2 x = clamp(ctr + int2(ox, oy), 0, sz - 1);
                float3 pk = pack.Load(int4(x, z, 0)).xyz;
                float P = prev.SampleLevel(LinearClamp, float3((float2(x) + d + 0.5) * inv, z), 0);
                float e = pk.x - P;
                G += float3(pk.y * pk.y, pk.y * pk.z, pk.z * pk.z);
                b += pk.yz * e;
            }
        float3 Gr = G;
        Gr.xz += Tune1.y;
        float det = Gr.x * Gr.z - Gr.y * Gr.y;
        float2 delta = float2(Gr.z * b.x - Gr.y * b.y, Gr.x * b.y - Gr.y * b.x) / max(det, 1e-12);
        float m = length(delta);
        if (m > 1.0) delta /= m;
        d += delta;
    }
    return d;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= LkCells.zw)) return;
    // Cells inside the region only, and when nothing but the global fit reads them, only every
    // Strides.x-th: thousands of samples already pin six affine parameters down.
    uint2 cell = LkCells.xy + id.xy * Strides.x;
    if (any(cell >= Size23.zw)) return;
    uint3 o = uint3(cell, id.z);
    if (Ctrl.Load(0) != 0) { FlowLoc[o] = 0; return; }

    // One estimate per 8x8 input pixels: the 7x7 L1 windows (14 px) still overlap, and the global
    // fit and the per-cell refinement need no denser sampling.
    float2 c0 = (float2(cell) + 0.5) * 8.0 - 0.5;                // L0 coords of this grid point
    float2 c4 = (c0 + 0.5) * (1.0 / 16.0) - 0.5;
    int2 p4 = clamp(int2(floor(c4 + 0.5)), 0, int2(Size4Out.xy) - 1);
    float2 d = Flow4.Load(int4(p4, id.z, 0)) * 2.0;               // L4 px -> L3 px
    float3 G;
    d = LkLevel(Pack3, Prev3, int2(Size23.zw), (c0 + 0.5) * 0.125 - 0.5, d, id.z, 2, G);
    d = LkLevel(Pack2, Prev2, int2(Size23.xy), (c0 + 0.5) * 0.25 - 0.5, d * 2.0, id.z, 2, G);
    d = LkLevel(Pack1, Prev1, int2(Size01.zw), (c0 + 0.5) * 0.5 - 0.5, d * 2.0, id.z, 3, G);
    // How well this window constrains motion: the smaller eigenvalue of the structure tensor,
    // as a mean squared gradient (luma / L1 px) over the window.
    float tr = 0.5 * (G.x + G.z), dd = 0.5 * (G.x - G.z);
    float lmin = (tr - sqrt(dd * dd + G.y * G.y)) / float((2 * LKR + 1) * (2 * LKR + 1));
    FlowLoc[o] = float4(d * 2.0, lmin, 0);                        // L1 px -> L0 px
}

// =============================================================================================
#elif TPASS == TP_GLOBAL

// Robust affine fit of the local flow field (one group per eye): head rotation moves the whole
// image almost affinely, and thousands of noisy local estimates pin that motion down far more
// precisely than any single window can. IRLS with Tukey weights rejects moving objects,
// parallax and aliased patches.
Texture2DArray<float4> FlowLoc : register(t0);
ByteAddressBuffer      Ctrl : register(t1);
RWByteAddressBuffer    Model : register(u0);   // per slice, 64 B: a0..a5, weight, inliers, prev a0..a5

#define GT 256
groupshared float gs[13][GT];
groupshared float gm[6];

[numthreads(GT, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint z = gid.z;
    uint base = z * 64;
    if (Ctrl.Load(0) != 0) {
        if (gi < 16) Model.Store(base + gi * 4, 0u);
        return;
    }
    // The cells LK estimated (every Strides.x-th inside the region), of which every Strides.y-th:
    // at most ~12k per eye, since thousands of inliers already fix six parameters to well below
    // 0.01 px and this pass runs as a single group.
    uint gStep = max(Strides.y, 1u);
    uint stride = max(Strides.x, 1u) * gStep;
    uint cellsX = (LkCells.z + gStep - 1) / gStep, cellsY = (LkCells.w + gStep - 1) / gStep;
    uint total = cellsX * cellsY;
    uint perThread = (total + GT - 1) / GT;
    float2 half0 = 0.5 * float2(Size01.xy);
    // The previous frame's pyramid exists around where its region was: where the region moved on
    // since (eye tracking), what LK found compares with an older frame; leave those cells out.
    float2 prevLo = float2(UnpackOrg(Ctrl.Load(8))) * ScaleInfo.zw, prevHi = prevLo + float2(Region.zw) * ScaleInfo.zw;
    prevLo -= float(Strides.w) - 48.0;
    prevHi += float(Strides.w) - 48.0;
    if (gi < 6) gm[gi] = 0.0;
    GroupMemoryBarrierWithGroupSync();

    [loop] for (uint it = 0; it < 4; ++it) {
        // sums kept in plain registers (an indexed array would spill to local memory)
        float4 accA = 0, accB = 0, accC = 0;
        float accN = 0;
        float a0 = gm[0], a1 = gm[1], a2 = gm[2], a3 = gm[3], a4 = gm[4], a5 = gm[5];
        [loop] for (uint j = 0; j < perThread; ++j) {
            uint i = gi + j * GT;
            uint2 cxy = LkCells.xy + uint2(i % cellsX, i / cellsX) * stride;
            float4 f = FlowLoc.Load(int4(cxy, z, 0));
            float2 c0 = (float2(cxy) + 0.5) * 8.0 - 0.5;
            float X = (c0.x - half0.x) / half0.x, Y = (c0.y - half0.y) / half0.y;
            bool inside = i < total && all(cxy < Size23.zw) && all(c0 >= 12.0) && all(c0 <= float2(Size01.xy) - 13.0) &&
                          all(c0 >= prevLo) && all(c0 <= prevHi);
            float w = inside ? saturate(f.z * (1.0 / 2e-4)) : 0.0;     // texture weight (mean sq. gradient)
            if (it > 0) {
                float2 r = f.xy - float2(a0 + a1 * X + a2 * Y, a3 + a4 * X + a5 * Y);
                float clipC = it == 1 ? 2.0 : (it == 2 ? 1.0 : 0.6);
                float u = length(r) / clipC;
                float t = saturate(1.0 - u * u);
                w *= t * t;                                            // Tukey biweight
            }
            accA += w * float4(1.0, X, Y, X * X);
            accB += w * float4(X * Y, Y * Y, f.x, X * f.x);
            accC += w * float4(Y * f.x, f.y, X * f.y, Y * f.y);
            accN += inside ? 1.0 : 0.0;
        }
        gs[0][gi] = accA.x; gs[1][gi] = accA.y; gs[2][gi] = accA.z; gs[3][gi] = accA.w;
        gs[4][gi] = accB.x; gs[5][gi] = accB.y; gs[6][gi] = accB.z; gs[7][gi] = accB.w;
        gs[8][gi] = accC.x; gs[9][gi] = accC.y; gs[10][gi] = accC.z; gs[11][gi] = accC.w;
        gs[12][gi] = accN;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint s = GT / 2; s > 0; s >>= 1) {
            if (gi < s) {
                [unroll] for (uint k3 = 0; k3 < 13; ++k3) gs[k3][gi] += gs[k3][gi + s];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (gi == 0) {
            float S = gs[0][0];
            float m00 = S, m01 = gs[1][0], m02 = gs[2][0], m11 = gs[3][0], m12 = gs[4][0], m22 = gs[5][0];
            float3 bx = float3(gs[6][0], gs[7][0], gs[8][0]), by = float3(gs[9][0], gs[10][0], gs[11][0]);
            float reg = 1e-3 * S + 1e-6;
            m11 += reg; m22 += reg;
            float c00 = m11 * m22 - m12 * m12, c01 = m02 * m12 - m01 * m22, c02 = m01 * m12 - m02 * m11;
            float c11 = m00 * m22 - m02 * m02, c12 = m01 * m02 - m00 * m12, c22 = m00 * m11 - m01 * m01;
            float det = m00 * c00 + m01 * c01 + m02 * c02;
            if (S > 4.0 && det > 1e-9 * S * S * S) {
                float inv = 1.0 / det;
                gm[0] = (c00 * bx.x + c01 * bx.y + c02 * bx.z) * inv;
                gm[1] = (c01 * bx.x + c11 * bx.y + c12 * bx.z) * inv;
                gm[2] = (c02 * bx.x + c12 * bx.y + c22 * bx.z) * inv;
                gm[3] = (c00 * by.x + c01 * by.y + c02 * by.z) * inv;
                gm[4] = (c01 * by.x + c11 * by.y + c12 * by.z) * inv;
                gm[5] = (c02 * by.x + c12 * by.y + c22 * by.z) * inv;
            } else if (S > 1e-3) {                                      // too little structure: translation only
                gm[0] = bx.x / S; gm[1] = 0; gm[2] = 0;
                gm[3] = by.x / S; gm[4] = 0; gm[5] = 0;
            }
            if (it == 3) {
                Model.Store4(base, asuint(float4(gm[0], gm[1], gm[2], gm[3])));
                Model.Store4(base + 16, asuint(float4(gm[4], gm[5], S, S / max(gs[12][0], 1.0))));
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// =============================================================================================
#elif TPASS == TP_REFINE

// Final flow per grid cell: the global model unless a local candidate explains the frame clearly
// better (moving objects, parallax). Also rates how well the chosen motion explains the frame,
// relative to local contrast; the accumulation trusts history only where this is good.
Texture2DArray<float4>   FlowLoc : register(t0);
ByteAddressBuffer        Model : register(t1);
Texture2DArray<float4>   FlowPrev : register(t2);
Texture2DArray<float4>   Pack1 : register(t3);
Texture2DArray<float>    Prev1 : register(t4);
ByteAddressBuffer        Ctrl : register(t5);
RWTexture2DArray<float4> Flow2 : register(u0);     // dx, dy (L0 px), confidence, 1 = global

#define RR 2                                         // 5x5 window at L1

static int2 gCtr;
static uint gZ;
static float2 gInv;

float Cost(float2 dL1)
{
    float r = 0;
    [unroll] for (int oy = -RR; oy <= RR; ++oy)
        [unroll] for (int ox = -RR; ox <= RR; ++ox) {
            int2 x = clamp(gCtr + int2(ox, oy), 0, int2(Size01.zw) - 1);
            float T = Pack1.Load(int4(x, gZ, 0)).x;
            float P = Prev1.SampleLevel(LinearClamp, float3((float2(x) + dL1 + 0.5) * gInv, gZ), 0);
            r += abs(T - P);
        }
    return r * (1.0 / ((2 * RR + 1) * (2 * RR + 1)));
}

float2 Lk(float2 d, uint iters)
{
    [loop] for (uint it = 0; it < iters; ++it) {
        float3 G = 0;
        float2 b = 0;
        [unroll] for (int oy = -RR; oy <= RR; ++oy)
            [unroll] for (int ox = -RR; ox <= RR; ++ox) {
                int2 x = clamp(gCtr + int2(ox, oy), 0, int2(Size01.zw) - 1);
                float3 pk = Pack1.Load(int4(x, gZ, 0)).xyz;
                float P = Prev1.SampleLevel(LinearClamp, float3((float2(x) + d + 0.5) * gInv, gZ), 0);
                G += float3(pk.y * pk.y, pk.y * pk.z, pk.z * pk.z);
                b += pk.yz * (pk.x - P);
            }
        G.xz += Tune1.y;
        float det = G.x * G.z - G.y * G.y;
        float2 delta = float2(G.z * b.x - G.y * b.y, G.x * b.y - G.y * b.x) / max(det, 1e-12);
        float m = length(delta);
        if (m > 0.5) delta *= 0.5 / m;
        d += delta;
    }
    return d;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= RefCells.zw)) return;
    uint3 id = uint3(RefCells.xy + tid.xy, tid.z);                 // L2 cell (only the region's)
    if (any(id.xy >= Size23.xy)) return;
    if (Ctrl.Load(0) != 0) { Flow2[id] = 0; return; }
    if (DebugFlow.z != 0) { Flow2[id] = float4(DebugFlow.xy, 1, 1); return; }

    gZ = id.z;
    gInv = 1.0 / float2(Size01.zw);
    float2 c0 = (float2(id.xy) + 0.5) * 4.0 - 0.5;
    float2 c1 = (c0 + 0.5) * 0.5 - 0.5;
    gCtr = int2(floor(c1 + 0.5));

    // global model at this point
    uint base = id.z * 64;
    float4 m0 = asfloat(Model.Load4(base));
    float2 m1 = asfloat(Model.Load2(base + 16));
    float2 half0 = 0.5 * float2(Size01.xy);
    float X = (c0.x - half0.x) / half0.x, Y = (c0.y - half0.y) / half0.y;
    float2 gv = float2(m0.x + m0.y * X + m0.z * Y, m0.w + m1.x * X + m1.y * Y);

    // window statistics (local contrast) for normalising residuals
    float s1 = 0, s2 = 0;
    [unroll] for (int oy = -RR; oy <= RR; ++oy)
        [unroll] for (int ox = -RR; ox <= RR; ++ox) {
            float T = Pack1.Load(int4(clamp(gCtr + int2(ox, oy), 0, int2(Size01.zw) - 1), gZ, 0)).x;
            s1 += T; s2 += T * T;
        }
    float n = (2 * RR + 1) * (2 * RR + 1);
    float sigma = sqrt(max(s2 / n - (s1 / n) * (s1 / n), 0.0));

    float costG = Cost(gv * 0.5);
    float rnG = costG / (sigma + 0.004);
    float2 d = gv;
    float isGlobal = 1.0;
    // Normalised residual: ~0.3-0.6 when the motion is right (interpolation, aliasing), ~1.4 when
    // it is unrelated. Confidence 1 below 0.5 * residualTol, 0 at residualTol.
    float conf = saturate(2.0 - 2.0 * rnG / Tune0.z);
#if LOCAL
    if (rnG > Tune2.y) {
        // The global model does not explain this cell: independent motion or parallax. Look for
        // a local motion, but trust it only as far as localTrust allows (periodic or aliased
        // texture can match at a wrong offset, which colour clamping cannot catch).
        int2 gmax = int2(Size23.zw) - 1;                          // local flow lives on the L3 grid
        int2 lc = min(int2(id.xy >> 1), gmax);
        float2 cand[6];
        cand[0] = FlowLoc.Load(int4(lc, id.z, 0)).xy;
        cand[1] = FlowLoc.Load(int4(clamp(lc + int2(-1, 0), 0, gmax), id.z, 0)).xy;
        cand[2] = FlowLoc.Load(int4(clamp(lc + int2(1, 0), 0, gmax), id.z, 0)).xy;
        cand[3] = FlowLoc.Load(int4(clamp(lc + int2(0, -1), 0, gmax), id.z, 0)).xy;
        cand[4] = FlowLoc.Load(int4(clamp(lc + int2(0, 1), 0, gmax), id.z, 0)).xy;
        cand[5] = FlowPrev.Load(int4(id.xy, id.z, 0)).xy;
        float best = 1e9;
        float2 bd = gv;
        [unroll] for (uint k = 0; k < 6; ++k) {
            float c = Cost(cand[k] * 0.5);
            if (c < best) { best = c; bd = cand[k]; }
        }
        if (best < Tune2.z * costG && length(bd - gv) > 0.3) {
            float2 r = Lk(bd * 0.5, 2) * 2.0;
            float cr = Cost(r * 0.5);
            if (cr < best) { bd = r; best = cr; }
            const float confG = conf;
            d = bd;
            isGlobal = 0.0;
            conf = Tune2.x * saturate(2.0 - 2.0 * (best / (sigma + 0.004)) / Tune0.z);
            // Is this motion real? A match alone isn't proof: aliased or periodic texture matches at
            // wrong offsets, and along an edge or stripes only the motion across them is measured.
            uint checks = uint(Tune3.x + 0.5);
            float tol = Tune3.y;
            if (checks & 1u) {   // well determined: the window has texture in two directions
                float3 G = 0;
                [unroll] for (int oy = -RR; oy <= RR; ++oy)
                    [unroll] for (int ox = -RR; ox <= RR; ++ox) {
                        float2 gr = Pack1.Load(int4(clamp(gCtr + int2(ox, oy), 0, int2(Size01.zw) - 1), gZ, 0)).yz;
                        G += float3(gr.x * gr.x, gr.x * gr.y, gr.y * gr.y);
                    }
                float tr = G.x + G.z, disc = sqrt(max(tr * tr - 4.0 * (G.x * G.z - G.y * G.y), 0.0));
                float lmin = 0.5 * (tr - disc), lmax = 0.5 * (tr + disc);
                float ratio = lmin / max(lmax, 1e-9);
                conf *= saturate((ratio - 0.5 * Tune3.z) / (0.5 * Tune3.z));
            }
            if (checks & 2u) {   // neighbours: a moving thing moves as a whole (two of four agree)
                float agree = 0;
                [unroll] for (uint k = 1; k < 5; ++k) agree += length(cand[k] - d) < tol ? 1.0 : 0.0;
                conf *= saturate(agree - 1.0);
            }
            if (checks & 4u) {   // last frame: it was moving like this already (here or next door)
                float m = 1e9;
                m = min(m, length(FlowPrev.Load(int4(id.xy, id.z, 0)).xy - d));
                m = min(m, length(FlowPrev.Load(int4(clamp(int2(id.xy) + int2(-1, 0), 0, int2(Size23.xy) - 1), id.z, 0)).xy - d));
                m = min(m, length(FlowPrev.Load(int4(clamp(int2(id.xy) + int2(1, 0), 0, int2(Size23.xy) - 1), id.z, 0)).xy - d));
                m = min(m, length(FlowPrev.Load(int4(clamp(int2(id.xy) + int2(0, -1), 0, int2(Size23.xy) - 1), id.z, 0)).xy - d));
                m = min(m, length(FlowPrev.Load(int4(clamp(int2(id.xy) + int2(0, 1), 0, int2(Size23.xy) - 1), id.z, 0)).xy - d));
                conf *= m < tol ? 1.0 : 0.0;
            }
            // a local motion the checks doubt is worth no more than the global model here (bit 16:
            // nothing: the global model has already failed)
            if (checks != 0u && conf < confG) {
                bool still = true;   // bit 32: back to the global model only where this cell followed it last frame
                if (checks & 32u) still = length(FlowPrev.Load(int4(id.xy, id.z, 0)).xy - gv) < tol;
                d = gv;
                conf = ((checks & 16u) || !still) ? 0.0 : confG;
                isGlobal = 1.0;
            }
        } else if (uint(Tune3.x + 0.5) & 8u) {
            conf = 0.0;   // bit 8: no local motion found where the global model fails: no history, as with localTrust 0
        }
    }
#else
    // localTrust = 0: history is not used where the global model fails, whatever the local motion
    // is, so finding it would change nothing in the output.
    if (rnG > Tune2.y) { conf = 0.0; isGlobal = 0.0; }
#endif
    Flow2[id] = float4(d, conf, isGlobal);
}

// =============================================================================================
#elif TPASS == TP_ACCUM

Texture2DArray<float4>   Src : register(t0);
Texture2DArray<float4>   Flow2 : register(t1);
Texture2DArray<float4>   Hist : register(t2);
ByteAddressBuffer        Ctrl : register(t3);
RWTexture2DArray<float4> HistOut : register(u0);

#define TILE 20
groupshared float3 sIn[TILE * TILE];

float3 ToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Lanczos2 without trigonometry: its zeros (single at 1, double at 2) times a quadratic fitted to
// the rest; within 0.005 of the real kernel everywhere.
float Lanczos2(float x)
{
    float t = min(x * x, 4.0);
    float b = 1.0 - 0.25 * t;
    return (1.0 - t) * (b * b) * (1.0 + t * (-0.52247 + 0.085085 * t));
}

// Catmull-Rom with 5 bilinear taps (corner taps dropped); alpha = bilinear accumulation weight.
// pos is in history (region) pixels.
float4 SampleHistoryCR(float2 pos, uint z)
{
    float2 inv = 1.0 / float2(Region.zw);
    float2 t = pos - 0.5;
    float2 tc = floor(t);
    float2 f = t - tc;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 p0 = (tc - 0.5) * inv;
    float2 p3 = (tc + 2.5) * inv;
    float2 p12 = (tc + 0.5 + w2 / w12) * inv;
    float4 ctr = Hist.SampleLevel(LinearClamp, float3(p12, z), 0);
    float3 c = ctr.rgb * (w12.x * w12.y);
    float ws = w12.x * w12.y;
    c += Hist.SampleLevel(LinearClamp, float3(p12.x, p0.y, z), 0).rgb * (w12.x * w0.y); ws += w12.x * w0.y;
    c += Hist.SampleLevel(LinearClamp, float3(p0.x, p12.y, z), 0).rgb * (w0.x * w12.y); ws += w0.x * w12.y;
    c += Hist.SampleLevel(LinearClamp, float3(p3.x, p12.y, z), 0).rgb * (w3.x * w12.y); ws += w3.x * w12.y;
    c += Hist.SampleLevel(LinearClamp, float3(p12.x, p3.y, z), 0).rgb * (w12.x * w3.y); ws += w12.x * w3.y;
    return float4(c / ws, ctr.a);
}

float SincPi(float x)
{
    x *= 3.14159265;
    return abs(x) < 1e-4 ? 1.0 : sin(x) / x;
}

// Lanczos-3 (6x6 texels): keeps more of the finest detail through each frame's resampling than
// Catmull-Rom. The history is resampled every frame, so what each resampling softens adds up; on
// the test scene this is worth +0.6 dB overall and +1.4 dB on detail above the input's Nyquist limit.
float4 SampleHistoryL3(float2 pos, uint z)
{
    float2 t = pos - 0.5;
    int2 tc = int2(floor(t));
    float2 f = t - float2(tc);
    float wx[6], wy[6];
    float sx = 0, sy = 0;
    [unroll] for (int k = 0; k < 6; ++k) {
        float dx = f.x - float(k - 2), dy = f.y - float(k - 2);
        wx[k] = SincPi(dx) * SincPi(dx / 3.0);
        wy[k] = SincPi(dy) * SincPi(dy / 3.0);
        sx += wx[k];
        sy += wy[k];
    }
    int2 mx = int2(Region.zw) - 1;
    float3 c = 0;
    [unroll] for (int j = 0; j < 6; ++j) {
        float3 row = 0;
        [unroll] for (int i = 0; i < 6; ++i) row += Hist.Load(int4(clamp(tc + int2(i - 2, j - 2), 0, mx), z, 0)).rgb * wx[i];
        c += row * wy[j];
    }
    float a = Hist.SampleLevel(LinearClamp, float3(pos / float2(Region.zw), z), 0).a;
    return float4(c / (sx * sy), a);
}

float4 SampleHistory(float2 pos, uint z)
{
    if (Tune2.w > 0.5) return SampleHistoryL3(pos, z);
    return SampleHistoryCR(pos, z);
}

void Accumulate(int2 q, int2 i0, uint z, bool first, int2 org, int2 prevOrg)
{
    float2 u = (float2(q) + 0.5) * ScaleInfo.zw - 0.5;        // input pixel coordinates
    int2 n = int2(floor(u + 0.5));
    float2 o = u - float2(n);

    float3 sum = 0, m1 = 0, m2 = 0, ymin = 1e5, ymax = -1e5;
    float wsum = 0;
    float lx[3], ly[3];                                        // separable: 6 kernel evaluations, not 18
    [unroll] for (int k = 0; k < 3; ++k) { lx[k] = Lanczos2(float(k - 1) - o.x); ly[k] = Lanczos2(float(k - 1) - o.y); }
    [unroll] for (int ky = -1; ky <= 1; ++ky)
        [unroll] for (int kx = -1; kx <= 1; ++kx) {
            int2 t = n + int2(kx, ky) - i0;
            float3 c = sIn[t.y * TILE + t.x];
            float w = lx[kx + 1] * ly[ky + 1];
            sum += c * w;
            wsum += w;
            float3 y = ToYCoCg(c);
            m1 += y;
            m2 += y * y;
            ymin = min(ymin, y);
            ymax = max(ymax, y);
        }
    // de-ring the Lanczos result against the 2x2 inputs that surround this output pixel
    int2 f = int2(floor(u)) - i0;
    float3 a0 = sIn[f.y * TILE + f.x], a1 = sIn[f.y * TILE + f.x + 1];
    float3 a2 = sIn[(f.y + 1) * TILE + f.x], a3 = sIn[(f.y + 1) * TILE + f.x + 1];
    float3 cur = clamp(sum / wsum, min(min(a0, a1), min(a2, a3)), max(max(a0, a1), max(a2, a3)));

    // how directly this frame measured this output pixel: 1 on top of an input sample
    float wNew = max(exp(-dot(o, o) / (2.0 * Tune0.w * Tune0.w)), Tune1.x);
    float3 outC = cur;
    float outW = wNew;

    if (!first) {
        float4 fl = Flow2.SampleLevel(LinearClamp, float3((u + 0.5) * 0.25 / float2(Size23.xy), z), 0);
        // where this pixel was last frame, in the history's pixels (made at last frame's region)
        float2 qp = (float2(q - prevOrg) + 0.5) + fl.xy * ScaleInfo.xy;
        if (all(qp >= 0.0) && all(qp <= float2(Region.zw))) {
            float4 h = SampleHistory(qp, z);
            float W = min(h.a, Tune0.x) * fl.z;     // flow confidence
            float3 mu = m1 * (1.0 / 9.0);
            float3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, 0.0));
            float3 ext = Tune0.y * sigma + 1.0 / 512.0;
            float3 bmin = max(mu - ext, ymin), bmax = min(mu + ext, ymax);
            float3 hy = ToYCoCg(h.rgb);
            // clip towards the neighbourhood mean (Salvi / Playdead)
            float3 ctrB = 0.5 * (bmin + bmax);
            float3 halfB = max(0.5 * (bmax - bmin), 1.0 / 1024.0);
            float3 r = hy - ctrB;
            float k = max(max(abs(r.x) / halfB.x, abs(r.y) / halfB.y), abs(r.z) / halfB.z);
            float3 hc = (k > 1.0) ? ctrB + r / k : hy;
            // history far outside the plausible range: disocclusion or bad motion -> forget it
            W *= saturate(1.0 + Tune1.w * (1.0 - k));
            float a = wNew / (W + wNew);
            outC = lerp(FromYCoCg(hc), cur, a);
            outW = W + wNew;
        }
    }
    HistOut[uint3(q - org, z)] = float4(outC, outW);
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    uint z = gid.z;
    const int2 org = int2(UnpackOrg(Ctrl.Load(12))), prevOrg = int2(UnpackOrg(Ctrl.Load(8)));
    int2 q0 = org + int2(gid.xy) * 16;                             // output pixels (region only)
    int2 qEnd = org + int2(Region.zw);
    int2 i0 = int2(floor((float2(q0) + 0.5) * ScaleInfo.zw - 0.5)) - 1;
    int2 inMax = int2(Size01.xy) - 1;
    for (uint k = gi; k < TILE * TILE; k += 64) {
        int2 p = clamp(i0 + int2(k % TILE, k / TILE), 0, inMax);
        sIn[k] = saturate(InX(Src.Load(int4(min(p + int2(SrcRect.xy), int2(SrcRect.zw) - 1), z + Misc.x, 0)).rgb));
    }
    GroupMemoryBarrierWithGroupSync();
    bool first = Ctrl.Load(0) != 0;
    [unroll] for (uint j = 0; j < 4; ++j) {
        int2 q = q0 + int2(gt.xy) + int2(j & 1, j >> 1) * 8;
        if (all(q < qEnd)) Accumulate(q, i0, z, first, org, prevOrg);
    }
}

// =============================================================================================
#elif TPASS == TP_OUT || TPASS == TP_OUTER || TPASS == TP_MARGIN

Texture2DArray<float4>   Hist : register(t0);
Texture2DArray<float4>   Src : register(t1);
ByteAddressBuffer        Ctrl : register(t2);   // OUT / OUTER: where the region is this frame (DECIDE)
RWTexture2DArray<float4> Proxy : register(u0);

static uint gZ;

#define TINT_CENTRE float3(1.0, 0.72, 1.0)
#define TINT_OUTER  float3(0.72, 0.86, 1.0)

int2 RegionOrg() { return int2(UnpackOrg(Ctrl.Load(12))); }

// Eye tracking markers, over the picture (perceptual colour): a green ring where the gaze is, and
// the calibration cross (white on a dark outline) straight ahead.
float3 Overlay(int2 g, float3 c)
{
    const float R = max(9.0, 0.013 * float(Size4Out.z));
    const float2 pc = float2(g) + 0.5;
    if (Marks.x != 0xffffffffu) {
        float e = abs(length(pc - float2(Marks.xy)) - R);
        c = lerp(c, 0.0, 0.7 * (1.0 - saturate(e - 2.5)));
        c = lerp(c, float3(0.25, 1.0, 0.35), 1.0 - saturate(e - 1.0));
    }
    if (Marks.z != 0xffffffffu) {
        float2 v = pc - float2(Marks.zw);
        float d = length(v);
        float arm = (d < 2.2 * R && d > 0.45 * R) ? min(abs(v.x), abs(v.y)) : 1e3;   // a cross with a gap
        float ring = abs(d - 1.5 * R);
        float dotc = d;                                                              // and a dot in the middle
        float e = min(min(arm, ring), dotc - 1.5);
        c = lerp(c, 0.0, 0.8 * (1.0 - saturate(e - 2.5)));
        c = lerp(c, float3(1.0, 1.0, 0.85), 1.0 - saturate(e - 1.0));
    }
    return c;
}

// This frame alone: Catmull-Rom (5 bilinear taps), or one bilinear tap with Bilinear edges;
// perceptual colour + alpha. t = input pixel coordinates (pixel i's centre at i) inside the source
// rectangle org..org+size, whose edges clamp the taps the way the edges of a texture of that size would.
float4 SpatialAt(float2 t, float2 org, float2 size)
{
    float2 inv = 1.0 / float2(SrcRect.zw);
    if (Misc.y & TF_BILINEAR) {
        float4 b = Src.SampleLevel(LinearClamp, float3((clamp(t + 0.5, 0.5, size - 0.5) + org) * inv, gZ + Misc.x), 0);
        return float4(saturate(InX(max(b.rgb, 0.0))), b.a);
    }
    float2 tc = floor(t);
    float2 f = t - tc;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 lo = 0.5, hi = size - 0.5;
    float2 p0 = (clamp(tc - 0.5, lo, hi) + org) * inv;
    float2 p3 = (clamp(tc + 2.5, lo, hi) + org) * inv;
    float2 p12 = (clamp(tc + 0.5 + w2 / w12, lo, hi) + org) * inv;
    float s = gZ + Misc.x;
    float4 ctr = Src.SampleLevel(LinearClamp, float3(p12, s), 0);
    float3 c = ctr.rgb * (w12.x * w12.y);
    float ws = w12.x * w12.y;
    c += Src.SampleLevel(LinearClamp, float3(p12.x, p0.y, s), 0).rgb * (w12.x * w0.y); ws += w12.x * w0.y;
    c += Src.SampleLevel(LinearClamp, float3(p0.x, p12.y, s), 0).rgb * (w0.x * w12.y); ws += w0.x * w12.y;
    c += Src.SampleLevel(LinearClamp, float3(p3.x, p12.y, s), 0).rgb * (w3.x * w12.y); ws += w3.x * w12.y;
    c += Src.SampleLevel(LinearClamp, float3(p12.x, p3.y, s), 0).rgb * (w12.x * w3.y); ws += w12.x * w3.y;
    return float4(saturate(InX(max(c / ws, 0.0))), ctr.a);
}

// at content output pixel g
float4 Spatial(uint2 g)
{
    return SpatialAt((float2(g) + 0.5) * ScaleInfo.zw - 0.5, float2(SrcRect.xy), float2(Size01.xy));
}

// at proxy pixel G, from the whole texture (outside the content rectangle)
float4 SpatialTex(int2 G)
{
    float2 s = float2(SrcRect.zw) / float2(DstRect.zw);
    return SpatialAt((float2(G) + 0.5) * s - 0.5, 0.0, float2(SrcRect.zw));
}

float SrcAlpha(uint2 g)
{
    float2 pos = clamp((float2(g) + 0.5) * ScaleInfo.zw, 0.5, float2(Size01.xy) - 0.5) + float2(SrcRect.xy);
    return Src.SampleLevel(LinearClamp, float3(pos / float2(SrcRect.zw), gZ + Misc.x), 0).a;
}

// 1 inside the rectangle (org, size) of the content, falling smoothly to 0 across the band at its
// inner edges (edges that lie on the border of the image need no blending); 0 outside it.
float RectWeight(int2 g, int2 org, int2 size)
{
    float band = float(Misc.w);
    if (band <= 0.0) return 1.0;
    int2 lo = org, hi = org + size - 1;
    float d = 1e6;
    if (org.x > 0) d = min(d, float(g.x - lo.x));
    if (org.x + size.x < int(Size4Out.z)) d = min(d, float(hi.x - g.x));
    if (org.y > 0) d = min(d, float(g.y - lo.y));
    if (org.y + size.y < int(Size4Out.w)) d = min(d, float(hi.y - g.y));
    return smoothstep(0.0, 1.0, (d + 0.5) / band);
}

// The sharpening area: centred on this frame's region, inside the content (origin, size)
int4 SharpRect()
{
    const int2 size = int2(Sharp.xy);
    const int2 mid = RegionOrg() + int2(Region.zw) / 2;
    return int4(clamp(mid - size / 2, int2(0, 0), int2(Size4Out.zw) - size), size);
}

float SharpWeight(int2 g)
{
    const int4 r = SharpRect();
    if (any(g < r.xy) || any(g >= r.xy + r.zw)) return 0.0;
    return RectWeight(g, r.xy, r.zw);
}

#if TPASS == TP_OUTER

#if SHARPEN
// The tile's spatial upscale with a 1-pixel apron (content pixels, clamped to the content), filled
// once per group when the tile meets the sharpening area, for RCAS
groupshared float4 gsT[18 * 18];
static int2 gsOrg;     // the content pixel at gsT[0]
static bool gsFilled;
AF4 FsrRcasLoadF(ASU2 p)
{
    const int2 q = clamp(int2(p) - gsOrg, 0, 17);
    return gsT[q.y * 18 + q.x];
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"
#endif

void Emit(uint2 gl)        // gl: the content output rectangle grown by the band (Strides.z) on every side
{
    int B = int(Strides.z);
    int2 G = int2(gl) + int2(DstRect.xy) - B;                         // proxy pixel
    if (any(G < 0) || any(G >= int2(DstRect.zw))) return;
    int2 g = G - int2(DstRect.xy);                                    // content output pixel
    if (any(g >= int2(Size4Out.zw) + B)) return;
    float4 c;
    if (all(g >= 0) && all(g < int2(Size4Out.zw))) {
        const int2 org = RegionOrg();
        if (all(g >= org) && all(g < org + int2(Region.zw))) return;   // OUT's
#if SHARPEN
        if (gsFilled) {   // RCAS over the upscale in the sharpening area
            const int2 q = g - gsOrg;
            c = gsT[q.y * 18 + q.x];
            const float ws = SharpWeight(g);
            if (ws > 0.0) {
                AF3 s3;
                FsrRcasF(s3.r, s3.g, s3.b, uint2(g), RcasCon);
                c.rgb = lerp(c.rgb, s3, ws);
            }
        } else
#endif
        c = Spatial(uint2(g));
    } else {
        c = SpatialTex(G);
    }
    if (Misc.y & TF_TINT) c.rgb *= TINT_OUTER;
    c.rgb = Overlay(g, c.rgb);
    Proxy[uint3(G, gZ + Misc.x)] = float4(OutX(c.rgb), c.a);
}

#elif TPASS == TP_MARGIN

void Emit(uint2 G)                                                 // G: proxy pixel
{
    if (any(G >= DstRect.zw)) return;
    int2 g = int2(G) - int2(DstRect.xy);
    if (all(g >= 0) && all(g < int2(Size4Out.zw))) return;            // the content: OUT / OUTER
    float4 c = SpatialTex(int2(G));
    if (Misc.y & TF_TINT) c.rgb *= TINT_OUTER;
    Proxy[uint3(G, gZ + Misc.x)] = float4(OutX(c.rgb), c.a);
}

#else

#if SHARPEN
static bool gRcasSpatial = false;   // RCAS over the spatial upscale (content pixels, computed here) instead of the history
AF4 FsrRcasLoadF(ASU2 p)
{
    if (gRcasSpatial)
        return Spatial(uint2(clamp(p, ASU2(0, 0), ASU2(Size4Out.zw) - ASU2(1, 1))));
    p = clamp(p, ASU2(0, 0), ASU2(Region.zw) - ASU2(1, 1));
    return Hist.Load(int4(p, gZ, 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"
#endif

// 1 inside the region, falling smoothly to 0 across the band at its inner edges (edges that lie on
// the border of the image need no blending).
float CentreWeight(uint2 g, int2 org)
{
    float band = float(Misc.w);
    if (band <= 0.0) return 1.0;
    int2 lo = org, hi = org + int2(Region.zw) - 1;
    float d = 1e6;
    if (org.x > 0) d = min(d, float(int(g.x) - lo.x));
    if (org.x + int(Region.z) < int(Size4Out.z)) d = min(d, float(hi.x - int(g.x)));
    if (org.y > 0) d = min(d, float(int(g.y) - lo.y));
    if (org.y + int(Region.w) < int(Size4Out.w)) d = min(d, float(hi.y - int(g.y)));
    return smoothstep(0.0, 1.0, (d + 0.5) / band);
}

void Emit(uint2 p)                                                 // p: region pixel
{
    if (any(p >= Region.zw)) return;
    const int2 org = RegionOrg();
    uint2 g = p + uint2(org);
#if SHARPEN
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, p, RcasCon);
#else
    float3 c = Hist.Load(int4(p, gZ, 0)).rgb;
#endif
    float t = CentreWeight(g, org);
    float a;
    if (t < 1.0) {
        float4 sp = Spatial(g);
#if SHARPEN
        // the sharpening area reaches past the region: blend into the sharpened upscale OUTER writes next to it
        const float ws = (Misc.y & TF_SHARPOUT) ? SharpWeight(int2(g)) : 0.0;
        if (ws > 0.0) {
            AF3 s3;
            gRcasSpatial = true;
            FsrRcasF(s3.r, s3.g, s3.b, g, RcasCon);
            gRcasSpatial = false;
            sp.rgb = lerp(sp.rgb, s3, ws);
        }
#endif
        c = lerp(sp.rgb, c, t);
        a = sp.a;
    } else {
        a = SrcAlpha(g);
    }
    if (Misc.y & TF_TINT) c *= lerp(TINT_OUTER, TINT_CENTRE, t);
    c = Overlay(int2(g), c);
    Proxy[uint3(g + DstRect.xy, gZ + Misc.x)] = float4(OutX(saturate(c)), a);
}

#endif

[numthreads(64, 1, 1)]
void main(uint3 lt : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    gZ = gid.z;
#if TPASS == TP_OUTER
    OuterTiles ot = GetOuterTiles(uint2(RegionOrg()));
    uint ti = gid.y * ot.tx + gid.x;
#if SHARPEN
    // a tile that meets the sharpening area: its upscale (and the apron) to groupshared memory first,
    // so that RCAS reads each pixel once computed (every thread reaches the barrier)
    const bool valid = ti < OuterCount(ot);
    const int2 T = int2(OuterTile(ot, valid ? ti : 0u) << 4u) - int(Strides.z);   // content pixel of the tile's corner
    const int4 sr = SharpRect();
    gsOrg = T - 1;
    gsFilled = valid && (Misc.y & TF_SHARPOUT) != 0 && T.x - 1 < sr.x + sr.z && T.x + 17 > sr.x && T.y - 1 < sr.y + sr.w &&
               T.y + 17 > sr.y;
    if (gsFilled)
        for (uint i = lt.x; i < 18u * 18u; i += 64u)
            gsT[i] = Spatial(uint2(clamp(gsOrg + int2(i % 18u, i / 18u), int2(0, 0), int2(Size4Out.zw) - 1)));
    GroupMemoryBarrierWithGroupSync();
    if (!valid) return;
#else
    if (ti >= OuterCount(ot)) return;
#endif
    AU2 gxy = ARmp8x8(lt.x) + (OuterTile(ot, ti) << 4u);
#else
    AU2 gxy = ARmp8x8(lt.x) + AU2(gid.x << 4u, gid.y << 4u);
#endif
    Emit(gxy);
    gxy.x += 8u;
    Emit(gxy);
    gxy.y += 8u;
    Emit(gxy);
    gxy.x -= 8u;
    Emit(gxy);
}

// =============================================================================================
#elif TPASS == TP_FXAA

// FXAA ([upscale] fxaa) over the whole texture, the slices of this run; dispatched indirectly
// (DECIDE: whenever this run writes the proxy). FXSTAGE 0, before the upscale: the source -> a copy
// of it, in its own encoding, that every pass after FP reads instead. FXSTAGE 1, after it: what OUT,
// OUTER and MARGIN wrote (into a copy of the proxy) -> the proxy (XFORM: the proxy's encoding).
Texture2DArray<float4>   FxIn : register(t0);
RWTexture2DArray<float4> FxOut : register(u0);
static uint gFxZ;
float3 FxColor(int2 p) { return saturate(InX(FxIn.Load(int4(p, gFxZ, 0)).rgb)); }
#include "fxaa.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
#if FXSTAGE == 0
    const uint2 size = SrcRect.zw;
#else
    const uint2 size = DstRect.zw;
#endif
    if (any(id.xy >= size)) return;
    gFxZ = id.z + Misc.x;
    gFxLo = int2(0, 0);
    gFxHi = int2(size) - 1;
    int2 q;
    const float w = FxaaBlend(int2(id.xy), q);
    const float4 c = FxIn.Load(int4(id.xy, gFxZ, 0));
#if XFORM == 2
    const float3 o = lerp(c.rgb, FxIn.Load(int4(q, gFxZ, 0)).rgb, w);
#else
    const float3 o = w > 0.0 ? InvX(lerp(FxColor(int2(id.xy)), FxColor(q), w)) : c.rgb;
#endif
    FxOut[uint3(id.xy, gFxZ)] = float4(o, c.a);
}

// =============================================================================================
#elif TPASS == TP_COPY

// The motion passes only ever read L1..L4 of the previous frame, and only around the region.
Texture2DArray<float>   C1 : register(t0);
Texture2DArray<float>   C2 : register(t1);
Texture2DArray<float>   C3 : register(t2);
Texture2DArray<float>   C4 : register(t3);
Texture2DArray<float4>  FlowCur : register(t4);
RWTexture2DArray<float> P1 : register(u0);
RWTexture2DArray<float> P2 : register(u1);
RWTexture2DArray<float> P3 : register(u2);
RWTexture2DArray<float> P4 : register(u3);
RWTexture2DArray<float4> FlowPrevOut : register(u4);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // each level's built part (WorkLo .. WorkHi), from this thread's place in it
    uint3 i1 = uint3(WorkLo(1) + id.xy, id.z), i2 = uint3(WorkLo(2) + id.xy, id.z);
    uint3 i3 = uint3(WorkLo(3) + id.xy, id.z), i4 = uint3(WorkLo(4) + id.xy, id.z);
    if (all(i2.xy < WorkHi(2))) FlowPrevOut[i2] = FlowCur.Load(int4(i2, 0));
    if (all(i1.xy < WorkHi(1))) P1[i1] = C1.Load(int4(i1, 0));
    if (all(i2.xy < WorkHi(2))) P2[i2] = C2.Load(int4(i2, 0));
    if (all(i3.xy < WorkHi(3))) P3[i3] = C3.Load(int4(i3, 0));
    if (all(i4.xy < WorkHi(4))) P4[i4] = C4.Load(int4(i4, 0));
}

#endif
