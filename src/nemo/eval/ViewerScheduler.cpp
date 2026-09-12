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
           ((state == nullptr) || (id >= state->id && id >= state->cancelFloor)) &&
           ((state != nullptr) || destinations_.size() < kMaxViewerDestinations);
}

std::uint64_t ViewerScheduler::remainingRangeLocked() const {
    std::uint64_t remaining = 0;
    for (const auto& entry : ranges_) {
        remaining += frameCount(entry.second.next, entry.second.last);
    }
    return remaining;
}

void ViewerScheduler::dropLocked(ViewerDestination destination, std::uint64_t frames) {
    counts_.dropped += frames;
    destinationCounts_[destination].dropped += frames;
}

void ViewerScheduler::dropRangeLocked(ViewerDestination destination) {
    const auto found = ranges_.find(destination);
    if (found != ranges_.end()) {
        dropLocked(destination, frameCount(found->second.next, found->second.last));
        ranges_.erase(found);
    }
}

bool ViewerScheduler::enqueueInteractive(ViewerScheduledRequest work) {
    work.revision = work.document->stateRevision();
    const ViewerDestination destination = work.destination;
    std::lock_guard const lock(mutex_);
    const auto previous = std::find_if(interactive_.begin(), interactive_.end(),
                                       [&](const auto& pending) { return pending.destination == destination; });
    if (!admissibleLocked(work.id, destination) ||
        (previous == interactive_.end() && interactive_.size() >= interactiveCapacity_)) {
        dropLocked(destination, 1);
        return false;
    }
    auto& state = destinations_[destination];
    work.token = nextToken_++;
    state.id = work.id;
    state.token = work.token;
    state.revision = work.revision;
    dropRangeLocked(destination);
    if (previous != interactive_.end()) {
        *previous = std::move(work);
        dropLocked(destination, 1);
    } else {
        interactive_.push_back(std::move(work));
    }
    return true;
}

bool ViewerScheduler::submit(Document document, EvaluationRequest request, std::uint64_t id,
                             ViewerDestination destination, std::chrono::steady_clock::time_point requestedAt,
                             std::string colorConfigPath) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .request = std::move(request),
                               .source = {},
                               .id = id,
                               .kind = ViewerRequestKind::Render,
                               .destination = destination,
                               .requestedAt = requestedAt,
                               .colorConfigPath = std::move(colorConfigPath)});
}

bool ViewerScheduler::probe(Document document, std::string source, std::uint64_t id, ViewerDestination destination,
                            std::chrono::steady_clock::time_point requestedAt, std::string colorConfigPath) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .request = {},
                               .source = std::move(source),
                               .id = id,
                               .kind = ViewerRequestKind::Probe,
                               .destination = destination,
                               .requestedAt = requestedAt,
                               .colorConfigPath = std::move(colorConfigPath)});
}

bool ViewerScheduler::requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                                   ViewerDestination destination, std::chrono::steady_clock::time_point requestedAt,
                                   std::string colorConfigPath) {
    auto snapshot = std::make_shared<const Document>(std::move(document));
    const auto revision = snapshot->stateRevision();
    std::lock_guard const lock(mutex_);
    if (first > last || !admissibleLocked(id, destination)) {
        dropLocked(destination, 1);
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
        dropLocked(destination, 1);
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
                                                 requestedAt,
                                                 std::move(colorConfigPath)},
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
        dropLocked(pending.destination, 1);
        return true;
    });
    for (auto it = ranges_.begin(); it != ranges_.end();) {
        if (it->second.work.id > id) {
            ++it;
        } else {
            dropLocked(it->first, frameCount(it->second.next, it->second.last));
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

void ViewerScheduler::cancel(std::uint64_t id, ViewerDestination destination) {
    std::lock_guard const lock(mutex_);
    const auto state = destinations_.find(destination);
    if (state != destinations_.end()) {
        if (id >= state->second.cancelFloor) {
            state->second.cancelFloor = id;
            state->second.cancelToken = nextToken_;
        }
        if (state->second.id <= id) {
            state->second.token = 0;
            state->second.cacheFloor = nextToken_;
        }
    }
    std::erase_if(interactive_, [&](const auto& pending) {
        if (pending.destination != destination || pending.id > id) {
            return false;
        }
        dropLocked(destination, 1);
        return true;
    });
    const auto range = ranges_.find(destination);
    if (range != ranges_.end() && range->second.work.id <= id) {
        dropLocked(destination, frameCount(range->second.next, range->second.last));
        ranges_.erase(range);
    }
}

void ViewerScheduler::retireDestination(ViewerDestination destination) {
    std::lock_guard const lock(mutex_);
    std::erase_if(interactive_, [&](const auto& pending) {
        if (pending.destination != destination) {
            return false;
        }
        dropLocked(destination, 1);
        return true;
    });
    dropRangeLocked(destination);
    destinations_.erase(destination);
    destinationCounts_.erase(destination);
}

bool ViewerScheduler::currentLocked(const ViewerScheduledRequest& request) const {
    const auto* state = stateLocked(request.destination);
    return (state != nullptr) && request.id >= cancelFloor_ && request.id >= state->cancelFloor &&
           request.id == state->id && request.token != 0 && request.token == state->token &&
           request.revision == state->revision;
}

bool ViewerScheduler::isCurrent(const ViewerScheduledRequest& request) const {
    std::lock_guard const lock(mutex_);
    return currentLocked(request);
}

bool ViewerScheduler::isCacheCurrent(const ViewerScheduledRequest& request) const {
    std::lock_guard const lock(mutex_);
    const auto* state = stateLocked(request.destination);
    return (state != nullptr) && request.token != 0 && request.token >= state->cacheFloor &&
           request.revision == state->revision && (request.id > cancelFloor_ || request.token >= cancelToken_) &&
           (request.id > state->cancelFloor || request.token >= state->cancelToken);
}

bool ViewerScheduler::complete(const ViewerScheduledRequest& request, bool published) {
    std::lock_guard const lock(mutex_);
    if (!currentLocked(request)) {
        ++counts_.staleRejected;
        ++destinationCounts_[request.destination].staleRejected;
        return false;
    }
    if (published) {
        ++counts_.completed;
        ++destinationCounts_[request.destination].completed;
    }
    return true;
}

void ViewerScheduler::clear() {
    std::lock_guard const lock(mutex_);
    for (const auto& pending : interactive_) {
        dropLocked(pending.destination, 1);
    }
    interactive_.clear();
    for (const auto& entry : ranges_) {
        dropLocked(entry.first, frameCount(entry.second.next, entry.second.last));
    }
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

ViewerSchedulerCounts ViewerScheduler::counts(ViewerDestination destination) const {
    std::lock_guard const lock(mutex_);
    const auto found = destinationCounts_.find(destination);
    auto result = found == destinationCounts_.end() ? ViewerSchedulerCounts{} : found->second;
    result.queued = 0;
    for (const auto& pending : interactive_) {
        if (pending.destination == destination) {
            ++result.queued;
        }
    }
    const auto range = ranges_.find(destination);
    if (range != ranges_.end()) {
        result.queued += frameCount(range->second.next, range->second.last);
    }
    return result;
}
}  // namespace nemo::eval
