#include "nemo/gpu/Device.hpp"

#include <algorithm>
#include <string>

namespace nemo::gpu {

struct Device::Token {};
Device::Device(Token) {}

namespace {

VkPhysicalDevice selectPhysicalDevice(VkInstance instance) {
    uint32_t count = 0;
    checkVulkan(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        throw GpuException(GpuError::NoDevice, "no Vulkan physical device found (no driver ICDs or no compatible GPU)");
    }
    std::vector<VkPhysicalDevice> physical_devices(count);
    checkVulkan(vkEnumeratePhysicalDevices(instance, &count, physical_devices.data()), "vkEnumeratePhysicalDevices");

    // Prefer a discrete GPU when one exists; otherwise take the first device
    // so software renderers and integrated GPUs still bootstrap.
    auto discrete = std::find_if(physical_devices.begin(), physical_devices.end(), [](VkPhysicalDevice device) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    });
    return discrete != physical_devices.end() ? *discrete : physical_devices.front();
}

std::vector<VkQueueFamilyProperties> queueFamilyProperties(VkPhysicalDevice physical) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties.data());
    return properties;
}

}  // namespace

std::unique_ptr<Device> Device::create(Instance& instance) {
    auto device = std::make_unique<Device>(Device::Token{});
    device->physical_ = selectPhysicalDevice(instance.handle());
    vkGetPhysicalDeviceProperties(device->physical_, &device->properties_);
    device->family_properties_ = queueFamilyProperties(device->physical_);
    // Storage-image effect kernels (issue #8): Slang emits RGBA32F storage
    // images with Unknown format, which requires the without-format
    // features. Enable each only when the device reports support; the
    // effect executor refuses to run when the format path is unavailable.
    vkGetPhysicalDeviceFeatures(device->physical_, &device->features_);
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageReadWithoutFormat = device->features_.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat = device->features_.shaderStorageImageWriteWithoutFormat;

    // Explicit queue selection per the class contract: the graphics family is
    // the first family advertising graphics+compute; the transfer family is a
    // dedicated transfer family when the driver exposes one, otherwise the
    // graphics family.
    auto graphics = std::find_if(device->family_properties_.begin(), device->family_properties_.end(),
                                 [](const VkQueueFamilyProperties& properties) {
                                     const auto flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                                     return (properties.queueFlags & flags) == flags && properties.queueCount > 0;
                                 });
    if (graphics == device->family_properties_.end()) {
        throw GpuException(GpuError::VulkanError, std::string("no graphics+compute queue family on physical device ") +
                                                      device->properties_.deviceName);
    }
    device->graphics_family_ = static_cast<uint32_t>(graphics - device->family_properties_.begin());

    device->transfer_family_ = device->graphics_family_;
    auto dedicated_transfer =
        std::find_if(device->family_properties_.begin(), device->family_properties_.end(),
                     [](const VkQueueFamilyProperties& properties) {
                         return (properties.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0 &&
                                (properties.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 &&
                                properties.queueCount > 0;
                     });
    if (dedicated_transfer != device->family_properties_.end()) {
        device->transfer_family_ = static_cast<uint32_t>(dedicated_transfer - device->family_properties_.begin());
    }

    // One queue per selected family, deduplicated when the driver offers no
    // dedicated transfer family.
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    std::vector<float> priorities(1, 1.0f);
    for (uint32_t family : {device->graphics_family_, device->transfer_family_}) {
        if (std::any_of(queue_infos.begin(), queue_infos.end(),
                        [&](const VkDeviceQueueCreateInfo& info) { return info.queueFamilyIndex == family; })) {
            continue;
        }
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = priorities.data();
        queue_infos.push_back(queue_info);
    }

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.pEnabledFeatures = &enabled;
    checkVulkan(vkCreateDevice(device->physical_, &create_info, nullptr, &device->device_), "vkCreateDevice");
    vkGetDeviceQueue(device->device_, device->graphics_family_, 0, &device->graphics_queue_);
    if (device->transfer_family_ != device->graphics_family_) {
        vkGetDeviceQueue(device->device_, device->transfer_family_, 0, &device->transfer_queue_);
    } else {
        device->transfer_queue_ = device->graphics_queue_;
    }
    return device;
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
}

VkQueueFlags Device::family_capabilities(uint32_t family) const {
    return family < family_properties_.size() ? family_properties_[family].queueFlags : 0;
}

VkQueue Device::queue(uint32_t family) const {
    if (family == graphics_family_)
        return graphics_queue_;
    if (family == transfer_family_)
        return transfer_queue_;
    return VK_NULL_HANDLE;
}

}  // namespace nemo::gpu
