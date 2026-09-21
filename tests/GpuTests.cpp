// GPU bootstrap tests (issue #2). The seam is the public nemo::gpu bootstrap
// interface: Instance/Device creation with explicit queue selection, the
// budgeted Allocator, and the SubmissionQueue submit + fence-wait helper.
//
// Tests guard themselves: with no usable Vulkan device they skip, so CI and
// machines without a device still pass. Validation-layer absence is separate
// from device absence — behavior tests run either way, and the
// zero-validation-message assertions apply only when validation is enabled.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"

namespace {

enum class BootstrapOutcome { Created, NoDevice, Failed };

// A created bootstrap, or why device creation was skipped or failed. The
// outcome enum makes the three states mutually exclusive; skipping stays in
// test scope because GTEST_SKIP returns void.
struct Bootstrap {
    std::unique_ptr<nemo::gpu::Instance> instance;
    std::unique_ptr<nemo::gpu::Device> device;
    BootstrapOutcome outcome = BootstrapOutcome::Created;  // placeholder
    std::string message;
};

Bootstrap createBootstrap(bool validation = true) {
    Bootstrap boot;
    try {
        boot.instance = nemo::gpu::Instance::create({.validation = validation});
        boot.device = nemo::gpu::Device::create(*boot.instance);
    } catch (const nemo::gpu::GpuException& error) {
        boot.outcome =
            error.errorCode() == nemo::gpu::GpuError::NoDevice ? BootstrapOutcome::NoDevice : BootstrapOutcome::Failed;
        boot.message = error.what();
    }
    return boot;
}

#define NEMO_SKIP_OR_FAIL(boot)                                                                                        \
    do {                                                                                                               \
        if ((boot).outcome == BootstrapOutcome::NoDevice)                                                              \
            GTEST_SKIP() << (boot).message;                                                                            \
        if ((boot).outcome == BootstrapOutcome::Failed) {                                                              \
            ADD_FAILURE() << "device creation failed: " << (boot).message;                                             \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

// Zero validation warnings/errors is the bootstrap acceptance bar (issue #2
// example 1). The layer emits benign info chatter (e.g. physical-device
// sorting notes); info messages do not fail the check.
void expectValidationClean(nemo::gpu::Instance& instance) {
    if (!instance.validation_enabled()) {
        return;
    }
    std::string collected;
    bool has_warnings = false;
    for (const auto& message : instance.take_debug_messages()) {
        collected += message.text + "\n";
        has_warnings = has_warnings || message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    EXPECT_FALSE(has_warnings) << "validation-layer messages:\n" << collected;
}

// Joins on scope exit so a failing ASSERT can never strand the signaler
// thread (std::terminate) and the gate is always released.
struct JoinThread {
    std::thread thread;
    ~JoinThread() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

}  // namespace

// Bootstrap creates instance + physical/logical device with explicit queue
// selection and destroys everything cleanly.
TEST(Gpu, BootstrapCreateDestroy) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    // Explicit queue selection: the selected graphics family must accept
    // graphics+compute work and the selected transfer family must accept
    // transfer work. Exact family topology is driver-dependent and not part
    // of the contract.
    const auto graphics_caps = boot.device->family_capabilities(boot.device->graphics_family());
    EXPECT_TRUE(graphics_caps & VK_QUEUE_GRAPHICS_BIT);
    EXPECT_TRUE(graphics_caps & VK_QUEUE_COMPUTE_BIT);
    EXPECT_TRUE(boot.device->family_capabilities(boot.device->transfer_family()) & VK_QUEUE_TRANSFER_BIT);

    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// Video queue selection (issue #10): when the physical device advertises a
// decode or encode video queue family, Device::create must reserve it and
// expose it; otherwise the accessor reports none. Internal consistency is
// the contract — never an assumption about driver capabilities.
TEST(Gpu, VideoQueueFamiliesReportedWhenAdvertised) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    if (boot.device->decode_family() || boot.device->encode_family()) {
        if (boot.device->decode_family()) {
            EXPECT_TRUE(boot.device->family_capabilities(*boot.device->decode_family()) &
                        VK_QUEUE_VIDEO_DECODE_BIT_KHR);
        }
        if (boot.device->encode_family()) {
            EXPECT_TRUE(boot.device->family_capabilities(*boot.device->encode_family()) &
                        VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
        }
    }

    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// The full acceptance path (issue #2 example 1): instance, device, and
// allocator are created, an empty command buffer is submitted, the fence is
// waited on, and everything is destroyed before the collected validation
// output is checked — destruction of every object must also be
// validation-clean. An empty submission proves bootstrap, resource lifetime,
// and synchronization — not GPU image processing (#8).
TEST(Gpu, EmptySubmissionCompletes) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    {
        auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = 0});
        nemo::gpu::SubmissionQueue submission(*boot.device, boot.device->transfer_family());
        // The helper resets its fence before each submit, so a fence signalled
        // by the first submission cannot let a broken second submission pass.
        submission.submit_and_wait([](VkCommandBuffer) {}, 5'000'000'000);
        submission.submit_and_wait([](VkCommandBuffer) {}, 5'000'000'000);
        // Destroy in reverse creation order: allocator, submission, device.
    }
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// Budget admission and accounting at the Nemo interface (issue #2: budgeted
// allocator wrapper; issue #97: charges are the driver's reported memory
// requirements and the working set is split by placement). Asserts configured
// budget admission, observable charged usage, release, and descriptive
// rejection. VMA block sizes, memory types, and heap-level values are
// deliberately not pinned.
TEST(Gpu, AllocatorBudgetAccounting) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    constexpr uint64_t kBudget = 1 << 20;
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = kBudget});
    EXPECT_EQ(allocator->budget(), kBudget);
    EXPECT_EQ(allocator->charged_bytes(), 0);

    const uint64_t required = allocator->buffer_allocation_bytes(512 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT_GE(required, 512u << 10) << "the charged requirement must cover the requested size";
    auto buffer = allocator->create_buffer(512 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT_NE(buffer.handle(), VK_NULL_HANDLE);
    EXPECT_EQ(buffer.size(), 512 << 10);
    EXPECT_EQ(buffer.charged_bytes(), required) << "the token reports the charge the allocator applied";
    EXPECT_EQ(allocator->charged_bytes(), required);
    EXPECT_EQ(allocator->device_charged_bytes(), required);
    EXPECT_EQ(allocator->host_charged_bytes(), 0u);

    const uint64_t requested = 768 << 10;
    try {
        auto rejected = allocator->create_buffer(requested, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ADD_FAILURE() << "over-budget allocation of " << requested << " bytes was admitted";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::BudgetExceeded);
        const std::string what = error.what();
        EXPECT_NE(what.find(std::to_string(requested)), std::string::npos)
            << "rejection does not name the requested size: " << what;
        EXPECT_NE(what.find(std::to_string(kBudget)), std::string::npos)
            << "rejection does not name the budget: " << what;
        EXPECT_NE(what.find(std::to_string(required)), std::string::npos)
            << "rejection does not name the live charge: " << what;
    }
    EXPECT_EQ(allocator->charged_bytes(), required) << "a rejected allocation must not charge";

    buffer = nemo::gpu::Buffer();
    EXPECT_EQ(allocator->charged_bytes(), 0) << "release must uncharge";
    EXPECT_EQ(allocator->device_charged_bytes(), 0u);
}

// Issue #97 narrow prerequisite (reservation before allocation): a
// reservation counts against the budget the moment it is made, so another
// caller cannot take the bytes an active working set is about to allocate;
// the matching allocation consumes it and is charged exactly once.
TEST(Gpu, AllocatorReservationPrecedesAllocation) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    constexpr uint64_t kBudget = 1 << 20;
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = kBudget});
    const uint64_t imageBytes =
        allocator->image_allocation_bytes(128, 128, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, 2);
    ASSERT_LT(imageBytes, kBudget);

    auto reservation = allocator->reserve(imageBytes);
    EXPECT_EQ(reservation.bytes(), imageBytes);
    EXPECT_EQ(allocator->reserved_bytes(), imageBytes);
    EXPECT_EQ(allocator->charged_bytes(), 0u);

    // A request that fits the raw budget but not the budget with the reserved
    // working set outstanding is refused, naming what is held.
    const uint64_t protectedBytes = allocator->buffer_allocation_bytes(
        static_cast<VkDeviceSize>(kBudget - imageBytes + imageBytes / 4), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ASSERT_GT(protectedBytes, kBudget - imageBytes);
    ASSERT_LT(protectedBytes, kBudget);
    try {
        auto protectedAllocation =
            allocator->create_buffer(static_cast<VkDeviceSize>(protectedBytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ADD_FAILURE() << "an allocation using reserved working-set bytes was admitted";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::BudgetExceeded);
        const std::string what = error.what();
        EXPECT_NE(what.find(std::to_string(imageBytes)), std::string::npos)
            << "rejection does not name what is reserved: " << what;
    }

    // Releasing returns the budget: the same request is admitted, so the
    // rejection above was the reservation and not the configured budget.
    reservation.release();
    EXPECT_EQ(allocator->reserved_bytes(), 0u);
    auto admitted =
        allocator->create_buffer(static_cast<VkDeviceSize>(protectedBytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT_EQ(admitted.charged_bytes(), protectedBytes);
    admitted = nemo::gpu::Buffer();
    EXPECT_EQ(allocator->charged_bytes(), 0u);

    // Consuming a reservation charges once and leaves the token empty and
    // unusable.
    reservation = allocator->reserve(imageBytes);
    // A create_* that fails leaves the token intact, so the retry still has
    // the working set reserved.
    try {
        auto refused = allocator->create_image(0, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, 0,
                                               &reservation);
        ADD_FAILURE() << "a zero-extent image was created";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::InvalidRequest);
    }
    EXPECT_EQ(reservation.bytes(), imageBytes) << "a failed allocation must leave the reservation intact";

    auto image = allocator->create_image(128, 128, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, 2,
                                         &reservation);
    EXPECT_EQ(image.charged_bytes(), imageBytes);
    EXPECT_EQ(allocator->charged_bytes(), imageBytes);
    EXPECT_EQ(allocator->device_charged_bytes(), imageBytes);
    EXPECT_EQ(allocator->reserved_bytes(), 0u) << "a consumed reservation must not stay reserved";
    EXPECT_EQ(reservation.bytes(), 0u);
    try {
        auto reused = allocator->create_buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               nemo::gpu::MemoryPreference::Device, &reservation);
        ADD_FAILURE() << "a consumed reservation token was accepted again";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::InvalidRequest);
    }

    // Host-visible placements are charged against the same budget and tracked
    // separately from device-local ones.
    auto staging =
        allocator->create_buffer(64 << 10, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nemo::gpu::MemoryPreference::HostMapped);
    EXPECT_GE(staging.charged_bytes(), 64u << 10);
    EXPECT_EQ(allocator->host_charged_bytes(), staging.charged_bytes());
    EXPECT_EQ(allocator->charged_bytes(), imageBytes + staging.charged_bytes());
    staging = nemo::gpu::Buffer();
    EXPECT_EQ(allocator->host_charged_bytes(), 0u);
    EXPECT_EQ(allocator->charged_bytes(), imageBytes);
}

// Issue #97 narrow prerequisite (eligible eviction before allocation): a
// request that does not fit first asks the registered eviction source to
// release reclaimable residency — including for callers that never mention
// eviction — and a request that still cannot fit fails honestly instead of
// degrading. The registration is RAII.
TEST(Gpu, AllocatorEligibleEvictionPrecedesAllocation) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    constexpr uint64_t kBudget = 1 << 20;
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = kBudget});
    const uint64_t eligibleBytes = allocator->buffer_allocation_bytes(640 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const uint64_t requestBytes = allocator->buffer_allocation_bytes(512 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ASSERT_GT(eligibleBytes + requestBytes, kBudget) << "the case needs a request that cannot fit";

    // Reclaimable residency. Whether to drop it is the owner's decision; the
    // allocator only asks for its bytes.
    nemo::gpu::Buffer eligible = allocator->create_buffer(640 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    uint64_t wanted = 0;
    unsigned calls = 0;
    auto evictor = allocator->register_evictor([&](uint64_t bytes) {
        ++calls;
        wanted = bytes;
        const uint64_t freed = eligible.charged_bytes();
        eligible = nemo::gpu::Buffer();
        return freed;
    });

    auto admitted = allocator->create_buffer(512 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT_EQ(calls, 1u);
    EXPECT_GT(wanted, 0u) << "the eviction source is told how much the request still needs";
    EXPECT_EQ(eligible.handle(), VK_NULL_HANDLE) << "the eligible residency must have been released";
    EXPECT_EQ(admitted.charged_bytes(), requestBytes);

    // Nothing eligible remains: the request is refused, naming the request,
    // the live charge and the budget, and no output is degraded to fit.
    const uint64_t refusedBytes = allocator->buffer_allocation_bytes(768 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ASSERT_GT(refusedBytes + admitted.charged_bytes(), kBudget);
    try {
        auto refused = allocator->create_buffer(768 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ADD_FAILURE() << "an allocation that cannot fit even after eviction was admitted";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::BudgetExceeded);
        const std::string what = error.what();
        EXPECT_NE(what.find(std::to_string(refusedBytes)), std::string::npos)
            << "rejection does not name the requested size: " << what;
        EXPECT_NE(what.find(std::to_string(admitted.charged_bytes())), std::string::npos)
            << "rejection does not name the live charge: " << what;
        EXPECT_NE(what.find(std::to_string(kBudget)), std::string::npos)
            << "rejection does not name the budget: " << what;
    }

    // Deregistration is RAII: the source is no longer asked and the failure
    // stays honest.
    evictor.reset();
    EXPECT_FALSE(evictor.registered());
    const unsigned before = calls;
    try {
        auto refused = allocator->create_buffer(768 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ADD_FAILURE() << "an allocation that cannot fit was admitted after deregistration";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::BudgetExceeded);
    }
    EXPECT_EQ(calls, before) << "a deregistered eviction source must not be called";
}

// Issue #97 eviction registration lifetime: deregistering a source waits for
// an invocation that is already running, so the registrant's captured state
// is still valid when reset() returns, and an eviction callback never runs
// under the allocator's accounting lock (the callback below spins while the
// main thread observes the allocator).
TEST(Gpu, AllocatorEvictorDeregistrationWaitsForRunningCallback) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    constexpr uint64_t kBudget = 1 << 20;
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = kBudget});
    auto resident = allocator->create_buffer(768 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::atomic<bool> deregistered{false};
    std::atomic<unsigned> observations{0};
    auto handle = allocator->register_evictor([&](uint64_t) {
        inside = true;
        while (!release)
            std::this_thread::yield();
        return 0u;  // nothing eligible: the admitting caller fails honestly
    });

    // The admission that cannot fit runs the callback on its own thread. The
    // release guard is declared after the joins so it runs first: a failing
    // expectation can never leave the callback spinning.
    JoinThread admission;
    admission.thread = std::thread([&] {
        try {
            auto refused = allocator->create_buffer(768 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        } catch (const nemo::gpu::GpuException&) {
        }
    });
    JoinThread deregistration;
    struct ReleaseOnExit {
        std::atomic<bool>& flag;
        ~ReleaseOnExit() { flag = true; }
    } releaseOnExit{release};
    for (unsigned spin = 0; spin != 200000 && !inside; ++spin) {
        // The callback holds no allocator lock, so accounting stays readable.
        observations += allocator->charged_bytes() > 0 ? 1u : 0u;
        std::this_thread::yield();
    }
    ASSERT_TRUE(inside) << "the eligible eviction source was not asked";
    EXPECT_GT(observations, 0u) << "accounting must stay readable while a callback runs";

    deregistration.thread = std::thread([&] {
        handle.reset();
        deregistered = true;
    });
    for (unsigned spin = 0; spin != 200000 && !deregistered; ++spin)
        std::this_thread::yield();
    EXPECT_FALSE(deregistered) << "deregistration returned while a callback was still running";
}

TEST(Gpu, AllocatorDeregistrationCancelsSnapshottedCallbacks) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = 1u << 20});
    auto resident = allocator->create_buffer(768u << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    nemo::gpu::Allocator::EvictorHandle removed;
    bool invokedAfterRemoval = false;
    auto canceller = allocator->register_evictor([&](uint64_t) {
        // Admission has snapshotted both registrations. Retire the second
        // owner's callback before its turn; it must never run after reset.
        removed.reset();
        return 0u;
    });
    removed = allocator->register_evictor([&](uint64_t) {
        invokedAfterRemoval = true;
        return 0u;
    });
    EXPECT_THROW(allocator->create_buffer(768u << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT), nemo::gpu::GpuException);
    EXPECT_FALSE(invokedAfterRemoval);
}

// Issue #97 active retention: an allocation stays charged while any owner is
// alive — here the retained token a submission holds — so dropping the
// allocating handle (cancellation) never frees or uncharges work still in
// flight, and the charge's bytes stay unavailable to other callers.
TEST(Gpu, AllocatorChargeFollowsFinalOwner) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    constexpr uint64_t kBudget = 1 << 20;
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = kBudget});
    auto buffer = allocator->create_buffer(256 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const uint64_t charge = buffer.charged_bytes();
    EXPECT_EQ(allocator->charged_bytes(), charge);

    std::shared_ptr<const void> retained = buffer.retain();
    buffer = nemo::gpu::Buffer();
    EXPECT_NE(retained, nullptr);
    EXPECT_EQ(allocator->charged_bytes(), charge) << "the charge follows the retained owner";
    try {
        auto refused = allocator->create_buffer(static_cast<VkDeviceSize>(kBudget), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ADD_FAILURE() << "budget held by an active token was handed out";
    } catch (const nemo::gpu::GpuException& error) {
        EXPECT_EQ(error.errorCode(), nemo::gpu::GpuError::BudgetExceeded);
    }

    retained.reset();
    EXPECT_EQ(allocator->charged_bytes(), 0u) << "the final owner releases the charge";
}

// Issue #22 shared retained GPU execution: the asynchronous SubmissionQueue
// contract — non-blocking capacity admission, unique completion identity,
// retained lifetimes, exception recovery, thread-safe shared queue, own-
// fence teardown. Gating uses a host-signalled timeline semaphore (the
// device enables the timelineSemaphore feature at creation) so tests never
// depend on GPU completion timing; every gate is released by a detached-
// scope signaler thread so a failing ASSERT can never hang the suite.
namespace {

using nemo::gpu::GpuError;
using nemo::gpu::GpuException;
using nemo::gpu::SubmissionQueue;

VkSemaphore createTimelineSemaphore(VkDevice device) {
    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type_info;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    nemo::gpu::checkVulkan(vkCreateSemaphore(device, &info, nullptr, &semaphore), "vkCreateSemaphore");
    return semaphore;
}

void signalTimeline(VkDevice device, VkSemaphore semaphore, std::uint64_t value) {
    VkSemaphoreSignalInfo signal_info{};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal_info.semaphore = semaphore;
    signal_info.value = value;
    nemo::gpu::checkVulkan(vkSignalSemaphore(device, &signal_info), "vkSignalSemaphore");
}

// Signals the gate after a delay, independent of the test body: a failing
// ASSERT in the main thread must not leave a gated submission pending.
std::thread delayedSignal(VkDevice device, VkSemaphore semaphore, std::uint64_t value, int delay_ms) {
    return std::thread([device, semaphore, value, delay_ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        signalTimeline(device, semaphore, value);
    });
}

}  // namespace

// submit → poll/wait lifecycle: identity is durable — a waited completion
// keeps reporting true idempotently (also after auto-harvest), invalid
// identities are rejected, and successive submissions never collide.
TEST(Gpu, AsyncSubmitPollWaitLifecycle) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    {
        SubmissionQueue queue(*boot.device, boot.device->transfer_family(), 2);
        auto first = queue.submit([](VkCommandBuffer) {}, {});
        ASSERT_TRUE(first.has_value());
        EXPECT_NE(*first, 0u);
        EXPECT_TRUE(queue.wait(*first, 5'000'000'000));

        // Idempotent retirement: the completed identity stays queryable.
        EXPECT_TRUE(queue.poll(*first));
        EXPECT_TRUE(queue.wait(*first, 0));

        auto second = queue.submit([](VkCommandBuffer) {}, {});
        ASSERT_TRUE(second.has_value());
        EXPECT_NE(*second, *first) << "completion identities must be unique across slot reuse";
        EXPECT_TRUE(queue.wait(*second, 5'000'000'000));
        EXPECT_TRUE(queue.poll(*second));

        // Invalid identities are rejected without touching queue state.
        EXPECT_FALSE(queue.poll(0));
        EXPECT_FALSE(queue.wait(0, 0));
        EXPECT_FALSE(queue.poll(7'777'777'777'777ull));
        EXPECT_FALSE(queue.wait(7'777'777'777'777ull, 0));
    }
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// Capacity admission is immediate: a full pool rejects without waiting for
// the GPU, and the freed slot becomes available after completion.
TEST(Gpu, AsyncCapacityRejectsImmediately) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    VkSemaphore gate = createTimelineSemaphore(boot.device->handle());
    {
        SubmissionQueue queue(*boot.device, boot.device->transfer_family(), 1);
        SubmissionQueue::TimelineSemaphores waits;
        waits.wait = {gate};
        waits.waitValues = {1};
        auto gated = queue.submit([](VkCommandBuffer) {}, {}, waits);
        // The gate is released by a timer thread, not the test body: a
        // failing ASSERT must never leave a gated submission pending.
        JoinThread signaler{delayedSignal(boot.device->handle(), gate, 1, 1000)};
        ASSERT_TRUE(gated.has_value());

        const auto start = std::chrono::steady_clock::now();
        auto rejected = queue.submit([](VkCommandBuffer) {}, {});
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_FALSE(rejected.has_value()) << "a full pool must reject immediately";
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);

        EXPECT_TRUE(queue.wait(*gated, 5'000'000'000));

        auto retried = queue.submit([](VkCommandBuffer) {}, {});
        ASSERT_TRUE(retried.has_value()) << "a reaped slot must admit a new submission";
        EXPECT_TRUE(queue.wait(*retried, 5'000'000'000));
    }
    vkDestroySemaphore(boot.device->handle(), gate, nullptr);
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// Retained lifetimes: resources stay alive while a submission is in flight
// even when the Completion identity is dropped (cancellation by dropping
// the result handle), and are released only after the fence completes.
TEST(Gpu, RetainedResourcesReleasedOnlyAfterCompletion) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    VkSemaphore gate = createTimelineSemaphore(boot.device->handle());
    {
        SubmissionQueue queue(*boot.device, boot.device->transfer_family(), 2);
        auto token = std::make_shared<int>(42);
        std::weak_ptr<const int> observed = token;

        SubmissionQueue::TimelineSemaphores waits;
        waits.wait = {gate};
        waits.waitValues = {1};
        auto pending = queue.submit([](VkCommandBuffer) {}, {token}, waits);
        JoinThread signaler{delayedSignal(boot.device->handle(), gate, 1, 1000)};
        ASSERT_TRUE(pending.has_value());

        // Cancellation by dropping the identity while the submission is
        // still gated: the retained token must stay alive.
        token.reset();
        EXPECT_FALSE(observed.expired()) << "retained resources dropped with the completion identity";

        EXPECT_TRUE(queue.wait(*pending, 5'000'000'000));
        queue.drain();
        EXPECT_TRUE(observed.expired()) << "resources must be released only after the fence completed";
    }
    vkDestroySemaphore(boot.device->handle(), gate, nullptr);
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// A recording callback that throws must propagate, recover its slot and
// resources, and leave the queue fully usable.
TEST(Gpu, RecordExceptionRecoversSlot) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    {
        SubmissionQueue queue(*boot.device, boot.device->transfer_family(), 2);
        EXPECT_THROW(
            {
                try {
                    queue.submit([](VkCommandBuffer) { throw std::runtime_error("record failed mid-recording"); }, {});
                } catch (const std::runtime_error& error) {
                    EXPECT_STREQ(error.what(), "record failed mid-recording");
                    throw;
                }
            },
            std::runtime_error);

        auto after = queue.submit([](VkCommandBuffer) {}, {});
        ASSERT_TRUE(after.has_value());
        EXPECT_TRUE(queue.wait(*after, 5'000'000'000));

        // The failed attempt issued no identity; the next successful submit
        // must not collide with any earlier identity, and the queue still
        // completes work normally.
        auto next = queue.submit([](VkCommandBuffer) {}, {});
        ASSERT_TRUE(next.has_value());
        EXPECT_NE(*next, *after);
        EXPECT_TRUE(queue.wait(*next, 5'000'000'000));

        queue.drain();
    }
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// One persistent queue (Device::submissions) shared by concurrent
// submitters running REAL recorded commands (per-submission vkCmdFillBuffer
// into distinct words of a host-mapped buffer): every submission completes
// exactly once, the recorded writes all land, and the device counters track
// the work. Each submission retains the buffer token, exercising the
// retained-lifetime machinery on the shared path.
TEST(Gpu, ConcurrentSubmittersSharePersistentQueue) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = 4096});
    auto buffer =
        allocator->create_buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nemo::gpu::MemoryPreference::HostMapped);
    ASSERT_NE(buffer.mapped(), nullptr);
    std::memset(buffer.mapped(), 0, 4096);

    const auto before = boot.device->submissionStats();
    SubmissionQueue& queue = boot.device->submissions(boot.device->transfer_family());
    std::atomic<int> completed{0};
    std::atomic<int> failures{0};
    {
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&queue, &completed, &failures, &buffer, worker] {
                for (int round = 0; round < 25; ++round) {
                    const uint32_t entry = static_cast<uint32_t>(worker) * 25 + static_cast<uint32_t>(round);
                    std::optional<SubmissionQueue::Completion> pending;
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    do {
                        pending = queue.submit(
                            [&buffer, entry](VkCommandBuffer command) {
                                vkCmdFillBuffer(command, buffer.handle(), entry * 4, 4, 0xC0FF0000u + entry);
                            },
                            {buffer.retain()});
                        if (!pending)
                            std::this_thread::yield();
                    } while (!pending && std::chrono::steady_clock::now() < deadline);
                    if (pending.has_value() && queue.wait(*pending, 30'000'000'000)) {
                        completed.fetch_add(1);
                    } else {
                        failures.fetch_add(1);
                    }
                }
            });
        }
        for (std::thread& thread : workers) {
            thread.join();
        }
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(completed.load(), 100);
    // The recorded commands actually executed: every filled word holds its
    // per-submission pattern (host-mapped readback after fence completion).
    const uint32_t* words = static_cast<const uint32_t*>(buffer.mapped());
    for (uint32_t entry = 0; entry < 100; ++entry) {
        EXPECT_EQ(words[entry], 0xC0FF0000u + entry) << "entry " << entry;
    }
    queue.drain();

    const auto after = boot.device->submissionStats();
    EXPECT_GE(after.submissions, before.submissions + 100);
    EXPECT_GE(after.completions, before.completions + 100);
    EXPECT_GE(after.waits, before.waits + 100);
    EXPECT_GT(after.cpuSubmitNs, before.cpuSubmitNs);
    EXPECT_GT(after.cpuWaitNs, before.cpuWaitNs);
    if (boot.device->family_timestamp_bits(boot.device->transfer_family()) > 0 &&
        (boot.device->hostQueryResetEnabled() || (boot.device->family_capabilities(boot.device->transfer_family()) &
                                                  (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))) {
        EXPECT_GE(after.gpuTimedSubmissions, before.gpuTimedSubmissions + 100);
        EXPECT_GT(after.gpuExecutionNs, before.gpuExecutionNs);
    } else {
        EXPECT_EQ(after.gpuTimedSubmissions, before.gpuTimedSubmissions);
        EXPECT_EQ(after.gpuExecutionNs, before.gpuExecutionNs);
    }

    buffer = nemo::gpu::Buffer();
    allocator.reset();
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// Legacy timeout semantics: the wait reports SubmissionTimeout, but the
// borrowed-resources rule drains the in-flight submission before the
// exception escapes (the gated work is released by the delayed signaler).
TEST(Gpu, LegacyTimeoutDrainsBeforeThrowing) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    VkSemaphore gate = createTimelineSemaphore(boot.device->handle());
    {
        SubmissionQueue queue(*boot.device, boot.device->transfer_family(), 2);
        SubmissionQueue::TimelineSemaphores waits;
        waits.wait = {gate};
        waits.waitValues = {1};
        JoinThread signaler{delayedSignal(boot.device->handle(), gate, 1, 500)};
        const auto start = std::chrono::steady_clock::now();
        try {
            queue.submit_and_wait([](VkCommandBuffer) {}, waits, 1'000'000);
            ADD_FAILURE() << "a gated submission must time out";
        } catch (const GpuException& error) {
            EXPECT_EQ(error.errorCode(), GpuError::SubmissionTimeout);
            const std::string what = error.what();
            EXPECT_NE(what.find("1000000"), std::string::npos) << "timeout message must name the timeout: " << what;
        }
        // drain-before-throw: the exception returns only after the gated
        // submission actually completed (signaler fired at ~500 ms).
        EXPECT_GE(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
            400);
        signaler.thread.join();

        // The queue recovered: legacy convenience still completes.
        queue.submit_and_wait([](VkCommandBuffer) {}, 5'000'000'000);
    }
    vkDestroySemaphore(boot.device->handle(), gate, nullptr);
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}

// Teardown drains only this queue's own pending fences (no device idle,
// no hang) and releases retained resources as each fence completes.
TEST(Gpu, QueueTeardownDrainsPendingSubmissions) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    VkSemaphore gate = createTimelineSemaphore(boot.device->handle());
    std::weak_ptr<const int> observed;
    {
        auto queue = std::make_unique<SubmissionQueue>(*boot.device, boot.device->transfer_family(), 2);
        auto token = std::make_shared<int>(7);
        observed = token;
        SubmissionQueue::TimelineSemaphores waits;
        waits.wait = {gate};
        waits.waitValues = {1};
        auto pending = queue->submit([](VkCommandBuffer) {}, {token}, waits);
        JoinThread signaler{delayedSignal(boot.device->handle(), gate, 1, 300)};
        ASSERT_TRUE(pending.has_value());
        token.reset();

        queue.reset();  // drains its own pending fence while the gate is closed
        EXPECT_TRUE(observed.expired()) << "teardown must release retained resources after completion";
    }
    vkDestroySemaphore(boot.device->handle(), gate, nullptr);
    boot.device.reset();
    expectValidationClean(*boot.instance);
    boot.instance.reset();
}
