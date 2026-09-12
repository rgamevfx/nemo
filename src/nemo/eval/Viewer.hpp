#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/media/ViewingTransform.hpp"

namespace nemo::eval {

struct ViewerFrame {
    // Immutable, completed display-referred output. Presentation and the
    // asynchronous viewer cache share this ownership; no image copy occurs.
    std::shared_ptr<const gpu::Image> image;
    ImageLayout layout;
    EvaluationRequest request;
    std::uint64_t revision{};
    std::uint64_t requestId{};
    bool cacheHit{false};
    // Accepted for asynchronous encoding, not proof of a persisted chunk.
    bool cacheQueued{false};
};

// Worker-confined orchestration over the shared native dependency plan:
// source decode -> native effects -> GPU OCIO. Never host pixel readback.
// Matching scene-linear results reuse #9's cache; distinct representations
// coexist. Returned display images are immutable and ready for presentation.
// timeout_ns bounds individual GPU waits, not CPU decoding/compilation or
// the total request. Cache compression is always asynchronous.
class ViewerSession {
public:
    using CachePublicationGuard = std::function<bool()>;

    // `ocioConfigPath` is the project's authored color configuration. Empty
    // keeps the OCIO application default: the $OCIO environment variable is
    // resolved on the first viewing request. A non-empty path overrides it for
    // this session only, without mutating process-global environment state.
    ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& shaderDirectory, std::string ocioConfigPath = {});
    ~ViewerSession();
    ViewerSession(const ViewerSession&) = delete;
    ViewerSession& operator=(const ViewerSession&) = delete;
    // The optional guard is checked before enqueue and by the asynchronous
    // cache writer through publication. It must be thread-safe; captured
    // owners must outlive the session/cache worker. An omitted guard preserves
    // direct callers' revision/generation freshness contract.
    [[nodiscard]] ViewerFrame render(const Document& document, const EvaluationRequest& request,
                                     std::uint64_t timeout_ns = 10'000'000'000ULL, std::uint64_t generation = 0,
                                     ViewerDestination destination = ViewerDestination::Interactive,
                                     CachePublicationGuard publicationGuard = {});
    // Configures persistent requested-only display cache storage. Setup is
    // worker-side and may allocate media resources; render remains live-first.
    void configureCache(const ViewerCacheOptions& options);
    // Shutdown/headless drain only; throws when asynchronous encode/mux/cache
    // admission reported an error.
    void flushCache();
    [[nodiscard]] ViewerCacheCounts cacheCounts() const;
    [[nodiscard]] std::optional<ViewerCacheCounts> tryCacheCounts() const;
    // Thread-safe headless/worker freshness signal; may wait for cache setup.
    // Interactive callers use scheduler publication guards instead.
    void supersedeCache(std::uint64_t revision, std::uint64_t generation,
                        ViewerDestination destination = ViewerDestination::Interactive);
    // Forgets a retired destination's publication freshness so a reused id
    // starts clean and capacity is released back to the bounded destination
    // table. Thread-safe; the scheduler already rejects its in-flight work.
    void retireDestination(ViewerDestination destination);

    struct SourceProbe {
        media::ClipInfo info;
        media::DecodeDecision decision;  // Selected candidate before decoding.
    };
    // Opens an independent decoder. May block; do not call on the UI thread.
    [[nodiscard]] SourceProbe probeSource(const Document& document, const std::string& sourceKey) const;
    [[nodiscard]] CacheCounts reuseCounts() const;

private:
    struct ViewingState {
        // Immutable effective OCIO snapshot. A configuration reload creates
        // a new session; changing the policy selects another snapshot.
        media::OcioGpuProgram program;
        std::string identity;
        std::unique_ptr<gpu::GpuViewingTransform> transform;
    };
    [[nodiscard]] ViewingState& viewingStateFor(const ColorPolicy& policy);
    // freshnessMutex_ is held by callers.
    std::uint64_t& generationForLocked(ViewerDestination destination);
    gpu::Instance& instance_;
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::string ocioConfigPath_;  // Resolve $OCIO on first viewing request.
    std::filesystem::path replayShader_;
    SourceSession sources_;
    EffectLibrary effects_;
    ResultCache<GpuNodeImage> reuse_;
    std::map<std::pair<std::string, std::string>, ViewingState> viewing_;
    std::map<ViewerDestination, std::uint64_t> latestRevisionByDestination_;
    std::map<ViewerDestination, std::uint64_t> latestGenerationByDestination_;
    mutable std::mutex cacheMutex_;
    std::unique_ptr<ViewerCache> cache_;
    mutable std::mutex freshnessMutex_;
    std::uint64_t nextRequestId_{1};
};

}  // namespace nemo::eval
