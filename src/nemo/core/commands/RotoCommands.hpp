#pragma once

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Roto.hpp"

namespace nemo {

// The single authoritative write for a node's authored Roto data (issue #93):
// one atomic topology transition.
//
// The command validates the candidate value completely before it publishes
// anything - unique element/point identities below the allocator watermarks,
// finite geometry, a closed shape with at least three points, a group parent
// that exists, is a group and stays acyclic, and every authored range - and
// applies lock semantics against the value the node currently holds, so a locked
// element can be neither edited nor removed (only unlocked). A rejected
// transition leaves the document, its revision and the history untouched, which
// is why the UI can keep a topology draft transient and commit it with one
// command.
[[nodiscard]] Command setRotoDataCommand(NetworkId network, NodeId node, RotoData data);

}  // namespace nemo
