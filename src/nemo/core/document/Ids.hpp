#pragma once

#include <cstdint>

namespace nemo {

// Document object identities are stable within a project's lifetime and are
// what serialization, commands, and automation all address objects by.
// Node and edge identities are local to a Network; network and instance
// identities are document-scoped.
using NetworkId = std::uint64_t;
using NetworkInstanceId = std::uint64_t;
using InterfacePortId = std::uint64_t;
using NodeId = std::uint64_t;
using EdgeId = std::uint64_t;
using MediaSourceId = std::uint64_t;
using MediaBinId = std::uint64_t;
inline constexpr NetworkId kInvalidNetwork = 0;
inline constexpr NetworkInstanceId kInvalidNetworkInstance = 0;
inline constexpr InterfacePortId kInvalidInterfacePort = 0;
// Node identity 0 is also the evaluation sentinel for an absent optional input
// slot (`EvaluationNodeId{}`): a declared port with no producer. Plan arrays
// keep this invalid entry to stay aligned with the schema.
inline constexpr NodeId kInvalidNode = 0;
inline constexpr EdgeId kInvalidEdge = 0;
inline constexpr MediaSourceId kInvalidMediaSource = 0;
inline constexpr MediaBinId kInvalidMediaBin = 0;
}  // namespace nemo
