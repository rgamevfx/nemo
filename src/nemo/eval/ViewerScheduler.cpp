#include "nemo/eval/ViewerScheduler.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nemo::eval {
namespace {
std::uint64_t frameCount(int first, int last) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(last) - first + 1);
}
}  // namespace

ViewerScheduler::ViewerScheduler(std::size_t interactiveCapacity) : interactiveCapacity_(interactiveCapacity) {
    if (interactiveCapacity_ == 0) {
        throw std::invalid_argument("viewer scheduler interactive capacity must be positive");
    }
}

const ViewerScheduler::DestinationState* ViewerScheduler::stateLocked(ViewerDestination destination) const {
    const auto found = destinations_.find(destination);
    return found == destinations_.end() ? nullptr : &found->second;
}

bool ViewerScheduler::admissibleLocked(std::uint64_t id, ViewerDestination destination) const {
    const auto* state = stateLocked(destination);
    return nextToken_ != std::numeric_limits<std::uint64_t>::max() && id >= cancelFloor_ &&
           ((state == nullptr) || id >= state->id) && ((state != nullptr) || destinations_.size() < kMaxDestinations);
}

std::uint64_t ViewerScheduler::remainingRangeLocked() const {
    std::uint64_t remaining = 0;
    for (const auto& entry : ranges_) {
        remaining += frameCount(entry.second.next, entry.second.last);
    }
    return remaining;
}

void ViewerScheduler::dropRangeLocked(ViewerDestination destination) {
    const auto found = ranges_.find(destination);
    if (found != ranges_.end()) {
        counts_.dropped += frameCount(found->second.next, found->second.last);
        ranges_.erase(found);
    }
}

bool ViewerScheduler::enqueueInteractive(ViewerScheduledRequest work) {
    work.revision = work.document->stateRevision();
    std::lock_guard const lock(mutex_);
    const auto previous = std::find_if(interactive_.begin(), interactive_.end(),
                                       [&](const auto& pending) { return pending.destination == work.destination; });
    if (!admissibleLocked(work.id, work.destination) ||
        (previous == interactive_.end() && interactive_.size() >= interactiveCapacity_)) {
        ++counts_.dropped;
        return false;
    }
    auto& state = destinations_[work.destination];
    work.token = nextToken_++;
    state.id = work.id;
    state.token = work.token;
    state.revision = work.revision;
    dropRangeLocked(work.destination);
    if (previous != interactive_.end()) {
        *previous = std::move(work);
        ++counts_.dropped;
    } else {
        interactive_.push_back(std::move(work));
    }
    return true;
}

bool ViewerScheduler::submit(Document document, EvaluationRequest request, std::uint64_t id,
                             ViewerDestination destination, std::chrono::steady_clock::time_point requestedAt) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .request = std::move(request),
                               .source = {},
                               .id = id,
                               .kind = ViewerRequestKind::Render,
                               .destination = destination,
                               .requestedAt = requestedAt});
}

bool ViewerScheduler::probe(Document document, std::string source, std::uint64_t id, ViewerDestination destination,
                            std::chrono::steady_clock::time_point requestedAt) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .request = {},
                               .source = std::move(source),
                               .id = id,
                               .kind = ViewerRequestKind::Probe,
                               .destination = destination,
                               .requestedAt = requestedAt});
}

bool ViewerScheduler::requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                                   ViewerDestination destination, std::chrono::steady_clock::time_point requestedAt) {
    auto snapshot = std::make_shared<const Document>(std::move(document));
    const auto revision = snapshot->stateRevision();
    std::lock_guard const lock(mutex_);
    if (first > last || !admissibleLocked(id, destination)) {
        ++counts_.dropped;
        return false;
    }
    auto& state = destinations_[destination];
    const auto token = nextToken_++;
    state.id = id;
    state.token = token;
    state.revision = revision;
    std::erase_if(interactive_, [&](const auto& pending) {
        if (pending.destination != destination) {
            return false;
        }
        ++counts_.dropped;
        return true;
    });
    dropRangeLocked(destination);
    ranges_.insert_or_assign(destination, Range{{std::move(snapshot),
                                                 std::move(request),
                                                 {},
                                                 id,
                                                 token,
                                                 revision,
                                                 ViewerRequestKind::CacheRange,
                                                 destination,
                                                 requestedAt},
                                                first,
                                                last});
    return true;
}

std::optional<ViewerScheduledRequest> ViewerScheduler::take() {
    std::lock_guard const lock(mutex_);
    if (!interactive_.empty()) {
        auto result = std::move(interactive_.front());
        interactive_.pop_front();
        return result;
    }
    if (ranges_.empty()) {
        return std::nullopt;
    }
    auto found = ranges_.begin();
    auto& range = found->second;
    auto result = range.work;
    result.request.localTime = range.next;
    if (range.next == range.last) {
        ranges_.erase(found);
    } else {
        ++range.next;
    }
    return result;
}

bool ViewerScheduler::hasWork() const {
    std::lock_guard const lock(mutex_);
    return !interactive_.empty() || !ranges_.empty();
}

void ViewerScheduler::cancel(std::uint64_t id) {
    std::lock_guard const lock(mutex_);
    if (id >= cancelFloor_) {
        cancelFloor_ = id;
        cancelToken_ = nextToken_;
    }
    std::erase_if(interactive_, [&](const auto& pending) {
        if (pending.id > id) {
            return false;
        }
        ++counts_.dropped;
        return true;
    });
    for (auto it = ranges_.begin(); it != ranges_.end();) {
        if (it->second.work.id > id) {
            ++it;
        } else {
            counts_.dropped += frameCount(it->second.next, it->second.last);
            it = ranges_.erase(it);
        }
    }
    for (auto& entry : destinations_) {
        if (entry.second.id <= id) {
            entry.second.token = 0;
            entry.second.cacheFloor = nextToken_;
        }
    }
}

bool ViewerScheduler::currentLocked(const ViewerScheduledRequest& request) const {
    const auto* state = stateLocked(request.destination);
    return (state != nullptr) && request.id >= cancelFloor_ && request.id == state->id && request.token != 0 &&
           request.token == state->token && request.revision == state->revision;
}

bool ViewerScheduler::isCurrent(const ViewerScheduledRequest& request) const {
    std::lock_guard const lock(mutex_);
    return currentLocked(request);
}

bool ViewerScheduler::isCacheCurrent(const ViewerScheduledRequest& request) const {
    std::lock_guard const lock(mutex_);
    const auto* state = stateLocked(request.destination);
    return (state != nullptr) && request.token != 0 && request.token >= state->cacheFloor &&
           request.revision == state->revision &&
           (request.id > cancelFloor_ || request.token >= cancelToken_);
}

bool ViewerScheduler::complete(const ViewerScheduledRequest& request, bool published) {
    std::lock_guard const lock(mutex_);
    if (!currentLocked(request)) {
        ++counts_.staleRejected;
        return false;
    }
    if (published) {
        ++counts_.completed;
    }
    return true;
}

void ViewerScheduler::clear() {
    std::lock_guard const lock(mutex_);
    counts_.dropped += interactive_.size() + remainingRangeLocked();
    interactive_.clear();
    ranges_.clear();
    for (auto& entry : destinations_) {
        entry.second.token = 0;
        entry.second.cacheFloor = nextToken_;
    }
}

ViewerSchedulerCounts ViewerScheduler::counts() const {
    std::lock_guard const lock(mutex_);
    auto result = counts_;
    result.queued = interactive_.size() + remainingRangeLocked();
    return result;
}
}  // namespace nemo::eval
