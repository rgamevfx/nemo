#pragma once

// Indexed, bounded viewer-cache storage for display-referred BC7 frames
// (issue #106, spec sections 8 and 11). One frame is one independently
// committed indexed record of compressed 4x4 blocks; replay uploads those
// blocks directly, so no video demux/decode, GOP dependency, YUV conversion
// or float reconstruction exists on the cached path. Scene-linear evaluator
// images and full-quality exports never enter this interface: the retained
// live frame is compressed, never reconstructed, and a BC7 frame is a
// display approximation only.
//
// Tiers and their byte budgets are separate: pending work (retained inputs,
// encode readback, queued payloads), compressed RAM, resident BC7 images and
// durable pack bytes each have an explicit bound, and every tier evicts the
// least recently used eligible record instead of growing.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/eval/ViewerDestination.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Bc7.hpp"
#include "nemo/gpu/Device.hpp"

namespace nemo::eval {

// The BC7 representation identity. This is both the on-disk pack namespace
// version and the tag a consumer mixes into its cache key, so a key can never
// name a representation the storage namespace cannot serve, and the format
// cutover is one string.
inline constexpr std::string_view kViewerCacheRepresentation{"bc7-frames-1"};

struct ViewerCacheOptions {
    std::filesystem::path directory;
    // Small secondary count bounds. The byte budgets below are the primary
    // admission policy; frame counts are not memory accounting.
    std::size_t maxPendingFrames{12};
    std::size_t maxMetadataEntries{16384};
    std::uint64_t maxDiskBytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maxRamBytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t maxResidentBytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t maxPendingBytes{256ULL * 1024ULL * 1024ULL};
    // Admitted asynchronous replay preparations per cache. Requests beyond
    // the bound coalesce into a retry rather than growing a queue.
    std::size_t maxReplayFrames{8};
};

// One display-only frame offered to the asynchronous BC7 writer. `identity`
// is the exact consumer-facing content key and `viewingIdentity` the applied
// projection/viewing identity it was keyed with; `description`/`request` are
// the real resolved description and request the frame was produced from, kept
// so a replay hit returns them unchanged instead of a re-derived guess.
// Destination scopes freshness only and is never persisted.
struct ViewerCachePublication {
    std::string identity;
    std::string viewingIdentity;
    std::int64_t localTime{0};
    std::uint64_t revision{0};
    std::uint64_t generation{0};
    std::shared_ptr<const gpu::Image> image;
    ImageLayout layout;
    ImageDescription description;
    EvaluationRequest request;
    ViewerDestination destination{ViewerDestination::Interactive};
    // Scheduler-owned freshness, checked by the writer through completion.
    // Must be thread-safe and outlive the cache writer; never call this cache.
    std::function<bool()> publicationGuard{};
};

struct ViewerCacheCounts {
    // Lookup outcomes: `hits` counts Ready frames only, `misses` counts an
    // identity this cache never accepted. Loading/Failed/Evicted are states,
    // not misses, and are reported through `loadingFrames`/`lastError`.
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    // Frames whose complete in-memory representation was published, which is
    // independent of a durable commit.
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
    std::uint64_t activeFrames{0};
    std::uint64_t encodingFrames{0};  // Frames handed to the BC7 encoder.
    std::uint64_t compressedHotHits{0};
    std::uint64_t compressedHotBytes{0};
    std::uint64_t residentBytes{0};
    std::uint64_t residentFrames{0};
    std::uint64_t uploadedFrames{0};
    std::uint64_t loadingFrames{0};
    std::uint64_t evictedFrames{0};
    std::uint64_t pendingBytes{0};
    std::string lastError;
};

// Internal readiness of one requested representation. A valid entry that is
// still being read or uploaded is Loading, never Missing: the caller must not
// take the live-render path for work this cache already admitted.
enum class ViewerCacheState { Missing, Loading, Ready, Failed, Evicted };

// A ready shared compressed texture and layout, not its producer's identity.
// ViewerSession supplies the validated consumer request and description.
// The image stays valid while held; evicting its residency drops only the
// cache's reference.
struct ViewerCacheResult {
    std::shared_ptr<const gpu::Bc7Image> image;
    ImageLayout layout;
};

struct ViewerCacheLookup {
    ViewerCacheState state{ViewerCacheState::Missing};
    std::optional<ViewerCacheResult> frame;
    std::string diagnostic;
};

// Deep storage/replay module behind ViewerSession's small integration seam.
// `lookup` never runs the graph, never decodes media and never waits: it
// answers with the current state and admits/coalesces the asynchronous
// read/upload a Loading answer describes.
class ViewerCache {
    // Declared before every user so the nested scope can name it.
    struct ForegroundGate;

public:
    // Foreground construction scope (issue #106): while one is alive the
    // asynchronous writer defers its own GPU submissions — block encode and
    // compressed upload — and submits as soon as the last scope ends, so a live
    // frame's presentation construction is never queued behind background cache
    // work on the shared device queue. It is a construction gate, not a device
    // wait: writer CPU-side work continues, no allocator lock is held while
    // waiting, and no second queue or serialization owner is introduced.
    //
    // The scope holds only weak state, so a scope that outlives its cache
    // releases nothing instead of touching freed memory, and a
    // default-constructed scope is an explicit no-op.
    class ForegroundScope {
    public:
        ForegroundScope() = default;
        ForegroundScope(ForegroundScope&&) noexcept = default;
        ForegroundScope& operator=(ForegroundScope&& other) noexcept;
        ~ForegroundScope();
        ForegroundScope(const ForegroundScope&) = delete;
        ForegroundScope& operator=(const ForegroundScope&) = delete;

    private:
        friend class ViewerCache;
        explicit ForegroundScope(std::shared_ptr<ForegroundGate> gate);
        std::weak_ptr<ForegroundGate> gate_;
    };

    ViewerCache(gpu::Device& device, gpu::Allocator& allocator, const std::filesystem::path& shaderDirectory);
    ~ViewerCache();
    ViewerCache(const ViewerCache&) = delete;
    ViewerCache& operator=(const ViewerCache&) = delete;

    // Admits one foreground construction scope, or an empty scope when this
    // cache has nothing to suppress (unconfigured, unavailable or shutting
    // down). Worker-thread use only, like every other cache entry point.
    [[nodiscard]] ForegroundScope foregroundScope();

    void configure(const ViewerCacheOptions& options);

    // Nonblocking. Ready carries the resident BC7 frame; Loading means work
    // for this identity is admitted or coalesced (retry, never re-render);
    // Missing means it was never accepted; Failed and Evicted name a record
    // that cannot serve a frame, with the reason in `diagnostic`.
    [[nodiscard]] ViewerCacheLookup lookup(const std::string& identity);

    bool enqueue(ViewerCachePublication publication);

    // Advances freshness for one destination. Revision changes reject
    // old-revision writes in that destination; a newer generation is applied
    // per identity when its replacement is enqueued. Existing callers use
    // the interactive default.
    void supersede(std::uint64_t revision, std::uint64_t generation,
                   ViewerDestination destination = ViewerDestination::Interactive);

    // Waits for all accepted work to become idle. Persistence and encoding
    // failures are retained in counts and reported as a runtime_error here;
    // lookup() never waits for them and therefore still returns live frames.
    void flush();

    [[nodiscard]] ViewerCacheCounts counts() const;
    // Never waits for the writer/durable/replay mutex. A busy cache reports no
    // snapshot so presentation can keep its previous counters.
    [[nodiscard]] std::optional<ViewerCacheCounts> tryCounts() const;

private:
    bool enqueueLocked(ViewerCachePublication publication);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::eval
