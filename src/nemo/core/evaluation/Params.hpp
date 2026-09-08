#pragma once

#include <array>
#include <map>
#include <sstream>
#include <string>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

namespace nemo {

// Node-describing error helpers and effective-parameter parsing shared by
// the CPU reference and the native GPU effect executor (spec section 10.3:
// both executors consume the same effective parameter/input state). Parsed
// values are recorded into `effectiveParams` — the plan carries resolved
// state, not authored guesses.

[[nodiscard]] inline std::string describeNode(const Node& node) {
    std::ostringstream text;
    text << "node '" << node.name << "' (id " << node.id << ", type '" << node.type << "')";
    return std::move(text).str();
}

[[noreturn]] inline void failNode(const Node& node, const std::string& what) {
    throw EvaluationException(describeNode(node) + ": " + what, node.id, node.name);
}

[[nodiscard]] inline std::array<float, 4> parseColor4(const Node& node,
                                                      std::map<std::string, std::string>& effectiveParams,
                                                      const char* key, std::array<float, 4> fallback) {
    const auto it = effectiveParams.find(key);
    if (it == effectiveParams.end()) {
        effectiveParams.emplace(key, [&] {
            std::ostringstream text;
            text << fallback[0] << ' ' << fallback[1] << ' ' << fallback[2] << ' ' << fallback[3];
            return std::move(text).str();
        }());
        return fallback;
    }
    std::istringstream stream(it->second);
    std::array<float, 4> value{};
    for (float& channel : value) {
        if (!(stream >> channel)) {
            failNode(node, std::string("parameter '") + key + "' must be 4 floats, got '" + it->second + "'");
        }
    }
    std::string extra;
    if (stream >> extra) {
        failNode(node, std::string("parameter '") + key + "' has extra tokens: '" + it->second + "'");
    }
    return value;
}

}  // namespace nemo
