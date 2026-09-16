// Nemo native effect contract (spec section 10.4, issue #34; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: blur (pass 2 of 2: vertical support + final result)
// version: 2
// ports:
//   - in_scratch : image2D input (intermediate written by blurHorizontal)
//   - in_main : image2D input (original main image, for the mask/mix blend)
//   - in_mask : image2D input (optional mask; bound to the main image as a
//     valid dummy descriptor with maskPresent=0 when unconnected)
//   - out_color : image2D output (native R32_SFLOAT channel planes, straight
//     alpha, scene-linear: logical pixel (x, y) of channel plane c at
//     (x, y + c*logicalHeight), issue #90)
// bindings (native binding contract v6, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms (this pass's coverage)
//   set 0, binding 1 : node-local payload ("nemo.nodes.blur.payload.v1")
//   set 0, binding 2 : per-input spatial geometry (scratch, main, mask)
//   set 1, binding 0 = scratch, 1 = main, 2 = mask
//   set 2, binding 0 : output image2D
//   set 3, binding 0 : normalized Gaussian weights (index i + raster support)
// dependencies:
//   region: output pixel (x, y) depends on the vertical neighborhood of the
//     intermediate (clamped to the scratch raster, which covers the main
//     input's whole Y range) plus the original pixel holding the same
//     full-resolution sample, located through the main input's own raster
//     origin and extent. Clamp-to-edge stays the border of the actual image;
//     a pixel outside a bound raster is outside that image's data and reads as
//     transparent black (issue #88), and an empty raster has no pixels at all.
//   channels: RGBA/RGB/Alpha per the declared `channels` choice.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: after both passes RGBA is unpremultiplied once (zero alpha -> zero
//   RGB) before the single mask/mix blend against the original pixel; the
//   mask stays in output-raster space and is never transformed. RGB and
//   Alpha modes preserve the unselected channels exactly.
//
// The identity selection (size 0) runs this same program with the original
// main image bound to the scratch slot: the blur.x <= 0 branch never reads
// it, so size 0 is an exact, single-pass identity.

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y; signed)
    int4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // support: half-open RASTER-INDEX rect (x, y, width, height) of the samples
    // that ARE this image's data; (-1,-1,-1,-1) means the raster declares no
    // support (a node-local scratch), a non-positive size means every sample is
    // transparent black. Samples outside it must be written transparent black.
    int4 support;
    // channels: the produced raster's channel planes (issue #90):
    // (plane count, 0, 0, 0).
    uint4 channels;
    // rgba: the produced plane index of each R/G/B/A role; -1 = role absent.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer BlurPayload {
    // (maskChannel, invertMask, mix, maskPresent)
    float4 mask;
    // (size full-res, channels bitmask, raster support, 0)
    float4 blur;
};

// Binding 0 is the ORIGINAL main image, not the filtered intermediate: the
// final pass both reads the original for the mask/mix blend and preserves the
// named channels this pass does not filter from it (issue #90).
[[vk::binding(0, 1)]]
RWTexture2D<float> in_main;
[[vk::binding(1, 1)]]
RWTexture2D<float> in_scratch;
[[vk::binding(2, 1)]]
RWTexture2D<float> in_mask;

[[vk::binding(0, 2)]]
RWTexture2D<float> out_color;

// Normalized Gaussian weights precomputed once per Blur preparation (index
// i+support); the kernel never evaluates exp() per output pixel.
[[vk::binding(0, 3)]]
StructuredBuffer<float> blurWeights;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    const int planeHeight = int(meta2.y);
    // The described image's data support (native binding contract v6): a sample
    // outside it is transparent black, and every plane — auxiliary ones
    // included — is initialized (issue #90). This is the pass that writes the
    // node result; the horizontal pass writes a scratch and declares no support.
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.x);
        return;
    }

    int mode = int(blur.y);
    // The original main image (binding 0) and the scratch intermediate (binding
    // 1) each cover their own rectangle of the lattice; the scratch's Y extent
    // is the row range the image's clamp-to-edge border lives on. A pixel
    // outside either raster is outside that image's data, and an empty raster
    // has no pixels at all: transparent black (issue #88), never an
    // out-of-bounds load.
    int2 mainPixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    int2 mainExtent = int2(inputGeometry[0].extent.xy);
    bool mainInside = mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    int2 scratchOffset = inputGeometry[1].regionAndOffset.zw;
    int2 scratchExtent = int2(inputGeometry[1].extent.xy);
    float4 orig = mainInside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, mainExtent.y)
                             : float4(0.0, 0.0, 0.0, 0.0);
    float4 processed;
    if (blur.x <= 0.0) {
        processed = orig;  // exact identity (both passes are identity)
    } else if (scratchExtent.x <= 0 || scratchExtent.y <= 0) {
        processed = float4(0.0, 0.0, 0.0, 0.0);
    } else {
        int taps = int(blur.z);

        float4 acc = float4(0.0, 0.0, 0.0, 0.0);
        for (int i = -taps; i <= taps; ++i) {
            float w = blurWeights[i + taps];
            int2 tap = int2(clamp(int(p.x) + scratchOffset.x, 0, scratchExtent.x - 1),
                            clamp(int(p.y) + i + scratchOffset.y, 0, scratchExtent.y - 1));
            acc += w * gpuLoadRgba(in_scratch, tap, inputGeometry[1].rgba, scratchExtent.y);
        }

        if (mode == 15) {  // RGBA: unpremultiply once at the final output
            processed = float4(acc.a != 0.0 ? acc.rgb / acc.a : float3(0.0, 0.0, 0.0), acc.a);
        } else if (mode == 7) {  // RGB: filtered RGB, exact original alpha
            processed = float4(acc.rgb, orig.a);
        } else {                 // Alpha: filtered alpha, exact original RGB
            processed = float4(orig.rgb, acc.a);
        }
    }

    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        int2 maskPixel = int2(p) + inputGeometry[2].regionAndOffset.zw;
        int2 maskExtent = int2(inputGeometry[2].extent.xy);
        bool maskInside =
            maskPixel.x >= 0 && maskPixel.y >= 0 && maskPixel.x < maskExtent.x && maskPixel.y < maskExtent.y;
        float selected =
            maskInside ? clamp(gpuLoadRgba(in_mask, maskPixel, inputGeometry[2].rgba, maskExtent.y)[channel], 0.0, 1.0)
                       : 0.0;
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in lerp()).
    float weight = coverage * mask.z;
    float4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : lerp(orig, processed, weight));
    gpuStoreRgba(out_color, int2(p), rgba, planeHeight, result);
    // Every plane the blur did not filter keeps its named channel from the
    // original main image at the same coordinate (issue #90).
    gpuPreserveAuxLattice(out_color, int2(p), planeHeight, in_main, mainExtent.y, channels.x);
}
