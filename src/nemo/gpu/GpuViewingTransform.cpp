#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/gpu/ChannelImage.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/media/ViewingTransform.hpp"
#include <cstring>

namespace nemo::gpu {

namespace {

// The native packed image holds a four-channel frame's stored channels, one
// texel per logical pixel (issue #98).
constexpr std::uint32_t kPackedChannels = 4;

}  // namespace

GpuViewingTransform::GpuViewingTransform(gpu::Device& device, gpu::Allocator& allocator,
                                         const media::OcioGpuProgram& program)
    : device_(device), allocator_(allocator), spirv_(gpu::compileGlslToSpirv(program.glsl)),
      descriptorSet_(program.descriptorSet),
      packedImageInterface_(program.pixelInterface == media::OcioGpuProgram::PixelInterface::PackedImage) {
    luts_.reserve(program.textures.size());
    for (const auto& texture : program.textures) {
        const VkFormat format = texture.channels == 1 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        if (texture.channels != 1 && texture.channels != 4)
            throw gpu::GpuException(gpu::GpuError::InvalidRequest, "OCIO GPU LUT must have one or four channels");
        auto image = allocator_.create_image(texture.width, texture.height, texture.dimensions == 3 ? texture.width : 1,
                                             format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        gpu::uploadImage(device_.submissions(device_.graphics_family()), allocator_, image, texture.values.data(),
                         texture.values.size() * sizeof(float), 10'000'000'000ULL);
        luts_.push_back(std::move(image));
    }
    if (!program.uniformBytes.empty()) {
        uniforms_ = allocator_.create_buffer(program.uniformBytes.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                             gpu::MemoryPreference::HostMapped);
        std::memcpy(uniforms_.mapped(), program.uniformBytes.data(), program.uniformBytes.size());
        bindings_.push_back({descriptorSet_, 0, gpu::DescriptorKind::UniformBuffer, &uniforms_});
    }
    for (std::size_t i = 0; i < luts_.size(); ++i)
        bindings_.push_back({descriptorSet_, program.textures[i].binding, gpu::DescriptorKind::CombinedImageSampler,
                             nullptr, &luts_[i], program.textures[i].dimensions == 3});
}

std::optional<GpuViewedImage> GpuViewingTransform::submit(const gpu::Image& source, ColorInterpretation sourceColor,
                                                          uint64_t admissionTimeout_ns) const {
    if (packedImageInterface_)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "this OCIO program converts the packed image in place; use "
                                "submitInputTransformInPlace");
    // Scene-linear composition results and non-color Data both still need their
    // display transform; a display-referred buffer already has it, so applying
    // it again is refused (the transform is applied exactly once).
    if (sourceColor == ColorInterpretation::DisplayReferred)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "GPU viewing transform requires scene-linear or data input; display-referred input "
                                "already has its viewing transform");
    if (source.format() != VK_FORMAT_R32G32B32A32_SFLOAT || source.dimensions() == 0 || source.dimensions() > 2)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "GPU viewing transform requires a 1D/2D RGBA32F source");
    const auto extent = source.extent();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4 * sizeof(float);
    auto input = allocator_.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto output =
        allocator_.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto image = allocator_.create_image(extent.width, extent.height, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                         source.dimensions());
    auto bindings = bindings_;
    bindings.push_back({descriptorSet_ + 1, 0, gpu::DescriptorKind::StorageBuffer, &input});
    bindings.push_back({descriptorSet_ + 1, 1, gpu::DescriptorKind::StorageBuffer, &output});
    auto pass = gpu::ComputePass::create(device_, spirv_, bindings);
    auto completion =
        device_.submissions(device_.graphics_family())
            .submit(
                [&](VkCommandBuffer command) {
                    gpu::recordImageBarrier(command, source, VK_IMAGE_LAYOUT_GENERAL,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_ACCESS_TRANSFER_READ_BIT);
                    VkBufferImageCopy copy{};
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.imageExtent = extent;
                    vkCmdCopyImageToBuffer(command, source.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           input.handle(), 1, &copy);
                    gpu::recordImageBarrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_ACCESS_MEMORY_READ_BIT);
                    VkMemoryBarrier readable{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT};
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &readable, 0, nullptr, 0, nullptr);
                    pass->record(command, static_cast<uint32_t>((bytes / (4 * sizeof(float)) + 63) / 64), 1, 1);
                    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                                            VK_ACCESS_TRANSFER_READ_BIT};
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 1, &written, 0, nullptr, 0, nullptr);
                    gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                    vkCmdCopyBufferToImage(command, output.handle(), image.handle(),
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_ACCESS_MEMORY_READ_BIT);
                },
                {pass->retain(), source.retain(), image.retain()}, {}, admissionTimeout_ns);
    if (!completion)
        return std::nullopt;
    return GpuViewedImage{std::move(image), *completion};
}

std::optional<gpu::SubmissionQueue::Completion>
GpuViewingTransform::submitInputTransformInPlace(const gpu::Image& image, uint64_t admission_timeout_ns) const {
    if (!packedImageInterface_)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "this OCIO program converts interleaved RGBA32F pixel buffers; use submit");
    const VkExtent3D extent = image.extent();
    // The surface IS the native packed four-channel image: the frame the media
    // module produced, at its logical extent, nothing else.
    if (image.dimensions() != 2 || image.format() != gpu::nativeChannelFormat(kPackedChannels) || extent.width == 0 ||
        extent.height == 0) {
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "the in-place input transform requires the packed four-channel native image "
                                "(a 2D RGBA32F frame at its logical extent)");
    }
    auto bindings = bindings_;
    bindings.push_back({descriptorSet_ + 1, 0, gpu::DescriptorKind::StorageImage, nullptr, &image});
    auto pass = gpu::ComputePass::create(device_, spirv_, bindings);
    auto& queue = device_.submissions(device_.graphics_family());
    return queue.submit(
        [&](VkCommandBuffer command) {
            // The frame was written by an earlier submission on this queue (the
            // media interop conversion or the software upload), so the
            // dependency is conservative rather than an assumed immediate
            // producer. Each pixel is read and written by the same invocation,
            // and the alpha component is stored back unchanged.
            gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            pass->record(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
        },
        {pass->retain(), image.retain()}, {}, admission_timeout_ns);
}

}  // namespace nemo::gpu
