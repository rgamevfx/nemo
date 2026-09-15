// Nemo native effect contract (spec section 10.4, issue #34; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: blurHorizontal (internal pass 1 of the `blur` node)
// version: 1
// ports:
//   - in_main : image2D input (main image, required port 0)
//   - out_color : image2D output (scratch of the separable filter)
// bindings (native binding contract v3, see GpuGeometry.slang):
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
//     over the main input's whole Y range.
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
// raster; clamp-to-edge border. size 0 is the exact identity.

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
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
    // The main input covers a wider rectangle of the same lattice than this
    // scratch pass (the horizontal support on both sides); its raster origin
    // and extent locate the taps, and clamping to that extent is the image's
    // clamp-to-edge border.
    int2 mainOffset = inputGeometry[0].regionAndOffset.zw;
    int2 mainExtent = int2(inputGeometry[0].extent.xy);
    if (blur.x <= 0.0) {
        out_color[p] = in_main.Load(int2(p) + mainOffset);  // exact identity
        return;
    }

    int support = int(blur.z);

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int i = -support; i <= support; ++i) {
        float w = blurWeights[i + support];
        float4 s = in_main.Load(int2(clamp(int(p.x) + i + mainOffset.x, 0, mainExtent.x - 1),
                                     int(p.y) + mainOffset.y));
        if ((mode & 8) != 0) { s.rgb *= s.a; }  // RGBA: premultiply before filtering
        acc += w * s;
    }

    float4 center = in_main.Load(int2(p) + mainOffset);
    if (mode == 7) {        // RGB: filter RGB, preserve original alpha
        acc.a = center.a;
    } else if (mode == 8) { // Alpha: filter alpha, preserve original RGB
        acc.rgb = center.rgb;
    }
    out_color[p] = acc;
}
