#include "nemo/eval/SourceSession.hpp"

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

std::shared_ptr<const gpu::Image> SourceSession::transparentBlack(const int width, const int height,
                                                                  const std::uint64_t timeout_ns) {
    // A Black policy is real transparent source output, not a substituted
    // frame: the raster is cleared on the device and retained per geometry, so
    // repeated black frames neither re-upload host arrays nor re-clear.
    const auto key = std::make_pair(width, height);
    if (const auto found = blackFrames_.find(key); found != blackFrames_.end()) {
        return found->second;
    }
    gpu::Image image = allocator_.create_image(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1,
                                               VK_FORMAT_R32G32B32A32_SFLOAT,
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
    auto shared = std::make_shared<const gpu::Image>(std::move(image));
    blackFrames_.emplace(key, shared);
    return shared;
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
                             std::shared_ptr<const gpu::Image> image, float pixelAspect,
                             const ColorInterpretation color) {
    while (frames_.size() >= kMaxCachedFrames) {
        const auto evicted = frameOrder_.front();
        frameOrder_.pop_front();
        frames_.erase(evicted);
    }
    frames_[cacheKey] = {std::move(image), pixelAspect, color};
    frameOrder_.push_back(cacheKey);
}

SourceSession::DecodedFrame SourceSession::acquire(const Document& document, const EffectiveSourceRequest& source,
                                                   const EvaluationRequest& request, const std::uint64_t timeout_ns) {
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

    std::lock_guard<std::mutex> lock(mutex_);
    // Boundary/missing policy outcome: transparent black is real source output
    // in the requested raster, in the request's own geometry. The cleared image
    // is retained per geometry, so repeated black frames neither re-upload host
    // arrays nor re-clear, and no decode is opened.
    if (source.transparentBlack) {
        const int width = std::max(scaledDimension(request.region.width, request.samplingScale), 1);
        const int height = std::max(scaledDimension(request.region.height, request.samplingScale), 1);
        auto black = transparentBlack(width, height, timeout_ns);
        return DecodedFrame{std::move(black), width, height, frame, 1.0F, ColorInterpretation::SceneLinear};
    }

    // Frame-independent decode identity (core's single owner): every frame of
    // this media with the same interpretation shares one decoder and one
    // classification; the frame itself is added to the frame-cache key below.
    std::string runtimeKey;
    appendSourceDecodeIdentity(runtimeKey, source, document.color.workingSpace, colorConfigIdentity());

    const std::pair<std::string, std::int64_t> cacheKey{runtimeKey, frame};

    // Cached frame (any representation): the same decode serves every
    // sibling representation and evaluation re-entry.
    if (const auto cached = frames_.find(cacheKey); cached != frames_.end()) {
        for (auto it = frameOrder_.begin(); it != frameOrder_.end(); ++it) {
            if (*it == cacheKey) {
                frameOrder_.erase(it);
                frameOrder_.push_back(cacheKey);
                break;
            }
        }
        return DecodedFrame{cached->second.image,
                            static_cast<int>(cached->second.image->extent().width),
                            static_cast<int>(cached->second.image->extent().height),
                            frame,
                            cached->second.pixelAspect,
                            cached->second.color};
    }

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
        // Shared source-fill path for stills/sequences: media::readImageFrame
        // already returns working-space straight-alpha float32 RGBA (or Data for
        // a Raw bypass). Upload it as one device image and leave it in GENERAL —
        // the layout the executor's decoded-frame hand-off assumes
        // (afterExternalWriteBeforeRead in GpuExecutor.cpp); uploadImage leaves
        // it SHADER_READ_ONLY_OPTIMAL.
        const media::ImageFrame decoded =
            media::readImageFrame(*color, colorChoiceOf(source), source.path, frame, context);
        const int width = decoded.image.width();
        const int height = decoded.image.height();
        const float pixelAspect = validatedPixelAspect(decoded.info.pixelAspect, source, context);
        gpu::Image image = allocator_.create_image(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1, VK_FORMAT_R32G32B32A32_SFLOAT,
            // SAMPLED is not used by the executor (it binds this as a storage
            // image), but uploadImage parks the image in
            // SHADER_READ_ONLY_OPTIMAL and every transition through that
            // layout legally requires SAMPLED or INPUT_ATTACHMENT usage.
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            2);
        const std::size_t bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kImageChannels * sizeof(float);
        gpu::uploadImage(device_.submissions(device_.graphics_family()), allocator_, image, decoded.image.data(), bytes,
                         timeout_ns);
        gpu::imageBarrier(
            device_.submissions(device_.graphics_family()), image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, timeout_ns);
        std::shared_ptr<const gpu::Image> shared = std::make_shared<gpu::Image>(std::move(image));
        const ColorInterpretation interpretation = decoded.info.color;
        cachePut(cacheKey, shared, pixelAspect, interpretation);
        return DecodedFrame{std::move(shared), width, height, frame, pixelAspect, interpretation};
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
        const int width = static_cast<int>(image->extent().width);
        const int height = static_cast<int>(image->extent().height);
        const std::int64_t decodedFrame = stateIt->second.nextFrame++;
        auto shared = std::shared_ptr<const gpu::Image>(std::move(image));
        if (decodedFrame == frame) {
            cachePut(cacheKey, shared, pixelAspect, interpretation);
            return DecodedFrame{std::move(shared), width, height, frame, pixelAspect, interpretation};
        }
        cachePut({runtimeKey, decodedFrame}, std::move(shared), pixelAspect, interpretation);
    }
}

}  // namespace nemo::eval
