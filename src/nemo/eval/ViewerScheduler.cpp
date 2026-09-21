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

ViewerPlaybackContext::ViewerPlaybackContext(Document input)
    : document(std::make_shared<const Document>(std::move(input))), revision(document->stateRevision()) {}

ViewerScheduler::ViewerScheduler(std::size_t interactiveCapacity, std::size_t playbackWindow)
    : interactiveCapacity_(interactiveCapacity), playbackWindow_(playbackWindow) {
    if (interactiveCapacity_ == 0) {
        throw std::invalid_argument("viewer scheduler interactive capacity must be positive");
    }
    if (playbackWindow_ == 0) {
        throw std::invalid_argument("viewer scheduler playback window must be positive");
    }
}

const ViewerScheduler::DestinationState* ViewerScheduler::stateLocked(ViewerDestination destination) const {
    const auto found = destinations_.find(destination);
    return found == destinations_.end() ? nullptr : &found->second;
}

ViewerScheduler::DestinationState& ViewerScheduler::stateForLocked(ViewerDestination destination) {
    const auto found = destinations_.find(destination);
    if (found != destinations_.end()) {
        return found->second;
    }
    // A destination's first unit — interactive or playback — creates its state
    // with a fresh epoch, so a re-created destination id can never revive a
    // retired window's in-flight replay frames.
    auto& state = destinations_[destination];
    state.playbackEpoch = nextPlaybackEpoch_++;
    return state;
}

void ViewerScheduler::dropPlaybackLocked(ViewerDestination destination, bool countDropped) {
    std::size_t dropped = 0;
    std::erase_if(playback_, [&](const auto& pending) {
        if (pending.destination != destination) {
            return false;
        }
        ++dropped;
        return true;
    });
    if (auto found = destinations_.find(destination); found != destinations_.end()) {
        found->second.playbackQueued -= std::min(found->second.playbackQueued, dropped);
    }
    if (countDropped && dropped != 0) {
        dropLocked(destination, dropped);
    }
}

void ViewerScheduler::supersedePlaybackLocked(ViewerDestination destination) {
    dropPlaybackLocked(destination, true);
    if (auto found = destinations_.find(destination); found != destinations_.end()) {
        found->second.playbackEpoch = nextPlaybackEpoch_++;
    }
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
    auto& state = stateForLocked(destination);
    work.token = nextToken_++;
    state.id = work.id;
    state.token = work.token;
    state.revision = work.revision;
    dropRangeLocked(destination);
    // A latest-wins submission retires the destination's ordered playback
    // window: a seek, an edit, a new probe or a metadata query states what the
    // panel wants NOW, so obsolete read-ahead is dropped rather than published
    // after it.
    supersedePlaybackLocked(destination);
    if (previous != interactive_.end()) {
        *previous = std::move(work);
        dropLocked(destination, 1);
    } else {
        interactive_.push_back(std::move(work));
    }
    return true;
}

bool ViewerScheduler::submit(Document document, ViewIntent intent, std::uint64_t id, ViewerDestination destination,
                             std::chrono::steady_clock::time_point requestedAt, std::string colorConfigPath) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .demand = std::move(intent),
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
                               .demand = EvaluationRequest{},
                               .source = std::move(source),
                               .id = id,
                               .kind = ViewerRequestKind::Probe,
                               .destination = destination,
                               .requestedAt = requestedAt,
                               .colorConfigPath = std::move(colorConfigPath)});
}

bool ViewerScheduler::describe(Document document, EvaluationRequest request, std::uint64_t id,
                               ViewerDestination destination, std::chrono::steady_clock::time_point requestedAt,
                               std::string colorConfigPath) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .demand = std::move(request),
                               .source = {},
                               .id = id,
                               .kind = ViewerRequestKind::Describe,
                               .destination = destination,
                               .requestedAt = requestedAt,
                               .colorConfigPath = std::move(colorConfigPath)});
}

bool ViewerScheduler::sample(Document document, EvaluationRequest request, std::uint64_t id,
                             ViewerDestination destination, std::chrono::steady_clock::time_point requestedAt,
                             std::string colorConfigPath) {
    return enqueueInteractive({.document = std::make_shared<const Document>(std::move(document)),
                               .demand = std::move(request),
                               .source = {},
                               .id = id,
                               .kind = ViewerRequestKind::Sample,
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
    auto& state = stateForLocked(destination);
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
    // A range states a new coverage demand for this destination, so it retires
    // the destination's ordered playback window exactly as an interactive
    // submission does.
    supersedePlaybackLocked(destination);
    ranges_.insert_or_assign(destination, Range{{.document = std::move(snapshot),
                                                 .demand = std::move(request),
                                                 .source = {},
                                                 .id = id,
                                                 .token = token,
                                                 .revision = revision,
                                                 .kind = ViewerRequestKind::CacheRange,
                                                 .destination = destination,
                                                 .requestedAt = requestedAt,
                                                 .colorConfigPath = std::move(colorConfigPath)},
                                                first,
                                                last});
    return true;
}

std::optional<std::uint64_t> ViewerScheduler::preparePlayback(const ViewerPlaybackContext& context, ViewIntent intent,
                                                              std::uint64_t id, ViewerDestination destination,
                                                              std::chrono::steady_clock::time_point requestedAt,
                                                              std::string colorConfigPath) {
    const auto revision = context.revision;
    std::lock_guard const lock(mutex_);
    const auto* existing = stateLocked(destination);
    // The destination's window bound is the only count bound the ordered
    // playback path needs; the destination table is bounded too, so the total
    // retained preparation stays bounded without a second accounting owner.
    if (!admissibleLocked(id, destination) || (existing != nullptr && existing->playbackQueued >= playbackWindow_)) {
        dropLocked(destination, 1);
        return std::nullopt;
    }
    auto& state = stateForLocked(destination);
    state.playbackQueued += 1;
    playback_.push_back({.document = context.document,
                         .demand = std::move(intent),
                         .source = {},
                         .id = id,
                         .token = nextToken_++,
                         .revision = revision,
                         .kind = ViewerRequestKind::Replay,
                         .destination = destination,
                         .playbackEpoch = state.playbackEpoch,
                         .requestedAt = requestedAt,
                         .colorConfigPath = std::move(colorConfigPath)});
    return revision;
}

std::optional<ViewerScheduledRequest> ViewerScheduler::take(bool admitRange) {
    std::lock_guard const lock(mutex_);
    if (!interactive_.empty()) {
        auto result = std::move(interactive_.front());
        interactive_.pop_front();
        return result;
    }
    // Ordered playback frames are foreground work: they are served before the
    // background range, and in admission order so a destination's read-ahead
    // stays in transport order.
    if (!playback_.empty()) {
        auto result = std::move(playback_.front());
        playback_.pop_front();
        if (auto found = destinations_.find(result.destination); found != destinations_.end()) {
            found->second.playbackQueued -= std::min<std::size_t>(found->second.playbackQueued, 1);
        }
        return result;
    }
    if (!admitRange || ranges_.empty()) {
        return std::nullopt;
    }
    auto found = ranges_.begin();
    auto& range = found->second;
    auto result = range.work;
    std::get<EvaluationRequest>(result.demand).localTime = range.next;
    if (range.next == range.last) {
        ranges_.erase(found);
    } else {
        ++range.next;
    }
    return result;
}

bool ViewerScheduler::hasWork(bool admitRange) const {
    std::lock_guard const lock(mutex_);
    return !interactive_.empty() || !playback_.empty() || (admitRange && !ranges_.empty());
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
    // Playback frames below the watermark are obsolete; the ones above it were
    // admitted after this cancellation and stay current.
    std::erase_if(playback_, [&](const auto& pending) {
        if (pending.id > id) {
            return false;
        }
        dropLocked(pending.destination, 1);
        if (auto found = destinations_.find(pending.destination); found != destinations_.end()) {
            found->second.playbackQueued -= std::min<std::size_t>(found->second.playbackQueued, 1);
        }
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
            // The destination's latest-wins state was retired, so its whole
            // ordered window is obsolete with it.
            entry.second.playbackEpoch = nextPlaybackEpoch_++;
        }
    }
}

void ViewerScheduler::cancel(std::uint64_t id, ViewerDestination destination) {
    std::lock_guard const lock(mutex_);
    const auto state = destinations_.find(destination);
    const bool supersedes = state == destinations_.end() || id >= state->second.cancelFloor;
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
    // A destination-scoped cancellation that moves the panel's watermark is its
    // supersession boundary: its queued read-ahead and its in-flight replay
    // frames are retired, without touching any other destination.
    if (supersedes) {
        dropPlaybackLocked(destination, true);
        if (state != destinations_.end()) {
            state->second.playbackEpoch = nextPlaybackEpoch_++;
        }
    }
    const auto range = ranges_.find(destination);
    if (range != ranges_.end() && range->second.work.id <= id) {
        dropLocked(destination, frameCount(range->second.next, range->second.last));
        ranges_.erase(range);
    }
}

void ViewerScheduler::cancelPlayback(ViewerDestination destination) {
    std::lock_guard const lock(mutex_);
    // Only the window moves: the destination's latest-wins identity is exactly
    // what must survive, so a panel can retire read-ahead while the frame it is
    // displaying stays current.
    dropPlaybackLocked(destination, true);
    if (auto found = destinations_.find(destination); found != destinations_.end()) {
        found->second.playbackEpoch = nextPlaybackEpoch_++;
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
    dropPlaybackLocked(destination, true);
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

bool ViewerScheduler::playbackCurrentLocked(const ViewerScheduledRequest& request) const {
    const auto* state = stateLocked(request.destination);
    return (state != nullptr) && request.token != 0 && request.playbackEpoch == state->playbackEpoch &&
           request.id >= cancelFloor_ && request.id >= state->cancelFloor;
}

bool ViewerScheduler::isCurrent(const ViewerScheduledRequest& request) const {
    std::lock_guard const lock(mutex_);
    // An ordered playback frame is current while its destination's window is:
    // its own successor never retires it, and only a latest-wins submission,
    // cancellation or retirement does.
    return request.kind == ViewerRequestKind::Replay ? playbackCurrentLocked(request) : currentLocked(request);
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
    const bool current =
        request.kind == ViewerRequestKind::Replay ? playbackCurrentLocked(request) : currentLocked(request);
    if (!current) {
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
    for (const auto& pending : playback_) {
        dropLocked(pending.destination, 1);
    }
    playback_.clear();
    for (const auto& entry : ranges_) {
        dropLocked(entry.first, frameCount(entry.second.next, entry.second.last));
    }
    ranges_.clear();
    for (auto& entry : destinations_) {
        entry.second.token = 0;
        entry.second.cacheFloor = nextToken_;
        entry.second.playbackQueued = 0;
        entry.second.playbackEpoch = nextPlaybackEpoch_++;
    }
}

ViewerSchedulerCounts ViewerScheduler::counts() const {
    std::lock_guard const lock(mutex_);
    auto result = counts_;
    result.queued = interactive_.size() + playback_.size() + remainingRangeLocked();
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
    for (const auto& pending : playback_) {
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
