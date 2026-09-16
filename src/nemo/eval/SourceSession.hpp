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
// validated media::readImageFrame read uploaded as one device image in the
// shared native layout, carrying the file's own named channels (issue #98).
// The kind is classified from the resolved reference path
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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/InputColor.hpp"
#include "nemo/media/VideoDecode.hpp"

namespace nemo::eval {

class SourceSession : public SourceDescriptionProvider {
public:
    // One decoded source frame handed to the executor: scene-linear
    // (project working space) or Data for a Raw bypass, full resolution,
    // association from description, GENERAL layout, GPU-complete. Its device
    // image is always the shared native layout of its own stored channel count
    // (issue #98): four channels are a packed RGBA32F image at the coverage
    // extent, one texel per logical pixel; any other count is an R32_SFLOAT
    // image of extent (coverage.width, coverage.height * description.channels
    // .size()), plane c of logical pixel (x, y) at (x, y + c*coverage.height).
    // Channels stay in `description.channels` order. A still/sequence frame
    // carries the file's own named channels; a clip frame carries R, G, B, A.
    struct DecodedFrame {
        std::shared_ptr<const gpu::Image> image;
        // LOGICAL storage extents of the retained raster, exactly `coverage`'s
        // extent: the device image's height is this times its plane count.
        int width{0};
        int height{0};
        std::int64_t frame{0};
        // Actual source pixel aspect (display width / height of one pixel),
        // carried from the decode metadata so downstream effect coordinate
        // math honors anamorphic media (issue #34 transform). 1.0 for
        // square-pixel sources; the image itself carries no metadata.
        float pixelAspect{1.0F};
        // Interpretation of the decoded pixels: SceneLinear working-space
        // samples, or Data when the request bypassed color conversion (issue
        // #81). A consumer must not assume every non-display-referred frame is
        // managed scene-linear.
        ColorInterpretation color{ColorInterpretation::SceneLinear};
        // What the retained raster means (issue #88): its logical format, signed
        // data bounds, pixel aspect, channels, precision, association and
        // interpretation. A policy-cleared frame retains the admitted header's
        // logical format with empty data bounds.
        ImageDescription description;
        // Where the retained raster actually sits: the signed full-resolution
        // origin of raster pixel (0, 0) plus its extent, in the frame's own
        // normalized coordinates. A windowed source's data raster does not
        // start at the format origin — anchor sampling on this geometry, never
        // on a request-global canvas. `width`/`height` are this extent.
        Region coverage;
    };

    // `mediaConvertSpirv` is the compiled mediaConvert kernel (the
    // Vulkan-resident decode path). Throws media::MediaDecodeError from
    // ClipDecoder::open when a clip cannot be opened or its interpretation
    // is outside the supported subset; unsupported hardware is NOT an
    // error — it downgrades to the measured software path.
    SourceSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& mediaConvertSpirv, std::string ocioConfigPath = {});
    ~SourceSession();
    SourceSession(const SourceSession&) = delete;
    SourceSession& operator=(const SourceSession&) = delete;

    // Returns the decoded frame for one RESOLVED effective source request:
    // mapping, selected coverage, boundary/missing policy outcome and the
    // authored color choices are decided once by the core resolver, and this
    // session resolves the RGB input color through the shared media owner
    // against the project configuration it was constructed with. A request
    // whose policy resolved to transparent black yields a real cleared
    // transparent-black raster in the requested geometry; a policy error is
    // refused here rather than decoding a substituted frame.
    [[nodiscard]] DecodedFrame acquire(const Document& document, const EffectiveSourceRequest& source,
                                       std::uint64_t timeout_ns);

    // Header-only description of the frame this request resolves to (issue
    // #88). No decode, no upload, no readback: a still/sequence frame is
    // described from its file header, and a clip from its container metadata
    // (ClipInfo), which is what the decode would report. A request whose policy
    // requires failure is refused with the shared core wording, and a source
    // whose authored geometry cannot be read at all is described as unknown
    // geometry rather than given an invented format. Never touches session
    // decode state, so it is safe on a planning thread while frames decode.
    [[nodiscard]] ImageDescription describe(const Document& document, const EffectiveSourceRequest& source) override;

    // OCIO content identity of the configuration this session resolves source
    // color against ("" when no configuration is available). The GPU executor
    // mixes it into source-node reuse keys, so a config edited in place can
    // never serve a stale decoded frame. The identity and every retained
    // processor are refreshed when the configuration's content changes.
    // Owned: the identity is recomputed per generation, so a caller never holds
    // a view into storage this session may replace.
    [[nodiscard]] std::string colorConfigIdentity() const;

    // Explicit refresh boundary for the color configuration. The owner that
    // replaced the project or deliberately reloaded the configuration calls
    // this so the retained identity, the retained OCIO processors and every
    // dependent source key are recomputed from the new content — the same
    // boundary ViewerRuntime and the headless entry points observe. Nothing
    // polls the configuration: a same-path content change is seen at this
    // boundary, never by a background watcher. Already-running decodes keep
    // their own handle on the previous generation until they finish.
    void refreshColorConfig();

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

    [[nodiscard]] DecoderState openState(const Document& document, const EffectiveSourceRequest& source,
                                         const std::shared_ptr<const media::InputColorCache>& color) const;

    // Retained input-color context for one working space; a sequence or clip
    // decodes many frames through a single validated OCIO processor.
    // Shared owner of the retained color state for one working space: the caller
    // holds it for as long as it uses the cache, so a refresh only drops this
    // lookup reference.
    [[nodiscard]] std::shared_ptr<const media::InputColorCache> colorFor(const std::string& workingSpace) const;

    // The retained snapshot (nullptr when no configuration is available).
    // Requires colorMutex_.
    [[nodiscard]] std::shared_ptr<const media::OcioConfigSnapshot> snapshotLocked() const;

    // One cleared full-resolution sample in the native layout of a description
    // with `channels` stored channels (packed RGBA32F for four, scalar planes
    // otherwise), retained under mutex_ and cached per count.
    [[nodiscard]] std::shared_ptr<const gpu::Image> transparentBlack(std::uint32_t channels, std::uint64_t timeout_ns);

    // Inserts a decoded frame into the bounded least-recently-used cache.
    // target frame shares ownership with the returned DecodedFrame. The
    // frame's actual pixel aspect is cached alongside it so a reused raster
    // reports the same source metadata.
    // One retained decoded raster plus the metadata a reuse must report with
    // it: the source pixel aspect, the frame's interpretation, and the
    // description/coverage the frame was decoded under — a reused raster that
    // forgot either would describe the same pixels differently.
    struct CachedFrame {
        std::shared_ptr<const gpu::Image> image;
        float pixelAspect{1.0F};
        ColorInterpretation color{ColorInterpretation::SceneLinear};
        ImageDescription description;
        Region coverage;
    };

    void cachePut(const std::pair<std::string, std::int64_t>& cacheKey, std::shared_ptr<const gpu::Image> image,
                  float pixelAspect, ColorInterpretation color, const ImageDescription& description,
                  const Region& coverage);

    // Serves one cached frame and marks it most recently used, or nothing when
    // the frame is not cached. `required` (when supplied) must equal the cached
    // frame's description: a raster decoded under a different header — a changed
    // format, data bounds or pixel aspect — is never served for the current one,
    // while pixel-only edits stay under the explicit revision/reload contract
    // because a description cannot observe them. Requires mutex_.
    [[nodiscard]] std::optional<DecodedFrame> cachedLocked(const std::pair<std::string, std::int64_t>& cacheKey,
                                                           const ImageDescription* required);

    gpu::Instance& instance_;
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::filesystem::path mediaConvertSpirv_;
    std::string ocioConfigPath_;

    // Color state is separate from decode state: the identity query runs on
    // caller threads, while decode is serialized on mutex_.
    mutable std::mutex colorMutex_;
    // ONE retained configuration snapshot for this session generation: the
    // identity the executor mixes into reuse keys and the snapshot every color
    // cache (and therefore every decode) is built from are the same bytes.
    mutable std::shared_ptr<const media::OcioConfigSnapshot> snapshot_;
    mutable std::map<std::string, std::shared_ptr<const media::InputColorCache>> colors_;
    mutable bool identityResolved_{false};
    mutable std::string identity_;
    // Cleared samples by stored channel count: the native layout of a frame
    // depends on that count, so one cleared image cannot stand for every
    // description (issue #98).
    std::map<std::uint32_t, std::shared_ptr<const gpu::Image>> blackFrames_;

    // Bounded runtime state. kMaxDecoders bounds open decoder contexts
    // (decode queues and NVDEC surfaces are the expensive resource);
    // kMaxCachedFrames bounds resident decoded frames.
    static constexpr std::size_t kMaxDecoders = 4;
    static constexpr std::size_t kMaxCachedFrames = 4;
    mutable std::mutex mutex_;
    std::map<std::string, DecoderState> decoders_;
    std::deque<std::string> decoderOrder_;           // LRU: front = least recently used
    std::map<std::string, DecodeKind> decodeKinds_;  // memoized, guarded by mutex_
    // Cached image plus the metadata it must be reported with: its actual source
    // pixel aspect, interpretation and description/coverage. An image frame is
    // only served when the frame's CURRENT header still describes it, so an
    // in-place format/data-window/aspect rewrite never returns a stale raster or
    // its old geometry.
    std::map<std::pair<std::string, std::int64_t>, CachedFrame> frames_;
    std::deque<std::pair<std::string, std::int64_t>> frameOrder_;  // LRU, same convention
};

}  // namespace nemo::eval
