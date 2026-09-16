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

ViewerProjection resolveViewerProjection(const std::vector<std::string>& requested,
                                         const std::vector<std::string>& channels) {
    const auto& selected = requested.empty() ? channels : requested;
    ViewerProjection projection;
    const auto primary = rgbaChannelIndices(selected);
    projection.applyViewingTransform =
        std::all_of(primary.begin(), primary.begin() + 3, [](int role) { return role >= 0; });
    if (projection.applyViewingTransform) {
        for (std::size_t role = 0; role < primary.size(); ++role)
            if (primary[role] >= 0)
                projection.roles[role] = channelIndex(channels, selected[primary[role]]);
    } else if (selected.size() == 1) {
        const auto plane = channelIndex(channels, selected.front());
        projection.roles = {plane, plane, plane, -1};
    } else {
        for (std::size_t slot = 0; slot < selected.size() && slot < projection.roles.size(); ++slot)
            projection.roles[slot] = channelIndex(channels, selected[slot]);
    }
    return projection;
}

ViewerSession::ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                             const std::filesystem::path& shaderDirectory, std::string ocioConfigPath)
    : instance_(instance), device_(device), allocator_(allocator), ocioConfigPath_(std::move(ocioConfigPath)),
      replayShader_(shaderDirectory / "mediaConvert.spv"),
      sources_(instance, device, allocator, replayShader_, ocioConfigPath_),
      effects_(loadSlangEffectLibrary(shaderDirectory)), projections_(device, allocator), reuse_(16) {}

ViewerSession::~ViewerSession() = default;

void ViewerSession::refreshColorConfig() {
    // Retire the retained viewing programs/LUTs and the source session's color
    // generation. Nothing polls: this is the sole boundary, called by the owner
    // that replaced the project or deliberately reloaded the configuration.
    viewing_.clear();
    sources_.refreshColorConfig();
}

ImageDescription ViewerSession::describe(const Document& document, const EvaluationRequest& request) {
    // Description is domain-independent: the target's authored format must not
    // depend on how much of it a panel happens to be looking at, so the plan is
    // taken from a minimal valid request that identifies the target and time.
    // Describing a graph never defines coverage, so the resolution stops at the
    // described dependency set: no region planning and no pixel work.
    EvaluationRequest query = request;
    query.region = Region{0, 0, 1, 1};
    query.fullWidth = 1;
    query.fullHeight = 1;
    validateRequestDomain(query);
    const ImageDescriptionPlan plan = describeDependencies(document, query, *effects_.contributions(), &sources_);
    const EvaluationNodeId outputKey{query.network, kInvalidNetworkInstance, query.output, kEvaluationWholeNode};
    return plan.nodes.at(outputKey).description;
}

ViewerSession::SourceProbe ViewerSession::probeSource(const Document& document, const std::string& sourceKey) const {
    const SourceSession::Probe probe = sources_.probe(document, sourceKey);
    return SourceProbe{probe.info, probe.decision};
}

CacheCounts ViewerSession::reuseCounts() const {
    return reuse_.counts();
}

void ViewerSession::configureCache(const ViewerCacheOptions& options) {
    std::scoped_lock stateLocks(cacheMutex_, freshnessMutex_);
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

std::optional<ViewerCacheCounts> ViewerSession::tryCacheCounts() const {
    std::unique_lock cacheLock(cacheMutex_, std::try_to_lock);
    if (!cacheLock.owns_lock())
        return std::nullopt;
    return cache_ ? cache_->tryCounts() : std::optional<ViewerCacheCounts>{ViewerCacheCounts{}};
}

std::uint64_t& ViewerSession::generationForLocked(ViewerDestination destination) {
    const auto found = latestGenerationByDestination_.find(destination);
    if (found != latestGenerationByDestination_.end())
        return found->second;
    if (latestGenerationByDestination_.size() >= kMaxViewerDestinations)
        throw std::runtime_error("viewer destination " + std::to_string(static_cast<std::uint32_t>(destination)) +
                                 " exceeds destination capacity " + std::to_string(kMaxViewerDestinations));
    return latestGenerationByDestination_.try_emplace(destination, 0).first->second;
}

void ViewerSession::supersedeCache(std::uint64_t revision, std::uint64_t generation, ViewerDestination destination) {
    std::scoped_lock stateLocks(cacheMutex_, freshnessMutex_);
    auto& currentGeneration = generationForLocked(destination);
    if (generation < currentGeneration)
        return;
    currentGeneration = generation;
    latestRevisionByDestination_[destination] = revision;
    if (cache_)
        cache_->supersede(revision, generation, destination);
}

void ViewerSession::retireDestination(ViewerDestination destination) {
    std::lock_guard freshnessLock(freshnessMutex_);
    latestRevisionByDestination_.erase(destination);
    latestGenerationByDestination_.erase(destination);
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

ResolvedView resolveViewIntent(const ViewIntent& intent, const ImageDescription& description,
                               ViewerResolutionPolicy& resolution) {
    ResolvedView view;
    EvaluationRequest& request = view.request;
    request.network = intent.network;
    request.output = intent.target;
    request.localTime = intent.localTime;
    // The domain is the described image's own logical format: the demand is
    // stated against the frame's ACTUAL geometry, never a request-global canvas
    // and never a substituted size.
    const int width = description.format.width;
    const int height = description.format.height;
    request.fullWidth = width;
    request.fullHeight = height;
    request.region = Region{0, 0, width, height};

    // The addressed layer's real channel names of THIS frame's described image
    // are the demand itself, so the request owns them once and every later
    // check reads the same list. A layer the frame does not carry, or a channel
    // the layer does not name, is reported with its reason; nothing is
    // substituted for it.
    request.channels.clear();
    for (const auto& channel : description.channels) {
        if (channelInViewLayer(channel, intent.layer))
            request.channels.push_back(channel);
    }
    if (request.channels.empty())
        throw ViewUnavailable(viewLayerReason(description, intent.layer), description);
    const std::string& selected = intent.channel;
    if (selected != kViewCompositeChannel && !hasChannel(request.channels, selected))
        throw ViewUnavailable(viewChannelUnavailable(selected, intent.layer), description);

    // One identified primary channel of a color-managed layer is isolated in the
    // presentation copy only: the evaluated frame still carries every channel of
    // the layer, so isolating a channel never changes what the graph produced. A
    // data channel is demanded by its exact name instead.
    if (selected != kViewCompositeChannel) {
        bool isolated = false;
        if (resolveViewerProjection({}, request.channels).applyViewingTransform) {
            const std::array<int, 4> roles = rgbaChannelIndices(std::span<const std::string>(&selected, 1));
            for (std::size_t role = 0; role < roles.size(); ++role) {
                if (roles[role] >= 0) {
                    // ViewerChannel: RGBA = 0, then R, G, B, A in role order.
                    view.presentationChannel = static_cast<gpu::ViewerChannel>(role + 1);
                    isolated = true;
                    break;
                }
            }
        }
        if (!isolated)
            request.channels.assign(1, selected);
    }

    const double pixelAspect = description.pixelAspect > 0.0F ? static_cast<double>(description.pixelAspect) : 1.0;
    request.samplingScale = resolution.resolve(intent.mode, width, height, pixelAspect, intent.viewportWidth,
                                               intent.viewportHeight, intent.zoom);
    // Coverage: whole-frame mode names the same domain at every pan and zoom,
    // otherwise the region this view actually shows. Sampling density and the
    // display transform are retained either way; a viewport the panel has not
    // measured yet simply covers the frame.
    const ViewerFit fit = aspectFit(width, height, pixelAspect, intent.viewportWidth, intent.viewportHeight);
    const double zoom = intent.zoom > 0.0 ? intent.zoom : 1.0;
    if (intent.forceFullFrame || !(fit.width > 0.0) || !(fit.height > 0.0)) {
        request.region = Region{0, 0, width, height};
    } else {
        const double sx = fit.width / width * zoom;
        const double sy = fit.height / height * zoom;
        const double visibleWidth = std::min<double>(width, intent.viewportWidth / sx);
        const double visibleHeight = std::min<double>(height, intent.viewportHeight / sy);
        const double centerX = std::clamp(width / 2.0 + intent.panX, visibleWidth / 2.0, width - visibleWidth / 2.0);
        const double centerY =
            std::clamp(height / 2.0 + intent.panY, visibleHeight / 2.0, height - visibleHeight / 2.0);
        const int x = std::max(0, static_cast<int>(std::floor(centerX - visibleWidth / 2)));
        const int y = std::max(0, static_cast<int>(std::floor(centerY - visibleHeight / 2)));
        const int right = std::min(width, static_cast<int>(std::ceil(centerX + visibleWidth / 2)));
        const int bottom = std::min(height, static_cast<int>(std::ceil(centerY + visibleHeight / 2)));
        request.region = Region{x, y, right - x, bottom - y};
    }
    // One canonical coverage reaches the executor, the cache and the panel: the
    // region is rounded out to the image-space sampling lattice and never
    // shrinks, so a region a cached frame carries always contains the raster
    // this request asks for.
    view.request = canonicalizeRequest(view.request);
    validateRequestDomain(view.request);
    return view;
}

ViewerFrame ViewerSession::render(const Document& document, const ViewIntent& intent,
                                  ViewerResolutionPolicy& resolution, std::uint64_t timeout_ns,
                                  std::uint64_t generation, ViewerDestination destination,
                                  CachePublicationGuard publicationGuard) {
    // Description first, on the same worker job: a minimal request that
    // identifies the target and its local time describes the CURRENT frame's
    // authored output without acquiring a pixel or touching the device, so
    // animated geometry, sequence headers and format changes are resolved for
    // exactly the frame this demand belongs to.
    EvaluationRequest query;
    query.network = intent.network;
    query.output = intent.target;
    query.localTime = intent.localTime;
    query.region = Region{0, 0, 1, 1};
    query.fullWidth = 1;
    query.fullHeight = 1;
    validateRequestDomain(query);
    const EvaluationNodeId outputKey{query.network, kInvalidNetworkInstance, query.output, kEvaluationWholeNode};
    ImageDescriptionPlan described = describeDependencies(document, query, *effects_.contributions(), &sources_);
    const ResolvedView view = resolveViewIntent(intent, described.nodes.at(outputKey).description, resolution);
    // The description this demand was stated against is the plan the key and the
    // execution consume, so one worker job resolves the authored state once.
    return renderResolved(document, view.request, std::move(described), timeout_ns, generation, destination,
                          std::move(publicationGuard), view.presentationChannel);
}

ViewerFrame ViewerSession::render(const Document& document, const EvaluationRequest& request, std::uint64_t timeout_ns,
                                  std::uint64_t generation, ViewerDestination destination,
                                  CachePublicationGuard publicationGuard) {
    return renderResolved(document, request, std::nullopt, timeout_ns, generation, destination,
                          std::move(publicationGuard), gpu::ViewerChannel::RGBA);
}

ViewerFrame ViewerSession::renderResolved(const Document& document, const EvaluationRequest& inputRequest,
                                          std::optional<ImageDescriptionPlan> described, std::uint64_t timeout_ns,
                                          std::uint64_t generation, ViewerDestination destination,
                                          CachePublicationGuard publicationGuard,
                                          gpu::ViewerChannel presentationChannel) {
    // Worker-only contract; validate here so a malformed request fails on
    // the caller's thread with a precise reason before any GPU work.
    validateRequest(document, inputRequest);
    const EvaluationRequest request = canonicalizeRequest(inputRequest);
    const auto requestId = nextRequestId_++;
    const auto revision = document.stateRevision();
    {
        std::lock_guard lock(freshnessMutex_);
        auto& currentGeneration = generationForLocked(destination);
        if (generation == 0)
            generation = currentGeneration + 1;
        if (generation >= currentGeneration) {
            latestRevisionByDestination_[destination] = revision;
            currentGeneration = generation;
        }
    }

    std::optional<std::string> identity;
    ImageLayout expected;
    expected.width = scaledDimension(request.region.width, request.samplingScale);
    expected.height = scaledDimension(request.region.height, request.samplingScale);
    expected.color = ColorInterpretation::DisplayReferred;
    // ONE resolved plan per render (issue #88): the cache key and the execution
    // consume the same resolved nodes, descriptions and source requests, so a
    // media header or authored-state change can never make the key describe
    // different pixels than the execution produced. When the caller already
    // resolved this target's description for this frame — the view intent does,
    // to state its demand against the actual format — that plan is adopted
    // instead, so the authored state is resolved exactly once per frame.
    const std::string colorIdentity = sources_.colorConfigIdentity();
    const RegionPlan plan =
        described ? planResolvedRegions(document, request, *effects_.contributions(), std::move(*described))
                  : planDependencyRegions(document, request, *effects_.contributions(), &sources_);
    const auto& description =
        plan.images.nodes
            .at(EvaluationNodeId{request.network, kInvalidNetworkInstance, request.output, kEvaluationWholeNode})
            .description;
    const auto selection = resolveViewerProjection(request.channels, description.channels);
    ViewingState* viewing = selection.applyViewingTransform ? &viewingStateFor(document.color) : nullptr;
    const std::string appliedViewing =
        "named-projection-v2/" + (viewing ? viewing->identity : std::string{"data-passthrough-v1"});

    if (cache_) {
        cache_->supersede(revision, generation, destination);
        // The same described plan the render path consumes (issue #88): the
        // lookup keys the authored target instead of decoding media or guessing
        // a canvas domain.
        const ResultKey key = queryViewerResultKey(document, request, effects_, colorIdentity, &sources_, &plan);
        identity = cacheIdentity(key, appliedViewing, cache_->optionsForIdentity());
        if (auto hit = cache_->lookup(*identity, expected, timeout_ns)) {
            ViewerFrame frame;
            frame.image = std::move(hit->image);
            frame.layout = hit->layout;
            frame.request = request;
            frame.description = plan.images.nodes
                                    .at(EvaluationNodeId{request.network, kInvalidNetworkInstance, request.output,
                                                         kEvaluationWholeNode})
                                    .description;
            frame.revision = revision;
            frame.requestId = requestId;
            frame.cacheHit = true;
            frame.presentationChannel = presentationChannel;
            return frame;
        }
    }

    // Shared dependency plan: scene-linear reuse under the evaluator's own
    // ticket; decoded frames flow through SourceSession. Compression is not
    // on this path, so the live frame is returned without waiting for it.
    const auto evaluation = evaluateGpu(document, request, effects_, device_, allocator_, timeout_ns, &reuse_,
                                        &sources_, colorIdentity, &plan);

    // Native viewing (issue #90): the composition carries its named channels in
    // the shared native layout, so the requested channels are gathered into the
    // interleaved RGBA32F presentation the viewing transform, the presentation
    // copy and the viewer cache all consume — entirely on the device, with no
    // host readback. With no selection the composition's projected roles are
    // used (the existing behaviour); a single named channel is presented as an
    // opaque gray view. Auxiliary channels are deliberately not part of the
    // display image: only the identified RGB roles are color-managed.
    //
    // A composition that ALREADY is that image — a packed four-channel raster
    // whose selected roles are its storage order at the exact logical extent —
    // needs no projection at all (issue #98): the frame retains the SAME
    // allocation through shared ownership of the evaluation's GpuNodeImage,
    // with no second image, no dispatch and no submission. Every other
    // selection, and any scalar-plane raster, still projects.
    const auto composition = evaluation.images.at(request.output);
    if (composition->layout.color == ColorInterpretation::DisplayReferred) {
        // Display-referred buffers must never receive a second projection and
        // viewing transform.
        const NodeInstance* node = document.network(request.network).graph().node(request.output);
        throw EvaluationException(describeNode(*node) +
                                      ": produced a display-referred result; the viewing transform is applied "
                                      "exactly once and a second application is refused",
                                  request.output, node->name);
    }
    auto& queue = device_.submissions(device_.graphics_family());
    std::optional<gpu::SubmissionQueue::Completion> pendingCompletion;
    std::shared_ptr<const gpu::Image> presentationSource;
    if (ChannelProjection::isIdentity(composition->image, selection.roles,
                                      static_cast<std::uint32_t>(composition->layout.width),
                                      static_cast<std::uint32_t>(composition->layout.height))) {
        presentationSource = std::shared_ptr<const gpu::Image>(composition, &composition->image);
    } else {
        std::optional<ChannelProjection::Submission> projection = projections_.submit(
            composition->image, selection.roles, static_cast<std::uint32_t>(composition->layout.width),
            static_cast<std::uint32_t>(composition->layout.height), timeout_ns);
        if (!projection)
            throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                    "viewer channel projection submission capacity exhausted");
        presentationSource = std::make_shared<gpu::Image>(std::move(projection->image));
        pendingCompletion = projection->completion;
    }

    std::shared_ptr<const gpu::Image> image;
    if (!selection.applyViewingTransform) {
        // No complete primary RGB: preserve data, including alpha-only masks.
        // The retained image is the projection's output, or — with nothing to
        // project — the composition itself, which the evaluation's own
        // completion covers; a completed evaluation is idempotent to wait for.
        const std::optional<gpu::SubmissionQueue::Completion> completion =
            pendingCompletion ? pendingCompletion : evaluation.completion;
        if (completion && !queue.wait(*completion, timeout_ns))
            throw gpu::GpuException(gpu::GpuError::SubmissionTimeout,
                                    "viewer channel projection did not complete within " + std::to_string(timeout_ns) +
                                        " ns");
        image = std::move(presentationSource);
    } else {
        if (!viewing->transform)
            viewing->transform = std::make_unique<gpu::GpuViewingTransform>(device_, allocator_, viewing->program);
        std::optional<gpu::GpuViewedImage> viewed =
            viewing->transform->submit(*presentationSource, composition->layout.color, timeout_ns);
        if (!viewed)
            throw gpu::GpuException(gpu::GpuError::InvalidRequest, "viewer transform submission capacity exhausted");
        if (!queue.wait(viewed->completion, timeout_ns))
            throw gpu::GpuException(gpu::GpuError::SubmissionTimeout,
                                    "viewer frame did not complete within " + std::to_string(timeout_ns) + " ns");
        image = std::make_shared<gpu::Image>(std::move(viewed->image));
    }
    ViewerFrame frame;
    frame.image = image;
    frame.layout = composition->layout;
    // The displayed representation is the RGBA projection of the composition's
    // named channels, so its layout names those four roles — not the
    // composition's whole named channel list (issue #90).
    frame.layout.channels = kViewerPresentationChannels;
    frame.layout.color = ColorInterpretation::DisplayReferred;
    // The described output the composition was actually produced from: framing
    // consumers read the real format instead of a global canvas guess.
    frame.description = evaluation.plan.description;
    frame.request = request;
    frame.revision = revision;
    frame.requestId = requestId;
    frame.cacheHit = false;
    // The display isolation the view asked for travels with the frame, so the
    // presentation is prepared from what this destination's view stated and
    // never from another panel's selection.
    frame.presentationChannel = presentationChannel;

    if (identity && (!publicationGuard || publicationGuard())) {
        // Chunk grouping is a storage concern, not a synthetic evaluation
        // request. Each frame keeps its full effective identity in the index.
        auto chunkGroupKey =
            appliedViewing + "/" + std::to_string(frame.layout.width) + "x" + std::to_string(frame.layout.height);
        frame.cacheQueued = cache_->enqueue(ViewerCachePublication{.identity = std::move(*identity),
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
