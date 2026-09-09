#include "nemo/gpu/Submit.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace nemo::gpu {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedNs(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

}  // namespace

SubmissionQueue::SubmissionQueue(Device& device, uint32_t queue_family, size_t capacity)
    : device_(&device), queue_family_(queue_family), queue_(device.queue(queue_family)) {
    if (queue_ == VK_NULL_HANDLE) {
        throw GpuException(GpuError::VulkanError, "SubmissionQueue bound to family " + std::to_string(queue_family) +
                                                      ", which the device does not expose");
    }
    if (capacity == 0 || capacity > std::numeric_limits<uint32_t>::max() / 2) {
        throw GpuException(GpuError::InvalidRequest, "SubmissionQueue capacity is outside the supported slot range");
    }

    // Exception-safe construction: if any step below throws, destroy the
    // handles created so far — the destructor will not run because the
    // object was never fully constructed.
    try {
        // GPU duration instrumentation (issue #22): when the family's queues
        // support timestamps, one begin/end query pair per slot measures the
        // executed command buffer end-to-end.
        timestamp_bits_ = device.family_timestamp_bits(queue_family_);
        // Command-buffer query reset is unavailable on transfer-only queues.
        // Prefer the queried host-reset feature; otherwise time only families
        // that can legally record vkCmdResetQueryPool.
        timed_ = timestamp_bits_ > 0 &&
                 (device.hostQueryResetEnabled() ||
                  (device.family_capabilities(queue_family_) & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)));
        if (timed_) {
            VkQueryPoolCreateInfo query_info{};
            query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_info.queryCount = static_cast<uint32_t>(2 * capacity);
            checkVulkan(vkCreateQueryPool(device.handle(), &query_info, nullptr, &query_pool_), "vkCreateQueryPool");
        }

        // One self-contained pool per slot: VkCommandPool host access is
        // externally synchronized against recording/reset of ANY command
        // buffer from it, so concurrent recordings must not share a pool.
        slots_.reserve(capacity);
        for (size_t index = 0; index < capacity; ++index) {
            slots_.push_back(std::make_unique<Slot>());
            auto& slot = slots_.back();
            slot->index = static_cast<uint32_t>(index);
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = queue_family;
            checkVulkan(vkCreateCommandPool(device.handle(), &pool_info, nullptr, &slot->command_pool),
                        "vkCreateCommandPool");
            VkCommandBufferAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate_info.commandPool = slot->command_pool;
            allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate_info.commandBufferCount = 1;
            checkVulkan(vkAllocateCommandBuffers(device.handle(), &allocate_info, &slot->command_buffer),
                        "vkAllocateCommandBuffers");
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            checkVulkan(vkCreateFence(device.handle(), &fence_info, nullptr, &slot->fence), "vkCreateFence");
        }
    } catch (...) {
        for (auto& slot : slots_) {
            if (slot != nullptr) {
                if (slot->fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device.handle(), slot->fence, nullptr);
                }
                if (slot->command_pool != VK_NULL_HANDLE) {
                    // Freeing the pool releases the command buffer with it.
                    vkDestroyCommandPool(device.handle(), slot->command_pool, nullptr);
                }
            }
        }
        if (query_pool_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device.handle(), query_pool_, nullptr);
        }
        throw;
    }
}

SubmissionQueue::~SubmissionQueue() {
    if (device_ == nullptr) {
        return;
    }
    // Destruction requires callers to have stopped accessing this wrapper.
    // No allocation and no exception path may free pending resources. Retry
    // transient host errors; device loss is the only terminal wait failure.
    for (auto& slot : slots_) {
        if (slot->state == Slot::State::Submitted) {
            VkResult result;
            do {
                result = vkWaitForFences(device_->handle(), 1, &slot->fence, VK_TRUE, UINT64_MAX);
                if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                    std::this_thread::yield();
            } while (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST);
            if (result == VK_SUCCESS)
                retireSlot(*slot);
        }
        slot->retained.clear();
        if (slot->fence)
            vkDestroyFence(device_->handle(), slot->fence, nullptr);
        if (slot->command_pool)
            vkDestroyCommandPool(device_->handle(), slot->command_pool, nullptr);
    }
    if (query_pool_)
        vkDestroyQueryPool(device_->handle(), query_pool_, nullptr);
}

// Device-loss submissions are quarantined in their bounded slot; no retry
// may reset their pool/fence. Other submission failures did not enqueue work.
void SubmissionQueue::recoverSlot(Slot& slot, bool deviceLost) {
    slot.completion = 0;
    if (deviceLost) {
        slot.state = Slot::State::Lost;
        poisoned_ = true;
    } else {
        slot.state = Slot::State::Idle;
        slot.retained.clear();
    }
}

// Lock held. Bookkeeping for an identity whose fence has signalled:
// release the retained resources, return the slot to the pool, and leave
// the identity durably completed (a valid id absent from every slot).
void SubmissionQueue::retireSlot(Slot& slot) {
    if (timed_) {
        uint64_t stamps[2] = {0, 0};
        // The fence has signalled, so both queries are written. A query
        // readback failure is non-fatal: the submission still completed —
        // only the instrumentation skips.
        if (vkGetQueryPoolResults(device_->handle(), query_pool_, slot.index * 2, 2, sizeof(stamps), stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint64_t mask = timestamp_bits_ >= 64 ? ~uint64_t{0} : ((uint64_t{1} << timestamp_bits_) - 1);
            const double period = device_->properties().limits.timestampPeriod;
            device_->addGpuExecutionNs(
                static_cast<uint64_t>(static_cast<double>((stamps[1] - stamps[0]) & mask) * period));
            device_->addGpuTimedSubmissions();
        }
    }
    device_->addCompletions();
    slot.state = Slot::State::Idle;
    slot.completion = 0;
    slot.retained.clear();
}

// Lock held. Retire every completed submission that no external waiter has
// pinned; slots pinned by wait()/drain() are left to their last waiter.
void SubmissionQueue::harvestIdle() {
    for (auto& slot : slots_) {
        if (slot->state != Slot::State::Submitted || slot->waiters != 0) {
            continue;
        }
        VkResult status = vkGetFenceStatus(device_->handle(), slot->fence);
        if (status == VK_NOT_READY) {
            continue;
        }
        checkVulkan(status, "vkGetFenceStatus");
        retireSlot(*slot);
    }
}

// Lock held.
SubmissionQueue::Slot* SubmissionQueue::findSubmitted(Completion completion) {
    auto found = std::find_if(slots_.begin(), slots_.end(), [&](const std::unique_ptr<Slot>& slot) {
        return slot->state == Slot::State::Submitted && slot->completion == completion;
    });
    return found != slots_.end() ? found->get() : nullptr;
}

std::optional<SubmissionQueue::Completion> SubmissionQueue::submit(const std::function<void(VkCommandBuffer)>& record,
                                                                   RetainedResources retained,
                                                                   const TimelineSemaphores& semaphores,
                                                                   uint64_t admissionTimeout_ns) {
    if (admissionTimeout_ns == 0)
        return trySubmit(record, std::move(retained), semaphores);
    const auto start = Clock::now();
    for (;;) {
        // Preserve the original owners: an unsuccessful attempt may have
        // reserved/recovered a slot before the FFmpeg queue-owner try-lock.
        if (auto completion = trySubmit(record, retained, semaphores))
            return completion;
        const auto elapsed = elapsedNs(start);
        if (elapsed >= admissionTimeout_ns)
            throw GpuException(GpuError::SubmissionTimeout,
                               "GPU submission admission timed out on queue family " + std::to_string(queue_family_));
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(std::min<std::uint64_t>(admissionTimeout_ns - elapsed, 100'000)));
    }
}

std::optional<SubmissionQueue::Completion>
SubmissionQueue::trySubmit(const std::function<void(VkCommandBuffer)>& record, RetainedResources retained,
                           const TimelineSemaphores& semaphores) {
    // Counts must match before passing the arrays to Vulkan. Waiting for
    // timeline value zero is valid and already satisfied.
    if (semaphores.wait.size() != semaphores.waitValues.size()) {
        throw GpuException(GpuError::InvalidRequest, "SubmissionQueue timeline wait semaphore/value count mismatch (" +
                                                         std::to_string(semaphores.wait.size()) + " semaphores, " +
                                                         std::to_string(semaphores.waitValues.size()) + " values)");
    }
    if (semaphores.signal.size() != semaphores.signalValues.size()) {
        throw GpuException(GpuError::InvalidRequest,
                           "SubmissionQueue timeline signal semaphore/value count mismatch (" +
                               std::to_string(semaphores.signal.size()) + " semaphores, " +
                               std::to_string(semaphores.signalValues.size()) + " values)");
    }

    const auto start = Clock::now();
    Slot* slot = nullptr;
    {
        // Non-blocking admission (issue #22): try the bookkeeping mutex —
        // another submit/harvest holding it momentarily means the caller is
        // rejected instead of queued. Never waits for the GPU or another
        // recording thread.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return std::nullopt;
        }
        if (poisoned_)
            throw GpuException(GpuError::VulkanError, "submission queue is unusable after device loss");
        harvestIdle();
        auto found = std::find_if(slots_.begin(), slots_.end(), [](const std::unique_ptr<Slot>& candidate) {
            return candidate->state == Slot::State::Idle;
        });
        if (found == slots_.end()) {
            // Bounded capacity admission: report full immediately.
            return std::nullopt;
        }
        slot = found->get();
        slot->state = Slot::State::Recording;
        slot->completion = 0;  // identity is assigned only after a successful submit
        slot->retained = std::move(retained);
        if (timed_ && device_->hostQueryResetEnabled())
            vkResetQueryPool(device_->handle(), query_pool_, slot->index * 2, 2);
    }
    // Recording happens outside the lock — each slot owns its command pool,
    // so concurrent recordings never share pool host access — and record
    // callbacks may take arbitrarily long. The reserved slot cannot be
    // reused underneath us: only this path moves a Recording slot forward.
    bool deviceLost = false;
    try {
        VkDevice device = device_->handle();
        checkVulkan(vkResetFences(device, 1, &slot->fence), "vkResetFences");
        checkVulkan(vkResetCommandBuffer(slot->command_buffer, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        checkVulkan(vkBeginCommandBuffer(slot->command_buffer, &begin_info), "vkBeginCommandBuffer");
        // GPU duration instrumentation (issue #22): bracket the recorded
        // work with a timestamp pair. The slot's queries were consumed by
        // the previous retire (fence signalled ⇒ execution complete), so
        // the reset here never races an in-flight read.
        if (timed_) {
            if (!device_->hostQueryResetEnabled())
                vkCmdResetQueryPool(slot->command_buffer, query_pool_, slot->index * 2, 2);
            vkCmdWriteTimestamp(slot->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool_, slot->index * 2);
        }
        record(slot->command_buffer);
        if (timed_) {
            vkCmdWriteTimestamp(slot->command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_,
                                slot->index * 2 + 1);
        }
        checkVulkan(vkEndCommandBuffer(slot->command_buffer), "vkEndCommandBuffer");

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &slot->command_buffer;
        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = static_cast<uint32_t>(semaphores.wait.size());
        timeline_info.pWaitSemaphoreValues = semaphores.waitValues.data();
        timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(semaphores.signal.size());
        timeline_info.pSignalSemaphoreValues = semaphores.signalValues.data();
        std::vector<VkPipelineStageFlags> wait_dst_stage_mask;
        if (!semaphores.wait.empty() || !semaphores.signal.empty()) {
            submit_info.waitSemaphoreCount = static_cast<uint32_t>(semaphores.wait.size());
            submit_info.pWaitSemaphores = semaphores.wait.data();
            submit_info.signalSemaphoreCount = static_cast<uint32_t>(semaphores.signal.size());
            submit_info.pSignalSemaphores = semaphores.signal.data();
            // Submission validation requires a non-null pWaitDstStageMask
            // whenever semaphores are waited on; timeline waits are ordered
            // by value, so the stage mask is a no-op (ALL_COMMANDS).
            wait_dst_stage_mask.assign(semaphores.wait.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            submit_info.pWaitDstStageMask = wait_dst_stage_mask.data();
            submit_info.pNext = &timeline_info;
        }
        VkResult submit_result;
        {
            // FFmpeg and every application wrapper share this queue owner.
            // A busy owner is backpressure, not a reason to block a caller.
            std::unique_lock queueLock(device_->queueMutex(queue_family_), std::try_to_lock);
            if (!queueLock.owns_lock()) {
                std::lock_guard lock(mutex_);
                recoverSlot(*slot, false);
                return std::nullopt;
            }
            submit_result = vkQueueSubmit(queue_, 1, &submit_info, slot->fence);
        }
        deviceLost = submit_result == VK_ERROR_DEVICE_LOST;
        if (submit_result != VK_SUCCESS) {
            throwVulkanError(submit_result, "vkQueueSubmit");
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        recoverSlot(*slot, deviceLost);
        throw;
    }
    Completion issued = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The id is assigned here — after vkQueueSubmit succeeded — so every
        // issued identity names genuinely in-flight work. Captured under the
        // lock: the slot cannot be reaped before the state transition.
        slot->state = Slot::State::Submitted;
        slot->completion = ++next_completion_;
        issued = slot->completion;
    }
    device_->addSubmissions();
    device_->addCpuSubmitNs(elapsedNs(start));
    return issued;
}

bool SubmissionQueue::poll(Completion completion) {
    // Validity is derived: 0 and never-issued ids are invalid; a valid id
    // that no slot carries has completed (ids leave a slot only on fence
    // signalled). No stored completed-history needed.
    std::lock_guard<std::mutex> lock(mutex_);
    if (completion == 0 || completion > next_completion_)
        return false;
    harvestIdle();
    Slot* slot = findSubmitted(completion);
    if (slot == nullptr) {
        return true;  // idempotent: a retired completion stays queryable
    }
    VkResult status = vkGetFenceStatus(device_->handle(), slot->fence);
    if (status == VK_NOT_READY) {
        return false;
    }
    checkVulkan(status, "vkGetFenceStatus");
    if (slot->waiters == 0) {
        // Unpinned: retire now (releases the retained resources).
        retireSlot(*slot);
    }
    // Pinned: the last waiter retires; the fence has signalled either way.
    return true;
}

bool SubmissionQueue::wait(Completion completion, uint64_t timeout_ns) {
    const auto start = Clock::now();
    device_->addWaits();
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completion == 0 || completion > next_completion_)
            return false;
        harvestIdle();
        slot = findSubmitted(completion);
        if (slot != nullptr) {
            // Pin the slot: while a waiter is blocked outside the lock,
            // neither the harvest nor a submitter may touch the fence, and
            // the retained resources stay until the last waiter retires.
            slot->waiters += 1;
        } else {
            // A valid id no slot carries has completed: idempotent true.
            device_->addCpuWaitNs(elapsedNs(start));
            return true;
        }
    }

    // Fence wait happens WITHOUT the queue mutex: waiting must never block
    // other submitters or waiters. The pin guarantees the fence cannot be
    // reset under us.
    VkResult waited = vkWaitForFences(device_->handle(), 1, &slot->fence, VK_TRUE, timeout_ns);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool last_waiter = --slot->waiters == 0;
        if (waited == VK_SUCCESS && last_waiter) {
            // Last observer retires: releases the retained resources and
            // makes the identity durably completed. Concurrent waiters that
            // observed fence success earlier already returned true.
            retireSlot(*slot);
        }
        // waited == SUCCESS with other waiters remaining: the fence has
        // signalled; the last waiter (or a later poll/harvest) retires.
    }
    if (waited != VK_SUCCESS && waited != VK_TIMEOUT) {
        checkVulkan(waited, "vkWaitForFences");  // throws; the pin is already released
    }
    device_->addCpuWaitNs(elapsedNs(start));
    // VK_TIMEOUT retains everything: the slot, its fence, and its retained
    // resources stay until completion (retry, drain, or destruction).
    return waited == VK_SUCCESS;
}

void SubmissionQueue::drain() {
    const auto start = Clock::now();
    std::vector<Slot*> pending;
    pending.reserve(slots_.size());
    std::vector<VkFence> fences;
    fences.reserve(slots_.size());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        harvestIdle();
        for (auto& slot : slots_) {
            if (slot->state == Slot::State::Submitted) {
                slot->waiters += 1;
                pending.push_back(slot.get());
                fences.push_back(slot->fence);
            }
        }
    }
    if (pending.empty()) {
        device_->addCpuWaitNs(elapsedNs(start));
        return;
    }
    // Pinned under the snapshot lock, so the fences cannot be reset or
    // retired while this wait runs outside the lock.
    try {
        checkVulkan(vkWaitForFences(device_->handle(), static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE,
                                    UINT64_MAX),
                    "vkWaitForFences");
    } catch (...) {
        // Unwind the pins so the slots stay harvestable for later drains.
        std::lock_guard<std::mutex> lock(mutex_);
        for (Slot* slot : pending) {
            slot->waiters -= 1;
        }
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Slot* slot : pending) {
            const bool last_waiter = --slot->waiters == 0;
            if (last_waiter && slot->state == Slot::State::Submitted) {
                retireSlot(*slot);
            }
        }
        device_->addWaits();
        device_->addCpuWaitNs(elapsedNs(start));
    }
}

void SubmissionQueue::submit_and_wait(const std::function<void(VkCommandBuffer)>& record, uint64_t timeout_ns) {
    submit_and_wait(record, TimelineSemaphores{}, timeout_ns);
}

void SubmissionQueue::submit_and_wait(const std::function<void(VkCommandBuffer)>& record,
                                      const TimelineSemaphores& semaphores, uint64_t timeout_ns) {
    std::optional<Completion> completion = submit(record, RetainedResources{}, semaphores);
    if (!completion.has_value()) {
        // The bounded pool was exhausted; a legacy caller expects a
        // synchronous convenience, so wait the pending submissions out and
        // try once more before reporting.
        drain();
        completion = submit(record, RetainedResources{}, semaphores);
        if (!completion.has_value()) {
            throw GpuException(GpuError::VulkanError, "submission queue capacity exhausted on family " +
                                                          std::to_string(queue_family_) +
                                                          "; pending submissions did not drain");
        }
    }
    if (!wait(*completion, timeout_ns)) {
        // Legacy borrowed-resources rule: the recording callback's resources
        // are caller-owned and NOT retained, so the in-flight submission
        // must complete before the exception escapes (unbounded on a wedged
        // GPU — the price of borrowed resources).
        drain();
        throw GpuException(GpuError::SubmissionTimeout, "submission on queue family " + std::to_string(queue_family_) +
                                                            " did not complete within " + std::to_string(timeout_ns) +
                                                            " ns");
    }
}

}  // namespace nemo::gpu
