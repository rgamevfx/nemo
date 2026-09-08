#include "nemo/gpu/Error.hpp"

#include <string>

namespace nemo::gpu {

[[nodiscard]] static std::string vulkanResultName(VkResult result) {
    // VK_RESULT_TO_STRING via the extension header when compiled in; a small
    // local table keeps the module free of the nonstandard helper.
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_INVALID_DEVICE_ADDRESS_EXT:
        return "VK_ERROR_INVALID_DEVICE_ADDRESS_EXT";
    default:
        return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

void throwVulkanError(VkResult result, const char* what) {
    throw GpuException(GpuError::VulkanError, std::string(what) + " failed: " + vulkanResultName(result));
}

}  // namespace nemo::gpu
