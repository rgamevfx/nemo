#include "nemo/gpu/Allocator.hpp"

#include <cstdlib>
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
    // VkBuffer -> its VMA allocation, so public Buffer handles stay free of
    // VMA types.
    std::unordered_map<VkBuffer, VmaAllocation> allocations;
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
    // Buffers must be released before their allocator (RAII ordering: the
    // allocator outlives buffers in the same scope). Abort loudly rather than
    // silently leaking device memory.
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

Buffer Allocator::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
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
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    checkVulkan(vmaCreateBuffer(impl_->vma, &buffer_info, &alloc_info, &buffer, &allocation, nullptr),
                "vmaCreateBuffer");

    impl_->charged += requested;
    impl_->allocations.emplace(buffer, allocation);
    return Buffer(this, buffer, size);
}

void Allocator::release_buffer(Buffer& buffer) {
    const auto allocation = impl_->allocations.find(buffer.handle());
    if (allocation == impl_->allocations.end() || buffer.allocator_ != this) {
        return;
    }
    impl_->charged -= static_cast<uint64_t>(buffer.size());
    vmaDestroyBuffer(impl_->vma, buffer.handle(), allocation->second);
    impl_->allocations.erase(allocation);
    buffer.allocator_ = nullptr;
    buffer.buffer_ = VK_NULL_HANDLE;
    buffer.size_ = 0;
}

Buffer::~Buffer() {
    if (allocator_ != nullptr) {
        allocator_->release_buffer(*this);
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(std::exchange(other.allocator_, nullptr)), buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)) {}

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
    }
    return *this;
}

}  // namespace nemo::gpu
