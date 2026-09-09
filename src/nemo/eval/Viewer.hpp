#pragma once

#include <filesystem>
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
};

// Worker-confined orchestration over the shared native dependency plan:
// source decode -> native effects -> GPU OCIO. Never host pixel readback.
// Matching scene-linear results reuse #9's cache; distinct representations
// coexist. Returned display images are immutable and ready for presentation.
// timeout_ns bounds individual GPU waits, not CPU decoding/compilation or
// the total request. Cache compression is always asynchronous.
class ViewerSession {
public:
    ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& shaderDirectory);
    ~ViewerSession();
    ViewerSession(const ViewerSession&) = delete;
    ViewerSession& operator=(const ViewerSession&) = delete;
    [[nodiscard]] ViewerFrame render(const Document& document, const EvaluationRequest& request,
                                     std::uint64_t timeout_ns = 10'000'000'000ULL, std::uint64_t generation = 0);

    // Configures persistent requested-only display cache storage. Setup is
    // worker-side and may allocate media resources; render remains live-first.
    void configureCache(const ViewerCacheOptions& options);
    // Shutdown/headless drain only; throws when asynchronous encode/mux/cache
    // admission reported an error.
    void flushCache();
    [[nodiscard]] ViewerCacheCounts cacheCounts() const;
    // Thread-safe UI freshness signal. It never dereferences a Document and
    // does not invalidate valid distinct frame representations.
    void supersedeCache(std::uint64_t revision, std::uint64_t generation);

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
    gpu::Instance& instance_;
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::string ocioConfigPath_;  // Resolve $OCIO on first viewing request.
    std::filesystem::path replayShader_;
    SourceSession sources_;
    EffectLibrary effects_;
    ResultCache<GpuNodeImage> reuse_;
    std::map<std::pair<std::string, std::string>, ViewingState> viewing_;
    mutable std::mutex cacheMutex_;
    std::unique_ptr<ViewerCache> cache_;
    mutable std::mutex freshnessMutex_;
    std::uint64_t latestRevision_{0};
    std::uint64_t latestGeneration_{0};
    std::uint64_t nextRequestId_{1};
};

}  // namespace nemo::eval
