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
};

// What one scheduled unit asks for. A render states an immutable VIEW INTENT
// that the worker resolves against the current frame's described image; a
// metadata or cache unit states the concrete request it addresses. The
// scheduler never interprets the demand — it only carries it to the worker
// under the request's identity, priority and cancellation policy.
using ViewerDemand = std::variant<EvaluationRequest, ViewIntent>;

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
    // `interactiveCapacity` bounds the total queued interactive/probe
    // descriptors across all destinations; a destination coalesces its own
    // queued work into at most one descriptor. A range is represented by one
    // descriptor per destination and therefore never allocates a vector
    // proportional to its span.
    explicit ViewerScheduler(std::size_t interactiveCapacity = 1);

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
    bool requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                      ViewerDestination destination = ViewerDestination::Cache,
                      std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now(),
                      std::string colorConfigPath = {});

    // Returns the highest-priority queued item: interactive/probe before the
    // next lazily admitted range frame. The returned request retains the
    // immutable document snapshot until execution and publication finish.
    [[nodiscard]] std::optional<ViewerScheduledRequest> take();
    [[nodiscard]] bool hasWork() const;

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
    };

    bool enqueueInteractive(ViewerScheduledRequest work);
    [[nodiscard]] bool currentLocked(const ViewerScheduledRequest& request) const;
    [[nodiscard]] bool admissibleLocked(std::uint64_t id, ViewerDestination destination) const;
    [[nodiscard]] std::uint64_t remainingRangeLocked() const;
    void dropLocked(ViewerDestination destination, std::uint64_t frames);
    void dropRangeLocked(ViewerDestination destination);
    [[nodiscard]] const DestinationState* stateLocked(ViewerDestination destination) const;

    const std::size_t interactiveCapacity_;
    mutable std::mutex mutex_;
    std::deque<ViewerScheduledRequest> interactive_;
    std::map<ViewerDestination, Range> ranges_;
    std::map<ViewerDestination, DestinationState> destinations_;
    // Per-destination accumulation beside the global counters; `queued` is
    // always recomputed from the live containers rather than accumulated.
    std::map<ViewerDestination, ViewerSchedulerCounts> destinationCounts_;
    std::uint64_t nextToken_{1};
    std::uint64_t cancelFloor_{};
    std::uint64_t cancelToken_{};
    ViewerSchedulerCounts counts_;
};

}  // namespace nemo::eval
