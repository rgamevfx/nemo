#include "HistoryController.hpp"
#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ParameterInteraction.hpp"
#include "ProjectFileController.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerItem.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/gpu/Error.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef NEMO_SLANG_SPV_DIR
#define NEMO_SLANG_SPV_DIR ""
#endif

// Issue #47 native multi-destination evidence. Two real viewer panels in one
// production workspace shell, each owning its own ViewerController and
// scheduler destination through ViewerControllerRegistry, render different
// graph targets concurrently. This scenario drives the real QQuickWindow with
// QTest input, records per-destination request identity from the production
// controllers, and captures the production window.
//
// It runs only on a real windowing platform with the Vulkan scene graph:
// NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1. The offscreen/Null CI
// configuration skips it instead of attaching a Vulkan device to a window
// that cannot present.
namespace {

nemo::NetworkId networkIdentity(const QString& value) {
    return static_cast<nemo::NetworkId>(value.toULongLong());
}

QQuickItem* visualByName(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualByName(child, name))
            return found;
    }
    return nullptr;
}

// The viewer panel body is a FocusScope named "viewerPanel"; it is the subtree
// that owns one panel's transport controls and frame fields.
QQuickItem* panelRootFor(QQuickItem* content, const QString& panelId) {
    QQuickItem* field = visualByName(content, QStringLiteral("viewerFrame_") + panelId);
    while (field && field->objectName() != QStringLiteral("viewerPanel"))
        field = field->parentItem();
    return field;
}

QJsonObject destinationSnapshot(nemo::ui::ViewerController& controller, nemo::ui::ViewerRuntime& runtime,
                                const QString& label) {
    QJsonObject record{{QStringLiteral("label"), label},
                       {QStringLiteral("has_destination"), controller.hasDestination()},
                       {QStringLiteral("destination_id"), static_cast<qint64>(controller.destinationId())},
                       {QStringLiteral("frame"), controller.frame()},
                       {QStringLiteral("timecode"), controller.timecode()},
                       {QStringLiteral("resolution_mode"), controller.resolutionMode()},
                       {QStringLiteral("channel"), controller.channel()},
                       {QStringLiteral("layer"), controller.layer()},
                       {QStringLiteral("playing"), controller.playing()},
                       {QStringLiteral("in_frame"), controller.inFrame()},
                       {QStringLiteral("out_frame"), controller.outFrame()},
                       {QStringLiteral("pending"), controller.pending()},
                       {QStringLiteral("outdated"), controller.outdated()},
                       {QStringLiteral("render_state"), controller.renderState()},
                       {QStringLiteral("status"), controller.status()},
                       {QStringLiteral("error"), controller.error()},
                       {QStringLiteral("has_presentation"), false}};
    if (const auto presentation = controller.presentation()) {
        const auto& request = presentation->request;
        record[QStringLiteral("has_presentation")] = true;
        record[QStringLiteral("request_id")] = static_cast<qint64>(presentation->requestId);
        record[QStringLiteral("revision")] = static_cast<qint64>(presentation->revision);
        record[QStringLiteral("output")] = QString::number(static_cast<qint64>(request.output));
        record[QStringLiteral("local_time")] = static_cast<qlonglong>(request.localTime);
        record[QStringLiteral("sampling_scale")] = request.samplingScale;
        record[QStringLiteral("cache_hit")] = presentation->cacheHit;
        record[QStringLiteral("raster_width")] = presentation->frame.width;
        record[QStringLiteral("raster_height")] = presentation->frame.height;
        record[QStringLiteral("full_width")] = request.imageWidth();
        record[QStringLiteral("full_height")] = request.imageHeight();
        record[QStringLiteral("region_x")] = request.region.x;
        record[QStringLiteral("region_y")] = request.region.y;
        record[QStringLiteral("region_width")] = request.region.width;
        record[QStringLiteral("region_height")] = request.region.height;
        record[QStringLiteral("destination_tag")] =
            static_cast<qint64>(static_cast<std::uint32_t>(presentation->destination));
    }
    if (controller.destination()) {
        const auto counts = runtime.counts(*controller.destination());
        record[QStringLiteral("queued")] = static_cast<qint64>(counts.queued);
        record[QStringLiteral("dropped")] = static_cast<qint64>(counts.dropped);
        record[QStringLiteral("stale_rejected")] = static_cast<qint64>(counts.staleRejected);
        record[QStringLiteral("completed")] = static_cast<qint64>(counts.completed);
    }
    return record;
}

class ViewerDestinationSurface : public testing::Test {
protected:
    QTemporaryDir directory_;
    std::unique_ptr<nemo::ui::ViewerRuntime> runtime_;
    std::unique_ptr<nemo::ProjectSession> session_;
    // One presentation interaction per project (issue #102): every controller
    // this fixture composes for the session shares it, and it outlives them.
    nemo::ui::ParameterInteraction interaction_;
    // The shared presentation history the application composes: declared after
    // the session and before the engine, so both lifetimes stay valid.
    std::unique_ptr<nemo::ui::HistoryController> history_;
    std::unique_ptr<nemo::ui::PanelContextRouter> router_;
    std::unique_ptr<nemo::ui::ViewerController> facade_;
    std::unique_ptr<nemo::ui::ViewerControllerRegistry> registry_;
    std::unique_ptr<nemo::workspace::WorkspaceController> workspace_;
    std::unique_ptr<nemo::ui::NativeFileChooser> chooser_;
    std::unique_ptr<nemo::ui::ProjectFileController> projectFile_;
    std::unique_ptr<nemo::ui::ParameterEditorRegistry> editors_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    std::unique_ptr<QSignalSpy> warnings_;
    QQuickWindow* window_{};
    QString panelA_;
    QString panelB_;
    QString colorAId_;
    QString colorBId_;
    qint64 destinationAId_{};
    nemo::ui::ViewerController* controllerA_{};
    nemo::ui::ViewerController* controllerB_{};
    QVector<QJsonObject> records_;
    QString evidenceDirectory_;
    // The cache configuration this fixture's runtime was bootstrapped with
    // (issue #106): each scenario states its own storage tier through
    // createCacheOptions() so one scenario body runs against several tiers
    // without a second copy of it.
    nemo::eval::ViewerCacheOptions cacheOptions_;
    // Absolute path of the colour configuration the fixture published through
    // OCIO; the reopened-cache scenario states it as an explicit alternative
    // identity (see replaceWorkerCacheSession).
    std::string viewingConfigFile_;
    std::uint64_t scratchRequestId_{};

    // Storage tier this fixture runs. The default is the mixed configuration the
    // panel scenarios have always used — a small compressed RAM tier beside a
    // bound on device residency and the disk budget; the ordered-range replay
    // scenario is instantiated on tiers that state its two required
    // configurations explicitly.
    [[nodiscard]] virtual nemo::eval::ViewerCacheOptions createCacheOptions() const {
        nemo::eval::ViewerCacheOptions cacheOptions;
        cacheOptions.directory = directory_.filePath(QStringLiteral("cache")).toStdString();
        cacheOptions.maxRamBytes = 64ULL * 1024ULL * 1024ULL;
        cacheOptions.maxResidentBytes = 256ULL * 1024ULL * 1024ULL;
        cacheOptions.maxPendingBytes = 256ULL * 1024ULL * 1024ULL;
        cacheOptions.maxDiskBytes = 1024ULL * 1024ULL * 1024ULL;
        return cacheOptions;
    }
    // Evidence and capture tag of the tier this fixture runs.
    [[nodiscard]] virtual QString cacheTierName() const { return QStringLiteral("default"); }

    // Issue #106 acceptance 3: the ordered playback window over an explicitly
    // populated 1080p range, forward and reverse, under this fixture's storage
    // tier. Defined below the class, shared verbatim by the tiers instantiated
    // at the bottom of this file.
    void cachedRangePlaysForwardAndReverseFromTheOrderedWindow();

    // Physical bytes the cache namespace holds, read from the file system
    // instead of from the cache's own accounting: packs, headers and dead
    // records included, exactly as the durable writer left them.
    [[nodiscard]] QJsonObject packInventory() const {
        std::uint64_t files = 0;
        std::uint64_t bytes = 0;
        std::error_code error;
        const auto directory = std::filesystem::path(cacheOptions_.directory);
        for (std::filesystem::recursive_directory_iterator entry(directory, error), end; entry != end;
             entry.increment(error)) {
            if (error)
                break;
            if (!entry->is_regular_file(error) || error) {
                error.clear();
                continue;
            }
            const auto size = std::filesystem::file_size(entry->path(), error);
            if (error) {
                error.clear();
                continue;
            }
            ++files;
            bytes += size;
        }
        return QJsonObject{{QStringLiteral("directory"), QString::fromStdString(directory.string())},
                           {QStringLiteral("files"), static_cast<qint64>(files)},
                           {QStringLiteral("bytes"), static_cast<qint64>(bytes)}};
    }

    // Replaces the worker's ViewerSession — and with it the ViewerCache that owns
    // the pack namespace — through the one public seam that can: the
    // colour-configuration identity a request states. ViewerRuntime builds a new
    // session (reopening the same directory, its index rebuilt by scanning the
    // packs) whenever a request states a path different from the session's; no
    // other seam reopens a cache, because ViewerSession::configureCache refuses a
    // second configuration and neither ViewerRuntime nor ViewerController exposes
    // its session. The swap vehicle is a metadata-only Describe, so a replacement
    // performs no render, no publication and no pack write, and its answer is
    // consumed from the default destination's mailbox — observing it is proof the
    // worker processed a request under the stated identity.
    bool replaceWorkerCacheSession(const std::string& colorConfigPath) {
        const auto presentation = controllerA_->presentation();
        if (!presentation)
            return false;
        while (runtime_->takeResult(nemo::eval::ViewerDestination::Interactive))
            ;  // the scratch destination must hold nothing of ours
        const auto id = ++scratchRequestId_;
        if (!runtime_->describe(session_->snapshot(), presentation->request, id,
                                nemo::eval::ViewerDestination::Interactive, colorConfigPath)) {
            return false;
        }
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 120000) {
            const auto result = runtime_->takeResult(nemo::eval::ViewerDestination::Interactive);
            if (!result) {
                QTest::qWait(5);
                continue;
            }
            const auto* described = std::get_if<nemo::ui::ViewerTargetDescription>(&*result);
            return described && described->requestId == id;
        }
        return false;
    }

    // Cache counters of the currently configured session, waited for: a busy
    // cache reports no snapshot, so the runtime keeps its previous destination
    // counters until one is available (ViewerRuntime::composeCountsLocked). A
    // replaced session is therefore observed by waiting for the predicate
    // instead of reading once.
    template <typename Predicate>
    nemo::ui::ViewerRuntimeCounts waitForCacheCounts(Predicate&& predicate, int timeoutMs = 30000) {
        QElapsedTimer deadline;
        deadline.start();
        auto observed = runtime_->counts(*controllerA_->destination());
        while (deadline.elapsed() < timeoutMs) {
            observed = runtime_->counts(*controllerA_->destination());
            if (predicate(observed))
                break;
            QTest::qWait(10);
        }
        return observed;
    }

    void SetUp() override {
        if (qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") != 1 ||
            qEnvironmentVariableIntValue("NEMO_TEST_VIEWER_WINDOW") != 1) {
            GTEST_SKIP() << "native viewer window evidence requires NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1";
        }
        if (std::string(NEMO_SLANG_SPV_DIR).empty())
            GTEST_SKIP() << "native multi-destination evidence requires compiled Slang shaders";
        const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                            "docs/evidence/issue12-view.ocio";
        qputenv("OCIO", config.string().c_str());
        viewingConfigFile_ = config.string();
        evidenceDirectory_ = qEnvironmentVariable("NEMO_ISSUE47_EVIDENCE_DIR");

        runtime_ = std::make_unique<nemo::ui::ViewerRuntime>();
        cacheOptions_ = createCacheOptions();
        std::vector<std::string> extensions{"VK_KHR_surface"};
        if (QGuiApplication::platformName() == QStringLiteral("wayland"))
            extensions.push_back("VK_KHR_wayland_surface");
        else if (QGuiApplication::platformName() == QStringLiteral("xcb"))
            extensions.push_back("VK_KHR_xcb_surface");
        try {
            runtime_->bootstrap(extensions, NEMO_SLANG_SPV_DIR, cacheOptions_);
        } catch (const nemo::gpu::GpuException& error) {
            if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
                GTEST_SKIP() << error.what();
            throw;
        }

        session_ = std::make_unique<nemo::ProjectSession>();
        history_ = std::make_unique<nemo::ui::HistoryController>(*session_);
        router_ = std::make_unique<nemo::ui::PanelContextRouter>(*session_);
        facade_ = std::make_unique<nemo::ui::ViewerController>(runtime_.get(), *session_, interaction_);
        registry_ = std::make_unique<nemo::ui::ViewerControllerRegistry>(runtime_.get(), *session_, interaction_);

        // A clean two-viewer workspace: two side-by-side panels in groups A and
        // B, each selecting one of the graph's two Viewer nodes. Written through
        // the production persistence schema so the shell loads it like any
        // saved layout.
        const auto workspacePath = directory_.filePath(QStringLiteral("workspace.json"));
        const auto panel = [](const QString& id, const QString& group, int viewerIndex) {
            return QJsonObject{{QStringLiteral("id"), id},
                               {QStringLiteral("type"), QStringLiteral("viewer")},
                               {QStringLiteral("group"), group},
                               {QStringLiteral("state"), QJsonObject{{QStringLiteral("viewerIndex"), viewerIndex}}}};
        };
        const auto leaf = [](const QString& id, const QString& active, const QJsonObject& onlyPanel) {
            return QJsonObject{{QStringLiteral("id"), id},
                               {QStringLiteral("kind"), QStringLiteral("tabs")},
                               {QStringLiteral("active"), active},
                               {QStringLiteral("panels"), QJsonArray{onlyPanel}}};
        };
        const QJsonObject layout{
            {QStringLiteral("version"), 1},
            {QStringLiteral("root"),
             QJsonObject{{QStringLiteral("id"), QStringLiteral("split-root")},
                         {QStringLiteral("kind"), QStringLiteral("split")},
                         {QStringLiteral("orientation"), QStringLiteral("horizontal")},
                         {QStringLiteral("ratio"), 0.5},
                         {QStringLiteral("children"),
                          QJsonArray{leaf(QStringLiteral("leaf-a"), QStringLiteral("panel-a"),
                                          panel(QStringLiteral("panel-a"), QStringLiteral("A"), 0)),
                                     leaf(QStringLiteral("leaf-b"), QStringLiteral("panel-b"),
                                          panel(QStringLiteral("panel-b"), QStringLiteral("B"), 1))}}}}};
        const QJsonObject document{{QStringLiteral("version"), 2},
                                   {QStringLiteral("activeWorkspaceId"), QStringLiteral("workspace-1")},
                                   {QStringLiteral("workspaces"),
                                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-1")},
                                                           {QStringLiteral("name"), QStringLiteral("Viewer split")},
                                                           {QStringLiteral("layout"), layout}}}}};
        QFile workspaceFile(workspacePath);
        ASSERT_TRUE(workspaceFile.open(QIODevice::WriteOnly));
        workspaceFile.write(QJsonDocument(document).toJson());
        workspaceFile.close();
        workspace_ = std::make_unique<nemo::workspace::WorkspaceController>(workspacePath);
        ASSERT_TRUE(workspace_->error().isEmpty()) << workspace_->error().toStdString();
        workspace_->registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"),
                                      QStringLiteral("ViewerPanel.qml"));
        panelA_ = QStringLiteral("panel-a");
        panelB_ = QStringLiteral("panel-b");
        router_->setWorkspaceController(workspace_.get());

        // Registry-created controllers exist before QML binds them, so the
        // scenario drives the same controllers the panels render through.
        controllerA_ = qobject_cast<nemo::ui::ViewerController*>(registry_->controller(panelA_));
        controllerB_ = qobject_cast<nemo::ui::ViewerController*>(registry_->controller(panelB_));
        ASSERT_NE(controllerA_, nullptr);
        ASSERT_NE(controllerB_, nullptr);
        ASSERT_NE(controllerA_, controllerB_);
        ASSERT_TRUE(controllerA_->destination().has_value());
        ASSERT_TRUE(controllerB_->destination().has_value());
        ASSERT_NE(*controllerA_->destination(), *controllerB_->destination());

        // Two distinct graph outputs, each attached to one Viewer node. Panel A
        // selects viewer index 0 and panel B index 1, so the destinations render
        // different targets concurrently.
        const auto scope = controllerA_->rootNetworkId();
        const auto colorA = controllerA_->createGraphNode(scope, QStringLiteral("constcolor"),
                                                          QStringLiteral("destColorA"), 0.0, 0.0, {}, {});
        const auto colorB = controllerA_->createGraphNode(scope, QStringLiteral("constcolor"),
                                                          QStringLiteral("destColorB"), 0.0, 80.0, {}, {});
        const auto viewerA = controllerA_->createGraphNode(scope, QStringLiteral("viewer"),
                                                           QStringLiteral("DestViewerA"), 0.0, 160.0, {}, {});
        const auto viewerB = controllerA_->createGraphNode(scope, QStringLiteral("viewer"),
                                                           QStringLiteral("DestViewerB"), 0.0, 240.0, {}, {});
        ASSERT_FALSE(colorA.isEmpty());
        ASSERT_FALSE(colorB.isEmpty());
        ASSERT_FALSE(viewerA.isEmpty());
        ASSERT_FALSE(viewerB.isEmpty());
        ASSERT_TRUE(controllerA_->connectOrReplaceGraph(scope, colorA, 0, viewerA, 0));
        ASSERT_TRUE(controllerA_->connectOrReplaceGraph(scope, colorB, 0, viewerB, 0));
        // Distinct colors make destination isolation visible in the capture as
        // well as provable from the request output identity.
        controllerA_->setNodeParameter(colorA, QStringLiteral("color"),
                                       QVariantList{QVariant{1.0}, QVariant{0.0}, QVariant{0.0}, QVariant{1.0}});
        controllerA_->setNodeParameter(colorB, QStringLiteral("color"),
                                       QVariantList{QVariant{0.0}, QVariant{0.0}, QVariant{1.0}, QVariant{1.0}});
        colorAId_ = colorA;
        colorBId_ = colorB;
        destinationAId_ = static_cast<qint64>(controllerA_->destinationId());

        chooser_ = std::make_unique<nemo::ui::NativeFileChooser>();
        projectFile_ = std::make_unique<nemo::ui::ProjectFileController>(*session_, *workspace_, *router_, *chooser_);
        editors_ = std::make_unique<nemo::ui::ParameterEditorRegistry>();

        engine_ = std::make_unique<QQmlApplicationEngine>();
        warnings_ = std::make_unique<QSignalSpy>(engine_.get(), &QQmlEngine::warnings);
        engine_->rootContext()->setContextProperty(QStringLiteral("workspace"), workspace_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("historyController"), history_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("panelContextRouter"), router_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("projectFile"), projectFile_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewerController"), facade_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewerControllers"), registry_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("parameterEditors"), editors_.get());
        engine_->load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine_->rootObjects().isEmpty());
        window_ = qobject_cast<QQuickWindow*>(engine_->rootObjects().constFirst());
        ASSERT_NE(window_, nullptr);

        // Attaching adopts the presentation device on the real window and shows
        // it; the runtime is the only owner of that device.
        const QString attachError = runtime_->attachToWindow(window_);
        ASSERT_TRUE(attachError.isEmpty()) << attachError.toStdString();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window_));
    }

    void TearDown() override {
        records_.clear();
        // Mirror the application: stop the worker before the QML engine
        // destroys the panels, then destroy them, then drain retained
        // presentation ownership before the runtime (and its devices) go away.
        if (runtime_)
            runtime_->stopWorker();
        warnings_.reset();
        engine_.reset();
        // Panels destroyed with the engine release their controllers through
        // deleteLater(). No event loop runs after the test body, so flush those
        // deferred deletions while the runtime they borrow is still alive.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (runtime_)
            runtime_->quiesceForTeardown();
        // Adapters that hold ProjectSession subscriptions and runtime borrows go
        // before their owners, so no destructor reads a freed session/device.
        editors_.reset();
        projectFile_.reset();
        chooser_.reset();
        registry_.reset();
        facade_.reset();
        history_.reset();
        workspace_.reset();
        router_.reset();
        session_.reset();
        runtime_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // Waits for a destination's newest presentation at `frame`, pumping the
    // event loop so panel bindings, scheduler polling and the worker advance.
    bool waitFor(nemo::ui::ViewerController& controller, int frame, int timeoutMs = 120000) {
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < timeoutMs) {
            const auto presentation = controller.presentation();
            if (presentation && presentation->request.localTime == frame && !controller.pending() &&
                !controller.outdated())
                return true;
            if (!controller.error().isEmpty())
                return false;
            QTest::qWait(10);
        }
        return false;
    }

    void record(const QString& label) {
        const auto geometry = [this](const QString& panelId) {
            QJsonObject value;
            if (auto* panel = panelRootFor(window_->contentItem(), panelId)) {
                value[QStringLiteral("width")] = panel->width();
                value[QStringLiteral("height")] = panel->height();
            }
            return value;
        };
        if (controllerA_) {
            auto entry = destinationSnapshot(*controllerA_, *runtime_, label + QStringLiteral("-A"));
            entry[QStringLiteral("panel")] = geometry(panelA_);
            records_.push_back(entry);
        }
        auto entry = destinationSnapshot(*controllerB_, *runtime_, label + QStringLiteral("-B"));
        entry[QStringLiteral("panel")] = geometry(panelB_);
        records_.push_back(entry);
    }

    void click(const QString& panelId, const QString& name) {
        auto* panel = panelRootFor(window_->contentItem(), panelId);
        ASSERT_NE(panel, nullptr) << "no panel root for " << panelId.toStdString();
        auto* target = visualByName(panel, name);
        ASSERT_NE(target, nullptr) << name.toStdString();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
        QTest::qWait(30);
    }

    void capture(const QString& name) {
        if (evidenceDirectory_.isEmpty())
            return;
        QDir().mkpath(evidenceDirectory_);
        // Park the pointer away from the transport so no hover tooltip lands in
        // the capture.
        QTest::mouseMove(window_, QPoint(4, 4));
        QTest::qWait(80);
        const auto grab = window_->grabWindow();
        EXPECT_TRUE(grab.save(evidenceDirectory_ + '/' + name + ".png"));
        const std::pair<QString, QString> panels[]{{QStringLiteral("a"), panelA_}, {QStringLiteral("b"), panelB_}};
        for (const auto& [tag, panelId] : panels) {
            auto* panel = panelRootFor(window_->contentItem(), panelId);
            if (!panel)
                continue;
            const QRect crop =
                QRectF(panel->mapToScene(QPointF{}), QSizeF(panel->width(), panel->height())).toAlignedRect();
            EXPECT_TRUE(grab.copy(crop).save(evidenceDirectory_ + '/' + name + '-' + tag + ".png"));
        }
    }

    void writeEvidence() {
        if (evidenceDirectory_.isEmpty())
            return;
        const QJsonObject environment{
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
            {QStringLiteral("device_pixel_ratio"), window_ ? window_->devicePixelRatio() : 0.0},
            {QStringLiteral("window_width"), window_ ? window_->width() : 0},
            {QStringLiteral("window_height"), window_ ? window_->height() : 0},
            {QStringLiteral("panel_a"), panelA_},
            {QStringLiteral("panel_b"), panelB_},
            {QStringLiteral("color_a"), colorAId_},
            {QStringLiteral("color_b"), colorBId_},
            {QStringLiteral("destination_a"), static_cast<qint64>(destinationAId_)},
            {QStringLiteral("destination_b"), static_cast<qint64>(controllerB_->destinationId())}};
        QFile environmentFile(evidenceDirectory_ + QStringLiteral("/environment.json"));
        ASSERT_TRUE(environmentFile.open(QIODevice::WriteOnly));
        environmentFile.write(QJsonDocument(environment).toJson(QJsonDocument::Indented));

        QFile recordsFile(evidenceDirectory_ + QStringLiteral("/records.jsonl"));
        ASSERT_TRUE(recordsFile.open(QIODevice::WriteOnly));
        for (const auto& entry : records_)
            recordsFile.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
    }
};

// Issue #84: two independent surfaces. Manipulating one viewer's view must not
// move, rescale or rewrite the other's presentation or its panel record.
TEST_F(ViewerDestinationSurface, ViewerViewsStayIndependentPerPanel) {
    ASSERT_TRUE(waitFor(*controllerA_, 0)) << controllerA_->error().toStdString();
    ASSERT_TRUE(waitFor(*controllerB_, 0)) << controllerB_->error().toStdString();

    const auto view = [this](const QString& panelId) {
        auto* item = visualByName(window_->contentItem(), QStringLiteral("viewerItem_") + panelId);
        const QRectF rect = item ? item->property("displayRect").toRectF() : QRectF();
        auto* controller = panelId == panelA_ ? controllerA_ : controllerB_;
        const auto presentation = controller ? controller->presentation() : nullptr;
        const auto region = presentation ? presentation->request.region : nemo::Region{};
        return std::pair<QRectF, nemo::Region>{rect, region};
    };
    const auto [rectABefore, regionABefore] = view(panelA_);
    const auto [rectBBefore, regionBBefore] = view(panelB_);
    ASSERT_GT(rectABefore.width(), 0.0);
    ASSERT_GT(rectBBefore.width(), 0.0);

    // Zoom panel A with the wheel over its own image area.
    auto* itemA = visualByName(window_->contentItem(), QStringLiteral("viewerItem_") + panelA_);
    ASSERT_NE(itemA, nullptr);
    const QPoint anchorA = itemA->mapToScene(QPointF(itemA->width() / 2, itemA->height() / 2)).toPoint();
    QTest::mouseMove(window_, anchorA);
    QTest::qWait(30);
    for (int notch = 0; notch < 6; ++notch) {
        QWheelEvent event(QPointF(anchorA), QPointF(window_->mapToGlobal(anchorA)), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window_, &event);
    }
    QTest::qWait(400);

    const auto [rectAAfter, regionAAfter] = view(panelA_);
    const auto [rectBAfter, regionBAfter] = view(panelB_);
    // A's request region shrank (its own view zoomed in) ...
    EXPECT_LT(regionAAfter.width, regionABefore.width);
    EXPECT_GT(rectAAfter.width(), rectABefore.width());
    // ... and B is exactly where it was, in the controller's request and in the
    // panel's own display transform.
    EXPECT_EQ(regionBAfter.width, regionBBefore.width);
    EXPECT_EQ(regionBAfter.x, regionBBefore.x);
    EXPECT_NEAR(rectBAfter.width(), rectBBefore.width(), 0.5);
    EXPECT_NEAR(rectBAfter.x(), rectBBefore.x(), 0.5);
    EXPECT_NEAR(rectBAfter.y(), rectBBefore.y(), 0.5);

    // Each panel's view is its own record: only the manipulated panel wrote one.
    const auto stateA = workspace_->panelState(panelA_);
    const auto stateB = workspace_->panelState(panelB_);
    EXPECT_EQ(stateA.value(QStringLiteral("zoomMode")).toString(), QStringLiteral("Scale"));
    EXPECT_FALSE(stateB.contains(QStringLiteral("zoom")));
    EXPECT_FALSE(stateB.contains(QStringLiteral("zoomMode")));
    record(QStringLiteral("views-independent"));
}

TEST_F(ViewerDestinationSurface, TwoViewerPanelsRenderIndependentDestinations) {
    ASSERT_TRUE(waitFor(*controllerA_, 0)) << controllerA_->error().toStdString();
    ASSERT_TRUE(waitFor(*controllerB_, 0)) << controllerB_->error().toStdString();
    const auto presentationA = controllerA_->presentation();
    const auto presentationB = controllerB_->presentation();
    ASSERT_TRUE(presentationA);
    ASSERT_TRUE(presentationB);

    // Each panel published the target its own Viewer node addresses.
    EXPECT_EQ(QString::number(static_cast<qint64>(presentationA->request.output)), colorAId_);
    EXPECT_EQ(QString::number(static_cast<qint64>(presentationB->request.output)), colorBId_);
    EXPECT_EQ(static_cast<std::uint32_t>(presentationA->destination),
              static_cast<std::uint32_t>(*controllerA_->destination()));
    EXPECT_EQ(static_cast<std::uint32_t>(presentationB->destination),
              static_cast<std::uint32_t>(*controllerB_->destination()));
    EXPECT_TRUE(controllerA_->error().isEmpty());
    EXPECT_TRUE(controllerB_->error().isEmpty());
    record(QStringLiteral("initial"));
    capture(QStringLiteral("viewer-wide"));

    // Rapid supersession on A: only A's newest frame publishes and B's request
    // identity and presentation are untouched.
    const auto requestB = presentationB->request;
    controllerA_->setFrame(1);
    controllerA_->setFrame(2);
    controllerA_->setFrame(3);
    ASSERT_TRUE(waitFor(*controllerA_, 3)) << controllerA_->error().toStdString();
    EXPECT_EQ(controllerA_->presentation()->request.localTime, 3);
    ASSERT_TRUE(controllerB_->presentation());
    EXPECT_EQ(controllerB_->presentation()->request, requestB);
    record(QStringLiteral("supersession"));

    // Independent display settings: resolution sampling differs per
    // destination without changing the peer's evaluated request.
    controllerA_->setResolutionMode(QStringLiteral("half"));
    controllerB_->setResolutionMode(QStringLiteral("quarter"));
    QElapsedTimer resolutionDeadline;
    resolutionDeadline.start();
    while (resolutionDeadline.elapsed() < 60000 && (controllerA_->presentation()->request.samplingScale != 2 ||
                                                    controllerB_->presentation()->request.samplingScale != 4)) {
        QTest::qWait(10);
    }
    EXPECT_EQ(controllerA_->presentation()->request.samplingScale, 2);
    EXPECT_EQ(controllerB_->presentation()->request.samplingScale, 4);
    EXPECT_EQ(controllerA_->presentation()->request.output, presentationA->request.output);
    EXPECT_EQ(controllerB_->presentation()->request.output, presentationB->request.output);
    nemo::EvaluationRequest requestBIdentity = controllerB_->presentation()->request;
    record(QStringLiteral("resolution"));

    // Channel intent is resolved by the worker (#98), so B now addresses A
    // rather than RGBA while retaining its own target, time and geometry.
    controllerA_->setChannel(QStringLiteral("R"));
    controllerB_->setChannel(QStringLiteral("A"));
    QTest::qWait(80);
    auto alphaRequest = requestBIdentity;
    alphaRequest.channels = {"A"};
    EXPECT_EQ(controllerB_->presentation()->request, alphaRequest);
    EXPECT_EQ(controllerA_->presentation()->request.output, presentationA->request.output);
    record(QStringLiteral("channel"));
    // Restore normal channels so the geometry captures are attributable to each
    // destination's color, not the isolated display channel. This republishes
    // B; the transport isolation baseline is taken after it settles.
    controllerA_->setChannel(QStringLiteral("RGBA"));
    controllerB_->setChannel(QStringLiteral("RGBA"));
    QTest::qWait(80);

    // Real native input on panel A's own transport controls; panel B's
    // evaluated request and publication stay where they were.
    click(panelA_, QStringLiteral("nextButton"));
    ASSERT_TRUE(waitFor(*controllerA_, 4)) << controllerA_->error().toStdString();
    click(panelA_, QStringLiteral("inButton"));
    click(panelA_, QStringLiteral("outButton"));
    click(panelA_, QStringLiteral("playButton"));
    EXPECT_TRUE(controllerA_->playing());
    QTest::qWait(140);
    click(panelA_, QStringLiteral("playButton"));
    EXPECT_FALSE(controllerA_->playing());
    QTest::qWait(30);
    ASSERT_TRUE(controllerB_->presentation());
    EXPECT_EQ(controllerB_->presentation()->request, requestBIdentity);
    record(QStringLiteral("transport"));

    // Narrow/compact geometry before any panel closes: both panels fall below
    // the 560px header/display breakpoint and the 530px compact transport
    // breakpoint, so the compact layout is captured with both destinations
    // live. The shell's 960px minimum is relaxed for this evidence run only.
    window_->setMinimumWidth(0);
    window_->resize(800, 640);
    QTest::qWait(200);
    capture(QStringLiteral("viewer-narrow"));
    record(QStringLiteral("narrow"));

    // Closing panel A while its newest frame is in flight retires only A's
    // destination. The panel is removed through the workspace, so the QML
    // panel destruction performs the release exactly as a real close does.
    const auto destinationA = *controllerA_->destination();
    controllerA_->setFrame(5);
    record(QStringLiteral("pre-close"));
    controllerA_ = nullptr;
    workspace_->closePanel(panelA_);
    QTest::qWait(250);
    EXPECT_EQ(registry_->activeCount(), 1);
    // Closing one panel rebuilds the surrounding split, so the survivor's
    // controller may be re-created (and re-allocated a destination). What the
    // destination owns is stable: B still renders its own target and publishes
    // its own new frames.
    EXPECT_GE(static_cast<std::uint32_t>(destinationA), 2u);
    controllerB_ = qobject_cast<nemo::ui::ViewerController*>(registry_->controller(panelB_));
    ASSERT_NE(controllerB_, nullptr);
    ASSERT_TRUE(controllerB_->presentation());
    EXPECT_EQ(controllerB_->presentation()->request.output, presentationB->request.output);
    const auto requestBClosed = controllerB_->presentation()->requestId;
    controllerB_->setFrame(1);
    ASSERT_TRUE(waitFor(*controllerB_, 1)) << controllerB_->error().toStdString();
    EXPECT_EQ(controllerB_->presentation()->request.output, presentationB->request.output);
    EXPECT_NE(controllerB_->presentation()->requestId, requestBClosed);
    QJsonObject closed = destinationSnapshot(*controllerB_, *runtime_, QStringLiteral("closed-a-b"));
    records_.push_back(closed);
    capture(QStringLiteral("viewer-closed"));

    writeEvidence();
    EXPECT_EQ(warnings_->count(), 0);
}

TEST_F(ViewerDestinationSurface, CachedToLiveTransitionKeepsTransportPacing) {
    ASSERT_TRUE(waitFor(*controllerA_, 0));
    ASSERT_TRUE(waitFor(*controllerB_, 0));
    const auto before = waitForCacheCounts(
        [](const auto& counts) { return counts.cachePublished >= 2 && counts.cacheActiveFrames == 0; });
    controllerA_->setFrame(1);
    ASSERT_TRUE(waitFor(*controllerA_, 1));
    (void)waitForCacheCounts([&](const auto& counts) { return counts.cachePublished > before.cachePublished; });
    controllerA_->setFrame(0);
    ASSERT_TRUE(waitFor(*controllerA_, 0));
    controllerA_->setFrameRate(10.0);
    controllerA_->setMarkInFrame(0);
    controllerA_->setMarkOutFrame(3);

    struct Presented {
        int frame;
        bool cached;
        qint64 at;
    };
    std::vector<Presented> presented;
    QElapsedTimer clock;
    const auto connection = QObject::connect(controllerA_, &nemo::ui::ViewerController::framePresented, controllerA_,
                                             [&](int frame, int, int, bool cached, double) {
                                                 if (!controllerA_->playing() || frame == 0)
                                                     return;
                                                 presented.push_back({frame, cached, clock.elapsed()});
                                                 if (frame == 3)
                                                     controllerA_->pause();
                                             });
    clock.start();
    controllerA_->play();
    while (controllerA_->playing() && clock.elapsed() < 15000)
        QTest::qWait(2);
    controllerA_->pause();
    QObject::disconnect(connection);
    ASSERT_EQ(presented.size(), 3U) << "each cached and live frame must reach the real viewer";
    EXPECT_TRUE(presented.front().cached);
    EXPECT_FALSE(presented[1].cached);
    for (std::size_t i = 0; i < presented.size(); ++i) {
        EXPECT_EQ(presented[i].frame, static_cast<int>(i + 1));
        if (i != 0)
            EXPECT_GE(presented[i].at - presented[i - 1].at, 40)
                << "frame " << presented[i].frame << " must wait for its own transport deadline";
    }
}

// Issue #106 acceptance 3, exercised at the existing native seams: an explicitly
// populated 1080p range replays through the ordered playback window, forward and
// reverse, without the serial one-outstanding-frame transport. Every presented
// frame is recorded with its own arrival time, raster and cache origin, so the
// delivered cadence is measured from actual presentations instead of a
// throughput counter.
//
// The same body runs against two distinct storage configurations, because a
// single mixed tier proves neither: RamRetainedRangeReplay keeps every BC7
// payload of the range in the RAM tier (a replay needs no pack read), and
// DiskBackedRangeReplay keeps a handful of frames in RAM beside a bounded
// resident budget, so the range survives only as packs and every replayed frame
// is prepared from disk. Both tiers walk the full ordered 200-frame range in
// both directions, measure the delivered cadence against the composition rate,
// prove the reopened-cache path where a public seam can enforce it, and record
// their own cache counters. The two tiers are the two TEST_Fs at the bottom of
// this file; this member is their one body.
void ViewerDestinationSurface::cachedRangePlaysForwardAndReverseFromTheOrderedWindow() {
    const QString tier = cacheTierName();
    ASSERT_TRUE(waitFor(*controllerA_, 0)) << controllerA_->error().toStdString();

    // A real 1920x1080 canvas addressed at Full resolution over the whole domain:
    // the requested frames are the full 1080p outputs, not a panel ROI.
    const auto authored = session_->submit(nemo::setNetworkFormatCommand(networkIdentity(controllerA_->rootNetworkId()),
                                                                         nemo::ImageFormat{1920, 1080, 1.0F}),
                                           nemo::EditOptions{session_->revision(), {}});
    ASSERT_TRUE(authored.committed);
    controllerA_->setResolutionMode(QStringLiteral("full"));
    controllerA_->setForceFullFrame(true);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 120000) {
        const auto presentation = controllerA_->presentation();
        if (presentation && presentation->frame.width == 1920 && presentation->frame.height == 1080 &&
            presentation->request.samplingScale == 1)
            break;
        if (!controllerA_->error().isEmpty())
            FAIL() << controllerA_->error().toStdString();
        QTest::qWait(10);
    }
    const auto fullFrame = controllerA_->presentation();
    ASSERT_TRUE(fullFrame);
    ASSERT_EQ(fullFrame->frame.width, 1920);
    ASSERT_EQ(fullFrame->frame.height, 1080);
    ASSERT_EQ(fullFrame->request.samplingScale, 1);

    constexpr int kFrames = 200;
    constexpr int kLast = kFrames - 1;
    // One cached frame is the compressed display raster: 16 bytes per 4x4 BC7
    // block, i.e. one byte per texel at this extent. The range's payload is
    // therefore a known quantity, and the tier assertions below measure the RAM
    // tier and the packs against it instead of trusting a flag.
    const std::uint64_t framePayloadBytes =
        static_cast<std::uint64_t>(fullFrame->frame.width) * static_cast<std::uint64_t>(fullFrame->frame.height);
    const std::uint64_t rangePayloadBytes = framePayloadBytes * static_cast<std::uint64_t>(kFrames);
    // The tier is a property of the configuration the runtime was bootstrapped
    // with, not of this scenario: enough RAM for the range's payloads retains all
    // of them in the RAM tier, anything less leaves the range to the packs.
    const bool ramRetainsWholeRange = cacheOptions_.maxRamBytes >= rangePayloadBytes;

    // Populate exactly the requested range through the production cache-request
    // path; the marks cover it so the transport loops over those frames.
    controllerA_->setMarkInFrame(0);
    controllerA_->setMarkOutFrame(kLast);
    controllerA_->requestRange(0, kLast);
    QElapsedTimer population;
    population.start();
    while (population.elapsed() < 600000) {
        const auto counts = runtime_->counts(*controllerA_->destination());
        if (counts.cachePublished >= static_cast<std::uint64_t>(kFrames))
            break;
        if (!controllerA_->cacheError().isEmpty())
            FAIL() << controllerA_->cacheError().toStdString();
        QTest::qWait(20);
    }
    const auto populated = runtime_->counts(*controllerA_->destination());
    ASSERT_GE(populated.cachePublished, static_cast<std::uint64_t>(kFrames))
        << "requested 1080p range did not finish preparing";

    // Every presented frame of both passes, in presentation order.
    struct Presented {
        QString phase;
        int frame{};
        bool cacheHit{};
        int width{};
        int height{};
        double sincePassStartMs{};
        double requestToSwapMs{};
    };
    QVector<Presented> presented;
    QString phase;
    QElapsedTimer passClock;
    QObject::connect(
        controllerA_, &nemo::ui::ViewerController::framePresented, controllerA_,
        [&presented, &phase, &passClock](int frame, int width, int height, bool cacheHit, double requestToSwapMs) {
            presented.push_back(Presented{phase, frame, cacheHit, width, height,
                                          passClock.isValid() ? static_cast<double>(passClock.elapsed()) : 0.0,
                                          requestToSwapMs});
        });

    // A destination's scheduler counters stop moving once the previous pass's
    // retired preparations have been rejected, so a pass measures only its own
    // work instead of the previous window's leftovers.
    const auto settledCounts = [&] {
        auto previous = runtime_->counts(*controllerA_->destination());
        QElapsedTimer stable;
        stable.start();
        while (stable.elapsed() < 3000) {
            QTest::qWait(100);
            const auto current = runtime_->counts(*controllerA_->destination());
            if (current == previous)
                break;
            previous = current;
            stable.restart();
        }
        return previous;
    };

    // Publication is not persistence: the durable writer commits each record to
    // its pack asynchronously, and the physical disk budget counts pack headers
    // and dead records beside the payloads (issue #106). The tier is only stated
    // once the durable share of the range is really on disk, so the reopened
    // cache below indexes a complete range instead of the writer's backlog.
    QElapsedTimer durability;
    durability.start();
    while (durability.elapsed() < 600000) {
        const auto counts = runtime_->counts(*controllerA_->destination());
        if (counts.cacheDiskBytes >= rangePayloadBytes * 9 / 10)
            break;
        if (!controllerA_->cacheError().isEmpty())
            FAIL() << controllerA_->cacheError().toStdString();
        QTest::qWait(20);
    }
    const auto populationCounts = settledCounts();
    const auto populationMs = static_cast<double>(population.elapsed());
    // Physical bytes on disk, read from the file system rather than from the
    // cache's own accounting.
    const QJsonObject packs = packInventory();

    // The tier this run actually produced. A RAM-retained configuration must
    // hold the whole range's payloads in RAM and therefore replay without a pack
    // read; the disk-backed configuration must hold a few frames at most, keep
    // device residency inside its budget, and carry the range in packs.
    EXPECT_GE(populationCounts.cachePublished, static_cast<std::uint64_t>(kFrames));
    EXPECT_GE(populationCounts.cacheDiskBytes, rangePayloadBytes * 9 / 10)
        << tier.toStdString() << ": the range is not durably stored (" << populationCounts.cacheDiskBytes << " of "
        << rangePayloadBytes << " payload bytes)";
    EXPECT_LE(populationCounts.cacheCompressedRamBytes, cacheOptions_.maxRamBytes)
        << tier.toStdString() << ": the RAM tier exceeded its own budget";
    EXPECT_LE(populationCounts.cacheResidentBytes, cacheOptions_.maxResidentBytes)
        << tier.toStdString() << ": device residency exceeded its own budget";
    if (ramRetainsWholeRange) {
        EXPECT_GE(populationCounts.cacheCompressedRamBytes, rangePayloadBytes)
            << tier.toStdString() << ": the RAM tier did not retain the whole range";
    } else {
        EXPECT_LT(populationCounts.cacheCompressedRamBytes, rangePayloadBytes / 4)
            << tier.toStdString() << ": the negligible RAM tier retained the range instead of leaving it to the packs";
    }

    // One pass of the transport over the marked range in the stated direction.
    // The frame the pass starts from is displayed first — a seek, the separate
    // first preparation — and then the ordered window walks the rest.
    struct Pass {
        int firstIndex{};
        nemo::ui::ViewerRuntimeCounts before;
        nemo::ui::ViewerRuntimeCounts after;
        double wallMs{};
    };
    const auto runPass = [&](const QString& tag, int from, int step) {
        phase = tag;
        controllerA_->setPlaybackDirection(step > 0 ? nemo::ui::ViewerController::PlaybackDirection::Forward
                                                    : nemo::ui::ViewerController::PlaybackDirection::Reverse);
        controllerA_->setFrame(from);
        EXPECT_TRUE(waitFor(*controllerA_, from)) << controllerA_->error().toStdString();
        QTest::qWait(200);
        Pass pass;
        pass.firstIndex = presented.size();
        pass.before = settledCounts();
        passClock.restart();
        controllerA_->play();
        EXPECT_TRUE(controllerA_->playing());
        const int expectedLast = step > 0 ? kLast : 0;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 180000) {
            if (presented.size() > pass.firstIndex && presented.back().frame == expectedLast)
                break;
            if (!controllerA_->error().isEmpty())
                break;
            QTest::qWait(2);
        }
        controllerA_->pause();
        EXPECT_FALSE(controllerA_->playing());
        // The pass clock is stopped before the settle wait, so the wall duration
        // is the transport's own, not the scenario's bookkeeping.
        pass.wallMs = static_cast<double>(passClock.elapsed());
        QTest::qWait(80);
        pass.after = runtime_->counts(*controllerA_->destination());
        return pass;
    };

    const Pass forward = runPass(QStringLiteral("forward"), 0, 1);
    capture(QStringLiteral("viewer-replay-forward-") + tier);
    const Pass reverse = runPass(QStringLiteral("reverse"), kLast, -1);
    capture(QStringLiteral("viewer-replay-reverse-") + tier);
    // The tiers must still be the tiers they were after both passes: a
    // RAM-retained run that quietly evicted its payloads, or a disk-backed run
    // that grew a RAM copy of the range, is not the configuration its evidence
    // claims.
    const auto afterPasses = runtime_->counts(*controllerA_->destination());
    if (ramRetainsWholeRange) {
        EXPECT_GE(afterPasses.cacheCompressedRamBytes, rangePayloadBytes)
            << tier.toStdString() << ": the RAM tier lost the range during playback";
    } else {
        EXPECT_LT(afterPasses.cacheCompressedRamBytes, rangePayloadBytes / 4)
            << tier.toStdString() << ": the range formed a RAM copy during playback";
    }
    EXPECT_LE(afterPasses.cacheResidentBytes, cacheOptions_.maxResidentBytes)
        << tier.toStdString() << ": device residency exceeded its own budget during playback";

    // The delivered sequence is the whole range in the stated direction, in
    // order, with no duplicate, no skipped and no stale frame: the window never
    // presents a successor ahead of its predecessor, and a frame from another
    // view or direction can never appear.
    struct Verified {
        QVector<Presented> frames;
        int expectedFrames{};
        int initialFrame{};
        int mismatches{};
        int repeats{};
        int omissions{};
        int cacheMisses{};
        QString firstMismatch;
    };
    const auto verifyPass = [&](const QString& tag, int startIndex, int initialFrame, int firstFrame, int step) {
        Verified verified;
        verified.initialFrame = initialFrame;
        for (int index = startIndex; index < presented.size(); ++index)
            verified.frames.push_back(presented.at(index));
        const int expectedLast = step > 0 ? kLast : 0;
        int end = -1;
        for (int index = 0; index < verified.frames.size(); ++index) {
            if (verified.frames.at(index).frame == expectedLast) {
                end = index;
                break;
            }
        }
        if (end < 0) {
            ADD_FAILURE() << tag.toStdString() << ": pass never reached frame " << expectedLast;
            verified.frames.clear();
            return verified;
        }
        verified.frames.resize(end + 1);

        // The exact sequence the ordered window has to deliver: every frame of
        // the range after the initial displayed frame, in the stated direction,
        // exactly once each.
        QVector<int> expected;
        for (int frame = firstFrame; step > 0 ? frame <= kLast : frame >= 0; frame += step)
            expected.push_back(frame);
        verified.expectedFrames = expected.size();

        std::map<int, int> delivered;
        for (const auto& frame : verified.frames) {
            ++delivered[frame.frame];
            EXPECT_EQ(frame.width, 1920) << tag.toStdString() << ": frame " << frame.frame << " raster width";
            EXPECT_EQ(frame.height, 1080) << tag.toStdString() << ": frame " << frame.frame << " raster height";
            if (!frame.cacheHit) {
                ++verified.cacheMisses;
                ADD_FAILURE() << tag.toStdString() << ": frame " << frame.frame << " was not replayed from the cache";
            }
        }
        for (int frame = 0; frame < expected.size(); ++frame) {
            const int want = expected.at(frame);
            const int got = frame < verified.frames.size() ? verified.frames.at(frame).frame : -1;
            if (want == got)
                continue;
            ++verified.mismatches;
            if (verified.firstMismatch.isEmpty()) {
                verified.firstMismatch =
                    QStringLiteral("at position %1: expected frame %2, presented %3").arg(frame).arg(want).arg(got);
            }
        }
        if (verified.frames.size() > expected.size())
            verified.mismatches += verified.frames.size() - expected.size();
        for (const int frame : expected)
            if (!delivered.contains(frame))
                ++verified.omissions;
        for (const auto& [frame, count] : delivered)
            if (count > 1)
                verified.repeats += count - 1;

        EXPECT_EQ(verified.mismatches, 0)
            << tag.toStdString() << ": delivered order differs from the expected sequence ("
            << verified.firstMismatch.toStdString() << ")";
        EXPECT_EQ(verified.omissions, 0) << tag.toStdString() << ": frames of the range were never presented";
        EXPECT_EQ(verified.repeats, 0) << tag.toStdString() << ": frames were presented more than once";
        EXPECT_EQ(verified.cacheMisses, 0) << tag.toStdString() << ": frames were not replayed from the cache";
        return verified;
    };
    const auto forwardFrames = verifyPass(QStringLiteral("forward"), forward.firstIndex, 0, 1, 1);
    const auto reverseFrames = verifyPass(QStringLiteral("reverse"), reverse.firstIndex, kLast, kLast - 1, -1);

    // Delivered cadence from the actual presentation instants. The composition
    // interval is 1000/24 ms, but the transport's pacing quantum and the Qt
    // frame-swap boundary both quantize it, so no single gap is the cadence: the
    // MEAN of the delivered intervals is, and it is what a pass running at
    // another rate cannot fake. A 15 fps delivery would land at 1.6x the nominal
    // interval and fail the mean, the trimmed mean and the median together.
    const double interval = 1000.0 / controllerA_->frameRate();
    struct Cadence {
        QVector<double> gaps;
        QJsonArray stallFrames;
        QJsonObject histogram;
        double spanMs{};
        double meanMs{};
        double meanRatio{};
        double p50Ms{};
        double p50Ratio{};
        double p90Ms{};
        double p90Ratio{};
        double p99Ms{};
        double minMs{};
        double maxMs{};
        double maxRatio{};
        double trimmedMeanMs{};
        double trimmedRatio{};
        int stalls{};
        int bursts{};
    };
    const auto cadenceFor = [&](const QVector<Presented>& pass) {
        Cadence cadence;
        for (int index = 1; index < pass.size(); ++index)
            cadence.gaps.push_back(pass.at(index).sincePassStartMs - pass.at(index - 1).sincePassStartMs);
        if (cadence.gaps.isEmpty())
            return cadence;
        // The stall and burst accounts are positional, so they are read while the
        // gaps are still in presentation order.
        const QString bucketNames[] = {
            QStringLiteral("lt_0.5x"),   QStringLiteral("0.5x_0.75x"), QStringLiteral("0.75x_0.9x"),
            QStringLiteral("0.9x_1.1x"), QStringLiteral("1.1x_1.25x"), QStringLiteral("1.25x_1.5x"),
            QStringLiteral("1.5x_2x"),   QStringLiteral("2x_4x"),      QStringLiteral("ge_4x")};
        const double bucketEdges[] = {0.5, 0.75, 0.9, 1.1, 1.25, 1.5, 2.0, 4.0, 0.0};
        constexpr int kBuckets = 9;
        int counts[kBuckets] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int index = 0; index < cadence.gaps.size(); ++index) {
            const double gap = cadence.gaps.at(index);
            for (int bucket = 0; bucket < kBuckets; ++bucket) {
                if (bucketEdges[bucket] <= 0.0 || gap < interval * bucketEdges[bucket]) {
                    ++counts[bucket];
                    break;
                }
            }
            if (gap > interval * 1.5) {
                ++cadence.stalls;
                cadence.stallFrames.append(QJsonObject{{QStringLiteral("frame"), pass.at(index + 1).frame},
                                                       {QStringLiteral("gap_ms"), gap},
                                                       {QStringLiteral("ratio"), gap / interval}});
            }
            if (gap < interval * 0.4)
                ++cadence.bursts;
        }
        for (int bucket = 0; bucket < kBuckets; ++bucket)
            cadence.histogram[bucketNames[bucket]] = counts[bucket];

        double total = 0.0;
        for (const double gap : cadence.gaps)
            total += gap;
        // The percentiles read a sorted copy: the delivered gaps stay in
        // presentation order, which is what the per-frame evidence states.
        QVector<double> sorted = cadence.gaps;
        std::sort(sorted.begin(), sorted.end());
        const int last = static_cast<int>(sorted.size()) - 1;
        cadence.spanMs = total;
        cadence.meanMs = total / static_cast<double>(sorted.size());
        cadence.p50Ms = sorted.at(sorted.size() / 2);
        cadence.p90Ms = sorted.at(std::min(last, static_cast<int>(sorted.size()) * 9 / 10));
        cadence.p99Ms = sorted.at(std::min(last, static_cast<int>(sorted.size()) * 99 / 100));
        cadence.minMs = sorted.first();
        cadence.maxMs = sorted.last();
        // The cadence with the two worst intervals removed: an isolated native
        // scheduling hiccup must neither hide a transport that is really late nor
        // be mistaken for one.
        double trimmed = total;
        for (int dropped = 0; dropped < 2 && dropped <= last; ++dropped)
            trimmed -= sorted.at(last - dropped);
        cadence.trimmedMeanMs = trimmed / static_cast<double>(std::max(1, static_cast<int>(sorted.size()) - 2));
        cadence.meanRatio = cadence.meanMs / interval;
        cadence.p50Ratio = cadence.p50Ms / interval;
        cadence.p90Ratio = cadence.p90Ms / interval;
        cadence.maxRatio = cadence.maxMs / interval;
        cadence.trimmedRatio = cadence.trimmedMeanMs / interval;
        return cadence;
    };
    const Cadence forwardCadence = cadenceFor(forwardFrames.frames);
    const Cadence reverseCadence = cadenceFor(reverseFrames.frames);

    const auto assertCadence = [&interval](const QString& tag, const Cadence& cadence, int expectedFrames) {
        ASSERT_FALSE(cadence.gaps.isEmpty()) << tag.toStdString() << ": no presented intervals to measure";
        EXPECT_EQ(cadence.gaps.size(), expectedFrames - 1)
            << tag.toStdString() << ": presented frames and measured intervals disagree";
        // The delivered rate itself: the mean interval of the whole pass. A
        // transport running at another rate cannot satisfy this, and an isolated
        // CI hiccup cannot break it.
        EXPECT_GT(cadence.meanRatio, 0.85)
            << tag.toStdString() << ": mean interval " << cadence.meanMs << " ms is faster than the composition rate";
        EXPECT_LT(cadence.meanRatio, 1.15) << tag.toStdString() << ": mean interval " << cadence.meanMs << " ms ("
                                           << 1000.0 / cadence.meanMs << " fps) is slower than the composition rate";
        EXPECT_GT(cadence.trimmedRatio, 0.9)
            << tag.toStdString() << ": the cadence without its two worst intervals is still fast";
        EXPECT_LT(cadence.trimmedRatio, 1.1)
            << tag.toStdString() << ": the cadence without its two worst intervals is already late";
        EXPECT_GT(cadence.p50Ratio, 0.7) << tag.toStdString() << ": median interval faster than the composition rate";
        EXPECT_LT(cadence.p50Ratio, 1.35) << tag.toStdString() << ": median interval slower than the composition rate";
        EXPECT_LT(cadence.p90Ratio, 2.0) << tag.toStdString() << ": a stall entered the delivered cadence";
        // The tail is reported, not hidden: a single native scheduling hiccup is
        // allowed for, a frame arriving a third of a second after its due time is
        // a broken promise (the stalls above 1.5x are listed in the evidence).
        EXPECT_LT(cadence.maxRatio, 8.0) << tag.toStdString() << ": the transport stalled for " << cadence.maxMs
                                         << " ms";
        // Two frames inside one composition interval is a burst, and a paced
        // transport cannot produce one: the frame clock advances by the interval
        // before the next frame is due.
        EXPECT_EQ(cadence.bursts, 0) << tag.toStdString() << ": frames were presented as a burst";
    };
    assertCadence(QStringLiteral("forward"), forwardCadence, forwardFrames.expectedFrames);
    assertCadence(QStringLiteral("reverse"), reverseCadence, reverseFrames.expectedFrames);
    EXPECT_EQ(forwardFrames.frames.size(), kFrames - 1);
    EXPECT_EQ(reverseFrames.frames.size(), kFrames - 1);

    // --- reopened cache -------------------------------------------------------
    // A second, independent proof that the range is really on disk and can be
    // served from there: the worker's ViewerSession is replaced, which replaces
    // the ViewerCache with it, and the replacement indexes the same namespace by
    // scanning the packs. Nothing of the old cache survives that swap — not the
    // RAM tier, not device residency, not the publication counters — so a frame
    // served afterwards can only be a pack read and its BC7 upload.
    //
    // The one public seam that can replace the session is the colour
    // configuration identity a request states: ViewerRuntime keeps one session
    // per identity and rebuilds it when a request states a different path.
    // Neither ViewerRuntime nor ViewerController exposes its session and
    // ViewerSession::configureCache refuses a second configuration, so this is
    // the only reopened-cache seam public API offers. The swap vehicle is a
    // metadata-only Describe, so a replacement renders nothing, publishes nothing
    // and writes no pack.
    //
    // Limitation, stated exactly: the ordered window's own replay entry point
    // (ViewerSession::replay) is served from the session's in-memory demand
    // records, which a reopened session does not have — the index ViewerCache
    // rebuilds from packs maps identities, not intents. The reopened evidence is
    // therefore taken through the panel's ordinary request path
    // (ViewerController::setFrame), which resolves the demand and then finds the
    // retained representation in the reopened cache; `published == 0` is what
    // proves that no live frame was produced for it.
    const std::string originalConfig = session_->colorConfigPath();
    const QVector<int> probes{6, 7, 8};
    QJsonArray probeRecords;
    int servedFromCache = 0;
    bool reopened = false;
    nemo::ui::ViewerRuntimeCounts reopenedCounts{};
    nemo::ui::ViewerRuntimeCounts afterProbes{};
    // The alternate identity has to differ from the active one as a string while
    // still naming the configuration the fixture published: it is the only OCIO
    // file this evidence may depend on.
    std::string alternateConfig = viewingConfigFile_;
    if (alternateConfig == originalConfig) {
        const std::filesystem::path file(viewingConfigFile_);
        alternateConfig = (file.parent_path() / "." / file.filename()).string();
    }
    if (viewingConfigFile_.empty()) {
        ADD_FAILURE() << "the fixture published no colour configuration file to state as the alternate identity";
    } else {
        EXPECT_NE(originalConfig, alternateConfig)
            << "the alternate configuration identity must differ from the active one";
        reopened = replaceWorkerCacheSession(alternateConfig) && replaceWorkerCacheSession(originalConfig);
        EXPECT_TRUE(reopened)
            << "the worker session could not be replaced through the public seams, so no reopened cache was observed";
    }
    if (reopened) {
        // The reopened cache's own counters. A busy cache reports no snapshot, so
        // the replacement is observed by waiting for the predicate instead of
        // reading once: the previous cache had published the whole range, this
        // one publishes nothing and indexes the packs it found.
        reopenedCounts = waitForCacheCounts([&](const nemo::ui::ViewerRuntimeCounts& counts) {
            return counts.cachePublished == 0 && counts.cacheDiskBytes >= rangePayloadBytes * 9 / 10;
        });
        EXPECT_EQ(reopenedCounts.cachePublished, 0)
            << "the reopened cache published frames of its own: the worker session was not replaced";
        EXPECT_GE(reopenedCounts.cacheDiskBytes, rangePayloadBytes * 9 / 10)
            << "the reopened cache did not index the range's packs";
        EXPECT_LT(reopenedCounts.cacheCompressedRamBytes, framePayloadBytes)
            << "the reopened cache retained compressed payloads in RAM";
        // Residency this early can only be a pack read that already uploaded a
        // frame; what matters is that it is inside the tier's own resident budget.
        EXPECT_LE(reopenedCounts.cacheResidentBytes, cacheOptions_.maxResidentBytes)
            << "the reopened cache exceeded the resident budget";

        for (const int frame : probes) {
            QElapsedTimer seekClock;
            seekClock.start();
            controllerA_->setFrame(frame);
            const bool arrived = waitFor(*controllerA_, frame);
            const auto presentation = controllerA_->presentation();
            const bool hit = arrived && presentation && presentation->request.localTime == frame &&
                             presentation->cacheHit && presentation->frame.width == 1920 &&
                             presentation->frame.height == 1080;
            if (hit)
                ++servedFromCache;
            probeRecords.append(QJsonObject{{QStringLiteral("frame"), frame},
                                            {QStringLiteral("arrived"), arrived},
                                            {QStringLiteral("cache_hit"), hit},
                                            {QStringLiteral("width"), presentation ? presentation->frame.width : 0},
                                            {QStringLiteral("height"), presentation ? presentation->frame.height : 0},
                                            {QStringLiteral("seek_ms"), static_cast<double>(seekClock.elapsed())},
                                            {QStringLiteral("status"), controllerA_->status()},
                                            {QStringLiteral("error"), controllerA_->error()}});
        }
        // A fresh snapshot of the reopened cache's counters: anything the probes
        // published, or any residency they took, is visible here.
        afterProbes = waitForCacheCounts(
            [](const nemo::ui::ViewerRuntimeCounts& counts) {
                return counts.cachePublished >= 1 || counts.cacheResidentFrames >= 1;
            },
            5000);
        EXPECT_EQ(servedFromCache, static_cast<int>(probes.size()))
            << "a frame of the range was not served from the reopened cache (the reopened cache published "
            << afterProbes.cachePublished << " frames of its own)";
        EXPECT_EQ(afterProbes.cachePublished, 0)
            << "a probe frame was rendered live instead of being served from the reopened cache";
    }

    // The reopened cache indexed the packs the first cache wrote: reopening the
    // namespace neither rewrote nor removed them.
    const QJsonObject packsAfterReopen = packInventory();

    if (!evidenceDirectory_.isEmpty()) {
        const auto tierJson = [](const nemo::ui::ViewerRuntimeCounts& counts) {
            return QJsonObject{
                {QStringLiteral("published"), static_cast<qint64>(counts.cachePublished)},
                {QStringLiteral("disk_bytes"), static_cast<qint64>(counts.cacheDiskBytes)},
                {QStringLiteral("compressed_ram_bytes"), static_cast<qint64>(counts.cacheCompressedRamBytes)},
                {QStringLiteral("resident_frames"), static_cast<qint64>(counts.cacheResidentFrames)},
                {QStringLiteral("resident_bytes"), static_cast<qint64>(counts.cacheResidentBytes)},
                {QStringLiteral("active_frames"), static_cast<qint64>(counts.cacheActiveFrames)},
                {QStringLiteral("queued"), static_cast<qint64>(counts.queued)},
                {QStringLiteral("dropped"), static_cast<qint64>(counts.dropped)},
                {QStringLiteral("stale_rejected"), static_cast<qint64>(counts.staleRejected)},
                {QStringLiteral("completed"), static_cast<qint64>(counts.completed)},
                {QStringLiteral("errors"), static_cast<qint64>(counts.cacheErrors)},
                {QStringLiteral("error"), QString::fromStdString(counts.cacheError)}};
        };
        const auto cadenceJson = [&](const Cadence& cadence) {
            QJsonArray gaps;
            for (const double gap : cadence.gaps)
                gaps.append(gap);
            return QJsonObject{{QStringLiteral("span_ms"), cadence.spanMs},
                               {QStringLiteral("measured_fps"), cadence.meanMs > 0.0 ? 1000.0 / cadence.meanMs : 0.0},
                               {QStringLiteral("mean_ms"), cadence.meanMs},
                               {QStringLiteral("mean_ratio"), cadence.meanRatio},
                               {QStringLiteral("trimmed_mean_ms"), cadence.trimmedMeanMs},
                               {QStringLiteral("trimmed_mean_ratio"), cadence.trimmedRatio},
                               {QStringLiteral("p50_ms"), cadence.p50Ms},
                               {QStringLiteral("p50_ratio"), cadence.p50Ratio},
                               {QStringLiteral("p90_ms"), cadence.p90Ms},
                               {QStringLiteral("p90_ratio"), cadence.p90Ratio},
                               {QStringLiteral("p99_ms"), cadence.p99Ms},
                               {QStringLiteral("min_ms"), cadence.minMs},
                               {QStringLiteral("max_ms"), cadence.maxMs},
                               {QStringLiteral("max_ratio"), cadence.maxRatio},
                               {QStringLiteral("stalls"), cadence.stalls},
                               {QStringLiteral("bursts"), cadence.bursts},
                               {QStringLiteral("stall_frames"), cadence.stallFrames},
                               {QStringLiteral("histogram"), cadence.histogram},
                               {QStringLiteral("gap_ms"), gaps}};
        };
        const auto deliveredJson = [](const Verified& verified) {
            return QJsonObject{{QStringLiteral("presented"), verified.frames.size()},
                               {QStringLiteral("expected"), verified.expectedFrames},
                               {QStringLiteral("initial_displayed_frame"), verified.initialFrame},
                               {QStringLiteral("mismatches"), verified.mismatches},
                               {QStringLiteral("repeats"), verified.repeats},
                               {QStringLiteral("omissions"), verified.omissions},
                               {QStringLiteral("cache_misses"), verified.cacheMisses},
                               {QStringLiteral("first_mismatch"), verified.firstMismatch}};
        };
        const auto framesJson = [](const Verified& verified, const Cadence& cadence) {
            QJsonArray frames;
            for (int index = 0; index < verified.frames.size(); ++index) {
                const auto& frame = verified.frames.at(index);
                frames.append(QJsonObject{{QStringLiteral("phase"), frame.phase},
                                          {QStringLiteral("frame"), frame.frame},
                                          {QStringLiteral("cache_hit"), frame.cacheHit},
                                          {QStringLiteral("width"), frame.width},
                                          {QStringLiteral("height"), frame.height},
                                          {QStringLiteral("since_pass_start_ms"), frame.sincePassStartMs},
                                          {QStringLiteral("gap_ms"), index > 0 ? cadence.gaps.at(index - 1) : 0.0},
                                          {QStringLiteral("request_to_swap_ms"), frame.requestToSwapMs}});
            }
            return frames;
        };
        QJsonArray frames;
        for (const auto& frame : framesJson(forwardFrames, forwardCadence))
            frames.append(frame);
        for (const auto& frame : framesJson(reverseFrames, reverseCadence))
            frames.append(frame);
        const QJsonObject report{
            {QStringLiteral("issue"), 106},
            {QStringLiteral("scenario"),
             QStringLiteral("cached 1080p range, ordered playback window, forward and reverse")},
            {QStringLiteral("tier"), tier},
            {QStringLiteral("frames_requested"), kFrames},
            {QStringLiteral("frame_rate"), controllerA_->frameRate()},
            {QStringLiteral("interval_ms"), interval},
            {QStringLiteral("frame_payload_bytes"), static_cast<qint64>(framePayloadBytes)},
            {QStringLiteral("range_payload_bytes"), static_cast<qint64>(rangePayloadBytes)},
            {QStringLiteral("ram_retains_whole_range"), ramRetainsWholeRange},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("timestamp_boundary"),
             QStringLiteral("GUI receipt of real destination-scoped Qt frameSwapped notifications; "
                            "includes queued delivery jitter, not physical scanout")},
            {QStringLiteral("device_pixel_ratio"), window_ ? window_->devicePixelRatio() : 0.0},
            {QStringLiteral("cache_options"),
             QJsonObject{{QStringLiteral("directory"), QString::fromStdString(cacheOptions_.directory)},
                         {QStringLiteral("max_ram_bytes"), static_cast<qint64>(cacheOptions_.maxRamBytes)},
                         {QStringLiteral("max_resident_bytes"), static_cast<qint64>(cacheOptions_.maxResidentBytes)},
                         {QStringLiteral("max_pending_bytes"), static_cast<qint64>(cacheOptions_.maxPendingBytes)},
                         {QStringLiteral("max_disk_bytes"), static_cast<qint64>(cacheOptions_.maxDiskBytes)},
                         {QStringLiteral("max_pending_frames"), static_cast<qint64>(cacheOptions_.maxPendingFrames)},
                         {QStringLiteral("max_replay_frames"), static_cast<qint64>(cacheOptions_.maxReplayFrames)}}},
            {QStringLiteral("population_ms"), populationMs},
            {QStringLiteral("packs_on_disk"), packs},
            {QStringLiteral("packs_after_reopen"), packsAfterReopen},
            {QStringLiteral("cache_after_population"), tierJson(populationCounts)},
            {QStringLiteral("cache_after_passes"), tierJson(afterPasses)},
            {QStringLiteral("forward_delivered"), deliveredJson(forwardFrames)},
            {QStringLiteral("reverse_delivered"), deliveredJson(reverseFrames)},
            {QStringLiteral("forward_cadence"), cadenceJson(forwardCadence)},
            {QStringLiteral("reverse_cadence"), cadenceJson(reverseCadence)},
            {QStringLiteral("forward_wall_ms"), forward.wallMs},
            {QStringLiteral("reverse_wall_ms"), reverse.wallMs},
            {QStringLiteral("forward_counters_before"), tierJson(forward.before)},
            {QStringLiteral("forward_counters_after"), tierJson(forward.after)},
            {QStringLiteral("reverse_counters_before"), tierJson(reverse.before)},
            {QStringLiteral("reverse_counters_after"), tierJson(reverse.after)},
            {QStringLiteral("reopened_cache"),
             QJsonObject{{QStringLiteral("observed"), reopened},
                         {QStringLiteral("seam"),
                          QStringLiteral("worker session replaced by the colour-configuration identity a request "
                                         "states (ViewerRuntime::describe with an alternate path)")},
                         {QStringLiteral("active_config"), QString::fromStdString(originalConfig)},
                         {QStringLiteral("alternate_config"), QString::fromStdString(alternateConfig)},
                         {QStringLiteral("probes"), probeRecords},
                         {QStringLiteral("served_from_cache"), servedFromCache},
                         {QStringLiteral("counters"), tierJson(reopenedCounts)},
                         {QStringLiteral("counters_after_probes"), tierJson(afterProbes)},
                         {QStringLiteral("ordered_replay_window_note"),
                          QStringLiteral("a reopened session has no in-memory demand records, so the ordered "
                                         "window's replay entry point cannot serve it: the probes go through the "
                                         "panel's request path, and published == 0 proves no live frame was "
                                         "produced for them")}}},
            {QStringLiteral("captures_note"),
             QStringLiteral("captures are taken immediately after each pass; grabbing during a pass would stall the "
                            "delivered cadence this scenario measures")},
            {QStringLiteral("frames"), frames}};
        QFile reportFile(evidenceDirectory_ + QStringLiteral("/replay-ordered-window-") + tier +
                         QStringLiteral(".json"));
        ASSERT_TRUE(reportFile.open(QIODevice::WriteOnly));
        reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    }
    EXPECT_EQ(warnings_->count(), 0);
}

// Tier 1 of issue #106 acceptance 3: enough RAM to retain every BC7 payload of
// the 200-frame 1080p range (200 x 1920 x 1080 bytes), so the ordered window
// replays the range without a single pack read.
class RamRetainedRangeReplay : public ViewerDestinationSurface {
protected:
    [[nodiscard]] nemo::eval::ViewerCacheOptions createCacheOptions() const override {
        auto options = ViewerDestinationSurface::createCacheOptions();
        options.maxRamBytes = 512ULL * 1024ULL * 1024ULL;
        options.maxResidentBytes = 512ULL * 1024ULL * 1024ULL;
        return options;
    }
    [[nodiscard]] QString cacheTierName() const override { return QStringLiteral("ram-retained"); }
};

// Tier 2 of issue #106 acceptance 3: a negligible RAM tier (a few 1080p frames)
// beside a bounded resident budget, so the range survives only as packs and
// every replayed frame is prepared from disk.
class DiskBackedRangeReplay : public ViewerDestinationSurface {
protected:
    [[nodiscard]] nemo::eval::ViewerCacheOptions createCacheOptions() const override {
        auto options = ViewerDestinationSurface::createCacheOptions();
        options.maxRamBytes = 32ULL * 1024ULL * 1024ULL;
        options.maxResidentBytes = 32ULL * 1024ULL * 1024ULL;
        return options;
    }
    [[nodiscard]] QString cacheTierName() const override { return QStringLiteral("disk-backed"); }
};

TEST_F(RamRetainedRangeReplay, CachedRangePlaysForwardAndReverseFromTheOrderedWindow) {
    cachedRangePlaysForwardAndReverseFromTheOrderedWindow();
}

TEST_F(DiskBackedRangeReplay, CachedRangePlaysForwardAndReverseFromTheOrderedWindow) {
    cachedRangePlaysForwardAndReverseFromTheOrderedWindow();
}

}  // namespace
