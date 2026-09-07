#include "nemo/gpu/Allocator.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
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
    // Buffer/image handles -> their VMA allocations, so public handles stay
    // free of VMA types.
    std::unordered_map<VkBuffer, VmaAllocation> buffer_allocations;
    std::unordered_map<VkImage, VmaAllocation> image_allocations;
};

std::unique_ptr<Allocator> Allocator::create(Instance& instance, Device& device, const AllocatorConfig& config) {
    auto allocator = std::make_unique<Allocator>(Token{});
    allocator->impl_ = std::make_unique<Impl>();
    allocator->impl_->budget = config.max_device_bytes;

    VmaAllocatorCreateInfo info{};
    info.physicalDevice = device.physical();
    info.device = device.handle();
    info.instance = instance.handle();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    checkVulkan(vmaCreateAllocator(&info, &allocator->impl_->vma), "vmaCreateAllocator");
    return allocator;
}

Allocator::~Allocator() {
    if (!impl_) {
        return;
    }
    // Allocations must be released before their allocator (RAII ordering:
    // the allocator outlives buffers and images in the same scope). Abort
    // loudly rather than silently leaking device memory.
    if (impl_->charged != 0) {
        std::abort();
    }
    if (impl_->vma != VK_NULL_HANDLE) {
        vmaDestroyAllocator(impl_->vma);
    }
}

uint64_t Allocator::budget() const {
    return impl_->budget;
}

uint64_t Allocator::charged_bytes() const {
    return impl_->charged;
}

Buffer Allocator::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryPreference preference) {
    const uint64_t requested = static_cast<uint64_t>(size);
    if (size == 0) {
        throw GpuException(GpuError::InvalidRequest, "create_buffer size must be positive");
    }
    // Invariant: charged <= budget. Comparing without an addition keeps the
    // admission check overflow-safe.
    if (requested > impl_->budget - impl_->charged) {
        throw GpuException(GpuError::BudgetExceeded, "buffer request of " + std::to_string(requested) + " bytes plus " +
                                                         std::to_string(impl_->charged) +
                                                         " already charged exceeds the " +
                                                         std::to_string(impl_->budget) + " byte allocation budget");
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    if (preference == MemoryPreference::HostMapped) {
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info{};
    checkVulkan(vmaCreateBuffer(impl_->vma, &buffer_info, &alloc_info, &buffer, &allocation, &allocation_info),
                "vmaCreateBuffer");

    impl_->charged += requested;
    impl_->buffer_allocations.emplace(buffer, allocation);
    return Buffer(this, buffer, size, allocation_info.pMappedData);
}

Image Allocator::create_image(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                              VkImageUsageFlags usage) {
    if (width == 0 || height == 0 || depth == 0) {
        throw GpuException(GpuError::InvalidRequest, "create_image extents must be positive");
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = depth == 1 ? (height == 1 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D) : VK_IMAGE_TYPE_3D;
    image_info.format = format;
    image_info.extent = {width, height, depth};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    checkVulkan(vmaCreateImage(impl_->vma, &image_info, &alloc_info, &image, &allocation, nullptr), "vmaCreateImage");

    // Charge with the driver-reported requirement (requested sizes have no
    // meaning for optimal-tiling images).
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(impl_->vma->m_hDevice, image, &requirements);
    const uint64_t bytes = requirements.size;
    if (bytes > impl_->budget - impl_->charged) {
        Image overage(this, image, VK_NULL_HANDLE, format, image_info.extent,
                      depth == 1 ? (height == 1 ? 1u : 2u) : 3u);
        release_image(overage);
        throw GpuException(GpuError::BudgetExceeded, "image request of " + std::to_string(bytes) + " bytes plus " +
                                                         std::to_string(impl_->charged) +
                                                         " already charged exceeds the " +
                                                         std::to_string(impl_->budget) + " byte allocation budget");
    }
    impl_->charged += bytes;
    impl_->image_allocations.emplace(image, allocation);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType =
        depth == 1 ? (height == 1 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D) : VK_IMAGE_VIEW_TYPE_3D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    checkVulkan(vkCreateImageView(impl_->vma->m_hDevice, &view_info, nullptr, &view), "vkCreateImageView");

    return Image(this, image, view, format, image_info.extent, depth == 1 ? (height == 1 ? 1u : 2u) : 3u);
}

void Allocator::release_buffer(Buffer& buffer) {
    const auto allocation = impl_->buffer_allocations.find(buffer.handle());
    if (allocation == impl_->buffer_allocations.end() || buffer.allocator_ != this) {
        return;
    }
    impl_->charged -= static_cast<uint64_t>(buffer.size());
    vmaDestroyBuffer(impl_->vma, buffer.handle(), allocation->second);
    impl_->buffer_allocations.erase(allocation);
    buffer.allocator_ = nullptr;
    buffer.buffer_ = VK_NULL_HANDLE;
    buffer.size_ = 0;
    buffer.mapped_ = nullptr;
}

Buffer::~Buffer() {
    if (allocator_ != nullptr) {
        allocator_->release_buffer(*this);
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(std::exchange(other.allocator_, nullptr)), buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)), mapped_(std::exchange(other.mapped_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        // Release any currently held buffer against its own allocator before
        // taking over the other's.
        if (allocator_ != nullptr) {
            allocator_->release_buffer(*this);
        }
        allocator_ = std::exchange(other.allocator_, nullptr);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

void Allocator::release_image(Image& image) {
    const auto allocation = impl_->image_allocations.find(image.handle());
    if (allocation == impl_->image_allocations.end() || image.allocator_ != this) {
        return;
    }
    if (image.view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(impl_->vma->m_hDevice, image.view_, nullptr);
    }
    // Read the driver-reported size before destroying the allocation: the
    // charge was taken from the memory requirements at creation.
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(impl_->vma, allocation->second, &info);
    impl_->charged -= static_cast<uint64_t>(info.size);
    vmaDestroyImage(impl_->vma, image.handle(), allocation->second);
    impl_->image_allocations.erase(allocation);
    image.allocator_ = nullptr;
    image.image_ = VK_NULL_HANDLE;
    image.view_ = VK_NULL_HANDLE;
}

Image::~Image() {
    if (allocator_ != nullptr) {
        allocator_->release_image(*this);
    }
}

Image::Image(Image&& other) noexcept
    : allocator_(std::exchange(other.allocator_, nullptr)), image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, VkExtent3D{})), dimensions_(std::exchange(other.dimensions_, 0u)) {}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        if (allocator_ != nullptr) {
            allocator_->release_image(*this);
        }
        allocator_ = std::exchange(other.allocator_, nullptr);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_ = std::exchange(other.extent_, VkExtent3D{});
        dimensions_ = std::exchange(other.dimensions_, 0u);
    }
    return *this;
}

}  // namespace nemo::gpu
