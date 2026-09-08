#include "nemo/gpu/MediaInterop.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "nemo/gpu/Error.hpp"

namespace nemo::gpu {

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& detail = {}) {
    throw GpuException(GpuError::VulkanError, "media interop: " + what + (detail.empty() ? "" : ": " + detail));
}

VkImageView createPlaneView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    checkVulkan(vkCreateImageView(device, &info, nullptr, &view), "vkCreateImageView (foreign video plane)");
    return view;
}

}  // namespace

struct MediaInterop::Impl {
    Device* device = nullptr;
    Allocator* allocator = nullptr;
    VkDescriptorSetLayout setLayouts[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::unique_ptr<SubmissionQueue> queue;
};

MediaInterop::~MediaInterop() {
    if (impl_ == nullptr) {
        return;
    }
    const VkDevice device = impl_->device->handle();
    if (impl_->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, impl_->descriptorPool, nullptr);
    }
    if (impl_->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, impl_->pipeline, nullptr);
    }
    if (impl_->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, impl_->pipelineLayout, nullptr);
    }
    for (VkDescriptorSetLayout layout : impl_->setLayouts) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        }
    }
    if (impl_->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, impl_->sampler, nullptr);
    }
}

std::unique_ptr<MediaInterop> MediaInterop::create(Device& device, Allocator& allocator,
                                                   const std::vector<std::uint32_t>& convertSpirv) {
    auto interop = std::unique_ptr<MediaInterop>(new MediaInterop());
    interop->impl_ = std::make_unique<Impl>();
    Impl& impl = *interop->impl_;
    impl.device = &device;
    impl.allocator = &allocator;
    const VkDevice vkDevice = device.handle();

    // One binding per set (mediaConvert.slang): set 0 = uniform meta;
    // set 1 = TWO sampled planes (bindings 0 and 1, luma + chroma);
    // set 2 = rgba32f storage output.
    const VkDescriptorSetLayoutBinding bindings[3][2] = {
        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, 0, nullptr}},
        {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
        {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, 0, nullptr}},
    };
    const uint32_t bindingCounts[3] = {1, 2, 1};
    for (int set = 0; set < 3; ++set) {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = bindingCounts[set];
        info.pBindings = bindings[set];
        checkVulkan(vkCreateDescriptorSetLayout(vkDevice, &info, nullptr, &impl.setLayouts[set]),
                    "vkCreateDescriptorSetLayout");
    }
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = impl.setLayouts;
    checkVulkan(vkCreatePipelineLayout(vkDevice, &layoutInfo, nullptr, &impl.pipelineLayout), "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = convertSpirv.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = convertSpirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checkVulkan(vkCreateShaderModule(vkDevice, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          nullptr,
                          0,
                          VK_SHADER_STAGE_COMPUTE_BIT,
                          module,
                          "main",
                          nullptr};
    pipelineInfo.layout = impl.pipelineLayout;
    checkVulkan(vkCreateComputePipelines(vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl.pipeline),
                "vkCreateComputePipelines");
    vkDestroyShaderModule(vkDevice, module, nullptr);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0F;
    checkVulkan(vkCreateSampler(vkDevice, &samplerInfo, nullptr, &impl.sampler), "vkCreateSampler");

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    checkVulkan(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &impl.descriptorPool), "vkCreateDescriptorPool");

    impl.queue = std::make_unique<SubmissionQueue>(device, device.graphics_family());
    return interop;
}

void MediaInterop::convertToRgba32f(ForeignVideoFrame& frame, Image& output, uint64_t timeout_ns) {
    const VkDevice vkDevice = impl_->device->handle();
    if (frame.planeCount == 0 || frame.planeCount > 2) {
        fail("foreign frame must carry 1..2 planes");
    }
    if (frame.multiplane && frame.planeCount != 1) {
        fail("multiplane foreign frame must carry exactly one image");
    }
    if (frame.width == 0 || frame.height == 0) {
        fail("foreign frame extent must be non-empty");
    }

    // meta = (width, height, 0, 0) for the kernel's region guard.
    Buffer uniform =
        impl_->allocator->create_buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryPreference::HostMapped);
    const std::uint32_t meta[4] = {frame.width, frame.height, 0, 0};
    std::memcpy(uniform.mapped(), meta, sizeof(meta));

    // Sample views on the borrowed images; freed after the synchronous
    // submit completes. Multiplane frames take one view per aspect plane
    // over images[0] (R8 luma, R8G8 chroma); per-plane frames take one
    // view per image.
    const uint32_t viewCount = frame.multiplane ? 2u : frame.planeCount;
    VkImageView views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkFormat planeFormats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
    if (frame.multiplane) {
        views[0] = createPlaneView(vkDevice, frame.images[0], planeFormats[0], VK_IMAGE_ASPECT_PLANE_0_BIT);
        views[1] = createPlaneView(vkDevice, frame.images[0], planeFormats[1], VK_IMAGE_ASPECT_PLANE_1_BIT);
    } else {
        for (uint32_t plane = 0; plane < frame.planeCount; ++plane) {
            views[plane] =
                createPlaneView(vkDevice, frame.images[plane], frame.formats[plane], VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    // Descriptor sets, one allocation per frame conversion.
    VkDescriptorSet sets[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = impl_->descriptorPool;
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts = impl_->setLayouts;
    checkVulkan(vkAllocateDescriptorSets(vkDevice, &allocInfo, sets), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo bufferInfo{uniform.handle(), 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo planeInfos[2] = {
        {impl_->sampler, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {impl_->sampler, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE, output.view(), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = sets[0],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &bufferInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = sets[1],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &planeInfos[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = sets[1],
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &planeInfos[1]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = sets[2],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &outputInfo},
    };
    vkUpdateDescriptorSets(vkDevice, 4, writes, 0, nullptr);

    // Cross-queue dependency: wait for the producer's decode signal, hand
    // the planes back at waitValue+1 when the conversion completed.
    SubmissionQueue::TimelineSemaphores semaphores;
    for (uint32_t plane = 0; plane < frame.planeCount; ++plane) {
        if (frame.multiplane && plane == 1) {
            continue;  // single multiplane image: one semaphore governs it
        }
        semaphores.wait.push_back(frame.semaphores[plane]);
        semaphores.waitValues.push_back(frame.waitValues[plane]);
        semaphores.signal.push_back(frame.semaphores[plane]);
        semaphores.signalValues.push_back(frame.waitValues[plane] + 1);
    }

    impl_->queue->submit_and_wait(
        [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipelineLayout, 0, 3, sets, 0, nullptr);

            // Synchronization2 barriers: the producer stage bits (video
            // decode) exist only in the FlagBits2 form. Multiplane frames
            // barrier the single image over both plane aspects; per-plane
            // frames barrier each image with COLOR aspects.
            const VkImageAspectFlags barrierAspects[2] = {
                frame.multiplane ? (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT)
                                 : VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT};
            const uint32_t barrierCount = frame.multiplane ? 1u : frame.planeCount;
            VkImageMemoryBarrier2 acquires[3] = {};
            for (uint32_t plane = 0; plane < barrierCount; ++plane) {
                VkImageMemoryBarrier2& barrier = acquires[plane];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                // The producer→consumer ordering is established by the
                // timeline-semaphore wait on this submission, so the
                // barrier itself carries no source stage: it performs the
                // layout/ownership transfer only (stages must be valid for
                // this queue family; video-decode stages are not).
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                barrier.srcAccessMask = VK_ACCESS_2_NONE;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                barrier.oldLayout = frame.layouts[plane];
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                // Concurrent-sharing frames (family IGNORED) need no
                // ownership transfer; exclusive-sharing frames transfer
                // decode -> graphics.
                barrier.srcQueueFamilyIndex = frame.queueFamilies[plane];
                barrier.dstQueueFamilyIndex = frame.queueFamilies[plane] == VK_QUEUE_FAMILY_IGNORED
                                                  ? VK_QUEUE_FAMILY_IGNORED
                                                  : impl_->device->graphics_family();
                barrier.image = frame.images[plane];
                barrier.subresourceRange = {barrierAspects[plane], 0, 1, 0, 1};
            }
            // Fresh output images start UNDEFINED; transition to the
            // contract's GENERAL layout for the storage write.
            VkImageMemoryBarrier2 outputAcquire{};
            outputAcquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            outputAcquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            outputAcquire.srcAccessMask = VK_ACCESS_2_NONE;
            outputAcquire.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            outputAcquire.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            outputAcquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            outputAcquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            outputAcquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            outputAcquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            outputAcquire.image = output.handle();
            outputAcquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo acquireInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            acquireInfo.imageMemoryBarrierCount = barrierCount + 1;
            acquireInfo.pImageMemoryBarriers = acquires;
            acquires[barrierCount] = outputAcquire;
            vkCmdPipelineBarrier2(cmd, &acquireInfo);

            vkCmdDispatch(cmd, (frame.width + 7) / 8, (frame.height + 7) / 8, 1);

            // Release back to the producer: restored layout/access and
            // family ownership, so the decoder's next pass over the planes
            // is properly ordered by the signaled timeline value.
            VkImageMemoryBarrier2 releases[2] = {};
            for (uint32_t plane = 0; plane < barrierCount; ++plane) {
                VkImageMemoryBarrier2& barrier = releases[plane];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                // Mirror image: the decode queue's next submission waits on
                // the signaled timeline value, so the destination stage is
                // NONE here (ownership hand-back only).
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                barrier.dstAccessMask = VK_ACCESS_2_NONE;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout = frame.layouts[plane];
                barrier.srcQueueFamilyIndex = frame.queueFamilies[plane] == VK_QUEUE_FAMILY_IGNORED
                                                  ? VK_QUEUE_FAMILY_IGNORED
                                                  : impl_->device->graphics_family();
                barrier.dstQueueFamilyIndex = frame.queueFamilies[plane];
                barrier.image = frame.images[plane];
                barrier.subresourceRange = {barrierAspects[plane], 0, 1, 0, 1};
            }
            VkDependencyInfo releaseInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            releaseInfo.imageMemoryBarrierCount = barrierCount;
            releaseInfo.pImageMemoryBarriers = releases;
            vkCmdPipelineBarrier2(cmd, &releaseInfo);
        },
        semaphores, timeout_ns);

    vkFreeDescriptorSets(vkDevice, impl_->descriptorPool, 3, sets);
    for (uint32_t plane = 0; plane < viewCount; ++plane) {
        vkDestroyImageView(vkDevice, views[plane], nullptr);
    }
    for (uint32_t plane = 0; plane < (frame.multiplane ? 1u : frame.planeCount); ++plane) {
        // The producer waits on the incremented value before reusing the
        // plane; our last access was the shader read recorded below.
        frame.waitValues[plane] += 1;
        frame.accesses[plane] = VK_ACCESS_SHADER_READ_BIT;
        if (frame.queueFamilies[plane] != VK_QUEUE_FAMILY_IGNORED) {
            frame.queueFamilies[plane] = impl_->device->graphics_family();
        }
    }
}

}  // namespace nemo::gpu
