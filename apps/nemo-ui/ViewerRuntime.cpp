#include "ViewerRuntime.hpp"
#include "nemo/gpu/Compile.hpp"

#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QVulkanFunctions>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

namespace nemo::ui {
namespace {
// Reserved destination ids: Interactive = 0 and Cache = 1 are the shared
// streams, so panel-instance destinations start above them.
constexpr std::uint32_t kFirstPanelDestination = 2;
}  // namespace

ViewerRuntime::~ViewerRuntime() {
    quiesceForTeardown();
}

void ViewerRuntime::bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                              eval::ViewerCacheOptions cacheOptions,
                              std::vector<eval::GpuNodeContribution> contributions) {
    if (instance_)
        throw std::runtime_error("viewer runtime already initialized");
    cacheOptions_ = std::move(cacheOptions);
    instance_ = gpu::Instance::create({.validation = true, .extensions = extensions});
    device_ = gpu::Device::create(*instance_, {.externalSharing = true});
    presentationDevice_ = gpu::Device::create(
        *instance_, {.presentation = true, .externalSharing = true, .physical = device_->physical()});
    // Initial admission cap, not the user-configurable accounting/eviction
    // policy owned by #14. Zero means no allocation, not unlimited memory.
    allocator_ = gpu::Allocator::create(*instance_, *device_, {.max_device_bytes = 2ULL << 30});
    // ONE delivery queue for the whole application, borrowing the native owners
    // and the compiled shader directory the viewer already uses (issue #94). It
    // starts its own worker and resolves its effects lazily, so bootstrap stays
    // a device-creation step. The application's native inventory (issue #37)
    // travels with it, so a delivery and the viewer beside it evaluate the same
    // installed effects.
    delivery_ = std::make_unique<eval::DeliveryQueue>(*instance_, *device_, *allocator_, shaders, 8, contributions);
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(device_->physical(), VK_FORMAT_R8G8B8A8_UNORM, &format);
    filterLinear_ = (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    // The viewer worker owns its own copy of the same inventory for as long as
    // the runtime lives: every ViewerSession it builds (the initial one and each
    // OCIO configuration swap) is constructed from it, so a package's effects
    // can never be missing from a frame or present in only one of the two
    // workers. The capture is the worker's own immutable list — no member state,
    // no lock, and no lifetime shared with the caller's vector.
    worker_ = std::thread([this, shaders, contributions = std::move(contributions)] { run(shaders, contributions); });
}

bool ViewerRuntime::submit(Document document, eval::ViewIntent intent, std::uint64_t id,
                           eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.submit(std::move(document), std::move(intent), id, destination,
                                         std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A fresh submission replaces the destination's old mailbox result. A
        // cache-range submission uses its own destination and must not erase
        // what that destination currently shows.
        if (accepted)
            results_.erase(destination);
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

std::optional<eval::ViewerDestination> ViewerRuntime::allocateDestination(const QString& panelId) {
    std::lock_guard lock(mutex_);
    // A panel keeps the destination it already holds: reallocation would leak
    // its scheduler state and let a stale presentation outlive its panel.
    if (const auto existing = panelDestinations_.find(panelId); existing != panelDestinations_.end())
        return existing->second;
    for (std::uint32_t id = kFirstPanelDestination; id < eval::kMaxViewerDestinations; ++id) {
        const auto destination = static_cast<eval::ViewerDestination>(id);
        const bool used = std::any_of(panelDestinations_.begin(), panelDestinations_.end(),
                                      [destination](const auto& entry) { return entry.second == destination; });
        if (used)
            continue;
        panelDestinations_.emplace(panelId, destination);
        return destination;
    }
    // Every bounded panel destination is live; refuse rather than alias one.
    return std::nullopt;
}

bool ViewerRuntime::retireDestination(eval::ViewerDestination destination) {
    std::unique_lock lock(mutex_);
    // Only an allocated panel destination is retirable. The reserved
    // interactive and cache streams are never panel-owned, and an unknown
    // destination must not drop another panel's state.
    const auto allocated = std::find_if(panelDestinations_.begin(), panelDestinations_.end(),
                                        [destination](const auto& entry) { return entry.second == destination; });
    if (stopping_ || allocated == panelDestinations_.end())
        return false;
    panelDestinations_.erase(allocated);
    results_.erase(destination);
    // Scheduler state goes immediately: an in-flight request for the retired
    // destination is rejected at publication instead of reaching the mailbox.
    scheduler_.retireDestination(destination);
    // The session is worker-owned, so its freshness state is erased by the
    // worker before any later work executes.
    retireQueue_.push_back(destination);
    lock.unlock();
    ready_.notify_all();
    return true;
}

bool ViewerRuntime::probe(Document document, std::string source, std::uint64_t id, eval::ViewerDestination destination,
                          std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.probe(std::move(document), std::move(source), id, destination,
                                        std::chrono::steady_clock::now(), std::move(colorConfigPath));
        if (accepted)
            results_.erase(destination);
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::describe(Document document, EvaluationRequest request, std::uint64_t id,
                             eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.describe(std::move(document), std::move(request), id, destination,
                                           std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A description is metadata for the next request, not a result to show:
        // it never displaces the destination's current frame.
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::sample(Document document, EvaluationRequest request, std::uint64_t id,
                           eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.sample(std::move(document), std::move(request), id, destination,
                                         std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A pick publishes a value, not a frame: like a description it must not
        // displace what the destination currently shows.
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                                 eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.requestRange(std::move(document), std::move(request), first, last, id, destination,
                                               std::chrono::steady_clock::now(), std::move(colorConfigPath));
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

void ViewerRuntime::cancel(std::uint64_t id) {
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            scheduler_.cancel(id);
        std::erase_if(results_, [this](const auto& entry) { return !scheduler_.isCurrent(entry.second.request); });
    }
    ready_.notify_all();
}

void ViewerRuntime::cancel(std::uint64_t id, eval::ViewerDestination destination) {
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            scheduler_.cancel(id, destination);
        // Dropping queued work is not enough: a result already in this
        // destination's mailbox must not reach its panel after the panel's
        // own cancellation watermark moved. Other destinations keep theirs.
        std::erase_if(results_, [this, destination](const auto& entry) {
            return entry.first == destination && !scheduler_.isCurrent(entry.second.request);
        });
    }
    ready_.notify_all();
}

ViewerRuntimeCounts ViewerRuntime::composeCountsLocked(const eval::ViewerSchedulerCounts& counts) const {
    // The viewer cache is shared by every destination; only the scheduler
    // counters are destination-scoped.
    if (session_) {
        if (auto snapshot = session_->tryCacheCounts())
            cacheCounts_ = std::move(*snapshot);
    }
    return ViewerRuntimeCounts{counts.queued,
                               counts.dropped,
                               counts.staleRejected,
                               counts.completed,
                               cacheCounts_.pendingFrames + cacheCounts_.activeFrames,
                               cacheCounts_.admissionDropped + cacheCounts_.admissionRejected,
                               cacheCounts_.errors,
                               cacheCounts_.published,
                               cacheCounts_.lastError,
                               cacheCounts_.diskBytes,
                               cacheCounts_.compressedHotBytes,
                               cacheCounts_.decodedHotFrames,
                               cacheCounts_.activeFrames};
}

ViewerRuntimeCounts ViewerRuntime::counts() const {
    std::lock_guard lock(mutex_);
    return composeCountsLocked(scheduler_.counts());
}

ViewerRuntimeCounts ViewerRuntime::counts(eval::ViewerDestination destination) const {
    std::lock_guard lock(mutex_);
    return composeCountsLocked(scheduler_.counts(destination));
}

std::optional<ViewerWorkResult> ViewerRuntime::takeResult(eval::ViewerDestination destination) {
    std::lock_guard lock(mutex_);
    const auto found = results_.find(destination);
    if (found == results_.end())
        return std::nullopt;
    auto result = std::move(found->second.result);
    results_.erase(found);
    return result;
}

bool ViewerRuntime::publish(ViewerWorkResult result, const Pending& pending) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        // complete() performs the destination-local identity check while the
        // runtime lock is held, so cancel() cannot race a publication into the
        // mailbox. A rejected result is dropped; GPU resource retirement is
        // still solely the shared submission mechanism's responsibility.
        if (!stopping_ && scheduler_.complete(pending, true)) {
            results_.insert_or_assign(pending.destination, Published{pending, std::move(result)});
            accepted = true;
        } else if (stopping_) {
            (void)scheduler_.complete(pending, false);
        }
    }
    if (accepted)
        emit resultReady();
    return accepted;
}

void ViewerRuntime::finishRange(const Pending& pending, bool cacheAccepted) {
    std::lock_guard lock(mutex_);
    // Admission is not persistence. Writer progress/errors are reported
    // separately by counts(), including after this evaluation completes.
    (void)scheduler_.complete(pending, cacheAccepted);
}

void ViewerRuntime::refreshColorConfig() {
    {
        const std::lock_guard lock(mutex_);
        colorRefresh_ = true;
    }
    ready_.notify_all();
}

void ViewerRuntime::run(const std::filesystem::path& shaders,
                        const std::vector<eval::GpuNodeContribution>& contributions) {
    std::unique_ptr<eval::ViewerSession> session;
    // Authored color configuration the current worker session was built with;
    // a different request config replaces the session, never the environment.
    std::string sessionColorConfig;
    std::vector<std::uint32_t> presentationShader;
    for (;;) {
        Pending pending;
        bool hasPending = false;
        bool colorRefresh = false;
        std::vector<eval::ViewerDestination> retired;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock,
                        [this] { return stopping_ || scheduler_.hasWork() || !retireQueue_.empty() || colorRefresh_; });
            if (stopping_)
                break;
            retired.swap(retireQueue_);
            colorRefresh = colorRefresh_;
            colorRefresh_ = false;
            if (auto next = scheduler_.take()) {
                pending = std::move(*next);
                hasPending = true;
            }
        }
        if (session && colorRefresh) {
            // Worker-owned: the retained programs/processors are retired here,
            // never on the caller's thread.
            session->refreshColorConfig();
        }
        // A retired destination's session freshness and Auto resolution state
        // are erased before any later work runs, so its in-flight request can
        // never be published as current. With no session yet there is nothing
        // to forget.
        for (const auto destination : retired)
            resolution_.erase(destination);
        if (session) {
            for (const auto destination : retired)
                session->retireDestination(destination);
        }
        if (!hasPending)
            continue;
        try {
            if (!session || sessionColorConfig != pending.colorConfigPath) {
                // The project's authored config replaces the worker-owned
                // ViewerSession; an empty path keeps the OCIO environment
                // fallback. No process-global state is mutated and the GUI
                // thread never waits for the swap. The replacement is built from
                // the SAME native inventory (issue #37), so swapping the color
                // configuration can never drop a package's effects.
                auto configured = std::make_unique<eval::ViewerSession>(*instance_, *device_, *allocator_, shaders,
                                                                        pending.colorConfigPath, contributions);
                configured->configureCache(cacheOptions_);
                session = std::move(configured);
                sessionColorConfig = pending.colorConfigPath;
                std::lock_guard lock(mutex_);
                session_ = session.get();
            }
            if (pending.kind == eval::ViewerRequestKind::Probe) {
                publish(SourceProbeResult{session->probeSource(*pending.document, pending.source), pending.id},
                        pending);
            } else if (pending.kind == eval::ViewerRequestKind::Describe) {
                publish(ViewerTargetDescription{session->describe(*pending.document, pending.request()), pending.id},
                        pending);
            } else if (pending.kind == eval::ViewerRequestKind::Sample) {
                // One on-demand working-space pixel: the same session, the same
                // plan mechanism and the same device as a render, so a pick can
                // never disagree with the frame beside it (issue #102).
                const auto sample =
                    session->sampleWorkingPixel(*pending.document, pending.request(), 10'000'000'000ULL);
                publish(ViewerWorkingSample{sample, pending.id}, pending);
            } else {
                auto publicationGuard = [this, pending] { return scheduler_.isCacheCurrent(pending); };
                eval::ViewerFrame frame;
                if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                    // Cache frames are a concrete, coverage-stating request that
                    // never replaces the interactive view, so they are rendered
                    // as such (issue #98 keeps the distinct headless contract).
                    frame = session->render(*pending.document, pending.request(), 10'000'000'000ULL, pending.id,
                                            pending.destination, std::move(publicationGuard));
                } else {
                    // ONE worker job resolves the view against the current
                    // frame's described image, states the demand from it and
                    // executes the plan it resolved (issue #98). The Auto
                    // hysteresis state is this destination's own, retained
                    // across frames.
                    frame = session->render(*pending.document, pending.intent(), resolution_[pending.destination],
                                            10'000'000'000ULL, pending.id, pending.destination,
                                            std::move(publicationGuard));
                }
                if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                    finishRange(pending, frame.cacheHit || frame.cacheQueued);
                } else {
                    bool current = false;
                    {
                        std::lock_guard lock(mutex_);
                        current = !stopping_ && scheduler_.isCurrent(pending);
                        if (!current)
                            (void)scheduler_.complete(pending, false);
                    }
                    if (current) {
                        if (presentationShader.empty())
                            presentationShader = gpu::loadSpirv(shaders / "viewerPresentation.spv");
                        // The isolation the frame's own view asked for: applied
                        // in the presentation copy only, never in evaluation or
                        // the cache, and never taken from another destination.
                        auto presentation = gpu::prepareViewerPresentation(
                            *device_, *allocator_, *presentationDevice_, *frame.image, frame.layout.color,
                            presentationShader, frame.presentationChannel);
                        auto result = std::make_shared<ViewerResult>(ViewerResult{
                            std::move(presentation), frame.layout, frame.description, frame.request, pending.id,
                            frame.revision, frame.cacheHit, pending.requestedAt, pending.destination});
                        publish(std::shared_ptr<const ViewerResult>(std::move(result)), pending);
                    }
                }
            }
        } catch (const eval::ViewUnavailable& error) {
            // The view addresses nothing the current frame carries: it has no
            // frame to publish, so the panel must not keep presenting an image
            // this selection does not produce — and the description that
            // refused it travels with the answer, so the selectors offer the
            // channels this frame really carries.
            if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                if (scheduler_.complete(pending, false))
                    emit rangeFailed(QStringLiteral("Cache frame %1: %2")
                                         .arg(std::get<EvaluationRequest>(pending.demand).localTime)
                                         .arg(QString::fromUtf8(error.what())),
                                     pending.id);
            } else {
                const auto& intent = pending.intent();
                publish(ViewerUnavailableView{error.what(), pending.id, intent.target, intent.localTime,
                                              pending.document->stateRevision(), error.description},
                        pending);
            }
        } catch (const std::exception& error) {
            if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                if (scheduler_.complete(pending, false))
                    emit rangeFailed(QStringLiteral("Cache frame %1: %2")
                                         .arg(std::get<EvaluationRequest>(pending.demand).localTime)
                                         .arg(QString::fromUtf8(error.what())),
                                     pending.id);
            } else {
                publish(ViewerFailure{error.what(), pending.id}, pending);
            }
        }
        flushValidation();
    }
    std::lock_guard lock(mutex_);
    session_ = nullptr;
}

QString ViewerRuntime::attachToWindow(QQuickWindow* window) {
    if (!presentationDevice_ || !window)
        return QStringLiteral("viewer requires an initialized device and a Qt window");
    // One presentation host serves every panel, so adopting the same window
    // again is a no-op and a second window is refused.
    if (attachedWindow_ == window)
        return {};
    if (attachedWindow_)
        return QStringLiteral("viewer presentation is already attached to another Qt window");
    qtInstance_.setVkInstance(instance_->handle());
    qtInstance_.setApiVersion(QVersionNumber(1, 3));
    if (!qtInstance_.create())
        return QStringLiteral("Qt could not adopt the application Vulkan instance");
    window->setVulkanInstance(&qtInstance_);
    window->setGraphicsDevice(
        QQuickGraphicsDevice::fromDeviceObjects(presentationDevice_->physical(), presentationDevice_->handle(),
                                                static_cast<int>(presentationDevice_->graphics_family()), 0));
    window->create();
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(window);
    if (surface == VK_NULL_HANDLE)
        return QStringLiteral("could not create a Vulkan surface on %1").arg(QGuiApplication::platformName());
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        vkGetInstanceProcAddr(instance_->handle(), "vkGetPhysicalDeviceSurfaceSupportKHR"));
    VkBool32 supported = VK_FALSE;
    if (!query ||
        query(presentationDevice_->physical(), presentationDevice_->graphics_family(), surface, &supported) !=
            VK_SUCCESS ||
        !supported)
        return QStringLiteral("graphics queue family %1 cannot present to this window")
            .arg(presentationDevice_->graphics_family());
    // Qt's render thread owns both connections for the runtime's lifetime:
    // beforeRendering pins every panel's presentation for the current frame
    // slot, and frameSwapped reports each destination newly on screen. Waking
    // the worker belongs to the mailbox signal, never to Qt's frame boundary.
    connect(
        window, &QQuickWindow::beforeRendering, this,
        [this, window] { windowState_.beginFrame(window, *presentationDevice_); }, Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            for (const auto& presented : windowState_.takeNewPresentedFrames()) {
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                 presented.result->requestedAt)
                                           .count();
                emit framePresented(presented.destination, static_cast<int>(presented.result->request.localTime),
                                    presented.result->frame.width, presented.result->frame.height,
                                    presented.result->cacheHit, elapsed);
            }
        },
        Qt::DirectConnection);
    window->show();
    attachedWindow_ = window;
    return {};
}

void ViewerRuntime::flushValidation() {
    if (!instance_)
        return;
    for (const auto& message : instance_->take_debug_messages())
        if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::cerr << "vulkan: " << message.text << '\n';
}

eval::DeliveryQueue& ViewerRuntime::deliveryQueue() {
    if (!delivery_)
        throw std::runtime_error("viewer runtime delivery queue is unavailable before bootstrap");
    return *delivery_;
}

void ViewerRuntime::stopWorker() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        scheduler_.clear();
        results_.clear();
    }
    ready_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void ViewerRuntime::quiesceForTeardown() {
    // Delivery first: its worker joins, its in-flight submissions are waited on
    // and its retained staging is released while the device still exists. Only
    // then are the viewer worker and the devices torn down.
    delivery_.reset();
    stopWorker();
    // Shutdown only: both execution and Qt's render loop must be stopped.
    // Qt's ordinary device-wide waits never touch the execution device.
    for (const auto* device : {device_.get(), presentationDevice_.get()}) {
        if (!device)
            continue;
        VkResult result;
        while ((result = vkDeviceWaitIdle(device->handle())) != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            // As with SubmissionQueue::drain, a transient wait failure is
            // never permission to release resources still used by the GPU.
            std::cerr << "viewer teardown: vkDeviceWaitIdle returned " << result << "; retaining owners and retrying\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (result == VK_ERROR_DEVICE_LOST)
            std::cerr << "viewer teardown: Vulkan device lost\n";
    }
    {
        std::lock_guard lock(mutex_);
        results_.clear();
    }
    flushValidation();
}
}  // namespace nemo::ui
