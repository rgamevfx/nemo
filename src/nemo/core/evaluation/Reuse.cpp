#include "nemo/core/evaluation/Reuse.hpp"

#include <algorithm>
#include <map>

#include "nemo/core/Hashing.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
namespace nemo {
namespace {

// Length-prefixed params records: count, then per record keyLen:key +
// valueLen:value. Injective over the whole map regardless of content. Values
// use their tagged, lossless canonical representation so distinct typed values
// never alias.
[[nodiscard]] std::string canonicalParams(const NodeInstance& node) {
    std::string canonical = std::to_string(node.params.size());
    canonical.push_back(':');
    for (const auto& [key, value] : node.params) {
        canonical += std::to_string(key.size());
        canonical.push_back(':');
        canonical += key;
        const std::string encoded = canonicalParameterValue(value);
        canonical += std::to_string(encoded.size());
        canonical.push_back(':');
        canonical += encoded;
    }
    return canonical;
}

// Canonical source-reference content for a source node's key (issue #11):
// the key it addresses plus the full reference (path, time mapping,
// interpretation policy), length-prefixed and injective. A source edit
// therefore invalidates only this node and its dependents, while the rest
// of the graph keeps its reuse identity.
[[nodiscard]] std::string canonicalSource(const Document& document, const NodeInstance& node) {
    const auto paramIt = node.params.find("source");
    const auto* sourceValue = paramIt != node.params.end() ? std::get_if<std::string>(&paramIt->second) : nullptr;
    const std::string key = sourceValue != nullptr ? *sourceValue : std::string{};
    const auto it = document.sources.find(key);
    std::string out;
    if (it == document.sources.end()) {
        appendCanonicalField(out, "key", key);
        appendCanonicalField(out, "unresolved", "1");
        return out;
    }
    appendCanonicalField(out, "key", key);
    appendCanonicalField(out, "path", it->second.path);
    appendCanonicalField(out, "offset", std::to_string(it->second.frameOffset));
    appendCanonicalField(out, "step", std::to_string(it->second.frameStep));
    appendCanonicalField(out, "revision", std::to_string(it->second.revision));
    appendCanonicalField(out, "interpretation", [&] {
        std::string text;
        for (const auto& [tag, value] : it->second.interpretation) {
            appendCanonicalField(text, tag.c_str(), value);
        }
        return text;
    }());
    return out;
}

}  // namespace

ResultKey nodeResultKey(const Document& document, const NodeInstance& node,
                        const std::vector<std::uint64_t>& inputKeyHashes, const EvaluationRequest& request,
                        const KeyContext& context) {
    //   network|impl|type|params|inputs|source|time|region|scale|channels|quality|working|tag
    // Input identity enters through the inputs' key hashes in port order,
    // so a change anywhere upstream changes every downstream key while
    // unrelated branches keep theirs (spec section 10.3: reuse follows
    // effective dependencies). Number fields (time, region, scale, input
    // hashes) are decimal and self-delimiting.
    //
    // Source nodes additionally carry the persistent source reference in
    // the canonical form: a Document::sources edit changes the key of the
    // source node and everything downstream of it, and only those (issue
    // #11 acceptance: source map changes invalidate only dependent
    // content). samplingScale is part of the identity like quality (spec
    // section 8: a reduced result must not satisfy a higher-resolution
    // request); region stays full-resolution so scale and ROI are
    // distinguishable in the identity.
    std::string canonical;
    canonical.reserve(112 + node.params.size() * 24);
    appendCanonicalField(canonical, "network", std::to_string(request.network));
    appendCanonicalField(
        canonical, "impl",
        std::to_string(
            document.network(request.network).graph().catalog().implementationVersion(node.type).value_or(1)));
    appendCanonicalField(canonical, "type", node.type);
    appendCanonicalField(canonical, "params", canonicalParams(node));
    canonical += "inputs:";
    for (const std::uint64_t inputHash : inputKeyHashes) {
        canonical += std::to_string(inputHash);
        canonical.push_back(',');
    }
    canonical.push_back('\x1F');
    if (node.type == "source") {
        appendCanonicalField(canonical, "source", canonicalSource(document, node));
    }
    appendCanonicalField(canonical, "time", std::to_string(request.localTime));
    appendCanonicalField(canonical, "region",
                         std::to_string(request.region.x) + ',' + std::to_string(request.region.y) + ',' +
                             std::to_string(request.region.width) + ',' + std::to_string(request.region.height));
    appendCanonicalField(canonical, "scale", std::to_string(request.samplingScale));
    appendCanonicalField(canonical, "domain",
                         std::to_string(request.imageWidth()) + ',' + std::to_string(request.imageHeight()));
    appendCanonicalField(canonical, "channels", request.channels);
    appendCanonicalField(canonical, "quality", qualityName(request.quality));
    appendCanonicalField(canonical, "working", document.color.workingSpace);
    appendCanonicalField(canonical, "tag", std::to_string(context.implementationTag));

    ResultKey key;
    key.canonical = std::move(canonical);
    key.hash = kFnv1a64Basis;
    hashMixText(key.hash, key.canonical);
    return key;
}

ResultKey viewerResultKey(const ResultKey& sceneLinearKey, const ColorPolicy& policy) {
    // Viewer representations bake the viewing transform (and delivery
    // interpretation for delivery outputs); upstream scene-linear results
    // must stay valid when only viewing state changes (ADR-0004).
    std::string canonical;
    canonical.reserve(sceneLinearKey.canonical.size() + policy.viewerTransform.size() +
                      policy.deliveryTransform.size() + 32);
    appendCanonicalField(canonical, "scene", sceneLinearKey.canonical);
    appendCanonicalField(canonical, "view", policy.viewerTransform);
    appendCanonicalField(canonical, "delivery", policy.deliveryTransform);
    ResultKey key;
    key.canonical = std::move(canonical);
    key.hash = kFnv1a64Basis;
    hashMixText(key.hash, key.canonical);
    return key;
}

}  // namespace nemo
