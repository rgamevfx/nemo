#include "ViewerCacheCommand.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#ifdef NEMO_BUILD_GPU
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/eval/Viewer.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#endif

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct ParameterEdit {
    std::string node;
    std::string key;
    std::string value;
    [[nodiscard]] bool requested() const { return !node.empty() || !key.empty() || !value.empty(); }
};

struct CacheCommandOptions {
    std::filesystem::path project;
    std::filesystem::path cacheDirectory;
    std::vector<std::int64_t> requestedFrames;
    std::string outputName;
    std::string replayOrder = "forward";
    std::string shaderDirectory;
    std::string codec = "h264-nvenc";
    int width = 1920;
    int height = 1080;
    int scale = 1;
    int chunkFrames = 12;
    int bitrateKbps = 8000;
    nemo::NetworkId network{nemo::kInvalidNetwork};
    bool staleSupersede = false;
    bool fidelity = false;
    std::string viewAfter;
    ParameterEdit edit;
};

[[nodiscard]] std::int64_t parseSigned(const std::string& text, const std::string& option) {
    if (text.empty() || text.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument(option + ": expected a decimal integer without whitespace");
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument(option + ": invalid integer '" + text + "'");
    return value;
}

[[nodiscard]] nemo::NetworkId parseNetworkId(const std::string& text, const std::string& option) {
    if (text.empty() || text.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument(option + ": expected a nonzero decimal integer without whitespace");
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == nemo::kInvalidNetwork)
        throw std::invalid_argument(option + ": expected a nonzero network id");
    return static_cast<nemo::NetworkId>(value);
}

[[nodiscard]] int parsePositiveInt(const std::string& text, const std::string& option,
                                   int maximum = std::numeric_limits<int>::max()) {
    const std::int64_t value = parseSigned(text, option);
    if (value <= 0 || value > maximum)
        throw std::invalid_argument(option + ": expected an integer in 1.." + std::to_string(maximum));
    return static_cast<int>(value);
}

[[nodiscard]] std::vector<std::int64_t> parseFrames(const std::string& text) {
    if (text.empty())
        throw std::invalid_argument("--frames: expected a nonempty comma-separated list");
    std::vector<std::int64_t> frames;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string token = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (token.empty())
            throw std::invalid_argument("--frames: empty frame in '" + text + "'");
        const std::int64_t frame = parseSigned(token, "--frames");
        if (frame < 0)
            throw std::invalid_argument("--frames: frame numbers must be nonnegative");
        frames.push_back(frame);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return frames;
}

[[nodiscard]] std::vector<std::int64_t> uniqueSortedFrames(const std::vector<std::int64_t>& input) {
    std::vector<std::int64_t> frames = input;
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

void parseOption(CacheCommandOptions& options, const std::string& flag, const std::string& value) {
    if (flag == "--cache-dir") {
        if (value.empty())
            throw std::invalid_argument(flag + ": empty path");
        options.cacheDirectory = value;
    } else if (flag == "--network-id") {
        options.network = parseNetworkId(value, flag);
    } else if (flag == "--frames") {
        options.requestedFrames = parseFrames(value);
    } else if (flag == "--width") {
        options.width = parsePositiveInt(value, flag, 8192);
    } else if (flag == "--height") {
        options.height = parsePositiveInt(value, flag, 8192);
    } else if (flag == "--scale") {
        options.scale = parsePositiveInt(value, flag, 4);
        if (!nemo::isSamplingScale(options.scale))
            throw std::invalid_argument(flag + ": supported values are 1, 2, and 4");
    } else if (flag == "--shaders") {
        if (value.empty())
            throw std::invalid_argument(flag + ": empty path");
        options.shaderDirectory = value;
    } else if (flag == "--codec") {
        if (value.empty() || value.find_first_of(" \t\r\n") != std::string::npos)
            throw std::invalid_argument(flag + ": expected a nonempty codec id without whitespace");
        options.codec = value;
    } else if (flag == "--chunk-frames") {
        options.chunkFrames = parsePositiveInt(value, flag, 4096);
    } else if (flag == "--bitrate-kbps") {
        options.bitrateKbps = parsePositiveInt(value, flag, std::numeric_limits<int>::max());
    } else if (flag == "--view-after") {
        if (value.empty())
            throw std::invalid_argument(flag + ": empty viewing transform");
        options.viewAfter = value;
    } else if (flag == "--edit-node") {
        options.edit.node = value;
    } else if (flag == "--edit-key") {
        options.edit.key = value;
    } else if (flag == "--edit-value") {
        options.edit.value = value;
    } else if (flag == "--replay") {
        if (value != "forward" && value != "reverse" && value != "random")
            throw std::invalid_argument(flag + ": supported values are forward, reverse, and random");
        options.replayOrder = value;
    } else if (flag == "--output") {
        if (value.empty())
            throw std::invalid_argument(flag + ": empty output name");
        options.outputName = value;
    } else {
        throw std::invalid_argument("unknown option " + flag);
    }
}

[[nodiscard]] CacheCommandOptions parseArguments(const std::vector<std::string>& args) {
    if (args.empty())
        throw std::invalid_argument("cache-viewer requires <project.json>");
    CacheCommandOptions options;
    options.project = args.front();
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string& flag = args[index];
        if (flag == "--stale-supersede" || flag == "--fidelity") {
            if (flag == "--stale-supersede")
                options.staleSupersede = true;
            else
                options.fidelity = true;
            continue;
        }
        if (flag.empty() || flag.front() != '-')
            throw std::invalid_argument("unexpected argument '" + flag + "'");
        if (index + 1 >= args.size())
            throw std::invalid_argument(flag + ": missing value");
        parseOption(options, flag, args[++index]);
    }
    if (options.cacheDirectory.empty())
        throw std::invalid_argument("cache-viewer requires --cache-dir PATH");
    if (options.requestedFrames.empty())
        throw std::invalid_argument("cache-viewer requires --frames f1,f2,...");
    if (options.edit.requested() &&
        (options.edit.node.empty() || options.edit.key.empty() || options.edit.value.empty()))
        throw std::invalid_argument("invalidation probe requires --edit-node, --edit-key and --edit-value");
    return options;
}

[[nodiscard]] std::vector<std::int64_t> replaySequence(const CacheCommandOptions& options) {
    std::vector<std::int64_t> sequence = uniqueSortedFrames(options.requestedFrames);
    if (options.replayOrder == "reverse") {
        std::reverse(sequence.begin(), sequence.end());
    } else if (options.replayOrder == "random") {
        // The seed is part of the harness contract: random replay is repeatable
        // evidence, not an unrecorded source of benchmark variance.
        std::mt19937_64 random(0x12c0ffeeULL);
        std::shuffle(sequence.begin(), sequence.end(), random);
    }
    return sequence;
}

[[nodiscard]] double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

#ifdef NEMO_BUILD_GPU

[[nodiscard]] Json encodeStatsJson(const nemo::media::EncodeStats& stats) {
    return Json{{"codec", stats.codec},
                {"profile", stats.profile},
                {"initialization_ms", stats.initializationMs},
                {"allocation_packing_ms", stats.allocationPackingMs},
                {"conversion_ms", stats.conversionMs},
                {"gpu_conversion_ms", stats.gpuConversionMs},
                {"host_to_device_ms", stats.hostToDeviceMs},
                {"host_to_device_bytes", stats.hostToDeviceBytes},
                {"device_to_host_ms", stats.deviceToHostMs},
                {"device_to_host_bytes", stats.deviceToHostBytes},
                {"device_to_device_ms", stats.deviceToDeviceMs},
                {"device_to_device_bytes", stats.deviceToDeviceBytes},
                {"staging_bytes", stats.stagingBytes},
                {"submission_drain_ms", stats.submissionDrainMs},
                {"mux_finalization_ms", stats.muxFinalizationMs},
                {"complete_chunk_ms", stats.completeChunkMs},
                {"cold_setup_ms", stats.coldSetupMs},
                {"warm_setup_ms", stats.warmSetupMs},
                {"session_chunk_count", stats.sessionChunkCount},
                {"session_reuse_count", stats.sessionReuseCount},
                {"encoded_bytes", stats.encodedBytes},
                {"encoded_frames", stats.encodedFrames},
                {"fallback_reason", stats.fallbackReason},
                {"session_reused", stats.sessionReused}};
}

[[nodiscard]] Json cacheCountsJson(const nemo::eval::ViewerCacheCounts& counts) {
    return Json{{"hits", counts.hits},
                {"misses", counts.misses},
                {"published", counts.published},
                {"stale_rejected", counts.staleRejected},
                {"encoded_frames", counts.encodedFrames},
                {"pending_frames", counts.pendingFrames},
                {"peak_pending_frames", counts.peakPendingFrames},
                {"disk_bytes", counts.diskBytes},
                {"admission_rejected", counts.admissionRejected},
                {"admission_dropped", counts.admissionDropped},
                {"active_frames", counts.activeFrames},
                {"encoding_frames", counts.encodingFrames},
                {"compressed_hot_hits", counts.compressedHotHits},
                {"decoded_hot_hits", counts.decodedHotHits},
                {"compressed_hot_bytes", counts.compressedHotBytes},
                {"compressed_hot_chunks", counts.compressedHotChunks},
                {"decoded_hot_frames", counts.decodedHotFrames},
                {"invalid_entries", counts.invalidEntries},
                {"errors", counts.errors},
                {"last_error", counts.lastError},
                {"decoded_frames", counts.decodedFrames},
                {"decoded_queue_peak", counts.decodedQueuePeak},
                {"hardware_decoded_frames", counts.hardwareDecodedFrames},
                {"software_decoded_frames", counts.softwareDecodedFrames},
                {"replay_fallback_reason", counts.replayFallbackReason},
                {"encode", encodeStatsJson(counts.encode)}};
}

[[nodiscard]] nemo::NetworkId selectedNetwork(const nemo::Document& document, const CacheCommandOptions& options) {
    return options.network == nemo::kInvalidNetwork ? document.rootNetworkId() : options.network;
}

[[nodiscard]] nemo::EvaluationRequest makeRequest(const nemo::Document& document, const CacheCommandOptions& options,
                                                  std::int64_t frame) {
    nemo::EvaluationRequest request;
    request.network = selectedNetwork(document, options);
    request.output = nemo::resolveOutput(document, request.network, options.outputName);
    request.localTime = frame;
    request.region = {0, 0, options.width, options.height};
    request.fullWidth = options.width;
    request.fullHeight = options.height;
    request.samplingScale = options.scale;
    request.quality = nemo::Quality::Full;
    request.channels = "RGBA";
    return request;
}

[[nodiscard]] Json latencyJson(const std::vector<double>& samples, const char* name) {
    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    const double fps = total > 0.0 ? static_cast<double>(samples.size()) / (total / 1000.0) : 0.0;
    Json result{{"name", name},
                {"requests", samples.size()},
                {"total_request_to_gpu_ready_ms", total},
                {"average_request_to_gpu_ready_ms", samples.empty() ? 0.0 : total / samples.size()},
                {"ready_fps", fps},
                {"timing_scope", "request through GPU-complete ViewerFrame; not visible-surface latency"}};
    if (!samples.empty()) {
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        result["min_request_to_gpu_ready_ms"] = sorted.front();
        result["max_request_to_gpu_ready_ms"] = sorted.back();
        result["p95_request_to_gpu_ready_ms"] = sorted[(sorted.size() * 95 + 99) / 100 - 1];
    }
    return result;
}

// Explicit diagnostic edits affect only an in-memory project session and the
// last explicitly requested frame. Never rewrites the input project.
[[nodiscard]] Json probeInvalidation(nemo::eval::ViewerSession& session, const nemo::Document& original,
                                     const CacheCommandOptions& options, std::uint64_t& generation) {
    nemo::ProjectSession projectSession(original);
    Json probes = Json::array();
    const auto frameNumber = options.requestedFrames.back();
    const auto probe = [&](const char* kind, nemo::Command command, bool viewOnly) {
        const auto result = projectSession.submit(std::move(command),
                                                  nemo::EditOptions{.expectedRevision = projectSession.revision(),
                                                                    .requestId = std::string{"cache-viewer:"} + kind});
        if (!result.committed)
            throw std::runtime_error(result.error ? result.error->message : "project edit was rejected");
        const nemo::Document document = projectSession.snapshot();
        const auto before = session.cacheCounts();
        const auto reuseBefore = session.reuseCounts();
        const auto request = makeRequest(document, options, frameNumber);
        const auto replacement = session.render(document, request, 10'000'000'000ULL, generation++);
        session.flushCache();
        const auto afterReplacement = session.cacheCounts();
        const auto reuseAfter = session.reuseCounts();
        const auto replay = session.render(document, request, 10'000'000'000ULL, generation++);
        const auto published = afterReplacement.published - before.published;
        const auto evaluations = reuseAfter.misses - reuseBefore.misses;
        const auto reused = reuseAfter.hits - reuseBefore.hits;
        const bool ok = !replacement.cacheHit && replay.cacheHit && published == 1 &&
                        (viewOnly ? evaluations == 0 && reused != 0 : evaluations != 0);
        probes.push_back({{"kind", kind},
                          {"frame", frameNumber},
                          {"replacement_cache_hit", replacement.cacheHit},
                          {"replacement_replay_hit", replay.cacheHit},
                          {"published_frames", published},
                          {"graph_evaluation_misses", evaluations},
                          {"upstream_reuse_hits", reused},
                          {"cache_before", cacheCountsJson(before)},
                          {"cache_after", cacheCountsJson(session.cacheCounts())},
                          {"ok", ok}});
    };
    if (options.edit.requested()) {
        const auto network = selectedNetwork(original, options);
        const auto& graph = original.network(network).graph();
        const auto* node = graph.nodeByName(options.edit.node);
        if (!node)
            throw std::runtime_error("invalidation probe: unknown node '" + options.edit.node + "'");
        const auto* descriptor = graph.descriptor(node->type);
        if (!descriptor)
            throw std::runtime_error("invalidation probe: node type '" + node->type + "' is unavailable");
        const auto typedValue = graph.catalog().parseParameterText(node->type, options.edit.key, options.edit.value);
        probe("upstream_parameter_change", nemo::setParamCommand(network, node->id, options.edit.key, typedValue),
              false);
    }
    return probes;
}

[[nodiscard]] nemo::CpuImage diagnosticReadback(const nemo::eval::ViewerFrame& frame, nemo::gpu::Device& device,
                                                nemo::gpu::Allocator& allocator) {
    if (!frame.image)
        throw std::runtime_error("diagnostic readback: viewer frame has no image");
    if (frame.layout.width <= 0 || frame.layout.height <= 0)
        throw std::runtime_error("diagnostic readback: viewer frame has invalid dimensions");
    nemo::CpuImage image(frame.layout);
    const std::size_t bytes = static_cast<std::size_t>(frame.layout.width) *
                              static_cast<std::size_t>(frame.layout.height) * nemo::kImageChannels * sizeof(float);
    auto& queue = device.submissions(device.graphics_family());
    nemo::gpu::downloadImage(queue, allocator, *frame.image, image.data(), bytes, 10'000'000'000ULL);
    return image;
}

[[nodiscard]] Json fidelityJson(const nemo::CpuImage& source, const nemo::CpuImage& replay) {
    if (source.width() != replay.width() || source.height() != replay.height())
        throw std::runtime_error("fidelity: source and replay dimensions differ");
    double squaredError = 0.0;
    float maxAbsoluteError = 0.0F;
    std::uint64_t samples = 0;
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const auto expected = source.pixel(x, y);
            const auto actual = replay.pixel(x, y);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float error = actual[channel] - expected[channel];
                if (!std::isfinite(error))
                    throw std::runtime_error("fidelity: non-finite decoded sample");
                squaredError += static_cast<double>(error) * static_cast<double>(error);
                maxAbsoluteError = std::max(maxAbsoluteError, std::abs(error));
                ++samples;
            }
        }
    }
    const double rmse = samples == 0 ? 0.0 : std::sqrt(squaredError / static_cast<double>(samples));
    const bool exact = rmse == 0.0;
    const double psnr = exact ? 0.0 : 20.0 * std::log10(1.0 / rmse);
    const Json psnrValue = exact ? Json(std::string{"inf"}) : Json(psnr);
    constexpr double kRmseTolerance = 0.08;
    return Json{
        {"performed", true},
        {"width", source.width()},
        {"height", source.height()},
        {"samples_rgb", samples},
        {"rmse", rmse},
        {"max_absolute_error", maxAbsoluteError},
        {"psnr_db", psnrValue},
        {"rmse_tolerance", kRmseTolerance},
        {"within_declared_tolerance", rmse <= kRmseTolerance},
        {"tolerance_scope", "provisional aggregate RMS diagnostic, not artist quality approval; local chroma-edge loss "
                            "is reported separately"},
        {"readback_scope",
         "two diagnostic full-frame GPU-to-host readbacks (source viewer and one cached replay); never the hot path"}};
}

int runGpuHarness(const CacheCommandOptions& options, Json& report) {
    std::ifstream input(options.project);
    if (!input)
        throw std::runtime_error("cannot open project: " + options.project.string());
    const nemo::LoadResult loaded = nemo::loadDocument(Json::parse(input));
    report["warnings"] = loaded.warnings;

    std::error_code directoryError;
    const bool hadExistingCache = std::filesystem::exists(options.cacheDirectory, directoryError) && !directoryError;
    std::filesystem::create_directories(options.cacheDirectory, directoryError);
    if (directoryError)
        throw std::runtime_error("cannot create cache directory '" + options.cacheDirectory.string() +
                                 "': " + directoryError.message());

    auto instance = nemo::gpu::Instance::create({.validation = true});
    // Device-resident NVENC interop requires the same external-memory device
    // capability used by the native UI runtime; do not silently use a
    // non-exportable device in this harness.
    auto device = nemo::gpu::Device::create(*instance, {.externalSharing = true});
    auto allocator = nemo::gpu::Allocator::create(*instance, *device, {.max_device_bytes = 2ULL << 30});
    const auto& properties = device->properties();
    report["device"] = {{"name", properties.deviceName},
                        {"vendor_id", properties.vendorID},
                        {"device_id", properties.deviceID},
                        {"driver_version", properties.driverVersion},
                        {"api_version", properties.apiVersion},
                        {"external_sharing_requested", true},
                        {"encode_queue_available", device->encode_family().has_value()}};

    std::filesystem::path shaders = options.shaderDirectory;
#ifdef NEMO_SLANG_SPV_DIR
    if (shaders.empty())
        shaders = NEMO_SLANG_SPV_DIR;
#endif
    if (shaders.empty())
        throw std::runtime_error("no Slang shader directory: pass --shaders <spv-dir> or configure NEMO_SLANG_SPV_DIR");

    nemo::eval::ViewerCacheOptions cacheOptions;
    cacheOptions.directory = options.cacheDirectory;
    cacheOptions.encoding.codec = options.codec;
    cacheOptions.encoding.gopSize = options.chunkFrames;
    cacheOptions.encoding.bitrateKbps = options.bitrateKbps;
    cacheOptions.chunkFrames = static_cast<std::size_t>(options.chunkFrames);
    // Keep the live asynchronous queue bounded independently of codec GOP
    // size; a large chunk request must not retain an unbounded float history.
    cacheOptions.maxPendingFrames = 12;
    cacheOptions.maxDecodedFrames = 2;
    const auto uniqueRequested = uniqueSortedFrames(options.requestedFrames);
    report["request"] = {{"network", selectedNetwork(loaded.document, options)},
                         {"frames", options.requestedFrames},
                         {"requested_count", options.requestedFrames.size()},
                         {"unique_requested_count", uniqueRequested.size()},
                         {"full_width", options.width},
                         {"full_height", options.height},
                         {"width", options.width},
                         {"height", options.height},
                         {"scale", options.scale},
                         {"representation_width", nemo::scaledDimension(options.width, options.scale)},
                         {"representation_height", nemo::scaledDimension(options.height, options.scale)},
                         {"codec", options.codec},
                         {"chunk_frames", options.chunkFrames},
                         {"bitrate_kbps", options.bitrateKbps},
                         {"fidelity_diagnostic", options.fidelity},
                         {"cache_directory", std::filesystem::absolute(options.cacheDirectory).string()},
                         {"preexisting_cache_directory", hadExistingCache}};
    int actualRepresentationWidth = 0;
    int actualRepresentationHeight = 0;
    std::vector<double> buildLatencies;
    std::optional<nemo::CpuImage> fidelitySource;
    std::optional<nemo::CpuImage> fidelityReplay;
    const std::int64_t fidelityFrame = uniqueRequested.front();
    std::uint64_t diagnosticReadbacks = 0;
    if (options.fidelity) {
        // Establish the source-view oracle in a standalone session with no
        // cache configured. This prevents a pre-existing disk entry from
        // becoming a self-comparison when --fidelity is requested.
        nemo::eval::ViewerSession sourceSession(*instance, *device, *allocator, shaders);
        const nemo::EvaluationRequest sourceRequest = makeRequest(loaded.document, options, fidelityFrame);
        nemo::eval::ViewerFrame sourceFrame =
            sourceSession.render(loaded.document, sourceRequest, 10'000'000'000ULL, 0);
        fidelitySource = diagnosticReadback(sourceFrame, *device, *allocator);
        ++diagnosticReadbacks;
    }
    const auto buildStart = Clock::now();
    nemo::eval::ViewerCacheCounts buildBefore;
    nemo::eval::ViewerCacheCounts buildAfter;
    std::uint64_t generation = 1;
    double editProbeMs = 0.0;
    {
        nemo::eval::ViewerSession session(*instance, *device, *allocator, shaders);
        session.configureCache(cacheOptions);
        buildBefore = session.cacheCounts();
        if ((!options.viewAfter.empty() || options.edit.requested()) && buildBefore.diskBytes != 0)
            throw std::invalid_argument("invalidation probes require an empty cache directory");
        const std::uint64_t revision = loaded.document.stateRevision();
        for (std::size_t requestIndex = 0; requestIndex < options.requestedFrames.size(); ++requestIndex) {
            const std::int64_t frameNumber = options.requestedFrames[requestIndex];
            const nemo::EvaluationRequest request = makeRequest(loaded.document, options, frameNumber);
            if (options.staleSupersede && generation == 1) {
                // Supersede before enqueueing the first request. Its explicit
                // old generation is therefore deterministically rejected by
                // the asynchronous cache writer, while the live ViewerFrame
                // still returns normally.
                session.supersedeCache(revision, generation + 1);
            }
            const auto renderStart = Clock::now();
            // The returned ViewerFrame owns a completed immutable GPU image. It
            // is intentionally scoped to this iteration: the harness never
            // retains a 200-frame float history.
            nemo::eval::ViewerFrame frame = session.render(loaded.document, request, 10'000'000'000ULL, generation);
            actualRepresentationWidth = frame.layout.width;
            actualRepresentationHeight = frame.layout.height;
            buildLatencies.push_back(elapsedMs(renderStart, Clock::now()));
            if (options.staleSupersede && generation == 1) {
                // Retry the same requested representation under the current
                // generation. This is replacement of one requested frame,
                // never speculative range filling.
                ++generation;
                const auto retryStart = Clock::now();
                [[maybe_unused]] nemo::eval::ViewerFrame retry =
                    session.render(loaded.document, request, 10'000'000'000ULL, generation);
                buildLatencies.push_back(elapsedMs(retryStart, Clock::now()));
            }
            const auto backlog = session.cacheCounts();
            if (backlog.pendingFrames + backlog.activeFrames >= cacheOptions.maxPendingFrames ||
                ((requestIndex + 1) % static_cast<std::size_t>(options.chunkFrames) == 0 &&
                 backlog.pendingFrames != 0)) {
                // Explicit range caching may backpressure at a chunk
                // boundary. This keeps the queue bounded without dropping
                // requested outputs; the elapsed drain remains in buildMs.
                session.flushCache();
            }
            ++generation;
        }
        // Encoding is asynchronous; this is the sole explicit drain in the
        // build phase and happens before the independent replay session opens.
        session.flushCache();
        buildAfter = session.cacheCounts();
        if (!options.viewAfter.empty() || options.edit.requested()) {
            const auto probeStart = Clock::now();
            report["invalidation_probes"] = probeInvalidation(session, loaded.document, options, generation);
            editProbeMs = elapsedMs(probeStart, Clock::now());
            for (const auto& probe : report["invalidation_probes"])
                if (!probe.at("ok").get<bool>())
                    report["errors"].push_back("invalidation probe failed: " + probe.at("kind").get<std::string>());
        }
    }
    report["request"]["actual_representation_width"] = actualRepresentationWidth;
    report["request"]["actual_representation_height"] = actualRepresentationHeight;
    const double buildMs = elapsedMs(buildStart, Clock::now()) - editProbeMs;
    const auto delta = [](std::uint64_t after, std::uint64_t before) {
        return after >= before ? after - before : 0ULL;
    };
    const std::uint64_t buildEncoded = delta(buildAfter.encodedFrames, buildBefore.encodedFrames);
    const std::uint64_t buildPublished = delta(buildAfter.published, buildBefore.published);
    const std::uint64_t buildHits = delta(buildAfter.hits, buildBefore.hits);
    const double reusableFps = buildMs > 0.0 ? static_cast<double>(buildPublished) / (buildMs / 1000.0) : 0.0;

    report["cold_build"] = {
        {"duration_ms", buildMs},
        {"cache_counts", cacheCountsJson(buildAfter)},
        {"latency", latencyJson(buildLatencies, "cold_build")},
        {"reusable_fps", reusableFps},
        {"reusable_fps_scope", "finalized published viewer frames divided by complete cache-build duration"},
        {"cache_build_duration_scope", "explicit requested renders plus asynchronous chunk finalization"}};
    report["transfer_costs"] = encodeStatsJson(buildAfter.encode);
    report["transfer_costs"]["scope"] =
        "encoder conversion, device bridge, codec and mux; excludes source decode, graph evaluation, "
        "presentation and separately reported diagnostic readbacks";
    report["backlog"] = {{"pending_frames_after_flush", buildAfter.pendingFrames},
                         {"active_frames_after_flush", buildAfter.activeFrames},
                         {"peak_pending_frames", buildAfter.peakPendingFrames},
                         {"max_pending_frames", cacheOptions.maxPendingFrames},
                         {"admission_rejected", buildAfter.admissionRejected},
                         {"admission_dropped", buildAfter.admissionDropped},
                         {"disk_bytes", buildAfter.diskBytes}};

    const std::vector<std::int64_t> replayFrames = replaySequence(options);
    std::vector<double> replayLatencies;
    double replayDiagnosticMs = 0.0;
    nemo::eval::ViewerCacheCounts replayBefore;
    nemo::eval::ViewerCacheCounts replayAfter;
    nemo::CacheCounts replayReuseBefore;
    nemo::CacheCounts replayReuseAfter;
    const auto replayStart = Clock::now();
    {
        nemo::eval::ViewerSession replay(*instance, *device, *allocator, shaders);
        replay.configureCache(cacheOptions);
        replayBefore = replay.cacheCounts();
        replayReuseBefore = replay.reuseCounts();
        for (const std::int64_t frameNumber : replayFrames) {
            const nemo::EvaluationRequest request = makeRequest(loaded.document, options, frameNumber);
            const auto renderStart = Clock::now();
            nemo::eval::ViewerFrame frame = replay.render(loaded.document, request, 10'000'000'000ULL, generation++);
            replayLatencies.push_back(elapsedMs(renderStart, Clock::now()));
            if (options.fidelity && !fidelityReplay && frameNumber == fidelityFrame) {
                const auto diagnosticStart = Clock::now();
                fidelityReplay = diagnosticReadback(frame, *device, *allocator);
                ++diagnosticReadbacks;
                replayDiagnosticMs += elapsedMs(diagnosticStart, Clock::now());
            }
        }
        replayAfter = replay.cacheCounts();
        replayReuseAfter = replay.reuseCounts();
    }
    const double replayMs = elapsedMs(replayStart, Clock::now()) - replayDiagnosticMs;
    const std::uint64_t buildMisses = delta(buildAfter.misses, buildBefore.misses);
    const std::uint64_t replayHits = delta(replayAfter.hits, replayBefore.hits);
    const std::uint64_t replayMisses = delta(replayAfter.misses, replayBefore.misses);
    const std::uint64_t replayGraphMisses = delta(replayReuseAfter.misses, replayReuseBefore.misses);
    const std::uint64_t buildResolved = buildPublished + buildHits;
    const bool allReplayHit = replayHits >= replayFrames.size() && replayMisses == 0;
    const bool noGraphReevaluation = replayGraphMisses == 0;
    const bool requestedOnly = buildPublished <= uniqueRequested.size();
    const bool allRequestedResolved = buildResolved >= uniqueRequested.size();
    const bool boundedPending = buildAfter.pendingFrames == 0 && buildAfter.activeFrames == 0 &&
                                buildAfter.peakPendingFrames <= cacheOptions.maxPendingFrames;

    report["warm_replay"] = {{"order", options.replayOrder},
                             {"frames", replayFrames},
                             {"duration_ms", replayMs},
                             {"cache_counts", cacheCountsJson(replayAfter)},
                             {"latency", latencyJson(replayLatencies, "warm_replay")},
                             {"independent_session", true},
                             {"timing_scope", "request through GPU-complete ViewerFrame; not visible-surface latency"}};
    report["assertions"]["requested_only"] = requestedOnly;
    report["assertions"]["requested_only_detail"] = "published viewer outputs are bounded by unique explicit requests; "
                                                    "dependencies are not counted as viewer frames";
    report["assertions"]["all_requested_resolved"] = allRequestedResolved;
    report["assertions"]["replay_all_cache_hits"] = allReplayHit;
    report["assertions"]["replay_graph_not_reevaluated"] = noGraphReevaluation;
    report["assertions"]["bounded_pending_queue"] = boundedPending;
    report["assertions"]["visible_latency_measured"] = false;
    report["assertions"]["visible_latency_note"] = "This headless command measures request-to-GPU-ready only; the UI "
                                                   "presentation surface owns visible-latency instrumentation.";
    report["build_resolution"] = {{"cache_hit_delta", buildHits},
                                  {"cache_miss_delta", buildMisses},
                                  {"encoded_frame_delta", buildEncoded},
                                  {"published_frame_delta", buildPublished},
                                  {"resolved_request_delta", buildResolved},
                                  {"requested_unique_frames", uniqueRequested.size()},
                                  {"preexisting_entries_accounted", true}};
    report["replay_evidence"] = {{"cache_hit_delta", replayHits},
                                 {"cache_miss_delta", replayMisses},
                                 {"graph_reuse_miss_delta", replayGraphMisses},
                                 {"build_encoded_frame_delta", buildEncoded},
                                 {"build_published_frame_delta", buildPublished},
                                 {"requested_unique_frames", uniqueRequested.size()},
                                 {"replay_frames", replayFrames.size()}};
    if (options.fidelity) {
        if (!fidelitySource || !fidelityReplay) {
            report["fidelity"] = {{"performed", false},
                                  {"error", "diagnostic source or replay readback was unavailable"},
                                  {"readbacks", diagnosticReadbacks}};
            report["errors"].push_back("fidelity diagnostic requested but source/replay readback was unavailable");
        } else {
            report["fidelity"] = fidelityJson(*fidelitySource, *fidelityReplay);
            report["fidelity"]["readbacks"] = diagnosticReadbacks;
            report["fidelity"]["replay_readback_ms"] = replayDiagnosticMs;
            report["fidelity"]["device_to_host_bytes"] = diagnosticReadbacks *
                                                         static_cast<std::uint64_t>(fidelitySource->width()) *
                                                         fidelitySource->height() * 4 * sizeof(float);
            if (!report["fidelity"]["within_declared_tolerance"].get<bool>())
                report["errors"].push_back("fidelity diagnostic exceeded its declared 4:2:0 tolerance");
        }
    } else {
        report["fidelity"] = {{"performed", false},
                              {"readbacks", 0},
                              {"note", "Pass --fidelity for one bounded source-view and cached-replay diagnostic "
                                       "comparison; no routine full-frame CPU readback is performed."}};
    }
    report["stale_supersede"] = {
        {"requested", options.staleSupersede},
        {"rejected_delta", delta(buildAfter.staleRejected, buildBefore.staleRejected)},
        {"observation", options.staleSupersede
                            ? "A superseded generation was issued before retrying the same requested frame."
                            : "Not requested; no supersede was injected."}};
    if (!requestedOnly)
        report["errors"].push_back("viewer cache published more frames than explicitly requested");
    if (!allRequestedResolved)
        report["errors"].push_back(
            "not every explicitly requested frame resolved as a cache hit or finalized published frame");
    if (!allReplayHit)
        report["errors"].push_back(
            "independent replay did not resolve every requested frame from finalized cache chunks");
    if (!noGraphReevaluation)
        report["errors"].push_back("independent replay re-evaluated graph work for a cached frame");
    if (!boundedPending)
        report["errors"].push_back("cache pending/active-frame bound was exceeded or did not drain at flush");
    if (buildAfter.admissionRejected != 0 || buildAfter.admissionDropped != 0)
        report["errors"].push_back("viewer cache rejected or dropped requested work at a cache admission bound");
    if (buildAfter.errors != 0 || replayAfter.errors != 0)
        report["errors"].push_back("viewer cache reported an asynchronous cache error: " +
                                   (buildAfter.lastError.empty() ? replayAfter.lastError : buildAfter.lastError));
    if (options.staleSupersede && delta(buildAfter.staleRejected, buildBefore.staleRejected) == 0)
        report["errors"].push_back("requested stale-supersede scenario produced no stale-write rejection");
    report["vulkan_validation"] = Json::array();
    for (const auto& message : instance->take_debug_messages()) {
        if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            report["vulkan_validation"].push_back(message.text);
            report["errors"].push_back("Vulkan validation: " + message.text);
        }
    }

    return report["errors"].empty() ? 0 : 1;
}

#endif  // NEMO_BUILD_GPU

}  // namespace

int commandViewerCache(const std::vector<std::string>& args) {
    Json report{{"ok", false}, {"command", "cache-viewer"}, {"errors", Json::array()}};
    try {
        const CacheCommandOptions options = parseArguments(args);
#ifdef NEMO_BUILD_GPU
        const int result = runGpuHarness(options, report);
        report["ok"] = result == 0;
        std::cout << report.dump(2) << '\n';
        return result;
#else
        report["errors"].push_back("cache-viewer requires a GPU-enabled nemo-cli build");
        std::cout << report.dump(2) << '\n';
        return 1;
#endif
    } catch (const std::exception& error) {
        report["errors"].push_back(error.what());
        std::cout << report.dump(2) << '\n';
        return 2;
    }
}
