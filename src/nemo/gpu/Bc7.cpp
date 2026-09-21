#include "nemo/gpu/Bc7.hpp"

#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"

#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nemo::gpu {
namespace {

// The one cache representation: BC7 RGBA, 4x4 blocks of 16 bytes, sampled as
// display-encoded UNORM. Never BC7_SRGB, which would silently decode.
constexpr VkFormat kBc7Format = VK_FORMAT_BC7_UNORM_BLOCK;
constexpr auto kBlockUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
constexpr auto kReadbackUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr auto kStagingUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
// Sampled for replay presentation; the compressed blocks are transferred in,
// so the representation needs no BC7 storage-image support.
constexpr auto kImageUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// One thread per 4x4 block, in 8x8-block groups: the dispatch is bounded by the
// raster, and every group's work is a fixed number of blocks.
constexpr std::uint32_t kGroupBlocks = 8;

[[nodiscard]] std::uint32_t blocksAcross(std::uint32_t extent) {
    return (extent + 3U) / 4U;
}

[[nodiscard]] std::uint32_t groupsAcross(std::uint32_t blocks) {
    return (blocks + kGroupBlocks - 1U) / kGroupBlocks;
}

void recordBufferBarrier(VkCommandBuffer command, const Buffer& buffer, VkPipelineStageFlags srcStage,
                         VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer.handle();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

[[nodiscard]] Allocator::Reservation reserveWorkingSet(Allocator& allocator, std::initializer_list<uint64_t> charges) {
    uint64_t total = 0;
    for (const auto bytes : charges) {
        if (bytes > std::numeric_limits<uint64_t>::max() - total)
            throw GpuException(GpuError::InvalidRequest, "BC7 working-set charge overflows");
        total += bytes;
    }
    return allocator.reserve(total);
}

// Compressed blocks -> BC7 texture. Rows are measured in texels for a
// block-compressed copy and both values must be whole blocks, so the packed
// payload is described as exactly blocksX/blocksY blocks wide; the copy extent
// stays the logical raster, which is what keeps odd dimensions odd.
void recordBlockUpload(VkCommandBuffer command, const Buffer& blocks, const Image& image, std::uint32_t width,
                       std::uint32_t height) {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = blocksAcross(width) * 4U;
    region.bufferImageHeight = blocksAcross(height) * 4U;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, blocks.handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

}  // namespace

struct Bc7Encoder::Impl {
    Device* device = nullptr;
    Allocator* allocator = nullptr;
    std::vector<std::uint32_t> spirv;
};

std::size_t bc7PayloadBytes(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0)
        throw GpuException(GpuError::InvalidRequest, "BC7 payload requires a nonempty raster");
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    const std::size_t blocksX = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocksY = (static_cast<std::size_t>(height) + 3) / 4;
    if (blocksY > kMax / blocksX)
        throw GpuException(GpuError::InvalidRequest, "BC7 payload size overflows for this raster");
    const std::size_t blocks = blocksX * blocksY;
    if (blocks > kMax / 16)
        throw GpuException(GpuError::InvalidRequest, "BC7 payload size overflows for this raster");
    return blocks * 16;
}

Bc7Encoder::Bc7Encoder(Device& device, Allocator& allocator, const std::filesystem::path& shaderDirectory)
    : impl_(std::make_unique<Impl>()) {
    // Real capability validation against this physical device, not an
    // assumption: the cache representation exists only where BC7 can be both
    // filled from compressed blocks and sampled for replay.
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device.physical(), kBc7Format, &properties);
    constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (!device.features().textureCompressionBC || (properties.optimalTilingFeatures & required) != required)
        throw GpuException(GpuError::InvalidRequest,
                           "viewer cache unavailable: device cannot sample and upload VK_FORMAT_BC7_UNORM_BLOCK");
    if (!device.features().shaderStorageImageReadWithoutFormat ||
        !device.features().shaderStorageImageWriteWithoutFormat)
        throw GpuException(GpuError::InvalidRequest,
                           "viewer cache unavailable: BC7 encoding requires storage image read/write without format");
    impl_->spirv = loadSpirv(shaderDirectory / "bc7Encode.spv");
    impl_->device = &device;
    impl_->allocator = &allocator;
}

Bc7Encoder::~Bc7Encoder() = default;

std::optional<Bc7Submission> Bc7Encoder::encode(const Image& displayRgba32f) {
    if (displayRgba32f.format() != VK_FORMAT_R32G32B32A32_SFLOAT || displayRgba32f.dimensions() != 2)
        throw GpuException(GpuError::InvalidRequest, "BC7 encode requires a 2D RGBA32F display image");
    const auto extent = displayRgba32f.extent();
    const std::size_t payload = bc7PayloadBytes(extent.width, extent.height);
    const std::uint32_t blocksX = blocksAcross(extent.width);
    const std::uint32_t blocksY = blocksAcross(extent.height);

    Allocator& allocator = *impl_->allocator;
    constexpr VkDeviceSize metaBytes = 16;  // std140 block, vec4-aligned
    auto reservation = reserveWorkingSet(
        allocator, {allocator.buffer_allocation_bytes(payload, kBlockUsage),
                    allocator.image_allocation_bytes(extent.width, extent.height, 1, kBc7Format, kImageUsage, 2),
                    allocator.buffer_allocation_bytes(payload, kReadbackUsage),
                    allocator.buffer_allocation_bytes(metaBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)});
    auto blocks = allocator.create_buffer(payload, kBlockUsage, MemoryPreference::Device, &reservation);
    auto image = allocator.create_image(extent.width, extent.height, 1, kBc7Format, kImageUsage, 2, &reservation);
    auto readback = allocator.create_buffer(payload, kReadbackUsage, MemoryPreference::HostMapped, &reservation);
    auto meta = allocator.create_buffer(metaBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryPreference::HostMapped,
                                        &reservation);
    const std::uint32_t metaValues[4] = {extent.width, extent.height, blocksX, 0};
    std::memcpy(meta.mapped(), metaValues, sizeof(metaValues));

    auto pass = ComputePass::create(*impl_->device, impl_->spirv,
                                    {{0, 0, DescriptorKind::StorageImage, nullptr, &displayRgba32f},
                                     {0, 1, DescriptorKind::StorageBuffer, &blocks},
                                     {0, 2, DescriptorKind::UniformBuffer, &meta}});

    auto shared = std::make_shared<Bc7Image>(Bc7Image{std::move(image)});
    auto& queue = impl_->device->submissions(impl_->device->graphics_family());
    auto retained =
        SubmissionQueue::RetainedResources{pass->retain(), shared->image.retain(), blocks.retain(), readback.retain()};
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            // The display frame is read where the graph left it; the barrier
            // orders this read after every earlier write on the queue, so no
            // host wait is needed between the graph and the encoder.
            recordImageBarrier(command, displayRgba32f, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            recordImageBarrier(command, shared->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
            pass->record(command, groupsAcross(blocksX), groupsAcross(blocksY), 1);
            recordBufferBarrier(command, blocks, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            recordBlockUpload(command, blocks, shared->image, extent.width, extent.height);
            recordImageBarrier(command, shared->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_ACCESS_SHADER_READ_BIT);
            // Persistence reads the compressed blocks only — never float pixels.
            VkBufferCopy copy{};
            copy.srcOffset = 0;
            copy.dstOffset = 0;
            copy.size = payload;
            vkCmdCopyBuffer(command, blocks.handle(), readback.handle(), 1, &copy);
        },
        std::move(retained), {}, 0);
    if (!completion)
        return std::nullopt;
    return Bc7Submission{std::move(shared), std::move(readback), *completion};
}

std::optional<Bc7Submission> Bc7Encoder::upload(std::uint32_t width, std::uint32_t height,
                                                std::span<const std::uint8_t> blocks) {
    const std::size_t payload = bc7PayloadBytes(width, height);
    if (blocks.size() != payload)
        throw GpuException(GpuError::InvalidRequest, "BC7 upload requires exactly " + std::to_string(payload) +
                                                         " compressed block bytes for " + std::to_string(width) + "x" +
                                                         std::to_string(height));
    Allocator& allocator = *impl_->allocator;
    const auto limit = impl_->device->properties().limits.maxImageDimension2D;
    if (width > limit || height > limit)
        throw GpuException(GpuError::InvalidRequest, "BC7 raster exceeds the device's 2D image extent limit");
    auto reservation =
        reserveWorkingSet(allocator, {allocator.buffer_allocation_bytes(payload, kStagingUsage),
                                      allocator.image_allocation_bytes(width, height, 1, kBc7Format, kImageUsage, 2)});
    auto staging = allocator.create_buffer(payload, kStagingUsage, MemoryPreference::HostMapped, &reservation);
    std::memcpy(staging.mapped(), blocks.data(), payload);
    auto image = allocator.create_image(width, height, 1, kBc7Format, kImageUsage, 2, &reservation);

    auto shared = std::make_shared<Bc7Image>(Bc7Image{std::move(image)});
    auto& queue = impl_->device->submissions(impl_->device->graphics_family());
    auto retained = SubmissionQueue::RetainedResources{shared->image.retain(), staging.retain()};
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            recordImageBarrier(command, shared->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
            recordBlockUpload(command, staging, shared->image, width, height);
            recordImageBarrier(command, shared->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_READ_BIT);
        },
        std::move(retained), {}, 0);
    if (!completion)
        return std::nullopt;
    return Bc7Submission{std::move(shared), Buffer{}, *completion};
}

}  // namespace nemo::gpu
