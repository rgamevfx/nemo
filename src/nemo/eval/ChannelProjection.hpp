#pragma once

// Native channel-plane projection for the viewer (issue #90).
//
// A native composition result is an R32_SFLOAT 2D image holding every named
// channel of the raster as a vertical plane: logical pixel (x, y) and channel c
// live at (x, y + c*H). The displayed representation is the existing
// display-referred RGBA32F image the presentation, encode and viewer-cache paths
// already consume, so the viewer needs exactly one device-side step between
// them: gather the selected named channels of the composition into the R,G,B,A
// roles of an interleaved RGBA32F image, on the worker thread, with no host
// readback.
//
// This is the same runtime-GLSL mechanism the retained OCIO adapters use
// (gpu::compileGlslToSpirv): the kernel is fixed text, compiled once per
// owner, and never recompiled per frame.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::eval {

class ChannelProjection {
public:
    struct Submission {
        gpu::Image image;
        gpu::SubmissionQueue::Completion completion;
    };

    ChannelProjection(gpu::Device& device, gpu::Allocator& allocator);
    ChannelProjection(const ChannelProjection&) = delete;
    ChannelProjection& operator=(const ChannelProjection&) = delete;

    // Gathers `roles` out of `source` into a fresh interleaved RGBA32F image of
    // the same logical size, recorded on the graphics queue.
    //
    // `source` must be an R32_SFLOAT 2D channel-plane image whose extent is
    // (width, height * planeCount); `roles` names the source plane of each
    // R,G,B,A role, resolved by the caller from the described channel names.
    // A role of -1 projects nemo::CpuImage::pixel's rule exactly: R/G/B read
    // 0.0, A reads 1.0 when the image carries at least one RGB role and 0.0
    // when it carries none — so a presentation never invents a channel the
    // image does not store.
    //
    // Writes precede later submissions on the same queue, so intermediate
    // consumers need no host wait. A final publisher must wait for completion.
    // Nothing is returned on admission backpressure. Throws GpuException for
    // invalid source geometry or format.
    [[nodiscard]] std::optional<Submission> submit(const gpu::Image& source, const std::array<std::int32_t, 4>& roles,
                                                   std::uint32_t width, std::uint32_t height,
                                                   std::uint64_t admissionTimeoutNs = 0) const;

private:
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::vector<std::uint32_t> spirv_;
};

}  // namespace nemo::eval
