#include "nemo/eval/SourceSession.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
namespace nemo::eval {

namespace {

[[noreturn]] void failSource(const NodeInstance& node, const std::string& what) {
    throw EvaluationException(describeNode(node) + ": " + what, node.id, node.name);
}

// Strict enum parsing: every value must name a declared member; anything
// else is an error naming the field, the value, and the supported set.
template <typename Enum>
[[nodiscard]] Enum parseField(const std::string& value, const std::map<std::string, Enum>& byName,
                              const std::string& field, const std::string& context, const std::string& supported) {
    const auto it = byName.find(value);
    if (it == byName.end()) {
        throw EvaluationException("source interpretation field '" + field + "' has unsupported value '" + value +
                                  "' (context: " + context + "; supported: " + supported + ")");
    }
    return it->second;
}

// A reference whose resolved path names still-image data (issue #62) takes
// the shared image read path; everything else is a clip. The pattern is
// expanded first so a sequence reference classifies by its frame path.
[[nodiscard]] bool resolvesToImageData(const std::string& pattern, std::int64_t frame) {
    return media::isImagePath(media::resolveFramePath(pattern, frame));
}

}  // namespace

media::ColorOverride SourceSession::interpretationOverride(const std::map<std::string, std::string>& map,
                                                           const std::string& context) {
    media::ColorOverride overrides;
    for (const auto& [field, value] : map) {
        if (field == "transfer") {
            static const std::map<std::string, gpu::MediaTransfer> byName{{"bt709", gpu::MediaTransfer::Bt709},
                                                                          {"srgb", gpu::MediaTransfer::Srgb},
                                                                          {"gamma22", gpu::MediaTransfer::Gamma22},
                                                                          {"gamma28", gpu::MediaTransfer::Gamma28},
                                                                          {"linear", gpu::MediaTransfer::Linear}};
            overrides.transfer = parseField(value, byName, field, context, "bt709, srgb, gamma22, gamma28, linear");
        } else if (field == "primaries") {
            static const std::map<std::string, gpu::MediaPrimaries> byName{{"bt709", gpu::MediaPrimaries::Bt709}};
            overrides.primaries = parseField(value, byName, field, context, "bt709");
        } else if (field == "matrix") {
            static const std::map<std::string, gpu::MediaMatrix> byName{{"bt709", gpu::MediaMatrix::Bt709},
                                                                        {"bt601", gpu::MediaMatrix::Bt601}};
            overrides.matrix = parseField(value, byName, field, context, "bt709, bt601");
        } else if (field == "range") {
            static const std::map<std::string, gpu::MediaYuvRange> byName{{"limited", gpu::MediaYuvRange::Limited},
                                                                          {"full", gpu::MediaYuvRange::Full}};
            overrides.range = parseField(value, byName, field, context, "limited, full");
        } else if (field == "chromaLocation") {
            static const std::map<std::string, gpu::MediaChromaLocation> byName{
                {"left", gpu::MediaChromaLocation::Left}};
            overrides.chromaLocation = parseField(value, byName, field, context, "left");
        } else {
            throw EvaluationException("source interpretation has unknown field '" + field + "' (context: " + context +
                                      "; known fields: transfer, primaries, matrix, range, chromaLocation)");
        }
    }
    return overrides;
}

SourceSession::SourceSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                             const std::filesystem::path& mediaConvertSpirv)
    : instance_(instance), device_(device), allocator_(allocator), mediaConvertSpirv_(mediaConvertSpirv) {}

SourceSession::~SourceSession() = default;

SourceSession::Probe SourceSession::probe(const Document& document, const std::string& key) const {
    const auto it = document.sources.find(key);
    if (it == document.sources.end()) {
        throw EvaluationException("no source reference named '" + key + "' in the document");
    }
    const SourceReference& reference = it->second;
    // A still or image-sequence reference is evidence from the shared image
    // read path (issue #62), not from a clip decoder; frame 0 only expands
    // the pattern for classification.
    if (resolvesToImageData(reference.path, 0)) {
        const media::ImageFrameInfo info = media::probeImageFrame(reference, "source '" + key + "'");
        return Probe{media::ClipInfo{.path = info.path,
                                     .codecName = info.formatName,
                                     .width = info.width,
                                     .height = info.height,
                                     .frameRate = 0.0,
                                     .frameCount = info.sequence ? -1 : 1,
                                     .pixelAspect = info.pixelAspect},
                     media::DecodeDecision{.hardware = false, .reason = "image read (OpenImageIO)"}};
    }
    // Transient decoder: only the open-time evidence (ClipInfo, decode
    // decision) is reported; nothing enters session state. The decision's
    // reason (empty exactly when hardware was selected) is the precise,
    // verbatim downgrade evidence the UI surfaces.
    const auto decoder =
        media::ClipDecoder::open(instance_, device_, allocator_, reference.path, mediaConvertSpirv_, document.color,
                                 interpretationOverride(reference.interpretation, "source '" + key + "'"));
    return Probe{decoder->info(), decoder->decision()};
}

SourceSession::DecoderState SourceSession::openState(const Document& document, const NodeInstance& node,
                                                     const SourceReference& reference) const {
    auto decoder =
        media::ClipDecoder::open(instance_, device_, allocator_, reference.path, mediaConvertSpirv_, document.color,
                                 interpretationOverride(reference.interpretation, "source node '" + node.name + "'"));
    return DecoderState{std::move(decoder), 0};
}

void SourceSession::cachePut(const std::pair<std::string, std::int64_t>& cacheKey,
                             std::shared_ptr<const gpu::Image> image) {
    while (frames_.size() >= kMaxCachedFrames) {
        const auto evicted = frameOrder_.front();
        frameOrder_.pop_front();
        frames_.erase(evicted);
    }
    frames_[cacheKey] = std::move(image);
    frameOrder_.push_back(cacheKey);
}

SourceSession::DecodedFrame SourceSession::acquire(const Document& document, NetworkId network,
                                                   const NodeInstance& node, std::int64_t localTime,
                                                   std::uint64_t timeout_ns) {
    const auto sourceParam = node.params.find("source");
    if (sourceParam == node.params.end()) {
        failSource(node, "parameter 'source' (the document source key) is required");
    }
    const auto* keyValue = std::get_if<std::string>(&sourceParam->second);
    if (keyValue == nullptr || keyValue->empty()) {
        failSource(node, "parameter 'source' (the document source key) must be a non-empty string");
    }
    const std::string& key = *keyValue;
    const auto referenceIt = document.sources.find(key);
    if (referenceIt == document.sources.end()) {
        std::string available;
        for (const auto& [name, unused] : document.sources) {
            (void)unused;
            available += (available.empty() ? "" : ", ") + name;
        }
        failSource(node, "no source reference named '" + key +
                             "' in the document (available: " + (available.empty() ? "none" : available) + ")");
    }
    const SourceReference& reference = referenceIt->second;

    // Persistent time mapping (spec section 4.3):
    // SourceReference::frameAt applies frame = frameOffset +
    // localTime*frameStep and rejects negative or overflowing results —
    // an error, never a wrapped value.
    std::int64_t frame = 0;
    try {
        frame = reference.frameAt(localTime);
    } catch (const std::runtime_error& error) {
        failSource(node, error.what());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // Reuse the core's canonical dependency identity, normalized to a full
    // source frame. Path/interpretation/working-space edits must not hit an
    // old decoder merely because the persistent source key is unchanged.
    EvaluationRequest keyRequest;
    keyRequest.network = network;
    const auto runtimeKey = nodeResultKey(document, node, {}, keyRequest).canonical;

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
        return DecodedFrame{cached->second, static_cast<int>(cached->second->extent().width),
                            static_cast<int>(cached->second->extent().height), frame};
    }

    // Classify the reference once per runtime key (issue #62): a still or
    // image-sequence pattern decodes through the shared image read path, a
    // clip through ClipDecoder. The key changes when the reference path or
    // its interpretation changes, so the memo cannot outlive its reference.
    const auto kindIt = decodeKinds_.find(runtimeKey);
    DecodeKind kind = DecodeKind::Clip;
    if (kindIt != decodeKinds_.end()) {
        kind = kindIt->second;
    } else {
        kind = resolvesToImageData(reference.path, frame) ? DecodeKind::Image : DecodeKind::Clip;
        decodeKinds_.emplace(runtimeKey, kind);
    }

    if (kind == DecodeKind::Image) {
        // Shared source-fill path for stills/sequences: media::readImageFrame
        // already returns scene-linear straight-alpha float32 RGBA. Upload it
        // as one device image and leave it in GENERAL — the layout the
        // executor's decoded-frame hand-off assumes (afterExternalWriteBeforeRead
        // in GpuExecutor.cpp); uploadImage leaves it SHADER_READ_ONLY_OPTIMAL.
        const media::ImageFrame decoded = media::readImageFrame(reference, frame, "source node '" + node.name + "'");
        const int width = decoded.image.width();
        const int height = decoded.image.height();
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
        cachePut(cacheKey, shared);
        return DecodedFrame{std::move(shared), width, height, frame};
    }

    // Open/position a decoder: a request behind the stream position
    // (backwards re-entry, evaluation restart) reopens and decodes forward
    // — the API-permitted seek.
    auto stateIt = decoders_.find(runtimeKey);
    if (stateIt == decoders_.end()) {
        auto state = openState(document, node, reference);
        if (decoders_.size() >= kMaxDecoders) {
            decoders_.erase(decoderOrder_.front());
            decoderOrder_.pop_front();
        }
        stateIt = decoders_.emplace(runtimeKey, std::move(state)).first;
    } else if (frame < stateIt->second.nextFrame) {
        stateIt->second = openState(document, node, reference);
    }
    std::erase(decoderOrder_, runtimeKey);
    decoderOrder_.push_back(runtimeKey);

    // Decode forward to the target frame; every intermediate frame enters
    // the bounded cache (scrubbing and sibling representations reuse it).
    while (true) {
        auto image = stateIt->second.decoder->next(timeout_ns);
        if (image == nullptr) {
            failSource(node, "frame " + std::to_string(frame) + " is past the end of source '" + key + "' (" +
                                 reference.path + "); the stream is exhausted at frame " +
                                 std::to_string(stateIt->second.nextFrame - 1));
        }
        const int width = static_cast<int>(image->extent().width);
        const int height = static_cast<int>(image->extent().height);
        const std::int64_t decodedFrame = stateIt->second.nextFrame++;
        auto shared = std::shared_ptr<const gpu::Image>(std::move(image));
        if (decodedFrame == frame) {
            cachePut(cacheKey, shared);
            return DecodedFrame{std::move(shared), width, height, frame};
        }
        cachePut({runtimeKey, decodedFrame}, std::move(shared));
    }
}

}  // namespace nemo::eval
