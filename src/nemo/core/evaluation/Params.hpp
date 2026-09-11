#pragma once

#include <array>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

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

}  // namespace nemo
