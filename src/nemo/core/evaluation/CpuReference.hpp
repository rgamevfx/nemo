#pragma once

#include <map>
#include <optional>
#include <string>
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

// Resolves the request's output node: the named output, or the unique
// Output node when `outputName` is empty. Throws EvaluationException when
// no Output node exists ("no Output node" appears in the message), when the
// named node is missing or is not an Output node, or when several Output
// nodes exist without a name to disambiguate.
[[nodiscard]] NodeId resolveOutput(const Document& document, const std::string& outputName = {});

// Shared dependency-first schedule consumed by CPU and native executors.
[[nodiscard]] std::vector<const Node*> scheduleDependencies(const Document& document, NodeId output);

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
// image identities already produced). Fills `step.inputs` and
// `step.inputImages`; returns the producing node ids in port order. Throws
// EvaluationException when a port is unconnected or its producer was not
// evaluated. Shared by the CPU reference and the native GPU executor so
// both record identical plan input state.
std::vector<NodeId> resolveStepInputs(const Document& document, const Node& node,
                                      const std::map<NodeId, ImageIdentity>& evaluated, PlanStep& step);

}  // namespace nemo
