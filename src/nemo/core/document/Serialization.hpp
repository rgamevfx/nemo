#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"

namespace nemo {

// Versioned JSON persistence (schema version rides in the file). Node and
// edge identities plus allocator high watermarks are persisted verbatim.
// Loading retains unknown node types and authored parameters, and reports
// unavailable node implementations as warnings instead of dropping data
// (spec section 10.7).
struct LoadResult {
    Document document;
    std::vector<std::string> warnings;
};

[[nodiscard]] nlohmann::json saveDocument(const Document& document);
// The caller supplies its active immutable schema snapshot. Catalog metadata
// is not written into the document; unavailable types still remain recoverable.
[[nodiscard]] LoadResult loadDocument(const nlohmann::json& json,
                                      std::shared_ptr<const NodeCatalog> catalog = builtinNodeCatalogPtr());

// Thrown when the file is structurally unusable (bad schema, malformed JSON).
struct DeserializeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace nemo
