// Test-only affine RGB contribution (issue #83 extension proof).
//
// This is NOT a shipped artist-facing node. It exists to prove that an
// ordinary effect can be contributed as a module plus one registration entry:
// a typed schema, a CPU adapter, and a native Slang adapter with node-local
// payload preparation, all assembled through the same production
// NodeContributions/EffectLibrary builders the built-ins use. It never
// impersonates Grade and never reaches a private evaluator branch.
//
// Issue #88 extension: the same example also declares its own image meaning and
// its own input demand. An authored integer shift makes it read its input at an
// absolute image-space offset, so the node describes a data window translated by
// that shift and declares an input requirement translated the other way — a
// demand that genuinely differs from the node's own region. Nothing outside this
// module and its registration entry changes for that.

#pragma once

#include <cstdint>
#include <string_view>

#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/eval/GpuContribution.hpp"

namespace nemo::test::affine {

// Persistent, namespaced identity of the test contribution. Namespacing keeps
// it from ever colliding with a shipped built-in identity; registration stays
// explicit and test-owned.
inline constexpr std::string_view kNodeType{"nemo.test.affine"};

// Second identity of the SAME pixel behavior, declared whole-frame-only
// (capabilities.supportsRegion = false). It exists so a test can prove that a
// region request over a whole-frame-only contribution escalates that node (and
// its inputs) to the whole image domain and still delivers the consumer's
// rectangle — the declared capability, not a node-name branch, decides.
inline constexpr std::string_view kWholeFrameNodeType{"nemo.test.affineWholeFrame"};

// Largest authored read offset the example accepts, in full-resolution pixels.
// The bound keeps the declared demand and the read index inside the executor's
// integer coordinate range; a larger value is a defined node-identifying error
// rather than an overflow.
inline constexpr std::int64_t kMaxAffineShift{1 << 20};

// Implementation version shared by the schema, the CPU adapter, and the
// native Slang adapter. A changed implementation requires a new version so it
// cannot serve a stale cached result. 2: the example declares its own
// description and input demand and reads at an absolute image-space offset
// (issue #88). 3: named channel storage and native channel-plane bindings.
inline constexpr std::uint64_t kImplementationVersion{3};

[[nodiscard]] NodeDescriptor affineDescriptor();
[[nodiscard]] NodeDescriptor affineWholeFrameDescriptor();
[[nodiscard]] NodeContribution affineContribution();
[[nodiscard]] NodeContribution affineWholeFrameContribution();
[[nodiscard]] eval::GpuNodeContribution affineGpuContribution();
[[nodiscard]] eval::GpuNodeContribution affineWholeFrameGpuContribution();

}  // namespace nemo::test::affine
