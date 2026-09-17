#include <gtest/gtest.h>

#include "HistoryController.hpp"
#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ProjectFileController.hpp"
#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/commands/RotoCommands.hpp"
#include "nemo/core/document/Roto.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/gpu/Error.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <filesystem>
#include <functional>
#include <memory>

namespace {

QQuickItem* namedItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* item = namedItem(child, name))
            return item;
    return nullptr;
}

class RotoSurface : public testing::Test {
protected:
    QTemporaryDir directory;
    std::unique_ptr<nemo::test::ScopedEnvironment> colorEnvironment;
    std::unique_ptr<nemo::ui::ViewerRuntime> runtime;
    std::unique_ptr<nemo::ProjectSession> session;
    std::unique_ptr<nemo::ui::HistoryController> history;
    std::unique_ptr<nemo::ui::PanelContextRouter> router;
    std::unique_ptr<nemo::ui::ViewerController> facade;
    std::unique_ptr<nemo::ui::ViewerControllerRegistry> registry;
    std::unique_ptr<nemo::workspace::WorkspaceController> workspace;
    std::unique_ptr<nemo::ui::NativeFileChooser> chooser;
    std::unique_ptr<nemo::ui::ProjectFileController> projectFile;
    std::unique_ptr<nemo::ui::ParameterEditorRegistry> editors;
    std::unique_ptr<QQmlApplicationEngine> engine;
    std::unique_ptr<QSignalSpy> warnings;
    QQuickWindow* window{};
    nemo::ui::ViewerController* controller{};
    QString network;
    QString roto;

    bool waitFor(const std::function<bool()>& predicate) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 30000) {
            if (predicate())
                return true;
            QTest::qWait(10);
        }
        return predicate();
    }

    void SetUp() override {
        if (qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") != 1 ||
            qEnvironmentVariableIntValue("NEMO_TEST_VIEWER_WINDOW") != 1)
            GTEST_SKIP() << "Roto requires native window input";
#ifndef NEMO_SLANG_SPV_DIR
        GTEST_SKIP() << "Roto requires native shaders";
#else
        const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                            "docs/evidence/issue12-view.ocio";
        colorEnvironment = std::make_unique<nemo::test::ScopedEnvironment>("OCIO", config.string());
        runtime = std::make_unique<nemo::ui::ViewerRuntime>();
        nemo::eval::ViewerCacheOptions options;
        options.directory = directory.filePath("cache").toStdString();
        options.encoding.codec = "libx264-cpu";
        options.chunkFrames = 1;
        std::vector<std::string> extensions{"VK_KHR_surface"};
        extensions.push_back(QGuiApplication::platformName() == "wayland" ? "VK_KHR_wayland_surface"
                                                                          : "VK_KHR_xcb_surface");
        try {
            runtime->bootstrap(extensions, NEMO_SLANG_SPV_DIR, options);
        } catch (const nemo::gpu::GpuException& error) {
            if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
                GTEST_SKIP() << error.what();
            throw;
        }
        session = std::make_unique<nemo::ProjectSession>();
        history = std::make_unique<nemo::ui::HistoryController>(*session);
        router = std::make_unique<nemo::ui::PanelContextRouter>(*session);
        facade = std::make_unique<nemo::ui::ViewerController>(runtime.get(), *session);
        registry = std::make_unique<nemo::ui::ViewerControllerRegistry>(runtime.get(), *session);
        const QString path = directory.filePath("workspace.json");
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(
            R"({"version":2,"activeWorkspaceId":"roto","workspaces":[{"id":"roto","name":"Roto authoring","layout":{"version":1,"root":{"id":"root","kind":"split","orientation":"horizontal","ratio":0.65,"children":[{"id":"v","kind":"tabs","active":"roto-view","panels":[{"id":"roto-view","type":"viewer","group":"A","state":{"viewerIndex":0}}]},{"id":"p","kind":"tabs","active":"roto-params","panels":[{"id":"roto-params","type":"parameters","group":"A","state":{"inspectors":[]}}]}]}}}]})");
        file.close();
        workspace = std::make_unique<nemo::workspace::WorkspaceController>(path);
        ASSERT_TRUE(workspace->error().isEmpty()) << workspace->error().toStdString();
        workspace->registerPanelType("viewer", "Viewer", "ViewerPanel.qml");
        workspace->registerPanelType("parameters", "Parameters", "ParametersPanel.qml");
        router->setWorkspaceController(workspace.get());
        controller = qobject_cast<nemo::ui::ViewerController*>(registry->controller("roto-view"));
        ASSERT_NE(controller, nullptr);
        network = controller->rootNetworkId();
        const auto canvas = session->submit(nemo::setNetworkFormatCommand(network.toULongLong(), {640, 360, 1.5F}),
                                            {session->revision(), {}});
        ASSERT_TRUE(canvas.committed);
        roto = controller->createGraphNode(network, "roto", "Roto", 0, 0, {}, {});
        ASSERT_FALSE(roto.isEmpty());
        const auto viewer = controller->createGraphNode(network, "viewer", "Viewer", 0, 90, {}, {});
        ASSERT_TRUE(controller->connectOrReplaceGraph(network, roto, 0, viewer, 0));
        controller->setActiveViewer(network, 0);
        chooser = std::make_unique<nemo::ui::NativeFileChooser>();
        projectFile = std::make_unique<nemo::ui::ProjectFileController>(*session, *workspace, *router, *chooser);
        editors = std::make_unique<nemo::ui::ParameterEditorRegistry>();
        for (const auto& contribution : nemo::builtinNodeContributions()->entries()) {
            for (const auto& editor : contribution.editors) {
                QStringList consumes;
                for (const auto& key : editor.consumes)
                    consumes.push_back(QString::fromStdString(key));
                ASSERT_TRUE(editors->registerEditor(QString::fromStdString(editor.id),
                                                    QUrl(QString::fromStdString(editor.source)), consumes,
                                                    QString::fromStdString(editor.presentation)));
            }
        }
        engine = std::make_unique<QQmlApplicationEngine>();
        warnings = std::make_unique<QSignalSpy>(engine.get(), &QQmlEngine::warnings);
        auto* context = engine->rootContext();
        context->setContextProperty("workspace", workspace.get());
        context->setContextProperty("historyController", history.get());
        context->setContextProperty("panelContextRouter", router.get());
        context->setContextProperty("projectFile", projectFile.get());
        context->setContextProperty("viewerController", facade.get());
        context->setContextProperty("viewerControllers", registry.get());
        context->setContextProperty("parameterEditors", editors.get());
        engine->load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine->rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow*>(engine->rootObjects().first());
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(runtime->attachToWindow(window).isEmpty());
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
        ASSERT_TRUE(waitFor([this] { return controller->presentation() != nullptr; }))
            << controller->error().toStdString();
        auto* panel = item("parametersPanel");
        ASSERT_NE(panel, nullptr);
        QVariant result;
        ASSERT_TRUE(QMetaObject::invokeMethod(panel, "openInspector", Qt::DirectConnection,
                                              Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, network),
                                              Q_ARG(QVariant, roto)));
        ASSERT_TRUE(waitFor([this] {
            auto* overlay = item("rotoOverlay_roto-view");
            return overlay && overlay->isVisible();
        }));
#endif
    }

    void TearDown() override {
        if (warnings) {
            for (const auto& emission : *warnings)
                for (const auto& warning : qvariant_cast<QList<QQmlError>>(emission[0]))
                    ADD_FAILURE() << warning.toString().toStdString();
        }
        if (runtime)
            runtime->stopWorker();
        warnings.reset();
        engine.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (runtime)
            runtime->quiesceForTeardown();
        editors.reset();
        projectFile.reset();
        chooser.reset();
        registry.reset();
        facade.reset();
        workspace.reset();
        router.reset();
        history.reset();
        session.reset();
        runtime.reset();
    }

    QQuickItem* item(const QString& name) { return namedItem(window->contentItem(), name); }
    QPoint center(QQuickItem* target) {
        return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
    }
    QRectF imageRect() {
        auto* image = item("viewerItem_roto-view");
        return image->mapRectToScene(image->property("displayRect").toRectF());
    }
    const nemo::RotoData* data() {
        const auto* node = session->document().network(network.toULongLong()).graph().node(roto.toULongLong());
        return node ? node->roto.get() : nullptr;
    }
    QObject* authoring() { return item("rotoOverlay_roto-view")->property("roto").value<QObject*>(); }
    void tool(const QString& name) {
        auto* button = item("rotoTool_" + name + "_roto-view");
        ASSERT_NE(button, nullptr);
        ASSERT_TRUE(button->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(button));
        QTest::qWait(20);
    }
    void dragBetween(const QPoint& start, const QPoint& end) {
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        move(end);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
        QTest::qWait(30);
    }
    void rectangle() {
        tool("rectangle");
        const auto image = imageRect();
        dragBetween((image.topLeft() + QPointF(image.width() * .25, image.height() * .25)).toPoint(),
                    (image.topLeft() + QPointF(image.width() * .65, image.height() * .65)).toPoint());
        ASSERT_TRUE(waitFor([this] { return data() && !data()->elements.empty(); }));
    }
    void submit(nemo::Command command) {
        const auto result = session->submit(std::move(command), {session->revision(), {}});
        ASSERT_TRUE(result.committed) << (result.error ? result.error->message : "not committed");
        QTest::qWait(30);
    }
    void move(const QPoint& position, Qt::MouseButton button = Qt::LeftButton) {
        QMouseEvent event(QEvent::MouseMove, QPointF(position), QPointF(window->mapToGlobal(position)), Qt::NoButton,
                          button, Qt::NoModifier);
        QGuiApplication::sendEvent(window, &event);
        QTest::qWait(20);
    }
    void capture(const QString& name) {
        const QString path = qEnvironmentVariable("NEMO93_EVIDENCE_DIR");
        if (path.isEmpty())
            return;
        QDir().mkpath(path);
        QTest::qWait(150);
        ASSERT_TRUE(window->grabWindow().save(path + '/' + name + ".png"));
    }
};

TEST_F(RotoSurface, RectangleFollowsAspectCorrectImageAndUndoesInOneStep) {
    tool("rectangle");
    const QRectF image = imageRect();
    const QPoint start = (image.topLeft() + QPointF(image.width() * .25, image.height() * .25)).toPoint();
    const QPoint end = (image.topLeft() + QPointF(image.width() * .65, image.height() * .65)).toPoint();
    const auto before = session->revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    move(end);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    capture("rectangle-release");
    ASSERT_TRUE(waitFor([this] { return data() && data()->elements.size() == 1; }))
        << authoring()->property("error").toString().toStdString();
    EXPECT_EQ(session->revision(), before + 1);
    const auto& shape = data()->elements.front();
    ASSERT_EQ(shape.points.size(), 4u);
    EXPECT_NEAR(shape.points.front().position.value[0], (start.x() - image.x()) * 640.0 / image.width(), 1.5);
    EXPECT_NEAR(shape.points.front().position.value[1], (start.y() - image.y()) * 360.0 / image.height(), 1.5);
    capture("rectangle-aspect");
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(!data() || data()->elements.empty());
    ASSERT_TRUE(history->redo());
    ASSERT_TRUE(data());
    EXPECT_EQ(data()->elements.size(), 1u);
    capture("rectangle-redo");
}

TEST_F(RotoSurface, EscapeDiscardsDraftAndMiddlePanDoesNotAuthorShapes) {
    tool("bezier");
    const QRectF image = imageRect();
    const QPoint start = (image.topLeft() + QPointF(image.width() * .3, image.height() * .3)).toPoint();
    const auto before = session->revision();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(70, 0));
    ASSERT_TRUE(authoring()->property("draftActive").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(40);
    EXPECT_FALSE(authoring()->property("draftActive").toBool());
    EXPECT_TRUE(!data() || data()->elements.empty());
    EXPECT_EQ(session->revision(), before);
    QWheelEvent wheel(QPointF(start), QPointF(window->mapToGlobal(start)), QPoint(), QPoint(0, 1440), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(window, &wheel);
    QTest::qWait(350);
    const auto previousPan = controller->property("pan").toPointF();
    const auto region = controller->property("presentedRegion").toRect();
    const auto magnified = imageRect();
    QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, start);
    move(start + QPoint(25, 15), Qt::MiddleButton);
    QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, start + QPoint(25, 15));
    EXPECT_EQ(session->revision(), before);
    const auto pan = controller->property("pan").toPointF();
    EXPECT_NEAR(pan.x() - previousPan.x(), -25.0 * region.width() / magnified.width(), 1.0);
    EXPECT_NEAR(pan.y() - previousPan.y(), -15.0 * region.height() / magnified.height(), 1.0);
    capture("cancelled-draft");
}

TEST_F(RotoSurface, CurveToolsPublishClosedEditableContours) {
    for (const QString name : {"bezier", "bspline", "ellipse"}) {
        SCOPED_TRACE(name.toStdString());
        tool(name);
        const auto image = imageRect();
        const auto first = (image.topLeft() + QPointF(image.width() * .3, image.height() * .3)).toPoint();
        const auto last = (image.topLeft() + QPointF(image.width() * .65, image.height() * .7)).toPoint();
        if (name == "ellipse") {
            dragBetween(first, last);
        } else {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, first);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(last.x(), first.y()));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, last);
            QTest::keyClick(window, Qt::Key_Return);
        }
        ASSERT_TRUE(waitFor([this] { return data() && data()->elements.size() == 1; }))
            << authoring()->property("error").toString().toStdString();
        EXPECT_EQ(data()->elements[0].kind, name == "bspline" ? nemo::RotoKind::BSpline : nemo::RotoKind::Bezier);
        EXPECT_GE(data()->elements[0].points.size(), 3u);
        capture(name);
        ASSERT_TRUE(history->undo());
        ASSERT_TRUE(!data() || data()->elements.empty());
    }
}

TEST_F(RotoSurface, PointDragCommitsOnceAndUndoCancelsAnOpenDraftFirst) {
    rectangle();
    ASSERT_TRUE(data());
    tool("select");
    const auto image = imageRect();
    const auto old = data()->elements[0].points[0].position;
    const QPoint point =
        (image.topLeft() + QPointF(old.value[0] * image.width() / 640.0, old.value[1] * image.height() / 360.0))
            .toPoint();
    const auto before = session->revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point);
    move(point + QPoint(30, 20));
    EXPECT_EQ(session->revision(), before);
    capture("point-preview");
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point + QPoint(30, 20));
    ASSERT_TRUE(waitFor([&] { return session->revision() == before + 1; }))
        << authoring()->property("error").toString().toStdString();
    EXPECT_NEAR(data()->elements[0].points[0].position.value[0], old.value[0] + 30 * 640.0 / image.width(), 1.0);
    EXPECT_NEAR(data()->elements[0].points[0].position.value[1], old.value[1] + 20 * 360.0 / image.height(), 1.0);
    const auto moved = data()->elements[0].points[0].position;
    tool("bezier");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    ASSERT_TRUE(authoring()->property("draftActive").toBool());
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(authoring()->property("draftActive").toBool());
    EXPECT_EQ(session->revision(), before + 1);
    EXPECT_EQ(data()->elements[0].points[0].position, moved);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(data()->elements[0].points[0].position, old);
    capture("point-undo");
}

TEST_F(RotoSurface, InspectorReflectsPublishedValuesUndoAndKeyedFrames) {
    rectangle();
    ASSERT_TRUE(data());
    const auto element = data()->elements[0].id;
    const nemo::ParameterAddress address{network.toULongLong(), roto.toULongLong(), "opacity", 0, element, 0};
    submit(nemo::setParametersCommand({{address, nemo::ParameterValue{0.8}}}));
    const auto fieldName = "rotoField_" + roto + "_opacity_" + QString::number(element) + "_";
    auto* field = item(fieldName);
    ASSERT_NE(field, nullptr);
    EXPECT_DOUBLE_EQ(field->property("value").toDouble(), 0.8);
    submit(nemo::setParametersCommand({{address, nemo::ParameterValue{0.35}}}));
    EXPECT_DOUBLE_EQ(field->property("value").toDouble(), 0.35);
    ASSERT_TRUE(history->undo());
    QTest::qWait(30);
    EXPECT_DOUBLE_EQ(field->property("value").toDouble(), 0.8);
    submit(nemo::setKeyframesCommand(
        {{address, nemo::Keyframe{.time = 0, .value = 0.8}}, {address, nemo::Keyframe{.time = 10, .value = 0.2}}}));
    ASSERT_TRUE(router->setGroupContext("A", {{"timelineClock", 5}}));
    ASSERT_TRUE(waitFor([&] { return std::abs(field->property("value").toDouble() - 0.5) < 0.0001; }));
    submit(nemo::setParamCommand(network.toULongLong(), roto.toULongLong(), "opacity", 0.9));
    EXPECT_NEAR(field->property("value").toDouble(), 0.5, 0.0001);
    auto* otherGroup = facade->createRotoControllerFor(network, roto, "B", window);
    ASSERT_NE(otherGroup, nullptr);
    ASSERT_TRUE(otherGroup->setProperty("frame", 10));
    QVariantMap otherState;
    ASSERT_TRUE(QMetaObject::invokeMethod(otherGroup, "parameterState", Qt::DirectConnection,
                                          Q_RETURN_ARG(QVariantMap, otherState),
                                          Q_ARG(QString, QString::number(element)), Q_ARG(QString, QString{}),
                                          Q_ARG(QString, QStringLiteral("opacity"))));
    EXPECT_DOUBLE_EQ(otherState.value("value").toDouble(), 0.2);
    EXPECT_NEAR(field->property("value").toDouble(), 0.5, 0.0001);
    capture("keyed-opacity");
}

TEST_F(RotoSurface, NestedGroupsAndLocksUseTheNativeHierarchy) {
    auto* add = item("rotoAddGroup_" + roto);
    ASSERT_NE(add, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(add));
    ASSERT_TRUE(waitFor([&] { return data() && data()->elements.size() == 1; }));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(add));
    ASSERT_TRUE(waitFor([&] { return data()->elements.size() == 2; }));
    EXPECT_EQ(data()->elements[1].parent, data()->elements[0].id);
    rectangle();
    ASSERT_EQ(data()->elements.size(), 3u);
    const auto shape = data()->elements[2];
    EXPECT_EQ(shape.parent, data()->elements[1].id);
    auto* lock = item("rotoLock_" + roto + "_" + QString::number(shape.id));
    ASSERT_NE(lock, nullptr);
    capture("hierarchy-before-lock");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(lock));
    ASSERT_TRUE(data()->elements[2].locked) << authoring()->property("error").toString().toStdString();
    const auto revision = session->revision();
    tool("select");
    const auto image = imageRect();
    const auto position = shape.points[0].position;
    const auto point = (image.topLeft() +
                        QPointF(position.value[0] * image.width() / 640.0, position.value[1] * image.height() / 360.0))
                           .toPoint();
    dragBetween(point, point + QPoint(30, 20));
    EXPECT_EQ(session->revision(), revision);
    EXPECT_EQ(data()->elements[2].points[0].position, position);
    capture("locked-hierarchy");
}

TEST_F(RotoSurface, MirroredFeatherAndTangentHandlesKeepThePointAnchored) {
    tool("ellipse");
    const auto image = imageRect();
    const double sx = image.width() / 640.0;
    const double sy = image.height() / 360.0;
    const auto screen = [&](double x, double y) { return (image.topLeft() + QPointF(x * sx, y * sy)).toPoint(); };
    dragBetween(screen(224, 126), screen(416, 234));
    ASSERT_TRUE(data() && data()->elements.size() == 1);
    auto shapes = *data();
    auto& shape = shapes.elements[0];
    shape.pivot = nemo::Vector2Value{{320, 180}};
    shape.scale = nemo::Vector2Value{{-2, 2}};
    shape.feather = 5;
    shape.points[0].feather = 10;
    const auto point = shape.points[0];
    submit(nemo::setRotoDataCommand(network.toULongLong(), roto.toULongLong(), shapes));
    tool("select");
    const double worldX = 320 - 2 * (point.position.value[0] - 320);
    const double worldY = 180 + 2 * (point.position.value[1] - 180);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, screen(worldX, worldY));
    const auto before = session->revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, screen(worldX - 30, worldY));
    move(screen(worldX - 50, worldY));
    capture("mirrored-feather-preview");
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, screen(worldX - 50, worldY));
    ASSERT_EQ(session->revision(), before + 1) << authoring()->property("error").toString().toStdString();
    EXPECT_NEAR(data()->elements[0].points[0].feather, 20.0, 1.0 / sx);
    EXPECT_EQ(data()->elements[0].points[0].position, point.position);
    const auto tangent = screen(worldX - 2 * point.outTangent.value[0], worldY + 2 * point.outTangent.value[1]);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, tangent);
    move(tangent + QPoint(25, 15));
    capture("tangent-preview");
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, tangent + QPoint(25, 15));
    ASSERT_EQ(session->revision(), before + 2);
    const auto changed = data()->elements[0].points[0];
    EXPECT_EQ(changed.position, point.position);
    EXPECT_NEAR(changed.outTangent.value[0], point.outTangent.value[0] - 25.0 / (2 * sx), 1.0);
    EXPECT_NEAR(changed.outTangent.value[1], point.outTangent.value[1] + 15.0 / (2 * sy), 1.0);
    EXPECT_FLOAT_EQ(changed.inTangent.value[0], -changed.outTangent.value[0]);
    EXPECT_FLOAT_EQ(changed.inTangent.value[1], -changed.outTangent.value[1]);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(data()->elements[0].points[0].outTangent, point.outTangent);
    EXPECT_EQ(data()->elements[0].points[0].position, point.position);
    capture("mirrored-handles");
}

TEST_F(RotoSurface, DenseAnimatedShapesRemainEditableAtProxyAndUpstreamFormatChanges) {
    nemo::RotoData shapes;
    nemo::RotoElement group;
    group.name = "Moving group";
    group.kind = nemo::RotoKind::Group;
    const auto groupId = nemo::appendRotoElement(shapes, std::move(group));
    for (int row = 0; row < 8; ++row)
        for (int column = 0; column < 8; ++column)
            ASSERT_NE(nemo::appendRotoEllipse(shapes, groupId, "Contour " + std::to_string(row * 8 + column),
                                              column * 80 + 40, row * 45 + 22.5, 26, 14),
                      0u);
    const auto networkId = network.toULongLong();
    const auto nodeId = roto.toULongLong();
    submit(nemo::setRotoDataCommand(networkId, nodeId, shapes));
    const nemo::ParameterAddress motion{networkId, nodeId, "translation", 0, groupId, 0};
    submit(nemo::setKeyframesCommand({{motion, nemo::Keyframe{.time = 0, .value = nemo::Vector2Value{{-10, 0}}}},
                                      {motion, nemo::Keyframe{.time = 10, .value = nemo::Vector2Value{{10, 0}}}}}));
    submit(nemo::setParamCommand(networkId, nodeId, "samples", std::int64_t{8}));
    ASSERT_TRUE(router->setGroupContext("A", {{"timelineClock", 5}}));
    controller->setResolutionMode("quarter");
    ASSERT_TRUE(waitFor([&] {
        const auto presentation = controller->presentation();
        return presentation && presentation->request.samplingScale == 4;
    }));
    capture("dense-animated-proxy");

    auto background = std::make_shared<nemo::NodeId>();
    auto reformat = std::make_shared<nemo::NodeId>();
    submit(nemo::addNodeCommand(networkId, "constcolor", "Background", background));
    submit(nemo::addNodeCommand(networkId, "reformat", "Background format", reformat));
    submit(nemo::transactionCommand(
        "Connect differently sized background",
        {nemo::setParamCommand(networkId, *reformat, "formatSource", nemo::ChoiceValue{"custom"}),
         nemo::setParamCommand(networkId, *reformat, "width", std::int64_t{320}),
         nemo::setParamCommand(networkId, *reformat, "height", std::int64_t{240}),
         nemo::setParamCommand(networkId, *reformat, "pixelAspect", 1.25),
         nemo::setParamCommand(networkId, nodeId, "replace", true),
         nemo::connectCommand(networkId, {*background, 0}, {*reformat, 0}),
         nemo::connectCommand(networkId, {*reformat, 0}, {nodeId, 0})}));
    controller->setChannel("A");
    const bool updated = waitFor([&] {
        const auto presentation = controller->presentation();
        return presentation && presentation->request.samplingScale == 4 &&
               controller->compositionSize() == QSizeF(320, 240) && controller->pixelAspect() == 1.25;
    });
    capture("dense-upstream-format");
    ASSERT_TRUE(updated) << "mode=" << controller->resolutionMode().toStdString() << " scale="
                         << (controller->presentation() ? controller->presentation()->request.samplingScale : 0)
                         << " format=" << controller->compositionSize().width() << 'x'
                         << controller->compositionSize().height() << " aspect=" << controller->pixelAspect()
                         << " status=" << controller->property("status").toString().toStdString();
    window->resize(950, 700);
    QTest::qWait(200);
    tool("select");
    const auto image = imageRect();
    const auto point = data()->elements[1].points[0].position;
    const double sx = image.width() / 320.0;
    const double sy = image.height() / 240.0;
    const auto start = (image.topLeft() + QPointF(point.value[0] * sx, point.value[1] * sy)).toPoint();
    const auto revision = session->revision();
    dragBetween(start, start + QPoint(20, 15));
    ASSERT_EQ(session->revision(), revision + 1) << authoring()->property("error").toString().toStdString();
    EXPECT_NEAR(data()->elements[1].points[0].position.value[0], point.value[0] + 20.0 / sx, 1.0);
    EXPECT_NEAR(data()->elements[1].points[0].position.value[1], point.value[1] + 15.0 / sy, 1.0);
    EXPECT_LE(item("rotoShapeEditor_" + roto)->width(), item("parametersPanel")->width());
    capture("dense-narrow-upstream-format");
}

TEST_F(RotoSurface, SavedRotoReopensWithoutRetainingAnOldProjectDraft) {
    rectangle();
    ASSERT_TRUE(data());
    const auto savedShapes = *data();
    const auto evidence = qEnvironmentVariable("NEMO93_EVIDENCE_DIR");
    const auto path = evidence.isEmpty() ? directory.filePath("authored.nemo") : evidence + "/native-authoring.nemo";
    QSignalSpy saved(projectFile.get(), &nemo::ui::ProjectFileController::saveFinished);
    projectFile->saveAs(QUrl::fromLocalFile(path));
    ASSERT_TRUE(waitFor([&] { return !saved.empty(); }));
    ASSERT_TRUE(saved.front()[0].toBool()) << projectFile->error().toStdString();
    tool("bezier");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, imageRect().center().toPoint());
    ASSERT_TRUE(authoring()->property("draftActive").toBool());
    ASSERT_TRUE(session->replaceDocument(session->snapshot()).replaced);
    QTest::qWait(30);
    ASSERT_FALSE(authoring()->property("draftActive").toBool());
    QSignalSpy opened(projectFile.get(), &nemo::ui::ProjectFileController::projectOpened);
    projectFile->openProject(QUrl::fromLocalFile(path));
    ASSERT_TRUE(waitFor([&] { return !opened.empty(); })) << projectFile->error().toStdString();
    if (opened.front()[0].toBool()) {
        auto* keep = item("keepCurrentWorkspaceButton");
        ASSERT_NE(keep, nullptr);
        ASSERT_TRUE(waitFor([&] { return keep->isVisible(); }));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(keep));
    }
    ASSERT_TRUE(data());
    EXPECT_TRUE(nemo::rotoContentEquals(*data(), savedShapes));
    EXPECT_FALSE(authoring()->property("draftActive").toBool());
    capture("native-reopened");
}

TEST_F(RotoSurface, PointTopologyAndLifetimeActionsUseAtomicHistory) {
    rectangle();
    tool("select");
    const auto image = imageRect();
    const auto first = data()->elements[0].points[0];
    const auto next = data()->elements[0].points[1];
    const auto position = (image.topLeft() + QPointF(first.position.value[0] * image.width() / 640,
                                                     first.position.value[1] * image.height() / 360))
                              .toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
    const auto click = [&](const QString& name) {
        auto* button = item(name + roto);
        EXPECT_NE(button, nullptr);
        if (button)
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(button));
        QTest::qWait(30);
    };
    click("rotoSmooth_");
    EXPECT_NE(data()->elements[0].points[0].outTangent, nemo::Vector2Value{});
    click("rotoCusp_");
    EXPECT_EQ(data()->elements[0].points[0].outTangent, nemo::Vector2Value{});
    click("rotoAddPoint_");
    ASSERT_EQ(data()->elements[0].points.size(), 5u);
    EXPECT_NEAR(data()->elements[0].points[1].position.value[0], (first.position.value[0] + next.position.value[0]) / 2,
                .01);
    EXPECT_NEAR(data()->elements[0].points[1].position.value[1], (first.position.value[1] + next.position.value[1]) / 2,
                .01);
    click("rotoRemovePoint_");
    ASSERT_EQ(data()->elements[0].points.size(), 4u);
    EXPECT_EQ(data()->elements[0].points[0].id, first.id);
    const auto enter = [&](const QString& name, Qt::Key key) {
        auto* field = item(name + roto);
        EXPECT_NE(field, nullptr);
        if (!field)
            return;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(field));
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, key);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(30);
    };
    enter("rotoFirstFrame_", Qt::Key_1);
    enter("rotoLastFrame_", Qt::Key_3);
    ASSERT_EQ(data()->elements[0].firstFrame, 1.0);
    ASSERT_EQ(data()->elements[0].lastFrame, 3.0);
    const auto before = session->revision();
    click("rotoClearLifetime_");
    EXPECT_EQ(session->revision(), before + 1);
    EXPECT_FALSE(data()->elements[0].firstFrame);
    EXPECT_FALSE(data()->elements[0].lastFrame);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(data()->elements[0].firstFrame, 1.0);
    EXPECT_EQ(data()->elements[0].lastFrame, 3.0);
    capture("point-topology-lifetime");
}

TEST_F(RotoSurface, InspectorOnlyScrubCancelsBeforeUndoingTheDocument) {
    rectangle();
    const auto element = data()->elements[0].id;
    const auto fieldName = "rotoField_" + roto + "_opacity_" + QString::number(element) + "_";
    workspace->closePanel("roto-view");
    ASSERT_TRUE(waitFor([&] { return item("viewerItem_roto-view") == nullptr && item(fieldName) != nullptr; }));
    // Closing a panel recreates the inspector. Render its pending layout before
    // measuring controls; provisional coordinates can hit a different row.
    ASSERT_FALSE(window->grabWindow().isNull());
    ASSERT_TRUE(waitFor([this] { return window->isActive(); }));
    for (const auto key : {Qt::Key_Escape, Qt::Key_Z}) {
        SCOPED_TRACE(key == Qt::Key_Escape ? "Escape" : "Undo");
        auto* field = item(fieldName);
        ASSERT_NE(field, nullptr);
        const auto start = center(field);
        // Keep press and movement in QtTest's native event sequence.
        QTest::mouseMove(window, start);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start - QPoint(45, 0), 30);
        ASSERT_TRUE(waitFor([&] { return field->property("scrubbing").toBool(); }));
        EXPECT_LT(field->property("displayedText").toString().toDouble(), 1.0);
        QTest::keyClick(window, key, key == Qt::Key_Z ? Qt::ControlModifier : Qt::NoModifier);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start - QPoint(45, 0));
        ASSERT_TRUE(data());
        ASSERT_EQ(data()->elements.size(), 1u);
        EXPECT_DOUBLE_EQ(data()->elements[0].opacity, 1.0);
    }
    capture("inspector-only-cancel");
}

TEST_F(RotoSurface, InspectorFlagsReflectUndoAndSelection) {
    rectangle();
    auto* flag = item("rotoFlag_" + roto + "_inverted");
    ASSERT_NE(flag, nullptr);
    EXPECT_FALSE(flag->property("checked").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(flag));
    ASSERT_TRUE(data()->elements[0].inverted);
    EXPECT_TRUE(flag->property("checked").toBool());
    ASSERT_TRUE(history->undo());
    QTest::qWait(30);
    EXPECT_FALSE(data()->elements[0].inverted);
    EXPECT_FALSE(flag->property("checked").toBool());
    rectangle();
    flag = item("rotoFlag_" + roto + "_inverted");
    ASSERT_NE(flag, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(flag));
    EXPECT_FALSE(data()->elements[0].inverted);
    EXPECT_TRUE(data()->elements[1].inverted);
    EXPECT_TRUE(flag->property("checked").toBool());
}

}  // namespace
