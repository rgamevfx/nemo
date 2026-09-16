// Nemo native effect contract (spec section 10.4, issue #11; contribution-local
// module per issue #83; media coordinates per issue #88)
// ---------------------------------------------------------
// name: source
// version: 3
// ports:
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - the node's `source` parameter names a Document source reference
//     (setSourceCommand); the shared runtime resolves the decoded frame and
//     this node's preparation consumes no values from it.
// bindings (native binding contract v5, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms (meta is signed)
//   set 0, binding 2 : per-input spatial geometry (the decoded frame's
//     coverage: full-resolution origin in regionAndOffset.xy, raster extent in
//     extent.xy; regionAndOffset.zw is unused because an external frame is not
//     a lattice sibling of the output raster)
//   set 1, binding 0 : the decoded full-resolution source frame
//     (rgba32f storage image, GENERAL, straight alpha) — supplied by the
//     shared source session, not by a graph edge
//   set 2, binding 0 : output image2D
//   No node-local payload: the kernel consumes the request and the coverage
//   geometry only.
// dependencies:
//   region: output pixel (x, y) depends only on the source sample at the
//     ABSOLUTE full-resolution coordinate (region.x + x*scale,
//     region.y + y*scale) minus the decoded frame's coverage origin — the
//     media keeps its own coordinates while the raster is reduced (spec
//     section 8/10.4: ROI and sampling resolution are distinct concerns, and
//     the reduction is explicit in meta2), so Full/Half/Quarter read the same
//     samples. A coordinate the coverage does not hold is transparent black:
//     the frame is never stretched to the composition frame, and an empty
//     data window yields an all-black raster.
//   channels: RGBA uniform.
//   temporal: the frame is resolved by the shared source-time mapping
//     (frameOffset + localTime*frameStep); the kernel is time-independent.
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
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_source;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    // The described image's data support (native binding contract v5): a sample
    // outside this Read's described data window is transparent black.
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        out_color[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    int scale = (int)meta2.z;
    int2 full = int2(meta.z + (int)p.x * scale, meta.w + (int)p.y * scale);
    int2 origin = inputGeometry[0].regionAndOffset.xy;
    int2 extent = int2(inputGeometry[0].extent.xy);
    int2 s = full - origin;
    bool covered = s.x >= 0 && s.y >= 0 && s.x < extent.x && s.y < extent.y;
    out_color[p] = covered ? in_source.Load(s) : float4(0.0, 0.0, 0.0, 0.0);
}
