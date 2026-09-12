#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"

namespace nemo {

// Versioned JSON persistence for the document's network definitions and
// instances. Schema v3 stores authored node and instance parameters as
// explicitly tagged ParameterValue records; v1/v2 text parameters are parsed
// through the active catalog during migration. Network-local node/edge/interface
// identities, typed terminals, authored layouts/routes, bindings, allocator
// high watermarks, media-library bins/entries/metadata/marks, and animation
// round-trip verbatim. Unknown node types remain inspectable data and are
// reported as warnings rather than silently dropped.
//
// The document root carries a format discriminator and explicit required-feature
// metadata: {"format": "nemo", "requiredFeatures": [{"id": ..., "version": ...}]}.
// Files without them are legacy and migrate through the "schema" field. Saving
// records the authored processing features the document uses; loading rejects an
// unknown or newer required feature with an actionable diagnostic instead of
// guessing its semantics. A newer "schema" is rejected the same way. Authored
// JSON this build does not model is retained verbatim in the model
// (Document::extension and the per-record extension members) so a load/save
// cycle loses nothing and a deleted record does not resurrect. A missing node
// type stays a warning, and its parameter records are preserved opaquely when
// they cannot be typed. A top-level "presentation" record belongs to the
// session/file envelope and is ignored by this codec.
//
// Source and color-config paths are stored verbatim; resolving or rebasing them
// to a project location is the file layer's job, never a codec side effect.
struct LoadResult {
    Document document;
    std::vector<std::string> warnings;
};

// Project format discriminator written by saveDocument.
inline constexpr std::string_view kProjectFormat = "nemo";

[[nodiscard]] nlohmann::json saveDocument(const Document& document);
// The caller supplies its active immutable schema snapshot. Catalog metadata
// is not written into the document; unavailable types still remain recoverable.
[[nodiscard]] LoadResult loadDocument(const nlohmann::json& json,
                                      std::shared_ptr<const NodeCatalog> catalog = builtinNodeCatalogPtr());

// Thrown when the file is structurally unusable (bad schema, malformed JSON) or
// requires a processing feature this build does not support.
struct DeserializeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace nemo
