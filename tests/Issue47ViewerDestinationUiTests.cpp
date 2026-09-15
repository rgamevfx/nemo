#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ProjectFileController.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerItem.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
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
        evidenceDirectory_ = qEnvironmentVariable("NEMO_ISSUE47_EVIDENCE_DIR");

        runtime_ = std::make_unique<nemo::ui::ViewerRuntime>();
        nemo::eval::ViewerCacheOptions cacheOptions;
        cacheOptions.directory = directory_.filePath(QStringLiteral("cache")).toStdString();
        cacheOptions.encoding.codec = "libx264-cpu";
        cacheOptions.chunkFrames = 1;
        std::vector<std::string> extensions{"VK_KHR_surface"};
        if (QGuiApplication::platformName() == QStringLiteral("wayland"))
            extensions.push_back("VK_KHR_wayland_surface");
        else if (QGuiApplication::platformName() == QStringLiteral("xcb"))
            extensions.push_back("VK_KHR_xcb_surface");
        try {
            runtime_->bootstrap(extensions, NEMO_SLANG_SPV_DIR, cacheOptions);
        } catch (const nemo::gpu::GpuException& error) {
            if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
                GTEST_SKIP() << error.what();
            throw;
        }

        session_ = std::make_unique<nemo::ProjectSession>();
        router_ = std::make_unique<nemo::ui::PanelContextRouter>(*session_);
        facade_ = std::make_unique<nemo::ui::ViewerController>(runtime_.get(), *session_);
        registry_ = std::make_unique<nemo::ui::ViewerControllerRegistry>(runtime_.get(), *session_);

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
            if (presentation && presentation->request.localTime == frame)
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
    const auto requestB = presentationB->requestId;
    controllerA_->setFrame(1);
    controllerA_->setFrame(2);
    controllerA_->setFrame(3);
    ASSERT_TRUE(waitFor(*controllerA_, 3)) << controllerA_->error().toStdString();
    EXPECT_EQ(controllerA_->presentation()->request.localTime, 3);
    ASSERT_TRUE(controllerB_->presentation());
    EXPECT_EQ(controllerB_->presentation()->requestId, requestB);
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

    // Panel-local channel isolation is presentation-only: it may republish the
    // same evaluation request, but B's evaluated request is unchanged.
    controllerA_->setChannel(QStringLiteral("R"));
    controllerB_->setChannel(QStringLiteral("A"));
    QTest::qWait(80);
    EXPECT_EQ(controllerB_->presentation()->request, requestBIdentity);
    EXPECT_EQ(controllerA_->presentation()->request.output, presentationA->request.output);
    record(QStringLiteral("channel"));
    // Restore normal channels so the geometry captures are attributable to each
    // destination's color, not the isolated display channel. This republishes
    // B; the transport isolation baseline is taken after it settles.
    controllerA_->setChannel(QStringLiteral("RGBA"));
    controllerB_->setChannel(QStringLiteral("RGBA"));
    QTest::qWait(80);
    const auto requestBChannel = controllerB_->presentation()->requestId;

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
    EXPECT_EQ(controllerB_->presentation()->requestId, requestBChannel);
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

}  // namespace
