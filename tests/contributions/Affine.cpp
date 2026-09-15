// Test-only affine RGB contribution: scale*x + offset, alpha preserved, no
// HDR/negative clamp (issue #83). The CPU and Slang pixel algorithms are
// authored independently; their agreement is evidence, not an oracle.

#include "Affine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

namespace nemo::test::affine {
namespace {

// Node-local payload: exactly the two authored colors the shader consumes.
// Trivially copyable and a multiple of 16 bytes, so it travels as the common
// uniform-buffer payload at set 0, binding 1 without a shared uniform struct.
struct AffinePayload {
    float scale[4]{};
    float offset[4]{};
};
static_assert(sizeof(AffinePayload) % 16 == 0);

ParameterSpec scaleSpec() {
    return ParameterSpec{
        .name = "scale",
        .type = ParameterType::Color,
        .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
        .label = "Scale",
    };
}

ParameterSpec offsetSpec() {
    return ParameterSpec{
        .name = "offset",
        .type = ParameterType::Color,
        .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
        .label = "Offset",
    };
}

// Independent CPU algorithm. Alpha is copied through untouched; RGB carry
// signed, out-of-range scene-linear values without clamping. The node's raster
// is its own requested coverage (issue #85) and each sample reads the input at
// the same absolute coordinate through the input's own anchor, so the input may
// cover more without changing a pixel.
[[nodiscard]] CpuImage executeAffine(const CpuNodeContext& context) {
    const std::array<float, 4> scale = effectiveColor4(context.catalog, context.node, context.effectiveParams, "scale");
    const std::array<float, 4> offset =
        effectiveColor4(context.catalog, context.node, context.effectiveParams, "offset");
    if (context.inputs.empty() || context.inputs[0] == nullptr) {
        failNode(context.node, "affine requires a connected input image");
    }
    const CpuImage& input = *context.inputs[0];
    const InputAnchor anchor = anchorInput(context, 0, input);
    CpuImage output(effectRasterLayout(context.request, input.layout().pixelAspect));
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            const std::array<float, 4> in = input.pixel(anchor.offsetX + x, anchor.offsetY + y);
            output.setPixel(
                x, y,
                {scale[0] * in[0] + offset[0], scale[1] * in[1] + offset[1], scale[2] * in[2] + offset[2], in[3]});
        }
    }
    return output;
}

// One local pass: input port 0 -> node result. No scratch, no weights.
eval::EffectPassDefinition affinePass() {
    return eval::EffectPassDefinition{
        .id = "affine",
        .shader = "Affine",
        .inputs = {eval::EffectImageRef{eval::EffectImageKind::Input, 0}},
        .output = eval::EffectImageRef{eval::EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation only: typed parameters become the node-local
// payload; no device, allocation, wait, or UI callback happens here.
[[nodiscard]] eval::GpuPreparation prepareAffine(const eval::GpuNodeContext& context) {
    const std::array<float, 4> scale = effectiveColor4(context.catalog, context.node, context.effectiveParams, "scale");
    const std::array<float, 4> offset =
        effectiveColor4(context.catalog, context.node, context.effectiveParams, "offset");
    AffinePayload payload;
    std::copy(scale.begin(), scale.end(), payload.scale);
    std::copy(offset.begin(), offset.end(), payload.offset);

    eval::GpuPreparation preparation;
    preparation.payload = eval::effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

NodeDescriptor affineDescriptor() {
    return NodeDescriptor{
        .type = std::string(kNodeType),
        .displayName = "Affine (test)",
        .group = "Tests",
        .isOutput = false,
        .implementationVersion = kImplementationVersion,
        .inputs = {PortSpec{.kind = PortKind::Image, .name = "input"}},
        .outputs = {PortSpec{.kind = PortKind::Image, .name = "output"}},
        .parameters = {scaleSpec(), offsetSpec()},
        .capabilities = NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"RGBA"}},
    };
}

// Identical pixel declaration, whole-frame-only capability: a region request
// over this type must escalate the node (and its inputs) to the whole image
// domain, and the consumer still receives its own rectangle.
NodeDescriptor affineWholeFrameDescriptor() {
    NodeDescriptor descriptor = affineDescriptor();
    descriptor.type = std::string(kWholeFrameNodeType);
    descriptor.displayName = "Affine whole-frame (test)";
    descriptor.capabilities.supportsRegion = false;
    return descriptor;
}

NodeContribution affineContribution() {
    return NodeContribution{
        .descriptor = affineDescriptor(),
        .role = NodeRole::Image,
        .cpu = CpuImplementation{.version = kImplementationVersion, .execute = &executeAffine},
        .nativeGpu = true,
    };
}

NodeContribution affineWholeFrameContribution() {
    NodeContribution contribution = affineContribution();
    contribution.descriptor = affineWholeFrameDescriptor();
    return contribution;
}

eval::GpuNodeContribution affineGpuContribution() {
    eval::GpuNodeContribution contribution;
    contribution.node = affineContribution();
    contribution.gpu = eval::GpuImplementation{
        .version = kImplementationVersion,
        .payloadLayout = "nemo.test.affine.payload.v1",
        .payloadSize = sizeof(AffinePayload),
        .passes = {affinePass()},
        .prepare = &prepareAffine,
    };
    return contribution;
}

eval::GpuNodeContribution affineWholeFrameGpuContribution() {
    eval::GpuNodeContribution contribution = affineGpuContribution();
    contribution.node = affineWholeFrameContribution();
    return contribution;
}

}  // namespace nemo::test::affine
