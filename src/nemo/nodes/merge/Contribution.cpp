#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/merge/Parameters.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor mergeDescriptor() {
    return NodeDescriptor{
        .type = "merge",
        .displayName = "Merge",
        .group = "Compositing",
        .implementationVersion = 2,
        .inputs = {{PortKind::Image, "A", false}, {PortKind::Image, "B", false}, {PortKind::Mask, "mask", true}},
        .outputs = {{PortKind::Image, "out"}},
        .parameters = {{.name = "operation",
                        .type = ParameterType::Choice,
                        .defaultValue = ParameterValue{ChoiceValue{"over"}},
                        .choices = {"over", "plus", "multiply", "screen", "difference"},
                        .label = "Operation",
                        .section = "Composite",
                        .editor = "nemo.merge.operation"},
                       {.name = "mix",
                        .type = ParameterType::Float,
                        .defaultValue = ParameterValue{1.0},
                        .minimum = 0.0,
                        .maximum = 1.0,
                        .label = "Mix",
                        .section = "Composite",
                        .editor = {}},
                       {.name = "maskChannel",
                        .type = ParameterType::Choice,
                        .defaultValue = ParameterValue{ChoiceValue{"A"}},
                        .choices = {"none", "R", "G", "B", "A"},
                        .label = "Mask Channel",
                        .section = "Mask",
                        .editor = {}},
                       {.name = "invertMask",
                        .type = ParameterType::Boolean,
                        .defaultValue = ParameterValue{false},
                        .label = "Invert Mask",
                        .section = "Mask",
                        .editor = {}}},
        .capabilities = builtinCapabilities()};
}

// Per-channel blend target for the extended Merge modes (issue #75): the value
// the foreground coverage interpolates toward from the background. Over is
// handled inline to preserve its existing exact expression; plus/product/
// complement-product/difference are the documented targets and are never
// clamped (scene-linear float stays outside [0, 1]).
[[nodiscard]] float mergeBlendTarget(MergeOperation operation, float background, float foreground) {
    switch (operation) {
    case MergeOperation::Plus:
        return background + foreground;
    case MergeOperation::Multiply:
        return background * foreground;
    case MergeOperation::Screen:
        return 1.0F - (1.0F - background) * (1.0F - foreground);
    case MergeOperation::Difference:
        return std::fabs(background - foreground);
    case MergeOperation::Over:
        break;
    }
    return foreground;
}

// Port roles are the declared schema, not a convention: A (index 0) is the
// background base and B (index 1) the foreground source. The optional mask is
// the third declared port (issue #75); an absent mask is a null raster, never a
// manufactured source, and the shared mask/mix blend owns the absent/None/
// invert/Mix cases.
CpuImage executeMerge(const CpuNodeContext& context) {
    const CpuImage& background = requiredImageInput(context, 0, "merge requires a connected A (background) input");
    const CpuImage& foreground = requiredImageInput(context, 1, "merge requires a connected B (foreground) input");
    if (background.width() <= 0 || background.height() <= 0 || foreground.width() != background.width() ||
        foreground.height() != background.height()) {
        failNode(context.node, "merge requires background (A) and foreground (B) rasters of the same non-empty size");
    }
    const MergeOperation operation = effectiveMergeOperation(context.catalog, context.node, context.effectiveParams);
    // The output raster keeps the spatial metadata the CPU dispatch path has
    // always produced for Merge: the background's extent and pixel aspect with
    // the storage defaults (RGBA float, scene-linear).
    ImageLayout layout;
    layout.width = background.width();
    layout.height = background.height();
    layout.pixelAspect = background.layout().pixelAspect;
    CpuImage composite(layout);
    for (int y = 0; y < background.height(); ++y) {
        for (int x = 0; x < background.width(); ++x) {
            const std::array<float, kImageChannels> bg = background.pixel(x, y);
            const std::array<float, kImageChannels> fg = foreground.pixel(x, y);
            std::array<float, kImageChannels> result{};
            if (operation == MergeOperation::Over) {
                // The reference's existing Over expression, unchanged: the
                // target is the foreground and the same operand order is kept,
                // so full coverage/mix stays bit-identical to prior documents.
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    result[channel] = fg[3] * fg[channel] + (1.0F - fg[3]) * bg[channel];
                }
            } else {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float target = mergeBlendTarget(operation, bg[channel], fg[channel]);
                    result[channel] = bg[channel] + fg[3] * (target - bg[channel]);
                }
            }
            // Unmasked alpha is operation-independent (foreground coverage over
            // the background), exactly as current Over.
            result[3] = fg[3] + (1.0F - fg[3]) * bg[3];
            composite.setPixel(x, y, result);
        }
    }
    // Shared mask/mix interpolation (absent mask or channel none = full
    // coverage): background + coverage*mix*(composite - background), so Mix 0 or
    // zero coverage returns the background exactly. The composite is consumed in
    // place; no second full raster is retained.
    return blendEffectOutput(context.node, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             background, std::move(composite), optionalImageInput(context, 2));
}

std::optional<std::string> validateMergeParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                   ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] {
        static_cast<void>(effectiveMergeOperation(catalog, node, effectiveParams));
        static_cast<void>(effectiveEffectMask(catalog, node, effectiveParams));
    });
}

}  // namespace

NodeContribution mergeContribution() {
    NodeContribution contribution;
    contribution.descriptor = mergeDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeMerge};
    contribution.validateParameters = &validateMergeParameters;
    contribution.editors = {NodeEditorContribution{.id = "nemo.merge.operation",
                                                   .source = "qrc:/qt/qml/Nemo/qml/MergeOperationEditor.qml",
                                                   .consumes = {}}};
    return contribution;
}

}  // namespace nemo::nodes
