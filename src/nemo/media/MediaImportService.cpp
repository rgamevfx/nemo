#include "nemo/media/MediaImportService.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
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

// The explicit sequence-pattern form ('#'/'@'), which names image data by
// construction (ImageSource's own rule, without opening a file).
[[nodiscard]] bool hasImagePattern(const std::string& path) {
    return path.find('#') != std::string::npos || path.find('@') != std::string::npos;
}

// Discovered coverage of one bounded scan (issue #80). A failed or
// irrelevant discovery adds nothing: absence is a reported fact, never a
// guessed bound.
void applyDiscoveryFacts(MediaProbeMetadata& probe, const SequenceDiscovery& discovery) {
    if (discovery.status != SequenceDiscoveryStatus::Sequence) {
        return;
    }
    probe.firstFrame = discovery.first;
    probe.lastFrame = discovery.last;
    probe.coverageQuality = CoverageQuality::Validated;
    probe.availableFrameCount = discovery.availableCount;
    probe.missingFrameCount = discovery.missingCount;
    probe.missingRanges.clear();
    probe.missingRanges.reserve(discovery.holes.size());
    for (const SequenceFrameRange& hole : discovery.holes) {
        probe.missingRanges.push_back(MediaFrameRange{hole.first, hole.last});
    }
}

// A still is one image with time-independent availability: it carries no
// interval at all, because a one-frame interval would turn every other local
// time into a boundary failure.
void applyStillFacts(MediaProbeMetadata& probe) {
    probe.duration = 1;
    probe.coverageQuality = CoverageQuality::Validated;
    probe.availableFrameCount = 1;
    probe.missingFrameCount = 0;
    probe.missingRanges.clear();
}

// Channel layout of a still's storage order (e.g. "RGBA"), the reader's own
// declaration. Empty when the reader reported none.
[[nodiscard]] std::string joinChannels(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) {
            out += " ";
        }
        out += name;
    }
    return out;
}

// A rational rate is reported only when the media's own rate really is that
// fraction; a floating average is never promoted to an authoritative
// numerator/denominator.
void applyRate(MediaProbeMetadata& probe, const double rate) {
    if (!(rate > 0.0)) {
        return;
    }
    static constexpr std::uint32_t kDenominators[] = {1, 1000, 1001, 24, 25, 30, 48, 50, 60, 120};
    for (const std::uint32_t denominator : kDenominators) {
        const double numerator = rate * static_cast<double>(denominator);
        if (numerator > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            continue;
        }
        const long long rounded = std::llround(numerator);
        if (rounded > 0 && std::abs(numerator - static_cast<double>(rounded)) < 1e-9) {
            probe.rateNumerator = static_cast<std::uint32_t>(rounded);
            probe.rateDenominator = denominator;
            return;
        }
    }
}

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

// Frames a decoded data raster back into its declared format for display
// (issue #88): the read returns the data extent alone, so a preview must place
// it at its own signed origin inside the frame's format, with transparent black
// where the source declares no samples. Nothing is stretched — a source whose
// data extends past its format keeps its authored framing, and data outside the
// format is cropped exactly as the display would crop it.
[[nodiscard]] CpuImage framedToFormat(const CpuImage& raster, const Region& coverage, const Region& format) {
    if (format.width <= 0 || format.height <= 0) {
        return raster;  // no known format to frame: the data raster is all there is
    }
    ImageLayout layout = raster.layout();
    layout.width = format.width;
    layout.height = format.height;
    CpuImage framed(layout);
    for (int y = 0; y < raster.height(); ++y) {
        const int targetY = coverage.y + y - format.y;
        if (targetY < 0 || targetY >= layout.height) {
            continue;
        }
        for (int x = 0; x < raster.width(); ++x) {
            const int targetX = coverage.x + x - format.x;
            if (targetX < 0 || targetX >= layout.width) {
                continue;
            }
            framed.setPixel(targetX, targetY, raster.pixel(x, y));
        }
    }
    return framed;
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

void importStill(const MediaImportRequest& request, const std::string& framePath, const std::int64_t probeFrame,
                 const bool wantsThumbnail, const SequenceDiscovery& discovery,
                 const std::shared_ptr<const InputColorCache>& colors, MediaImportResult& result) {
    const std::string context = "source '" + request.sourceKey + "'";
    // Header facts and the resolved input interpretation come from the image
    // adapter, which owns those rules; this service does not re-validate or
    // re-derive color metadata. The frame path is already resolved by the
    // caller, so the mapping is applied exactly once.
    const ImageFrameInfo info = probeImageFrame(*colors, request.inputColor, framePath, context);
    const bool sequence = discovery.status == SequenceDiscoveryStatus::Sequence ||
                          hasImagePattern(request.reference.path) || info.sequence;

    MediaProbeMetadata probe;
    probe.width = info.width;
    probe.height = info.height;
    probe.duration = sequence ? 0 : 1;
    probe.codec = info.formatName;
    probe.colorPrimaries = imagePrimariesName(info.primaries);
    probe.colorTransfer = imageTransferName(info.transfer);
    probe.provenance = "oiio";
    probe.status = MediaProbeStatus::Ready;
    probe.pixelAspect = info.pixelAspect;
    probe.precision = info.nativePrecision;
    probe.channels = joinChannels(info.channelNames);
    probe.declaredInputColorSpace = info.declaredColorSpace;
    result.inputColor = info.inputColor;
    if (sequence) {
        applyDiscoveryFacts(probe, discovery);
        // A scan that could not complete reports the file's own facts and no
        // coverage: an unknown interval is never invented from the probed
        // frame.
    } else {
        applyStillFacts(probe);
    }

    result.kind = sequence ? MediaKind::Sequence : MediaKind::Image;
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
    // transform, exactly like the clip path. The read returns the data raster
    // alone, so the preview re-frames it into the declared format through its
    // own coverage: the thumbnail shows the frame as authored, not just the
    // data window.
    const ImageFrame decoded = readImageFrame(*colors, request.inputColor, framePath, probeFrame, context);
    // The decoded read is authoritative for the frame actually produced.
    result.probe.width = decoded.info.width;
    result.probe.height = decoded.info.height;
    result.probe.pixelAspect = decoded.info.pixelAspect;
    result.pixelFormat = decoded.info.nativePrecision;
    result.bitDepth = bitDepthFromPrecision(decoded.info.nativePrecision);
    const CpuImage framed = framedToFormat(decoded.image, decoded.info.coverage, decoded.info.description.format);
    const PreviewSize preview = previewSize(framed.width(), framed.height(), decoded.info.pixelAspect,
                                            request.thumbnailWidth, request.thumbnailHeight);
    CpuImage thumb = downsampleTo(framed, preview.width, preview.height);
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
                const bool wantsThumbnail, const std::shared_ptr<const InputColorCache>& colors,
                MediaImportResult& result) {
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
    // The decoder shares ownership of the retained context, so a later refresh
    // replaces the context for new work without invalidating this decode.
    SoftwareClip clip =
        decodeClipFrameSoftware(request.reference.path, sourceFrame, maxWidth, maxHeight, request.colorPolicy,
                                overrides, ClipColorInput{request.inputColor, colors});
    if (clip.frames.empty()) {
        failImport("media import: " + sourceDiagnostic(request.reference, resolved) + ": no frame at source frame " +
                   std::to_string(sourceFrame) + " (source time " + std::to_string(request.frame) + ")");
    }

    MediaProbeMetadata probe;
    probe.width = clip.info.width;
    probe.height = clip.info.height;
    probe.duration = clip.info.frameCount > 0 ? clip.info.frameCount : 0;
    probe.codec = clip.info.codecName;
    probe.colorPrimaries = imagePrimariesName(ImagePrimaries::Rec709);
    probe.colorTransfer = mediaTransferName(clip.metadata.transfer);
    probe.colorMatrix = mediaMatrixName(clip.metadata.matrix);
    probe.provenance = "ffmpeg-software";
    probe.status = MediaProbeStatus::Ready;
    probe.pixelAspect = clip.info.pixelAspect;
    probe.precision = clip.pixelFormat;
    result.inputColor = clip.inputColor;
    applyRate(probe, clip.info.frameRate);
    // Coverage quality traces to the reader's own provenance, never to "count
    // > 0": a container-DECLARED frame count (VideoDecode's
    // FrameCountQuality::Reliable) may bound the range, while an estimated or
    // absent count is reported without bounds so Auto never fabricates one.
    if (clip.info.frameCountQuality == FrameCountQuality::Reliable && clip.info.frameCount > 0) {
        probe.firstFrame = 0;
        probe.lastFrame = clip.info.frameCount - 1;
        probe.coverageQuality = CoverageQuality::Validated;
        probe.availableFrameCount = clip.info.frameCount;
        probe.missingFrameCount = 0;
    } else if (clip.info.frameCountQuality == FrameCountQuality::Estimated && clip.info.frameCount > 0) {
        probe.coverageQuality = CoverageQuality::Estimated;
        probe.availableFrameCount = clip.info.frameCount;
    } else {
        probe.coverageQuality = CoverageQuality::Unknown;
    }

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

MediaImportResult inspectMediaSource(const MediaImportRequest& request,
                                     const std::shared_ptr<const InputColorCache>& sourceColor) {
    MediaImportResult result;
    result.request = request;
    result.probe.status = MediaProbeStatus::Failed;
    std::shared_ptr<const InputColorCache> colors = sourceColor;
    try {
        if (!colors) {
            // No retained context was supplied: build one for this request's
            // policy, owned by this call. A policy failure (a missing config) is
            // the probe's diagnostic and names the offending relationship.
            colors = std::make_shared<InputColorCache>(
                SourceColorPolicy{request.colorConfig, request.colorPolicy.workingSpace});
        }
        bool wantsThumbnail = false;
        validateRequest(request, wantsThumbnail);

        // `frame` is a source-local time: map it through the reference's
        // offset/step exactly once, then use the mapped source frame for
        // existence, classification, decode, and diagnostics. A sequence
        // reference with frameOffset 1001 therefore resolves local time 0 to
        // source frame 1001, not to a literal file "0000".
        std::int64_t mappedFrame = 0;
        try {
            mappedFrame = request.reference.frameAt(request.frame);
        } catch (const std::exception& error) {
            failImport("media import: source time " + std::to_string(request.frame) + " failed: " + error.what());
        }
        std::int64_t sourceFrame = mappedFrame;
        std::string resolved = resolveFramePath(request.reference.path, sourceFrame);

        // Discovery runs before any presumed frame is probed (issue #80): a
        // sequence must be inspected through a member that exists. It is
        // bounded, cancellable, and advisory — a failed scan never changes the
        // mapped-frame diagnostics.
        SequenceDiscovery discovery;
        discovery.pattern = request.reference.path;
        const bool mappedExists = std::filesystem::exists(resolved);
        const bool selectionExists = mappedExists || std::filesystem::exists(request.reference.path);
        const bool candidate =
            hasImagePattern(request.reference.path) || (mappedExists ? isImagePath(resolved) : selectionExists);
        if (candidate) {
            discovery = discoverSequenceRange(request.reference.path, request.cancel.get());
        }
        // The probe reads one explicit source frame; a fresh selection is
        // normalized to the canonical pattern and aligned to the discovered
        // first member, so the frame is opened exactly once, the reported
        // probed frame is the file actually inspected, and an unmapped
        // candidate's bounds never reject it.
        SourceReference reading = request.reference;
        if (request.alignment == ProbeAlignment::DiscoverAvailable &&
            discovery.status == SequenceDiscoveryStatus::Sequence) {
            sourceFrame = discovery.first;
            resolved = resolveFramePath(discovery.pattern, sourceFrame);
            reading.path = discovery.pattern;
            reading.frameOffset = sourceFrame;
            reading.frameStep = 1;
            reading.firstFrame.reset();
            reading.lastFrame.reset();
        } else if (sourceFrame != mappedFrame) {
            reading.frameOffset = sourceFrame;
            reading.frameStep = 1;
            reading.firstFrame.reset();
            reading.lastFrame.reset();
        }

        result.discovery = discovery;
        result.probedFrame = sourceFrame;
        if (!std::filesystem::exists(resolved)) {
            result.offline = true;
            failImport("media import: source path does not exist: " + sourceDiagnostic(request.reference, resolved));
        }
        if (isImagePath(resolved)) {
            importStill(request, resolved, sourceFrame, wantsThumbnail, discovery, colors, result);
        } else {
#if defined(NEMO_MEDIA_FFMPEG)
            importClip(request, sourceFrame, resolved, wantsThumbnail, colors, result);
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

MediaImportResult inspectMediaSource(const MediaImportRequest& request) {
    return inspectMediaSource(request, {});
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
        // The superseded generation's bounded scan stops early instead of
        // finishing work whose result can no longer be published.
        if (const auto previous = cancelFlags.find(key); previous != cancelFlags.end()) {
            previous->second->store(true);
        }
        auto flag = std::make_shared<std::atomic<bool>>(false);
        request.cancel = flag;
        cancelFlags[key] = std::move(flag);
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
        // Forget the key's current generation: a job already decoding or
        // scanning stops early and drops its result when it finishes, and
        // queued/unconsumed work is gone.
        latestGeneration.erase(sourceKey);
        if (const auto flag = cancelFlags.find(sourceKey); flag != cancelFlags.end()) {
            flag->second->store(true);
            cancelFlags.erase(flag);
        }
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
            MediaImportResult result = inspectMediaSource(entry.request, colorsFor(entry.request));
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
                if (const auto flag = cancelFlags.find(entry.request.sourceKey);
                    flag != cancelFlags.end() && flag->second == entry.request.cancel) {
                    cancelFlags.erase(flag);  // the scan is over; the flag has no more readers
                }
            }
        }
    }

    // The worker's retained input-color context: one shared owner rebuilt only
    // when the requested policy (config reference + working space) changes, so
    // consecutive probes of one project never re-open the configuration. A
    // construction failure leaves the context null and the probe's own
    // try/catch reports the offending config through its diagnostic.
    [[nodiscard]] const std::shared_ptr<const InputColorCache>& colorsFor(const MediaImportRequest& request) {
        const SourceColorPolicy policy{request.colorConfig, request.colorPolicy.workingSpace};
        // The generation is part of the key: a same-path project reopen resets
        // the projectGeneration even when the config reference and working space
        // are unchanged, so the previous project's context is never reused.
        if (!colors || !(policy == colorsPolicy) || request.projectGeneration != colorsGeneration) {
            colorsPolicy = policy;
            colorsGeneration = request.projectGeneration;
            try {
                colors = std::make_shared<InputColorCache>(policy);
            } catch (...) {
                colors.reset();
            }
        }
        return colors;
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
    // Cancellation flags for in-flight work, so a superseded or cancelled
    // request's bounded directory scan stops early.
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancelFlags;
    // Worker-owned retained input-color context (see colorsFor).
    std::shared_ptr<const InputColorCache> colors;
    SourceColorPolicy colorsPolicy;
    std::uint64_t colorsGeneration{0};
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
