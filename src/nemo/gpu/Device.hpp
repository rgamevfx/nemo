#pragma once

#include <memory>
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
// graphics family. `queue()` hands out only the selected families.
//
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

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkPhysicalDevice physical() const { return physical_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }

    [[nodiscard]] uint32_t graphics_family() const { return graphics_family_; }
    [[nodiscard]] uint32_t transfer_family() const { return transfer_family_; }

    // Features enabled at creation: shaderStorageImageReadWithoutFormat and
    // shaderStorageImageWriteWithoutFormat, each only when the physical
    // device reports support. Native effect kernels (issue #8) read/write
    // storage images whose format Slang emits as Unknown.
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
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures features_{};
};

}  // namespace nemo::gpu
