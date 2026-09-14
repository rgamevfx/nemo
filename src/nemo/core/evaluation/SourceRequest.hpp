#pragma once

// Effective source request (issue #75, delivered by #79).
//
// A Read node (`source`) names a shared media reference through its `source`
// parameter, while the choices that belong to the Read itself — range mode,
// custom range, offset/step mapping, boundary/missing policies, explicit input
// transform and alpha association, migrated media-interpretation hints — are
// ordinary node parameters on that node. This header is the single owner that
// combines those node-scoped choices with the shared reference and the
// committed shared facts into one plain request.
//
// Consumers (CPU source provider, GPU source session, media import worker,
// Read editor/controller queries) consume the resolved request; none of them
// re-derives mapping, policies or interpretation precedence. The request is
// deliberately media- and framework-free: it carries the authored color
// *choices* (`inputTransform`, `inputColorSpace`, `alpha`, interpretation
// hints) and never an OCIO object, config handle, or decoded transform. The
// media module turns those choices into a real input-to-working conversion.
//
// Two entry points exist:
//   * resolveSourceRequest(document, node, localTime) — a Read node's request.
//   * resolveSourceRequest(sourceKey, reference, metadata, localTime) — the
//     source-scoped request existing raw-reference workers and non-Read
//     consumers need; it carries the shared reference's own mapping and
//     interpretation and no node overrides.
//
// Resolution is exclusive, never compositional: a Read's node mapping replaces
// the shared reference's offset/step/range (it is never added to it), so a
// legacy value is applied exactly once.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/MediaCatalog.hpp"

namespace nemo {

// ---------------------------------------------------------------------------
// Node-scoped Read parameters: names and frozen semantic defaults.
//
// NodeCatalog declares the same names with the same defaults for authoring,
// validation and presentation; these constants are the semantic owner used by
// the resolver, so an undeclared (older) catalog still resolves identically.
// ---------------------------------------------------------------------------
// The Read's media reference parameter: the address a Read binding writes.
inline constexpr std::string_view kReadParamSourceKey = "source";
inline constexpr std::string_view kReadParamRangeMode = "rangeMode";
inline constexpr std::string_view kReadParamRangeFirst = "rangeFirst";
inline constexpr std::string_view kReadParamRangeLast = "rangeLast";
inline constexpr std::string_view kReadParamFrameOffset = "frameOffset";
inline constexpr std::string_view kReadParamFrameStep = "frameStep";
inline constexpr std::string_view kReadParamBeforePolicy = "beforePolicy";
inline constexpr std::string_view kReadParamAfterPolicy = "afterPolicy";
inline constexpr std::string_view kReadParamMissingPolicy = "missingPolicy";
inline constexpr std::string_view kReadParamInputTransform = "inputTransform";
inline constexpr std::string_view kReadParamInputColorSpace = "inputColorSpace";
inline constexpr std::string_view kReadParamAlphaMode = "alphaMode";
inline constexpr std::string_view kReadParamSourceTransfer = "sourceTransfer";
inline constexpr std::string_view kReadParamSourcePrimaries = "sourcePrimaries";
inline constexpr std::string_view kReadParamSourceMatrix = "sourceMatrix";
inline constexpr std::string_view kReadParamSourceRange = "sourceRange";
inline constexpr std::string_view kReadParamSourceChromaLocation = "sourceChromaLocation";

inline constexpr std::string_view kReadRangeModeAuto = "auto";
inline constexpr std::string_view kReadRangeModeCustom = "custom";

// Recognized `SourceReference.interpretation` keys. The vocabulary is shared
// with the media adapter's single parser; the resolver only ever emits these
// keys and rejects an authored value it does not recognize.
inline constexpr std::string_view kSourceHintTransfer = "transfer";
inline constexpr std::string_view kSourceHintPrimaries = "primaries";
inline constexpr std::string_view kSourceHintMatrix = "matrix";
inline constexpr std::string_view kSourceHintRange = "range";
inline constexpr std::string_view kSourceHintChromaLocation = "chromaLocation";

// Bit per recognized hint key, in the order above. `nodeInterpretationKeys`
// reports which resolved hints came from the node scope (node
// `source*` parameter non-`auto`) rather than from the shared reference, so a
// consumer can name the origin without re-deriving the precedence.
inline constexpr std::uint8_t kSourceHintTransferBit = 1U << 0;
inline constexpr std::uint8_t kSourceHintPrimariesBit = 1U << 1;
inline constexpr std::uint8_t kSourceHintMatrixBit = 1U << 2;
inline constexpr std::uint8_t kSourceHintRangeBit = 1U << 3;
inline constexpr std::uint8_t kSourceHintChromaLocationBit = 1U << 4;
inline constexpr std::uint8_t kSourceHintAllBits = 0x1FU;

// Node param name of one recognized hint key; empty for an unknown key.
[[nodiscard]] std::string_view sourceHintParameterName(std::string_view hintKey) noexcept;
[[nodiscard]] std::uint8_t sourceHintBit(std::string_view hintKey) noexcept;

// Authored range ownership. `NodeCustom` is a Read's explicit range, `SharedReference`
// is the legacy authored range that non-Read consumers still read from the
// shared reference, `DiscoveredFacts` is the source's discovered original
// range (Auto), and `Unbounded` means no interval (a still, or a source whose
// coverage is unknown).
enum class SourceBoundsOrigin { Unbounded, SharedReference, NodeCustom, DiscoveredFacts };

[[nodiscard]] const char* sourceBoundsOriginName(SourceBoundsOrigin origin) noexcept;

// Complete Read choice set mirroring the node parameters above. A gesture
// writes only its affected parameters; readAuthoredOverrides composes the set
// consumed by validation and resolution.
struct ReadNodeOverrides {
    std::string rangeMode{std::string(kReadRangeModeAuto)};
    std::int64_t rangeFirst{0};
    std::int64_t rangeLast{0};
    std::int64_t frameOffset{0};
    std::int64_t frameStep{1};
    std::string beforePolicy{"error"};
    std::string afterPolicy{"error"};
    std::string missingPolicy{"error"};
    std::string inputTransform{"auto"};
    std::string inputColorSpace;
    std::string alphaMode{"auto"};
    std::string sourceTransfer{"auto"};
    std::string sourcePrimaries{"auto"};
    std::string sourceMatrix{"auto"};
    std::string sourceRange{"auto"};
    std::string sourceChromaLocation{"auto"};

    [[nodiscard]] bool operator==(const ReadNodeOverrides&) const = default;
};

// Admissibility of one authored set, naming the offending relationship (a zero
// Step, an inverted custom range, an unknown choice, `explicit` without a color
// space). Returned so presentation/command validation and the resolver can
// report the same relationship through their own error interfaces.
[[nodiscard]] std::optional<std::string> readOverridesProblem(const ReadNodeOverrides& overrides);

// Apply the node's non-auto hints over an existing shared interpretation map.
// Returns the node-origin bit mask. Also supports an empty map before first
// binding, so an artist can supply missing decode facts before probing media.
[[nodiscard]] std::uint8_t applyReadInterpretationHints(std::map<std::string, std::string>& interpretation,
                                                        const ReadNodeOverrides& overrides);

// Reads the authored set from one Read node (authored value, else descriptor
// default, else frozen default). Wrong value types throw a node-identifying
// error; readOverridesProblem checks the resulting choices and relationships.
[[nodiscard]] ReadNodeOverrides readAuthoredOverrides(const Document& document, const NodeInstance& node);

// The inverse of resolution: the Read choice set that reproduces this shared
// reference's mapping and recognized fill-only interpretation hints. A timing,
// range or hint value this build cannot apply is left on the shared reference —
// where every other consumer still reports it — rather than invented as node
// state. Read-node construction and schema migration both consume this one
// translation instead of copying fields.
[[nodiscard]] ReadNodeOverrides readAuthoredOverrides(const SourceReference& reference);

// The node parameter records that author one Read choice set, in the frozen
// kReadParam* names. A value equal to the frozen default is omitted, because it
// carries no information: an authored record means the Read chose that value,
// while an omitted one keeps following the descriptor default instead of pinning
// today's literal. A value the vocabulary does not support is never emitted.
[[nodiscard]] std::vector<std::pair<std::string, ParameterValue>>
readOverrideParameters(const ReadNodeOverrides& overrides);

// The parameter records that initialize one Read without overwriting anything it
// already holds: the authored set's informative values, minus the keys the target
// scope already authors as a value or already animates. A cleared Read (an
// authored empty `source`) or a keyed one therefore keeps its trim, offset and
// channels, and a partial author keeps exactly the fields it wrote. This is the
// single fill-only initialization owner, used by the binding command and by
// schema migration.
[[nodiscard]] std::vector<std::pair<std::string, ParameterValue>>
readInitializationParameters(const Document& document, const ParameterAddress& target,
                             const ReadNodeOverrides& overrides);

// Where a resolved request's mapped frame fell, relative to the selected
// interval and the discovered holes.
enum class SourceRequestStatus { Ok, BeforeRange, AfterRange, MissingFrame };

[[nodiscard]] const char* sourceRequestStatusName(SourceRequestStatus status) noexcept;

// Map a local composition frame to a source frame: offset + local * step,
// computed exactly once. Throws std::overflow_error when the 64-bit result is
// not representable; a negative result is *not* an error here, because an
// explicit policy may legitimately cover it.
[[nodiscard]] std::int64_t mapSourceFrame(std::int64_t frameOffset, std::int64_t frameStep, std::int64_t localTime);

// The "Start At" alternate editor for the same mapping: returns the offset
// that aligns `startAt` with the first (forward) or last (reverse) frame of the
// selected source range. Throws std::overflow_error when no offset is
// representable, so the documented mapping is preserved exactly.
[[nodiscard]] std::int64_t startAtOffset(std::int64_t startAt, std::int64_t rangeFirst, std::int64_t rangeLast,
                                         std::int64_t frameStep);

// Effective mapping: the Read's (or shared reference's) offset/step plus the
// selected inclusive source interval. Exactly one authority maps a local frame.
struct EffectiveSourceMapping {
    std::int64_t frameOffset{0};
    std::int64_t frameStep{1};
    std::optional<std::int64_t> firstFrame;
    std::optional<std::int64_t> lastFrame;
    SourceBoundsOrigin origin{SourceBoundsOrigin::Unbounded};
    // Boundary policies are applied to the mapped request only for an interval
    // that is authoritative: an authored interval (node or shared reference) or
    // a discovered interval whose coverage is Validated. An Estimated or
    // Unknown interval stays informational and never fabricates a failure.
    bool boundsEnforced{false};

    [[nodiscard]] bool bounded() const noexcept { return firstFrame.has_value() && lastFrame.has_value(); }
    [[nodiscard]] std::int64_t frameFor(std::int64_t localTime) const;
    // Local frame aligned to the first forward / last reverse source frame.
    // No value for unbounded, fractional, or unrepresentable alignment.
    [[nodiscard]] std::optional<std::int64_t> startAt() const noexcept;
    [[nodiscard]] bool covers(std::int64_t sourceFrame) const noexcept;
    [[nodiscard]] bool operator==(const EffectiveSourceMapping&) const = default;
};

// Explicit input transform choice. `Explicit` selects an OCIO input color
// space by name (media resolves it); `Raw` bypasses transfer/gamut conversion
// while keeping format/decode validation.
enum class InputTransformMode { Auto, Explicit, Raw };

// Alpha association choice. `Auto` uses reliable source metadata.
enum class AlphaMode { Auto, Straight, Premultiplied };

// Before/After coverage policy. `Error` preserves the current explicit-failure
// contract, `Hold` reads the boundary frame, `Black` produces transparent
// black in the request raster.
enum class BoundaryPolicy { Error, Hold, Black };

// Missing-frame (hole inside the selected interval) policy. Deliberately has no
// Hold: substituting a neighbouring file would invent coverage.
enum class MissingFramePolicy { Error, Black };

[[nodiscard]] const char* inputTransformModeName(InputTransformMode mode) noexcept;
[[nodiscard]] const char* alphaModeName(AlphaMode mode) noexcept;
[[nodiscard]] const char* boundaryPolicyName(BoundaryPolicy policy) noexcept;
[[nodiscard]] const char* missingFramePolicyName(MissingFramePolicy policy) noexcept;

// One resolved, plain, media-free source request.
struct EffectiveSourceRequest {
    std::string sourceKey;
    std::string path;
    std::uint64_t revision{0};
    MediaKind kind{MediaKind::Unknown};
    std::int64_t localTime{0};
    EffectiveSourceMapping mapping;
    // The requested source frame (offset + local * step, once) and the frame a
    // consumer actually opens after the boundary policy (equal unless Hold
    // selected the boundary frame).
    std::int64_t sourceFrame{0};
    std::int64_t readFrame{0};
    SourceRequestStatus status{SourceRequestStatus::Ok};
    // True when the applicable policy requires failure; the consumer raises the
    // node-identifying error at its own seam (the resolver never throws for a
    // policy decision, so a facts query can inspect it).
    bool policyError{false};
    bool transparentBlack{false};
    bool holdApplied{false};
    BoundaryPolicy beforePolicy{BoundaryPolicy::Error};
    BoundaryPolicy afterPolicy{BoundaryPolicy::Error};
    MissingFramePolicy missingPolicy{MissingFramePolicy::Error};

    // Committed shared coverage facts, copied (never re-derived). Absent
    // optional means the fact is unknown; CoverageQuality labels how far the
    // interval may be trusted.
    CoverageQuality coverage{CoverageQuality::Unknown};
    std::optional<std::int64_t> originalFirstFrame;
    std::optional<std::int64_t> originalLastFrame;
    std::optional<std::int64_t> originalFrameCount;
    std::optional<std::int64_t> missingFrameCount;
    std::vector<MediaFrameRange> missingRanges;

    // Fill-only media-interpretation hints: the node's non-auto `source*`
    // choices, else the shared reference's interpretation. A hint fills only a
    // field the file/stream left unspecified, exactly like the previous shared
    // policy, so tagged media still wins and legacy pixels are preserved.
    std::map<std::string, std::string> interpretation;
    std::uint8_t nodeInterpretationKeys{0};

    // Authored color choices; media resolves them, core never does.
    InputTransformMode inputTransform{InputTransformMode::Auto};
    std::string inputColorSpace;
    AlphaMode alpha{AlphaMode::Auto};

    [[nodiscard]] bool dataBypass() const noexcept { return inputTransform == InputTransformMode::Raw; }
    [[nodiscard]] bool explicitInputSpace() const noexcept { return inputTransform == InputTransformMode::Explicit; }
    // True when `sourceFrame` is inside the selected interval and inside a
    // discovered hole (a missing member), as opposed to outside the interval.
    [[nodiscard]] bool missingAt(std::int64_t sourceFrame) const noexcept;
    // Boundary policy that applies to a frame outside the interval; absent when
    // no boundary applies (inside the interval, or the interval is not
    // enforced).
    [[nodiscard]] std::optional<BoundaryPolicy> boundaryPolicyFor(std::int64_t sourceFrame) const noexcept;
};

// Reads one Read node's effective params. `sourceNode` is the request-local
// resolved node (instance overrides and animation already applied). Throws
// EvaluationException naming the node for a malformed authored value, an
// unknown source key, or an unrepresentable mapping.
[[nodiscard]] EffectiveSourceRequest resolveSourceRequest(const Document& document, const NodeInstance& sourceNode,
                                                          std::int64_t localTime);

// Source-scoped resolution for existing raw-reference consumers (Media Bin
// workers, CLI source queries, non-Read consumers): the shared reference owns
// the mapping, and node overrides do not exist. `metadata` supplies the
// committed facts and kind when the caller has them.
[[nodiscard]] EffectiveSourceRequest resolveSourceRequest(std::string_view sourceKey, const SourceReference& reference,
                                                          const MediaMetadata* metadata, std::int64_t localTime);

// Actionable description of a policy outcome that requires failure: names the
// offending source key and path, the requested source frame, the boundary or
// sequence relationship and the policy that failed. Empty when the request does
// not require failure. The evaluator attaches the offending node, so this text
// carries the source relationship only; every executor (CPU provider seam, GPU
// source session) reports this one wording instead of inventing its own.
[[nodiscard]] std::string sourcePolicyProblem(const EffectiveSourceRequest& request);

// Canonical identity of everything that makes one decoded frame's decode owner
// reusable: the media identity (path, content revision, kind), the effective
// input interpretation (mode, named input color space, alpha association, the
// fill-only encoded-layout hints) and the color context the consumer converts
// against (working space, opaque configuration content identity). It is
// deliberately frame-independent, so decoding successive frames of one clip
// reuses one decoder while a changed interpretation or configuration can never
// reuse another's. A Raw/Data request bypasses transfer/gamut conversion, so it
// excludes the working space and configuration identity: nothing color-converted
// is being identified.
void appendSourceDecodeIdentity(std::string& canonical, const EffectiveSourceRequest& request,
                                std::string_view workingSpace, std::string_view colorConfigIdentity);

// Full reusable-content identity of one effective request: the decode identity
// plus everything that makes this frame's *result* specific - effective mapping
// (offset, step, selected interval, its origin and enforcement), coverage facts,
// the before/after/missing policies, and the outcome (requested and read frames,
// transparent black). Node identity and the shared source key are deliberately
// excluded: equivalent effective requests must hash identically wherever they
// are reached from, while a differing mapping, coverage, policy, revision,
// interpretation or color context must never alias.
void appendEffectiveSourceIdentity(std::string& canonical, const EffectiveSourceRequest& request,
                                   std::string_view workingSpace, std::string_view colorConfigIdentity);

}  // namespace nemo
