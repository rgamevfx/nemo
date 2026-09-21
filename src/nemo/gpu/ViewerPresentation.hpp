#pragma once

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/Bc7.hpp"
#include "nemo/gpu/ComputePass.hpp"

namespace nemo::gpu {
// Presentation-only display isolation. The selection is applied in the
// presentation copy, never in evaluation or the viewer cache, so a panel can
// inspect one channel without changing what the graph produced or what the
// cache stores. RGBA is the composite: the stored RGB, presented opaquely
// (issue #99) — alpha reaches pixels only through an explicit Premult node, so
// an alpha the graph replaced cannot change the presented color. Alpha is
// deliberately NOT a mode: alpha is data, so it is demanded by name and
// presented from the demanded image's RGB. BC7 retains the fourth component,
// but does not change this established channel-selection behavior.
enum class ViewerChannel : std::uint32_t { RGBA = 0, Red = 1, Green = 2, Blue = 3 };

struct PresentationReady;
struct ViewerPresentation {
    // Consumer-device RGBA8 UNORM image; its token retains both shared images.
    Image image;
    std::shared_ptr<PresentationReady> ready;
};

// Worker-only conversion and external sharing, NOT another view transform.
// Input: completed display-referred RGBA32F 2D image in GENERAL on producer.
// Quantization writes directly into exportable memory; no full-frame sharing
// copy or CPU readback. Output is immutable, released to EXTERNAL ownership.
// `channel` selects the presentation-only display isolation: Red/Green/Blue
// replicate that channel over an opaque alpha, while RGBA presents the stored
// RGB over an opaque alpha (issue #99). Every selection is opaque, so the
// surface never composites the image by its own alpha.
// Asynchronous: the call returns as soon as the work is submitted. GPU
// ordering is carried entirely by the exported semaphore, which signals when
// the copy completes — there is no producer-side host wait, so the caller must
// pin the result until every consumer submission that samples it finishes (the
// completion-retained tokens keep the resources alive regardless).
// Timeout/cancellation retain all referenced resources through GPU completion.
// Both logical devices must be distinct and on the same physical GPU; they
// and their Instance outlive all results and completion-retained tokens.
[[nodiscard]] ViewerPresentation prepareViewerPresentation(Device& producer, Allocator& allocator, Device& consumer,
                                                           const Image& source, ColorInterpretation color,
                                                           const std::vector<std::uint32_t>& spirv,
                                                           ViewerChannel channel = ViewerChannel::RGBA,
                                                           std::uint64_t timeout_ns = 10'000'000'000ULL);

// Replay counterpart for a completed BC7 frame (issue #106): samples the
// compressed texture directly into the same shared RGBA8 surface. This is the
// only replay presentation path — no full-float reconstruction, no
// reinterpretation of the frame as a working image, and no second viewing
// transform, because the viewing transform was applied before encoding. Sampling is
// linear UNORM at 1:1 texel centres through a NEAREST sampler, so no sRGB
// decode and no resampling can occur. `channel` is the identical
// presentation-only isolation the live overload applies, so a cached frame and
// the live frame beside it present the same selection. `source.image` keeps the
// requested logical extent (odd dimensions stay odd) and its padded edge blocks
// are never presented.
// Same device, ownership, lifetime and asynchrony contract as the live
// overload; `spirv` is the BC7 sampling kernel (viewerPresentationBc7.spv).
[[nodiscard]] ViewerPresentation prepareViewerPresentation(Device& producer, Allocator& allocator, Device& consumer,
                                                           const Bc7Image& source,
                                                           const std::vector<std::uint32_t>& spirv,
                                                           ViewerChannel channel = ViewerChannel::RGBA,
                                                           std::uint64_t timeout_ns = 10'000'000'000ULL);

// Consumer render-thread ONLY, before its frame submission. Queue the
// imported semaphore wait and record the matching external acquire into
// command. Repeated calls are no-ops. No host GPU wait. The caller must pin
// the complete result until every consumer submission sampling it finishes,
// including the empty semaphore-wait submission if the frame is abandoned.
// The consumer's queue and device-idle operations must all be confined to
// this same thread (Qt's render thread), never an execution worker.
void acquireViewerPresentation(Device& consumer, const ViewerPresentation& presentation, VkCommandBuffer command);
}  // namespace nemo::gpu
