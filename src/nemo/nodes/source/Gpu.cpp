// Read (source): real-media fill (issue #83 node-local GPU module).
//
// Decoding is the shared runtime's job (the source session resolves the
// effective source request); this module only declares the local pass that
// places that frame. The decoded frame's coverage travels in the shared
// per-input geometry block — a Read's external frame binds at set 1 binding 0 as
// External 0, with its own full-resolution origin and raster extent — so the
// kernel maps each output sample to its ABSOLUTE full-resolution coordinate and
// reads that sample directly. There is no fill-ratio resize: the media keeps its
// own coordinates, so Full/Half/Quarter representations read the same samples,
// and a coordinate the decoded frame does not cover is transparent black
// (issue #88). The frame itself never passes through node preparation, so no GPU
// handle, allocation, or wait enters the preparation callback.

#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"

namespace nemo::eval::nodes {
namespace {

constexpr const char* kSourceGlsl = R"GLSL(
// Real-media source (issue #11): the decoded, scene-linear, full-resolution
// frame binds at set 1 binding 0 (supplied by the shared source session, not by
// a graph edge). Output pixel p of the reduced raster covers the absolute
// full-resolution coordinate (meta.z + p.x*scale, meta.w + p.y*scale); the
// decoded frame's coverage supplies its own origin, so the sample is that
// coordinate minus the origin. Outside the coverage is transparent black: the
// media is never stretched to the composition frame.
layout(set = 1, binding = 0) restrict readonly uniform image2D in_source;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    // The described image's data support (native binding contract v7): a sample
    // outside this Read's described data window is transparent black, and every
    // plane is initialized (issue #90).
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight, channels.y, channels.x);
        return;
    }
    int scale = int(meta2.z);
    ivec2 full = ivec2(meta.z + int(p.x) * scale, meta.w + int(p.y) * scale);
    ivec2 origin = inputGeometry[0].regionAndOffset.xy;
    ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    ivec2 s = full - origin;
    bool covered = s.x >= 0 && s.y >= 0 && s.x < extent.x && s.y < extent.y;
    // The decoded frame is a native channel image of its own (issue #98) and the
    // frame's plane c is this result's plane c: the described channel list is
    // the frame's, in plane order. A plane the frame does not physically carry
    // stays numeric zero, and the frame's plane height is its logical height.
    const int framePlanes = int(inputGeometry[0].channels.x);
    // The produced pixel: one register for a packed raster, one plane store per
    // channel for a scalar one (issues #90, #98).
    vec4 pixel = vec4(0.0);
    for (int plane = 0; plane < int(channels.x); ++plane) {
        float value = (plane < framePlanes && covered)
                          ? gpuLoadChannel(in_source, s, plane, extent.y, inputGeometry[0].channels.y)
                          : 0.0;
        gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, plane, value);
    }
    gpuPixelStore(out_color, ivec2(p), channels.y, pixel);
}
)GLSL";

[[nodiscard]] EffectPassDefinition sourcePass() {
    return EffectPassDefinition{
        .id = "source",
        .shader = "source/source",
        .glsl = nemo::nodes::gpuGlsl({}, kSourceGlsl),
        // The decoded frame is supplied by the shared source session, not by a
        // graph edge: external reference 0.
        .inputs = {EffectImageRef{EffectImageKind::External, 0}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Procedural: the kernel reads the request and the frame's bound geometry only,
// so preparation selects its single local pass and carries no values. The shared
// runtime already resolved and decoded the frame; preparation must not touch it.
[[nodiscard]] GpuPreparation prepareSource(const GpuNodeContext&) {
    GpuPreparation preparation;
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution sourceGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::sourceContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.passes = {sourcePass()};
    implementation.prepare = &prepareSource;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
