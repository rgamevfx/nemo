// Test Pattern: procedural generator (issue #83 node-local GPU module).
//
// The module owns its local pass definition, the complete GLSL reference
// source of that pass, and its data-only preparation. It declares no payload:
// everything the kernel consumes travels in the common request at set 0
// binding 0, so set 0 binding 1 stays unused.

#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"

namespace nemo::eval::nodes {
namespace {

// Complete GLSL reference source of the single local pass (independent of the
// Slang implementation; both must agree with the CPU reference).
constexpr const char* kTestpatternGlsl = R"GLSL(
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // The described image's data support (native binding contract v5): a sample
    // outside it is transparent black, never an extrapolated pattern.
    if (!gpuHasData(ivec2(p))) {
        gpuStore(out_color, ivec2(p), vec4(0.0));
        return;
    }
    // Full-resolution coordinate frame (issue #11): the reduced raster
    // samples the frame at fullX = region.x + p.x * scale, so the pattern
    // is the SAME image every representation — not a smaller replica.
    int scale = int(meta2.z);
    int fullX = int(meta.z) + int(p.x) * scale;
    int fullY = int(meta.w) + int(p.y) * scale;
    int fullWidth = int(meta.x);
    int fullHeight = int(meta.y);
    float u = fullWidth > 1 ? float(fullX) / float(fullWidth - 1) : 0.0;
    float v = fullHeight > 1 ? float(fullY) / float(fullHeight - 1) : 0.0;
    int barWidth = max(2, fullWidth / 16);
    int barPos = (int(misc.x) * (fullWidth / 8)) % (fullWidth + barWidth);
    bool inBar = fullX >= barPos && fullX < barPos + barWidth;
    gpuStore(out_color, ivec2(p), vec4(u, v, inBar ? 1.0 : 0.0, 1.0));
}
)GLSL";

[[nodiscard]] EffectPassDefinition testpatternPass() {
    return EffectPassDefinition{
        .id = "testpattern",
        .shader = "testpattern/testpattern",
        .glsl = nemo::nodes::gpuGlsl({}, kTestpatternGlsl, false),
        .inputs = {},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Procedural: the kernel reads the common request only, so preparation
// selects its single local pass and carries no values.
[[nodiscard]] GpuPreparation prepareTestpattern(const GpuNodeContext&) {
    GpuPreparation preparation;
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution testpatternGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::testpatternContribution();
    GpuImplementation implementation;
    // The schema descriptor is the version owner; a changed kernel/contract
    // must change the persisted version with it.
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.passes = {testpatternPass()};
    implementation.prepare = &prepareTestpattern;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
