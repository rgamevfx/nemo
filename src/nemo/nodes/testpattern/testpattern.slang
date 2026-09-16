// Nemo native effect contract (spec section 10.4, issue #8; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: testpattern
// version: 3 (schema descriptor identity; see nemo/nodes/testpattern/Contribution.cpp)
// ports:
//   - out_color : image2D output (native R32_SFLOAT channel planes, straight
//     alpha, scene-linear: logical pixel (x, y) of channel plane c at
//     (x, y + c*logicalHeight), issue #90)
// parameters:
//   - none (the pattern is procedural)
// bindings (native binding contract v6, shared by every Nemo effect):
//   set 0, binding 0 : common request uniforms (meta/meta2/misc) describing
//     the raster THIS pass produces
//   set 0, binding 1 : node-local payload (when the node declares one)
//   set 0, binding 3 : the produced raster's channel plan (issue #90; unused by
//     this node: it produces every plane itself)
//   set 0, binding 2 : per-input spatial geometry, one entry per set-1
//     binding (see GpuGeometry.slang) — declared by every pass that binds an
//     Input or Scratch image
//   set 1, binding n : input image2D (native R32_SFLOAT channel planes,
//     straight alpha, issue #90)
//   set 2, binding 0 : output image2D (native R32_SFLOAT channel planes)
//   This node declares no payload of its own, so set 0 binding 1 is unused.
// dependencies:
//   region: output pixel (x, y) depends only on the full-image coordinate
//     (region.x + x*scale, region.y + y*scale) — full-resolution
//     coordinate semantics under every declared samplingScale (issue #11) and
//     under region-limited requests (issue #85).
//   channels: RGBA, all channels processed uniformly.
//   temporal: blue bar position depends on misc.x (local time).
// quality_modes: 1 (single full-quality path)
// precision: rgba32f (full float) — operation requirement, not a codec
//   format (spec section 10.4).

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y; signed)
    int4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
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

[[vk::binding(0, 2)]]
RWTexture2D<float> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    // The described image's data support (native binding contract v6): a sample
    // outside it is transparent black, never an extrapolated pattern, and every
    // plane — auxiliary ones included — is initialized (issue #90).
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), int(meta2.y), channels.x);
        return;
    }

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

    // The pattern's four roles are written into the raster's own planes
    // (issue #90); a role the described raster does not store is dropped.
    gpuStoreRgba(out_color, int2(p), rgba, int(meta2.y), float4(u, v, inBar ? 1.0 : 0.0, 1.0));
}
