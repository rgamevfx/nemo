#pragma once

// Immutable node contribution projection (issue #83, spec sections 10.2-10.4).
//
// One explicitly assembled list of built-in node modules supplies, per node
// type: the immutable schema descriptor, the node's role in the network, an
// optional CPU reference adapter, an optional authoring-time parameter
// interpretation, optional namespaced editor declarations, an optional input
// requirement rule and an optional output description rule. The schema
// catalog stays free of callbacks, Qt objects and GPU handles: this projection
// is the runtime registration the CPU executor consumes, and the GPU
// EffectLibrary projects the same list onto its own backend.
//
// Assembly is atomic and immutable: every declaration is validated before the
// snapshot exists, so no caller can observe a partially assembled inventory,
// and there is no mutation, unregister or service-locator API. Node-local
// pixel implementations live in their module (src/nemo/nodes/<slug>); shared
// conventions that several effects genuinely share stay in core (Params.hpp,
// EffectCpu.hpp). CPU and GPU pixel implementations remain independent
// (ADR-0004): agreement between them is evidence, not shared code.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

namespace nemo {

class Document;
struct NodeInstance;
struct ImageFormat;
class SourceProvider;
struct EffectiveSourceRequest;

// What a node is in the network. Ordinary effects produce one image from their
// declared inputs; a Source reads external media through the provider seam; an
// Output is the network's delivery sink; the Viewer is display-only. Roles are
// shared executor behavior, so adding an ordinary effect never needs a new
// node-type branch in an executor.
enum class NodeRole { Image, Source, Output, Viewer };

// One node's resolved evaluation context. `inputs` is declared-port aligned and
// holds null for an absent optional slot; the adapter never allocates a
// placeholder for one. `effectiveParams` borrows the immutable defaulted,
// overridden and animated parameter state shared with description and planning.
// `inputRequests` is the coverage each image
// in `inputs` was actually produced with (issue #85), in the same declared-port
// order, so a spatial effect reads an input through its real origin instead of
// assuming the input raster starts where its own request does. The slot of an
// absent optional input holds a default request and is never read.
//
// `description` is this node's resolved description (issue #88) — the meaning
// of the image it produces — and `inputDescriptions` is the same declared-port
// aligned view of its inputs' descriptions, null for an absent optional slot.
// `source` is the node's pre-resolved effective source request, non-null only
// for a Source node: the source callback consumes it instead of re-resolving
// authored state during execution.
struct CpuNodeContext {
    const Document& document;
    const NodeCatalog& catalog;
    const NodeInstance& node;
    const EvaluationRequest& request;
    const ParameterValues& effectiveParams;
    std::span<const CpuImage* const> inputs;
    SourceProvider* sources;
    std::span<const EvaluationRequest> inputRequests;
    const ImageDescription& description;
    const EffectiveSourceRequest* source;
    std::span<const ImageDescription* const> inputDescriptions;
    // The authored format of the network that owns this node (issue #92): the
    // composition's saved canvas, not this node's runtime coordinate frame. A
    // node whose authored values are stated in the composition's own space reads
    // it here; a node whose values are stated against the image it processes
    // (Crop's bottom-left box) converts through its INPUT's described format
    // instead, which travels in `description`/`inputDescriptions`. Null only for
    // a direct adapter invocation without a network scope.
    const ImageFormat* owningFormat{nullptr};
};

// One declared input port's demand (issue #88): the full-resolution signed
// region this node's description or pixel implementation reads from that port,
// and the channels it requires there (issue #90). A port outside the returned
// vector, or an entry whose region is empty, inherits the node's own request
// region. EMPTY channels mean "every channel the node's own demand names" (the
// inherited default), never "no channels"; an explicit list names channels
// exactly, and because it is a declaration it must be honest: a name the
// producer's described image does not carry is a declaration error against that
// producer. A node whose own policy tolerates a channel its input lacks states
// only the names that really exist (or leaves the port inherited, which is
// intersected with the producer's described channels). A contribution states
// only what is different about its inputs.
struct InputRequirement {
    Region region;
    std::vector<std::string> channels{};
};

// One node's region context (issue #85): the resolved state the node's
// dependency rules project input coverage from, without executing anything.
// `request` is the coverage the node has been asked to produce. `pixelAspect`
// is the node's own resolved output aspect (equal to `description.pixelAspect`):
// an aspect is always a real finite positive number, because an image without
// obtainable geometry fails where it is described rather than travelling as a
// zero sentinel. `effectiveParams` borrows the same immutable resolved parameter
// state execution consumes; callbacks never fill defaults or re-resolve
// animation/source state. `description` is
// the node's resolved description and `inputs` the descriptions of its declared
// input ports (null for an absent optional slot), so a rule that depends on an
// input's real geometry reads it instead of assuming the node's own.
struct NodeRegionContext {
    const NodeCatalog& catalog;
    const NodeInstance& node;
    const EvaluationRequest& request;
    const ParameterValues& effectiveParams;
    float pixelAspect{1.0F};
    const ImageDescription& description;
    std::span<const ImageDescription* const> inputs;
    // The authored format of the network that owns this node (issue #92): the
    // composition's saved canvas. A rule that states its demand in the
    // composition's own space reads it here; a rule stated against the image it
    // reads (Crop's bottom-left box) converts through its input's described
    // format, which travels in `inputs`/`description`. Null only for a direct
    // rule invocation without a network scope.
    const ImageFormat* owningFormat{nullptr};
};

// One node's description context (issue #88): everything a node needs to state
// the meaning of the image it produces, resolved once, before any pixel work.
// `inputs` is declared-port aligned with null for an absent optional slot, and
// `inherited` is the shared default this node would have without a rule of its
// own: the connected main input's description, or the owning network's authored
// canvas (issue #96) for a generator. A node that changes nothing about its
// image declares no rule at all and keeps `inherited` verbatim.
struct NodeDescriptionContext {
    const Document& document;
    const NodeCatalog& catalog;
    const NodeInstance& node;
    std::int64_t localTime;
    std::span<const ImageDescription* const> inputs;
    ImageDescription inherited;
    // The authored format of the network that OWNS this node (issue #92): the
    // same saved canvas a generator's `inherited` description falls back to,
    // never the selected viewer/Read. A node whose description is stated against
    // the composition's saved format (Reformat's to-format target) reads it here
    // instead of re-deriving geometry from its inputs; a node whose description
    // is stated against the image it processes (Crop's box, converted against
    // the input's own height) reads that input's format from `inputs` instead.
    // Null only for a direct rule invocation that has no network scope at all; a
    // real plan always supplies it.
    const ImageFormat* owningFormat{nullptr};
};

// A node's CPU reference pixel implementation. `version` is the implementation
// identity and must equal the descriptor's implementationVersion, which is what
// the content-derived result key carries, so changed implementations can never
// serve a stale cached image.
// Callbacks may run concurrently; captures must be immutable or synchronized.
// Context references and spans are borrowed only for the callback invocation.
struct CpuImplementation {
    std::uint64_t version{1};
    std::function<CpuImage(const CpuNodeContext&)> execute;
};

// Optional namespaced editor contribution. `source` is a presentation resource
// locator resolved by the existing editor host (a QML URL in the desktop
// application); `consumes` lists the node parameter keys this one editor owns,
// and `presentation` is the host layout it needs ("row" or "section"). These
// are presentation declarations only: no Qt object, widget or callback lives in
// this model.
struct NodeEditorContribution {
    std::string id;
    std::string source;
    std::vector<std::string> consumes;
    std::string presentation{"row"};
};

// Authoring-time admissibility of one node's resolved parameters: the failure
// message, or nullopt when they are admissible.
using NodeParameterValidation =
    std::function<std::optional<std::string>(const NodeCatalog&, const NodeInstance&, const ParameterValues&)>;

// One registered built-in node. `describe` is the last field by contract.
struct NodeContribution {
    NodeDescriptor descriptor;
    NodeRole role{NodeRole::Image};
    std::optional<CpuImplementation> cpu;
    std::string cpuUnavailableReason;
    bool nativeGpu{true};
    // True when the node's own pixel implementation decides EVERY channel of the
    // image it produces, so the executors must not preserve the main input's
    // auxiliary channels over it (issue #90). An ordinary effect addresses its
    // main input's RGBA projection and inherits the rest, which is exactly what
    // the shared preservation rule copies; a node that remaps or creates
    // channels — Shuffle — owns its output layout and declares true. This is
    // behavioral registration identity and participates in the fingerprint.
    bool ownsChannelLayout{false};
    // Applies the node's typed parameter interpretation for an authoring
    // gesture without evaluating the graph. Empty for nodes whose parameters
    // carry no interpretation of their own.
    NodeParameterValidation validateParameters;
    std::vector<NodeEditorContribution> editors;
    // Spatial dependency rule (issue #88): the input requirements this node's
    // pixel implementation reads for a regional request, indexed by declared
    // input port. A port the result does not cover (and an absent optional
    // slot) inherits this node's own request region and channels, so an empty
    // result means "every real port needs exactly this node's region and
    // channels". Rules are the node's own math (Blur's halo, Transform's
    // inverse map and filter footprint), never a node-type branch in the
    // executor: the planner merely rounds, unions and clips what a contribution
    // returns against the producing node's own geometry.
    std::function<std::vector<InputRequirement>(const NodeRegionContext&)> inputRequirements;
    // Output description rule (issue #88): what the image this node produces is.
    // An empty rule keeps the description the node inherits (its main input's,
    // or the owning network's canvas for a generator), which is the shared
    // default: a simple effect states nothing about its image and repeats no
    // image rule. A rule that changes part of the description returns the whole
    // description, so what it does not restate is explicitly its own choice.
    std::function<ImageDescription(const NodeDescriptionContext&)> describe;
};

// An immutable, exactly assembled registration snapshot. The constructor
// validates every declaration atomically: invalid descriptors (through the
// schema catalog's own validation), duplicate node identities, conflicting
// editor declarations, an implementation version that disagrees with its
// schema, and a promised CPU adapter that is missing are rejected with
// std::invalid_argument before any snapshot is observable.
class NodeContributions {
public:
    explicit NodeContributions(std::vector<NodeContribution> contributions);

    // The schema-only catalog projection: exactly this registration's
    // descriptors, with no callbacks.
    [[nodiscard]] std::shared_ptr<const NodeCatalog> catalog() const { return catalog_; }

    [[nodiscard]] const NodeContribution* find(std::string_view type) const;

    [[nodiscard]] std::span<const NodeContribution> entries() const { return contributions_; }

    // Cached registration identity: order-independent, and independent of
    // display names, labels, editor metadata and callback addresses.
    [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

    // Verifies that this registration still covers one non-structural node
    // before its result is reused or executed. Throws EvaluationException
    // identifying the node when the node's persistent type is not part of this
    // registration, or when the catalog's typed schema for that type differs
    // from the registered descriptor in identity-relevant fields (persistent
    // type, implementation version, isOutput, ports, parameters and their
    // defaults/bounds/choices/nonzero flags, capabilities). Display names,
    // grouping, labels, sections, rows, soft ranges, steps, decimals and editor
    // metadata are presentation and never participate.
    //
    // Backend availability is deliberately separate: a GPU-only contribution
    // may carry no CPU adapter, and the CPU executor reports that honestly from
    // the projection rather than from this schema check.
    void validate(const NodeCatalog& catalog, const NodeInstance& node) const;

    // Authoring seam (issue #75): the node-local typed interpretation applied to
    // one resolved node's parameters. Returns the failure message, or nullopt
    // when the parameters are admissible or the node type contributes no
    // parameter interpretation (unknown/schema-only nodes stay recoverable).
    [[nodiscard]] std::optional<std::string> validateParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                                const ParameterValues& effectiveParams) const;

private:
    std::vector<NodeContribution> contributions_;
    std::shared_ptr<const NodeCatalog> catalog_;
    std::uint64_t fingerprint_{0};
};

// The single explicit built-in list (src/nemo/nodes/BuiltinNodes.inc) as
// contributions. Callers that register an additional contribution extend this
// vector rather than maintaining a second node inventory.
[[nodiscard]] std::vector<NodeContribution> builtinContributions();

// The immutable default registration, built once. Not a mutable global: it is
// a shared immutable snapshot, and consumers may hold their own snapshot built
// from an extended list.
[[nodiscard]] std::shared_ptr<const NodeContributions> builtinNodeContributions();

}  // namespace nemo
