// Nemo native effect contract (spec section 10.4, issue #8)
// ---------------------------------------------------------
// name: merge
// version: 1
// ports:
//   - in_a : image2D input, port A (over base / background)
//   - in_b : image2D input, port B (over source / foreground)
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - operation = "over" (the only supported operation; anything else is a
//     declared limitation, not a silent fallback)
// bindings: shared effect contract (see testpattern.slang). Port order is
//   declared: set 1 binding 0 = A, binding 1 = B — swapping them is a
//   different result and fails declared image comparison.
// dependencies:
//   region: output pixel (x, y) depends only on inputs (x, y); no
//     neighborhood reads, so no support-region expansion.
//   channels: RGBA uniform.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight (non-premultiplied), identical to the CPU reference —
//   out.rgb = A.a*B? no: out.rgb = B.a*B.rgb + (1 - B.a)*A.rgb with
//   out.a = B.a + (1 - B.a)*A.a. Premultiplied interpretation is a
//   declared comparison failure, not an alternate path.

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    float4 param0;
    float4 param1;
}

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_a;
[[vk::binding(1, 1)]]
RWTexture2D<float4> in_b;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    float4 bg = in_a.Load(p);   // port A: over base (background)
    float4 fg = in_b.Load(p);   // port B: over source (foreground)

    // Straight-alpha "over", exactly the CPU reference expression.
    float4 result;
    result.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    result.w = fg.a + (1.0 - fg.a) * bg.a;
    out_color[p] = result;
}
