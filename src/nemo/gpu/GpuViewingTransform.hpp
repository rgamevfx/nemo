#pragma once

#include "nemo/gpu/ComputePass.hpp"
namespace nemo::media {
struct OcioGpuProgram;
}

namespace nemo::gpu {

struct GpuViewedImage {
    gpu::Image image;  // Display-referred RGBA32F, GENERAL layout after completion.
    gpu::SubmissionQueue::Completion completion;
};

// Worker-side OCIO preparation; compiles and uploads immutable LUTs once.
// Device/Instance must outlive this object and all submitted work. Calls to
// submit may run concurrently; the source must be an RGBA32F 1D/2D image in
// GENERAL with TRANSFER_SRC usage, produced on the same graphics queue (or
// externally synchronized). Output is display-referred, never scene-linear.
class GpuViewingTransform {
public:
    GpuViewingTransform(const GpuViewingTransform&) = delete;
    GpuViewingTransform& operator=(const GpuViewingTransform&) = delete;
    GpuViewingTransform(GpuViewingTransform&&) = delete;
    GpuViewingTransform& operator=(GpuViewingTransform&&) = delete;
    GpuViewingTransform(gpu::Device& device, gpu::Allocator& allocator, const media::OcioGpuProgram& program);
    // No host wait or pixel readback. nullopt reports shared queue capacity.
    // Dropping the result cancels publication, not GPU resource ownership.
    [[nodiscard]] std::optional<GpuViewedImage> submit(const gpu::Image& source) const;

private:
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::vector<std::uint32_t> spirv_;
    gpu::Buffer uniforms_;
    std::vector<gpu::Image> luts_;
    std::vector<gpu::ComputeBinding> bindings_;
    std::uint32_t descriptorSet_;
};
}  // namespace nemo::gpu
