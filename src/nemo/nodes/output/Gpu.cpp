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
    imageStore(out_color, ivec2(p), imageLoad(in_color, ivec2(p)));
}
)GLSL";

[[nodiscard]] EffectPassDefinition outputPass() {
    return EffectPassDefinition{
        .id = "output",
        .shader = "output/output",
        .glsl = nemo::nodes::gpuGlsl({}, kOutputGlsl),
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
