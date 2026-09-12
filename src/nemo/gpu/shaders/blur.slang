// Nemo native effect contract (spec section 10.4, issue #34)
// ---------------------------------------------------------
// name: blur (pass 2 of 2: vertical support + final result)
// version: 1
// ports:
//   - in_scratch : image2D input (intermediate written by blurHorizontal)
//   - in_main : image2D input (original main image, for the mask/mix blend)
//   - in_mask : image2D input (optional mask; bound to the main image as a
//     valid dummy descriptor with maskPresent=0 when unconnected)
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// bindings: shared effect contract (see testpattern.slang). Port order is
//   declared: set 1 binding 0 = scratch, 1 = main, 2 = mask.
// dependencies:
//   region: output pixel (x, y) depends on the vertical neighborhood of the
//     intermediate plus the original pixel at (x, y).
//   channels: RGBA/RGB/Alpha per the declared `channels` choice.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: after both passes RGBA is unpremultiplied once (zero alpha -> zero
//   RGB) before the single mask/mix blend against the original pixel; the
//   mask stays in output-raster space and is never transformed. RGB and
//   Alpha modes preserve the unselected channels exactly.

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
RWTexture2D<float4> in_scratch;
[[vk::binding(1, 1)]]
RWTexture2D<float4> in_main;
[[vk::binding(2, 1)]]
RWTexture2D<float4> in_mask;

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
    float4 orig = in_main.Load(int2(p));
    float4 processed;
    if (blur.x <= 0.0) {
        processed = orig;  // exact identity (both passes are identity)
    } else {
        int height = int(meta2.y);
        int support = int(blur.z);

        float4 acc = float4(0.0, 0.0, 0.0, 0.0);
        for (int i = -support; i <= support; ++i) {
            float w = blurWeights[i + support];
            acc += w * in_scratch.Load(int2(int(p.x), clamp(int(p.y) + i, 0, height - 1)));
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
        float selected = clamp(in_mask.Load(int2(p))[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in lerp()).
    float weight = coverage * mask.z;
    out_color[p] = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : lerp(orig, processed, weight));
}
