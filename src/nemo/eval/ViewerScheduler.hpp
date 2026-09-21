#pragma once

// Application request policy for the viewer (issue #13). This module is
// headless: it owns bounded admission, priority, cancellation, immutable
// snapshots, and publication freshness, but never touches Qt, Vulkan, or
// cache/GPU resource ownership.

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/eval/ViewIntent.hpp"
#include "nemo/eval/ViewerDestination.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace nemo::eval {

enum class ViewerRequestKind : std::uint8_t {
    Render,
    Probe,
    CacheRange,
    // Metadata-only description of one target (issue #88): the worker resolves
    // the target's authored output description through the shared dependency
    // planner, without acquiring a pixel. A caller that must learn one node's
    // authored channels before it can offer them — the mapping inspector's
    // availability query — needs this as an independent answer, so it travels
    // through the same bounded admission, coalescing and cancellation as every
    // other request. A viewer frame does NOT use it: a render resolves its own
    // current-frame description (issue #98).
    Describe,
    // One on-demand working-space pixel of a target (issue #102): the concrete
    // single-pixel demand a viewport color pick states. It travels through the
    // same bounded admission, coalescing and cancellation as every other
    // request, so a pick can never bypass the scheduler that owns viewer work.
    Sample,
    // One frame of the transport's ordered playback window (issue #106): the
    // worker serves it from the retained display-cache representation and never
    // evaluates the graph. It is admitted into its own bounded, ordered,
    // per-destination window instead of the latest-wins interactive slot, so
    // admitting frame N+1 cannot make frame N stale before its scheduled
    // display, while a seek/edit still supersedes the whole window. It states a
    // ViewIntent, exactly like a live render, so a replayed frame describes the
    // same view a live one would.
    Replay,
};

// What one scheduled unit asks for. A render states an immutable VIEW INTENT
// that the worker resolves against the current frame's described image; a
// metadata or cache unit states the concrete request it addresses. The
// scheduler never interprets the demand — it only carries it to the worker
// under the request's identity, priority and cancellation policy.
using ViewerDemand = std::variant<EvaluationRequest, ViewIntent>;

// Shared for an uninterrupted playback context, not rebuilt on ordinary ticks.
// Capturing owns the snapshot and computes its content stamp exactly once.
struct ViewerPlaybackContext {
    explicit ViewerPlaybackContext(Document input);
    const std::shared_ptr<const Document> document;
    const std::uint64_t revision;
};

struct ViewerScheduledRequest {
    // The document is immutable for the entire request. Range frames share
    // one snapshot instead of copying/allocating a request for every frame at
    // admission time.
    std::shared_ptr<const Document> document;
    ViewerDemand demand;
    std::string source;  // non-empty only for Probe
    std::uint64_t id{};
    std::uint64_t token{};  // unique scheduler identity, distinct from id
    std::uint64_t revision{};
    ViewerRequestKind kind{ViewerRequestKind::Render};
    ViewerDestination destination{ViewerDestination::Interactive};
    // Ordered playback window identity (issue #106), meaningful for a Replay
    // unit: the destination's epoch at admission. Every latest-wins submission
    // for that destination (interactive render, probe, description, sample,
    // range, cancellation, retirement) advances the epoch, which retires the
    // whole window without touching the destination's latest-wins identity —
    // that is what keeps an ordered read-ahead from superseding itself.
    std::uint64_t playbackEpoch{};
    std::chrono::steady_clock::time_point requestedAt{};
    // Color configuration the worker's ViewerSession must use for this request
    // (empty = the OCIO application default). Carried with the immutable
    // document so a project's authored config replaces the worker session
    // without touching process-global environment state.
    std::string colorConfigPath;

    // The view a render resolves. Only a Render unit holds an intent.
    [[nodiscard]] const ViewIntent& intent() const { return std::get<ViewIntent>(demand); }
    // The concrete request a Describe/CacheRange unit addresses. Empty for a
    // Probe, which names its media source instead.
    [[nodiscard]] const EvaluationRequest& request() const { return std::get<EvaluationRequest>(demand); }
};

struct ViewerSchedulerCounts {
    // queued includes the lazily represented remaining range frames. It is a
    // work count, not an allocation count; at most one range descriptor lives
    // per destination in the scheduler.
    std::uint64_t queued{};
    std::uint64_t dropped{};
    std::uint64_t staleRejected{};
    std::uint64_t completed{};
};
class ViewerScheduler final {
public:
    // Default bound for ONE destination's ordered playback window (issue #106):
    // enough read-ahead to keep the transport clock ahead of preparation, small
    // enough that a superseded window is cheap to discard and that the retained
    // compressed frames stay inside the cache's own replay bound.
    static constexpr std::size_t kDefaultPlaybackWindow = 8;

    // `interactiveCapacity` bounds the total queued interactive/probe
    // descriptors across all destinations; a destination coalesces its own
    // queued work into at most one descriptor. A range is represented by one
    // descriptor per destination and therefore never allocates a vector
    // proportional to its span. `playbackWindow` bounds the ordered playback
    // frames queued for ONE destination (issue #106); the total is bounded
    // because destinations are.
    explicit ViewerScheduler(std::size_t interactiveCapacity = 1, std::size_t playbackWindow = kDefaultPlaybackWindow);

    ViewerScheduler(const ViewerScheduler&) = delete;
    ViewerScheduler& operator=(const ViewerScheduler&) = delete;

    // All methods are thread-safe. Submission methods take ownership of the
    // input document and capture an immutable snapshot before returning.
    // New work supersedes queued work for its destination; in-flight work is
    // retained by the evaluator/GPU and is rejected at publication. A render
    // states an immutable view intent (issue #98) — the worker resolves it
    // against the current frame's described image — so the panel never needs a
    // separate description round trip to state a demand.
    bool submit(Document document, ViewIntent intent, std::uint64_t id,
                ViewerDestination destination = ViewerDestination::Interactive,
                std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
                std::string colorConfigPath = {});
    bool probe(Document document, std::string source, std::uint64_t id,
               ViewerDestination destination = ViewerDestination::Interactive,
               std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
               std::string colorConfigPath = {});
    // Metadata-only target description (issue #88). `request` identifies the
    // target (network, output, local time); its domain is not consulted, so a
    // caller that does not yet know the target's format still gets the
    // authored description back through the normal interactive admission path.
    bool describe(Document document, EvaluationRequest request, std::uint64_t id,
                  ViewerDestination destination = ViewerDestination::Interactive,
                  std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
                  std::string colorConfigPath = {});
    // One on-demand working-space pixel of a target (issue #102). `request` is
    // the concrete single-pixel demand; it shares the interactive admission and
    // coalescing with every other request, so a pick cannot preempt or displace
    // work it never asked to replace unless the caller gave it that destination.
    bool sample(Document document, EvaluationRequest request, std::uint64_t id,
                ViewerDestination destination = ViewerDestination::Interactive,
                std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
                std::string colorConfigPath = {});
    bool requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                      ViewerDestination destination = ViewerDestination::Cache,
                      std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
                      std::string colorConfigPath = {});

    // One frame of the ordered playback window (issue #106). `intent` states the
    // frame's view exactly as a live render would; the unit is appended to its
    // destination's window in admission order and is served from the retained
    // display-cache representation only. A frame never supersedes its
    // predecessor, and admitting a successor never makes its predecessor stale.
    // The window is bounded per destination by `playbackWindow`; admission
    // beyond that bound (or below the destination's cancellation watermark) is
    // rejected and counted as dropped. Every latest-wins submission
    // (submit/probe/describe/sample/requestRange) and every cancellation
    // supersedes the destination's whole window, which is how a seek, edit,
    // stop or view change retires obsolete preparation without waiting for the
    // in-flight GPU work it retained.
    //
    // Returns the admitted context's revision; nullopt when refused. Admission
    // retains the existing context without copying or fingerprinting its graph.
    [[nodiscard]] std::optional<std::uint64_t>
    preparePlayback(const ViewerPlaybackContext& context, ViewIntent intent, std::uint64_t id,
                    ViewerDestination destination = ViewerDestination::Interactive,
                    std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
                    std::string colorConfigPath = {});

    // Returns the highest-priority queued item: interactive/probe before the
    // next ordered playback frame before the next lazily admitted range frame.
    // The returned request retains the immutable document snapshot until
    // execution and publication finish.
    // A busy cache may defer lazy range production without consuming its next
    // frame or preventing foreground work and ordered replay from being served.
    [[nodiscard]] std::optional<ViewerScheduledRequest> take(bool admitRange = true);
    [[nodiscard]] bool hasWork(bool admitRange = true) const;

    // Cancellation invalidates work at or below `id` on all destinations,
    // clears queued descriptors, and never waits for in-flight GPU work.
    void cancel(std::uint64_t id);

    // Destination-scoped cancellation. Drops queued interactive descriptors
    // and the lazily represented range for `destination` whose id is at or
    // below `id`, and clears that destination's publication identity when its
    // state id is at or below `id`. Queued or in-flight work for every other
    // destination is untouched and the global cancel watermark does not move.
    // In-flight GPU work is never waited for.
    void cancel(std::uint64_t id, ViewerDestination destination);

    // Drops the queued interactive descriptors and the lazily represented
    // range for a destination being retired, then forgets its publication
    // state so an in-flight request for it is rejected at publication. No
    // effect on any other destination; in-flight GPU work is not waited for.
    void retireDestination(ViewerDestination destination);

    // Retires ONLY a destination's ordered playback window: its queued replay
    // frames are dropped and its in-flight ones are rejected at publication,
    // while the destination's latest-wins request, result and publication
    // identity are untouched. This is how a panel that stops walking its window
    // — pause, direction change, a view or context change, a range — retires
    // obsolete read-ahead without disturbing the frame it is displaying.
    void cancelPlayback(ViewerDestination destination);

    // `isCurrent` is a publication check. `complete` atomically accepts a
    // current result (or records a stale rejection) and returns whether it
    // was current. Callers must drop stale images; GPU ownership remains with
    // the shared execution mechanism until completion.
    [[nodiscard]] bool isCurrent(const ViewerScheduledRequest& request) const;
    // Cache history may finish encoding after a same-revision scrub. Explicit
    // cancellation or an edited snapshot still rejects its asynchronous write.
    [[nodiscard]] bool isCacheCurrent(const ViewerScheduledRequest& request) const;
    [[nodiscard]] bool complete(const ViewerScheduledRequest& request, bool published);

    // Invalidates and drops all queued work. In-flight requests become stale.
    void clear();

    // Global work/policy counters. `queued` counts live descriptors; the
    // accumulated fields include work dropped while retiring a destination.
    [[nodiscard]] ViewerSchedulerCounts counts() const;

    // Counters scoped to one destination. `queued` is that destination's live
    // interactive descriptors plus its remaining lazily represented range
    // frames; `dropped`, `staleRejected` and `completed` are that
    // destination's accumulated values. An unknown destination reports zeros.
    [[nodiscard]] ViewerSchedulerCounts counts(ViewerDestination destination) const;

private:
    struct Range {
        ViewerScheduledRequest work;
        int next{};
        int last{};
    };

    struct DestinationState {
        std::uint64_t id{};
        std::uint64_t token{};
        std::uint64_t revision{};
        std::uint64_t cacheFloor{};
        // Per-destination cancellation watermark: publication must clear both
        // this destination's floor/token and the global cancelFloor_/cancelToken_.
        std::uint64_t cancelFloor{};
        std::uint64_t cancelToken{};
        // Ordered playback window identity (issue #106). The epoch is allocated
        // from a process-wide counter so a re-created destination can never
        // revive a retired window's in-flight frames.
        std::uint64_t playbackEpoch{};
        std::size_t playbackQueued{};
    };

    bool enqueueInteractive(ViewerScheduledRequest work);
    [[nodiscard]] bool currentLocked(const ViewerScheduledRequest& request) const;
    [[nodiscard]] bool playbackCurrentLocked(const ViewerScheduledRequest& request) const;
    [[nodiscard]] bool admissibleLocked(std::uint64_t id, ViewerDestination destination) const;
    [[nodiscard]] std::uint64_t remainingRangeLocked() const;
    void dropLocked(ViewerDestination destination, std::uint64_t frames);
    void dropRangeLocked(ViewerDestination destination);
    // Supersedes a destination's whole ordered playback window and advances its
    // epoch, so both queued and in-flight replay frames are retired without
    // touching its latest-wins identity. Every latest-wins submission and every
    // cancellation calls this.
    void supersedePlaybackLocked(ViewerDestination destination);
    void dropPlaybackLocked(ViewerDestination destination, bool countDropped);
    [[nodiscard]] const DestinationState* stateLocked(ViewerDestination destination) const;
    [[nodiscard]] DestinationState& stateForLocked(ViewerDestination destination);

    const std::size_t interactiveCapacity_;
    const std::size_t playbackWindow_;
    mutable std::mutex mutex_;
    std::deque<ViewerScheduledRequest> interactive_;
    // Ordered playback frames across all destinations; per-destination order is
    // the admission order because a destination's frames are appended in it.
    std::deque<ViewerScheduledRequest> playback_;
    std::map<ViewerDestination, Range> ranges_;
    std::map<ViewerDestination, DestinationState> destinations_;
    // Per-destination accumulation beside the global counters; `queued` is
    // always recomputed from the live containers rather than accumulated.
    std::map<ViewerDestination, ViewerSchedulerCounts> destinationCounts_;
    std::uint64_t nextToken_{1};
    std::uint64_t nextPlaybackEpoch_{1};
    std::uint64_t cancelFloor_{};
    std::uint64_t cancelToken_{};
    ViewerSchedulerCounts counts_;
};

}  // namespace nemo::eval
