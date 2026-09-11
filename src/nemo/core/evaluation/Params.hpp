#pragma once

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <array>
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
