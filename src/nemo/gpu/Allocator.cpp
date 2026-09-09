#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/ExternalHandle.hpp"

#include <algorithm>
#include <array>

#include <mutex>
#include <string>
#include <utility>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace nemo::gpu {

struct Allocator::Token {};
Allocator::Allocator(Token) {}

struct Allocator::Impl {
    uint64_t budget = 0;
    uint64_t charged = 0;
    VmaAllocator vma = VK_NULL_HANDLE;
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    bool externalSharing = false;
    ~Impl() {
        if (vma != VK_NULL_HANDLE)
            vmaDestroyAllocator(vma);
    }
    struct Allocation {
        std::shared_ptr<Impl> allocator;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint64_t charge = 0;
        ~Allocation() {
            if (!allocator)
                return;
            std::lock_guard lock(allocator->mutex);
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(allocator->device, view, nullptr);
            if (image != VK_NULL_HANDLE)
                vmaDestroyImage(allocator->vma, image, allocation);
            if (buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator->vma, buffer, allocation);
            allocator->charged -= charge;
        }
    };
    struct SharedAllocation {
        std::shared_ptr<Impl> allocator;
        std::array<VkDevice, 2> devices{};
        std::array<VkDeviceMemory, 2> memory{};
        std::array<VkImage, 2> images{};
        std::array<VkImageView, 2> views{};
        uint64_t charge = 0;
        ~SharedAllocation() {
            std::lock_guard lock(allocator->mutex);
            for (unsigned i = 2; i != 0;) {
                --i;
                if (views[i])
                    vkDestroyImageView(devices[i], views[i], nullptr);
                if (images[i])
                    vkDestroyImage(devices[i], images[i], nullptr);
                if (memory[i])
                    vkFreeMemory(devices[i], memory[i], nullptr);
            }
            allocator->charged -= charge;
        }
    };
};

std::unique_ptr<Allocator> Allocator::create(Instance& instance, Device& device, const AllocatorConfig& config) {
    auto allocator = std::make_unique<Allocator>(Token{});
    allocator->impl_ = std::make_shared<Impl>();
    allocator->impl_->budget = config.max_device_bytes;
    allocator->impl_->device = device.handle();
    allocator->impl_->physical = device.physical();
    allocator->impl_->externalSharing = device.external_sharing_enabled();
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = device.physical();
    info.device = device.handle();
    info.instance = instance.handle();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    checkVulkan(vmaCreateAllocator(&info, &allocator->impl_->vma), "vmaCreateAllocator");
    return allocator;
}

Allocator::~Allocator() = default;
uint64_t Allocator::budget() const {
    return impl_->budget;
}
uint64_t Allocator::charged_bytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->charged;
}

Buffer Allocator::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryPreference preference) {
    if (size == 0)
        throw GpuException(GpuError::InvalidRequest, "create_buffer size must be positive");
    // Allocate the owner before Vulkan work. It is destroyed after the lock
    // on all exceptional exits, so partial allocations retire safely.
    auto owner = std::make_shared<Impl::Allocation>();
    owner->allocator = impl_;
    std::lock_guard lock(impl_->mutex);
    const uint64_t requested = static_cast<uint64_t>(size);
    if (requested > impl_->budget - impl_->charged) {
        throw GpuException(GpuError::BudgetExceeded, "buffer request of " + std::to_string(requested) + " bytes plus " +
                                                         std::to_string(impl_->charged) +
                                                         " already charged exceeds the " +
                                                         std::to_string(impl_->budget) + " byte allocation budget");
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    if (preference == MemoryPreference::HostMapped) {
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VmaAllocationInfo mappedInfo{};
    checkVulkan(vmaCreateBuffer(impl_->vma, &info, &allocationInfo, &owner->buffer, &owner->allocation, &mappedInfo),
                "vmaCreateBuffer");
    owner->charge = requested;
    impl_->charged += requested;
    return Buffer(owner, owner->buffer, size, mappedInfo.pMappedData);
}

Image Allocator::create_image(uint32_t width, uint32_t height, uint32_t depth, VkFormat format, VkImageUsageFlags usage,
                              uint32_t dimensions) {
    if (width == 0 || height == 0 || depth == 0)
        throw GpuException(GpuError::InvalidRequest, "create_image extents must be positive");
    if (dimensions == 0)
        dimensions = depth > 1 ? 3u : height > 1 ? 2u : 1u;
    if (dimensions > 3 || (dimensions < 3 && depth != 1) || (dimensions == 1 && height != 1))
        throw GpuException(GpuError::InvalidRequest, "create_image dimensionality disagrees with extents");
    auto owner = std::make_shared<Impl::Allocation>();
    owner->allocator = impl_;
    std::lock_guard lock(impl_->mutex);
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = dimensions == 1 ? VK_IMAGE_TYPE_1D : dimensions == 2 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
    info.format = format;
    info.extent = {width, height, depth};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    checkVulkan(vmaCreateImage(impl_->vma, &info, &allocationInfo, &owner->image, &owner->allocation, nullptr),
                "vmaCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(impl_->device, owner->image, &requirements);
    const uint64_t bytes = requirements.size;
    if (bytes > impl_->budget - impl_->charged) {
        throw GpuException(GpuError::BudgetExceeded, "image request of " + std::to_string(bytes) + " bytes plus " +
                                                         std::to_string(impl_->charged) +
                                                         " already charged exceeds the " +
                                                         std::to_string(impl_->budget) + " byte allocation budget");
    }
    owner->charge = bytes;
    impl_->charged += bytes;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = owner->image;
    viewInfo.viewType = dimensions == 1   ? VK_IMAGE_VIEW_TYPE_1D
                        : dimensions == 2 ? VK_IMAGE_VIEW_TYPE_2D
                                          : VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    checkVulkan(vkCreateImageView(impl_->device, &viewInfo, nullptr, &owner->view), "vkCreateImageView");
    return Image(owner, owner->image, owner->view, format, info.extent, dimensions);
}

std::pair<Image, Image> Allocator::create_shared_image(Device& consumer, uint32_t width, uint32_t height,
                                                       VkFormat format, VkImageUsageFlags usage) {
    if (!impl_->externalSharing || !consumer.external_sharing_enabled())
        throw GpuException(GpuError::InvalidRequest,
                           "shared presentation images require external sharing on both devices");
    if (consumer.physical() != impl_->physical || consumer.handle() == impl_->device)
        throw GpuException(GpuError::InvalidRequest,
                           "shared presentation images require distinct devices on the same GPU");
    if (width == 0 || height == 0)
        throw GpuException(GpuError::InvalidRequest, "shared presentation image extents must be positive");

    VkPhysicalDeviceExternalImageFormatInfo externalQuery{};
    externalQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    externalQuery.handleType = detail::memoryHandleType;
    VkPhysicalDeviceImageFormatInfo2 query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.pNext = &externalQuery;
    query.format = format;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_OPTIMAL;
    query.usage = usage;
    VkExternalImageFormatProperties externalProperties{};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    properties.pNext = &externalProperties;
    const auto supported = vkGetPhysicalDeviceImageFormatProperties2(impl_->physical, &query, &properties);
    const auto& external = externalProperties.externalMemoryProperties;
    constexpr auto required = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if (supported != VK_SUCCESS || (external.externalMemoryFeatures & required) != required ||
        !(external.compatibleHandleTypes & detail::memoryHandleType))
        throw GpuException(GpuError::InvalidRequest,
                           "GPU cannot export/import the requested optimal-tiling presentation format and usage");
    if (width > properties.imageFormatProperties.maxExtent.width ||
        height > properties.imageFormatProperties.maxExtent.height)
        throw GpuException(GpuError::InvalidRequest, "shared presentation dimensions exceed external image limits");

    auto owner = std::make_shared<Impl::SharedAllocation>();
    owner->allocator = impl_;
    owner->devices = {impl_->device, consumer.handle()};
    std::lock_guard lock(impl_->mutex);
    VkExternalMemoryImageCreateInfo externalImage{};
    externalImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImage.handleTypes = detail::memoryHandleType;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalImage;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::array<VkMemoryRequirements, 2> requirements{};
    for (unsigned i = 0; i != 2; ++i) {
        checkVulkan(vkCreateImage(owner->devices[i], &imageInfo, nullptr, &owner->images[i]), "vkCreateImage(shared)");
        vkGetImageMemoryRequirements(owner->devices[i], owner->images[i], &requirements[i]);
    }
    const auto bytes = std::max(requirements[0].size, requirements[1].size);
    if (bytes > impl_->budget - impl_->charged)
        throw GpuException(GpuError::BudgetExceeded, "shared presentation image request of " + std::to_string(bytes) +
                                                         " bytes plus " + std::to_string(impl_->charged) +
                                                         " already charged exceeds the " +
                                                         std::to_string(impl_->budget) + " byte allocation budget");
    if (bytes > properties.imageFormatProperties.maxResourceSize)
        throw GpuException(GpuError::InvalidRequest,
                           "shared presentation allocation exceeds external image resource limit");
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(impl_->physical, &memoryProperties);
    const auto compatibleTypes = requirements[0].memoryTypeBits & requirements[1].memoryTypeBits;
    uint32_t memoryType = 0;
    for (; memoryType < memoryProperties.memoryTypeCount; ++memoryType)
        if ((compatibleTypes & (1u << memoryType)) &&
            (memoryProperties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    if (memoryType == memoryProperties.memoryTypeCount)
        throw GpuException(GpuError::InvalidRequest,
                           "shared presentation image has no compatible device-local memory type");
    owner->charge = bytes;
    impl_->charged += bytes;

    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = owner->images[0];
    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.pNext = &dedicated;
    exportInfo.handleTypes = detail::memoryHandleType;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &exportInfo;
    allocate.allocationSize = bytes;
    allocate.memoryTypeIndex = memoryType;
    checkVulkan(vkAllocateMemory(impl_->device, &allocate, nullptr, &owner->memory[0]), "vkAllocateMemory(export)");
    checkVulkan(vkBindImageMemory(impl_->device, owner->images[0], owner->memory[0], 0), "vkBindImageMemory(export)");
#if defined(_WIN32)
    const auto getHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(impl_->device, "vkGetMemoryWin32HandleKHR"));
    if (!getHandle)
        throw GpuException(GpuError::InvalidRequest, "vkGetMemoryWin32HandleKHR unavailable");
    VkMemoryGetWin32HandleInfoKHR get{};
    get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    get.memory = owner->memory[0];
    get.handleType = detail::memoryHandleType;
    HANDLE value = nullptr;
    checkVulkan(getHandle(impl_->device, &get, &value), "vkGetMemoryWin32HandleKHR");
    detail::NativeHandle handle(value);
    VkImportMemoryWin32HandleInfoKHR import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import.handle = handle.get();
#else
    const auto getHandle =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(impl_->device, "vkGetMemoryFdKHR"));
    if (!getHandle)
        throw GpuException(GpuError::InvalidRequest, "vkGetMemoryFdKHR unavailable");
    VkMemoryGetFdInfoKHR get{};
    get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get.memory = owner->memory[0];
    get.handleType = detail::memoryHandleType;
    int value = -1;
    checkVulkan(getHandle(impl_->device, &get, &value), "vkGetMemoryFdKHR");
    detail::NativeHandle handle(value);
    VkImportMemoryFdInfoKHR import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.fd = handle.get();
#endif
    import.handleType = detail::memoryHandleType;
    dedicated.image = owner->images[1];
    import.pNext = &dedicated;
    allocate.pNext = &import;
    checkVulkan(vkAllocateMemory(consumer.handle(), &allocate, nullptr, &owner->memory[1]), "vkAllocateMemory(import)");
    handle.imported();
    checkVulkan(vkBindImageMemory(consumer.handle(), owner->images[1], owner->memory[1], 0),
                "vkBindImageMemory(import)");
    for (unsigned i = 0; i != 2; ++i) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = owner->images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checkVulkan(vkCreateImageView(owner->devices[i], &view, nullptr, &owner->views[i]),
                    "vkCreateImageView(shared)");
    }
    return {Image(owner, owner->images[0], owner->views[0], format, imageInfo.extent, 2),
            Image(owner, owner->images[1], owner->views[1], format, imageInfo.extent, 2)};
}

Buffer::~Buffer() = default;
Buffer::Buffer(Buffer&& other) noexcept
    : owner_(std::move(other.owner_)), buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)), mapped_(std::exchange(other.mapped_, nullptr)) {}
Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}
Image::~Image() = default;
Image::Image(Image&& other) noexcept
    : owner_(std::move(other.owner_)), image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, VkExtent3D{})), dimensions_(std::exchange(other.dimensions_, 0u)) {}
Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_ = std::exchange(other.extent_, VkExtent3D{});
        dimensions_ = std::exchange(other.dimensions_, 0u);
    }
    return *this;
}
}  // namespace nemo::gpu
