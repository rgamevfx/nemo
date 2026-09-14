#include "nemo/nodes/Builtins.hpp"

#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor viewerDescriptor() {
    return NodeDescriptor{.type = "viewer",
                          .displayName = "Viewer",
                          .group = "I/O",
                          .isOutput = false,
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "color"}},
                          .outputs = {},
                          .parameters = {},
                          .capabilities = builtinCapabilities()};
}

}  // namespace

// The Viewer is display-only: the presentation module attaches it to an
// upstream result and owns every pixel it shows. It deliberately has no CPU or
// GPU pixel adapter, and it is not a network output, so an evaluation request
// can never select it as a result.
NodeContribution viewerContribution() {
    NodeContribution contribution;
    contribution.descriptor = viewerDescriptor();
    contribution.role = NodeRole::Viewer;
    contribution.nativeGpu = false;
    contribution.cpuUnavailableReason =
        "the Viewer is display-only: it has no CPU pixel implementation and presents an attached result instead";
    return contribution;
}

}  // namespace nemo::nodes
