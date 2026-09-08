#include "nemo/gpu/Submit.hpp"

#include <string>
#include <utility>

namespace nemo::gpu {

SubmissionQueue::SubmissionQueue(Device& device, uint32_t queue_family)
    : device_(device.handle()), queue_(device.queue(queue_family)) {
    if (queue_ == VK_NULL_HANDLE) {
        throw GpuException(GpuError::VulkanError, "SubmissionQueue bound to family " + std::to_string(queue_family) +
                                                      ", which the device does not expose");
    }

    // Exception-safe construction: if any step below throws, destroy the
    // handles created so far — the destructor will not run because the
    // object was never fully constructed.
    try {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        checkVulkan(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        checkVulkan(vkAllocateCommandBuffers(device_, &allocate_info, &command_buffer_), "vkAllocateCommandBuffers");

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        checkVulkan(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence");
    } catch (...) {
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            // Freeing the pool releases the command buffer with it.
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        throw;
    }
}

SubmissionQueue::~SubmissionQueue() {
    if (device_ != VK_NULL_HANDLE && command_pool_ != VK_NULL_HANDLE) {
        // Completion rule: an in-flight submission (e.g. one whose wait timed
        // out) must finish before the pool and fence are destroyed, so
        // resources it references outlive it.
        vkDeviceWaitIdle(device_);
    }
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, fence_, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        // Freeing the pool releases the command buffer with it.
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
}

void SubmissionQueue::submit_and_wait(const std::function<void(VkCommandBuffer)>& record, uint64_t timeout_ns) {
    checkVulkan(vkResetFences(device_, 1, &fence_), "vkResetFences");
    checkVulkan(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    checkVulkan(vkBeginCommandBuffer(command_buffer_, &begin_info), "vkBeginCommandBuffer");
    record(command_buffer_);
    checkVulkan(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;
    checkVulkan(vkQueueSubmit(queue_, 1, &submit_info, fence_), "vkQueueSubmit");

    VkResult waited = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
    if (waited == VK_TIMEOUT) {
        // The submission may still be executing; the destructor waits for
        // completion (see the class contract) so referenced resources stay
        // alive until then.
        throw GpuException(GpuError::SubmissionTimeout,
                           "submission on queue family did not complete within " + std::to_string(timeout_ns) + " ns");
    }
    checkVulkan(waited, "vkWaitForFences");
}

}  // namespace nemo::gpu
