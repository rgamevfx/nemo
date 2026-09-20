#include "nemo/media/DeliveryOutput.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/media/ViewingTransform.hpp"

#if defined(NEMO_MEDIA_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace nemo::media {

namespace {

// --- the closed inventories of the authored settings -----------------------
//
// One list per setting, so the refusal text and the acceptance test can never
// disagree, and a second value is added only with real evidence that the
// encoder produces it.

constexpr std::array<std::string_view, 3> kFileTypes{"exr", "mov", "mp4"};
constexpr std::array<std::string_view, 3> kProresProfiles{"422", "4444", "4444xq"};
constexpr std::array<std::string_view, 4> kColorModes{"raw", "project", "colorspace", "display"};

// A ceiling on the frame rate, so an implausible authored value (a typo, a
// unit mistake) is refused by validation instead of by an overflowing time
// base inside the encoder.
constexpr double kMaxFrameRate = 1000.0;

[[nodiscard]] bool isOneOf(const auto& choices, const std::string& value) {
    return std::find(choices.begin(), choices.end(), std::string_view{value}) != choices.end();
}

template <std::size_t Size>
[[nodiscard]] std::string listText(const std::array<std::string_view, Size>& choices) {
    std::string text;
    for (const std::string_view choice : choices) {
        if (!text.empty()) {
            text += ", ";
        }
        text += choice;
    }
    return text;
}

[[nodiscard]] std::string numberText(const double value) {
    std::string text = std::to_string(value);
    // std::to_string keeps six decimals; drop the trailing zeros so a refusal
    // quotes the value the author typed rather than its float noise.
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

[[nodiscard]] bool isMovieFileType(const std::string& fileType) {
    return fileType == "mov" || fileType == "mp4";
}

}  // namespace

std::string validateDeliveryOutput(const DeliveryOutputOptions& options) {
    if (!isOneOf(kFileTypes, options.fileType)) {
        return "file type '" + options.fileType + "' is not supported (supported: " + listText(kFileTypes) + ")";
    }
    if (isMovieFileType(options.fileType)) {
        // The time base is derived from this value exactly, so it has to be a
        // number a frame rate can be.
        if (!std::isfinite(options.frameRate) || options.frameRate <= 0.0 || options.frameRate > kMaxFrameRate) {
            return "frame rate " + numberText(options.frameRate) +
                   " fps is not usable (supported: above 0 and at most " + numberText(kMaxFrameRate) + " fps)";
        }
    }
    if (options.fileType == "mov" && !isOneOf(kProresProfiles, options.profile)) {
        return "ProRes profile '" + options.profile + "' is not supported (supported: " + listText(kProresProfiles) +
               ")";
    }
    if (options.fileType == "mp4" && options.bitrateKbps < 1) {
        return "bit rate " + std::to_string(options.bitrateKbps) + " kb/s is not usable (supported: at least 1 kb/s)";
    }
    if (options.fileType == "exr" && !options.compression.empty() && !isSupportedExrCompression(options.compression)) {
        return "compression '" + options.compression +
               "' is not a supported EXR compression (supported: " + supportedExrCompressions() + ")";
    }
    if (!isOneOf(kColorModes, options.colorMode)) {
        return "color mode '" + options.colorMode + "' is not supported (supported: " + listText(kColorModes) + ")";
    }
    if ((options.colorMode == "colorspace" || options.colorMode == "display") && options.outputTransform.empty()) {
        return "color mode '" + options.colorMode + "' was chosen without an output transform";
    }
    if (!options.lutFile.empty()) {
        // Submission validation is filesystem-free. The worker's retained OCIO
        // processor checks existence/readability before any output is opened.
        if (!isSupportedTransformFile(options.lutFile)) {
            return "LUT file '" + options.lutFile +
                   "' has no registered reader for its extension (supported: " + supportedTransformFileExtensions() +
                   ")";
        }
    }
    return {};
}

std::vector<std::string> deliveryTransformChoices(const std::string& configPath, const std::string& mode) {
    if (!isOneOf(kColorModes, mode)) {
        throw OcioException("delivery color: '" + mode +
                            "' is not an output color mode (supported: " + listText(kColorModes) + ")");
    }
    if (mode == "raw") {
        // No transform is applied at all, so there is nothing to choose from and
        // no configuration to load.
        return {};
    }
    const OcioConfigSnapshot snapshot(configPath);
    if (mode == "project") {
        // The project's own delivery transform comes from the document's color
        // policy, not from a list: the choices belong to that owner.
        return {};
    }
    return mode == "colorspace" ? snapshot.colorSpaces() : snapshot.displayViews();
}

// ---------------------------------------------------------------------------
// Delivery color (retained per job)
// ---------------------------------------------------------------------------

struct DeliveryColorProcessor::Impl {
    // Null for a raw delivery without a LUT: nothing was resolved and no
    // configuration was loaded.
    std::shared_ptr<const OcioOutputTransform> transform;
    std::string description;
};

DeliveryColorProcessor::DeliveryColorProcessor(const DeliveryOutputOptions& options, const ColorPolicy& policy,
                                               const std::string& configPath)
    : impl_(std::make_unique<Impl>()) {
    if (const std::string problem = validateDeliveryOutput(options); !problem.empty()) {
        throw OcioException("delivery color: " + problem);
    }
    ExportTransformKind kind = ExportTransformKind::None;
    std::string name;
    if (options.colorMode == "project") {
        if (policy.deliveryTransform.empty()) {
            throw OcioException("delivery color: the project has no delivery transform to apply; set the project's "
                                "delivery transform or choose another output color mode");
        }
        kind = ExportTransformKind::DisplayView;
        name = policy.deliveryTransform;
    } else if (options.colorMode == "colorspace") {
        kind = ExportTransformKind::ColorSpace;
        name = options.outputTransform;
    } else if (options.colorMode == "display") {
        kind = ExportTransformKind::DisplayView;
        name = options.outputTransform;
    }
    if (kind == ExportTransformKind::None && options.lutFile.empty()) {
        impl_->description = "raw values (no output color transform)";
        return;
    }
    // ONE configuration load for the whole job, and ONE resolution of the base
    // and the LUT: every frame of the range applies these processors, and
    // nothing re-resolves per frame. The snapshot's config is kept alive by the
    // retained transform, exactly as the input transform keeps its own.
    const OcioConfigSnapshot snapshot(configPath);
    impl_->transform = snapshot.outputTransform(policy.workingSpace, kind, std::move(name), options.lutFile);
    impl_->description = impl_->transform->description();
}

DeliveryColorProcessor::~DeliveryColorProcessor() = default;
DeliveryColorProcessor::DeliveryColorProcessor(DeliveryColorProcessor&&) noexcept = default;
DeliveryColorProcessor& DeliveryColorProcessor::operator=(DeliveryColorProcessor&&) noexcept = default;

void DeliveryColorProcessor::apply(CpuImage& image) const {
    if (impl_ == nullptr) {
        throw OcioException("delivery color: this processor is empty (it was moved from)");
    }
    if (impl_->transform != nullptr) {
        impl_->transform->apply(image);
    }
}

const std::string& DeliveryColorProcessor::description() const noexcept {
    static const std::string kEmpty;
    return impl_ != nullptr ? impl_->description : kEmpty;
}

// ---------------------------------------------------------------------------
// Stills: the shared image writer
// ---------------------------------------------------------------------------

void writeDeliveryImage(const std::string& path, const CpuImage& image, const ImageDescription& description,
                        const DeliveryOutputOptions& options) {
    if (isMovieFileType(options.fileType)) {
        throw DeliveryMovieError(path, "file type '" + options.fileType +
                                           "' is a movie; a single still needs the movie writer, not a still write");
    }
    if (options.fileType != "exr") {
        throw DeliveryMovieError(path, "file type '" + options.fileType +
                                           "' is not supported (supported: " + listText(kFileTypes) + ")");
    }
    // The delivered description is the frame contract: a still that carries
    // different channels than the one it claims to deliver would silently write
    // a different image, so the mismatch is refused by name.
    if (!description.channels.empty() && description.channels != image.layout().channels) {
        throw ImageIoException(path, "the image's channels do not match the delivered description (image: " +
                                         std::to_string(image.channelCount()) + " channels, description: " +
                                         std::to_string(description.channels.size()) + " channels)");
    }
    ImageWriteOptions write;
    write.precision = options.precision;
    write.compression = options.compression;
    // The described geometry, stated explicitly: the raster's own first-sample
    // coordinate as the data window origin and the description's format as the
    // display window, so a reader recovers the authored framing instead of the
    // raster extent.
    if (description.dataBounds.width > 0 && description.dataBounds.height > 0) {
        write.windowX = description.dataBounds.x;
        write.windowY = description.dataBounds.y;
    }
    if (description.format.width > 0 && description.format.height > 0) {
        write.formatX = description.format.x;
        write.formatY = description.format.y;
        write.formatWidth = description.format.width;
        write.formatHeight = description.format.height;
    }
    write.pixelAspect = description.pixelAspect;
    writeImage(path, image, write);
}

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------

#if defined(NEMO_MEDIA_FFMPEG)

namespace {

// --- FFmpeg ownership: the same discipline the viewer encoder uses. Each guard
// owns exactly one allocation, so every error path releases the same set and no
// path can report a half-open container.

struct CodecContextDelete {
    void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
};
struct FormatContextDelete {
    void operator()(AVFormatContext* value) const { avformat_free_context(value); }
};
struct PacketDelete {
    void operator()(AVPacket* value) const { av_packet_free(&value); }
};
struct FrameDelete {
    void operator()(AVFrame* value) const { av_frame_free(&value); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDelete>;
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDelete>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDelete>;
using FramePtr = std::unique_ptr<AVFrame, FrameDelete>;

[[nodiscard]] std::string avError(const int status) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(status, text, sizeof(text));
    return text;
}

// The one video layout a delivery movie encodes: which chroma geometry, at
// which code value range, and whether an alpha plane is stored. The stream's
// color tags are derived from this same structure, so the tags can never claim
// a layout the writer did not produce.
struct MovieLayout {
    AVCodecID codecId{AV_CODEC_ID_NONE};
    AVPixelFormat pixelFormat{AV_PIX_FMT_NONE};
    int bitDepth{10};
    // Chroma subsampling factor along x: 1 (4:4:4), 2 (4:2:2 and 4:2:0).
    int chromaStep{1};
    // 4:2:0 also halves the chroma rows.
    bool chromaRowsHalved{false};
    bool alpha{false};
    AVChromaLocation chromaLocation{AVCHROMA_LOC_LEFT};
};

// The movie layout of one authored profile. Every value here is a claim the
// encoder is asked to honour and is verified after the encoder opens.
[[nodiscard]] MovieLayout layoutFor(const DeliveryOutputOptions& options) {
    MovieLayout layout;
    if (options.fileType == "mov") {
        layout.codecId = AV_CODEC_ID_PRORES;
        layout.bitDepth = 10;
        if (options.profile == "422") {
            // ProRes 422: 10-bit 4:2:2, no alpha plane.
            layout.pixelFormat = AV_PIX_FMT_YUV422P10LE;
            layout.chromaStep = 2;
            layout.chromaLocation = AVCHROMA_LOC_LEFT;
        } else {
            // ProRes 4444 / 4444 XQ: 10-bit 4:4:4 with the alpha plane the
            // profile is chosen for. No subsampling means no chroma siting.
            layout.pixelFormat = AV_PIX_FMT_YUVA444P10LE;
            layout.chromaStep = 1;
            layout.alpha = true;
            layout.chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
        }
        return layout;
    }
    // H.264 MP4: 8-bit 4:2:0, the layout every player can decode, and no alpha
    // plane (H.264 carries none — the delivery is video-only).
    layout.codecId = AV_CODEC_ID_H264;
    layout.bitDepth = 8;
    layout.pixelFormat = AV_PIX_FMT_YUV420P;
    layout.chromaStep = 2;
    layout.chromaRowsHalved = true;
    layout.chromaLocation = AVCHROMA_LOC_CENTER;
    return layout;
}

// The code-value range and Rec.709 coefficients of the conversion this writer
// performs: limited (MPEG) range, the same quantization the stream is tagged
// with.
struct RgbToYuv {
    float lumaOffset;
    float lumaScale;
    float chromaOffset;
    float chromaScale;
    float maxCode;
};

[[nodiscard]] RgbToYuv conversionFor(const int bitDepth) {
    const float maxCode = bitDepth == 10 ? 1023.0F : 255.0F;
    const float chromaOffset = bitDepth == 10 ? 512.0F : 128.0F;
    const float chromaScale = bitDepth == 10 ? 896.0F : 224.0F;
    return RgbToYuv{bitDepth == 10 ? 64.0F : 16.0F, bitDepth == 10 ? 876.0F : 219.0F, chromaOffset, chromaScale,
                    maxCode};
}

void encodeRgb(const RgbToYuv& conversion, const float r, const float g, const float b, float& luma, float& cb,
               float& cr) {
    const float red = std::clamp(r, 0.0F, 1.0F);
    const float green = std::clamp(g, 0.0F, 1.0F);
    const float blue = std::clamp(b, 0.0F, 1.0F);
    // Rec.709 luma with limited-range quantization (Y' 16..235 / 64..940,
    // Cb/Cr 16..240 / 64..960): the exact inverse of what a player applies for
    // the Rec.709 matrix and MPEG range this stream declares.
    const float value = 0.2126F * red + 0.7152F * green + 0.0722F * blue;
    luma = conversion.lumaOffset + conversion.lumaScale * value;
    cb = conversion.chromaOffset + (conversion.chromaScale / 2.0F) / (1.0F - 0.0722F) * (blue - value);
    cr = conversion.chromaOffset + (conversion.chromaScale / 2.0F) / (1.0F - 0.2126F) * (red - value);
}

// Writes one delivered frame into the codec's planes.
//
// The image is read through its own RGBA projection, so a frame whose channels
// are named differently (an auxiliary pass beside RGBA, an alpha-only matte)
// still reads its identified primary channels, and a missing role follows the
// image contract rather than being invented here. Alpha is written linearly
// over the full code range for the profiles that carry it, and RGB is NEVER
// scaled by alpha: a straight source keeps the color hidden under zero alpha,
// and no premultiply or unpremultiply happens anywhere in this path.
//
// Chroma is a box filter over each chroma sample's own footprint — a
// reproducible, resolution-independent choice — and its placement is what the
// stream declares (horizontally centred for 4:2:2, centred for 4:2:0), so the
// chroma location tag describes this conversion rather than a guess.
void writeFramePlanes(const CpuImage& image, AVFrame& frame, const MovieLayout& layout) {
    const RgbToYuv conversion = conversionFor(layout.bitDepth);
    const int width = image.width();
    const int height = image.height();
    const int chromaWidth = layout.chromaStep == 1 ? width : width / 2;
    const int chromaHeight = layout.chromaRowsHalved ? height / 2 : height;
    const auto store = [&](std::uint8_t* plane, const int stride, const int x, const int y, const float value) {
        const float clamped = std::clamp(value, 0.0F, conversion.maxCode);
        const auto code = static_cast<unsigned>(std::lround(clamped));
        if (layout.bitDepth == 10) {
            reinterpret_cast<std::uint16_t*>(plane + static_cast<std::ptrdiff_t>(y) * stride)[x] =
                static_cast<std::uint16_t>(code);
        } else {
            plane[static_cast<std::ptrdiff_t>(y) * stride + x] = static_cast<std::uint8_t>(code);
        }
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, kImageChannels> rgba = image.pixel(x, y);
            float luma = 0.0F;
            float cb = 0.0F;
            float cr = 0.0F;
            encodeRgb(conversion, rgba[0], rgba[1], rgba[2], luma, cb, cr);
            store(frame.data[0], frame.linesize[0], x, y, luma);
        }
    }
    const int step = layout.chromaStep;
    const int rows = layout.chromaRowsHalved ? 2 : 1;
    for (int y = 0; y < chromaHeight; ++y) {
        for (int x = 0; x < chromaWidth; ++x) {
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            for (int dy = 0; dy < rows; ++dy) {
                for (int dx = 0; dx < step; ++dx) {
                    const std::array<float, kImageChannels> rgba = image.pixel(x * step + dx, y * rows + dy);
                    r += rgba[0];
                    g += rgba[1];
                    b += rgba[2];
                }
            }
            const float count = static_cast<float>(step * rows);
            float luma = 0.0F;
            float cb = 0.0F;
            float cr = 0.0F;
            encodeRgb(conversion, r / count, g / count, b / count, luma, cb, cr);
            store(frame.data[1], frame.linesize[1], x, y, cb);
            store(frame.data[2], frame.linesize[2], x, y, cr);
        }
    }
    if (!layout.alpha) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            store(frame.data[3], frame.linesize[3], x, y, image.pixel(x, y)[3] * conversion.maxCode);
        }
    }
}

// True when the encoder declares `format` among the pixel formats it accepts. A
// null list means the encoder decides at open; a non-null list is an explicit
// capability statement, and a format outside it is refused by name instead of
// being offered to a codec that cannot consume it.
[[nodiscard]] bool encoderAcceptsPixelFormat(const AVCodec* codec, const AVPixelFormat format) {
    if (codec->pix_fmts == nullptr) {
        return true;
    }
    for (int index = 0; codec->pix_fmts[index] != AV_PIX_FMT_NONE; ++index) {
        if (codec->pix_fmts[index] == format) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string pixelFormatName(const AVPixelFormat format) {
    const char* name = av_get_pix_fmt_name(format);
    return name != nullptr ? name : "unknown pixel format";
}

struct ResolvedEncoder {
    const AVCodec* codec{nullptr};
    std::string id;
};

// Resolves the encoder for one movie. Every candidate is an encoder this build
// registers AND that declares the movie's pixel format, so an unavailable
// engine is reported (with the reason each candidate was rejected) instead of
// being silently replaced by a different codec.
[[nodiscard]] ResolvedEncoder resolveEncoder(const std::string& path, const DeliveryOutputOptions& options,
                                             const MovieLayout& layout) {
    const std::vector<std::string> candidates = options.fileType == "mov"
                                                    ? std::vector<std::string>{"prores_ks"}
                                                    : std::vector<std::string>{"libx264", "h264_nvenc"};
    std::string rejected;
    for (const std::string& id : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(id.c_str());
        if (codec == nullptr) {
            rejected += (rejected.empty() ? "" : "; ") + id + " is not registered in this libavcodec build";
            continue;
        }
        if (!encoderAcceptsPixelFormat(codec, layout.pixelFormat)) {
            rejected += (rejected.empty() ? "" : "; ") + id + " does not accept " + pixelFormatName(layout.pixelFormat);
            continue;
        }
        return ResolvedEncoder{codec, id};
    }
    throw DeliveryMovieError(path, "no encoder is available for this movie (" + rejected + ")");
}

[[nodiscard]] bool containerNeedsGlobalHeader(const AVFormatContext& format) {
    return (format.oformat->flags & AVFMT_GLOBALHEADER) != 0;
}

// The ProRes profile value of one authored profile: FF_PROFILE_PRORES_STANDARD
// is the 422 profile, and the two 4444 profiles differ only by their bit rate
// target.
[[nodiscard]] int proresProfileValue(const std::string& profile) {
    if (profile == "422") {
        return FF_PROFILE_PRORES_STANDARD;
    }
    return profile == "4444" ? FF_PROFILE_PRORES_4444 : FF_PROFILE_PRORES_XQ;
}

}  // namespace

bool deliveryStoresAlpha(const DeliveryOutputOptions& options) {
    return options.fileType == "exr" || layoutFor(options).alpha;
}

struct DeliveryMovieWriter::Impl {
    std::string path;
    DeliveryOutputOptions options;
    ImageDescription description;
    MovieLayout layout;
    AVRational frameRate{24, 1};
    std::string encoderId;
    int width{0};
    int height{0};

    CodecContextPtr codec;
    FormatContextPtr format;
    PacketPtr packet;
    AVStream* stream{nullptr};
    std::int64_t nextPts{0};
    int submitted{0};
    int drained{0};
    bool fileOpened{false};
    bool finished{false};

    ~Impl() { abandon(); }

    // Removes an unfinished output: a partial movie must never be left where a
    // completed one belongs. The container is closed first, and only a path
    // this writer opened is ever removed.
    void abandon() noexcept {
        codec.reset();
        packet.reset();
        if (format != nullptr && format->pb != nullptr) {
            avio_closep(&format->pb);
        }
        format.reset();
        if (fileOpened && !finished && !path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }

    void start();
    void appendFrame(const CpuImage& image);
    void drainPackets();
    void complete();
};

void DeliveryMovieWriter::Impl::start() {
    // The declared semantics of the stream, in one place: the matrix and range
    // tags describe the conversion this writer performs (Rec.709 coefficients,
    // limited range) and are therefore always justified. Primaries and transfer
    // are NOT: the RGB the caller hands over may be scene-linear, raw, or the
    // output of an arbitrary LUT, and none of those is Rec.709-encoded — so
    // they stay explicitly unspecified rather than claiming a transfer the
    // writer cannot know. A consumer that knows the delivered encoding may
    // state it; this adapter never guesses it.
    //
    // What actually reaches a finished file (measured against this build's
    // FFmpeg, and identical to what its own CLI produces for the same
    // settings): an H.264/MP4 stream carries these in its VUI, so the matrix
    // and range are readable and the primaries/transfer read as unknown; a
    // ProRes/MOV stream carries no container colour atom, and the ProRes
    // encoder writes none into its frame header either — so a ProRes delivery
    // makes no colour claim at all rather than a wrong one.
    const int colorMatrix = AVCOL_SPC_BT709;
    const int colorRange = AVCOL_RANGE_MPEG;
    const int colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    const int colorTransfer = AVCOL_TRC_UNSPECIFIED;

    const ResolvedEncoder resolved = resolveEncoder(path, options, layout);
    encoderId = resolved.id;

    codec.reset(avcodec_alloc_context3(resolved.codec));
    if (codec == nullptr) {
        throw DeliveryMovieError(path, "codec context allocation failed for encoder '" + encoderId + "'");
    }
    codec->width = width;
    codec->height = height;
    codec->time_base = AVRational{frameRate.den, frameRate.num};
    codec->framerate = frameRate;
    codec->pix_fmt = layout.pixelFormat;
    if (!std::isfinite(description.pixelAspect) || description.pixelAspect <= 0.0) {
        throw DeliveryMovieError(path, "the delivered pixel aspect must be finite and positive");
    }
    codec->sample_aspect_ratio = av_d2q(description.pixelAspect, 90000);
    codec->color_range = static_cast<AVColorRange>(colorRange);
    codec->colorspace = static_cast<AVColorSpace>(colorMatrix);
    codec->color_primaries = static_cast<AVColorPrimaries>(colorPrimaries);
    codec->color_trc = static_cast<AVColorTransferCharacteristic>(colorTransfer);
    codec->chroma_sample_location = layout.chromaLocation;
    if (options.fileType == "mp4") {
        codec->bit_rate = static_cast<std::int64_t>(options.bitrateKbps) * 1000;
    }
    if (options.fileType == "mov") {
        const int profile = proresProfileValue(options.profile);
        codec->profile = profile;
        // The encoder's own option is set too: the ProRes profile is a private
        // option of prores_ks, and stating it explicitly is what makes the
        // profile the encoder actually encodes verifiable after it opens.
        const int status = av_opt_set_int(codec->priv_data, "profile", profile, 0);
        if (status < 0) {
            throw DeliveryMovieError(path, "ProRes profile '" + options.profile +
                                               "' could not be configured: " + avError(status));
        }
    }
    AVFormatContext* raw = nullptr;
    const char* container = options.fileType == "mov" ? "mov" : "mp4";
    if (avformat_alloc_output_context2(&raw, nullptr, container, path.c_str()) < 0 || raw == nullptr) {
        throw DeliveryMovieError(path, std::string{"the "} + container + " container could not be created");
    }
    format.reset(raw);
    if (containerNeedsGlobalHeader(*format)) {
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    const int openStatus = avcodec_open2(codec.get(), resolved.codec, nullptr);
    if (openStatus < 0) {
        throw DeliveryMovieError(path, "encoder '" + encoderId + "' could not be opened for " +
                                           pixelFormatName(layout.pixelFormat) + ": " + avError(openStatus));
    }
    // The claims are verified, not assumed: an encoder that opened with a
    // different profile or pixel format would produce a movie whose authored
    // settings and actual layout disagree.
    if (codec->pix_fmt != layout.pixelFormat) {
        throw DeliveryMovieError(path, "encoder '" + encoderId + "' opened with " + pixelFormatName(codec->pix_fmt) +
                                           " instead of the requested " + pixelFormatName(layout.pixelFormat));
    }
    if (options.fileType == "mov") {
        const int expected = proresProfileValue(options.profile);
        if (codec->profile != expected) {
            throw DeliveryMovieError(path, "encoder '" + encoderId + "' opened with ProRes profile " +
                                               std::to_string(codec->profile) + " instead of " + options.profile);
        }
    }

    stream = avformat_new_stream(format.get(), nullptr);
    if (stream == nullptr) {
        throw DeliveryMovieError(path, "the output stream could not be created");
    }
    if (avcodec_parameters_from_context(stream->codecpar, codec.get()) < 0) {
        throw DeliveryMovieError(path, "the codec parameters could not be published to the container");
    }
    // The stream states the same conversion and the same layout, so the reader
    // of the finished movie sees exactly what was encoded.
    stream->codecpar->format = layout.pixelFormat;
    stream->codecpar->color_range = static_cast<AVColorRange>(colorRange);
    stream->codecpar->color_space = static_cast<AVColorSpace>(colorMatrix);
    stream->codecpar->color_primaries = static_cast<AVColorPrimaries>(colorPrimaries);
    stream->codecpar->color_trc = static_cast<AVColorTransferCharacteristic>(colorTransfer);
    stream->codecpar->chroma_location = layout.chromaLocation;
    stream->sample_aspect_ratio = codec->sample_aspect_ratio;
    stream->time_base = codec->time_base;
    stream->avg_frame_rate = frameRate;
    stream->r_frame_rate = frameRate;
    if (avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        throw DeliveryMovieError(path, "the output file could not be opened for writing");
    }
    fileOpened = true;
    if (avformat_write_header(format.get(), nullptr) < 0) {
        throw DeliveryMovieError(path, "the container header could not be written");
    }
    packet.reset(av_packet_alloc());
    if (packet == nullptr) {
        throw DeliveryMovieError(path, "packet allocation failed");
    }
}

void DeliveryMovieWriter::Impl::drainPackets() {
    while (true) {
        const int status = avcodec_receive_packet(codec.get(), packet.get());
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return;
        }
        if (status < 0) {
            throw DeliveryMovieError(path, "packet receive failed: " + avError(status));
        }
        ++drained;
        av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
        packet->stream_index = stream->index;
        const int writeStatus = av_interleaved_write_frame(format.get(), packet.get());
        av_packet_unref(packet.get());
        if (writeStatus < 0) {
            throw DeliveryMovieError(path, "packet write failed: " + avError(writeStatus));
        }
    }
}

void DeliveryMovieWriter::Impl::appendFrame(const CpuImage& image) {
    if (finished) {
        throw DeliveryMovieError(path, "the movie is already complete; no further frame can be appended");
    }
    if (image.width() != width || image.height() != height) {
        throw DeliveryMovieError(path, "frame geometry " + std::to_string(image.width()) + "x" +
                                           std::to_string(image.height()) + " does not match the movie's " +
                                           std::to_string(width) + "x" + std::to_string(height));
    }
    if (!description.channels.empty() && description.channels != image.layout().channels) {
        throw DeliveryMovieError(path, "frame channels do not match the movie's declared channels");
    }
    FramePtr frame(av_frame_alloc());
    if (frame == nullptr) {
        throw DeliveryMovieError(path, "frame allocation failed");
    }
    frame->format = layout.pixelFormat;
    frame->width = width;
    frame->height = height;
    // ONE frame buffer per appended frame, freed as soon as the encoder has
    // taken its reference: the writer's own memory never grows with the movie's
    // length.
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        throw DeliveryMovieError(path, "frame buffer allocation failed");
    }
    frame->color_range = codec->color_range;
    frame->colorspace = codec->colorspace;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->chroma_location = layout.chromaLocation;
    frame->sample_aspect_ratio = codec->sample_aspect_ratio;
    writeFramePlanes(image, *frame, layout);
    frame->pts = nextPts++;
    const int sendStatus = avcodec_send_frame(codec.get(), frame.get());
    if (sendStatus < 0) {
        throw DeliveryMovieError(path, "frame submission failed: " + avError(sendStatus));
    }
    ++submitted;
    frame.reset();
    drainPackets();
}

void DeliveryMovieWriter::Impl::complete() {
    if (finished) {
        return;
    }
    if (submitted == 0) {
        throw DeliveryMovieError(path, "no frames were appended; an empty movie is not a deliverable output");
    }
    const int flushStatus = avcodec_send_frame(codec.get(), nullptr);
    if (flushStatus < 0 && flushStatus != AVERROR_EOF) {
        throw DeliveryMovieError(path, "encoder flush failed: " + avError(flushStatus));
    }
    drainPackets();
    if (drained != submitted) {
        throw DeliveryMovieError(path, "the encoder drained " + std::to_string(drained) + " of " +
                                           std::to_string(submitted) + " submitted frames");
    }
    if (av_write_trailer(format.get()) < 0) {
        throw DeliveryMovieError(path, "the container trailer could not be written");
    }
    if (avio_closep(&format->pb) < 0) {
        throw DeliveryMovieError(path, "the output file could not be closed");
    }
    finished = true;
}

DeliveryMovieWriter::DeliveryMovieWriter(const std::string& temporaryPath, const ImageDescription& description,
                                         const DeliveryOutputOptions& options)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = temporaryPath;
    impl_->options = options;
    impl_->description = description;
    if (temporaryPath.empty()) {
        throw DeliveryMovieError(temporaryPath, "no output path was given for the delivery movie");
    }
    if (const std::string problem = validateDeliveryOutput(options); !problem.empty()) {
        throw DeliveryMovieError(temporaryPath, problem);
    }
    if (!isMovieFileType(options.fileType)) {
        throw DeliveryMovieError(temporaryPath,
                                 "file type '" + options.fileType + "' is not a movie format (supported: mov, mp4)");
    }
    if (hasNoImageFormat(description)) {
        throw DeliveryMovieError(temporaryPath, "the delivered description has no image format (width " +
                                                    std::to_string(description.format.width) + ", height " +
                                                    std::to_string(description.format.height) + ")");
    }
    impl_->width = description.format.width;
    impl_->height = description.format.height;
    impl_->layout = layoutFor(options);
    if (impl_->layout.chromaStep == 2 && impl_->width % 2 != 0) {
        throw DeliveryMovieError(temporaryPath, "an " + options.fileType +
                                                    " movie stores 4:2:2 or 4:2:0 chroma, so "
                                                    "the frame width must be even (got " +
                                                    std::to_string(impl_->width) + ")");
    }
    if (impl_->layout.chromaRowsHalved && impl_->height % 2 != 0) {
        throw DeliveryMovieError(temporaryPath,
                                 "an " + options.fileType +
                                     " movie stores 4:2:0 chroma, so the frame height must be even (got " +
                                     std::to_string(impl_->height) + ")");
    }
    if (av_image_check_size(static_cast<unsigned>(impl_->width), static_cast<unsigned>(impl_->height), 0, nullptr) <
        0) {
        throw DeliveryMovieError(temporaryPath, "the frame dimensions are outside the encodable range (" +
                                                    std::to_string(impl_->width) + "x" + std::to_string(impl_->height) +
                                                    ")");
    }
    // Convert the authored rate to FFmpeg's rational time base.
    impl_->frameRate = av_d2q(options.frameRate, 90000);
    if (impl_->frameRate.num <= 0 || impl_->frameRate.den <= 0) {
        throw DeliveryMovieError(temporaryPath, "frame rate " + numberText(options.frameRate) +
                                                    " fps has no usable rational time base");
    }
    impl_->start();
}

DeliveryMovieWriter::~DeliveryMovieWriter() = default;
DeliveryMovieWriter::DeliveryMovieWriter(DeliveryMovieWriter&&) noexcept = default;
DeliveryMovieWriter& DeliveryMovieWriter::operator=(DeliveryMovieWriter&&) noexcept = default;

void DeliveryMovieWriter::append(const CpuImage& image) {
    if (impl_ == nullptr) {
        throw DeliveryMovieError("", "this movie writer is empty (it was moved from)");
    }
    impl_->appendFrame(image);
}

void DeliveryMovieWriter::finish() {
    if (impl_ == nullptr) {
        throw DeliveryMovieError("", "this movie writer is empty (it was moved from)");
    }
    impl_->complete();
}

#else  // NEMO_MEDIA_FFMPEG

// The movie encoder is the FFmpeg adapter (issue #10), which this build does
// not have: the delivery movie path reports that explicitly, exactly as the
// clip-import path does, instead of failing to link or silently writing
// something else. Stills remain fully available.
struct DeliveryMovieWriter::Impl {};

DeliveryMovieWriter::DeliveryMovieWriter(const std::string& temporaryPath, const ImageDescription& description,
                                         const DeliveryOutputOptions& options)
    : impl_(std::make_unique<Impl>()) {
    static_cast<void>(description);
    static_cast<void>(options);
    throw DeliveryMovieError(temporaryPath, "writing a delivery movie requires the FFmpeg/GPU build "
                                            "(NEMO_BUILD_GPU=ON); this build writes still delivery only");
}

DeliveryMovieWriter::~DeliveryMovieWriter() = default;
DeliveryMovieWriter::DeliveryMovieWriter(DeliveryMovieWriter&&) noexcept = default;
DeliveryMovieWriter& DeliveryMovieWriter::operator=(DeliveryMovieWriter&&) noexcept = default;

void DeliveryMovieWriter::append(const CpuImage&) {
    throw DeliveryMovieError("", "writing a delivery movie requires the FFmpeg/GPU build (NEMO_BUILD_GPU=ON)");
}

void DeliveryMovieWriter::finish() {
    throw DeliveryMovieError("", "writing a delivery movie requires the FFmpeg/GPU build (NEMO_BUILD_GPU=ON)");
}

#endif  // NEMO_MEDIA_FFMPEG

}  // namespace nemo::media
