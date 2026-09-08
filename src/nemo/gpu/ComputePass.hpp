#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::gpu {

enum class DescriptorKind { UniformBuffer, StorageBuffer, CombinedImageSampler, StorageImage };

struct ComputeBinding {
    uint32_t set{};
    uint32_t binding{};
    DescriptorKind kind{};
    // Borrowed only during create(); the pass captures handles and owners.
    const Buffer* buffer{};
    const Image* image{};
    // LUT1D uses LINEAR; OCIO tetrahedral 3D LUT sampling uses NEAREST.
    bool nearest{false};
    // CombinedImageSampler may instead reference a foreign plane view.
    // Its owner must retain the view, image, and decoder backing resources.
    VkImageView foreignView = VK_NULL_HANDLE;
    std::shared_ptr<const void> foreignOwner{};
};

// GPU-module device-lifetime cache, not a global registry. Full SPIR-V and
// normalized (set,binding,kind) layout identify immutable pipelines. Descriptor
// bundles are reused only after every pass/submission reference retires.
// All cache and pool access is synchronized. Device outlives every entry.
class ComputePipelineCache {
public:
    ComputePipelineCache() = default;
    ~ComputePipelineCache();
    ComputePipelineCache(const ComputePipelineCache&) = delete;
    ComputePipelineCache& operator=(const ComputePipelineCache&) = delete;
    static std::unique_ptr<ComputePipelineCache> create(Device& device);
    struct Entry;
    [[nodiscard]] std::shared_ptr<Entry> entry(const std::vector<std::uint32_t>& spirv,
                                               const std::vector<ComputeBinding>& bindings);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Immutable per-pass descriptor contents, samplers, and resource ownership;
// compiled pipelines are shared through the Device cache. The source wrapper
// objects may move or die immediately after create(). Host writes/GPU access
// to the retained allocations still require caller synchronization.
// record()/retain() may run concurrently; moving/destruction must not overlap
// access to the pass object. Device/Instance outlive all retained tokens.
class ComputePass {
public:
    ComputePass() = default;
    ~ComputePass();
    ComputePass(ComputePass&&) noexcept;
    ComputePass& operator=(ComputePass&&) noexcept;
    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;
    static std::unique_ptr<ComputePass> create(Device& device, const std::vector<std::uint32_t>& spirv,
                                               const std::vector<ComputeBinding>& bindings);
    // Pass this token to submit whenever record() references this pass.
    // Destruction of the pass or cancellation never frees submitted state.
    [[nodiscard]] std::shared_ptr<const void> retain() const;
    void record(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) const;
    // Headless convenience. Timeout throws but the shared queue retains all
    // submitted state; neither pass destruction nor retry resets live work.
    void dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t timeout_ns);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Cold upload via staging; ends SHADER_READ_ONLY_OPTIMAL. 1D/2D/3D supported.
// Synchronous helpers retain their staging/image owners and drain on timeout.
void uploadImage(SubmissionQueue& queue, Allocator& allocator, const Image& image, const void* data, std::size_t bytes,
                 uint64_t timeout_ns);

// Record-only barrier for batched dependencies; no submission or host wait.
void recordImageBarrier(VkCommandBuffer cmd, const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                        VkAccessFlags dst_access);
void imageBarrier(SubmissionQueue& queue, const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access, uint64_t timeout_ns);

// Explicit diagnostic readback ONLY. GENERAL -> copy -> GENERAL in one
// submission. Native evaluation and resident viewing never call this.
void downloadImage(SubmissionQueue& queue, Allocator& allocator, const Image& image, void* data, std::size_t bytes,
                   uint64_t timeout_ns);
}  // namespace nemo::gpu
