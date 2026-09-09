#pragma once

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/ComputePass.hpp"

namespace nemo::gpu {
struct PresentationReady;
struct ViewerPresentation {
    // Consumer-device RGBA8 UNORM image. Its token retains BOTH allocations.
    Image image;
    std::shared_ptr<PresentationReady> ready;
};

// Worker-only conversion and external sharing, NOT another view transform.
// Input: completed display-referred RGBA32F 2D image in GENERAL on producer.
// Quantization/premultiplication writes directly into exportable memory;
// no full-frame sharing copy or CPU readback. Output is immutable, released
// to EXTERNAL ownership, with producer completion observed before return.
// Timeout/cancellation retain all referenced resources through GPU completion.
// Both logical devices must be distinct and on the same physical GPU; they
// and their Instance outlive all results and completion-retained tokens.
[[nodiscard]] ViewerPresentation prepareViewerPresentation(Device& producer, Allocator& allocator, Device& consumer,
                                                           const Image& source, ColorInterpretation color,
                                                           const std::vector<std::uint32_t>& spirv,
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
