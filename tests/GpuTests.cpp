// GPU bootstrap tests (issue #2). The seam is the public nemo::gpu bootstrap
// interface: Instance/Device creation with explicit queue selection, the
// budgeted Allocator, and the SubmissionQueue submit + fence-wait helper.
//
// Tests guard themselves: with no usable Vulkan device they skip, so CI and
// machines without a device still pass. Validation-layer absence is separate
// from device absence — behavior tests run either way, and the
// zero-validation-message assertions apply only when validation is enabled.

#include <gtest/gtest.h>

#include <memory>
#include <string>

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
// allocator wrapper). Asserts configured-budget admission, observable charged
// usage, release, and descriptive rejection. VMA block sizes, memory types,
// and heap-level values are deliberately not pinned.
TEST(Gpu, AllocatorBudgetAccounting) {
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    ASSERT_NE(boot.device, nullptr);

    constexpr uint64_t kBudget = 1 << 20;
    auto allocator = nemo::gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = kBudget});
    EXPECT_EQ(allocator->budget(), kBudget);
    EXPECT_EQ(allocator->charged_bytes(), 0);

    auto buffer = allocator->create_buffer(512 << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT_NE(buffer.handle(), VK_NULL_HANDLE);
    EXPECT_EQ(buffer.size(), 512 << 10);
    EXPECT_EQ(allocator->charged_bytes(), 512 << 10);

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
    }
    EXPECT_EQ(allocator->charged_bytes(), 512 << 10) << "a rejected allocation must not charge";

    buffer = nemo::gpu::Buffer();
    EXPECT_EQ(allocator->charged_bytes(), 0) << "release must uncharge";
}
