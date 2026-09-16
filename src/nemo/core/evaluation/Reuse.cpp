#include "nemo/core/evaluation/Reuse.hpp"

#include <algorithm>
#include <bit>
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
// `resolved` is the request the plan already resolved for this node (issue
// #88), so key computation and execution can never disagree about which frame
// a source node reads; without one the request is resolved here, and a Read
// with no bound source reference keeps the defined "unresolved" identity
// instead of inventing a frame.
//
// The color-config identity is mixed here rather than into every key: it enters
// the graph at the source seam, and every dependent inherits it through its
// inputs' key hashes (ADR-0004/ADR-0007).
[[nodiscard]] std::string canonicalSource(const Document& document, const NodeInstance& node, std::int64_t localTime,
                                          std::string_view colorConfigIdentity,
                                          const EffectiveSourceRequest* resolved) {
    std::string out;
    if (resolved != nullptr) {
        appendEffectiveSourceIdentity(out, *resolved, document.color.workingSpace, colorConfigIdentity);
        return out;
    }
    const auto paramIt = node.params.find("source");
    const auto* sourceValue = paramIt != node.params.end() ? std::get_if<std::string>(&paramIt->second) : nullptr;
    const std::string key = sourceValue != nullptr ? *sourceValue : std::string{};
    if (!document.sources.contains(key)) {
        appendCanonicalField(out, "key", key);
        appendCanonicalField(out, "unresolved", "1");
        return out;
    }
    // A malformed authored choice or an unrepresentable mapping is an
    // evaluation error with node identity; letting it surface here keeps key
    // computation from inventing a fallback identity for a node that cannot run.
    const EffectiveSourceRequest request = resolveSourceRequest(document, node, localTime);
    appendEffectiveSourceIdentity(out, request, document.color.workingSpace, colorConfigIdentity);
    return out;
}

[[nodiscard]] std::string canonicalRegion(const Region& region) {
    return std::to_string(region.x) + ',' + std::to_string(region.y) + ',' + std::to_string(region.width) + ',' +
           std::to_string(region.height);
}

// Canonical content of one described image (issue #88). Every semantic field
// participates: the logical format and the signed data window (so sliding a
// window, or losing overscan, is a different image), the retained-edge-domain
// claim (issue #92: the same rectangle answered outside of is a different
// image than one that is transparent there), the pixel aspect as exact bits
// (not a decimal rendering, which would lose a one-ULP difference), the
// channel naming, precision, alpha association and colour interpretation. A
// description-less key (a direct caller that has no plan) simply omits the
// block; the coordinate contract version is mixed in regardless, so a change to
// what the numbers mean can never be served from an older representation.
[[nodiscard]] std::string canonicalDescription(const ImageDescription& description) {
    std::string canonical;
    appendCanonicalField(canonical, "format", canonicalRegion(description.format));
    appendCanonicalField(canonical, "data", canonicalRegion(description.dataBounds));
    // The retained-edge-domain claim (issue #92): two images with the same
    // rectangle but a different answer outside it are different images, so a
    // result produced under one can never be served for the other.
    appendCanonicalField(canonical, "edge", description.edgeExtension ? "1" : "0");
    appendCanonicalField(canonical, "par", std::to_string(std::bit_cast<std::uint32_t>(description.pixelAspect)));
    for (const std::string& channel : description.channels) {
        appendCanonicalField(canonical, "channel", channel);
    }
    appendCanonicalField(canonical, "precision", std::to_string(static_cast<int>(description.precision)));
    appendCanonicalField(canonical, "association", std::to_string(static_cast<int>(description.association)));
    appendCanonicalField(canonical, "color", std::to_string(static_cast<int>(description.color)));
    return canonical;
}

}  // namespace

ResultKey nodeResultKey(const Document& document, const NodeInstance& node,
                        const std::vector<std::uint64_t>& inputKeyHashes, const EvaluationRequest& request,
                        const KeyContext& context) {
    //   network|impl|type|effectiveParams|inputs|source|coordinates|image|time|
    //   region|scale|channels|quality|working|tag
    // Input identity enters through the inputs' key hashes in port order,
    // so a change anywhere upstream changes every downstream key while
    // unrelated branches keep theirs (spec section 10.3: reuse follows
    // effective dependencies). `node` is a request-local resolved copy when
    // evaluating animation; no document revision is part of this identity.
    //
    // Source nodes additionally carry their resolved effective request in the
    // canonical form, and every node carries the described image it produces
    // plus the coordinate contract version: a Document::sources edit changes the
    // key of the source node and everything downstream of it, and only those
    // (issue #11 acceptance), while a changed format, data window, aspect,
    // channel naming, association or interpretation can never serve an image
    // with different meaning (issue #88 story 89). samplingScale is part of the
    // identity like quality (spec section 8: a reduced result must not satisfy a
    // higher-resolution request); region stays full-resolution so scale and ROI
    // are distinguishable in the identity.
    std::string canonical;
    canonical.reserve(224 + node.params.size() * 24);
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
        appendCanonicalField(
            canonical, "source",
            canonicalSource(document, node, request.localTime, context.colorConfigIdentity, context.source));
    }
    // The described image this key is computed for (issue #88), and the version
    // of the coordinate contract it is expressed in. A direct caller that has no
    // plan supplies neither and keeps a description-independent key; every
    // executor supplies the description, so format, data window, aspect,
    // channels, precision, association and interpretation all decide reuse.
    appendCanonicalField(canonical, "coordinates", kImageCoordinateContract);
    if (context.description != nullptr) {
        appendCanonicalField(canonical, "image", canonicalDescription(*context.description));
    }
    appendCanonicalField(canonical, "time", std::to_string(request.localTime));
    appendCanonicalField(canonical, "region",
                         std::to_string(request.region.x) + ',' + std::to_string(request.region.y) + ',' +
                             std::to_string(request.region.width) + ',' + std::to_string(request.region.height));
    appendCanonicalField(canonical, "scale", std::to_string(request.samplingScale));
    appendCanonicalField(canonical, "domain",
                         std::to_string(request.imageWidth()) + ',' + std::to_string(request.imageHeight()));
    // The demanded channel names enter the identity explicitly and in declared
    // order (issue #90): an auxiliary demand is never collapsed away, so a
    // result computed without it can never serve a request that asks for it.
    // An empty demand is the "every named channel" default and contributes no
    // name fields, which is distinct from any explicit name.
    canonical += "channels:";
    for (const std::string& channel : request.channels) {
        appendCanonicalField(canonical, "channel", channel);
    }
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
