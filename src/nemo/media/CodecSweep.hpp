#pragma once

// Corrective codec experiment (#23), not integrated viewer-cache gate evidence.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nemo/media/ViewerEncode.hpp"

namespace nemo::media {

struct SweepOptions {
    std::vector<std::string> codecs = {"h264-nvenc", "hevc-nvenc", "libx264-cpu", "libx265-cpu"};
    std::vector<int> chunkSizes = {4, 8, 12};
    int bitrateKbps = 2000;
    std::string profile;  // empty: explicit H.264 high / HEVC main
    int bitDepth = 8;
    int64_t maxFrames = 200;  // upper bound; a shorter source remains a valid workload
    int width = 1920;
    int height = 1080;
    const EncodeFailure* injectedFailure = nullptr;  // same scoped encoder failure seam
};

struct ChunkDecodeStats {
    double decodeMs = 0.0;  // open + decode + RGB conversion; excludes comparison
    double squaredError = 0.0;
    uint64_t samples = 0;
    double maxAbsoluteError = 0.0;
};

// Full-image RGB comparison, peak 1.0, no clipping; alpha excluded. Validates
// exact frame count, dimensions, corruption and finite pixels before success.
// Replay uses the Media decoder's bilinear left-sited chroma reconstruction.
[[nodiscard]] ChunkDecodeStats compareViewerChunk(const std::string& path, std::span<const CpuImage> reference);

struct SweepMeasurements {
    EncodeStats encode;  // totals over successfully verified chunks, all ms
    double decodeMs = 0.0;
    std::optional<double> seekMsAtBoundary;
    double psnrDb = 0.0;
    double maxAbsoluteError = 0.0;
    uint64_t containerBytes = 0;
    double firstChunkMs = 0.0;
    std::optional<double> subsequentChunkMs;
    double preparationMs = 0.0;  // source open/decode/convert/sample, separate from chunk encoding
};

struct SweepEntry {
    std::string codec;
    std::string profile;
    int bitDepth = 8;
    int bitrateKbps = 0;
    int chunkFrames = 0;
    int64_t requestedFrames = 0;
    int64_t verifiedFrames = 0;
    int verifiedChunks = 0;
    std::string unavailableReason;
    std::optional<SweepMeasurements> measurements;  // absent on ANY candidate failure
    uint64_t retainedReferenceBytes = 0;            // peak active chunk float payload
    std::optional<uint64_t> retainedDecodeBytes;    // verified replay float payload; unknown if decode failed
    std::optional<uint64_t> processPeakRssKiB;      // cumulative process VmHWM, NOT decoder/VRAM
};

struct SweepReport {
    std::string sourceDescription;
    int width = 0;
    int height = 0;
    std::vector<SweepEntry> entries;
    [[nodiscard]] std::string table() const;
};

// Path input retains at most one chunk of scaled references plus one decoded
// replay frame. Each candidate reopens the source; OS/library warmth is not
// controlled, and encoder/device/pool resources are fresh for EVERY chunk.
[[nodiscard]] SweepReport runCodecSweep(const std::string& sourcePath, const SweepOptions& options);
[[nodiscard]] SweepReport runCodecSweep(std::span<const CpuImage> reference, const SweepOptions& options);

}  // namespace nemo::media
