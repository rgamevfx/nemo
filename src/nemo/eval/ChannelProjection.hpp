#pragma once

// Native channel projection for the viewer (issues #90, #98).
//
// A native composition result carries every named channel of its raster in the
// shared native representation: four stored channels are one packed RGBA32F
// texel per logical pixel, any other count is one R32_SFLOAT scalar plane per
// channel stacked vertically at (x, y + c*H). The displayed representation is
// the existing display-referred interleaved RGBA32F image the presentation,
// encode and viewer-cache paths already consume, so the viewer needs exactly
// one device-side step between them: gather the selected named channels of the
// composition into the R,G,B,A roles of an interleaved RGBA32F image, on the
// worker thread, with no host readback.
//
// A source that is ALREADY that image — the packed four-channel layout with the
// canonical R,G,B,A roles at the exact logical extent — needs no step at all;
// isIdentity answers exactly that question so a caller can retain the same
// allocation instead of paying for an identical second image.
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

    // True exactly when `source` already IS the requested presentation: the
    // packed four-channel image of exactly `width` x `height` whose components
    // are the R, G, B and A roles in order — i.e. `roles` is {0, 1, 2, 3} and
    // the source is a 2D RGBA32F image at exactly that logical extent. Any
    // other count, names, order, format or padded extent is a real projection.
    //
    // A pure query: no device work, no allocation. A caller that sees true may
    // retain the source itself (a shared owner of the same allocation, never a
    // copy) as the displayed image.
    [[nodiscard]] static bool isIdentity(const gpu::Image& source, const std::array<std::int32_t, 4>& roles,
                                         std::uint32_t width, std::uint32_t height);

    // Gathers `roles` out of `source` into a fresh interleaved RGBA32F image of
    // the same logical size, recorded on the graphics queue.
    //
    // `source` must be one of the two native representations, decided by the
    // image's ACTUAL format (never by a description):
    //   * four components per texel (packed RGBA32F at (width, height)): each
    //     role names a COMPONENT of the texel;
    //   * one component per texel (R32_SFLOAT planes at (width,
    //     height * planeCount)): each role names a PLANE, addressed at
    //     (x, y + role*height).
    // A role of -1 projects nemo::CpuImage::pixel's rule exactly: R/G/B read
    // 0.0, A reads 1.0 when the image carries at least one RGB role and 0.0
    // when it carries none — so a presentation never invents a channel the
    // image does not store. Roles are resolved by the caller from the described
    // channel names; names and stored order are never rewritten here.
    //
    // Writes precede later submissions on the same queue, so intermediate
    // consumers need no host wait. A final publisher must wait for completion.
    // Nothing is returned on admission backpressure. Throws GpuException for an
    // invalid source format, extent or role.
    [[nodiscard]] std::optional<Submission> submit(const gpu::Image& source, const std::array<std::int32_t, 4>& roles,
                                                   std::uint32_t width, std::uint32_t height,
                                                   std::uint64_t admissionTimeoutNs = 0) const;

private:
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::vector<std::uint32_t> spirv_;
};

}  // namespace nemo::eval
