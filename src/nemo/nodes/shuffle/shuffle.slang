// Nemo native effect contract (spec section 10.4, issue #90; Shuffle is the
// modern socket-based reference node)
// ---------------------------------------------------------
// name: shuffle
// version: 1
// ports:
//   - in_base   : image2D input (port 0, B: the required base image)
//   - in_second : image2D input (port 1, A: optional; an absent slot binds the
//     base image as a valid dummy descriptor, and every A-sourced row then
//     resolves to zero)
//   - out_color : image2D output (native channel planes)
// parameters (see nemo/nodes/shuffle/Parameters.hpp and Contribution.cpp):
//   - input1/input2 Choice(B,A), in1/in2 and out1/out2 String layer selectors:
//     the mapping editor's own grouping state. They never resolve a row: every
//     row states its input, its source channel name and its output channel name
//     in full.
//   - for row i = 0..7: sourceKind{i} Choice(B,A,zero,one), sourceChannel{i}
//     String (the exact stored channel name, for B/A rows), outputChannel{i}
//     String (empty disables the row).
//   - defaults: rows 0..3 are B's R,G,B,A mapped onto R,G,B,A; rows 4..7 write
//     nothing.
// bindings (native binding contract v6, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.shuffle.payload.v1"):
//     eight resolved rows (kind, source plane, output plane, 0) + the base
//     format the constants cover
//   set 0, binding 2 : per-input spatial geometry (base, second)
//   set 0, binding 3 : channel plan (unused here: this node produces every plane)
//   set 1, binding 0 = base (B), binding 1 = second (A)
//   set 2, binding 0 : output image2D
// dependencies:
//   region: output pixel (x, y) reads the base pixel and the A pixel holding the
//     same full-resolution sample, each located through its own raster origin
//     and extent. A sample an input does not hold is outside its data:
//     transparent black, never an out-of-bounds load.
//   channels: NOT uniform. The result carries the base image's channels first in
//     the base's own order — untouched unless a row replaces that channel — then
//     every authored output channel the base does not already have, in row
//     order. A row naming an existing channel replaces its value at its
//     existing plane. A source channel may fan out to any number of rows; a row
//     whose source name the chosen input does not carry, or whose input is
//     disconnected, is numeric zero. Zero/one constants read no image: zero is
//     numeric zero and one is 1 only inside the BASE IMAGE'S FORMAT, so the
//     constants never claim an overscan sample the union data window carries
//     from A. A plane no rule provides is numeric zero.
//   temporal: none.
// quality_modes: 1
// precision: r32f channel planes (full float)

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
    // rgba: the produced plane index of each R,G,B,A role; -1 = role absent.
    // Unused here: this node addresses the result's planes directly through its
    // resolved mapping rows.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer ShufflePayload {
    // (kind, source plane, output plane, 0) per authored row; kind -1 = the row
    // writes nothing, 0 = input B, 1 = input A, 2 = constant zero, 3 = constant
    // one.
    int4 shuffleRows[8];
    // (base format width, base format height, 0, 0).
    int4 shuffleFormat;
};

[[vk::binding(0, 1)]]
RWTexture2D<float> in_base;
[[vk::binding(1, 1)]]
RWTexture2D<float> in_second;

[[vk::binding(0, 2)]]
RWTexture2D<float> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);

    // The described result's data support (native binding contract v6). This
    // node produces every plane itself, so the outside-support branch zeroes
    // them explicitly (issue #90).
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.x);
        return;
    }

    // Each input is located through its own raster origin and extent: a sample
    // it does not hold is outside its data (transparent black), and A's samples
    // keep their own coordinates rather than being stretched onto B.
    int2 basePixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    int2 baseExtent = int2(inputGeometry[0].extent.xy);
    bool baseInside = basePixel.x >= 0 && basePixel.y >= 0 && basePixel.x < baseExtent.x && basePixel.y < baseExtent.y;
    int2 secondPixel = int2(p) + inputGeometry[1].regionAndOffset.zw;
    int2 secondExtent = int2(inputGeometry[1].extent.xy);
    bool secondInside =
        secondPixel.x >= 0 && secondPixel.y >= 0 && secondPixel.x < secondExtent.x && secondPixel.y < secondExtent.y;

    // Untouched B: the result carries B's channels first, in B's own order, so a
    // plane B physically holds and no row replaces keeps B's value at unchanged
    // coordinates. Bounding by the BOUND image's plane count (never by the
    // description) keeps a cleared frame's absent planes at numeric zero.
    const int basePlanes = int(inputGeometry[0].channels.x);
    const int untouched = int(channels.x) < basePlanes ? int(channels.x) : basePlanes;
    for (int plane = 0; plane < untouched; ++plane) {
        float value = baseInside ? gpuLoadPlane(in_base, basePixel, plane, baseExtent.y) : 0.0;
        gpuStorePlane(out_color, int2(p), plane, planeHeight, value);
    }

    // The constants cover the base image's FORMAT, in full-resolution
    // coordinates: a constant row writes 1 only where the base format claims a
    // pixel, even when the result's data window includes A's overscan.
    const int scale = int(meta2.z);
    int2 full = int2(meta.z + int(p.x) * scale, meta.w + int(p.y) * scale);
    bool insideFormat = full.x >= 0 && full.y >= 0 && full.x < shuffleFormat.x && full.y < shuffleFormat.y;

    for (int row = 0; row < 8; ++row) {
        const int4 entry = shuffleRows[row];
        if (entry.x < 0) { continue; }  // nothing to write
        const int outputPlane = entry.z;
        if (outputPlane < 0 || outputPlane >= int(channels.x)) { continue; }
        float value = 0.0;
        if (entry.x == 0) {  // input B: the exact named channel, or zero when absent
            value = (entry.y >= 0 && baseInside) ? gpuLoadPlane(in_base, basePixel, entry.y, baseExtent.y) : 0.0;
        } else if (entry.x == 1) {  // input A at its own coordinates, or zero
            value = (entry.y >= 0 && secondInside) ? gpuLoadPlane(in_second, secondPixel, entry.y, secondExtent.y)
                                                   : 0.0;
        } else if (entry.x == 3) {  // constant one, defined on the base format
            value = insideFormat ? 1.0 : 0.0;
        }
        // kind 2 (constant zero) needs no read and writes numeric zero.
        gpuStorePlane(out_color, int2(p), outputPlane, planeHeight, value);
    }
}
