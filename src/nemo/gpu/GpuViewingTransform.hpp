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
// The program's `pixelLayout` selects which entry point is legal (issue #90):
// an Rgba32fBuffers program drives `submit`, and a ChannelPlanes program drives
// `submitInputTransformPlanesInPlace`. Using the wrong one for a program is
// refused, never silently reinterpreted.
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
                                                       uint64_t admissionTimeout_ns = 0) const;

    // In-place OCIO input-to-working transform over a native channel-plane image
    // (issue #90): an R32_SFLOAT 2D image with exactly four planes R,G,B,A at
    // (x, y + c*H) — the decoded-frame contract — where the RGB planes are
    // converted and the alpha plane (and any further plane) is left untouched.
    // One invocation per logical pixel; nothing is read back. Returns the
    // recorded completion so the caller can wait exactly as it waits for its own
    // decode, or nothing when the queue reports admission backpressure. Throws
    // GpuException for a source that is not a four-plane R32_SFLOAT 2D image or
    // for a display-referred source.
    [[nodiscard]] std::optional<gpu::SubmissionQueue::Completion>
    submitInputTransformPlanesInPlace(const gpu::Image& image, uint64_t admission_timeout_ns = 0) const;

private:
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::vector<std::uint32_t> spirv_;
    gpu::Buffer uniforms_;
    std::vector<gpu::Image> luts_;
    std::vector<gpu::ComputeBinding> bindings_;
    std::uint32_t descriptorSet_;
    // True when the retained program is a ChannelPlanes program (issue #90);
    // recorded from `OcioGpuProgram::pixelLayout` at construction so this
    // header needs no media type.
    bool channelPlanes_{false};
};

}  // namespace nemo::gpu
