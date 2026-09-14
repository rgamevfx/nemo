// Test-only affine RGB contribution (issue #83 extension proof).
//
// This is NOT a shipped artist-facing node. It exists to prove that an
// ordinary effect can be contributed as a module plus one registration entry:
// a typed schema, a CPU adapter, and a native Slang adapter with node-local
// payload preparation, all assembled through the same production
// NodeContributions/EffectLibrary builders the built-ins use. It never
// impersonates Grade and never reaches a private evaluator branch.

#pragma once

#include <string_view>

#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/eval/GpuContribution.hpp"

namespace nemo::test::affine {

// Persistent, namespaced identity of the test contribution. Namespacing keeps
// it from ever colliding with a shipped built-in identity; registration stays
// explicit and test-owned.
inline constexpr std::string_view kNodeType{"nemo.test.affine"};

// Implementation version shared by the schema, the CPU adapter, and the
// native Slang adapter. A changed implementation requires a new version so it
// cannot serve a stale cached result.
inline constexpr std::uint64_t kImplementationVersion{1};

[[nodiscard]] NodeDescriptor affineDescriptor();
[[nodiscard]] NodeContribution affineContribution();
[[nodiscard]] eval::GpuNodeContribution affineGpuContribution();

}  // namespace nemo::test::affine
