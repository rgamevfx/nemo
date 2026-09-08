#include "nemo/gpu/MediaInterop.hpp"
#include "nemo/gpu/ComputePass.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "nemo/gpu/Error.hpp"

namespace nemo::gpu {

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& detail = {}) {
    throw GpuException(GpuError::VulkanError, "media interop: " + what + (detail.empty() ? "" : ": " + detail));
}

VkImageView createPlaneView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    checkVulkan(vkCreateImageView(device, &info, nullptr, &view), "vkCreateImageView (foreign video plane)");
    return view;
}

}  // namespace

struct MediaInterop::Impl {
    Device* device = nullptr;
    Allocator* allocator = nullptr;
    std::vector<std::uint32_t> spirv;
};

MediaInterop::~MediaInterop() = default;
struct MediaInterop::Token {};
MediaInterop::MediaInterop(Token) {}

std::unique_ptr<MediaInterop> MediaInterop::create(Device& device, Allocator& allocator,
                                                   const std::vector<std::uint32_t>& convertSpirv) {
    auto interop = std::make_unique<MediaInterop>(Token{});
    interop->impl_ = std::make_unique<Impl>();
    interop->impl_->device = &device;
    interop->impl_->allocator = &allocator;
    interop->impl_->spirv = convertSpirv;
    return interop;
}

void MediaInterop::convertToRgba32f(ForeignVideoFrame& frame, Image& output, uint64_t timeout_ns) {
    const auto completion = submitToRgba32f(frame, output);
    if (!completion)
        fail("submission capacity exhausted");
    auto& queue = impl_->device->submissions(impl_->device->graphics_family());
    try {
        if (!queue.wait(*completion, timeout_ns))
            throw GpuException(GpuError::SubmissionTimeout, "media conversion wait timed out");
    } catch (...) {
        // The synchronous decoder adapter hands frame bookkeeping back only
        // after this call; drain before unwinding its borrowed decoder state.
        queue.drain();
        throw;
    }
}

std::optional<SubmissionQueue::Completion> MediaInterop::submitToRgba32f(ForeignVideoFrame& frame, Image& output) {
    if (!frame.owner)
        fail("foreign frame requires retained ownership");
    const VkDevice vkDevice = impl_->device->handle();
    if (frame.chromaLocation != MediaChromaLocation::Left) {
        fail("foreign frame chroma location must be left (the only supported sample position)");
    }
    if (frame.primaries != MediaPrimaries::Bt709) {
        // The kernel maps into scene-linear Rec.709 with no chromaticity
        // conversion: anything else would be mislabeled output, so it is an
        // explicit error, not a guess.
        fail("foreign frame primaries must be bt709 (the only supported working-space source)");
    }
    if (frame.width == 0 || frame.height == 0) {
        fail("foreign frame extent must be non-empty");
    }
    if (frame.planeCount == 0 || frame.planeCount > 2) {
        fail("foreign frame must carry 1..2 planes");
    }
    if (frame.multiplane && frame.planeCount != 1) {
        fail("multiplane foreign frame must carry exactly one image");
    }

    // meta = (width, height, 0, 0) for the kernel's region guard; param0
    // carries the declared color interpretation: (transfer, range, matrix,
    // 0). The kernel converts Y′CbCr(range/matrix) → R′G′B′ and inverts
    // `transfer` into scene-linear Rec.709.
    Buffer uniform =
        impl_->allocator->create_buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryPreference::HostMapped);
    const std::uint32_t meta[4] = {frame.width, frame.height, 0, 0};
    const float param0[4] = {static_cast<float>(frame.transfer), static_cast<float>(frame.range),
                             static_cast<float>(frame.matrix), 0.0F};
    std::memcpy(uniform.mapped(), meta, sizeof(meta));
    std::memcpy(static_cast<std::byte*>(uniform.mapped()) + 32, param0, sizeof(param0));

    struct PlaneViews {
        VkDevice device;
        std::shared_ptr<const void> frame;
        VkImageView views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        ~PlaneViews() {
            for (auto view : views)
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device, view, nullptr);
        }
    };
    auto owners = std::make_shared<PlaneViews>();
    owners->device = vkDevice;
    owners->frame = frame.owner;
    auto& views = owners->views;
    const VkFormat planeFormats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
    if (frame.multiplane) {
        views[0] = createPlaneView(vkDevice, frame.images[0], planeFormats[0], VK_IMAGE_ASPECT_PLANE_0_BIT);
        views[1] = createPlaneView(vkDevice, frame.images[0], planeFormats[1], VK_IMAGE_ASPECT_PLANE_1_BIT);
    } else {
        for (uint32_t plane = 0; plane < frame.planeCount; ++plane) {
            views[plane] =
                createPlaneView(vkDevice, frame.images[plane], frame.formats[plane], VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    std::vector<ComputeBinding> bindings = {
        {0, 0, DescriptorKind::UniformBuffer, &uniform},
        {1, 0, DescriptorKind::CombinedImageSampler, nullptr, nullptr, false, views[0], owners},
        {1, 1, DescriptorKind::CombinedImageSampler, nullptr, nullptr, false, views[1], owners},
        {2, 0, DescriptorKind::StorageImage, nullptr, &output},
    };
    auto pass = ComputePass::create(*impl_->device, impl_->spirv, bindings);

    // Cross-queue dependency: wait for the producer's decode signal, hand
    // the planes back at waitValue+1 when the conversion completed.
    SubmissionQueue::TimelineSemaphores semaphores;
    for (uint32_t plane = 0; plane < frame.planeCount; ++plane) {
        if (frame.multiplane && plane == 1) {
            continue;  // single multiplane image: one semaphore governs it
        }
        semaphores.wait.push_back(frame.semaphores[plane]);
        semaphores.waitValues.push_back(frame.waitValues[plane]);
        semaphores.signal.push_back(frame.semaphores[plane]);
        semaphores.signalValues.push_back(frame.waitValues[plane] + 1);
    }

    auto& queue = impl_->device->submissions(impl_->device->graphics_family());
    const auto completion = queue.submit(
        [&](VkCommandBuffer cmd) {
            // Synchronization2 barriers: the producer stage bits (video
            // decode) exist only in the FlagBits2 form. Multiplane frames
            // barrier the single image over both plane aspects; per-plane
            // frames barrier each image with COLOR aspects.
            const VkImageAspectFlags barrierAspects[2] = {
                frame.multiplane
                    ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT)
                    : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT)};
            const uint32_t barrierCount = frame.multiplane ? 1u : frame.planeCount;
            VkImageMemoryBarrier2 acquires[3] = {};
            for (uint32_t plane = 0; plane < barrierCount; ++plane) {
                VkImageMemoryBarrier2& barrier = acquires[plane];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                // Chain the ALL_COMMANDS semaphore wait into the layout
                // transition. NONE would let that transition race the
                // producer's decode/DPB accesses even though sampling waits.
                // ALL_COMMANDS is valid here; VIDEO_DECODE is not a stage
                // supported by this graphics queue family.
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_NONE;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                barrier.oldLayout = frame.layouts[plane];
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                // Concurrent-sharing frames (family IGNORED) need no
                // ownership transfer; exclusive-sharing frames transfer
                // decode -> graphics.
                barrier.srcQueueFamilyIndex = frame.queueFamilies[plane];
                barrier.dstQueueFamilyIndex = frame.queueFamilies[plane] == VK_QUEUE_FAMILY_IGNORED
                                                  ? VK_QUEUE_FAMILY_IGNORED
                                                  : impl_->device->graphics_family();
                barrier.image = frame.images[plane];
                barrier.subresourceRange = {barrierAspects[plane], 0, 1, 0, 1};
            }
            // Fresh output images start UNDEFINED; transition to the
            // contract's GENERAL layout for the storage write.
            VkImageMemoryBarrier2 outputAcquire{};
            outputAcquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            outputAcquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            outputAcquire.srcAccessMask = VK_ACCESS_2_NONE;
            outputAcquire.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            outputAcquire.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            outputAcquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            outputAcquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            outputAcquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            outputAcquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            outputAcquire.image = output.handle();
            outputAcquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo acquireInfo{};
            acquireInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            acquireInfo.imageMemoryBarrierCount = barrierCount + 1;
            acquireInfo.pImageMemoryBarriers = acquires;
            acquires[barrierCount] = outputAcquire;
            vkCmdPipelineBarrier2(cmd, &acquireInfo);

            pass->record(cmd, (frame.width + 7) / 8, (frame.height + 7) / 8, 1);

            // Release back to the producer: restored layout/access and
            // family ownership, so the decoder's next pass over the planes
            // is properly ordered by the signaled timeline value.
            VkImageMemoryBarrier2 releases[2] = {};
            for (uint32_t plane = 0; plane < barrierCount; ++plane) {
                VkImageMemoryBarrier2& barrier = releases[plane];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                // Mirror image: the decode queue's next submission waits on
                // the signaled timeline value, so the destination stage is
                // NONE here (ownership hand-back only).
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                barrier.dstAccessMask = VK_ACCESS_2_NONE;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout = frame.layouts[plane];
                barrier.srcQueueFamilyIndex = frame.queueFamilies[plane] == VK_QUEUE_FAMILY_IGNORED
                                                  ? VK_QUEUE_FAMILY_IGNORED
                                                  : impl_->device->graphics_family();
                barrier.dstQueueFamilyIndex = frame.queueFamilies[plane];
                barrier.image = frame.images[plane];
                barrier.subresourceRange = {barrierAspects[plane], 0, 1, 0, 1};
            }
            VkDependencyInfo releaseInfo{};
            releaseInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            releaseInfo.imageMemoryBarrierCount = barrierCount;
            releaseInfo.pImageMemoryBarriers = releases;
            vkCmdPipelineBarrier2(cmd, &releaseInfo);
        },
        {pass->retain()}, semaphores);
    if (!completion)
        return std::nullopt;
    for (uint32_t plane = 0; plane < (frame.multiplane ? 1u : frame.planeCount); ++plane) {
        // The producer waits on the incremented value before reusing the
        // plane; our last access was the shader read recorded below.
        frame.waitValues[plane] += 1;
        frame.accesses[plane] = VK_ACCESS_SHADER_READ_BIT;
        if (frame.queueFamilies[plane] != VK_QUEUE_FAMILY_IGNORED) {
            frame.queueFamilies[plane] = impl_->device->graphics_family();
        }
    }
    return completion;
}

}  // namespace nemo::gpu
