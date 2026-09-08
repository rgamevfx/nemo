#include "nemo/media/VideoDecode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "nemo/gpu/ComputePass.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace nemo::media {

namespace {

// ---------------------------------------------------------------------------
// FFmpeg RAII guards: every AV* object allocated in this file is owned by a
// guard, so allocation failures and thrown errors cannot leak contexts.
// ---------------------------------------------------------------------------

struct FormatGuard {
    AVFormatContext* context = nullptr;
    ~FormatGuard() {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct CodecContextGuard {
    AVCodecContext* context = nullptr;
    ~CodecContextGuard() { avcodec_free_context(&context); }
};

struct PacketGuard {
    AVPacket* packet = nullptr;
    PacketGuard() : packet(av_packet_alloc()) {}
    ~PacketGuard() { av_packet_free(&packet); }
};

struct FrameGuard {
    AVFrame* frame = nullptr;
    FrameGuard() : frame(av_frame_alloc()) {}
    ~FrameGuard() { av_frame_free(&frame); }
};

// Failures always name the clip, the offending format, and the reason.
[[noreturn]] void fail(const std::string& clip, const std::string& format, const std::string& reason, int status = 0) {
    std::string detail;
    if (status < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(status, buffer, sizeof(buffer));
        detail = std::string(" (") + buffer + ")";
    }
    throw MediaDecodeError(clip, format, reason + detail);
}

[[noreturn]] void failStatus(const std::string& clip, const std::string& what, int status = 0) {
    fail(clip, "container/codec", what, status);
}

[[nodiscard]] std::string pixelFormatName(AVPixelFormat format) {
    const char* name = av_get_pix_fmt_name(format);
    return name != nullptr ? name : "unknown pixel format";
}

template <typename T>
[[nodiscard]] const char* colorName(T value, const char* (*lookup)(T), const char* fallback) {
    const char* name = lookup(value);
    return name != nullptr ? name : fallback;
}

[[nodiscard]] std::string declaredProfile(const AVCodecParameters* parameters) {
    if (parameters->profile == AV_PROFILE_UNKNOWN) {
        return {};
    }
    const char* name = avcodec_profile_name(parameters->codec_id, parameters->profile);
    if (name == nullptr) {
        return "profile " + std::to_string(parameters->profile);
    }
    return std::string("profile ") + name;
}

// ---------------------------------------------------------------------------
// Declared color interpretation: an explicitly supported subset. Anything
// outside it is rejected with clip/format/reason — never silently guessed.
// ---------------------------------------------------------------------------

// Chroma arrangement of a supported pixel format.
enum class ChromaKind { Yuv420, Yuv444, Nv12, Gray };

struct FormatSpec {
    AVPixelFormat format;
    ChromaKind kind;
    int depth;
};

constexpr FormatSpec kSupportedFormats[] = {
    {AV_PIX_FMT_YUV420P, ChromaKind::Yuv420, 8},      {AV_PIX_FMT_YUVJ420P, ChromaKind::Yuv420, 8},
    {AV_PIX_FMT_NV12, ChromaKind::Nv12, 8},           {AV_PIX_FMT_YUV444P, ChromaKind::Yuv444, 8},
    {AV_PIX_FMT_YUVJ444P, ChromaKind::Yuv444, 8},     {AV_PIX_FMT_YUV420P10LE, ChromaKind::Yuv420, 10},
    {AV_PIX_FMT_YUV444P10LE, ChromaKind::Yuv444, 10}, {AV_PIX_FMT_GRAY8, ChromaKind::Gray, 8},
    {AV_PIX_FMT_GRAY10LE, ChromaKind::Gray, 10},      {AV_PIX_FMT_GRAY16LE, ChromaKind::Gray, 16},
};

constexpr const char* kSupportedFormatNames =
    "yuv420p, yuvj420p, nv12, yuv444p, yuvj444p, yuv420p10le, yuv444p10le, gray8, gray10le, gray16le";

[[nodiscard]] const FormatSpec* findFormatSpec(AVPixelFormat format) {
    for (const FormatSpec& spec : kSupportedFormats) {
        if (spec.format == format) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] const FormatSpec* requireFormatSpec(AVPixelFormat format, const std::string& clip) {
    const FormatSpec* spec = findFormatSpec(format);
    if (spec == nullptr) {
        fail(clip, pixelFormatName(format),
             "unsupported pixel format; the decoder converts a conservative explicit subset, supported "
             "formats: " +
                 std::string(kSupportedFormatNames));
    }
    return spec;
}

// Inverse transfer with signed extension; matrix expansion may produce
// negative RGB even when the stored YUV samples are unsigned.
[[nodiscard]] float transferToLinear(float value, gpu::MediaTransfer transfer) {
    switch (transfer) {
    case gpu::MediaTransfer::Srgb:
        return value < 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    case gpu::MediaTransfer::Gamma22:
        return std::copysign(std::pow(std::abs(value), 2.2F), value);
    case gpu::MediaTransfer::Gamma28:
        return std::copysign(std::pow(std::abs(value), 2.8F), value);
    case gpu::MediaTransfer::Linear:
        return value;
    case gpu::MediaTransfer::Bt709:
    default:
        return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
    }
}

// One decoded frame → RGBA float32. `linearize` selects the contract: the
// source path inverts the declared transfer into scene-linear Rec.709;
// viewer replay keeps the baked display-referred R′G′B′ untouched. The
// frame's actual pixel format is validated BEFORE any plane access.
[[nodiscard]] CpuImage convertDecodedFrame(const AVFrame* frame, const MediaColorMetadata& color,
                                           const std::string& clip, bool linearize) {
    const FormatSpec* spec = requireFormatSpec(static_cast<AVPixelFormat>(frame->format), clip);
    const std::string formatName = pixelFormatName(spec->format);
    if (frame->width <= 0 || frame->height <= 0 || av_image_check_size(frame->width, frame->height, 0, nullptr) < 0) {
        fail(clip, formatName, "decoded frame has invalid dimensions");
    }
    const bool subsampled = spec->kind == ChromaKind::Yuv420 || spec->kind == ChromaKind::Nv12;
    const int chromaWidth = subsampled ? (frame->width + 1) / 2 : frame->width;
    const int chromaHeight = subsampled ? (frame->height + 1) / 2 : frame->height;
    const int planes = spec->kind == ChromaKind::Gray ? 1 : spec->kind == ChromaKind::Nv12 ? 2 : 3;
    for (int plane = 0; plane < planes; ++plane) {
        const int components = plane == 1 && spec->kind == ChromaKind::Nv12 ? 2 : 1;
        const int rowBytes = (plane == 0 ? frame->width : chromaWidth) * components * (spec->depth > 8 ? 2 : 1);
        if (!frame->data[plane] || std::abs(static_cast<int64_t>(frame->linesize[plane])) < rowBytes) {
            fail(clip, formatName,
                 "decoded frame has missing plane or invalid stride at plane " + std::to_string(plane));
        }
    }
    const auto sample = [&](int plane, int x, int y, int component = 0) -> float {
        const int step = spec->kind == ChromaKind::Nv12 && plane == 1 ? 2 : 1;
        const auto* row = frame->data[plane] + static_cast<ptrdiff_t>(y) * frame->linesize[plane];
        const int offset = x * step + component;
        if (spec->depth == 8)
            return row[offset];
        return static_cast<float>(row[offset * 2] | (static_cast<unsigned>(row[offset * 2 + 1]) << 8));
    };
    const auto chromaSample = [&](int plane, int component, int x, int y) {
        if (!subsampled)
            return sample(plane, x, y, component);
        // Left-sited 4:2:0: centers are (2k, 2j + 0.5) in luma coordinates.
        const float cx = static_cast<float>(x) / 2.0F;
        const float cy = std::max(0.0F, static_cast<float>(y) / 2.0F - 0.25F);
        const int x0 = std::min(static_cast<int>(cx), chromaWidth - 1);
        const int y0 = std::min(static_cast<int>(cy), chromaHeight - 1);
        const int x1 = std::min(x0 + 1, chromaWidth - 1);
        const int y1 = std::min(y0 + 1, chromaHeight - 1);
        const float a = std::lerp(sample(plane, x0, y0, component), sample(plane, x1, y0, component), cx - x0);
        const float b = std::lerp(sample(plane, x0, y1, component), sample(plane, x1, y1, component), cx - x0);
        return std::lerp(a, b, cy - y0);
    };
    const bool fullRange = color.range == gpu::MediaYuvRange::Full;
    const float scale = static_cast<float>(1 << (spec->depth - 8));
    const float maximum = static_cast<float>((1u << spec->depth) - 1);
    const float yOffset = fullRange ? 0.0F : 16.0F * scale;
    const float yRange = fullRange ? maximum : 219.0F * scale;
    const float cRange = fullRange ? maximum : 224.0F * scale;
    const float cOffset = 128.0F * scale;
    CpuImage image(
        ImageLayout{.width = frame->width,
                    .height = frame->height,
                    .color = linearize ? ColorInterpretation::SceneLinear : ColorInterpretation::DisplayReferred});
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            const float yy = (sample(0, x, y) - yOffset) / yRange;
            float uu = 0.0F;
            float vv = 0.0F;
            if (spec->kind != ChromaKind::Gray) {
                uu = (chromaSample(1, 0, x, y) - cOffset) / cRange;
                vv = (chromaSample(spec->kind == ChromaKind::Nv12 ? 1 : 2, spec->kind == ChromaKind::Nv12 ? 1 : 0, x,
                                   y) -
                      cOffset) /
                     cRange;
            }
            const bool bt601 = color.matrix == gpu::MediaMatrix::Bt601;
            float r = yy + (bt601 ? 1.402F : 1.5748F) * vv;
            float g = yy - (bt601 ? 0.344136F : 0.187324F) * uu - (bt601 ? 0.714136F : 0.468124F) * vv;
            float b = yy + (bt601 ? 1.772F : 1.8556F) * uu;
            if (linearize) {
                r = transferToLinear(r, color.transfer);
                g = transferToLinear(g, color.transfer);
                b = transferToLinear(b, color.transfer);
            }
            image.setPixel(x, y, {r, g, b, 1.0F});
        }
    }
    return image;
}

// Resolve the clip's color interpretation from its DECLARED stream metadata.
// Unspecified fields must be resolved by `overrides` — the decoder never
// guesses: unspecified transfer/primaries/matrix/range (and chroma location
// on subsampled formats) is an error naming what is missing. Overrides only
// fill missing fields; a stream-tagged field keeps its declared value.
[[nodiscard]] MediaColorMetadata resolveColor(const AVCodecParameters* parameters, const std::string& clip,
                                              const ColorPolicy& policy, const ColorOverride& overrides) {
    if (policy.workingSpace != "linear") {
        fail(clip, policy.workingSpace,
             "working space '" + policy.workingSpace +
                 "' is outside the decoder's explicit supported subset; only the declared default 'linear' "
                 "(scene-linear Rec.709) is supported for source interpretation");
    }
    const FormatSpec* spec = requireFormatSpec(static_cast<AVPixelFormat>(parameters->format), clip);

    MediaColorMetadata color;
    color.bitDepth = spec->depth;

    const std::string formatName = pixelFormatName(spec->format);

    // Transfer characteristic.
    switch (parameters->color_trc) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:  // identical published curve to BT.709
        color.transfer = gpu::MediaTransfer::Bt709;
        break;
    case AVCOL_TRC_IEC61966_2_1:
        color.transfer = gpu::MediaTransfer::Srgb;
        break;
    case AVCOL_TRC_GAMMA22:
        color.transfer = gpu::MediaTransfer::Gamma22;
        break;
    case AVCOL_TRC_GAMMA28:
        color.transfer = gpu::MediaTransfer::Gamma28;
        break;
    case AVCOL_TRC_LINEAR:
        color.transfer = gpu::MediaTransfer::Linear;
        break;
    case AVCOL_TRC_UNSPECIFIED:
        if (overrides.transfer.has_value()) {
            color.transfer = *overrides.transfer;
        } else {
            fail(clip, formatName,
                 "source transfer characteristic is unspecified; interpreting it is unsupported without "
                 "an explicit ColorOverride.transfer — the decoder never guesses a transfer "
                 "(or re-tag the clip)");
        }
        break;
    default:
        fail(clip, formatName,
             std::string("unsupported source transfer characteristic ") +
                 colorName(parameters->color_trc, av_color_transfer_name, "?") +
                 " (supported: bt709, smpte170m, iec61966-2-1/srgb, gamma22, gamma28, linear)");
    }

    // Primaries: only Rec.709 — the working space is scene-linear Rec.709,
    // so no chromaticity mapping exists yet; other primaries must not be
    // passed through unconverted (that would relabel them Rec.709).
    switch (parameters->color_primaries) {
    case AVCOL_PRI_BT709:
        color.primaries = gpu::MediaPrimaries::Bt709;
        break;
    case AVCOL_PRI_UNSPECIFIED:
        if (overrides.primaries.has_value()) {
            color.primaries = *overrides.primaries;
        } else {
            fail(clip, formatName,
                 "source primaries are unspecified; interpreting them is unsupported without an "
                 "explicit ColorOverride.primaries (scene-linear Rec.709 is the only supported target, "
                 "so only bt709 can be declared)");
        }
        break;
    default:
        fail(clip, formatName,
             std::string("unsupported source primaries ") +
                 colorName(parameters->color_primaries, av_color_primaries_name, "?") +
                 " (only bt709 is supported: mapping other chromaticities into the Rec.709 working "
                 "space is not implemented, and the decoder will not silently assume them)");
    }

    // Matrix coefficients and chroma position — meaningless for grayscale
    // (no chroma); the position is also meaningless at 4:4:4.
    if (spec->kind != ChromaKind::Gray) {
        switch (parameters->color_space) {
        case AVCOL_SPC_BT709:
            color.matrix = gpu::MediaMatrix::Bt709;
            break;
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
            color.matrix = gpu::MediaMatrix::Bt601;
            break;
        case AVCOL_SPC_UNSPECIFIED:
            if (overrides.matrix.has_value()) {
                color.matrix = *overrides.matrix;
            } else {
                fail(clip, formatName,
                     "source matrix coefficients are unspecified; interpreting them is unsupported "
                     "without an explicit ColorOverride.matrix");
            }
            break;
        default:
            fail(clip, formatName,
                 std::string("unsupported source matrix coefficients ") +
                     colorName(parameters->color_space, av_color_space_name, "?") +
                     " (supported: bt709, smpte170m/bt470bg)");
        }

        if (spec->kind == ChromaKind::Yuv420 || spec->kind == ChromaKind::Nv12) {
            switch (parameters->chroma_location) {
            case AVCHROMA_LOC_LEFT:
                color.chromaLocation = gpu::MediaChromaLocation::Left;
                break;
            case AVCHROMA_LOC_UNSPECIFIED:
                if (overrides.chromaLocation.has_value()) {
                    color.chromaLocation = *overrides.chromaLocation;
                } else {
                    fail(clip, formatName,
                         "source chroma sample position is unspecified; interpreting it is unsupported "
                         "without an explicit ColorOverride.chromaLocation (only left is supported)");
                }
                break;
            default:
                fail(clip, formatName,
                     std::string("unsupported source chroma sample position ") +
                         colorName(parameters->chroma_location, av_chroma_location_name, "?") +
                         " (only left is supported)");
            }
        }
    }

    // Quantization range.
    switch (parameters->color_range) {
    case AVCOL_RANGE_MPEG:
        color.range = gpu::MediaYuvRange::Limited;
        break;
    case AVCOL_RANGE_JPEG:
        color.range = gpu::MediaYuvRange::Full;
        break;
    case AVCOL_RANGE_UNSPECIFIED:
        if (overrides.range.has_value()) {
            color.range = *overrides.range;
        } else if (spec->format == AV_PIX_FMT_YUVJ420P || spec->format == AV_PIX_FMT_YUVJ444P) {
            color.range = gpu::MediaYuvRange::Full;  // YUVJ declares full range by definition
        } else {
            fail(clip, formatName,
                 "source quantization range is unspecified; interpreting it is unsupported without an "
                 "explicit ColorOverride.range");
        }
        break;
    default:
        fail(clip, formatName, "source quantization range is invalid");
    }

    if (parameters->width <= 0 || parameters->height <= 0) {
        fail(clip, formatName, "stream declares non-positive dimensions");
    }
    return color;
}

[[nodiscard]] MediaColorMetadata frameColor(const AVFrame* frame, const AVCodecParameters* stream,
                                            const std::string& clip, const ColorPolicy& policy,
                                            const ColorOverride& overrides) {
    AVCodecParameters tags = *stream;  // Borrowed pointers; only scalar metadata is inspected.
    tags.format = frame->format;
    tags.width = frame->width;
    tags.height = frame->height;
    if (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
        fail(clip, pixelFormatName(static_cast<AVPixelFormat>(frame->format)), "corrupt decoded frame");
    }
    if (frame->format == AV_PIX_FMT_VULKAN) {
        if (!frame->hw_frames_ctx || !frame->data[0])
            fail(clip, "vulkan", "missing hardware frame context or planes");
        const auto* hw = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (hw->format != AV_PIX_FMT_VULKAN || hw->sw_format != AV_PIX_FMT_NV12) {
            fail(clip, pixelFormatName(hw->sw_format), "unsupported Vulkan surface format; expected nv12");
        }
        tags.format = hw->sw_format;
    }
    if (frame->color_trc != AVCOL_TRC_UNSPECIFIED)
        tags.color_trc = frame->color_trc;
    if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED)
        tags.color_primaries = frame->color_primaries;
    if (frame->colorspace != AVCOL_SPC_UNSPECIFIED)
        tags.color_space = frame->colorspace;
    if (frame->color_range != AVCOL_RANGE_UNSPECIFIED)
        tags.color_range = frame->color_range;
    if (frame->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
        tags.chroma_location = frame->chroma_location;
    return resolveColor(&tags, clip, policy, overrides);
}

// Compiled mediaConvert SPIR-V for the Vulkan-resident conversion.
[[nodiscard]] std::vector<std::uint32_t> loadConvertSpirv(const std::filesystem::path& file, const std::string& clip) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        fail(clip, file.string(), "mediaConvert SPIR-V not found (build the nemo_shaders target)");
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.size() < 4 || bytes.size() % 4 != 0 || std::memcmp(bytes.data(), "\x03\x02\x23\x07", 4) != 0) {
        fail(clip, file.string(), "is not a valid SPIR-V module");
    }
    std::vector<std::uint32_t> spirv(bytes.size() / 4);
    std::memcpy(spirv.data(), bytes.data(), bytes.size());
    return spirv;
}

[[nodiscard]] double frameRateOf(const AVStream* stream) {
    const AVRational rate = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    return rate.num != 0 ? static_cast<double>(rate.num) / static_cast<double>(rate.den) : 0.0;
}

// Owns common initialization without opening the codec: the native path must
// attach its Vulkan device and format callback before openCodec().
struct PreparedDecoder {
    FormatGuard format;
    CodecContextGuard codec;
    int streamIndex = -1;
    AVStream* stream = nullptr;             // Borrowed from format.
    const AVCodec* decoderCodec = nullptr;  // FFmpeg's static registry.

    explicit PreparedDecoder(const std::string& path) {
        const int openStatus = avformat_open_input(&format.context, path.c_str(), nullptr, nullptr);
        if (openStatus < 0) {
            failStatus(path, "avformat_open_input failed", openStatus);
        }
        if (avformat_find_stream_info(format.context, nullptr) < 0) {
            failStatus(path, "avformat_find_stream_info failed");
        }
        streamIndex = av_find_best_stream(format.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            failStatus(path, "no video stream");
        }
        stream = format.context->streams[streamIndex];
        decoderCodec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoderCodec == nullptr) {
            failStatus(path, "no decoder for codec id " + std::to_string(stream->codecpar->codec_id));
        }
        codec.context = avcodec_alloc_context3(decoderCodec);
        if (codec.context == nullptr) {
            failStatus(path, "avcodec_alloc_context3 failed");
        }
        if (avcodec_parameters_to_context(codec.context, stream->codecpar) < 0) {
            failStatus(path, "avcodec_parameters_to_context failed");
        }
        codec.context->thread_count = 1;  // Deterministic decode for image assertions.
        codec.context->err_recognition = AV_EF_EXPLODE;
    }

    PreparedDecoder(const PreparedDecoder&) = delete;
    PreparedDecoder& operator=(const PreparedDecoder&) = delete;

    [[nodiscard]] ClipInfo openCodec(const std::string& path) {
        const int status = avcodec_open2(codec.context, decoderCodec, nullptr);
        if (status < 0) {
            const std::string profile = declaredProfile(stream->codecpar);
            fail(path, profile,
                 std::string("avcodec_open2 failed") + (profile.empty() ? "" : " (unsupported " + profile + ")"),
                 status);
        }
        return {path,
                avcodec_get_name(codec.context->codec_id),
                codec.context->width,
                codec.context->height,
                frameRateOf(stream),
                stream->nb_frames > 0 ? stream->nb_frames : -1};
    }
};

}  // namespace

struct ClipDecoder::Impl {
    FormatGuard format;
    int streamIndex = -1;
    AVCodecContext* codecContext = nullptr;
    AVBufferRef* hwDevice = nullptr;
    int hardwareSetupError = 0;
    std::unique_ptr<gpu::MediaInterop> interop;
    gpu::Device* device = nullptr;
    gpu::Allocator* allocator = nullptr;
    std::unique_ptr<gpu::SubmissionQueue> queue;
    ClipInfo info;
    DecodeDecision decision;
    MediaColorMetadata color;
    ColorPolicy policy;
    ColorOverride overrides;
    bool opened = false;
    bool endOfStreamReached = false;
    int64_t decodedFrameCount = 0;
    int64_t framesDecodedHardware = 0;
    // Extension names handed to FFmpeg's AVVulkanDeviceContext: owned per
    // decoder instance for its whole lifetime (strings AND the char* array)
    // — never a borrowed thread-local array.
    std::vector<std::string> extensionStorage;
    std::vector<const char*> extensionNames;

    // Destruction order: the codec closes first (it frees its video
    // session and internal command pools on the device), then the frame
    // pool, then the device context. Reverse order leaves FFmpeg objects
    // alive across the device teardown and trips validation.
    ~Impl() {
        if (codecContext != nullptr) {
            avcodec_free_context(&codecContext);
        }
        if (hwDevice != nullptr) {
            av_buffer_unref(&hwDevice);
        }
    }
};

ClipDecoder::~ClipDecoder() = default;

std::unique_ptr<ClipDecoder> ClipDecoder::open(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                               const std::string& path, const std::filesystem::path& convertSpirv,
                                               const ColorPolicy& policy, const ColorOverride& overrides) {
    auto decoder = std::unique_ptr<ClipDecoder>(new ClipDecoder());
    auto impl = std::make_unique<Impl>();
    decoder->impl_ = std::move(impl);
    Impl& d = *decoder->impl_;

    PreparedDecoder prepared(path);
    auto& codec = prepared.codec;
    AVStream* stream = prepared.stream;

    // Resolve the declared color interpretation BEFORE path selection: an
    // interpretation outside the explicit subset is a hard error for both
    // paths (no silent relabeling as scene-linear).
    d.color = resolveColor(stream->codecpar, path, policy, overrides);
    d.policy = policy;
    d.overrides = overrides;

    // Capability-measured decode path selection (issues #10/#21): Vulkan
    // video decode is used exactly when the device reserved a decode queue
    // AND libavcodec ships a Vulkan frame configuration for this codec AND
    // the stream decodes into the 8-bit 4:2:0 surfaces the device-resident
    // converter consumes. Otherwise software decode with the precise
    // recorded reason — no silent substitution.
    bool hasVulkanConfig = false;
    for (int configIndex = 0;; ++configIndex) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(prepared.decoderCodec, configIndex);
        if (config == nullptr) {
            break;
        }
        if (config->pix_fmt == AV_PIX_FMT_VULKAN) {
            hasVulkanConfig = true;
            break;
        }
    }
    const FormatSpec* streamSpec = findFormatSpec(static_cast<AVPixelFormat>(stream->codecpar->format));
    const bool nv12SurfaceStream = streamSpec != nullptr && streamSpec->depth == 8 &&
                                   (streamSpec->kind == ChromaKind::Yuv420 || streamSpec->kind == ChromaKind::Nv12);
    if (device.decode_family() && hasVulkanConfig && nv12SurfaceStream) {
        // AVVulkanDeviceContext over the application device: decode runs on
        // the device's video decode queue; the produced frames are
        // Vulkan-resident planes on the application device, so interop
        // needs no external-memory bridge at all.
        AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (deviceRef == nullptr) {
            failStatus(path, "av_hwdevice_ctx_alloc (vulkan) failed");
        }
        d.hwDevice = deviceRef;
        AVHWDeviceContext* deviceContext = reinterpret_cast<AVHWDeviceContext*>(deviceRef->data);
        AVVulkanDeviceContext* vulkan = reinterpret_cast<AVVulkanDeviceContext*>(deviceContext->hwctx);
        vulkan->get_proc_addr = &vkGetInstanceProcAddr;
        vulkan->inst = instance.handle();
        vulkan->phys_dev = device.physical();
        vulkan->act_dev = device.handle();
        vulkan->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vulkan->queue_family_index = device.graphics_family();
        vulkan->nb_graphics_queues = 1;
        vulkan->queue_family_tx_index = device.transfer_family();
        vulkan->nb_tx_queues = 1;
        vulkan->queue_family_comp_index = device.graphics_family();
        vulkan->nb_comp_queues = 1;
        vulkan->queue_family_encode_index = device.encode_family().value_or(-1);
        vulkan->nb_encode_queues = device.encode_family() ? 1 : 0;
        vulkan->queue_family_decode_index = device.decode_family().value_or(-1);
        vulkan->nb_decode_queues = device.decode_family() ? 1 : 0;
        deviceContext->user_opaque = &device;
        vulkan->lock_queue = [](AVHWDeviceContext* context, uint32_t family, uint32_t) {
            static_cast<gpu::Device*>(context->user_opaque)->queueMutex(family).lock();
        };
        vulkan->unlock_queue = [](AVHWDeviceContext* context, uint32_t family, uint32_t) {
            static_cast<gpu::Device*>(context->user_opaque)->queueMutex(family).unlock();
        };
        // Extension names are owned by this decoder instance for its whole
        // lifetime (strings AND the char* array).
        d.extensionStorage.reserve(device.enabled_extensions().size());
        for (const std::string& extension : device.enabled_extensions()) {
            d.extensionStorage.push_back(extension);
        }
        d.extensionNames.reserve(d.extensionStorage.size());
        for (const std::string& extension : d.extensionStorage) {
            d.extensionNames.push_back(extension.c_str());
        }
        vulkan->enabled_dev_extensions = d.extensionNames.data();
        vulkan->nb_enabled_dev_extensions = static_cast<int>(d.extensionNames.size());

        const int deviceInitStatus = av_hwdevice_ctx_init(deviceRef);
        if (deviceInitStatus < 0) {
            av_buffer_unref(&d.hwDevice);
            d.decision = {false, "Vulkan hwdevice init failed over the application device"};
        }
    } else if (!device.decode_family()) {
        d.decision = {false, "device has no reserved Vulkan video decode queue family"};
    } else if (!hasVulkanConfig) {
        d.decision = {false, "libavcodec build has no Vulkan hwaccel for this codec"};
    } else {
        d.decision = {false, "stream pixel format " +
                                 pixelFormatName(static_cast<AVPixelFormat>(stream->codecpar->format)) +
                                 " does not decode into the 8-bit 4:2:0 surfaces the device-resident "
                                 "converter consumes"};
    }

    if (d.hwDevice != nullptr) {
        // The codec context takes its own references; both are released by
        // avcodec_free_context when the decoder is destroyed.
        codec.context->hw_device_ctx = av_buffer_ref(d.hwDevice);
        if (codec.context->hw_device_ctx == nullptr) {
            failStatus(path, "av_buffer_ref (vulkan hw contexts) failed");
        }
        d.decision = {true, {}};
        // The interop converter consumes the device-resident planes.
        d.interop = gpu::MediaInterop::create(device, allocator, loadConvertSpirv(convertSpirv, path));
    }

    // FFmpeg unrefs hw_frames_ctx before each get_format callback; attach
    // a fresh reference here, not before avcodec_open2.
    codec.context->opaque = &d;
    codec.context->get_format = [](AVCodecContext* context, const enum AVPixelFormat* formats)->enum AVPixelFormat {
        auto& state = *static_cast<Impl*>(context->opaque);
        // Sequence headers may change after open; never force a new 10-bit
        // or 4:4:4 sequence into the eight-bit NV12 conversion contract.
        const bool nv12Compatible = context->sw_pix_fmt == AV_PIX_FMT_YUV420P ||
                                    context->sw_pix_fmt == AV_PIX_FMT_YUVJ420P ||
                                    context->sw_pix_fmt == AV_PIX_FMT_NV12;
        if (state.hwDevice != nullptr && nv12Compatible) {
            for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == AV_PIX_FMT_VULKAN) {
                    AVBufferRef* frames = nullptr;
                    int status = avcodec_get_hw_frames_parameters(context, state.hwDevice, *format, &frames);
                    if (status < 0) {
                        av_buffer_unref(&frames);
                        state.hardwareSetupError = status;
                        break;
                    }
                    auto* pool = reinterpret_cast<AVHWFramesContext*>(frames->data);
                    pool->sw_format = AV_PIX_FMT_NV12;
                    auto* vkPool = static_cast<AVVulkanFramesContext*>(pool->hwctx);
                    vkPool->flags =
                        static_cast<AVVkFrameFlags>(AV_VK_FRAME_FLAG_NONE | AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE);
                    vkPool->usage = static_cast<VkImageUsageFlagBits>(vkPool->usage | VK_IMAGE_USAGE_SAMPLED_BIT);
                    status = av_hwframe_ctx_init(frames);
                    if (status < 0) {
                        av_buffer_unref(&frames);
                        state.hardwareSetupError = status;
                        break;
                    }
                    context->hw_frames_ctx = frames;
                    return AV_PIX_FMT_VULKAN;
                }
            }
        }
        for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*format);
            if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
                return *format;
        }
        return AV_PIX_FMT_NONE;
    };
    d.info = prepared.openCodec(path);
    d.codecContext = std::exchange(codec.context, nullptr);
    d.format.context = std::exchange(prepared.format.context, nullptr);
    d.streamIndex = prepared.streamIndex;
    d.device = &device;
    d.allocator = &allocator;

    d.queue = std::make_unique<gpu::SubmissionQueue>(device, device.graphics_family());
    d.opened = true;
    return decoder;
}

const ClipInfo& ClipDecoder::info() const {
    return impl_->info;
}

const DecodeDecision& ClipDecoder::decision() const {
    return impl_->decision;
}

std::unique_ptr<gpu::Image> ClipDecoder::next(uint64_t timeout_ns) {
    Impl& impl = *impl_;
    if (!impl.opened) {
        failStatus(impl.info.path, "decoder is not open");
    }
    if (impl.endOfStreamReached) {
        return nullptr;
    }

    PacketGuard packet;
    FrameGuard frame;
    if (packet.packet == nullptr || frame.frame == nullptr) {
        failStatus(impl.info.path, "av_packet_alloc/av_frame_alloc failed");
    }
    while (true) {
        const int receiveStatus = avcodec_receive_frame(impl.codecContext, frame.frame);
        if (receiveStatus == 0) {
            break;
        }
        if (receiveStatus == AVERROR_EOF) {
            impl.endOfStreamReached = true;
            return nullptr;
        }
        if (receiveStatus != AVERROR(EAGAIN)) {
            failStatus(impl.info.path, "avcodec_receive_frame failed", receiveStatus);
        }
        // Feed more input: read packets until the decoder accepts one (or
        // the stream ends, which flushes the decoder).
        while (true) {
            const int readStatus = av_read_frame(impl.format.context, packet.packet);
            if (readStatus == AVERROR_EOF) {
                const int sendStatus = avcodec_send_packet(impl.codecContext, nullptr);  // flush
                if (sendStatus < 0 && sendStatus != AVERROR_EOF) {
                    failStatus(impl.info.path, "avcodec_send_packet (flush) failed", sendStatus);
                }
                break;
            }
            if (readStatus < 0) {
                failStatus(impl.info.path, "av_read_frame failed", readStatus);
            }
            if (packet.packet->stream_index != impl.streamIndex) {
                av_packet_unref(packet.packet);
                continue;
            }
            const int sendStatus = avcodec_send_packet(impl.codecContext, packet.packet);
            av_packet_unref(packet.packet);
            if (sendStatus == 0) {
                break;
            }
            failStatus(impl.info.path, "avcodec_send_packet failed", sendStatus);
        }
    }

    ++impl.decodedFrameCount;
    impl.color = frameColor(frame.frame, impl.format.context->streams[impl.streamIndex]->codecpar, impl.info.path,
                            impl.policy, impl.overrides);
    if (frame.frame->format == AV_PIX_FMT_VULKAN) {
        impl.decision = {true, {}};
        // Hardware path: frames are Vulkan-resident NV12 planes on the
        // application device; the conversion kernel keeps every pixel on
        // device and interprets them per the DECLARED color metadata. No
        // CPU readback occurs here.
        auto* vkFrame = reinterpret_cast<AVVkFrame*>(frame.frame->data[0]);
        gpu::ForeignVideoFrame foreign;
        AVFrame* retainedFrame = av_frame_clone(frame.frame);
        if (retainedFrame == nullptr)
            failStatus(impl.info.path, "av_frame_clone failed");
        foreign.owner = std::shared_ptr<AVFrame>(retainedFrame, [](AVFrame* retained) { av_frame_free(&retained); });
        foreign.transfer = impl.color.transfer;
        foreign.matrix = impl.color.matrix;
        foreign.range = impl.color.range;
        foreign.chromaLocation = impl.color.chromaLocation;
        foreign.primaries = impl.color.primaries;
        if (vkFrame->img[0] == VK_NULL_HANDLE || vkFrame->sem[0] == VK_NULL_HANDLE) {
            fail(impl.info.path, "vulkan", "decoded frame has missing image or semaphore");
        }
        // FFmpeg 6.1 allocates NV12 as a single two-plane multiplane image
        // by default; honor the recorded form instead of assuming.
        foreign.multiplane = vkFrame->img[1] == VK_NULL_HANDLE;
        if (foreign.multiplane) {
            foreign.planeCount = 1;
            foreign.images[0] = vkFrame->img[0];
            foreign.formats[0] = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        } else {
            foreign.planeCount = 2;
            foreign.images[0] = vkFrame->img[0];
            foreign.images[1] = vkFrame->img[1];
            foreign.formats[0] = VK_FORMAT_R8_UNORM;
            foreign.formats[1] = VK_FORMAT_R8G8_UNORM;
            if (vkFrame->sem[1] == VK_NULL_HANDLE)
                fail(impl.info.path, "nv12", "missing chroma semaphore");
        }
        foreign.semaphores[0] = vkFrame->sem[0];
        foreign.semaphores[1] = vkFrame->sem[1];
        foreign.waitValues[0] = vkFrame->sem_value[0];
        foreign.waitValues[1] = vkFrame->sem_value[1];
        foreign.queueFamilies[0] = vkFrame->queue_family[0];
        foreign.queueFamilies[1] = vkFrame->queue_family[1];
        foreign.layouts[0] = vkFrame->layout[0];
        foreign.layouts[1] = vkFrame->layout[1];
        foreign.accesses[0] = vkFrame->access[0];
        foreign.accesses[1] = vkFrame->access[1];
        // Vulkan video decode writes the planes on the decode queue
        // (2.x-named stage bit; numerically the same stage in the sync-1
        // pipeline-barrier API the interop path submits).
        foreign.producerStages[0] = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        foreign.producerStages[1] = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        foreign.width = static_cast<uint32_t>(frame.frame->width);
        foreign.height = static_cast<uint32_t>(frame.frame->height);

        auto output = std::make_unique<gpu::Image>(
            impl.allocator->create_image(foreign.width, foreign.height, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        const auto completion = impl.interop->submitToRgba32f(foreign, *output);
        if (!completion)
            fail(impl.info.path, "vulkan", "GPU submission capacity exhausted");

        // Hand the updated interop state back to FFmpeg for plane reuse.
        vkFrame->sem_value[0] = foreign.waitValues[0];
        vkFrame->sem_value[1] = foreign.waitValues[1];
        vkFrame->access[0] = static_cast<VkAccessFlagBits>(foreign.accesses[0]);
        vkFrame->access[1] = static_cast<VkAccessFlagBits>(foreign.accesses[1]);
        auto& submissions = impl.device->submissions(impl.device->graphics_family());
        try {
            if (!submissions.wait(*completion, timeout_ns))
                throw gpu::GpuException(gpu::GpuError::SubmissionTimeout, "decoded frame conversion timed out");
        } catch (...) {
            // Decoder teardown owns FFmpeg extension-name storage. This
            // synchronous convenience path drains before that state unwinds.
            submissions.drain();
            throw;
        }

        impl.framesDecodedHardware++;
        return output;
    }

    if (impl.decision.hardware) {
        impl.decision = {false, "hardware decode rejected the clip/profile; FFmpeg produced software " +
                                    pixelFormatName(static_cast<AVPixelFormat>(frame.frame->format))};
        if (impl.hardwareSetupError < 0) {
            char detail[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(impl.hardwareSetupError, detail, sizeof(detail));
            impl.decision.reason += std::string("; Vulkan frame setup: ") + detail;
        }
    }

    // Measured software path (chosen explicitly at open time when the
    // device has no usable Vulkan video configuration): the actual frame is
    // validated and converted per its DECLARED interpretation to the
    // scene-linear contract, then uploaded to device residency. The upload
    // is the capability-dependent transfer cost this path carries.
    const CpuImage pixels = convertDecodedFrame(frame.frame, impl.color, impl.info.path, /*linearize=*/true);
    auto output = std::make_unique<gpu::Image>(impl.allocator->create_image(
        static_cast<uint32_t>(frame.frame->width), static_cast<uint32_t>(frame.frame->height), 1,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    gpu::uploadImage(*impl.queue, *impl.allocator, *output, pixels.data(),
                     static_cast<size_t>(pixels.width()) * pixels.height() * 4 * sizeof(float), timeout_ns);
    // Keep the contract layout consistent with the interop path: GENERAL.
    gpu::imageBarrier(*impl.queue, *output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, timeout_ns);
    return output;
}

// Shared software-decode engine: opens the clip, resolves the DECLARED color
// interpretation, and converts every frame to RGBA float32. `linearize`
// selects the contract: the source path inverts the declared transfer into
// scene-linear working images; viewer replay keeps the baked
// display-referred representation untouched.
[[nodiscard]] SoftwareClip decodeSoftware(const std::string& path, int64_t maxFrames, bool linearize,
                                          const ColorPolicy& policy, const ColorOverride& overrides) {
    SoftwareClip result;
    PreparedDecoder prepared(path);
    auto& format = prepared.format;
    auto& context = prepared.codec;
    const int streamIndex = prepared.streamIndex;
    AVStream* stream = prepared.stream;
    result.info = prepared.openCodec(path);

    result.metadata = resolveColor(stream->codecpar, path, policy, overrides);

    PacketGuard packet;
    FrameGuard decoded;
    if (packet.packet == nullptr || decoded.frame == nullptr) {
        failStatus(path, "av_packet_alloc/av_frame_alloc failed");
    }
    const auto convertFrame = [&](const AVFrame* yuv) {
        const auto color = frameColor(yuv, stream->codecpar, path, policy, overrides);
        if (!linearize &&
            (color.transfer != gpu::MediaTransfer::Bt709 || color.matrix != gpu::MediaMatrix::Bt709 ||
             color.range != gpu::MediaYuvRange::Limited || color.bitDepth != 8 || yuv->format != AV_PIX_FMT_YUV420P)) {
            fail(path, pixelFormatName(static_cast<AVPixelFormat>(yuv->format)),
                 "unsupported viewer representation; expected tagged limited-range BT.709 8-bit yuv420p");
        }
        if (!result.frames.empty() && color != result.metadata) {
            fail(path, pixelFormatName(static_cast<AVPixelFormat>(yuv->format)),
                 "changing color interpretation within a clip is unsupported");
        }
        result.metadata = color;
        result.frames.push_back(convertDecodedFrame(yuv, color, path, linearize));
    };
    if (maxFrames == 0)
        return result;

    bool flushed = false;
    while (true) {
        int receiveStatus;
        while ((receiveStatus = avcodec_receive_frame(context.context, decoded.frame)) == 0) {
            convertFrame(decoded.frame);
            av_frame_unref(decoded.frame);
            if (maxFrames >= 0 && static_cast<int64_t>(result.frames.size()) >= maxFrames) {
                return result;
            }
        }
        if (receiveStatus != AVERROR(EAGAIN) && receiveStatus != AVERROR_EOF) {
            failStatus(path, "avcodec_receive_frame failed", receiveStatus);
        }
        if (flushed) {
            break;  // flushed decoder drained dry
        }
        int readStatus = av_read_frame(format.context, packet.packet);
        if (readStatus == AVERROR_EOF) {
            const int sendStatus = avcodec_send_packet(context.context, nullptr);
            if (sendStatus < 0 && sendStatus != AVERROR_EOF) {
                failStatus(path, "avcodec_send_packet (flush) failed", sendStatus);
            }
            flushed = true;
            continue;
        }
        if (readStatus < 0) {
            failStatus(path, "av_read_frame failed", readStatus);
        }
        if (packet.packet->stream_index != streamIndex) {
            av_packet_unref(packet.packet);
            continue;
        }
        const int sendStatus = avcodec_send_packet(context.context, packet.packet);
        av_packet_unref(packet.packet);
        if (sendStatus < 0) {
            failStatus(path, "avcodec_send_packet failed", sendStatus);
        }
    }
    return result;
}

SoftwareClip decodeClipSoftware(const std::string& path, int64_t maxFrames, const ColorPolicy& policy,
                                const ColorOverride& overrides) {
    return decodeSoftware(path, maxFrames, /*linearize=*/true, policy, overrides);
}

SoftwareClip decodeViewerChunkSoftware(const std::string& path, int64_t maxFrames) {
    return decodeSoftware(path, maxFrames, /*linearize=*/false, ColorPolicy{}, ColorOverride{});
}

}  // namespace nemo::media
