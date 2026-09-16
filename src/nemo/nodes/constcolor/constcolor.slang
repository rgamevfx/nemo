// Nemo native effect contract (spec section 10.4, issue #8; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: constcolor
// version: 2
// ports:
//   - out_color : image2D output (native R32_SFLOAT channel planes, straight
//     alpha, scene-linear: logical pixel (x, y) of channel plane c at
//     (x, y + c*logicalHeight), issue #90)
// parameters:
//   - color = the authored scene-linear RGBA color (straight alpha; may lie
//     outside [0, 1] — stored values are never clamped)
// bindings (native binding contract v6, see testpattern.slang):
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
    // support (a node-local scratch), a non-positive size means every sample is
    // transparent black. Samples outside it must be written transparent black.
    int4 support;
    // channels: the produced raster's channel planes (issue #90):
    // (plane count, 0, 0, 0).
    uint4 channels;
    // rgba: the produced plane index of each R/G/B/A role; -1 = role absent.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer ConstcolorPayload {
    float4 color;
};

[[vk::binding(0, 2)]]
RWTexture2D<float> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // The described image's data support (native binding contract v6): a sample
    // outside it is transparent black, never the authored color computed there,
    // and every plane — auxiliary ones included — is initialized (issue #90).
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), int(meta2.y), channels.x);
        return;
    }
    // A generator writes the roles its described raster carries; a role the
    // raster does not store is dropped rather than manufactured (issue #90).
    gpuStoreRgba(out_color, int2(p), rgba, int(meta2.y), color);
}
