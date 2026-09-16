#pragma once

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
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
                                                              const ParameterValues& effectiveParams, const char* key) {
    const auto authored = effectiveParams.find(key);
    if (authored != effectiveParams.end()) {
        return authored->second;
    }
    const auto declared = catalog.parameterDefault(node.type, key);
    if (!declared) {
        failNode(node, std::string("parameter '") + key + "' has no declared default");
    }
    return *declared;
}

[[nodiscard]] inline std::array<float, 4> effectiveColor4(const NodeCatalog& catalog, const NodeInstance& node,
                                                          const ParameterValues& effectiveParams, const char* key) {
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

[[nodiscard]] inline float effectiveNumber(const NodeCatalog& catalog, const NodeInstance& node,
                                           const ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* number = std::get_if<double>(&value);
    if (number == nullptr || !std::isfinite(*number) || !std::isfinite(static_cast<float>(*number))) {
        failNode(node, std::string("parameter '") + key + "' must be a finite, float-representable number, got '" +
                           parameterValueText(value) + "'");
    }
    return static_cast<float>(*number);
}

[[nodiscard]] inline bool effectiveFlag(const NodeCatalog& catalog, const NodeInstance& node,
                                        const ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr) {
        failNode(node, std::string("parameter '") + key + "' must be boolean, got '" + parameterValueText(value) + "'");
    }
    return *flag;
}

[[nodiscard]] inline const std::string& effectiveChoice(const NodeCatalog& catalog, const NodeInstance& node,
                                                        const ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* choice = std::get_if<ChoiceValue>(&value);
    if (choice == nullptr) {
        failNode(node,
                 std::string("parameter '") + key + "' must be a choice, got '" + parameterValueText(value) + "'");
    }
    return choice->value;
}

// A free-text parameter (a name, a selector, a path fragment). The value is
// returned verbatim: this seam never trims, normalizes or renames authored
// text, and an authored name is an exact identifier wherever it is consumed.
[[nodiscard]] inline std::string effectiveText(const NodeCatalog& catalog, const NodeInstance& node,
                                               const ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr) {
        failNode(node, std::string("parameter '") + key + "' must be text, got '" + parameterValueText(value) + "'");
    }
    return *text;
}

[[nodiscard]] inline EffectMaskParameters effectiveEffectMask(const NodeCatalog& catalog, const NodeInstance& node,
                                                              const ParameterValues& effectiveParams) {
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
