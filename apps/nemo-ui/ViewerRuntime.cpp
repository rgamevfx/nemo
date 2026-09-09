#include "ViewerRuntime.hpp"
#include "nemo/gpu/Compile.hpp"

#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QVulkanFunctions>

#include <chrono>
#include <iostream>
#include <utility>

namespace nemo::ui {
ViewerRuntime::~ViewerRuntime() {
    quiesceForTeardown();
}

void ViewerRuntime::bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                              eval::ViewerCacheOptions cacheOptions) {
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
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(device_->physical(), VK_FORMAT_R8G8B8A8_UNORM, &format);
    filterLinear_ = (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    worker_ = std::thread([this, shaders] { run(shaders); });
}

bool ViewerRuntime::submit(Document document, EvaluationRequest request, std::uint64_t id,
                           eval::ViewerDestination destination) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.submit(std::move(document), std::move(request), id, destination);
        // A fresh interactive submission replaces the old mailbox result. A
        // cache-range submission uses its own destination and must not erase
        // what is currently shown.
        if (accepted)
            results_.erase(destination);
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::probe(Document document, std::string source, std::uint64_t id) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.probe(std::move(document), std::move(source), id);
        if (accepted)
            results_.erase(eval::ViewerDestination::Interactive);
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.requestRange(std::move(document), std::move(request), first, last, id);
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

ViewerRuntimeCounts ViewerRuntime::counts() const {
    std::lock_guard lock(mutex_);
    const eval::ViewerSchedulerCounts counts = scheduler_.counts();
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
                               cacheCounts_.lastError};
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

void ViewerRuntime::run(const std::filesystem::path& shaders) {
    std::unique_ptr<eval::ViewerSession> session;
    std::vector<std::uint32_t> presentationShader;
    for (;;) {
        Pending pending;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || scheduler_.hasWork(); });
            if (stopping_)
                break;
            auto next = scheduler_.take();
            if (!next)
                continue;
            pending = std::move(*next);
        }
        try {
            if (!session) {
                auto configured = std::make_unique<eval::ViewerSession>(*instance_, *device_, *allocator_, shaders);
                configured->configureCache(cacheOptions_);
                session = std::move(configured);
                std::lock_guard lock(mutex_);
                session_ = session.get();
            }
            if (pending.kind == eval::ViewerRequestKind::Probe) {
                publish(SourceProbeResult{session->probeSource(*pending.document, pending.source), pending.id},
                        pending);
            } else {
                auto publicationGuard = [this, pending] { return scheduler_.isCacheCurrent(pending); };
                auto frame = session->render(*pending.document, pending.request, 10'000'000'000ULL, pending.id,
                                             pending.destination, std::move(publicationGuard));
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
                        auto presentation =
                            gpu::prepareViewerPresentation(*device_, *allocator_, *presentationDevice_, *frame.image,
                                                           frame.layout.color, presentationShader);
                        auto result = std::make_shared<ViewerResult>(
                            ViewerResult{std::move(presentation), frame.layout, frame.request, pending.id,
                                         frame.revision, frame.cacheHit, pending.requestedAt});
                        publish(std::shared_ptr<const ViewerResult>(std::move(result)), pending);
                    }
                }
            }
        } catch (const std::exception& error) {
            if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                if (scheduler_.complete(pending, false))
                    emit rangeFailed(QStringLiteral("Cache frame %1: %2")
                                         .arg(pending.request.localTime)
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
    return {};
}

void ViewerRuntime::flushValidation() {
    if (!instance_)
        return;
    for (const auto& message : instance_->take_debug_messages())
        if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::cerr << "vulkan: " << message.text << '\n';
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
