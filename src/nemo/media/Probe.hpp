#pragma once

// Hardware media codec capability probe (issue #10, spec section 10.4).
//
// NVDEC/NVENC engines are treated as capability-dependent engines, not
// codecs: every claim carries evidence, never an assumption. A codec that
// is merely registered in libavcodec is NOT reported as usable — hardware
// engine support is measured by actually initializing the codec (see
// CapabilityEvidence). An unsupported path carries a clear reason string;
// nothing degrades silently to a semantically different path.

#include <string>
#include <vector>

namespace nemo::gpu {
class Device;
}

namespace nemo::media {

// What evidence backs a capability claim.
enum class CapabilityEvidence {
    Unavailable,        // not usable; `reason` explains precisely why
    RegisteredOnly,     // compiled into libavcodec, not init-verified
    InitVerified,       // codec opened successfully (real engine test)
};

// One decoder or encoder capability claim.
struct MediaCapability {
    std::string codec;   // e.g. "h264-vulkan", "hevc-nvenc", "libx264"
    bool hardware;       // true = GPU engine (Vulkan video / NVDEC / NVENC)
    CapabilityEvidence evidence;
    std::string reason;  // empty exactly when evidence != Unavailable
};

// Probe results for the declared reference codecs.
struct MediaCapabilities {
    std::vector<MediaCapability> decoders;
    std::vector<MediaCapability> encoders;

    // True when the device reserved a video decode queue family and the
    // Vulkan video decode extensions are enabled.
    bool vulkanVideoDecodeQueues{false};
};

// Probes hardware decode/encode capability. `device` (optional) supplies
// the Vulkan video queue evidence for Vulkan-video claims; without it,
// Vulkan-video decoders can only be reported RegisteredOnly.
//
// Deliberately NOT a static list: encoder claims are init-verified by
// opening the codec with a minimal frames configuration, because encoder
// registration alone lies (e.g. av1_nvenc is registered in libavcodec but
// the NVENC engine rejects it on hardware without AV1 encode support).
[[nodiscard]] MediaCapabilities probeMediaCapabilities(const gpu::Device* device);

// Renders the capabilities as a diagnostic report (CLI `probe-media`).
[[nodiscard]] std::string formatMediaCapabilities(const MediaCapabilities& capabilities);

}  // namespace nemo::media
