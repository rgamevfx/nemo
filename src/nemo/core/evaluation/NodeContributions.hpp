#pragma once

// Immutable node contribution projection (issue #83, spec sections 10.2-10.4).
//
// One explicitly assembled list of built-in node modules supplies, per node
// type: the immutable schema descriptor, the node's role in the network, an
// optional CPU reference adapter, an optional authoring-time parameter
// interpretation, and optional namespaced editor declarations. The schema
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
class SourceProvider;

// What a node is in the network. Ordinary effects produce one image from their
// declared inputs; a Source reads external media through the provider seam; an
// Output is the network's delivery sink; the Viewer is display-only. Roles are
// shared executor behavior, so adding an ordinary effect never needs a new
// node-type branch in an executor.
enum class NodeRole { Image, Source, Output, Viewer };

// One node's resolved evaluation context. `inputs` is declared-port aligned and
// holds null for an absent optional slot; the adapter never allocates a
// placeholder for one. `effectiveParams` is the resolved static/animated
// parameter state the executor will record in the plan, and an adapter adds the
// values it actually consumed to it. `inputRequests` is the coverage each image
// in `inputs` was actually produced with (issue #85), in the same declared-port
// order, so a spatial effect reads an input through its real origin instead of
// assuming the input raster starts where its own request does. The slot of an
// absent optional input holds a default request and is never read.
struct CpuNodeContext {
    const Document& document;
    const NodeCatalog& catalog;
    const NodeInstance& node;
    const EvaluationRequest& request;
    ParameterValues& effectiveParams;
    std::span<const CpuImage* const> inputs;
    SourceProvider* sources;
    std::span<const EvaluationRequest> inputRequests;
};

// One node's region context (issue #85): the resolved state the node's
// dependency rules project input coverage from, without executing anything.
// `request` is the coverage the node has been asked to produce, `pixelAspect`
// is the resolved pixel aspect of the node's main input (1 for a generator, 0
// when a source's aspect is not known yet), and `effectiveParams` is the same
// request-local resolved parameter state execution will consume.
struct NodeRegionContext {
    const NodeCatalog& catalog;
    const NodeInstance& node;
    const EvaluationRequest& request;
    ParameterValues& effectiveParams;
    float pixelAspect{1.0F};
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
    std::function<std::optional<std::string>(const NodeCatalog&, const NodeInstance&, ParameterValues&)>;

// One registered built-in node. `inputRegions` is the last field by contract.
struct NodeContribution {
    NodeDescriptor descriptor;
    NodeRole role{NodeRole::Image};
    std::optional<CpuImplementation> cpu;
    std::string cpuUnavailableReason;
    bool nativeGpu{true};
    // Applies the node's typed parameter interpretation for an authoring
    // gesture without evaluating the graph. Empty for nodes whose parameters
    // carry no interpretation of their own.
    NodeParameterValidation validateParameters;
    std::vector<NodeEditorContribution> editors;
    // Spatial dependency rule (issue #85): the input coverage this node's
    // pixel implementation reads for a regional request. The result is indexed
    // by declared input port; a port the result does not cover (and an absent
    // optional slot) falls back to the node's own request, so an empty result
    // means "every real port needs exactly this node's region". Rules are the
    // node's own math (Blur's halo, Transform's inverse map and filter
    // footprint), never a node-type branch in the executor: the planner merely
    // clips and lattice-aligns what a contribution returns, and escalates the
    // whole image domain for a node whose capabilities declare
    // supportsRegion=false.
    std::function<std::vector<Region>(const NodeRegionContext&)> inputRegions;
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
                                                                ParameterValues& effectiveParams) const;

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
