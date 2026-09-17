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

#include "DeliveryController.hpp"
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
#include "nemo/media/ImageIO.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
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
    // The Write regression borrows the runtime's ONE delivery queue (issue #94):
    // the application composes the same pair, and the adapter owns presentation
    // state only. Constructed only by that scenario, retained until after QML
    // teardown.
    std::unique_ptr<nemo::ui::DeliveryController> delivery_;
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
        delivery_.reset();
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

    // Compact labels retain the shared Alt-click keying gesture.
    auto* keyLabel = visualByName(item(QStringLiteral("cropBoxEditor_") + cropId_),
                                  QStringLiteral("label_") + cropId_ + QStringLiteral("_x"));
    ASSERT_NE(keyLabel, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::AltModifier, center(keyLabel));
    QTest::qWait(50);
    EXPECT_EQ(controller_->nodeParameterKeyStatus(network_, cropId_, QStringLiteral("x")), QStringLiteral("key"));
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("x")), 200.0);
    auto* editor = item(QStringLiteral("cropBoxEditor_") + cropId_);
    auto* softness = visualByName(editor, QStringLiteral("param_") + cropId_ + QStringLiteral("_softness"));
    ASSERT_NE(softness, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(softness));
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    typeText(window_, QStringLiteral("125"));
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(40);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("softness")), 125.0);
    auto* slider = visualByName(editor, QStringLiteral("slider_") + cropId_ + QStringLiteral("_softness"));
    ASSERT_NE(slider, nullptr);
    const auto beforeSlider = session_->revision();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(slider));
    QTest::qWait(40);
    EXPECT_LT(authoredNumber(cropId_, QStringLiteral("softness")), 100.0);
    EXPECT_EQ(session_->revision(), beforeSlider + 1);
    ASSERT_TRUE(history_->undo());
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("softness")), 125.0);
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, center(slider));
    QTest::keyClick(window_, Qt::Key_Escape);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, center(slider));
    QTest::qWait(40);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("softness")), 125.0);

    auto* flag = item(QStringLiteral("cropFlag_") + cropId_ + QStringLiteral("_intersect"));
    ASSERT_NE(flag, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(flag));
    QTest::qWait(40);
    EXPECT_TRUE(std::get<bool>(
        session_->queryValues(networkIdentity(network_), nodeIdentity(cropId_), "intersect").front().value));
    auto* reset = item(QStringLiteral("cropReset_") + cropId_);
    ASSERT_NE(reset, nullptr);
    const auto beforeReset = session_->revision();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(reset));
    QTest::qWait(40);
    EXPECT_DOUBLE_EQ(
        visualByName(editor, QStringLiteral("cropBox_") + cropId_ + QStringLiteral("_x"))->property("value").toDouble(),
        0.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1920.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("top")), 1080.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("softness")), 125.0);
    EXPECT_EQ(session_->revision(), beforeReset + 1);
    ASSERT_TRUE(history_->undo());
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("x")), 200.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 1000.0);
    ASSERT_TRUE(controller_->setNamedFormat(QStringLiteral("SmallCrop"), 500, 300, 1.0));
    QTest::qWait(40);
    auto* preset = item(QStringLiteral("cropPreset_") + cropId_);
    ASSERT_NE(preset, nullptr);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(preset));
    QTest::qWait(30);
    QTest::keyClick(window_, Qt::Key_Down);
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(40);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 500.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("top")), 300.0);
    setCropBox(100.0, 50.0, 400.0, 250.0);
    QTest::qWait(40);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(reset));
    QTest::qWait(40);
    EXPECT_DOUBLE_EQ(
        visualByName(editor, QStringLiteral("cropBox_") + cropId_ + QStringLiteral("_x"))->property("value").toDouble(),
        0.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("right")), 500.0);
    EXPECT_DOUBLE_EQ(authoredNumber(cropId_, QStringLiteral("top")), 300.0);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(toggle));
    QTest::qWait(40);
    capture(QStringLiteral("issue92-crop-editor"));
    window_->resize(950, 700);
    QTest::qWait(100);
    capture(QStringLiteral("issue92-crop-editor-narrow"));
    for (const auto& warning : *warnings_) {
        for (const auto& error : warning.front().value<QList<QQmlError>>())
            ADD_FAILURE() << error.toString().toStdString();
    }
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

// Issue #94: a registered section can fail to load while its generic anchor
// remains visible. Exercise the real inspector and the real Deliver action
// through the runtime's ONE delivery queue — the application's own composition —
// not a standalone editor or a controller-only echo.
TEST_F(CropReformatSurface, Issue94WriteInspectorEditsAndDeliversExplicitly) {
    // The panel borrows the queue the runtime owns: one delivery worker on the
    // application's device and allocator, stopped before they are torn down.
    delivery_ = std::make_unique<nemo::ui::DeliveryController>(*session_, runtime_->deliveryQueue());
    engine_->rootContext()->setContextProperty(QStringLiteral("deliveryController"), delivery_.get());
    const QString write =
        controller_->createGraphNode(network_, QStringLiteral("write"), QStringLiteral("Write1"), 180.0, 90.0, {}, {});
    ASSERT_FALSE(write.isEmpty());
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, plateId_, 0, write, 0));
    controller_->setNodeParameter(write, QStringLiteral("precision"), QStringLiteral("float"));
    controller_->setNodeParameter(write, QStringLiteral("compression"), QStringLiteral("dwaa"));
    inspect(write);
    ASSERT_TRUE(waitFor([&] { return item(QStringLiteral("writeDeliveryEditor_") + write) != nullptr; }, 2000))
        << "Write's registered section must render, not leave an empty File row";

    // One combo's current entries. A model assigned from QML crosses back as a
    // string list, a variant list or a JS sequence depending on how the binding
    // was made, so all three forms are read the same way.
    const auto modelEntries = [](QQuickItem* box) {
        QStringList entries;
        const QVariant model = box->property("model");
        if (model.canConvert<QStringList>())
            entries = model.toStringList();
        if (entries.isEmpty() && model.canConvert<QVariantList>()) {
            const QVariantList list = model.toList();
            for (const QVariant& value : list)
                entries.push_back(value.toString());
        }
        if (entries.isEmpty() && model.canConvert<QJSValue>()) {
            const QJSValue sequence = model.value<QJSValue>();
            if (sequence.isArray()) {
                const int length = sequence.property(QStringLiteral("length")).toInt();
                for (int index = 0; index < length; ++index)
                    entries.push_back(sequence.property(index).toString());
            }
        }
        return entries;
    };
    // Selects one entry of a shared combo through its real popup, the same way
    // the native controls are driven elsewhere: the arrow beside a typeable
    // combo's value is the menu's target (its body belongs to the field), a
    // plain combo opens from anywhere, and the click lands on the popup's own
    // delegate for that entry.
    const auto chooseEntry = [&](QQuickItem* box, const QString& entry) {
        ASSERT_NE(box, nullptr);
        const bool typeable = box->property("editable").toBool();
        const QPointF local = typeable ? QPointF(box->width() - 12.0, box->height() / 2.0)
                                       : QPointF(box->width() / 2.0, box->height() / 2.0);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, box->mapToScene(local).toPoint());
        QTest::qWait(80);
        const int index = modelEntries(box).indexOf(entry);
        ASSERT_GE(index, 0) << entry.toStdString();
        auto* popup = box->property("popup").value<QObject*>();
        ASSERT_NE(popup, nullptr);
        auto* list = popup->property("contentItem").value<QQuickItem*>();
        ASSERT_NE(list, nullptr) << "the combo's menu must show its entries";
        QQuickItem* delegate = nullptr;
        ASSERT_TRUE(
            QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate), Q_ARG(int, index)));
        ASSERT_NE(delegate, nullptr) << "the menu shows entry " << index;
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          delegate->mapToScene(QPointF(delegate->width() / 2, delegate->height() / 2)).toPoint());
        QTest::qWait(80);
    };
    const auto enter = [&](QQuickItem* field, const QString& text) {
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(field));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        typeText(window_, text);
        QTest::keyClick(window_, Qt::Key_Return);
        QTest::qWait(30);
    };
    const auto clearField = [&](QQuickItem* field) {
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(field));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window_, Qt::Key_Delete);
        QTest::keyClick(window_, Qt::Key_Return);
        QTest::qWait(30);
    };
    const auto authoredText = [&](const QString& key) {
        return QString::fromStdString(std::get<std::string>(
            session_->queryValues(networkIdentity(network_), nodeIdentity(write), key.toStdString()).front().value));
    };
    const auto authoredFlag = [&](const QString& key) {
        return std::get<bool>(
            session_->queryValues(networkIdentity(network_), nodeIdentity(write), key.toStdString()).front().value);
    };

    // Every authored setting renders exactly once: EXR's own controls, the
    // always-present output-color rows and the file flags.
    for (const QString& name :
         {QStringLiteral("writeFile_") + write, QStringLiteral("writeNumber_") + write + "_frameFirst",
          QStringLiteral("writeNumber_") + write + "_frameLast",
          QStringLiteral("writeNumber_") + write + "_frameOffset", QStringLiteral("writeChoice_") + write + "_fileType",
          QStringLiteral("writeChoice_") + write + "_precision",
          QStringLiteral("writeChoice_") + write + "_compression",
          QStringLiteral("writeChoice_") + write + "_colorMode", QStringLiteral("writeTransform_") + write,
          QStringLiteral("writeLut_") + write, QStringLiteral("writeFlag_") + write + "_createDirectories",
          QStringLiteral("writeFlag_") + write + "_overwrite", QStringLiteral("writeDeliver_") + write}) {
        auto* control = item(name);
        ASSERT_NE(control, nullptr) << name.toStdString();
        EXPECT_TRUE(control->isVisible()) << name.toStdString();
        EXPECT_GT(control->width(), 0) << name.toStdString();
        EXPECT_GT(control->height(), 0) << name.toStdString();
    }
    auto* precision = item(QStringLiteral("writeChoice_") + write + "_precision");
    auto* compression = item(QStringLiteral("writeChoice_") + write + "_compression");
    auto* fileType = item(QStringLiteral("writeChoice_") + write + "_fileType");
    auto* colorMode = item(QStringLiteral("writeChoice_") + write + "_colorMode");
    auto* transform = item(QStringLiteral("writeTransform_") + write);
    auto* profile = item(QStringLiteral("writeChoice_") + write + "_profile");
    auto* frameRate = item(QStringLiteral("writeNumber_") + write + "_frameRate");
    auto* bitrate = item(QStringLiteral("writeNumber_") + write + "_bitrateKbps");
    ASSERT_NE(transform, nullptr);
    ASSERT_NE(profile, nullptr);
    ASSERT_NE(frameRate, nullptr);
    ASSERT_NE(bitrate, nullptr);
    // A movie setting is not shown while a still format is authored: the format
    // row states its own controls and never a stale value from another format.
    EXPECT_FALSE(profile->isVisible());
    EXPECT_FALSE(frameRate->isVisible());
    EXPECT_FALSE(bitrate->isVisible());
    EXPECT_EQ(precision->property("currentText").toString(), QStringLiteral("float"));
    EXPECT_EQ(compression->property("currentText").toString(), QStringLiteral("dwaa"));
    EXPECT_EQ(fileType->property("currentText").toString(), QStringLiteral("exr"));
    EXPECT_EQ(colorMode->property("currentText").toString(), QStringLiteral("raw"));
    EXPECT_TRUE(modelEntries(transform).isEmpty()) << "raw names no explicit transform to enumerate";
    ASSERT_TRUE(session_->undo(nemo::EditOptions{session_->revision(), {}}).committed);
    ASSERT_TRUE(waitFor([&] { return compression->property("currentText").toString() == "zip"; }, 2000));
    ASSERT_TRUE(session_->redo(nemo::EditOptions{session_->revision(), {}}).committed);
    ASSERT_TRUE(waitFor([&] { return compression->property("currentText").toString() == "dwaa"; }, 2000));

    // The transform entries are the ACTIVE project config's own (discovered once
    // per generation/config/mode by the delivery adapter), and a mode that
    // resolves no explicit transform enumerates nothing at all.
    const QVariantMap colorspaces = delivery_->transformChoices(QStringLiteral("colorspace"));
    EXPECT_TRUE(colorspaces.value(QStringLiteral("error")).toString().isEmpty())
        << colorspaces.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_FALSE(colorspaces.value(QStringLiteral("choices")).toStringList().isEmpty())
        << "the project config must enumerate its own colorspaces";
    EXPECT_TRUE(
        delivery_->transformChoices(QStringLiteral("raw")).value(QStringLiteral("choices")).toStringList().isEmpty());

    // The authored color mode really drives that discovery through the control.
    chooseEntry(colorMode, QStringLiteral("display"));
    ASSERT_TRUE(waitFor([&] { return !modelEntries(transform).isEmpty(); }, 2000))
        << "the display mode must state the config's display/view entries";
    const QStringList views = modelEntries(transform);
    EXPECT_EQ(views,
              delivery_->transformChoices(QStringLiteral("display")).value(QStringLiteral("choices")).toStringList());
    chooseEntry(transform, views.first());
    ASSERT_TRUE(waitFor([&] { return authoredText(QStringLiteral("outputTransform")) == views.first(); }, 2000))
        << "the authored transform must be the entry the artist selected";

    // The optional LUT is a path the artist states: authored verbatim, with no
    // extension or existence rewrite by the panel.
    auto* lut = item(QStringLiteral("writeLut_") + write);
    ASSERT_NE(lut, nullptr);
    enter(lut, QStringLiteral("looks/filmic.cube"));
    EXPECT_EQ(authoredText(QStringLiteral("lutFile")), QStringLiteral("looks/filmic.cube"));
    clearField(lut);
    EXPECT_TRUE(authoredText(QStringLiteral("lutFile")).isEmpty());

    const QString pattern = directory_.filePath(QStringLiteral("delivery/shot.####.exr"));
    enter(item(QStringLiteral("writeFile_") + write), pattern);
    EXPECT_EQ(authoredText(QStringLiteral("file")), pattern);
    auto* frameLastField = item(QStringLiteral("writeNumber_") + write + "_frameLast");
    enter(frameLastField, QStringLiteral("3"));
    EXPECT_DOUBLE_EQ(authoredNumber(write, QStringLiteral("frameLast")), 3);
    capture(QStringLiteral("issue94-write-ready"));
    controller_->setFrame(2);
    QTest::qWait(100);
    EXPECT_FALSE(QDir(directory_.filePath(QStringLiteral("delivery"))).exists())
        << "opening/editing the Write inspector and changing time must not write";

    auto* deliver = item(QStringLiteral("writeDeliver_") + write);
    ASSERT_TRUE(deliver->isEnabled());
    const auto revision = session_->revision();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    ASSERT_TRUE(waitFor([&] { return !delivery_->jobs().isEmpty() && !delivery_->busy(); }, 60000));
    const auto job = delivery_->jobs().front().toMap();
    ASSERT_EQ(job.value(QStringLiteral("state")).toString(), QStringLiteral("completed"))
        << job.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(job.value(QStringLiteral("writtenFrames")).toInt(), 3);
    EXPECT_TRUE(job.value(QStringLiteral("fullQuality")).toBool())
        << "a delivery is the full-quality reference image, never a viewer-cache frame";
    EXPECT_EQ(job.value(QStringLiteral("colorMode")).toString(), QStringLiteral("display"));
    EXPECT_EQ(session_->revision(), revision) << "delivery is outside document history";
    const QString firstFile = directory_.filePath(QStringLiteral("delivery/shot.0001.exr"));
    for (int frame = 1; frame <= 3; ++frame) {
        const auto path = directory_.filePath(QStringLiteral("delivery/shot.%1.exr").arg(frame, 4, 10, QChar('0')));
        const auto readback = nemo::media::readImage(path.toStdString());
        EXPECT_EQ(readback.header.nativePrecision, "float");
        EXPECT_EQ(readback.header.channelNames, (std::vector<std::string>{"R", "G", "B", "A"}));
        EXPECT_EQ(readback.image.width(), session_->snapshot().network(networkIdentity(network_)).format().width);
    }
    capture(QStringLiteral("issue94-write-completed"));
    QFile first(firstFile);
    ASSERT_TRUE(first.open(QIODevice::ReadOnly));
    const QByteArray original = first.readAll();
    first.close();

    // A destination collision is the SEAM's refusal, resolved by its worker
    // before any write: the request is accepted first, and the refusal then
    // arrives as that job's own failure naming the file it would have replaced.
    // The panel performs no preflight of its own and surfaces none.
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    ASSERT_TRUE(waitFor([&] { return delivery_->jobs().size() == 2; }, 5000))
        << "a colliding request is accepted and refused by the job, not synchronously";
    ASSERT_TRUE(waitFor([&] { return !delivery_->busy(); }, 60000));
    const auto refused = delivery_->jobs().front().toMap();
    EXPECT_EQ(refused.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
    EXPECT_EQ(refused.value(QStringLiteral("writtenFrames")).toInt(), 0);
    auto* failure = item(QStringLiteral("writeFailure_") + write);
    auto* problem = item(QStringLiteral("writeProblem_") + write);
    ASSERT_NE(failure, nullptr);
    ASSERT_NE(problem, nullptr);
    ASSERT_TRUE(waitFor([&] { return failure->isVisible(); }, 2000));
    EXPECT_TRUE(failure->property("text").toString().contains(firstFile))
        << failure->property("text").toString().toStdString();
    EXPECT_TRUE(delivery_->error().isEmpty()) << "a job's own refusal is not a submission failure";
    EXPECT_FALSE(problem->isVisible()) << "the panel raises no refusal of its own";
    ASSERT_TRUE(first.open(QIODevice::ReadOnly));
    EXPECT_EQ(first.readAll(), original);
    first.close();
    window_->resize(950, 700);
    QTest::qWait(100);
    capture(QStringLiteral("issue94-write-narrow-refusal"));
    // Back to the reference size before the next real gestures: the narrow
    // evidence states the compact rows, and the gestures below then land where
    // the panel really shows them.
    window_->resize(1568, 926);
    QTest::qWait(100);

    // Explicit authorization must recover from the refusal through the same
    // public submit path, rather than leaving Overwrite permanently stuck.
    auto* overwrite = item(QStringLiteral("writeFlag_") + write + "_overwrite");
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(overwrite));
    ASSERT_TRUE(authoredFlag(QStringLiteral("overwrite")));
    controller_->setNodeParameter(write, QStringLiteral("precision"), QStringLiteral("half"));
    QTest::qWait(30);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    ASSERT_TRUE(waitFor([&] { return delivery_->jobs().size() == 3; }, 5000))
        << failure->property("text").toString().toStdString();
    ASSERT_TRUE(waitFor([&] { return !delivery_->busy(); }, 60000));
    EXPECT_EQ(delivery_->jobs().front().toMap().value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
    EXPECT_EQ(nemo::media::readImage(firstFile.toStdString()).header.nativePrecision, "half");
    EXPECT_FALSE(failure->isVisible());

    // --- the authored format states its own settings -------------------------
    // MOV replaces the EXR controls with its ProRes profile and the movie frame
    // rate; the profile is a choice, the frame rate a typed number.
    chooseEntry(fileType, QStringLiteral("mov"));
    ASSERT_TRUE(waitFor([&] { return profile->isVisible() && frameRate->isVisible(); }, 2000));
    EXPECT_FALSE(precision->isVisible());
    EXPECT_FALSE(compression->isVisible());
    EXPECT_FALSE(bitrate->isVisible());
    EXPECT_EQ(profile->property("currentText").toString(), QStringLiteral("422"));
    chooseEntry(profile, QStringLiteral("4444xq"));
    // The new choice is an ordinary authored edit: it undoes and redoes through
    // the shared host, and the control states the authored value either way.
    ASSERT_TRUE(session_->undo(nemo::EditOptions{session_->revision(), {}}).committed);
    ASSERT_TRUE(waitFor([&] { return profile->property("currentText").toString() == "422"; }, 2000));
    ASSERT_TRUE(session_->redo(nemo::EditOptions{session_->revision(), {}}).committed);
    ASSERT_TRUE(waitFor([&] { return profile->property("currentText").toString() == "4444xq"; }, 2000));
    enter(frameRate, QStringLiteral("25"));
    EXPECT_EQ(std::get<nemo::ChoiceValue>(
                  session_->queryValues(networkIdentity(network_), nodeIdentity(write), "profile").front().value)
                  .value,
              "4444xq");
    EXPECT_DOUBLE_EQ(authoredNumber(write, QStringLiteral("frameRate")), 25.0);
    const QString moviePath = directory_.filePath(QStringLiteral("delivery/shot.mov"));
    enter(item(QStringLiteral("writeFile_") + write), moviePath);
    EXPECT_EQ(authoredText(QStringLiteral("file")), moviePath) << "the authored path is never rewritten";
    enter(frameLastField, QStringLiteral("2"));
    capture(QStringLiteral("issue94-write-movie"));
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    ASSERT_TRUE(waitFor([&] { return delivery_->jobs().size() == 4; }, 5000));
    ASSERT_TRUE(waitFor([&] { return !delivery_->busy(); }, 120000));
    const auto movie = delivery_->jobs().front().toMap();
    ASSERT_EQ(movie.value(QStringLiteral("state")).toString(), QStringLiteral("completed"))
        << movie.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(movie.value(QStringLiteral("fileType")).toString(), QStringLiteral("mov"));
    EXPECT_EQ(movie.value(QStringLiteral("profile")).toString(), QStringLiteral("4444xq"));
    EXPECT_EQ(movie.value(QStringLiteral("writtenFrames")).toInt(), 2);
    EXPECT_TRUE(QFileInfo::exists(moviePath)) << "one movie file carries the whole range";
    EXPECT_GT(QFileInfo(moviePath).size(), 0);
    capture(QStringLiteral("issue94-write-movie-delivered"));

    // MP4 states its bitrate and the same frame rate.
    chooseEntry(fileType, QStringLiteral("mp4"));
    ASSERT_TRUE(waitFor([&] { return bitrate->isVisible() && frameRate->isVisible() && !profile->isVisible(); }, 2000));
    enter(bitrate, QStringLiteral("8000"));
    EXPECT_DOUBLE_EQ(authoredNumber(write, QStringLiteral("bitrateKbps")), 8000.0);
    const QString mp4Path = directory_.filePath(QStringLiteral("delivery/shot.mp4"));
    enter(item(QStringLiteral("writeFile_") + write), mp4Path);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    ASSERT_TRUE(waitFor([&] { return delivery_->jobs().size() == 5; }, 5000));
    ASSERT_TRUE(waitFor([&] { return !delivery_->busy(); }, 120000));
    const auto clip = delivery_->jobs().front().toMap();
    ASSERT_EQ(clip.value(QStringLiteral("state")).toString(), QStringLiteral("completed"))
        << clip.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(clip.value(QStringLiteral("fileType")).toString(), QStringLiteral("mp4"));
    EXPECT_EQ(clip.value(QStringLiteral("writtenFrames")).toInt(), 2);
    EXPECT_GT(QFileInfo(mp4Path).size(), 0);

    // A cancelled movie publishes NO partial output: its container is finalized
    // only after the whole range encoded, so nothing appears at its destination.
    chooseEntry(fileType, QStringLiteral("mov"));
    ASSERT_TRUE(waitFor([&] { return profile->isVisible() && frameRate->isVisible(); }, 2000));
    const QString cancelledMovie = directory_.filePath(QStringLiteral("delivery/stopped.mov"));
    enter(item(QStringLiteral("writeFile_") + write), cancelledMovie);
    enter(frameLastField, QStringLiteral("400"));
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    auto* cancel = item(QStringLiteral("writeCancel_") + write);
    ASSERT_TRUE(waitFor([&] { return cancel->isVisible(); }, 2000));
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(cancel));
    ASSERT_TRUE(waitFor([&] { return !delivery_->busy(); }, 60000));
    EXPECT_EQ(delivery_->jobs().front().toMap().value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
    EXPECT_FALSE(QFileInfo::exists(cancelledMovie)) << "a cancelled movie leaves no file at its destination";

    // A cancelled still sequence reports exactly the frames it finalized: each
    // published EXR is a real file and no other frame is claimed.
    chooseEntry(fileType, QStringLiteral("exr"));
    ASSERT_TRUE(waitFor([&] { return precision->isVisible() && !bitrate->isVisible(); }, 2000));
    enter(item(QStringLiteral("writeFile_") + write), directory_.filePath(QStringLiteral("delivery/cancel.####.exr")));
    enter(frameLastField, QStringLiteral("400"));
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(deliver));
    ASSERT_TRUE(waitFor([&] { return cancel->isVisible(); }, 2000));
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(cancel));
    ASSERT_TRUE(waitFor([&] { return !delivery_->busy(); }, 60000));
    const auto cancelled = delivery_->jobs().front().toMap();
    EXPECT_EQ(cancelled.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
    EXPECT_LT(cancelled.value(QStringLiteral("writtenFrames")).toInt(), 400);
    EXPECT_EQ(cancelled.value(QStringLiteral("failedFrames")).toInt(), 0);
    capture(QStringLiteral("issue94-write-cancelled"));
    EXPECT_EQ(warnings_->count(), 0);
}

}  // namespace
