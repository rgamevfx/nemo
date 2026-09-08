#include "nemo/media/ViewerEncode.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
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

}  // namespace

std::vector<std::uint8_t> yuv420pFromDisplayReferred(const CpuImage& image) {
    const int width = image.width();
    const int height = image.height();
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    std::vector<std::uint8_t> planes(static_cast<size_t>(width) * height +
                                     2 * static_cast<size_t>(chromaWidth) * chromaHeight);
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
        cbv = 128.0F + 224.0F * (-0.11457F * r - 0.38543F * g + 0.5F * b);
        crv = 128.0F + 224.0F * (0.5F * r - 0.41869F * g - 0.08131F * b);
    };
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const auto rgba = image.pixel(col, row);
            float yv = 0, cbv = 0, crv = 0;
            encode709(rgba[0], rgba[1], rgba[2], yv, cbv, crv);
            y[row * width + col] = static_cast<uint8_t>(std::clamp(yv, 0.0F, 255.0F));
            if (row % 2 == 0 && col % 2 == 0) {
                // Chroma averaged over the co-sited 2x2 block.
                const auto mean = [&](int channel) {
                    float sum = 0.0F;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const auto sample =
                                image.pixel(std::min(col + dx, width - 1), std::min(row + dy, height - 1));
                            sum += std::clamp(sample[channel], 0.0F, 1.0F);
                        }
                    }
                    return sum / 4.0F;
                };
                float yv2 = 0, cbv2 = 0, crv2 = 0;
                encode709(mean(0), mean(1), mean(2), yv2, cbv2, crv2);
                cb[(row / 2) * chromaWidth + (col / 2)] = static_cast<uint8_t>(std::clamp(cbv2, 0.0F, 255.0F));
                cr[(row / 2) * chromaWidth + (col / 2)] = static_cast<uint8_t>(std::clamp(crv2, 0.0F, 255.0F));
            }
        }
    }
    return planes;
}

EncodeStats encodeViewerChunk(const std::string& outputPath, const std::vector<CpuImage>& displayReferredFrames,
                              const EncodeOptions& options) {
    if (displayReferredFrames.empty()) {
        throw MediaCodecError(options.codec, "no frames to encode");
    }
    const int width = displayReferredFrames.front().width();
    const int height = displayReferredFrames.front().height();

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

    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrames = nullptr;
    auto releaseHw = [&]() {
        if (hwFrames != nullptr) {
            av_buffer_unref(&hwFrames);
        }
        if (hwDevice != nullptr) {
            av_buffer_unref(&hwDevice);
        }
    };

    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    if (codec == nullptr) {
        throw MediaCodecError(options.codec, "context allocation failed");
    }
    codec->width = width;
    codec->height = height;
    codec->time_base = AVRational{1, 24};
    codec->framerate = AVRational{24, 1};
    codec->gop_size = options.gopSize;
    codec->bit_rate = static_cast<int64_t>(options.bitrateKbps) * 1000;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_range = AVCOL_RANGE_MPEG;

    if (hardware) {
        // NVENC consumes CUDA frames: upload measured, not hidden.
        codec->pix_fmt = AV_PIX_FMT_CUDA;
        if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            avcodec_free_context(&codec);
            releaseHw();
            throw MediaCodecError(options.codec, "CUDA hwdevice init failed (engine unavailable on this device)");
        }
        hwFrames = av_hwframe_ctx_alloc(hwDevice);
        if (hwFrames == nullptr) {
            avcodec_free_context(&codec);
            releaseHw();
            throw MediaCodecError(options.codec, "frame pool allocation failed");
        }
        AVHWFramesContext* frames = reinterpret_cast<AVHWFramesContext*>(hwFrames->data);
        frames->format = AV_PIX_FMT_CUDA;
        frames->sw_format = AV_PIX_FMT_YUV420P;
        frames->width = width;
        frames->height = height;
        frames->initial_pool_size = 4;
        if (av_hwframe_ctx_init(hwFrames) < 0) {
            avcodec_free_context(&codec);
            releaseHw();
            throw MediaCodecError(options.codec, "frame pool init failed");
        }
        codec->hw_frames_ctx = av_buffer_ref(hwFrames);
    } else {
        codec->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    const int openStatus = avcodec_open2(codec, encoder, nullptr);
    if (openStatus < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(openStatus, error, sizeof(error));
        avcodec_free_context(&codec);
        releaseHw();
        throw MediaCodecError(options.codec, std::string("encoder open failed: ") + error);
    }

    // Muxer.
    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, nullptr, outputPath.c_str()) < 0 || format == nullptr) {
        avcodec_free_context(&codec);
        releaseHw();
        throw MediaCodecError(options.codec, "output context allocation failed");
    }
    AVStream* stream = avformat_new_stream(format, nullptr);
    if (stream == nullptr || avcodec_parameters_from_context(stream->codecpar, codec) != 0) {
        avcodec_free_context(&codec);
        releaseHw();
        throw MediaCodecError(options.codec, "stream setup failed");
    }
    stream->time_base = AVRational{1, 24};
    if (avio_open(&format->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        avcodec_free_context(&codec);
        releaseHw();
        throw MediaCodecError(options.codec, "output open failed: " + outputPath);
    }
    if (avformat_write_header(format, nullptr) < 0) {
        avcodec_free_context(&codec);
        releaseHw();
        throw MediaCodecError(options.codec, "header write failed");
    }

    EncodeStats stats;
    stats.codec = options.codec;
    const auto timerStart = std::chrono::steady_clock::now();
    double uploadNsTotal = 0.0;
    int encodedCount = 0;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&]() {
        while (avcodec_receive_packet(codec, packet) == 0) {
            av_packet_rescale_ts(packet, AVRational{1, 24}, stream->time_base);
            packet->stream_index = stream->index;
            stats.encodedBytes += packet->size;
            if (av_interleaved_write_frame(format, packet) != 0) {
                av_packet_free(&packet);
                av_frame_free(&frame);
                avcodec_free_context(&codec);
                releaseHw();
                throw MediaCodecError(options.codec, "packet write failed");
            }
            av_packet_unref(packet);
        }
    };

    for (const CpuImage& display : displayReferredFrames) {
        AVFrame* source = nullptr;
        if (hardware) {
            // Measured capability-dependent transfer: CPU staging into
            // device-resident encode frames.
            const auto uploadStart = std::chrono::steady_clock::now();
            AVFrame* deviceFrame = av_frame_alloc();
            deviceFrame->format = AV_PIX_FMT_CUDA;
            deviceFrame->width = width;
            deviceFrame->height = height;
            if (av_hwframe_get_buffer(hwFrames, deviceFrame, 0) < 0) {
                av_frame_free(&deviceFrame);
                throw MediaCodecError(options.codec, "device frame allocation failed");
            }
            const std::vector<uint8_t> planes = yuv420pFromDisplayReferred(display);
            AVFrame* cpuFrame = av_frame_alloc();
            cpuFrame->format = AV_PIX_FMT_YUV420P;
            cpuFrame->width = width;
            cpuFrame->height = height;
            if (av_frame_get_buffer(cpuFrame, 0) < 0) {
                av_frame_free(&cpuFrame);
                av_frame_free(&deviceFrame);
                throw MediaCodecError(options.codec, "frame buffer allocation failed");
            }
            const uint8_t* y = planes.data();
            const uint8_t* cb = y + static_cast<size_t>(width) * height;
            const uint8_t* cr = cb + static_cast<size_t>(width / 2) * (height / 2);
            for (int row = 0; row < height; ++row) {
                std::memcpy(cpuFrame->data[0] + row * cpuFrame->linesize[0], y + row * width, width);
            }
            for (int row = 0; row < height / 2; ++row) {
                std::memcpy(cpuFrame->data[1] + row * cpuFrame->linesize[1], cb + row * (width / 2), width / 2);
                std::memcpy(cpuFrame->data[2] + row * cpuFrame->linesize[2], cr + row * (width / 2), width / 2);
            }
            if (av_hwframe_transfer_data(deviceFrame, cpuFrame, 0) < 0) {
                av_frame_free(&cpuFrame);
                av_frame_free(&deviceFrame);
                throw MediaCodecError(options.codec, "device frame upload failed");
            }
            uploadNsTotal +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - uploadStart)
                    .count();
            av_frame_free(&cpuFrame);
            deviceFrame->pts = encodedCount;
            source = deviceFrame;
        } else {
            AVFrame* cpu = av_frame_alloc();
            cpu->format = AV_PIX_FMT_YUV420P;
            cpu->width = width;
            cpu->height = height;
            if (av_frame_get_buffer(cpu, 0) < 0) {
                av_frame_free(&cpu);
                throw MediaCodecError(options.codec, "frame buffer allocation failed");
            }
            const std::vector<uint8_t> planes = yuv420pFromDisplayReferred(display);
            const uint8_t* y = planes.data();
            const uint8_t* cb = y + static_cast<size_t>(width) * height;
            const uint8_t* cr = cb + static_cast<size_t>(width / 2) * (height / 2);
            for (int row = 0; row < height; ++row) {
                std::memcpy(cpu->data[0] + row * cpu->linesize[0], y + row * width, width);
            }
            for (int row = 0; row < height / 2; ++row) {
                std::memcpy(cpu->data[1] + row * cpu->linesize[1], cb + row * (width / 2), width / 2);
                std::memcpy(cpu->data[2] + row * cpu->linesize[2], cr + row * (width / 2), width / 2);
            }
            cpu->pts = encodedCount;
            source = cpu;
        }
        ++encodedCount;
        if (avcodec_send_frame(codec, source) != 0) {
            av_frame_free(&source);
            throw MediaCodecError(options.codec, "frame submission failed");
        }
        av_frame_free(&source);
        drain();
    }
    if (avcodec_send_frame(codec, nullptr) == 0) {
        drain();
    }
    const double encodeNsTotal =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - timerStart).count();
    av_write_trailer(format);
    avio_closep(&format->pb);
    avformat_free_context(format);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    releaseHw();

    stats.encodedFrames = encodedCount;
    stats.encodeMsPerFrame = encodedCount > 0 ? encodeNsTotal / 1e6 / encodedCount : 0.0;
    stats.uploadNsPerFrame = encodedCount > 0 ? uploadNsTotal / encodedCount : 0.0;
    return stats;
}

}  // namespace nemo::media
