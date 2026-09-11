#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"

namespace nemo {

// Versioned JSON persistence for the document's network definitions and
// instances. Network-local node/edge/interface identities, typed terminals,
// authored layouts/routes, bindings, and allocator high watermarks round-trip
// verbatim. Unknown node types remain inspectable data and are reported as
// warnings rather than silently dropped.
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
