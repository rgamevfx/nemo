#pragma once

#include "ViewerItem.hpp"
#include "nemo/eval/Viewer.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/eval/ViewerScheduler.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"

#include <QObject>
#include <QString>
#include <QVulkanInstance>

#include <chrono>
#include <condition_variable>
#include <cstdint>
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
using ViewerWorkResult = std::variant<std::shared_ptr<const ViewerResult>, SourceProbeResult, ViewerTargetDescription,
                                      ViewerUnavailableView, ViewerFailure>;

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
    void bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                   eval::ViewerCacheOptions cacheOptions);
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
    // `first` and `last` are inclusive local-time frames. The range is held
    // as one lazy descriptor per destination and produces cache publications
    // only; it never replaces that destination's interactive viewer result.
    bool requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id,
                      eval::ViewerDestination destination = eval::ViewerDestination::Cache,
                      std::string colorConfigPath = {});
    // Nonblocking cancellation. `id` is a generation watermark: queued work
    // is dropped immediately and in-flight work is rejected at publication.
    // The global form moves the shared watermark; the destination form only
    // invalidates that destination.
    void cancel(std::uint64_t id);
    void cancel(std::uint64_t id, eval::ViewerDestination destination);
    [[nodiscard]] ViewerRuntimeCounts counts() const;
    // Scheduler counters for one destination beside the shared cache counts.
    [[nodiscard]] ViewerRuntimeCounts counts(eval::ViewerDestination destination) const;

    [[nodiscard]] std::optional<ViewerWorkResult>
    takeResult(eval::ViewerDestination destination = eval::ViewerDestination::Interactive);
    // Call before Qt window destruction; quiesce follows Qt teardown.
    void stopWorker();
    void quiesceForTeardown();
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

    void run(const std::filesystem::path& shaders);
    bool publish(ViewerWorkResult result, const Pending& pending);
    void finishRange(const Pending& pending, bool cacheAccepted);
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
