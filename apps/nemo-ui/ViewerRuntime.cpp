#include "ViewerRuntime.hpp"
#include "nemo/gpu/Compile.hpp"

#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QVulkanFunctions>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

namespace nemo::ui {
namespace {
// Reserved destination ids: Interactive = 0 and Cache = 1 are the shared
// streams, so panel-instance destinations start above them.
constexpr std::uint32_t kFirstPanelDestination = 2;
// Cache-progress wake interval: background range admission waits for retained
// bytes to retire, and loading replay starts here before doubling its backoff.
// Neither waits for the GUI thread; foreground work wakes the worker immediately.
constexpr auto kCacheProgressInterval = std::chrono::milliseconds(2);
constexpr auto kMaximumReplayBackoff = std::chrono::milliseconds(64);
}  // namespace

ViewerRuntime::~ViewerRuntime() {
    quiesceForTeardown();
}

void ViewerRuntime::bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                              eval::ViewerCacheOptions cacheOptions,
                              std::vector<eval::GpuNodeContribution> contributions) {
    if (instance_)
        throw std::runtime_error("viewer runtime already initialized");
    cacheOptions_ = std::move(cacheOptions);
    instance_ = gpu::Instance::create({.validation = true, .extensions = extensions});
    device_ = gpu::Device::create(*instance_, {.externalSharing = true});
    presentationDevice_ = gpu::Device::create(
        *instance_, {.presentation = true, .externalSharing = true, .physical = device_->physical()});
    // Initial admission cap, not the user-configurable accounting/eviction
    // policy owned by #14. Zero means no allocation, not unlimited memory.
    allocator_ = gpu::Allocator::create(*instance_, *device_, {.max_device_bytes = 2ULL << 30});
    // ONE delivery queue for the whole application, borrowing the native owners
    // and the compiled shader directory the viewer already uses (issue #94). It
    // starts its own worker and resolves its effects lazily, so bootstrap stays
    // a device-creation step. The application's native inventory (issue #37)
    // travels with it, so a delivery and the viewer beside it evaluate the same
    // installed effects.
    delivery_ = std::make_unique<eval::DeliveryQueue>(*instance_, *device_, *allocator_, shaders, 8, contributions);
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(device_->physical(), VK_FORMAT_R8G8B8A8_UNORM, &format);
    filterLinear_ = (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    // The viewer worker owns its own copy of the same inventory for as long as
    // the runtime lives: every ViewerSession it builds (the initial one and each
    // OCIO configuration swap) is constructed from it, so a package's effects
    // can never be missing from a frame or present in only one of the two
    // workers. The capture is the worker's own immutable list — no member state,
    // no lock, and no lifetime shared with the caller's vector.
    worker_ = std::thread([this, shaders, contributions = std::move(contributions)] { run(shaders, contributions); });
}

bool ViewerRuntime::submit(Document document, eval::ViewIntent intent, std::uint64_t id,
                           eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.submit(std::move(document), std::move(intent), id, destination,
                                         std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A fresh submission replaces the destination's old mailbox result. A
        // cache-range submission uses its own destination and must not erase
        // what that destination currently shows.
        if (accepted)
            results_.erase(destination);
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

std::optional<eval::ViewerDestination> ViewerRuntime::allocateDestination(const QString& panelId) {
    std::lock_guard lock(mutex_);
    // A panel keeps the destination it already holds: reallocation would leak
    // its scheduler state and let a stale presentation outlive its panel.
    if (const auto existing = panelDestinations_.find(panelId); existing != panelDestinations_.end())
        return existing->second;
    for (std::uint32_t id = kFirstPanelDestination; id < eval::kMaxViewerDestinations; ++id) {
        const auto destination = static_cast<eval::ViewerDestination>(id);
        const bool used = std::any_of(panelDestinations_.begin(), panelDestinations_.end(),
                                      [destination](const auto& entry) { return entry.second == destination; });
        if (used)
            continue;
        panelDestinations_.emplace(panelId, destination);
        return destination;
    }
    // Every bounded panel destination is live; refuse rather than alias one.
    return std::nullopt;
}

bool ViewerRuntime::retireDestination(eval::ViewerDestination destination) {
    std::unique_lock lock(mutex_);
    // Only an allocated panel destination is retirable. The reserved
    // interactive and cache streams are never panel-owned, and an unknown
    // destination must not drop another panel's state.
    const auto allocated = std::find_if(panelDestinations_.begin(), panelDestinations_.end(),
                                        [destination](const auto& entry) { return entry.second == destination; });
    if (stopping_ || allocated == panelDestinations_.end())
        return false;
    panelDestinations_.erase(allocated);
    results_.erase(destination);
    replayResults_.erase(destination);
    // Scheduler state goes immediately: an in-flight request for the retired
    // destination is rejected at publication instead of reaching the mailbox.
    scheduler_.retireDestination(destination);
    // The session is worker-owned, so its freshness state is erased by the
    // worker before any later work executes.
    retireQueue_.push_back(destination);
    lock.unlock();
    ready_.notify_all();
    return true;
}

bool ViewerRuntime::probe(Document document, std::string source, std::uint64_t id, eval::ViewerDestination destination,
                          std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.probe(std::move(document), std::move(source), id, destination,
                                        std::chrono::steady_clock::now(), std::move(colorConfigPath));
        if (accepted)
            results_.erase(destination);
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::describe(Document document, EvaluationRequest request, std::uint64_t id,
                             eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.describe(std::move(document), std::move(request), id, destination,
                                           std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A description is metadata for the next request, not a result to show:
        // it never displaces the destination's current frame.
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::sample(Document document, EvaluationRequest request, std::uint64_t id,
                           eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.sample(std::move(document), std::move(request), id, destination,
                                         std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A pick publishes a value, not a frame: like a description it must not
        // displace what the destination currently shows.
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

bool ViewerRuntime::requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                                 eval::ViewerDestination destination, std::string colorConfigPath) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            accepted = scheduler_.requestRange(std::move(document), std::move(request), first, last, id, destination,
                                               std::chrono::steady_clock::now(), std::move(colorConfigPath));
    }
    if (accepted)
        ready_.notify_one();
    return accepted;
}

std::optional<std::uint64_t> ViewerRuntime::prepareReplay(const eval::ViewerPlaybackContext& context,
                                                          eval::ViewIntent intent, std::uint64_t id,
                                                          eval::ViewerDestination destination,
                                                          std::string colorConfigPath) {
    std::optional<std::uint64_t> revision;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            revision = scheduler_.preparePlayback(context, std::move(intent), id, destination,
                                                  std::chrono::steady_clock::now(), std::move(colorConfigPath));
        // A preparation never displaces the destination's current frame or its
        // latest-wins mailbox result: that is exactly what makes admitting
        // frame N+1 unable to retire frame N.
    }
    if (revision)
        ready_.notify_one();
    return revision;
}

void ViewerRuntime::cancelPlayback(eval::ViewerDestination destination) {
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            scheduler_.cancelPlayback(destination);
        // Prepared frames of the retired window are dropped here rather than
        // delivered late; the worker's pending retries are rejected by the same
        // identity check when their backoff elapses.
        replayResults_.erase(destination);
    }
    ready_.notify_all();
}

void ViewerRuntime::cancel(std::uint64_t id) {
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            scheduler_.cancel(id);
        std::erase_if(results_, [this](const auto& entry) { return !scheduler_.isCurrent(entry.second.request); });
        for (auto& entry : replayResults_) {
            std::erase_if(entry.second, [this](const auto& pending) { return !scheduler_.isCurrent(pending.request); });
        }
    }
    ready_.notify_all();
}

void ViewerRuntime::cancel(std::uint64_t id, eval::ViewerDestination destination) {
    {
        std::lock_guard lock(mutex_);
        if (!stopping_)
            scheduler_.cancel(id, destination);
        // Dropping queued work is not enough: a result already in this
        // destination's mailbox must not reach its panel after the panel's
        // own cancellation watermark moved. Other destinations keep theirs.
        std::erase_if(results_, [this, destination](const auto& entry) {
            return entry.first == destination && !scheduler_.isCurrent(entry.second.request);
        });
        if (const auto replay = replayResults_.find(destination); replay != replayResults_.end()) {
            std::erase_if(replay->second,
                          [this](const auto& pending) { return !scheduler_.isCurrent(pending.request); });
        }
    }
    ready_.notify_all();
}

ViewerRuntimeCounts ViewerRuntime::composeCountsLocked(const eval::ViewerSchedulerCounts& counts) const {
    // The viewer cache is shared by every destination; only the scheduler
    // counters are destination-scoped.
    if (session_) {
        if (auto snapshot = session_->tryCacheCounts())
            cacheCounts_ = std::move(*snapshot);
    }
    return ViewerRuntimeCounts{counts.queued,
                               counts.dropped,
                               counts.staleRejected,
                               counts.completed,
                               cacheCounts_.pendingFrames + cacheCounts_.activeFrames,
                               cacheCounts_.admissionDropped + cacheCounts_.admissionRejected,
                               cacheCounts_.errors,
                               cacheCounts_.published,
                               cacheCounts_.lastError,
                               cacheCounts_.diskBytes,
                               cacheCounts_.compressedHotBytes,
                               cacheCounts_.residentFrames,
                               cacheCounts_.residentBytes,
                               cacheCounts_.activeFrames};
}

ViewerRuntimeCounts ViewerRuntime::counts() const {
    std::lock_guard lock(mutex_);
    return composeCountsLocked(scheduler_.counts());
}

ViewerRuntimeCounts ViewerRuntime::counts(eval::ViewerDestination destination) const {
    std::lock_guard lock(mutex_);
    return composeCountsLocked(scheduler_.counts(destination));
}

std::optional<ViewerWorkResult> ViewerRuntime::takeResult(eval::ViewerDestination destination) {
    std::lock_guard lock(mutex_);
    // The latest-wins mailbox is consumed first: a seek/edit answer is what the
    // panel wants now, and its ordered replay backlog is obsolete beside it.
    if (const auto found = results_.find(destination); found != results_.end()) {
        auto result = std::move(found->second.result);
        results_.erase(found);
        return result;
    }
    const auto found = replayResults_.find(destination);
    if (found == replayResults_.end())
        return std::nullopt;
    // A prepared frame whose window has since been superseded is dropped here
    // rather than delivered late.
    while (!found->second.empty()) {
        auto result = std::move(found->second.front().result);
        const bool current = scheduler_.isCurrent(found->second.front().request);
        found->second.pop_front();
        if (current)
            return result;
    }
    replayResults_.erase(found);
    return std::nullopt;
}

bool ViewerRuntime::publish(ViewerWorkResult result, const Pending& pending) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        // complete() performs the destination-local identity check while the
        // runtime lock is held, so cancel() cannot race a publication into the
        // mailbox. A rejected result is dropped; GPU resource retirement is
        // still solely the shared submission mechanism's responsibility.
        if (!stopping_ && scheduler_.complete(pending, true)) {
            results_.insert_or_assign(pending.destination, Published{pending, std::move(result)});
            accepted = true;
        } else if (stopping_) {
            (void)scheduler_.complete(pending, false);
        }
    }
    if (accepted)
        emit resultReady();
    return accepted;
}

void ViewerRuntime::finishRange(const Pending& pending, bool cacheAccepted) {
    std::lock_guard lock(mutex_);
    // Admission is not persistence. Writer progress/errors are reported
    // separately by counts(), including after this evaluation completes.
    (void)scheduler_.complete(pending, cacheAccepted);
}

bool ViewerRuntime::publishReplay(ViewerWorkResult result, const Pending& pending) {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        // The ordered playback identity check: a frame stays current for as
        // long as its destination's window does, so a successor never displaces
        // it, while a seek/edit/cancel/retirement retires it here.
        if (!stopping_ && scheduler_.complete(pending, true)) {
            replayResults_[pending.destination].push_back(Published{pending, std::move(result)});
            accepted = true;
        } else if (stopping_) {
            (void)scheduler_.complete(pending, false);
        }
    }
    if (accepted)
        emit resultReady();
    return accepted;
}

void ViewerRuntime::keepPendingReplay(Pending pending, std::chrono::milliseconds backoff) {
    // Bounded by the scheduler's own playback window: every entry here is one
    // in-flight Replay unit whose retained representation is still loading, and
    // a unit the scheduler has since superseded is dropped instead of retried.
    if (!scheduler_.isCurrent(pending)) {
        (void)scheduler_.complete(pending, false);
        return;
    }
    const auto delay = std::min(backoff, kMaximumReplayBackoff);
    PendingReplay entry{.request = std::move(pending),
                        .retryAt = std::chrono::steady_clock::now() + delay,
                        .backoff = std::min(delay * 2, kMaximumReplayBackoff)};
    // Kept in retry-deadline order so the worker always wakes for the earliest
    // preparation that may have become resident.
    const auto position = std::find_if(replayPending_.begin(), replayPending_.end(),
                                       [&](const auto& queued) { return queued.retryAt > entry.retryAt; });
    replayPending_.insert(position, std::move(entry));
}

void ViewerRuntime::refreshColorConfig() {
    {
        const std::lock_guard lock(mutex_);
        colorRefresh_ = true;
    }
    ready_.notify_all();
}

void ViewerRuntime::run(const std::filesystem::path& shaders,
                        const std::vector<eval::GpuNodeContribution>& contributions) {
    std::unique_ptr<eval::ViewerSession> session;
    // Authored color configuration the current worker session was built with;
    // a different request config replaces the session, never the environment.
    std::string sessionColorConfig;
    std::vector<std::uint32_t> presentationShader;
    std::vector<std::uint32_t> replayShader;
    for (;;) {
        Pending pending;
        std::chrono::milliseconds retryBackoff = kCacheProgressInterval;
        bool retry = false;
        bool hasPending = false;
        bool colorRefresh = false;
        std::vector<eval::ViewerDestination> retired;
        // Range construction is demand-driven background work, not a history
        // of float frames waiting behind the encoder or a slow disk. Keep its
        // next frame in the scheduler while accepted cache work owns bytes.
        // Foreground requests/replay remain serviceable throughout this wait.
        const bool admitRange = !session || session->cacheCounts().pendingBytes == 0;
        {
            std::unique_lock lock(mutex_);
            const bool waitingOnRange = !admitRange && scheduler_.hasWork();
            const auto work = [this, admitRange, waitingOnRange] {
                return stopping_ || scheduler_.hasWork(admitRange || !waitingOnRange) || !retireQueue_.empty() ||
                       colorRefresh_;
            };
            auto wakeAt = std::chrono::steady_clock::time_point::max();
            if (!replayPending_.empty())
                wakeAt = replayPending_.front().retryAt;
            if (waitingOnRange)
                wakeAt = std::min(wakeAt, std::chrono::steady_clock::now() + kCacheProgressInterval);
            if (wakeAt == std::chrono::steady_clock::time_point::max())
                ready_.wait(lock, work);
            else
                (void)ready_.wait_until(lock, wakeAt, work);
            if (stopping_)
                break;
            retired.swap(retireQueue_);
            colorRefresh = colorRefresh_;
            colorRefresh_ = false;
            const auto now = std::chrono::steady_clock::now();
            if (!replayPending_.empty() && replayPending_.front().retryAt <= now) {
                // The oldest pending preparation is due: its frame is earlier in
                // the transport than anything newly admitted, so it goes first.
                pending = std::move(replayPending_.front().request);
                retryBackoff = replayPending_.front().backoff;
                replayPending_.pop_front();
                retry = true;
                hasPending = true;
            } else if (auto next = scheduler_.take(admitRange)) {
                pending = std::move(*next);
                hasPending = true;
            }
        }
        if (session && colorRefresh) {
            // Worker-owned: the retained programs/processors are retired here,
            // never on the caller's thread.
            session->refreshColorConfig();
        }
        // A retired destination's session freshness and Auto resolution state
        // are erased before any later work runs, so its in-flight request can
        // never be published as current. With no session yet there is nothing
        // to forget.
        for (const auto destination : retired)
            resolution_.erase(destination);
        if (session) {
            for (const auto destination : retired)
                session->retireDestination(destination);
        }
        if (!hasPending)
            continue;
        // A preparation the scheduler has since superseded — a seek, an edit, a
        // stop or a destination retirement — is dropped before it does any work.
        if (retry && !scheduler_.isCurrent(pending)) {
            (void)scheduler_.complete(pending, false);
            continue;
        }
        try {
            if (!session || sessionColorConfig != pending.colorConfigPath) {
                // The project's authored config replaces the worker-owned
                // ViewerSession; an empty path keeps the OCIO environment
                // fallback. No process-global state is mutated and the GUI
                // thread never waits for the swap. The replacement is built from
                // the SAME native inventory (issue #37), so swapping the color
                // configuration can never drop a package's effects.
                auto configured = std::make_unique<eval::ViewerSession>(*instance_, *device_, *allocator_, shaders,
                                                                        pending.colorConfigPath, contributions);
                // Release the old cache's exclusive writer lease before the
                // replacement opens the same directory. GUI counts must never
                // retain the worker-owned pointer across its destruction.
                {
                    std::lock_guard lock(mutex_);
                    session_ = nullptr;
                }
                session.reset();
                configured->configureCache(cacheOptions_);
                session = std::move(configured);
                sessionColorConfig = pending.colorConfigPath;
                std::lock_guard lock(mutex_);
                session_ = session.get();
            }
            execute(pending, *session, shaders, presentationShader, replayShader);
        } catch (const eval::ViewerReplayPending&) {
            // A valid retained representation for this exact demand exists but is
            // still loading. It is not a miss: the graph is never evaluated for
            // it, and the preparation is kept for a later retry while the worker
            // continues with everything else.
            keepPendingReplay(std::move(pending), retryBackoff);
        } catch (const eval::ViewUnavailable& error) {
            // The view addresses nothing the current frame carries: it has no
            // frame to publish, so the panel must not keep presenting an image
            // this selection does not produce — and the description that
            // refused it travels with the answer, so the selectors offer the
            // channels this frame really carries.
            if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                if (scheduler_.complete(pending, false))
                    emit rangeFailed(QStringLiteral("Cache frame %1: %2")
                                         .arg(std::get<EvaluationRequest>(pending.demand).localTime)
                                         .arg(QString::fromUtf8(error.what())),
                                     pending.id);
            } else if (pending.kind == eval::ViewerRequestKind::Replay) {
                // A read-ahead frame the retained index refuses is a neighbour
                // the transport does not walk to: it is dropped, never rendered.
                publishReplay(ViewerReplayMiss{pending.intent().localTime, pending.id}, pending);
            } else {
                const auto& intent = pending.intent();
                publish(ViewerUnavailableView{error.what(), pending.id, intent.target, intent.localTime,
                                              pending.document->stateRevision(), error.description},
                        pending);
            }
        } catch (const std::exception& error) {
            if (pending.kind == eval::ViewerRequestKind::CacheRange) {
                if (scheduler_.complete(pending, false))
                    emit rangeFailed(QStringLiteral("Cache frame %1: %2")
                                         .arg(std::get<EvaluationRequest>(pending.demand).localTime)
                                         .arg(QString::fromUtf8(error.what())),
                                     pending.id);
            } else if (pending.kind == eval::ViewerRequestKind::Replay) {
                publishReplay(ViewerReplayMiss{pending.intent().localTime, pending.id}, pending);
            } else {
                publish(ViewerFailure{error.what(), pending.id}, pending);
            }
        }
        flushValidation();
    }
    std::lock_guard lock(mutex_);
    session_ = nullptr;
}

void ViewerRuntime::execute(Pending& pending, eval::ViewerSession& session, const std::filesystem::path& shaders,
                            std::vector<std::uint32_t>& presentationShader, std::vector<std::uint32_t>& replayShader) {
    if (pending.kind == eval::ViewerRequestKind::Probe) {
        publish(SourceProbeResult{session.probeSource(*pending.document, pending.source), pending.id}, pending);
        return;
    }
    if (pending.kind == eval::ViewerRequestKind::Describe) {
        publish(ViewerTargetDescription{session.describe(*pending.document, pending.request()), pending.id}, pending);
        return;
    }
    if (pending.kind == eval::ViewerRequestKind::Sample) {
        // One on-demand working-space pixel: the same session, the same
        // plan mechanism and the same device as a render, so a pick can
        // never disagree with the frame beside it (issue #102).
        const auto sample = session.sampleWorkingPixel(*pending.document, pending.request(), 10'000'000'000ULL);
        publish(ViewerWorkingSample{sample, pending.id}, pending);
        return;
    }
    if (pending.kind == eval::ViewerRequestKind::Replay) {
        executeReplay(pending, session, shaders, presentationShader, replayShader);
        return;
    }
    auto publicationGuard = [this, pending] { return scheduler_.isCacheCurrent(pending); };
    if (pending.kind == eval::ViewerRequestKind::CacheRange) {
        // Cache frames are a concrete, coverage-stating request that
        // never replaces the interactive view, so they are rendered
        // as such (issue #98 keeps the distinct headless contract).
        const auto frame = session.render(*pending.document, pending.request(), 10'000'000'000ULL, pending.id,
                                          pending.destination, std::move(publicationGuard));
        finishRange(pending, frame.cacheHit || frame.cacheQueued);
        return;
    }
    // Foreground construction scope (issue #106): while it is alive the
    // session's cache writer defers its own GPU submissions, so this frame's
    // evaluation and its presentation construction win the shared device queue.
    // It is a construction gate, not a device wait, and it is released on every
    // path — including a thrown ViewerReplayPending, which leaves no live frame.
    auto foreground = session.foregroundScope();
    // ONE worker job resolves the view against the current
    // frame's described image, states the demand from it and
    // executes the plan it resolved (issue #98). The Auto
    // hysteresis state is this destination's own, retained
    // across frames. A known valid retained representation is served from it
    // without evaluating the graph; a representation that is still loading
    // throws ViewerReplayPending instead of evaluating.
    const auto frame = session.render(*pending.document, pending.intent(), resolution_[pending.destination],
                                      10'000'000'000ULL, pending.id, pending.destination, std::move(publicationGuard));
    bool current = false;
    {
        std::lock_guard lock(mutex_);
        current = !stopping_ && scheduler_.isCurrent(pending);
        if (!current)
            (void)scheduler_.complete(pending, false);
    }
    if (!current)
        return;
    auto presentation = presentFrame(frame, shaders, presentationShader, replayShader);
    auto result = std::make_shared<ViewerResult>(ViewerResult{
        std::move(presentation), frame.layout, frame.description, frame.request, pending.id, frame.revision,
        frame.cacheHit, static_cast<bool>(frame.replay), pending.requestedAt, pending.destination});
    publish(std::shared_ptr<const ViewerResult>(std::move(result)), pending);
}

void ViewerRuntime::executeReplay(const Pending& pending, eval::ViewerSession& session,
                                  const std::filesystem::path& shaders, std::vector<std::uint32_t>& presentationShader,
                                  std::vector<std::uint32_t>& replayShader) {
    // Replay-only: the retained display-cache representation or nothing. The
    // graph is never evaluated here, so a speculative neighbour can never
    // become a render, and a loading representation keeps the preparation
    // pending instead of substituting a live frame.
    // The same foreground construction gate as the live path: a replay frame's
    // presentation construction is ordered ahead of the writer's own encode and
    // upload submissions. It never waits for anything.
    auto foreground = session.foregroundScope();
    // The admission-time revision travels with the unit and IS the snapshot's
    // identity, so the retained record is validated without fingerprinting the
    // document again on an ordinary playback tick.
    auto frame =
        session.replay(pending.intent(), pending.revision, resolution_[pending.destination], pending.destination);
    if (!frame) {
        publishReplay(ViewerReplayMiss{pending.intent().localTime, pending.id}, pending);
        return;
    }
    auto presentation = presentFrame(*frame, shaders, presentationShader, replayShader);
    auto result = std::make_shared<ViewerResult>(
        ViewerResult{std::move(presentation), frame->layout, frame->description, frame->request, pending.id,
                     frame->revision, true, true, pending.requestedAt, pending.destination});
    publishReplay(std::shared_ptr<const ViewerResult>(std::move(result)), pending);
}

gpu::ViewerPresentation ViewerRuntime::presentFrame(const eval::ViewerFrame& frame,
                                                    const std::filesystem::path& shaders,
                                                    std::vector<std::uint32_t>& presentationShader,
                                                    std::vector<std::uint32_t>& replayShader) {
    if (frame.replay) {
        // One direct BC7-sample-to-shared-RGBA8 pass: the compressed frame is
        // sampled straight into the established presentation surface, with the
        // same external-memory/semaphore handoff the live path uses. Its own
        // module is required because a BC7 block-compressed image cannot bind
        // to the live module's storage-image read.
        if (replayShader.empty())
            replayShader = gpu::loadSpirv(shaders / "viewerPresentationBc7.spv");
        return gpu::prepareViewerPresentation(*device_, *allocator_, *presentationDevice_, *frame.replay, replayShader,
                                              frame.presentationChannel);
    }
    if (presentationShader.empty())
        presentationShader = gpu::loadSpirv(shaders / "viewerPresentation.spv");
    // The isolation the frame's own view asked for: applied in the presentation
    // copy only, never in evaluation or the cache, and never taken from another
    // destination.
    return gpu::prepareViewerPresentation(*device_, *allocator_, *presentationDevice_, *frame.image, frame.layout.color,
                                          presentationShader, frame.presentationChannel);
}

QString ViewerRuntime::attachToWindow(QQuickWindow* window) {
    if (!presentationDevice_ || !window)
        return QStringLiteral("viewer requires an initialized device and a Qt window");
    // One presentation host serves every panel, so adopting the same window
    // again is a no-op and a second window is refused.
    if (attachedWindow_ == window)
        return {};
    if (attachedWindow_)
        return QStringLiteral("viewer presentation is already attached to another Qt window");
    qtInstance_.setVkInstance(instance_->handle());
    qtInstance_.setApiVersion(QVersionNumber(1, 3));
    if (!qtInstance_.create())
        return QStringLiteral("Qt could not adopt the application Vulkan instance");
    window->setVulkanInstance(&qtInstance_);
    window->setGraphicsDevice(
        QQuickGraphicsDevice::fromDeviceObjects(presentationDevice_->physical(), presentationDevice_->handle(),
                                                static_cast<int>(presentationDevice_->graphics_family()), 0));
    window->create();
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(window);
    if (surface == VK_NULL_HANDLE)
        return QStringLiteral("could not create a Vulkan surface on %1").arg(QGuiApplication::platformName());
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        vkGetInstanceProcAddr(instance_->handle(), "vkGetPhysicalDeviceSurfaceSupportKHR"));
    VkBool32 supported = VK_FALSE;
    if (!query ||
        query(presentationDevice_->physical(), presentationDevice_->graphics_family(), surface, &supported) !=
            VK_SUCCESS ||
        !supported)
        return QStringLiteral("graphics queue family %1 cannot present to this window")
            .arg(presentationDevice_->graphics_family());
    // Qt's render thread owns both connections for the runtime's lifetime:
    // beforeRendering pins every panel's presentation for the current frame
    // slot, and frameSwapped reports each destination newly on screen. Waking
    // the worker belongs to the mailbox signal, never to Qt's frame boundary.
    connect(
        window, &QQuickWindow::beforeRendering, this,
        [this, window] { windowState_.beginFrame(window, *presentationDevice_); }, Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            for (const auto& presented : windowState_.takeNewPresentedFrames()) {
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                 presented.result->requestedAt)
                                           .count();
                emit framePresented(presented.destination, static_cast<int>(presented.result->request.localTime),
                                    presented.result->frame.width, presented.result->frame.height,
                                    presented.result->cacheHit, elapsed);
            }
        },
        Qt::DirectConnection);
    window->show();
    attachedWindow_ = window;
    return {};
}

void ViewerRuntime::flushValidation() {
    if (!instance_)
        return;
    for (const auto& message : instance_->take_debug_messages())
        if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::cerr << "vulkan: " << message.text << '\n';
}

eval::DeliveryQueue& ViewerRuntime::deliveryQueue() {
    if (!delivery_)
        throw std::runtime_error("viewer runtime delivery queue is unavailable before bootstrap");
    return *delivery_;
}

void ViewerRuntime::stopWorker() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        scheduler_.clear();
        results_.clear();
        replayResults_.clear();
        // replayPending_ is worker-owned; it is dropped after the join below,
        // never while the worker may still be reading it.
    }
    ready_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void ViewerRuntime::quiesceForTeardown() {
    // Delivery first: its worker joins, its in-flight submissions are waited on
    // and its retained staging is released while the device still exists. Only
    // then are the viewer worker and the devices torn down.
    delivery_.reset();
    stopWorker();
    // Waiting for device idle alone does not retire completion-owned tokens.
    // Release producer submissions while their imported consumer images and
    // semaphores still have a valid logical device.
    if (device_)
        device_->submissions(device_->graphics_family()).drain();
    // Shutdown only: both execution and Qt's render loop must be stopped.
    // Qt's ordinary device-wide waits never touch the execution device.
    for (const auto* device : {device_.get(), presentationDevice_.get()}) {
        if (!device)
            continue;
        VkResult result;
        while ((result = vkDeviceWaitIdle(device->handle())) != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            // As with SubmissionQueue::drain, a transient wait failure is
            // never permission to release resources still used by the GPU.
            std::cerr << "viewer teardown: vkDeviceWaitIdle returned " << result << "; retaining owners and retrying\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (result == VK_ERROR_DEVICE_LOST)
            std::cerr << "viewer teardown: Vulkan device lost\n";
    }
    {
        std::lock_guard lock(mutex_);
        results_.clear();
        replayResults_.clear();
        replayPending_.clear();
    }
    flushValidation();
}
}  // namespace nemo::ui
