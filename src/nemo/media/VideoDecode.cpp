#include "nemo/media/VideoDecode.hpp"

#include <cstring>
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
#include <libswscale/swscale.h>
}

namespace nemo::media {

namespace {

// FFmpeg AV* objects stay in RAII guards; no library types leak further.
struct FormatGuard {
    AVFormatContext* context = nullptr;
    ~FormatGuard() {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct PacketGuard {
    AVPacket* packet = nullptr;
    PacketGuard() : packet(av_packet_alloc()) {}
    ~PacketGuard() { av_packet_free(&packet); }
};

struct CodecContextGuard {
    AVCodecContext* context = nullptr;
    ~CodecContextGuard() { avcodec_free_context(&context); }
};

struct FrameGuard {
    AVFrame* frame = nullptr;
    FrameGuard() : frame(av_frame_alloc()) {}
    ~FrameGuard() { av_frame_free(&frame); }
};

[[noreturn]] void fail(const std::string& path, const std::string& what, int status = 0) {
    std::string detail;
    if (status < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(status, buffer, sizeof(buffer));
        detail = std::string(": ") + buffer;
    }
    throw std::runtime_error("media clip: " + path + ": " + what + detail);
}

// Manual YUV420p -> RGBA float32 conversion with BT.709 limited-range
// decode — byte-for-byte the same matrix/range formula the mediaConvert
// kernel declares, so the software reference path and the hardware interop
// path are comparable by construction (swscale would otherwise silently
// pick BT.601/full-range for untagged streams).
std::vector<float> yuv420pToRgba32f(const AVFrame* frame) {
    std::vector<float> pixels(static_cast<size_t>(frame->width) * frame->height * 4);
    const uint8_t* yPlane = frame->data[0];
    const uint8_t* uPlane = frame->data[1];
    const uint8_t* vPlane = frame->data[2];
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            const float yv = static_cast<float>(yPlane[y * frame->linesize[0] + x]);
            const int cx = x / 2;
            const int cy = y / 2;
            const float u = static_cast<float>(uPlane[cy * frame->linesize[1] + cx]);
            const float v = static_cast<float>(vPlane[cy * frame->linesize[2] + cx]);
            const float yy = (yv - 16.0F) * (255.0F / 219.0F) / 255.0F;
            const float uu = (u - 128.0F) * (255.0F / 224.0F) / 255.0F;
            const float vv = (v - 128.0F) * (255.0F / 224.0F) / 255.0F;
            const float r = yy + 1.5748F * vv;
            const float g = yy - 0.1873F * uu - 0.4681F * vv;
            const float b = yy + 1.8556F * uu;
            const size_t offset = (static_cast<size_t>(y) * frame->width + x) * 4;
            pixels[offset + 0] = r;
            pixels[offset + 1] = g;
            pixels[offset + 2] = b;
            pixels[offset + 3] = 1.0F;
        }
    }
    return pixels;
}

}  // namespace

struct ClipDecoder::Impl {
    FormatGuard format;
    int streamIndex = -1;
    AVCodecContext* codecContext = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrames = nullptr;
    std::unique_ptr<gpu::MediaInterop> interop;
    gpu::Device* device = nullptr;
    gpu::Allocator* allocator = nullptr;
    std::unique_ptr<gpu::SubmissionQueue> queue;
    ClipInfo info;
    DecodeDecision decision;
    bool opened = false;
    bool endOfStreamReached = false;
    int64_t decodedFrameCount = 0;
    int64_t framesDecodedHardware = 0;

    // Destruction order: the codec closes first (it frees its video
    // session and internal command pools on the device), then the frame
    // pool, then the device context. Reverse order leaves FFmpeg objects
    // alive across the device teardown and trips validation.
    ~Impl() {
        if (codecContext != nullptr) {
            avcodec_free_context(&codecContext);
        }
        if (hwFrames != nullptr) {
            av_buffer_unref(&hwFrames);
        }
        if (hwDevice != nullptr) {
            av_buffer_unref(&hwDevice);
        }
    }
};

ClipDecoder::~ClipDecoder() = default;

std::unique_ptr<ClipDecoder> ClipDecoder::open(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                               const std::string& path, const std::filesystem::path& convertSpirv) {
    auto decoder = std::unique_ptr<ClipDecoder>(new ClipDecoder());
    auto impl = std::make_unique<Impl>();
    decoder->impl_ = std::move(impl);

    FormatGuard format;
    const int openStatus = avformat_open_input(&format.context, path.c_str(), nullptr, nullptr);
    if (openStatus < 0) {
        fail(path, "avformat_open_input failed", openStatus);
    }
    if (avformat_find_stream_info(format.context, nullptr) < 0) {
        fail(path, "avformat_find_stream_info failed");
    }
    const int streamIndex = av_find_best_stream(format.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        fail(path, "no video stream");
    }
    AVStream* stream = format.context->streams[streamIndex];
    const AVCodec* decoderCodec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoderCodec == nullptr) {
        fail(path, "no decoder for codec id " + std::to_string(stream->codecpar->codec_id));
    }

    decoder->impl_->device = &device;
    decoder->impl_->allocator = &allocator;
    decoder->impl_->streamIndex = streamIndex;

    AVCodecContext* codec = avcodec_alloc_context3(decoderCodec);
    if (codec == nullptr) {
        fail(path, "avcodec_alloc_context3 failed");
    }
    if (avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
        fail(path, "avcodec_parameters_to_context failed");
    }
    codec->thread_count = 1;  // deterministic decode for image assertions

    // Capability-measured decode path selection (issue #10): Vulkan video
    // decode is used exactly when the device reserved a decode queue AND
    // libavcodec ships a Vulkan frame configuration for this codec.
    // Otherwise software decode with the precise recorded reason — no
    // silent substitution.
    bool hasVulkanConfig = false;
    for (int configIndex = 0;; ++configIndex) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoderCodec, configIndex);
        if (config == nullptr) {
            break;
        }
        if (config->pix_fmt == AV_PIX_FMT_VULKAN) {
            hasVulkanConfig = true;
            break;
        }
    }
    if (device.decode_family() && hasVulkanConfig) {
        // AVVulkanDeviceContext over the application device: decode runs on
        // the device's video decode queue; the produced frames are
        // Vulkan-resident planes on the application device, so interop
        // needs no external-memory bridge at all.
        AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (deviceRef == nullptr) {
            fail(path, "av_hwdevice_ctx_alloc (vulkan) failed");
        }
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
        vulkan->lock_queue = nullptr;
        vulkan->unlock_queue = nullptr;
        static thread_local std::vector<const char*> extensionNames;
        extensionNames.clear();
        for (const std::string& extension : device.enabled_extensions()) {
            extensionNames.push_back(extension.c_str());
        }
        vulkan->enabled_dev_extensions = extensionNames.data();
        vulkan->nb_enabled_dev_extensions = static_cast<int>(extensionNames.size());

        const int deviceInitStatus = av_hwdevice_ctx_init(deviceRef);
        if (deviceInitStatus < 0) {
            av_free(deviceRef);
            decoder->impl_->decision = {false, "Vulkan hwdevice init failed over the application device"};
        } else {
            decoder->impl_->hwDevice = deviceRef;
            AVBufferRef* framesRef = av_hwframe_ctx_alloc(decoder->impl_->hwDevice);
            if (framesRef == nullptr) {
                decoder->impl_->decision = {false, "Vulkan frame pool allocation failed"};
            }
            AVHWFramesContext* framesContext = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
            framesContext->format = AV_PIX_FMT_VULKAN;
            framesContext->sw_format = AV_PIX_FMT_NV12;
            framesContext->width = codec->width;
            framesContext->height = codec->height;
            framesContext->initial_pool_size = 4;
            AVVulkanFramesContext* vkFrames = static_cast<AVVulkanFramesContext*>(framesContext->hwctx);
            // Per-plane images (R8 luma + R8G8 chroma): the interop converter
            // samples single-plane views directly, no multiplane plumbing.
            // Explicit flag selection (NONE disables autodetect OR'ing).
            vkFrames->flags = static_cast<AVVkFrameFlags>(AV_VK_FRAME_FLAG_NONE | AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE);
            vkFrames->tiling = VK_IMAGE_TILING_OPTIMAL;
            // Sampling + transfer staging only; the decoder adds the
            // video-decode usage bits (with the required video-profile
            // list) when it initializes the session.
            vkFrames->usage = static_cast<VkImageUsageFlagBits>(
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            const int framesInitStatus = av_hwframe_ctx_init(framesRef);
            if (framesInitStatus < 0) {
                av_free(framesRef);
                decoder->impl_->decision = {false, "Vulkan frame pool init failed (decode surfaces unavailable)"};
            } else {
                decoder->impl_->hwFrames = framesRef;
            }
        }
    } else if (!device.decode_family()) {
        decoder->impl_->decision = {false, "device has no reserved Vulkan video decode queue family"};
    } else {
        decoder->impl_->decision = {false, "libavcodec build has no Vulkan hwaccel for this codec"};
    }

    if (decoder->impl_->hwFrames != nullptr) {
        codec->hw_device_ctx = av_buffer_ref(decoder->impl_->hwDevice);
        codec->hw_frames_ctx = av_buffer_ref(decoder->impl_->hwFrames);
        decoder->impl_->decision = {true, {}};
        // The interop converter consumes the device-resident planes.
        std::vector<std::uint32_t> spirv;
        FILE* file = fopen(convertSpirv.string().c_str(), "rb");
        if (file == nullptr) {
            fail(path,
                 "mediaConvert SPIR-V not found at " + convertSpirv.string() + " (build the nemo_shaders target)");
        }
        std::uint8_t header[4] = {0};
        if (std::fread(header, 1, 4, file) != 4 || std::memcmp(header, "\x03\x02\x23\x07", 4) != 0) {
            std::fclose(file);
            fail(path, convertSpirv.string() + " is not a SPIR-V module");
        }
        std::fseek(file, 0, SEEK_END);
        const long bytes = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (bytes < 0 || bytes % 4 != 0) {
            std::fclose(file);
            fail(path, convertSpirv.string() + " has invalid SPIR-V size");
        }
        spirv.resize(static_cast<size_t>(bytes) / 4);
        const size_t readBytes = std::fread(spirv.data(), 1, static_cast<size_t>(bytes), file);
        std::fclose(file);
        if (readBytes != static_cast<size_t>(bytes)) {
            fail(path, "short read of " + convertSpirv.string());
        }
        decoder->impl_->interop = gpu::MediaInterop::create(device, allocator, spirv);
    }

    // Prefer Vulkan frames only when our hardware pool is attached;
    // otherwise decode plainly (software) — never an unusable hwaccel.
    codec->get_format = [](AVCodecContext* context, const enum AVPixelFormat* formats)->enum AVPixelFormat {
        if (context->hw_frames_ctx != nullptr) {
            for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == AV_PIX_FMT_VULKAN) {
                    return AV_PIX_FMT_VULKAN;
                }
            }
        }
        return formats[0];
    };
    const int codecStatus = avcodec_open2(codec, decoderCodec, nullptr);
    if (codecStatus < 0) {
        fail(path, "avcodec_open2 failed", codecStatus);
    }

    decoder->impl_->codecContext = codec;
    codec = nullptr;  // impl owns it now
    decoder->impl_->format.context = format.context;
    format.context = nullptr;

    // Public clip metadata.
    ClipInfo& info = decoder->impl_->info;
    info.path = path;
    info.codecName = avcodec_get_name(decoder->impl_->codecContext->codec_id);
    info.width = decoder->impl_->codecContext->width;
    info.height = decoder->impl_->codecContext->height;
    const AVRational rate = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    info.frameRate = rate.num != 0 ? static_cast<double>(rate.num) / static_cast<double>(rate.den) : 0.0;
    info.frameCount = stream->nb_frames > 0 ? stream->nb_frames : -1;

    decoder->impl_->queue = std::make_unique<gpu::SubmissionQueue>(device, device.graphics_family());
    decoder->impl_->opened = true;
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
        fail(impl.info.path, "decoder is not open");
    }
    if (impl.endOfStreamReached) {
        return nullptr;
    }

    PacketGuard packet;
    FrameGuard frame;
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
            fail(impl.info.path, "avcodec_receive_frame failed", receiveStatus);
        }
        // Feed more input: read packets until the decoder accepts one (or
        // the stream ends, which flushes the decoder).
        while (true) {
            const int readStatus = av_read_frame(impl.format.context, packet.packet);
            if (readStatus == AVERROR_EOF) {
                const int sendStatus = avcodec_send_packet(impl.codecContext, nullptr);  // flush
                if (sendStatus < 0 && sendStatus != AVERROR_EOF) {
                    fail(impl.info.path, "avcodec_send_packet (flush) failed", sendStatus);
                }
                break;
            }
            if (readStatus < 0) {
                fail(impl.info.path, "av_read_frame failed", readStatus);
            }
            if (packet.packet->stream_index != impl.streamIndex) {
                continue;
            }
            const int sendStatus = avcodec_send_packet(impl.codecContext, packet.packet);
            if (sendStatus == 0 || sendStatus == AVERROR(EAGAIN) || sendStatus == AVERROR_EOF) {
                break;
            }
            fail(impl.info.path, "avcodec_send_packet failed", sendStatus);
        }
    }

    ++impl.decodedFrameCount;
    if (frame.frame->format == AV_PIX_FMT_VULKAN) {
        // Hardware path: frames are Vulkan-resident NV12 planes on the
        // application device; the conversion kernel keeps every pixel on
        // device. No CPU readback occurs here.
        auto* vkFrame = reinterpret_cast<AVVkFrame*>(frame.frame->data[0]);
        gpu::ForeignVideoFrame foreign;
        const VkFormat* planeFormats = av_vkfmt_from_pixfmt(AV_PIX_FMT_NV12);
        // FFmpeg 6.1 allocates NV12 as a single two-plane multiplane image
        // by default; honor the recorded form instead of assuming.
        foreign.multiplane = vkFrame->img[1] == VK_NULL_HANDLE;
        if (foreign.multiplane) {
            foreign.planeCount = 1;
            foreign.images[0] = vkFrame->img[0];
            foreign.formats[0] = planeFormats != nullptr ? planeFormats[0] : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        } else {
            foreign.planeCount = 2;
            foreign.images[0] = vkFrame->img[0];
            foreign.images[1] = vkFrame->img[1];
            foreign.formats[0] = planeFormats != nullptr ? planeFormats[0] : VK_FORMAT_R8_UNORM;
            foreign.formats[1] = planeFormats != nullptr && planeFormats[1] != VK_FORMAT_UNDEFINED
                                     ? planeFormats[1]
                                     : VK_FORMAT_R8G8_UNORM;
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
        try {
            impl.interop->convertToRgba32f(foreign, *output, timeout_ns);
        } catch (const std::exception& error) {
            throw;
        }

        // Hand the updated interop state back to FFmpeg for plane reuse.
        vkFrame->sem_value[0] = foreign.waitValues[0];
        vkFrame->sem_value[1] = foreign.waitValues[1];
        vkFrame->access[0] = static_cast<VkAccessFlagBits>(foreign.accesses[0]);
        vkFrame->access[1] = static_cast<VkAccessFlagBits>(foreign.accesses[1]);

        impl.framesDecodedHardware++;
        return output;
    }

    // Measured software path (chosen explicitly at open time when the
    // device has no Vulkan video queues): explicit 709 conversion to the
    // contract, then the frame uploads to device residency. The upload is
    // the capability-dependent transfer cost this path carries.
    std::vector<float> pixels = yuv420pToRgba32f(frame.frame);
    auto output = std::make_unique<gpu::Image>(impl.allocator->create_image(
        static_cast<uint32_t>(frame.frame->width), static_cast<uint32_t>(frame.frame->height), 1,
        VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    gpu::uploadImage(*impl.queue, *impl.allocator, *output, pixels.data(), pixels.size() * sizeof(float), timeout_ns);
    // Keep the contract layout consistent with the interop path: GENERAL.
    gpu::imageBarrier(*impl.queue, *output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, timeout_ns);
    return output;
}

SoftwareClip decodeClipSoftware(const std::string& path, int64_t maxFrames) {
    SoftwareClip result;
    FormatGuard format;
    const int openStatus = avformat_open_input(&format.context, path.c_str(), nullptr, nullptr);
    if (openStatus < 0) {
        fail(path, "avformat_open_input failed", openStatus);
    }
    if (avformat_find_stream_info(format.context, nullptr) < 0) {
        fail(path, "avformat_find_stream_info failed");
    }
    const int streamIndex = av_find_best_stream(format.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        fail(path, "no video stream");
    }
    AVStream* stream = format.context->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        fail(path, "no decoder for codec id " + std::to_string(stream->codecpar->codec_id));
    }
    CodecContextGuard context;
    context.context = avcodec_alloc_context3(codec);
    if (context.context == nullptr) {
        fail(path, "avcodec_alloc_context3 failed");
    }
    if (avcodec_parameters_to_context(context.context, stream->codecpar) < 0) {
        fail(path, "avcodec_parameters_to_context failed");
    }
    context.context->thread_count = 1;
    if (avcodec_open2(context.context, codec, nullptr) < 0) {
        fail(path, "avcodec_open2 failed");
    }

    result.info.path = path;
    result.info.codecName = avcodec_get_name(context.context->codec_id);
    result.info.width = context.context->width;
    result.info.height = context.context->height;
    const AVRational rate = stream->avg_frame_rate.num != 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    result.info.frameRate = rate.num != 0 ? static_cast<double>(rate.num) / static_cast<double>(rate.den) : 0.0;

    PacketGuard packet;
    FrameGuard decoded;
    const auto convertFrame = [&](const AVFrame* yuv) {
        CpuImage image(result.info.width, result.info.height);
        std::vector<float> pixels = yuv420pToRgba32f(yuv);
        for (int y = 0; y < yuv->height; ++y) {
            for (int x = 0; x < yuv->width; ++x) {
                const size_t offset = (static_cast<size_t>(y) * yuv->width + x) * 4;
                image.setPixel(x, y, {pixels[offset + 0], pixels[offset + 1], pixels[offset + 2], 1.0F});
            }
        }
        result.frames.push_back(std::move(image));
    };
    while (av_read_frame(format.context, packet.packet) >= 0) {
        if (packet.packet->stream_index != streamIndex) {
            av_packet_unref(packet.packet);
            continue;
        }
        if (avcodec_send_packet(context.context, packet.packet) == 0) {
            while (avcodec_receive_frame(context.context, decoded.frame) == 0) {
                convertFrame(decoded.frame);
                av_frame_unref(decoded.frame);
                if (maxFrames >= 0 && static_cast<int64_t>(result.frames.size()) >= maxFrames) {
                    av_packet_unref(packet.packet);
                    return result;
                }
            }
        }
        av_packet_unref(packet.packet);
    }
    // Flush the delayed decoder (B-frame reordering holds frames).
    if (avcodec_send_packet(context.context, nullptr) == 0) {
        while (avcodec_receive_frame(context.context, decoded.frame) == 0) {
            convertFrame(decoded.frame);
            av_frame_unref(decoded.frame);
        }
    }
    return result;
}

}  // namespace nemo::media
