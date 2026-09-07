#pragma once

#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>

namespace nemo::gpu {

// GPU failures always explain what failed and where the relationship broke
// down (device, allocation request, submission) rather than returning bare
// result codes. Mirrors the core GraphException pattern.
enum class GpuError {
    NoDevice,           ///< no usable Vulkan physical device on this machine
    LayerUnavailable,   ///< requested validation layer/machinery is not installed
    VulkanError,        ///< a vk* call returned an error result
    BudgetExceeded,     ///< an allocation would exceed the configured budget
    InvalidRequest,     ///< a request is malformed (e.g. zero-sized allocation)
    SubmissionTimeout,  ///< a submitted command buffer did not complete in time
};

class GpuException : public std::runtime_error {
public:
    GpuException(GpuError code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] GpuError errorCode() const { return code_; }

private:
    GpuError code_;
};

// Throws a descriptive GpuException naming the failing vk* call and result.
[[noreturn]] void throwVulkanError(VkResult result, const char* what);

inline void checkVulkan(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throwVulkanError(result, what);
    }
}

}  // namespace nemo::gpu
