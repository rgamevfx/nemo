// Nemo native effect contract (spec section 10.4, issue #34; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: blurHorizontal (internal pass 1 of the `blur` node)
// version: 3
// ports:
//   - in_main : image2D input (main image, required port 0)
//   - out_color : image2D output (scratch of the separable filter)
// bindings (native binding contract v7, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms (this scratch pass's coverage)
//   set 0, binding 1 : node-local payload ("nemo.nodes.blur.payload.v1")
//   set 0, binding 2 : per-input spatial geometry (main)
//   set 1, binding 0 = main image
//   set 2, binding 0 : scratch image2D
//   set 3, binding 0 : normalized Gaussian weights (index i + raster support)
//   This pass does NOT apply the mask/mix — that happens once, after the
//   second (vertical) pass.
// dependencies:
//   region: output pixel (x, y) depends on the horizontal neighborhood
//     [x - ceil(size/scale), x + ceil(size/scale)] of the main input, located
//     through the input's own raster origin and clamped to its extent (the
//     image's clamp-to-edge border); the scratch raster is the node's X range
//     over the main input's whole Y range. A pixel outside the bound raster is
//     outside that image's data and reads as transparent black (issue #88); an
//     empty raster has no pixels to clamp to and filters to transparent black.
//   channels: RGBA/RGB/Alpha per the declared `channels` choice.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: RGBA filters premultiplied RGB+alpha (unpremultiplied once by pass
//   2); RGB filters RGB and preserves alpha; Alpha filters alpha and
//   preserves RGB.
//
// Exact normalized separable Gaussian: size is the FULL-RESOLUTION support
// radius, sigma = size/3, raster support = ceil(size/samplingScale) and
// sample i sits at full-res offset i*samplingScale; weights
// exp(-0.5*(i*samplingScale/sigma)^2) normalized per axis over the executed
// raster; clamp-to-edge border. size 0 is the exact identity. The radius has no
// authored maximum (issue #103); the tap count is the checked, representable
// support the preparation computed, so the kernel never saturates a larger
// requested size.

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
    // support. This pass writes the separable filter's INTERMEDIATE, which is a
    // node-local scratch: the executor sends (-1,-1,-1,-1) and the kernel writes
    // the whole raster, because the intermediate carries filtered rows for the
    // vertical pass rather than the node's own data window.
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
cbuffer BlurPayload {
    // (maskChannel, invertMask, mix, maskPresent)
    float4 mask;
    // (size full-res, channels bitmask, raster support, 0)
    float4 blur;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

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

    int mode = int(blur.y);
    const int planeHeight = int(meta2.y);
    // The main input covers a wider rectangle of the same lattice than this
    // scratch pass (the horizontal support on both sides); its raster origin
    // and extent locate the taps, and clamping to that extent is the image's
    // clamp-to-edge border. An input with an empty raster has no pixels to
    // clamp to: the filtered result is transparent black (issue #88).
    int2 mainOffset = inputGeometry[0].regionAndOffset.zw;
    int2 mainExtent = int2(inputGeometry[0].extent.xy);
    if (mainExtent.x <= 0 || mainExtent.y <= 0) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.y, channels.x);
        return;
    }
    int lastX = mainExtent.x - 1;
    int lastY = mainExtent.y - 1;
    int2 center = int2(p) + mainOffset;
    if (blur.x <= 0.0) {
        // Exact identity: the whole RGBA projection of the center sample.
        float4 centered = gpuLoadRgba(in_main, int2(clamp(center.x, 0, lastX), clamp(center.y, 0, lastY)),
                                      inputGeometry[0].rgba, mainExtent.y, inputGeometry[0].channels.y);
        gpuStorePixel(out_color, int2(p), planeHeight, channels.x, channels.y, channels.z, rgba, centered, in_main,
                      mainExtent.y, inputGeometry[0].channels.y);
        return;
    }

    // Raster support in samples per axis; named `taps` so the local never
    // shadows the request's `support` word (native binding contract v7).
    int taps = int(blur.z);

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int i = -taps; i <= taps; ++i) {
        float w = blurWeights[i + taps];
        float4 s = gpuLoadRgba(in_main,
                               int2(clamp(int(p.x) + i + mainOffset.x, 0, lastX),
                                             clamp(int(p.y) + mainOffset.y, 0, lastY)),
                               inputGeometry[0].rgba, mainExtent.y, inputGeometry[0].channels.y);
        if ((mode & 8) != 0) { s.rgb *= s.a; }  // RGBA: premultiply before filtering
        acc += w * s;
    }

    float4 centerPixel = gpuLoadRgba(in_main, int2(clamp(center.x, 0, lastX), clamp(center.y, 0, lastY)),
                                     inputGeometry[0].rgba, mainExtent.y, inputGeometry[0].channels.y);
    if (mode == 7) {        // RGB: filter RGB, preserve original alpha
        acc.a = centerPixel.a;
    } else if (mode == 8) { // Alpha: filter alpha, preserve original RGB
        acc.rgb = centerPixel.rgb;
    }
    // The intermediate carries the node's named channels too, so the vertical
    // pass's own preservation reads a filtered-but-unchanged auxiliary value at
    // the same coordinate (issue #90).
    gpuStorePixel(out_color, int2(p), planeHeight, channels.x, channels.y, channels.z, rgba, acc, in_main,
                  mainExtent.y, inputGeometry[0].channels.y);
}
