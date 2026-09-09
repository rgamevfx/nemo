#pragma once

#include "nemo/eval/Viewer.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"

#include <QObject>
#include <QVulkanInstance>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

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

// GUI submits immutable snapshots; one worker owns the decoder/evaluator.
// Both request and result mailboxes have one slot. Obsolete work may finish,
// but cannot publish. Stop/join are shutdown-only, never source-load operations.
class ViewerRuntime final : public QObject {
    Q_OBJECT
public:
    ViewerRuntime() = default;
    ~ViewerRuntime() override;
    void bootstrap(const std::vector<std::string>& extensions, const std::filesystem::path& shaders,
                   eval::ViewerCacheOptions cacheOptions);
    [[nodiscard]] QString attachToWindow(QQuickWindow* window);
    void submit(Document document, EvaluationRequest request, std::uint64_t id);
    void probe(Document document, std::string source, std::uint64_t id);
    [[nodiscard]] std::optional<ViewerWorkResult> takeResult();
    // Call before Qt window destruction; quiesce follows Qt teardown.
    void stopWorker();
    void quiesceForTeardown();
    [[nodiscard]] gpu::Device& presentationDevice() const { return *presentationDevice_; }
    [[nodiscard]] bool presentationFilterLinear() const { return filterLinear_; }

signals:
    void resultReady();

private:
    struct Pending {
        Document document;
        EvaluationRequest request;
        std::string source;  // nonempty: probe rather than render
        std::uint64_t id{};
        std::chrono::steady_clock::time_point requestedAt{};
    };
    void enqueue(Pending pending);
    void run(const std::filesystem::path& shaders);
    void publish(ViewerWorkResult result, std::uint64_t id);
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
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Pending> pending_;
    std::optional<ViewerWorkResult> result_;
    std::uint64_t latestId_{};
    std::uint64_t latestRevision_{};
    eval::ViewerCacheOptions cacheOptions_;
    // Borrowed from run() under mutex_. Cleared before worker-side destruction.
    eval::ViewerSession* session_{};
    bool stopping_{};
    std::thread worker_;
};
}  // namespace nemo::ui
