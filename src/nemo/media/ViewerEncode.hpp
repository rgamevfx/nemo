#pragma once

// Hardware encode path for the viewer-cache representation (issue #10,
// spec sections 8/10.4, ADR-0004).
//
// The compact viewer representation is display-referred Rec.709 4:2:0
// (8-bit YUV420p, limited range). Encoders run behind this Media API:
// NVENC (h264-nvenc, hevc-nvenc) when the engine is available, CPU
// encoders (libx264, libx265) as declared comparators — never silently:
// each encode reports the codec used, and an unavailable hardware encoder
// fails with a clear reason rather than silently encoding on CPU.
//
// Every encode carries measured statistics (encode ms/frame, bytes, and
// the measured upload cost when the source leaves device residency — the
// capability-dependent transfer the spec requires exposing). The codec /
// chunk-size decision itself remains an evidence-gated prototype decision
// (the sweep harness, not this module).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/evaluation/Image.hpp"

namespace nemo::media {

struct MediaCodecError : std::runtime_error {
    MediaCodecError(std::string codecName, std::string detail)
        : std::runtime_error("media encode: " + codecName + ": " + detail), codec(std::move(codecName)),
          message(std::move(detail)) {}
    std::string codec;
    std::string message;
};

// Deterministic, opt-in failure injection (issue #21): names the encode
// stage that must fail and the 1-based occurrence of that stage's action.
// Carried in EncodeOptions — scoped to one encode call, no global mutable
// hooks — so tests drive it through the public encode API. An injected
// failure throws MediaCodecError with a message naming the stage and
// codec; it never returns partial statistics and leaves no partial
// output file.
struct EncodeFailure {
    enum class Stage {
        Allocation,    // context / hw-device / frame-pool allocation phase
        Init,          // encoder open (after hw pool init on the hw path)
        Submission,    // frame submission (counted per frame)
        Write,         // packet write to the muxer (counted per packet)
        Finalization,  // after the final drain, before the trailer write
    };
    Stage stage = Stage::Allocation;
    int occurrence = 1;  // 1-based: the Nth action of the stage fails
};

struct EncodeOptions {
    // Encoder id from probeMediaCapabilities (e.g. "h264-nvenc").
    std::string codec;
    int gopSize = 24;  // intra period; chunk-sized GOPs for seeks
    int bitrateKbps = 2000;
    // When non-null, the named stage fails deterministically on the Nth
    // action (issue #21 robustness tests). Null = never injected.
    const EncodeFailure* injectedFailure = nullptr;
};
struct EncodeStats {
    std::string codec;
    double encodeMsPerFrame = 0.0;  // measured wall time / frame
    double uploadNsPerFrame = 0.0;  // measured hw-upload cost (nvenc path)
    int64_t encodedBytes = 0;
    int encodedFrames = 0;
};

// Encodes the given frames as one independently decodable chunk (one GOP,
// intra-only start) to `outputPath` (mp4). Input is display-referred
// RGBA float32 with DisplayReferred layout — the baked BT.709 viewer
// output after its viewing transform; scene-linear input is rejected.
// The 4:2:0 downconversion here is matrix/range math only and re-applies no
// transfer. The chunk's complete interpretation (issue #21) is written as
// display-referred BT.709 — primaries BT.709, transfer BT.709, matrix
// BT.709, limited (MPEG) range, left chroma location, 8-bit 4:2:0 — into
// both the container stream and the coded bitstream, so replay needs no
// outside knowledge. Throws MediaCodecError naming `codec` when the
// encoder is not available on this build/device; on any error the
// partially written output is removed (never a partial-success report).
[[nodiscard]] EncodeStats encodeViewerChunk(const std::string& outputPath,
                                            const std::vector<CpuImage>& displayReferredFrames,
                                            const EncodeOptions& options);

}  // namespace nemo::media
