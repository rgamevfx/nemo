#pragma once

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/ComputePass.hpp"
namespace nemo::media {
struct OcioGpuProgram;
}

namespace nemo::gpu {

struct GpuViewedImage {
    gpu::Image image;  // Display-referred RGBA32F, GENERAL layout after completion.
    gpu::SubmissionQueue::Completion completion;
    ColorInterpretation color{ColorInterpretation::DisplayReferred};
};

// Worker-side OCIO preparation; compiles and uploads immutable LUTs once.
// Device/Instance must outlive this object and all submitted work. Calls to
// submit may run concurrently; the source must be an RGBA32F 1D/2D image in
// GENERAL with TRANSFER_SRC usage, produced on the same graphics queue (or
// externally synchronized). The executor applies one OCIO processor, so it
// serves the viewer's display transform and the source path's retained
// input-to-working transform alike; the output interpretation is the
// caller's, and a display-referred input is always refused (never viewed
// twice).
//
// The program's `pixelInterface` selects which entry point is legal (issues
// #90, #98): a buffer program drives `submit`, and a packed-image program
// drives `submitInputTransformInPlace`, which converts the native packed
// four-channel image a decoded media frame already is. Using the wrong entry
// point for a program is refused, never silently reinterpreted.
class GpuViewingTransform {
public:
    GpuViewingTransform(const GpuViewingTransform&) = delete;
    GpuViewingTransform& operator=(const GpuViewingTransform&) = delete;
    GpuViewingTransform(GpuViewingTransform&&) = delete;
    GpuViewingTransform& operator=(GpuViewingTransform&&) = delete;
    GpuViewingTransform(gpu::Device& device, gpu::Allocator& allocator, const media::OcioGpuProgram& program);
    // No execution wait or pixel readback. Zero admissionTimeout_ns reports
    // shared queue backpressure as nullopt; positive values wait for admission.
    // Dropping the result cancels publication, not GPU resource ownership.
    // Source interpretation is mandatory: viewed/cache-replay images must
    // never enter this transform a second time.
    [[nodiscard]] std::optional<GpuViewedImage> submit(const gpu::Image& source, ColorInterpretation sourceColor,
                                                       uint64_t admission_timeout_ns = 0) const;

    // In-place OCIO input-to-working transform over the native packed
    // four-channel image itself (issues #90, #98): a 2D RGBA32F image holding
    // the logical raster, one texel per pixel, which the kernel reads and
    // writes once — no staging buffer, no copy and no second image per frame.
    // One invocation per logical pixel; nothing is read back. Returns the
    // recorded completion so the caller can wait exactly as it waits for its
    // own decode, or nothing when the queue reports admission backpressure.
    // Throws GpuException for a source that is not the packed four-channel
    // native image or for a program that is not the image variant.
    [[nodiscard]] std::optional<gpu::SubmissionQueue::Completion>
    submitInputTransformInPlace(const gpu::Image& image, uint64_t admission_timeout_ns = 0) const;

private:
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::vector<std::uint32_t> spirv_;
    gpu::Buffer uniforms_;
    std::vector<gpu::Image> luts_;
    std::vector<gpu::ComputeBinding> bindings_;
    std::uint32_t descriptorSet_;
    // True when the retained program declares the packed-image interface
    // (recorded from `OcioGpuProgram::pixelInterface` at construction, so this
    // header needs no media type).
    bool packedImageInterface_{false};
};

}  // namespace nemo::gpu
