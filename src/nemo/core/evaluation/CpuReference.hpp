#pragma once

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Ids.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
#include <stdexcept>

namespace nemo {

// Evaluation failures always identify the offending node when one exists
// (repo rule: errors identify the offending relationship).
struct EvaluationException : std::runtime_error {
    EvaluationException(std::string message, NodeId node = kInvalidNode, std::string nodeName = {})
        : std::runtime_error(message), node(node), nodeName(std::move(nodeName)) {}

    [[nodiscard]] bool hasNode() const { return node != kInvalidNode; }

    NodeId node{kInvalidNode};
    std::string nodeName;
};

// What the CPU reference produced: the plan (the portable, GPU-ready
// contract) plus a host pixel buffer that belongs to this reference only
// (ADR-0004: CPU pixel buffers must not define the universal interface).
struct CpuEvaluation {
    EvaluationPlan plan;
    CpuImage image;
};

inline constexpr std::uint32_t kEvaluationWholeNode = std::numeric_limits<std::uint32_t>::max();

struct EvaluationNodeId {
    NetworkId network{kInvalidNetwork};
    NetworkInstanceId instance{kInvalidNetworkInstance};
    NodeId node{kInvalidNode};
    std::uint32_t outputPort{kEvaluationWholeNode};
    // Persistent instance IDs identify definitions, while this path
    // disambiguates the same nested definition reached through multiple
    // outer occurrences.
    std::vector<NetworkInstanceId> path{};

    friend bool operator==(const EvaluationNodeId&, const EvaluationNodeId&) = default;
    friend bool operator<(const EvaluationNodeId& left, const EvaluationNodeId& right) {
        return std::tie(left.path, left.network, left.instance, left.node, left.outputPort) <
               std::tie(right.path, right.network, right.instance, right.node, right.outputPort);
    }
};

struct ExpandedNode {
    EvaluationNodeId id;
    const NodeInstance* node{};
    // One entry per declared input port, in port order. An absent optional
    // input holds the invalid sentinel `EvaluationNodeId{}` (node ==
    // kInvalidNode) so later stages keep declared-port alignment.
    std::vector<EvaluationNodeId> inputs;
    std::optional<EvaluationNodeId> alias;
};

// Coverage padding for regional planning (issue #85): an interactive consumer
// asks for the visible region, which moves by a few pixels per pan. Planned
// coverage is therefore rounded outward to whole blocks of this many raster
// samples (clipped to the image domain) before per-node coverage is derived, so
// a small pan lands inside coverage that is already resident instead of
// recomputing the same pixels at a shifted origin. Padding only ever grows
// coverage; the delivered raster is cropped back to the consumer's normalized
// request. Set to 0 to plan exactly the demanded regions.
inline constexpr int kCoveragePaddingRasterPixels = 64;

// Header-only geometry seam for real source media (issue #88). Describing a
// source is a metadata operation: it reads the media's own header/discovery
// facts, never a decoded frame, so planning a graph cannot block on a decoder,
// allocate a device image or discover geometry by rendering.
//
// The provider receives the SAME pre-resolved effective source request the
// frame seam receives (mapping, selected coverage, policy outcome, authored
// color choices), so geometry is reported for exactly the frame the request
// will read. A source whose geometry cannot be obtained must fail here, with a
// provider or node identifying error: the execution layer wraps it with the
// offending node and never invents a format.
class SourceDescriptionProvider {
public:
    virtual ~SourceDescriptionProvider() = default;

    [[nodiscard]] virtual ImageDescription describe(const Document& document, const EffectiveSourceRequest& source) = 0;
};

// One node's resolved image state (issue #88): the node with its immutable
// effective parameters (instance overrides, animation and scope) already
// resolved, the description of the image it produces, and — for a source node —
// the effective source request it reads. Resolved ONCE here and then shared by
// description, dependency planning, key computation and CPU/native execution,
// so no later stage re-derives animated or source state.
struct ResolvedImageNode {
    NodeInstance node;
    ImageDescription description;
    std::optional<EffectiveSourceRequest> source;
};

// The described dependency set of one request (issue #88): the dependency-first
// expansion plus every scheduled node's resolved state. There is no coverage
// and no pixel callback here — describing a graph never runs it.
struct ImageDescriptionPlan {
    std::vector<ExpandedNode> order;
    std::map<EvaluationNodeId, ResolvedImageNode> nodes;
};

// The regional schedule of one request (issue #88): the resolved description
// plan for the whole dependency set, the coverage each node must actually
// produce, and the normalized request the caller is answered with. A consumer
// reads an input through the coverage recorded for its producer, so differently
// sized and differently anchored rasters are ordinary, not an error.
struct RegionPlan {
    ImageDescriptionPlan images;
    std::map<EvaluationNodeId, EvaluationRequest> requests;
    EvaluationRequest request;
    // Origin identity of this plan (issue #88): the exact immutable state it was
    // resolved from. A consumer that is HANDED a plan (the viewer shares one
    // between its cache key and its execution) verifies these fields instead of
    // re-deriving the authored state, so a plan that belongs to another document
    // snapshot, registration or demand is refused rather than executed — and a
    // request and its plan can never disagree. The plan a caller builds for
    // itself always matches its own call.
    //
    // `demand` is the canonical form of the request this plan was planned for
    // (the planner rebases `request` onto the described target format, so
    // `demand` is the field to compare against a caller's own request).
    const Document* document{};
    std::uint64_t documentRevision{};
    const NodeContributions* contributions{};
    EvaluationRequest demand;
};

// Describes every node the request depends on, dependency-first (issue #88).
// Each node's declaration (registration, schema compatibility), parameters and
// description are validated here, and a source node's geometry comes from the
// provided `sources` seam, so every metadata/declaration failure is reported —
// with the offending node and source — before a single pixel is dispatched.
//
// Description precedence is the contribution contract: a node declares a
// `describe` rule and states the whole description it produces; without one it
// keeps what it inherits, which is its connected main input's description or,
// for a generator, the owning network's authored canvas (issue #96). A source
// node's description comes from its media provider for exactly the frame the
// request reads; a source whose geometry cannot be obtained fails here with the
// offending node and source, so no empty, zero or invented description ever
// reaches a consumer. Every accepted description is complete: positive format,
// finite positive aspect, named channels.
[[nodiscard]] ImageDescriptionPlan describeDependencies(const Document& document, const EvaluationRequest& request,
                                                        const NodeContributions& contributions,
                                                        SourceDescriptionProvider* sources = nullptr);

// Projects one request onto per-node coverage and descriptions (issue #88).
// Descriptions are resolved first, from them the request's logical format, and
// only then is coverage demanded from the output backwards: each node's request
// is the union of what its consumers need, and each node's own registered
// dependency rule (`inputRequirements` in its NodeContribution) states what it
// reads from its inputs. Extras:
//
//  * A node whose declared capabilities say supportsRegion=false cannot honour
//    a sub-region internally, so its own coverage AND every input's coverage
//    escalate to that node's whole useful domain (its format unioned with its
//    data bounds, so an off-format data window survives). Downstream consumers
//    keep their own smaller requests and read the escalated raster through its
//    origin.
//  * Absent optional slots stay absent and demand nothing.
//  * Nested network instances are routing aliases: they forward their coverage
//    to the selected internal producer unchanged.
//  * Coverage is padding-limited to the producing node's useful domain, but the
//    demanded region itself is never discarded, clipped away or escalated
//    somewhere else: a demand outside the format is exactly the transparent
//    black the caller asked for. Every returned region carries a
//    lattice-anchored origin and a signed origin.
//  * Each per-node request states that node's OWN format as its domain and the
//    channels its consumers demand, and is validated against the executor's
//    limits and the node's declared capabilities before any pixel work.
[[nodiscard]] RegionPlan planDependencyRegions(const Document& document, const EvaluationRequest& request,
                                               const NodeContributions& contributions,
                                               SourceDescriptionProvider* sources = nullptr);

// Expands a scoped request into one dependency-first plan. Network instances
// are represented by alias steps whose source is the selected formal output;
// formal inputs resolve to the instance's parent bindings. No persistent graph
// is copied or mutated.
[[nodiscard]] std::vector<ExpandedNode> expandDependencies(const Document& document, NetworkId network, NodeId output);

// Resolves the request's output node in `network`: a named output, or the
// network's designated default output. Throws EvaluationException when no
// Output node exists, the named node is missing/not an Output, or no default
// can disambiguate multiple outputs.
[[nodiscard]] NodeId resolveOutput(const Document& document, NetworkId network, const std::string& outputName = {});

// Shared dependency-first schedule consumed by CPU and native executors.
[[nodiscard]] std::vector<const NodeInstance*> scheduleDependencies(const Document& document, NetworkId network,
                                                                    NodeId output);

// External seam for real source media (issue #11). The persistent Document
// carries only source references; the decoded frames live behind this
// interface, supplied by the execution layer (eval's SourceSession / any
// decode provider). The provider receives the *resolved* effective source
// request (issue #75): mapping, selected coverage, boundary/missing policy
// outcome, media-interpretation hints and authored color choices are already
// combined by `resolveSourceRequest`, so no provider re-derives them.
//
// A provider returns the frame interpreted into the document's declared
// scene-linear working space per the request's encoding (issue #21 semantics)
// and covering the request's raster — full-resolution region at the request's
// sampling scale. A request whose policy resolved to `transparentBlack` is
// served with a real transparent-black raster by the provider, and a request
// whose policy resolved to `policyError` must never reach the provider. The
// CPU reference NEVER evaluates a source node as a synthetic pattern: without a
// provider the evaluation fails with an explicit node-identifying error.
// Throwing providers produce the same treatment; the evaluator attaches the
// offending node. The same provider describes that source's geometry
// (SourceDescriptionProvider), so a Read's format and its pixels come from one
// owner and can never disagree.
class SourceProvider : public SourceDescriptionProvider {
public:
    virtual ~SourceProvider() = default;

    [[nodiscard]] virtual CpuImage frame(const Document& document, const EffectiveSourceRequest& source,
                                         const EvaluationRequest& request) = 0;

    // OCIO content identity of the color configuration this provider resolves
    // media color against; empty when it has none (the legacy fixed
    // interpretation, which is a defined identity rather than a missing one).
    // The evaluator mixes it into source-node result keys, so provider and
    // configuration content can never be swapped under a cached result.
    //
    // Returned by value on purpose: a provider may replace its retained
    // configuration (and its cached identity) at any time, so a borrowed view
    // would invite a stale or dangling identity at a caller that keeps it.
    [[nodiscard]] virtual std::string colorConfigIdentity() const { return {}; }
};

// Evaluates the document graph topologically to satisfy `request.output`.
// The graph is acyclic by construction (Graph rejects cycles); only the
// required dependencies of the output are scheduled (spec section 10.3).
// Descriptions, parameters and source requests are resolved once, up front
// (issue #88): every metadata, declaration, parameter and source-geometry
// failure is reported with its offending node before the first raster exists,
// and execution consumes exactly the resolved state the plan recorded.
// With `reuse` (issue #9), matching content-keyed results are served from
// the cache, computed results are published under the evaluation ticket's
// freshness guard, and plan steps record reuse evidence. With `sources`
// (issue #11), source nodes are served by the provider — the same provider
// describes their geometry; without one, evaluation rejects them explicitly.
// Throws EvaluationException with node-identifying messages.
//
// `contributions` (issue #83) is the immutable node registration this
// evaluation executes: the caller may hold its own snapshot (a test-only
// contribution extends builtinContributions()), and the default is the
// built-in registration. Every non-structural node's schema/backend
// compatibility is checked against it before any cache or alias hit, and the
// snapshot is retained for the call's duration.
[[nodiscard]] CpuEvaluation
evaluateCpu(const Document& document, EvaluationRequest request, ResultCache<CpuImage>* reuse = nullptr,
            SourceProvider* sources = nullptr,
            std::shared_ptr<const NodeContributions> contributions = builtinNodeContributions());

// Checks only executor extent/sampling limits, before raster/ROI arithmetic.
// An authored format may exceed current execution capabilities without being
// invalid persistent state. No graph walk, allocation or media probe.
//
// The region is signed and may lie outside the domain (issue #88): the domain
// is the image's logical format, and a demand beyond it is a real request for
// transparent black, so overscan and off-format windows are representable
// rather than invalid. Only the raster extent and the coordinate range a signed
// origin may reach are bounded.
void validateRequestDomain(const EvaluationRequest& request);

// Validates a request for any executor (CPU reference and native GPU,
// issue #8): quality must be Full (spec section 8), an explicit channel demand
// must be part of the node types' declared vocabulary, the region must be
// within the reference bounds, the output node must exist and be an Output
// node, and the sampling scale must be declared (spec section 8: nodes
// declare supported reductions; a request beyond what the scheduled nodes
// declare is rejected, never approximated). An empty channel demand means
// every channel the requested image names (issue #90) and needs no
// declaration. Throws EvaluationException otherwise.
void validateRequest(const Document& document, const EvaluationRequest& request);

// Resolves `node`'s inputs in declared port order against `evaluated` (the
// image identities already produced) within `network`. Fills `step.inputs`
// and `step.inputImages`; returns producer node ids in port order.
std::vector<NodeId> resolveStepInputs(const Document& document, NetworkId network, const NodeInstance& node,
                                      const std::map<NodeId, ImageIdentity>& evaluated, PlanStep& step);

}  // namespace nemo
