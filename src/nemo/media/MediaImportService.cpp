#include "nemo/media/MediaImportService.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/ViewingTransform.hpp"

#if defined(NEMO_MEDIA_FFMPEG)
#include "nemo/media/VideoDecode.hpp"
#endif

namespace nemo::media {

namespace {

// Mirrors ViewerReferenceDecoder's declared bound so a preview request can
// never ask for an unbounded allocation.
constexpr int kMaxThumbnailDimension = 8192;

// Internal control-flow failure: converted to `result.error`, never thrown
// out of inspectMediaSource.
struct ImportFailure {
    std::string message;
};

[[noreturn]] void failImport(std::string message) {
    throw ImportFailure{std::move(message)};
}

// Name the authored pattern when the resolved frame path differs, so a
// missing sequence frame is reported against the pattern the user authored.
[[nodiscard]] std::string sourceDiagnostic(const SourceReference& reference, const std::string& resolved) {
    return resolved == reference.path ? reference.path : reference.path + " (frame path " + resolved + ")";
}

[[nodiscard]] std::string imageTransferName(const ImageTransfer transfer) {
    switch (transfer) {
    case ImageTransfer::Linear:
        return "linear";
    case ImageTransfer::Srgb:
        return "srgb";
    case ImageTransfer::Gamma22:
        return "gamma22";
    case ImageTransfer::Gamma28:
        return "gamma28";
    case ImageTransfer::Bt709:
        return "bt709";
    }
    return {};
}

inline constexpr const char* kImagePrimariesName = "bt709";  // ImagePrimaries::Rec709

// Native OpenImageIO precision name (ImageIO's typeName vocabulary) to the
// contract's per-channel bit depth.
[[nodiscard]] int bitDepthFromPrecision(const std::string& precision) {
    if (precision == "uint8" || precision == "int8") {
        return 8;
    }
    if (precision == "uint16" || precision == "int16" || precision == "half") {
        return 16;
    }
    if (precision == "uint32" || precision == "int32" || precision == "float") {
        return 32;
    }
    if (precision == "double") {
        return 64;
    }
    return 0;  // unknown precision is reported as 0, never guessed
}

// Aspect-preserving fit inside the requested MAXIMUM bounds for the still
// preview (the clip preview is fitted by the decoder's conversion owner from
// the decoded frame). The displayed image is the source raster widened by
// pixelAspect (an anamorphic source displays wider than its pixel
// dimensions). The preview raster is square-pixel and carries that displayed
// shape, so a consumer that only stretches the raster (QML Image
// PreserveAspectFit) cannot distort it.
struct PreviewSize {
    int width;
    int height;
};

[[nodiscard]] PreviewSize previewSize(const int sourceWidth, const int sourceHeight, const double pixelAspect,
                                      const int maxWidth, const int maxHeight) {
    const double aspect = (static_cast<double>(std::max(sourceWidth, 1)) * std::max(pixelAspect, 1e-6)) /
                          static_cast<double>(std::max(sourceHeight, 1));
    // Bounds are ints; an extreme source aspect must not overflow the cast.
    const auto rounded = [](const double value) {
        return static_cast<int>(std::llround(std::clamp(value, 0.0, 1.0e9)));
    };
    int width = maxWidth;
    int height = rounded(static_cast<double>(maxWidth) / aspect);
    if (height > maxHeight) {
        height = maxHeight;
        width = rounded(static_cast<double>(maxHeight) * aspect);
    }
    return {std::clamp(width, 1, maxWidth), std::clamp(height, 1, maxHeight)};
}

// Bounded point-sample downsample onto the preview raster. Output (x, y)
// reads the source pixel at the center of its fill-ratio cell — the same
// integer mapping convertDecodedFrame applies for a bounded clip decode, so
// a still and a clip of the same size produce the same sampling.
[[nodiscard]] CpuImage downsampleTo(const CpuImage& source, int width, int height) {
    if (source.width() <= 0 || source.height() <= 0) {
        failImport("media import: decoded frame has no pixels");
    }
    ImageLayout layout = source.layout();
    layout.width = width;
    layout.height = height;
    // The preview raster is square-pixel; the displayed shape is baked into
    // its dimensions by previewSize().
    layout.pixelAspect = 1.0F;
    CpuImage output(layout);
    constexpr std::int64_t kMinIndex = 0;
    for (int y = 0; y < height; ++y) {
        const int sourceY = static_cast<int>(std::clamp(((static_cast<std::int64_t>(y) * 2 + 1) * source.height()) /
                                                            (2 * static_cast<std::int64_t>(height)),
                                                        kMinIndex, static_cast<std::int64_t>(source.height() - 1)));
        for (int x = 0; x < width; ++x) {
            const int sourceX = static_cast<int>(std::clamp(((static_cast<std::int64_t>(x) * 2 + 1) * source.width()) /
                                                                (2 * static_cast<std::int64_t>(width)),
                                                            kMinIndex, static_cast<std::int64_t>(source.width() - 1)));
            output.setPixel(x, y, source.pixel(sourceX, sourceY));
        }
    }
    return output;
}

// Display-referred preview through the project's viewing transform. A
// requested thumbnail that cannot be resolved is reported, not substituted
// with an untransformed or synthetic image.
[[nodiscard]] bool toDisplayReferred(CpuImage& image, const MediaImportRequest& request, MediaImportResult& result) {
    try {
        applyViewingTransformCpu(image, request.colorConfig, request.colorPolicy);
        return true;
    } catch (const std::exception& error) {
        result.fallbackReason += "; thumbnail unavailable: ";
        result.fallbackReason += error.what();
        return false;
    }
}

void validateRequest(const MediaImportRequest& request, bool& wantsThumbnail) {
    if (request.reference.path.empty()) {
        failImport("media import: source reference has no path");
    }
    if (request.thumbnailWidth < 0 || request.thumbnailHeight < 0) {
        failImport("media import: thumbnail dimensions must not be negative");
    }
    // `frame` is a source-local time, not a file index: a reference's
    // offset/step decides which frames are valid, and frameAt reports a
    // negative mapped frame or an overflow with its own diagnostic.
    wantsThumbnail = request.thumbnailWidth > 0 || request.thumbnailHeight > 0;
    if (!wantsThumbnail) {
        return;
    }
    if (request.thumbnailWidth <= 0 || request.thumbnailHeight <= 0) {
        failImport("media import: thumbnail width and height must both be positive or both zero");
    }
    if (request.thumbnailWidth > kMaxThumbnailDimension || request.thumbnailHeight > kMaxThumbnailDimension) {
        failImport("media import: thumbnail dimensions exceed the " + std::to_string(kMaxThumbnailDimension) +
                   " pixel bound");
    }
}

void importStill(const MediaImportRequest& request, const std::int64_t sourceFrame, const bool wantsThumbnail,
                 MediaImportResult& result) {
    const std::string context = "source '" + request.sourceKey + "'";
    // Header facts for the requested source-local time and the strict
    // interpretation rules come from the image adapter; this service does not
    // re-validate color metadata.
    const ImageFrameInfo info = probeImageFrame(request.reference, context, request.frame);

    MediaProbeMetadata probe;
    probe.width = info.width;
    probe.height = info.height;
    probe.duration = info.sequence ? 0 : 1;
    probe.codec = info.formatName;
    probe.colorPrimaries = kImagePrimariesName;
    probe.colorTransfer = imageTransferName(info.transfer);
    probe.provenance = "oiio";
    probe.status = MediaProbeStatus::Ready;

    result.kind = info.sequence ? MediaKind::Sequence : MediaKind::Image;
    result.pixelAspect = info.pixelAspect;
    result.pixelFormat = info.nativePrecision;
    result.bitDepth = bitDepthFromPrecision(info.nativePrecision);
    // The image adapter's read is one interleaved RGBA plane from a single
    // image; there is no container stream or codec profile.
    result.streamIndex = 0;
    result.planeCount = 1;
    result.fallbackReason = "software: still/sequence read on the CPU reference path (OpenImageIO)";
    result.probe = std::move(probe);

    if (!wantsThumbnail) {
        return;
    }
    // readImageFrame returns scene-linear straight-alpha pixels; the preview
    // is reduced first and then taken to display through the viewing
    // transform, exactly like the clip path.
    const ImageFrame decoded = readImageFrame(request.reference, sourceFrame, context);
    // The decoded read is authoritative for the frame actually produced.
    result.probe.width = decoded.info.width;
    result.probe.height = decoded.info.height;
    result.pixelFormat = decoded.info.nativePrecision;
    result.bitDepth = bitDepthFromPrecision(decoded.info.nativePrecision);
    const PreviewSize preview = previewSize(decoded.image.width(), decoded.image.height(), decoded.info.pixelAspect,
                                            request.thumbnailWidth, request.thumbnailHeight);
    CpuImage thumb = downsampleTo(decoded.image, preview.width, preview.height);
    if (toDisplayReferred(thumb, request, result)) {
        result.thumbnail = std::make_shared<const CpuImage>(std::move(thumb));
    }
}

#if defined(NEMO_MEDIA_FFMPEG)

[[nodiscard]] std::string mediaTransferName(const gpu::MediaTransfer transfer) {
    switch (transfer) {
    case gpu::MediaTransfer::Bt709:
        return "bt709";
    case gpu::MediaTransfer::Srgb:
        return "srgb";
    case gpu::MediaTransfer::Gamma22:
        return "gamma22";
    case gpu::MediaTransfer::Gamma28:
        return "gamma28";
    case gpu::MediaTransfer::Linear:
        return "linear";
    }
    return {};
}

[[nodiscard]] std::string mediaMatrixName(const gpu::MediaMatrix matrix) {
    switch (matrix) {
    case gpu::MediaMatrix::Bt709:
        return "bt709";
    case gpu::MediaMatrix::Bt601:
        return "bt601";
    }
    return {};
}

[[nodiscard]] std::string mediaRangeName(const gpu::MediaYuvRange range) {
    return range == gpu::MediaYuvRange::Limited ? "limited" : "full";
}

[[nodiscard]] std::string mediaChromaName(const gpu::MediaChromaLocation) {
    return "left";  // the only supported siting
}

void importClip(const MediaImportRequest& request, const std::int64_t sourceFrame, const std::string& resolved,
                const bool wantsThumbnail, MediaImportResult& result) {
    const std::string context = "source '" + request.sourceKey + "'";
    const ColorOverride overrides = colorOverrideFromInterpretation(request.reference.interpretation, context);
    // One frame, bounded by the requested preview MAXIMA: the decoder's
    // conversion owner fits the decoded frame's displayed shape and returns
    // an already square-pixel preview, while `clip.info` keeps the native
    // geometry. No second resize happens here. The decoder also validates
    // the actual decoded format, planes, dimensions and bit depth and
    // resolves the declared color metadata; earlier frames are discarded as
    // they decode.
    const int maxWidth = wantsThumbnail ? request.thumbnailWidth : 0;
    const int maxHeight = wantsThumbnail ? request.thumbnailHeight : 0;
    SoftwareClip clip = decodeClipFrameSoftware(request.reference.path, sourceFrame, maxWidth, maxHeight,
                                                request.colorPolicy, overrides);
    if (clip.frames.empty()) {
        failImport("media import: " + sourceDiagnostic(request.reference, resolved) + ": no frame at source frame " +
                   std::to_string(sourceFrame) + " (source time " + std::to_string(request.frame) + ")");
    }

    MediaProbeMetadata probe;
    probe.width = clip.info.width;
    probe.height = clip.info.height;
    probe.duration = clip.info.frameCount > 0 ? clip.info.frameCount : 0;
    probe.codec = clip.info.codecName;
    probe.colorPrimaries = kImagePrimariesName;  // the supported primaries are Rec.709
    probe.colorTransfer = mediaTransferName(clip.metadata.transfer);
    probe.colorMatrix = mediaMatrixName(clip.metadata.matrix);
    probe.provenance = "ffmpeg-software";
    probe.status = MediaProbeStatus::Ready;

    result.kind = MediaKind::Video;
    result.frameRate = clip.info.frameRate;
    result.pixelAspect = clip.info.pixelAspect;
    result.pixelFormat = clip.pixelFormat;
    result.bitDepth = clip.metadata.bitDepth;
    result.streamIndex = clip.streamIndex;
    result.profile = clip.profile;
    result.planeCount = clip.planeCount;
    result.colorRange = mediaRangeName(clip.metadata.range);
    result.chromaLocation = mediaChromaName(clip.metadata.chromaLocation);
    result.fallbackReason =
        "software: import/probe decodes on the CPU reference path (no GPU device owned by the import worker)";
    result.probe = std::move(probe);

    if (!wantsThumbnail) {
        return;
    }
    // Already bounded and square-pixel; only the display transform remains.
    CpuImage thumb = std::move(clip.frames.front());
    if (toDisplayReferred(thumb, request, result)) {
        result.thumbnail = std::make_shared<const CpuImage>(std::move(thumb));
    }
}

#endif  // NEMO_MEDIA_FFMPEG

}  // namespace

MediaImportResult inspectMediaSource(const MediaImportRequest& request) {
    MediaImportResult result;
    result.request = request;
    result.probe.status = MediaProbeStatus::Failed;
    try {
        bool wantsThumbnail = false;
        validateRequest(request, wantsThumbnail);

        // `frame` is a source-local time: map it through the reference's
        // offset/step exactly once, then use the mapped source frame for
        // existence, classification, decode, and diagnostics. A sequence
        // reference with frameOffset 1001 therefore resolves local time 0 to
        // source frame 1001, not to a literal file "0000".
        std::int64_t sourceFrame = 0;
        try {
            sourceFrame = request.reference.frameAt(request.frame);
        } catch (const std::exception& error) {
            failImport("media import: source time " + std::to_string(request.frame) + " failed: " + error.what());
        }
        const std::string resolved = resolveFramePath(request.reference.path, sourceFrame);
        if (!std::filesystem::exists(resolved)) {
            result.offline = true;
            failImport("media import: source path does not exist: " + sourceDiagnostic(request.reference, resolved));
        }

        if (isImagePath(resolved)) {
            importStill(request, sourceFrame, wantsThumbnail, result);
        } else {
#if defined(NEMO_MEDIA_FFMPEG)
            importClip(request, sourceFrame, resolved, wantsThumbnail, result);
#else
            failImport("media import: " + resolved +
                       " is a video clip; clip import requires the FFmpeg/GPU build (NEMO_BUILD_GPU=ON)");
#endif
        }
    } catch (const ImportFailure& failure) {
        result.error = failure.message;
        result.probe.status = MediaProbeStatus::Failed;
    } catch (const std::exception& error) {
        // MediaDecodeError / ImageIoException / OcioException already name
        // the offending clip, file, format, or config.
        result.error = error.what();
        result.probe.status = MediaProbeStatus::Failed;
    } catch (...) {
        result.error = "media import: unknown failure";
        result.probe.status = MediaProbeStatus::Failed;
    }
    return result;
}

struct MediaImportService::Impl {
    struct Entry {
        MediaImportRequest request;
        std::uint64_t generation{0};
    };

    explicit Impl(std::size_t maxPending) : bound(std::max<std::size_t>(maxPending, 1)) {
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool submit(MediaImportRequest request) {
        const std::lock_guard lock(mutex);
        if (stopping) {
            return false;
        }
        const std::string key = request.sourceKey;
        const bool known = outstandingKeys.contains(key);
        if (!known && outstandingKeys.size() >= bound) {
            return false;  // at the outstanding bound; the caller collects and retries
        }
        if (known) {
            // Coalesce: the newest reference for this source wins, and any
            // unconsumed older result is discarded.
            std::erase_if(pending, [&key](const Entry& entry) { return entry.request.sourceKey == key; });
            std::erase_if(results, [&key](const MediaImportResult& result) { return result.request.sourceKey == key; });
        } else {
            outstandingKeys.insert(key);
        }
        const std::uint64_t generation = ++generationCounter;
        latestGeneration[key] = generation;
        pending.push_back(Entry{std::move(request), generation});
        wake.notify_one();
        return true;
    }

    std::optional<MediaImportResult> takeResult() {
        const std::lock_guard lock(mutex);
        if (results.empty()) {
            return std::nullopt;
        }
        MediaImportResult result = std::move(results.front());
        results.pop_front();
        outstandingKeys.erase(result.request.sourceKey);
        return result;
    }

    void cancel(const std::string& sourceKey) {
        const std::lock_guard lock(mutex);
        // Forget the key's current generation: a job already decoding drops
        // its result when it finishes, and queued/unconsumed work is gone.
        latestGeneration.erase(sourceKey);
        std::erase_if(pending, [&sourceKey](const Entry& entry) { return entry.request.sourceKey == sourceKey; });
        std::erase_if(results,
                      [&sourceKey](const MediaImportResult& result) { return result.request.sourceKey == sourceKey; });
        outstandingKeys.erase(sourceKey);
    }

    void run() {
        for (;;) {
            Entry entry;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !pending.empty(); });
                if (stopping) {
                    return;  // shutdown drops queued work
                }
                entry = std::move(pending.front());
                pending.pop_front();
            }
            // All filesystem/decode work is off the service lock and off the
            // caller's thread.
            MediaImportResult result = inspectMediaSource(entry.request);
            {
                const std::lock_guard lock(mutex);
                const auto latest = latestGeneration.find(entry.request.sourceKey);
                const bool current = latest != latestGeneration.end() && latest->second == entry.generation;
                if (current) {
                    results.push_back(std::move(result));
                    if (!hasPendingLocked(entry.request.sourceKey)) {
                        // Published: the generation is only needed again on
                        // the next submit, so the map stays bounded.
                        latestGeneration.erase(entry.request.sourceKey);
                    }
                } else if (!hasPendingLocked(entry.request.sourceKey)) {
                    outstandingKeys.erase(entry.request.sourceKey);  // superseded or cancelled
                }
            }
        }
    }

    [[nodiscard]] bool hasPendingLocked(const std::string& key) const {
        return std::any_of(pending.begin(), pending.end(),
                           [&key](const Entry& entry) { return entry.request.sourceKey == key; });
    }

    std::size_t bound;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> pending;
    std::deque<MediaImportResult> results;
    std::set<std::string> outstandingKeys;
    std::map<std::string, std::uint64_t> latestGeneration;
    std::uint64_t generationCounter{0};
    bool stopping{false};
};

MediaImportService::MediaImportService(const std::size_t maxPending) : impl_(std::make_unique<Impl>(maxPending)) {}

MediaImportService::~MediaImportService() = default;

bool MediaImportService::submit(MediaImportRequest request) {
    return impl_->submit(std::move(request));
}

std::optional<MediaImportResult> MediaImportService::takeResult() {
    return impl_->takeResult();
}

void MediaImportService::cancel(const std::string& sourceKey) {
    impl_->cancel(sourceKey);
}

}  // namespace nemo::media
