// Final production export staging (issue #94 story 84 under #97) — the GPU-owned
// transfer, not the diagnostic readback.
//
// Ownership: this file owns exactly one thing, the device-to-host copy of a
// completed export raster and the accounting that makes it production work
// rather than a debugging aid. It allocates its staging buffer through the
// application's Allocator (so the charge is admitted and reported by the same
// budget every device allocation is), retains the source image and that buffer
// through the device's SubmissionQueue until the fence signals, and returns the
// application's own `CpuImage` contract. Media encodes that; nothing here knows
// about EXR, ProRes or the viewer cache.

#include "nemo/gpu/ExportStaging.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "nemo/gpu/ChannelImage.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::gpu {

ExportStaging::ExportStaging(Device& device, Allocator& allocator) : device_(&device), allocator_(&allocator) {}

ExportStaging::~ExportStaging() = default;

StagedExport ExportStaging::stage(const Image& source, const ImageLayout& layout, const std::uint64_t timeout_ns) {
    const auto channelCount = static_cast<std::uint32_t>(layout.channels.size());
    if (layout.width <= 0 || layout.height <= 0 || channelCount == 0) {
        throw GpuException(GpuError::InvalidRequest,
                           "export staging needs a raster with a positive extent and at least one stored channel");
    }
    if (source.dimensions() != 2) {
        throw GpuException(GpuError::InvalidRequest, "export staging only transfers a two-dimensional raster");
    }
    // The source must BE the native representation of that channel count (issue
    // #98), never a guess: four stored channels are the packed four-component
    // image, every other count is one scalar plane per channel. A source whose
    // real format contradicts the description would otherwise be copied through
    // a wrong stride into the delivered file.
    if (source.format() != nativeChannelFormat(channelCount)) {
        throw GpuException(GpuError::InvalidRequest, "the export source is not the native " +
                                                         std::to_string(channelCount) +
                                                         "-channel layout of the staged description");
    }
    const std::uint32_t components = nativeChannelComponents(source.format());
    const VkExtent3D extent = source.extent();
    if (extent.width != static_cast<std::uint32_t>(layout.width) ||
        extent.height != nativeChannelHeight(static_cast<std::uint32_t>(layout.height), channelCount)) {
        throw GpuException(GpuError::InvalidRequest,
                           "the export source extent " + std::to_string(extent.width) + "x" +
                               std::to_string(extent.height) + " does not match the staged description " +
                               std::to_string(layout.width) + "x" + std::to_string(layout.height) + " with " +
                               std::to_string(channelCount) + " channels");
    }

    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * components * sizeof(float);
    Buffer staging = allocator_->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryPreference::HostMapped);

    // GENERAL -> TRANSFER_SRC -> copy -> GENERAL in one submission: the export
    // staging transfer must not change the resident image's layout convention,
    // so a result the evaluation still holds stays readable exactly where the
    // executor left it.
    auto& queue = device_->submissions(device_->graphics_family());
    const VkBuffer destination = staging.handle();
    const VkImage image = source.handle();
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            recordImageBarrier(command, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = extent;
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, 1, &copy);
            recordImageBarrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        },
        {staging.retain(), source.retain()}, {}, timeout_ns);
    if (!completion) {
        throw GpuException(GpuError::SubmissionTimeout,
                           "the export staging transfer could not be admitted to the device submission queue");
    }
    if (!queue.wait(*completion, timeout_ns)) {
        throw GpuException(GpuError::SubmissionTimeout,
                           "the export staging transfer did not complete within its timeout");
    }

    CpuImage stagedImage(layout);
    const float* staged = static_cast<const float*>(staging.mapped());
    if (components == 4) {
        std::memcpy(stagedImage.data(), staged, static_cast<std::size_t>(bytes));
    } else {
        // The scalar-plane representation interleaves into the description's
        // channel order: stored channel c of logical pixel (x, y) sits at plane
        // row (y + c * H).
        const auto width = static_cast<std::size_t>(layout.width);
        const auto height = static_cast<std::size_t>(layout.height);
        float* pixels = stagedImage.data();
        for (std::size_t channel = 0; channel < channelCount; ++channel) {
            for (std::size_t y = 0; y < height; ++y) {
                const float* row = staged + (channel * height + y) * width;
                for (std::size_t x = 0; x < width; ++x) {
                    pixels[(y * width + x) * channelCount + channel] = row[x];
                }
            }
        }
    }
    peakStagingBytes_ = std::max(peakStagingBytes_, static_cast<std::uint64_t>(bytes));
    return StagedExport{std::move(stagedImage), static_cast<std::uint64_t>(bytes)};
}

}  // namespace nemo::gpu
