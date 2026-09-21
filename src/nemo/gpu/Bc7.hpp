#pragma once

// GPU-side BC7 frame encoding and compressed upload for the viewer cache
// (issue #106). This is one concrete production encoder, not a codec
// framework: the cache representation is fixed (BC7 RGBA, 4x4 blocks of 16
// bytes) and every method here serves exactly that representation.
//
// Representation contract:
// - A `Bc7Image` is a sampleable VK_FORMAT_BC7_UNORM_BLOCK texture holding one
//   display-referred frame, usable after its submission completes. Its extent IS the requested logical
//   raster: odd dimensions stay odd. BC7 block padding is internal to the
//   format and is never a logical extent, so a partial edge block can never be
//   presented as extra texels. Blocks hold already display-encoded UNORM
//   samples — sampling them is linear UNORM sampling, never an sRGB decode —
//   and the viewing transform has already been applied before encoding.
// - A `Bc7Image` is a display approximation for playback only. It is never a
//   scene-linear input, effect input, picker source, bake or export source, and
//   nothing in this module converts it back to float pixels.
//
// Lifetime: Device, Allocator and Instance outlive every encoder and every
// retained token. `encode`/`upload` never wait for the GPU; the returned
// `completion` identity owns every resource the submission references, and the
// caller observes it through the shared SubmissionQueue of the graphics family
// (`Device::submissions(...).poll/wait`).
//
// Threading: one encoder may be used from several worker threads. Both methods
// only read immutable encoder state and go through the thread-safe allocator,
// compute-pass and submission machinery.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::gpu {

// One display-referred BC7 frame: a sampleable BC7 texture of the requested
// logical extent, retained by the submission until its completion is observed.
struct Bc7Image {
    Image image;
};

// Payload size of a BC7 raster: ceil(width/4) * ceil(height/4) * 16 bytes.
// Checked arithmetic — a zero raster or an overflowing size is refused rather
// than wrapped. This is the size of the compressed-block buffer, not a VRAM
// total: allocation accounting uses the allocator's own reported charges.
[[nodiscard]] std::size_t bc7PayloadBytes(std::uint32_t width, std::uint32_t height);

// A submitted BC7 frame.
// - `image` is the completion-owned BC7 texture. Its extent is the requested
//   logical raster.
// - `readback` is the persistently mapped compressed-block buffer of exactly
//   `bc7PayloadBytes(width, height)` bytes after `encode`; it is empty for
//   `upload`. It never holds float or reconstructed pixels.
// - `completion` is the shared-queue identity that keeps every referenced
//   resource alive until its fence signals.
struct Bc7Submission {
    std::shared_ptr<const Bc7Image> image;
    Buffer readback;
    SubmissionQueue::Completion completion = 0;
};

// Production Vulkan compute BC7 encoder (mode 6: one subset, RGBAP 7.7.7.7.1
// endpoints, sixteen 4-bit indices — the finest index precision BC7 offers
// while carrying alpha in the same endpoint pair). It validates the actual
// device before doing any work and refuses to run without real support.
class Bc7Encoder {
public:
    // Validates that this device can sample and upload VK_FORMAT_BC7_UNORM_BLOCK
    // and can read storage images without a declared format, then loads the
    // compiled kernel from `shaderDirectory` (bc7Encode.spv).
    // Throws GpuException(InvalidRequest) with an identifying message when the
    // device cannot carry the cache representation or the kernel is missing —
    // the caller reports cache unavailability and live rendering continues.
    Bc7Encoder(Device& device, Allocator& allocator, const std::filesystem::path& shaderDirectory);
    ~Bc7Encoder();
    Bc7Encoder(const Bc7Encoder&) = delete;
    Bc7Encoder& operator=(const Bc7Encoder&) = delete;

    // Encodes a completed display-referred 2D RGBA32F image into a BC7 texture
    // and returns the completion-owned submission, whose `readback` holds the
    // compressed blocks for persistence. The source image is retained through
    // GPU completion. std::nullopt means the shared submission queue could not
    // admit the work (bounded capacity) — never a silent alternative encoding.
    // Throws GpuException(InvalidRequest) for a source that is not a 2D
    // RGBA32F image or a raster whose payload size does not check out.
    [[nodiscard]] std::optional<Bc7Submission> encode(const Image& displayRgba32f);

    // Uploads already-compressed blocks (`bc7PayloadBytes(width, height)`
    // bytes, exactly the bytes `encode` reads back) into a BC7 texture of the
    // given logical raster. The returned submission has an empty `readback`.
    // Throws GpuException(InvalidRequest) for a mismatched block count.
    [[nodiscard]] std::optional<Bc7Submission> upload(std::uint32_t width, std::uint32_t height,
                                                      std::span<const std::uint8_t> blocks);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::gpu
