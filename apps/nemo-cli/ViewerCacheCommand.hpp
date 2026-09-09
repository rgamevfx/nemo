#pragma once

#include <string>
#include <vector>

// Headless GPU cache-build/replay harness. The command is declared outside the
// nemo namespace to match the existing nemo-cli command functions; main() owns
// dispatch and usage text.
//
// Required: cache-viewer <project.json> --cache-dir PATH --frames f1,f2,...
// Optional: --replay forward|reverse|random, --width W --height H,
// --scale 1|2|4, --shaders DIR, --codec ID, --chunk-frames N,
// --bitrate-kbps N, --output NAME, --stale-supersede, --fidelity.
int commandViewerCache(const std::vector<std::string>& args);
