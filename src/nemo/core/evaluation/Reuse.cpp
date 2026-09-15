#include "nemo/core/evaluation/Reuse.hpp"

#include <algorithm>
#include <map>

#include "nemo/core/Hashing.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
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

// Canonical effective-source content for a source node's key (issues #11/#75):
// the resolved request — path, content revision, effective mapping, selected
// coverage, policies, media-interpretation hints and authored color choices —
// plus the color-config content identity the executor supplied. Node identity
// and the shared source key are deliberately absent, so equivalent effective
// requests match wherever they are reached from, while two Reads of one file
// with different mapping, policies or interpretation never alias.
//
// The color-config identity is mixed here rather than into every key: it enters
// the graph at the source seam, and every dependent inherits it through its
// inputs' key hashes (ADR-0004/ADR-0007).
[[nodiscard]] std::string canonicalSource(const Document& document, const NodeInstance& node, std::int64_t localTime,
                                          std::string_view colorConfigIdentity) {
    const auto paramIt = node.params.find("source");
    const auto* sourceValue = paramIt != node.params.end() ? std::get_if<std::string>(&paramIt->second) : nullptr;
    const std::string key = sourceValue != nullptr ? *sourceValue : std::string{};
    if (!document.sources.contains(key)) {
        std::string out;
        appendCanonicalField(out, "key", key);
        appendCanonicalField(out, "unresolved", "1");
        return out;
    }
    // A malformed authored choice or an unrepresentable mapping is an
    // evaluation error with node identity; letting it surface here keeps key
    // computation from inventing a fallback identity for a node that cannot run.
    const EffectiveSourceRequest request = resolveSourceRequest(document, node, localTime);
    std::string out;
    appendEffectiveSourceIdentity(out, request, document.color.workingSpace, colorConfigIdentity);
    return out;
}

}  // namespace

ResultKey nodeResultKey(const Document& document, const NodeInstance& node,
                        const std::vector<std::uint64_t>& inputKeyHashes, const EvaluationRequest& request,
                        const KeyContext& context) {
    //   network|impl|type|effectiveParams|inputs|source|time|region|scale|
    //   channels|quality|working|tag
    // Input identity enters through the inputs' key hashes in port order,
    // so a change anywhere upstream changes every downstream key while
    // unrelated branches keep theirs (spec section 10.3: reuse follows
    // effective dependencies). `node` is a request-local resolved copy when
    // evaluating animation; no document revision is part of this identity.
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
        appendCanonicalField(canonical, "source",
                             canonicalSource(document, node, request.localTime, context.colorConfigIdentity));
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

ResultKey nodeContentKey(const Document& document, const NodeInstance& node,
                         const std::vector<std::uint64_t>& inputContentHashes, const EvaluationRequest& request,
                         const KeyContext& context) {
    EvaluationRequest full = request;
    full.fullWidth = request.imageWidth();
    full.fullHeight = request.imageHeight();
    full.region = {0, 0, full.fullWidth, full.fullHeight};
    return nodeResultKey(document, node, inputContentHashes, full, context);
}

ResultKey regionResultKey(const ResultKey& contentKey, const EvaluationRequest& request) {
    ResultKey key;
    key.canonical.reserve(contentKey.canonical.size() + 96);
    appendCanonicalField(key.canonical, "spatial-v1", contentKey.canonical);
    appendCanonicalField(key.canonical, "coverage",
                         std::to_string(request.region.x) + ',' + std::to_string(request.region.y) + ',' +
                             std::to_string(request.region.width) + ',' + std::to_string(request.region.height));
    appendCanonicalField(key.canonical, "scale", std::to_string(request.samplingScale));
    appendCanonicalField(key.canonical, "domain",
                         std::to_string(request.imageWidth()) + ',' + std::to_string(request.imageHeight()));
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
