#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Device.hpp"

namespace nemo::gpu {

// A bounded, reusable-slot submission path for one queue with a documented
// lifetime and completion contract (spec section 10.2, issue #22 shared
// retained GPU execution).
//
// Async contract (`submit`/`poll`/`wait`/`drain`):
// - `submit` records `record` into a slot's primary command buffer and
//   submits it to the bound queue without waiting. Capacity admission is
//   non-blocking: when every slot is in use — or the slot bookkeeping is
//   momentarily held by another submit/harvest — it returns std::nullopt
//   immediately — never waiting for the GPU or another recording thread.
//   On success it returns a `Completion` id from the queue-local monotonic
//   counter, assigned only after vkQueueSubmit succeeded — an id always
//   names a genuinely in-flight submission.
// - Completion identity is durable and derived, not stored: a valid id
//   (1 ≤ id ≤ highest issued) that no longer appears on any active slot is
//   completed, because ids leave a slot only when its fence has signalled
//   (retirement) and failed recordings/submits never issue one. poll/wait
//   therefore keep returning true idempotently forever after completion,
//   including after auto-harvest — a late observer can never mistake a
//   completed submission for an invalid one. Ids above the highest issued
//   and 0 report false.
// - `retained` holds shared_ptr tokens for every resource the recorded
//   commands reference. They are released when the fence is observed
//   signalled (first unpinned observation reaps; concurrent waiters report
//   fence success and the last one retires) — a timed-out submission, a
//   dropped `Completion` handle (cancellation by dropping the result
//   identity), or a caller that loses interest can never strand
//   use-after-free. On a recording failure the resources are released
//   immediately — the command buffer never reached the GPU. On a
//   device-loss submission is quarantined in its slot until teardown and
//   rejects future admission. Other failed submissions release their owners.
// - `poll(completion)` reports completion without blocking. `wait(
//   completion, timeout_ns)` blocks at most `timeout_ns`; VK_TIMEOUT
//   returns false and retains the resources. A wait loses nothing by
//   racing another waiter: fence success is reported by every waiter, and
//   resource retirement happens exactly once, by the last observer.
// - `drain()` waits for every submission pending at call time to complete
//   (own fences only) and releases their retained resources. It never
//   calls vkDeviceWaitIdle and never waits for other SubmissionQueue
//   wrappers' work beyond what this queue submitted.
// - The destructor drains its own pending fences (issue #22: no device
//   idle) so an in-flight or timed-out submission can never reference
//   freed pool memory. Teardown retries transient wait failures; only device
//   loss is terminal. Callers must stop accessing the wrapper before teardown.
//
// Legacy contract (`submit_and_wait`, unchanged signatures):
// - Synchronous convenience over the slot machinery: records, submits, and
//   waits for the fence. On timeout it drains before throwing
//   GpuException(SubmissionTimeout) — legacy callers' recording-callback
//   resources are borrowed, not retained, so the submission must actually
//   complete before the exception escapes (this makes the legacy effective
//   wait unbounded on a wedged GPU; that is the price of borrowed
//   resources). The fence is reset before every submit, so a submission
//   whose commands never run cannot be confused with one completed by a
//   previous submit.
//
// Threading contract: one SubmissionQueue per queue is no longer required —
// every method is threadsafe. Each slot owns its own VkCommandPool (pools
// are externally synchronized against concurrent recording/reset of their
// command buffers), so recording never needs a shared lock. Slot state and
// identity bookkeeping are serialized on the queue's mutex; `wait` and
// `drain` never block while holding it. vkQueueSubmit is serialized across
// ALL SubmissionQueue wrappers and FFmpeg on the family via
// Device::queueMutex(family). A slot being recorded or waited on is never
// handed to another submitter.
class SubmissionQueue {
public:
    // Resource tokens kept alive until the submission's fence completes.
    using RetainedResources = std::vector<std::shared_ptr<const void>>;
    // Queue-local monotonic submission identity; 0 is never a valid id.
    using Completion = std::uint64_t;

    // Timeline-semaphore dependency struct (issue #10 interop): the
    // submitted command buffer waits for each `wait` semaphore at
    // `waitValues` before executing, and signals each `signal` semaphore at
    // `signalValues` when the command buffer completes. wait/waitValues and
    // signal/signalValues must be equal-length. Zero wait values are valid.
    struct TimelineSemaphores {
        std::vector<VkSemaphore> wait;
        std::vector<std::uint64_t> waitValues;
        std::vector<VkSemaphore> signal;
        std::vector<std::uint64_t> signalValues;
    };

    // Binds queue index 0 of `queue_family` with `capacity` reusable slots.
    // `device` must outlive the SubmissionQueue (the queue handle and device
    // handle are borrowed). Throws GpuException on an unknown family, a
    // zero capacity, malformed semaphores, or any Vulkan setup failure.
    SubmissionQueue(Device& device, uint32_t queue_family, size_t capacity = 4);
    ~SubmissionQueue();

    SubmissionQueue(const SubmissionQueue&) = delete;
    SubmissionQueue& operator=(const SubmissionQueue&) = delete;

    // Records `record` into a free slot, submits it, and returns the
    // completion identity — or std::nullopt when the bounded pool is full
    // or the bookkeeping mutex is momentarily held by another submit/
    // harvest or queue owner is busy (try-lock admission). The recording
    // callback and Vulkan driver calls run on this worker; submit never
    // waits for GPU completion. Throws on recording or Vulkan failure.
    std::optional<Completion> submit(const std::function<void(VkCommandBuffer)>& record, RetainedResources retained,
                                     const TimelineSemaphores& semaphores = {});

    // True when `completion` has completed — idempotently true forever
    // after; false while still in flight, or for an invalid identity (0 or
    // never issued).
    bool poll(Completion completion);

    // Waits up to `timeout_ns` for `completion`. True once the fence has
    // signalled — idempotently true afterwards, with resources retired by
    // the last observer. False: timed out (resources stay retained; retry
    // or drain) or an invalid identity.
    bool wait(Completion completion, uint64_t timeout_ns);

    // Waits (unbounded) for every submission pending at call time and
    // releases their retained resources. Own fences only — no device idle.
    void drain();

    // Synchronous convenience forms. Same signatures and GpuException
    // behavior as before; implemented over the slot pool. On timeout they
    // drain before throwing SubmissionTimeout (borrowed-resources rule
    // above); on capacity exhaustion they drain and retry once, then throw.
    void submit_and_wait(const std::function<void(VkCommandBuffer)>& record, uint64_t timeout_ns);
    void submit_and_wait(const std::function<void(VkCommandBuffer)>& record, const TimelineSemaphores& semaphores,
                         uint64_t timeout_ns);

private:
    // One self-contained submission unit: its own command pool (pool host
    // access is externally synchronized, so no two recordings may ever
    // share one), primary command buffer, fence, and timestamp query pair.
    // Slots are heap-stable so a fence waited on outside the lock can never
    // dangle.
    struct Slot {
        uint32_t index = 0;  // slot number: command buffer and query-pair index
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        enum class State { Idle, Recording, Submitted, Lost } state = State::Idle;
        Completion completion = 0;  // identity of the current submission; 0 = none
        RetainedResources retained;
        unsigned waiters = 0;  // wait()/drain() pins holding this slot outside the lock
    };

    void recoverSlot(Slot& slot, bool deviceLost);  // lock held: handle a failed submission
    void harvestIdle();                             // lock held: retire signalled unpinned slots
    Slot* findSubmitted(Completion completion);     // lock held
    void retireSlot(Slot& slot);                    // lock held: fence signalled + no pins left

    Device* device_ = nullptr;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    // GPU duration instrumentation (issue #22): one timestamp query pair per
    // slot, allocated when the family reports timestampValidBits > 0.
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    bool timed_ = false;
    uint32_t timestamp_bits_ = 0;
    std::vector<std::unique_ptr<Slot>> slots_;
    // Identity bookkeeping: ids are assigned only on successful submit and
    // validity is derived — a valid id absent from every slot is completed.
    Completion next_completion_ = 0;
    bool poisoned_ = false;  // device loss: bounded quarantine, no further admission
    std::mutex mutex_;
};

}  // namespace nemo::gpu
