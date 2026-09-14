#include "nemo/nodes/Builtins.hpp"

#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor outputDescriptor() {
    return NodeDescriptor{.type = "output",
                          .displayName = "Output",
                          .group = "I/O",
                          .isOutput = true,
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "color"}},
                          .outputs = {},
                          .parameters = {},
                          .capabilities = builtinCapabilities()};
}

}  // namespace

// The Output is a network delivery role, not a pixel-filter adapter. Shared
// evaluation preserves its output-layout contract and reuses the input buffer
// when that layout already matches.
NodeContribution outputContribution() {
    NodeContribution contribution;
    contribution.descriptor = outputDescriptor();
    contribution.role = NodeRole::Output;
    return contribution;
}

}  // namespace nemo::nodes
