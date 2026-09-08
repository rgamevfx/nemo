#include "nemo/media/CodecSweep.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nemo/media/VideoDecode.hpp"
#include "nemo/media/ViewerEncode.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace nemo::media {

namespace {

using clock = std::chrono::steady_clock;

struct ChunkTimings {
    double encodeMsPerFrame = 0.0;
    double uploadNsPerFrame = 0.0;
    double decodeMsPerFrame = 0.0;
    double seekMsAtBoundary = 0.0;
    double psnrDb = -0.0;
    double bytesPerFrame = 0.0;
};

// Encodes the frames as one independently decodable chunk and measures.
[[nodiscard]] bool encodeChunk(const std::string& path, const std::vector<CpuImage>& frames,
                               const EncodeOptions& options, ChunkTimings& timings) {
    try {
        const EncodeStats stats = encodeViewerChunk(path, frames, options);
        timings.encodeMsPerFrame = stats.encodeMsPerFrame;
        timings.uploadNsPerFrame = stats.uploadNsPerFrame;
        timings.bytesPerFrame =
            stats.encodedFrames > 0 ? static_cast<double>(stats.encodedBytes) / stats.encodedFrames : 0.0;
        return true;
    } catch (const MediaCodecError&) {
        return false;  // unavailable codec: recorded as a gap in the table
    }
}

// Decodes the chunk back and measures decode cost + fidelity (PSNR in the
// display-referred float space against the same source frames).
[[nodiscard]] bool decodeChunkAndMeasure(const std::string& path, const std::vector<CpuImage>& source,
                                         ChunkTimings& timings, bool seekAtBoundary) {
    const auto decodeStart = clock::now();
    const SoftwareClip decoded = decodeViewerChunkSoftware(path);
    if (decoded.frames.empty()) {
        return false;
    }
    const double decodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - decodeStart).count();
    timings.decodeMsPerFrame = decodeNs / 1e6 / decoded.frames.size();

    if (seekAtBoundary) {
        // Chunk-boundary seek cost: fresh open + decode of the first frame
        // (the boundary). Re-open and prime through the decoder to the
        // boundary frame — this is the actual viewer-replay behavior at a
        // chunk boundary (issue #10 acceptance example 2).
        const auto seekStart = clock::now();
        const SoftwareClip boundary = decodeViewerChunkSoftware(path, 1);
        timings.seekMsAtBoundary =
            boundary.frames.empty()
                ? -1.0
                : std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - seekStart).count() / 1e6;
    }

    // Fidelity: PSNR over the decode-back RGBA against the source frames
    // (the same 709 matrix on both sides; deltas are codec + 4:2:0 error).
    double mse = 0.0;
    size_t samples = 0;
    for (size_t frame = 0; frame < decoded.frames.size(); ++frame) {
        if (frame >= source.size()) {
            break;
        }
        const CpuImage& a = decoded.frames[frame];
        const CpuImage& b = source[frame];
        for (int y = 0; y < a.height(); y += 2) {
            for (int x = 0; x < a.width(); x += 2) {
                const auto pa = a.pixel(x, y);
                const auto pb = b.pixel(x, y);
                for (int channel = 0; channel < 3; ++channel) {
                    const double difference = static_cast<double>(pa[channel]) - static_cast<double>(pb[channel]);
                    mse += difference * difference;
                    ++samples;
                }
            }
        }
    }
    if (samples == 0 || mse == 0.0) {
        timings.psnrDb = 100.0;  // bit-exact
    } else {
        timings.psnrDb = 10.0 * std::log10(1.0 / (mse / static_cast<double>(samples)));
    }
    return true;
}

}  // namespace

SweepReport runCodecSweep(const std::vector<CpuImage>& sourceDisplayReferred, const std::vector<std::string>& codecs,
                          const std::vector<int>& chunkSizes) {
    SweepReport report;
    const auto tempDir = std::filesystem::temp_directory_path() / "nemo-codec-sweep";
    std::filesystem::create_directories(tempDir);

    for (const std::string& codec : codecs) {
        for (int chunkFrames : chunkSizes) {
            SweepEntry entry;
            entry.codec = codec;
            entry.chunkFrames = chunkFrames;
            double encodeMsTotal = 0.0;
            double uploadNsTotal = 0.0;
            double decodeMsTotal = 0.0;
            double seekMsTotal = 0.0;
            double psnrMin = 1000.0;
            double bytesPerFrameTotal = 0.0;
            int chunks = 0;
            bool usable = true;

            for (size_t start = 0; start + 1 < sourceDisplayReferred.size() + 1; start += chunkFrames) {
                if (start >= sourceDisplayReferred.size()) {
                    break;
                }
                const size_t count = std::min(static_cast<size_t>(chunkFrames), sourceDisplayReferred.size() - start);
                std::vector<CpuImage> frames(sourceDisplayReferred.begin() + static_cast<long>(start),
                                             sourceDisplayReferred.begin() + static_cast<long>(start + count));
                const std::string path = (tempDir / ("chunk-" + codec + "-" + std::to_string(chunkFrames) + "-" +
                                                     std::to_string(start) + ".mp4"))
                                             .string();

                ChunkTimings chunkTimings;
                if (!encodeChunk(path, frames, {codec, chunkFrames, 2000}, chunkTimings)) {
                    usable = false;
                    break;
                }
                encodeMsTotal += chunkTimings.encodeMsPerFrame * static_cast<double>(count);
                uploadNsTotal += chunkTimings.uploadNsPerFrame * static_cast<double>(count);
                bytesPerFrameTotal += chunkTimings.bytesPerFrame * static_cast<double>(count);
                if (!decodeChunkAndMeasure(path, frames, chunkTimings, chunks > 0)) {
                    usable = false;
                    break;
                }
                decodeMsTotal += chunkTimings.decodeMsPerFrame * static_cast<double>(count);
                seekMsTotal += chunkTimings.seekMsAtBoundary;
                psnrMin = std::min(psnrMin, chunkTimings.psnrDb);
                ++chunks;
            }
            if (!usable) {
                // Unavailable candidate: recorded as a measured gap (the
                // table renders n/a), never as an all-zero row.
                entry.psnrDb = -1.0;
            }

            if (usable && chunks > 0) {
                entry.encodeMsPerFrame = encodeMsTotal / static_cast<double>(sourceDisplayReferred.size());
                entry.uploadNsPerFrame = uploadNsTotal / static_cast<double>(sourceDisplayReferred.size()) / 1e6;
                entry.decodeMsPerFrame = decodeMsTotal / static_cast<double>(sourceDisplayReferred.size());
                entry.seekMsAtBoundary = chunks > 0 ? seekMsTotal / static_cast<double>(chunks - 1) : -1.0;
                entry.psnrDb = psnrMin;
                entry.bytesPerFrame = bytesPerFrameTotal / static_cast<double>(sourceDisplayReferred.size());
            }
            report.entries.push_back(entry);
        }
        // Peak decode resources: VmHWM after the decode-back workload.
        std::ifstream statusFile("/proc/self/status");
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.starts_with("VmHWM:")) {
                // "VmHWM:  12345 kB" — skip the label + colon.
                const long kb = std::strtol(line.c_str() + 6, nullptr, 10);
                report.peakDecodeVmHwmKb = std::max(report.peakDecodeVmHwmKb, kb);
                report.available = true;
            }
        }
    }
    // Clean the temp chunks (they are measurement artifacts).
    std::filesystem::remove_all(tempDir);
    return report;
}

std::string SweepReport::table() const {
    const auto fixed = [](double value, int digits = 2) {
        std::ostringstream text;
        if (value < 0) {
            text << "n/a";
        } else {
            text.precision(digits);
            text << std::fixed << value;
        }
        return text.str();
    };
    std::string out =
        "| codec | chunk | encode ms/frame | upload ms/frame | decode ms/frame | seek ms @ boundary | PSNR dB (min) | "
        "B/frame |\n";
    out += "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (const SweepEntry& entry : entries) {
        out += "| " + entry.codec + " | " + std::to_string(entry.chunkFrames) + " | " + fixed(entry.encodeMsPerFrame) +
               " | " + fixed(entry.uploadNsPerFrame) + " | " + fixed(entry.decodeMsPerFrame) + " | " +
               fixed(entry.seekMsAtBoundary) + " | " + fixed(entry.psnrDb) + " | " + fixed(entry.bytesPerFrame, 0) +
               " |\n";
    }
    if (available) {
        out +=
            "\npeak decode resources (process VmHWM after decode-back workload): " + std::to_string(peakDecodeVmHwmKb) +
            " KiB\n";
    }
    return out;
}

}  // namespace nemo::media
