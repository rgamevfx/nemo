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

// The regional schedule of one request (issue #85): the dependency-first node
// order plus the coverage each node must actually produce. A consumer reads an
// input through the coverage recorded for its producer, so differently sized
// and differently anchored rasters are ordinary, not an error.
struct RegionPlan {
    std::vector<ExpandedNode> order;
    std::map<EvaluationNodeId, EvaluationRequest> requests;
};

// Projects one request onto per-node coverage (issue #85). Coverage is demanded
// from the output backwards: each node's request is the union of what its
// consumers need, and each node's own registered dependency rule (`inputRegions`
// in its NodeContribution) states what it reads from its inputs. Extras:
//
//  * A node whose declared capabilities say supportsRegion=false cannot honour
//    a sub-region internally, so its own coverage AND every input's coverage
//    escalate to the whole image domain; downstream consumers keep their own
//    (smaller) requests and read the escalated raster through its origin.
//  * Absent optional slots stay absent and demand nothing.
//  * Nested network instances are routing aliases: they forward their coverage
//    to the selected internal producer unchanged.
//  * Every returned region is clipped to the image domain and anchored to the
//    request's sampling lattice; an empty result (a demand entirely outside the
//    domain) conservatively escalates rather than producing an empty raster.
//
// `pixelAspects` supplies the resolved pixel aspect of an input the planner
// cannot know yet (decoded external media, keyed by the producing node). A
// supplied value overrides everything else; generators have the square-pixel
// aspect 1, a source whose aspect is unknown reports 0 (unknown, never assumed
// square), and every other node propagates its main input's aspect. A node that
// needs a tight bound but sees an unknown aspect requests the whole input.
[[nodiscard]] RegionPlan planDependencyRegions(const Document& document, const EvaluationRequest& request,
                                               const NodeContributions& contributions,
                                               const std::map<EvaluationNodeId, float>& pixelAspects = {});

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
// offending node.
class SourceProvider {
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
// With `reuse` (issue #9), matching content-keyed results are served from
// the cache, computed results are published under the evaluation ticket's
// freshness guard, and plan steps record reuse evidence. With `sources`
// (issue #11), source nodes are served by the provider; without one,
// evaluation rejects them explicitly. Throws EvaluationException with
// node-identifying messages.
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

// Validates a request for any executor (CPU reference and native GPU,
// issue #8): quality must be Full (spec section 8), channels RGBA, region
// within the reference bounds, the output node must exist and be an Output
// node, and the sampling scale must be declared (spec section 8: nodes
// declare supported reductions; a request beyond what the scheduled nodes
// declare is rejected, never approximated). Throws EvaluationException
// otherwise.
void validateRequest(const Document& document, const EvaluationRequest& request);

// Resolves `node`'s inputs in declared port order against `evaluated` (the
// image identities already produced) within `network`. Fills `step.inputs`
// and `step.inputImages`; returns producer node ids in port order.
std::vector<NodeId> resolveStepInputs(const Document& document, NetworkId network, const NodeInstance& node,
                                      const std::map<NodeId, ImageIdentity>& evaluated, PlanStep& step);

}  // namespace nemo
