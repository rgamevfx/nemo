// Nemo native effect contract (spec section 10.4, issue #8; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: write
// version: 1
// ports:
//   - in_color : image2D input (the Write node's image input, port 0)
//   - out_color : image2D output (native channel image, issue #98: packed
//     RGBA32F at the logical raster for four stored channels, otherwise
//     R32_SFLOAT scalar planes at W*(H*C); straight alpha, scene-linear)
// parameters: none
// bindings (native binding contract v7, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 2 : per-input spatial geometry (the upstream raster origin)
//   set 1, binding 0 : input image2D (port 0)
//   set 2, binding 0 : output image2D
//   This node declares no payload of its own, so set 0 binding 1 is unused.
// dependencies:
//   region: output pixel (x, y) depends only on the input pixel holding the
//     same full-resolution sample, located through its own raster origin and
//     extent (region evaluation, wider cache-backed inputs, signed or
//     beyond-format origins). A sample the input raster does not hold is
//     outside its data and reads as transparent black (issue #88).
//   channels: RGBA uniform.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)

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
    // channels: the produced raster's channel facts (issues #90, #98):
    // (stored channel count, components per texel — 4 = packed four-channel
    // RGBA32F at the logical raster, 1 = R32_SFLOAT scalar planes — whether the
    // channel plan fills any stored channel, 0).
    uint4 channels;
    // rgba: the produced STORED channel of each R/G/B/A role; -1 = role absent.
    // The four stored channels of a packed image are NOT assumed to be R/G/B/A.
    int4 rgba;
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
    // The described image's data support (native binding contract v7): a sample
    // outside it is transparent black, never an upstream value carried there,
    // and every stored channel — auxiliary ones included — is initialized (issue
    // #90).
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), int(meta2.y), channels.y, channels.x);
        return;
    }
    // The upstream result may cover a wider, differently anchored or smaller
    // rectangle of the same lattice; its own origin and extent place it. A
    // sample it does not hold is outside its data: transparent black
    // (issue #88), never an out-of-bounds load.
    int2 q = int2(p) + inputGeometry[0].regionAndOffset.zw;
    int2 extent = int2(inputGeometry[0].extent.xy);
    bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
    float4 value = inside ? gpuLoadRgba(in_color, q, inputGeometry[0].rgba, extent.y,
                                        inputGeometry[0].channels.y) : float4(0.0, 0.0, 0.0, 0.0);
    // The adapter carries every described channel, not just the four roles
    // (issue #90).
    gpuStorePixel(out_color, int2(p), int(meta2.y), channels.x, channels.y, channels.z, rgba, value, in_color,
                  extent.y, inputGeometry[0].channels.y);
}
