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
        // 4: A is foreground; B is the background and inherited main input.
        .implementationVersion = 4,
        .inputs = {{PortKind::Image, "A", false}, {PortKind::Image, "B", false}, {PortKind::Mask, "mask", true}},
        .mainInput = 1,
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

// A (index 0) is foreground; B (index 1) is background and the main pipe.
// The optional mask is the third declared port; an absent mask is a null raster, never a
// manufactured source, and the shared mask/mix blend owns the absent/None/
// invert/Mix cases. Each input is read at the output coordinates through the
// coverage it was actually produced with (issue #85): A, B and the mask may
// cover more than this node's raster — a halo, a whole-domain escalation, a
// resident cache rectangle — so Merge reads exactly the requested window of
// each instead of demanding identically shaped rasters. An input that holds no
// sample at all is a valid empty image and contributes transparent black
// (issue #88), never an out-of-bounds read.
CpuImage executeMerge(const CpuNodeContext& context) {
    const CpuImage& background = requiredImageInput(context, 1, "merge requires a connected B (background) input");
    const CpuImage& foreground = requiredImageInput(context, 0, "merge requires a connected A (foreground) input");
    const InputAnchor backgroundAnchor = anchorInput(context, 1, background);
    const InputAnchor foregroundAnchor = anchorInput(context, 0, foreground);
    const MergeOperation operation = effectiveMergeOperation(context.catalog, context.node, context.effectiveParams);
    // The output raster carries the resolved description: the main input's
    // logical format, pixel aspect, channels and interpretation, at the
    // requested region and sampling scale.
    CpuImage composite(effectRasterLayout(context));
    for (int y = 0; y < composite.height(); ++y) {
        for (int x = 0; x < composite.width(); ++x) {
            const std::array<float, kImageChannels> bg =
                sampledPixel(background, backgroundAnchor.offsetX + x, backgroundAnchor.offsetY + y);
            const std::array<float, kImageChannels> fg =
                sampledPixel(foreground, foregroundAnchor.offsetX + x, foregroundAnchor.offsetY + y);
            std::array<float, kImageChannels> result{};
            if (operation == MergeOperation::Over) {
                // Straight-alpha A over B; keep the existing numerical convention.
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
    // place; no second full raster is retained. The optional mask is the third
    // declared port, and it too is read through its own coverage.
    return blendEffectOutput(context, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             background, std::move(composite), 2);
}

// Merge's described data bounds (issue #88) are the union of its two image
// inputs' data windows: the composite holds a sample wherever either operand
// does, and an operand's data window may extend past the format (overscan) or be
// empty. Everything else — the logical format, pixel aspect, channels and
// interpretation — is inherited from the main input B (the background base);
// the foreground A never changes the result's meaning, and the optional mask
// only blends values.
//
// The retained-edge-domain claim (issue #92) is the exception that the
// foreground A DOES contribute to, because Merge's pixel math is pointwise in
// its operands: where an operand answers requested coordinates outside its
// retained domain, its real values are composited into the result, so an operand
// that extends makes the composite extend too. With neither operand extending,
// the claim stays exactly what the main input B declared.
[[nodiscard]] ImageDescription describeMerge(const NodeDescriptionContext& context) {
    ImageDescription described = context.inherited;
    // Normalize before the union: an empty main input (B) has no edge to extend,
    // even when its raw declaration carries the flag. The foreground must not
    // revive that claim.
    described.edgeExtension = hasEdgeExtension(context.inherited);
    Region bounds = described.dataBounds;
    // B is already inherited as the main input. A (index 0) contributes
    // foreground coverage; the optional mask at port 2 only blends values and
    // never adds data.
    if (!context.inputs.empty()) {
        const ImageDescription* const input = context.inputs[0];
        if (input != nullptr && input->dataBounds.width > 0 && input->dataBounds.height > 0) {
            bounds = regionUnion(bounds, input->dataBounds);
        }
        described.edgeExtension = described.edgeExtension || (input != nullptr && hasEdgeExtension(*input));
    }
    described.dataBounds = bounds;
    return described;
}

std::optional<std::string> validateMergeParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                   const ParameterValues& effectiveParams) {
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
    contribution.describe = &describeMerge;
    contribution.editors = {NodeEditorContribution{.id = "nemo.merge.operation",
                                                   .source = "qrc:/qt/qml/Nemo/qml/MergeOperationEditor.qml",
                                                   .consumes = {}}};
    return contribution;
}

}  // namespace nemo::nodes
