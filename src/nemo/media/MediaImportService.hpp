#pragma once

// Asynchronous media import/probe/preview service (issue #43).
//
// One worker thread turns a Document SourceReference snapshot into validated
// probe metadata plus a bounded, real thumbnail. It consumes only the
// application media contract: stills and '#'/'@' image sequences go through
// the shared image adapter (ImageSource.hpp), clips through the source
// decoder's bounded single-frame read (VideoDecode.hpp). Neither path is
// re-implemented here: format, plane, bit-depth and color-interpretation
// validation stays with the adapters that own it, and the preview is
// display-referred through the existing OCIO viewing transform — never
// viewer-cache replay and never a synthetic image.
//
// Contract:
//   * `submit`, `takeResult` and `cancel` are the only thread-safe entry
//     points; all filesystem, FFmpeg and OpenImageIO work happens on the
//     worker thread. There are no callbacks into a GUI thread.
//   * Results carry the full submitted request, so `requestId`,
//     `sourceKey` and `reference.revision` let a consumer reject a result
//     whose source has since changed.
//   * `maxPending` bounds outstanding sources (queued, decoding, or
//     awaiting collection). A submission for a source that is already
//     outstanding coalesces: the newest reference wins and any unconsumed
//     result for that source is dropped. Submitting a new source at the
//     bound returns false; the caller retries after collecting results.
//   * Failure is data, not an exception: a missing path, malformed file,
//     unsupported format or ambiguous color interpretation fills
//     `error`/`offline` and leaves the probe Failed. `inspectMediaSource`
//     never throws.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/MediaCatalog.hpp"
#include "nemo/core/evaluation/Image.hpp"

namespace nemo::media {

// Plain request. `reference` is a snapshot: the worker never reads Document
// state, it decodes exactly the path/interpretation/revision given here.
struct MediaImportRequest {
    std::uint64_t requestId{0};  // caller identity, echoed in the result
    std::string sourceKey;       // Document::sources key; the coalescing key
    SourceReference reference;   // path, interpretation, and revision snapshot
    ColorPolicy colorPolicy;     // working space + viewer transform to apply
    std::string colorConfig;     // OCIO config path; empty falls back to $OCIO
    int thumbnailWidth{160};     // maximum preview width; both zero = none
    int thumbnailHeight{90};     // maximum preview height, not a forced size
    std::int64_t frame{0};       // source-local time; mapped via reference.frameAt
};

// Plain result. `probe` is a proposal until a caller commits it through the
// catalog command; `thumbnail` is display-referred scene pixels reduced
// inside the requested maximum bounds (null when no thumbnail was requested
// or the viewing transform could not be resolved — see `fallbackReason`).
struct MediaImportResult {
    MediaImportRequest request;  // identity: requestId/sourceKey/revision
    MediaKind kind{MediaKind::Unknown};
    MediaProbeMetadata probe;  // status Ready on success, Failed on error
    double frameRate{0.0};
    double pixelAspect{1.0};
    std::string pixelFormat;  // actual decoded format, e.g. "yuv420p", "half"
    int bitDepth{0};
    // Actual decoded/stream evidence: the selected video stream, the declared
    // codec profile ("" when undeclared), and the validated plane count
    // (0 when no frame was produced). Stills report the image adapter's
    // single interleaved RGBA plane as stream 0, plane 1.
    int streamIndex{-1};
    std::string profile;
    int planeCount{0};
    std::string colorRange;      // Y′CbCr range when the source has one
    std::string chromaLocation;  // chroma siting when the source has one
    bool hardware{false};        // the import worker always decodes software
    std::string fallbackReason;  // measured software reason; empty iff hardware
    bool offline{false};         // the resolved source path does not exist
    std::string error;           // path/format/reason diagnostic; empty on success
    // Display-referred, square-pixel: the raster carries the source's
    // displayed shape (pixelAspect baked in), fitted inside the requested
    // maximum bounds with the source aspect preserved.
    std::shared_ptr<const CpuImage> thumbnail;
};

// Synchronous single-source inspect on the calling thread. Used by smoke
// drivers and by tests; exceptions are converted into `result.error`.
[[nodiscard]] MediaImportResult inspectMediaSource(const MediaImportRequest& request);

class MediaImportService {
public:
    // `maxPending` bounds outstanding sources; zero is clamped to one.
    explicit MediaImportService(std::size_t maxPending = 32);
    ~MediaImportService();
    MediaImportService(const MediaImportService&) = delete;
    MediaImportService& operator=(const MediaImportService&) = delete;

    // Thread-safe, non-blocking. Returns false when the outstanding bound is
    // reached or the service is shutting down. Coalesces per `sourceKey`.
    bool submit(MediaImportRequest request);

    // Thread-safe, non-blocking poll. Returns nullopt when no result is
    // ready; collecting a result releases its outstanding slot.
    [[nodiscard]] std::optional<MediaImportResult> takeResult();

    // Thread-safe, non-blocking. Drops queued work and any unconsumed result
    // for `sourceKey`; a job already decoding is discarded when it finishes.
    void cancel(const std::string& sourceKey);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::media
