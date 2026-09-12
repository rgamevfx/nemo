#pragma once

// Runtime real-media source state for native evaluation (issue #11, spec
// sections 10.2/10.4).
//
// The persistent model carries source identity and time mapping as plain
// data (Document::sources, set through setSourceCommand) — never GPU or
// decoder objects. This session is the execution layer that owns the
// runtime counterpart: one open decode context per source key plus a
// bounded cache of decoded device-resident frames, consumed by the native
// GPU executor through the SAME dependency plan synthetic fixtures use.
//
// Two decode kinds (issue #62): a clip reference is served by a
// ClipDecoder, and a still image or image-sequence pattern is served by a
// validated media::readImageFrame read uploaded as one scene-linear rgba32f
// device image. The kind is classified from the resolved reference path
// once per runtime key; both kinds produce the SAME DecodedFrame contract
// described below.
//
// Ownership handoff (issue #11): the decoded frame is GPU-complete on
// return (every decode path waits its own completion) and GENERAL-laid-out,
// so the executor binds it directly; the executor retains the frame with
// its submission until the consuming effect batch completes. Eviction here
// can therefore never free in-flight work.
//
// Time mapping: frame = frameOffset + localTime * frameStep. Overflowing or
// negative frames are rejected — never silently clamped.
//
// Backwards re-entry: ClipDecoder decodes forward only, so a request
// behind the stream position reopens the clip and decodes forward to the
// target (the API-permitted seek). Both the open-decoder count and the
// decoded-frame cache are bounded; eviction is least-recently-used.
//
// Thread-confined decoders: all decode state is serialized on one mutex;
// `probe` needs no lock, reading the reference directly (a transient
// decoder for a clip, the shared image read for a still/sequence).

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/VideoDecode.hpp"

namespace nemo::eval {

class SourceSession {
public:
    // One decoded source frame handed to the executor. Scene-linear
    // (project working space), full resolution, rgba32f, straight alpha,
    // GENERAL layout, GPU-complete.
    struct DecodedFrame {
        std::shared_ptr<const gpu::Image> image;
        int width{0};
        int height{0};
        std::int64_t frame{0};
    };

    // `mediaConvertSpirv` is the compiled mediaConvert kernel (the
    // Vulkan-resident decode path). Throws media::MediaDecodeError from
    // ClipDecoder::open when a clip cannot be opened or its interpretation
    // is outside the supported subset; unsupported hardware is NOT an
    // error — it downgrades to the measured software path.
    SourceSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& mediaConvertSpirv);
    ~SourceSession();
    SourceSession(const SourceSession&) = delete;
    SourceSession& operator=(const SourceSession&) = delete;

    // Resolves `node`'s `source` parameter against document.sources within
    // the explicit network scope, maps frame = frameOffset + localTime*frameStep
    // (rejecting overflow and negative frames), parses the reference's
    // interpretation map strictly into a ColorOverride, and returns the
    // decoded frame for the mapped frame.
    [[nodiscard]] DecodedFrame acquire(const Document& document, NetworkId network, const NodeInstance& node,
                                       std::int64_t localTime, std::uint64_t timeout_ns);

    // Decode-path evidence for `key`'s reference without touching session
    // decode state: opens a transient decoder and reports its ClipInfo plus
    // the measured DecodeDecision (empty reason exactly when the hardware
    // path was selected). A still/sequence reference instead reports the
    // image read: hardware false with reason "image read (OpenImageIO)".
    // Thread-safe against concurrent acquire calls.
    struct Probe {
        media::ClipInfo info;
        media::DecodeDecision decision;
    };
    [[nodiscard]] Probe probe(const Document& document, const std::string& key) const;

    // Strict parsing of a SourceReference interpretation map into the
    // media module's ColorOverride. An EMPTY map means strict stream tags:
    // the decoder resolves the declared color metadata and ambiguous or
    // unsupported metadata is an error, never a silent guess. A non-empty
    // map fills only UNSPECIFIED fields (a stream-tagged field keeps its
    // declared value — the stream is authoritative). Unknown fields or
    // values are rejected with `context` in the message.
    [[nodiscard]] static media::ColorOverride interpretationOverride(const std::map<std::string, std::string>& map,
                                                                     const std::string& context);

private:
    // One open source: sequential decoder plus the frame its next next()
    // call returns (ClipDecoder has no seek — backwards re-entry reopens).
    struct DecoderState {
        std::unique_ptr<media::ClipDecoder> decoder;
        std::int64_t nextFrame{0};
    };

    // Which shared source-fill path a runtime key resolves to (issue #62):
    // classified from the resolved reference path and memoized, because the
    // runtime key already changes whenever the reference path or its
    // interpretation changes.
    enum class DecodeKind { Clip, Image };

    [[nodiscard]] DecoderState openState(const Document& document, const NodeInstance& node,
                                         const SourceReference& reference) const;

    // Inserts a decoded frame into the bounded least-recently-used cache.
    // target frame shares ownership with the returned DecodedFrame.
    void cachePut(const std::pair<std::string, std::int64_t>& cacheKey, std::shared_ptr<const gpu::Image> image);

    gpu::Instance& instance_;
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::filesystem::path mediaConvertSpirv_;

    // Bounded runtime state. kMaxDecoders bounds open decoder contexts
    // (decode queues and NVDEC surfaces are the expensive resource);
    // kMaxCachedFrames bounds resident decoded frames.
    static constexpr std::size_t kMaxDecoders = 4;
    static constexpr std::size_t kMaxCachedFrames = 4;
    mutable std::mutex mutex_;
    std::map<std::string, DecoderState> decoders_;
    std::deque<std::string> decoderOrder_;           // LRU: front = least recently used
    std::map<std::string, DecodeKind> decodeKinds_;  // memoized, guarded by mutex_
    std::map<std::pair<std::string, std::int64_t>, std::shared_ptr<const gpu::Image>> frames_;
    std::deque<std::pair<std::string, std::int64_t>> frameOrder_;  // LRU, same convention
};

}  // namespace nemo::eval
