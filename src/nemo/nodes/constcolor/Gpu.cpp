// Constant Color: generator with a node-local color payload (issue #83).
//
// The authored color is packed by this module into its own payload at set 0
// binding 1; no shared uniform structure learns about Constant Color.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: exactly the authored scene-linear RGBA color.
struct ConstcolorPayload {
    float color[4]{};
};
static_assert(sizeof(ConstcolorPayload) % 16 == 0);

constexpr const char* kConstcolorGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform ConstcolorPayload {
    vec4 color;
};
)GLSL";

constexpr const char* kConstcolorGlslBody = R"GLSL(
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    imageStore(out_color, ivec2(p), color);
}
)GLSL";

[[nodiscard]] EffectPassDefinition constcolorPass() {
    return EffectPassDefinition{
        .id = "constcolor",
        .shader = "constcolor/constcolor",
        .glsl = nemo::nodes::gpuGlsl(kConstcolorGlslPayload, kConstcolorGlslBody),
        .inputs = {},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: the shared typed color parameter becomes this
// node's payload. No device, allocation, wait, or UI callback happens here.
[[nodiscard]] GpuPreparation prepareConstcolor(const GpuNodeContext& context) {
    const std::array<float, 4> color = effectiveColor4(context.catalog, context.node, context.effectiveParams, "color");
    ConstcolorPayload payload;
    std::copy(color.begin(), color.end(), payload.color);

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution constcolorGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::constcolorContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.constcolor.payload.v1";
    implementation.payloadSize = sizeof(ConstcolorPayload);
    implementation.passes = {constcolorPass()};
    implementation.prepare = &prepareConstcolor;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
