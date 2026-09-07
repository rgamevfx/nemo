#pragma once

#include <cstdint>

namespace nemo {

// Document object identities are stable within a project's lifetime and are
// what serialization, commands, and automation all address objects by.
using NodeId = std::uint64_t;
using EdgeId = std::uint64_t;

inline constexpr NodeId kInvalidNode = 0;
inline constexpr EdgeId kInvalidEdge = 0;

}  // namespace nemo
