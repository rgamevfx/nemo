#pragma once

// External GPU projection of installed packages (issue #37).
//
// The native GPU inventory this build runs with: the built-in contributions
// plus one contribution per installed package. An installed package that
// declares a native implementation keeps the manifest's payload size, payload
// layout and complete shader, and its preparation callback invokes the
// package's own `prepare` through the fixed ABI, so the GPU path and the CPU
// adapter of the same package are two independent implementations of one
// declared contract (ADR-0004) rather than one wrapping the other.
//
// This header is only available in a build with the native GPU module; the
// loader itself (InstalledPackages) never depends on it.

#include <vector>

#include "nemo/eval/GpuContribution.hpp"
#include "nemo/extensions/InstalledPackages.hpp"

namespace nemo::extensions {

// The built-in GPU contributions followed by one contribution per installed
// package, in discovery order. Every registered node type stays covered: a
// package whose manifest declares no native implementation, or one that
// declares a binding contract this build does not implement, is projected with
// `nativeGpu` false and an explicit unavailability reason, so requesting it
// fails honestly while every other node stays usable.
[[nodiscard]] std::vector<nemo::eval::GpuNodeContribution> gpuContributions(const InstalledPackages& packages);

}  // namespace nemo::extensions
