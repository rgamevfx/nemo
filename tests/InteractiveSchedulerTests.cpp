#include "nemo/eval/ViewerScheduler.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace nemo;
using namespace nemo::eval;
Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}
EvaluationRequest scopedRequest() {
    EvaluationRequest request;
    request.network = NetworkId{1};
    return request;
}

TEST(Interactive, FullQueueReportsBackpressureWithoutDroppingAnotherViewer) {
    ViewerScheduler scheduler(1);
    ASSERT_TRUE(scheduler.submit({}, scopedRequest(), 1));
    EXPECT_FALSE(scheduler.submit({}, scopedRequest(), 1, static_cast<ViewerDestination>(2)));
    EXPECT_EQ(scheduler.counts().queued, 1u);
    EXPECT_EQ(scheduler.counts().dropped, 1u);
    const auto accepted = scheduler.take();
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->destination, ViewerDestination::Interactive);
    EXPECT_TRUE(scheduler.complete(*accepted, true));
}

TEST(Interactive, CacheHistorySurvivesScrubButCancellationCannotBeResurrected) {
    ViewerScheduler scheduler;
    ASSERT_TRUE(scheduler.submit({}, scopedRequest(), 1));
    const auto first = scheduler.take();
    ASSERT_TRUE(first);
    EvaluationRequest next;
    next.network = NetworkId{1};
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
    ASSERT_TRUE(scheduler.submit({}, scopedRequest(), 3));
    EXPECT_FALSE(scheduler.isCacheCurrent(*first));
    const auto resumed = scheduler.take();
    ASSERT_TRUE(resumed);
    EXPECT_TRUE(scheduler.isCacheCurrent(*resumed));
}

TEST(Interactive, CurrentFramePreemptsLazyRangeAndOnlyExplicitFramesAreTaken) {
    ViewerScheduler scheduler;
    Document document;
    EvaluationRequest request;
    request.network = document.rootNetworkId();
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
    const NodeId color = rootGraph(document).addNode("constcolor", "color");
    commands.push(setParamCommand(document.rootNetworkId(), color, "color", ColorValue{{0.1F, 0.2F, 0.3F, 1.0F}}));
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    const auto otherViewer = static_cast<ViewerDestination>(2);
    ASSERT_TRUE(scheduler.submit(document, request, 1));
    const auto old = scheduler.take();
    ASSERT_TRUE(old);
    ASSERT_TRUE(scheduler.submit(document, request, 1, otherViewer));
    const auto other = scheduler.take();
    ASSERT_TRUE(other);
    commands.push(setParamCommand(document.rootNetworkId(), color, "color", ColorValue{{0.7F, 0.8F, 0.9F, 1.0F}}));
    ASSERT_TRUE(scheduler.submit(document, request, 2));
    EXPECT_EQ(std::get<ColorValue>(rootGraph(*old->document).nodeByName("color")->params.at("color")).value,
              (std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F}));
    EXPECT_FALSE(scheduler.complete(*old, true));
    EXPECT_TRUE(scheduler.complete(*other, true));
    const auto current = scheduler.take();
    ASSERT_TRUE(current);
    EXPECT_EQ(std::get<ColorValue>(rootGraph(*current->document).nodeByName("color")->params.at("color")).value,
              (std::array<float, 4>{0.7F, 0.8F, 0.9F, 1.0F}));
    EXPECT_TRUE(scheduler.complete(*current, true));
    EXPECT_EQ(scheduler.counts().staleRejected, 1u);
}

TEST(Interactive, CancellationRejectsInflightAndSupersededBacklogIsCounted) {
    ViewerScheduler scheduler;
    Document document;
    EvaluationRequest request;
    request.network = document.rootNetworkId();
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
    ASSERT_TRUE(scheduler.requestRange({}, scopedRequest(), last, last, 1));
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
    ASSERT_TRUE(scheduler.submit({}, scopedRequest(), 1));
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

TEST(Interactive, DestinationScopedCancelDropsOnlyThatDestinationsWork) {
    ViewerScheduler scheduler(2);
    Document document;
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    const auto other = static_cast<ViewerDestination>(2);
    ASSERT_TRUE(scheduler.submit(document, request, 1));
    const auto inflight = scheduler.take();
    ASSERT_TRUE(inflight);
    ASSERT_TRUE(scheduler.submit(document, request, 2));
    ASSERT_TRUE(scheduler.submit(document, request, 1, other));
    scheduler.cancel(3, ViewerDestination::Interactive);
    EXPECT_EQ(scheduler.counts().dropped, 1u);
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).dropped, 1u);
    EXPECT_EQ(scheduler.counts(other).dropped, 0u);
    EXPECT_FALSE(scheduler.complete(*inflight, true));
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).staleRejected, 1u);
    EXPECT_EQ(scheduler.counts(other).staleRejected, 0u);
    const auto otherWork = scheduler.take();
    ASSERT_TRUE(otherWork);
    EXPECT_EQ(otherWork->destination, other);
    // A destination-scoped cancel does not move the global watermark, so an
    // older id queued for another destination stays current.
    EXPECT_TRUE(scheduler.isCurrent(*otherWork));
    EXPECT_TRUE(scheduler.complete(*otherWork, true));
    EXPECT_FALSE(scheduler.take());
    EXPECT_EQ(scheduler.counts(other).queued, 0u);
    EXPECT_EQ(scheduler.counts(other).completed, 1u);
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).queued, 0u);
    EXPECT_EQ(scheduler.counts().completed, 1u);
    // The destination watermark refuses re-admission below the cancelled id and
    // admits an id at the watermark, exactly like the global watermark.
    EXPECT_FALSE(scheduler.submit(document, request, 2, ViewerDestination::Interactive));
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).dropped, 2u);
    EXPECT_EQ(scheduler.counts(other).dropped, 0u);
    ASSERT_TRUE(scheduler.submit(document, request, 3, ViewerDestination::Interactive));
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).queued, 1u);
}

TEST(Interactive, RetireDestinationDropsQueuedWorkAndRejectsInflight) {
    ViewerScheduler scheduler(3);
    Document document;
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    const auto retired = static_cast<ViewerDestination>(2);
    const auto kept = static_cast<ViewerDestination>(3);
    ASSERT_TRUE(scheduler.submit(document, request, 1, retired));
    const auto inflight = scheduler.take();
    ASSERT_TRUE(inflight);
    ASSERT_TRUE(scheduler.submit(document, request, 2, retired));
    ASSERT_TRUE(scheduler.requestRange(document, request, 10, 12, 3, retired));
    ASSERT_TRUE(scheduler.submit(document, request, 5, kept));
    EXPECT_EQ(scheduler.counts(retired).dropped, 1u);
    EXPECT_EQ(scheduler.counts(retired).queued, 3u);
    EXPECT_EQ(scheduler.counts(kept).queued, 1u);
    scheduler.retireDestination(retired);
    EXPECT_EQ(scheduler.counts(retired).queued, 0u);
    EXPECT_EQ(scheduler.counts(retired).dropped, 0u);
    EXPECT_FALSE(scheduler.isCurrent(*inflight));
    EXPECT_FALSE(scheduler.complete(*inflight, true));
    EXPECT_EQ(scheduler.counts(kept).queued, 1u);
    EXPECT_EQ(scheduler.counts(kept).staleRejected, 0u);
    const auto keptWork = scheduler.take();
    ASSERT_TRUE(keptWork);
    EXPECT_EQ(keptWork->destination, kept);
    EXPECT_TRUE(scheduler.complete(*keptWork, true));
    EXPECT_EQ(scheduler.counts(kept).completed, 1u);
    EXPECT_FALSE(scheduler.take());
    EXPECT_EQ(scheduler.counts().dropped, 4u);
    EXPECT_EQ(scheduler.counts().staleRejected, 1u);
    EXPECT_EQ(scheduler.counts().completed, 1u);
}

TEST(Interactive, CountsAreScopedPerDestinationAndGlobalStillAggregates) {
    ViewerScheduler scheduler(4);
    Document document;
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    const auto second = static_cast<ViewerDestination>(2);
    const auto third = static_cast<ViewerDestination>(3);
    ASSERT_TRUE(scheduler.submit(document, request, 1));
    ASSERT_TRUE(scheduler.requestRange(document, request, 0, 3, 1, second));
    ASSERT_TRUE(scheduler.submit(document, request, 1, third));
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).queued, 1u);
    EXPECT_EQ(scheduler.counts(second).queued, 4u);
    EXPECT_EQ(scheduler.counts(third).queued, 1u);
    EXPECT_EQ(scheduler.counts().queued, 6u);
    EXPECT_EQ(scheduler.counts().dropped, scheduler.counts(ViewerDestination::Interactive).dropped +
                                              scheduler.counts(second).dropped + scheduler.counts(third).dropped);
    ASSERT_TRUE(scheduler.submit(document, request, 2, second));
    EXPECT_EQ(scheduler.counts(second).dropped, 4u);
    EXPECT_EQ(scheduler.counts(second).queued, 1u);
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).dropped, 0u);
    EXPECT_EQ(scheduler.counts(third).dropped, 0u);
    const auto interactive = scheduler.take();
    ASSERT_TRUE(interactive);
    EXPECT_EQ(interactive->destination, ViewerDestination::Interactive);
    EXPECT_TRUE(scheduler.complete(*interactive, true));
    EXPECT_EQ(scheduler.counts(ViewerDestination::Interactive).completed, 1u);
    EXPECT_EQ(scheduler.counts(second).completed, 0u);
    EXPECT_EQ(scheduler.counts(third).completed, 0u);
    EXPECT_EQ(scheduler.counts().completed, 1u);
    EXPECT_EQ(scheduler.counts().queued, scheduler.counts(ViewerDestination::Interactive).queued +
                                             scheduler.counts(second).queued + scheduler.counts(third).queued);
    EXPECT_EQ(scheduler.counts(static_cast<ViewerDestination>(9)).queued, 0u);
    EXPECT_EQ(scheduler.counts(static_cast<ViewerDestination>(9)).dropped, 0u);
}
}  // namespace
