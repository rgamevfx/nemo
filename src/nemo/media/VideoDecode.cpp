#include "nemo/media/VideoDecode.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "nemo/gpu/ChannelImage.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/media/ViewingTransform.hpp"

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

// Channels of one decoded clip frame: the contract's R, G, B and A, which the
// shared native layout stores as the four components of one packed RGBA32F
// texel per logical pixel (issue #98). Video carries no alpha, so its alpha
// component stays exactly 1.0. The count is what both decode paths and
// `convertDecodedFrame` produce; the packed device extent is the logical
// raster, which `nativeChannelHeight` computes.
constexpr std::uint32_t kDecodedChannels = kImageChannels;

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

// The RGB transfer vocabulary is shared with the image path (InputColor.hpp);
// the decode kernel's enum carries the same five curves.
[[nodiscard]] gpu::MediaTransfer toGpuTransfer(const ImageTransfer transfer) {
    switch (transfer) {
    case ImageTransfer::Linear:
        return gpu::MediaTransfer::Linear;
    case ImageTransfer::Srgb:
        return gpu::MediaTransfer::Srgb;
    case ImageTransfer::Gamma22:
        return gpu::MediaTransfer::Gamma22;
    case ImageTransfer::Gamma28:
        return gpu::MediaTransfer::Gamma28;
    case ImageTransfer::Bt709:
        return gpu::MediaTransfer::Bt709;
    }
    return gpu::MediaTransfer::Bt709;
}

[[nodiscard]] ImageTransfer toImageTransfer(const gpu::MediaTransfer transfer) {
    switch (transfer) {
    case gpu::MediaTransfer::Linear:
        return ImageTransfer::Linear;
    case gpu::MediaTransfer::Srgb:
        return ImageTransfer::Srgb;
    case gpu::MediaTransfer::Gamma22:
        return ImageTransfer::Gamma22;
    case gpu::MediaTransfer::Gamma28:
        return ImageTransfer::Gamma28;
    case gpu::MediaTransfer::Bt709:
        return ImageTransfer::Bt709;
    }
    return ImageTransfer::Bt709;
}

// Plane count of a supported format: gray is one plane, NV12 is two
// (luma + interleaved chroma), planar 4:2:0/4:4:4 is three.
[[nodiscard]] constexpr int planeCountOf(const ChromaKind kind) {
    switch (kind) {
    case ChromaKind::Gray:
        return 1;
    case ChromaKind::Nv12:
        return 2;
    case ChromaKind::Yuv420:
    case ChromaKind::Yuv444:
        return 3;
    }
    return 3;
}

// One decoded frame → RGBA float32. `linearize` selects the contract: the
// source path inverts the declared transfer into scene-linear Rec.709;
// viewer replay keeps the baked display-referred R′G′B′ untouched. The
// frame's actual pixel format is validated BEFORE any plane access. When
// `decodedFormat`/`decodedPlanes` are non-null they receive that validated
// format name and plane count, so callers do not re-derive them.
[[nodiscard]] CpuImage convertDecodedFrame(const AVFrame* frame, const MediaColorMetadata& color,
                                           const std::string& clip, bool linearize, int width = 0, int height = 0,
                                           std::string* decodedFormat = nullptr, int* decodedPlanes = nullptr,
                                           const ResolvedInputColor* rgb = nullptr) {
    const FormatSpec* spec = requireFormatSpec(static_cast<AVPixelFormat>(frame->format), clip);
    const std::string formatName = pixelFormatName(spec->format);
    if (decodedFormat != nullptr) {
        *decodedFormat = formatName;
    }
    const int planes = planeCountOf(spec->kind);
    if (decodedPlanes != nullptr) {
        *decodedPlanes = planes;
    }
    if (frame->width <= 0 || frame->height <= 0 || av_image_check_size(frame->width, frame->height, 0, nullptr) < 0) {
        fail(clip, formatName, "decoded frame has invalid dimensions");
    }
    const bool subsampled = spec->kind == ChromaKind::Yuv420 || spec->kind == ChromaKind::Nv12;
    const int chromaWidth = subsampled ? (frame->width + 1) / 2 : frame->width;
    const int chromaHeight = subsampled ? (frame->height + 1) / 2 : frame->height;
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
    if (width == 0)
        width = frame->width;
    if (height == 0)
        height = frame->height;
    // Raw/Data samples are non-color data; the viewer-replay path is
    // display-referred; everything else is working-space scene-linear.
    const ColorInterpretation interpretation =
        !linearize ? ColorInterpretation::DisplayReferred
                   : (rgb != nullptr && rgb->raw() ? ColorInterpretation::Data : ColorInterpretation::SceneLinear);
    CpuImage image(ImageLayout{.width = width, .height = height, .color = interpretation});
    for (int outputY = 0; outputY < height; ++outputY) {
        const int y = static_cast<int>((static_cast<int64_t>(outputY) * 2 + 1) * frame->height / (2 * height));
        for (int outputX = 0; outputX < width; ++outputX) {
            const int x = static_cast<int>((static_cast<int64_t>(outputX) * 2 + 1) * frame->width / (2 * width));
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
                // An OCIO input color space or a Raw bypass leaves the encoded
                // R'G'B' untouched here: the shared input-color conversion (or
                // the bypass) is applied exactly once, right after this layout
                // step. The Y'CbCr matrix/range expansion above is mandatory
                // decoding and is never skipped.
                if (rgb == nullptr || (!rgb->ocio() && !rgb->raw())) {
                    const ImageTransfer transfer = rgb != nullptr ? rgb->transfer : toImageTransfer(color.transfer);
                    r = imageTransferToLinear(r, transfer);
                    g = imageTransferToLinear(g, transfer);
                    b = imageTransferToLinear(b, transfer);
                }
            }
            image.setPixel(outputX, outputY, {r, g, b, 1.0F});
        }
    }
    return image;
}

// The clip's decoded color necessities (matrix, range, chroma position, bit
// depth) plus the stream's declared RGB facts. Unspecified decode fields must
// be resolved by `overrides` — the decoder never guesses: an unspecified
// matrix/range (and chroma location on subsampled formats) is an error naming
// what is missing. Overrides only fill missing fields; a stream-tagged field
// keeps its declared value.
struct DecodeColor {
    MediaColorMetadata color;
    bool transferKnown{false};
    ImageTransfer transfer{ImageTransfer::Linear};
    bool primariesKnown{false};
    ImagePrimaries primaries{ImagePrimaries::Rec709};
};

// `allowUnspecifiedRgb` is true when the project's input-color context decides
// the RGB interpretation (a named OCIO space, the declared metadata transfer,
// a Raw bypass, or the config's file rule); it is false for the source-scoped
// path, which keeps the previous strict "an unspecified transfer is an error"
// contract.
[[nodiscard]] DecodeColor resolveDecodeColor(const AVCodecParameters* parameters, const std::string& clip,
                                             const std::string& workingSpace, const ColorOverride& overrides,
                                             const bool allowUnspecifiedRgb) {
    if (!allowUnspecifiedRgb && workingSpace != "linear") {
        fail(clip, workingSpace,
             "working space '" + workingSpace +
                 "' is outside the decoder's explicit supported subset; only the declared default 'linear' "
                 "(scene-linear Rec.709) is supported for source interpretation");
    }
    const FormatSpec* spec = requireFormatSpec(static_cast<AVPixelFormat>(parameters->format), clip);

    DecodeColor out;
    MediaColorMetadata& color = out.color;
    color.bitDepth = spec->depth;

    const std::string formatName = pixelFormatName(spec->format);

    // Transfer characteristic.
    switch (parameters->color_trc) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:  // identical published curve to BT.709
        out.transfer = ImageTransfer::Bt709;
        out.transferKnown = true;
        break;
    case AVCOL_TRC_IEC61966_2_1:
        out.transfer = ImageTransfer::Srgb;
        out.transferKnown = true;
        break;
    case AVCOL_TRC_GAMMA22:
        out.transfer = ImageTransfer::Gamma22;
        out.transferKnown = true;
        break;
    case AVCOL_TRC_GAMMA28:
        out.transfer = ImageTransfer::Gamma28;
        out.transferKnown = true;
        break;
    case AVCOL_TRC_LINEAR:
        out.transfer = ImageTransfer::Linear;
        out.transferKnown = true;
        break;
    case AVCOL_TRC_UNSPECIFIED:
        if (overrides.transfer.has_value()) {
            out.transfer = toImageTransfer(*overrides.transfer);
            out.transferKnown = true;
        } else if (!allowUnspecifiedRgb) {
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

    // Primaries: only Rec.709 is representable by the metadata path — the
    // working space is scene-linear Rec.709, so no chromaticity mapping exists
    // there; other primaries must not be passed through unconverted (that would
    // relabel them Rec.709). A named OCIO input color space does the mapping.
    switch (parameters->color_primaries) {
    case AVCOL_PRI_BT709:
        out.primaries = ImagePrimaries::Rec709;
        out.primariesKnown = true;
        break;
    case AVCOL_PRI_UNSPECIFIED:
        if (overrides.primaries.has_value()) {
            out.primaries = ImagePrimaries::Rec709;
            out.primariesKnown = true;
        } else if (!allowUnspecifiedRgb) {
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
    return out;
}

// Resolves the clip's decode necessities AND its encoded-RGB interpretation.
// The RGB half is the shared media resolver's single owner (InputColor.hpp):
// a named OCIO input color space, the declared metadata transfer with the
// fill-only hints, or a Raw bypass. Y'CbCr matrix/range/chroma decoding stays
// here because it is mandatory codec layout work, not an RGB interpretation.
[[nodiscard]] ClipColorResolution resolveClipColor(const AVCodecParameters* parameters, const std::string& clip,
                                                   const ColorPolicy& policy, const ColorOverride& overrides,
                                                   const ClipColorInput& input, const std::string& context) {
    const bool colorResolved = input.cache != nullptr;
    // A project input-color context owns the policy; the legacy ColorPolicy is
    // used only by the source-scoped path that has no context.
    const std::string workingSpace = colorResolved ? input.cache->policy().workingSpace : policy.workingSpace;
    // With a project input-color context the request's merged fill-only hints
    // carry the Y'CbCr decode fields as well (the legacy ColorOverride parameter
    // is the source-scoped path's vocabulary). They are parsed through the
    // single strict owner here, so an untagged stream's matrix/range/chroma is
    // filled exactly once from the same authored map the RGB half uses.
    ColorOverride decodeOverrides = overrides;
    if (colorResolved && !input.choice.hints.empty()) {
        try {
            decodeOverrides = colorOverrideFromInterpretation(input.choice.hints, context);
        } catch (const std::invalid_argument& error) {
            fail(clip, "color", error.what());
        }
    }
    const DecodeColor decoded = resolveDecodeColor(parameters, clip, workingSpace, decodeOverrides, colorResolved);

    ClipColorResolution out;
    out.decode = decoded.color;
    if (colorResolved) {
        EncodedColorFacts facts;
        facts.clip = true;
        facts.transferKnown = decoded.transferKnown;
        facts.transfer = decoded.transfer;
        facts.primariesKnown = decoded.primariesKnown;
        facts.primaries = decoded.primaries;
        const FormatSpec* spec = findFormatSpec(static_cast<AVPixelFormat>(parameters->format));
        const std::string formatName = spec != nullptr ? pixelFormatName(spec->format) : std::string{"color"};
        try {
            out.rgb = input.cache->resolve(input.choice, facts, clip, context);
        } catch (const InputColorException& error) {
            fail(clip, formatName, error.what());
        } catch (const OcioException& error) {
            fail(clip, formatName, error.what());
        }
    } else {
        // Source-scoped path: the declared/file interpretation with no project
        // configuration, exactly as before.
        out.rgb.kind = InputTransformKind::MetadataTransfer;
        out.rgb.workingSpace = workingSpace;
        out.rgb.transfer = decoded.transfer;
        out.rgb.primaries = decoded.primaries;
        out.rgb.origin = (overrides.transfer.has_value() || overrides.primaries.has_value())
                             ? InputTransformOrigin::SourceInterpretation
                             : InputTransformOrigin::FileMetadata;
        out.rgb.alpha = ResolvedAlpha::Straight;  // video carries no alpha
    }
    // Report the interpretation the frames are actually converted by; the
    // decode necessities stay the stream's declared matrix/range/chroma.
    out.decode.transfer = toGpuTransfer(out.rgb.transfer);
    return out;
}

// Resolves one frame's decode necessities and its encoded-RGB interpretation.
// Frame-level color tags override the stream's when present, so a clip whose
// per-frame tags change is caught by the caller's equality check.
[[nodiscard]] ClipColorResolution frameColor(const AVFrame* frame, const AVCodecParameters* stream,
                                             const std::string& clip, const ColorPolicy& policy,
                                             const ColorOverride& overrides, const ClipColorInput& input,
                                             const std::string& context) {
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
    return resolveClipColor(&tags, clip, policy, overrides, input, context);
}

// Compiled mediaConvert SPIR-V for the Vulkan-resident conversion.
[[nodiscard]] std::vector<std::uint32_t> loadConvertSpirv(const std::filesystem::path& file, const std::string& clip) {
    try {
        return gpu::loadSpirv(file);
    } catch (const gpu::GpuException& error) {
        fail(clip, file.string(), error.what());
    }
}

[[nodiscard]] double frameRateOf(const AVStream* stream) {
    const AVRational rate = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    return rate.num != 0 ? static_cast<double>(rate.num) / static_cast<double>(rate.den) : 0.0;
}

struct MemoryReader {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    AVIOContext* io{nullptr};
    std::size_t position{0};

    explicit MemoryReader(std::shared_ptr<const std::vector<std::uint8_t>> data) : bytes(std::move(data)) {}
    ~MemoryReader() { avio_context_free(&io); }

    static int read(void* opaque, unsigned char* buffer, int bufferSize) {
        auto& reader = *static_cast<MemoryReader*>(opaque);
        if (buffer == nullptr || bufferSize <= 0)
            return AVERROR(EINVAL);
        if (reader.position >= reader.bytes->size())
            return AVERROR_EOF;
        const std::size_t available = reader.bytes->size() - reader.position;
        const std::size_t amount = std::min(available, static_cast<std::size_t>(bufferSize));
        std::memcpy(buffer, reader.bytes->data() + reader.position, amount);
        reader.position += amount;
        return static_cast<int>(amount);
    }

    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& reader = *static_cast<MemoryReader*>(opaque);
        if ((whence & AVSEEK_SIZE) != 0)
            return static_cast<int64_t>(reader.bytes->size());
        whence &= ~AVSEEK_FORCE;
        int64_t base = 0;
        if (whence == SEEK_CUR)
            base = static_cast<int64_t>(reader.position);
        else if (whence == SEEK_END)
            base = static_cast<int64_t>(reader.bytes->size());
        else if (whence != SEEK_SET)
            return -1;
        if (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset)
            return -1;
        if (offset < 0 && (offset == std::numeric_limits<int64_t>::min() || base < -offset))
            return -1;
        const int64_t target = base + offset;
        if (target < 0 || static_cast<std::uint64_t>(target) > reader.bytes->size())
            return -1;
        reader.position = static_cast<std::size_t>(target);
        return target;
    }

    void open(const std::string& name) {
        constexpr int bufferSize = 32 * 1024;
        auto* buffer = static_cast<unsigned char*>(av_malloc(bufferSize));
        if (buffer == nullptr)
            failStatus(name, "memory AVIO buffer allocation failed");
        io = avio_alloc_context(buffer, bufferSize, 0, this, &MemoryReader::read, nullptr, &MemoryReader::seek);
        if (io == nullptr) {
            av_free(buffer);
            failStatus(name, "memory AVIO context allocation failed");
        }
    }
};

// Owns container metadata independently of decoder setup. Native execution
// prepares the codec before attaching its Vulkan device and format callback.
struct PreparedDecoder {
    std::unique_ptr<MemoryReader> memory;
    FormatGuard format;
    CodecContextGuard codec;
    int streamIndex = -1;
    AVStream* stream = nullptr;             // Borrowed from format.
    const AVCodec* decoderCodec = nullptr;  // FFmpeg's static registry.

    explicit PreparedDecoder(const std::string& path)
        : PreparedDecoder(path, std::shared_ptr<const std::vector<std::uint8_t>>{}) {}

    PreparedDecoder(const std::string& path, std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                    bool inspectPackets = true)
        : memory(bytes ? std::make_unique<MemoryReader>(std::move(bytes)) : nullptr) {
        int openStatus = 0;
        if (memory) {
            format.context = avformat_alloc_context();
            if (format.context == nullptr)
                failStatus(path, "avformat_alloc_context failed");
            memory->open(path);
            format.context->pb = memory->io;
            format.context->flags |= AVFMT_FLAG_CUSTOM_IO;
            openStatus = avformat_open_input(&format.context, nullptr, nullptr, nullptr);
        } else {
            openStatus = avformat_open_input(&format.context, path.c_str(), nullptr, nullptr);
        }
        if (openStatus < 0)
            failStatus(path, "avformat_open_input failed", openStatus);
        if (inspectPackets && avformat_find_stream_info(format.context, nullptr) < 0)
            failStatus(path, "avformat_find_stream_info failed");
        streamIndex = av_find_best_stream(format.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0)
            failStatus(path, "no video stream");
        stream = format.context->streams[streamIndex];
    }

    void prepareCodec(const std::string& path) {
        decoderCodec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoderCodec == nullptr)
            failStatus(path, "no decoder for codec id " + std::to_string(stream->codecpar->codec_id));
        codec.context = avcodec_alloc_context3(decoderCodec);
        if (codec.context == nullptr)
            failStatus(path, "avcodec_alloc_context3 failed");
        if (avcodec_parameters_to_context(codec.context, stream->codecpar) < 0)
            failStatus(path, "avcodec_parameters_to_context failed");
        codec.context->thread_count = 1;  // Deterministic decode for image assertions.
        codec.context->err_recognition = AV_EF_EXPLODE;
    }

    PreparedDecoder(const PreparedDecoder&) = delete;
    PreparedDecoder& operator=(const PreparedDecoder&) = delete;

    [[nodiscard]] ClipInfo openCodec(const std::string& path) {
        if (codec.context == nullptr)
            prepareCodec(path);
        const int status = avcodec_open2(codec.context, decoderCodec, nullptr);
        if (status < 0) {
            const std::string profile = declaredProfile(stream->codecpar);
            fail(path, profile,
                 std::string("avcodec_open2 failed") + (profile.empty() ? "" : " (unsupported " + profile + ")"),
                 status);
        }
        ClipInfo result = metadata(path);
        result.width = codec.context->width;
        result.height = codec.context->height;
        return result;
    }

    [[nodiscard]] ClipInfo metadata(const std::string& path) const {
        const AVRational aspect = av_guess_sample_aspect_ratio(format.context, stream, nullptr);
        // Owner-approved compatibility policy (#88): unspecified clip PAR
        // keeps the existing square-pixel default; explicit header PAR wins.
        return {path,
                avcodec_get_name(stream->codecpar->codec_id),
                stream->codecpar->width,
                stream->codecpar->height,
                frameRateOf(stream),
                stream->nb_frames > 0 ? stream->nb_frames : -1,
                aspect.num > 0 && aspect.den > 0 ? av_q2d(aspect) : 1.0,
                stream->nb_frames > 0 ? FrameCountQuality::Reliable : FrameCountQuality::Unknown};
    }
};

}  // namespace

ClipInfo inspectClipHeader(const std::string& path) {
    // Stream probing may decode frames internally; descriptions use only the
    // container header and report unavailable geometry rather than doing that.
    return PreparedDecoder(path, {}, false).metadata(path);
}

void uploadNativeImage(gpu::SubmissionQueue& queue, gpu::Allocator& allocator, const gpu::Image& image,
                       const CpuImage& raster, const uint64_t timeout_ns) {
    const std::size_t channels = raster.channelCount();
    const auto width = static_cast<std::uint32_t>(std::max(raster.width(), 0));
    const auto height = static_cast<std::uint32_t>(std::max(raster.height(), 0));
    if (width == 0 || height == 0 || channels == 0) {
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "native image upload: the raster has no samples");
    }
    // Refuse a mismatched target instead of uploading through a wrong stride:
    // the native layout of this raster's channel count is the image's shape, and
    // a caller that allocated a different one would otherwise bind garbage.
    const auto storedCount = static_cast<std::uint32_t>(channels);
    const VkExtent3D extent = image.extent();
    const std::uint64_t nativeHeight = gpu::nativeChannelHeight(height, storedCount);
    if (image.format() != gpu::nativeChannelFormat(storedCount) || image.dimensions() != 2 || extent.width != width ||
        extent.height != nativeHeight) {
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "native image upload: the target image is not the " + std::to_string(storedCount) +
                                    "-channel layout of extent (" + std::to_string(width) + ", " +
                                    std::to_string(nativeHeight) + ")");
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    const std::size_t bytes = pixels * channels * sizeof(float);
    gpu::Buffer staging = allocator.create_buffer(static_cast<VkDeviceSize>(bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                  gpu::MemoryPreference::HostMapped);
    if (storedCount == 4) {
        // Four stored channels ARE the packed texel: the raster's own bytes are
        // already in wire order, so the staging fill is one contiguous copy with
        // no transpose and no per-sample work.
        std::memcpy(staging.mapped(), raster.data(), bytes);
    } else {
        // The scalar-plane layout stacks channels vertically, so the interleaved
        // samples are transposed directly into the one required staging
        // allocation: Vulkan copy regions have a row pitch, not a pixel stride.
        auto* destination = static_cast<float*>(staging.mapped());
        const float* source = raster.data();
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            for (std::size_t channel = 0; channel < channels; ++channel)
                destination[channel * pixels + pixel] = *source++;
    }

    const VkBuffer stagingBuffer = staging.handle();
    const VkImage imageHandle = image.handle();
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyBufferToImage(command, stagingBuffer, imageHandle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        },
        {staging.retain(), image.retain()}, {}, timeout_ns);
    if (!completion) {
        throw gpu::GpuException(gpu::GpuError::SubmissionTimeout, "native image upload submission capacity exhausted");
    }
    if (!queue.wait(*completion, timeout_ns)) {
        throw gpu::GpuException(gpu::GpuError::SubmissionTimeout, "native image upload did not complete");
    }
}

struct ClipDecoder::Impl {
    std::unique_ptr<MemoryReader> memory;
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
    // Decode necessities (matrix/range/chroma/bit depth) plus the reported
    // transfer/primaries, and the resolved encoded-RGB interpretation the
    // frames are converted by (issue #81).
    MediaColorMetadata color;
    ResolvedInputColor rgb;
    ClipColorInput colorInput;
    // Retained OCIO GPU input-transform pass (hardware path only): built once
    // from the resolved input color space, never per frame and never with a
    // host readback.
    std::unique_ptr<gpu::GpuViewingTransform> inputTransform;
    ColorPolicy policy;
    ColorOverride overrides;
    bool opened = false;
    bool viewerReplay = false;
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

std::unique_ptr<ClipDecoder> ClipDecoder::openInternal(gpu::Instance& instance, gpu::Device& device,
                                                       gpu::Allocator& allocator, const std::string& path,
                                                       const std::filesystem::path& convertSpirv,
                                                       const ColorPolicy& policy, const ColorOverride& overrides,
                                                       const ClipColorInput& color, bool viewerReplay,
                                                       std::shared_ptr<const std::vector<std::uint8_t>> memoryBytes) {
    auto decoder = std::unique_ptr<ClipDecoder>(new ClipDecoder());
    auto impl = std::make_unique<Impl>();
    decoder->impl_ = std::move(impl);
    Impl& d = *decoder->impl_;

    PreparedDecoder prepared(path, std::move(memoryBytes));
    prepared.prepareCodec(path);
    auto& codec = prepared.codec;
    AVStream* stream = prepared.stream;

    // Resolve the encoded-RGB interpretation and the decode necessities BEFORE
    // path selection: an interpretation outside the supported set is a hard
    // error for both paths (no silent relabeling as scene-linear).
    d.viewerReplay = viewerReplay;
    d.policy = policy;
    d.overrides = overrides;
    d.colorInput = color;
    {
        const ClipColorResolution resolved =
            resolveClipColor(stream->codecpar, path, policy, overrides, color, "clip '" + path + "'");
        d.color = resolved.decode;
        d.rgb = resolved.rgb;
    }

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

    if (d.hwDevice != nullptr && d.rgb.ocio()) {
        // Retained OCIO GPU input-transform pass for the hardware path: the
        // Y'CbCr matrix/range decode stays in mediaConvert and the RGB
        // transfer/gamut conversion runs IN PLACE over the decoded frame's own
        // packed four-channel image, with no staging copy, no second image and
        // no host readback. Extracted from the decoder's own retained snapshot:
        // no second load of the config path, so the program always matches the
        // identity and the CPU processors of this generation.
        d.inputTransform = std::make_unique<gpu::GpuViewingTransform>(
            device, allocator,
            d.colorInput.cache->snapshot().inputTransformImageGpu(d.colorInput.cache->policy().workingSpace,
                                                                  d.rgb.colorSpace));
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
    codec.context->get_format = [](AVCodecContext* context, const AVPixelFormat* formats) -> AVPixelFormat {
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
                    vkPool->flags = static_cast<AVVkFrameFlags>(AV_VK_FRAME_FLAG_NONE);
                    // The decoder surfaces are only consumed as transfer
                    // sources by MediaInterop. Do not request sampled or
                    // storage usage: FFmpeg derives its default image flags
                    // from those bits and would add ALIAS/MUTABLE/EXTENDED,
                    // which this Vulkan video format explicitly rejects.
                    vkPool->usage = static_cast<VkImageUsageFlagBits>(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                                                                      VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                                                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                    vkPool->img_flags = 0;
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
    d.memory = std::move(prepared.memory);
    d.device = &device;
    d.allocator = &allocator;

    d.queue = std::make_unique<gpu::SubmissionQueue>(device, device.graphics_family());
    d.opened = true;
    return decoder;
}

std::unique_ptr<ClipDecoder> ClipDecoder::open(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                               const std::string& path, const std::filesystem::path& convertSpirv,
                                               const ColorPolicy& policy, const ColorOverride& overrides,
                                               const ClipColorInput& color) {
    return openInternal(instance, device, allocator, path, convertSpirv, policy, overrides, color, false, {});
}

std::unique_ptr<ClipDecoder> ClipDecoder::openViewer(gpu::Instance& instance, gpu::Device& device,
                                                     gpu::Allocator& allocator, const std::string& path,
                                                     const std::filesystem::path& convertSpirv) {
    return openInternal(instance, device, allocator, path, convertSpirv, ColorPolicy{}, ColorOverride{}, {}, true, {});
}
std::unique_ptr<ClipDecoder> ClipDecoder::openViewerMemory(gpu::Instance& instance, gpu::Device& device,
                                                           gpu::Allocator& allocator, const std::string& name,
                                                           std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                                                           const std::filesystem::path& convertSpirv) {
    if (!bytes || bytes->empty())
        failStatus(name, "viewer memory chunk is empty");
    if (bytes->size() > static_cast<std::size_t>(std::numeric_limits<int64_t>::max()))
        failStatus(name, "viewer memory chunk exceeds addressable AVIO size");
    return openInternal(instance, device, allocator, name, convertSpirv, ColorPolicy{}, ColorOverride{}, {}, true,
                        std::move(bytes));
}

const ResolvedInputColor& ClipDecoder::inputColor() const {
    return impl_->rgb;
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
    {
        const ClipColorResolution resolved =
            frameColor(frame.frame, impl.format.context->streams[impl.streamIndex]->codecpar, impl.info.path,
                       impl.policy, impl.overrides, impl.colorInput, "clip '" + impl.info.path + "'");
        impl.color = resolved.decode;
        impl.rgb = resolved.rgb;
    }
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
        foreign.copyBeforeSampling = true;
        // mediaConvert performs the mandatory Y'CbCr range/matrix expansion and
        // the declared-transfer inversion; an OCIO input space or a Raw bypass
        // leaves R'G'B' for the retained GPU input pass below (or for the
        // consumer, for Raw), so the encoded domain is preserved until the one
        // authoritative conversion.
        foreign.sourceLinearization = !impl.viewerReplay && !impl.rgb.ocio() && !impl.rgb.raw();
        foreign.transfer = toGpuTransfer(impl.rgb.transfer);
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

        // The decoded frame is stored in the shared native layout: four
        // channels are one packed RGBA32F texel per logical pixel (issue #98),
        // so the device extent is the frame's own W×H and the decode→contract
        // conversion stays entirely on device.
        auto output = std::make_unique<gpu::Image>(impl.allocator->create_image(
            foreign.width, static_cast<std::uint32_t>(gpu::nativeChannelHeight(foreign.height, kDecodedChannels)), 1,
            gpu::nativeChannelFormat(kDecodedChannels), VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            2));
        const auto completion = impl.interop->submitToRgba32f(foreign, *output, timeout_ns);
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
        if (impl.inputTransform != nullptr) {
            // Retained OCIO pass IN PLACE over the decoded frame's own packed
            // four-channel buffer: one float4 load/store per pixel, no staging
            // copy, no buffer and no second image per frame. No host readback
            // and no per-frame compilation; alpha is stored back unchanged.
            const std::optional<gpu::SubmissionQueue::Completion> converted =
                impl.inputTransform->submitInputTransformInPlace(*output, timeout_ns);
            if (!converted)
                failStatus(impl.info.path, "GPU input transform submission capacity exhausted");
            if (!submissions.wait(*converted, timeout_ns))
                failStatus(impl.info.path, "GPU input transform timed out");
        }
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
    // scene-linear contract, then uploaded to device residency in the shared
    // native layout, exactly as the hardware path produces it — one addressing
    // convention for decoded frames whatever decoded them. Four channels are a
    // contiguous staging copy in the raster's own byte order; the upload is the
    // capability-dependent transfer cost this path carries.
    const CpuImage pixels = convertDecodedFrame(frame.frame, impl.color, impl.info.path,
                                                /*linearize=*/!impl.viewerReplay, 0, 0, nullptr, nullptr,
                                                impl.viewerReplay ? nullptr : &impl.rgb);
    const auto channels = static_cast<std::uint32_t>(pixels.channelCount());
    auto output = std::make_unique<gpu::Image>(impl.allocator->create_image(
        static_cast<std::uint32_t>(frame.frame->width),
        static_cast<std::uint32_t>(gpu::nativeChannelHeight(static_cast<std::uint32_t>(frame.frame->height), channels)),
        1, gpu::nativeChannelFormat(channels),
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        2));
    // Leaves the image in GENERAL, the layout the interop conversion and every
    // decoded-frame consumer bind.
    uploadNativeImage(*impl.queue, *impl.allocator, *output, pixels, timeout_ns);
    return output;
}

std::unique_ptr<gpu::Image> ClipDecoder::nextViewer(uint64_t timeout_ns) {
    if (!impl_->viewerReplay)
        failStatus(impl_->info.path, "viewer next requested from a source decoder");
    return next(timeout_ns);
}

namespace {

// One software decode owner for both collection APIs and incremental
// reference preparation. No second interpretation or packet-pump policy.
// `width`/`height` are exact output dimensions unless `fitBounds` is set, in
// which case they are MAXIMUM preview bounds and the output is fitted to the
// decoded frame's displayed shape (see fitPreview).
class SoftwareReader {
public:
    SoftwareReader(const std::string& path, bool linearize, const ColorPolicy& policy, const ColorOverride& overrides,
                   const ClipColorInput& color, int width = 0, int height = 0, bool fitBounds = false)
        : prepared_(path), info_(prepared_.openCodec(path)), policy_(policy), overrides_(overrides), colorInput_(color),
          linearize_(linearize), width_(width), height_(height), fitBounds_(fitBounds),
          profile_(declaredProfile(prepared_.stream->codecpar)) {
        const ClipColorResolution resolved =
            resolveClipColor(prepared_.stream->codecpar, path, policy_, overrides_, colorInput_, "clip '" + path + "'");
        metadata_ = resolved.decode;
        rgb_ = resolved.rgb;
        if (!packet_.packet || !decoded_.frame)
            failStatus(path, "av_packet_alloc/av_frame_alloc failed");
    }

    [[nodiscard]] const ClipInfo& info() const { return info_; }
    [[nodiscard]] const MediaColorMetadata& metadata() const { return metadata_; }
    // The resolved encoded-RGB interpretation the frames were converted by.
    [[nodiscard]] const ResolvedInputColor& inputColor() const { return rgb_; }
    // Actual decoded pixel format of the last produced frame ("" before the
    // first frame), validated from the frame itself.
    [[nodiscard]] const std::string& pixelFormat() const { return pixelFormat_; }
    // Plane count of that validated format (0 before the first frame).
    [[nodiscard]] int planeCount() const { return planeCount_; }
    // Container facts resolved at open time.
    [[nodiscard]] int streamIndex() const { return prepared_.streamIndex; }
    [[nodiscard]] const std::string& profile() const { return profile_; }
    [[nodiscard]] std::optional<CpuImage> next() {
        const auto& path = info_.path;
        while (true) {
            const int receive = avcodec_receive_frame(prepared_.codec.context, decoded_.frame);
            if (receive == 0) {
                const ClipColorResolution resolved =
                    frameColor(decoded_.frame, prepared_.stream->codecpar, path, policy_, overrides_, colorInput_,
                               "clip '" + path + "'");
                const auto& color = resolved.decode;
                if (!linearize_ &&
                    (color.transfer != gpu::MediaTransfer::Bt709 || color.matrix != gpu::MediaMatrix::Bt709 ||
                     color.range != gpu::MediaYuvRange::Limited || color.bitDepth != 8 ||
                     decoded_.frame->format != AV_PIX_FMT_YUV420P))
                    fail(path, pixelFormatName(static_cast<AVPixelFormat>(decoded_.frame->format)),
                         "unsupported viewer representation; expected tagged limited-range BT.709 8-bit yuv420p");
                if (seenFrame_ && color != metadata_)
                    failStatus(path, "changing color interpretation within a clip is unsupported");
                if (seenFrame_ && resolved.rgb != rgb_)
                    failStatus(path, "changing input color interpretation within a clip is unsupported");
                if (decoded_.frame->width != info_.width || decoded_.frame->height != info_.height)
                    failStatus(path, "changing frame dimensions within a clip is unsupported");
                metadata_ = color;
                rgb_ = resolved.rgb;
                seenFrame_ = true;
                resolveOutputSize();
                const int outWidth = outputResolved_ ? outputWidth_ : width_;
                const int outHeight = outputResolved_ ? outputHeight_ : height_;
                auto image = convertDecodedFrame(decoded_.frame, color, path, linearize_, outWidth, outHeight,
                                                 &pixelFormat_, &planeCount_, linearize_ ? &rgb_ : nullptr);
                if (linearize_ && colorInput_.cache != nullptr) {
                    // Encoded-domain unassociation, then the resolved
                    // transfer/gamut conversion exactly once.
                    try {
                        colorInput_.cache->apply(image, rgb_);
                    } catch (const InputColorException& error) {
                        failStatus(path, "input color: " + std::string(error.what()));
                    } catch (const OcioException& error) {
                        failStatus(path, "input color: " + std::string(error.what()));
                    }
                    image.setColorInterpretation(rgb_.raw() ? ColorInterpretation::Data
                                                            : ColorInterpretation::SceneLinear);
                }
                av_frame_unref(decoded_.frame);
                return image;
            }
            if (receive == AVERROR_EOF)
                return std::nullopt;
            if (receive != AVERROR(EAGAIN))
                failStatus(path, "avcodec_receive_frame failed", receive);
            if (flushed_)
                failStatus(path, "decoder requested input after flush");
            const int readStatus = av_read_frame(prepared_.format.context, packet_.packet);
            if (readStatus == AVERROR_EOF) {
                const int status = avcodec_send_packet(prepared_.codec.context, nullptr);
                if (status < 0 && status != AVERROR_EOF)
                    failStatus(path, "avcodec_send_packet (flush) failed", status);
                flushed_ = true;
                continue;
            }
            if (readStatus < 0)
                failStatus(path, "av_read_frame failed", readStatus);
            if (packet_.packet->stream_index != prepared_.streamIndex) {
                av_packet_unref(packet_.packet);
                continue;
            }
            const int status = avcodec_send_packet(prepared_.codec.context, packet_.packet);
            av_packet_unref(packet_.packet);
            if (status < 0)
                failStatus(path, "avcodec_send_packet failed", status);
        }
    }

private:
    // Bounded preview: fit the DISPLAYED image (decoded pixels widened by
    // SAR) inside the maximum bounds, emitting a square-pixel raster whose
    // dimensions carry the displayed shape. Identical to the still preview
    // fit in MediaImportService; both are leaf formulas for their own decode
    // path rather than a shared abstraction.
    static void fitPreview(const int sourceWidth, const int sourceHeight, const double pixelAspect, const int maxWidth,
                           const int maxHeight, int& outWidth, int& outHeight) {
        const double aspect = (static_cast<double>(std::max(sourceWidth, 1)) * std::max(pixelAspect, 1e-6)) /
                              static_cast<double>(std::max(sourceHeight, 1));
        const auto rounded = [](const double value) {
            return static_cast<int>(std::llround(std::clamp(value, 0.0, 1.0e9)));
        };
        int width = maxWidth;
        int height = rounded(static_cast<double>(maxWidth) / aspect);
        if (height > maxHeight) {
            height = maxHeight;
            width = rounded(static_cast<double>(maxHeight) * aspect);
        }
        outWidth = std::clamp(width, 1, maxWidth);
        outHeight = std::clamp(height, 1, maxHeight);
    }

    // Resolve the fitted output size once, from the actual decoded frame's
    // width/height/SAR; a clip's dimensions are validated constant for the
    // whole stream.
    void resolveOutputSize() {
        if (outputResolved_ || !fitBounds_ || width_ <= 0 || height_ <= 0 || decoded_.frame == nullptr ||
            decoded_.frame->width <= 0 || decoded_.frame->height <= 0) {
            return;
        }
        double pixelAspect = info_.pixelAspect;
        const AVRational sar = decoded_.frame->sample_aspect_ratio;
        if (sar.num > 0 && sar.den > 0) {
            pixelAspect = av_q2d(sar);
        }
        fitPreview(decoded_.frame->width, decoded_.frame->height, pixelAspect, width_, height_, outputWidth_,
                   outputHeight_);
        outputResolved_ = true;
    }

    PreparedDecoder prepared_;
    ClipInfo info_;
    ColorPolicy policy_;
    ColorOverride overrides_;
    ClipColorInput colorInput_;
    MediaColorMetadata metadata_;
    ResolvedInputColor rgb_;
    PacketGuard packet_;
    FrameGuard decoded_;
    bool linearize_;
    int width_;
    int height_;
    bool fitBounds_{false};
    bool outputResolved_{false};
    int outputWidth_{0};
    int outputHeight_{0};
    std::string pixelFormat_;
    int planeCount_{0};
    std::string profile_;
    bool seenFrame_ = false;
    bool flushed_ = false;
};

[[nodiscard]] SoftwareClip decodeSoftware(const std::string& path, int64_t maxFrames, bool linearize,
                                          const ColorPolicy& policy, const ColorOverride& overrides,
                                          const ClipColorInput& color) {
    SoftwareReader reader(path, linearize, policy, overrides, color);
    SoftwareClip result;
    result.info = reader.info();
    result.metadata = reader.metadata();
    result.inputColor = reader.inputColor();
    result.streamIndex = reader.streamIndex();
    result.profile = reader.profile();
    while (maxFrames < 0 || static_cast<int64_t>(result.frames.size()) < maxFrames) {
        auto frame = reader.next();
        if (!frame)
            break;
        result.metadata = reader.metadata();
        result.pixelFormat = reader.pixelFormat();
        result.planeCount = reader.planeCount();
        result.frames.push_back(std::move(*frame));
    }
    return result;
}

// Bounded single-frame read: discards frames before `frameIndex`, retains
// only the requested frame, and never holds more than one decoded frame.
// With `fitBounds`, `width`/`height` are maximum preview bounds.
[[nodiscard]] SoftwareClip decodeSoftwareFrame(const std::string& path, int64_t frameIndex, bool linearize,
                                               const ColorPolicy& policy, const ColorOverride& overrides, int width,
                                               int height, bool fitBounds, const ClipColorInput& color) {
    if (frameIndex < 0)
        failStatus(path, "frame index must be >= 0");
    SoftwareReader reader(path, linearize, policy, overrides, color, width, height, fitBounds);
    SoftwareClip result;
    result.info = reader.info();
    result.metadata = reader.metadata();
    result.inputColor = reader.inputColor();
    result.streamIndex = reader.streamIndex();
    result.profile = reader.profile();
    for (int64_t index = 0; index <= frameIndex; ++index) {
        auto frame = reader.next();
        if (!frame)
            return result;  // stream ended before the requested frame
        result.metadata = reader.metadata();
        result.pixelFormat = reader.pixelFormat();
        result.planeCount = reader.planeCount();
        if (index == frameIndex)
            result.frames.push_back(std::move(*frame));
    }
    return result;
}
}  // namespace

struct ViewerReferenceDecoder::Impl {
    SoftwareReader reader;
    Impl(const std::string& path, int width, int height) : reader(path, false, {}, {}, {}, width, height) {}
};

ViewerReferenceDecoder::ViewerReferenceDecoder(const std::string& path, int width, int height) {
    if (!((width == 0 && height == 0) || (width > 0 && height > 0 && width <= 8192 && height <= 8192)))
        failStatus(path, "reference dimensions must both be zero (native) or in 1..8192");
    impl_ = std::make_unique<Impl>(path, width, height);
}
ViewerReferenceDecoder::~ViewerReferenceDecoder() = default;
const ClipInfo& ViewerReferenceDecoder::info() const {
    return impl_->reader.info();
}
std::optional<CpuImage> ViewerReferenceDecoder::next() {
    return impl_->reader.next();
}

SoftwareClip decodeClipSoftware(const std::string& path, int64_t maxFrames, const ColorPolicy& policy,
                                const ColorOverride& overrides, const ClipColorInput& color) {
    return decodeSoftware(path, maxFrames, /*linearize=*/true, policy, overrides, color);
}

SoftwareClip decodeClipFrameSoftware(const std::string& path, int64_t frameIndex, int width, int height,
                                     const ColorPolicy& policy, const ColorOverride& overrides,
                                     const ClipColorInput& color) {
    if (!((width == 0 && height == 0) || (width > 0 && height > 0)))
        failStatus(path, "bounded frame dimensions must both be zero (native) or both positive");
    return decodeSoftwareFrame(path, frameIndex, /*linearize=*/true, policy, overrides, width, height,
                               /*fitBounds=*/true, color);
}

ColorOverride colorOverrideFromInterpretation(const std::map<std::string, std::string>& interpretation,
                                              const std::string& context) {
    // The error text names the field, the value, and the supported set; the
    // runtime source session rethrows it as its own exception type without
    // rewriting the message.
    const auto parseField = [&context](const std::string& value, const auto& byName, const std::string& field,
                                       const std::string& supported) {
        const auto it = byName.find(value);
        if (it == byName.end()) {
            throw std::invalid_argument("source interpretation field '" + field + "' has unsupported value '" + value +
                                        "' (context: " + context + "; supported: " + supported + ")");
        }
        return it->second;
    };
    ColorOverride overrides;
    for (const auto& [field, value] : interpretation) {
        if (field == "transfer") {
            static const std::map<std::string, gpu::MediaTransfer> byName{{"bt709", gpu::MediaTransfer::Bt709},
                                                                          {"srgb", gpu::MediaTransfer::Srgb},
                                                                          {"gamma22", gpu::MediaTransfer::Gamma22},
                                                                          {"gamma28", gpu::MediaTransfer::Gamma28},
                                                                          {"linear", gpu::MediaTransfer::Linear}};
            overrides.transfer = parseField(value, byName, field, "bt709, srgb, gamma22, gamma28, linear");
        } else if (field == "primaries") {
            static const std::map<std::string, gpu::MediaPrimaries> byName{{"bt709", gpu::MediaPrimaries::Bt709}};
            overrides.primaries = parseField(value, byName, field, "bt709");
        } else if (field == "matrix") {
            static const std::map<std::string, gpu::MediaMatrix> byName{{"bt709", gpu::MediaMatrix::Bt709},
                                                                        {"bt601", gpu::MediaMatrix::Bt601}};
            overrides.matrix = parseField(value, byName, field, "bt709, bt601");
        } else if (field == "range") {
            static const std::map<std::string, gpu::MediaYuvRange> byName{{"limited", gpu::MediaYuvRange::Limited},
                                                                          {"full", gpu::MediaYuvRange::Full}};
            overrides.range = parseField(value, byName, field, "limited, full");
        } else if (field == "chromaLocation") {
            static const std::map<std::string, gpu::MediaChromaLocation> byName{
                {"left", gpu::MediaChromaLocation::Left}};
            overrides.chromaLocation = parseField(value, byName, field, "left");
        } else {
            throw std::invalid_argument("source interpretation has unknown field '" + field + "' (context: " + context +
                                        "; known fields: transfer, primaries, matrix, range, chromaLocation)");
        }
    }
    return overrides;
}

SoftwareClip decodeViewerChunkSoftware(const std::string& path, int64_t maxFrames) {
    return decodeSoftware(path, maxFrames, /*linearize=*/false, ColorPolicy{}, ColorOverride{}, {});
}

}  // namespace nemo::media
