#pragma once

// Foreign video-frame interop (issue #10, spec section 10.4).
//
// Vulkan video decode (through the Media module's FFmpeg hwaccel, backed by
// the NVDEC engine) produces NV12 plane images and timeline semaphores on
// the application device. This surface is the GPU-module ownership boundary
// for consuming them: layout transitions (decode-dst ↔ shader-read), queue
// family ownership (exclusive-sharing frames move decode→graphics→decode),
// the cross-queue timeline waits/signals, and the dispatch of the
// mediaConvert kernel. All Vulkan execution and synchronization stays here;
// the Media module sees plain structs and one synchronous call.
//
// "Foreign" means the images/semaphores are created and destroyed by the
// external decoder library: this module borrows them for the duration of
// one submission and never destroys them.
//
// Lifetime/completion contract: convert() is synchronous (SubmissionQueue
// fence); it returns only after the conversion completed and the planes
// were restored for reuse by the decoder. The ForeignVideoFrame's images
// must outlive the call.

#include <vulkan/vulkan.h>

#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Submit.hpp"

namespace nemo::gpu {

// One external producer frame: up to two planes (NV12: R8 luma + R8G8
// chroma), each with its own timeline semaphore the producer signals at
// `waitValues` when the plane is readable.
struct ForeignVideoFrame {
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
    uint32_t planeCount = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// The interop converter: owns the convert pipeline + descriptor machinery
// and one graphics SubmissionQueue. Descriptor lifecycle differs from the
// effect ComputePass (per-frame rewrites of foreign views), so it builds
// its own; the pipeline table mirrors mediaConvert.slang's declared
// bindings: set 0 binding 0 uniform, set 1 bindings 0/1 sampled planes,
// set 2 binding 0 rgba32f storage output.
class MediaInterop {
public:
    // `convertSpirv` is the compiled mediaConvert compute kernel.
    static std::unique_ptr<MediaInterop> create(Device& device, Allocator& allocator,
                                                const std::vector<std::uint32_t>& convertSpirv);
    ~MediaInterop();
    MediaInterop(const MediaInterop&) = delete;
    MediaInterop& operator=(const MediaInterop&) = delete;

    // Converts one foreign NV12 frame into `output` (rgba32f storage image,
    // GENERAL layout maintained). Synchronous. Establishes the producer→
    // conversion dependency with in-queue timeline waits, signals the
    // producer semaphores back at waitValue+1, and updates `frame`
    // (waitValues, queueFamilies, accesses) so the producer can reuse the
    // planes. Throws GpuException naming the failure.
    void convertToRgba32f(ForeignVideoFrame& frame, Image& output, uint64_t timeout_ns);

private:
    MediaInterop() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::gpu
