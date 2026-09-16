// Nemo native effect contract (spec section 10.4, issue #8; operations and
// masking per issue #75; contribution-local module per issue #83)
// ---------------------------------------------------------
// name: merge
// version: 3
// ports:
//   - in_a : image2D input, port A (background / base)
//   - in_b : image2D input, port B (foreground / source)
//   - in_mask : image2D input, optional port 2 (mask; absent binds a valid
//     dummy descriptor and reports maskPresent = 0, never a manufactured
//     source)
//   - out_color : image2D output (native channel image, issue #98: packed
//     RGBA32F at the logical raster for four stored channels, otherwise
//     R32_SFLOAT scalar planes at W*(H*C); straight alpha, scene-linear)
// parameters:
//   - operation = "over" | "plus" | "multiply" | "screen" | "difference"
//     (over is the default; an unknown value is rejected by the schema and
//     by both executors, never a silent fallback). The code travels in
//     op.x: 0 Over, 1 Plus, 2 Multiply, 3 Screen, 4 Difference.
//   - mask = (maskChannel, invertMask, mix, maskPresent), the shared effect
//     mask word.
// bindings (native binding contract v7, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.merge.payload.v1")
//   set 0, binding 2 : per-input spatial geometry (A, B, mask)
//   set 1, binding 0 = A, binding 1 = B, binding 2 = mask — swapping A/B is a
//     different result and fails declared image comparison
//   set 2, binding 0 : output image2D
// dependencies:
//   region: output pixel (x, y) depends only on the input pixels holding the
//     same full-resolution sample; each input's raster origin and extent are
//     bound with it (region evaluation, wider cache-backed inputs, signed or
//     beyond-format origins, mixed formats), so no neighborhood reads and no
//     support-region expansion. A sample outside an operand's raster is outside
//     that operand's data and reads as transparent black (issue #88): an
//     operand may hold less than the request, or nothing at all.
//   channels: RGBA uniform.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight (non-premultiplied), identical to the CPU reference.
//   Over keeps its existing expression exactly: out.rgb = fg.a*fg.rgb +
//   (1 - fg.a)*bg.rgb. The extended modes interpolate background -> blend
//   target by foreground alpha: Plus target bg+fg, Multiply bg*fg, Screen
//   1-(1-bg)*(1-fg), Difference |bg-fg|. Alpha is op-independent:
//   out.a = fg.a + (1 - fg.a)*bg.a. Scene-linear RGB is never clamped; the
//   final result is bg + coverage*mix*(composite - bg) with coverage 1 when
//   the mask is absent or None. Premultiplied interpretation is a declared
//   comparison failure, not an alternate path.

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y; signed)
    int4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // support: half-open RASTER-INDEX rect (x, y, width, height) of the samples
    // that ARE this image's data; (-1,-1,-1,-1) means the raster declares no
    // support (a node-local scratch), a non-positive size means every sample is
    // transparent black. Samples outside it must be written transparent black.
    int4 support;
    // channels: the produced raster's channel facts (issues #90, #98):
    // (stored channel count, components per texel — 4 = packed four-channel
    // RGBA32F at the logical raster, 1 = R32_SFLOAT scalar planes — whether the
    // channel plan fills any stored channel, 0).
    uint4 channels;
    // rgba: the produced STORED channel of each R/G/B/A role; -1 = role absent.
    // The four stored channels of a packed image are NOT assumed to be R/G/B/A.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer MergePayload {
    // (maskChannel, invertMask, mix, maskPresent)
    float4 mask;
    // x = operation code
    float4 op;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_a;
[[vk::binding(1, 1)]]
RWTexture2D<float4> in_b;
// Optional mask slot (port 2). An absent mask binds a valid dummy
// descriptor with maskPresent = 0, so the read is always in bounds.
[[vk::binding(2, 1)]]
RWTexture2D<float4> in_mask;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    // The described image's data support (native binding contract v7): a sample
    // outside it is transparent black, never a composite of operands that hold
    // nothing there, and every stored channel — auxiliary ones included — is
    // initialized
    // (issue #90).
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), int(meta2.y), channels.y, channels.x);
        return;
    }

    // Same-lattice inputs: the pixels holding the same full-resolution sample
    // as this pass output pixel, located through each input's own origin and
    // extent. A pixel outside an operand's raster is outside its data (a
    // smaller, differently anchored or empty data window): transparent black,
    // never an out-of-bounds load.
    int2 aPixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    int2 aExtent = int2(inputGeometry[0].extent.xy);
    int2 bPixel = int2(p) + inputGeometry[1].regionAndOffset.zw;
    int2 bExtent = int2(inputGeometry[1].extent.xy);
    bool aInside = aPixel.x >= 0 && aPixel.y >= 0 && aPixel.x < aExtent.x && aPixel.y < aExtent.y;
    bool bInside = bPixel.x >= 0 && bPixel.y >= 0 && bPixel.x < bExtent.x && bPixel.y < bExtent.y;

    // Port A is the background/base and port B the foreground/source: each is
    // gathered through the plane roles of its own geometry entry (issue #90).
    float4 bg = aInside ? gpuLoadRgba(in_a, aPixel, inputGeometry[0].rgba, aExtent.y, inputGeometry[0].channels.y)
                        : float4(0.0, 0.0, 0.0, 0.0);
    float4 fg = bInside ? gpuLoadRgba(in_b, bPixel, inputGeometry[1].rgba, bExtent.y, inputGeometry[1].channels.y)
                        : float4(0.0, 0.0, 0.0, 0.0);

    int operation = int(op.x);
    float4 composite;
    if (operation == 0) {
        // Over keeps the CPU reference's existing expression exactly.
        composite.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    } else {
        float3 target;
        if (operation == 1) { target = bg.xyz + fg.xyz; }
        else if (operation == 2) { target = bg.xyz * fg.xyz; }
        else if (operation == 3) { target = float3(1.0, 1.0, 1.0) - (float3(1.0, 1.0, 1.0) - bg.xyz) * (float3(1.0, 1.0, 1.0) - fg.xyz); }
        else { target = abs(bg.xyz - fg.xyz); }   // Difference
        composite.xyz = bg.xyz + fg.a * (target - bg.xyz);
    }
    // Alpha is operation-independent, as in current Over.
    composite.w = fg.a + (1.0 - fg.a) * bg.a;

    // Shared mask/mix coverage: absent mask or None has full coverage
    // regardless of inversion; otherwise the clamped selected stored channel
    // is optionally inverted. Scene-linear RGB is never clamped.
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        int2 maskPixel = int2(p) + inputGeometry[2].regionAndOffset.zw;
        int2 maskExtent = int2(inputGeometry[2].extent.xy);
        bool maskInside =
            maskPixel.x >= 0 && maskPixel.y >= 0 && maskPixel.x < maskExtent.x && maskPixel.y < maskExtent.y;
        float selected =
            maskInside ? clamp(gpuLoadRgba(in_mask, maskPixel, inputGeometry[2].rgba, maskExtent.y,
                                           inputGeometry[2].channels.y)[channel], 0.0, 1.0)
                       : 0.0;
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the background, weight 1 the
    // unmasked composite, so Mix 0 or zero coverage returns the background.
    float weight = coverage * mask.z;
    float4 result = weight <= 0.0 ? bg : (weight >= 1.0 ? composite : lerp(bg, composite, weight));
    // The background is this node's main input: every stored channel the composite did
    // not write keeps its named channel from A at the same coordinate (#90).
    gpuStorePixel(out_color, int2(p), int(meta2.y), channels.x, channels.y, channels.z, rgba, result, in_a, aExtent.y,
                  inputGeometry[0].channels.y);
}
