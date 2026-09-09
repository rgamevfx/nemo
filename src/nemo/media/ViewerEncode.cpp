#include "nemo/media/ViewerEncode.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "nemo/gpu/ViewerEncodeInterop.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
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
    planes.resize(static_cast<std::size_t>(width) * height * 3 / 2);
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

struct SessionState {
    CodecContextPtr codec;
    BufferRefPtr hardwareDevice;
    std::string encoderName;
    std::string profile;
    int width = 0;
    int height = 0;
    int gopSize = 0;
    int bitrateKbps = 0;
    std::int64_t nextPts = 0;

    [[nodiscard]] bool matches(const std::string& name, const std::string& wantedProfile, int frameWidth,
                               int frameHeight, int gop, int bitrate) const {
        return codec != nullptr && encoderName == name && profile == wantedProfile && width == frameWidth &&
               height == frameHeight && gopSize == gop && bitrateKbps == bitrate;
    }

    void resetCodec() {
        codec.reset();
        nextPts = 0;
        encoderName.clear();
        profile.clear();
        width = height = gopSize = bitrateKbps = 0;
    }
};

struct ResolvedEncoder {
    const AVCodec* codec = nullptr;
    bool hardware = false;
    bool hevc = false;
    std::string profile;
};

[[nodiscard]] ResolvedEncoder resolveEncoder(const EncodeOptions& options) {
    ResolvedEncoder resolved;
    for (const auto& entry : kEncoders) {
        if (options.codec == entry.id) {
            resolved.hardware = entry.hardware;
            resolved.codec = avcodec_find_encoder_by_name(entry.avcodecName);
            if (resolved.codec == nullptr) {
                throw MediaCodecError(options.codec, "encoder not registered in this libavcodec build");
            }
            break;
        }
    }
    if (resolved.codec == nullptr) {
        throw MediaCodecError(options.codec, "unknown encoder id (probe with probe-media)");
    }
    resolved.hevc = resolved.codec->id == AV_CODEC_ID_HEVC;
    resolved.profile = options.profile.empty() ? (resolved.hevc ? "main" : "high") : options.profile;
    if (resolved.profile != (resolved.hevc ? "main" : "high")) {
        throw MediaCodecError(options.codec, "unsupported profile '" + resolved.profile +
                                                 "'; supported 8-bit profile is " + (resolved.hevc ? "main" : "high"));
    }
    return resolved;
}

[[nodiscard]] std::string deviceStagingReason(const EncodeOptions& options, const ResolvedEncoder& encoder) {
    if (!encoder.hardware) {
        return "capability probe: selected codec '" + options.codec +
               "' is software-only and accepts host YUV420P frames; compact staging is required";
    }
    bool acceptsCuda = false;
    std::string advertised;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(encoder.codec, index);
        if (config == nullptr)
            break;
        if (config->pix_fmt == AV_PIX_FMT_CUDA || config->device_type == AV_HWDEVICE_TYPE_CUDA)
            acceptsCuda = true;
        if (!advertised.empty())
            advertised += ", ";
        const char* pixelFormat = av_get_pix_fmt_name(config->pix_fmt);
        advertised += pixelFormat != nullptr ? pixelFormat : "unknown-pixfmt";
        advertised += "/";
        const char* deviceType = av_hwdevice_get_type_name(config->device_type);
        advertised += deviceType != nullptr ? deviceType : "unknown-device";
    }
    if (!acceptsCuda) {
        throw MediaCodecError(options.codec,
                              "encoder capability probe advertises no CUDA input for the requested hardware codec (" +
                                  advertised + ")");
    }
    return "capability probe: " + options.codec +
           " advertises CUDA input; if Vulkan->CUDA interop initialization fails, "
           "compact GPU-YUV staging crosses device->host before CUDA upload "
           "(advertised configs: " +
           advertised + ")";
}

[[nodiscard]] AVFrame* makeYuv420pFrame(int width, int height, const std::vector<std::uint8_t>& planes);

struct PreparedInput {
    std::vector<std::uint8_t> planes;
    FramePtr deviceFrame;
};

using FrameProvider = std::function<void(std::size_t, PreparedInput&, EncodeStats&, AVBufferRef*)>;
[[nodiscard]] EncodeStats encodeYuvChunk(const std::string& outputPath, int width, int height, std::size_t frameCount,
                                         const EncodeOptions& options, const FrameProvider& provide,
                                         SessionState* session, EncodeStats stats) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const auto completeStart = Clock::now();
    stats.codec = options.codec;
    if (frameCount == 0) {
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
    if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
        throw MediaCodecError(options.codec,
                              "viewer chunks encode as 4:2:0 left chroma; dimensions must be positive and even (got " +
                                  std::to_string(width) + "x" + std::to_string(height) + ")");
    }
    if (av_image_check_size(static_cast<unsigned>(width), static_cast<unsigned>(height), 0, nullptr) < 0) {
        throw MediaCodecError(options.codec, "frame dimensions are outside the encodable range (" +
                                                 std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    const ResolvedEncoder resolved = resolveEncoder(options);
    stats.profile = resolved.profile;
    FailureInjector injection(options);
    CodecContextPtr ownedCodec;
    BufferRefPtr ownedHwDevice;
    BufferRefPtr ownedHwFrames;
    AVCodecContext* codec = nullptr;
    const auto setupStart = Clock::now();
    try {
        if (session != nullptr &&
            session->matches(options.codec, resolved.profile, width, height, options.gopSize, options.bitrateKbps)) {
            codec = session->codec.get();
            // Compatible live sessions never enter terminal EOF between
            // chunks. Zero-delay encoding accounts every requested frame
            // before finalizing its independent container.
            stats.sessionReused = true;
            stats.sessionReuseCount = 1;
            stats.warmSetupMs = elapsed(setupStart);
        } else {
            if (session != nullptr)
                session->resetCodec();
            ownedCodec.reset(avcodec_alloc_context3(resolved.codec));
            if (ownedCodec == nullptr) {
                throw MediaCodecError(options.codec, "context allocation failed");
            }
            codec = ownedCodec.get();
            codec->width = width;
            codec->height = height;
            codec->time_base = AVRational{1, 24};
            codec->framerate = AVRational{24, 1};
            codec->gop_size = options.gopSize;
            codec->max_b_frames = 0;
            codec->bit_rate = static_cast<int64_t>(options.bitrateKbps) * 1000;
            codec->profile = resolved.hevc ? FF_PROFILE_HEVC_MAIN : FF_PROFILE_H264_HIGH;
            codec->thread_count = session ? 1 : 2;
            if (!resolved.hardware && resolved.hevc) {
                const int status = av_opt_set(codec->priv_data, "x265-params",
                                              session ? "pools=2:frame-threads=1" : "pools=2:frame-threads=2", 0);
                if (status < 0)
                    throw MediaCodecError(options.codec, "x265 thread configuration failed: " + avError(status));
            }
            if (session) {
                const auto set = [&](const char* name, const char* value) {
                    const int status = av_opt_set(codec->priv_data, name, value, 0);
                    if (status < 0)
                        throw MediaCodecError(options.codec, std::string("persistent encoder option ") + name + ": " +
                                                                 avError(status));
                };
                set("forced-idr", "1");
                set("tune", resolved.hardware ? "ull" : "zerolatency");
                if (resolved.hardware) {
                    set("delay", "0");
                    set("zerolatency", "1");
                    set("rc-lookahead", "0");
                }
            }
            codec->color_primaries = AVCOL_PRI_BT709;
            codec->color_trc = AVCOL_TRC_BT709;
            codec->colorspace = AVCOL_SPC_BT709;
            codec->color_range = AVCOL_RANGE_MPEG;
            codec->chroma_sample_location = AVCHROMA_LOC_LEFT;

            if (resolved.hardware) {
                const int idrStatus = av_opt_set(codec->priv_data, "forced-idr", "1", 0);
                if (idrStatus < 0)
                    throw MediaCodecError(options.codec,
                                          "independent chunk IDR configuration failed: " + avError(idrStatus));
                codec->pix_fmt = AV_PIX_FMT_CUDA;
                AVBufferRef* device = nullptr;
                if (session != nullptr && session->hardwareDevice != nullptr) {
                    device = av_buffer_ref(session->hardwareDevice.get());
                } else if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
                    throw MediaCodecError(options.codec,
                                          "CUDA hwdevice init failed (engine unavailable on this device)");
                }
                if (device == nullptr)
                    throw MediaCodecError(options.codec, "CUDA hwdevice reference failed");
                ownedHwDevice.reset(device);
                AVBufferRef* pool = av_hwframe_ctx_alloc(ownedHwDevice.get());
                if (pool == nullptr)
                    throw MediaCodecError(options.codec, "frame pool allocation failed");
                ownedHwFrames.reset(pool);
                AVHWFramesContext* frames = reinterpret_cast<AVHWFramesContext*>(ownedHwFrames->data);
                frames->format = AV_PIX_FMT_CUDA;
                frames->sw_format = AV_PIX_FMT_YUV420P;
                frames->width = width;
                frames->height = height;
                frames->initial_pool_size = 4;
                if (av_hwframe_ctx_init(ownedHwFrames.get()) < 0)
                    throw MediaCodecError(options.codec, "frame pool init failed");
                codec->hw_frames_ctx = av_buffer_ref(ownedHwFrames.get());
                if (codec->hw_frames_ctx == nullptr)
                    throw MediaCodecError(options.codec, "frame pool reference failed");
            } else {
                codec->pix_fmt = AV_PIX_FMT_YUV420P;
            }
            injection.check(EncodeFailure::Stage::Allocation);
            const int openStatus = avcodec_open2(codec, resolved.codec, nullptr);
            if (openStatus < 0)
                throw MediaCodecError(options.codec, "encoder open failed: " + avError(openStatus));
            injection.check(EncodeFailure::Stage::Init);
            stats.coldSetupMs = elapsed(setupStart);
            if (session != nullptr) {
                session->encoderName = options.codec;
                session->profile = resolved.profile;
                session->width = width;
                session->height = height;
                session->gopSize = options.gopSize;
                session->bitrateKbps = options.bitrateKbps;
                if (ownedHwDevice != nullptr) {
                    AVBufferRef* retained = av_buffer_ref(ownedHwDevice.get());
                    if (retained == nullptr)
                        throw MediaCodecError(options.codec, "CUDA hwdevice retention failed");
                    session->hardwareDevice.reset(retained);
                }
                session->codec = std::move(ownedCodec);
            }
        }
        stats.initializationMs = elapsed(completeStart);
        stats.sessionChunkCount = 1;
        const std::int64_t chunkStartPts = session ? session->nextPts : 0;

        FormatContextPtr format;
        AVFormatContext* raw = nullptr;
        if (avformat_alloc_output_context2(&raw, nullptr, nullptr, outputPath.c_str()) < 0 || raw == nullptr)
            throw MediaCodecError(options.codec, "output context allocation failed");
        format.reset(raw);
        AVStream* stream = avformat_new_stream(format.get(), nullptr);
        if (stream == nullptr || avcodec_parameters_from_context(stream->codecpar, codec) != 0)
            throw MediaCodecError(options.codec, "stream setup failed");
        stream->codecpar->format = AV_PIX_FMT_YUV420P;
        stream->codecpar->color_primaries = AVCOL_PRI_BT709;
        stream->codecpar->color_trc = AVCOL_TRC_BT709;
        stream->codecpar->color_space = AVCOL_SPC_BT709;
        stream->codecpar->color_range = AVCOL_RANGE_MPEG;
        stream->codecpar->chroma_location = AVCHROMA_LOC_LEFT;
        OutputGuard output{std::filesystem::path(outputPath), format.get()};
        const auto muxStart = Clock::now();
        if (avio_open(&format->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0)
            throw MediaCodecError(options.codec, "output open failed: " + outputPath);
        output.fileWritten = true;
        if (avformat_write_header(format.get(), nullptr) < 0)
            throw MediaCodecError(options.codec, "header write failed");
        stats.muxFinalizationMs += elapsed(muxStart);
        PacketPtr packet(av_packet_alloc());
        if (packet == nullptr)
            throw MediaCodecError(options.codec, "packet allocation failed");
        PreparedInput input;
        int submittedFrames = 0;
        int drainedPackets = 0;
        bool firstPacketKey = false;
        const std::size_t expectedBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2;
        input.planes.reserve(expectedBytes);
        auto drain = [&]() {
            while (true) {
                const auto receiveStart = Clock::now();
                const int receiveStatus = avcodec_receive_packet(codec, packet.get());
                stats.submissionDrainMs += elapsed(receiveStart);
                if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF)
                    return;
                if (receiveStatus < 0)
                    throw MediaCodecError(options.codec, "packet receive failed: " + avError(receiveStatus));
                ++drainedPackets;
                if (drainedPackets == 1)
                    firstPacketKey = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                if (packet->pts != chunkStartPts + drainedPackets - 1)
                    throw MediaCodecError(options.codec, "encoded packet does not cover the requested chunk frame");
                packet->pts -= chunkStartPts;
                if (packet->dts != AV_NOPTS_VALUE)
                    packet->dts -= chunkStartPts;
                const auto packetStart = Clock::now();
                av_packet_rescale_ts(packet.get(), AVRational{1, 24}, stream->time_base);
                packet->stream_index = stream->index;
                packet->duration = av_rescale_q(1, AVRational{1, 24}, stream->time_base);
                stats.encodedBytes += packet->size;
                injection.check(EncodeFailure::Stage::Write);
                const int writeStatus = av_interleaved_write_frame(format.get(), packet.get());
                av_packet_unref(packet.get());
                stats.muxFinalizationMs += elapsed(packetStart);
                if (writeStatus < 0)
                    throw MediaCodecError(options.codec, "packet write failed: " + avError(writeStatus));
            }
        };
        for (std::size_t index = 0; index < frameCount; ++index) {
            input.planes.clear();
            input.deviceFrame.reset();
            provide(index, input, stats, resolved.hardware ? codec->hw_frames_ctx : nullptr);
            const auto allocationStart = Clock::now();
            FramePtr cpuFrame;
            AVFrame* source = input.deviceFrame.get();
            if (source == nullptr) {
                if (input.planes.size() != expectedBytes)
                    throw MediaCodecError(options.codec, "YUV provider returned an invalid compact 4:2:0 staging size");
                cpuFrame.reset(makeYuv420pFrame(width, height, input.planes));
                if (cpuFrame == nullptr)
                    throw MediaCodecError(options.codec, "frame buffer allocation failed");
                source = cpuFrame.get();
            } else if (!input.planes.empty()) {
                throw MediaCodecError(options.codec, "device YUV provider returned both host and device frames");
            }
            if (resolved.hardware && input.deviceFrame == nullptr)
                throw MediaCodecError(options.codec, "hardware YUV provider returned no device frame");
            stats.allocationPackingMs += elapsed(allocationStart);
            source->pts = chunkStartPts + static_cast<int64_t>(index);
            if (index == 0) {
                source->pict_type = AV_PICTURE_TYPE_I;
            }
            injection.check(EncodeFailure::Stage::Submission);
            const auto sendStart = Clock::now();
            const int sendStatus = avcodec_send_frame(codec, source);
            stats.submissionDrainMs += elapsed(sendStart);
            if (sendStatus < 0)
                throw MediaCodecError(options.codec, "frame submission failed: " + avError(sendStatus));
            ++submittedFrames;
            drain();
        }
        if (!session) {
            const auto flushStart = Clock::now();
            const int flushStatus = avcodec_send_frame(codec, nullptr);
            stats.submissionDrainMs += elapsed(flushStart);
            if (flushStatus < 0 && flushStatus != AVERROR_EOF)
                throw MediaCodecError(options.codec, "encoder flush failed: " + avError(flushStatus));
            drain();
        }
        if (submittedFrames != static_cast<int>(frameCount) || drainedPackets != submittedFrames || !firstPacketKey)
            throw MediaCodecError(options.codec,
                                  "encoder drained incomplete frames or lacked a keyframe at chunk start");

        const auto trailerStart = Clock::now();
        injection.check(EncodeFailure::Stage::Finalization);
        if (av_write_trailer(format.get()) < 0)
            throw MediaCodecError(options.codec, "trailer write failed");
        if (avio_closep(&format->pb) < 0)
            throw MediaCodecError(options.codec, "output close failed");
        output.success = true;
        stats.muxFinalizationMs += elapsed(trailerStart);
        stats.encodedFrames = drainedPackets;
        stats.completeChunkMs = elapsed(completeStart);

        if (session)
            session->nextPts += submittedFrames;
        return stats;
    } catch (...) {
        if (session != nullptr)
            session->resetCodec();
        throw;
    }
}

void addViewerGpuStats(const gpu::ViewerEncodeStats& source, EncodeStats& target, bool accumulateStaging) {
    target.allocationPackingMs += source.allocationPackingMs;
    target.gpuConversionMs += source.gpuConversionMs;
    target.hostToDeviceMs += source.hostToDeviceMs;
    target.hostToDeviceBytes += source.hostToDeviceBytes;
    target.deviceToDeviceMs += source.deviceToDeviceMs;
    target.deviceToDeviceBytes += source.deviceToDeviceBytes;
    target.deviceToHostMs += source.deviceToHostMs;
    target.deviceToHostBytes += source.deviceToHostBytes;
    if (accumulateStaging)
        target.stagingBytes += source.stagingBytes;
    else
        target.stagingBytes = std::max(target.stagingBytes, source.stagingBytes);
}

}  // namespace

EncodeStats encodeViewerChunk(const std::string& outputPath, std::span<const CpuImage> displayReferredFrames,
                              const EncodeOptions& options) {
    if (displayReferredFrames.empty())
        throw MediaCodecError(options.codec, "no frames to encode");
    const int width = displayReferredFrames.front().width();
    const int height = displayReferredFrames.front().height();
    if (width % 2 != 0 || height % 2 != 0) {
        throw MediaCodecError(options.codec,
                              "viewer chunks encode as 4:2:0 left chroma; dimensions must be even (got " +
                                  std::to_string(width) + "x" + std::to_string(height) + ")");
    }
    for (std::size_t index = 0; index < displayReferredFrames.size(); ++index) {
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
    const auto provider = [&](std::size_t index, PreparedInput& input, EncodeStats& stats, AVBufferRef*) {
        const auto start = std::chrono::steady_clock::now();
        yuv420pFromDisplayReferred(displayReferredFrames[index], input.planes);
        stats.conversionMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    return encodeYuvChunk(outputPath, width, height, displayReferredFrames.size(), options, provider, nullptr, {});
}

struct ViewerChunkEncoder::Impl {
    gpu::Instance* instance = nullptr;
    gpu::Device* device = nullptr;
    gpu::Allocator* allocator = nullptr;
    EncodeOptions options;
    std::optional<EncodeFailure> failure;
    SessionState session;
    // Declared after the codec session so its destructor drains retained
    // CUDA/Vulkan frames before the session's hardware context is released.
    std::unique_ptr<gpu::ViewerEncodeInterop> adapter;
};

ViewerChunkEncoder::ViewerChunkEncoder(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                       const EncodeOptions& options)
    : impl_(std::make_unique<Impl>()) {
    impl_->instance = &instance;
    impl_->device = &device;
    impl_->allocator = &allocator;
    impl_->options = options;
    if (options.injectedFailure) {
        impl_->failure = *options.injectedFailure;
        impl_->options.injectedFailure = &*impl_->failure;
    }
}

ViewerChunkEncoder::~ViewerChunkEncoder() = default;

EncodeStats ViewerChunkEncoder::encode(const std::string& outputPath,
                                       std::span<const DeviceViewerFrame> displayReferredFrames) {
    const auto chunkStart = std::chrono::steady_clock::now();
    if (displayReferredFrames.empty())
        throw MediaCodecError(impl_->options.codec, "no frames to encode");
    const DeviceViewerFrame& first = displayReferredFrames.front();
    if (first.image == nullptr)
        throw MediaCodecError(impl_->options.codec, "device frame has no image");
    const int width = first.layout.width;
    const int height = first.layout.height;
    if (first.layout.color != ColorInterpretation::DisplayReferred)
        throw MediaCodecError(impl_->options.codec, "device frame is scene-linear; apply the viewing transform first");
    if (first.layout.precision != Precision::Float32 || first.image->format() != VK_FORMAT_R32G32B32A32_SFLOAT)
        throw MediaCodecError(impl_->options.codec, "device frame must be a completed RGBA32F image");
    if (width <= 0 || height <= 0)
        throw MediaCodecError(impl_->options.codec, "device viewer frames require positive dimensions");
    const auto extent = first.image->extent();
    if (extent.width != static_cast<std::uint32_t>(width) || extent.height != static_cast<std::uint32_t>(height))
        throw MediaCodecError(impl_->options.codec, "device image extent does not match its declared ImageLayout");
    for (std::size_t index = 0; index < displayReferredFrames.size(); ++index) {
        const DeviceViewerFrame& frame = displayReferredFrames[index];
        if (frame.image == nullptr)
            throw MediaCodecError(impl_->options.codec, "device frame " + std::to_string(index) + " has no image");
        if (frame.layout != first.layout || frame.image->format() != VK_FORMAT_R32G32B32A32_SFLOAT)
            throw MediaCodecError(impl_->options.codec, "device frame " + std::to_string(index) +
                                                            " does not match the first frame representation");
        const auto frameExtent = frame.image->extent();
        if (frameExtent.width != static_cast<std::uint32_t>(width) ||
            frameExtent.height != static_cast<std::uint32_t>(height))
            throw MediaCodecError(impl_->options.codec, "device frame " + std::to_string(index) +
                                                            " image extent does not match its ImageLayout");
    }
    // 4:2:0 storage needs even extents. GPU conversion edge-pads the image;
    // the cache index retains the exact requested extent for replay cropping.
    const int encodedWidth = (width + 1) & ~1;
    const int encodedHeight = (height + 1) & ~1;
    const ResolvedEncoder resolved = resolveEncoder(impl_->options);
    if (!impl_->adapter)
        impl_->adapter =
            std::make_unique<gpu::ViewerEncodeInterop>(*impl_->instance, *impl_->device, *impl_->allocator);
    try {
        impl_->adapter->prepare();
    } catch (const gpu::ViewerEncodeError& error) {
        throw MediaCodecError(impl_->options.codec, error.what());
    }
    bool directDeviceInterop = false;
    std::string bridgeReason;
    if (resolved.hardware) {
        try {
            directDeviceInterop = impl_->adapter->ensureDirectInterop(encodedWidth, encodedHeight, bridgeReason);
        } catch (const gpu::ViewerEncodeError& error) {
            throw MediaCodecError(impl_->options.codec, error.what());
        }
    }
    EncodeStats seed;
    if (directDeviceInterop) {
        seed.fallbackReason.clear();
    } else if (resolved.hardware) {
        seed.fallbackReason = "Vulkan-to-CUDA capability probe failed: " + bridgeReason + "; " +
                              deviceStagingReason(impl_->options, resolved);
    } else {
        seed.fallbackReason = deviceStagingReason(impl_->options, resolved);
    }
    const auto provider = [&](std::size_t index, PreparedInput& input, EncodeStats& stats, AVBufferRef* cudaFrames) {
        gpu::ViewerEncodeStats gpuStats;
        try {
            if (directDeviceInterop) {
                input.deviceFrame.reset(av_frame_alloc());
                if (input.deviceFrame == nullptr)
                    throw MediaCodecError(impl_->options.codec, "device frame allocation failed");
                impl_->adapter->convertToCuda(*displayReferredFrames[index].image, width, height, encodedWidth,
                                              encodedHeight, cudaFrames, input.deviceFrame.get(), gpuStats);
            } else {
                impl_->adapter->convertToHost(*displayReferredFrames[index].image, width, height, encodedWidth,
                                              encodedHeight, input.planes, gpuStats);
                if (resolved.hardware) {
                    input.deviceFrame.reset(av_frame_alloc());
                    if (input.deviceFrame == nullptr)
                        throw MediaCodecError(impl_->options.codec, "device frame allocation failed");
                    impl_->adapter->uploadHostToCuda(input.planes, encodedWidth, encodedHeight, cudaFrames,
                                                     input.deviceFrame.get(), gpuStats);
                    input.planes.clear();
                }
            }
        } catch (const MediaCodecError&) {
            throw;
        } catch (const gpu::ViewerEncodeError& error) {
            throw MediaCodecError(impl_->options.codec, error.what());
        }
        addViewerGpuStats(gpuStats, stats, directDeviceInterop);
    };
    const double preparationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - chunkStart).count();
    try {
        EncodeStats result = encodeYuvChunk(outputPath, encodedWidth, encodedHeight, displayReferredFrames.size(),
                                            impl_->options, provider, &impl_->session, seed);
        if (directDeviceInterop)
            impl_->adapter->finishChunk();
        result.initializationMs += preparationMs;
        if (result.sessionReused)
            result.warmSetupMs += preparationMs;
        else
            result.coldSetupMs += preparationMs;
        result.completeChunkMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - chunkStart).count();
        return result;
    } catch (...) {
        if (directDeviceInterop)
            impl_->adapter->abortChunk();
        throw;
    }
}

}  // namespace nemo::media
