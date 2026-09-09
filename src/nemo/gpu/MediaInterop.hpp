#pragma once

// Foreign video-frame interop (issues #10/#21, spec section 10.4).
//
// Vulkan video decode (through the Media module's FFmpeg hwaccel, backed by
// the NVDEC engine) produces NV12 plane images and timeline semaphores on
// the application device. This surface is the GPU-module ownership boundary
// for consuming them: layout transitions (decode-dst ↔ shader-read), queue
// family ownership (exclusive-sharing frames move decode→graphics→decode),
// the cross-queue timeline waits/signals, and the dispatch of the
// mediaConvert kernel. All Vulkan execution and synchronization stays here;
// the Media module sees frame metadata and completion identities.
//
// Foreign images/semaphores are never destroyed here. The owner token must
// retain them and their decoder context until completion. Device and Instance
// outlive all work. Preparation is worker-side; admission never waits for GPU
// capacity. Frame metadata itself is caller-synchronized.
//
// Color interpretation travels as plain data on the frame: the media module
// resolves the clip's declared color metadata (transfer, primaries, matrix,
// range, chroma location) and records what the Y/Cb/Cr samples mean. The
// mediaConvert kernel converts Y′CbCr(range/matrix) → R′G′B′ and then
// linearizes by `transfer` — matrix conversion alone yields nonlinear
// R′G′B′, never scene-linear RGB.

#include <vulkan/vulkan.h>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::gpu {

// Plain-data color interpretation consumed by the mediaConvert kernel. The
// supported set is deliberately explicit: combinations outside it are
// rejected by the media module rather than silently guessed.

// Source transfer characteristic. The kernel inverts it into scene-linear
// Rec.709; `Linear` passes the decoded values through unlinearized.
enum class MediaTransfer { Bt709, Srgb, Gamma22, Gamma28, Linear };

// Y′CbCr → R′G′B′ matrix coefficients.
enum class MediaMatrix { Bt709, Bt601 };

// Luma/chroma quantization range.
enum class MediaYuvRange { Limited, Full };

// Chroma sample position. Only left (horizontally co-sited with even luma
// columns, vertically midway between luma rows) is supported.
enum class MediaChromaLocation { Left };

// Source chromaticities. Only Rec.709 is supported: the working space is
// scene-linear Rec.709, so no chromaticity mapping exists yet; other
// primaries are an explicit error, not an identity pass-through.
enum class MediaPrimaries { Bt709 };

// One external producer frame: up to two planes (NV12: R8 luma + R8G8
// chroma), each with its own timeline semaphore the producer signals at
// `waitValues` when the plane is readable.
struct ForeignVideoFrame {
    std::shared_ptr<const void> owner;
    VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFormat formats[2] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkSemaphore semaphores[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint64_t waitValues[2] = {};
    // Queue family currently owning each plane (VK_QUEUE_FAMILY_IGNORED for
    // concurrent-sharing frames).
    uint32_t queueFamilies[2] = {};
    // Layout each plane is in when handed over, and that it must be
    // restored to for the producer's next use.
    VkImageLayout layouts[2] = {};
    // Access the producer finished with (barrier source access).
    VkAccessFlags accesses[2] = {};
    VkPipelineStageFlags producerStages[2] = {};
    // Single multiplane image (ffmpeg default): planeCount==1 with
    // formats[0] = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM and views taken per
    // aspect plane. false = per-plane images (images[0]=Y, images[1]=UV).
    bool multiplane = false;
    MediaTransfer transfer = MediaTransfer::Bt709;
    MediaMatrix matrix = MediaMatrix::Bt709;
    MediaYuvRange range = MediaYuvRange::Limited;
    MediaChromaLocation chromaLocation = MediaChromaLocation::Left;
    MediaPrimaries primaries = MediaPrimaries::Bt709;
    uint32_t planeCount = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Decoder surfaces may omit SAMPLED/MUTABLE image capabilities because
    // Vulkan video format support only permits decode plus transfer-source
    // usage. When set, interop copies each plane into an application-owned
    // sampled image before dispatching mediaConvert; the copy remains in the
    // same GPU submission as conversion.
    bool copyBeforeSampling = false;
    // The metadata transfer remains the source's actual transfer. When true,
    // mediaConvert inverts it to scene-linear; when false, it passes through
    // the decoded display-referred R′G′B′ values without inversion.
    bool sourceLinearization = true;
};

// Uses the shared device execution queue and immutable compute pipeline cache.
// Per-dispatch descriptors, views, uniform and foreign owners are retained by
// the submitted completion, independently of this converter's lifetime.
class MediaInterop {
public:
    struct Token;
    explicit MediaInterop(Token);
    // `convertSpirv` is the compiled mediaConvert compute kernel.
    static std::unique_ptr<MediaInterop> create(Device& device, Allocator& allocator,
                                                const std::vector<std::uint32_t>& convertSpirv);
    ~MediaInterop();
    MediaInterop(const MediaInterop&) = delete;
    MediaInterop& operator=(const MediaInterop&) = delete;

    // Converts one foreign NV12 frame into `output` (rgba32f storage image,
    // GENERAL layout maintained) using the frame's declared color
    // interpretation. Synchronous. Establishes the producer→conversion
    // dependency with in-queue timeline waits, signals the producer
    // semaphores back at waitValue+1, and updates `frame` (waitValues,
    // queueFamilies, accesses) so the producer can reuse the planes.
    // Throws GpuException naming the failure.
    void convertToRgba32f(ForeignVideoFrame& frame, Image& output, uint64_t timeout_ns);
    // Asynchronous equivalent. Requires frame.owner. Returns nullopt on
    // capacity exhaustion without modifying producer state. On success,
    // producer waitValues advance immediately; producer reuse must wait on
    // those timeline values. Dropping the completion never frees live work.
    // A positive admission timeout opts synchronous workers into bounded
    // waiting for queue capacity, without waiting for GPU execution here.
    [[nodiscard]] std::optional<SubmissionQueue::Completion> submitToRgba32f(ForeignVideoFrame& frame, Image& output,
                                                                             uint64_t admissionTimeout_ns = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::gpu
