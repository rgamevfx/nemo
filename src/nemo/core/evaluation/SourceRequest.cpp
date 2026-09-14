#include "nemo/core/evaluation/SourceRequest.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
namespace {

[[nodiscard]] const NodeCatalog& documentCatalog(const Document& document) {
    return document.network(document.rootNetworkId()).graph().catalog();
}

// One authored value for a Read choice: the node's own param when authored,
// else the descriptor's declared default, else the frozen semantic default
// this header owns (so an older catalog still resolves identically).
[[nodiscard]] const ParameterValue* effectiveReadParameter(const Document& document, const NodeInstance& node,
                                                           std::string_view key) {
    const auto authored = node.params.find(std::string(key));
    if (authored != node.params.end())
        return &authored->second;
    return documentCatalog(document).parameterDefault(node.type, key);
}

[[noreturn]] void failRead(const NodeInstance& node, const std::string& what) {
    throw EvaluationException(describeNode(node) + ": " + what, node.id, node.name);
}

// Reads one authored value as text. A Choice or String param both author text;
// any other representation is a node error, never a silent default.
[[nodiscard]] std::string readAuthoredText(const Document& document, const NodeInstance& node, std::string_view key,
                                           const std::string& fallback) {
    const ParameterValue* value = effectiveReadParameter(document, node, key);
    if (value == nullptr)
        return fallback;
    if (const auto* choice = std::get_if<ChoiceValue>(value))
        return choice->value;
    if (const auto* text = std::get_if<std::string>(value))
        return *text;
    failRead(node, "parameter '" + std::string(key) + "' must be authored as a choice or a string, got '" +
                       parameterValueText(*value) + "'");
}

[[nodiscard]] std::int64_t readAuthoredInteger(const Document& document, const NodeInstance& node, std::string_view key,
                                               std::int64_t fallback) {
    const ParameterValue* value = effectiveReadParameter(document, node, key);
    if (value == nullptr)
        return fallback;
    const auto* number = std::get_if<std::int64_t>(value);
    if (number == nullptr)
        failRead(node,
                 "parameter '" + std::string(key) + "' must be an integer, got '" + parameterValueText(*value) + "'");
    return *number;
}

[[nodiscard]] std::string allowedList(std::initializer_list<std::string_view> allowed) {
    std::string supported;
    for (const std::string_view candidate : allowed) {
        if (!supported.empty())
            supported += ", ";
        supported += std::string(candidate);
    }
    return supported;
}

[[nodiscard]] std::optional<std::string> requireOneOf(const std::string& chosen, std::string_view what,
                                                      std::initializer_list<std::string_view> allowed) {
    if (std::find(allowed.begin(), allowed.end(), std::string_view(chosen)) != allowed.end())
        return std::nullopt;
    return std::string(what) + " must be one of " + allowedList(allowed) + ", got '" + chosen + "'";
}

[[nodiscard]] const MediaMetadata* mediaMetadataFor(const Document& document, std::string_view sourceKey) {
    for (const auto& entry : document.mediaCatalog().entries()) {
        if (entry.sourceKey == sourceKey)
            return &entry.metadata;
    }
    return nullptr;
}

// One vocabulary for the recognized media-interpretation hints: the shared
// interpretation key, the node parameter that overrides it, the override field,
// and the values this build can actually apply. Validation, the
// reference-to-parameter translation and the resolver's merged hint map all read
// this one table, so a value an adapter cannot apply is never invented into node
// state.
struct SourceHintVocabulary {
    std::string_view hintKey;
    std::string_view parameterName;
    std::string ReadNodeOverrides::*field;
    std::array<std::string_view, 5> allowed;
    std::size_t allowedCount;
};

inline constexpr SourceHintVocabulary kSourceHintVocabulary[] = {
    {kSourceHintTransfer,
     kReadParamSourceTransfer,
     &ReadNodeOverrides::sourceTransfer,
     {"bt709", "srgb", "gamma22", "gamma28", "linear"},
     5},
    {kSourceHintPrimaries,
     kReadParamSourcePrimaries,
     &ReadNodeOverrides::sourcePrimaries,
     {"bt709", "", "", "", ""},
     1},
    {kSourceHintMatrix, kReadParamSourceMatrix, &ReadNodeOverrides::sourceMatrix, {"bt709", "bt601", "", "", ""}, 2},
    {kSourceHintRange, kReadParamSourceRange, &ReadNodeOverrides::sourceRange, {"limited", "full", "", "", ""}, 2},
    {kSourceHintChromaLocation,
     kReadParamSourceChromaLocation,
     &ReadNodeOverrides::sourceChromaLocation,
     {"left", "", "", "", ""},
     1},
};

[[nodiscard]] const SourceHintVocabulary* sourceHintSpec(std::string_view hintKey) noexcept {
    for (const SourceHintVocabulary& spec : kSourceHintVocabulary) {
        if (spec.hintKey == hintKey)
            return &spec;
    }
    return nullptr;
}

[[nodiscard]] bool sourceHintValueSupported(std::string_view hintKey, std::string_view value) noexcept {
    const SourceHintVocabulary* spec = sourceHintSpec(hintKey);
    if (spec == nullptr)
        return false;
    for (std::size_t i = 0; i < spec->allowedCount; ++i) {
        if (spec->allowed[i] == value)
            return true;
    }
    return false;
}

[[nodiscard]] std::string supportedHintValues(const SourceHintVocabulary& spec) {
    std::string supported;
    for (std::size_t i = 0; i < spec.allowedCount; ++i) {
        if (!supported.empty())
            supported += ", ";
        supported += std::string(spec.allowed[i]);
    }
    return supported;
}

[[nodiscard]] const MediaProbeMetadata* committedProbe(const MediaMetadata* metadata) {
    return metadata != nullptr && metadata->committedProbe ? &*metadata->committedProbe : nullptr;
}

[[nodiscard]] BoundaryPolicy parseBoundaryPolicy(const std::string& value, BoundaryPolicy fallback) {
    if (value == "error")
        return BoundaryPolicy::Error;
    if (value == "hold")
        return BoundaryPolicy::Hold;
    if (value == "black")
        return BoundaryPolicy::Black;
    return fallback;
}

[[nodiscard]] MissingFramePolicy parseMissingPolicy(const std::string& value) {
    return value == "black" ? MissingFramePolicy::Black : MissingFramePolicy::Error;
}

// Records where the mapped request fell and the policy it selects. A policy
// decision never throws here: the consumer raises the node-identifying error at
// its own seam, so a facts query can inspect the same outcome.
void applyCoveragePolicy(EffectiveSourceRequest& request) {
    const EffectiveSourceMapping& mapping = request.mapping;
    request.status = SourceRequestStatus::Ok;
    request.readFrame = request.sourceFrame;
    request.transparentBlack = false;
    request.policyError = false;
    request.holdApplied = false;

    const bool bounded = mapping.boundsEnforced && mapping.bounded();
    const bool inside = bounded && mapping.covers(request.sourceFrame);
    if (bounded && !inside) {
        const bool before = request.sourceFrame < *mapping.firstFrame;
        request.status = before ? SourceRequestStatus::BeforeRange : SourceRequestStatus::AfterRange;
        const BoundaryPolicy policy = before ? request.beforePolicy : request.afterPolicy;
        switch (policy) {
        case BoundaryPolicy::Error:
            request.policyError = true;
            return;
        case BoundaryPolicy::Black:
            request.transparentBlack = true;
            return;
        case BoundaryPolicy::Hold:
            break;
        }
        // Hold reads the boundary frame itself. A boundary frame that is a hole
        // is still a missing-frame condition, never permission to seek another
        // file.
        const std::int64_t boundary = before ? *mapping.firstFrame : *mapping.lastFrame;
        if (request.coverage == CoverageQuality::Validated && request.missingAt(boundary)) {
            request.status = SourceRequestStatus::MissingFrame;
            request.policyError = request.missingPolicy == MissingFramePolicy::Error;
            request.transparentBlack = request.missingPolicy == MissingFramePolicy::Black;
            return;
        }
        request.readFrame = boundary;
        request.holdApplied = true;
        return;
    }

    // A hole only exists inside an interval whose discovery validated it; an
    // estimated coverage never fabricates a missing-frame failure.
    if (inside && request.coverage == CoverageQuality::Validated && request.missingAt(request.sourceFrame)) {
        request.status = SourceRequestStatus::MissingFrame;
        request.policyError = request.missingPolicy == MissingFramePolicy::Error;
        request.transparentBlack = request.missingPolicy == MissingFramePolicy::Black;
    }
}

}  // namespace

std::string_view sourceHintParameterName(std::string_view hintKey) noexcept {
    if (hintKey == kSourceHintTransfer)
        return kReadParamSourceTransfer;
    if (hintKey == kSourceHintPrimaries)
        return kReadParamSourcePrimaries;
    if (hintKey == kSourceHintMatrix)
        return kReadParamSourceMatrix;
    if (hintKey == kSourceHintRange)
        return kReadParamSourceRange;
    if (hintKey == kSourceHintChromaLocation)
        return kReadParamSourceChromaLocation;
    return {};
}

std::uint8_t sourceHintBit(std::string_view hintKey) noexcept {
    if (hintKey == kSourceHintTransfer)
        return kSourceHintTransferBit;
    if (hintKey == kSourceHintPrimaries)
        return kSourceHintPrimariesBit;
    if (hintKey == kSourceHintMatrix)
        return kSourceHintMatrixBit;
    if (hintKey == kSourceHintRange)
        return kSourceHintRangeBit;
    if (hintKey == kSourceHintChromaLocation)
        return kSourceHintChromaLocationBit;
    return 0U;
}

const char* sourceBoundsOriginName(SourceBoundsOrigin origin) noexcept {
    switch (origin) {
    case SourceBoundsOrigin::Unbounded:
        return "unbounded";
    case SourceBoundsOrigin::SharedReference:
        return "source-reference";
    case SourceBoundsOrigin::NodeCustom:
        return "node-custom";
    case SourceBoundsOrigin::DiscoveredFacts:
        return "discovered";
    }
    return "unbounded";
}

const char* sourceRequestStatusName(SourceRequestStatus status) noexcept {
    switch (status) {
    case SourceRequestStatus::Ok:
        return "ok";
    case SourceRequestStatus::BeforeRange:
        return "before-range";
    case SourceRequestStatus::AfterRange:
        return "after-range";
    case SourceRequestStatus::MissingFrame:
        return "missing-frame";
    }
    return "ok";
}

const char* inputTransformModeName(InputTransformMode mode) noexcept {
    switch (mode) {
    case InputTransformMode::Auto:
        return "auto";
    case InputTransformMode::Explicit:
        return "explicit";
    case InputTransformMode::Raw:
        return "raw";
    }
    return "auto";
}

const char* alphaModeName(AlphaMode mode) noexcept {
    switch (mode) {
    case AlphaMode::Auto:
        return "auto";
    case AlphaMode::Straight:
        return "straight";
    case AlphaMode::Premultiplied:
        return "premultiplied";
    }
    return "auto";
}

const char* boundaryPolicyName(BoundaryPolicy policy) noexcept {
    switch (policy) {
    case BoundaryPolicy::Error:
        return "error";
    case BoundaryPolicy::Hold:
        return "hold";
    case BoundaryPolicy::Black:
        return "black";
    }
    return "error";
}

const char* missingFramePolicyName(MissingFramePolicy policy) noexcept {
    return policy == MissingFramePolicy::Black ? "black" : "error";
}

std::int64_t mapSourceFrame(std::int64_t frameOffset, std::int64_t frameStep, std::int64_t localTime) {
    if (frameStep == 0)
        throw std::invalid_argument("source frame mapping: frameStep must not be zero");
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const bool multiplyOverflows =
        localTime > 0
            ? (frameStep > 0 ? localTime > maximum / frameStep : frameStep < minimum / localTime)
            : (localTime < 0 && (frameStep > 0 ? localTime < minimum / frameStep : localTime < maximum / frameStep));
    if (multiplyOverflows)
        throw std::overflow_error("source frame mapping multiplication overflows the 64-bit range");
    const std::int64_t product = localTime * frameStep;
    if ((product > 0 && frameOffset > maximum - product) || (product < 0 && frameOffset < minimum - product))
        throw std::overflow_error("source frame mapping overflows the 64-bit range");
    return frameOffset + product;
}

std::int64_t startAtOffset(std::int64_t startAt, std::int64_t rangeFirst, std::int64_t rangeLast,
                           std::int64_t frameStep) {
    if (frameStep == 0)
        throw std::invalid_argument("source frame mapping: frameStep must not be zero");
    if (rangeFirst > rangeLast)
        throw std::invalid_argument("source frame mapping: rangeFirst must not exceed rangeLast");
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::int64_t anchor = frameStep < 0 ? rangeLast : rangeFirst;
    const std::int64_t product = mapSourceFrame(0, frameStep, startAt);
    if ((product > 0 && anchor < minimum + product) || (product < 0 && anchor > maximum + product))
        throw std::overflow_error("source frame mapping overflows the 64-bit range");
    return anchor - product;
}

std::int64_t EffectiveSourceMapping::frameFor(std::int64_t localTime) const {
    return mapSourceFrame(frameOffset, frameStep, localTime);
}

std::optional<std::int64_t> EffectiveSourceMapping::startAt() const noexcept {
    if (!bounded() || frameStep == 0)
        return std::nullopt;
    const std::int64_t anchor = frameStep < 0 ? *lastFrame : *firstFrame;
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((frameOffset > 0 && anchor < minimum + frameOffset) || (frameOffset < 0 && anchor > maximum + frameOffset))
        return std::nullopt;
    const std::int64_t delta = anchor - frameOffset;
    if ((delta == minimum && frameStep == -1) || delta % frameStep != 0)
        return std::nullopt;
    return delta / frameStep;
}

bool EffectiveSourceMapping::covers(std::int64_t sourceFrame) const noexcept {
    if (!bounded())
        return false;
    return sourceFrame >= *firstFrame && sourceFrame <= *lastFrame;
}

bool EffectiveSourceRequest::missingAt(std::int64_t frame) const noexcept {
    return std::any_of(missingRanges.begin(), missingRanges.end(),
                       [frame](const MediaFrameRange& range) { return range.contains(frame); });
}

std::optional<BoundaryPolicy> EffectiveSourceRequest::boundaryPolicyFor(std::int64_t frame) const noexcept {
    if (!mapping.boundsEnforced || !mapping.bounded())
        return std::nullopt;
    if (mapping.covers(frame))
        return std::nullopt;
    return frame < *mapping.firstFrame ? std::optional<BoundaryPolicy>{beforePolicy}
                                       : std::optional<BoundaryPolicy>{afterPolicy};
}

std::optional<std::string> readOverridesProblem(const ReadNodeOverrides& overrides) {
    if (const auto problem = requireOneOf(overrides.rangeMode, "rangeMode", {kReadRangeModeAuto, kReadRangeModeCustom}))
        return problem;
    if (overrides.rangeMode == kReadRangeModeCustom && overrides.rangeFirst > overrides.rangeLast)
        return "rangeFirst (" + std::to_string(overrides.rangeFirst) + ") must not exceed rangeLast (" +
               std::to_string(overrides.rangeLast) + ") while rangeMode is 'custom'";
    if (overrides.frameStep == 0)
        return "frameStep must not be zero (a Read source mapping cannot stall)";
    if (const auto problem = requireOneOf(overrides.beforePolicy, "beforePolicy", {"error", "hold", "black"}))
        return problem;
    if (const auto problem = requireOneOf(overrides.afterPolicy, "afterPolicy", {"error", "hold", "black"}))
        return problem;
    if (const auto problem = requireOneOf(overrides.missingPolicy, "missingPolicy", {"error", "black"}))
        return problem;
    if (const auto problem = requireOneOf(overrides.inputTransform, "inputTransform", {"auto", "explicit", "raw"}))
        return problem;
    if (overrides.inputTransform == "explicit" && overrides.inputColorSpace.empty())
        return "inputColorSpace must name an input color space while inputTransform is 'explicit'";
    if (const auto problem = requireOneOf(overrides.alphaMode, "alphaMode", {"auto", "straight", "premultiplied"}))
        return problem;
    for (const SourceHintVocabulary& spec : kSourceHintVocabulary) {
        const std::string& chosen = overrides.*spec.field;
        if (chosen == "auto")
            continue;
        if (!sourceHintValueSupported(spec.hintKey, chosen))
            return std::string(spec.parameterName) + " must be auto or one of " + supportedHintValues(spec) +
                   ", got '" + chosen + "'";
    }
    return std::nullopt;
}

ReadNodeOverrides readAuthoredOverrides(const SourceReference& reference) {
    // The inverse of resolution: the Read choices that reproduce this shared
    // reference's mapping and recognized fill-only interpretation hints. A value
    // the adapter cannot apply is left on the shared reference (where every
    // consumer still reports it) rather than invented as node state.
    ReadNodeOverrides overrides;
    overrides.frameOffset = reference.frameOffset;
    overrides.frameStep = reference.frameStep;
    if (reference.firstFrame && reference.lastFrame) {
        overrides.rangeMode = std::string(kReadRangeModeCustom);
        overrides.rangeFirst = *reference.firstFrame;
        overrides.rangeLast = *reference.lastFrame;
    }
    for (const SourceHintVocabulary& spec : kSourceHintVocabulary) {
        const auto value = reference.interpretation.find(std::string(spec.hintKey));
        if (value != reference.interpretation.end() && sourceHintValueSupported(spec.hintKey, value->second))
            overrides.*spec.field = value->second;
    }
    return overrides;
}

std::vector<std::pair<std::string, ParameterValue>> readOverrideParameters(const ReadNodeOverrides& overrides) {
    std::vector<std::pair<std::string, ParameterValue>> parameters;
    parameters.reserve(8);
    const auto record = [&parameters](std::string_view key, ParameterValue value) {
        parameters.emplace_back(std::string(key), std::move(value));
    };
    const auto choice = [](std::string_view value) { return ParameterValue{ChoiceValue{std::string(value)}}; };
    // A value equal to the frozen default carries no information: it is omitted,
    // so an authored record means "this Read chose this" and a default keeps
    // following the descriptor (and any future default) instead of pinning it.
    if (overrides.frameOffset != 0)
        record(kReadParamFrameOffset, ParameterValue{overrides.frameOffset});
    if (overrides.frameStep != 1)
        record(kReadParamFrameStep, ParameterValue{overrides.frameStep});
    if (overrides.rangeMode != kReadRangeModeAuto)
        record(kReadParamRangeMode, choice(overrides.rangeMode));
    if (overrides.rangeFirst != 0)
        record(kReadParamRangeFirst, ParameterValue{overrides.rangeFirst});
    if (overrides.rangeLast != 0)
        record(kReadParamRangeLast, ParameterValue{overrides.rangeLast});
    if (overrides.beforePolicy != "error")
        record(kReadParamBeforePolicy, choice(overrides.beforePolicy));
    if (overrides.afterPolicy != "error")
        record(kReadParamAfterPolicy, choice(overrides.afterPolicy));
    if (overrides.missingPolicy != "error")
        record(kReadParamMissingPolicy, choice(overrides.missingPolicy));
    if (overrides.inputTransform != "auto")
        record(kReadParamInputTransform, choice(overrides.inputTransform));
    if (!overrides.inputColorSpace.empty())
        record(kReadParamInputColorSpace, ParameterValue{overrides.inputColorSpace});
    if (overrides.alphaMode != "auto")
        record(kReadParamAlphaMode, choice(overrides.alphaMode));
    for (const SourceHintVocabulary& spec : kSourceHintVocabulary) {
        const std::string& chosen = overrides.*spec.field;
        if (chosen != "auto")
            record(spec.parameterName, choice(chosen));
    }
    return parameters;
}

ReadNodeOverrides readAuthoredOverrides(const Document& document, const NodeInstance& node) {
    const ReadNodeOverrides defaults;
    ReadNodeOverrides overrides;
    overrides.rangeMode = readAuthoredText(document, node, kReadParamRangeMode, std::string(kReadRangeModeAuto));
    overrides.rangeFirst = readAuthoredInteger(document, node, kReadParamRangeFirst, defaults.rangeFirst);
    overrides.rangeLast = readAuthoredInteger(document, node, kReadParamRangeLast, defaults.rangeLast);
    overrides.frameOffset = readAuthoredInteger(document, node, kReadParamFrameOffset, defaults.frameOffset);
    overrides.frameStep = readAuthoredInteger(document, node, kReadParamFrameStep, defaults.frameStep);
    overrides.beforePolicy = readAuthoredText(document, node, kReadParamBeforePolicy, defaults.beforePolicy);
    overrides.afterPolicy = readAuthoredText(document, node, kReadParamAfterPolicy, defaults.afterPolicy);
    overrides.missingPolicy = readAuthoredText(document, node, kReadParamMissingPolicy, defaults.missingPolicy);
    overrides.inputTransform = readAuthoredText(document, node, kReadParamInputTransform, defaults.inputTransform);
    overrides.inputColorSpace = readAuthoredText(document, node, kReadParamInputColorSpace, {});
    overrides.alphaMode = readAuthoredText(document, node, kReadParamAlphaMode, defaults.alphaMode);
    for (const SourceHintVocabulary& spec : kSourceHintVocabulary)
        overrides.*spec.field = readAuthoredText(document, node, spec.parameterName, "auto");
    return overrides;
}

std::vector<std::pair<std::string, ParameterValue>> readInitializationParameters(const Document& document,
                                                                                 const ParameterAddress& target,
                                                                                 const ReadNodeOverrides& overrides) {
    const ParameterValues* scope = nullptr;
    if (target.instance != kInvalidNetworkInstance) {
        if (const NetworkInstance* occurrence = document.instance(target.instance)) {
            const auto node = occurrence->params.find(target.node);
            if (node != occurrence->params.end())
                scope = &node->second;
        }
    } else if (const NodeInstance* node = document.network(target.network).graph().node(target.node)) {
        scope = &node->params;
    }
    std::vector<std::pair<std::string, ParameterValue>> parameters;
    for (auto& [name, value] : readOverrideParameters(overrides)) {
        if (scope != nullptr && scope->find(name) != scope->end())
            continue;  // the target already authors this field
        const ParameterAddress address{target.network, target.node, name, target.instance};
        if (document.animationChannel(address) != nullptr)
            continue;  // the field is animated; a static initialization would shadow it
        parameters.emplace_back(std::move(name), std::move(value));
    }
    return parameters;
}

std::uint8_t applyReadInterpretationHints(std::map<std::string, std::string>& interpretation,
                                          const ReadNodeOverrides& overrides) {
    std::uint8_t nodeKeys = 0;
    for (const SourceHintVocabulary& spec : kSourceHintVocabulary) {
        const std::string& chosen = overrides.*spec.field;
        if (chosen == "auto")
            continue;
        interpretation[std::string(spec.hintKey)] = chosen;
        nodeKeys |= sourceHintBit(spec.hintKey);
    }
    return nodeKeys;
}

EffectiveSourceRequest resolveSourceRequest(const Document& document, const NodeInstance& sourceNode,
                                            std::int64_t localTime) {
    if (sourceNode.type != "source")
        failRead(sourceNode, "effective source request requires a Read node (type 'source')");
    const auto keyIt = sourceNode.params.find("source");
    const ParameterValue* keyValue = keyIt != sourceNode.params.end()
                                         ? &keyIt->second
                                         : documentCatalog(document).parameterDefault(sourceNode.type, "source");
    const auto* keyText = keyValue == nullptr ? nullptr : std::get_if<std::string>(keyValue);
    if (keyText == nullptr || keyText->empty())
        failRead(sourceNode, "source node needs a non-empty 'source' parameter naming a document source");
    const auto referenceIt = document.sources.find(*keyText);
    if (referenceIt == document.sources.end())
        failRead(sourceNode, "unresolved source '" + *keyText +
                                 "': no source reference with this key in the document (real media is never "
                                 "evaluated as synthetic content)");
    const ReadNodeOverrides overrides = readAuthoredOverrides(document, sourceNode);
    if (const auto problem = readOverridesProblem(overrides))
        failRead(sourceNode, *problem);
    const SourceReference& reference = referenceIt->second;
    const MediaMetadata* metadata = mediaMetadataFor(document, *keyText);

    EffectiveSourceRequest request;
    request.sourceKey = *keyText;
    request.path = reference.path;
    request.revision = reference.revision;
    request.kind = metadata != nullptr ? metadata->kind : MediaKind::Unknown;
    request.localTime = localTime;
    request.mapping.frameOffset = overrides.frameOffset;
    request.mapping.frameStep = overrides.frameStep;

    const MediaProbeMetadata* probe = committedProbe(metadata);
    if (probe != nullptr) {
        request.coverage = probe->coverageQuality;
        request.originalFirstFrame = probe->firstFrame;
        request.originalLastFrame = probe->lastFrame;
        request.originalFrameCount = probe->availableFrameCount;
        request.missingFrameCount = probe->missingFrameCount;
        request.missingRanges = probe->missingRanges;
    }
    if (overrides.rangeMode == kReadRangeModeCustom) {
        // A custom range is the artist's own interval: an explicit choice, so it
        // is authoritative even when the discovered coverage is estimated.
        request.mapping.firstFrame = overrides.rangeFirst;
        request.mapping.lastFrame = overrides.rangeLast;
        request.mapping.origin = SourceBoundsOrigin::NodeCustom;
        request.mapping.boundsEnforced = true;
    } else if (probe != nullptr && probe->firstFrame && probe->lastFrame) {
        request.mapping.firstFrame = probe->firstFrame;
        request.mapping.lastFrame = probe->lastFrame;
        request.mapping.origin = SourceBoundsOrigin::DiscoveredFacts;
        // Only a validated interval may fail a request; estimated timing is
        // reported as a fact and never fabricated into a boundary failure.
        request.mapping.boundsEnforced = probe->coverageQuality == CoverageQuality::Validated;
    } else if (reference.firstFrame && reference.lastFrame) {
        request.mapping.firstFrame = reference.firstFrame;
        request.mapping.lastFrame = reference.lastFrame;
        request.mapping.origin = SourceBoundsOrigin::SharedReference;
        request.mapping.boundsEnforced = true;
    }

    request.beforePolicy = parseBoundaryPolicy(overrides.beforePolicy, BoundaryPolicy::Error);
    request.afterPolicy = parseBoundaryPolicy(overrides.afterPolicy, BoundaryPolicy::Error);
    request.missingPolicy = parseMissingPolicy(overrides.missingPolicy);
    request.inputTransform = overrides.inputTransform == "explicit" ? InputTransformMode::Explicit
                             : overrides.inputTransform == "raw"    ? InputTransformMode::Raw
                                                                    : InputTransformMode::Auto;
    request.inputColorSpace = overrides.inputColorSpace;
    request.alpha = overrides.alphaMode == "straight"        ? AlphaMode::Straight
                    : overrides.alphaMode == "premultiplied" ? AlphaMode::Premultiplied
                                                             : AlphaMode::Auto;

    // Fill-only media-interpretation hints: the node's non-auto choice wins over
    // the shared reference's policy, and both only ever fill a field the
    // file/stream left unspecified. The shared map is carried verbatim, so a
    // field this build does not model keeps reporting exactly as it did.
    request.interpretation = reference.interpretation;
    request.nodeInterpretationKeys = applyReadInterpretationHints(request.interpretation, overrides);

    try {
        request.sourceFrame = request.mapping.frameFor(localTime);
    } catch (const std::exception& error) {
        // Overflow always fails before any boundary logic: a mapping that cannot
        // be represented is never silently clamped into a policy.
        failRead(sourceNode, std::string("source time mapping failed: ") + error.what());
    }
    applyCoveragePolicy(request);
    return request;
}

EffectiveSourceRequest resolveSourceRequest(std::string_view sourceKey, const SourceReference& reference,
                                            const MediaMetadata* metadata, std::int64_t localTime) {
    EffectiveSourceRequest request;
    request.sourceKey = std::string(sourceKey);
    request.path = reference.path;
    request.revision = reference.revision;
    request.kind = metadata != nullptr ? metadata->kind : MediaKind::Unknown;
    request.localTime = localTime;
    request.mapping.frameOffset = reference.frameOffset;
    request.mapping.frameStep = reference.frameStep;
    if (request.mapping.frameStep == 0)
        throw EvaluationException("source '" + request.sourceKey + "': frameStep must not be zero");
    const MediaProbeMetadata* probe = committedProbe(metadata);
    if (probe != nullptr) {
        request.coverage = probe->coverageQuality;
        request.originalFirstFrame = probe->firstFrame;
        request.originalLastFrame = probe->lastFrame;
        request.originalFrameCount = probe->availableFrameCount;
        request.missingFrameCount = probe->missingFrameCount;
        request.missingRanges = probe->missingRanges;
    }
    // Existing non-Read consumers keep the authored shared mapping exactly:
    // discovery facts are reported, but only an authored interval bounds the
    // request, so a Media Bin worker or a Viewer timeline sees today's frames.
    if (reference.firstFrame && reference.lastFrame) {
        request.mapping.firstFrame = reference.firstFrame;
        request.mapping.lastFrame = reference.lastFrame;
        request.mapping.origin = SourceBoundsOrigin::SharedReference;
        request.mapping.boundsEnforced = true;
    }
    request.interpretation = reference.interpretation;
    try {
        request.sourceFrame = request.mapping.frameFor(localTime);
    } catch (const std::exception& error) {
        throw EvaluationException("source '" + request.sourceKey + "': source time mapping failed: " + error.what());
    }
    applyCoveragePolicy(request);
    return request;
}

std::string sourcePolicyProblem(const EffectiveSourceRequest& request) {
    if (!request.policyError)
        return {};
    const std::string frame = std::to_string(request.sourceFrame);
    const std::string source = "source '" + request.sourceKey + "' (" + request.path + ")";
    switch (request.status) {
    case SourceRequestStatus::BeforeRange:
        return source + ": local frame " + std::to_string(request.localTime) + " maps to source frame " + frame +
               ", before the selected range first frame " +
               (request.mapping.firstFrame ? std::to_string(*request.mapping.firstFrame) : std::string{"(unknown)"}) +
               " (beforePolicy is 'error')";
    case SourceRequestStatus::AfterRange:
        return source + ": local frame " + std::to_string(request.localTime) + " maps to source frame " + frame +
               ", after the selected range last frame " +
               (request.mapping.lastFrame ? std::to_string(*request.mapping.lastFrame) : std::string{"(unknown)"}) +
               " (afterPolicy is 'error')";
    case SourceRequestStatus::MissingFrame:
        return source + ": requests source frame " + frame +
               ", which is inside the discovered sequence range but has no member file (missingPolicy is 'error')";
    case SourceRequestStatus::Ok:
        break;
    }
    return source + ": the request resolves to a policy that requires failure";
}

void appendSourceDecodeIdentity(std::string& canonical, const EffectiveSourceRequest& request,
                                std::string_view workingSpace, std::string_view colorConfigIdentity) {
    appendCanonicalField(canonical, "sourcePath", request.path);
    appendCanonicalField(canonical, "sourceRevision", std::to_string(request.revision));
    appendCanonicalField(canonical, "sourceKind", std::to_string(static_cast<int>(request.kind)));
    appendCanonicalField(canonical, "inputTransform", inputTransformModeName(request.inputTransform));
    appendCanonicalField(canonical, "inputColorSpace", request.inputColorSpace);
    appendCanonicalField(canonical, "alpha", alphaModeName(request.alpha));
    for (const auto& [field, value] : request.interpretation)
        appendCanonicalField(canonical, "hint", field + '=' + value);
    if (!request.dataBypass()) {
        appendCanonicalField(canonical, "working", workingSpace);
        appendCanonicalField(canonical, "colorConfigIdentity", colorConfigIdentity);
    }
}

void appendEffectiveSourceIdentity(std::string& canonical, const EffectiveSourceRequest& request,
                                   std::string_view workingSpace, std::string_view colorConfigIdentity) {
    // One decode-owner identity, then the frame-specific outcome: a decoder can
    // be reused across frames, a *result* never is.
    appendSourceDecodeIdentity(canonical, request, workingSpace, colorConfigIdentity);
    appendCanonicalField(canonical, "offset", std::to_string(request.mapping.frameOffset));
    appendCanonicalField(canonical, "step", std::to_string(request.mapping.frameStep));
    appendCanonicalField(canonical, "rangeFirst",
                         request.mapping.firstFrame ? std::to_string(*request.mapping.firstFrame) : std::string{"-"});
    appendCanonicalField(canonical, "rangeLast",
                         request.mapping.lastFrame ? std::to_string(*request.mapping.lastFrame) : std::string{"-"});
    appendCanonicalField(canonical, "rangeOrigin",
                         std::to_string(static_cast<int>(request.mapping.origin)) +
                             (request.mapping.boundsEnforced ? "!" : "-"));
    appendCanonicalField(canonical, "coverage", std::to_string(static_cast<int>(request.coverage)));
    appendCanonicalField(canonical, "originalFirst",
                         request.originalFirstFrame ? std::to_string(*request.originalFirstFrame) : std::string{"-"});
    appendCanonicalField(canonical, "originalLast",
                         request.originalLastFrame ? std::to_string(*request.originalLastFrame) : std::string{"-"});
    appendCanonicalField(canonical, "availableCount",
                         request.originalFrameCount ? std::to_string(*request.originalFrameCount) : std::string{"-"});
    appendCanonicalField(canonical, "missingCount",
                         request.missingFrameCount ? std::to_string(*request.missingFrameCount) : std::string{"-"});
    for (const MediaFrameRange& hole : request.missingRanges)
        appendCanonicalField(canonical, "hole", std::to_string(hole.first) + ':' + std::to_string(hole.last));
    appendCanonicalField(canonical, "beforePolicy", boundaryPolicyName(request.beforePolicy));
    appendCanonicalField(canonical, "afterPolicy", boundaryPolicyName(request.afterPolicy));
    appendCanonicalField(canonical, "missingPolicy", missingFramePolicyName(request.missingPolicy));
    appendCanonicalField(canonical, "sourceFrame", std::to_string(request.sourceFrame));
    appendCanonicalField(canonical, "readFrame", std::to_string(request.readFrame));
    appendCanonicalField(canonical, "transparentBlack", request.transparentBlack ? "1" : "0");
}

}  // namespace nemo
