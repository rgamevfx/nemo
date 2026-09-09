#include "nemo/gpu/Device.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Submit.hpp"

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

std::unique_ptr<Device> Device::create(Instance& instance, const DeviceConfig& config) {
    auto device = std::make_unique<Device>(Device::Token{});
    device->presentation_ = config.presentation;
    device->external_sharing_ = config.externalSharing;
    device->physical_ = config.physical != VK_NULL_HANDLE ? config.physical : selectPhysicalDevice(instance.handle());
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
    // Compact viewer encoding writes R8 luma/chroma storage planes.
    enabled.shaderStorageImageExtendedFormats = device->features_.shaderStorageImageExtendedFormats;
    VkPhysicalDeviceHostQueryResetFeatures hostQueryReset{};
    hostQueryReset.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    VkPhysicalDeviceFeatures2 queried{};
    queried.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    queried.pNext = &hostQueryReset;
    vkGetPhysicalDeviceFeatures2(device->physical_, &queried);
    device->host_query_reset_ = hostQueryReset.hostQueryReset == VK_TRUE;
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
    ycbcrFeatures.pNext = &hostQueryReset;
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
        throw GpuException(GpuError::InvalidRequest,
                           std::string("no graphics+compute queue family on physical device ") +
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

    // Selected families, deduplicated when the driver offers no dedicated
    // transfer family (VUID-02802 requires unique family indices).
    std::vector<uint32_t> reserved_families;
    for (uint32_t family : {device->graphics_family_, device->transfer_family_}) {
        if (std::find(reserved_families.begin(), reserved_families.end(), family) == reserved_families.end()) {
            reserved_families.push_back(family);
        }
    }
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

    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    const float priority = 1.0f;
    for (uint32_t family : reserved_families) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
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
    if (config.presentation && !advertised(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        throw GpuException(GpuError::InvalidRequest, "Vulkan presentation requires VK_KHR_swapchain");
    enable(config.presentation, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (config.externalSharing) {
#if defined(_WIN32)
        const char* sharingExtensions[] = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
#else
        const char* sharingExtensions[] = {"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd"};
#endif
        for (const auto* extension : sharingExtensions) {
            if (!advertised(extension))
                throw GpuException(GpuError::InvalidRequest, std::string("GPU presentation sharing requires ") +
                                                                 extension + " on " + device->properties_.deviceName);
            enable(true, extension);
        }
    }
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
    // Teardown order (issue #22): persistent submission queues first — each
    // drains only its own pending fences, never vkDeviceWaitIdle — then the
    // compute pipeline cache, then the VkDevice. Nothing may be in flight
    // when the pipelines and the device handle go away.
    submission_queues_.clear();
    compute_pipeline_cache_.reset();
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

uint32_t Device::family_timestamp_bits(uint32_t family) const {
    return family < family_properties_.size() ? family_properties_[family].timestampValidBits : 0;
}

std::mutex& Device::queueMutex(uint32_t family) {
    std::lock_guard<std::mutex> lock(queue_mutexes_mutex_);
    return queue_mutexes_[family];
}

SubmissionQueue& Device::submissions(uint32_t family) {
    std::lock_guard<std::mutex> lock(submissions_mutex_);
    std::unique_ptr<SubmissionQueue>& entry = submission_queues_[family];
    if (!entry) {
        entry = std::make_unique<SubmissionQueue>(*this, family);
    }
    return *entry;
}

ComputePipelineCache& Device::computePipelineCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (!compute_pipeline_cache_) {
        compute_pipeline_cache_ = ComputePipelineCache::create(*this);
    }
    return *compute_pipeline_cache_;
}

SubmissionStats Device::submissionStats() const {
    SubmissionStats snapshot;
    snapshot.submissions = counters_.submissions.load(std::memory_order_relaxed);
    snapshot.waits = counters_.waits.load(std::memory_order_relaxed);
    snapshot.pipelineCreations = counters_.pipelineCreations.load(std::memory_order_relaxed);
    snapshot.cpuSubmitNs = counters_.cpuSubmitNs.load(std::memory_order_relaxed);
    snapshot.cpuWaitNs = counters_.cpuWaitNs.load(std::memory_order_relaxed);
    snapshot.completions = counters_.completions.load(std::memory_order_relaxed);
    snapshot.gpuExecutionNs = counters_.gpuExecutionNs.load(std::memory_order_relaxed);
    snapshot.gpuTimedSubmissions = counters_.gpuTimedSubmissions.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace nemo::gpu
