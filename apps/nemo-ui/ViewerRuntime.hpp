#pragma once

#include "ViewerItem.hpp"
#include "nemo/eval/DeliveryJob.hpp"
#include "nemo/eval/GpuContribution.hpp"
#include "nemo/eval/Viewer.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/eval/ViewerScheduler.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"

#include <QObject>
#include <QString>
#include <QVulkanInstance>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

class QQuickWindow;

namespace nemo::ui {

struct ViewerResult {
    gpu::ViewerPresentation presentation;
    ImageLayout frame;
    // The described output this result was produced from (issue #88): the
    // panel frames its view and builds its next request from the ACTUAL
    // authored format, data bounds and pixel aspect, not from a request-global
    // canvas guess.
    ImageDescription description;
    EvaluationRequest request;
    std::uint64_t requestId{};
    std::uint64_t revision{};
    bool cacheHit{};
    // True when the frame was served from the retained display-cache
    // representation (issue #106) rather than evaluated: the presentation was
    // sampled directly from the compressed frame. The panel presents it
    // exactly like a live frame; the flag exists so a replay preparation can be
    // told from a live one when it is consumed.
    bool replay{};
    std::chrono::steady_clock::time_point requestedAt{};
    // Destination that produced this result. Only that destination's panel
    // may publish or report it.
    eval::ViewerDestination destination{};
};
struct SourceProbeResult {
    eval::ViewerSession::SourceProbe source;
    std::uint64_t requestId{};
};
// Metadata-only answer to a Describe request: the target's authored output
// description, resolved on the worker through the shared dependency planner
// (no pixels, no device work, no decoder measurement).
struct ViewerTargetDescription {
    ImageDescription description;
    std::uint64_t requestId{};
};
struct ViewerFailure {
    std::string message;
    std::uint64_t requestId{};
};
// One completed on-demand working-space sample (issue #102). The values are the
// target's OWN working RGB(A) at the requested full-resolution pixel, produced
// by the same viewer session that renders frames — never a display byte and
// never a fabricated fallback.
struct ViewerWorkingSample {
    std::array<float, 4> rgba{0.0F, 0.0F, 0.0F, 1.0F};
    std::uint64_t requestId{};
};
// A view the current frame cannot answer (issue #98): the layer or channel it
// addresses is not part of what this frame's described image carries. The
// answer still carries the description that refused it, with the identity it
// belongs to, so the panel adopts the CURRENT frame's channels and states the
// reason from ONE worker job — no retry and no second request. A sequence or
// source whose frame changes its channels therefore updates the layer and
// channel selectors even while the current selection is refused, and the artist
// can select a layer this frame really carries.
struct ViewerUnavailableView {
    std::string message;
    std::uint64_t requestId{};
    NodeId target{kInvalidNode};
    std::int64_t localTime{};
    std::uint64_t revision{};
    ImageDescription description;
};
// A replay preparation found no retained display-cache representation for the
// frame it names (issue #106): never visited, missing, failed or evicted. The
// transport may render THAT ONE frame live — the frame it is due to show —
// while a prefetched neighbour is simply dropped. This is an answer, not an
// error, and it is never a reason to publish a substitute image.
struct ViewerReplayMiss {
    std::int64_t localTime{};
    std::uint64_t requestId{};
};
using ViewerWorkResult = std::variant<std::shared_ptr<const ViewerResult>, SourceProbeResult, ViewerTargetDescription,
                                      ViewerWorkingSample, ViewerUnavailableView, ViewerFailure, ViewerReplayMiss>;

struct ViewerRuntimeCounts {
    std::uint64_t queued{};
    std::uint64_t dropped{};
    std::uint64_t staleRejected{};
    std::uint64_t completed{};
    std::uint64_t cacheQueued{};
    std::uint64_t cacheDropped{};
    std::uint64_t cacheErrors{};
    std::uint64_t cachePublished{};
    std::string cacheError;
    std::uint64_t cacheDiskBytes{};
    std::uint64_t cacheCompressedRamBytes{};
    // Retained compressed frames resident on the device (issue #106) and the
    // bytes they are charged for; the RAM hot set is the field above.
    std::uint64_t cacheResidentFrames{};
    std::uint64_t cacheResidentBytes{};
    std::uint64_t cacheActiveFrames{};
    [[nodiscard]] bool operator==(const ViewerRuntimeCounts&) const = default;
};

// GUI submits immutable snapshots to the headless ViewerScheduler. The
// worker owns decoder/evaluator/cache access and is the only thread allowed
// to perform potentially blocking render, decode, presentation preparation,
// or cache work. Cancellation only changes scheduler publication state: GPU
// work already submitted may finish and retains its resources under #22.
//
// One runtime serves every viewer panel. Each panel holds a bounded
// panel-instance destination allocated here; publication, cancellation,
// counters and the retained presentation are scoped to it, while the
// scheduler, worker, session and Qt window presentation host are shared.
class ViewerRuntime final : public QObject {
    Q_OBJECT
public:
    ViewerRuntime() = default;
    ~ViewerRuntime() override;
    // `contributions` is the application's COMPLETE native inventory (issue
    // #37): the built-in projection plus every installed package's GPU callbacks
    // and node metadata, assembled once by the owner of the extension packages.
    // The default is the real built-in-only assembly
    // `eval::builtinGpuContributions()` — exactly the inventory this runtime
    // loaded implicitly before — so an application with no installed packages
    // is unchanged, while an application that supplies packages gets their
    // effects in the viewer worker AND in the shared delivery queue. One
    // immutable copy is handed to the delivery queue, and one is moved into the
    // viewer worker: this runtime takes contribution VALUES, never a package or
    // loader reference, so it never mutates the list, never registers it
    // anywhere process-global, and never loads package code itself.
    void bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                   eval::ViewerCacheOptions cacheOptions,
                   std::vector<eval::GpuNodeContribution> contributions = eval::builtinGpuContributions());
    // Idempotent for the same window: the first successful call adopts the
    // Vulkan presentation device, hosts every panel's frame-slot pinning and
    // shows the window. A second window is refused rather than sharing one
    // presentation host.
    [[nodiscard]] QString attachToWindow(QQuickWindow* window);

    // Bounded panel-instance destination allocation for `panelId`. The lowest
    // free id at or above the panel base is assigned; ids 0 and 1 stay
    // reserved for the default interactive stream and the explicit cache
    // range. A panel that already holds a destination keeps it. Exhaustion is
    // reported instead of aliasing another panel's destination.
    [[nodiscard]] std::optional<eval::ViewerDestination> allocateDestination(const QString& panelId);
    // Releases an allocated panel destination: its scheduler identity, queued
    // work, mailbox result and presentation channel are dropped, and the
    // worker-owned session forgets its freshness state before any later work.
    // An unallocated destination — including the reserved ids — is refused.
    bool retireDestination(eval::ViewerDestination destination);

    // Submits one immutable view intent (issue #98): the worker resolves it
    // against the current frame's described image and executes it in one
    // request. The presentation isolation the view stated travels back with
    // the frame, so nothing about it is recorded per destination here.
    bool submit(Document document, eval::ViewIntent intent, std::uint64_t id, eval::ViewerDestination destination,
                std::string colorConfigPath = {});

    // Queues a color-configuration refresh for the active worker session. The
    // runtime owns the session on its worker thread, so this is fire-and-forget
    // from any caller (the project-replacement observer) and retires nothing on
    // the calling thread. Idempotent and safe from several controllers: the
    // flag coalesces and the next worker iteration retires the retained OCIO
    // processors/viewing programs exactly once.
    void refreshColorConfig();

    bool probe(Document document, std::string source, std::uint64_t id,
               eval::ViewerDestination destination = eval::ViewerDestination::Interactive,
               std::string colorConfigPath = {});
    // Metadata-only target description (issue #88). Queued through the same
    // bounded worker admission as a render, so the panel learns the actual
    // authored format without touching media, the device, or the GUI thread.
    bool describe(Document document, EvaluationRequest request, std::uint64_t id,
                  eval::ViewerDestination destination = eval::ViewerDestination::Interactive,
                  std::string colorConfigPath = {});
    // Issue #102: ONE on-demand working-space pixel of `request`'s target. The
    // caller states a single full-resolution sample; the worker evaluates it in
    // the working space and publishes ViewerWorkingSample (or ViewerFailure).
    // It is admitted exactly like every other viewer request, so it can never
    // run beside, displace or be displaced by work it was not given.
    bool sample(Document document, EvaluationRequest request, std::uint64_t id,
                eval::ViewerDestination destination = eval::ViewerDestination::Interactive,
                std::string colorConfigPath = {});
    // `first` and `last` are inclusive local-time frames. The range is held
    // as one lazy descriptor per destination and produces cache publications
    // only; it never replaces that destination's interactive viewer result.
    bool requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                      eval::ViewerDestination destination = eval::ViewerDestination::Cache,
                      std::string colorConfigPath = {});
    // One frame of the transport's ordered playback window (issue #106). The
    // worker serves it from the retained display-cache representation only and
    // never evaluates the graph: a frame with no retained representation
    // publishes ViewerReplayMiss, and one whose representation is still loading
    // is kept pending and retried without blocking foreground work. Results
    // arrive in an ordered per-destination queue, so a prepared frame is never
    // displaced by its own successor, and a latest-wins submission or
    // cancellation for that destination retires the whole window.
    //
    // Returns the admitted unit's document revision — the immutable snapshot's
    // identity, computed once at admission — which the worker hands to the
    // replay probe so an ordinary playback tick validates its retained record
    // without fingerprinting the whole document again. nullopt when refused.
    [[nodiscard]] std::optional<std::uint64_t>
    prepareReplay(const eval::ViewerPlaybackContext& context, eval::ViewIntent intent, std::uint64_t id,
                  eval::ViewerDestination destination = eval::ViewerDestination::Interactive,
                  std::string colorConfigPath = {});
    // Nonblocking cancellation. `id` is a generation watermark: queued work
    // is dropped immediately and in-flight work is rejected at publication.
    // The global form moves the shared watermark; the destination form only
    // invalidates that destination.
    void cancel(std::uint64_t id);
    void cancel(std::uint64_t id, eval::ViewerDestination destination);
    // Retires only a destination's ordered playback window (issue #106): queued
    // and in-flight replay preparations are dropped and rejected while the
    // destination's latest-wins request, mailbox result and current frame are
    // untouched. Nonblocking; no GPU work is waited for.
    void cancelPlayback(eval::ViewerDestination destination);
    [[nodiscard]] ViewerRuntimeCounts counts() const;
    // Scheduler counters for one destination beside the shared cache counts.
    [[nodiscard]] ViewerRuntimeCounts counts(eval::ViewerDestination destination) const;

    [[nodiscard]] std::optional<ViewerWorkResult>
    takeResult(eval::ViewerDestination destination = eval::ViewerDestination::Interactive);
    // Call before Qt window destruction; quiesce follows Qt teardown.
    void stopWorker();
    void quiesceForTeardown();
    // The ONE native delivery job seam this application owns (issue #94). The
    // queue borrows this runtime's own Instance/Device/Allocator and the same
    // compiled shader directory the viewer renders with, so a delivery evaluates
    // through the same native path at full quality and never creates a second
    // device, allocator, renderer or decoder. bootstrap() initializes it, and
    // teardown stops it before those native objects are destroyed.
    [[nodiscard]] eval::DeliveryQueue& deliveryQueue();

    [[nodiscard]] gpu::Device& presentationDevice() const { return *presentationDevice_; }
    [[nodiscard]] bool presentationFilterLinear() const { return filterLinear_; }
    // Shared presentation host owned by the runtime for its whole lifetime, so
    // every panel's ViewerItem pins its images through the same frame slots and
    // closing one panel cannot retire another panel's in-flight pins.
    [[nodiscard]] WindowPresentationState& windowPresentation() { return windowState_; }

signals:
    void resultReady();
    void rangeFailed(QString message, qulonglong requestId);
    // A destination's newly presented frame reached Qt's frameSwapped
    // boundary. Not a physical scanout timestamp.
    void framePresented(eval::ViewerDestination destination, int frame, int width, int height, bool cacheHit,
                        double requestToSwapMs);

private:
    using Pending = eval::ViewerScheduledRequest;

    void run(const std::filesystem::path& shaders, const std::vector<eval::GpuNodeContribution>& contributions);
    // Executes one scheduled unit on the worker session.
    void execute(Pending& pending, eval::ViewerSession& session, const std::filesystem::path& shaders,
                 std::vector<std::uint32_t>& presentationShader, std::vector<std::uint32_t>& replayShader);
    // Serves one ordered playback frame from the retained representation and
    // publishes it (or the miss answer) on the destination's ordered replay
    // queue.
    void executeReplay(const Pending& pending, eval::ViewerSession& session, const std::filesystem::path& shaders,
                       std::vector<std::uint32_t>& presentationShader, std::vector<std::uint32_t>& replayShader);
    // The presentation copy of one frame: the live float path, or the direct
    // BC7-sample path when the frame was served from the retained cache
    // representation. The module each path needs is loaded on first use.
    [[nodiscard]] gpu::ViewerPresentation presentFrame(const eval::ViewerFrame& frame,
                                                       const std::filesystem::path& shaders,
                                                       std::vector<std::uint32_t>& presentationShader,
                                                       std::vector<std::uint32_t>& replayShader);
    bool publish(ViewerWorkResult result, const Pending& pending);
    // Ordered ready replay results, appended in the worker's own order and
    // consumed by the panel's transport clock. A latest-wins result never waits
    // behind them.
    bool publishReplay(ViewerWorkResult result, const Pending& pending);
    void finishRange(const Pending& pending, bool cacheAccepted);
    // Bounded worker-owned list of replay preparations whose retained
    // representation is still loading, ordered by retry deadline. A worker may
    // wait for its own I/O; it never waits for the GUI thread.
    struct PendingReplay {
        Pending request;
        std::chrono::steady_clock::time_point retryAt{};
        std::chrono::milliseconds backoff{};
    };
    void keepPendingReplay(Pending pending, std::chrono::milliseconds backoff);
    // mutex_ is held by callers; refreshes the shared cache snapshot and
    // spreads it over the given scheduler counters.
    [[nodiscard]] ViewerRuntimeCounts composeCountsLocked(const eval::ViewerSchedulerCounts& counts) const;
    void flushValidation();

    // Destroy Qt's adopting wrapper BEFORE its borrowed Vulkan instance.
    std::unique_ptr<gpu::Instance> instance_;
    // Timed-out execution submissions can retain imported images. Their
    // device drains and retires those owners BEFORE the consumer is destroyed.
    std::unique_ptr<gpu::Device> presentationDevice_;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<gpu::Allocator> allocator_;
    // The application's explicit delivery jobs (issue #94). Declared after the
    // native owners so it is destroyed before them, and stopped explicitly in
    // quiesceForTeardown too: a delivery's own worker and its retained
    // submissions never outlive the device they borrow.
    std::unique_ptr<eval::DeliveryQueue> delivery_;
    QVulkanInstance qtInstance_;
    bool filterLinear_{};

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    // Interactive admission is bounded by a global descriptor count while
    // queued work coalesces per destination, so one slot per bounded
    // destination admits exactly one queued request for every panel. The
    // default of one would let one panel's queued request reject another
    // panel's submission.
    eval::ViewerScheduler scheduler_{eval::kMaxViewerDestinations};
    struct Published {
        Pending request;
        ViewerWorkResult result;
    };
    std::map<eval::ViewerDestination, Published> results_;
    // Ordered ready playback results per destination (issue #106). A latest-wins
    // result is consumed first; the replay queue is drained in the worker's own
    // order so a prepared frame is never lost behind its successor.
    std::map<eval::ViewerDestination, std::deque<Published>> replayResults_;
    // Worker-only: replay preparations whose retained representation is still
    // loading, kept in retry-deadline order. Bounded by the scheduler's own
    // playback window because every entry is one in-flight Replay unit.
    std::deque<PendingReplay> replayPending_;
    // Panel-instance allocation table, keyed by panel identity so a panel
    // keeps one destination for as long as it lives.
    std::map<QString, eval::ViewerDestination> panelDestinations_;
    // Worker-only Auto resolution state per destination (issue #98): the
    // hysteresis that keeps a steady view from oscillating between sampling
    // representations lives with the worker that resolves the demand, and is
    // retained per destination for as long as the destination is allocated.
    // Only the worker thread touches it.
    std::map<eval::ViewerDestination, ViewerResolutionPolicy> resolution_;
    // Destinations retired by the GUI but not yet applied to the worker-owned
    // session. Drained before any later work for that destination executes.
    std::vector<eval::ViewerDestination> retireQueue_;
    // Coalesced color-configuration refresh request for the worker.
    bool colorRefresh_{false};
    eval::ViewerCacheOptions cacheOptions_;
    // Borrowed only under mutex_; worker clears before destroying its session.
    // GUI counter reads use try-lock snapshots, never a blocking cache query.
    eval::ViewerSession* session_{};
    mutable eval::ViewerCacheCounts cacheCounts_;
    bool stopping_{};
    std::thread worker_;

    // Shared Qt render-thread presentation host for every panel's nodes.
    WindowPresentationState windowState_;
    // Borrowed Qt window; only compared for attach idempotency, never
    // dereferenced outside the render-thread connections it owns.
    QQuickWindow* attachedWindow_{};
};
}  // namespace nemo::ui
