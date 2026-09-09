#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Instance.hpp"

namespace nemo::gpu {

class SubmissionQueue;
// Device-owned compute pipeline cache (defined in ComputePass.hpp; issue #22).
class ComputePipelineCache;

// Aggregate counters across every SubmissionQueue and the device-owned
// compute pipeline cache bound to one Device (issue #22). Submissions/waits/
// completions and the CPU-side submit/wait time are charged by the
// SubmissionQueue machinery; pipelineCreations is charged by the compute
// pipeline cache on a cache miss. GPU execution duration is measured per
// slot with a timestamp query pair (begin at TOP_OF_PIPE, end at
// BOTTOM_OF_PIPE, scaled by timestampPeriod) when timestamps and a legal
// host/command query-reset path are available. Counters are individually
// atomic; a snapshot during concurrent updates is not transactional.
struct SubmissionStats {
    std::uint64_t submissions = 0;          // successful submit()/submit_and_wait()
    std::uint64_t waits = 0;                // wait() calls plus nonempty drain() calls
    std::uint64_t pipelineCreations = 0;    // compute pipeline cache misses that built a VkPipeline
    std::uint64_t cpuSubmitNs = 0;          // CPU time spent in submit (record + queue submit)
    std::uint64_t cpuWaitNs = 0;            // CPU time spent in wait()/drain
    std::uint64_t completions = 0;          // fences observed signalled (resources released)
    std::uint64_t gpuExecutionNs = 0;       // GPU execution time of timed submissions
    std::uint64_t gpuTimedSubmissions = 0;  // submissions measured with timestamp queries
};

struct DeviceConfig {
    bool presentation = false;
    bool externalSharing = false;
    // An explicit physical device must belong to the supplied Instance.
    VkPhysicalDevice physical = VK_NULL_HANDLE;
};

// Owns the physical/logical device pair with explicit queue selection.
//
// Queue selection contract (headless bootstrap): the graphics family is the
// first family advertising graphics+compute; the transfer family is a
// dedicated transfer family when the driver exposes one, otherwise the
// graphics family. When the driver advertises video decode/encode queue
// families (VK_KHR_video_*), the corresponding video family is reserved
// and its extension is enabled — hardware media decode (issue #10) needs
// it, capability-measured rather than assumed: absent queues report
// nullopt, never a silent substitute. `queue()` hands out only the
// selected families.
//
// Submission ownership (issue #22): Device::submissions(family) returns the
// persistent, device-lifetime SubmissionQueue for a family — lazily created
// under a mutex, shared by every consumer of that family (compute dispatch,
// graph batches, barriers). Borrowed-handle wrappers (constructing
// SubmissionQueue directly) remain legal and serialize vkQueueSubmit
// against the persistent queue and FFmpeg through
// Device::queueMutex(family) — an immutable per-family mutex.
//
// Teardown order in ~Device: the persistent submission queues are destroyed
// first (each drains only its own pending fences — no device idle), then
// the compute pipeline cache, then the VkDevice. Device/Instance must still
// outlive any retained allocation tokens; retained resources reference the
// device, not the other way around.
//
// Threading contract: VkQueue acquisition and submission are externally
// synchronized per queue. queueMutex(), submissions(), submissionStats()
// and every SubmissionQueue method are threadsafe; the remaining accessors
// are immutable after create().
class Device {
public:
    // Pass-key construction: `Token` is a private type, so callers outside
    // this header cannot construct a Device without going through create().
    struct Token;
    explicit Device(Token);
    ~Device();
    // Presentation enables swapchain support. Qt adopts a separate logical
    // device, never an execution queue. External sharing is opt-in and
    // requires the platform's memory and semaphore handle extensions.
    static std::unique_ptr<Device> create(Instance& instance, const DeviceConfig& config = {});
    [[nodiscard]] uint32_t graphics_family() const { return graphics_family_; }
    [[nodiscard]] uint32_t transfer_family() const { return transfer_family_; }
    [[nodiscard]] bool external_sharing_enabled() const { return external_sharing_; }
    [[nodiscard]] bool presentation_enabled() const { return presentation_; }

    // Reserved video queue families, when the driver advertises them
    // (VK_KHR_video_decode/encode extensions enabled at creation). Absent
    // video support reports nullopt — capability measured, not assumed.
    [[nodiscard]] std::optional<uint32_t> decode_family() const { return decode_family_; }
    [[nodiscard]] std::optional<uint32_t> encode_family() const { return encode_family_; }

    // Device extensions enabled at creation (the hardware-media interop
    // (issue #10) declares this list to FFmpeg's AVVulkanDeviceContext).
    [[nodiscard]] const std::vector<std::string>& enabled_extensions() const { return enabled_extensions_; }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkPhysicalDevice physical() const { return physical_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }

    // Physical-device feature support queried at creation. Creation enables
    // shaderStorageImageReadWithoutFormat and shaderStorageImageWriteWithout
    // Format — each exactly when reported here — because native effect
    // kernels (issue #8) read/write storage images whose format Slang emits
    // as Unknown. The effect executor refuses to run when either is absent.
    [[nodiscard]] const VkPhysicalDeviceFeatures& features() const { return features_; }
    [[nodiscard]] bool hostQueryResetEnabled() const { return host_query_reset_; }

    // Queue-family capabilities as advertised by the driver; 0 for an unknown
    // family index.
    [[nodiscard]] VkQueueFlags family_capabilities(uint32_t family) const;

    // timestampValidBits advertised for `family`'s queues (0 when the family
    // does not support timestamp queries) — the SubmissionQueue uses this to
    // decide whether GPU execution duration can be instrumented.
    [[nodiscard]] uint32_t family_timestamp_bits(uint32_t family) const;

    // The selected queue for `family`, or VK_NULL_HANDLE when the family is
    // not selected.
    [[nodiscard]] VkQueue queue(uint32_t family) const;

    // Immutable mutex serializing vkQueueSubmit on `family` across every
    // SubmissionQueue wrapper (persistent or borrowed) and FFmpeg's own
    // submits (issue #22 shared queue protocol). The returned reference is
    // stable for the Device lifetime.
    [[nodiscard]] std::mutex& queueMutex(uint32_t family);

    // The persistent, device-lifetime SubmissionQueue for `family`
    // (queue index 0), lazily created under a mutex and shared by every
    // consumer of that family. Throws GpuException for a family the device
    // does not expose. Callers must not hold the reference past device
    // destruction; the Device destructor drains (waits the queues' own
    // pending fences) before destroying the cache and VkDevice.
    [[nodiscard]] SubmissionQueue& submissions(uint32_t family);

    // The device-owned compute pipeline cache (issue #22): immutable
    // pipelines cached by full SPIR-V + normalized descriptor layout,
    // created lazily on first call and destroyed with the device.
    [[nodiscard]] ComputePipelineCache& computePipelineCache();

    // Atomic snapshot of the submission/pipeline counters (issue #22).
    [[nodiscard]] SubmissionStats submissionStats() const;

private:
    // Module-internal metric increments. SubmissionQueue and
    // ComputePipelineCache are the only writers (friend declarations).
    friend class SubmissionQueue;
    friend class ComputePipelineCache;
    void addGpuExecutionNs(std::uint64_t ns) { counters_.gpuExecutionNs.fetch_add(ns, std::memory_order_relaxed); }
    void addGpuTimedSubmissions(std::uint64_t n = 1) {
        counters_.gpuTimedSubmissions.fetch_add(n, std::memory_order_relaxed);
    }
    void addSubmissions(std::uint64_t n = 1) { counters_.submissions.fetch_add(n, std::memory_order_relaxed); }
    void addWaits(std::uint64_t n = 1) { counters_.waits.fetch_add(n, std::memory_order_relaxed); }
    void addPipelineCreations(std::uint64_t n = 1) {
        counters_.pipelineCreations.fetch_add(n, std::memory_order_relaxed);
    }
    void addCpuSubmitNs(std::uint64_t ns) { counters_.cpuSubmitNs.fetch_add(ns, std::memory_order_relaxed); }
    void addCpuWaitNs(std::uint64_t ns) { counters_.cpuWaitNs.fetch_add(ns, std::memory_order_relaxed); }
    void addCompletions(std::uint64_t n = 1) { counters_.completions.fetch_add(n, std::memory_order_relaxed); }

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    bool presentation_{false};
    bool external_sharing_{false};
    bool host_query_reset_ = false;
    std::vector<VkQueueFamilyProperties> family_properties_;
    uint32_t graphics_family_ = 0;
    uint32_t transfer_family_ = 0;
    std::optional<uint32_t> decode_family_;
    std::optional<uint32_t> encode_family_;
    std::vector<std::string> enabled_extensions_;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures features_{};

    struct Counters {
        std::atomic<std::uint64_t> submissions{0};
        std::atomic<std::uint64_t> waits{0};
        std::atomic<std::uint64_t> gpuExecutionNs{0};
        std::atomic<std::uint64_t> gpuTimedSubmissions{0};
        std::atomic<std::uint64_t> pipelineCreations{0};
        std::atomic<std::uint64_t> cpuSubmitNs{0};
        std::atomic<std::uint64_t> cpuWaitNs{0};
        std::atomic<std::uint64_t> completions{0};
    };
    Counters counters_;

    // Per-family submission mutexes: unordered_map nodes are stable, so the
    // returned references stay valid while other entries are added.
    std::mutex queue_mutexes_mutex_;
    std::unordered_map<uint32_t, std::mutex> queue_mutexes_;

    // Persistent submission queues, lazily created per family.
    std::mutex submissions_mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<SubmissionQueue>> submission_queues_;

    // Device-owned opaque compute pipeline cache (ComputePipelineCache,
    // defined in ComputePass.hpp); destroyed in ~Device before the VkDevice.
    std::unique_ptr<ComputePipelineCache> compute_pipeline_cache_;

    // Serializes lazy creation of the compute pipeline cache.
    std::mutex cache_mutex_;
};

}  // namespace nemo::gpu
