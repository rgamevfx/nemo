#pragma once

// GPU factory declarations of the built-in node modules (issue #83).
//
// One declaration per NEMO_NODE(...) line of the single builtin list, so the
// native inventory cannot drift from the schema/CPU inventory: adding a node
// means one entry in BuiltinNodes.inc plus its module, never a second manual
// list. `nemo::eval::builtinGpuContributions()` (EffectLibrary.cpp) expands
// the same list to build the assembly, and each factory returns its node's
// core projection with the GPU implementation attached — or an explicit
// reason when the role has no pixel work.

#include "nemo/eval/GpuContribution.hpp"

namespace nemo::eval::nodes {

#define NEMO_NODE(name) [[nodiscard]] GpuNodeContribution name##GpuContribution();
#include "nemo/nodes/BuiltinNodes.inc"
#undef NEMO_NODE

}  // namespace nemo::eval::nodes
