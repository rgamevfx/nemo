#pragma once

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Ids.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include <stdexcept>

namespace nemo {

// Evaluation failures always identify the offending node when one exists
// (repo rule: errors identify the offending relationship).
struct EvaluationException : std::runtime_error {
    EvaluationException(std::string message, NodeId node = kInvalidNode, std::string nodeName = {})
        : std::runtime_error(std::move(message)), node(node), nodeName(std::move(nodeName)) {}

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
// decode provider). A provider returns the frame interpreted into the
// document's declared scene-linear working space per the reference's
// encoding (issue #21 semantics) and covering the request's raster —
// full-resolution region at the request's sampling scale.
//
// The CPU reference NEVER evaluates a source node as a synthetic pattern:
// without a provider the evaluation fails with an explicit node-identifying
// error. Throwing providers produce the same treatment; the evaluator
// attaches the offending node.
class SourceProvider {
public:
    virtual ~SourceProvider() = default;

    [[nodiscard]] virtual CpuImage frame(const Document& document, const SourceReference& source,
                                         std::int64_t mappedFrame, const EvaluationRequest& request) = 0;
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
[[nodiscard]] CpuEvaluation evaluateCpu(const Document& document, EvaluationRequest request,
                                        ResultCache<CpuImage>* reuse = nullptr, SourceProvider* sources = nullptr);

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
