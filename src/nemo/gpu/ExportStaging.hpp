#pragma once

// Final production export staging (issue #94 story 84, the narrow #97
// capability under #14): the ONE device-to-host transfer a delivered frame takes
// on its way to the media encoder.
//
// This is NOT the diagnostic `ComputePass` readback. It is an export-stage
// operation with its own contract: it is charged through the application's
// `Allocator` (so a delivery's host staging participates in the same byte
// admission as every device allocation), it retains the source image and its own
// staging buffer through the device's `SubmissionQueue` until the fence actually
// signals, and it produces the application image contract (`CpuImage`) the media
// owner encodes — never a codec-specific or display-referred representation, and
// never a viewer-cache frame.
//
// Lifetime/threading: Instance/Device/Allocator must outlive this object, which
// is confined to the one worker that owns a delivery job. Each `stage` call is
// synchronous with respect to its own submission (it waits the fence), so the
// returned raster is complete; the transfer keeps no state between calls beyond
// the peak byte high-water mark it reports.

#include <cstdint>
#include <string>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"

namespace nemo::gpu {

// One staged frame: the logical raster in the description's own channel order
// (interleaved float32, straight alpha) plus the bytes this transfer charged to
// the shared allocator.
struct StagedExport {
    CpuImage image;
    std::uint64_t stagingBytes{0};
};

class ExportStaging {
public:
    ExportStaging(Device& device, Allocator& allocator);
    ~ExportStaging();
    ExportStaging(const ExportStaging&) = delete;
    ExportStaging& operator=(const ExportStaging&) = delete;

    // Copies the completed GENERAL-layout native image `source` to host memory
    // and returns it as a `CpuImage` with `layout`'s logical geometry,
    // interpretation and named channel order.
    //
    // `source` must BE the native representation of `layout.channels.size()`
    // stored channels (four channels: packed RGBA32F at the logical extent; any
    // other count: one R32_SFLOAT plane per channel at W×(H*C), plane c of
    // logical pixel (x, y) at (x, y + c*H)). A source whose real format or extent
    // contradicts the layout is refused with GpuException rather than copied
    // through a wrong stride. Throws the allocator's own BudgetExceeded when the
    // staging buffer does not fit, and GpuException(SubmissionTimeout) when the
    // transfer does not complete in `timeout_ns` (the submission retains its
    // resources and remains the queue's responsibility).
    [[nodiscard]] StagedExport stage(const Image& source, const ImageLayout& layout, std::uint64_t timeout_ns);

    // Largest staging charge observed by this object: the peak per-frame export
    // staging bytes, which is the bounded working set one delivery job required.
    [[nodiscard]] std::uint64_t peakStagingBytes() const { return peakStagingBytes_; }

private:
    Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    std::uint64_t peakStagingBytes_ = 0;
};

}  // namespace nemo::gpu
