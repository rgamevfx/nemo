#pragma once

#include <functional>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Device.hpp"

namespace nemo::gpu {

// A single-queue command submission path with a documented lifetime and
// completion contract (spec section 10.2).
//
// Lifetime/completion contract:
// - `submit_and_wait` is synchronous: it returns only after the submitted
//   command buffer completed (fence signalled). All resources referenced by
//   the recording callback — buffers, images, descriptor sets — must outlive
//   the call; they may be released again immediately after it returns
//   without a timeout.
// - On timeout it throws GpuException(SubmissionTimeout) naming the queue
//   family and the timeout. The submission may still be executing after the
//   throw; the fence and command buffer stay inside the SubmissionQueue, and
//   every resource referenced by the recording callback must be kept alive
//   until the SubmissionQueue is destroyed (see below) or the fence
//   completes. This is the issue #2 form of the spec 10.2 rule: stale
//   submissions finish after cancellation, resources are retained until
//   completion.
// - The destructor enforces completion: it waits for the device to go idle
//   before destroying the command pool, so an in-flight timed-out submission
//   can never reference freed pool memory.
//
// Threading contract: one SubmissionQueue per queue. The wrapped VkQueue,
// command pool, and fence are externally synchronized; callers must not use
// a SubmissionQueue from more than one thread.
class SubmissionQueue {
public:
    // Binds queue index 0 of `queue_family`. `device` must outlive the
    // SubmissionQueue (the queue handle and device handle are borrowed).
    SubmissionQueue(Device& device, uint32_t queue_family);
    ~SubmissionQueue();

    SubmissionQueue(const SubmissionQueue&) = delete;
    SubmissionQueue& operator=(const SubmissionQueue&) = delete;

    // Records `record` into a fresh primary command buffer, submits it to the
    // bound queue, and waits for the fence. The fence is reset before every
    // submit, so a submission whose commands never run cannot be confused
    // with one completed by a previous submit. Throws GpuException with a
    // descriptive message on any vk* error or on timeout.
    void submit_and_wait(const std::function<void(VkCommandBuffer)>& record, uint64_t timeout_ns);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

}  // namespace nemo::gpu
