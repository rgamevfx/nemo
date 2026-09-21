#pragma once

#include <memory>
#include <string>
#include <vector>

#ifdef NEMO_BUILD_GPU
#include "nemo/eval/GpuContribution.hpp"
#endif

namespace nemo {
class NodeContributions;
}  // namespace nemo

// Headless BC7 cache-build/replay harness. The command is declared outside the
// nemo namespace to match the existing nemo-cli command functions; main() owns
// dispatch and usage text.
//
// Required: cache-viewer <project.json> --cache-dir PATH --frames f1,f2,...
// Optional: --replay forward|reverse|random, --width W --height H,
// --scale 1|2|4, --shaders DIR, --output NAME, --stale-supersede, --fidelity,
// --edit-node NAME --edit-key KEY --edit-value VALUE.
//
// The cache representation is BC7 (issue #106): the build phase renders the
// explicitly requested frames and the independent replay session resolves them
// from retained compressed frames without evaluating the graph. A retained
// frame whose blocks are still loading is retried until ready — never
// re-rendered — so `--fidelity` compares the live display reference against the
// sampled BC7 representation, not against a second live render.
//
// `contributions` is the composed CPU inventory the probe session is built
// with, so this harness evaluates the SAME node set the application and the CPU
// reference do. Under a GPU build, `gpuContributions` is the composed native
// list (built-ins plus installed packages) the viewer sessions render with; it
// is passed by value because a ViewerSession takes ownership of its library.
int commandViewerCache(const std::vector<std::string>& args,
                       std::shared_ptr<const nemo::NodeContributions> contributions
#ifdef NEMO_BUILD_GPU
                       ,
                       std::vector<nemo::eval::GpuNodeContribution> gpuContributions
#endif
);
