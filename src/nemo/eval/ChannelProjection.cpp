#include "nemo/eval/ChannelProjection.hpp"

#include "nemo/gpu/ChannelImage.hpp"
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

// One invocation per logical pixel: gather the selected channels and write the
// interleaved RGBA32F presentation source. The projection rule (missing RGB ->
// 0, missing A -> 1 when any RGB role exists, else 0) is applied here exactly
// as nemo::CpuImage::pixel applies it on the reference path.
//
// The source binding is deliberately unformatted (issue #98): the same kernel
// reads whichever native representation the composition actually carries — the
// packed four-channel image, where a role names a component of the texel, or
// the scalar-plane image, where a role names a plane at (x, y + role*H). The
// representation is the image's own format, carried into the request, never a
// guess from the described channel list.
constexpr const char* kChannelProjectionGlsl = R"GLSL(
#version 450
#extension GL_EXT_shader_image_load_formatted : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer ProjectionRequest {
    // Source index of the R, G, B and A roles; -1 = the role is absent. For a
    // packed source the index is a component of the texel, for a plane source
    // the index of a plane.
    ivec4 roles;
    // (logical width, logical height, components per texel, reserved).
    uvec4 geometry;
};

layout(set = 1, binding = 0) readonly uniform image2D sourceImage;
layout(set = 2, binding = 0, rgba32f) writeonly uniform image2D targetRgba;

float projectionRole(ivec2 p, int role, int planeHeight, bool packed) {
    if (role < 0) { return 0.0; }
    if (packed) {
        const vec4 texel = imageLoad(sourceImage, p);
        return texel[role];
    }
    return imageLoad(sourceImage, ivec2(p.x, p.y + role * planeHeight)).x;
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(geometry.x) || p.y >= int(geometry.y)) { return; }
    const bool packed = geometry.z == 4u;
    const int planeHeight = int(geometry.y);
    vec4 value = vec4(0.0, 0.0, 0.0, (roles.x >= 0 || roles.y >= 0 || roles.z >= 0) ? 1.0 : 0.0);
    value.x = projectionRole(p, roles.x, planeHeight, packed);
    value.y = projectionRole(p, roles.y, planeHeight, packed);
    value.z = projectionRole(p, roles.z, planeHeight, packed);
    if (roles.w >= 0) {
        value.w = projectionRole(p, roles.w, planeHeight, packed);
    }
    imageStore(targetRgba, p, value);
}
)GLSL";

// Components per texel of the shared packed representation: one texel holds
// the R, G, B and A of one logical pixel.
constexpr std::uint32_t kPackedComponents = 4;

[[noreturn]] void failProjection(const std::string& what) {
    throw gpu::GpuException(gpu::GpuError::InvalidRequest, "viewer channel projection: " + what);
}

}  // namespace

ChannelProjection::ChannelProjection(gpu::Device& device, gpu::Allocator& allocator)
    : device_(device), allocator_(allocator), spirv_(gpu::compileGlslToSpirv(kChannelProjectionGlsl)) {}

bool ChannelProjection::isIdentity(const gpu::Image& source, const std::array<std::int32_t, 4>& roles,
                                   std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || roles != std::array<std::int32_t, 4>{0, 1, 2, 3}) {
        return false;
    }
    if (source.dimensions() != 2 || source.format() != gpu::nativeChannelFormat(kPackedComponents)) {
        return false;
    }
    const VkExtent3D extent = source.extent();
    return extent.width == width && extent.height == height;
}

std::optional<ChannelProjection::Submission> ChannelProjection::submit(const gpu::Image& source,
                                                                       const std::array<std::int32_t, 4>& roles,
                                                                       std::uint32_t width, std::uint32_t height,
                                                                       std::uint64_t admissionTimeoutNs) const {
    const VkExtent3D extent = source.extent();
    const std::uint32_t components = gpu::nativeChannelComponents(source.format());
    if (source.dimensions() != 2 || components == 0) {
        failProjection(
            "the source is not a native channel image (packed RGBA32F or scalar R32_SFLOAT planes), format " +
            std::to_string(static_cast<int>(source.format())));
    }
    if (width == 0 || height == 0 || extent.width < width) {
        failProjection("the source extent does not hold the requested logical raster");
    }
    // The role vocabulary follows the source's actual representation: four
    // components per texel names a component, one component per texel names a
    // plane. A role outside it would silently read an unrelated channel.
    std::uint32_t roleLimit = 0;
    if (components == kPackedComponents) {
        if (extent.height < height) {
            failProjection("the packed source does not hold the requested logical raster");
        }
        roleLimit = components;
    } else {
        if (extent.height % height != 0) {
            failProjection("the scalar-plane source extent is not a whole number of planes of the requested raster");
        }
        roleLimit = static_cast<std::uint32_t>(extent.height / height);
    }
    for (const std::int32_t role : roles) {
        if (role < -1 || role >= static_cast<std::int32_t>(roleLimit)) {
            failProjection("role " + std::to_string(role) + " is not one of the source's " + std::to_string(roleLimit) +
                           " stored " + (components == kPackedComponents ? "components" : "planes"));
        }
    }

    auto request =
        allocator_.create_buffer(2 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gpu::MemoryPreference::HostMapped);
    const std::array<std::uint32_t, 4> geometry{width, height, components, 0};
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
