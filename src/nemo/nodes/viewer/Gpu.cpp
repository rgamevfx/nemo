// Viewer: presentation role (issue #83).
//
// The Viewer is not a pixel filter: it selects what the application presents
// on screen, and the presentation path (ViewerSession/ViewerPresentation)
// owns that work. It therefore contributes no pixel implementation, and says
// so explicitly instead of pretending to have one — a request that reaches it
// as a pixel node fails naming the node and this reason, never with a
// substituted image.

#include <optional>
#include <string>

#include "nemo/eval/GpuContribution.hpp"
#include "nemo/nodes/Builtins.hpp"

namespace nemo::eval::nodes {

GpuNodeContribution viewerGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::viewerContribution();
    contribution.gpu = std::nullopt;
    contribution.gpuUnavailableReason =
        "the Viewer node presents the composition on screen; it is not a pixel implementation";
    return contribution;
}

}  // namespace nemo::eval::nodes
