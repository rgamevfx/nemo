#pragma once

#include <memory>
#include <utility>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Device.hpp"

namespace nemo::gpu {

// Memory placement preference. Device: device-local, never mapped (upload
// via staging copies). HostMapped: host-visible and persistently mapped;
// used for staging and for small results that a validation path reads back
// (never for routine per-frame readback — spec section 10.2/11).
enum class MemoryPreference { Device, HostMapped };

struct AllocatorConfig {
    // Admission budget owned by Nemo: no outstanding allocation set may
    // exceed this many bytes (measured as requested sizes; images are
    // charged with the driver's reported memory requirement). VMA handles
    // the actual memory blocks; Nemo decides what is allowed through.
    uint64_t max_device_bytes = 0;
};

// Move-only application handle. retain() shares allocation ownership with
// submissions without making the writable handle copyable.
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
    // Persistently mapped host pointer for HostMapped buffers; nullptr for
    // device buffers. Valid for the buffer's lifetime.
    [[nodiscard]] void* mapped() const { return mapped_; }
    [[nodiscard]] std::shared_ptr<const void> retain() const { return owner_; }

private:
    friend class Allocator;
    Buffer(std::shared_ptr<const void> owner, VkBuffer buffer, VkDeviceSize size, void* mapped = nullptr)
        : owner_(std::move(owner)), buffer_(buffer), size_(size), mapped_(mapped) {}

    std::shared_ptr<const void> owner_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

// A created image with a matching COLOR-aspect view. Move-only RAII handle
// like Buffer. One mip level, one array layer, no cube faces — the
// LUT-texture shape this milestone needs.
class Image {
public:
    Image() = default;
    ~Image();

    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    [[nodiscard]] VkImage handle() const { return image_; }
    [[nodiscard]] VkImageView view() const { return view_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkExtent3D extent() const { return extent_; }
    // 1, 2, or 3 dimensions.
    [[nodiscard]] uint32_t dimensions() const { return dimensions_; }
    [[nodiscard]] std::shared_ptr<const void> retain() const { return owner_; }

private:
    friend class Allocator;
    Image(std::shared_ptr<const void> owner, VkImage image, VkImageView view, VkFormat format, VkExtent3D extent,
          uint32_t dimensions)
        : owner_(std::move(owner)), image_(image), view_(view), format_(format), extent_(extent),
          dimensions_(dimensions) {}

    std::shared_ptr<const void> owner_;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent3D extent_{};
    uint32_t dimensions_ = 0;
};

// Budgeted allocation wrapper over VulkanMemoryAllocator (VMA). Nemo owns
// configured-budget admission and accounting; VMA is the allocation
// machinery behind a narrow RAII interface and is never exposed publicly.
// Bootstrap allocation and lifetime behavior (issue #2) extended with
// host-mapped buffers and LUT images for the viewing transform (issue #6);
// no custom suballocation, eviction, or pooling yet.
//
// Lifetime: Instance and Device outlive all retained allocations. Allocation
// tokens retain the VMA state, even after the Allocator wrapper is destroyed.
// Threading: creation, retirement, and accounting are synchronized. Clients
// synchronize host writes and GPU access to each resource independently.
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

    // Creates a device-preferred or host-mapped buffer of `size` bytes.
    // Throws BudgetExceeded naming the requested size, the live charge, and
    // the budget when the request does not fit; throws a descriptive
    // GpuException when the underlying allocation fails.
    [[nodiscard]] Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                       MemoryPreference preference = MemoryPreference::Device);

    // Creates an optimal-tiling image with one mip level and one layer and
    // a matching COLOR-aspect view. The budget is charged with the driver's
    // dimensions=0 infers LUT dimensionality from extents. Image-processing
    // callers pass 2 explicitly, including one-row/one-pixel representations.
    // reported memory requirement. Throws BudgetExceeded / descriptive
    // GpuException like create_buffer.
    [[nodiscard]] Image create_image(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                     VkImageUsageFlags usage, uint32_t dimensions = 0);

    // Dedicated external-memory presentation allocation on two logical
    // devices of the same physical GPU. Both 2D images have identical
    // metadata and share one retained owner. Physical bytes are charged once
    // to this allocator; either image retains both sides and that charge.
    // Both devices must outlive all tokens. No queue operations or readback.
    [[nodiscard]] std::pair<Image, Image> create_shared_image(Device& consumer, uint32_t width, uint32_t height,
                                                              VkFormat format, VkImageUsageFlags usage);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace nemo::gpu
