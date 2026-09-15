// Nemo native effect contract (spec section 10.4, issue #8; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: output
// version: 1
// ports:
//   - in_color : image2D input (the composed result, port 0)
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters: none
// bindings (native binding contract v3, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 2 : per-input spatial geometry (the upstream raster origin)
//   set 1, binding 0 : input image2D (port 0)
//   set 2, binding 0 : output image2D
//   This node declares no payload of its own, so set 0 binding 1 is unused.
// dependencies:
//   region: output pixel (x, y) depends only on the input pixel holding the
//     same full-resolution sample, located through its own raster origin
//     (region evaluation and wider cache-backed inputs).
//   channels: RGBA uniform.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_color;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    out_color[p] = in_color.Load(int2(p) + inputGeometry[0].regionAndOffset.zw);
}
