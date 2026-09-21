#include "ViewerCacheCommand.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#ifdef NEMO_BUILD_GPU
#include <vulkan/vulkan.h>

#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/eval/Viewer.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Bc7.hpp"
#include "nemo/gpu/Compile.hpp"
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
    int width = 1920;
    int height = 1080;
    int scale = 1;
    nemo::NetworkId network{nemo::kInvalidNetwork};
    bool staleSupersede = false;
    bool fidelity = false;
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

// The single BC7 cache representation (issue #106): independent compressed
// frames, no video chunk/codec/decode state. Counts that describe a codec or a
// chunk no longer exist, and the CLI reports exactly the cache's own names.
[[nodiscard]] Json cacheCountsJson(const nemo::eval::ViewerCacheCounts& counts) {
    return Json{{"hits", counts.hits},
                {"misses", counts.misses},
                {"published", counts.published},
                {"stale_rejected", counts.staleRejected},
                {"encoded_frames", counts.encodedFrames},
                {"pending_frames", counts.pendingFrames},
                {"peak_pending_frames", counts.peakPendingFrames},
                {"pending_bytes", counts.pendingBytes},
                {"disk_bytes", counts.diskBytes},
                {"admission_rejected", counts.admissionRejected},
                {"admission_dropped", counts.admissionDropped},
                {"invalid_entries", counts.invalidEntries},
                {"active_frames", counts.activeFrames},
                {"encoding_frames", counts.encodingFrames},
                {"compressed_hot_hits", counts.compressedHotHits},
                {"compressed_hot_bytes", counts.compressedHotBytes},
                {"resident_bytes", counts.residentBytes},
                {"resident_frames", counts.residentFrames},
                {"uploaded_frames", counts.uploadedFrames},
                {"loading_frames", counts.loadingFrames},
                {"evicted_frames", counts.evictedFrames},
                {"errors", counts.errors},
                {"last_error", counts.lastError}};
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

// A retained frame whose compressed blocks are still loading is NOT a miss: the
// harness retries the same request until the preparation is ready, and never
// falls back to a graph render for it. This is the synchronous consumer the
// ViewerReplayPending contract exists for, used by both the build phase (a
// fresh-process reopen can find a Loading entry) and the replay phase.
template <typename Request>
[[nodiscard]] nemo::eval::ViewerFrame retryPending(Request&& request, std::uint64_t& retries) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        try {
            return request();
        } catch (const nemo::eval::ViewerReplayPending& pending) {
            if (Clock::now() >= deadline) {
                throw std::runtime_error("cache-viewer: retained representation '" + pending.identity +
                                         "' did not become ready: " + pending.what());
            }
            ++retries;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

// Explicit diagnostic edits affect only an in-memory project session and the
// last explicitly requested frame. Never rewrites the input project.
[[nodiscard]] Json probeInvalidation(nemo::eval::ViewerSession& session, const nemo::Document& original,
                                     const CacheCommandOptions& options, std::uint64_t& generation,
                                     const std::shared_ptr<const nemo::NodeContributions>& contributions) {
    // The probe session edits a copy of the document; it is composed with the
    // same inventory the document was read against, so a package node edit is
    // validated exactly as it would be in the live session.
    nemo::ProjectSession projectSession(original, 256, contributions);
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
        std::uint64_t retries = 0;
        const auto replayGeneration = generation++;
        const auto replay = retryPending(
            [&] { return session.render(document, request, 10'000'000'000ULL, replayGeneration); }, retries);
        const auto published = afterReplacement.published - before.published;
        const auto evaluations = reuseAfter.misses - reuseBefore.misses;
        const auto reused = reuseAfter.hits - reuseBefore.hits;
        const bool ok = !replacement.cacheHit && replay.cacheHit && published == 1 &&
                        (viewOnly ? evaluations == 0 && reused != 0 : evaluations != 0);
        probes.push_back({{"kind", kind},
                          {"frame", frameNumber},
                          {"replacement_cache_hit", replacement.cacheHit},
                          {"replacement_replay_hit", replay.cacheHit},
                          {"replay_is_bc7", replay.replay != nullptr},
                          {"replay_retries", retries},
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

// Diagnostic-only sample of one completed display-referred frame through the
// production presentation module on the SAME device, into a host-readable
// RGBA32F image. The module quantizes to 8-bit display levels and writes them
// as floats, so both the live reference and the BC7 replay are measured after
// the identical transform: the difference between them is BC7 loss alone.
//
// `blockCompressed` selects the BC7 module, whose binding 0 is a combined image
// sampler over the compressed texture (NEAREST at texel centres); the live
// module reads the float image as a storage image. Neither pass host-waits the
// producer queue: this helper waits only its own diagnostic submission.
[[nodiscard]] nemo::gpu::Image samplePresentation(nemo::gpu::Device& device, nemo::gpu::Allocator& allocator,
                                                  const nemo::gpu::Image& source, bool blockCompressed,
                                                  const std::vector<std::uint32_t>& spirv, std::uint64_t timeout_ns) {
    const VkExtent3D extent = source.extent();
    if (extent.width == 0 || extent.height == 0)
        throw std::runtime_error("diagnostic sample: source image has invalid dimensions");
    auto output = allocator.create_image(extent.width, extent.height, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 2);
    // std140 uniform block, vec4-aligned; 0 is ViewerChannel::RGBA (the
    // composite, no display isolation).
    constexpr VkDeviceSize kChannelBytes = 16;
    auto channelBuffer = allocator.create_buffer(kChannelBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                 nemo::gpu::MemoryPreference::HostMapped);
    std::memset(channelBuffer.mapped(), 0, static_cast<std::size_t>(kChannelBytes));
    auto pass = nemo::gpu::ComputePass::create(
        device, spirv,
        {{0, 0,
          blockCompressed ? nemo::gpu::DescriptorKind::CombinedImageSampler : nemo::gpu::DescriptorKind::StorageImage,
          nullptr, &source, blockCompressed},
         {0, 1, nemo::gpu::DescriptorKind::StorageImage, nullptr, &output},
         {0, 2, nemo::gpu::DescriptorKind::UniformBuffer, &channelBuffer}});
    auto& queue = device.submissions(device.graphics_family());
    const VkImageLayout sourceLayout =
        blockCompressed ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    const VkAccessFlags sourceAccess =
        blockCompressed ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_MEMORY_WRITE_BIT;
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            // The compressed texture is read where encode/upload left it; the
            // float display image keeps its own GENERAL convention.
            nemo::gpu::recordImageBarrier(command, source, sourceLayout, sourceLayout,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, sourceAccess,
                                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            nemo::gpu::recordImageBarrier(command, output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                          VK_ACCESS_SHADER_WRITE_BIT);
            pass->record(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        },
        {pass->retain()}, {}, timeout_ns);
    if (!completion)
        throw std::runtime_error("diagnostic sample: submission capacity unavailable");
    if (!queue.wait(*completion, timeout_ns))
        throw std::runtime_error("diagnostic sample: presentation pass did not complete");
    return output;
}

[[nodiscard]] nemo::CpuImage diagnosticReadback(const nemo::eval::ViewerFrame& frame, nemo::gpu::Device& device,
                                                nemo::gpu::Allocator& allocator,
                                                const std::vector<std::uint32_t>& presentationSpirv,
                                                const std::vector<std::uint32_t>& bc7Spirv, std::uint64_t timeout_ns) {
    const nemo::gpu::Image* source = nullptr;
    bool blockCompressed = false;
    if (frame.replay) {
        source = &frame.replay->image;
        blockCompressed = true;
    } else if (frame.image) {
        source = frame.image.get();
    } else {
        throw std::runtime_error("diagnostic readback: viewer frame carries neither a live nor a replay image");
    }
    const nemo::gpu::Image sampled = samplePresentation(device, allocator, *source, blockCompressed,
                                                        blockCompressed ? bc7Spirv : presentationSpirv, timeout_ns);
    nemo::CpuImage image(nemo::ImageLayout{.width = static_cast<int>(sampled.extent().width),
                                           .height = static_cast<int>(sampled.extent().height),
                                           .channels = {"R", "G", "B", "A"},
                                           .color = nemo::ColorInterpretation::DisplayReferred});
    const std::size_t bytes = static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height()) *
                              nemo::kImageChannels * sizeof(float);
    nemo::gpu::downloadImage(device.submissions(device.graphics_family()), allocator, sampled, image.data(), bytes,
                             timeout_ns);
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
                    throw std::runtime_error("fidelity: non-finite sampled value");
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
    constexpr double kRmseTolerance = 0.02;
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
        {"representation", "bc7-unorm-4x4"},
        {"tolerance_scope", "BC7-loss tolerance on the sampled display levels; the live reference and the cached "
                            "replay go through the same production presentation module, so the measured difference "
                            "is the compressed representation's loss alone"},
        {"readback_scope",
         "two diagnostic GPU-to-host readbacks (one live reference sample and one sampled BC7 replay frame); never "
         "the hot path"}};
}

int runGpuHarness(const CacheCommandOptions& options, Json& report,
                  const std::shared_ptr<const nemo::NodeContributions>& contributions,
                  const std::vector<nemo::eval::GpuNodeContribution>& gpuContributions) {
    // Read against the composed catalog: an installed package's node type is
    // known here, exactly as it is in the application session.
    const nemo::ProjectReadResult loaded = nemo::ProjectFile::read(options.project, contributions->catalog());
    if (!loaded.ok)
        throw std::runtime_error(loaded.error.message.empty() ? "cannot open project: " + options.project.string()
                                                              : loaded.error.message);
    report["warnings"] = loaded.warnings;
    // Resolved project OCIO configuration passed explicitly to the viewer; no
    // process-global environment mutation.
    const std::string& ocioConfigPath = loaded.colorConfigPath;
    report["color_config"] = ocioConfigPath;

    std::error_code directoryError;
    const bool hadExistingCache = std::filesystem::exists(options.cacheDirectory, directoryError) && !directoryError;
    std::filesystem::create_directories(options.cacheDirectory, directoryError);
    if (directoryError)
        throw std::runtime_error("cannot create cache directory '" + options.cacheDirectory.string() +
                                 "': " + directoryError.message());

    auto instance = nemo::gpu::Instance::create({.validation = true});
    // The headless harness performs no cross-device presentation handoff, so it
    // asks for no external-memory device capability: the BC7 path is a
    // same-device encode/upload/sample path.
    auto device = nemo::gpu::Device::create(*instance);
    auto allocator = nemo::gpu::Allocator::create(*instance, *device, {.max_device_bytes = 2ULL << 30});
    const auto& properties = device->properties();
    VkFormatProperties bc7Properties{};
    vkGetPhysicalDeviceFormatProperties(device->physical(), VK_FORMAT_BC7_UNORM_BLOCK, &bc7Properties);
    report["device"] = {
        {"name", properties.deviceName},
        {"vendor_id", properties.vendorID},
        {"device_id", properties.deviceID},
        {"driver_version", properties.driverVersion},
        {"api_version", properties.apiVersion},
        {"decode_queue_available", device->decode_family().has_value()},
        {"bc7_sampled_supported", (bc7Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0}};

    std::filesystem::path shaders = options.shaderDirectory;
#ifdef NEMO_SLANG_SPV_DIR
    if (shaders.empty())
        shaders = NEMO_SLANG_SPV_DIR;
#endif
    if (shaders.empty())
        throw std::runtime_error("no Slang shader directory: pass --shaders <spv-dir> or configure NEMO_SLANG_SPV_DIR");

    // Shared cache contract (issue #106): byte bounds are primary, the directory
    // is the only per-invocation choice, and no codec/GOP/bitrate/chunk knob
    // exists any more.
    nemo::eval::ViewerCacheOptions cacheOptions;
    cacheOptions.directory = options.cacheDirectory;
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
                         {"representation", "bc7-unorm-4x4"},
                         {"fidelity_diagnostic", options.fidelity},
                         {"cache_directory", std::filesystem::absolute(options.cacheDirectory).string()},
                         {"preexisting_cache_directory", hadExistingCache}};
    int actualRepresentationWidth = 0;
    int actualRepresentationHeight = 0;
    std::vector<double> buildLatencies;
    std::optional<nemo::CpuImage> fidelitySource;
    std::optional<nemo::CpuImage> fidelityReplay;
    std::vector<std::uint32_t> presentationSpirv;
    std::vector<std::uint32_t> bc7Spirv;
    const std::int64_t fidelityFrame = uniqueRequested.front();
    std::uint64_t diagnosticReadbacks = 0;
    if (options.fidelity) {
        presentationSpirv = nemo::gpu::loadSpirv(shaders / "viewerPresentation.spv");
        bc7Spirv = nemo::gpu::loadSpirv(shaders / "viewerPresentationBc7.spv");
        // Establish the source-view oracle in a standalone session with no
        // cache configured. This prevents a pre-existing retained entry from
        // becoming a self-comparison when --fidelity is requested.
        nemo::eval::ViewerSession sourceSession(*instance, *device, *allocator, shaders, ocioConfigPath,
                                                gpuContributions);
        const nemo::EvaluationRequest sourceRequest = makeRequest(loaded.document, options, fidelityFrame);
        nemo::eval::ViewerFrame sourceFrame =
            sourceSession.render(loaded.document, sourceRequest, 10'000'000'000ULL, 0);
        fidelitySource =
            diagnosticReadback(sourceFrame, *device, *allocator, presentationSpirv, bc7Spirv, 10'000'000'000ULL);
        ++diagnosticReadbacks;
    }
    const auto buildStart = Clock::now();
    nemo::eval::ViewerCacheCounts buildBefore;
    nemo::eval::ViewerCacheCounts buildAfter;
    std::uint64_t generation = 1;
    std::uint64_t buildRetries = 0;
    double editProbeMs = 0.0;
    {
        nemo::eval::ViewerSession session(*instance, *device, *allocator, shaders, ocioConfigPath, gpuContributions);
        session.configureCache(cacheOptions);
        buildBefore = session.cacheCounts();
        if (options.edit.requested() && buildBefore.diskBytes != 0)
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
            // retains a float history. A frame whose retained blocks are still
            // loading (fresh-process reopen) is retried, never re-rendered.
            nemo::eval::ViewerFrame frame = retryPending(
                [&] { return session.render(loaded.document, request, 10'000'000'000ULL, generation); }, buildRetries);
            actualRepresentationWidth = frame.layout.width;
            actualRepresentationHeight = frame.layout.height;
            buildLatencies.push_back(elapsedMs(renderStart, Clock::now()));
            if (options.staleSupersede && generation == 1) {
                // Retry the same requested representation under the current
                // generation. This is replacement of one requested frame,
                // never speculative range filling.
                ++generation;
                const auto retryStart = Clock::now();
                [[maybe_unused]] nemo::eval::ViewerFrame retry = retryPending(
                    [&] { return session.render(loaded.document, request, 10'000'000'000ULL, generation); },
                    buildRetries);
                buildLatencies.push_back(elapsedMs(retryStart, Clock::now()));
            }
            const auto backlog = session.cacheCounts();
            if (backlog.pendingFrames + backlog.activeFrames >= cacheOptions.maxPendingFrames) {
                // The writer queue is bounded by frames AND bytes; an explicit
                // range build drains at the frame bound so requested outputs
                // are never dropped, and the elapsed drain stays in buildMs.
                session.flushCache();
            }
            ++generation;
        }
        // Encoding is asynchronous; this is the sole explicit drain in the
        // build phase and happens before the independent replay session opens.
        session.flushCache();
        buildAfter = session.cacheCounts();
        if (options.edit.requested()) {
            const auto probeStart = Clock::now();
            report["invalidation_probes"] =
                probeInvalidation(session, loaded.document, options, generation, contributions);
            editProbeMs = elapsedMs(probeStart, Clock::now());
            for (const auto& probe : report["invalidation_probes"])
                if (!probe.at("ok").get<bool>())
                    report["errors"].push_back("invalidation probe failed: " + probe.at("kind").get<std::string>());
        }
    }
    report["request"]["actual_representation_width"] = actualRepresentationWidth;
    report["request"]["actual_representation_height"] = actualRepresentationHeight;
    if (actualRepresentationWidth > 0 && actualRepresentationHeight > 0) {
        report["request"]["bc7_payload_bytes"] =
            nemo::gpu::bc7PayloadBytes(static_cast<std::uint32_t>(actualRepresentationWidth),
                                       static_cast<std::uint32_t>(actualRepresentationHeight));
    }
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
        {"replay_pending_retries", buildRetries},
        {"cache_build_duration_scope", "explicit requested renders plus asynchronous BC7 encode/upload finalization"}};
    report["cache_accounting"] = {
        {"encoded_frames", buildAfter.encodedFrames},
        {"uploaded_frames", buildAfter.uploadedFrames},
        {"resident_frames", buildAfter.residentFrames},
        {"resident_bytes", buildAfter.residentBytes},
        {"compressed_hot_hits", buildAfter.compressedHotHits},
        {"compressed_hot_bytes", buildAfter.compressedHotBytes},
        {"pending_bytes", buildAfter.pendingBytes},
        {"disk_bytes", buildAfter.diskBytes},
        {"scope", "BC7 block encode, compressed upload, resident/hot-set and disk accounting; excludes source decode, "
                  "graph evaluation, presentation and separately reported diagnostic readbacks"}};
    report["backlog"] = {{"pending_frames_after_flush", buildAfter.pendingFrames},
                         {"active_frames_after_flush", buildAfter.activeFrames},
                         {"peak_pending_frames", buildAfter.peakPendingFrames},
                         {"pending_bytes_after_flush", buildAfter.pendingBytes},
                         {"max_pending_frames", cacheOptions.maxPendingFrames},
                         {"max_pending_bytes", cacheOptions.maxPendingBytes},
                         {"admission_rejected", buildAfter.admissionRejected},
                         {"admission_dropped", buildAfter.admissionDropped},
                         {"disk_bytes", buildAfter.diskBytes}};

    const std::vector<std::int64_t> replayFrames = replaySequence(options);
    std::vector<double> replayLatencies;
    double replayDiagnosticMs = 0.0;
    std::uint64_t replayRetries = 0;
    std::uint64_t replayBc7Frames = 0;
    nemo::eval::ViewerCacheCounts replayBefore;
    nemo::eval::ViewerCacheCounts replayAfter;
    nemo::CacheCounts replayReuseBefore;
    nemo::CacheCounts replayReuseAfter;
    auto replayStart = Clock::now();
    const auto replayRequest = makeRequest(loaded.document, options, 0);
    const auto replayRevision = loaded.document.stateRevision();
    {
        nemo::eval::ViewerSession replay(*instance, *device, *allocator, shaders, ocioConfigPath, gpuContributions);
        replay.configureCache(cacheOptions);
        replayBefore = replay.cacheCounts();
        replayReuseBefore = replay.reuseCounts();
        for (const std::int64_t frameNumber : replayFrames) {
            auto request = replayRequest;
            request.localTime = frameNumber;
            const auto renderStart = Clock::now();
            const auto replayGeneration = generation++;
            nemo::eval::ViewerFrame frame = retryPending(
                [&] { return replay.render(loaded.document, request, 10'000'000'000ULL, replayGeneration); },
                replayRetries);
            replayLatencies.push_back(elapsedMs(renderStart, Clock::now()));
            if (frame.replay)
                ++replayBc7Frames;
            if (options.fidelity && !fidelityReplay && frameNumber == fidelityFrame) {
                if (!frame.replay)
                    throw std::runtime_error("fidelity: the requested frame was not served from the BC7 cache");
                const auto diagnosticStart = Clock::now();
                fidelityReplay =
                    diagnosticReadback(frame, *device, *allocator, presentationSpirv, bc7Spirv, 10'000'000'000ULL);
                ++diagnosticReadbacks;
                replayDiagnosticMs += elapsedMs(diagnosticStart, Clock::now());
            }
        }
        // Reopening validates persisted identities against this document once.
        // Ordinary indexed replay is a separate phase: its API has no Document
        // and cannot silently hide description/planning behind a cache hit.
        const auto reopenedCounts = replay.cacheCounts();
        const bool reopenedAllHit = replayBc7Frames == replayFrames.size() &&
                                    reopenedCounts.misses == replayBefore.misses &&
                                    replay.reuseCounts().misses == replayReuseBefore.misses;
        report["reopened_replay"] = {
            {"duration_ms", elapsedMs(replayStart, Clock::now()) - replayDiagnosticMs},
            {"latency", latencyJson(replayLatencies, "reopened_replay")},
            {"cache_counts", cacheCountsJson(reopenedCounts)},
            {"replay_pending_retries", replayRetries},
            {"all_cache_hits", reopenedAllHit},
            {"scope", "fresh-session identity validation and compressed disk preparation; first access may plan"}};
        report["assertions"]["reopened_cache_hits"] = reopenedAllHit;
        if (!reopenedAllHit)
            report["errors"].push_back("reopened cache did not serve every requested frame without live evaluation");
        replayBefore = reopenedCounts;
        replayReuseBefore = replay.reuseCounts();
        replayLatencies.clear();
        replayRetries = 0;
        replayBc7Frames = 0;
        replayStart = Clock::now();
        for (const auto frameNumber : replayFrames) {
            auto request = replayRequest;
            request.localTime = frameNumber;
            const auto started = Clock::now();
            const auto frame = retryPending(
                [&] {
                    auto retained = replay.replay(request, replayRevision);
                    if (!retained)
                        throw std::runtime_error("validated cache frame disappeared before indexed replay");
                    return std::move(*retained);
                },
                replayRetries);
            replayLatencies.push_back(elapsedMs(started, Clock::now()));
            if (frame.replay)
                ++replayBc7Frames;
        }
        replayAfter = replay.cacheCounts();
        replayReuseAfter = replay.reuseCounts();
    }
    const double replayMs = elapsedMs(replayStart, Clock::now());
    const std::uint64_t buildMisses = delta(buildAfter.misses, buildBefore.misses);
    const std::uint64_t replayHits = delta(replayAfter.hits, replayBefore.hits);
    const std::uint64_t replayMisses = delta(replayAfter.misses, replayBefore.misses);
    const std::uint64_t replayGraphMisses = delta(replayReuseAfter.misses, replayReuseBefore.misses);
    const std::uint64_t buildResolved = buildPublished + buildHits;
    const bool allReplayHit = replayHits >= replayFrames.size() && replayMisses == 0;
    const bool noGraphReevaluation = replayGraphMisses == 0;
    const bool allReplayBc7 = replayBc7Frames == replayFrames.size();
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
                             {"lookup", "validated concrete replay; no Document access or graph planning"},
                             {"replay_pending_retries", replayRetries},
                             {"bc7_frames", replayBc7Frames},
                             {"timing_scope", "request through GPU-complete ViewerFrame; not visible-surface latency"}};
    report["assertions"]["requested_only"] = requestedOnly;
    report["assertions"]["requested_only_detail"] = "published viewer outputs are bounded by unique explicit requests; "
                                                    "dependencies are not counted as viewer frames";
    report["assertions"]["all_requested_resolved"] = allRequestedResolved;
    report["assertions"]["replay_all_cache_hits"] = allReplayHit;
    report["assertions"]["replay_graph_not_reevaluated"] = noGraphReevaluation;
    report["assertions"]["replay_frames_are_bc7"] = allReplayBc7;
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
                                 {"replay_frames", replayFrames.size()},
                                 {"replay_bc7_frames", replayBc7Frames}};
    if (options.fidelity) {
        if (!fidelitySource || !fidelityReplay) {
            report["fidelity"] = {{"performed", false},
                                  {"error", "diagnostic live reference or BC7 replay sample was unavailable"},
                                  {"readbacks", diagnosticReadbacks}};
            report["errors"].push_back("fidelity diagnostic requested but the live reference or BC7 sample was "
                                       "unavailable");
        } else {
            report["fidelity"] = fidelityJson(*fidelitySource, *fidelityReplay);
            report["fidelity"]["readbacks"] = diagnosticReadbacks;
            report["fidelity"]["replay_sample_ms"] = replayDiagnosticMs;
            report["fidelity"]["device_to_host_bytes"] = diagnosticReadbacks *
                                                         static_cast<std::uint64_t>(fidelitySource->width()) *
                                                         fidelitySource->height() * 4 * sizeof(float);
            if (!report["fidelity"]["within_declared_tolerance"].get<bool>())
                report["errors"].push_back("fidelity diagnostic exceeded its declared BC7-loss tolerance");
        }
    } else {
        report["fidelity"] = {{"performed", false},
                              {"readbacks", 0},
                              {"note", "Pass --fidelity for one bounded live-reference and sampled-BC7-replay "
                                       "diagnostic comparison; no routine full-frame CPU readback is performed."}};
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
            "independent replay did not resolve every requested frame from retained BC7 cache entries");
    if (!noGraphReevaluation)
        report["errors"].push_back("independent replay re-evaluated graph work for a cached frame");
    if (!allReplayBc7)
        report["errors"].push_back("independent replay returned a non-BC7 frame for a requested frame");
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

int commandViewerCache(const std::vector<std::string>& args,
                       std::shared_ptr<const nemo::NodeContributions> contributions
#ifdef NEMO_BUILD_GPU
                       ,
                       std::vector<nemo::eval::GpuNodeContribution> gpuContributions
#endif
) {
    Json report{{"ok", false}, {"command", "cache-viewer"}, {"errors", Json::array()}};
    try {
        if (!contributions)
            throw std::invalid_argument("cache-viewer requires a composed node inventory");
        const CacheCommandOptions options = parseArguments(args);
#ifdef NEMO_BUILD_GPU
        const int result = runGpuHarness(options, report, contributions, gpuContributions);
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
