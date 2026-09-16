#include "nemo/eval/ChannelProjection.hpp"

#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace nemo::eval {
namespace {

using gpu::ComputeBinding;
using gpu::ComputePass;
using gpu::DescriptorKind;

// One invocation per logical pixel: gather the selected planes and write the
// interleaved RGBA32F presentation source. The projection rule (missing RGB ->
// 0, missing A -> 1 when any RGB role exists, else 0) is applied here exactly
// as nemo::CpuImage::pixel applies it on the reference path.
constexpr const char* kChannelProjectionGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer ProjectionRequest {
    // Source plane index of the R, G, B and A roles; -1 = the role is absent.
    ivec4 roles;
    // (logical width, logical height, reserved, reserved).
    uvec4 geometry;
};

layout(set = 1, binding = 0, r32f) readonly uniform image2D sourcePlanes;
layout(set = 2, binding = 0, rgba32f) writeonly uniform image2D targetRgba;

float projectionPlane(ivec2 p, int plane, int planeHeight) {
    return imageLoad(sourcePlanes, ivec2(p.x, p.y + plane * planeHeight)).x;
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(geometry.x) || p.y >= int(geometry.y)) { return; }
    const int planeHeight = int(geometry.y);
    vec4 value = vec4(0.0, 0.0, 0.0, (roles.x >= 0 || roles.y >= 0 || roles.z >= 0) ? 1.0 : 0.0);
    if (roles.x >= 0) { value.x = projectionPlane(p, roles.x, planeHeight); }
    if (roles.y >= 0) { value.y = projectionPlane(p, roles.y, planeHeight); }
    if (roles.z >= 0) { value.z = projectionPlane(p, roles.z, planeHeight); }
    if (roles.w >= 0) { value.w = projectionPlane(p, roles.w, planeHeight); }
    imageStore(targetRgba, p, value);
}
)GLSL";

}  // namespace

ChannelProjection::ChannelProjection(gpu::Device& device, gpu::Allocator& allocator)
    : device_(device), allocator_(allocator), spirv_(gpu::compileGlslToSpirv(kChannelProjectionGlsl)) {}

std::optional<ChannelProjection::Submission> ChannelProjection::submit(const gpu::Image& source,
                                                                       const std::array<std::int32_t, 4>& roles,
                                                                       std::uint32_t width, std::uint32_t height,
                                                                       std::uint64_t admissionTimeoutNs) const {
    if (source.format() != VK_FORMAT_R32_SFLOAT || source.dimensions() != 2)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "viewer channel projection requires a 2D R32_SFLOAT channel-plane source");
    const VkExtent3D extent = source.extent();
    if (width == 0 || height == 0 || extent.width < width || extent.height % height != 0)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                "viewer channel projection source extent does not hold the requested logical raster");

    auto request =
        allocator_.create_buffer(2 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gpu::MemoryPreference::HostMapped);
    const std::array<std::uint32_t, 4> geometry{width, height, 0, 0};
    std::memcpy(request.mapped(), roles.data(), 4 * sizeof(std::int32_t));
    std::memcpy(static_cast<std::byte*>(request.mapped()) + 16, geometry.data(), 4 * sizeof(std::uint32_t));

    auto target = allocator_.create_image(width, height, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                          2);
    auto pass = ComputePass::create(device_, spirv_,
                                    {ComputeBinding{0, 0, DescriptorKind::StorageBuffer, &request},
                                     ComputeBinding{1, 0, DescriptorKind::StorageImage, nullptr, &source},
                                     ComputeBinding{2, 0, DescriptorKind::StorageImage, nullptr, &target}});
    auto& queue = device_.submissions(device_.graphics_family());
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            // The composition may have been written by an earlier submission on
            // this queue (native effects, a source decode), so the dependency is
            // conservative rather than an assumed immediate producer.
            gpu::recordImageBarrier(command, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            gpu::recordImageBarrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_SHADER_WRITE_BIT);
            pass->record(command, (width + 7) / 8, (height + 7) / 8, 1);
        },
        {pass->retain(), source.retain(), target.retain()}, {}, admissionTimeoutNs);
    if (!completion)
        return std::nullopt;
    return Submission{std::move(target), *completion};
}

}  // namespace nemo::eval
