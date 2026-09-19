#pragma once

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
// Merge composite operations (issue #75). Over stays the default with the
// straight-alpha convention; the other four extend foreground coverage to an
// explicit per-channel blend target. The typed interpretation
// is shared by the CPU reference, the GPU executor, and both shader front ends,
// so an unknown authored value fails explicitly in every executor instead of
// silently selecting a fallback.
enum class MergeOperation { Over, Plus, Multiply, Screen, Difference };

// Merge's typed operation (issue #75). Both executors resolve the authored
// choice through this one function, so an unsupported value names the same
// supported set in every execution path (the descriptor's choices reject it
// earlier still, on author and on deserialize).
[[nodiscard]] inline MergeOperation effectiveMergeOperation(const NodeCatalog& catalog, const NodeInstance& node,
                                                            const ParameterValues& effectiveParams) {
    const std::string& operation = effectiveChoice(catalog, node, effectiveParams, "operation");
    if (operation == "over") {
        return MergeOperation::Over;
    }
    if (operation == "plus") {
        return MergeOperation::Plus;
    }
    if (operation == "multiply") {
        return MergeOperation::Multiply;
    }
    if (operation == "screen") {
        return MergeOperation::Screen;
    }
    if (operation == "difference") {
        return MergeOperation::Difference;
    }
    failNode(node,
             "parameter 'operation' must be one of over, plus, multiply, screen, difference, got '" + operation + "'");
}
}  // namespace nemo
