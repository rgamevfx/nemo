#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"

namespace nemo {

// Versioned JSON persistence (schema version rides in the file). Loading
// retains unknown node types and reports them as warnings instead of
// dropping data: projects must recover when dependencies return
// (spec section 10.7).
struct LoadResult {
    Document document;
    std::vector<std::string> warnings;
};

[[nodiscard]] nlohmann::json saveDocument(const Document& document);
[[nodiscard]] LoadResult loadDocument(const nlohmann::json& json);

// Thrown when the file is structurally unusable (bad schema, malformed JSON).
struct DeserializeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace nemo
