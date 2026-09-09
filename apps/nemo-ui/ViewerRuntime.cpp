#include "ViewerRuntime.hpp"
#include "nemo/gpu/Compile.hpp"

#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QVulkanFunctions>

#include <chrono>
#include <iostream>

namespace nemo::ui {
ViewerRuntime::~ViewerRuntime() {
    quiesceForTeardown();
}

void ViewerRuntime::bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders) {
    if (instance_)
        throw std::runtime_error("viewer runtime already initialized");
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

void ViewerRuntime::enqueue(Pending pending) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        latestId_ = pending.id;
        pending_ = std::move(pending);
        result_.reset();
    }
    ready_.notify_one();
}
void ViewerRuntime::submit(Document document, EvaluationRequest request, std::uint64_t id) {
    enqueue({std::move(document), std::move(request), {}, id});
}
void ViewerRuntime::probe(Document document, std::string source, std::uint64_t id) {
    enqueue({std::move(document), {}, std::move(source), id});
}
std::optional<ViewerWorkResult> ViewerRuntime::takeResult() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}
void ViewerRuntime::publish(ViewerWorkResult result, std::uint64_t id) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || (id != 0 && id != latestId_))
            return;
        result_ = std::move(result);
    }
    emit resultReady();
}

void ViewerRuntime::run(const std::filesystem::path& shaders) {
    std::unique_ptr<eval::ViewerSession> session;
    std::vector<std::uint32_t> presentationShader;
    for (;;) {
        Pending pending;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_)
                break;
            pending = std::move(*pending_);
            pending_.reset();
        }
        try {
            if (!session)
                session = std::make_unique<eval::ViewerSession>(*instance_, *device_, *allocator_, shaders);
            if (!pending.source.empty()) {
                publish(SourceProbeResult{session->probeSource(pending.document, pending.source), pending.id},
                        pending.id);
            } else {
                if (presentationShader.empty())
                    presentationShader = gpu::loadSpirv(shaders / "viewerPresentation.spv");
                auto frame = session->render(pending.document, pending.request);
                auto presentation = gpu::prepareViewerPresentation(*device_, *allocator_, *presentationDevice_,
                                                                   frame.image, frame.layout.color, presentationShader);
                auto result = std::make_shared<ViewerResult>(
                    ViewerResult{std::move(presentation), frame.layout, frame.request, pending.id, frame.revision});
                publish(std::shared_ptr<const ViewerResult>(std::move(result)), pending.id);
            }
        } catch (const std::exception& error) {
            publish(ViewerFailure{error.what(), pending.id}, pending.id);
        }
        flushValidation();
    }
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
        pending_.reset();
    }
    ready_.notify_one();
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
        result_.reset();
    }
    flushValidation();
}
}  // namespace nemo::ui
