#pragma once

#include <array>
#include <cmath>
#include <map>
#include <sstream>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

namespace nemo {

// Node-describing error helpers and effective-parameter parsing shared by
// the CPU reference and the native GPU effect executor (spec section 10.3:
// both executors consume the same effective parameter/input state). Parsed
// values are recorded into `effectiveParams` — the plan carries resolved
// state, not authored guesses.

[[nodiscard]] inline std::string describeNode(const NodeInstance& node) {
    std::ostringstream text;
    text << "node '" << node.name << "' (id " << node.id << ", type '" << node.type << "')";
    return std::move(text).str();
}

[[noreturn]] inline void failNode(const NodeInstance& node, const std::string& what) {
    throw EvaluationException(describeNode(node) + ": " + what, node.id, node.name);
}

[[nodiscard]] inline const std::string& effectiveParameter(const NodeCatalog& catalog, const NodeInstance& node,
                                                           std::map<std::string, std::string>& effectiveParams,
                                                           const char* key) {
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

[[nodiscard]] inline std::array<float, 4> parseColor4(const NodeCatalog& catalog, const NodeInstance& node,
                                                      std::map<std::string, std::string>& effectiveParams,
                                                      const char* key) {
    const auto& text = effectiveParameter(catalog, node, effectiveParams, key);
    std::istringstream stream(text);
    std::array<float, 4> value{};
    for (float& channel : value) {
        if (!(stream >> channel) || !std::isfinite(channel)) {
            failNode(node, std::string("parameter '") + key + "' must be 4 finite floats, got '" + text + "'");
        }
    }
    std::string extra;
    if (stream >> extra) {
        failNode(node, std::string("parameter '") + key + "' has extra tokens: '" + text + "'");
    }
    return value;
}

}  // namespace nemo
