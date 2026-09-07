#pragma once

#include <memory>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Device.hpp"

namespace nemo::gpu {

struct AllocatorConfig {
    // Admission budget owned by Nemo: no outstanding allocation set may
    // exceed this many bytes (measured as requested sizes). VMA handles the
    // actual memory blocks; Nemo decides what is allowed through.
    uint64_t max_device_bytes = 0;
};

// A created device buffer. Move-only RAII handle: destruction releases the
// underlying VMA allocation and uncharges it from the owning Allocator.
// VMA types stay private to the module; the public surface only carries
// Vulkan handles and sizes.
class Buffer {
public:
    Buffer() = default;
    ~Buffer();

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }

private:
    friend class Allocator;
    Buffer(class Allocator* allocator, VkBuffer buffer, VkDeviceSize size)
        : allocator_(allocator), buffer_(buffer), size_(size) {}

    Allocator* allocator_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
};

// Budgeted allocation wrapper over VulkanMemoryAllocator (VMA). Nemo owns
// configured-budget admission and accounting; VMA is the allocation
// machinery behind a narrow RAII interface and is never exposed publicly.
// Issue #2 scope: bootstrap allocation and lifetime behavior only — no
// custom suballocation, eviction, or pooling yet.
//
// Lifetime contract: `instance` and `device` must outlive the Allocator;
// created Buffers must outlive their Allocator (RAII ordering in a shared
// scope gives both; the destructor aborts in debug if buffers are still
// charged).
//
// Threading contract: not thread-safe in this bootstrap; revisit and bound
// concurrent allocation when evaluation (#8) starts submitting work.
class Allocator {
public:
    // Pass-key construction: `Token` is a private type, so callers outside
    // this header cannot construct an Allocator without going through
    // create().
    struct Token;
    explicit Allocator(Token);
    static std::unique_ptr<Allocator> create(Instance& instance, Device& device, const AllocatorConfig& config = {});
    ~Allocator();

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    [[nodiscard]] uint64_t budget() const;
    // Sum of requested sizes for live allocations charged against the budget.
    [[nodiscard]] uint64_t charged_bytes() const;

    // Creates a device-preferred buffer of `size` bytes. Throws BudgetExceeded
    // naming the requested size, the live charge, and the budget when the
    // request does not fit; throws a descriptive GpuException when the
    // underlying allocation fails.
    [[nodiscard]] Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage);

private:
    friend class Buffer;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void release_buffer(Buffer& buffer);
};

}  // namespace nemo::gpu
