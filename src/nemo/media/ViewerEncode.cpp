#include "nemo/media/ViewerEncode.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace nemo::media {

namespace {

// Encoder registry: id -> libavcodec encoder name + hardware flag.
struct EncoderInfo {
    const char* id;
    const char* avcodecName;
    bool hardware;
};

constexpr EncoderInfo kEncoders[] = {
    {"h264-nvenc", "h264_nvenc", true},
    {"hevc-nvenc", "hevc_nvenc", true},
    {"libx264-cpu", "libx264", false},
    {"libx265-cpu", "libx265", false},
};

// --- Exception-safe FFmpeg ownership (issue #21). Each guard owns one
// FFmpeg allocation; the encode function holds them so every error path —
// including injected failures — releases the same set, and no error path
// reports partial success.

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
struct BufferRefDelete {
    void operator()(AVBufferRef* value) const { av_buffer_unref(&value); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDelete>;
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDelete>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDelete>;
using FramePtr = std::unique_ptr<AVFrame, FrameDelete>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDelete>;

[[nodiscard]] std::string avError(int status) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(status, text, sizeof(text));
    return text;
}

// Deterministic failure injection (issue #21): counts stage actions and
// throws exactly when the caller-requested (stage, occurrence) is hit.
// Lives only inside one encode call — no global mutable state.
class FailureInjector {
public:
    explicit FailureInjector(const EncodeOptions& options) : failure_(options.injectedFailure), codec_(options.codec) {}

    // Counts one action of `stage`; throws when this action is the
    // injected occurrence. Stages other than the injected one return.
    void check(EncodeFailure::Stage stage) {
        if (failure_ == nullptr || failure_->stage != stage) {
            return;
        }
        const int occurrence = ++count_;
        if (occurrence == failure_->occurrence) {
            throw MediaCodecError(codec_, std::string("injected failure: ") + stageName(stage) + " occurrence " +
                                              std::to_string(occurrence));
        }
    }

private:
    [[nodiscard]] static const char* stageName(EncodeFailure::Stage stage) {
        switch (stage) {
        case EncodeFailure::Stage::Allocation:
            return "allocation";
        case EncodeFailure::Stage::Init:
            return "init";
        case EncodeFailure::Stage::Submission:
            return "submission";
        case EncodeFailure::Stage::Write:
            return "write";
        case EncodeFailure::Stage::Finalization:
            return "finalization";
        }
        return "unknown";
    }

    const EncodeFailure* failure_;
    const std::string& codec_;
    int count_ = 0;
};
// Removes the encode's own output when the encode does not finish: the
// error paths never leave a partial file behind (avio_open truncates the
// path either way, so the remove discards only bytes this call wrote).
// The AVIO context is closed here too — avformat_free_context does not
// close `pb`, so an error path that skips this would leak it.
struct OutputGuard {
    std::filesystem::path path;
    AVFormatContext* format = nullptr;  // set once the format exists
    bool fileWritten = false;           // avio_open succeeded
    bool success = false;
    ~OutputGuard() {
        if (!success) {
            if (format != nullptr && format->pb != nullptr) {
                avio_closep(&format->pb);
            }
            if (fileWritten) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    }
};

// Packs display-referred RGBA (already converted by
// yuv420pFromDisplayReferred) into an 8-bit YUV420P AVFrame. Returns
// nullptr on allocation failure; the caller frees the frame.
[[nodiscard]] AVFrame* makeYuv420pFrame(int width, int height, const std::vector<std::uint8_t>& planes) {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        return nullptr;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    const std::uint8_t* y = planes.data();
    const std::uint8_t* cb = y + static_cast<size_t>(width) * height;
    const std::uint8_t* cr = cb + static_cast<size_t>(width / 2) * (height / 2);
    for (int row = 0; row < height; ++row) {
        std::memcpy(frame->data[0] + row * frame->linesize[0], y + row * width, static_cast<size_t>(width));
    }
    for (int row = 0; row < height / 2; ++row) {
        std::memcpy(frame->data[1] + row * frame->linesize[1], cb + row * (width / 2), static_cast<size_t>(width / 2));
        std::memcpy(frame->data[2] + row * frame->linesize[2], cr + row * (width / 2), static_cast<size_t>(width / 2));
    }
    return frame;
}

void yuv420pFromDisplayReferred(const CpuImage& image, std::vector<std::uint8_t>& planes) {
    const int width = image.width();
    const int height = image.height();
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    auto* y = planes.data();
    auto* cb = planes.data() + static_cast<size_t>(width) * height;
    auto* cr = cb + static_cast<size_t>(chromaWidth) * chromaHeight;
    const auto encode709 = [](float r, float g, float b, float& yv, float& cbv, float& crv) {
        r = std::clamp(r, 0.0F, 1.0F);
        g = std::clamp(g, 0.0F, 1.0F);
        b = std::clamp(b, 0.0F, 1.0F);
        // Rec.709 matrix, limited range: Y' 16..235, Cb/Cr 16..240.
        const float luma = 0.2126F * r + 0.7152F * g + 0.0722F * b;
        yv = 16.0F + 219.0F * luma;
        cbv = 128.0F + (112.0F / (1.0F - 0.0722F)) * (b - luma);
        crv = 128.0F + (112.0F / (1.0F - 0.2126F)) * (r - luma);
    };
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const auto rgba = image.pixel(col, row);
            float yv = 0, cbv = 0, crv = 0;
            encode709(rgba[0], rgba[1], rgba[2], yv, cbv, crv);
            y[row * width + col] = static_cast<uint8_t>(std::clamp(yv, 0.0F, 255.0F));
            if (row % 2 == 0 && col % 2 == 0) {
                // Left-sited chroma: centered horizontally on the even
                // luma sample and halfway between the two luma rows.
                const auto mean = [&](int channel) {
                    float sum = 0.0F;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const auto sample = image.pixel(std::clamp(col + dx, 0, width - 1), row + dy);
                            sum += std::clamp(sample[channel], 0.0F, 1.0F) * (dx == 0 ? 2.0F : 1.0F);
                        }
                    }
                    return sum / 8.0F;
                };
                float yv2 = 0, cbv2 = 0, crv2 = 0;
                encode709(mean(0), mean(1), mean(2), yv2, cbv2, crv2);
                cb[(row / 2) * chromaWidth + (col / 2)] = static_cast<uint8_t>(std::clamp(cbv2, 0.0F, 255.0F));
                cr[(row / 2) * chromaWidth + (col / 2)] = static_cast<uint8_t>(std::clamp(crv2, 0.0F, 255.0F));
            }
        }
    }
}

}  // namespace

EncodeStats encodeViewerChunk(const std::string& outputPath, std::span<const CpuImage> displayReferredFrames,
                              const EncodeOptions& options) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const auto completeStart = Clock::now();
    EncodeStats stats;
    stats.codec = options.codec;
    if (displayReferredFrames.empty()) {
        throw MediaCodecError(options.codec, "no frames to encode");
    }
    if (options.gopSize < 1) {
        throw MediaCodecError(options.codec, "gopSize must be >= 1");
    }
    if (options.bitrateKbps < 1) {
        throw MediaCodecError(options.codec, "bitrateKbps must be >= 1");
    }
    if (options.bitDepth != 8) {
        throw MediaCodecError(options.codec, "unsupported bit depth " + std::to_string(options.bitDepth) +
                                                 "; viewer encoder supports 8-bit 4:2:0 only");
    }
    const int width = displayReferredFrames.front().width();
    const int height = displayReferredFrames.front().height();
    // Left-chroma 4:2:0 needs even dimensions, and the buffer math below
    // must stay inside addressable ranges (memory safety, issue #21).
    if (width % 2 != 0 || height % 2 != 0) {
        throw MediaCodecError(options.codec,
                              "viewer chunks encode as 4:2:0 left chroma; dimensions must be even (got " +
                                  std::to_string(width) + "x" + std::to_string(height) + ")");
    }
    if (av_image_check_size(static_cast<unsigned>(width), static_cast<unsigned>(height), 0, nullptr) < 0) {
        throw MediaCodecError(options.codec, "frame dimensions are outside the encodable range (" +
                                                 std::to_string(width) + "x" + std::to_string(height) + ")");
    }
    for (size_t index = 0; index < displayReferredFrames.size(); ++index) {
        const CpuImage& frame = displayReferredFrames[index];
        if (frame.width() != width || frame.height() != height) {
            throw MediaCodecError(options.codec, "frame " + std::to_string(index) + " is " +
                                                     std::to_string(frame.width()) + "x" +
                                                     std::to_string(frame.height()) + ", expected " +
                                                     std::to_string(width) + "x" + std::to_string(height));
        }
        if (frame.layout().color != ColorInterpretation::DisplayReferred) {
            throw MediaCodecError(options.codec, "frame " + std::to_string(index) +
                                                     " is scene-linear; apply the viewing transform before encoding");
        }
    }

    const AVCodec* encoder = nullptr;
    bool hardware = false;
    for (const auto& entry : kEncoders) {
        if (options.codec == entry.id) {
            hardware = entry.hardware;
            encoder = avcodec_find_encoder_by_name(entry.avcodecName);
            if (encoder == nullptr) {
                throw MediaCodecError(options.codec, "encoder not registered in this libavcodec build");
            }
            break;
        }
    }
    if (encoder == nullptr) {
        throw MediaCodecError(options.codec, "unknown encoder id (probe with probe-media)");
    }
    const bool hevc = encoder->id == AV_CODEC_ID_HEVC;
    stats.profile = options.profile.empty() ? (hevc ? "main" : "high") : options.profile;
    if (stats.profile != (hevc ? "main" : "high")) {
        throw MediaCodecError(options.codec, "unsupported profile '" + stats.profile +
                                                 "'; supported 8-bit profile is " + (hevc ? "main" : "high"));
    }

    FailureInjector injection(options);
    CodecContextPtr codec(avcodec_alloc_context3(encoder));
    if (codec == nullptr) {
        throw MediaCodecError(options.codec, "context allocation failed");
    }
    codec->width = width;
    codec->height = height;
    codec->time_base = AVRational{1, 24};
    codec->framerate = AVRational{24, 1};
    codec->gop_size = options.gopSize;
    codec->bit_rate = static_cast<int64_t>(options.bitrateKbps) * 1000;
    codec->profile = hevc ? FF_PROFILE_HEVC_MAIN : FF_PROFILE_H264_HIGH;
    // Bound the experiment's codec worker count; rate control remains ABR.
    codec->thread_count = 2;
    if (!hardware && hevc) {
        const int status = av_opt_set(codec->priv_data, "x265-params", "pools=2:frame-threads=2", 0);
        if (status < 0)
            throw MediaCodecError(options.codec, "x265 thread configuration failed: " + avError(status));
    }
    // Complete display-referred interpretation (issue #21): every field
    // the chunk must carry so replay is unambiguous — the transfer is as
    // much interpretation metadata as the matrix.
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->color_trc = AVCOL_TRC_BT709;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_range = AVCOL_RANGE_MPEG;
    codec->chroma_sample_location = AVCHROMA_LOC_LEFT;

    BufferRefPtr hwDevice;
    BufferRefPtr hwFrames;
    if (hardware) {
        // NVENC consumes CUDA frames: transfer API submission is measured separately.
        codec->pix_fmt = AV_PIX_FMT_CUDA;
        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            throw MediaCodecError(options.codec, "CUDA hwdevice init failed (engine unavailable on this device)");
        }
        hwDevice.reset(device);
        AVBufferRef* pool = av_hwframe_ctx_alloc(hwDevice.get());
        if (pool == nullptr) {
            throw MediaCodecError(options.codec, "frame pool allocation failed");
        }
        hwFrames.reset(pool);
        AVHWFramesContext* frames = reinterpret_cast<AVHWFramesContext*>(hwFrames->data);
        frames->format = AV_PIX_FMT_CUDA;
        frames->sw_format = AV_PIX_FMT_YUV420P;
        frames->width = width;
        frames->height = height;
        frames->initial_pool_size = 4;
        // The allocation phase ends here: context, device and pool all
        // exist and must be released by the unwound error path.
        injection.check(EncodeFailure::Stage::Allocation);
        if (av_hwframe_ctx_init(hwFrames.get()) < 0) {
            throw MediaCodecError(options.codec, "frame pool init failed");
        }
        codec->hw_frames_ctx = av_buffer_ref(hwFrames.get());
        if (codec->hw_frames_ctx == nullptr) {
            throw MediaCodecError(options.codec, "frame pool reference failed");
        }
    } else {
        codec->pix_fmt = AV_PIX_FMT_YUV420P;
        injection.check(EncodeFailure::Stage::Allocation);
    }

    const int openStatus = avcodec_open2(codec.get(), encoder, nullptr);
    if (openStatus < 0) {
        throw MediaCodecError(options.codec, "encoder open failed: " + avError(openStatus));
    }
    // The init phase ends here: codec open, and on the hw path the pool
    // is initialized and referenced.
    injection.check(EncodeFailure::Stage::Init);
    stats.initializationMs = elapsed(completeStart);
    auto muxStart = Clock::now();

    FormatContextPtr format;
    {
        AVFormatContext* raw = nullptr;
        if (avformat_alloc_output_context2(&raw, nullptr, nullptr, outputPath.c_str()) < 0 || raw == nullptr) {
            throw MediaCodecError(options.codec, "output context allocation failed");
        }
        format.reset(raw);
    }
    AVStream* stream = avformat_new_stream(format.get(), nullptr);
    if (stream == nullptr || avcodec_parameters_from_context(stream->codecpar, codec.get()) != 0) {
        throw MediaCodecError(options.codec, "stream setup failed");
    }
    // The container must describe the coded representation, not the
    // device staging format, and must carry the interpretation even where
    // avcodec_parameters_from_context would leave a field unset. Replay
    // reads primaries/transfer/matrix/range/chroma from this metadata.
    stream->codecpar->format = AV_PIX_FMT_YUV420P;
    stream->codecpar->color_primaries = AVCOL_PRI_BT709;
    stream->codecpar->color_trc = AVCOL_TRC_BT709;
    stream->codecpar->color_space = AVCOL_SPC_BT709;
    stream->codecpar->color_range = AVCOL_RANGE_MPEG;
    stream->codecpar->chroma_location = AVCHROMA_LOC_LEFT;
    OutputGuard output{std::filesystem::path(outputPath), format.get()};
    if (avio_open(&format->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        throw MediaCodecError(options.codec, "output open failed: " + outputPath);
    }
    output.fileWritten = true;
    if (avformat_write_header(format.get(), nullptr) < 0) {
        throw MediaCodecError(options.codec, "header write failed");
    }
    stats.muxFinalizationMs += elapsed(muxStart);
    auto allocationStart = Clock::now();

    PacketPtr packet(av_packet_alloc());
    if (packet == nullptr) {
        throw MediaCodecError(options.codec, "packet allocation failed");
    }

    std::vector<uint8_t> planes(static_cast<size_t>(width) * height * 3 / 2);
    stats.allocationPackingMs += elapsed(allocationStart);
    int encodedCount = 0;

    // Drains finished packets. A receive error or a write failure throws:
    // a swallowed drain error is exactly the partial-success report the
    // corrected contracts forbid.
    const auto drain = [&]() {
        while (true) {
            const auto receiveStart = Clock::now();
            const int receiveStatus = avcodec_receive_packet(codec.get(), packet.get());
            stats.submissionDrainMs += elapsed(receiveStart);
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                return;
            }
            if (receiveStatus < 0) {
                throw MediaCodecError(options.codec, "packet receive failed: " + avError(receiveStatus));
            }
            muxStart = Clock::now();
            av_packet_rescale_ts(packet.get(), AVRational{1, 24}, stream->time_base);
            packet->stream_index = stream->index;
            // movenc derives the last sample's duration from packet
            // duration; leaving it 0 collapses a single-frame chunk into a
            // zero-length edit list the demuxer then drops entirely.
            packet->duration = av_rescale_q(1, AVRational{1, 24}, stream->time_base);
            stats.encodedBytes += packet->size;
            injection.check(EncodeFailure::Stage::Write);
            const int writeStatus = av_interleaved_write_frame(format.get(), packet.get());
            av_packet_unref(packet.get());
            stats.muxFinalizationMs += elapsed(muxStart);
            if (writeStatus < 0) {
                throw MediaCodecError(options.codec, "packet write failed: " + avError(writeStatus));
            }
        }
    };

    for (const CpuImage& display : displayReferredFrames) {
        const auto conversionStart = Clock::now();
        yuv420pFromDisplayReferred(display, planes);
        stats.conversionMs += elapsed(conversionStart);
        allocationStart = Clock::now();
        FramePtr cpuFrame(makeYuv420pFrame(width, height, planes));
        if (cpuFrame == nullptr)
            throw MediaCodecError(options.codec, "frame buffer allocation failed");
        FramePtr deviceFrame;
        AVFrame* source = cpuFrame.get();
        if (hardware) {
            deviceFrame.reset(av_frame_alloc());
            if (deviceFrame == nullptr)
                throw MediaCodecError(options.codec, "device frame allocation failed");
            deviceFrame->format = AV_PIX_FMT_CUDA;
            deviceFrame->width = width;
            deviceFrame->height = height;
            if (av_hwframe_get_buffer(hwFrames.get(), deviceFrame.get(), 0) < 0)
                throw MediaCodecError(options.codec, "device frame allocation failed");
        }
        stats.allocationPackingMs += elapsed(allocationStart);
        if (hardware) {
            const auto transferStart = Clock::now();
            const int status = av_hwframe_transfer_data(deviceFrame.get(), cpuFrame.get(), 0);
            stats.hostToDeviceMs += elapsed(transferStart);
            if (status < 0)
                throw MediaCodecError(options.codec, "device frame upload failed: " + avError(status));
            // FFmpeg's CUDA transfer copies min(src,dst pitch) bytes per
            // row, not just the active pixels. It submits asynchronous
            // copies on H2D; this timer does not isolate DMA completion.
            for (int plane = 0; plane < 3; ++plane) {
                const auto rowBytes = std::min(cpuFrame->linesize[plane], deviceFrame->linesize[plane]);
                stats.hostToDeviceBytes += static_cast<uint64_t>(rowBytes) * (plane == 0 ? height : height / 2);
            }
            source = deviceFrame.get();
        }
        source->pts = encodedCount++;
        injection.check(EncodeFailure::Stage::Submission);
        const auto sendStart = Clock::now();
        const int sendStatus = avcodec_send_frame(codec.get(), source);
        stats.submissionDrainMs += elapsed(sendStart);
        if (sendStatus < 0)
            throw MediaCodecError(options.codec, "frame submission failed: " + avError(sendStatus));
        drain();
    }
    const auto flushStart = Clock::now();
    const int flushStatus = avcodec_send_frame(codec.get(), nullptr);
    stats.submissionDrainMs += elapsed(flushStart);
    if (flushStatus < 0 && flushStatus != AVERROR_EOF) {
        throw MediaCodecError(options.codec, "encoder flush failed: " + avError(flushStatus));
    }
    drain();

    // Finalization: the trailer and the close are checked — a failure
    // here is an error, never a silent success (the output guard then
    // removes the unusable file).
    muxStart = Clock::now();
    injection.check(EncodeFailure::Stage::Finalization);
    const int trailerStatus = av_write_trailer(format.get());
    if (trailerStatus < 0) {
        throw MediaCodecError(options.codec, "trailer write failed: " + avError(trailerStatus));
    }
    if (avio_closep(&format->pb) < 0) {
        throw MediaCodecError(options.codec, "output close failed");
    }
    output.success = true;
    stats.muxFinalizationMs += elapsed(muxStart);

    stats.encodedFrames = encodedCount;
    stats.completeChunkMs = elapsed(completeStart);
    return stats;
}

}  // namespace nemo::media
