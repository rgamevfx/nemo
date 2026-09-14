#pragma once

// The one explicit built-in node list (issue #83).
//
// BuiltinNodes.inc holds the bare `NEMO_NODE(<slug>)` names; this header turns
// them into core factory declarations in namespace nemo::nodes. The GPU
// projection declares its own factories from the same .inc, so the two backends
// cannot drift into two inventories. A node module implements
// <slug>Contribution() in src/nemo/nodes/<slug>/Contribution.cpp.

#include "nemo/core/evaluation/NodeContributions.hpp"

namespace nemo::nodes {

#define NEMO_NODE(name) [[nodiscard]] NodeContribution name##Contribution();
#include "nemo/nodes/BuiltinNodes.inc"
#undef NEMO_NODE

}  // namespace nemo::nodes
