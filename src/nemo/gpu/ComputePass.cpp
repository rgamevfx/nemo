#include "nemo/gpu/ComputePass.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nemo::gpu {

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& detail = {}) {
    throw GpuException(GpuError::VulkanError, "compute pass: " + what + (detail.empty() ? "" : ": " + detail));
}

[[nodiscard]] VkDescriptorType descriptorType(DescriptorKind kind) {
    switch (kind) {
    case DescriptorKind::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorKind::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorKind::CombinedImageSampler:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case DescriptorKind::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    fail("unknown descriptor kind");
}

// Normalized cache key: full SPIR-V plus, per set, the sorted packed
// (binding, kind) pairs. Descriptor content (which buffer/image a binding
// points at) is deliberately excluded — passes with the same layout shape
// share one pipeline.
struct CacheKey {
    std::vector<std::uint32_t> spirv;
    std::vector<std::vector<std::uint64_t>> sets;

    bool operator==(const CacheKey& other) const = default;

    [[nodiscard]] std::size_t hash() const {
        std::size_t seed = spirv.size();
        for (std::uint32_t word : spirv) {
            seed ^= std::hash<std::uint32_t>{}(word) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        }
        for (const auto& set : sets) {
            for (std::uint64_t packed : set) {
                seed ^= std::hash<std::uint64_t>{}(packed) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            }
            seed ^= 0x51ed270b + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

[[nodiscard]] uint64_t packedBinding(uint32_t binding, DescriptorKind kind) {
    return (static_cast<uint64_t>(binding) << 32) | static_cast<uint32_t>(kind);
}

// Validates the binding table and builds the normalized key. Both
// ComputePipelineCache::entry and ComputePass::create funnel through this.
[[nodiscard]] CacheKey normalizeAndValidate(const std::vector<ComputeBinding>& bindings) {
    CacheKey key;
    uint32_t maxSet = 0;
    std::vector<std::pair<uint32_t, uint32_t>> seen;  // (set, binding) pairs
    for (const ComputeBinding& binding : bindings) {
        for (const auto& [set, index] : seen) {
            if (set == binding.set && index == binding.binding) {
                fail("duplicate binding",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
        }
        seen.emplace_back(binding.set, binding.binding);
        maxSet = std::max(maxSet, binding.set);

        switch (binding.kind) {
        case DescriptorKind::UniformBuffer:
        case DescriptorKind::StorageBuffer:
            if (binding.buffer == nullptr || binding.buffer->handle() == VK_NULL_HANDLE) {
                fail("buffer binding without a Buffer",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            if (binding.image != nullptr || binding.foreignView != VK_NULL_HANDLE) {
                fail("buffer binding with an image resource",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            break;
        case DescriptorKind::StorageImage:
            if (binding.image == nullptr || binding.image->view() == VK_NULL_HANDLE) {
                fail("storage image binding without an Image",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            if (binding.foreignView != VK_NULL_HANDLE || binding.foreignOwner != nullptr) {
                fail("storage image binding with a foreign view (only CombinedImageSampler accepts one)",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            break;
        case DescriptorKind::CombinedImageSampler:
            if (binding.image != nullptr && binding.foreignView != VK_NULL_HANDLE) {
                fail("combined image sampler bound to both an Image and a foreign view",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            if ((binding.image == nullptr || binding.image->view() == VK_NULL_HANDLE) &&
                binding.foreignView == VK_NULL_HANDLE) {
                fail("combined image sampler without an Image or a foreign view",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            if (binding.foreignView != VK_NULL_HANDLE && binding.foreignOwner == nullptr) {
                fail("foreign view without a foreignOwner token (the plane would not survive a retained submission)",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            if (binding.foreignView == VK_NULL_HANDLE && binding.foreignOwner != nullptr) {
                fail("foreignOwner token without a foreignView",
                     "(set " + std::to_string(binding.set) + ", binding " + std::to_string(binding.binding) + ")");
            }
            break;
        default:
            fail("unknown descriptor kind");
        }
    }
    if (bindings.empty()) {
        // A layout derivation with no bindings is indistinguishable from a
        // mistake (and every consumer declares at least input/output).
        fail("no bindings");
    }

    key.sets.resize(maxSet + 1);
    for (const ComputeBinding& binding : bindings) {
        key.sets[binding.set].push_back(packedBinding(binding.binding, binding.kind));
    }
    for (auto& set : key.sets) {
        std::sort(set.begin(), set.end());
    }
    return key;
}

// One pass-owned sampler, kept alive by shared token so a retained
// submission outlives the pass that created it.
struct SamplerState {
    VkDevice device = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    ~SamplerState() {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
        }
    }
};

// Retained submit-and-wait for the borrowed-resource helpers below (their
// staging buffers/images are RAII locals): capacity exhaustion drains and
// retries once; a timeout drains — completing the fence and releasing the
// retained tokens — before throwing, so the submission can never outlive
// the locals.
void submitAndWaitRetained(SubmissionQueue& queue, const std::function<void(VkCommandBuffer)>& record,
                           SubmissionQueue::RetainedResources retained, uint64_t timeout_ns, const char* what) {
    std::optional<SubmissionQueue::Completion> completion = queue.submit(record, retained);
    if (!completion) {
        queue.drain();
        completion = queue.submit(record, std::move(retained));
    }
    if (!completion) {
        throw GpuException(GpuError::SubmissionTimeout,
                           std::string(what) + ": submission queue still full after drain");
    }
    if (!queue.wait(*completion, timeout_ns)) {
        queue.drain();
        throw GpuException(GpuError::SubmissionTimeout,
                           std::string(what) + ": timed out after " + std::to_string(timeout_ns) + " ns");
    }
}

void fillImageBarrier(VkImageMemoryBarrier& barrier, const Image& image, VkImageLayout oldLayout,
                      VkImageLayout newLayout, VkAccessFlags src_access, VkAccessFlags dst_access) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.handle();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

// Writes `sets`' descriptors from the current binding table. Every arm
// rewrites — a moved/rebound resource can never leave a stale descriptor
// behind.
void writeDescriptors(const VkDevice vkDevice, const std::vector<ComputeBinding>& bindings,
                      const std::vector<VkSampler>& samplers, const std::vector<VkDescriptorSet>& sets) {
    const std::size_t count = bindings.size();
    std::vector<VkDescriptorBufferInfo> bufferInfos(count);
    std::vector<VkDescriptorImageInfo> imageInfos(count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const ComputeBinding& binding = bindings[i];
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets.at(binding.set);
        write.dstBinding = binding.binding;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType(binding.kind);
        if (binding.kind == DescriptorKind::CombinedImageSampler) {
            const VkImageView view = binding.image != nullptr ? binding.image->view() : binding.foreignView;
            imageInfos[i] = {samplers[i], view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            write.pImageInfo = &imageInfos[i];
        } else if (binding.kind == DescriptorKind::StorageImage) {
            imageInfos[i] = {VK_NULL_HANDLE, binding.image->view(), VK_IMAGE_LAYOUT_GENERAL};
            write.pImageInfo = &imageInfos[i];
        } else {
            bufferInfos[i] = {binding.buffer->handle(), 0, VK_WHOLE_SIZE};
            write.pBufferInfo = &bufferInfos[i];
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(vkDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// ComputePipelineCache
// ---------------------------------------------------------------------------

// One cached pipeline and its descriptor-bundle pool. Held through
// shared_ptr so a retained dispatch token keeps every referenced handle
// alive past the cache wrapper (and past the pass).
struct ComputePipelineCache::Entry {
    VkDevice device = VK_NULL_HANDLE;
    // Immutable after entry construction.
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> setLayouts;  // index = set number
    uint32_t maxSet = 0;
    std::vector<VkDescriptorPoolSize> poolSizes;

    // Bundle pool: allocation and free-list reuse are serialized; a bundle
    // is handed out only when no submission can still reference it.
    std::mutex mutex;
    std::vector<VkDescriptorPool> pools;
    std::vector<std::vector<VkDescriptorSet>> freeBundles;
    std::size_t bundleCount = 0;

    ~Entry() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            freeBundles.clear();
        }
        if (device == VK_NULL_HANDLE) {
            return;  // construction failed before the device was recorded
        }
        for (VkDescriptorPool pool : pools) {
            if (pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, pool, nullptr);
            }
        }
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, layout, nullptr);
        }
        for (VkDescriptorSetLayout setLayout : setLayouts) {
            if (setLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            }
        }
    }

    // Returns a bundle to the free list (called by DispatchState's
    // destructor after the submission's fence signalled).
    void release(std::vector<VkDescriptorSet> sets) {
        if (sets.empty())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        freeBundles.push_back(std::move(sets));
    }

    // Takes a completed bundle when one is free, otherwise allocates a new
    // one from the (grown on demand) pool.
    [[nodiscard]] std::vector<VkDescriptorSet> acquire() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!freeBundles.empty()) {
            auto sets = std::move(freeBundles.back());
            freeBundles.pop_back();
            return sets;
        }
        return allocateBundle();
    }

private:
    // Out-of-pool failure grows the pool: Nemo never blocks dispatch on
    // descriptor reuse accounting.
    [[nodiscard]] std::vector<VkDescriptorSet> allocateBundle() {
        // Reserve retirement storage before issuing handles. release() is
        // called from a noexcept destructor and must not allocate.
        freeBundles.reserve(bundleCount + 1);
        std::vector<VkDescriptorSet> sets(maxSet + 1, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorSetCount = maxSet + 1;
        info.pSetLayouts = setLayouts.data();
        info.descriptorPool = pools.back();
        VkResult result = vkAllocateDescriptorSets(device, &info, sets.data());
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = maxSet + 1;
            poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            pools.reserve(pools.size() + 1);
            VkDescriptorPool pool = VK_NULL_HANDLE;
            checkVulkan(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool),
                        "vkCreateDescriptorPool (compute bundle growth)");
            pools.push_back(pool);
            info.descriptorPool = pool;
            result = vkAllocateDescriptorSets(device, &info, sets.data());
        }
        checkVulkan(result, "vkAllocateDescriptorSets (compute descriptor bundle)");
        ++bundleCount;
        return sets;
    }
};

// Token returned by ComputePass::retain(): owns the armed bundle, the
// pass-owned samplers, and a retain() token per bound resource; everything
// the recorded commands reference survives until the submission's fence
// signals. Destruction returns the bundle to the pipeline entry's free
// list.
namespace {

struct DispatchState {
    std::shared_ptr<ComputePipelineCache::Entry> entry;
    std::vector<std::shared_ptr<const void>> resources;
    std::vector<VkDescriptorSet> sets;
    ~DispatchState() {
        if (entry != nullptr) {
            entry->release(std::move(sets));
        }
    }
};

}  // namespace

struct ComputePipelineCache::Impl {
    Device* device = nullptr;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::mutex mutex;
    std::unordered_map<CacheKey, std::shared_ptr<Entry>, decltype([](const CacheKey& key) { return key.hash(); })>
        entries;

    ~Impl() {
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        if (pipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(device->handle(), pipelineCache, nullptr);
        }
    }
};

ComputePipelineCache::~ComputePipelineCache() = default;

std::unique_ptr<ComputePipelineCache> ComputePipelineCache::create(Device& device) {
    auto cache = std::make_unique<ComputePipelineCache>();
    cache->impl_ = std::make_unique<Impl>();
    cache->impl_->device = &device;
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    checkVulkan(vkCreatePipelineCache(device.handle(), &info, nullptr, &cache->impl_->pipelineCache),
                "vkCreatePipelineCache (compute)");
    return cache;
}

std::shared_ptr<ComputePipelineCache::Entry> ComputePipelineCache::entry(const std::vector<std::uint32_t>& spirv,
                                                                         const std::vector<ComputeBinding>& bindings) {
    Device& device = *impl_->device;
    if (spirv.empty()) {
        fail("no SPIR-V");
    }
    for (const auto& binding : bindings) {
        if (binding.set >= device.properties().limits.maxBoundDescriptorSets)
            fail("descriptor set exceeds device limit", std::to_string(binding.set));
    }
    CacheKey key = normalizeAndValidate(bindings);
    key.spirv = spirv;
    Impl& impl = *impl_;
    // Built under the lock: pipeline creation is rare (device-lifetime
    // cache) and never on the dispatch hot path after warm-up.
    std::lock_guard<std::mutex> lock(impl.mutex);
    const auto found = impl.entries.find(key);
    if (found != impl.entries.end()) {
        return found->second;
    }

    auto entry = std::make_shared<Entry>();
    entry->device = device.handle();
    const VkDevice vkDevice = entry->device;

    // Descriptor set layouts from the binding table. Sets between 0 and the
    // highest referenced set that have no bindings get empty layouts (a
    // pipeline layout cannot skip set indices).
    std::unordered_map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> perSet;
    uint32_t counts[4] = {};  // indexed by DescriptorKind
    for (const ComputeBinding& binding : bindings) {
        VkDescriptorSetLayoutBinding layout{};
        layout.binding = binding.binding;
        layout.descriptorType = descriptorType(binding.kind);
        layout.descriptorCount = 1;
        layout.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        perSet[binding.set].push_back(layout);
        ++counts[static_cast<uint32_t>(binding.kind)];
    }
    const uint32_t maxSet = static_cast<uint32_t>(key.sets.size() - 1);
    entry->maxSet = maxSet;
    entry->setLayouts.resize(maxSet + 1);
    for (uint32_t set = 0; set <= maxSet; ++set) {
        const auto& layoutBindings = perSet[set];
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
        layoutInfo.pBindings = layoutBindings.empty() ? nullptr : layoutBindings.data();
        checkVulkan(vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &entry->setLayouts[set]),
                    "vkCreateDescriptorSetLayout");
    }

    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCreateInfo.setLayoutCount = static_cast<uint32_t>(entry->setLayouts.size());
    layoutCreateInfo.pSetLayouts = entry->setLayouts.data();
    checkVulkan(vkCreatePipelineLayout(vkDevice, &layoutCreateInfo, nullptr, &entry->layout), "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = spirv.data();
    struct Module {
        VkDevice device;
        VkShaderModule handle{};
        ~Module() {
            if (handle)
                vkDestroyShaderModule(device, handle, nullptr);
        }
    } shaderModule{vkDevice};
    checkVulkan(vkCreateShaderModule(vkDevice, &moduleInfo, nullptr, &shaderModule.handle), "vkCreateShaderModule");
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule.handle;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = entry->layout;
    checkVulkan(vkCreateComputePipelines(vkDevice, impl.pipelineCache, 1, &pipelineInfo, nullptr, &entry->pipeline),
                "vkCreateComputePipelines");
    device.addPipelineCreations();

    // Descriptor pool sized from the binding table; grown on demand.
    for (uint32_t kind = 0; kind < 4; ++kind) {
        if (counts[kind] != 0) {
            entry->poolSizes.push_back({descriptorType(static_cast<DescriptorKind>(kind)), counts[kind]});
        }
    }
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = maxSet + 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(entry->poolSizes.size());
    poolInfo.pPoolSizes = entry->poolSizes.data();
    entry->pools.reserve(1);
    VkDescriptorPool pool = VK_NULL_HANDLE;
    checkVulkan(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    entry->pools.push_back(pool);

    return impl.entries.emplace(std::move(key), std::move(entry)).first->second;
}

// ---------------------------------------------------------------------------
// ComputePass
// ---------------------------------------------------------------------------

struct ComputePass::Impl {
    Device* device = nullptr;
    std::shared_ptr<DispatchState> state;
};

ComputePass::~ComputePass() = default;
ComputePass::ComputePass(ComputePass&&) noexcept = default;
ComputePass& ComputePass::operator=(ComputePass&&) noexcept = default;

std::unique_ptr<ComputePass> ComputePass::create(Device& device, const std::vector<std::uint32_t>& spirv,
                                                 const std::vector<ComputeBinding>& bindings) {
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->state = std::make_shared<DispatchState>();
    auto& state = *impl->state;
    state.entry = device.computePipelineCache().entry(spirv, bindings);
    state.sets = state.entry->acquire();
    state.resources.reserve(bindings.size() * 2);
    std::vector<VkSampler> samplers(bindings.size(), VK_NULL_HANDLE);
    std::shared_ptr<SamplerState> filters[2];
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        if (binding.buffer)
            state.resources.push_back(binding.buffer->retain());
        if (binding.image)
            state.resources.push_back(binding.image->retain());
        if (binding.foreignOwner)
            state.resources.push_back(binding.foreignOwner);
        if (binding.kind != DescriptorKind::CombinedImageSampler)
            continue;
        auto& sampler = filters[binding.nearest ? 1 : 0];
        if (!sampler) {
            sampler = std::make_shared<SamplerState>();
            sampler->device = device.handle();
            VkSamplerCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter = binding.nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            info.minFilter = info.magFilter;
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.maxLod = 1.0F;
            checkVulkan(vkCreateSampler(device.handle(), &info, nullptr, &sampler->sampler), "vkCreateSampler");
        }
        samplers[i] = sampler->sampler;
        state.resources.push_back(sampler);
    }
    // Descriptor contents and retained handles are immutable from here on.
    // The source Buffer/Image wrappers may now move or be destroyed.
    writeDescriptors(device.handle(), bindings, samplers, state.sets);
    auto pass = std::make_unique<ComputePass>();
    pass->impl_ = std::move(impl);
    return pass;
}

std::shared_ptr<const void> ComputePass::retain() const {
    return impl_->state;
}

void ComputePass::record(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) const {
    const auto& state = *impl_->state;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state.entry->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state.entry->layout, 0,
                            static_cast<uint32_t>(state.sets.size()), state.sets.data(), 0, nullptr);
    vkCmdDispatch(cmd, x, y, z);
}

void ComputePass::dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t timeout_ns) {
    auto& queue = impl_->device->submissions(impl_->device->graphics_family());
    const auto recordDispatch = [this, x, y, z](VkCommandBuffer cmd) { record(cmd, x, y, z); };
    auto completion = queue.submit(recordDispatch, {retain()});
    if (!completion) {
        queue.drain();
        completion = queue.submit(recordDispatch, {retain()});
    }
    if (!completion)
        throw GpuException(GpuError::InvalidRequest, "compute dispatch: submission capacity exhausted");
    if (!queue.wait(*completion, timeout_ns))
        throw GpuException(GpuError::SubmissionTimeout,
                           "compute dispatch: timed out after " + std::to_string(timeout_ns) + " ns");
}

// ---------------------------------------------------------------------------
// Transfer helpers
// ---------------------------------------------------------------------------

void recordImageBarrier(VkCommandBuffer cmd, const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                        VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{};
    fillImageBarrier(barrier, image, oldLayout, newLayout, src_access, dst_access);
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void uploadImage(SubmissionQueue& queue, Allocator& allocator, const Image& image, const void* data, std::size_t bytes,
                 uint64_t timeout_ns) {
    if (bytes == 0) {
        throw GpuException(GpuError::InvalidRequest, "uploadImage: empty upload");
    }
    Buffer staging = allocator.create_buffer(static_cast<VkDeviceSize>(bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             MemoryPreference::HostMapped);
    std::memcpy(staging.mapped(), data, bytes);

    const VkExtent3D extent = image.extent();
    const VkBuffer stagingBuffer = staging.handle();
    const VkImage imageHandle = image.handle();
    submitAndWaitRetained(
        queue,
        [&](VkCommandBuffer cmd) {
            recordImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT);

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyBufferToImage(cmd, stagingBuffer, imageHandle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            recordImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_READ_BIT);
        },
        {staging.retain(), image.retain()}, timeout_ns, "uploadImage");
}

void imageBarrier(SubmissionQueue& queue, const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access, uint64_t timeout_ns) {
    submitAndWaitRetained(
        queue,
        [&](VkCommandBuffer cmd) {
            recordImageBarrier(cmd, image, oldLayout, newLayout, src_stage, src_access, dst_stage, dst_access);
        },
        {image.retain()}, timeout_ns, "imageBarrier");
}

void downloadImage(SubmissionQueue& queue, Allocator& allocator, const Image& image, void* data, std::size_t bytes,
                   uint64_t timeout_ns) {
    if (bytes == 0 || data == nullptr) {
        throw GpuException(GpuError::InvalidRequest, "downloadImage: empty download");
    }
    Buffer staging = allocator.create_buffer(static_cast<VkDeviceSize>(bytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             MemoryPreference::HostMapped);
    // GENERAL → TRANSFER_SRC → copy → GENERAL in one submission: the
    // diagnostic readback must not change the resident image's layout
    // convention.
    const VkExtent3D extent = image.extent();
    const VkBuffer stagingBuffer = staging.handle();
    const VkImage imageHandle = image.handle();
    submitAndWaitRetained(
        queue,
        [&](VkCommandBuffer cmd) {
            recordImageBarrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyImageToBuffer(cmd, imageHandle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &copy);

            recordImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        },
        {staging.retain(), image.retain()}, timeout_ns, "downloadImage");
    std::memcpy(data, staging.mapped(), bytes);
}

}  // namespace nemo::gpu
