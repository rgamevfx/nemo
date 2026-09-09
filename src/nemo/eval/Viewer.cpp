#include "nemo/eval/Viewer.hpp"
#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/media/ViewingTransform.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace nemo::eval {
namespace {

[[nodiscard]] std::string programIdentity(const media::OcioGpuProgram& program) {
    std::uint64_t hash = kFnv1a64Basis;
    hashMixText(hash, program.glsl);
    hashMix(hash, program.uniformBytes.data(), program.uniformBytes.size());
    hashMix(hash, &program.descriptorSet, sizeof(program.descriptorSet));
    hashMix(hash, &program.textureBindingStart, sizeof(program.textureBindingStart));
    for (const auto& texture : program.textures) {
        const std::array<unsigned, 5> layout{texture.width, texture.height, texture.dimensions, texture.channels,
                                             texture.binding};
        hashMix(hash, layout.data(), sizeof(layout));
        hashMix(hash, texture.values.data(), texture.values.size() * sizeof(float));
    }
    return "ocio-gpu-v1:" + std::to_string(hash);
}

[[nodiscard]] std::string cacheIdentity(const ResultKey& key, const std::string& viewingIdentity,
                                        const ViewerCacheOptions& options) {
    std::string identity;
    identity.reserve(key.canonical.size() + viewingIdentity.size() + options.encoding.codec.size() + 192);
    appendCanonicalField(identity, "viewer-result", key.canonical);
    appendCanonicalField(identity, "ocio-program", viewingIdentity);
    appendCanonicalField(identity, "interpretation", "bt709/limited/left/420/8bit/display-v1");
    appendCanonicalField(identity, "codec", options.encoding.codec);
    appendCanonicalField(identity, "gop", std::to_string(options.encoding.gopSize));
    appendCanonicalField(identity, "bitrate", std::to_string(options.encoding.bitrateKbps));
    appendCanonicalField(identity, "profile", options.encoding.profile);
    appendCanonicalField(identity, "depth", std::to_string(options.encoding.bitDepth));
    appendCanonicalField(identity, "chunk-frames", std::to_string(options.chunkFrames));
    return identity;
}

}  // namespace

ViewerSession::ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                             const std::filesystem::path& shaderDirectory)
    : instance_(instance), device_(device), allocator_(allocator), replayShader_(shaderDirectory / "mediaConvert.spv"),
      sources_(instance, device, allocator, replayShader_),
      effects_(loadSlangEffectLibrary(shaderDirectory, shaderDirectory)), reuse_(16) {}

ViewerSession::~ViewerSession() = default;

ViewerSession::SourceProbe ViewerSession::probeSource(const Document& document, const std::string& sourceKey) const {
    const SourceSession::Probe probe = sources_.probe(document, sourceKey);
    return SourceProbe{probe.info, probe.decision};
}

CacheCounts ViewerSession::reuseCounts() const {
    return reuse_.counts();
}

void ViewerSession::configureCache(const ViewerCacheOptions& options) {
    std::lock_guard freshnessLock(freshnessMutex_);
    std::lock_guard cacheLock(cacheMutex_);
    if (cache_)
        throw std::logic_error("viewer cache is already configured");
    auto cache = std::make_unique<ViewerCache>(instance_, device_, allocator_, replayShader_);
    cache->configure(options);
    for (const auto& [destination, generation] : latestGenerationByDestination_) {
        const auto revision = latestRevisionByDestination_.find(destination);
        cache->supersede(revision == latestRevisionByDestination_.end() ? 0 : revision->second, generation,
                         destination);
    }
    cache_ = std::move(cache);
}

void ViewerSession::flushCache() {
    std::lock_guard cacheLock(cacheMutex_);
    if (cache_)
        cache_->flush();
}

ViewerCacheCounts ViewerSession::cacheCounts() const {
    std::lock_guard cacheLock(cacheMutex_);
    if (!cache_)
        return {};
    return cache_->counts();
}

void ViewerSession::supersedeCache(std::uint64_t revision, std::uint64_t generation, ViewerDestination destination) {
    // This short freshness section is safe from the UI thread. It never
    // performs cache I/O, decode, or GPU waits; those remain worker-owned.
    std::lock_guard freshnessLock(freshnessMutex_);
    auto& currentGeneration = latestGenerationByDestination_[destination];
    if (generation < currentGeneration)
        return;
    currentGeneration = generation;
    latestRevisionByDestination_[destination] = revision;
    std::lock_guard cacheLock(cacheMutex_);
    if (cache_)
        cache_->supersede(revision, generation, destination);
}

ViewerSession::ViewingState& ViewerSession::viewingStateFor(const ColorPolicy& policy) {
    if (ocioConfigPath_.empty())
        ocioConfigPath_ = media::resolveConfigPath({});
    const auto key = std::pair{policy.workingSpace, policy.viewerTransform};
    if (const auto found = viewing_.find(key); found != viewing_.end())
        return found->second;
    auto program = media::buildViewingTransformGpu(ocioConfigPath_, policy.workingSpace, policy.viewerTransform,
                                                   media::TransformKind::Viewer);
    auto identity = programIdentity(program);
    return viewing_.emplace(key, ViewingState{std::move(program), std::move(identity), {}}).first->second;
}

ViewerFrame ViewerSession::render(const Document& document, const EvaluationRequest& request, std::uint64_t timeout_ns,
                                  std::uint64_t generation, ViewerDestination destination,
                                  CachePublicationGuard publicationGuard) {
    // Worker-only contract; validate here so a malformed request fails on
    // the caller's thread with a precise reason before any GPU work.
    validateRequest(document, request);
    const auto requestId = nextRequestId_++;
    const auto revision = document.stateRevision();
    {
        std::lock_guard lock(freshnessMutex_);
        auto& currentGeneration = latestGenerationByDestination_[destination];
        if (generation == 0)
            generation = currentGeneration + 1;
        if (generation >= currentGeneration) {
            latestRevisionByDestination_[destination] = revision;
            currentGeneration = generation;
        }
    }

    // Identity describes the exact generated program/LUT/uniform snapshot
    // used below, never newly read config bytes paired with an old program.
    auto& viewing = viewingStateFor(document.color);
    std::optional<std::string> identity;
    ImageLayout expected;
    expected.width = scaledDimension(request.region.width, request.samplingScale);
    expected.height = scaledDimension(request.region.height, request.samplingScale);
    expected.color = ColorInterpretation::DisplayReferred;
    if (cache_) {
        cache_->supersede(revision, generation, destination);
        const ResultKey key = queryViewerResultKey(document, request, effects_);
        identity = cacheIdentity(key, viewing.identity, cache_->optionsForIdentity());
        if (auto hit = cache_->lookup(*identity, expected, timeout_ns)) {
            ViewerFrame frame;
            frame.image = std::move(hit->image);
            frame.layout = hit->layout;
            frame.request = request;
            frame.revision = revision;
            frame.requestId = requestId;
            frame.cacheHit = true;
            return frame;
        }
    }

    // Shared dependency plan: scene-linear reuse under the evaluator's own
    // ticket; decoded frames flow through SourceSession. Compression is not
    // on this path, so the live frame is returned without waiting for it.
    const auto evaluation =
        evaluateGpu(document, request, effects_, device_, allocator_, timeout_ns, &reuse_, &sources_);

    const GpuNodeImage& composition = *evaluation.images.at(request.output);
    if (composition.layout.color != ColorInterpretation::SceneLinear) {
        const Node* node = document.graph.node(request.output);
        throw EvaluationException(
            describeNode(*node) + ": produced a " +
                std::string(composition.layout.color == ColorInterpretation::DisplayReferred ? "display-referred"
                                                                                             : "uninterpreted") +
                " result; the viewing transform is applied exactly once and a second application is refused",
            request.output, node->name);
    }

    if (!viewing.transform)
        viewing.transform = std::make_unique<gpu::GpuViewingTransform>(device_, allocator_, viewing.program);
    std::optional<gpu::GpuViewedImage> viewed =
        viewing.transform->submit(composition.image, composition.layout.color, timeout_ns);
    if (!viewed)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "viewer transform submission capacity exhausted");

    auto& queue = device_.submissions(device_.graphics_family());
    if (!queue.wait(viewed->completion, timeout_ns))
        throw gpu::GpuException(gpu::GpuError::SubmissionTimeout,
                                "viewer frame did not complete within " + std::to_string(timeout_ns) + " ns");

    auto image = std::make_shared<gpu::Image>(std::move(viewed->image));
    ViewerFrame frame;
    frame.image = image;
    frame.layout = composition.layout;
    frame.layout.color = ColorInterpretation::DisplayReferred;
    frame.request = request;
    frame.revision = revision;
    frame.requestId = requestId;
    frame.cacheHit = false;

    if (identity && (!publicationGuard || publicationGuard())) {
        // Chunk grouping is a storage concern, not a synthetic evaluation
        // request. Each frame keeps its full effective identity in the index.
        auto chunkGroupKey =
            viewing.identity + "/" + std::to_string(frame.layout.width) + "x" + std::to_string(frame.layout.height);
        cache_->enqueue(ViewerCachePublication{.identity = std::move(*identity),
                                               .chunkGroupKey = std::move(chunkGroupKey),
                                               .localTime = request.localTime,
                                               .revision = revision,
                                               .generation = generation,
                                               .image = image,
                                               .layout = frame.layout,
                                               .destination = destination,
                                               .publicationGuard = std::move(publicationGuard)});
    }
    return frame;
}

}  // namespace nemo::eval
