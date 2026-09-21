#pragma once

#include <string_view>

namespace nemo::eval {

// Shared by shader execution and preactivation package validation, including
// CPU-only hosts. Change this identity whenever binding layout or semantics
// change; node-local payload layouts carry their own versions.
inline constexpr std::string_view kEffectBindingContractVersion = "nemo.native.bindings.v8";

}  // namespace nemo::eval
