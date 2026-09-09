// Viewer-chunk encode tests (issue #21).
//
// The encode contract: one independently decodable mp4 chunk carrying the
// COMPLETE display-referred interpretation — BT.709 primaries, BT.709
// transfer, BT.709 matrix, limited (MPEG) range, left chroma location,
// 8-bit 4:2:0 — in the container stream (nclx: primaries/transfer/matrix/
// range) and the coded bitstream (VUI: all fields, plus the coded format
// and chroma location, which an mp4 sample description does not carry).
// Replay through decodeViewerChunkSoftware must return the baked
// display-referred values (no re-applied linearization/view transform).
//
// Robustness seam: failures are injected deterministically through the
// public EncodeOptions.injectedFailure (no global mutable hooks) to prove
// that every error path releases FFmpeg resources (sanitizer evidence) and
// never reports partial success — the partially written file is removed.
//

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "nemo/media/CodecSweep.hpp"
#include "nemo/media/VideoDecode.hpp"
#include "nemo/media/ViewerEncode.hpp"

using namespace nemo;
using namespace nemo::media;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;

CpuImage flatImage(int width, int height, float r, float g, float b) {
    CpuImage image(ImageLayout{.width = width, .height = height, .color = ColorInterpretation::DisplayReferred});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, {r, g, b, 1.0F});
        }
    }
    return image;
}

// Gradient frame: exercises 4:2:0 chroma subsampling, not just flat color.
CpuImage gradientImage(int width, int height) {
    CpuImage image(ImageLayout{.width = width, .height = height, .color = ColorInterpretation::DisplayReferred});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float r = static_cast<float>(x) / static_cast<float>(width - 1);
            const float g = static_cast<float>(y) / static_cast<float>(height - 1);
            image.setPixel(x, y, {r, g, 0.25F, 1.0F});
        }
    }
    return image;
}

// Removes the encode output it owns (the encode's own guard removes it on
// failure; this removes it after a passing run).
struct TempFile {
    explicit TempFile(std::string name) : path(std::filesystem::temp_directory_path() / std::move(name)) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::filesystem::path path;
};

EncodeOptions cpuOptions() {
    EncodeOptions options;
    options.codec = "libx264-cpu";
    options.bitrateKbps = 8000;  // small flat frames: quantization stays tiny
    return options;
}

// Interpretation metadata as it must be readable from an encoded chunk.
struct ChunkMetadata {
    AVPixelFormat format;
    AVColorPrimaries primaries;
    AVColorTransferCharacteristic transfer;
    AVColorSpace matrix;
    AVColorRange range;
    AVChromaLocation chromaLocation;
};

AVStream* videoStreamOf(AVFormatContext* raw) {
    for (unsigned index = 0; index < raw->nb_streams; ++index) {
        if (raw->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return raw->streams[index];
        }
    }
    throw std::runtime_error("fixture failure: no video stream");
}

// Opens the encoded chunk with libavformat — independent of the encoder
// under test AND of the decoder — and reads the stream's container-level
// interpretation metadata.
[[nodiscard]] ChunkMetadata readChunkMetadata(const std::filesystem::path& path) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.string().c_str(), nullptr, nullptr) < 0 || raw == nullptr) {
        throw std::runtime_error("fixture failure: cannot open " + path.string());
    }
    struct FormatCloser {
        AVFormatContext* value;
        ~FormatCloser() { avformat_close_input(&value); }
    } closer{raw};

    const AVCodecParameters* parameters = videoStreamOf(raw)->codecpar;
    return {static_cast<AVPixelFormat>(parameters->format),
            static_cast<AVColorPrimaries>(parameters->color_primaries),
            static_cast<AVColorTransferCharacteristic>(parameters->color_trc),
            static_cast<AVColorSpace>(parameters->color_space),
            static_cast<AVColorRange>(parameters->color_range),
            static_cast<AVChromaLocation>(parameters->chroma_location)};
}

// Decodes the chunk's first frame with libavcodec and reads the color
// signal info the DECODER sees — the coded VUI, not container tags. This
// is what proves the interpretation travels inside the chunk, including
// the coded 8-bit 4:2:0 format and chroma location, which an mp4 sample
// description does not carry. Single-frame chunks emit their frame only at
// flush, so the decoder is flushed before giving up.
[[nodiscard]] ChunkMetadata decodeFirstFrameMetadata(const std::filesystem::path& path) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.string().c_str(), nullptr, nullptr) < 0 || raw == nullptr) {
        throw std::runtime_error("fixture failure: cannot open " + path.string());
    }
    struct FormatCloser {
        AVFormatContext* value;
        ~FormatCloser() { avformat_close_input(&value); }
    } closer{raw};
    AVStream* stream = videoStreamOf(raw);

    struct CodecContext {
        AVCodecContext* value = nullptr;
        ~CodecContext() {
            if (value)
                avcodec_free_context(&value);
        }
    } codec;
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        throw std::runtime_error("fixture failure: no decoder for the encoded chunk");
    }
    codec.value = avcodec_alloc_context3(decoder);
    if (codec.value == nullptr || avcodec_parameters_to_context(codec.value, stream->codecpar) < 0 ||
        avcodec_open2(codec.value, decoder, nullptr) < 0) {
        throw std::runtime_error("fixture failure: decoder setup failed");
    }
    struct Packet {
        AVPacket* value = av_packet_alloc();
        ~Packet() {
            if (value)
                av_packet_free(&value);
        }
    } packet;
    struct Frame {
        AVFrame* value = av_frame_alloc();
        ~Frame() {
            if (value)
                av_frame_free(&value);
        }
    } frame;
    if (packet.value == nullptr || frame.value == nullptr) {
        throw std::runtime_error("fixture failure: packet/frame allocation failed");
    }

    const auto metadataOf = [&]() {
        const ChunkMetadata metadata{static_cast<AVPixelFormat>(frame.value->format),
                                     frame.value->color_primaries,
                                     frame.value->color_trc,
                                     frame.value->colorspace,
                                     frame.value->color_range,
                                     frame.value->chroma_location};
        av_frame_unref(frame.value);
        return metadata;
    };
    while (av_read_frame(raw, packet.value) == 0) {
        if (packet.value->stream_index == stream->index) {
            if (avcodec_send_packet(codec.value, packet.value) < 0) {
                throw std::runtime_error("fixture failure: chunk packet undecodable");
            }
            if (avcodec_receive_frame(codec.value, frame.value) == 0) {
                return metadataOf();
            }
        }
        av_packet_unref(packet.value);
    }
    if (avcodec_send_packet(codec.value, nullptr) == 0 && avcodec_receive_frame(codec.value, frame.value) == 0) {
        return metadataOf();
    }
    throw std::runtime_error("fixture failure: chunk decoded no frame");
}

EncodeFailure injected(EncodeFailure::Stage stage, int occurrence) {
    EncodeFailure failure;
    failure.stage = stage;
    failure.occurrence = occurrence;
    return failure;
}

TEST(MediaEncode, ChunkComparisonRejectsIncompleteDecodeAndDimensionMismatch) {
    TempFile output("nemo-encode-incomplete-comparison.mp4");
    const std::vector<CpuImage> single{flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)};
    static_cast<void>(encodeViewerChunk(output.path.string(), single, cpuOptions()));
    const std::vector<CpuImage> two(2, single.front());
    EXPECT_THROW(static_cast<void>(compareViewerChunk(output.path.string(), two)), std::runtime_error);
    const std::vector<CpuImage> wrong{flatImage(32, 24, 0.5F, 0.5F, 0.5F)};
    EXPECT_THROW(static_cast<void>(compareViewerChunk(output.path.string(), wrong)), std::runtime_error);
}

TEST(MediaEncode, SweepFrameLimitDoesNotRejectShortDeclaredSources) {
    TempFile output("nemo-encode-short-source.mp4");
    const std::vector<CpuImage> frames{flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)};
    static_cast<void>(encodeViewerChunk(output.path.string(), frames, cpuOptions()));
    SweepOptions options;
    options.codecs = {"libx264-cpu"};
    options.chunkSizes = {8};
    options.maxFrames = 8;
    options.width = kWidth;
    options.height = kHeight;
    for (const auto& report : {runCodecSweep(output.path.string(), options), runCodecSweep(frames, options)}) {
        ASSERT_TRUE(report.entries[0].measurements) << report.entries[0].unavailableReason;
        EXPECT_EQ(report.entries[0].verifiedFrames, 1);
        EXPECT_FALSE(report.entries[0].measurements->seekMsAtBoundary);
    }
}

TEST(MediaEncode, CompleteChunkTimingUsesMillisecondsAndIncludesExclusiveStages) {
    TempFile output("nemo-encode-timing.mp4");
    const std::vector<CpuImage> frames(8, gradientImage(kWidth, kHeight));
    const auto start = std::chrono::steady_clock::now();
    const auto stats = encodeViewerChunk(output.path.string(), frames, cpuOptions());
    const auto wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const double stages = stats.initializationMs + stats.allocationPackingMs + stats.conversionMs +
                          stats.hostToDeviceMs + stats.submissionDrainMs + stats.muxFinalizationMs;
    EXPECT_LE(stages, stats.completeChunkMs);
    EXPECT_LE(stats.completeChunkMs, wall);
    EXPECT_GT(stats.completeChunkMs, wall / 100.0);  // detects ns/us/seconds-to-ms scale errors
    EXPECT_EQ(decodeViewerChunkSoftware(output.path.string()).frames.size(), 8u);
}
}  // namespace

// Acceptance example 3 (metadata half): the container stream carries the
// display-referred BT.709 interpretation — primaries, transfer, matrix,
// limited range — which is all an mp4 sample description can record.
TEST(MediaEncode, EncodedMetadataIsCompleteDisplayReferredBt709) {
    TempFile output("nemo-encode-metadata.mp4");
    const std::vector<CpuImage> display = {flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F),
                                           flatImage(kWidth, kHeight, 0.8F, 0.2F, 0.1F),
                                           gradientImage(kWidth, kHeight)};
    const EncodeStats stats = encodeViewerChunk(output.path.string(), display, cpuOptions());
    EXPECT_EQ(stats.encodedFrames, 3);
    EXPECT_GT(stats.encodedBytes, 0);

    const ChunkMetadata metadata = readChunkMetadata(output.path);
    EXPECT_EQ(metadata.primaries, AVCOL_PRI_BT709);
    EXPECT_EQ(metadata.transfer, AVCOL_TRC_BT709);
    EXPECT_EQ(metadata.matrix, AVCOL_SPC_BT709);
    EXPECT_EQ(metadata.range, AVCOL_RANGE_MPEG);
}

// The same interpretation must travel inside the bitstream: the decoder
// reads primaries/transfer/matrix/range AND chroma location and the coded
// 8-bit 4:2:0 format from the chunk itself, so replay needs no outside
// knowledge of how the chunk was baked.
TEST(MediaEncode, BitstreamCarriesBt709SignalInfo) {
    TempFile output("nemo-encode-vui.mp4");
    const std::vector<CpuImage> display = {flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)};
    static_cast<void>(encodeViewerChunk(output.path.string(), display, cpuOptions()));

    const ChunkMetadata metadata = decodeFirstFrameMetadata(output.path);
    EXPECT_EQ(metadata.format, AV_PIX_FMT_YUV420P);
    EXPECT_EQ(metadata.primaries, AVCOL_PRI_BT709);
    EXPECT_EQ(metadata.transfer, AVCOL_TRC_BT709);
    EXPECT_EQ(metadata.matrix, AVCOL_SPC_BT709);
    EXPECT_EQ(metadata.range, AVCOL_RANGE_MPEG);
    EXPECT_EQ(metadata.chromaLocation, AVCHROMA_LOC_LEFT);
}

// Acceptance example 3 (pixels half): replay through the explicit viewer
// chunk decoder returns the BAKED display-referred values. Midgray 0.5
// display must survive as 0.5 — linearizing it would read ~0.214.
TEST(MediaEncode, NonlinearViewerValuesSurviveReplay) {
    TempFile output("nemo-encode-replay.mp4");
    std::vector<CpuImage> display = {flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F),
                                     flatImage(kWidth, kHeight, 0.8F, 0.2F, 0.1F), gradientImage(kWidth, kHeight)};
    static_cast<void>(encodeViewerChunk(output.path.string(), display, cpuOptions()));

    const SoftwareClip replay = decodeViewerChunkSoftware(output.path.string(), -1);
    ASSERT_EQ(replay.frames.size(), display.size());
    EXPECT_EQ(replay.frames[0].layout().color, ColorInterpretation::DisplayReferred);

    // Flat fields: no subsampling error, only codec + rounding — tight.
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto midgray = replay.frames[0].pixel(x, y);
            EXPECT_NEAR(midgray[0], 0.5F, 0.02F) << "midgray linearized instead of preserved at " << x << "," << y;
            EXPECT_NEAR(midgray[1], 0.5F, 0.02F);
            EXPECT_NEAR(midgray[2], 0.5F, 0.02F);
            const auto color = replay.frames[1].pixel(x, y);
            EXPECT_NEAR(color[0], 0.8F, 0.03F);
            EXPECT_NEAR(color[1], 0.2F, 0.03F);
            EXPECT_NEAR(color[2], 0.1F, 0.03F);
        }
    }

    // Gradient: subsampled chroma costs a few LSB — measured, bounded.
    double maxError = 0.0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto a = replay.frames[2].pixel(x, y);
            const auto b = display[2].pixel(x, y);
            for (int channel = 0; channel < 3; ++channel) {
                maxError =
                    std::max(maxError, std::abs(static_cast<double>(a[channel]) - static_cast<double>(b[channel])));
            }
        }
    }
    EXPECT_LT(maxError, 0.08) << "gradient replay degraded by " << maxError;
}

TEST(MediaEncode, ChromaSamplesMatchDeclaredLeftSiting) {
    TempFile output("nemo-encode-chroma-siting.mp4");
    CpuImage image = flatImage(kWidth, kHeight, 0.0F, 0.0F, 0.0F);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 31; x < kWidth; ++x)
            image.setPixel(x, y, {1.0F, 0.0F, 0.0F, 1.0F});
    }
    static_cast<void>(encodeViewerChunk(output.path.string(), std::span(&image, 1), cpuOptions()));
    const auto replay = decodeViewerChunkSoftware(output.path.string());
    ASSERT_EQ(replay.frames.size(), 1u);
    // At left-sited x=30 a symmetric chroma filter sees 1/4 red; the
    // luma is black. R' = (1 - Kr) / 4 = .19685. A centered 2x2 box
    // mislabeled "left" instead yields .3937 (a half-pixel chroma shift).
    EXPECT_NEAR(replay.frames[0].pixel(30, 24)[0], 0.19685F, 0.035F);
}

// A single-frame chunk must remain independently decodable: movenc derives
// the only sample's duration from the packet duration, so a dropped
// duration collapses the chunk into a zero-length edit list the demuxer
// discards (regression for the observed undecodable 1-frame chunk).
TEST(MediaEncode, SingleFrameChunkIsDecodable) {
    TempFile output("nemo-encode-single-frame.mp4");
    const std::vector<CpuImage> display = {flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)};
    static_cast<void>(encodeViewerChunk(output.path.string(), display, cpuOptions()));

    const SoftwareClip replay = decodeViewerChunkSoftware(output.path.string(), -1);
    ASSERT_EQ(replay.frames.size(), 1u);
    const auto pixel = replay.frames[0].pixel(0, 0);
    EXPECT_NEAR(pixel[0], 0.5F, 0.05F);
}

TEST(MediaEncode, SaturatedGreenDoesNotGainRedFromWrongMatrix) {
    TempFile output("nemo-encode-bt709-green.mp4");
    static_cast<void>(encodeViewerChunk(output.path.string(), std::array{flatImage(kWidth, kHeight, 0.0F, 1.0F, 0.0F)},
                                        cpuOptions()));
    const auto replay = decodeViewerChunkSoftware(output.path.string());
    ASSERT_EQ(replay.frames.size(), 1u);
    const auto pixel = replay.frames[0].pixel(32, 24);
    EXPECT_NEAR(pixel[0], 0.0F, 0.01F);
    EXPECT_NEAR(pixel[1], 1.0F, 0.01F);
    EXPECT_NEAR(pixel[2], 0.0F, 0.01F);
}

TEST(MediaEncode, SceneLinearInputRequiresViewingTransform) {
    TempFile output("nemo-encode-reject-scene-linear.mp4");
    const CpuImage sceneLinear(kWidth, kHeight);
    EXPECT_THROW(static_cast<void>(encodeViewerChunk(output.path.string(), std::span(&sceneLinear, 1), cpuOptions())),
                 MediaCodecError);
    EXPECT_FALSE(std::filesystem::exists(output.path));
}

// Acceptance example 4: an injected allocation failure throws a named
// diagnostic and releases everything (sanitizer evidence); no partial
// file survives.
TEST(MediaEncode, InjectedAllocationFailureThrowsAndCleansUp) {
    TempFile output("nemo-encode-inject-allocation.mp4");
    EncodeOptions options = cpuOptions();
    const EncodeFailure failure = injected(EncodeFailure::Stage::Allocation, 1);
    options.injectedFailure = &failure;

    try {
        static_cast<void>(
            encodeViewerChunk(output.path.string(), std::array{flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)}, options));
        FAIL() << "expected injected allocation failure to throw";
    } catch (const MediaCodecError& error) {
        EXPECT_EQ(error.codec, "libx264-cpu");
        EXPECT_NE(std::string(error.message).find("allocation"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path)) << "error path left a partial output file";
}

TEST(MediaEncode, InjectedInitFailureThrowsAndCleansUp) {
    TempFile output("nemo-encode-inject-init.mp4");
    EncodeOptions options = cpuOptions();
    const EncodeFailure failure = injected(EncodeFailure::Stage::Init, 1);
    options.injectedFailure = &failure;

    try {
        static_cast<void>(
            encodeViewerChunk(output.path.string(), std::array{flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)}, options));
        FAIL() << "expected injected init failure to throw";
    } catch (const MediaCodecError& error) {
        EXPECT_EQ(error.codec, "libx264-cpu");
        EXPECT_NE(std::string(error.message).find("init"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path)) << "error path left a partial output file";
}

// The Nth submission fails: the first frame must already be submitted and
// its encoder state released when the error unwinds.
TEST(MediaEncode, InjectedSubmissionFailureThrowsAndCleansUp) {
    TempFile output("nemo-encode-inject-submission.mp4");
    EncodeOptions options = cpuOptions();
    const EncodeFailure failure = injected(EncodeFailure::Stage::Submission, 2);
    options.injectedFailure = &failure;
    const std::vector<CpuImage> display = {
        flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F), flatImage(kWidth, kHeight, 0.8F, 0.2F, 0.1F),
        flatImage(kWidth, kHeight, 0.1F, 0.9F, 0.3F), flatImage(kWidth, kHeight, 0.0F, 0.0F, 0.0F)};

    try {
        static_cast<void>(encodeViewerChunk(output.path.string(), display, options));
        FAIL() << "expected injected submission failure to throw";
    } catch (const MediaCodecError& error) {
        EXPECT_EQ(error.codec, "libx264-cpu");
        EXPECT_NE(std::string(error.message).find("submission"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path)) << "error path left a partial output file";
}

// The Nth packet write fails mid-drain: packets already written must be
// discarded with the file, not reported as success.
TEST(MediaEncode, InjectedWriteFailureThrowsAndCleansUp) {
    TempFile output("nemo-encode-inject-write.mp4");
    EncodeOptions options = cpuOptions();
    options.gopSize = 4;  // several packets across 8 frames
    const EncodeFailure failure = injected(EncodeFailure::Stage::Write, 2);
    options.injectedFailure = &failure;
    std::vector<CpuImage> display;
    for (int index = 0; index < 8; ++index) {
        display.push_back(flatImage(kWidth, kHeight, 0.1F * index + 0.2F, 0.4F, 0.6F - 0.05F * index));
    }

    try {
        static_cast<void>(encodeViewerChunk(output.path.string(), display, options));
        FAIL() << "expected injected write failure to throw";
    } catch (const MediaCodecError& error) {
        EXPECT_EQ(error.codec, "libx264-cpu");
        EXPECT_NE(std::string(error.message).find("write"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path)) << "error path left a partial output file";
}

// The finalization failure hits after every frame drained and before the
// trailer: the full encode must still end in a throw, not a partial
// success report.
TEST(MediaEncode, InjectedFinalizationFailureThrowsAndCleansUp) {
    TempFile output("nemo-encode-inject-finalization.mp4");
    EncodeOptions options = cpuOptions();
    const EncodeFailure failure = injected(EncodeFailure::Stage::Finalization, 1);
    options.injectedFailure = &failure;
    const std::vector<CpuImage> display = {flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F)};

    try {
        static_cast<void>(encodeViewerChunk(output.path.string(), display, options));
        FAIL() << "expected injected finalization failure to throw";
    } catch (const MediaCodecError& error) {
        EXPECT_EQ(error.codec, "libx264-cpu");
        EXPECT_NE(std::string(error.message).find("finalization"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path)) << "error path left a partial output file";
}

// Memory safety: odd dimensions cannot map onto left-chroma 4:2:0; the
// encode rejects them with a diagnostic instead of overflowing the
// subsampled planes.
TEST(MediaEncode, OddDimensionsFailWithDiagnostic) {
    TempFile output("nemo-encode-odd-dims.mp4");
    const std::vector<CpuImage> display = {flatImage(65, kHeight, 0.5F, 0.5F, 0.5F)};

    try {
        static_cast<void>(encodeViewerChunk(output.path.string(), display, cpuOptions()));
        FAIL() << "expected odd dimensions to be rejected";
    } catch (const MediaCodecError& error) {
        EXPECT_NE(std::string(error.message).find("4:2:0"), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.message).find("65"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path));
}

// A frame list with inconsistent dimensions is rejected, naming the
// offending frame.
TEST(MediaEncode, InconsistentFrameDimensionsFailWithDiagnostic) {
    TempFile output("nemo-encode-mixed-dims.mp4");
    const std::vector<CpuImage> display = {flatImage(kWidth, kHeight, 0.5F, 0.5F, 0.5F),
                                           flatImage(32, 24, 0.5F, 0.5F, 0.5F)};

    try {
        static_cast<void>(encodeViewerChunk(output.path.string(), display, cpuOptions()));
        FAIL() << "expected inconsistent dimensions to be rejected";
    } catch (const MediaCodecError& error) {
        EXPECT_NE(std::string(error.message).find("frame 1"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(output.path));
}
