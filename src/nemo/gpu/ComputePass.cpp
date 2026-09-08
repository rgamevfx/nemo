#include "nemo/gpu/ComputePass.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

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

}  // namespace

struct ComputePass::Impl {
    Device* device = nullptr;
    std::unordered_map<uint32_t, VkDescriptorSetLayout> set_layouts;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkDescriptorSet> descriptor_sets;
    std::vector<VkSampler> samplers;
};

ComputePass::~ComputePass() {
    if (!impl_) {
        return;
    }
    const VkDevice device = impl_->device->handle();
    for (VkSampler sampler : impl_->samplers) {
        vkDestroySampler(device, sampler, nullptr);
    }
    if (impl_->descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, impl_->descriptor_pool, nullptr);
    }
    if (impl_->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, impl_->pipeline, nullptr);
    }
    if (impl_->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, impl_->pipeline_layout, nullptr);
    }
    for (auto& [set, layout] : impl_->set_layouts) {
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
}

ComputePass::ComputePass(ComputePass&&) noexcept = default;
ComputePass& ComputePass::operator=(ComputePass&&) noexcept = default;

std::unique_ptr<ComputePass> ComputePass::create(Device& device, const std::vector<std::uint32_t>& spirv,
                                                 const std::vector<ComputeBinding>& bindings) {
    if (spirv.empty()) {
        fail("no SPIR-V");
    }
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    const VkDevice vkDevice = device.handle();

    // Descriptor set layouts from the binding table. Sets between 0 and the
    // highest referenced set that have no bindings get empty layouts (a
    // pipeline layout cannot skip set indices).
    std::unordered_map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> perSet;
    uint32_t maxSet = 0;
    for (const ComputeBinding& binding : bindings) {
        VkDescriptorSetLayoutBinding layout{};
        layout.binding = binding.binding;
        layout.descriptorType = descriptorType(binding.kind);
        layout.descriptorCount = 1;
        layout.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        perSet[binding.set].push_back(layout);
        maxSet = std::max(maxSet, binding.set);
    }

    std::vector<VkDescriptorSetLayout> layoutOrder(maxSet + 1, VK_NULL_HANDLE);
    for (uint32_t set = 0; set <= maxSet; ++set) {
        const auto& layoutBindings = perSet[set];
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
        layoutInfo.pBindings = layoutBindings.empty() ? nullptr : layoutBindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        checkVulkan(vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &layout),
                    "vkCreateDescriptorSetLayout");
        impl->set_layouts.emplace(set, layout);
        layoutOrder[set] = layout;
    }

    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCreateInfo.setLayoutCount = static_cast<uint32_t>(layoutOrder.size());
    layoutCreateInfo.pSetLayouts = layoutOrder.data();
    checkVulkan(vkCreatePipelineLayout(vkDevice, &layoutCreateInfo, nullptr, &impl->pipeline_layout),
                "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checkVulkan(vkCreateShaderModule(vkDevice, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = module;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = impl->pipeline_layout;
    checkVulkan(vkCreateComputePipelines(vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl->pipeline),
                "vkCreateComputePipelines");
    vkDestroyShaderModule(vkDevice, module, nullptr);

    // Descriptor pool sized from the binding table.
    std::vector<VkDescriptorPoolSize> poolSizes;
    std::unordered_map<VkDescriptorType, uint32_t> counts;
    for (const ComputeBinding& binding : bindings) {
        ++counts[descriptorType(binding.kind)];
    }
    for (const auto& [type, count] : counts) {
        poolSizes.push_back({type, count});
    }
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = maxSet + 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    checkVulkan(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &impl->descriptor_pool), "vkCreateDescriptorPool");

    // Allocate one set per index and write the descriptors.
    for (uint32_t set = 0; set <= maxSet; ++set) {
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = impl->descriptor_pool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &layoutOrder[set];
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        checkVulkan(vkAllocateDescriptorSets(vkDevice, &allocateInfo, &descriptorSet), "vkAllocateDescriptorSets");
        impl->descriptor_sets.emplace(set, descriptorSet);
    }
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    bufferInfos.reserve(bindings.size());
    imageInfos.reserve(bindings.size());
    for (const ComputeBinding& binding : bindings) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = impl->descriptor_sets.at(binding.set);
        write.dstBinding = binding.binding;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType(binding.kind);
        if (binding.kind == DescriptorKind::CombinedImageSampler) {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = binding.nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            samplerInfo.minFilter = samplerInfo.magFilter;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 1.0F;
            VkSampler sampler = VK_NULL_HANDLE;
            checkVulkan(vkCreateSampler(vkDevice, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
            impl->samplers.push_back(sampler);
            imageInfos.push_back({sampler, binding.image->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            write.pImageInfo = &imageInfos.back();
        } else if (binding.kind == DescriptorKind::StorageImage) {
            // GENERAL throughout: the effect executor writes a result and
            // hands the same image to the next dependent pass (spec 10.4),
            // synchronized by imageBarrier between dispatches.
            imageInfos.push_back({VK_NULL_HANDLE, binding.image->view(), VK_IMAGE_LAYOUT_GENERAL});
            write.pImageInfo = &imageInfos.back();
        } else {
            bufferInfos.push_back({binding.buffer->handle(), 0, VK_WHOLE_SIZE});
            write.pBufferInfo = &bufferInfos.back();
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(vkDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    auto pass = std::make_unique<ComputePass>();
    pass->impl_ = std::move(impl);
    return pass;
}

void ComputePass::dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t timeout_ns) {
    SubmissionQueue queue(*impl_->device, impl_->device->graphics_family());
    std::vector<VkDescriptorSet> sets;
    sets.reserve(impl_->descriptor_sets.size());
    for (uint32_t set = 0; set < impl_->descriptor_sets.size(); ++set) {
        sets.push_back(impl_->descriptor_sets.at(set));
    }
    queue.submit_and_wait(
        [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline_layout, 0,
                                    static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            vkCmdDispatch(cmd, x, y, z);
        },
        timeout_ns);
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
    queue.submit_and_wait(
        [&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image.handle();
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &toTransfer);

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyBufferToImage(cmd, staging.handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);

            VkImageMemoryBarrier toShader{};
            toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShader.image = image.handle();
            toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toShader);
        },
        timeout_ns);
}

void imageBarrier(SubmissionQueue& queue, const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access, uint64_t timeout_ns) {
    queue.submit_and_wait(
        [&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.handle();
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        },
        timeout_ns);
}

void downloadImage(SubmissionQueue& queue, Allocator& allocator, const Image& image, void* data, std::size_t bytes,
                   uint64_t timeout_ns) {
    if (bytes == 0 || data == nullptr) {
        throw GpuException(GpuError::InvalidRequest, "downloadImage: empty download");
    }
    Buffer staging = allocator.create_buffer(static_cast<VkDeviceSize>(bytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             MemoryPreference::HostMapped);
    // GENERAL → TRANSFER_SRC → copy → GENERAL: the diagnostic readback
    // must not change the resident image's layout convention.
    imageBarrier(queue, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT, timeout_ns);
    const VkExtent3D extent = image.extent();
    queue.submit_and_wait(
        [&](VkCommandBuffer cmd) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyImageToBuffer(cmd, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle(), 1,
                                   &copy);
        },
        timeout_ns);
    imageBarrier(queue, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT, timeout_ns);
    std::memcpy(data, staging.mapped(), bytes);
}

}  // namespace nemo::gpu
