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
    MediaCodecError(std::string codec, std::string message)
        : std::runtime_error("media encode: " + codec + ": " + std::move(message)), codec(std::move(codec)),
          message(std::move(message)) {}

    std::string codec;
    std::string message;
};

// One independently-decodable chunk encode.
struct EncodeOptions {
    // Encoder id from probeMediaCapabilities (e.g. "h264-nvenc").
    std::string codec;
    int gopSize = 24;       // intra period; chunk-sized GOPs for seeks
    int bitrateKbps = 2000;
};

struct EncodeStats {
    std::string codec;
    double encodeMsPerFrame = 0.0;   // measured wall time / frame
    double uploadNsPerFrame = 0.0;   // measured hw-upload cost (nvenc path)
    int64_t encodedBytes = 0;
    int encodedFrames = 0;
};

// Encodes the given frames as one independently decodable chunk (one GOP,
// intra-only start) to `outputPath` (mp4). Input is display-referred
// RGBA float32 (the viewer output after the viewing transform, 4:2:0
// downconversion happens here). Throws MediaCodecError naming `codec`
// when the encoder is not available on this build/device.
[[nodiscard]] EncodeStats encodeViewerChunk(const std::string& outputPath,
                                            const std::vector<CpuImage>& displayReferredFrames,
                                            const EncodeOptions& options);

// Display-referred scene-linear float RGBA -> Rec.709 4:2:0 8-bit planes.
// Pure math used by encodeViewerChunk and the fidelity measurement; also
// exposed for tests. Premultiplied-alpha-safe: alpha < 1 composites onto
// the declared black.
[[nodiscard]] std::vector<std::uint8_t> yuv420pFromDisplayReferred(const CpuImage& image);

}  // namespace nemo::media
