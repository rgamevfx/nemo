#pragma once
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Instance.hpp"

namespace nemo::gpu {

// Owns the physical/logical device pair with explicit queue selection.
//
// Queue selection contract (headless bootstrap): the graphics family is the
// first family advertising graphics+compute; the transfer family is a
// dedicated transfer family when the driver exposes one, otherwise the
// graphics family. When the driver advertises video decode/encode queue
// families (VK_KHR_video_*), the corresponding video family is reserved
// and its extension is enabled — hardware media decode (issue #10) needs
// it, capability-measured rather than assumed: absent queues report
// nullopt, never a silent substitute. `queue()` hands out only the
// selected families.
// Threading contract: VkQueue acquisition and submission are externally
// synchronized per queue; this type is not thread-safe. The SubmissionQueue
// helper owns the synchronization discipline for its queue.
class Device {
public:
    // Pass-key construction: `Token` is a private type, so callers outside
    // this header cannot construct a Device without going through create().
    struct Token;
    explicit Device(Token);
    ~Device();
    static std::unique_ptr<Device> create(Instance& instance);
    [[nodiscard]] uint32_t graphics_family() const { return graphics_family_; }
    [[nodiscard]] uint32_t transfer_family() const { return transfer_family_; }

    // Reserved video queue families, when the driver advertises them
    // (VK_KHR_video_decode/encode extensions enabled at creation). Absent
    // video support reports nullopt — capability measured, not assumed.
    [[nodiscard]] std::optional<uint32_t> decode_family() const { return decode_family_; }
    [[nodiscard]] std::optional<uint32_t> encode_family() const { return encode_family_; }

    // Device extensions enabled at creation (the hardware-media interop
    // (issue #10) declares this list to FFmpeg's AVVulkanDeviceContext).
    [[nodiscard]] const std::vector<std::string>& enabled_extensions() const { return enabled_extensions_; }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkPhysicalDevice physical() const { return physical_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }


    // Physical-device feature support queried at creation. Creation enables
    // shaderStorageImageReadWithoutFormat and shaderStorageImageWriteWithout
    // Format — each exactly when reported here — because native effect
    // kernels (issue #8) read/write storage images whose format Slang emits
    // as Unknown. The effect executor refuses to run when either is absent.
    [[nodiscard]] const VkPhysicalDeviceFeatures& features() const { return features_; }

    // Queue-family capabilities as advertised by the driver; 0 for an unknown
    // family index.
    [[nodiscard]] VkQueueFlags family_capabilities(uint32_t family) const;

    // The selected queue for `family`, or VK_NULL_HANDLE when the family is
    // not selected.
    [[nodiscard]] VkQueue queue(uint32_t family) const;

private:
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    std::vector<VkQueueFamilyProperties> family_properties_;
    uint32_t graphics_family_ = 0;
    uint32_t transfer_family_ = 0;
    std::optional<uint32_t> decode_family_;
    std::optional<uint32_t> encode_family_;
    std::vector<std::string> enabled_extensions_;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures features_{};
};

}  // namespace nemo::gpu
