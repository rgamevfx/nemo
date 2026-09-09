#include "nemo/media/CodecSweep.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>

#include "nemo/media/VideoDecode.hpp"

namespace nemo::media {
namespace {
using Clock = std::chrono::steady_clock;
double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class TemporaryChunks {
public:
    TemporaryChunks() {
        std::random_device random;
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                             ("nemo-codec-sweep-" + std::to_string(random()) + "-" + std::to_string(random()));
            // Atomic creation, never adopt (and later delete) an existing directory.
            if (std::filesystem::create_directory(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("codec-sweep: unable to create isolated temporary directory");
    }
    ~TemporaryChunks() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryChunks(const TemporaryChunks&) = delete;
    TemporaryChunks& operator=(const TemporaryChunks&) = delete;
    [[nodiscard]] std::string chunk() const { return (path_ / "chunk.mp4").string(); }

private:
    std::filesystem::path path_;
};

std::optional<uint64_t> peakRss() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmHWM:")) {
            std::istringstream value(line.substr(6));
            uint64_t kib;
            if (value >> kib)
                return kib;
        }
    }
    return std::nullopt;
}

void validateOptions(const SweepOptions& options) {
    if (options.codecs.empty() || std::ranges::any_of(options.codecs, [](const auto& s) { return s.empty(); }))
        throw std::invalid_argument("codec-sweep: codecs must be a nonempty list of nonempty IDs");
    if (options.chunkSizes.empty() || std::ranges::any_of(options.chunkSizes, [](int n) { return n <= 0; }))
        throw std::invalid_argument("codec-sweep: chunk sizes must be positive integers");
    if (options.maxFrames <= 0)
        throw std::invalid_argument("codec-sweep: max-frames must be positive");
    if (options.bitrateKbps <= 0 || options.bitDepth <= 0)
        throw std::invalid_argument("codec-sweep: bitrate-kbps and bit-depth must be positive");
    if (options.width <= 0 || options.height <= 0 || options.width > 8192 || options.height > 8192 ||
        options.width % 2 || options.height % 2)
        throw std::invalid_argument("codec-sweep: width/height must be even and in 2..8192");
    // A bad chunk limit must not turn this harness into an unbounded float history.
    constexpr uint64_t referenceLimit = 512ULL * 1024 * 1024;
    const uint64_t frameBytes = static_cast<uint64_t>(options.width) * options.height * 4 * sizeof(float);
    for (int chunk : options.chunkSizes) {
        if (static_cast<uint64_t>(std::min<int64_t>(chunk, options.maxFrames)) > referenceLimit / frameBytes)
            throw std::invalid_argument(
                "codec-sweep: configured reference chunk bound exceeds 512 MiB; reduce chunk or dimensions");
    }
}

void addStats(EncodeStats& total, const EncodeStats& part) {
    total.codec = part.codec;
    total.profile = part.profile;
    total.initializationMs += part.initializationMs;
    total.allocationPackingMs += part.allocationPackingMs;
    total.conversionMs += part.conversionMs;
    total.gpuConversionMs += part.gpuConversionMs;
    total.hostToDeviceMs += part.hostToDeviceMs;
    total.hostToDeviceBytes += part.hostToDeviceBytes;
    total.deviceToDeviceMs += part.deviceToDeviceMs;
    total.deviceToDeviceBytes += part.deviceToDeviceBytes;
    total.deviceToHostMs += part.deviceToHostMs;
    total.deviceToHostBytes += part.deviceToHostBytes;
    total.stagingBytes = std::max(total.stagingBytes, part.stagingBytes);
    total.submissionDrainMs += part.submissionDrainMs;
    total.muxFinalizationMs += part.muxFinalizationMs;
    total.completeChunkMs += part.completeChunkMs;
    total.coldSetupMs += part.coldSetupMs;
    total.warmSetupMs += part.warmSetupMs;
    total.sessionChunkCount += part.sessionChunkCount;
    total.sessionReuseCount += part.sessionReuseCount;
    total.encodedBytes += part.encodedBytes;
    total.encodedFrames += part.encodedFrames;
    total.sessionReused = total.sessionReused || part.sessionReused;
    if (!part.fallbackReason.empty())
        total.fallbackReason = part.fallbackReason;
}

SweepReport sweep(const std::string& path, std::span<const CpuImage> supplied, const SweepOptions& options) {
    validateOptions(options);
    if (path.empty() && supplied.empty())
        throw std::invalid_argument("codec-sweep: in-memory workload is empty");
    SweepReport report;
    report.width = options.width;
    report.height = options.height;
    report.sourceDescription =
        path.empty() ? "caller-supplied display-referred RGB; caller retention excluded from chunk payload" : path;
    TemporaryChunks storage;
    const auto chunkPath = storage.chunk();
    for (const auto& codec : options.codecs) {
        for (const int chunkSize : options.chunkSizes) {
            SweepEntry entry;
            entry.codec = codec;
            entry.profile = options.profile.empty() ? "auto" : options.profile;
            entry.bitDepth = options.bitDepth;
            entry.bitrateKbps = options.bitrateKbps;
            entry.chunkFrames = chunkSize;
            entry.requestedFrames =
                path.empty() ? std::min<int64_t>(options.maxFrames, supplied.size()) : options.maxFrames;
            SweepMeasurements measured;
            try {
                std::unique_ptr<ViewerReferenceDecoder> reader;
                const auto preparationStart = Clock::now();
                if (!path.empty()) {
                    reader = std::make_unique<ViewerReferenceDecoder>(path, options.width, options.height);
                    const auto& info = reader->info();
                    report.sourceDescription = path + " (" + std::to_string(info.width) + "x" +
                                               std::to_string(info.height) + ", " + std::to_string(info.frameRate) +
                                               " fps source)";
                    if (std::abs(info.frameRate - 24.0) > 0.001)
                        throw std::runtime_error(
                            "reference source must be 24 fps; temporal resampling is not implemented");
                    if (info.frameCount > 0)
                        entry.requestedFrames = std::min(options.maxFrames, info.frameCount);
                }
                measured.preparationMs = elapsedMs(preparationStart);
                double squaredError = 0.0;
                uint64_t samples = 0;
                double seekTotal = 0.0;
                double subsequentTotal = 0.0;
                for (int64_t start = 0; start < entry.requestedFrames;) {
                    const auto prepare = Clock::now();
                    auto count = static_cast<size_t>(std::min<int64_t>(chunkSize, entry.requestedFrames - start));
                    std::vector<CpuImage> owned;
                    std::span<const CpuImage> frames;
                    if (reader) {
                        owned.reserve(count);
                        for (size_t index = 0; index < count; ++index) {
                            auto frame = reader->next();
                            if (!frame) {
                                if (reader->info().frameCount > 0)
                                    throw std::runtime_error("incomplete source decode: expected " +
                                                             std::to_string(entry.requestedFrames) +
                                                             " frames, reached EOF at " +
                                                             std::to_string(start + static_cast<int64_t>(index)));
                                // With no declared count, clean EOF defines the
                                // selected workload; decoder/demux errors still throw.
                                entry.requestedFrames = start + static_cast<int64_t>(index);
                                break;
                            }
                            owned.push_back(std::move(*frame));
                        }
                        if (owned.empty()) {
                            measured.preparationMs += elapsedMs(prepare);
                            break;
                        }
                        count = owned.size();
                        frames = owned;
                    } else {
                        frames = supplied.subspan(static_cast<size_t>(start), count);
                    }
                    measured.preparationMs += elapsedMs(prepare);
                    for (const auto& frame : frames) {
                        if (frame.width() != options.width || frame.height() != options.height)
                            throw std::runtime_error("reference frame dimensions do not match sweep dimensions");
                    }
                    const uint64_t frameBytes =
                        static_cast<uint64_t>(options.width) * options.height * 4 * sizeof(float);
                    entry.retainedReferenceBytes = std::max(entry.retainedReferenceBytes, count * frameBytes);
                    EncodeOptions encodeOptions;
                    encodeOptions.codec = codec;
                    encodeOptions.gopSize = chunkSize;
                    encodeOptions.bitrateKbps = options.bitrateKbps;
                    encodeOptions.profile = options.profile;
                    encodeOptions.bitDepth = options.bitDepth;
                    encodeOptions.injectedFailure = options.injectedFailure;
                    const auto stats = encodeViewerChunk(chunkPath, frames, encodeOptions);
                    entry.profile = stats.profile;
                    const auto decoded = compareViewerChunk(chunkPath, frames);
                    entry.retainedDecodeBytes = frameBytes;
                    if (stats.encodedFrames != static_cast<int>(count))
                        throw std::runtime_error("encoder returned incomplete frame count");
                    if (entry.verifiedChunks > 0) {
                        const auto seekStart = Clock::now();
                        ViewerReferenceDecoder boundary(chunkPath);
                        auto first = boundary.next();
                        if (!first || first->width() != options.width || first->height() != options.height)
                            throw std::runtime_error("boundary decode failed or has wrong dimensions");
                        seekTotal += elapsedMs(seekStart);
                        subsequentTotal += stats.completeChunkMs;
                    } else {
                        measured.firstChunkMs = stats.completeChunkMs;
                    }
                    // Credit only fully finalized, independently decoded chunks.
                    measured.containerBytes += std::filesystem::file_size(chunkPath);
                    addStats(measured.encode, stats);
                    measured.decodeMs += decoded.decodeMs;
                    squaredError += decoded.squaredError;
                    samples += decoded.samples;
                    measured.maxAbsoluteError = std::max(measured.maxAbsoluteError, decoded.maxAbsoluteError);
                    entry.verifiedFrames += static_cast<int64_t>(count);
                    ++entry.verifiedChunks;
                    start += static_cast<int64_t>(count);
                }
                if (entry.verifiedFrames == 0)
                    throw std::runtime_error("codec-sweep: source contains no decodable frames");
                if (entry.verifiedChunks > 1) {
                    measured.seekMsAtBoundary = seekTotal / (entry.verifiedChunks - 1);
                    measured.subsequentChunkMs = subsequentTotal / (entry.verifiedChunks - 1);
                }
                measured.psnrDb = squaredError == 0.0 ? std::numeric_limits<double>::infinity()
                                                      : 10.0 * std::log10(static_cast<double>(samples) / squaredError);
                entry.measurements = measured;
            } catch (const std::exception& error) {
                entry.unavailableReason = error.what();
            }
            entry.processPeakRssKiB = peakRss();
            report.entries.push_back(std::move(entry));
        }
    }
    return report;
}
}  // namespace

ChunkDecodeStats compareViewerChunk(const std::string& path, std::span<const CpuImage> reference) {
    if (reference.empty())
        throw std::invalid_argument("codec-sweep: cannot compare empty reference");
    ChunkDecodeStats stats;
    auto start = Clock::now();
    ViewerReferenceDecoder reader(path);
    stats.decodeMs = elapsedMs(start);
    size_t index = 0;
    while (true) {
        start = Clock::now();
        auto frame = reader.next();
        stats.decodeMs += elapsedMs(start);
        if (!frame)
            break;
        if (index >= reference.size())
            throw std::runtime_error("codec-sweep: decoded more frames than reference");
        const auto& expected = reference[index++];
        if (frame->width() != expected.width() || frame->height() != expected.height())
            throw std::runtime_error("codec-sweep: decoded dimensions differ from reference");
        if (expected.layout().color != ColorInterpretation::DisplayReferred)
            throw std::invalid_argument("codec-sweep: fidelity reference must be display-referred");
        for (int y = 0; y < frame->height(); ++y) {
            for (int x = 0; x < frame->width(); ++x) {
                const auto actual = frame->pixel(x, y);
                const auto desired = expected.pixel(x, y);
                for (int channel = 0; channel < 3; ++channel) {
                    const double error = static_cast<double>(actual[channel]) - desired[channel];
                    if (!std::isfinite(error))
                        throw std::runtime_error("codec-sweep: non-finite fidelity sample");
                    stats.squaredError += error * error;
                    stats.maxAbsoluteError = std::max(stats.maxAbsoluteError, std::abs(error));
                    ++stats.samples;
                }
            }
        }
    }
    if (index != reference.size())
        throw std::runtime_error("codec-sweep: incomplete chunk decode: expected " + std::to_string(reference.size()) +
                                 ", decoded " + std::to_string(index));
    return stats;
}

SweepReport runCodecSweep(const std::string& sourcePath, const SweepOptions& options) {
    if (sourcePath.empty())
        throw std::invalid_argument("codec-sweep: source path is empty");
    return sweep(sourcePath, {}, options);
}
SweepReport runCodecSweep(std::span<const CpuImage> reference, const SweepOptions& options) {
    return sweep({}, reference, options);
}

std::string SweepReport::table() const {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "source: " << sourceDescription << "\nreference: " << width << "x" << height
        << " display-referred Rec.709 RGBA32F, 24 fps; nearest source luma pixel center, bilinear left-sited chroma; "
           "no linearization/view transform. Full-image RGB PSNR (peak 1, no clipping, alpha excluded) and max "
           "absolute error.\n"
           "Each chunk: fresh encoder/device/pool and closed independent MP4. First chunk is first-use in this "
           "candidate, "
           "NOT guaranteed process-cold. Subsequent chunks reuse no encoder resources; OS/library caches uncontrolled. "
           "Warm encoder/device reuse: n/a (not implemented). Source decoder reused within candidate.\n"
           "Stages in ms/frame, denominator = verified frames: init includes codec/device/pool setup; alloc/pack = "
           "frame buffers and host packing; RGB/YUV = CPU conversion; H2D API = av_hwframe_transfer_data submission "
           "latency only (asynchronous CUDA copies). Isolated DMA completion time: n/a (no completion timer); "
           "send/drain may include upload dependency waits, excludes mux; mux includes header, packet writes, trailer "
           "and close. Complete includes setup "
           "through "
           "closed readable output and bookkeeping, excludes source preparation and decode verification. "
           "Complete fps = frames / complete seconds, not integrated cache throughput.\n"
           "Transfer bytes: host->device CUDA copy extents (min host/device pitch x plane rows), including copied "
           "padding; CPU rows have no transfer. "
           "No device->host transfer measured. Rate control: target ABR; codec defaults otherwise, "
           "CPU threads=2 (x265 pools=2, frame-threads=2); bitrate is a target, not a payload guarantee.\n\n"
           "| codec | profile | bits | kbps | chunk | verified/requested | init ms/f | alloc/pack ms/f | RGB/YUV ms/f "
           "| H2D API ms/f | H2D B | send/drain ms/f | mux ms/f | complete ms/f | complete fps | first chunk ms | "
           "later "
           "chunk mean ms | prepare ms/f | decode ms/f | boundary ms | PSNR dB | max error | container B/f |\n"
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& entry : entries) {
        out << "| " << entry.codec << " | " << entry.profile << " | " << entry.bitDepth << " | " << entry.bitrateKbps
            << " | " << entry.chunkFrames << " | " << entry.verifiedFrames << '/' << entry.requestedFrames;
        if (!entry.measurements) {
            for (int column = 0; column < 17; ++column)
                out << " | n/a";
            out << " |\n";
        } else {
            const auto& m = *entry.measurements;
            const double frames = static_cast<double>(entry.verifiedFrames);
            const auto& s = m.encode;
            out << " | " << s.initializationMs / frames << " | " << s.allocationPackingMs / frames << " | "
                << s.conversionMs / frames << " | ";
            if (s.hostToDeviceBytes)
                out << s.hostToDeviceMs / frames;
            else
                out << "n/a";
            out << " | " << s.hostToDeviceBytes << " | " << s.submissionDrainMs / frames << " | "
                << s.muxFinalizationMs / frames << " | " << s.completeChunkMs / frames << " | "
                << frames * 1000.0 / s.completeChunkMs << " | " << m.firstChunkMs << " | ";
            if (m.subsequentChunkMs)
                out << *m.subsequentChunkMs;
            else
                out << "n/a";
            out << " | " << m.preparationMs / frames << " | " << m.decodeMs / frames << " | ";
            if (m.seekMsAtBoundary)
                out << *m.seekMsAtBoundary;
            else
                out << "n/a";
            out << " | " << m.psnrDb << " | " << m.maxAbsoluteError << " | " << m.containerBytes / frames << " |\n";
        }
    }
    out << "\nMemory scopes: reference/replay payloads are host allocations (not process RSS). No full-resolution "
           "source "
           "RGBA32F history is retained. Source/replay codec surfaces, internal buffers, CUDA pool/encoder VRAM: "
           "n/a (not instrumented). Process VmHWM is cumulative across candidates, includes libraries, allocator "
           "retention "
           "and codec/reference buffers; it is NOT isolated decoder memory.\n";
    for (const auto& entry : entries) {
        out << "- " << entry.codec << '/' << entry.chunkFrames << ": reference peak payload "
            << entry.retainedReferenceBytes << " B; verified replay payload ";
        if (entry.retainedDecodeBytes)
            out << *entry.retainedDecodeBytes << " B";
        else
            out << "n/a (no successful decode)";
        out << "; process peak RSS ";
        if (entry.processPeakRssKiB)
            out << *entry.processPeakRssKiB << " KiB";
        else
            out << "n/a (VmHWM unavailable)";
        if (!entry.unavailableReason.empty())
            out << "; unavailable: " << entry.unavailableReason;
        out << '\n';
    }
    out << "\nFidelity is relative to the sampled reference, not the 4K source; it includes RGB/YUV quantization, "
           "4:2:0 subsampling/reconstruction and compression, not resolution-approximation error. "
           "Similar aggregate PSNR does not establish codec neutrality. Diagnostic workload only unless explicitly "
           "3840x2160 source -> 1920x1080 / 200 frames; neither proves #11 integration or #16 visible "
           "latency/defaults.\n";
    return out.str();
}
}  // namespace nemo::media
