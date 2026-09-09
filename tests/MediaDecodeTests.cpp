#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "nemo/core/document/Document.hpp"
#include "nemo/media/CodecSweep.hpp"
#include "nemo/media/VideoDecode.hpp"

namespace {

// FFV1 preserves the authored integer samples; expectations below come from
// BT.709's published inverse OETF, not Nemo's encoder or conversion helpers.
class TaggedClip {
public:
    explicit TaggedClip(AVPixelFormat pixels, int y = 126, int cb = 128, int cr = 128,
                        AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709, int lowBits = 0, bool split = false) {
        path = std::filesystem::temp_directory_path() /
               (std::string("nemo-source-") + ::testing::UnitTest::GetInstance()->current_test_info()->name() + "-" +
                av_get_pix_fmt_name(pixels) + "-" + std::to_string(y) + "-" + std::to_string(cb) + "-" +
                std::to_string(transfer) + ".mkv");
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        if (!encoder)
            throw std::runtime_error("FFV1 encoder unavailable");
        codec.reset(avcodec_alloc_context3(encoder));
        if (!codec)
            throw std::bad_alloc();
        codec->width = 64;
        codec->height = 48;
        codec->pix_fmt = pixels;
        codec->time_base = {1, 24};
        codec->color_primaries = AVCOL_PRI_BT709;
        codec->color_trc = transfer;
        codec->colorspace = AVCOL_SPC_BT709;
        codec->color_range = AVCOL_RANGE_MPEG;
        codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
        check(avcodec_open2(codec.get(), encoder, nullptr));
        check(avformat_alloc_output_context2(&output.context, nullptr, "matroska", path.c_str()));
        AVFormatContext* format = output.context;
        if (!format)
            throw std::bad_alloc();
        AVStream* stream = avformat_new_stream(format, nullptr);
        if (!stream)
            throw std::bad_alloc();
        check(avcodec_parameters_from_context(stream->codecpar, codec.get()));
        stream->time_base = codec->time_base;
        stream->avg_frame_rate = {24, 1};
        check(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE));
        check(avformat_write_header(format, nullptr));
        frame.reset(av_frame_alloc());
        packet.reset(av_packet_alloc());
        if (!frame || !packet)
            throw std::bad_alloc();
        frame->format = pixels;
        frame->width = 64;
        frame->height = 48;
        check(av_frame_get_buffer(frame.get(), 0));
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixels);
        const int values[] = {y, cb, cr};
        for (int plane = 0; plane < descriptor->nb_components; ++plane) {
            const int rows = plane == 0 ? 48 : AV_CEIL_RSHIFT(48, descriptor->log2_chroma_h);
            const int cols = plane == 0 ? 64 : AV_CEIL_RSHIFT(64, descriptor->log2_chroma_w);
            for (int row = 0; row < rows; ++row) {
                for (int col = 0; col < cols; ++col) {
                    auto* dest = frame->data[plane] + row * frame->linesize[plane];
                    if (descriptor->comp[plane].depth == 8)
                        dest[col] = split && plane == 0 ? (col < 32 ? 16 : 235) : values[plane];
                    else
                        reinterpret_cast<uint16_t*>(dest)[col] =
                            static_cast<uint16_t>(values[plane] * 4 + (plane == 0 ? lowBits : 0));
                }
            }
        }
        frame->pts = 0;
        check(avcodec_send_frame(codec.get(), frame.get()));
        check(avcodec_send_frame(codec.get(), nullptr));
        int status;
        while ((status = avcodec_receive_packet(codec.get(), packet.get())) == 0) {
            av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(format, packet.get()));
        }
        if (status != AVERROR_EOF)
            check(status);
        check(av_write_trailer(format));
        check(avio_closep(&format->pb));
    }
    ~TaggedClip() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    std::filesystem::path path;

private:
    static void check(int status) {
        if (status < 0)
            throw std::runtime_error("FFV1 fixture failure: " + std::to_string(status));
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
    std::unique_ptr<AVCodecContext, CodecDelete> codec;
    std::unique_ptr<AVFrame, FrameDelete> frame;
    std::unique_ptr<AVPacket, PacketDelete> packet;
    struct Output {
        AVFormatContext* context = nullptr;
        ~Output() {
            if (context) {
                if (context->pb)
                    avio_closep(&context->pb);
                avformat_free_context(context);
            }
        }
    } output;
};

TEST(MediaDecode, IncrementalReferenceSamplesIndependentTaggedPixelsWithoutLinearizing) {
    TaggedClip clip(AV_PIX_FMT_YUV420P, 126, 100, 150);
    nemo::media::ViewerReferenceDecoder reader(clip.path.string(), 32, 24);
    auto frame = reader.next();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->width(), 32);
    EXPECT_EQ(frame->height(), 24);
    EXPECT_EQ(frame->layout().color, nemo::ColorInterpretation::DisplayReferred);
    // Independently evaluated limited-range BT.709 Y=126,Cb=100,Cr=150.
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 32; ++x) {
            auto pixel = frame->pixel(x, y);
            EXPECT_NEAR(pixel[0], 0.656951, 0.00001);
            EXPECT_NEAR(pixel[1], 0.479722, 0.00001);
            EXPECT_NEAR(pixel[2], 0.270333, 0.00001);
        }
    }
    EXPECT_FALSE(reader.next());
}

TEST(MediaDecode, IncrementalReferencePreservesIndependentSpatialBoundary) {
    TaggedClip clip(AV_PIX_FMT_YUV420P, 126, 128, 128, AVCOL_TRC_BT709, 0, true);
    nemo::media::ViewerReferenceDecoder reader(clip.path.string(), 32, 24);
    auto frame = reader.next();
    ASSERT_TRUE(frame);
    EXPECT_FLOAT_EQ(frame->pixel(15, 12)[0], 0.0F);
    EXPECT_FLOAT_EQ(frame->pixel(16, 12)[0], 1.0F);
}

TEST(MediaDecode, SweepFrameLimitAcceptsCleanEofWithoutContainerFrameCount) {
    TaggedClip clip(AV_PIX_FMT_YUV420P);
    nemo::media::SweepOptions options;
    options.codecs = {"libx264-cpu"};
    options.chunkSizes = {1};
    options.maxFrames = 8;
    options.width = 32;
    options.height = 24;
    const auto report = nemo::media::runCodecSweep(clip.path.string(), options);
    ASSERT_TRUE(report.entries[0].measurements) << report.entries[0].unavailableReason;
    EXPECT_EQ(report.entries[0].verifiedFrames, 1);
    EXPECT_FALSE(report.entries[0].measurements->seekMsAtBoundary);
}

TEST(MediaDecode, TaggedNonlinearMidgrayBecomesSceneLinear) {
    TaggedClip clip(AV_PIX_FMT_YUV444P);
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    const auto pixel = decoded.frames[0].pixel(32, 24);
    // Inverse BT.709 at (126-16)/219 = 0.502283: 0.261769.
    for (int channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(pixel[channel], 0.261769, 0.003);
}

TEST(MediaDecode, TaggedLinearTransferIsNotLinearizedAgain) {
    TaggedClip clip(AV_PIX_FMT_YUV444P, 126, 128, 128, AVCOL_TRC_LINEAR);
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    const auto pixel = decoded.frames[0].pixel(32, 24);
    for (int channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(pixel[channel], 0.502283, 0.003);
}

TEST(MediaDecode, TenBitPrecisionSurvivesConversion) {
    TaggedClip clip(AV_PIX_FMT_YUV444P10LE, 126, 128, 128, AVCOL_TRC_LINEAR, 1);
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    // 505 is not divisible by four: truncating to 8-bit loses this level.
    EXPECT_NEAR(decoded.frames[0].pixel(32, 24)[0], 0.50342466F, 0.00001F);
}

TEST(MediaDecode, TaggedColorUsesTransferAndMatrix) {
    TaggedClip clip(AV_PIX_FMT_YUV444P, 126, 100, 150);
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    const auto pixel = decoded.frames[0].pixel(32, 24);
    // Independently evaluated BT.709 limited-range YCbCr and inverse OETF.
    EXPECT_NEAR(pixel[0], 0.4351, 0.004);
    EXPECT_NEAR(pixel[1], 0.2402, 0.004);
    EXPECT_NEAR(pixel[2], 0.0880, 0.004);
}

TEST(MediaDecode, GrayscaleFfv1ConvertsWithoutChromaPlanes) {
    TaggedClip clip(AV_PIX_FMT_GRAY8);
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    EXPECT_EQ(decoded.frames[0].width(), 64);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(decoded.frames[0].pixel(32, 24)[channel], 0.261769, 0.003);
    }
}

TEST(MediaDecode, HighDepth444ConvertsIntoSceneLinear) {
    TaggedClip clip(AV_PIX_FMT_YUV444P10LE);
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    EXPECT_NEAR(decoded.frames[0].pixel(32, 24)[0], 0.261769, 0.003);
}

TEST(MediaDecode, MissingTransferIsNotSilentlyRelabeledLinear) {
    TaggedClip clip(AV_PIX_FMT_YUV444P, 126, 128, 128, AVCOL_TRC_UNSPECIFIED);
    EXPECT_THROW(static_cast<void>(nemo::media::decodeClipSoftware(clip.path.string())), std::runtime_error);
}

TEST(MediaDecode, ExplicitOverrideResolvesMissingTransferOnly) {
    TaggedClip clip(AV_PIX_FMT_YUV444P, 126, 128, 128, AVCOL_TRC_UNSPECIFIED);
    nemo::media::ColorOverride interpretation;
    interpretation.transfer = nemo::gpu::MediaTransfer::Bt709;
    const auto decoded = nemo::media::decodeClipSoftware(clip.path.string(), -1, {}, interpretation);
    ASSERT_EQ(decoded.frames.size(), 1u);
    EXPECT_NEAR(decoded.frames[0].pixel(32, 24)[0], 0.261769, 0.003);

    TaggedClip tagged(AV_PIX_FMT_YUV444P, 126, 128, 128, AVCOL_TRC_LINEAR);
    const auto linear = nemo::media::decodeClipSoftware(tagged.path.string(), -1, {}, interpretation);
    ASSERT_EQ(linear.frames.size(), 1u);
    EXPECT_NEAR(linear.frames[0].pixel(32, 24)[0], 0.502283, 0.003);
}

TEST(MediaDecode, UnsupportedWorkingPolicyIsNotSilentlyIgnored) {
    TaggedClip clip(AV_PIX_FMT_YUV444P);
    nemo::ColorPolicy policy;
    policy.workingSpace = "ACEScg";
    try {
        static_cast<void>(nemo::media::decodeClipSoftware(clip.path.string(), -1, policy));
        FAIL() << "unsupported working policy accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("ACEScg"), std::string::npos);
    }
}

TEST(MediaDecode, CorruptInputNamesTheClip) {
    const auto path = std::filesystem::temp_directory_path() / "nemo-corrupt-media.mkv";
    {
        std::ofstream file(path);
        file << "not a media container";
    }
    try {
        static_cast<void>(nemo::media::decodeClipSoftware(path.string()));
        FAIL() << "corrupt input accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(path.string()), std::string::npos);
    }
    std::filesystem::remove(path);
}

}  // namespace
