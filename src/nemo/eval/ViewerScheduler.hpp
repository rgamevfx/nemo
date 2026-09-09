#pragma once

// Application request policy for the viewer (issue #13). This module is
// headless: it owns bounded admission, priority, cancellation, immutable
// snapshots, and publication freshness, but never touches Qt, Vulkan, or
// cache/GPU resource ownership.

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Request.hpp"
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

namespace nemo::eval {

enum class ViewerRequestKind : std::uint8_t {
    Render,
    Probe,
    CacheRange,
};

struct ViewerScheduledRequest {
    // The document is immutable for the entire request. Range frames share
    // one snapshot instead of copying/allocating a request for every frame at
    // admission time.
    std::shared_ptr<const Document> document;
    EvaluationRequest request;
    std::string source;  // non-empty only for Probe
    std::uint64_t id{};
    std::uint64_t token{};  // unique scheduler identity, distinct from id
    std::uint64_t revision{};
    ViewerRequestKind kind{ViewerRequestKind::Render};
    ViewerDestination destination{ViewerDestination::Interactive};
    std::chrono::steady_clock::time_point requestedAt{};
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
    // `interactiveCapacity` bounds queued interactive/probe descriptors per
    // destination. A range is represented by one descriptor per destination
    // and therefore never allocates a vector proportional to its span.
    explicit ViewerScheduler(std::size_t interactiveCapacity = 1);

    ViewerScheduler(const ViewerScheduler&) = delete;
    ViewerScheduler& operator=(const ViewerScheduler&) = delete;

    // All methods are thread-safe. Submission methods take ownership of the
    // input document and capture an immutable snapshot before returning.
    // New work supersedes queued work for its destination; in-flight work is
    // retained by the evaluator/GPU and is rejected at publication.
    bool submit(Document document, EvaluationRequest request, std::uint64_t id,
                ViewerDestination destination = ViewerDestination::Interactive,
                std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now());
    bool probe(Document document, std::string source, std::uint64_t id,
               ViewerDestination destination = ViewerDestination::Interactive,
               std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now());
    bool requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                      ViewerDestination destination = ViewerDestination::Cache,
                      std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now());

    // Returns the highest-priority queued item: interactive/probe before the
    // next lazily admitted range frame. The returned request retains the
    // immutable document snapshot until execution and publication finish.
    [[nodiscard]] std::optional<ViewerScheduledRequest> take();
    [[nodiscard]] bool hasWork() const;

    // Cancellation invalidates work at or below `id` on all destinations,
    // clears queued descriptors, and never waits for in-flight GPU work.
    void cancel(std::uint64_t id);

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

    [[nodiscard]] ViewerSchedulerCounts counts() const;

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
    };

    bool enqueueInteractive(ViewerScheduledRequest work);
    [[nodiscard]] bool currentLocked(const ViewerScheduledRequest& request) const;
    [[nodiscard]] bool admissibleLocked(std::uint64_t id, ViewerDestination destination) const;
    [[nodiscard]] std::uint64_t remainingRangeLocked() const;
    void dropRangeLocked(ViewerDestination destination);
    [[nodiscard]] const DestinationState* stateLocked(ViewerDestination destination) const;

    static constexpr std::size_t kMaxDestinations = 64;
    const std::size_t interactiveCapacity_;
    mutable std::mutex mutex_;
    std::deque<ViewerScheduledRequest> interactive_;
    std::map<ViewerDestination, Range> ranges_;
    std::map<ViewerDestination, DestinationState> destinations_;
    std::uint64_t nextToken_{1};
    std::uint64_t cancelFloor_{};
    std::uint64_t cancelToken_{};
    ViewerSchedulerCounts counts_;
};

}  // namespace nemo::eval
