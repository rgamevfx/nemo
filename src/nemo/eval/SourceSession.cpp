#include "nemo/eval/SourceSession.hpp"
#include "nemo/core/evaluation/Params.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace nemo::eval {

namespace {

[[noreturn]] void failSource(const Node& node, const std::string& what) {
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
    // Transient decoder: only the open-time evidence (ClipInfo, decode
    // decision) is reported; nothing enters session state. The decision's
    // reason (empty exactly when hardware was selected) is the precise,
    // verbatim downgrade evidence the UI surfaces.
    const auto decoder =
        media::ClipDecoder::open(instance_, device_, allocator_, it->second.path, mediaConvertSpirv_, document.color,
                                 interpretationOverride(it->second.interpretation, "source '" + key + "'"));
    return Probe{decoder->info(), decoder->decision()};
}

SourceSession::DecoderState SourceSession::openState(const Document& document, const Node& node,
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

SourceSession::DecodedFrame SourceSession::acquire(const Document& document, const Node& node, std::int64_t localTime,
                                                   std::uint64_t timeout_ns) {
    const auto sourceParam = node.params.find("source");
    if (sourceParam == node.params.end()) {
        failSource(node, "parameter 'source' (the document source key) is required");
    }
    const std::string& key = sourceParam->second;
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
    const auto runtimeKey = nodeResultKey(document, node, {}, EvaluationRequest{}).canonical;

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
