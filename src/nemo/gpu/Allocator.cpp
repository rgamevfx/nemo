#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/ExternalHandle.hpp"

#include <algorithm>
#include <array>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace nemo::gpu {

namespace {

// Eviction rounds one admission may trigger before it fails honestly: enough
// for a released working set to be reused, bounded so two callers evicting
// for each other cannot spin.
constexpr unsigned kMaxEvictionAttempts = 4;

// The create info every buffer allocation and requirement query uses, so both
// describe the same object.
VkBufferCreateInfo bufferCreateInfo(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return info;
}

// Validates extents and resolves the default dimensionality; shared by
// create_image and image_allocation_bytes so both describe the same image.
VkImageCreateInfo imageCreateInfo(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                  VkImageUsageFlags usage, uint32_t& dimensions) {
    if (width == 0 || height == 0 || depth == 0)
        throw GpuException(GpuError::InvalidRequest, "create_image extents must be positive");
    if (dimensions == 0)
        dimensions = depth > 1 ? 3u : height > 1 ? 2u : 1u;
    if (dimensions > 3 || (dimensions < 3 && depth != 1) || (dimensions == 1 && height != 1))
        throw GpuException(GpuError::InvalidRequest, "create_image dimensionality disagrees with extents");
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
    return info;
}

// The driver's memory requirement, queried on a throwaway object: admission
// and reservation must know the real cost before anything is allocated, and
// the requirement is never a payload guess (issue #97). The query describes
// exactly the object the matching create_* allocates.
VkMemoryRequirements bufferRequirements(VkDevice device, const VkBufferCreateInfo& info) {
    VkBuffer buffer = VK_NULL_HANDLE;
    checkVulkan(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer (requirement query)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    vkDestroyBuffer(device, buffer, nullptr);
    return requirements;
}

VkMemoryRequirements imageRequirements(VkDevice device, const VkImageCreateInfo& info) {
    VkImage image = VK_NULL_HANDLE;
    checkVulkan(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage (requirement query)");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    vkDestroyImage(device, image, nullptr);
    return requirements;
}

// Honest rejection: the request, what is already accounted and the budget.
std::string budgetRejection(const char* what, uint64_t bytes, uint64_t charged, uint64_t reserved, uint64_t budget) {
    return std::string(what) + " request of " + std::to_string(bytes) + " bytes plus " + std::to_string(charged) +
           " already charged and " + std::to_string(reserved) + " reserved exceeds the " + std::to_string(budget) +
           " byte allocation budget";
}

}  // namespace

struct Allocator::Token {};
Allocator::Allocator(Token) {}

struct Allocator::EvictorEntry {
    Allocator::Evictor evict;
    // Held while the callback runs, so deregistration can wait for an
    // invocation that already started before the registrant destroys the
    // state it captured. No allocator lock is ever held while this one is
    // taken, and no allocator lock is taken by the callback's own work.
    std::recursive_mutex mutex;
};

struct Allocator::Impl {
    uint64_t budget = 0;
    uint64_t charged = 0;
    uint64_t deviceCharged = 0;
    uint64_t hostCharged = 0;
    uint64_t reserved = 0;
    VmaAllocator vma = VK_NULL_HANDLE;
    // Accounting and VMA. Never held while an eviction callback runs.
    std::mutex mutex;
    // The registration list, held only long enough to add, snapshot or
    // remove a registration.
    std::mutex evictorMutex;
    std::vector<std::shared_ptr<EvictorEntry>> evictors;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    bool externalSharing = false;

    // Bytes that may still be charged by an allocation or reservation.
    // Callers hold `mutex`.
    uint64_t free_bytes() const {
        const uint64_t used = charged + reserved;
        return budget > used ? budget - used : 0;
    }

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
        // Charge and placement of this allocation, released only when the
        // last owner of the token is destroyed.
        uint64_t charge = 0;
        bool host = false;
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
            (host ? allocator->hostCharged : allocator->deviceCharged) -= charge;
        }
    };
    struct SharedAllocation {
        std::shared_ptr<Impl> allocator;
        std::array<VkDevice, 2> devices{};
        std::array<VkDeviceMemory, 2> memory{};
        std::array<VkImage, 2> images{};
        std::array<VkImageView, 2> views{};
        // The physical bytes of the one presentation allocation, charged
        // once however many tokens retain it.
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
            allocator->deviceCharged -= charge;
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
uint64_t Allocator::device_charged_bytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->deviceCharged;
}
uint64_t Allocator::host_charged_bytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->hostCharged;
}
uint64_t Allocator::reserved_bytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->reserved;
}

uint64_t Allocator::buffer_allocation_bytes(VkDeviceSize size, VkBufferUsageFlags usage) const {
    if (size == 0)
        throw GpuException(GpuError::InvalidRequest, "create_buffer size must be positive");
    return bufferRequirements(impl_->device, bufferCreateInfo(size, usage)).size;
}

uint64_t Allocator::image_allocation_bytes(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                           VkImageUsageFlags usage, uint32_t dimensions) const {
    return imageRequirements(impl_->device, imageCreateInfo(width, height, depth, format, usage, dimensions)).size;
}

Allocator::Reservation::Reservation(std::shared_ptr<Impl> allocator, uint64_t bytes)
    : allocator_(std::move(allocator)), bytes_(bytes), valid_(true) {}
Allocator::Reservation::~Reservation() {
    release();
}
Allocator::Reservation::Reservation(Reservation&& other) noexcept
    : allocator_(std::move(other.allocator_)), bytes_(std::exchange(other.bytes_, 0)),
      valid_(std::exchange(other.valid_, false)) {}
Allocator::Reservation& Allocator::Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        release();
        allocator_ = std::move(other.allocator_);
        bytes_ = std::exchange(other.bytes_, 0);
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}
void Allocator::Reservation::release() {
    const uint64_t bytes = bytes_;
    bytes_ = 0;
    valid_ = false;
    if (allocator_ && bytes != 0) {
        std::lock_guard lock(allocator_->mutex);
        allocator_->reserved -= bytes;
    }
}

Allocator::EvictorHandle::EvictorHandle(std::shared_ptr<Impl> allocator, std::shared_ptr<EvictorEntry> entry)
    : allocator_(std::move(allocator)), entry_(std::move(entry)) {}
Allocator::EvictorHandle::~EvictorHandle() {
    reset();
}
Allocator::EvictorHandle::EvictorHandle(EvictorHandle&& other) noexcept
    : allocator_(std::move(other.allocator_)), entry_(std::move(other.entry_)) {}
Allocator::EvictorHandle& Allocator::EvictorHandle::operator=(EvictorHandle&& other) noexcept {
    if (this != &other) {
        reset();
        allocator_ = std::move(other.allocator_);
        entry_ = std::move(other.entry_);
    }
    return *this;
}
void Allocator::EvictorHandle::reset() {
    if (!allocator_ || !entry_) {
        allocator_.reset();
        entry_.reset();
        return;
    }
    auto allocator = std::move(allocator_);
    auto entry = std::move(entry_);
    {
        std::lock_guard lock(allocator->evictorMutex);
        auto& registered = allocator->evictors;
        registered.erase(std::remove(registered.begin(), registered.end(), entry), registered.end());
    }
    // An invocation that already started may still be touching the state the
    // registrant destroys next; wait for it before returning.
    std::lock_guard lock(entry->mutex);
    entry->evict = {};
}

uint64_t Allocator::reservationCoverage(const char* what, uint64_t bytes, Reservation* reservation) const {
    if (!reservation)
        return 0;
    if (reservation->allocator_.get() != impl_.get())
        throw GpuException(GpuError::InvalidRequest,
                           std::string(what) + " reservation belongs to a different allocator");
    if (!reservation->valid_)
        throw GpuException(GpuError::InvalidRequest,
                           std::string(what) + " reservation token is empty (already consumed or released)");
    return std::min(bytes, reservation->bytes_);
}

void Allocator::chargeLocked(uint64_t bytes, bool host, Reservation* reservation) {
    if (reservation) {
        const uint64_t covered = std::min(bytes, reservation->bytes_);
        reservation->bytes_ -= covered;
        impl_->reserved -= covered;
        // A working-set token remains usable until its remaining bytes are spent.
        reservation->valid_ = reservation->bytes_ != 0;
    }
    impl_->charged += bytes;
    (host ? impl_->hostCharged : impl_->deviceCharged) += bytes;
}

uint64_t Allocator::evict(uint64_t wanted) {
    std::array<std::shared_ptr<EvictorEntry>, kMaxEvictors> registered;
    {
        std::lock_guard lock(impl_->evictorMutex);
        std::copy(impl_->evictors.begin(), impl_->evictors.end(), registered.begin());
    }
    uint64_t freed = 0;
    for (const auto& entry : registered) {
        if (!entry || freed >= wanted)
            break;
        // No allocator lock is held here: the callback releases its own
        // eligible residency, which destroys allocation tokens.
        std::lock_guard lock(entry->mutex);
        if (entry->evict)
            freed += entry->evict(wanted - freed);
    }
    return freed;
}

template <typename Allocate>
auto Allocator::admit(const char* what, uint64_t bytes, Reservation* reservation,
                      Allocate&& allocate) -> decltype(allocate()) {
    for (unsigned attempt = 0;; ++attempt) {
        uint64_t shortfall = 0;
        std::string rejection;
        {
            std::lock_guard lock(impl_->mutex);
            const uint64_t covered = reservationCoverage(what, bytes, reservation);
            if (bytes - covered <= impl_->free_bytes())
                return allocate();
            shortfall = bytes - covered - impl_->free_bytes();
            rejection = budgetRejection(what, bytes, impl_->charged, impl_->reserved, impl_->budget);
        }
        if (attempt >= kMaxEvictionAttempts || evict(shortfall) == 0)
            throw GpuException(GpuError::BudgetExceeded, std::move(rejection));
    }
}

Allocator::Reservation Allocator::reserve(uint64_t bytes) {
    if (bytes == 0)
        throw GpuException(GpuError::InvalidRequest, "reserve requires a positive byte count");
    for (unsigned attempt = 0;; ++attempt) {
        uint64_t shortfall = 0;
        std::string rejection;
        {
            std::lock_guard lock(impl_->mutex);
            if (bytes <= impl_->free_bytes()) {
                impl_->reserved += bytes;
                return Reservation(impl_, bytes);
            }
            shortfall = bytes - impl_->free_bytes();
            rejection = budgetRejection("reservation", bytes, impl_->charged, impl_->reserved, impl_->budget);
        }
        if (attempt >= kMaxEvictionAttempts || evict(shortfall) == 0)
            throw GpuException(GpuError::BudgetExceeded, std::move(rejection));
    }
}

Allocator::EvictorHandle Allocator::register_evictor(Evictor evict) {
    if (!evict)
        throw GpuException(GpuError::InvalidRequest, "register_evictor requires a callback");
    auto entry = std::make_shared<EvictorEntry>();
    entry->evict = std::move(evict);
    {
        std::lock_guard lock(impl_->evictorMutex);
        if (impl_->evictors.size() >= kMaxEvictors)
            throw GpuException(GpuError::InvalidRequest,
                               "at most " + std::to_string(kMaxEvictors) + " eligible evictors may be registered");
        impl_->evictors.push_back(entry);
    }
    return EvictorHandle(impl_, std::move(entry));
}

Buffer Allocator::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryPreference preference,
                                Reservation* reservation) {
    const uint64_t bytes = buffer_allocation_bytes(size, usage);
    // Allocate the owner before Vulkan work. It is destroyed after any lock
    // this function takes, so partial allocations retire safely.
    auto owner = std::make_shared<Impl::Allocation>();
    owner->allocator = impl_;
    owner->host = preference == MemoryPreference::HostMapped;
    const VkBufferCreateInfo info = bufferCreateInfo(size, usage);
    VmaAllocationCreateInfo allocationInfo{};
    if (preference == MemoryPreference::HostMapped) {
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    return admit("buffer", bytes, reservation, [&] {
        VmaAllocationInfo mappedInfo{};
        checkVulkan(
            vmaCreateBuffer(impl_->vma, &info, &allocationInfo, &owner->buffer, &owner->allocation, &mappedInfo),
            "vmaCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(impl_->device, owner->buffer, &requirements);
        if (requirements.size != bytes)
            throw GpuException(GpuError::VulkanError,
                               "vmaCreateBuffer produced different memory requirements than the requirement query");
        owner->charge = bytes;
        chargeLocked(bytes, owner->host, reservation);
        return Buffer(owner, owner->buffer, size, bytes, mappedInfo.pMappedData);
    });
}

Image Allocator::create_image(uint32_t width, uint32_t height, uint32_t depth, VkFormat format, VkImageUsageFlags usage,
                              uint32_t dimensions, Reservation* reservation) {
    const VkImageCreateInfo info = imageCreateInfo(width, height, depth, format, usage, dimensions);
    const uint64_t bytes = imageRequirements(impl_->device, info).size;
    auto owner = std::make_shared<Impl::Allocation>();
    owner->allocator = impl_;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return admit("image", bytes, reservation, [&] {
        checkVulkan(vmaCreateImage(impl_->vma, &info, &allocationInfo, &owner->image, &owner->allocation, nullptr),
                    "vmaCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(impl_->device, owner->image, &requirements);
        if (requirements.size != bytes)
            throw GpuException(GpuError::VulkanError,
                               "vmaCreateImage produced different memory requirements than the requirement query");
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = owner->image;
        viewInfo.viewType = dimensions == 1   ? VK_IMAGE_VIEW_TYPE_1D
                            : dimensions == 2 ? VK_IMAGE_VIEW_TYPE_2D
                                              : VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checkVulkan(vkCreateImageView(impl_->device, &viewInfo, nullptr, &owner->view), "vkCreateImageView");
        // Charge last: a failure above leaves the reservation intact and the
        // budget untouched.
        owner->charge = bytes;
        chargeLocked(bytes, false, reservation);
        return Image(owner, owner->image, owner->view, format, info.extent, dimensions, bytes);
    });
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
    // Both sides' requirements are queried before anything is allocated, so
    // admission (and eligible eviction) precedes every Vulkan allocation here
    // too, and the physical bytes are accounted as the driver reports them.
    const std::array<VkDevice, 2> devices{impl_->device, consumer.handle()};
    std::array<VkMemoryRequirements, 2> requirements{};
    for (unsigned i = 0; i != 2; ++i)
        requirements[i] = imageRequirements(devices[i], imageInfo);
    const uint64_t bytes = std::max(requirements[0].size, requirements[1].size);
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
    // Keep partial ownership outside admission's accounting lock: any Vulkan
    // failure must unwind that lock before releasing the shared allocation.
    auto owner = std::make_shared<Impl::SharedAllocation>();
    owner->allocator = impl_;
    owner->devices = {impl_->device, consumer.handle()};
    return admit("shared presentation image", bytes, nullptr, [&] {
        for (unsigned i = 0; i != 2; ++i) {
            checkVulkan(vkCreateImage(owner->devices[i], &imageInfo, nullptr, &owner->images[i]),
                        "vkCreateImage(shared)");
            VkMemoryRequirements allocated{};
            vkGetImageMemoryRequirements(owner->devices[i], owner->images[i], &allocated);
            if (allocated.size != requirements[i].size || allocated.memoryTypeBits != requirements[i].memoryTypeBits)
                throw GpuException(
                    GpuError::VulkanError,
                    "shared presentation image memory requirements changed between the requirement query "
                    "and the allocation");
        }

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
        checkVulkan(vkBindImageMemory(impl_->device, owner->images[0], owner->memory[0], 0),
                    "vkBindImageMemory(export)");
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
        checkVulkan(vkAllocateMemory(consumer.handle(), &allocate, nullptr, &owner->memory[1]),
                    "vkAllocateMemory(import)");
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
        owner->charge = bytes;
        chargeLocked(bytes, false, nullptr);
        return std::pair{Image(owner, owner->images[0], owner->views[0], format, imageInfo.extent, 2, bytes),
                         Image(owner, owner->images[1], owner->views[1], format, imageInfo.extent, 2, bytes)};
    });
}

Buffer::~Buffer() = default;
Buffer::Buffer(Buffer&& other) noexcept
    : owner_(std::move(other.owner_)), buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)), charged_(std::exchange(other.charged_, 0)),
      mapped_(std::exchange(other.mapped_, nullptr)) {}
Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        charged_ = std::exchange(other.charged_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}
Image::~Image() = default;
Image::Image(Image&& other) noexcept
    : owner_(std::move(other.owner_)), image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, VkExtent3D{})), dimensions_(std::exchange(other.dimensions_, 0u)),
      charged_(std::exchange(other.charged_, 0)) {}
Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_ = std::exchange(other.extent_, VkExtent3D{});
        dimensions_ = std::exchange(other.dimensions_, 0u);
        charged_ = std::exchange(other.charged_, 0);
    }
    return *this;
}
}  // namespace nemo::gpu
