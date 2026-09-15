#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/grade/Parameters.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor gradeDescriptor() {
    const auto channelEditor = std::string{"nemo.channels.rgb"};
    const ChannelHint additive{ChannelLink::Additive, true};
    const ChannelHint multiplicative{ChannelLink::Multiplicative, true};
    return NodeDescriptor{.type = "grade",
                          .displayName = "Grade",
                          .group = "Color",
                          .implementationVersion = 1,
                          .inputs = effectImageInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              {.name = "lift",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
                               .label = "Lift",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = -1.0,
                               .softMaximum = 1.0,
                               .channels = additive},
                              {.name = "gain",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Gain",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 2.0,
                               .channels = multiplicative},
                              {.name = "multiply",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Multiply",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 2.0,
                               .channels = multiplicative},
                              {.name = "offset",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
                               .label = "Offset",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = -1.0,
                               .softMaximum = 1.0,
                               .channels = additive},
                              {.name = "gamma",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Gamma",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = 0.01,
                               .softMaximum = 4.0,
                               .channels = multiplicative},
                              mixParameterSpec("Primary"),
                              {.name = "blackpoint",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
                               .label = "Blackpoint",
                               .section = "Range",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 1.0,
                               .channels = additive},
                              {.name = "whitepoint",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Whitepoint",
                               .section = "Range",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 2.0,
                               .channels = additive},
                              {.name = "channels",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"RGB"}},
                               .choices = {"RGB", "RGBA", "R", "G", "B", "Alpha", "None"},
                               .label = "Channels",
                               .section = "Options",
                               .editor = {}},
                              {.name = "reverse",
                               .type = ParameterType::Boolean,
                               .defaultValue = ParameterValue{false},
                               .label = "Reverse",
                               .section = "Options",
                               .editor = {}},
                              {.name = "clampBlack",
                               .type = ParameterType::Boolean,
                               .defaultValue = ParameterValue{true},
                               .label = "Clamp Black",
                               .section = "Options",
                               .editor = {}},
                              {.name = "clampWhite",
                               .type = ParameterType::Boolean,
                               .defaultValue = ParameterValue{false},
                               .label = "Clamp White",
                               .section = "Options",
                               .editor = {}},
                          }),
                          .capabilities = builtinCapabilities()};
}

// Grade selects channels with a bitmask (R1/G2/B4/A8).
constexpr std::array<std::uint32_t, kImageChannels> kChannelBits{1U, 2U, 4U, 8U};

// Grade's signed power (issue #34 contract): zero stays zero for every exponent
// and negative/HDR values keep their sign, so the operation is a domain-safe
// extension rather than an implicit clamp or a complex result.
[[nodiscard]] float signedPow(float base, float exponent) {
    if (exponent == 1.0F) {
        return base;
    }
    if (base == 0.0F) {
        return 0.0F;
    }
    return std::copysign(std::pow(std::fabs(base), exponent), base);
}

struct GradeCoefficients {
    std::array<float, kImageChannels> slope{};
    std::array<float, kImageChannels> intercept{};
    std::array<float, kImageChannels> exponent{};
    std::array<bool, kImageChannels> enabled{};
};

// Forward: y = signedPow(a*x + b, 1/gamma) with a = (gain-lift)*multiply/
// (whitepoint-blackpoint) and b = lift + offset - blackpoint*a.
// Reverse:  x = (signedPow(y, gamma) - b) / a.
// Disabled channels are exact pass-through. The shared metadata seam
// (effectiveGrade) owns validation, including singular and unrepresentable
// enabled-channel settings, so this derives only the coefficients execution
// consumes.
[[nodiscard]] GradeCoefficients resolveGradeCoefficients(const GradeParameters& params) {
    GradeCoefficients coefficients;
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        coefficients.enabled[channel] = (params.channels & kChannelBits[channel]) != 0;
        if (!coefficients.enabled[channel]) {
            continue;
        }
        const float slope = (params.gain[channel] - params.lift[channel]) * params.multiply[channel] /
                            (params.whitepoint[channel] - params.blackpoint[channel]);
        coefficients.slope[channel] = slope;
        coefficients.intercept[channel] =
            params.lift[channel] + params.offset[channel] - params.blackpoint[channel] * slope;
        coefficients.exponent[channel] = params.reverse ? params.gamma[channel] : 1.0F / params.gamma[channel];
    }
    return coefficients;
}

[[nodiscard]] CpuImage applyGrade(const GradeParameters& params, const EvaluationRequest& request,
                                  const CpuImage& input, const InputAnchor& anchor) {
    const GradeCoefficients coefficients = resolveGradeCoefficients(params);
    CpuImage output(effectRasterLayout(request, &input));
    const int width = output.width();
    const int inputWidth = input.width();
    for (int y = 0; y < output.height(); ++y) {
        // Grade is pointwise, so each output sample reads the input sample at
        // the same absolute coordinates (issue #85): the input raster may start
        // elsewhere and be larger (a halo, a whole-domain escalation, a resident
        // rectangle), which the anchor places.
        const std::size_t row = static_cast<std::size_t>(anchor.offsetY + y) * static_cast<std::size_t>(inputWidth) +
                                static_cast<std::size_t>(anchor.offsetX);
        const float* source = input.data() + row * kImageChannels;
        float* destination =
            output.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * kImageChannels;
        for (int x = 0; x < width; ++x) {
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                const float value = source[channel];
                if (!coefficients.enabled[channel]) {
                    destination[channel] = value;
                    continue;
                }
                float result;
                if (params.reverse) {
                    result = (signedPow(value, coefficients.exponent[channel]) - coefficients.intercept[channel]) /
                             coefficients.slope[channel];
                } else {
                    result = signedPow(coefficients.slope[channel] * value + coefficients.intercept[channel],
                                       coefficients.exponent[channel]);
                }
                if (params.clampBlack && result < 0.0F) {
                    result = 0.0F;
                }
                if (params.clampWhite && result > 1.0F) {
                    result = 1.0F;
                }
                destination[channel] = result;
            }
            source += kImageChannels;
            destination += kImageChannels;
        }
    }
    return output;
}

CpuImage executeGrade(const CpuNodeContext& context) {
    const CpuImage& input = requiredImageInput(context, 0, "native effect requires a connected main image input");
    if (input.width() <= 0 || input.height() <= 0) {
        failNode(context.node, "native effect requires a non-empty input raster");
    }
    const InputAnchor anchor = anchorInput(context, 0, input);
    CpuImage processed = applyGrade(effectiveGrade(context.catalog, context.node, context.effectiveParams),
                                    context.request, input, anchor);
    return blendEffectOutput(context, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             input, std::move(processed));
}

std::optional<std::string> validateGradeParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                   ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] {
        static_cast<void>(effectiveGrade(catalog, node, effectiveParams));
        static_cast<void>(effectiveEffectMask(catalog, node, effectiveParams));
    });
}

}  // namespace

NodeContribution gradeContribution() {
    NodeContribution contribution;
    contribution.descriptor = gradeDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeGrade};
    contribution.validateParameters = &validateGradeParameters;
    contribution.editors = {NodeEditorContribution{.id = "nemo.channels.rgb",
                                                   .source = "qrc:/qt/qml/Nemo/qml/ChannelEditor.qml",
                                                   .consumes = {}}};
    return contribution;
}

}  // namespace nemo::nodes
