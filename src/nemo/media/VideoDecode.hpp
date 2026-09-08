#pragma once

// Hardware video decode with Vulkan interop (issue #10, spec section 10.4).
//
// Demuxes and decodes a clip through FFmpeg and delivers the application
// image contract (RGBA float32, scene-linear, straight alpha — the same
// contract as ImageIO). The decode path is capability-measured, never
// assumed:
//
//   * When the application device reserved a Vulkan video decode queue
//     (Device::decode_family) and libavcodec ships the Vulkan hwaccel, the
//     clip is decoded ON the application device into Vulkan-resident NV12
//     planes and converted to the contract by a device-resident kernel
//     (MediaInterop + mediaConvert.slang) — no CPU readback anywhere
//     between decode and contract.
//   * Otherwise the decoder uses the software path and states exactly why
//     in `decision().reason` (no silent substitution — the frame records
//     report the path per frame).
//
// Pixel reads are a declared diagnostic (readBackLast), mirroring the
// effect executor's readBack contract (spec section 10.4 "no routine
// readback"): production consumption of decoded frames is the returned
// device-resident Image.
//
// All AV*/FFmpeg types stay in the .cpp — the public surface is the
// application image contract only (Media module boundary rule).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/MediaInterop.hpp"

namespace nemo::media {

struct ClipInfo {
    std::string path;
    std::string codecName;  // e.g. "h264"
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    int64_t frameCount = -1;  // -1 when the container does not declare it
};

// How the clip is being decoded, with the measured reason when the
// hardware path is not used.
struct DecodeDecision {
    bool hardware = false;
    std::string reason;  // empty exactly when hardware == true
};

class ClipDecoder {
public:
    // Opens `path` and chooses the decode path. `convertSpirv` is the
    // compiled mediaConvert kernel (Vulkan-resident path). Throws a
    // MediaIoException-style error naming the path when the clip cannot be
    // opened; unsupported hardware is NOT an error — it downgrades to the
    // measured software path with the reason recorded.
    static std::unique_ptr<ClipDecoder> open(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                             const std::string& path, const std::filesystem::path& convertSpirv);
    ~ClipDecoder();
    ClipDecoder(const ClipDecoder&) = delete;
    ClipDecoder& operator=(const ClipDecoder&) = delete;

    [[nodiscard]] const ClipInfo& info() const;
    [[nodiscard]] const DecodeDecision& decision() const;

    // Decodes the next frame and returns its device-resident rgba32f
    // image (or nullptr at end of stream). The returned Image stays valid
    // until the next next() call or decoder destruction.
    [[nodiscard]] std::unique_ptr<gpu::Image> next(uint64_t timeout_ns);

    // Diagnostic readback ONLY of the frame last returned by next().
    [[nodiscard]] CpuImage readBackLast(uint64_t timeout_ns);

private:
    ClipDecoder() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Software decode reference/fallback: decodes one clip fully (software
// decoder + swscale conversion) into the application contract. Used by
// tests as the correctness oracle for the hardware path and by consumers
// on devices without Vulkan video queues; the caller chooses it explicitly
// (it is never selected silently).
struct SoftwareClip {
    ClipInfo info;
    std::vector<CpuImage> frames;
};

[[nodiscard]] SoftwareClip decodeClipSoftware(const std::string& path, int64_t maxFrames = -1);

}  // namespace nemo::media
