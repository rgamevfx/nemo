#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nemo/eval/ChannelProjection.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/eval/ViewIntent.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/media/ViewingTransform.hpp"

namespace nemo::eval {

// The four roles the displayed representation carries (issue #90): the viewer
// projects the composition's named channels into exactly these, and any
// further named channel stays a data channel that no color transform touches.
inline const std::vector<std::string> kViewerPresentationChannels{"R", "G", "B", "A"};

struct ViewerProjection {
    std::array<std::int32_t, 4> roles{-1, -1, -1, -1};
    bool applyViewingTransform{false};
};

// Resolve storage planes and viewing policy together, before lookup/execution.
// Complete identified primary RGB uses named roles regardless of storage order.
// Other selections are data: a single plane is opaque gray; multiple planes
// retain requested order. Empty demand uses the image's actual channel names.
[[nodiscard]] ViewerProjection resolveViewerProjection(const std::vector<std::string>& requested,
                                                       const std::vector<std::string>& channels);

struct ViewerFrame {
    // Immutable, completed display-referred output. Presentation and the
    // asynchronous viewer cache share this ownership; no image copy occurs.
    std::shared_ptr<const gpu::Image> image;
    ImageLayout layout;
    // The described output this frame was produced from (issue #88): its actual
    // format, data bounds, pixel aspect, channels and interpretation. Framing
    // consumers read this instead of guessing a global canvas, and because it
    // travels with the frame no per-frame description round trip is needed.
    ImageDescription description;
    EvaluationRequest request;
    std::uint64_t revision{};
    std::uint64_t requestId{};
    bool cacheHit{false};
    // Accepted for asynchronous encoding, not proof of a persisted chunk.
    bool cacheQueued{false};
    // The presentation-only display isolation the view asked for (issue #98):
    // RGBA presents the stored RGB opaquely (issue #99), a single identified
    // primary RGB channel of a color-managed layer is isolated in the
    // presentation copy, and alpha is not isolated at all — it is demanded as
    // data and carried in the evaluated image's RGB. It never changes the
    // evaluated frame.
    gpu::ViewerChannel presentationChannel{gpu::ViewerChannel::RGBA};
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
    // The viewer's own entry point (issue #98): ONE worker job resolves the
    // immutable view intent against the current frame's described image — the
    // authored format for exactly its local time, the addressed layer/channel
    // and the demand this view produces — then keys and executes the SAME
    // resolved plan, and returns the frame with the description it was actually
    // produced from. The panel therefore needs no description round trip to
    // state a demand, and a steady frame costs one completed worker request.
    //
    // `resolution` is this destination's Auto hysteresis state. The worker
    // retains it for the destination's lifetime, so a steady view does not
    // oscillate between representations and a neighbouring frame cannot
    // re-resolve it; the frame's request states the effective sampling scale
    // the panel presents. An unavailable layer or channel throws
    // ViewUnavailable, which states that this view has nothing to present and
    // carries the description that refused it, so the caller can adopt the
    // current frame's channels in the same job instead of retrying.
    [[nodiscard]] ViewerFrame render(const Document& document, const ViewIntent& intent,
                                     ViewerResolutionPolicy& resolution, std::uint64_t timeout_ns = 10'000'000'000ULL,
                                     std::uint64_t generation = 0,
                                     ViewerDestination destination = ViewerDestination::Interactive,
                                     CachePublicationGuard publicationGuard = {});
    // Worker-only metadata query (issue #88): the target's authored output
    // description — its actual format, data bounds, pixel aspect, channels and
    // interpretation — resolved through the same shared dependency planner the
    // render path uses, WITHOUT acquiring a pixel or touching the device.
    // `request` identifies the target (network, output, local time); its domain
    // is not consulted, so the caller can ask before it knows the format. Real
    // media is described from the source session's metadata, never by decoding
    // a frame to measure it.
    [[nodiscard]] ImageDescription describe(const Document& document, const EvaluationRequest& request);

    // Configures persistent requested-only display cache storage. Setup is
    // worker-side and may allocate media resources; render remains live-first.
    void configureCache(const ViewerCacheOptions& options);
    // Explicit color refresh boundary (project replacement or a deliberate
    // configuration reload): retires the retained viewing programs/LUTs and the
    // source session's retained OCIO processors, so the next render re-reads
    // the configuration content. Nothing polls for file changes; the owner
    // must invoke this boundary after replacing the configuration.
    void refreshColorConfig();

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
    // The shared execution body of both render entry points: validates the
    // concrete request, plans (or adopts) exactly one region plan, keys it,
    // executes it and turns the result into the displayed representation.
    // `described` is the description plan the view-intent path already resolved
    // for this document, target and local time; the concrete-request path
    // leaves it empty and is planned here.
    [[nodiscard]] ViewerFrame renderResolved(const Document& document, const EvaluationRequest& request,
                                             std::optional<ImageDescriptionPlan> described, std::uint64_t timeout_ns,
                                             std::uint64_t generation, ViewerDestination destination,
                                             CachePublicationGuard publicationGuard,
                                             gpu::ViewerChannel presentationChannel);
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
    ChannelProjection projections_;
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
