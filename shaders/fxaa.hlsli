// FXAA ([upscale] fxaa), written after Timothy Lottes' FXAA 3.11 (NVIDIA): its quality path at
// preset 12 (edge search steps 1, 1.5, 2, 4, 12) with the default settings (subpixel 0.75, edge
// threshold 0.166, darkest edges 0.0833), on perceptual colour with Rec. 601 luma.
//
// The including pass defines FxColor(int2 p): the perceptual colour of pixel p (p is in range), and
// sets gFxLo / gFxHi, the pixels it may read (inclusive; reads further out are clamped). FxaaBlend
// says how far the pixel's colour moves towards which neighbour; the pass mixes the two (in the
// display encoding, as FXAA does; HDR in linear light, which the perceptual encoding would darken).

static int2 gFxLo, gFxHi;

float FxLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float FxLumaAt(int2 p) { return FxLuma(FxColor(clamp(p, gFxLo, gFxHi))); }

// luma between pixels (pixel coordinates, centres at + 0.5), as FXAA's filtered fetches see it
float FxLumaBilinear(float2 pos)
{
    const float2 t = pos - 0.5;
    const int2 i = int2(floor(t));
    const float2 f = t - float2(i);
    const float a = FxLumaAt(i), b = FxLumaAt(i + int2(1, 0)), c = FxLumaAt(i + int2(0, 1)), d = FxLumaAt(i + int2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float FxaaBlend(int2 p, out int2 q)
{
    q = p;
    const float lumaM = FxLuma(FxColor(p));
    float lumaN = FxLumaAt(p + int2(0, -1)), lumaS = FxLumaAt(p + int2(0, 1));
    const float lumaW = FxLumaAt(p + int2(-1, 0)), lumaE = FxLumaAt(p + int2(1, 0));
    const float rangeMax = max(max(max(lumaN, lumaS), max(lumaW, lumaE)), lumaM);
    const float rangeMin = min(min(min(lumaN, lumaS), min(lumaW, lumaE)), lumaM);
    const float range = rangeMax - rangeMin;
    if (range < max(0.0833, rangeMax * 0.166)) return 0.0;   // no edge here

    const float lumaNW = FxLumaAt(p + int2(-1, -1)), lumaNE = FxLumaAt(p + int2(1, -1));
    const float lumaSW = FxLumaAt(p + int2(-1, 1)), lumaSE = FxLumaAt(p + int2(1, 1));
    // is the edge horizontal or vertical?
    const float edgeHorz = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaN + lumaS - 2.0 * lumaM) +
                           abs(lumaNE + lumaSE - 2.0 * lumaE);
    const float edgeVert = abs(lumaNW + lumaNE - 2.0 * lumaN) + 2.0 * abs(lumaW + lumaE - 2.0 * lumaM) +
                           abs(lumaSW + lumaSE - 2.0 * lumaS);
    const bool horz = edgeHorz >= edgeVert;
    // how much this pixel stands out from its 3x3 (the subpixel part)
    const float avg = (2.0 * (lumaN + lumaS + lumaW + lumaE) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    const float subC = saturate(abs(avg - lumaM) / range);
    const float subF = (3.0 - 2.0 * subC) * subC * subC;
    const float subH = subF * subF * 0.75;

    // the neighbour across the edge: the side with the larger step
    if (!horz) { lumaN = lumaW; lumaS = lumaE; }
    const float gradN = lumaN - lumaM, gradS = lumaS - lumaM;
    const bool pairN = abs(gradN) >= abs(gradS);
    const float gradient = max(abs(gradN), abs(gradS));
    const float lengthSign = pairN ? -1.0 : 1.0;
    const float lumaNN = (pairN ? lumaN : lumaS) + lumaM;   // the two pixels across the edge, summed

    // walk along the edge, half a pixel over to it, until its luma leaves the pair's average
    const float2 pM = float2(p) + 0.5;
    const float2 along = horz ? float2(1.0, 0.0) : float2(0.0, 1.0);
    const float2 pB = pM + (horz ? float2(0.0, 0.5 * lengthSign) : float2(0.5 * lengthSign, 0.0));
    const float scaled = gradient * 0.25;
    const float steps[5] = { 1.0, 1.5, 2.0, 4.0, 12.0 };
    float2 pN = pB - along * steps[0], pP = pB + along * steps[0];
    float endN = FxLumaBilinear(pN) - lumaNN * 0.5, endP = FxLumaBilinear(pP) - lumaNN * 0.5;
    bool doneN = abs(endN) >= scaled, doneP = abs(endP) >= scaled;
    [unroll] for (int k = 1; k < 5; ++k) {
        if (doneN && doneP) break;
        const bool stepN = !doneN, stepP = !doneP;
        if (stepN) pN -= along * steps[k];
        if (stepP) pP += along * steps[k];
        if (k == 4) break;   // (the last step is taken without looking: the edge goes on)
        if (stepN) { endN = FxLumaBilinear(pN) - lumaNN * 0.5; doneN = abs(endN) >= scaled; }
        if (stepP) { endP = FxLumaBilinear(pP) - lumaNN * 0.5; doneP = abs(endP) >= scaled; }
    }
    const float dstN = horz ? pM.x - pN.x : pM.y - pN.y, dstP = horz ? pP.x - pM.x : pP.y - pM.y;
    const bool mBelow = lumaM - lumaNN * 0.5 < 0.0;
    const bool nearN = dstN < dstP;
    // the end nearest this pixel must be where the edge really ends (its luma crosses back)
    const bool good = ((nearN ? endN : endP) < 0.0) != mBelow;
    const float offset = good ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;
    const float o = max(offset, subH) * lengthSign;
    // between this pixel and the one across the edge
    q = clamp(p + (horz ? int2(0, o >= 0.0 ? 1 : -1) : int2(o >= 0.0 ? 1 : -1, 0)), gFxLo, gFxHi);
    return abs(o);
}
