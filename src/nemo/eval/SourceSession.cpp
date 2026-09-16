#include "nemo/eval/SourceSession.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/ViewingTransform.hpp"

namespace nemo::eval {

namespace {

// A request that reached this session owns its own diagnostics: it is resolved
// per node already, so errors carry the source key rather than a node copy.
[[noreturn]] void failRequest(const EffectiveSourceRequest& source, const std::string& what) {
    throw EvaluationException("source '" + source.sourceKey + "' (" + source.path + "): " + what);
}

// Actual source pixel aspect, validated: a non-finite or non-positive value is
// a node error, never silently replaced by a square-pixel assumption (issue
// #34: the transform honors real anamorphic media).
[[nodiscard]] float validatedPixelAspect(double aspect, const EffectiveSourceRequest& source,
                                         const std::string& context) {
    if (!std::isfinite(aspect) || aspect <= 0.0) {
        failRequest(source, context + " reports an invalid pixel aspect (" + std::to_string(aspect) + ")");
    }
    return static_cast<float>(aspect);
}

// A reference whose path names still-image data (issue #62) takes the shared
// image read path; everything else is a clip. Classification reads the
// reference's own path: a '#'/'@' pattern is an image sequence by
// construction, so an out-of-range or missing frame is reported by the image
// reader (path + authored range) instead of being misclassified as a clip.
[[nodiscard]] bool resolvesToImageData(const std::string& path) {
    return media::isImagePath(path);
}

// The resolved color choice the request carries, in the media vocabulary.
[[nodiscard]] media::InputColorChoice colorChoiceOf(const EffectiveSourceRequest& source) {
    media::InputColorChoice choice;
    choice.mode = source.inputTransform;
    choice.inputColorSpace = source.inputColorSpace;
    choice.alpha = source.alpha;
    choice.hints = source.interpretation;
    choice.nodeHintKeys = source.nodeInterpretationKeys;
    return choice;
}

// The channel planes of a decoded clip frame: clip decoding always produces the
// four contract channels R, G, B, A (alpha opaque), stored as that many
// R32_SFLOAT planes stacked vertically in one device image (issue #90). The
// device image's height is therefore this many times the logical frame height,
// and every logical extent this session reports divides it back out.
constexpr std::uint32_t kVideoPlanes = kImageChannels;

// The description of a frame that occupies one whole raster (issue #88): a
// decoded clip frame covers its extent, so its format and data bounds are that
// raster; a policy-produced cleared frame holds no authored samples at all, so
// it keeps its source's format while `dataBounds` is empty. One builder, so a
// cleared frame and a decoded frame can never describe their shared format
// differently.
[[nodiscard]] ImageDescription rasterDescription(const Region& format, const Region& dataBounds,
                                                 const float pixelAspect, const ColorInterpretation color) {
    return ImageDescription{.format = format,
                            .dataBounds = dataBounds,
                            .pixelAspect = pixelAspect,
                            .channels = {"R", "G", "B", "A"},
                            .precision = Precision::Float32,
                            .association = ImageAssociation::Straight,
                            .color = color};
}

}  // namespace

SourceSession::SourceSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                             const std::filesystem::path& mediaConvertSpirv, std::string ocioConfigPath)
    : instance_(instance), device_(device), allocator_(allocator), mediaConvertSpirv_(mediaConvertSpirv),
      ocioConfigPath_(std::move(ocioConfigPath)) {}

SourceSession::~SourceSession() = default;

std::shared_ptr<const media::OcioConfigSnapshot> SourceSession::snapshotLocked() const {
    if (!snapshot_) {
        try {
            // The effective configuration: the project's authored reference, or
            // the OCIO application default when none is declared.
            snapshot_ = std::make_shared<const media::OcioConfigSnapshot>(ocioConfigPath_);
        } catch (const media::OcioException&) {
            // No usable configuration: the legacy metadata-only policy. A
            // config-backed working space fails explicitly when it resolves.
            return nullptr;
        }
    }
    return snapshot_;
}

void SourceSession::refreshColorConfig() {
    const std::lock_guard lock(colorMutex_);
    // Owner-side generation replacement: drop the lookup references only, so
    // the next use loads the configuration content afresh and rebuilds every
    // working-space cache. A decode already running holds its own shared owner
    // on the previous generation until it completes. The last identity string is
    // left in place (never cleared in place) and simply stops being authoritative.
    snapshot_.reset();
    colors_.clear();
    identityResolved_ = false;
}

std::string SourceSession::colorConfigIdentity() const {
    std::lock_guard lock(colorMutex_);
    if (!identityResolved_) {
        identityResolved_ = true;
        const std::shared_ptr<const media::OcioConfigSnapshot> snapshot = snapshotLocked();
        identity_ = snapshot ? snapshot->identity() : std::string{};
    }
    return identity_;
}

std::shared_ptr<const media::InputColorCache> SourceSession::colorFor(const std::string& workingSpace) const {
    std::lock_guard lock(colorMutex_);
    const auto found = colors_.find(workingSpace);
    if (found != colors_.end()) {
        return found->second;
    }
    // Same snapshot as the identity: new config bytes can never produce pixels
    // under an old identity.
    auto created = std::make_shared<const media::InputColorCache>(
        media::SourceColorPolicy{ocioConfigPath_, workingSpace}, snapshotLocked());
    colors_.emplace(workingSpace, created);
    return created;
}

std::shared_ptr<const gpu::Image> SourceSession::transparentBlack(const std::uint64_t timeout_ns) {
    // Empty data needs a valid binding, not a request-sized source raster.
    // Retain one cleared sample; all other coordinates are transparent outside
    // its coverage. It is stored in the native channel-plane layout like every
    // other decoded frame — one plane per logical channel — so a consumer never
    // needs a second addressing convention for policy-produced frames. The
    // plane count is the widest a projection can address; a source with more
    // declared channels has its extra planes zeroed by the consumer's own
    // channel mapping, exactly as for any other frame, and a cleared frame is
    // zero in every channel either way.
    if (blackFrame_)
        return blackFrame_;
    gpu::Image image = allocator_.create_image(1, kImageChannels, 1, VK_FORMAT_R32_SFLOAT,
                                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                               2);
    const VkClearColorValue transparent{{0.0F, 0.0F, 0.0F, 0.0F}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto& queue = device_.submissions(device_.graphics_family());
    auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(command, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1,
                                 &range);
            gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        },
        {image.retain()}, {}, timeout_ns);
    if (!completion) {
        throw gpu::GpuException(gpu::GpuError::SubmissionTimeout, "transparent-black source submission capacity "
                                                                  "exhausted");
    }
    if (!queue.wait(*completion, timeout_ns)) {
        throw gpu::GpuException(gpu::GpuError::SubmissionTimeout, "transparent-black source did not complete");
    }
    blackFrame_ = std::make_shared<const gpu::Image>(std::move(image));
    return blackFrame_;
}

ImageDescription SourceSession::describe(const Document& document, const EffectiveSourceRequest& source) {
    if (source.policyError) {
        // The shared wording, raised before any pixel work: a request that
        // cannot produce pixels is never given a geometry to plan with.
        failRequest(source, nemo::sourcePolicyProblem(source));
    }
    const ColorInterpretation color =
        source.dataBypass() ? ColorInterpretation::Data : ColorInterpretation::SceneLinear;
    const std::string context = "source '" + source.sourceKey + "'";
    // A still or image-sequence reference is described from its file header
    // through the shared media owner, which also decides the geometry a Black
    // policy frame still inherits. No plane is loaded.
    if (resolvesToImageData(source.path)) {
        const std::shared_ptr<const media::InputColorCache> colors = colorFor(document.color.workingSpace);
        return media::describeSourceImage(*colors, colorChoiceOf(source), source, context);
    }
    // Container metadata must not require a usable pixel conversion backend.
    try {
        const media::ClipInfo info = media::inspectClipHeader(source.path);
        const float pixelAspect = validatedPixelAspect(info.pixelAspect, source, context);
        const Region format{0, 0, std::max(info.width, 0), std::max(info.height, 0)};
        // A Black policy frame holds no authored samples: its format and pixel
        // aspect are the container's, and its data bounds are EMPTY, which is
        // how the contract separates a known-empty image from unavailable
        // geometry.
        const Region dataBounds = source.transparentBlack ? Region{} : format;
        return ImageDescription{.format = format,
                                .dataBounds = dataBounds,
                                .pixelAspect = pixelAspect,
                                .channels = {"R", "G", "B", "A"},
                                .precision = Precision::Float32,
                                .association = ImageAssociation::Straight,
                                .color = color};
    } catch (const EvaluationException&) {
        throw;
    } catch (const std::exception& error) {
        // No container metadata at all: the source's geometry is unknown, which
        // is a failure naming the source and its path — never a placeholder
        // format the plan could mistake for authored geometry.
        failRequest(source, std::string{"the source's metadata is unavailable: "} + error.what());
    }
}

SourceSession::Probe SourceSession::probe(const Document& document, const std::string& key) const {
    const auto it = document.sources.find(key);
    if (it == document.sources.end()) {
        throw EvaluationException("no source reference named '" + key + "' in the document");
    }
    // Source-scoped resolution: the shared reference owns the mapping and its
    // own interpretation, with no node overrides (that is what a Media Bin
    // probe reports). A transient probe carries no committed facts.
    const EffectiveSourceRequest source = resolveSourceRequest(key, it->second, nullptr, 0);
    const std::shared_ptr<const media::InputColorCache> color = colorFor(document.color.workingSpace);
    // A still or image-sequence reference is evidence from the shared image
    // read path (issue #62), not from a clip decoder.
    if (resolvesToImageData(source.path)) {
        const media::ImageFrameInfo info =
            media::probeImageFrame(*color, colorChoiceOf(source), source.path, "source '" + key + "'");
        return Probe{media::ClipInfo{.path = info.path,
                                     .codecName = info.formatName,
                                     .width = info.width,
                                     .height = info.height,
                                     .frameRate = 0.0,
                                     .frameCount = info.sequence ? -1 : 1,
                                     .pixelAspect = info.pixelAspect,
                                     .frameCountQuality = media::FrameCountQuality::Unknown},
                     media::DecodeDecision{.hardware = false, .reason = "image read (OpenImageIO)"}};
    }
    // Transient decoder: only the open-time evidence (ClipInfo, decode
    // decision) is reported; nothing enters session state. The decision's
    // reason (empty exactly when hardware was selected) is the precise,
    // verbatim downgrade evidence the UI surfaces.
    const auto decoder =
        media::ClipDecoder::open(instance_, device_, allocator_, source.path, mediaConvertSpirv_, document.color, {},
                                 media::ClipColorInput{colorChoiceOf(source), color});
    return Probe{decoder->info(), decoder->decision()};
}

SourceSession::DecoderState SourceSession::openState(const Document& document, const EffectiveSourceRequest& source,
                                                     const std::shared_ptr<const media::InputColorCache>& color) const {
    auto decoder = media::ClipDecoder::open(instance_, device_, allocator_, source.path, mediaConvertSpirv_,
                                            document.color, {}, media::ClipColorInput{colorChoiceOf(source), color});
    return DecoderState{std::move(decoder), 0};
}

void SourceSession::cachePut(const std::pair<std::string, std::int64_t>& cacheKey,
                             std::shared_ptr<const gpu::Image> image, const float pixelAspect,
                             const ColorInterpretation color, const ImageDescription& description,
                             const Region& coverage) {
    while (frames_.size() >= kMaxCachedFrames) {
        const auto evicted = frameOrder_.front();
        frameOrder_.pop_front();
        frames_.erase(evicted);
    }
    frames_[cacheKey] = {std::move(image), pixelAspect, color, description, coverage};
    frameOrder_.push_back(cacheKey);
}

std::optional<SourceSession::DecodedFrame>
SourceSession::cachedLocked(const std::pair<std::string, std::int64_t>& cacheKey, const ImageDescription* required) {
    const auto found = frames_.find(cacheKey);
    if (found == frames_.end() || (required != nullptr && !(found->second.description == *required))) {
        return std::nullopt;
    }
    std::erase(frameOrder_, cacheKey);
    frameOrder_.push_back(cacheKey);
    // The retained raster's LOGICAL extents: its device image is stored in the
    // plane layout (height times the channel count), while `coverage` is the
    // geometry consumers sample through, so the cached coverage is the
    // authority for what this frame's width/height mean.
    return DecodedFrame{.image = found->second.image,
                        .width = found->second.coverage.width,
                        .height = found->second.coverage.height,
                        .frame = cacheKey.second,
                        .pixelAspect = found->second.pixelAspect,
                        .color = found->second.color,
                        .description = found->second.description,
                        .coverage = found->second.coverage};
}

SourceSession::DecodedFrame SourceSession::acquire(const Document& document, const EffectiveSourceRequest& source,
                                                   const std::uint64_t timeout_ns) {
    if (source.policyError) {
        // One wording for the failure, shared with the CPU reference: the core
        // helper names the source relationship (boundary vs missing member) and
        // the policy that failed. The evaluator attaches the node identity, and
        // a substituted frame is never an answer.
        failRequest(source, nemo::sourcePolicyProblem(source));
    }
    const std::string& key = source.sourceKey;
    const std::int64_t frame = source.readFrame;
    const std::shared_ptr<const media::InputColorCache> color = colorFor(document.color.workingSpace);
    // Diagnose an invalid working target before any cache lookup: a cached
    // decode must never hide that the requested target has no meaning.
    // Raw/Data is deliberately exempt, exactly as the resolution path exempts it
    // and as core's decode/result identity excludes the working target for it —
    // so a working-target change can never make a Raw read fail (or succeed)
    // depending on cache state.
    if (!source.dataBypass()) {
        try {
            color->requireWorkingTarget();
        } catch (const std::exception& error) {
            // The diagnosis belongs to the source that was requested, not to a
            // bare media exception escaping into the evaluator: the session
            // reports its own contextual error for a target with no meaning.
            failRequest(source, error.what());
        }
    }

    // A Black policy frame still describes itself: the admitted header's format,
    // pixel aspect, channels, association and interpretation, with EMPTY data
    // bounds because a cleared frame authors no samples. The query is
    // header-only and happens before the decode lock, so a black range neither
    // decodes nor holds the decode lock while it reads a header. A source whose
    // metadata cannot be read at all fails by name and path (describe).
    const std::optional<ImageDescription> blackDescription =
        source.transparentBlack ? std::optional<ImageDescription>{describe(document, source)} : std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    // Empty data remains a connected image with its header's logical format.
    // The retained cleared sample has full-resolution coverage independent of
    // preview density; no decoder is opened.
    if (source.transparentBlack) {
        auto black = transparentBlack(timeout_ns);
        const ImageDescription& description = *blackDescription;
        return DecodedFrame{.image = std::move(black),
                            .width = 1,
                            .height = 1,
                            .frame = frame,
                            .pixelAspect = description.pixelAspect,
                            .color = description.color,
                            .description = description,
                            .coverage = Region{0, 0, 1, 1}};
    }

    // Frame-independent decode identity (core's single owner): every frame of
    // this media with the same interpretation shares one decoder and one
    // classification; the frame itself is added to the frame-cache key below.
    std::string runtimeKey;
    appendSourceDecodeIdentity(runtimeKey, source, document.color.workingSpace, colorConfigIdentity());

    const std::pair<std::string, std::int64_t> cacheKey{runtimeKey, frame};

    // Classify the reference once per runtime key (issue #62): a still or
    // image-sequence pattern decodes through the shared image read path, a clip
    // through ClipDecoder. The key changes when the path, the effective request
    // or the config content changes, so the memo cannot outlive its reference.
    const auto kindIt = decodeKinds_.find(runtimeKey);
    DecodeKind kind = DecodeKind::Clip;
    if (kindIt != decodeKinds_.end()) {
        kind = kindIt->second;
    } else {
        kind = resolvesToImageData(source.path) ? DecodeKind::Image : DecodeKind::Clip;
        decodeKinds_.emplace(runtimeKey, kind);
    }

    const std::string context = "source '" + key + "'";
    if (kind == DecodeKind::Image) {
        // The frame's CURRENT header is queried before any cache lookup: a
        // rewritten format, data window or pixel aspect at the same path changes
        // the description, and a raster decoded under the previous header must
        // never be served beneath it (nor reported with the old geometry). The
        // query is header-only, so a hit still costs no decode.
        const media::ImageHeader header = media::inspectImageHeader(media::resolveFramePath(source.path, frame));
        const ImageDescription current = media::describeImageFrame(header, source);
        if (auto reuse = cachedLocked(cacheKey, &current)) {
            return *std::move(reuse);
        }

        // Shared source-fill path for stills/sequences: media::readImageFrame
        // returns working-space float32 samples (or Data for a Raw bypass) in
        // the file's OWN named channels. Upload them as one device image in the
        // native channel-plane layout — channel c of logical pixel (x, y) at
        // (x, y + c*height) — and leave it in GENERAL, the layout the executor's
        // decoded-frame hand-off assumes (afterExternalWriteBeforeRead in
        // GpuExecutor.cpp). No channel is renamed, padded to four or projected
        // here: the frame keeps exactly the channels its description names.
        const media::ImageFrame decoded = media::readImageFrame(*color, colorChoiceOf(source), header, context);
        const int width = decoded.image.width();
        const int height = decoded.image.height();
        const int channels = static_cast<int>(decoded.image.channelCount());
        const float pixelAspect = validatedPixelAspect(decoded.info.pixelAspect, source, context);
        // The description and coverage come from the same read the raster came
        // from, so the retained image can never be bound under a geometry or a
        // meaning its samples do not have.
        const Region coverage = decoded.info.coverage;
        if (coverage.width != width || coverage.height != height) {
            failRequest(source, "decoded raster is " + std::to_string(width) + "x" + std::to_string(height) +
                                    " but its described coverage is " + std::to_string(coverage.width) + "x" +
                                    std::to_string(coverage.height));
        }
        // The plane layout stacks the declared channels vertically, so the
        // device image's height is the logical height times the channel count.
        // Refuse a frame that has no samples or whose planes do not fit the
        // driver's 2D extent limit by naming the frame, instead of letting image
        // creation or the upload fail opaquely.
        if (channels <= 0 || width <= 0 || height <= 0) {
            failRequest(source, "the decoded frame has no samples or declares no channels");
        }
        const auto planeHeight = static_cast<std::uint64_t>(height) * static_cast<std::uint64_t>(channels);
        const std::uint32_t maxExtent = device_.properties().limits.maxImageDimension2D;
        if (static_cast<std::uint64_t>(width) > maxExtent || planeHeight > maxExtent) {
            failRequest(source, "the frame's " + std::to_string(width) + "x" + std::to_string(height) +
                                    " raster with " + std::to_string(channels) + " channels exceeds the device's " +
                                    std::to_string(maxExtent) +
                                    "-pixel image extent limit in the native channel-plane layout");
        }
        gpu::Image image = allocator_.create_image(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(planeHeight), 1, VK_FORMAT_R32_SFLOAT,
            // SAMPLED is not used by the executor (it binds this as a storage
            // image), but every transition through a sampled layout legally
            // requires SAMPLED or INPUT_ATTACHMENT usage.
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            2);
        media::uploadChannelPlanes(device_.submissions(device_.graphics_family()), allocator_, image, decoded.image,
                                   timeout_ns);
        std::shared_ptr<const gpu::Image> shared = std::make_shared<gpu::Image>(std::move(image));
        const ColorInterpretation interpretation = decoded.info.color;
        const ImageDescription& description = decoded.info.description;
        cachePut(cacheKey, shared, pixelAspect, interpretation, description, coverage);
        return DecodedFrame{.image = std::move(shared),
                            .width = width,
                            .height = height,
                            .frame = frame,
                            .pixelAspect = pixelAspect,
                            .color = interpretation,
                            .description = description,
                            .coverage = coverage};
    }

    // A clip frame's geometry comes from its open decode owner, not from a
    // header this session can query cheaply: a container whose geometry changed
    // in place follows the revision/reload contract, which reopens the decoder,
    // exactly as its pixel content does.
    if (auto reuse = cachedLocked(cacheKey, nullptr)) {
        return *std::move(reuse);
    }

    // Open/position a decoder: a request behind the stream position (backwards
    // re-entry, evaluation restart) reopens and decodes forward — the
    // API-permitted seek.
    auto stateIt = decoders_.find(runtimeKey);
    if (stateIt == decoders_.end()) {
        auto state = openState(document, source, color);
        if (decoders_.size() >= kMaxDecoders) {
            decoders_.erase(decoderOrder_.front());
            decoderOrder_.pop_front();
        }
        stateIt = decoders_.emplace(runtimeKey, std::move(state)).first;
    } else if (frame < stateIt->second.nextFrame) {
        stateIt->second = openState(document, source, color);
    }
    std::erase(decoderOrder_, runtimeKey);
    decoderOrder_.push_back(runtimeKey);

    // ClipInfo is stable for the open decoder; validate it once and carry it
    // across every frame this decoder yields.
    const float pixelAspect = validatedPixelAspect(stateIt->second.decoder->info().pixelAspect, source, context);
    const ColorInterpretation interpretation =
        stateIt->second.decoder->inputColor().raw() ? ColorInterpretation::Data : ColorInterpretation::SceneLinear;

    // Decode forward to the target frame; every intermediate frame enters the
    // bounded cache (scrubbing and sibling representations reuse it).
    while (true) {
        auto image = stateIt->second.decoder->next(timeout_ns);
        if (image == nullptr) {
            failRequest(source, "frame " + std::to_string(frame) +
                                    " is past the end of the source; the stream is "
                                    "exhausted at frame " +
                                    std::to_string(stateIt->second.nextFrame - 1));
        }
        const auto planeHeight = image->extent().height;
        const int width = static_cast<int>(image->extent().width);
        const int height = static_cast<int>(planeHeight / kVideoPlanes);
        if (height <= 0 || planeHeight != static_cast<std::uint32_t>(height) * kVideoPlanes) {
            failRequest(source, "decoded frame image is not a channel-plane frame: its extent is " +
                                    std::to_string(image->extent().width) + "x" + std::to_string(planeHeight) +
                                    ", which is not a logical raster with " + std::to_string(kVideoPlanes) +
                                    " stacked channel planes");
        }
        const Region coverage{0, 0, width, height};
        const ImageDescription description = rasterDescription(coverage, coverage, pixelAspect, interpretation);
        const std::int64_t decodedFrame = stateIt->second.nextFrame++;
        auto shared = std::shared_ptr<const gpu::Image>(std::move(image));
        if (decodedFrame == frame) {
            cachePut(cacheKey, shared, pixelAspect, interpretation, description, coverage);
            return DecodedFrame{.image = std::move(shared),
                                .width = width,
                                .height = height,
                                .frame = frame,
                                .pixelAspect = pixelAspect,
                                .color = interpretation,
                                .description = description,
                                .coverage = coverage};
        }
        cachePut({runtimeKey, decodedFrame}, std::move(shared), pixelAspect, interpretation, description, coverage);
    }
}

}  // namespace nemo::eval
