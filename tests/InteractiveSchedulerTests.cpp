#include "nemo/eval/ViewerScheduler.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace nemo;
using namespace nemo::eval;

TEST(Interactive, FullQueueReportsBackpressureWithoutDroppingAnotherViewer) {
    ViewerScheduler scheduler(1);
    ASSERT_TRUE(scheduler.submit({}, {}, 1));
    EXPECT_FALSE(scheduler.submit({}, {}, 1, static_cast<ViewerDestination>(2)));
    EXPECT_EQ(scheduler.counts().queued, 1u);
    EXPECT_EQ(scheduler.counts().dropped, 1u);
    const auto accepted = scheduler.take();
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->destination, ViewerDestination::Interactive);
    EXPECT_TRUE(scheduler.complete(*accepted, true));
}

TEST(Interactive, CacheHistorySurvivesScrubButCancellationCannotBeResurrected) {
    ViewerScheduler scheduler;
    ASSERT_TRUE(scheduler.submit({}, {}, 1));
    const auto first = scheduler.take();
    ASSERT_TRUE(first);
    EvaluationRequest next;
    next.localTime = 1;
    ASSERT_TRUE(scheduler.submit({}, next, 2));
    EXPECT_FALSE(scheduler.isCurrent(*first));
    EXPECT_TRUE(scheduler.isCacheCurrent(*first));
    scheduler.cancel(1);
    EXPECT_FALSE(scheduler.isCacheCurrent(*first));
    const auto newer = scheduler.take();
    ASSERT_TRUE(newer);
    EXPECT_TRUE(scheduler.isCacheCurrent(*newer));
    scheduler.cancel(3);
    ASSERT_TRUE(scheduler.submit({}, {}, 3));
    EXPECT_FALSE(scheduler.isCacheCurrent(*first));
    const auto resumed = scheduler.take();
    ASSERT_TRUE(resumed);
    EXPECT_TRUE(scheduler.isCacheCurrent(*resumed));
}

TEST(Interactive, CurrentFramePreemptsLazyRangeAndOnlyExplicitFramesAreTaken) {
    ViewerScheduler scheduler;
    Document document;
    EvaluationRequest request;
    ASSERT_TRUE(scheduler.requestRange(document, request, 10, 12, 1));
    request.localTime = 42;
    ASSERT_TRUE(scheduler.submit(document, request, 1));
    const auto interactive = scheduler.take();
    ASSERT_TRUE(interactive);
    EXPECT_EQ(interactive->kind, ViewerRequestKind::Render);
    EXPECT_EQ(interactive->request.localTime, 42);
    EXPECT_TRUE(scheduler.complete(*interactive, true));
    for (const int frame : {10, 11, 12}) {
        const auto cached = scheduler.take();
        ASSERT_TRUE(cached);
        EXPECT_EQ(cached->kind, ViewerRequestKind::CacheRange);
        EXPECT_EQ(cached->request.localTime, frame);
        EXPECT_TRUE(scheduler.complete(*cached, true));
    }
    EXPECT_FALSE(scheduler.take());
}

TEST(Interactive, SupersedingOneViewerPreservesOtherDestinationAndSnapshot) {
    ViewerScheduler scheduler(2);
    Document document;
    CommandStack commands(document);
    const NodeId color = document.graph.addNode("constcolor", "color");
    commands.push(setParamCommand(color, "color", "0.1 0.2 0.3 1"));
    EvaluationRequest request;
    const auto otherViewer = static_cast<ViewerDestination>(2);
    ASSERT_TRUE(scheduler.submit(document, request, 1));
    const auto old = scheduler.take();
    ASSERT_TRUE(old);
    ASSERT_TRUE(scheduler.submit(document, request, 1, otherViewer));
    const auto other = scheduler.take();
    ASSERT_TRUE(other);
    commands.push(setParamCommand(color, "color", "0.7 0.8 0.9 1"));
    ASSERT_TRUE(scheduler.submit(document, request, 2));
    EXPECT_EQ(old->document->graph.nodeByName("color")->params.at("color"), "0.1 0.2 0.3 1");
    EXPECT_FALSE(scheduler.complete(*old, true));
    EXPECT_TRUE(scheduler.complete(*other, true));
    const auto current = scheduler.take();
    ASSERT_TRUE(current);
    EXPECT_EQ(current->document->graph.nodeByName("color")->params.at("color"), "0.7 0.8 0.9 1");
    EXPECT_TRUE(scheduler.complete(*current, true));
    EXPECT_EQ(scheduler.counts().staleRejected, 1u);
}

TEST(Interactive, CancellationRejectsInflightAndSupersededBacklogIsCounted) {
    ViewerScheduler scheduler;
    Document document;
    EvaluationRequest request;
    ASSERT_TRUE(scheduler.requestRange(document, request, 0, 999999, 1));
    const auto inflight = scheduler.take();
    ASSERT_TRUE(inflight);
    ASSERT_TRUE(scheduler.requestRange(document, request, 30, 31, 2));
    EXPECT_EQ(scheduler.counts().dropped, 999999u);
    EXPECT_EQ(scheduler.counts().queued, 2u);
    scheduler.cancel(3);
    EXPECT_FALSE(scheduler.complete(*inflight, true));
    EXPECT_FALSE(scheduler.take());
    EXPECT_EQ(scheduler.counts().dropped, 1000001u);
    ASSERT_TRUE(scheduler.submit(document, request, 4));
    const auto resumed = scheduler.take();
    ASSERT_TRUE(resumed);
    EXPECT_TRUE(scheduler.complete(*resumed, true));
}

TEST(Interactive, RangeAtLastRepresentableFrameTerminatesWithoutWrapping) {
    ViewerScheduler scheduler;
    const int last = std::numeric_limits<int>::max();
    ASSERT_TRUE(scheduler.requestRange({}, {}, last, last, 1));
    const auto frame = scheduler.take();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->request.localTime, last);
    EXPECT_TRUE(scheduler.complete(*frame, true));
    EXPECT_FALSE(scheduler.take());
    EXPECT_EQ(scheduler.counts().queued, 0u);
}

TEST(Interactive, CancelledSubmissionRetainsResourcesUntilActualGpuCompletion) {
    auto instance = gpu::Instance::create({.validation = true});
    std::unique_ptr<gpu::Device> device;
    try {
        device = gpu::Device::create(*instance);
    } catch (const gpu::GpuException& error) {
        if (error.errorCode() == gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }
    auto& queue = device->submissions(device->transfer_family());
    // A real host-signalled semaphore makes the in-flight interval
    // deterministic. Cleanup releases the gate even after a fatal assertion.
    struct Gate {
        gpu::Device& device;
        gpu::SubmissionQueue& queue;
        VkSemaphore semaphore{};
        bool signalled{};
        void release() {
            if (signalled)
                return;
            VkSemaphoreSignalInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
            signal.semaphore = semaphore;
            signal.value = 1;
            gpu::checkVulkan(vkSignalSemaphore(device.handle(), &signal), "interactive gate signal");
            signalled = true;
        }
        ~Gate() {
            if (semaphore) {
                release();
                queue.drain();
                vkDestroySemaphore(device.handle(), semaphore, nullptr);
            }
        }
    } gate{*device, queue};
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &type;
    gpu::checkVulkan(vkCreateSemaphore(device->handle(), &create, nullptr, &gate.semaphore), "interactive gate");
    ViewerScheduler scheduler;
    ASSERT_TRUE(scheduler.submit({}, {}, 1));
    const auto request = scheduler.take();
    ASSERT_TRUE(request);
    auto retained = std::make_shared<int>(42);
    std::weak_ptr<const int> lifetime = retained;
    gpu::SubmissionQueue::TimelineSemaphores waits;
    waits.wait = {gate.semaphore};
    waits.waitValues = {1};
    const auto completion = queue.submit([](VkCommandBuffer) {}, {retained}, waits);
    ASSERT_TRUE(completion);
    retained.reset();
    scheduler.cancel(2);
    EXPECT_FALSE(scheduler.complete(*request, true));
    EXPECT_FALSE(queue.poll(*completion));
    EXPECT_FALSE(lifetime.expired()) << "Cancellation is not GPU completion";
    gate.release();
    ASSERT_TRUE(queue.wait(*completion, 5'000'000'000ULL));
    EXPECT_TRUE(lifetime.expired());
    for (const auto& message : instance->take_debug_messages())
        EXPECT_LT(message.severity, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) << message.text;
}
}  // namespace
