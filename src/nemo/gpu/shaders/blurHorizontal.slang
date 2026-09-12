// Nemo native effect contract (spec section 10.4, issue #34)
// ---------------------------------------------------------
// name: blurHorizontal (internal pass 1 of the `blur` effect)
// version: 1
// ports:
//   - in_main : image2D input (main image, required port 0)
//   - out_color : image2D output (intermediate of the separable filter)
// bindings: shared effect contract (see testpattern.slang); set 1 binding 0
//   = main image. This pass does NOT apply the mask/mix — that happens once,
//   after the second (vertical) pass.
// dependencies:
//   region: output pixel (x, y) depends on the horizontal neighborhood
//     [x - ceil(size/scale), x + ceil(size/scale)] — fixed support, no
//     runtime support-region expansion.
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

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    float4 param0;
    float4 param1;
    // issue #34 native effects: append-only extension shared by EVERY effect
    // kernel (existing offsets unchanged, so preexisting kernels stay
    // ABI-aligned with the C++ EffectUniforms mirror).
    float4 mask;             // (maskChannel, invertMask, mix, maskPresent)
    float4 gradeBlackpoint;
    float4 gradeWhitepoint;
    float4 gradeLift;
    float4 gradeGain;
    float4 gradeMultiply;
    float4 gradeOffset;
    float4 gradeGamma;
    float4 gradeFlags;       // (channels bitmask, reverse, clampBlack, clampWhite)
    float4 blur;             // (size full-res, channels bitmask, 0, 0)
    float4 transform;        // (translateX, translateY, scale, rotate degrees)
    float4 transformFlags;   // (filter, pixelAspect, cos, sin)
}

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
    if (blur.x <= 0.0) {
        out_color[p] = in_main.Load(int2(p));  // exact identity
        return;
    }

    int width = int(meta2.x);
    int support = int(blur.z);

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int i = -support; i <= support; ++i) {
        float w = blurWeights[i + support];
        float4 s = in_main.Load(int2(clamp(int(p.x) + i, 0, width - 1), int(p.y)));
        if ((mode & 8) != 0) { s.rgb *= s.a; }  // RGBA: premultiply before filtering
        acc += w * s;
    }

    float4 center = in_main.Load(int2(p));
    if (mode == 7) {        // RGB: filter RGB, preserve original alpha
        acc.a = center.a;
    } else if (mode == 8) { // Alpha: filter alpha, preserve original RGB
        acc.rgb = center.rgb;
    }
    out_color[p] = acc;
}
