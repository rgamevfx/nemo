#pragma once

// Device-viewer YUV conversion and Vulkan/CUDA transfer (issue #12).
//
// This adapter is the GPU ownership boundary for viewer encoding. It owns
// compute conversion, Vulkan copies and queue synchronization, and retains
// every cross-API frame until the associated work and CUDA stream have
// drained. Media supplies only the completed RGBA32F image and, for direct
// hardware input, an already selected CUDA frame context plus destination
// AVFrame. No codec context or codec policy crosses this interface.
//
// Threading/lifetime contract: one adapter instance is thread-confined and
// calls are serialized by its owner. Instance, Device and Allocator must
// outlive the adapter. `convertToCuda` is synchronous with respect to the
// submitted Vulkan work, but retained Vulkan/CUDA frame owners remain live
// until `finishChunk`, `abortChunk`, or adapter destruction. Destruction
// drains both Nemo queue submissions and FFmpeg's CUDA transfer stream before
// releasing Vulkan interop contexts; the CUDA frame context must therefore
// remain valid until that drain completes.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"

// FFmpeg types are deliberately opaque here. The optional implementation
// target includes libavutil/libavcodec; the base nemo_gpu target does not
// depend on FFmpeg or its codec policy.
struct AVBufferRef;
struct AVFrame;

namespace nemo::gpu {

class ViewerEncodeError : public std::runtime_error {
public:
    explicit ViewerEncodeError(const std::string& message) : std::runtime_error(message) {}
};

// GPU-side timing and transfer accounting. Media translates this plain data
// into its public EncodeStats without exposing media types to the GPU module.
struct ViewerEncodeStats {
    double allocationPackingMs = 0.0;
    double gpuConversionMs = 0.0;
    double hostToDeviceMs = 0.0;
    std::uint64_t hostToDeviceBytes = 0;
    double deviceToDeviceMs = 0.0;
    std::uint64_t deviceToDeviceBytes = 0;
    double deviceToHostMs = 0.0;
    std::uint64_t deviceToHostBytes = 0;
    std::uint64_t stagingBytes = 0;
};

class ViewerEncodeInterop {
public:
    ViewerEncodeInterop(Instance& instance, Device& device, Allocator& allocator);
    ~ViewerEncodeInterop();

    ViewerEncodeInterop(const ViewerEncodeInterop&) = delete;
    ViewerEncodeInterop& operator=(const ViewerEncodeInterop&) = delete;

    // Builds the conversion pipeline before a chunk's setup timing boundary.
    // Calling it more than once is harmless and does not submit work.
    void prepare();

    // Capability-probes the Vulkan export frame pool used by direct CUDA
    // input. False is a measured capability failure and fills `reason`; other
    // failures throw ViewerEncodeError. Width and height are the even,
    // edge-padded codec dimensions.
    [[nodiscard]] bool ensureDirectInterop(int width, int height, std::string& reason);

    // Converts a completed display-referred RGBA32F image to packed 8-bit
    // Rec.709 limited-range YUV420P, edge-padding odd source extents to the
    // supplied even encoded dimensions. This path synchronously drains the
    // conversion/readback submissions before returning.
    void convertToHost(const Image& image, int sourceWidth, int sourceHeight, int encodedWidth, int encodedHeight,
                       std::vector<std::uint8_t>& planes, ViewerEncodeStats& stats);

    // Uploads compact host YUV420P staging to the supplied CUDA destination
    // frame. The host frame is temporary and the adapter owns transfer timing
    // and byte accounting; `cudaFrames` is a hardware-frame context.
    void uploadHostToCuda(const std::vector<std::uint8_t>& planes, int width, int height, AVBufferRef* cudaFrames,
                          AVFrame* destination, ViewerEncodeStats& stats);

    // Converts the image and copies its GPU YUV planes into the supplied
    // FFmpeg CUDA destination frame. `cudaFrames` is a hardware-frame context,
    // not an AVCodecContext; `destination` is allocated and owned by the
    // caller. The adapter retains internal source/destination references
    // until finishChunk/abortChunk/destruction.
    void convertToCuda(const Image& image, int sourceWidth, int sourceHeight, int encodedWidth, int encodedHeight,
                       AVBufferRef* cudaFrames, AVFrame* destination, ViewerEncodeStats& stats);

    // Releases successful chunk-retained plane/frame references after the
    // caller has finalized encoding.
    void finishChunk();

    // Failure-only cleanup. If work cannot drain yet, the adapter retains
    // its owners and remains quarantined until destruction can drain it.
    void abortChunk() noexcept;
    [[nodiscard]] bool quarantined() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::gpu
