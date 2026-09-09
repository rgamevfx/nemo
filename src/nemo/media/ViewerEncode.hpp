#pragma once

// Hardware encode path for the viewer-cache representation (issue #10,
// spec sections 8/10.4, ADR-0004).
//
// The compact viewer representation is display-referred Rec.709 4:2:0
// (8-bit YUV420p, limited range). Encoders run behind this Media API:
// NVENC (h264-nvenc, hevc-nvenc) when the engine is available, CPU
// encoders (libx264, libx265) as declared comparators. Device input first
// uses the GPU-owned ViewerEncodeInterop adapter's Vulkan-to-CUDA
// external-memory path; a capability-proven failure uses compact measured
// staging rather than silently substituting another codec or calling a
// universal CpuImage readback. Media retains codec selection/session/mux
// ownership; the adapter retains native GPU synchronization and retirement.
//
// Statistics separate host preparation, transfer, codec work and muxing;
// completeChunkMs includes setup through finalized, closed readable output.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"

namespace nemo::media {

struct MediaCodecError : std::runtime_error {
    MediaCodecError(std::string codecName, std::string detail)
        : std::runtime_error("media encode: " + codecName + ": " + detail), codec(std::move(codecName)),
          message(std::move(detail)) {}
    std::string codec;
    std::string message;
};

// Deterministic, opt-in failure injection (issue #21): names the encode
// stage that must fail and the 1-based occurrence of that stage's action.
// Carried in EncodeOptions — scoped to one encode call, no global mutable
// hooks — so tests drive it through the public encode API. An injected
// failure throws MediaCodecError with a message naming the stage and codec;
// it never returns partial statistics and leaves no partial output file.
struct EncodeFailure {
    enum class Stage {
        Allocation,    // context / hw-device / frame-pool allocation phase
        Init,          // encoder open (after hw pool init on the hw path)
        Submission,    // frame submission (counted per frame)
        Write,         // packet write to the muxer (counted per packet)
        Finalization,  // after the final drain, before the trailer write
    };
    Stage stage = Stage::Allocation;
    int occurrence = 1;  // 1-based: the Nth action of the stage fails
};

struct EncodeOptions {
    // Encoder id from probeMediaCapabilities (e.g. "h264-nvenc").
    std::string codec;
    int gopSize = 24;  // intra period; chunk-sized GOPs for seeks
    int bitrateKbps = 2000;
    // When non-null, the named stage fails deterministically on the Nth
    // action (issue #21 robustness tests). Null = never injected.
    const EncodeFailure* injectedFailure = nullptr;
    std::string profile;  // empty resolves to H.264 high / HEVC main
    int bitDepth = 8;     // unsupported precision is rejected, never substituted
};

struct EncodeStats {
    std::string codec;
    std::string profile;
    double initializationMs = 0.0;     // validation, codec/device/pool setup
    double allocationPackingMs = 0.0;  // host/device frame allocation and host packing
    double conversionMs = 0.0;         // CPU RGB -> YUV only
    double gpuConversionMs = 0.0;      // device RGBA -> Rec.709 4:2:0 conversion
    double hostToDeviceMs = 0.0;       // transfer API submission latency, NOT isolated DMA completion
    uint64_t hostToDeviceBytes = 0;    // CUDA copy extents including copied row padding
    double deviceToDeviceMs = 0.0;     // Vulkan YUV -> CUDA surface transfer
    uint64_t deviceToDeviceBytes = 0;  // bytes moved by Vulkan/CUDA interop
    double deviceToHostMs = 0.0;       // compact GPU-YUV -> host staging readback
    uint64_t deviceToHostBytes = 0;    // bytes moved device -> host
    uint64_t stagingBytes = 0;         // peak owned staging payload; excludes codec internals and input images
    double submissionDrainMs = 0.0;    // codec send/receive; may include upload dependency waits, excludes mux
    double muxFinalizationMs = 0.0;    // container setup, packets, trailer and close
    double completeChunkMs = 0.0;      // enclosing wall time, not the sum of stages
    double coldSetupMs = 0.0;          // setup cost when a session/context was opened
    double warmSetupMs = 0.0;          // preparation cost when a session was reused
    uint64_t sessionChunkCount = 0;
    uint64_t sessionReuseCount = 0;
    int64_t encodedBytes = 0;
    int encodedFrames = 0;
    bool sessionReused = false;
    // Empty for direct Vulkan->CUDA device input. Device fallback records the
    // exact capability/transfer failure and never labels missing interop as
    // unsupported hardware.
    std::string fallbackReason;
};

// A completed display-referred RGBA32F image produced by the native executor.
// The caller retains the image until encode() returns; no routine full-frame
// CpuImage readback is part of this interface.
struct DeviceViewerFrame {
    const gpu::Image* image = nullptr;
    ImageLayout layout;
};

// Encodes independently decodable viewer chunks from completed device images.
// Same-size zero-delay codec sessions persist without terminal EOF between
// chunks. Every requested packet must be available before mux finalization;
// each chunk starts at an IDR, including one-frame and sparse chunks.
// Thread-confined; the constructor snapshots options and failure controls.
// Device images with odd extents are edge-padded to even codec storage;
// consumers retain the requested layout and crop that padding on replay.
class ViewerChunkEncoder {
public:
    ViewerChunkEncoder(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                       const EncodeOptions& options);
    ~ViewerChunkEncoder();
    ViewerChunkEncoder(const ViewerChunkEncoder&) = delete;
    ViewerChunkEncoder& operator=(const ViewerChunkEncoder&) = delete;

    [[nodiscard]] EncodeStats encode(const std::string& outputPath,
                                     std::span<const DeviceViewerFrame> displayReferredFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Encodes the given CPU frames as one independently decodable chunk (one GOP,
// intra-only start) to `outputPath` (mp4). Input is display-referred RGBA
// float32 with DisplayReferred layout — the baked BT.709 viewer output after
// its viewing transform; scene-linear input is rejected.
[[nodiscard]] EncodeStats encodeViewerChunk(const std::string& outputPath,
                                            std::span<const CpuImage> displayReferredFrames,
                                            const EncodeOptions& options);

}  // namespace nemo::media
