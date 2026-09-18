#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/premult/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

struct PremultPayload {
    std::int32_t operation[4]{};  // (selected role bits, multiplier role, 0, 0)
};
static_assert(sizeof(PremultPayload) % 16 == 0);

constexpr const char* kPremultGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform PremultPayload {
    ivec4 premultOperation;
};
)GLSL";

constexpr const char* kPremultGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight, channels.y, channels.x);
        return;
    }
    ivec2 mainPixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    ivec2 mainExtent = ivec2(inputGeometry[0].extent.xy);
    bool inside = mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    vec4 value = inside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, mainExtent.y,
                                      inputGeometry[0].channels.y) : vec4(0.0);
    float multiplier = value[premultOperation.y];
    for (int role = 0; role < 4; ++role) {
        if ((premultOperation.x & (1 << role)) != 0) { value[role] *= multiplier; }
    }
    gpuStorePixel(out_color, ivec2(p), planeHeight, channels.x, channels.y, channels.z, rgba, value, in_main,
                  mainExtent.y, inputGeometry[0].channels.y);
}
)GLSL";

[[nodiscard]] EffectPassDefinition premultPass() {
    return EffectPassDefinition{.id = "premult",
                                .shader = "premult/premult",
                                .glsl = nemo::nodes::gpuGlsl(kPremultGlslPayload, kPremultGlslBody),
                                .inputs = {EffectImageRef{EffectImageKind::Input, 0}},
                                .output = EffectImageRef{EffectImageKind::Output, 0}};
}

[[nodiscard]] GpuPreparation preparePremult(const GpuNodeContext& context) {
    const PremultParameters params = effectivePremult(context.catalog, context.node, context.effectiveParams);
    if (context.inputDescriptions.empty() || context.inputDescriptions[0] == nullptr)
        failNode(context.node, "premult requires a connected image input");
    const auto roles = rgbaChannelIndices(context.inputDescriptions[0]->channels);
    bool changes = false;
    for (std::size_t role = 0; role < roles.size(); ++role)
        changes = changes || ((params.channels & (1U << role)) != 0U && roles[role] >= 0);
    if (changes && roles[static_cast<std::size_t>(params.byRole)] < 0)
        failNode(context.node, "parameter 'by' selects a channel the input does not carry");

    PremultPayload payload;
    payload.operation[0] = static_cast<std::int32_t>(params.channels);
    payload.operation[1] = params.byRole;
    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution premultGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::premultContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.premult.payload.v1";
    implementation.payloadSize = sizeof(PremultPayload);
    implementation.passes = {premultPass()};
    implementation.prepare = &preparePremult;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
