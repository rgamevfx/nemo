#pragma once

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace nemo {

// Node-describing error helpers and effective-parameter resolution shared by
// the CPU reference and the native GPU effect executor. Effective values stay
// typed all the way through planning and execution; text is only produced for
// diagnostics.

[[nodiscard]] inline std::string describeNode(const NodeInstance& node) {
    std::ostringstream text;
    text << "node '" << node.name << "' (id " << node.id << ", type '" << node.type << "')";
    return std::move(text).str();
}

[[noreturn]] inline void failNode(const NodeInstance& node, const std::string& what) {
    throw EvaluationException(describeNode(node) + ": " + what, node.id, node.name);
}

[[nodiscard]] inline const ParameterValue& effectiveParameter(const NodeCatalog& catalog, const NodeInstance& node,
                                                              ParameterValues& effectiveParams, const char* key) {
    const auto authored = effectiveParams.find(key);
    if (authored != effectiveParams.end()) {
        return authored->second;
    }
    const auto declared = catalog.parameterDefault(node.type, key);
    if (!declared) {
        failNode(node, std::string("parameter '") + key + "' has no declared default");
    }
    return effectiveParams.emplace(key, *declared).first->second;
}

[[nodiscard]] inline std::array<float, 4> effectiveColor4(const NodeCatalog& catalog, const NodeInstance& node,
                                                          ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* color = std::get_if<ColorValue>(&value);
    if (color == nullptr) {
        failNode(node, std::string("parameter '") + key + "' must be a color, got '" + parameterValueText(value) + "'");
    }
    return color->value;
}

// ---------------------------------------------------------------------------
// Native effect parameter contract (issue #34), shared by the CPU reference
// and the GPU executor. Executors own the pixel math; these helpers own the
// typed interpretation and admissibility of the authored metadata, so both
// executors reject the same inputs with the same node-identifying errors.
// ---------------------------------------------------------------------------

// Channel bitmask: R1/G2/B4/A8. Grade and Blur choices share it.
inline constexpr std::uint32_t kEffectChannelR = 1U;
inline constexpr std::uint32_t kEffectChannelG = 2U;
inline constexpr std::uint32_t kEffectChannelB = 4U;
inline constexpr std::uint32_t kEffectChannelA = 8U;

// Common optional-mask controls. `channel` is -1 for none, else the stored
// channel index (0 R, 1 G, 2 B, 3 A); `mix` is the blend weight.
struct EffectMaskParameters {
    int channel{-1};
    bool invert{false};
    float mix{1.0F};
};

// Grade coefficients are per-channel color values; `channels` selects which
// bitmask channels are transformed. Gamma is per channel.
struct GradeParameters {
    std::array<float, 4> blackpoint{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> whitepoint{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> lift{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> gain{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> multiply{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> offset{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> gamma{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB};
    bool reverse{false};
    bool clampBlack{true};
    bool clampWhite{false};
};

struct BlurParameters {
    float size{0.0F};
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA};
};

struct TransformParameters {
    float translateX{0.0F};
    float translateY{0.0F};
    float scale{1.0F};
    float rotate{0.0F};
    // 0 Cubic, 1 Linear, 2 Nearest (a plain int, not an enum, per contract).
    int filter{0};
};

[[nodiscard]] inline float effectiveNumber(const NodeCatalog& catalog, const NodeInstance& node,
                                           ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* number = std::get_if<double>(&value);
    if (number == nullptr || !std::isfinite(*number) || !std::isfinite(static_cast<float>(*number))) {
        failNode(node, std::string("parameter '") + key + "' must be a finite, float-representable number, got '" +
                           parameterValueText(value) + "'");
    }
    return static_cast<float>(*number);
}

[[nodiscard]] inline bool effectiveFlag(const NodeCatalog& catalog, const NodeInstance& node,
                                        ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr) {
        failNode(node, std::string("parameter '") + key + "' must be boolean, got '" + parameterValueText(value) + "'");
    }
    return *flag;
}

[[nodiscard]] inline const std::string& effectiveChoice(const NodeCatalog& catalog, const NodeInstance& node,
                                                        ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* choice = std::get_if<ChoiceValue>(&value);
    if (choice == nullptr) {
        failNode(node,
                 std::string("parameter '") + key + "' must be a choice, got '" + parameterValueText(value) + "'");
    }
    return choice->value;
}

[[nodiscard]] inline EffectMaskParameters effectiveEffectMask(const NodeCatalog& catalog, const NodeInstance& node,
                                                              ParameterValues& effectiveParams) {
    EffectMaskParameters mask;
    const std::string& channel = effectiveChoice(catalog, node, effectiveParams, "maskChannel");
    if (channel == "none") {
        mask.channel = -1;
    } else if (channel == "R") {
        mask.channel = 0;
    } else if (channel == "G") {
        mask.channel = 1;
    } else if (channel == "B") {
        mask.channel = 2;
    } else if (channel == "A") {
        mask.channel = 3;
    } else {
        failNode(node, "parameter 'maskChannel' must be one of none, R, G, B, A, got '" + channel + "'");
    }
    mask.invert = effectiveFlag(catalog, node, effectiveParams, "invertMask");
    mask.mix = effectiveNumber(catalog, node, effectiveParams, "mix");
    if (!(mask.mix >= 0.0F) || !(mask.mix <= 1.0F)) {
        failNode(node, "parameter 'mix' must be within [0, 1]");
    }
    return mask;
}

[[nodiscard]] inline GradeParameters effectiveGrade(const NodeCatalog& catalog, const NodeInstance& node,
                                                    ParameterValues& effectiveParams) {
    GradeParameters grade;
    grade.blackpoint = effectiveColor4(catalog, node, effectiveParams, "blackpoint");
    grade.whitepoint = effectiveColor4(catalog, node, effectiveParams, "whitepoint");
    grade.lift = effectiveColor4(catalog, node, effectiveParams, "lift");
    grade.gain = effectiveColor4(catalog, node, effectiveParams, "gain");
    grade.multiply = effectiveColor4(catalog, node, effectiveParams, "multiply");
    grade.offset = effectiveColor4(catalog, node, effectiveParams, "offset");
    grade.gamma = effectiveColor4(catalog, node, effectiveParams, "gamma");
    const std::string& channels = effectiveChoice(catalog, node, effectiveParams, "channels");
    if (channels == "RGB") {
        grade.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB;
    } else if (channels == "RGBA") {
        grade.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA;
    } else if (channels == "R") {
        grade.channels = kEffectChannelR;
    } else if (channels == "G") {
        grade.channels = kEffectChannelG;
    } else if (channels == "B") {
        grade.channels = kEffectChannelB;
    } else if (channels == "Alpha") {
        grade.channels = kEffectChannelA;
    } else if (channels == "None") {
        grade.channels = 0U;
    } else {
        failNode(node, "parameter 'channels' must be one of RGB, RGBA, R, G, B, Alpha, None, got '" + channels + "'");
    }
    grade.reverse = effectiveFlag(catalog, node, effectiveParams, "reverse");
    grade.clampBlack = effectiveFlag(catalog, node, effectiveParams, "clampBlack");
    grade.clampWhite = effectiveFlag(catalog, node, effectiveParams, "clampWhite");

    // Shared admissibility: every enabled channel must admit a finite forward
    // operation, and a reverse grade must also be invertible. Disabled
    // (unselected) channels pass through untouched and are not constrained,
    // even when their coefficients are singular.
    for (int channel = 0; channel < 4; ++channel) {
        if ((grade.channels & (1U << channel)) == 0)
            continue;
        const float gamma = grade.gamma[static_cast<std::size_t>(channel)];
        if (!(gamma > 0.0F) || !std::isfinite(gamma)) {
            failNode(node, "parameter 'gamma' must be finite and positive for every enabled channel");
        }
        // Every coefficient component of an enabled channel must itself be
        // finite; disabled channels are exempt.
        const auto componentFinite = [&](const std::array<float, 4>& values) {
            return std::isfinite(values[static_cast<std::size_t>(channel)]);
        };
        if (!componentFinite(grade.blackpoint) || !componentFinite(grade.whitepoint) || !componentFinite(grade.lift) ||
            !componentFinite(grade.gain) || !componentFinite(grade.multiply) || !componentFinite(grade.offset) ||
            !componentFinite(grade.gamma)) {
            failNode(node, "grade color coefficients must be finite for every enabled channel");
        }
        // Forward uses 1/gamma, reverse uses gamma; the exponent actually
        // applied must be representable as a finite float.
        const float exponent = grade.reverse ? gamma : 1.0F / gamma;
        if (!std::isfinite(exponent)) {
            failNode(node, "parameter 'gamma' has no finite exponent for an enabled channel");
        }
        const float blackpoint = grade.blackpoint[static_cast<std::size_t>(channel)];
        const float whitepoint = grade.whitepoint[static_cast<std::size_t>(channel)];
        if (whitepoint == blackpoint) {
            failNode(node, "parameters 'whitepoint' and 'blackpoint' must differ for every enabled channel");
        }
        const float slope =
            (grade.gain[static_cast<std::size_t>(channel)] - grade.lift[static_cast<std::size_t>(channel)]) *
            grade.multiply[static_cast<std::size_t>(channel)] / (whitepoint - blackpoint);
        const float intercept = grade.lift[static_cast<std::size_t>(channel)] +
                                grade.offset[static_cast<std::size_t>(channel)] - blackpoint * slope;
        if (!std::isfinite(slope) || !std::isfinite(intercept)) {
            failNode(node, "grade parameters produce unrepresentable coefficients for an enabled channel");
        }
        if (grade.reverse && slope == 0.0F) {
            failNode(node, "reverse grade requires a nonzero slope for every enabled channel");
        }
    }
    return grade;
}

[[nodiscard]] inline BlurParameters effectiveBlur(const NodeCatalog& catalog, const NodeInstance& node,
                                                  ParameterValues& effectiveParams) {
    BlurParameters blur;
    blur.size = effectiveNumber(catalog, node, effectiveParams, "size");
    if (!(blur.size >= 0.0F) || !(blur.size <= 100.0F)) {
        failNode(node, "parameter 'size' must be within [0, 100]");
    }
    const std::string& channels = effectiveChoice(catalog, node, effectiveParams, "channels");
    if (channels == "RGBA") {
        blur.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA;
    } else if (channels == "RGB") {
        blur.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB;
    } else if (channels == "Alpha") {
        blur.channels = kEffectChannelA;
    } else {
        failNode(node, "parameter 'channels' must be one of RGBA, RGB, Alpha, got '" + channels + "'");
    }
    return blur;
}

[[nodiscard]] inline TransformParameters effectiveTransform(const NodeCatalog& catalog, const NodeInstance& node,
                                                            ParameterValues& effectiveParams) {
    TransformParameters transform;
    transform.translateX = effectiveNumber(catalog, node, effectiveParams, "translateX");
    transform.translateY = effectiveNumber(catalog, node, effectiveParams, "translateY");
    transform.scale = effectiveNumber(catalog, node, effectiveParams, "scale");
    if (!(transform.scale >= 0.1F) || !(transform.scale <= 3.0F)) {
        failNode(node, "parameter 'scale' must be within [0.1, 3]");
    }
    transform.rotate = effectiveNumber(catalog, node, effectiveParams, "rotate");
    const std::string& filter = effectiveChoice(catalog, node, effectiveParams, "filter");
    if (filter == "Cubic") {
        transform.filter = 0;
    } else if (filter == "Linear") {
        transform.filter = 1;
    } else if (filter == "Nearest") {
        transform.filter = 2;
    } else {
        failNode(node, "parameter 'filter' must be one of Cubic, Linear, Nearest, got '" + filter + "'");
    }
    return transform;
}

// Resolves one expanded occurrence's static overrides and request-local
// animation without copying nodes that have neither. The returned pointer is
// valid while `localNode` remains in scope. This metadata seam is shared by
// the CPU evaluator, GPU executor, and viewer-key query; it never mutates the
// captured Document snapshot.
[[nodiscard]] inline const NodeInstance* resolveEffectiveNode(const Document& document,
                                                              const ExpandedNode& expandedNode,
                                                              std::optional<NodeInstance>& localNode, double time) {
    const NodeInstance& authoredNode = *expandedNode.node;
    const NodeInstance* effectiveNode = &authoredNode;
    if (expandedNode.id.instance != kInvalidNetworkInstance) {
        const NetworkInstance* occurrence = document.instance(expandedNode.id.instance);
        if (occurrence == nullptr)
            throw EvaluationException("evaluation references missing network instance");
        if (const auto overrides = occurrence->params.find(authoredNode.id); overrides != occurrence->params.end()) {
            localNode = authoredNode;
            for (const auto& [key, value] : overrides->second)
                localNode->params[key] = value;
            effectiveNode = &*localNode;
        }
    }

    bool hasAnimation = false;
    for (const auto& channel : document.animationChannels()) {
        if (channel.address.network == expandedNode.id.network && channel.address.node == authoredNode.id &&
            (channel.address.instance == kInvalidNetworkInstance ||
             channel.address.instance == expandedNode.id.instance)) {
            hasAnimation = true;
            break;
        }
    }
    if (hasAnimation) {
        if (!localNode)
            localNode = *effectiveNode;
        applyAnimationParameters(document, expandedNode.id.network, authoredNode.id, expandedNode.id.instance, time,
                                 localNode->params);
        effectiveNode = &*localNode;
    }
    return effectiveNode;
}

}  // namespace nemo
