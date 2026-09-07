#pragma once

// A SPIR-V compute pass over explicit descriptor bindings (issue #6).
//
// This is the smallest execution surface the viewing transform needs and the
// reusable seam for native effect execution (issue #8): build a compute
// pipeline from SPIR-V, bind storage/uniform buffers and sampled images by
// (set, binding), and run one dispatch synchronously. Image layout transitions
// and staging uploads are explicit Transfer helpers, not pass machinery.
//
// Lifetime/completion contract (spec section 10.2): dispatch() is
// synchronous — it returns only after the submitted command buffer completed
// (SubmissionQueue fence). Every bound resource must outlive the ComputePass
// (pass-owned samplers excepted). On timeout the SubmissionQueue contract
// applies: resources stay alive until the pass is destroyed.
//
// Thread safety: not thread-safe, like the Device it borrows.
//
// Validation-only CPU readback: host-mapped output buffers let a test or
// diagnostic read dispatch results. Routine viewer/display paths must not
// read back (spec section 11 no-readback gate is issue #8/#11 evidence).

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::gpu {

enum class DescriptorKind { UniformBuffer, StorageBuffer, CombinedImageSampler };

struct ComputeBinding {
    uint32_t set{};
    uint32_t binding{};
    DescriptorKind kind{};
    // Buffers: the bound Buffer must outlive the pass. Images: the bound
    // Image must outlive the pass; the pass owns its sampler.
    const Buffer* buffer{};
    const Image* image{};
    // Image sampling filter: LUT1D textures need LINEAR, 3D LUTs NEAREST
    // (OCIO's tetrahedral interpolation does its own filtering).
    bool nearest{false};
};

class ComputePass {
public:
    ComputePass() = default;
    ~ComputePass();

    ComputePass(ComputePass&&) noexcept;
    ComputePass& operator=(ComputePass&&) noexcept;

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    // Builds a compute pipeline from `spirv` (entry point "main") with the
    // given descriptor bindings. Descriptor set layouts are derived from
    // the binding table; every (set, binding) pair must be unique and every
    // set the SPIR-V references must be covered.
    // Throws a descriptive GpuException on any Vulkan failure.
    static std::unique_ptr<ComputePass> create(Device& device, const std::vector<std::uint32_t>& spirv,
                                               const std::vector<ComputeBinding>& bindings);

    // Records bindPipeline + bindDescriptorSets + dispatch(x, y, z) and
    // waits for completion. Graphics queue (graphics+compute family).
    void dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t timeout_ns);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Uploads `bytes` from `data` into `image` through a staging buffer and
// transitions the image to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
// Synchronous (SubmissionQueue fence). 1D/2D/3D images supported.
void uploadImage(SubmissionQueue& queue, Allocator& allocator, const Image& image, const void* data, std::size_t bytes,
                 uint64_t timeout_ns);

}  // namespace nemo::gpu
