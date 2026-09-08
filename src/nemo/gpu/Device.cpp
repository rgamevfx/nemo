#include "nemo/gpu/Device.hpp"

#include <algorithm>
#include <cstring>
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
    // Timeline semaphores: the cross-queue dependency for external video
    // frames (issue #10 interop). 1.2-promoted feature; via the KHR
    // extension feature struct so the 1.0-style enabled-features path
    // stays unchanged.
    // Synchronization2: FFmpeg's Vulkan video decoder submits with
    // barrier2/submit2 on the supplied device (issue #10 interop).
    VkPhysicalDeviceSynchronization2Features sync2Features{};
    sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2Features.synchronization2 = VK_TRUE;
    // SamplerYcbcrConversion: sampling the decoded NV12 planes needs the
    // YCbCr conversion feature for the chroma-plane views (issue #10).
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures{};
    ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrFeatures.samplerYcbcrConversion = VK_TRUE;
    sync2Features.pNext = &ycbcrFeatures;
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeatures.pNext = &sync2Features;
    timelineFeatures.timelineSemaphore = VK_TRUE;
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

    // Video queue reservation (issue #10): a decode/encode video family is
    // reserved when the driver advertises the video extensions that make it
    // functional — never silently absent, never assumed present.
    uint32_t extension_count = 0;
    checkVulkan(vkEnumerateDeviceExtensionProperties(device->physical_, nullptr, &extension_count, nullptr),
                "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> extensions(extension_count);
    checkVulkan(vkEnumerateDeviceExtensionProperties(device->physical_, nullptr, &extension_count, extensions.data()),
                "vkEnumerateDeviceExtensionProperties");
    const auto advertised = [&](const char* name) {
        return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
    };
    constexpr const char* kVideoQueue = "VK_KHR_video_queue";
    constexpr const char* kVideoDecode = "VK_KHR_video_decode_queue";
    constexpr const char* kVideoDecodeH264 = "VK_KHR_video_decode_h264";
    constexpr const char* kVideoDecodeH265 = "VK_KHR_video_decode_h265";
    constexpr const char* kVideoEncode = "VK_KHR_video_encode_queue";
    constexpr const char* kVideoEncodeH264 = "VK_KHR_video_encode_h264";
    constexpr const char* kVideoEncodeH265 = "VK_KHR_video_encode_h265";
    constexpr const char* kVideoMaintenance1 = "VK_KHR_video_maintenance1";
    const bool video_queue = advertised(kVideoQueue);
    const bool video_decode = video_queue && advertised(kVideoDecode);
    const bool video_encode = video_queue && advertised(kVideoEncode);

    std::vector<uint32_t> reserved_families = {device->graphics_family_, device->transfer_family_};
    const auto reserve_video = [&](VkQueueFlags flag, std::optional<uint32_t>& into) {
        const auto found = std::find_if(device->family_properties_.begin(), device->family_properties_.end(),
                                        [&](const VkQueueFamilyProperties& properties) {
                                            return (properties.queueFlags & flag) != 0 && properties.queueCount > 0;
                                        });
        if (found != device->family_properties_.end()) {
            into = static_cast<uint32_t>(found - device->family_properties_.begin());
            if (std::find(reserved_families.begin(), reserved_families.end(), *into) == reserved_families.end()) {
                reserved_families.push_back(*into);
            }
        }
    };
    if (video_decode) {
        reserve_video(VK_QUEUE_VIDEO_DECODE_BIT_KHR, device->decode_family_);
    }
    if (video_encode) {
        reserve_video(VK_QUEUE_VIDEO_ENCODE_BIT_KHR, device->encode_family_);
    }

    // One queue per selected family, deduplicated when the driver offers no
    // dedicated transfer family.
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    std::vector<float> priorities(1, 1.0f);
    for (uint32_t family : reserved_families) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = priorities.data();
        queue_infos.push_back(queue_info);
    }

    // Device extensions enabled exactly when advertised: video decode (with
    // the codec profiles the media path uses) and video encode stay
    // capability-measured; the enabled list is also reported to FFmpeg's
    // AVVulkanDeviceContext (issue #10).
    const auto enable = [&](bool condition, const char* name) {
        if (condition) {
            device->enabled_extensions_.emplace_back(name);
        }
    };
    enable(video_queue, kVideoQueue);
    enable(video_decode, kVideoDecode);
    enable(video_decode && advertised(kVideoDecodeH264), kVideoDecodeH264);
    enable(video_decode && advertised(kVideoDecodeH265), kVideoDecodeH265);
    enable(video_encode, kVideoEncode);
    enable(video_encode && advertised(kVideoEncodeH264), kVideoEncodeH264);
    enable(video_encode && advertised(kVideoEncodeH265), kVideoEncodeH265);
    enable(video_queue && advertised(kVideoMaintenance1), kVideoMaintenance1);
    std::vector<const char*> extension_names;
    extension_names.reserve(device->enabled_extensions_.size());
    for (const std::string& extension : device->enabled_extensions_) {
        extension_names.push_back(extension.c_str());
    }

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &timelineFeatures;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.pEnabledFeatures = &enabled;
    if (!extension_names.empty()) {
        create_info.enabledExtensionCount = static_cast<uint32_t>(extension_names.size());
        create_info.ppEnabledExtensionNames = extension_names.data();
    }
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
