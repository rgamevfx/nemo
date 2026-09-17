// Native Crop/Reformat workflow regression (issue #92, stories 43-44, 46-47,
// 50, 52).
//
// The reference workflow is driven through the real presentation the application
// composes: a Parameters panel and a Viewer panel in one group, the shared
// ProjectSession -> commands -> history seam, and the production
// ViewerController/ViewerRuntime on an actual QQuickWindow. Every assertion is
// about authored document state or the panel's own published transform, never
// about an internal helper: a drag is real pointer input, a typed value is real
// key input, and a cancel is the real Escape/Undo path.
//
// It runs only on a real windowing platform with the Vulkan scene graph:
// NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 (same gate as issue #47/#74).

#include "HistoryController.hpp"
#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ProjectFileController.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerRuntime.hpp"
#include "Workspace.hpp"
#include "WorkspaceController.hpp"

#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/gpu/Error.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef NEMO_SLANG_SPV_DIR
#define NEMO_SLANG_SPV_DIR ""
#endif

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
// that owns one panel's image area, its view transform and the crop handles.
QQuickItem* viewerPanelFor(QQuickItem* content, const QString& panelId) {
    QQuickItem* field = visualByName(content, QStringLiteral("viewerFrame_") + panelId);
    while (field && field->objectName() != QStringLiteral("viewerPanel"))
        field = field->parentItem();
    return field;
}

// One key event per character, so a typed value really travels the same key
// path an artist's keyboard does.
void typeText(QWindow* window, const QString& text) {
    for (const QChar character : text)
        QTest::keyClick(window, character.toLatin1());
}

nemo::NetworkId networkIdentity(const QString& value) {
    return static_cast<nemo::NetworkId>(value.toULongLong());
}

nemo::NodeId nodeIdentity(const QString& value) {
    return static_cast<nemo::NodeId>(value.toULongLong());
}

// One zero-argument panel function read back as a value (Qt.rect, a number).
// The panel's own mapping is what the artist's pointer is interpreted through,
// so a scenario addresses the handles where the panel really states them.
QVariant callFunction(QQuickItem* item, const char* name) {
    QVariant result;
    const QMetaObject* meta = item ? item->metaObject() : nullptr;
    if (!meta)
        return result;
    for (int index = 0; index < meta->methodCount(); ++index) {
        const auto candidate = meta->method(index);
        if (candidate.name() != name || candidate.parameterCount() != 0)
            continue;
        candidate.invoke(item, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result));
        break;
    }
    return result;
}

// The first visible item carrying `property` == `value`. A combo popup's own
// delegate declares `modelData`, so a scenario selects the entry the artist
// sees instead of reaching into the control's model.
QQuickItem* visualWithProperty(QQuickItem* root, const char* property, const QVariant& value) {
    if (!root)
        return nullptr;
    if (root->property(property) == value)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualWithProperty(child, property, value))
            return found;
    }
    return nullptr;
}

class CropReformatSurface : public testing::Test {
protected:
    QTemporaryDir directory_;
    std::unique_ptr<nemo::ui::ViewerRuntime> runtime_;
    std::unique_ptr<nemo::ProjectSession> session_;
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
    QString viewerPanelId_{QStringLiteral("panel-viewer")};
    QString parametersPanelId_{QStringLiteral("panel-params")};
    nemo::ui::ViewerController* controller_{};
    QString network_;
    QString plateId_;
    QString cropId_;
    QString viewerId_;
    QString evidenceDirectory_;

    void SetUp() override {
        if (qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") != 1 ||
            qEnvironmentVariableIntValue("NEMO_TEST_VIEWER_WINDOW") != 1) {
            GTEST_SKIP() << "native Crop/Reformat evidence requires NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1";
        }
        if (std::string(NEMO_SLANG_SPV_DIR).empty())
            GTEST_SKIP() << "native Crop/Reformat evidence requires compiled Slang shaders";
        const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                            "docs/evidence/issue12-view.ocio";
        qputenv("OCIO", config.string().c_str());
        evidenceDirectory_ = qEnvironmentVariable("NEMO92_EVIDENCE_DIR");

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
        history_ = std::make_unique<nemo::ui::HistoryController>(*session_);
        router_ = std::make_unique<nemo::ui::PanelContextRouter>(*session_);
        facade_ = std::make_unique<nemo::ui::ViewerController>(runtime_.get(), *session_);
        registry_ = std::make_unique<nemo::ui::ViewerControllerRegistry>(runtime_.get(), *session_);

        // A viewer and a parameters panel in the SAME group, so the handles and
        // the inspector really share the group context the contract describes.
        const auto workspacePath = directory_.filePath(QStringLiteral("workspace.json"));
        const auto panel = [](const QString& id, const QString& type) {
            const QJsonObject item{
                {QStringLiteral("id"), id},
                {QStringLiteral("type"), type},
                {QStringLiteral("group"), QStringLiteral("A")},
                {QStringLiteral("state"),
                 QJsonObject{{QStringLiteral("viewerIndex"), 0}, {QStringLiteral("inspectors"), QJsonArray{}}}}};
            return QJsonObject{{QStringLiteral("id"), id + QStringLiteral("-leaf")},
                               {QStringLiteral("kind"), QStringLiteral("tabs")},
                               {QStringLiteral("active"), id},
                               {QStringLiteral("panels"), QJsonArray{item}}};
        };
        const QJsonObject layout{{QStringLiteral("version"), 1},
                                 {QStringLiteral("root"),
                                  QJsonObject{{QStringLiteral("id"), QStringLiteral("split-root")},
                                              {QStringLiteral("kind"), QStringLiteral("split")},
                                              {QStringLiteral("orientation"), QStringLiteral("horizontal")},
                                              {QStringLiteral("ratio"), 0.62},
                                              {QStringLiteral("children"),
                                               QJsonArray{panel(viewerPanelId_, QStringLiteral("viewer")),
                                                          panel(parametersPanelId_, QStringLiteral("parameters"))}}}}};
        const QJsonObject document{{QStringLiteral("version"), 2},
                                   {QStringLiteral("activeWorkspaceId"), QStringLiteral("workspace-1")},
                                   {QStringLiteral("workspaces"),
                                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-1")},
                                                           {QStringLiteral("name"), QStringLiteral("Crop reformat")},
                                                           {QStringLiteral("layout"), layout}}}}};
        QFile workspaceFile(workspacePath);
        ASSERT_TRUE(workspaceFile.open(QIODevice::WriteOnly));
        workspaceFile.write(QJsonDocument(document).toJson());
        workspaceFile.close();
        workspace_ = std::make_unique<nemo::workspace::WorkspaceController>(workspacePath);
        ASSERT_TRUE(workspace_->error().isEmpty()) << workspace_->error().toStdString();
        workspace_->registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"),
                                      QStringLiteral("ViewerPanel.qml"));
        workspace_->registerPanelType(QStringLiteral("parameters"), QStringLiteral("Parameters"),
                                      QStringLiteral("ParametersPanel.qml"));
        router_->setWorkspaceController(workspace_.get());

        controller_ = qobject_cast<nemo::ui::ViewerController*>(registry_->controller(viewerPanelId_));
        ASSERT_NE(controller_, nullptr);
        ASSERT_TRUE(controller_->destination().has_value());

        // TestPattern -> Crop -> Viewer. The crop is the DIRECTLY viewed node,
        // which is exactly the node whose box the handles may edit.
        network_ = controller_->rootNetworkId();
        plateId_ = controller_->createGraphNode(network_, QStringLiteral("testpattern"), QStringLiteral("plate"), 0.0,
                                                0.0, {}, {});
        ASSERT_FALSE(plateId_.isEmpty());
        cropId_ =
            controller_->createGraphNode(network_, QStringLiteral("crop"), QStringLiteral("crop1"), 0.0, 90.0, {}, {});
        ASSERT_FALSE(cropId_.isEmpty()) << "the crop node type must be registered";
        viewerId_ = controller_->createGraphNode(network_, QStringLiteral("viewer"), QStringLiteral("view1"), 0.0,
                                                 180.0, {}, {});
        ASSERT_FALSE(viewerId_.isEmpty());
        ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, plateId_, 0, cropId_, 0));
        ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, cropId_, 0, viewerId_, 0));
        controller_->setActiveViewer(network_, 0);

        chooser_ = std::make_unique<nemo::ui::NativeFileChooser>();
        projectFile_ = std::make_unique<nemo::ui::ProjectFileController>(*session_, *workspace_, *router_, *chooser_);
        editors_ = std::make_unique<nemo::ui::ParameterEditorRegistry>();
        for (const auto& contribution : nemo::builtinNodeContributions()->entries()) {
            for (const auto& editor : contribution.editors) {
                QStringList consumes;
                for (const auto& key : editor.consumes)
                    consumes.push_back(QString::fromStdString(key));
                ASSERT_TRUE(editors_->registerEditor(QString::fromStdString(editor.id),
                                                     QUrl(QString::fromStdString(editor.source)), consumes,
                                                     QString::fromStdString(editor.presentation)));
            }
        }

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

        const QString attachError = runtime_->attachToWindow(window_);
        ASSERT_TRUE(attachError.isEmpty()) << attachError.toStdString();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window_));
    }

    void TearDown() override {
        if (runtime_)
            runtime_->stopWorker();
        warnings_.reset();
        engine_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (runtime_)
            runtime_->quiesceForTeardown();
        editors_.reset();
        projectFile_.reset();
        chooser_.reset();
        registry_.reset();
        facade_.reset();
        workspace_.reset();
        router_.reset();
        history_.reset();
        session_.reset();
        runtime_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // --- scene helpers -----------------------------------------------------
    QQuickItem* item(const QString& name) const { return visualByName(window_->contentItem(), name); }

    QPoint center(QQuickItem* target) const {
        return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
    }

    QQuickItem* viewerPanel() const { return viewerPanelFor(window_->contentItem(), viewerPanelId_); }

    QQuickItem* parametersPanel() const { return item(QStringLiteral("parametersPanel")); }

    bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 120000) const {
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < timeoutMs) {
            if (predicate())
                return true;
            QTest::qWait(10);
        }
        return predicate();
    }

    // --- document helpers --------------------------------------------------
    double authoredNumber(const QString& node, const QString& key) const {
        const auto values = session_->queryValues(networkIdentity(network_), nodeIdentity(node), key.toStdString());
        for (const auto& value : values) {
            if (value.key != key.toStdString())
                continue;
            if (const auto* number = std::get_if<double>(&value.value))
                return *number;
            if (const auto* integer = std::get_if<std::int64_t>(&value.value))
                return static_cast<double>(*integer);
        }
        return std::nan("");
    }

    void setCropBox(double x, double y, double right, double top) {
        // Four authored edits through the shared command path; the caller states
        // the expected box afterwards, so this is setup only.
        controller_->setNodeParameter(cropId_, QStringLiteral("x"), x);
        controller_->setNodeParameter(cropId_, QStringLiteral("y"), y);
        controller_->setNodeParameter(cropId_, QStringLiteral("right"), right);
        controller_->setNodeParameter(cropId_, QStringLiteral("top"), top);
    }

    void inspect(const QString& node) {
        auto* panel = parametersPanel();
        ASSERT_NE(panel, nullptr);
        QVariant result;
        ASSERT_TRUE(QMetaObject::invokeMethod(panel, "openInspector", Qt::DirectConnection,
                                              Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, network_),
                                              Q_ARG(QVariant, node)));
        QTest::qWait(50);
    }

    void closeInspection(const QString& node) {
        auto* panel = parametersPanel();
        ASSERT_NE(panel, nullptr);
        QVariant result;
        ASSERT_TRUE(QMetaObject::invokeMethod(panel, "closeInspector", Qt::DirectConnection,
                                              Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, network_),
                                              Q_ARG(QVariant, node)));
        QTest::qWait(50);
    }

    // --- input helpers -----------------------------------------------------
    void moveTo(const QPoint& position, Qt::MouseButton held = Qt::LeftButton) {
        QMouseEvent move(QEvent::MouseMove, QPointF(position), QPointF(window_->mapToGlobal(position)), Qt::NoButton,
                         held, Qt::NoModifier);
        QGuiApplication::sendEvent(window_, &move);
        QTest::qWait(5);
    }

    // A real press / motion / release gesture: the intermediate moves carry the
    // held button, so the panel sees one continuous drag.
    void drag(const QPoint& from, const QPoint& to, Qt::MouseButton button = Qt::LeftButton, int steps = 4) {
        QTest::mousePress(window_, button, Qt::NoModifier, from);
        for (int step = 1; step <= steps; ++step) {
            const QPoint position(from.x() + (to.x() - from.x()) * step / steps,
                                  from.y() + (to.y() - from.y()) * step / steps);
            moveTo(position, button);
        }
        QTest::mouseRelease(window_, button, Qt::NoModifier, to);
        QTest::qWait(20);
    }

    void capture(const QString& name) {
        if (evidenceDirectory_.isEmpty())
            return;
        QDir().mkpath(evidenceDirectory_);
        QTest::mouseMove(window_, QPoint(4, 4));
        QTest::qWait(80);
        EXPECT_TRUE(window_->grabWindow().save(evidenceDirectory_ + '/' + name + ".png"));
    }

    QRectF cropRect() {
        auto* image = item(QStringLiteral("viewerItem_") + viewerPanelId_);
        return image->mapRectToScene(callFunction(viewerPanel(), "cropScreenRect").toRectF());
    }

    QRectF imageRect() {
        auto* image = item(QStringLiteral("viewerItem_") + viewerPanelId_);
        return image->mapRectToScene(image->property("displayRect").toRectF());
    }

    // The authored coordinate a dragged edge must land on: the pointer's own
    // screen position converted through the panel's published mapping. This is
    // the contract the handle states, not a helper's arithmetic. `scale` is the
    // scale the GESTURE used, which a reformat drag freezes at press.
    double edgeFollowsPointer(double authoredEdge, double edgeScreenX, int pointerX, double scale) {
        const double sx = scale * controller_->pixelAspect();
        return authoredEdge + (pointerX - edgeScreenX) / sx;
    }

    QVariant overlay() { return viewerPanel() ? viewerPanel()->property("cropOverlay") : QVariant(); }

    double imageScale() { return callFunction(viewerPanel(), "imageScale").toDouble(); }

    void awaitFirstFrame() {
        ASSERT_TRUE(waitFor([this] { return controller_->presentation() != nullptr; }))
            << controller_->error().toStdString();
    }

    void awaitOverlay() {
        ASSERT_TRUE(waitFor([this] { return overlay().isValid() && !overlay().isNull(); }))
            << "the inspected, directly viewed crop must publish a handle overlay";
    }
};

// Story 44: dragging an edge handle edits the authored box in ONE history entry,
// and Undo restores exactly the authored value the drag began from. A drag on
// blank image area still pans, and the middle button and wheel keep their
// existing behaviour.
TEST_F(CropReformatSurface, Issue92CropHandlesDragTheAuthoredBox) {
    awaitFirstFrame();
    setCropBox(200.0, 200.0, 1000.0, 900.0);
    QTest::qWait(200);
    inspect(cropId_);
    awaitOverlay();

    const QRectF before = cropRect();
    ASSERT_GT(before.width(), 8.0);
    const double scale = imageScale();
    ASSERT_GT(scale, 0.0);

    const auto revisionBefore = session_->revision();
    const QPoint start(static_cast<int>(std::lround(before.right())) - 1,
                       static_cast<int>(std::lround(before.center().y())));
    const QPoint end(start.x() + 40, start.y());
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    moveTo(QPoint(start.x() + 20, start.y()));
    ASSERT_TRUE(viewerPanel()->property("cropGestureActive").toBool());
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0)
        << "a live drag previews without publishing";
    moveTo(end);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, end);
    QTest::qWait(30);

    // The box's right edge is exactly where the pointer is: one authored
    // coordinate, converted through the panel's own published mapping.
    const double expected = edgeFollowsPointer(1000.0, before.right(), end.x(), scale);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("right")), expected, 0.75);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("x")), 200.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("y")), 200.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("top")), 900.0);
    EXPECT_EQ(session_->revision(), revisionBefore + 1) << "one box drag is one history entry";
    capture(QStringLiteral("issue92-crop-drag"));

    // Exactly one authored transition, and the shared history undoes it: one
    // drag is one undo step back to the value the gesture began from.
    const nemo::EditOptions undoOptions{session_->revision(), {}};
    ASSERT_TRUE(session_->undo(undoOptions).committed);
    QTest::qWait(20);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0) << "Undo restores the authored box";

    // A press on blank image area is still the existing pan gesture.
    awaitOverlay();
    const double panBefore = viewerPanel()->property("viewPanX").toDouble();
    const QRectF box = cropRect();
    ASSERT_GT(box.width(), 8.0);
    const QPoint blank(qRound(box.x() + box.width() * 0.25), qRound(box.y() + box.height() * 0.25));
    drag(blank, QPoint(blank.x() - 30, blank.y() + 10));
    EXPECT_NE(viewerPanel()->property("viewPanX").toDouble(), panBefore) << "blank area still pans";
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);

    // The wheel zooms this panel's own view, and a middle drag is never a box
    // edit: the existing gestures are untouched by the handle layer.
    const double zoomBefore = imageScale();
    const QPoint overImage = center(item(QStringLiteral("viewerItem_") + viewerPanelId_));
    QWheelEvent wheel(QPointF(overImage), QPointF(window_->mapToGlobal(overImage)), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(window_, &wheel);
    QTest::qWait(300);
    EXPECT_GT(imageScale(), zoomBefore);
    const auto revisionAfterWheel = session_->revision();
    drag(overImage, QPoint(overImage.x() - 20, overImage.y()), Qt::MiddleButton);
    EXPECT_EQ(session_->revision(), revisionAfterWheel);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);

    EXPECT_EQ(warnings_->count(), 0);
}

// Story 44: handles belong ONLY to the crop this panel directly views while the
// same group's inspector has it open. Another node's inspection withdraws them.
TEST_F(CropReformatSurface, Issue92CropHandlesRequireTheInspectedDirectlyViewedCrop) {
    awaitFirstFrame();
    auto* layer = item(QStringLiteral("cropHandleLayer_") + viewerPanelId_);
    ASSERT_NE(layer, nullptr);
    EXPECT_FALSE(layer->isVisible()) << "an uninspected crop draws no handles";

    inspect(cropId_);
    awaitOverlay();
    EXPECT_TRUE(layer->isVisible());
    capture(QStringLiteral("issue92-crop-overlay"));

    // The group's inspection moves to the upstream plate and the crop closes:
    // the box is withdrawn even though the crop still renders.
    inspect(plateId_);
    closeInspection(cropId_);
    ASSERT_TRUE(waitFor([this] { return overlay().isNull() || !overlay().isValid(); }))
        << "handles must belong to the inspected node only";
    EXPECT_FALSE(layer->isVisible());
    EXPECT_EQ(warnings_->count(), 0);
}

// Story 44: Escape discards a live box gesture through the shared history
// owner. The authored values and the history are untouched, and the release
// that follows publishes nothing.
TEST_F(CropReformatSurface, Issue92CropEscapeCancelsTheGesture) {
    awaitFirstFrame();
    setCropBox(200.0, 200.0, 1000.0, 900.0);
    QTest::qWait(200);
    inspect(cropId_);
    awaitOverlay();

    const auto revisionBefore = session_->revision();
    const QRectF box = cropRect();
    ASSERT_GT(box.width(), 8.0);
    const QPoint start(static_cast<int>(std::lround(box.right())) - 1, static_cast<int>(std::lround(box.center().y())));
    const QPoint moved(start.x() + 60, start.y());
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    moveTo(moved);
    ASSERT_TRUE(viewerPanel()->property("cropGestureActive").toBool());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTest::qWait(20);
    EXPECT_FALSE(viewerPanel()->property("cropGestureActive").toBool());
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, moved);
    QTest::qWait(20);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);
    EXPECT_EQ(session_->revision(), revisionBefore) << "a cancelled gesture publishes no history entry";

    // The same cancellation is reachable through the shared Undo owner: a live
    // drag previews, and the shared Undo discards it instead of publishing an
    // edit — the release that follows changes neither the box nor the history.
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    moveTo(QPoint(start.x() + 70, start.y()));
    ASSERT_TRUE(viewerPanel()->property("cropGestureActive").toBool());
    const auto revisionDuringGesture = session_->revision();
    ASSERT_TRUE(history_->undo()) << "the live box gesture must be the shared history target";
    QTest::qWait(20);
    EXPECT_FALSE(viewerPanel()->property("cropGestureActive").toBool()) << "shared Undo cancels the live gesture";
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, QPoint(start.x() + 70, start.y()));
    QTest::qWait(20);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);
    EXPECT_EQ(session_->revision(), revisionDuringGesture) << "a cancelled gesture publishes no history entry";
    EXPECT_EQ(warnings_->count(), 0);
}

TEST_F(CropReformatSurface, Issue92CropInspectionChangeCancelsTheGesture) {
    awaitFirstFrame();
    setCropBox(200.0, 200.0, 1000.0, 900.0);
    QTest::qWait(200);
    inspect(cropId_);
    awaitOverlay();

    const auto revisionBefore = session_->revision();
    const QRectF box = cropRect();
    const QPoint start(qRound(box.right()) - 1, qRound(box.center().y()));
    const QPoint moved = start + QPoint(45, 0);
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    moveTo(moved);
    ASSERT_TRUE(viewerPanel()->property("cropGestureActive").toBool());

    inspect(plateId_);
    closeInspection(cropId_);
    ASSERT_TRUE(waitFor([this] { return !viewerPanel()->property("cropGestureActive").toBool(); }));
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, moved);
    QTest::qWait(30);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);
    EXPECT_EQ(session_->revision(), revisionBefore);
    EXPECT_TRUE(overlay().isNull() || !overlay().isValid());
    EXPECT_EQ(warnings_->count(), 0);
}

// Story 43: the consumed coordinates stay reachable through the shared typed,
// stepped and Alt-click keying semantics — the same cells and the same commands
// an ordinary numeric row uses.
TEST_F(CropReformatSurface, Issue92CropCoordinatesShareTypedAndKeyedSemantics) {
    awaitFirstFrame();
    setCropBox(200.0, 200.0, 1000.0, 900.0);
    QTest::qWait(200);
    inspect(cropId_);
    awaitOverlay();

    auto* field = item(QStringLiteral("cropBox_") + cropId_ + QStringLiteral("_right"));
    ASSERT_NE(field, nullptr) << "the crop box editor must own the right coordinate";
    const auto revisionBefore = session_->revision();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(field));
    QTest::qWait(20);
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    typeText(window_, QStringLiteral("1400"));
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(50);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1400.0);
    EXPECT_EQ(session_->revision(), revisionBefore + 1) << "one typed value is one history entry";

    // The size display is a presentation conversion: a width typed there authors
    // `right` from the box's own left edge, so the box keeps its x.
    auto* toggle = item(QStringLiteral("cropBoxSize_") + cropId_);
    ASSERT_NE(toggle, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(toggle));
    QTest::qWait(20);
    auto* widthField = item(QStringLiteral("cropBox_") + cropId_ + QStringLiteral("_right"));
    ASSERT_NE(widthField, nullptr);
    EXPECT_NEAR(widthField->property("value").toDouble(), 1200.0, 1e-9);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(widthField));
    QTest::qWait(20);
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    typeText(window_, QStringLiteral("800"));
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(50);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0)
        << "width 800 from x 200 authors right = 1000";
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("x")), 200.0);

    // Keying a consumed coordinate goes through the shared key command and the
    // shared key cell, at the frame the panel is on.
    auto* keyCell = visualByName(item(QStringLiteral("cropBoxEditor_") + cropId_),
                                 QStringLiteral("key_") + cropId_ + QStringLiteral("_x"));
    ASSERT_NE(keyCell, nullptr);
    ASSERT_TRUE(keyCell->isVisible());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(keyCell));
    QTest::qWait(50);
    EXPECT_EQ(controller_->nodeParameterKeyStatus(network_, cropId_, QStringLiteral("x")), QStringLiteral("key"));
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("x")), 200.0);
    capture(QStringLiteral("issue92-crop-editor"));
    EXPECT_EQ(warnings_->count(), 0);
}

TEST_F(CropReformatSurface, Issue92ExposedCropCoordinateEditsOnlyItsInstance) {
    awaitFirstFrame();
    setCropBox(200.0, 200.0, 1000.0, 900.0);
    const QString group = controller_->collapseSelection(network_, QVariantList{plateId_, cropId_}, "Crop controls");
    ASSERT_FALSE(group.isEmpty()) << controller_->error().toStdString();
    QString definition;
    for (const auto& entry : controller_->graphNodes()) {
        const auto node = entry.toMap();
        if (node.value("id").toString() == group)
            definition = node.value("definition").toString();
    }
    ASSERT_FALSE(definition.isEmpty());
    ASSERT_TRUE(controller_->promoteParameter(definition, cropId_, "x", ""));
    const auto row = [this, &group] {
        return controller_->parameterInspector(network_, group)
            .value("sections")
            .toList()
            .first()
            .toMap()
            .value("parameters")
            .toList()
            .first()
            .toMap();
    };
    const QString key = row().value("key").toString();
    inspect(group);
    auto* field = item("param_" + group + "_" + key);
    ASSERT_NE(field, nullptr);
    ASSERT_TRUE(field->isVisible()) << "an exposed coordinate must not mount the whole node's box editor";
    ASSERT_TRUE(field->isEnabled());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(field));
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    typeText(window_, "350");
    QTest::keyClick(window_, Qt::Key_Return);
    ASSERT_TRUE(waitFor([&] { return row().value("value").toDouble() == 350.0; }, 2000))
        << parametersPanel()->property("gestureError").toString().toStdString();
    const auto underlying = session_->queryValues(networkIdentity(definition), nodeIdentity(cropId_), "x");
    ASSERT_EQ(underlying.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(underlying.front().value), 200.0);
    ASSERT_TRUE(history_->undo());
    EXPECT_DOUBLE_EQ(row().value("value").toDouble(), 200.0);
    capture(QStringLiteral("issue92-crop-exposure"));
    EXPECT_EQ(warnings_->count(), 0);
}

TEST_F(CropReformatSurface, Issue92CropCenterAndCornerPreserveCoordinatesAndRedo) {
    awaitFirstFrame();
    setCropBox(200.0, 200.0, 1000.0, 900.0);
    inspect(cropId_);
    awaitOverlay();
    const auto revision = session_->revision();
    const QRectF box = cropRect();
    const double scale = imageScale();
    ASSERT_GT(scale, 0.0);
    const QPoint centerPoint = box.center().toPoint();
    drag(centerPoint, centerPoint + QPoint(30, 20));
    const double dx = 30.0 / (scale * controller_->pixelAspect());
    const double dy = -20.0 / scale;
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("x")), 200.0 + dx, 0.01);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("right")), 1000.0 + dx, 0.01);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("y")), 200.0 + dy, 0.01);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("top")), 900.0 + dy, 0.01);
    EXPECT_EQ(session_->revision(), revision + 1);
    ASSERT_TRUE(history_->undo());
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("y")), 200.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("top")), 900.0);
    ASSERT_TRUE(history_->redo());
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("y")), 200.0 + dy, 0.01);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("top")), 900.0 + dy, 0.01);
    ASSERT_TRUE(history_->undo());
    awaitOverlay();

    const QPoint corner = cropRect().topLeft().toPoint();
    drag(corner, corner + QPoint(-20, -20));
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("x")), 200.0 - 20.0 / scale, 2.0);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("top")), 900.0 + 20.0 / scale, 2.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("y")), 200.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);
    const QRectF moved = cropRect();
    EXPECT_NEAR(moved.left(), corner.x() - 20, 1.0);
    EXPECT_NEAR(moved.top(), corner.y() - 20, 1.0);
    capture(QStringLiteral("issue92-crop-center-corner"));
}

TEST_F(CropReformatSurface, Issue92CropTracksItsInputFormatAndCancelsAnUpstreamChange) {
    awaitFirstFrame();
    const QString upstream = controller_->createGraphNode(network_, QStringLiteral("reformat"),
                                                          QStringLiteral("upstream"), 0.0, 270.0, {}, {});
    ASSERT_FALSE(upstream.isEmpty());
    controller_->setNodeParameter(upstream, QStringLiteral("type"), QStringLiteral("box"));
    controller_->setNodeParameter(upstream, QStringLiteral("boxWidth"), 960);
    controller_->setNodeParameter(upstream, QStringLiteral("boxHeight"), 540);
    controller_->setNodeParameter(upstream, QStringLiteral("forceShape"), true);
    controller_->setNodeParameter(upstream, QStringLiteral("resize"), QStringLiteral("distort"));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, plateId_, 0, upstream, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, upstream, 0, cropId_, 0));
    setCropBox(100.0, 100.0, 800.0, 400.0);
    inspect(cropId_);
    ASSERT_TRUE(waitFor([this] {
        const auto frame = controller_->presentation();
        return frame && frame->request.imageHeight() == 540;
    }));
    awaitOverlay();
    auto* imageItem = item(QStringLiteral("viewerItem_") + viewerPanelId_);
    ASSERT_NE(imageItem, nullptr);
    ASSERT_TRUE(waitFor([this, imageItem] {
        const QRectF image = imageItem->mapRectToScene(imageItem->property("displayRect").toRectF());
        return image.height() > 0 && std::abs((cropRect().top() - image.top()) / image.height() - 140.0 / 540.0) < 0.01;
    })) << "bottom-left box coordinates belong to the 540-high input, not the 1080-high composition";

    const QRectF box = cropRect();
    const QPoint start(qRound(box.right()), qRound(box.center().y()));
    const QPoint moved = start + QPoint(20, 0);
    const auto revision = session_->revision();
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    moveTo(moved);
    ASSERT_TRUE(viewerPanel()->property("cropGestureActive").toBool());
    controller_->setNodeParameter(upstream, QStringLiteral("boxHeight"), 600);
    ASSERT_TRUE(waitFor([this] { return !viewerPanel()->property("cropGestureActive").toBool(); }));
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, moved);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 800.0);
    EXPECT_EQ(session_->revision(), revision + 1) << "only the upstream edit commits";
    ASSERT_TRUE(waitFor([this, imageItem] {
        const auto frame = controller_->presentation();
        const QRectF image = imageItem->mapRectToScene(imageItem->property("displayRect").toRectF());
        return frame && frame->request.imageHeight() == 600 && image.height() > 0 &&
               std::abs((cropRect().top() - image.top()) / image.height() - 1.0 / 3.0) < 0.01;
    }));
    capture(QStringLiteral("issue92-upstream-format"));
}

// Reformat translates the authored box enclosure to zero. Freeze the pointer
// mapping while the output dimensions change during a drag.
TEST_F(CropReformatSurface, Issue92ReformatCropHandlesMapOutputOffsetsBack) {
    awaitFirstFrame();
    setCropBox(201.5, 150.25, 810.5, 755.75);
    controller_->setNodeParameter(cropId_, QStringLiteral("reformat"), true);
    QTest::qWait(50);
    ASSERT_TRUE(waitFor([this] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->request.imageWidth() == 610 && presentation->request.imageHeight() == 606;
    })) << "the reformat output must be the floor/ceil enclosure of the box";
    inspect(cropId_);
    awaitOverlay();

    // The box IS the output, so the drawn box covers the presented image
    // exactly: the authored coordinates were translated by the enclosure origin
    // rather than drawn in the output's own space.
    const QRectF image = imageRect();
    const QRectF box = cropRect();
    ASSERT_GT(image.width(), 8.0);
    EXPECT_NEAR(box.x(), image.x(), 2.0);
    EXPECT_NEAR(box.y(), image.y(), 2.0);
    EXPECT_NEAR(box.width(), image.width(), 2.5);
    EXPECT_NEAR(box.height(), image.height(), 2.5);
    capture(QStringLiteral("issue92-reformat-crop"));

    // A frozen gesture: the authored delta is the pointer delta through the
    // mapping captured at press, even though the output size changes as the
    // preview runs.
    const double scale = imageScale();
    ASSERT_GT(scale, 0.0);
    const QPoint start(static_cast<int>(std::lround(box.right())) - 1, static_cast<int>(std::lround(box.center().y())));
    const QPoint end(start.x() - 60, start.y());
    drag(start, end, Qt::LeftButton, 3);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("right")),
                edgeFollowsPointer(810.5, box.right(), end.x(), scale), 1.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("x")), 201.5);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("top")), 755.75);
    EXPECT_EQ(warnings_->count(), 0);
}

// Story 43/52/56: the handles keep addressing the same authored box under zoom,
// an anamorphic canvas pixel aspect and proxy sampling, and the authored box
// survives a save/reopen.
TEST_F(CropReformatSurface, Issue92CropHandlesHoldUnderZoomParProxyAndReopen) {
    awaitFirstFrame();
    nemo::ImageFormat anamorphic;
    anamorphic.width = 1920;
    anamorphic.height = 1080;
    anamorphic.pixelAspect = 2.0F;
    const nemo::EditOptions options{session_->revision(), {}};
    ASSERT_TRUE(
        session_->submit(nemo::setNetworkFormatCommand(networkIdentity(network_), anamorphic), options).committed);
    setCropBox(0.0, 0.0, 1920.0, 1080.0);
    ASSERT_TRUE(waitFor([this] { return controller_->pixelAspect() == 2.0; }));
    inspect(cropId_);
    awaitOverlay();

    // An anamorphic canvas: the box is the whole image, so its screen rectangle
    // is the presented image, not its square-pixel equivalent.
    const QRectF image = imageRect();
    const QRectF box = cropRect();
    EXPECT_NEAR(box.width(), image.width(), 2.0);
    EXPECT_NEAR(box.height(), image.height(), 2.0);

    // Zoom in with the wheel, then drag: the delta converts through the panel's
    // own scale at this cursor-anchored zoom and this pixel aspect.
    const QPoint anchor = center(item(QStringLiteral("viewerItem_") + viewerPanelId_));
    for (int notch = 0; notch < 4; ++notch) {
        QWheelEvent wheel(QPointF(anchor), QPointF(window_->mapToGlobal(anchor)), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window_, &wheel);
    }
    QTest::qWait(400);
    const double scale = imageScale();
    // Keep the right handle visible: after zooming the full-image box extends
    // beyond the viewport. Pan the real surface before addressing that edge.
    auto* viewport = item(QStringLiteral("viewerItem_") + viewerPanelId_);
    const QRectF viewportRect = viewport->mapRectToScene(viewport->boundingRect());
    const int panDelta = qRound(viewportRect.right() - 40.0 - cropRect().right());
    drag(anchor, anchor + QPoint(panDelta, 0), Qt::MiddleButton);
    controller_->setNodeParameter(cropId_, QStringLiteral("right"), 1900.0);
    ASSERT_TRUE(waitFor([this] { return controller_->presentedRegion().x() > 0; }));
    awaitOverlay();
    const QRectF zoomed = cropRect();
    ASSERT_GT(zoomed.width(), 8.0);
    const QPoint start(static_cast<int>(std::lround(zoomed.right())) - 1,
                       static_cast<int>(std::lround(zoomed.center().y())));
    const QPoint end(start.x() - 30, start.y());
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    // A new preview now renders the whole domain rather than the viewport.
    // Its raster origin changes under the held pointer, not its coordinates.
    controller_->setForceFullFrame(true);
    moveTo(start - QPoint(10, 0));
    ASSERT_TRUE(waitFor([this] { return controller_->presentedRegion().x() == 0; }));
    moveTo(end);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, end);
    EXPECT_NEAR(authoredNumber(cropId_, QStringLiteral("right")),
                edgeFollowsPointer(1900.0, zoomed.right(), end.x(), scale), 1.0);
    capture(QStringLiteral("issue92-crop-anamorphic-zoom"));

    // Proxy sampling changes the request density only: the published box
    // rectangle stays at the same authored coordinates.
    const QRectF beforeProxy = cropRect();
    controller_->setResolutionMode(QStringLiteral("half"));
    ASSERT_TRUE(waitFor([this] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->request.samplingScale == 2;
    }));
    const QRectF afterProxy = cropRect();
    EXPECT_NEAR(afterProxy.width(), beforeProxy.width(), 2.0);
    EXPECT_NEAR(afterProxy.x(), beforeProxy.x(), 2.0);

    // Reopen: the authored box and its handles come back from the project.
    const double authoredRight = authoredNumber(cropId_, QStringLiteral("right"));
    const auto saved = session_->prepareSave(directory_.filePath(QStringLiteral("crop.nemo")).toStdString());
    const auto written = nemo::ProjectFile::writeAtomic(saved);
    ASSERT_TRUE(written.ok) << written.error.message;
    auto loaded = nemo::ProjectFile::read(saved.target);
    ASSERT_TRUE(loaded.ok) << loaded.error.message;
    ASSERT_TRUE(session_->open(std::move(loaded)).replaced);
    QTest::qWait(300);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), authoredRight);
    EXPECT_EQ(warnings_->count(), 0);
}

TEST_F(CropReformatSurface, Issue92ReformatOrientationUsesSharedBooleanEditing) {
    awaitFirstFrame();
    const QString reformat = controller_->createGraphNode(network_, QStringLiteral("reformat"),
                                                          QStringLiteral("orient"), 0.0, 270.0, {}, {});
    ASSERT_FALSE(reformat.isEmpty());
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, cropId_, 0, reformat, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, reformat, 0, viewerId_, 0));
    controller_->setNodeParameter(reformat, QStringLiteral("flip"), true);
    controller_->setNodeParameter(reformat, QStringLiteral("resize"), QStringLiteral("fit"));
    inspect(reformat);
    auto* flip = item(QStringLiteral("toggle_") + reformat + QStringLiteral("_flip"));
    ASSERT_NE(flip, nullptr);
    ASSERT_TRUE(flip->isVisible());
    EXPECT_TRUE(flip->property("checked").toBool());
    auto* resize = item(QStringLiteral("choice_") + reformat + QStringLiteral("_resize"));
    ASSERT_NE(resize, nullptr);
    EXPECT_EQ(resize->property("currentText").toString(), QStringLiteral("fit"));
    const auto authoredFlip = [&] {
        return std::get<bool>(
            session_->queryValues(networkIdentity(network_), nodeIdentity(reformat), "flip").front().value);
    };
    const auto revision = session_->revision();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(flip));
    ASSERT_TRUE(waitFor([&] { return !authoredFlip(); }, 2000));
    EXPECT_EQ(session_->revision(), revision + 1);
    ASSERT_TRUE(history_->undo());
    EXPECT_TRUE(authoredFlip());
    EXPECT_EQ(warnings_->count(), 0);
    capture(QStringLiteral("issue92-reformat-compact"));

    // Compact labels keep the shared key actions, without idle key buttons.
    auto* flipLabel = item(QStringLiteral("label_") + reformat + QStringLiteral("_flip"));
    ASSERT_NE(flipLabel, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::AltModifier, center(flipLabel));
    QTest::qWait(30);
    EXPECT_EQ(controller_->nodeParameterKeyStatus(network_, reformat, QStringLiteral("flip")), QStringLiteral("key"));
    QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier, center(flipLabel));
    QTest::qWait(50);
    capture(QStringLiteral("issue92-reformat-key-menu"));
    auto* removeKey = visualWithProperty(window_->contentItem(), "text",
                                         QStringLiteral("Remove Key at Frame %1").arg(controller_->frame()));
    ASSERT_NE(removeKey, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(removeKey));
    QTest::qWait(50);
    EXPECT_EQ(controller_->nodeParameterKeyStatus(network_, reformat, QStringLiteral("flip")), QStringLiteral("none"));
    EXPECT_EQ(warnings_->count(), 0);
}

// Story 50/51: the Reformat format editor is the ONE control for the format
// source and the explicit size, and a document preset is applied BY VALUE: the
// node copies the preset's numbers, neither the preset nor the network canvas is
// rewritten, and a later preset change cannot mutate the node.
TEST_F(CropReformatSurface, Issue92ReformatFormatEditorAppliesPresetsByValue) {
    awaitFirstFrame();
    const QString reformatId = controller_->createGraphNode(network_, QStringLiteral("reformat"),
                                                            QStringLiteral("reformat1"), 0.0, 270.0, {}, {});
    ASSERT_FALSE(reformatId.isEmpty()) << "the reformat node type must be registered";
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, cropId_, 0, reformatId, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, reformatId, 0, viewerId_, 0));
    QTest::qWait(200);
    inspect(reformatId);
    ASSERT_TRUE(waitFor([this, &reformatId] {
        return item(QStringLiteral("reformatFormatEditor_") + reformatId) != nullptr;
    })) << "the reformat format editor must be the registered control";

    const auto canvasBefore = session_->document().network(networkIdentity(network_)).format();
    const double nodeWidthBefore = authoredNumber(reformatId, QStringLiteral("width"));

    // Create a preset through the compact popup.
    auto* presetsButton = item(QStringLiteral("reformatPresetEdit_") + reformatId);
    ASSERT_NE(presetsButton, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(presetsButton));
    QTest::qWait(80);
    auto* nameField = item(QStringLiteral("reformatPresetName_") + reformatId);
    ASSERT_NE(nameField, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(nameField));
    typeText(window_, QStringLiteral("Wide2to1"));
    const auto setDraft = [this, &reformatId](const QString& objectName, const QString& text) {
        auto* draft = item(objectName + reformatId);
        ASSERT_NE(draft, nullptr);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(draft));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        typeText(window_, text);
        QTest::keyClick(window_, Qt::Key_Return);
        QTest::qWait(20);
    };
    setDraft(QStringLiteral("reformatPresetWidth_"), QStringLiteral("2048"));
    setDraft(QStringLiteral("reformatPresetHeight_"), QStringLiteral("858"));
    setDraft(QStringLiteral("reformatPresetAspect_"), QStringLiteral("2"));
    capture(QStringLiteral("issue92-reformat-preset-popup"));
    auto* saveButton = item(QStringLiteral("reformatPresetSave_") + reformatId);
    ASSERT_NE(saveButton, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(saveButton));
    QTest::qWait(80);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), nodeWidthBefore)
        << "creating a preset never edits the node";
    const auto storedPreset = [this]() {
        for (const auto& preset : session_->queryNamedFormats()) {
            if (preset.name == "Wide2to1")
                return preset.format;
        }
        return nemo::ImageFormat{};
    };
    EXPECT_EQ(storedPreset().width, 2048);
    EXPECT_EQ(storedPreset().height, 858);
    auto* closeButton = item(QStringLiteral("reformatPresetClose_") + reformatId);
    ASSERT_NE(closeButton, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(closeButton));
    QTest::qWait(50);

    // Apply it from the preset chooser: the node copies the value and states
    // the custom source in ONE authored transition.
    const auto revisionBefore = session_->revision();
    auto* presetBox = item(QStringLiteral("reformatOutput_") + reformatId);
    ASSERT_NE(presetBox, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(presetBox));
    QTest::qWait(80);
    QTest::keyClick(window_, Qt::Key_Down);
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(80);
    EXPECT_EQ(session_->revision(), revisionBefore + 1) << "applying a preset is one history entry";
    const auto sourceValue = session_->queryValues(networkIdentity(network_), nodeIdentity(reformatId), "formatSource");
    ASSERT_FALSE(sourceValue.empty());
    EXPECT_EQ(std::get<nemo::ChoiceValue>(sourceValue.front().value).value, "custom");
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 2048.0);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("height")), 858.0);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("pixelAspect")), 2.0);
    EXPECT_EQ(session_->document().network(networkIdentity(network_)).format().width, canvasBefore.width)
        << "applying a preset never changes the network canvas";
    EXPECT_EQ(session_->document().network(networkIdentity(network_)).format().pixelAspect, canvasBefore.pixelAspect);
    capture(QStringLiteral("issue92-reformat-preset"));

    // Change the preset later: it is copied by value, so the node is untouched.
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(presetsButton));
    QTest::qWait(80);
    nameField = item(QStringLiteral("reformatPresetName_") + reformatId);
    ASSERT_NE(nameField, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(nameField));
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    typeText(window_, QStringLiteral("Wide2to1"));
    setDraft(QStringLiteral("reformatPresetWidth_"), QStringLiteral("4096"));
    saveButton = item(QStringLiteral("reformatPresetSave_") + reformatId);
    ASSERT_NE(saveButton, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(saveButton));
    QTest::qWait(80);
    EXPECT_EQ(storedPreset().width, 4096);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 2048.0)
        << "a preset edit must not mutate a node that applied it";

    // Delete the preset: the node keeps the value it copied.
    auto* deleteButton = item(QStringLiteral("reformatPresetDelete_") + reformatId);
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_TRUE(deleteButton->isEnabled());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deleteButton));
    QTest::qWait(80);
    bool presetRemains = false;
    for (const auto& preset : session_->queryNamedFormats())
        presetRemains = presetRemains || preset.name == "Wide2to1";
    EXPECT_FALSE(presetRemains);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 2048.0);

    // Applying a custom draft is atomic; closing an unapplied draft is inert.
    auto* apply = item(QStringLiteral("reformatApply_") + reformatId);
    ASSERT_NE(apply, nullptr);
    const auto beforeApply = session_->revision();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(apply));
    QTest::qWait(60);
    EXPECT_EQ(session_->revision(), beforeApply + 1);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 4096.0);
    ASSERT_TRUE(history_->undo());
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 2048.0);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(presetsButton));
    QTest::qWait(40);
    setDraft(QStringLiteral("reformatPresetWidth_"), QStringLiteral("1234"));
    const auto beforeCancel = session_->revision();
    QTest::keyClick(window_, Qt::Key_Escape);
    QTest::qWait(40);
    EXPECT_EQ(session_->revision(), beforeCancel);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 2048.0);
    auto* editor = item(QStringLiteral("reformatFormatEditor_") + reformatId);
    ASSERT_NE(editor, nullptr);
    auto* formatPopup = editor->findChild<QObject*>(QStringLiteral("reformatPresetPopup_") + reformatId);
    ASSERT_NE(formatPopup, nullptr);
    ASSERT_FALSE(formatPopup->property("visible").toBool()) << "Escape closes the unapplied format draft";

    const auto chooseType = [&](int index) {
        auto* type = visualByName(editor, QStringLiteral("choice_") + reformatId + QStringLiteral("_type"));
        ASSERT_NE(type, nullptr);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(type));
        QTest::qWait(30);
        const QStringList names{QStringLiteral("to format"), QStringLiteral("to box"), QStringLiteral("scale")};
        auto* entry = visualWithProperty(window_->contentItem(), "modelData", names[index]);
        ASSERT_NE(entry, nullptr);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(entry));
        QTest::qWait(40);
    };
    const auto enterNumber = [&](const QString& key, const QString& text) {
        auto* field = item(QStringLiteral("reformat_") + reformatId + '_' + key);
        ASSERT_NE(field, nullptr);
        ASSERT_TRUE(field->isVisible());
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(field));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        typeText(window_, text);
        QTest::keyClick(window_, Qt::Key_Return);
        QTest::qWait(30);
    };
    chooseType(1);
    enterNumber(QStringLiteral("boxWidth"), QStringLiteral("640"));
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("boxWidth")), 640.0);
    capture(QStringLiteral("issue92-reformat-box"));
    chooseType(2);
    enterNumber(QStringLiteral("scaleX"), QStringLiteral("0.5"));
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("scaleX")), 0.5);
    capture(QStringLiteral("issue92-reformat-scale"));
    chooseType(0);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("width")), 2048.0);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("boxWidth")), 640.0);
    EXPECT_DOUBLE_EQ(authoredNumber(reformatId, QStringLiteral("scaleX")), 0.5);
    window_->resize(950, 700);
    QTest::qWait(100);
    auto* clamp = item(QStringLiteral("toggle_") + reformatId + QStringLiteral("_clamp"));
    ASSERT_NE(clamp, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(clamp));
    QTest::qWait(30);
    EXPECT_TRUE(std::get<bool>(
        session_->queryValues(networkIdentity(network_), nodeIdentity(reformatId), "clamp").front().value));
    capture(QStringLiteral("issue92-reformat-narrow"));
    EXPECT_EQ(warnings_->count(), 0);
}

}  // namespace
