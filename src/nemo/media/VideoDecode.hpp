#pragma once

// Hardware video decode with Vulkan interop (issues #10/#21, spec 10.4).
//
// Two distinct color contracts, never conflated:
//
//   * Source decode (`ClipDecoder::next`, `decodeClipSoftware`) interprets
//     the source and produces scene-linear working images in the project
//     ColorPolicy's working space. The Y′CbCr → matrix/range step yields
//     NONLINEAR R′G′B′ only; the declared transfer is inverted afterwards
//     (matrix conversion is not linearization). Interpretation honors
//     transfer, primaries, matrix, range, chroma location and bit depth
//     from the stream's declared color metadata; ambiguous metadata is an
//     error (or an explicit `ColorOverride`), never a silent guess.
//   * Viewer-cache replay (`decodeViewerChunkSoftware`) decodes the baked
//     display-referred representation without re-applying any source
//     linearization or view transform; the chunk's interpretation metadata
//     round-trips through `SoftwareClip::metadata`.
//
// Format validation happens on the ACTUAL decoded frame (pixel format,
// dimensions, planes, bit depth) before any plane access; unsupported
// formats fail with a MediaDecodeError naming clip/format/reason.
//
//   * When the application device reserved a Vulkan video decode queue
//     (Device::decode_family) and libavcodec ships the Vulkan hwaccel AND
//     the stream decodes into the 8-bit 4:2:0 surfaces the interop
//     converter consumes, the clip is decoded ON the application device
//     and converted by a device-resident kernel (MediaInterop +
//     mediaConvert.slang) — no CPU readback anywhere between decode and
//     contract. Any other combination is a measured software path whose
//     `decision().reason` states exactly why (no silent substitution).
//
// Production consumption of decoded frames is the returned device-resident
// Image; there is no routine readback.
//
// All AV*/FFmpeg types stay in the .cpp — the public surface is the
// application image contract only (Media module boundary rule).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/MediaInterop.hpp"

namespace nemo::media {

// Decode failures name the clip, the offending format, and the reason
// (repo rule: errors identify the offending relationship).
struct MediaDecodeError : std::runtime_error {
    MediaDecodeError(std::string clip, std::string format, std::string reason)
        : std::runtime_error("media decode: " + clip + ": " + format + ": " + reason), clip(std::move(clip)),
          format(std::move(format)), reason(std::move(reason)) {}

    std::string clip;
    std::string format;
    std::string reason;
};

struct ClipInfo {
    std::string path;
    std::string codecName;  // e.g. "h264"
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    int64_t frameCount = -1;   // -1 when the container does not declare it
    double pixelAspect = 1.0;  // Display width / height of one source pixel.
};

// How the clip is being decoded, with the measured reason when the
// hardware path is not used.
struct DecodeDecision {
    bool hardware = false;
    std::string reason;  // empty exactly when hardware == true
};

// The color interpretation resolved for a clip: what its samples mean and
// (on the source path) how they were linearized into the working space.
struct MediaColorMetadata {
    gpu::MediaTransfer transfer = gpu::MediaTransfer::Bt709;
    gpu::MediaPrimaries primaries = gpu::MediaPrimaries::Bt709;
    gpu::MediaMatrix matrix = gpu::MediaMatrix::Bt709;
    gpu::MediaYuvRange range = gpu::MediaYuvRange::Limited;
    gpu::MediaChromaLocation chromaLocation = gpu::MediaChromaLocation::Left;
    int bitDepth = 8;
    [[nodiscard]] bool operator==(const MediaColorMetadata&) const = default;
};

// Explicit resolution for stream color metadata that is UNSPECIFIED. An
// override only fills a missing field; a field the stream tags keeps its
// declared value. Passing an override for an already-tagged field is
// ignored (the stream is authoritative).
struct ColorOverride {
    std::optional<gpu::MediaTransfer> transfer;
    std::optional<gpu::MediaPrimaries> primaries;
    std::optional<gpu::MediaMatrix> matrix;
    std::optional<gpu::MediaYuvRange> range;
    std::optional<gpu::MediaChromaLocation> chromaLocation;
};

// Thread-confined: calls on a decoder must not overlap.
class ClipDecoder {
public:
    // Opens `path` and chooses the decode path. `convertSpirv` is the
    // compiled mediaConvert kernel (Vulkan-resident path). `policy` names
    // the working space the source is linearized into; `overrides` resolve
    // unspecified stream color metadata. Throws MediaDecodeError naming
    // the clip when it cannot be opened, or when the color interpretation
    // is not in the supported subset; unsupported hardware is NOT an
    // error — it downgrades to the measured software path with the reason
    // recorded.
    static std::unique_ptr<ClipDecoder> open(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                             const std::string& path, const std::filesystem::path& convertSpirv,
                                             const ColorPolicy& policy = {}, const ColorOverride& overrides = {});
    ~ClipDecoder();
    ClipDecoder(const ClipDecoder&) = delete;
    ClipDecoder& operator=(const ClipDecoder&) = delete;

    [[nodiscard]] const ClipInfo& info() const;
    // Before the first next(): selected candidate. After next(): actual
    // produced frame path, including any FFmpeg hardware fallback.
    [[nodiscard]] const DecodeDecision& decision() const;

    // Decodes the next frame and returns its device-resident rgba32f
    // image, scene-linear in the declared working space. The caller owns
    // the returned image exclusively (unique_ptr) for as long as it
    // wishes; frames are independent device allocations and the decoder
    // never touches the image again. nullptr at end of stream.
    // The allocator, device, and instance must outlive every returned image.
    [[nodiscard]] std::unique_ptr<gpu::Image> next(uint64_t timeout_ns);

private:
    ClipDecoder() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Software decoded storage. Source frames are SceneLinear; viewer replay
// frames are DisplayReferred. metadata describes the encoded samples.
struct SoftwareClip {
    ClipInfo info;
    std::vector<CpuImage> frames;
    MediaColorMetadata metadata;
};

// Source path: converts the clip to scene-linear working images per the
// project ColorPolicy (default "linear" = scene-linear Rec.709).
// Supported sources: Rec.709 primaries; BT.709/601 matrices; limited/full
// range; BT.709, sRGB, gamma 2.2/2.8, or linear transfers. Formats are 8-bit
// planar 420/444 or NV12, 10-bit little-endian planar 420/444, and 8/10/16-bit
// grayscale. Subsampled chroma must be left-sited. Other interpretations,
// working spaces, and changes of interpretation within a software clip
// fail explicitly rather than silently relabeling samples.
[[nodiscard]] SoftwareClip decodeClipSoftware(const std::string& path, int64_t maxFrames = -1,
                                              const ColorPolicy& policy = {}, const ColorOverride& overrides = {});

// Viewer-cache replay path: decodes an encoded viewer chunk WITHOUT the
// source linearization — the frames stay the baked display-referred
// R′G′B′ the encoder wrote, and `metadata` carries the interpretation read
// back from the chunk's declared color tags (all must be specified).
[[nodiscard]] SoftwareClip decodeViewerChunkSoftware(const std::string& path, int64_t maxFrames = -1);

}  // namespace nemo::media
