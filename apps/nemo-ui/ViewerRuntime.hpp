#pragma once

#include "nemo/eval/Viewer.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/eval/ViewerScheduler.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"

#include <QObject>
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
    EvaluationRequest request;
    std::uint64_t requestId{};
    std::uint64_t revision{};
    bool cacheHit{};
    std::chrono::steady_clock::time_point requestedAt{};
};
struct SourceProbeResult {
    eval::ViewerSession::SourceProbe source;
    std::uint64_t requestId{};
};
struct ViewerFailure {
    std::string message;
    std::uint64_t requestId{};
};
using ViewerWorkResult = std::variant<std::shared_ptr<const ViewerResult>, SourceProbeResult, ViewerFailure>;

struct ViewerRuntimeCounts {
    std::uint64_t queued{};
    std::uint64_t dropped{};
    std::uint64_t staleRejected{};
    std::uint64_t completed{};
};

// GUI submits immutable snapshots to the headless ViewerScheduler. The
// worker owns decoder/evaluator/cache access and is the only thread allowed
// to perform potentially blocking render, decode, presentation preparation,
// or cache work. Cancellation only changes scheduler publication state: GPU
// work already submitted may finish and retains its resources under #22.
class ViewerRuntime final : public QObject {
    Q_OBJECT
public:
    ViewerRuntime() = default;
    ~ViewerRuntime() override;
    void bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                   eval::ViewerCacheOptions cacheOptions);
    [[nodiscard]] QString attachToWindow(QQuickWindow* window);

    // The destination default preserves the existing single-viewer API.
    bool submit(Document document, EvaluationRequest request, std::uint64_t id,
                eval::ViewerDestination destination = eval::ViewerDestination::Interactive);
    bool probe(Document document, std::string source, std::uint64_t id);
    // `first` and `last` are inclusive local-time frames. The range is held
    // as one lazy descriptor and produces cache publications only; it never
    // replaces the interactive viewer result.
    bool requestRange(Document document, EvaluationRequest request, int first, int last, std::uint64_t id);
    // Nonblocking cancellation. `id` is a generation watermark: queued work
    // is dropped immediately and in-flight work is rejected at publication.
    void cancel(std::uint64_t id);
    [[nodiscard]] ViewerRuntimeCounts counts() const;

    [[nodiscard]] std::optional<ViewerWorkResult>
    takeResult(eval::ViewerDestination destination = eval::ViewerDestination::Interactive);
    // Call before Qt window destruction; quiesce follows Qt teardown.
    void stopWorker();
    void quiesceForTeardown();
    [[nodiscard]] gpu::Device& presentationDevice() const { return *presentationDevice_; }
    [[nodiscard]] bool presentationFilterLinear() const { return filterLinear_; }

signals:
    void resultReady();
    void rangeFailed(QString message, qulonglong requestId);

private:
    using Pending = eval::ViewerScheduledRequest;

    void run(const std::filesystem::path& shaders);
    bool publish(ViewerWorkResult result, const Pending& pending);
    void finishRange(const Pending& pending);
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
    eval::ViewerScheduler scheduler_;
    struct Published {
        Pending request;
        ViewerWorkResult result;
    };
    std::map<eval::ViewerDestination, Published> results_;
    eval::ViewerCacheOptions cacheOptions_;
    bool stopping_{};
    std::thread worker_;
};
}  // namespace nemo::ui
