#pragma once

// Hardware video decode with Vulkan interop (issues #10/#21, spec 10.4).
//
// Two distinct color contracts, never conflated:
//
//   * Source decode (`ClipDecoder::next`, `decodeClipSoftware`) interprets
//     the source and produces scene-linear working images in the project's
//     working space (or Data for a Raw bypass, issue #81). The Y′CbCr →
//     matrix/range step yields NONLINEAR R′G′B′ only and is mandatory codec
//     layout work; the resolved RGB input color — a named OCIO input space,
//     the declared transfer, or no conversion at all — is applied afterwards
//     exactly once (matrix conversion is not linearization). Decode
//     interpretation honors matrix, range, chroma location and bit depth from
//     the stream's declared metadata with the fill-only interpretation hints;
//     ambiguous metadata is an error, never a silent guess. A `ClipColorInput`
//     with a project input-color context also lets the config's own file rule
//     resolve an otherwise undeclared RGB interpretation.
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
// Image; there is no routine readback. Every decoded frame — whichever path
// produced it — is stored in the shared native layout of its four channels
// (issue #98): a packed RGBA32F image of extent (logical width, logical
// height) whose texel (x, y) holds the frame's R, G, B and A, left in GENERAL.
// The four components are one packed vector per pixel, exactly as for any
// other four-channel native image.
//
// All AV*/FFmpeg types stay in the .cpp — the public surface is the
// application image contract only (Media module boundary rule).

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
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
#include "nemo/media/InputColor.hpp"

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

// Provenance of a clip's frame count. A container-declared count is
// `Reliable`; a value derived from duration and rate is `Estimated` and must
// never be promoted to authoritative coverage; `Unknown` means the container
// declared nothing (the count stays -1 and no bounds may be invented from it).
enum class FrameCountQuality { Unknown, Estimated, Reliable };

struct ClipInfo {
    std::string path;
    std::string codecName;  // e.g. "h264"
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    int64_t frameCount = -1;   // -1 when the container does not declare it
    double pixelAspect = 1.0;  // Display width / height; unspecified clip PAR defaults to square.
    FrameCountQuality frameCountQuality{FrameCountQuality::Unknown};
};

// Container/stream description without opening a decoder, selecting a device,
// or producing pixels. Pixel-format support is checked only during execution.
[[nodiscard]] ClipInfo inspectClipHeader(const std::string& path);

// Uploads one decoded interleaved raster into the shared native image layout
// (issue #98). Four stored channels occupy the packed RGBA32F image of exactly
// `raster.width()` x `raster.height()`: the interleaved samples are copied
// contiguously, so the upload is one mapped staging fill and the byte count is
// the raster's own. Any other channel count keeps the scalar-plane layout: one
// R32_SFLOAT 2D image of extent (width, channelCount * height), where channel c
// of logical pixel (x, y) lives at (x, y + c*height), transposed into the same
// single staging buffer. Channel names and their order are never rewritten:
// the stored order is the raster's own. The image is left in GENERAL, the
// layout every decoded-frame consumer binds. `image` must already be the
// raster's native layout — `nativeChannelFormat`/`nativeChannelHeight` of its
// actual channel count — and a mismatch is refused rather than uploaded
// through a wrong stride. Synchronous; throws GpuException.
void uploadNativeImage(gpu::SubmissionQueue& queue, gpu::Allocator& allocator, const gpu::Image& image,
                       const CpuImage& raster, uint64_t timeout_ns);

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

// Encoded-RGB interpretation of a clip's frames (issue #81). A null `cache`
// keeps the previous declared-transfer behavior (the source-scoped path the
// media import worker and viewer replay use). A cache resolves the authored
// choices and the fill-only hints through the shared media resolver
// (InputColor.hpp) and converts the frames into the cache's working space: a
// named OCIO input color space, the declared metadata transfer, or a Raw/Data
// bypass. Y'CbCr matrix/range/chroma decoding is mandatory codec layout work
// and is never provided by the RGB input color space.
struct ClipColorInput {
    InputColorChoice choice;
    // Shared owner: a decoder holds its cache for as long as it can produce
    // frames, so a refresh never invalidates a decode in flight.
    std::shared_ptr<const InputColorCache> cache;
};

// The clip's resolved color: the decode necessities plus the resolved
// encoded-RGB interpretation the frames are converted by.
struct ClipColorResolution {
    MediaColorMetadata decode;
    ResolvedInputColor rgb;
};

// Thread-confined: calls on a decoder must not overlap.
class ClipDecoder {
public:
    // Opens `path` and chooses the decode path. `convertSpirv` is the
    // compiled mediaConvert kernel (Vulkan-resident path). `policy` names
    // the working space the source is linearized into; `overrides` resolve
    // unspecified stream color metadata; `color` supplies the project's
    // retained input-color context for a source decode. Throws
    // MediaDecodeError naming the clip when it cannot be opened, or when the
    // color interpretation is not in the supported subset; unsupported
    // hardware is NOT an error — it downgrades to the measured software path
    // with the reason recorded.
    static std::unique_ptr<ClipDecoder> open(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                                             const std::string& path, const std::filesystem::path& convertSpirv,
                                             const ColorPolicy& policy = {}, const ColorOverride& overrides = {},
                                             const ClipColorInput& color = {});

    // Opens a viewer-cache chunk in display-referred mode. This mode keeps
    // the encoded transfer untouched and never applies source linearization
    // or a viewing transform. Returned frames remain device-resident.
    static std::unique_ptr<ClipDecoder> openViewer(gpu::Instance& instance, gpu::Device& device,
                                                   gpu::Allocator& allocator, const std::string& path,
                                                   const std::filesystem::path& convertSpirv);

    // Opens a viewer-cache chunk from bounded caller-owned compressed bytes.
    // This entry point is viewer-only; source decode has no memory replay API.
    static std::unique_ptr<ClipDecoder> openViewerMemory(gpu::Instance& instance, gpu::Device& device,
                                                         gpu::Allocator& allocator, const std::string& name,
                                                         std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                                                         const std::filesystem::path& convertSpirv);
    ~ClipDecoder();
    ClipDecoder(const ClipDecoder&) = delete;
    ClipDecoder& operator=(const ClipDecoder&) = delete;

    [[nodiscard]] const ClipInfo& info() const;
    // The resolved encoded-RGB interpretation this decoder converts frames by
    // (issue #81): its kind tells a consumer whether the produced frames are
    // working-space scene-linear or Raw/Data.
    [[nodiscard]] const ResolvedInputColor& inputColor() const;
    // Before the first next(): selected candidate. After next(): actual
    // produced frame path, including any FFmpeg hardware fallback.
    [[nodiscard]] const DecodeDecision& decision() const;

    // Decodes the next frame and returns its device-resident image in the
    // shared native layout: the packed RGBA32F image of extent (logical width,
    // logical height) holding the frame's R, G, B and A components, left in
    // GENERAL. Source mode is scene-linear; openViewer() mode is
    // display-referred.
    [[nodiscard]] std::unique_ptr<gpu::Image> next(uint64_t timeout_ns);
    [[nodiscard]] std::unique_ptr<gpu::Image> nextViewer(uint64_t timeout_ns);

private:
    static std::unique_ptr<ClipDecoder> openInternal(gpu::Instance& instance, gpu::Device& device,
                                                     gpu::Allocator& allocator, const std::string& path,
                                                     const std::filesystem::path& convertSpirv,
                                                     const ColorPolicy& policy, const ColorOverride& overrides,
                                                     const ClipColorInput& color, bool viewerReplay,
                                                     std::shared_ptr<const std::vector<std::uint8_t>> memoryBytes);
    ClipDecoder() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Software decoded storage. Source frames are SceneLinear (or Data for a Raw
// bypass); viewer replay frames are DisplayReferred. metadata describes the
// encoded samples.
struct SoftwareClip {
    ClipInfo info;
    std::vector<CpuImage> frames;
    MediaColorMetadata metadata;
    // The resolved encoded-RGB interpretation the frames were converted by
    // (kind, space, origin, alpha), so a consumer reports the real choice
    // instead of re-deriving it.
    ResolvedInputColor inputColor;
    // Actual decoded frame pixel format (e.g. "yuv420p"), validated from the
    // frame itself before plane access. Empty when no frame was produced.
    std::string pixelFormat;
    // Selected video stream index in the container.
    int streamIndex{-1};
    // Declared codec profile ("profile <name>"); empty when the stream
    // declares none.
    std::string profile;
    // Plane count of the validated decoded format (1 gray, 2 NV12, 3
    // planar). Zero before any frame was produced.
    int planeCount{0};
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
                                              const ColorPolicy& policy = {}, const ColorOverride& overrides = {},
                                              const ClipColorInput& color = {});

// Bounded source-frame read for import/probe/preview (issue #43): decodes
// forward from the container start to `frameIndex` (0-based), retaining at
// most that one frame. `width`/`height` are zero (preserve native size) or
// MAXIMUM preview bounds; with non-zero bounds the returned frame is fitted
// to the decoded frame's DISPLAYED shape (its width/height widened by the
// frame's sample aspect ratio) and emitted as a square-pixel raster, while
// `info` keeps the native geometry. Same source-linear contract,
// ColorPolicy/ColorOverride resolution, and actual-decoded-frame format
// validation as decodeClipSoftware; `metadata`, `pixelFormat`, `planeCount`
// and the stream facts describe the retained frame. The returned clip holds
// ZERO frames when the stream ends before `frameIndex` — the caller reports
// the missing frame rather than substituting another one. No full-clip
// accumulation: earlier frames are discarded as they decode.
[[nodiscard]] SoftwareClip decodeClipFrameSoftware(const std::string& path, int64_t frameIndex, int width = 0,
                                                   int height = 0, const ColorPolicy& policy = {},
                                                   const ColorOverride& overrides = {},
                                                   const ClipColorInput& color = {});

// Resolve a Document source's interpretation map into the decoder's explicit
// ColorOverride vocabulary. `context` names the offending relationship
// ("source 'shot.mov'"). Unknown fields and unsupported values throw
// std::invalid_argument naming the field, the value, and the supported set —
// never silently ignored or guessed. Shared by the runtime source session
// and the import service so the interpretation vocabulary has one owner.
[[nodiscard]] ColorOverride colorOverrideFromInterpretation(const std::map<std::string, std::string>& interpretation,
                                                            const std::string& context);

// Viewer-cache replay path: decodes an encoded viewer chunk WITHOUT the
// source linearization — the frames stay the baked display-referred
// R′G′B′ the encoder wrote, and `metadata` carries the interpretation read
// back from the chunk's declared color tags (all must be specified).
[[nodiscard]] SoftwareClip decodeViewerChunkSoftware(const std::string& path, int64_t maxFrames = -1);

// Thread-confined incremental display-referred reference/replay decoder.
// Zero dimensions preserve native size; otherwise sample source luma pixel
// centers by nearest neighbor, reconstruct left-sited chroma bilinearly at
// that source coordinate, then expand limited-range BT.709 to float RGB.
// No source-sized float image, linearization or view transform is produced.
// Owns codec surfaces plus one decoded YUV frame; next() transfers ownership
// of one output image to the caller. EOF and corrupt input are distinct.
class ViewerReferenceDecoder {
public:
    explicit ViewerReferenceDecoder(const std::string& path, int width = 0, int height = 0);
    ~ViewerReferenceDecoder();
    ViewerReferenceDecoder(const ViewerReferenceDecoder&) = delete;
    ViewerReferenceDecoder& operator=(const ViewerReferenceDecoder&) = delete;
    [[nodiscard]] const ClipInfo& info() const;
    // The resolved encoded-RGB interpretation this decoder converts frames by
    // (issue #81): its kind tells a consumer whether the produced frames are
    // working-space scene-linear or Raw/Data.
    [[nodiscard]] const ResolvedInputColor& inputColor() const;
    [[nodiscard]] std::optional<CpuImage> next();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::media
