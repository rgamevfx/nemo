#pragma once

// Codec + independently-decodable-chunk experiment harness (issue #10,
// spec section 10.4, ADR-0004). Produces the MEASURED comparison the gate
// requires: encoding cost, chunk-boundary seek cost, decode-back fidelity
// against the source representation, and encoded bytes/frame — compared,
// not presupposed. The final codec/chunk-size choice is explicitly an
// evidence-gated prototype decision recorded from this table; nothing here
// defines completion.

#include <string>
#include <vector>

#include "nemo/core/evaluation/Image.hpp"

namespace nemo::media {

struct SweepEntry {
    std::string codec;       // encoder id (e.g. "hevc-nvenc")
    int chunkFrames = 0;     // independently decodable chunk size
    double encodeMsPerFrame = 0.0;
    double uploadNsPerFrame = 0.0;   // measured device-upload cost (hw path)
    double seekMsAtBoundary = 0.0;   // open + decode first chunk frame
    double decodeMsPerFrame = 0.0;
    double psnrDb = 0.0;             // fidelity vs source representation
    double bytesPerFrame = 0.0;
};

struct SweepReport {
    std::vector<SweepEntry> entries;
    // Peak decode resources: process VmHWM (peak resident set, KiB)
    // captured after the decode-back runs (issue #10 acceptance example 2).
    long peakDecodeVmHwmKb = 0;
    bool available = false;
    // Markdown table of the measured results (evidence artifact).
    [[nodiscard]] std::string table() const;
};

// Runs the sweep over `sourceDisplayReferred` frames (RGBA float32,
// display-referred — the viewer output shape). Chunks are encoded as
// independent files (each decodable from its own keyframe, so seek cost at
// a chunk boundary is measured as open+prime+first-frame). Unavailable
// codecs produce an entry with psnrDb < 0 and a codec name preserved (the
// evidence table shows the gap rather than dropping the candidate).
[[nodiscard]] SweepReport runCodecSweep(const std::vector<CpuImage>& sourceDisplayReferred,
                                        const std::vector<std::string>& codecs,
                                        const std::vector<int>& chunkSizes);

}  // namespace nemo::media
