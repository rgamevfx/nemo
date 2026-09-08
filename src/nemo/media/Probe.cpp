#include "nemo/media/Probe.hpp"

#include "nemo/gpu/Device.hpp"
#include <algorithm>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace nemo::media {

namespace {

// Init-verifies a hardware encoder by opening it for real: registration in
// libavcodec is not evidence of engine support (nvenc encoder entries exist
// for hardware that cannot run them, e.g. AV1 on Pascal). A minimal valid
// configuration is opened; nothing is encoded.
MediaCapability verifyEncoder(const char* name, const char* codecName, bool hardware) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (codec == nullptr) {
        return {codecName, hardware, CapabilityEvidence::Unavailable,
                "encoder not registered in this libavcodec build"};
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        return {codecName, hardware, CapabilityEvidence::Unavailable, "encoder context allocation failed"};
    }
    context->width = 256;
    context->height = 256;
    context->time_base = AVRational{1, 24};
    context->framerate = AVRational{24, 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->bit_rate = 2'000'000;
    context->gop_size = 24;
    char error[AV_ERROR_MAX_STRING_SIZE] = {0};
    const int status = avcodec_open2(context, codec, nullptr);
    if (status < 0) {
        avcodec_free_context(&context);
        av_strerror(status, error, sizeof(error));
        return {codecName, hardware, CapabilityEvidence::Unavailable, std::string("encoder open failed: ") + error};
    }
    avcodec_free_context(&context);
    return {codecName, hardware, CapabilityEvidence::InitVerified, {}};
}

// Vulkan video decode is init-verified only against a real Device: without
// the reserved video decode queue the decoder cannot run, and the claim
// must say so rather than imply usable hardware.
MediaCapability vulkanDecoderClaim(const gpu::Device* device, const char* codecName, AVCodecID codecId,
                                   bool queuesAvailable) {
    const AVCodec* decoder = avcodec_find_decoder(codecId);
    if (decoder == nullptr) {
        return {codecName, true, CapabilityEvidence::Unavailable, "decoder not registered in this libavcodec build"};
    }
    bool vulkanHwConfig = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
        if (config == nullptr) {
            break;
        }
        if (config->pix_fmt == AV_PIX_FMT_VULKAN) {
            vulkanHwConfig = true;
            break;
        }
    }
    if (!vulkanHwConfig) {
        return {codecName, true, CapabilityEvidence::Unavailable,
                "libavcodec build has no Vulkan frame configuration for this decoder"};
    }
    if (device == nullptr) {
        return {codecName, true, CapabilityEvidence::RegisteredOnly, {}};
    }
    if (!queuesAvailable) {
        return {codecName, true, CapabilityEvidence::Unavailable,
                "Vulkan device did not reserve a video decode queue family (extensions/queues absent)"};
    }
    return {codecName, true, CapabilityEvidence::InitVerified, {}};
}

MediaCapability softwareDecoderClaim(AVCodecID codecId, const char* codecName) {
    const AVCodec* decoder = avcodec_find_decoder(codecId);
    if (decoder == nullptr) {
        return {codecName, false, CapabilityEvidence::Unavailable, "software decoder not in this libavcodec build"};
    }
    return {codecName, false, CapabilityEvidence::RegisteredOnly, {}};
}

}  // namespace

MediaCapabilities probeMediaCapabilities(const gpu::Device* device) {
    MediaCapabilities capabilities;
    capabilities.vulkanVideoDecodeQueues = device != nullptr && device->decode_family().has_value();

    // Hardware decode candidates measured against the device; the software
    // decoder of the same codec is the declared always-available baseline
    // path (its use is reported explicitly, never silent — the decoder
    // surfaces hardwareDecoded per frame).
    capabilities.decoders.push_back(
        vulkanDecoderClaim(device, "h264-vulkan", AV_CODEC_ID_H264, capabilities.vulkanVideoDecodeQueues));
    capabilities.decoders.push_back(
        vulkanDecoderClaim(device, "hevc-vulkan", AV_CODEC_ID_HEVC, capabilities.vulkanVideoDecodeQueues));
    capabilities.decoders.push_back(softwareDecoderClaim(AV_CODEC_ID_H264, "h264-software"));
    capabilities.decoders.push_back(softwareDecoderClaim(AV_CODEC_ID_HEVC, "hevc-software"));

    capabilities.encoders.push_back(verifyEncoder("h264_nvenc", "h264-nvenc", true));
    capabilities.encoders.push_back(verifyEncoder("hevc_nvenc", "hevc-nvenc", true));
    capabilities.encoders.push_back(verifyEncoder("libx264", "libx264-cpu", false));
    capabilities.encoders.push_back(verifyEncoder("libx265", "libx265-cpu", false));
    return capabilities;
}

std::string formatMediaCapabilities(const MediaCapabilities& capabilities) {
    std::ostringstream out;
    const auto render = [&](const std::vector<MediaCapability>& list, const char* title) {
        out << title << '\n';
        for (const MediaCapability& capability : list) {
            out << "  " << (capability.hardware ? "hw  " : "cpu ") << capability.codec << ": ";
            switch (capability.evidence) {
            case CapabilityEvidence::InitVerified:
                out << "init-verified";
                break;
            case CapabilityEvidence::RegisteredOnly:
                out << "registered (not init-verified)";
                break;
            case CapabilityEvidence::Unavailable:
                out << "unavailable: " << capability.reason;
                break;
            }
            out << '\n';
        }
    };
    render(capabilities.decoders, "decoders:");
    render(capabilities.encoders, "encoders:");
    out << "vulkan video decode queues: " << (capabilities.vulkanVideoDecodeQueues ? "reserved" : "absent") << '\n';
    return out.str();
}

}  // namespace nemo::media
