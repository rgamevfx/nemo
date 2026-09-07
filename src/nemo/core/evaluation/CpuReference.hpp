#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Ids.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"

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

// Evaluates the document graph topologically to satisfy `request.output`.
// The graph is acyclic by construction (Graph rejects cycles); only the
// required dependencies of the output are scheduled (spec section 10.3).
// Throws EvaluationException with node-identifying messages.
[[nodiscard]] CpuEvaluation evaluateCpu(const Document& document, EvaluationRequest request);

}  // namespace nemo
