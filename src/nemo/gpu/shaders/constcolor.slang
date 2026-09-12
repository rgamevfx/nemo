// Nemo native effect contract (spec section 10.4, issue #8)
// ---------------------------------------------------------
// name: constcolor
// version: 1
// ports:
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - param0 = color (4 floats, straight alpha; may lie outside [0, 1] —
//     scene-linear values are stored, never clamped)
// bindings: shared effect contract (see testpattern.slang)
// dependencies:
//   region: output pixel (x, y) depends only on the constant color; no
//     neighborhood reads.
//   channels: RGBA uniform.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // param0 = color
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

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    out_color[p] = param0;
}
