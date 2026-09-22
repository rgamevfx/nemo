#include "nemo/eval/Viewer.hpp"
#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/gpu/ExportStaging.hpp"
#include "nemo/media/ViewingTransform.hpp"
#include <algorithm>
#include <array>
#include <bit>
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

// Representation identity of one cached display frame (issue #106): the
// effective content key of the viewer representation, the concrete OCIO
// program that produced it, and the BC7 representation version. Codec,
// profile, bitrate and chunking settings are gone with the video
// representation; the cache stores one selected SDR display format, so there
// is exactly one representation version to state.
[[nodiscard]] std::string cacheIdentity(const ResultKey& key, const std::string& viewingIdentity) {
    std::string identity;
    identity.reserve(key.canonical.size() + viewingIdentity.size() + 128);
    appendCanonicalField(identity, "viewer-result", key.canonical);
    appendCanonicalField(identity, "ocio-program", viewingIdentity);
    appendCanonicalField(identity, "representation", kViewerCacheRepresentation);
    return identity;
}

[[nodiscard]] std::string regionField(const Region& region) {
    return std::to_string(region.x) + "," + std::to_string(region.y) + "," + std::to_string(region.width) + "," +
           std::to_string(region.height);
}

[[nodiscard]] std::string channelField(const std::vector<std::string>& channels) {
    std::string text;
    for (const auto& channel : channels) {
        text += channel;
        text.push_back('\n');
    }
    return text;
}

// Primary RGB roles the presentation may isolate out of a color-managed layer.
// Alpha is deliberately excluded: alpha is data, so it is demanded by name and
// presented from the demanded image's RGB (issue #99).
constexpr std::size_t kIsolatedViewRoles = 3;

// The signed pixel aspect a demand is resolved against: a description always
// carries a finite positive value, and the fallback keeps a malformed one from
// producing a degenerate fit.
[[nodiscard]] double demandPixelAspect(const ImageDescription& description) {
    return description.pixelAspect > 0.0F ? static_cast<double>(description.pixelAspect) : 1.0;
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
                             const std::filesystem::path& shaderDirectory, std::string ocioConfigPath,
                             std::vector<GpuNodeContribution> contributions)
    : instance_(instance), device_(device), allocator_(allocator), ocioConfigPath_(std::move(ocioConfigPath)),
      shaderDirectory_(shaderDirectory), replayShader_(shaderDirectory / "mediaConvert.spv"),
      sources_(instance, device, allocator, replayShader_, ocioConfigPath_),
      // The caller's complete inventory becomes THIS session's immutable native
      // projection (issue #37). The built-in default is the same assembly the
      // implicit load used to build, so a caller that supplies nothing still gets
      // exactly the built-in effects; a caller that supplies installed package
      // contributions gets those and only those.
      effects_(std::move(contributions), EffectBackend::Slang, shaderDirectory), projections_(device, allocator),
      reuse_(16) {}

ViewerSession::~ViewerSession() = default;

void ViewerSession::refreshColorConfig() {
    // Retire the retained viewing programs/LUTs and the source session's color
    // generation. Nothing polls: this is the sole boundary, called by the owner
    // that replaced the project or deliberately reloaded the configuration. The
    // validated frame records were resolved under the retired configuration, so
    // they go with it: a record may only ever describe the color state it was
    // produced with.
    viewing_.clear();
    forgetRecords();
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

std::array<float, 4> ViewerSession::sampleWorkingPixel(const Document& document, const EvaluationRequest& inputRequest,
                                                       const std::uint64_t timeout_ns) {
    // Bounded by the demand: a pick is one full-resolution pixel, so the
    // readback can never become a frame download through a wider request.
    validateRequestDomain(inputRequest);
    const EvaluationRequest request = canonicalizeRequest(inputRequest);
    if (request.samplingScale != 1 || request.region.width != 1 || request.region.height != 1) {
        throw EvaluationException("a viewport sample is exactly one full-resolution pixel", request.output);
    }
    validateRequest(document, request);
    const std::string colorIdentity = sources_.colorConfigIdentity();
    // The same shared description/plan/key mechanism the viewer render uses, so
    // a sample and the frame beside it can never resolve the authored state
    // differently. Nothing is published: a pick is a read.
    const RegionPlan plan = planDependencyRegions(document, request, *effects_.contributions(), &sources_);
    const auto evaluation = evaluateGpu(document, request, effects_, device_, allocator_, timeout_ns, &reuse_,
                                        &sources_, colorIdentity, &plan);
    const auto composition = evaluation.images.at(request.output);
    if (!hasPrimaryRgb(composition->layout.channels))
        throw EvaluationException("the sampled image has no complete primary RGB channels", request.output);
    // The working raster is the composition's own output: the viewing transform
    // and the presentation projection are deliberately downstream of this point,
    // so what is sampled is what the effect chain produced.
    gpu::ExportStaging staging(device_, allocator_);
    const auto staged = staging.stage(composition->image, composition->layout, timeout_ns);
    return staged.image.pixel(0, 0);
}

CacheCounts ViewerSession::reuseCounts() const {
    return reuse_.counts();
}

void ViewerSession::configureCache(const ViewerCacheOptions& options) {
    std::scoped_lock stateLocks(cacheMutex_, freshnessMutex_);
    if (cache_)
        throw std::logic_error("viewer cache is already configured");
    // No record can outlive the cache it was keyed against: an identity is only
    // meaningful for the storage that produced it.
    forgetRecords();
    auto cache = std::make_unique<ViewerCache>(device_, allocator_, shaderDirectory_);
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

ViewerCache::ForegroundScope ViewerSession::foregroundScope() {
    // No configured cache means no asynchronous writer to gate, so the scope is
    // empty and holding it is a no-op. The gate itself belongs to the cache
    // owner: it holds whatever weak state the writer needs, so a scope that
    // outlives the cache releases nothing rather than touching freed state.
    return cache_ ? cache_->foregroundScope() : ViewerCache::ForegroundScope{};
}

void ViewerSession::retireDestination(ViewerDestination destination) {
    std::lock_guard freshnessLock(freshnessMutex_);
    latestRevisionByDestination_.erase(destination);
    latestGenerationByDestination_.erase(destination);
}

namespace {

// Exact canonical form of a demand's fractional geometry: the value's own bits,
// the pattern the reuse keys already use for canonical numbers. A decimal
// rendering would lose precision and could alias two distinct views.
[[nodiscard]] std::string exactNumberField(const double value) {
    return std::to_string(std::bit_cast<std::uint64_t>(value));
}

}  // namespace

std::string ViewerSession::intentRecordKey(ViewerDestination destination, const ViewIntent& intent) {
    std::string key;
    appendCanonicalField(key, "view-intent", std::to_string(static_cast<std::uint32_t>(destination)));
    appendCanonicalField(key, "network", std::to_string(intent.network));
    appendCanonicalField(key, "target", std::to_string(intent.target));
    appendCanonicalField(key, "local-time", std::to_string(intent.localTime));
    appendCanonicalField(key, "resolution", viewerResolutionName(intent.mode));
    appendCanonicalField(key, "coverage", intent.forceFullFrame ? "whole-frame" : "view");
    appendCanonicalField(key, "zoom", exactNumberField(intent.zoom));
    appendCanonicalField(key, "pan-x", exactNumberField(intent.panX));
    appendCanonicalField(key, "pan-y", exactNumberField(intent.panY));
    appendCanonicalField(key, "viewport-width", exactNumberField(intent.viewportWidth));
    appendCanonicalField(key, "viewport-height", exactNumberField(intent.viewportHeight));
    appendCanonicalField(key, "layer", intent.layer);
    appendCanonicalField(key, "channel", intent.channel);
    return key;
}

std::string ViewerSession::requestRecordKey(ViewerDestination destination, const EvaluationRequest& request) {
    std::string key;
    appendCanonicalField(key, "view-request", std::to_string(static_cast<std::uint32_t>(destination)));
    appendCanonicalField(key, "network", std::to_string(request.network));
    appendCanonicalField(key, "output", std::to_string(request.output));
    appendCanonicalField(key, "local-time", std::to_string(request.localTime));
    appendCanonicalField(key, "region", regionField(request.region));
    appendCanonicalField(key, "domain", std::to_string(request.fullWidth) + "x" + std::to_string(request.fullHeight));
    appendCanonicalField(key, "scale", std::to_string(request.samplingScale));
    appendCanonicalField(key, "quality", qualityName(request.quality));
    appendCanonicalField(key, "channels", channelField(request.channels));
    return key;
}

std::string ViewerSession::frameRecordKey(const EvaluationRequest& request) {
    std::string key;
    appendCanonicalField(key, "frame", std::to_string(request.network));
    appendCanonicalField(key, "target", std::to_string(request.output));
    appendCanonicalField(key, "local-time", std::to_string(request.localTime));
    return key;
}

std::optional<ViewerSession::FrameMatch> ViewerSession::frameRecordForIntent(const ViewIntent& intent,
                                                                             const std::uint64_t revision,
                                                                             const std::string& colorIdentity,
                                                                             ViewerResolutionPolicy& resolution) const {
    EvaluationRequest identity;
    identity.network = intent.network;
    identity.output = intent.target;
    identity.localTime = intent.localTime;
    const auto known = findRecord(frameRecordKey(identity), revision, colorIdentity);
    if (!known)
        return std::nullopt;
    const FrameRecord& record = **known;
    ResolvedView view;
    try {
        view = resolveViewIntent(intent, record.description, resolution);
    } catch (const ViewUnavailable&) {
        return std::nullopt;
    }
    // An empty channel demand means every channel the image names, so it is
    // compared as exactly that set: a concrete request that named no channels
    // and the view that names this image's channels are the same demand.
    EvaluationRequest recorded = record.request;
    if (recorded.channels.empty())
        recorded.channels = record.description.channels;
    if (view.request != recorded)
        return std::nullopt;
    return FrameMatch{*known, view.presentationChannel};
}

std::optional<std::shared_ptr<const ViewerSession::FrameRecord>>
ViewerSession::findRecord(const std::string& key, std::uint64_t revision, const std::string& colorIdentity) const {
    std::lock_guard lock(recordsMutex_);
    const auto found = records_.find(key);
    // A record is only ever the answer for the snapshot it was resolved
    // against: an edited document and a reloaded colour configuration both
    // invalidate the resolution it holds, while a playback tick that only
    // advances local time leaves it valid because local time is part of the key.
    if (found == records_.end() || found->second->documentRevision != revision ||
        found->second->colorIdentity != colorIdentity)
        return std::nullopt;
    return found->second;
}

void ViewerSession::record(const std::string& key, FrameRecord entry) {
    auto stored = std::make_shared<const FrameRecord>(std::move(entry));
    std::lock_guard lock(recordsMutex_);
    // A re-recorded demand replaces its record in place; the bounded index only
    // ever drops the oldest record, which costs one re-resolution of a demand
    // nobody is playing.
    const auto existing = records_.find(key);
    if (existing != records_.end()) {
        existing->second = std::move(stored);
        return;
    }
    records_.emplace(key, std::move(stored));
    recordOrder_.push_back(key);
    while (recordOrder_.size() > kMaxFrameRecords) {
        records_.erase(recordOrder_.front());
        recordOrder_.pop_front();
    }
}

void ViewerSession::forgetRecords() {
    std::lock_guard lock(recordsMutex_);
    records_.clear();
    recordOrder_.clear();
}

ViewerFrame ViewerSession::replayFrame(const ViewerCacheResult& result, const EvaluationRequest& request,
                                       const ImageDescription& description, gpu::ViewerChannel channel,
                                       std::uint64_t requestId, std::uint64_t revision) {
    ViewerFrame frame;
    // Pixels may be shared by equivalent nodes. Rebind their representation to
    // this consumer's validated demand, never the original producer's target.
    // The record already owns its description; replay needs no graph query.
    frame.replay = result.image;
    frame.layout = result.layout;
    frame.description = description;
    frame.request = request;
    frame.revision = revision;
    frame.requestId = requestId;
    frame.cacheHit = true;
    frame.presentationChannel = channel;
    return frame;
}

std::optional<ViewerFrame> ViewerSession::serveRecorded(const FrameRecord& record, const RequestTicket& ticket,
                                                        const gpu::ViewerChannel presentationChannel) {
    if (!cache_ || record.cacheIdentity.empty())
        return std::nullopt;
    const ViewerCacheLookup lookup = cache_->lookup(record.cacheIdentity);
    // A validated representation that is still preparing is NOT a miss and
    // never becomes one: the caller retries instead of rendering the graph.
    if (lookup.state == ViewerCacheState::Loading) {
        throw ViewerReplayPending(record.cacheIdentity,
                                  "viewer replay for this frame is still loading; retry instead of re-rendering");
    }
    if (lookup.state != ViewerCacheState::Ready || !lookup.frame)
        return std::nullopt;
    return replayFrame(*lookup.frame, record.request, record.description, presentationChannel, ticket.requestId,
                       ticket.revision);
}

ViewerSession::RequestTicket ViewerSession::beginRequest(const Document& document, std::uint64_t generation,
                                                         ViewerDestination destination) {
    RequestTicket ticket;
    ticket.requestId = nextRequestId_++;
    ticket.revision = document.stateRevision();
    {
        std::lock_guard lock(freshnessMutex_);
        auto& currentGeneration = generationForLocked(destination);
        if (generation == 0)
            generation = currentGeneration + 1;
        if (generation >= currentGeneration) {
            latestRevisionByDestination_[destination] = ticket.revision;
            currentGeneration = generation;
        }
        ticket.generation = generation;
    }
    if (cache_)
        cache_->supersede(ticket.revision, generation, destination);
    return ticket;
}

std::optional<ViewerFrame> ViewerSession::replay(const ViewIntent& intent, const std::uint64_t snapshotRevision,
                                                 ViewerResolutionPolicy& resolution, ViewerDestination destination) {
    // Replay-only: the caller's snapshot revision and the colour configuration
    // decide whether this session still knows what the demand resolved to, and
    // the cache alone decides whether its representation can be served yet. The
    // document is never fingerprinted here, so a playback tick costs no document
    // walk, and the served frame carries exactly the revision the caller
    // submitted. A replay call is not a request: it never advances a
    // destination's freshness, never supersedes the cache watermark and never
    // takes a generation.
    RequestTicket ticket;
    ticket.requestId = nextRequestId_++;
    ticket.revision = snapshotRevision;
    const std::string colorIdentity = sources_.colorConfigIdentity();
    if (const auto known = findRecord(intentRecordKey(destination, intent), ticket.revision, colorIdentity)) {
        if (const std::optional<ViewerFrame> frame = serveRecorded(**known, ticket, (*known)->presentationChannel))
            return frame;
    }
    // The frame's own record, when the view asks for exactly what it holds: a
    // frame filled through an explicitly requested cache range is replay-ready
    // for the equivalent view without describing or planning anything. A view
    // this frame does not satisfy stays a miss, so a neighbour is never filled
    // by a render.
    if (const auto frameMatch = frameRecordForIntent(intent, ticket.revision, colorIdentity, resolution)) {
        if (const std::optional<ViewerFrame> frame =
                serveRecorded(*frameMatch->record, ticket, frameMatch->presentationChannel))
            return frame;
    }
    return std::nullopt;
}

std::optional<ViewerFrame> ViewerSession::replay(const EvaluationRequest& request, const std::uint64_t snapshotRevision,
                                                 ViewerDestination destination) {
    const auto known = findRecord(requestRecordKey(destination, canonicalizeRequest(request)), snapshotRevision,
                                  sources_.colorConfigIdentity());
    if (!known)
        return std::nullopt;
    RequestTicket ticket;
    ticket.requestId = nextRequestId_++;
    ticket.revision = snapshotRevision;
    return serveRecorded(**known, ticket, (*known)->presentationChannel);
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
    const int samplingScale =
        resolution.resolve(intent.mode, description.format.width, description.format.height,
                           demandPixelAspect(description), intent.viewportWidth, intent.viewportHeight, intent.zoom);
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

    // One identified primary RGB channel of a color-managed layer is isolated
    // in the presentation copy only: the evaluated frame still carries every
    // channel of the layer, so isolating a channel never changes what the graph
    // produced. ALPHA is deliberately not a display isolation: it is not color,
    // and a stored matte belongs to the image's data, so alpha is demanded by
    // name like any other data channel and the displayed matte travels in the
    // RGB of the evaluated image instead of in a fourth component (issue #99).
    // Any other unmatched name is demanded by its exact name as well.
    if (selected != kViewCompositeChannel) {
        bool isolated = false;
        if (resolveViewerProjection({}, request.channels).applyViewingTransform) {
            const std::array<int, 4> roles = rgbaChannelIndices(std::span<const std::string>(&selected, 1));
            for (std::size_t role = 0; role < kIsolatedViewRoles; ++role) {
                if (roles[role] >= 0) {
                    // ViewerChannel: RGBA = 0, then R, G, B in role order.
                    view.presentationChannel = static_cast<gpu::ViewerChannel>(role + 1);
                    isolated = true;
                    break;
                }
            }
        }
        if (!isolated)
            request.channels.assign(1, selected);
    }

    const double pixelAspect = demandPixelAspect(description);
    request.samplingScale = samplingScale;
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
    const std::string colorIdentity = sources_.colorConfigIdentity();
    const std::string recordKey = intentRecordKey(destination, intent);
    const RequestTicket ticket = beginRequest(document, generation, destination);
    // Known valid same-snapshot hit (issue #106): the demand's own record first
    // (the exact view intent), then the FRAME's own record when the view asks
    // for exactly what that frame already holds — the case an explicitly
    // populated cache range creates, whose frames were visited as concrete
    // requests. Either way nothing is described, planned or executed: the
    // resolved representation is looked up directly, which is what lets an
    // ordinary playback tick reach a compressed frame without touching the
    // graph. A recorded demand is authoritative for its view: returning to a
    // frame does not refine it just because another frame moved the Auto
    // hysteresis.
    if (const auto known = findRecord(recordKey, ticket.revision, colorIdentity)) {
        if (const std::optional<ViewerFrame> frame = serveRecorded(**known, ticket, (*known)->presentationChannel))
            return *frame;
    }
    if (const auto frameMatch = frameRecordForIntent(intent, ticket.revision, colorIdentity, resolution)) {
        if (const std::optional<ViewerFrame> frame =
                serveRecorded(*frameMatch->record, ticket, frameMatch->presentationChannel))
            return *frame;
    }
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
    return renderResolved(document, view.request, std::move(described), std::move(recordKey), timeout_ns, ticket,
                          destination, std::move(publicationGuard), view.presentationChannel);
}

ViewerFrame ViewerSession::render(const Document& document, const EvaluationRequest& request, std::uint64_t timeout_ns,
                                  std::uint64_t generation, ViewerDestination destination,
                                  CachePublicationGuard publicationGuard) {
    // The concrete demand is validated before anything else — including the
    // record index — so a malformed request fails with the same precise reason
    // the execution path reports, and a rejected demand can never be looked up.
    validateRequest(document, request);
    const std::string colorIdentity = sources_.colorConfigIdentity();
    // The canonical demand IS this path's identity, so the record index is keyed
    // by it and the same request in the same snapshot is the same
    // representation — never a naked local time or a session-wide stamp.
    const std::string recordKey = requestRecordKey(destination, canonicalizeRequest(request));
    const RequestTicket ticket = beginRequest(document, generation, destination);
    if (const auto known = findRecord(recordKey, ticket.revision, colorIdentity)) {
        if (const std::optional<ViewerFrame> frame = serveRecorded(**known, ticket, (*known)->presentationChannel))
            return *frame;
    }
    return renderResolved(document, request, std::nullopt, std::move(recordKey), timeout_ns, ticket, destination,
                          std::move(publicationGuard), gpu::ViewerChannel::RGBA);
}

ViewerFrame ViewerSession::renderResolved(const Document& document, const EvaluationRequest& inputRequest,
                                          std::optional<ImageDescriptionPlan> described, std::string recordKey,
                                          std::uint64_t timeout_ns, RequestTicket ticket, ViewerDestination destination,
                                          CachePublicationGuard publicationGuard,
                                          gpu::ViewerChannel presentationChannel) {
    // Worker-only contract check for both entry points; a malformed request
    // fails on the caller's thread with a precise reason before any GPU work.
    // The concrete-request entry point has already made this check (before it
    // consulted the record index) and the view-intent entry point is checked
    // here, on the request it resolved from the current frame's description.
    validateRequest(document, inputRequest);
    const EvaluationRequest request = canonicalizeRequest(inputRequest);
    const auto requestId = ticket.requestId;
    const auto revision = ticket.revision;

    std::optional<std::string> identity;
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
        // The same described plan the render path consumes (issue #88): the
        // lookup keys the authored target instead of decoding media or guessing
        // a canvas domain.
        const ResultKey key = queryViewerResultKey(document, request, effects_, colorIdentity, &sources_, &plan);
        identity = cacheIdentity(key, appliedViewing);
        // The demand is resolved: its request and representation identity are
        // recorded before the lookup, so the next tick for it — and a replay
        // call from the playback window — resolves nothing at all (the frame's
        // real description travels with the cache record itself). The record is
        // validated by the snapshot and colour stamps it carries, never by a
        // naked frame number.
        FrameRecord entry;
        entry.request = request;
        entry.description = description;
        entry.cacheIdentity = *identity;
        entry.documentRevision = revision;
        entry.colorIdentity = colorIdentity;
        entry.presentationChannel = presentationChannel;
        // The demand's own key (the view intent or the concrete request it was
        // asked as) and the FRAME IDENTITY key, which both render paths share: a
        // frame filled through an explicit cache range is therefore already
        // replay-ready for the equivalent view without planning it again.
        record(recordKey, entry);
        record(frameRecordKey(request), std::move(entry));
        const ViewerCacheLookup lookup = cache_->lookup(*identity);
        if (lookup.state == ViewerCacheState::Loading) {
            // Not a miss: a validated representation is preparing. The caller
            // waits for the preparation it already has instead of rendering the
            // graph a second time.
            throw ViewerReplayPending(*identity,
                                      "viewer replay for this frame is still loading; retry instead of re-rendering");
        }
        if (lookup.state == ViewerCacheState::Ready && lookup.frame)
            return replayFrame(*lookup.frame, request, description, presentationChannel, requestId, revision);
        // Missing, Failed or Evicted: a genuine miss for THIS representation, so
        // the live path below produces it and hands the display frame to the
        // asynchronous cache again.
    }

    // Shared dependency plan: scene-linear reuse under the evaluator's own
    // ticket; decoded frames flow through SourceSession. Cache preparation is
    // not on this path, so the live frame is returned without waiting for it.
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
    // The LIVE representation: an executed, display-referred float image. This
    // frame is not a replay, so `replay` stays empty and the consumer samples
    // the float image as it always has.
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
        // The publication carries the frame's real description and request
        // alongside the live display image: a replay hit then returns exactly
        // what the frame meant when it was produced, without a description
        // round trip. Storage identity is the one key the viewer looked up.
        frame.cacheQueued = cache_->enqueue(ViewerCachePublication{.identity = *identity,
                                                                   .viewingIdentity = appliedViewing,
                                                                   .localTime = request.localTime,
                                                                   .revision = revision,
                                                                   .generation = ticket.generation,
                                                                   .image = frame.image,
                                                                   .layout = frame.layout,
                                                                   .description = frame.description,
                                                                   .request = request,
                                                                   .destination = destination,
                                                                   .publicationGuard = std::move(publicationGuard)});
    }
    return frame;
}

}  // namespace nemo::eval
