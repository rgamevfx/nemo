// Nemo native effect contract (spec section 10.4, issue #8; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: constcolor
// version: 1
// ports:
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - color = the authored scene-linear RGBA color (straight alpha; may lie
//     outside [0, 1] — stored values are never clamped)
// bindings (native binding contract v3, see testpattern.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.constcolor.payload.v1")
//   set 2, binding 0 : output image2D
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
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
};

[[vk::binding(1, 0)]]
cbuffer ConstcolorPayload {
    float4 color;
};

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    out_color[p] = color;
}
