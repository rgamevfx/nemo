#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
    // exceed this many bytes plus the bytes held by open reservations. Every
    // charge is the driver's reported memory requirement for the created
    // object, whether it lands in device-local or host-visible memory (the
    // one budget covers both placements, and the two are counted separately).
    // VMA handles the actual memory blocks; Nemo decides what is allowed
    // through.
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
    // Bytes charged to the allocator for this allocation: the driver's
    // reported memory requirement, which is at least `size()`. The charge is
    // held until the last owner of the allocation — this handle or any token
    // from retain() — is destroyed.
    [[nodiscard]] uint64_t charged_bytes() const { return charged_; }
    // Persistently mapped host pointer for HostMapped buffers; nullptr for
    // device buffers. Valid for the buffer's lifetime.
    [[nodiscard]] void* mapped() const { return mapped_; }
    [[nodiscard]] std::shared_ptr<const void> retain() const { return owner_; }

private:
    friend class Allocator;
    Buffer(std::shared_ptr<const void> owner, VkBuffer buffer, VkDeviceSize size, uint64_t charged,
           void* mapped = nullptr)
        : owner_(std::move(owner)), buffer_(buffer), size_(size), charged_(charged), mapped_(mapped) {}

    std::shared_ptr<const void> owner_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    uint64_t charged_ = 0;
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
    // Bytes charged to the allocator for this image: the driver's reported
    // memory requirement. Both images of a shared presentation allocation
    // report that allocation's single charge. The charge is held until the
    // last owner of the allocation — this handle or any token from retain()
    // — is destroyed.
    [[nodiscard]] uint64_t charged_bytes() const { return charged_; }
    [[nodiscard]] std::shared_ptr<const void> retain() const { return owner_; }

private:
    friend class Allocator;
    Image(std::shared_ptr<const void> owner, VkImage image, VkImageView view, VkFormat format, VkExtent3D extent,
          uint32_t dimensions, uint64_t charged)
        : owner_(std::move(owner)), image_(image), view_(view), format_(format), extent_(extent),
          dimensions_(dimensions), charged_(charged) {}

    std::shared_ptr<const void> owner_;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent3D extent_{};
    uint32_t dimensions_ = 0;
    uint64_t charged_ = 0;
};

// Budgeted allocation wrapper over VulkanMemoryAllocator (VMA). Nemo owns
// configured-budget admission and accounting; VMA is the allocation
// machinery behind a narrow RAII interface and is never exposed publicly.
// Bootstrap allocation and lifetime behavior (issue #2) extended with
// host-mapped buffers, LUT images (issue #6) and the byte admission,
// reservation and eligible-eviction capability (narrow #97 prerequisite for
// #106). No custom suballocation or pooling.
//
// Admission: every charge is the driver's reported memory requirement for
// the created object. A request that does not fit the combined active working
// set (live charges plus open reservations) first asks the registered
// eligible-eviction sources to release reclaimable residency, then throws
// BudgetExceeded naming the request, what is charged/reserved and the budget.
// A Reservation holds budget for a working set that is allocated in several
// steps, so nothing can take an intermediate cost between the requirement
// query and the allocation it belongs to.
//
// Lifetime: Instance and Device outlive all retained allocations. Allocation
// tokens retain the VMA state, even after the Allocator wrapper is destroyed.
// A charge is released only when the last owner of an allocation — its handle
// or any token from retain(), including one retained by a submission — is
// destroyed; this interface never releases an active allocation early.
// Threading: creation, retirement, and accounting are synchronized. Clients
// synchronize host writes and GPU access to each resource independently.
// Eviction callbacks run on the requesting thread with no allocator lock
// held; see register_evictor.
class Allocator {
private:
    struct Impl;
    struct EvictorEntry;

public:
    // Pass-key construction: `Token` is a private type, so callers outside
    // this header cannot construct an Allocator without going through
    // create().
    struct Token;

    // An eligible-eviction source: `wantedBytes` is how much the pending
    // request still needs, and the return value is how many bytes the
    // callback actually released (0 when nothing eligible could be freed).
    //
    // The callback runs on the thread that requested the allocation, with no
    // allocator accounting lock held — the only lock taken here is that
    // registration's own, which also serializes it against deregistration. It
    // may take its owner's locks (including blocking ones) and may allocate
    // through other owners, but it must not call back into this Allocator
    // (create_*, reserve, register_evictor): those would wait for a lock this
    // thread already holds. A callback whose own caller already holds the
    // lock it would need to release residency must not block on it; taking it
    // with try_lock and returning 0 is the honest answer (admission then
    // reports the real limitation instead of deadlocking).
    using Evictor = std::function<uint64_t(uint64_t wantedBytes)>;

    // Move-only RAII registration of one evictor. Deregistering (reset() or
    // destruction) removes the registration and returns only after a callback
    // invocation that had already started has finished, so the registrant's
    // captured state stays valid until then.
    class EvictorHandle {
    public:
        EvictorHandle() = default;
        ~EvictorHandle();

        EvictorHandle(EvictorHandle&& other) noexcept;
        EvictorHandle& operator=(EvictorHandle&& other) noexcept;

        EvictorHandle(const EvictorHandle&) = delete;
        EvictorHandle& operator=(const EvictorHandle&) = delete;

        [[nodiscard]] bool registered() const { return entry_ != nullptr; }
        void reset();

    private:
        friend class Allocator;
        EvictorHandle(std::shared_ptr<Impl> allocator, std::shared_ptr<EvictorEntry> entry);

        std::shared_ptr<Impl> allocator_;
        std::shared_ptr<EvictorEntry> entry_;
    };

    // Move-only RAII reservation of `bytes` of the budget for a combined
    // active working set. The bytes count against the budget from reserve()
    // until matching allocations consume them. Each successful create_* takes
    // its own charge from the token; the remainder stays reserved for later
    // allocations in the same working set. A create_* that throws leaves the
    // token intact, and release()/destruction returns the remainder. One token
    // is used by one caller at a time.
    class Reservation {
    public:
        Reservation() = default;
        ~Reservation();

        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;

        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        // Bytes still held by this token; zero once consumed or released.
        [[nodiscard]] uint64_t bytes() const { return bytes_; }
        void release();

    private:
        friend class Allocator;
        Reservation(std::shared_ptr<Impl> allocator, uint64_t bytes);

        std::shared_ptr<Impl> allocator_;
        uint64_t bytes_ = 0;
        bool valid_ = false;
    };

    explicit Allocator(Token);
    static std::unique_ptr<Allocator> create(Instance& instance, Device& device, const AllocatorConfig& config = {});
    ~Allocator();

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    // Bound on registered eviction sources; registering more is a request
    // error rather than an unbounded callback set.
    static constexpr std::size_t kMaxEvictors = 8;

    [[nodiscard]] uint64_t budget() const;
    // Live charges against the budget, split by the placement of the
    // allocation (device-local vs host-visible), and the bytes held by
    // unconsumed reservations. charged_bytes() is the sum of the two
    // placements; the budget also covers reserved_bytes().
    [[nodiscard]] uint64_t charged_bytes() const;
    [[nodiscard]] uint64_t device_charged_bytes() const;
    [[nodiscard]] uint64_t host_charged_bytes() const;
    [[nodiscard]] uint64_t reserved_bytes() const;

    // The driver's reported memory requirement for the request create_* makes
    // with the same arguments: what admission and reservation must account
    // for, never a payload estimate. Same validation as create_*.
    [[nodiscard]] uint64_t buffer_allocation_bytes(VkDeviceSize size, VkBufferUsageFlags usage) const;
    [[nodiscard]] uint64_t image_allocation_bytes(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                                  VkImageUsageFlags usage, uint32_t dimensions = 0) const;

    // Reserves budget before the working set is allocated. Throws
    // BudgetExceeded after eligible eviction cannot make the reservation fit,
    // and InvalidRequest for a zero-byte reservation.
    [[nodiscard]] Reservation reserve(uint64_t bytes);

    // Registers an eviction source used by admission. Throws InvalidRequest
    // for an empty callback or when kMaxEvictors registrations already exist.
    [[nodiscard]] EvictorHandle register_evictor(Evictor evict);

    // Creates a device-preferred or host-mapped buffer of `size` bytes. The
    // budget is charged with the driver's reported memory requirement.
    // `reservation`, when given, must be a nonempty token from this allocator;
    // this allocation consumes up to its charge from the remaining reservation.
    // Throws BudgetExceeded
    // naming the request, the live charge/reservations and the budget when
    // the request does not fit even after eligible eviction; throws a
    // descriptive GpuException when the underlying allocation fails.
    [[nodiscard]] Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                       MemoryPreference preference = MemoryPreference::Device,
                                       Reservation* reservation = nullptr);

    // Creates an optimal-tiling image with one mip level and one layer and a
    // matching COLOR-aspect view. dimensions=0 infers dimensionality from
    // extents. Image-processing callers pass 2 explicitly, including
    // one-row/one-pixel representations. Charged and reserved like
    // create_buffer: the budget is charged with the driver's reported memory
    // requirement.
    [[nodiscard]] Image create_image(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                     VkImageUsageFlags usage, uint32_t dimensions = 0,
                                     Reservation* reservation = nullptr);

    // Dedicated external-memory presentation allocation on two logical
    // devices of the same physical GPU. Both 2D images have identical
    // metadata and share one retained owner. Physical bytes are charged once
    // to this allocator; either image retains both sides and that charge, and
    // both report it through charged_bytes(). Admits like create_buffer
    // (including eligible eviction) but takes no reservation. Both devices
    // must outlive all tokens. No queue operations or readback.
    [[nodiscard]] std::pair<Image, Image> create_shared_image(Device& consumer, uint32_t width, uint32_t height,
                                                              VkFormat format, VkImageUsageFlags usage);

private:
    // Admission around an allocation: consumes `*reservation`'s coverage,
    // invokes registered evictors without any allocator lock held when the
    // combined active working set does not fit, and re-checks bounded times
    // before throwing BudgetExceeded. `allocate` runs while the accounting
    // lock is held, creates the Vulkan objects, verifies the requirement and
    // charges the budget (chargeLocked); it returns the created token.
    template <typename Allocate>
    auto admit(const char* what, uint64_t bytes, Reservation* reservation, Allocate&& allocate) -> decltype(allocate());

    // Requires impl_->mutex: how much of `bytes` the reservation covers,
    // validating that the token is this allocator's own unconsumed token.
    uint64_t reservationCoverage(const char* what, uint64_t bytes, Reservation* reservation) const;
    // Requires impl_->mutex: charges `bytes` to the placement and consumes
    // the same amount from `*reservation`.
    void chargeLocked(uint64_t bytes, bool host, Reservation* reservation);
    // Invokes every registered evictor without the accounting lock held.
    uint64_t evict(uint64_t wanted);

    std::shared_ptr<Impl> impl_;
};

}  // namespace nemo::gpu
