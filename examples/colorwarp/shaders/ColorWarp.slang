// ColorWarp (issue #37) — installed package kernel, Slang -> SPIR-V.
// ---------------------------------------------------------
// name: ColorWarp
// payload: "org.nemo.colorwarp.payload.v1", 592 bytes (manifest gpu.payloadBytes)
//   set 0, binding 1: float4 control[36] = (hue displacement, saturation
//     displacement, 0, 0) in cell coordinates, stable index i = (r-1)*12 + h for
//     ring r = 1..3 and spoke h = 0..11; then float4 settings = (strength,
//     flags, identity, 0).
// bindings (nemo.native.bindings.v8, the shared support this kernel includes):
//   set 0, binding 0 : common request uniforms (meta is signed)
//   set 0, binding 1 : this package's payload
//   set 0, binding 2 : per-input spatial geometry (the main input)
//   set 0, binding 3 : the produced raster's channel plan
//   set 1, binding 0 : main input image
//   set 2, binding 0 : produced raster
// dependencies:
//   region: pointwise. Output pixel (x, y) reads only the input pixel holding
//     the same full-resolution sample; the input's own raster origin and extent
//     are bound with it, and a sample outside that raster is transparent black.
//   channels: RGBA roles only. Every other stored channel of the produced raster
//     is preserved at unchanged coordinates by the shared channel plan, and the
//     roles this raster does not store are never manufactured (gpuStorePixel).
//   temporal: none. The mesh is resolved parameters, so animation arrives as a
//     different payload rather than as a kernel branch.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight (non-premultiplied); flags bit 0 makes the sample premultiplied
//   and the kernel unassociates and re-associates it explicitly. An alpha of
//   exactly zero keeps the sample's own values.
//
// The wheel math is the contract's, in float: luma Y = .2126R + .7152G + .0722B,
// opponent u = (2R-G-B)/sqrt(6), v = (G-B)/sqrt(2), chroma c = hypot(u, v),
// scale s = 1 + |Y|, radius = c/(s+c), hue = atan2(v,u)/(2pi) mod 1, cell
// coordinates U = 12*hue and V = 4*radius. The mesh displaces (U, V) by
// strength * the smoothstep-tensor interpolated knot displacement, and the
// mapped position decodes back through the same luma. The CPU reference is an
// independent implementation of the same contract in double (ADR-0004): it is
// the same semantics, not shared code.
//
// Two float properties are deliberate and match the CPU:
//   * the inverse uses the complement s/(s+c) directly, not 1 - radius, because
//     c/(s+c) saturates at exactly 1 for a large finite chroma and 1 - radius
//     would then divide by zero for a perfectly representable input;
//   * a pixel whose interpolated displacement is exactly zero — the neutral
//     axis, and every point of the fixed boundary rings — is returned unchanged,
//     so the identity is exact instead of a round trip through the opponent
//     basis.
//
// Nothing is clamped. A sample that is not a number has no color to map, and a
// mapped position the wheel cannot represent has no value: both write the
// not-a-number marker rather than a substituted, clamped or echoed color. This
// kernel has no per-pixel error channel (see the package README, "CPU and GPU
// disagreement"): the validator, which prepare() re-runs before any payload
// exists, is the refusal gate for a mesh, and the CPU path reports the same
// unrepresentable result as a diagnostic.

#include "GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y; signed)
    int4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // support: half-open RASTER-INDEX rect of the samples that ARE this image's
    // data; (-1,-1,-1,-1) declares no support (a node-local scratch).
    int4 support;
    // channels: (stored channel count, components per texel, whether the channel
    // plan fills any stored channel, 0).
    uint4 channels;
    // rgba: the produced raster's stored channel of each R,G,B,A role; -1 absent.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer ColorWarpPayload {
    // (hue displacement, saturation displacement, 0, 0) per editable point, in
    // cell coordinates and stable index order.
    float4 control[36];
    // (strength, flags, identity, 0).
    float4 settings;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

static const int kSpokeCount = 12;
static const int kBoundaryRing = 4;
static const float kLumaR = 0.2126;
static const float kLumaG = 0.7152;
static const float kLumaB = 0.0722;
static const float kInvSqrt6 = 0.4082482904638631;
static const float kInvSqrt2 = 0.7071067811865476;
static const float kSqrtTwoThirds = 0.8164965809277260;
static const float kTwoPi = 6.2831853071795865;
static const float kInvTwoPi = 0.15915494309189535;
static const float kMaxFiniteFloat = 3.402823466e38;

// The not-a-number marker: a mapped position with no representable value is
// reported as "not a number", never as an invented, clamped or echoed color.
float colorWarpNaN()
{
    return asfloat(0x7FC00000u);
}

// hypot without the overflow: scaling by the larger magnitude keeps the result
// finite for the same inputs the CPU's hypot handles, so the two backends read
// the same wheel position.
float colorWarpHypot(float u, float v)
{
    const float magnitude = max(abs(u), abs(v));
    if (!(magnitude > 0.0) || magnitude > kMaxFiniteFloat) { return magnitude; }
    return magnitude * length(float2(u / magnitude, v / magnitude));
}

float colorWarpSmoothstep(float t)
{
    return t * t * (3.0 - 2.0 * t);
}

// One knot's authored displacement. The centre (ring 0) and the outer boundary
// (ring 4) are fixed at zero; spokes wrap.
float2 colorWarpKnot(int spoke, int ring)
{
    if (ring <= 0 || ring >= kBoundaryRing) { return float2(0.0, 0.0); }
    const int wrapped = ((spoke % kSpokeCount) + kSpokeCount) % kSpokeCount;
    return control[(ring - 1) * kSpokeCount + wrapped].xy;
}

// The displacement field: tensor (smoothstep-weighted bilinear) interpolation of
// one cell's four knots, C1 across every edge. `v` is used as given — the
// encoding above produces [0, 4], and the fixed boundary ring is the identity
// there — so no value is clamped.
float2 colorWarpDisplacement(float u, float v)
{
    const int cellU = int(floor(u));
    const int cellV = min(int(floor(v)), kBoundaryRing - 1);
    const float su = colorWarpSmoothstep(u - floor(u));
    const float sv = colorWarpSmoothstep(v - float(cellV));
    const float2 bottomLeft = colorWarpKnot(cellU, cellV);
    const float2 bottomRight = colorWarpKnot(cellU + 1, cellV);
    const float2 topLeft = colorWarpKnot(cellU, cellV + 1);
    const float2 topRight = colorWarpKnot(cellU + 1, cellV + 1);
    const float w0 = (1.0 - su) * (1.0 - sv);
    const float w1 = su * (1.0 - sv);
    const float w2 = (1.0 - su) * sv;
    const float w3 = su * sv;
    return w0 * bottomLeft + w1 * bottomRight + w2 * topLeft + w3 * topRight;
}

// The mapping, on straight (non-premultiplied) scene-linear RGB. Returns the
// sample itself when the interpolated displacement is exactly zero, and the
// not-a-number marker when the wheel has no representable value at the mapped
// position or the mapped position has no finite float value of its own.
float3 colorWarpStraight(float3 rgb)
{
    const float3 undefined = float3(colorWarpNaN(), colorWarpNaN(), colorWarpNaN());
    const float luma = kLumaR * rgb.x + kLumaG * rgb.y + kLumaB * rgb.z;
    const float opponentU = (2.0 * rgb.x - rgb.y - rgb.z) * kInvSqrt6;
    const float opponentV = (rgb.y - rgb.z) * kInvSqrt2;
    const float chroma = colorWarpHypot(opponentU, opponentV);
    if (chroma == 0.0) { return rgb; }  // the neutral axis is fixed: no invented hue
    const float scale = 1.0 + abs(luma);
    const float total = scale + chroma;
    float hue = atan2(opponentV, opponentU) * kInvTwoPi;  // (-0.5, 0.5]
    if (hue < 0.0) { hue += 1.0; }                        // [0, 1)
    const float cellU = hue * float(kSpokeCount);                // [0, 12)
    const float cellV = (chroma / total) * float(kBoundaryRing);  // [0, 4)
    if (!(abs(cellU) <= kMaxFiniteFloat) || !(abs(cellV) <= kMaxFiniteFloat)) { return undefined; }
    const float2 displacement = colorWarpDisplacement(cellU, cellV);
    if (displacement.x == 0.0 && displacement.y == 0.0) { return rgb; }  // exact identity
    // 1 - mapped radius, from the stable complement rather than from 1 - radius.
    const float complement = scale / total - settings.x * displacement.y / float(kBoundaryRing);
    if (!(complement > 0.0)) { return undefined; }
    const float mappedChroma = scale * (1.0 - complement) / complement;
    float mappedTurns = (cellU + settings.x * displacement.x) / float(kSpokeCount);
    mappedTurns -= floor(mappedTurns);  // [0, 1): the hue axis is periodic
    const float mappedU = mappedChroma * cos(kTwoPi * mappedTurns);
    const float mappedV = mappedChroma * sin(kTwoPi * mappedTurns);
    const float q0 = kSqrtTwoThirds * mappedU;
    const float q1 = -mappedU * kInvSqrt6 + mappedV * kInvSqrt2;
    const float q2 = -mappedU * kInvSqrt6 - mappedV * kInvSqrt2;
    // The same luma is restored exactly: the correction is orthogonal to the
    // opponent plane, so the mapping never changes Y.
    const float correction = luma - (kLumaR * q0 + kLumaG * q1 + kLumaB * q2);
    return float3(q0 + correction, q1 + correction, q2 + correction);
}

// One sample: bypass/identity pass it through untouched, premultiplication is
// unassociated and re-associated explicitly, alpha is never warped.
float4 colorWarpPixel(float4 source)
{
    const int flags = int(settings.y);
    const bool premultiplied = (flags & 1) != 0;
    // identity: every displacement is zero, or strength is zero.
    if (settings.z != 0.0) { return source; }
    // bypass bit: an explicit bypass, and a data-only or incomplete-RGB raster,
    // whose meaning a color effect must not invent.
    if ((flags & 2) != 0) { return source; }
    const float alpha = source.w;
    // A transparent premultiplied sample keeps its own values: there is no
    // unassociated color to warp and none is invented.
    if (premultiplied && alpha == 0.0) { return source; }
    // A sample that is not a number has no color to map.
    if (!all(abs(source.xyz) <= kMaxFiniteFloat)) {
        return float4(colorWarpNaN(), colorWarpNaN(), colorWarpNaN(), source.w);
    }
    const float3 straight = premultiplied ? source.xyz / alpha : source.xyz;
    const float3 mapped = colorWarpStraight(straight);
    return float4(premultiplied ? mapped * alpha : mapped, source.w);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);

    // The described image's data support: a sample outside it is transparent
    // black, and every stored channel — auxiliary ones included — is
    // initialized on that path.
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.y, channels.x);
        return;
    }

    // Same-lattice input: the pixel holding the same full-resolution sample,
    // located through the input's own raster origin and extent. A sample the
    // input does not hold is outside its data (a smaller or empty data window):
    // transparent black, never an out-of-bounds load.
    const int2 mainPixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    const int2 mainExtent = int2(inputGeometry[0].extent.xy);
    const bool mainInside =
        mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    const float4 source =
        mainInside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, mainExtent.y, inputGeometry[0].channels.y)
                   : float4(0.0, 0.0, 0.0, 0.0);

    const float4 result = colorWarpPixel(source);
    // Every stored channel this pass's own math did not write keeps its named
    // channel from the main input at the SAME coordinate (issue #90): an
    // auxiliary or data layer survives the warp instead of being projected away.
    gpuStorePixel(out_color, int2(p), planeHeight, channels.x, channels.y, channels.z, rgba, result, in_main,
                  int(mainExtent.y), inputGeometry[0].channels.y);
}
