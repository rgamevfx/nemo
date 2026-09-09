// Nemo native effect contract (spec section 10.4, issue #8)
// ---------------------------------------------------------
// name: testpattern
// version: 1
// ports:
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - none (the pattern is procedural)
// bindings (fixed effect contract, shared by every Nemo effect):
//   set 0, binding 0 : EffectUniforms (std140, uint4/float4 words only)
//   set 1, binding n : input image2D (rgba32f storage image, straight alpha)
//   set 2, binding 0 : output image2D (rgba32f storage image)
// dependencies:
//   region: output pixel (x, y) depends only on the full-image coordinate
//     (region.x + x*scale, region.y + y*scale) — full-resolution
//     coordinate semantics under every declared samplingScale (issue #11).
//   channels: RGBA, all channels processed uniformly.
//   temporal: blue bar position depends on misc.x (local time).
// quality_modes: 1 (single full-quality path)
// precision: rgba32f (full float) — operation requirement, not a codec
//   format (spec section 10.4).

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // param0/param1: effect-specific declared parameters (unused here)
    float4 param0;
    float4 param1;
}

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    // Full-resolution coordinate frame (issue #11): the reduced raster
    // samples the frame at fullX = region.x + p.x * scale, so the pattern
    // is the SAME image every representation — not a smaller replica.
    int scale = (int)meta2.z;
    int fullX = (int)meta.z + (int)p.x * scale;
    int fullY = (int)meta.w + (int)p.y * scale;
    int fullWidth = (int)meta.x;
    int fullHeight = (int)meta.y;
    float u = fullWidth > 1 ? (float)fullX / (float)(fullWidth - 1) : 0.0;
    float v = fullHeight > 1 ? (float)fullY / (float)(fullHeight - 1) : 0.0;
    int barWidth = max(2, fullWidth / 16);
    // Integer math, identical to the GLSL equivalent and the CPU reference
    // (front ends must agree for non-integer times, not just integer frames).
    int barPos = (int(misc.x) * (fullWidth / 8)) % (fullWidth + barWidth);
    bool inBar = fullX >= barPos && fullX < barPos + barWidth;

    out_color[p] = float4(u, v, inBar ? 1.0 : 0.0, 1.0);
}
