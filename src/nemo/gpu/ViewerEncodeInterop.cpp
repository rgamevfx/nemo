#include "nemo/gpu/ViewerEncodeInterop.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

namespace nemo::gpu {
namespace {

[[nodiscard]] std::string avError(int status) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(status, text, sizeof(text));
    return text;
}

[[noreturn]] void fail(const std::string& what) {
    throw ViewerEncodeError(what);
}

// Packs the compact viewer staging payload into a temporary FFmpeg host
// frame. Media owns codec policy; this adapter owns the host/device transfer
// boundary for hardware fallback.
[[nodiscard]] AVFrame* makeYuv420pFrame(int width, int height, const std::vector<std::uint8_t>& planes) {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr)
        return nullptr;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    const std::uint8_t* y = planes.data();
    const std::uint8_t* cb = y + static_cast<std::size_t>(width) * height;
    const std::uint8_t* cr = cb + static_cast<std::size_t>(width / 2) * (height / 2);
    for (int row = 0; row < height; ++row)
        std::memcpy(frame->data[0] + static_cast<std::ptrdiff_t>(row) * frame->linesize[0],
                    y + static_cast<std::size_t>(row) * width, static_cast<std::size_t>(width));
    for (int row = 0; row < height / 2; ++row) {
        std::memcpy(frame->data[1] + static_cast<std::ptrdiff_t>(row) * frame->linesize[1],
                    cb + static_cast<std::size_t>(row) * (width / 2), static_cast<std::size_t>(width / 2));
        std::memcpy(frame->data[2] + static_cast<std::ptrdiff_t>(row) * frame->linesize[2],
                    cr + static_cast<std::size_t>(row) * (width / 2), static_cast<std::size_t>(width / 2));
    }
    return frame;
}

struct GpuYuvPlanes {
    Image y;
    Image u;
    Image v;
};

void deleteFrame(AVFrame* value) {
    av_frame_free(&value);
}
void deleteBufferRef(AVBufferRef* value) {
    av_buffer_unref(&value);
}

using FramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, void (*)(AVBufferRef*)>;

FramePtr makeFrame() {
    return FramePtr(av_frame_alloc(), deleteFrame);
}
BufferRefPtr makeBufferRef(AVBufferRef* ref) {
    return BufferRefPtr(ref, deleteBufferRef);
}

class GpuYuvConverter {
public:
    GpuYuvConverter(Device& device, Allocator& allocator) : device_(device), allocator_(allocator) {
        if (!device.features().shaderStorageImageExtendedFormats)
            fail("GPU lacks R8 storage-image extended format support");
        try {
            spirv_ = compileGlslToSpirv(R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba32f) readonly uniform image2D source;
layout(set = 0, binding = 1, r8) writeonly uniform image2D y_plane;
layout(set = 0, binding = 2, r8) writeonly uniform image2D u_plane;
layout(set = 0, binding = 3, r8) writeonly uniform image2D v_plane;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(source);
    ivec2 encodedSize = imageSize(y_plane);
    if (p.x >= encodedSize.x || p.y >= encodedSize.y) return;
    vec3 value = clamp(imageLoad(source, min(p, size - 1)).rgb, 0.0, 1.0);
    float luma = dot(value, vec3(0.2126, 0.7152, 0.0722));
    imageStore(y_plane, p, vec4(clamp((16.0 + 219.0 * luma) / 255.0, 0.0, 1.0), 0.0, 0.0, 1.0));
    if ((p.x & 1) != 0 || (p.y & 1) != 0) return;
    vec3 mean = vec3(0.0);
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            mean += clamp(imageLoad(source, ivec2(clamp(p.x + dx, 0, size.x - 1), min(p.y + dy, size.y - 1))).rgb, 0.0, 1.0)
                     * (dx == 0 ? 2.0 : 1.0);
    mean /= 8.0;
    float chromaLuma = dot(mean, vec3(0.2126, 0.7152, 0.0722));
    float cb = 128.0 + (112.0 / (1.0 - 0.0722)) * (mean.b - chromaLuma);
    float cr = 128.0 + (112.0 / (1.0 - 0.2126)) * (mean.r - chromaLuma);
    imageStore(u_plane, p / 2, vec4(clamp(cb / 255.0, 0.0, 1.0), 0.0, 0.0, 1.0));
    imageStore(v_plane, p / 2, vec4(clamp(cr / 255.0, 0.0, 1.0), 0.0, 0.0, 1.0));
}
)glsl");
        } catch (const std::exception& error) {
            fail("GPU Rec.709 conversion pipeline creation failed: " + std::string(error.what()));
        }
    }

    [[nodiscard]] GpuYuvPlanes convertDevice(const Image& image, int sourceWidth, int sourceHeight, int encodedWidth,
                                             int encodedHeight, ViewerEncodeStats& stats) const {
        const auto extent = image.extent();
        if (extent.width != static_cast<std::uint32_t>(sourceWidth) ||
            extent.height != static_cast<std::uint32_t>(sourceHeight))
            fail("device image extent does not match its declared ImageLayout");
        GpuYuvPlanes output;
        try {
            const auto allocationStart = std::chrono::steady_clock::now();
            const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            output.y =
                allocator_.create_image(static_cast<std::uint32_t>(encodedWidth),
                                        static_cast<std::uint32_t>(encodedHeight), 1, VK_FORMAT_R8_UNORM, usage, 2);
            output.u =
                allocator_.create_image(static_cast<std::uint32_t>(encodedWidth / 2),
                                        static_cast<std::uint32_t>(encodedHeight / 2), 1, VK_FORMAT_R8_UNORM, usage, 2);
            output.v =
                allocator_.create_image(static_cast<std::uint32_t>(encodedWidth / 2),
                                        static_cast<std::uint32_t>(encodedHeight / 2), 1, VK_FORMAT_R8_UNORM, usage, 2);
            std::vector<ComputeBinding> bindings = {
                {0, 0, DescriptorKind::StorageImage, nullptr, &image},
                {0, 1, DescriptorKind::StorageImage, nullptr, &output.y},
                {0, 2, DescriptorKind::StorageImage, nullptr, &output.u},
                {0, 3, DescriptorKind::StorageImage, nullptr, &output.v},
            };
            auto pass = ComputePass::create(device_, spirv_, bindings);
            stats.allocationPackingMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - allocationStart).count();
            auto& queue = device_.submissions(device_.graphics_family());
            const auto conversionStart = std::chrono::steady_clock::now();
            const auto completion = queue.submit(
                [&](VkCommandBuffer command) {
                    recordImageBarrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                    for (const Image* outputImage : {&output.y, &output.u, &output.v}) {
                        recordImageBarrier(command, *outputImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_SHADER_WRITE_BIT);
                    }
                    pass->record(command, (static_cast<std::uint32_t>(encodedWidth) + 7) / 8,
                                 (static_cast<std::uint32_t>(encodedHeight) + 7) / 8, 1);
                },
                {pass->retain(), image.retain(), output.y.retain(), output.u.retain(), output.v.retain()}, {},
                10'000'000'000ULL);
            if (!completion)
                fail("GPU Rec.709 conversion submission capacity exhausted");
            if (!queue.wait(*completion, 10'000'000'000ULL))
                fail("GPU Rec.709 conversion timed out");
            stats.gpuConversionMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - conversionStart).count();
            return output;
        } catch (const ViewerEncodeError&) {
            throw;
        } catch (const std::exception& error) {
            fail("GPU Rec.709 conversion failed: " + std::string(error.what()));
        }
    }

    void convertToHost(const Image& image, int sourceWidth, int sourceHeight, int encodedWidth, int encodedHeight,
                       std::vector<std::uint8_t>& planes, ViewerEncodeStats& stats) const {
        GpuYuvPlanes output = convertDevice(image, sourceWidth, sourceHeight, encodedWidth, encodedHeight, stats);
        const std::uint32_t width = output.y.extent().width;
        const std::uint32_t height = output.y.extent().height;
        const std::size_t yBytes = static_cast<std::size_t>(width) * height;
        const std::size_t uvBytes = static_cast<std::size_t>(width / 2) * (height / 2);
        planes.resize(yBytes + uvBytes * 2);
        auto& queue = device_.submissions(device_.graphics_family());
        const auto readbackStart = std::chrono::steady_clock::now();
        downloadImage(queue, allocator_, output.y, planes.data(), yBytes, 10'000'000'000ULL);
        downloadImage(queue, allocator_, output.u, planes.data() + yBytes, uvBytes, 10'000'000'000ULL);
        downloadImage(queue, allocator_, output.v, planes.data() + yBytes + uvBytes, uvBytes, 10'000'000'000ULL);
        stats.deviceToHostMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readbackStart).count();
        stats.deviceToHostBytes += yBytes + uvBytes * 2;
        // Packed device planes + host pixels + the largest per-plane
        // readback buffer; excludes codec internals and input viewer images.
        stats.stagingBytes = std::max<std::uint64_t>(stats.stagingBytes, 2 * (yBytes + uvBytes * 2) + yBytes);
    }

    void copyToVulkan(const GpuYuvPlanes& source, const AVFrame& destinationOwner, int width, int height,
                      ViewerEncodeStats& stats) const {
        auto* destination = reinterpret_cast<AVVkFrame*>(destinationOwner.data[0]);
        if (destination == nullptr)
            fail("Vulkan destination frame has no AVVkFrame payload");
        auto& queue = device_.submissions(device_.graphics_family());
        const Image* sourceImages[] = {&source.y, &source.u, &source.v};
        const std::uint32_t planeWidths[] = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(width / 2),
                                             static_cast<std::uint32_t>(width / 2)};
        const std::uint32_t planeHeights[] = {static_cast<std::uint32_t>(height),
                                              static_cast<std::uint32_t>(height / 2),
                                              static_cast<std::uint32_t>(height / 2)};
        SubmissionQueue::TimelineSemaphores semaphores;
        for (int plane = 0; plane < 3; ++plane) {
            if (destination->sem[plane] != VK_NULL_HANDLE) {
                semaphores.wait.push_back(destination->sem[plane]);
                semaphores.waitValues.push_back(destination->sem_value[plane]);
                semaphores.signal.push_back(destination->sem[plane]);
                semaphores.signalValues.push_back(destination->sem_value[plane] + 1);
            }
        }
        auto retainedOwner = makeFrame();
        if (retainedOwner == nullptr || av_frame_ref(retainedOwner.get(), &destinationOwner) < 0)
            fail("Vulkan destination frame retention failed");
        const auto start = std::chrono::steady_clock::now();
        const auto completion = queue.submit(
            [&](VkCommandBuffer command) {
                for (int plane = 0; plane < 3; ++plane) {
                    recordImageBarrier(command, *sourceImages[plane], VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_ACCESS_TRANSFER_READ_BIT);
                    VkImageMemoryBarrier destinationBarrier{};
                    destinationBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    destinationBarrier.srcAccessMask = destination->access[plane];
                    destinationBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    destinationBarrier.oldLayout = destination->layout[plane];
                    destinationBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    destinationBarrier.srcQueueFamilyIndex = destination->queue_family[plane];
                    destinationBarrier.dstQueueFamilyIndex = destination->queue_family[plane] == VK_QUEUE_FAMILY_IGNORED
                                                                 ? VK_QUEUE_FAMILY_IGNORED
                                                                 : device_.graphics_family();
                    destinationBarrier.image = destination->img[plane];
                    destinationBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &destinationBarrier);
                    VkImageCopy copy{};
                    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.dstSubresource = copy.srcSubresource;
                    copy.extent = {planeWidths[plane], planeHeights[plane], 1};
                    vkCmdCopyImage(command, sourceImages[plane]->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   destination->img[plane], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    VkImageMemoryBarrier destinationRelease = destinationBarrier;
                    destinationRelease.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    destinationRelease.dstAccessMask = 0;
                    destinationRelease.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    destinationRelease.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    destinationRelease.srcQueueFamilyIndex = destinationBarrier.dstQueueFamilyIndex;
                    destinationRelease.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &destinationRelease);
                    recordImageBarrier(command, *sourceImages[plane], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0);
                }
            },
            {source.y.retain(), source.u.retain(), source.v.retain(),
             std::shared_ptr<const void>(std::move(retainedOwner))},
            semaphores, 10'000'000'000ULL);
        if (!completion)
            fail("Vulkan YUV-to-CUDA copy submission capacity exhausted");
        if (!queue.wait(*completion, 10'000'000'000ULL))
            fail("Vulkan YUV-to-CUDA copy timed out");
        for (int plane = 0; plane < 3; ++plane) {
            destination->layout[plane] = VK_IMAGE_LAYOUT_GENERAL;
            destination->access[plane] = VK_ACCESS_TRANSFER_WRITE_BIT;
            destination->queue_family[plane] = device_.graphics_family();
            if (destination->sem[plane] != VK_NULL_HANDLE)
                ++destination->sem_value[plane];
        }
        stats.deviceToDeviceMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        stats.deviceToDeviceBytes +=
            static_cast<std::uint64_t>(width) * height + static_cast<std::uint64_t>(width) * height / 2;
    }

private:
    Device& device_;
    Allocator& allocator_;
    std::vector<std::uint32_t> spirv_;
};

class VulkanCudaBridge {
public:
    VulkanCudaBridge(Instance& instance, Device& device) : instance_(instance), device_(device) {}

    static bool isCapabilityFailure(int status) {
        return status == AVERROR(ENOSYS) || status == AVERROR(ENODEV) || status == AVERROR(ENOTSUP) ||
               status == AVERROR(EOPNOTSUPP);
    }

    [[nodiscard]] bool ensure(int width, int height, std::string& reason) {
        if (quarantined_)
            fail("previous transfer failed; retained interop resources require encoder teardown");
        if (vulkanFrames_ != nullptr && width_ == width && height_ == height)
            return true;
        vulkanFrames_.reset();
        vulkanDevice_.reset();
        // FFmpeg borrows these strings until both contexts have been freed.
        extensionStorage_ = device_.enabled_extensions();
        extensionNames_.clear();
        extensionNames_.reserve(extensionStorage_.size());
        for (const std::string& extension : extensionStorage_)
            extensionNames_.push_back(extension.c_str());
        AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (deviceRef == nullptr)
            fail("av_hwdevice_ctx_alloc(VULKAN) failed");
        vulkanDevice_ = makeBufferRef(deviceRef);
        auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(deviceRef->data);
        auto* vulkan = reinterpret_cast<AVVulkanDeviceContext*>(deviceContext->hwctx);
        vulkan->get_proc_addr = &vkGetInstanceProcAddr;
        vulkan->inst = instance_.handle();
        vulkan->phys_dev = device_.physical();
        vulkan->act_dev = device_.handle();
        vulkan->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vulkan->queue_family_index = static_cast<int>(device_.graphics_family());
        vulkan->nb_graphics_queues = 1;
        vulkan->queue_family_tx_index = static_cast<int>(device_.transfer_family());
        vulkan->nb_tx_queues = 1;
        vulkan->queue_family_comp_index = static_cast<int>(device_.graphics_family());
        vulkan->nb_comp_queues = 1;
        vulkan->queue_family_encode_index = device_.encode_family() ? static_cast<int>(*device_.encode_family()) : -1;
        vulkan->nb_encode_queues = device_.encode_family() ? 1 : 0;
        vulkan->queue_family_decode_index = device_.decode_family() ? static_cast<int>(*device_.decode_family()) : -1;
        vulkan->nb_decode_queues = device_.decode_family() ? 1 : 0;
        deviceContext->user_opaque = &device_;
        vulkan->lock_queue = [](AVHWDeviceContext* context, std::uint32_t family, std::uint32_t) {
            static_cast<Device*>(context->user_opaque)->queueMutex(family).lock();
        };
        vulkan->unlock_queue = [](AVHWDeviceContext* context, std::uint32_t family, std::uint32_t) {
            static_cast<Device*>(context->user_opaque)->queueMutex(family).unlock();
        };
        vulkan->enabled_dev_extensions = extensionNames_.data();
        vulkan->nb_enabled_dev_extensions = static_cast<int>(extensionNames_.size());
        const int deviceStatus = av_hwdevice_ctx_init(deviceRef);
        if (deviceStatus < 0) {
            reason = "Vulkan device init over Nemo device failed: " + avError(deviceStatus);
            vulkanDevice_.reset();
            if (isCapabilityFailure(deviceStatus))
                return false;
            fail(reason);
        }
        AVBufferRef* framesRef = av_hwframe_ctx_alloc(vulkanDevice_.get());
        if (framesRef == nullptr)
            fail("av_hwframe_ctx_alloc(VULKAN) failed");
        vulkanFrames_ = makeBufferRef(framesRef);
        auto* frames = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
        frames->format = AV_PIX_FMT_VULKAN;
        frames->sw_format = AV_PIX_FMT_YUV420P;
        frames->width = width;
        frames->height = height;
        auto* vulkanFrames = reinterpret_cast<AVVulkanFramesContext*>(frames->hwctx);
        vulkanFrames->flags = AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
        vulkanFrames->usage =
            static_cast<VkImageUsageFlagBits>(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const int frameStatus = av_hwframe_ctx_init(framesRef);
        if (frameStatus < 0) {
            reason = "Vulkan YUV420P export frame pool init failed: " + avError(frameStatus);
            vulkanFrames_.reset();
            vulkanDevice_.reset();
            if (isCapabilityFailure(frameStatus))
                return false;
            fail(reason);
        }
        width_ = width;
        height_ = height;
        reason.clear();
        return true;
    }

    void transfer(const GpuYuvPlanes& planes, AVBufferRef* cudaFrames, AVFrame& destination, int width, int height,
                  ViewerEncodeStats& stats) {
        if (cudaFrames == nullptr)
            fail("CUDA destination frame context is null");
        const auto allocationStart = std::chrono::steady_clock::now();
        auto source = makeFrame();
        if (source == nullptr)
            fail("Vulkan source frame allocation failed");
        source->format = AV_PIX_FMT_VULKAN;
        source->width = width;
        source->height = height;
        if (av_hwframe_get_buffer(vulkanFrames_.get(), source.get(), 0) < 0)
            fail("Vulkan source frame allocation failed");
        auto* vkFrame = reinterpret_cast<AVVkFrame*>(source->data[0]);
        if (vkFrame == nullptr)
            fail("Vulkan source frame has no AVVkFrame payload");
        stats.allocationPackingMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - allocationStart).count();
        // The GPU adapter owns all Vulkan queue work and copy synchronization.
        // FFmpeg owns CUDA frame allocation and transfer semantics.
        copyConverter_->copyToVulkan(planes, *source, width, height, stats);
        const auto destinationStart = std::chrono::steady_clock::now();
        destination.format = AV_PIX_FMT_CUDA;
        destination.width = width;
        destination.height = height;
        if (av_hwframe_get_buffer(cudaFrames, &destination, 0) < 0)
            fail("CUDA destination frame allocation failed");
        auto retainedSource = makeFrame();
        auto retainedDestination = makeFrame();
        if (retainedSource == nullptr || retainedDestination == nullptr ||
            av_frame_ref(retainedSource.get(), source.get()) < 0 ||
            av_frame_ref(retainedDestination.get(), &destination) < 0)
            fail("interop frame retention failed before device transfer");
        retainedSources_.push_back(std::move(retainedSource));
        retainedDestinations_.push_back(std::move(retainedDestination));
        // These surface owners remain resident until this chunk completes.
        // Count each Vulkan memory handle once, plus CUDA row-pitched pixels
        // and the application's packed GPU YUV payload.
        std::uint64_t staged = static_cast<std::uint64_t>(width) * height * 3 / 2;
        for (int plane = 0; plane < 3; ++plane) {
            bool firstAllocation = vkFrame->mem[plane] != VK_NULL_HANDLE;
            for (int previous = 0; previous < plane; ++previous)
                firstAllocation = firstAllocation && vkFrame->mem[previous] != vkFrame->mem[plane];
            if (firstAllocation)
                staged += vkFrame->size[plane];
            staged += static_cast<std::uint64_t>(destination.linesize[plane]) * (plane == 0 ? height : height / 2);
        }
        stats.stagingBytes += staged;
        stats.allocationPackingMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - destinationStart).count();
        const auto transferStart = std::chrono::steady_clock::now();
        const int status = av_hwframe_transfer_data(&destination, source.get(), 0);
        stats.deviceToDeviceMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - transferStart).count();
        if (status < 0)
            fail("Vulkan-to-CUDA device transfer failed: " + avError(status));
        stats.deviceToDeviceBytes +=
            static_cast<std::uint64_t>(width) * height + static_cast<std::uint64_t>(width) * height / 2;
    }

    void setConverter(GpuYuvConverter& converter) { copyConverter_ = &converter; }

    [[nodiscard]] bool drainTransfers() noexcept {
        bool complete = true;
        try {
            device_.submissions(device_.graphics_family()).drain();
        } catch (...) {
            complete = false;
        }
        // FFmpeg's hardware-to-host transfer is the public synchronization
        // boundary for its CUDA stream. This path is failure-only: it drains
        // before releasing the external Vulkan/CUDA frame owners.
        for (const FramePtr& destination : retainedDestinations_) {
            if (destination == nullptr) {
                complete = false;
                continue;
            }
            auto host = makeFrame();
            if (host == nullptr) {
                complete = false;
                continue;
            }
            host->format = AV_PIX_FMT_YUV420P;
            host->width = destination->width;
            host->height = destination->height;
            if (av_frame_get_buffer(host.get(), 0) < 0 ||
                av_hwframe_transfer_data(host.get(), destination.get(), 0) < 0)
                complete = false;
        }
        if (complete) {
            retainedSources_.clear();
            retainedDestinations_.clear();
        }
        return complete;
    }

    ~VulkanCudaBridge() {
        // Do not release AVFrame owners or their external images until both
        // Nemo queue work and FFmpeg's CUDA stream have completed. A wedged
        // device blocks teardown rather than permitting a use-after-free.
        while (!drainTransfers())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // FFmpeg borrows the extension-name arrays below. Destroy its frame
        // and device contexts while those arrays and the CUDA session live.
        vulkanFrames_.reset();
        vulkanDevice_.reset();
    }

    void finishChunk() {
        retainedSources_.clear();
        retainedDestinations_.clear();
    }

    [[nodiscard]] bool quarantined() const { return quarantined_; }
    [[nodiscard]] bool abortChunk() {
        quarantined_ = true;
        return drainTransfers();
    }

private:
    Instance& instance_;
    Device& device_;
    BufferRefPtr vulkanDevice_{nullptr, deleteBufferRef};
    BufferRefPtr vulkanFrames_{nullptr, deleteBufferRef};
    std::vector<std::string> extensionStorage_;
    std::vector<const char*> extensionNames_;
    int width_ = 0;
    int height_ = 0;
    std::vector<FramePtr> retainedSources_;
    std::vector<FramePtr> retainedDestinations_;
    bool quarantined_ = false;
    GpuYuvConverter* copyConverter_ = nullptr;
};

}  // namespace

struct ViewerEncodeInterop::Impl {
    Instance* instance = nullptr;
    Device* device = nullptr;
    Allocator* allocator = nullptr;
    std::unique_ptr<GpuYuvConverter> converter;
    std::vector<GpuYuvPlanes> retainedGpuPlanes;
    // Bridge is declared last so it is destroyed first: CUDA mappings drain
    // while conversion images and the allocator still own their resources.
    std::unique_ptr<VulkanCudaBridge> bridge;
};

ViewerEncodeInterop::ViewerEncodeInterop(Instance& instance, Device& device, Allocator& allocator)
    : impl_(std::make_unique<Impl>()) {
    impl_->instance = &instance;
    impl_->device = &device;
    impl_->allocator = &allocator;
}

ViewerEncodeInterop::~ViewerEncodeInterop() = default;

void ViewerEncodeInterop::prepare() {
    try {
        if (!impl_->converter)
            impl_->converter = std::make_unique<GpuYuvConverter>(*impl_->device, *impl_->allocator);
    } catch (const ViewerEncodeError&) {
        throw;
    } catch (const std::exception& error) {
        fail("GPU viewer conversion pipeline creation failed: " + std::string(error.what()));
    }
}

bool ViewerEncodeInterop::ensureDirectInterop(int width, int height, std::string& reason) {
    try {
        prepare();
        if (!impl_->bridge)
            impl_->bridge = std::make_unique<VulkanCudaBridge>(*impl_->instance, *impl_->device);
        return impl_->bridge->ensure(width, height, reason);
    } catch (const ViewerEncodeError&) {
        throw;
    } catch (const std::exception& error) {
        fail("Vulkan/CUDA interop initialization failed: " + std::string(error.what()));
    }
}

void ViewerEncodeInterop::convertToHost(const Image& image, int sourceWidth, int sourceHeight, int encodedWidth,
                                        int encodedHeight, std::vector<std::uint8_t>& planes,
                                        ViewerEncodeStats& stats) {
    try {
        if (!impl_->converter)
            impl_->converter = std::make_unique<GpuYuvConverter>(*impl_->device, *impl_->allocator);
        impl_->converter->convertToHost(image, sourceWidth, sourceHeight, encodedWidth, encodedHeight, planes, stats);
    } catch (const ViewerEncodeError&) {
        throw;
    } catch (const std::exception& error) {
        fail("GPU viewer staging conversion failed: " + std::string(error.what()));
    }
}

void ViewerEncodeInterop::convertToCuda(const Image& image, int sourceWidth, int sourceHeight, int encodedWidth,
                                        int encodedHeight, AVBufferRef* cudaFrames, AVFrame* destination,
                                        ViewerEncodeStats& stats) {
    if (destination == nullptr)
        fail("CUDA destination frame is null");
    if (!impl_->bridge || impl_->bridge->quarantined())
        fail("Vulkan/CUDA interop is not available for this chunk");
    if (!impl_->converter)
        impl_->converter = std::make_unique<GpuYuvConverter>(*impl_->device, *impl_->allocator);
    try {
        impl_->retainedGpuPlanes.push_back(
            impl_->converter->convertDevice(image, sourceWidth, sourceHeight, encodedWidth, encodedHeight, stats));
        impl_->bridge->setConverter(*impl_->converter);
        impl_->bridge->transfer(impl_->retainedGpuPlanes.back(), cudaFrames, *destination, encodedWidth, encodedHeight,
                                stats);
    } catch (const ViewerEncodeError&) {
        throw;
    } catch (const std::exception& error) {
        fail("Vulkan/CUDA viewer transfer failed: " + std::string(error.what()));
    }
}

void ViewerEncodeInterop::uploadHostToCuda(const std::vector<std::uint8_t>& planes, int width, int height,
                                           AVBufferRef* cudaFrames, AVFrame* destination, ViewerEncodeStats& stats) {
    if (cudaFrames == nullptr)
        fail("CUDA destination frame context is null");
    if (destination == nullptr)
        fail("CUDA destination frame is null");
    const std::size_t expectedBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2;
    if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0 || planes.size() != expectedBytes)
        fail("hardware fallback returned an invalid compact 4:2:0 staging size");
    try {
        const auto allocationStart = std::chrono::steady_clock::now();
        FramePtr host(makeYuv420pFrame(width, height, planes), deleteFrame);
        if (host == nullptr)
            fail("host staging frame allocation failed");
        destination->format = AV_PIX_FMT_CUDA;
        destination->width = width;
        destination->height = height;
        if (av_hwframe_get_buffer(cudaFrames, destination, 0) < 0)
            fail("CUDA destination frame allocation failed");
        stats.allocationPackingMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - allocationStart).count();
        const auto transferStart = std::chrono::steady_clock::now();
        const int status = av_hwframe_transfer_data(destination, host.get(), 0);
        stats.hostToDeviceMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - transferStart).count();
        if (status < 0)
            fail("device frame upload failed: " + avError(status));
        for (int plane = 0; plane < 3; ++plane) {
            const int rowBytes = std::min(host->linesize[plane], destination->linesize[plane]);
            stats.hostToDeviceBytes +=
                static_cast<std::uint64_t>(rowBytes) * static_cast<std::uint64_t>(plane == 0 ? height : height / 2);
        }
    } catch (const ViewerEncodeError&) {
        throw;
    } catch (const std::exception& error) {
        fail("host-to-CUDA viewer transfer failed: " + std::string(error.what()));
    }
}

void ViewerEncodeInterop::finishChunk() {
    if (impl_->bridge)
        impl_->bridge->finishChunk();
    impl_->retainedGpuPlanes.clear();
}

void ViewerEncodeInterop::abortChunk() noexcept {
    if (!impl_->bridge)
        return;
    const bool complete = impl_->bridge->abortChunk();
    if (complete)
        impl_->retainedGpuPlanes.clear();
}

bool ViewerEncodeInterop::quarantined() const {
    return impl_->bridge != nullptr && impl_->bridge->quarantined();
}

}  // namespace nemo::gpu
