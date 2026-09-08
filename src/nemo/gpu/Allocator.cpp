#include "nemo/gpu/Allocator.hpp"

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
                vkDestroyImageView(allocator->vma->m_hDevice, view, nullptr);
            if (image != VK_NULL_HANDLE)
                vmaDestroyImage(allocator->vma, image, allocation);
            if (buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator->vma, buffer, allocation);
            allocator->charged -= charge;
        }
    };
};

std::unique_ptr<Allocator> Allocator::create(Instance& instance, Device& device, const AllocatorConfig& config) {
    auto allocator = std::make_unique<Allocator>(Token{});
    allocator->impl_ = std::make_shared<Impl>();
    allocator->impl_->budget = config.max_device_bytes;
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

Image Allocator::create_image(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                              VkImageUsageFlags usage) {
    if (width == 0 || height == 0 || depth == 0)
        throw GpuException(GpuError::InvalidRequest, "create_image extents must be positive");
    auto owner = std::make_shared<Impl::Allocation>();
    owner->allocator = impl_;
    std::lock_guard lock(impl_->mutex);
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = depth == 1 ? (height == 1 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D) : VK_IMAGE_TYPE_3D;
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
    vkGetImageMemoryRequirements(impl_->vma->m_hDevice, owner->image, &requirements);
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
    viewInfo.viewType =
        depth == 1 ? (height == 1 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D) : VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    checkVulkan(vkCreateImageView(impl_->vma->m_hDevice, &viewInfo, nullptr, &owner->view), "vkCreateImageView");
    return Image(owner, owner->image, owner->view, format, info.extent, depth == 1 ? (height == 1 ? 1u : 2u) : 3u);
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
