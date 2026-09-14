// Test-only affine RGB contribution (issue #83 extension proof).
// -------------------------------------------------------------
// Separate Slang adapter for the same declared contract the CPU adapter
// implements independently: out.rgb = scale.rgb * in.rgb + offset.rgb, alpha
// copied through unchanged, no clamp of negative or HDR values.
//
// bindings (native binding contract v2):
//   set 0 binding 0 : common request uniforms (meta/meta2/misc)
//   set 0 binding 1 : this node's own payload ("nemo.test.affine.payload.v1")
//   set 1 binding 0 : input image, port 0
//   set 2 binding 0 : node result
// precision: rgba32f (full float)
// alpha: straight (non-premultiplied), identical to the CPU adapter.

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
cbuffer AffinePayload {
    float4 scale;
    float4 offset;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_image;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_image;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    float4 x = in_image.Load(int2(p));
    out_image[p] = float4(scale.x * x.x + offset.x,
                          scale.y * x.y + offset.y,
                          scale.z * x.z + offset.z,
                          x.w);
}
