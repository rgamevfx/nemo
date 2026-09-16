// Output: the composition result adapter (issue #83 node-local GPU module).
//
// Output owns a plain one-pass copy of port 0 to the node result. It declares
// no payload: the kernel needs only the common request. The role-specific
// meaning of the node (network output vs. pixel filter) stays with the shared
// evaluator; this module only supplies its declared pixel behavior.

#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"

namespace nemo::eval::nodes {
namespace {

constexpr const char* kOutputGlsl = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_color;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // The described image's data support (native binding contract v5): a sample
    // outside it is transparent black, never an upstream value carried there.
    if (!gpuHasData(ivec2(p))) {
        gpuStore(out_color, ivec2(p), vec4(0.0));
        return;
    }
    // The upstream result may cover a wider, differently anchored or smaller
    // rectangle of the same lattice, so the pass locates it through its own
    // origin and extent instead of assuming its own. A sample it does not hold
    // is outside its data: transparent black (issue #88), never an
    // out-of-bounds load.
    ivec2 q = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
    gpuStore(out_color, ivec2(p), inside ? imageLoad(in_color, q) : vec4(0.0));
}
)GLSL";

[[nodiscard]] EffectPassDefinition outputPass() {
    return EffectPassDefinition{
        .id = "output",
        .shader = "output/output",
        .glsl = nemo::nodes::gpuGlsl({}, kOutputGlsl, true),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

[[nodiscard]] GpuPreparation prepareOutput(const GpuNodeContext&) {
    GpuPreparation preparation;
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution outputGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::outputContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.passes = {outputPass()};
    implementation.prepare = &prepareOutput;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
