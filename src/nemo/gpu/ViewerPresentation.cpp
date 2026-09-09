#include "nemo/gpu/ViewerPresentation.hpp"
#include "nemo/gpu/ExternalHandle.hpp"

namespace nemo::gpu {
struct PresentationReady {
    VkDevice producer{};
    VkDevice consumer{};
    VkSemaphore signal{};
    VkSemaphore wait{};
    bool acquired{false};  // Consumer render-thread confined after publication.
    ~PresentationReady() {
        if (wait)
            vkDestroySemaphore(consumer, wait, nullptr);
        if (signal)
            vkDestroySemaphore(producer, signal, nullptr);
    }
};

namespace {
std::shared_ptr<PresentationReady> shareReadiness(Device& producer, Device& consumer) {
    VkPhysicalDeviceExternalSemaphoreInfo query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    query.handleType = detail::semaphoreHandleType;
    VkExternalSemaphoreProperties properties{};
    properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    vkGetPhysicalDeviceExternalSemaphoreProperties(producer.physical(), &query, &properties);
    constexpr auto required =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if ((properties.externalSemaphoreFeatures & required) != required ||
        !(properties.compatibleHandleTypes & detail::semaphoreHandleType))
        throw GpuException(GpuError::InvalidRequest, "GPU cannot export/import opaque binary presentation semaphores");
    auto ready = std::make_shared<PresentationReady>();
    ready->producer = producer.handle();
    ready->consumer = consumer.handle();
    VkExportSemaphoreCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    external.handleTypes = detail::semaphoreHandleType;
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &external;
    checkVulkan(vkCreateSemaphore(producer.handle(), &create, nullptr, &ready->signal), "vkCreateSemaphore(export)");
    create.pNext = nullptr;
    checkVulkan(vkCreateSemaphore(consumer.handle(), &create, nullptr, &ready->wait), "vkCreateSemaphore(import)");
#if defined(_WIN32)
    const auto getHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(producer.handle(), "vkGetSemaphoreWin32HandleKHR"));
    const auto importHandle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(consumer.handle(), "vkImportSemaphoreWin32HandleKHR"));
    if (!getHandle || !importHandle)
        throw GpuException(GpuError::InvalidRequest, "Win32 presentation semaphore entry points unavailable");
    VkSemaphoreGetWin32HandleInfoKHR get{};
    get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    get.semaphore = ready->signal;
    get.handleType = detail::semaphoreHandleType;
    HANDLE value = nullptr;
    checkVulkan(getHandle(producer.handle(), &get, &value), "vkGetSemaphoreWin32HandleKHR");
    detail::NativeHandle handle(value);
    VkImportSemaphoreWin32HandleInfoKHR import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    import.handle = handle.get();
#else
    const auto getHandle =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(producer.handle(), "vkGetSemaphoreFdKHR"));
    const auto importHandle =
        reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(consumer.handle(), "vkImportSemaphoreFdKHR"));
    if (!getHandle || !importHandle)
        throw GpuException(GpuError::InvalidRequest, "FD presentation semaphore entry points unavailable");
    VkSemaphoreGetFdInfoKHR get{};
    get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    get.semaphore = ready->signal;
    get.handleType = detail::semaphoreHandleType;
    int value = -1;
    checkVulkan(getHandle(producer.handle(), &get, &value), "vkGetSemaphoreFdKHR");
    detail::NativeHandle handle(value);
    VkImportSemaphoreFdInfoKHR import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import.fd = handle.get();
#endif
    import.semaphore = ready->wait;
    import.handleType = detail::semaphoreHandleType;
    checkVulkan(importHandle(consumer.handle(), &import), "import presentation semaphore");
    handle.imported();
    return ready;
}
}  // namespace

ViewerPresentation prepareViewerPresentation(Device& producer, Allocator& allocator, Device& consumer,
                                             const Image& source, ColorInterpretation color,
                                             const std::vector<std::uint32_t>& spirv, std::uint64_t timeout_ns) {
    if (color != ColorInterpretation::DisplayReferred)
        throw GpuException(GpuError::InvalidRequest, "viewer presentation requires the completed viewing transform");
    if (source.format() != VK_FORMAT_R32G32B32A32_SFLOAT || source.dimensions() != 2)
        throw GpuException(GpuError::InvalidRequest, "viewer presentation requires a 2D RGBA32F image");
    if (producer.handle() == consumer.handle() || producer.physical() != consumer.physical() ||
        !producer.external_sharing_enabled() || !consumer.external_sharing_enabled())
        throw GpuException(GpuError::InvalidRequest,
                           "viewer presentation requires separate sharing-enabled devices on the same GPU");
    if (!producer.features().shaderStorageImageReadWithoutFormat ||
        !producer.features().shaderStorageImageWriteWithoutFormat)
        throw GpuException(GpuError::InvalidRequest,
                           "viewer presentation requires storage image read/write without format");
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(producer.physical(), VK_FORMAT_R8G8B8A8_UNORM, &properties);
    constexpr auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & required) != required)
        throw GpuException(GpuError::InvalidRequest,
                           "viewer presentation requires sampled/storage RGBA8 UNORM support");
    const auto extent = source.extent();
    auto [output, imported] = allocator.create_shared_image(
        consumer, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto ready = shareReadiness(producer, consumer);
    auto pass = ComputePass::create(producer, spirv,
                                    {{0, 0, DescriptorKind::StorageImage, nullptr, &source},
                                     {0, 1, DescriptorKind::StorageImage, nullptr, &output}});
    auto& queue = producer.submissions(producer.graphics_family());
    SubmissionQueue::TimelineSemaphores handoff;
    handoff.signal = {ready->signal};
    handoff.signalValues = {0};
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            recordImageBarrier(command, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            recordImageBarrier(command, output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT);
            pass->record(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            VkImageMemoryBarrier release{};
            release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            release.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            release.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            release.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            release.srcQueueFamilyIndex = producer.graphics_family();
            release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            release.image = output.handle();
            release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &release);
        },
        {pass->retain(), ready}, handoff);
    if (!completion)
        throw GpuException(GpuError::SubmissionTimeout, "viewer presentation queue capacity unavailable");
    if (!queue.wait(*completion, timeout_ns))
        throw GpuException(GpuError::SubmissionTimeout, "viewer presentation timed out; GPU resources remain retained");
    return {std::move(imported), std::move(ready)};
}

void acquireViewerPresentation(Device& consumer, const ViewerPresentation& presentation, VkCommandBuffer command) {
    if (!presentation.ready || presentation.ready->consumer != consumer.handle() || command == VK_NULL_HANDLE)
        throw GpuException(GpuError::InvalidRequest, "viewer acquire requires its consumer device and command buffer");
    if (presentation.ready->acquired)
        return;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &presentation.ready->wait;
    submit.pWaitDstStageMask = &stage;
    checkVulkan(vkQueueSubmit(consumer.queue(consumer.graphics_family()), 1, &submit, VK_NULL_HANDLE),
                "vkQueueSubmit(external viewer acquire)");
    VkImageMemoryBarrier acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex = consumer.graphics_family();
    acquire.image = presentation.image.handle();
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &acquire);
    presentation.ready->acquired = true;
}
}  // namespace nemo::gpu
