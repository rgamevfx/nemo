#pragma once

// Requested-only asynchronous viewer cache (issue #12, spec sections 8 and
// 11). The cache stores independently finalized display-referred chunks on
// disk and uploads a matching chunk only for an explicit viewer request.
// Scene-linear evaluator images and full-quality exports never enter this
// interface.

#include "nemo/eval/ViewerDestination.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/ViewerEncode.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace nemo::eval {

struct ViewerCacheOptions {
    std::filesystem::path directory;
    media::EncodeOptions encoding{.codec = "h264-nvenc",
                                  .gopSize = 12,
                                  .bitrateKbps = 8000,
                                  .injectedFailure = nullptr,
                                  .profile = {},
                                  .bitDepth = 8};
    std::size_t chunkFrames{12};
    std::size_t maxPendingFrames{12};
    // Simultaneous decode operations; retained replay has a separate fixed
    // four-frame GPU hot queue and bounded compressed-RAM tier.
    std::size_t maxDecodedFrames{2};
    // Hard bound for indexed identities; excess persisted metadata and new
    // publications are rejected rather than growing the in-memory index.
    std::size_t maxMetadataEntries{16384};
    std::uint64_t maxDiskBytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
};

// One display-only frame accepted by the asynchronous cache writer. The
// chunkGroupKey controls storage batching; identity remains the exact
// consumer-facing cache key. Destination scopes freshness only; it is not
// persisted in the consumer-facing identity.
struct ViewerCachePublication {
    std::string identity;
    std::string chunkGroupKey;
    std::int64_t localTime{0};
    std::uint64_t revision{0};
    std::uint64_t generation{0};
    std::shared_ptr<const gpu::Image> image;
    ImageLayout layout;
    ViewerDestination destination{ViewerDestination::Interactive};
    // Scheduler-owned freshness, checked by the writer through completion.
    // Must be thread-safe and outlive the cache writer; never call this cache.
    std::function<bool()> publicationGuard{};
};

struct ViewerCacheCounts {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t published{0};
    std::uint64_t staleRejected{0};
    std::uint64_t encodedFrames{0};
    std::uint64_t pendingFrames{0};
    std::uint64_t peakPendingFrames{0};
    std::uint64_t diskBytes{0};
    std::uint64_t errors{0};
    std::uint64_t admissionRejected{0};
    std::uint64_t admissionDropped{0};
    std::uint64_t invalidEntries{0};
    std::uint64_t decodedFrames{0};
    std::uint64_t hardwareDecodedFrames{0};
    std::uint64_t softwareDecodedFrames{0};
    std::uint64_t decodedQueuePeak{0};
    std::uint64_t activeFrames{0};
    std::uint64_t encodingFrames{0};  // Active frames handed to the media encoder.
    std::uint64_t compressedHotHits{0};
    std::uint64_t decodedHotHits{0};
    std::uint64_t compressedHotBytes{0};
    std::uint64_t compressedHotChunks{0};
    std::uint64_t decodedHotFrames{0};
    std::string replayFallbackReason;
    std::string lastError;
    media::EncodeStats encode;
};

struct ViewerCacheResult {
    std::shared_ptr<const gpu::Image> image;
    ImageLayout layout;
};

// Deep storage/replay module behind ViewerSession's small integration seam.
// `lookup` is synchronous only for decoding/uploading a matching disk entry;
// compression is always performed by the worker and never by render().
class ViewerCache {
public:
    ViewerCache(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                const std::filesystem::path& convertSpirv);
    ~ViewerCache();
    ViewerCache(const ViewerCache&) = delete;
    ViewerCache& operator=(const ViewerCache&) = delete;

    void configure(const ViewerCacheOptions& options);

    [[nodiscard]] std::optional<ViewerCacheResult> lookup(const std::string& identity, const ImageLayout& expected,
                                                          std::uint64_t timeout_ns);

    bool enqueue(ViewerCachePublication publication);
    // Publishes an already-rendered group without exposing a partial batch
    // to the worker. Moves records; oversized groups are rejected.
    bool enqueueBatch(std::span<ViewerCachePublication> publications);

    // Advances freshness for one destination. Revision changes reject
    // old-revision writes in that destination; a newer generation is applied
    // per identity when its replacement is enqueued. Existing callers use
    // the interactive default.
    void supersede(std::uint64_t revision, std::uint64_t generation,
                   ViewerDestination destination = ViewerDestination::Interactive);

    // Returns the immutable representation settings used in cache identity.
    // The reference remains valid for the cache lifetime after configure().
    [[nodiscard]] const ViewerCacheOptions& optionsForIdentity() const;
    // Waits for all accepted work to become idle. Compression failures are
    // retained in counts and reported as a runtime_error here; render() never
    // waits for compression and therefore still returns its live frame.
    void flush();

    [[nodiscard]] ViewerCacheCounts counts() const;
    // Never waits for the writer/decoder mutex. A busy cache reports no
    // snapshot so presentation can keep its previous counters.
    [[nodiscard]] std::optional<ViewerCacheCounts> tryCounts() const;

private:
    bool enqueueLocked(ViewerCachePublication publication);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::eval
