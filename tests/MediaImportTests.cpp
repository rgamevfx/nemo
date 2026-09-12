// Async media import/probe/preview tests (issue #43).
//
// These cover the uncertain seams: the bounded single-frame clip read, the
// source-revision/request identity that lets a consumer reject stale work,
// coalescing/cancellation under a bounded outstanding set, offline and
// malformed-source diagnostics, and the display-referred preview contract.
// Decode/format/color validation itself is owned by the media adapters and
// already covered by MediaTests/MediaDecodeTests; this file asserts only
// what the import service adds on top.

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/MediaImportService.hpp"

using namespace nemo;
using namespace nemo::media;

namespace {

std::filesystem::path importDir() {
    const auto dir = std::filesystem::temp_directory_path() / "nemo-media-import-tests";
    std::filesystem::create_directories(dir);
    return dir;
}

// Minimal OCIO v2 config: a linear working space and one matrix-only display
// view. The transform math is covered by ColorTests; here it only has to be
// a real, resolvable viewing transform.
std::filesystem::path writeColorConfig() {
    const auto path = importDir() / "import-color.ocio";
    std::string config;
    config += "ocio_profile_version: 2\n";
    config += "search_path: \"\"\n";
    config += "roles:\n  default: linear\n  scene_linear: linear\n";
    config += "colorspaces:\n";
    config += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
    config += "  - !<ColorSpace>\n    name: display_view\n";
    config +=
        "    from_reference: !<MatrixTransform> {matrix: [1.1, 0.0, 0.0, 0.0, 0.0, 0.95, 0.0, 0.0, 0.0, 0.0, 1.05, "
        "0.0, 0.0, 0.0, 0.0, 1.0]}\n";
    config += "displays:\n  sRGB:\n    - !<View> {name: rec709, colorspace: display_view}\n";
    std::ofstream file(path);
    file << config;
    return path;
}

// An 8x4 PNG whose declared sRGB color space makes the interpretation
// explicit (the same fixture shape MediaTests uses).
std::filesystem::path writeTaggedPng() {
    const auto path = importDir() / "tagged.png";
    auto output = OIIO::ImageOutput::create(path.string());
    if (!output) {
        throw std::runtime_error("PNG output unavailable");
    }
    OIIO::ImageSpec spec(8, 4, 3, OIIO::TypeDesc::UINT8);
    spec.channelnames = {"R", "G", "B"};
    if (!output->open(path.string(), spec)) {
        throw std::runtime_error("PNG open failed");
    }
    std::vector<unsigned char> pixels(8 * 4 * 3);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<unsigned char>(40 + (i * 7) % 180);
    }
    if (!output->write_image(OIIO::TypeDesc::UINT8, pixels.data()) || !output->close()) {
        throw std::runtime_error("PNG write failed");
    }
    return path;
}

std::filesystem::path writeSequenceFrame(const std::filesystem::path& pattern, const std::int64_t frame,
                                         const int width = 4, const int height = 2) {
    CpuImage image(width, height);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixel(x, y, {0.25F + 0.05F * static_cast<float>(frame), 0.5F, 0.75F, 1.0F});
        }
    }
    const std::string path = resolveFramePath(pattern.string(), frame);
    writeImage(path, image, OutputPrecision::Half);
    return path;
}

// One clip with the authored color tags the import probe must read. FFV1
// keeps the authored integer samples, so expectations come from the tagged
// stream, not from an encoder.
class ClipFixture {
public:
    ClipFixture(const std::string& tag, const AVColorTransferCharacteristic transfer) {
        path = importDir() / ("import-" + tag + ".mkv");
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        if (!encoder) {
            throw std::runtime_error("FFV1 encoder unavailable");
        }
        codec.reset(avcodec_alloc_context3(encoder));
        if (!codec) {
            throw std::bad_alloc();
        }
        codec->width = 64;
        codec->height = 48;
        codec->pix_fmt = AV_PIX_FMT_YUV420P;
        codec->time_base = {1, 24};
        codec->color_primaries = AVCOL_PRI_BT709;
        codec->color_trc = transfer;
        codec->colorspace = AVCOL_SPC_BT709;
        codec->color_range = AVCOL_RANGE_MPEG;
        codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
        check(avcodec_open2(codec.get(), encoder, nullptr));
        check(avformat_alloc_output_context2(&output.context, nullptr, "matroska", path.string().c_str()));
        AVFormatContext* format = output.context;
        if (!format) {
            throw std::bad_alloc();
        }
        AVStream* stream = avformat_new_stream(format, nullptr);
        if (!stream) {
            throw std::bad_alloc();
        }
        check(avcodec_parameters_from_context(stream->codecpar, codec.get()));
        stream->time_base = codec->time_base;
        stream->avg_frame_rate = {24, 1};
        check(avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE));
        check(avformat_write_header(format, nullptr));
        frame.reset(av_frame_alloc());
        packet.reset(av_packet_alloc());
        if (!frame || !packet) {
            throw std::bad_alloc();
        }
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = 64;
        frame->height = 48;
        check(av_frame_get_buffer(frame.get(), 0));
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(AV_PIX_FMT_YUV420P);
        const int values[] = {126, 128, 128};
        for (int plane = 0; plane < descriptor->nb_components; ++plane) {
            const int rows = plane == 0 ? 48 : AV_CEIL_RSHIFT(48, descriptor->log2_chroma_h);
            const int cols = plane == 0 ? 64 : AV_CEIL_RSHIFT(64, descriptor->log2_chroma_w);
            for (int row = 0; row < rows; ++row) {
                auto* dest = frame->data[plane] + row * frame->linesize[plane];
                for (int col = 0; col < cols; ++col) {
                    dest[col] = static_cast<unsigned char>(values[plane]);
                }
            }
        }
        frame->pts = 0;
        check(avcodec_send_frame(codec.get(), frame.get()));
        check(avcodec_send_frame(codec.get(), nullptr));
        int status = 0;
        while ((status = avcodec_receive_packet(codec.get(), packet.get())) == 0) {
            av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(format, packet.get()));
        }
        if (status != AVERROR_EOF) {
            check(status);
        }
        check(av_write_trailer(format));
        check(avio_closep(&format->pb));
    }

    ~ClipFixture() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::filesystem::path path;

private:
    static void check(const int status) {
        if (status < 0) {
            throw std::runtime_error("FFV1 fixture failure: " + std::to_string(status));
        }
    }

    struct CodecDelete {
        void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
    };
    struct FrameDelete {
        void operator()(AVFrame* value) const { av_frame_free(&value); }
    };
    struct PacketDelete {
        void operator()(AVPacket* value) const { av_packet_free(&value); }
    };
    struct Output {
        AVFormatContext* context = nullptr;
        ~Output() {
            if (context) {
                if (context->pb) {
                    avio_closep(&context->pb);
                }
                avformat_free_context(context);
            }
        }
    } output;
    std::unique_ptr<AVCodecContext, CodecDelete> codec;
    std::unique_ptr<AVFrame, FrameDelete> frame;
    std::unique_ptr<AVPacket, PacketDelete> packet;
};

MediaImportRequest makeRequest(const std::string& sourceKey, const std::string& path, const std::uint64_t requestId,
                               const int width = 16, const int height = 12, const std::int64_t frame = 0) {
    MediaImportRequest request;
    request.requestId = requestId;
    request.sourceKey = sourceKey;
    request.reference.path = path;
    request.colorConfig = writeColorConfig().string();
    request.thumbnailWidth = width;
    request.thumbnailHeight = height;
    request.frame = frame;
    return request;
}

std::optional<MediaImportResult> waitForResult(MediaImportService& service, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = service.takeResult()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

}  // namespace

// Acceptance example 1 + 2: a real still import reports validated probe
// facts and a bounded display-referred preview, with the software path
// declared rather than a hardware claim.
TEST(MediaImport, StillImportProducesProbeAndDisplayReferredThumbnail) {
    const auto png = writeTaggedPng();
    const MediaImportResult result = inspectMediaSource(makeRequest("plate", png.string(), 41));

    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_FALSE(result.offline);
    EXPECT_EQ(result.request.requestId, 41u);
    EXPECT_EQ(result.request.sourceKey, "plate");
    EXPECT_EQ(result.kind, MediaKind::Image);
    EXPECT_EQ(result.probe.status, MediaProbeStatus::Ready);
    EXPECT_EQ(result.probe.width, 8);
    EXPECT_EQ(result.probe.height, 4);
    EXPECT_EQ(result.probe.duration, 1);
    EXPECT_EQ(result.probe.codec, "png");
    EXPECT_EQ(result.probe.colorTransfer, "srgb");
    EXPECT_EQ(result.probe.provenance, "oiio");
    EXPECT_EQ(result.pixelFormat, "uint8");
    EXPECT_EQ(result.bitDepth, 8);
    EXPECT_EQ(result.streamIndex, 0);  // the image adapter's single RGBA plane
    EXPECT_EQ(result.planeCount, 1);
    EXPECT_TRUE(result.profile.empty());
    EXPECT_DOUBLE_EQ(result.pixelAspect, 1.0);
    EXPECT_FALSE(result.hardware);
    EXPECT_FALSE(result.fallbackReason.empty());

    ASSERT_NE(result.thumbnail, nullptr);
    // Maximum bounds are 16x12 and the 8x4 source is 2:1, so the preview
    // keeps its shape instead of being forced to the bounds.
    EXPECT_EQ(result.thumbnail->width(), 16);
    EXPECT_EQ(result.thumbnail->height(), 8);
    EXPECT_EQ(result.thumbnail->layout().color, ColorInterpretation::DisplayReferred);
    EXPECT_FLOAT_EQ(result.thumbnail->layout().pixelAspect, 1.0F);
    // Real decoded pixels went through the viewing transform: the tagged
    // PNG is not black, and the display matrix is diagonal-positive.
    const auto sample = result.thumbnail->pixel(0, 0);
    EXPECT_TRUE(std::isfinite(sample[0]));
    EXPECT_GT(sample[0], 0.0F);
}

// Sequence references resolve the requested pattern frame, report Sequence,
// and still produce a real preview from that frame.
TEST(MediaImport, SequenceImportResolvesPatternFrame) {
    const auto pattern = importDir() / "shot.####.exr";
    writeSequenceFrame(pattern, 0);
    writeSequenceFrame(pattern, 3);

    const MediaImportResult result = inspectMediaSource(makeRequest("shot", pattern.string(), 7, 4, 2, 3));

    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.kind, MediaKind::Sequence);
    EXPECT_EQ(result.probe.status, MediaProbeStatus::Ready);
    EXPECT_EQ(result.probe.width, 4);
    EXPECT_EQ(result.probe.height, 2);
    EXPECT_EQ(result.probe.codec, "openexr");
    EXPECT_EQ(result.probe.colorTransfer, "linear");
    EXPECT_EQ(result.pixelFormat, "half");
    EXPECT_EQ(result.bitDepth, 16);
    ASSERT_NE(result.thumbnail, nullptr);
    EXPECT_EQ(result.thumbnail->width(), 4);
    EXPECT_EQ(result.thumbnail->height(), 2);
    EXPECT_EQ(result.thumbnail->layout().color, ColorInterpretation::DisplayReferred);

    // The requested frame is the one decoded: a later frame path that is
    // absent is reported against the authored pattern, not silently
    // substituted with frame 0.
    const MediaImportResult missing = inspectMediaSource(makeRequest("shot", pattern.string(), 8, 4, 2, 9));
    EXPECT_TRUE(missing.offline);
    EXPECT_NE(missing.error.find(resolveFramePath(pattern.string(), 9)), std::string::npos) << missing.error;
    EXPECT_NE(missing.error.find(pattern.string()), std::string::npos) << missing.error;
    EXPECT_EQ(missing.probe.status, MediaProbeStatus::Failed);
    EXPECT_EQ(missing.thumbnail, nullptr);
}

// `frame` is a source-local time: offset/step map it to the file frame, and
// the probe reports THAT frame's actual header (differing dimensions here),
// not the first frame's.
TEST(MediaImport, SourceTimeMappingDrivesSequenceFrameAndProbe) {
    const auto pattern = importDir() / "mapped.####.exr";
    writeSequenceFrame(pattern, 1001, 4, 2);
    writeSequenceFrame(pattern, 1003, 6, 3);

    SourceReference reference;
    reference.path = pattern.string();
    reference.frameOffset = 1001;
    reference.frameStep = 2;

    // Local time 1 -> source frame 1003.
    MediaImportRequest request = makeRequest("mapped", pattern.string(), 61, 8, 8, 1);
    request.reference = reference;
    const MediaImportResult result = inspectMediaSource(request);
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.probe.width, 6);
    EXPECT_EQ(result.probe.height, 3);
    ASSERT_NE(result.thumbnail, nullptr);
    EXPECT_EQ(result.thumbnail->width(), 8);  // bounds 8x8, 6:3 = 2:1
    EXPECT_EQ(result.thumbnail->height(), 4);

    // Local time 0 -> source frame 1001 (a literal frame 0 does not exist).
    MediaImportRequest head = makeRequest("mapped", pattern.string(), 62, 8, 8, 0);
    head.reference = reference;
    const MediaImportResult first = inspectMediaSource(head);
    EXPECT_TRUE(first.error.empty()) << first.error;
    EXPECT_EQ(first.probe.width, 4);
    EXPECT_EQ(first.probe.height, 2);

    // A negative local time is legal when the offset allows it: it maps to
    // source frame 999 and is reported as a missing path, not rejected as a
    // bad index.
    MediaImportRequest negative = makeRequest("mapped", pattern.string(), 63, 8, 8, -1);
    negative.reference = reference;
    const MediaImportResult missing = inspectMediaSource(negative);
    EXPECT_TRUE(missing.offline);
    EXPECT_NE(missing.error.find(resolveFramePath(pattern.string(), 999)), std::string::npos) << missing.error;
}

// thumbnailWidth/Height are maxima: an anamorphic source keeps its DISPLAYED
// aspect in a square-pixel preview, so a consumer that only stretches the
// raster cannot distort it.
TEST(MediaImport, AnamorphicPreviewKeepsDisplayedAspect) {
    const auto path = importDir() / "anamorphic.exr";
    ImageLayout layout;
    layout.width = 8;
    layout.height = 4;
    layout.pixelAspect = 2.0F;
    CpuImage image(layout);
    for (int y = 0; y < layout.height; ++y) {
        for (int x = 0; x < layout.width; ++x) {
            image.setPixel(x, y, {0.5F, 0.5F, 0.5F, 1.0F});
        }
    }
    writeImage(path.string(), image, OutputPrecision::Half);

    const MediaImportResult result = inspectMediaSource(makeRequest("anamorphic", path.string(), 71, 16, 12));
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_DOUBLE_EQ(result.pixelAspect, 2.0);  // source evidence is preserved
    ASSERT_NE(result.thumbnail, nullptr);
    // Displayed aspect is (8*2):4 = 4:1, fitted inside 16x12.
    EXPECT_EQ(result.thumbnail->width(), 16);
    EXPECT_EQ(result.thumbnail->height(), 4);
    EXPECT_FLOAT_EQ(result.thumbnail->layout().pixelAspect, 1.0F);  // square-pixel preview
}

// A missing file is offline data, not an exception: the diagnostic names the
// path and no preview is fabricated.
TEST(MediaImport, MissingSourceIsOfflineWithPathDiagnostic) {
    const auto path = importDir() / "does-not-exist.png";
    const MediaImportResult result = inspectMediaSource(makeRequest("gone", path.string(), 3));

    EXPECT_TRUE(result.offline);
    EXPECT_FALSE(result.error.empty());
    EXPECT_NE(result.error.find(path.string()), std::string::npos) << result.error;
    EXPECT_EQ(result.probe.status, MediaProbeStatus::Failed);
    EXPECT_EQ(result.kind, MediaKind::Unknown);
    EXPECT_EQ(result.thumbnail, nullptr);
}

// Ambiguous declared color metadata is rejected by the image adapter before
// pixels are pulled in; the import service surfaces that diagnostic and
// produces no thumbnail.
TEST(MediaImport, AmbiguousColorIsRejectedWithoutThumbnail) {
    const auto path = importDir() / "untagged.tif";
    CpuImage image(2, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            image.setPixel(x, y, {0.5F, 0.25F, 0.125F, 1.0F});
        }
    }
    writeImage(path.string(), image, OutputPrecision::Float32);

    const MediaImportResult result = inspectMediaSource(makeRequest("ambiguous", path.string(), 5));

    EXPECT_FALSE(result.offline);
    EXPECT_FALSE(result.error.empty());
    EXPECT_NE(result.error.find(path.string()), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("ambiguous"), std::string::npos) << result.error;
    EXPECT_EQ(result.probe.status, MediaProbeStatus::Failed);
    EXPECT_EQ(result.thumbnail, nullptr);
}

// The clip probe reports evidence measured from the actual decoded frame
// (format, bit depth, range, siting) and a bounded display-referred preview.
TEST(MediaImport, ClipImportReportsActualFrameEvidence) {
    const ClipFixture clip("tagged", AVCOL_TRC_BT709);
    const MediaImportRequest request = makeRequest("clip", clip.path.string(), 11, 16, 12);

    MediaImportService service(4);
    ASSERT_TRUE(service.submit(request));
    const auto result = waitForResult(service, std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error.empty()) << result->error;
    EXPECT_FALSE(result->offline);
    EXPECT_EQ(result->request.requestId, 11u);
    EXPECT_EQ(result->kind, MediaKind::Video);
    EXPECT_EQ(result->probe.status, MediaProbeStatus::Ready);
    EXPECT_EQ(result->probe.width, 64);
    EXPECT_EQ(result->probe.height, 48);
    EXPECT_EQ(result->probe.codec, "ffv1");
    EXPECT_EQ(result->probe.colorTransfer, "bt709");
    EXPECT_EQ(result->probe.colorMatrix, "bt709");
    EXPECT_DOUBLE_EQ(result->frameRate, 24.0);
    EXPECT_EQ(result->pixelFormat, "yuv420p");
    EXPECT_EQ(result->bitDepth, 8);
    EXPECT_EQ(result->streamIndex, 0);
    EXPECT_EQ(result->planeCount, 3);  // planar 4:2:0
    EXPECT_EQ(result->colorRange, "limited");
    EXPECT_EQ(result->chromaLocation, "left");
    EXPECT_FALSE(result->hardware);
    ASSERT_NE(result->thumbnail, nullptr);
    EXPECT_EQ(result->thumbnail->width(), 16);
    EXPECT_EQ(result->thumbnail->height(), 12);
    EXPECT_EQ(result->thumbnail->layout().color, ColorInterpretation::DisplayReferred);
    // The tagged mid-gray clip decodes to real non-black display pixels.
    const auto sample = result->thumbnail->pixel(0, 0);
    EXPECT_TRUE(std::isfinite(sample[0]));
    EXPECT_GT(sample[0], 0.0F);

    // Bounds are maxima: a square request on a 4:3 clip yields a 4:3
    // square-pixel preview, fitted by the decode conversion owner.
    const MediaImportResult fitted = inspectMediaSource(makeRequest("clip", clip.path.string(), 13, 16, 16));
    EXPECT_TRUE(fitted.error.empty()) << fitted.error;
    EXPECT_EQ(fitted.probe.width, 64);  // probe metadata stays native
    EXPECT_EQ(fitted.probe.height, 48);
    ASSERT_NE(fitted.thumbnail, nullptr);
    EXPECT_EQ(fitted.thumbnail->width(), 16);
    EXPECT_EQ(fitted.thumbnail->height(), 12);
    EXPECT_FLOAT_EQ(fitted.thumbnail->layout().pixelAspect, 1.0F);

    // Requesting a frame past the single-frame clip reports the missing
    // source frame instead of decoding a substitute.
    const MediaImportRequest beyond = makeRequest("clip", clip.path.string(), 12, 16, 12, 5);
    const MediaImportResult missing = inspectMediaSource(beyond);
    EXPECT_EQ(missing.probe.status, MediaProbeStatus::Failed);
    EXPECT_NE(missing.error.find("no frame at source frame 5"), std::string::npos) << missing.error;
    EXPECT_EQ(missing.thumbnail, nullptr);
}

// An untagged stream still needs an explicit interpretation; when the
// reference supplies one, import honors it (the shared media vocabulary),
// and an unknown field is rejected naming the field.
TEST(MediaImport, InterpretationOverrideResolvesMissingStreamTransfer) {
    const ClipFixture clip("untagged", AVCOL_TRC_UNSPECIFIED);

    const MediaImportResult rejected = inspectMediaSource(makeRequest("clip", clip.path.string(), 21, 16, 12));
    EXPECT_EQ(rejected.probe.status, MediaProbeStatus::Failed);
    EXPECT_NE(rejected.error.find("transfer"), std::string::npos) << rejected.error;

    MediaImportRequest overridden = makeRequest("clip", clip.path.string(), 22, 16, 12);
    overridden.reference.interpretation = {{"transfer", "bt709"}};
    const MediaImportResult accepted = inspectMediaSource(overridden);
    EXPECT_TRUE(accepted.error.empty()) << accepted.error;
    EXPECT_EQ(accepted.probe.status, MediaProbeStatus::Ready);
    EXPECT_EQ(accepted.probe.colorTransfer, "bt709");
    ASSERT_NE(accepted.thumbnail, nullptr);

    MediaImportRequest unknown = makeRequest("clip", clip.path.string(), 23, 16, 12);
    unknown.reference.interpretation = {{"colorimetry", "bt709"}};
    const MediaImportResult badField = inspectMediaSource(unknown);
    EXPECT_EQ(badField.probe.status, MediaProbeStatus::Failed);
    EXPECT_NE(badField.error.find("unknown field 'colorimetry'"), std::string::npos) << badField.error;
}

// Newest request per source wins: a superseded queued request and any
// unconsumed result for that source are dropped, so a stale probe can never
// be applied to a newer reference.
TEST(MediaImport, ServiceCoalescesSupersededRequestsBySourceKey) {
    const auto png = writeTaggedPng();
    MediaImportService service(4);
    ASSERT_TRUE(service.submit(makeRequest("plate", png.string(), 100)));
    ASSERT_TRUE(service.submit(makeRequest("plate", png.string(), 101)));

    const auto result = waitForResult(service, std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->request.requestId, 101u);
    EXPECT_EQ(result->request.sourceKey, "plate");
    EXPECT_FALSE(service.takeResult().has_value());  // the superseded result is gone
}

// Cancelling a source drops queued work and an unconsumed result, while an
// unrelated source still completes.
TEST(MediaImport, ServiceCancelDropsSupersededSourceWork) {
    const auto png = writeTaggedPng();
    MediaImportService service(4);
    ASSERT_TRUE(service.submit(makeRequest("cancelled", png.string(), 200)));
    ASSERT_TRUE(service.submit(makeRequest("kept", png.string(), 201)));
    service.cancel("cancelled");

    const auto result = waitForResult(service, std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->request.sourceKey, "kept");
    EXPECT_FALSE(service.takeResult().has_value());
}

// Outstanding work is bounded and released by collecting the result, so a
// caller sees backpressure instead of an unbounded queue.
TEST(MediaImport, ServiceBoundsOutstandingAndReleasesOnCollect) {
    const auto missingA = importDir() / "absent-a.png";
    const auto missingB = importDir() / "absent-b.png";
    MediaImportService service(1);
    ASSERT_TRUE(service.submit(makeRequest("a", missingA.string(), 300)));
    EXPECT_FALSE(service.submit(makeRequest("b", missingB.string(), 301)));  // at the bound

    const auto result = waitForResult(service, std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->request.sourceKey, "a");
    EXPECT_TRUE(result->offline);

    EXPECT_TRUE(service.submit(makeRequest("b", missingB.string(), 302)));  // slot released
    const auto second = waitForResult(service, std::chrono::seconds(30));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->request.requestId, 302u);
}
