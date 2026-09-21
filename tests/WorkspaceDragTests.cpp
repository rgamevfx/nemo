#include "GraphInteraction.hpp"
#include "GraphItem.hpp"
#include "HistoryController.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterInteraction.hpp"
#include "ProjectFileController.hpp"
#include "TimelineItem.hpp"
#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Json = nlohmann::json;

const Json* containing(const Json& node, const std::string& id) {
    if (node.at("kind") == "tabs") {
        for (const auto& panel : node.at("panels")) {
            if (panel.at("id") == id) {
                return &node;
            }
        }
    } else {
        for (const auto& child : node.at("children")) {
            if (const auto* found = containing(child, id)) {
                return found;
            }
        }
    }
    return nullptr;
}

QQuickItem* visual(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) {
        return root;
    }
    for (auto* child : root->childItems()) {
        if (auto* found = visual(child, name)) {
            return found;
        }
    }
    return nullptr;
}

QVariantMap prefixedNodeRecord(const QVariantList& nodes, const QString& prefix) {
    for (const auto& value : nodes) {
        const auto node = value.toMap();
        if (node.value(QStringLiteral("id")).toString().startsWith(prefix))
            return node;
    }
    return {};
}

class WorkspaceDragTest : public testing::Test {
protected:
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController controller{directory.filePath("workspace.json")};
    nemo::ui::ViewerRuntime viewerRuntime;
    nemo::ProjectSession projectSession;
    // The shared presentation history the application composes: declared after
    // the session and before the engine, so both lifetimes stay valid.
    nemo::ui::HistoryController historyController{projectSession};
    nemo::ui::PanelContextRouter panelContextRouter{projectSession};
    // Context bindings persist through the same workspace presentation state.
    // Rendering and document ownership remain in their existing objects.
    nemo::ui::ParameterInteraction parameterInteraction;
    nemo::ui::ViewerController viewerController{&viewerRuntime, projectSession, parameterInteraction};
    // Main.qml reads the project file state; the harness injects the same
    // adapter and shared native chooser the application composes.
    nemo::ui::NativeFileChooser chooser;
    nemo::ui::ProjectFileController projectFile{projectSession, controller, panelContextRouter, chooser};
    QQmlApplicationEngine engine;
    QSignalSpy warnings{&engine, &QQmlEngine::warnings};
    QQuickWindow* window = nullptr;
    QString source;
    QString other;
    QString target;

    void SetUp() override {
        controller.registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"),
                                     QStringLiteral("ViewerPanel.qml"), QString());
        controller.registerPanelType(QStringLiteral("nodegraph"), QStringLiteral("Nodegraph"),
                                     QStringLiteral("GraphPanel.qml"), QString());
        controller.registerPanelType(QStringLiteral("timeline"), QStringLiteral("Timeline"),
                                     QStringLiteral("TimelinePanel.qml"), QString());
        panelContextRouter.setWorkspaceController(&controller);
        engine.rootContext()->setContextProperty("workspace", &controller);
        engine.rootContext()->setContextProperty("historyController", &historyController);
        engine.rootContext()->setContextProperty("panelContextRouter", &panelContextRouter);
        engine.rootContext()->setContextProperty("projectFile", &projectFile);
        engine.rootContext()->setContextProperty("viewerController", &viewerController);
        engine.load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine.rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        ASSERT_NE(window, nullptr);
        // Main starts hidden until its host configures the rendering backend.
        // These workspace-only gestures use the real unloaded viewer on
        // Qt's headless Null RHI; native image presentation is verified separately.
        window->show();
        window->requestActivate();
        QTest::qWait(60);
        const auto initial = snapshot();
        source = QString::fromStdString(initial["children"][0]["children"][0]["panels"][0]["id"]);
        other = QString::fromStdString(initial["children"][0]["children"][1]["panels"][0]["id"]);
        target = QString::fromStdString(containing(initial, other.toStdString())->at("id"));
    }

    void TearDown() override {
        if (window) {
            window->close();
            QTest::qWait(20);
        }
        EXPECT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
        EXPECT_EQ(warnings.count(), 0) << "The real QML scene must not emit runtime warnings";
    }

    Json snapshot() const { return Json::parse(QJsonDocument::fromVariant(controller.root()).toJson().toStdString()); }

    QQuickItem* item(const QString& name) {
        auto* found = visual(window->contentItem(), name);
        if (!found) {
            throw std::runtime_error("Missing QML item " + name.toStdString());
        }
        return found;
    }

    // The interaction core the graph panel drives; the paint item is a consumer
    // of it, so geometry, selection and hover are asked of the core.
    nemo::ui::GraphInteraction* graphInteraction() {
        auto* found = window->findChild<QObject*>(QStringLiteral("graphInteraction"));
        EXPECT_NE(found, nullptr) << "the graph panel must own its interaction core";
        return qobject_cast<nemo::ui::GraphInteraction*>(found);
    }

    QPoint center(const QString& name) {
        auto* found = item(name);
        return found->mapToScene(QPointF(found->width() / 2, found->height() / 2)).toPoint();
    }

    void hoverDrag(QPoint from, QPoint to) {
        const auto before = snapshot();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 12; ++step) {
            QTest::mouseMove(window, from + (to - from) * step / 12, 2);
        }
        EXPECT_EQ(snapshot(), before) << "Hovering must never mutate the layout";
    }

    void release(QPoint to) {
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QTest::qWait(40);
        // Observe a completed scenegraph frame, not just updated QML properties.
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        window->update();
        EXPECT_TRUE(rendered.wait(1000));
    }

    void drag(QPoint from, QPoint to) {
        hoverDrag(from, to);
        release(to);
    }

    // Qt 6.4's QtTest has no wheel helper, so the harness constructs the wheel
    // event and delivers it to the window — the real input path, with no
    // test-only handler in production code.
    void wheel(const QPoint& position, int angleDelta, const QPoint& pixelDelta = QPoint()) {
        QWheelEvent event(QPointF(position), QPointF(window->mapToGlobal(position)), pixelDelta, QPoint(0, angleDelta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window, &event);
    }

    // Wheels the graph in until the panel's zoom reaches `target`, anchored at
    // `anchor`, then lets the burst settle. A framed root network can sit at the
    // 0.2 floor, where a card is 5.6 px tall and the guarded middle band a card
    // press must land in is sub-pixel: aiming at a card or a pipe body by an
    // integer pixel then acquires a port of the same or a neighbouring card.
    // Tests that have to name such a point wheel in first.
    void zoomGraphIn(const QPoint& anchor, double target) {
        auto* panel = item("graphPanel");
        for (int step = 0; step < 40 && panel->property("zoom").toDouble() < target; ++step) {
            wheel(anchor, 120);
            QTest::qWait(20);
        }
        QTest::qWait(320);
        ASSERT_GE(panel->property("zoom").toDouble(), target) << "the harness must reach the zoom it aims at";
    }

    // The graph panel's own screen-to-scene mapping, so a test can name the
    // image point a gesture is anchored at without reimplementing it.
    QPointF scenePointAt(QQuickItem* panel, const QPointF& surfacePoint) {
        QVariant result;
        EXPECT_TRUE(QMetaObject::invokeMethod(panel, "scenePoint", Q_RETURN_ARG(QVariant, result),
                                              Q_ARG(QVariant, surfacePoint.x()), Q_ARG(QVariant, surfacePoint.y())));
        const auto point = result.toMap();
        return QPointF(point.value(QStringLiteral("x")).toDouble(), point.value(QStringLiteral("y")).toDouble());
    }

    // Any panel other than `panelId`, read from the real layout.
    QString otherPanelId(const QString& panelId) {
        const Json root = snapshot();
        std::function<QString(const Json&)> first = [&](const Json& node) -> QString {
            if (node.at("kind") == "tabs") {
                for (const auto& panel : node.at("panels")) {
                    const auto id = QString::fromStdString(panel.at("id").get<std::string>());
                    if (id != panelId)
                        return id;
                }
                return {};
            }
            for (const auto& child : node.at("children")) {
                if (const auto found = first(child); !found.isEmpty())
                    return found;
            }
            return {};
        };
        return first(root);
    }

    // The rendering path the capture was taken on: the offscreen harness asks
    // for Null, the opt-in native configuration for a real API.
    [[nodiscard]] QString graphicsApiName() const {
        const auto* interface = window ? window->rendererInterface() : nullptr;
        switch (interface ? interface->graphicsApi() : QSGRendererInterface::Unknown) {
        case QSGRendererInterface::OpenGL:
            return QStringLiteral("opengl");
        case QSGRendererInterface::Vulkan:
            return QStringLiteral("vulkan");
        case QSGRendererInterface::Null:
            return QStringLiteral("null");
        case QSGRendererInterface::Software:
            return QStringLiteral("software");
        default:
            return QStringLiteral("unknown");
        }
    }

    // Graph evidence record for a recorded evidence directory, when one is set:
    // the view the gesture reached, the sequence of applied states, and (in the
    // opt-in native configuration) the capture of the real surface. On the
    // offscreen/Null harness a window grab carries no scene-graph content, which
    // the record states rather than implying pixels it does not have.
    void recordEvidence(const QString& name, const QJsonObject& extra = {}) {
        const auto write = [&](const QString& directory, const char* captureVariable) {
            if (directory.isEmpty())
                return;
            QDir().mkpath(directory);
            auto* panel = item("graphPanel");
            QJsonObject environment{
                {QStringLiteral("platform"), QGuiApplication::platformName()},
                {QStringLiteral("graphics_api"), graphicsApiName()},
                {QStringLiteral("native_ui"), qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") == 1},
                {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
                {QStringLiteral("device_pixel_ratio"), window->devicePixelRatio()},
                {QStringLiteral("window_width"), window->width()},
                {QStringLiteral("window_height"), window->height()},
                {QStringLiteral("appearance_preset"), controller.appearancePreset()},
                {QStringLiteral("accent_override"), controller.accentOverride()},
                {QStringLiteral("record"), name},
                {QStringLiteral("graph_panel"), panel->property("panelId").toString()},
                {QStringLiteral("graph_zoom"), panel->property("zoom").toDouble()},
                {QStringLiteral("graph_pan_x"), panel->property("panX").toDouble()},
                {QStringLiteral("graph_pan_y"), panel->property("panY").toDouble()},
                {QStringLiteral("graph_network"), panel->property("graphNetworkId").toString()},
                {QStringLiteral("graph_selection"), panel->property("selectedNodeIds").toJsonArray()}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                environment.insert(it.key(), it.value());
            QFile environmentFile(directory + QStringLiteral("/graph-") + name + QStringLiteral(".json"));
            EXPECT_TRUE(environmentFile.open(QIODevice::WriteOnly));
            environmentFile.write(QJsonDocument(environment).toJson(QJsonDocument::Indented));
            // The window grab carries the graph only when the scene graph
            // actually renders (the opt-in native configuration); on the
            // offscreen/Null harness it is written anyway, so the gap is visible
            // rather than implied.
            if (qEnvironmentVariableIntValue(captureVariable) == 1) {
                QTest::mouseMove(window, QPoint(4, 4));
                QTest::qWait(60);
                EXPECT_TRUE(
                    window->grabWindow().save(directory + QStringLiteral("/graph-") + name + QStringLiteral(".png")));
            }
        };
        write(qEnvironmentVariable("NEMO84_EVIDENCE_DIR"), "NEMO84_GRAPH_CAPTURE");
        write(qEnvironmentVariable("NEMO100_EVIDENCE_DIR"), "NEMO100_GRAPH_CAPTURE");
    }

    // One persisted graph view record for a network, as the project stores it.
    QVariantMap storedView(const QString& panelId, const QString& network) {
        return controller.panelState(panelId).value(QStringLiteral("graphViews")).toMap().value(network).toMap();
    }
};

TEST_F(WorkspaceDragTest, ClicksCancellationAndNormalReleaseRemainDistinct) {
    const auto before = snapshot();
    const auto from = center("panelType_" + source);
    const auto to = center("leaf_" + target);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, from);
    QTest::qWait(30);
    auto* menu = window->findChild<QObject*>("panelTypeMenu_" + source);
    ASSERT_NE(menu, nullptr);
    EXPECT_TRUE(menu->property("visible").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(30);

    hoverDrag(from, to);
    EXPECT_TRUE(item("dockPreview")->isVisible());
    EXPECT_TRUE(item("dockLabel")->isVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    release(to);
    EXPECT_EQ(snapshot(), before);
    EXPECT_FALSE(item("dockPreview")->isVisible());
    drag(from, QPoint(-30, -30));
    EXPECT_EQ(snapshot(), before);

    // Qt 6.4 reports canceled after a normal UngrabExclusive as cleanup.
    // That notification must not discard this accepted drop.
    drag(from, to);
    const auto after = snapshot();
    const auto* leaf = containing(after, source.toStdString());
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(leaf->at("id"), target.toStdString());
    EXPECT_EQ(leaf->at("panels").size(), 2);
    EXPECT_EQ(leaf->at("active"), source.toStdString());
    EXPECT_FALSE(item("dockPreview")->isVisible());
    EXPECT_FALSE(item("dockLabel")->isVisible());
}

TEST_F(WorkspaceDragTest, TabsReorderSplitAndRestoreAfterRealGestures) {
    drag(center("panelHeader_" + source), center("leaf_" + target));
    auto* first = item("panelTab_" + other);
    const auto gap = first->mapToScene(QPointF(2, first->height() / 2)).toPoint();
    hoverDrag(center("panelTab_" + source), gap);
    EXPECT_TRUE(item("tabLine")->isVisible());
    EXPECT_FALSE(item("dockPreview")->isVisible());
    release(gap);
    const auto reordered = snapshot();
    const auto* group = containing(reordered, source.toStdString());
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->at("panels")[0]["id"], source.toStdString());

    auto* leaf = item("leaf_" + target);
    const auto right = leaf->mapToScene(QPointF(leaf->width() - 8, leaf->height() / 2)).toPoint();
    hoverDrag(center("panelTab_" + source), right);
    EXPECT_TRUE(item("dockPreview")->isVisible());
    const auto previewWidth = item("dockPreview")->width();
    release(right);
    const auto split = snapshot();
    const auto* moved = containing(split, source.toStdString());
    const auto* remaining = containing(split, other.toStdString());
    ASSERT_NE(moved, nullptr);
    ASSERT_NE(remaining, nullptr);
    EXPECT_NE(moved->at("id"), remaining->at("id"));
    EXPECT_NEAR(item("leaf_" + QString::fromStdString(moved->at("id")))->width(), previewWidth, 1);
    ASSERT_TRUE(controller.save());
    nemo::workspace::WorkspaceController restored(directory.filePath("workspace.json"));
    EXPECT_EQ(restored.root(), controller.root());
}

TEST_F(WorkspaceDragTest, DeactivationCancelsAndSmallTargetsRejectSplits) {
    const auto before = snapshot();
    const auto from = center("panelHeader_" + source);
    const auto to = center("leaf_" + target);
    hoverDrag(from, to);
    QQuickWindow anotherWindow;
    anotherWindow.resize(200, 100);
    anotherWindow.show();
    anotherWindow.requestActivate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return anotherWindow.isActive() && !window->isActive(); }));
    release(to);
    EXPECT_EQ(snapshot(), before);
    EXPECT_FALSE(item("dockLabel")->isVisible());
    anotherWindow.hide();
    window->requestActivate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->isActive(); }));

    controller.setRatio(QString::fromStdString(before["children"][0]["id"]), 0.88);
    // A metadata update publishes the new ratio without simulating a divider.
    controller.setGroup(source, "B");
    QTest::qWait(40);
    auto* narrow = item("leaf_" + target);
    const auto edge = narrow->mapToScene(QPointF(narrow->width() - 4, narrow->height() / 2)).toPoint();
    const auto resized = snapshot();
    hoverDrag(center("panelHeader_" + source), edge);
    EXPECT_FALSE(item("dockPreview")->isVisible());
    release(edge);
    EXPECT_EQ(snapshot(), resized);
}

TEST_F(WorkspaceDragTest, EdgeTargetsAndDividersWorkAcrossNestedLayoutChanges) {
    for (int edge = 0; edge < 4; ++edge) {
        SCOPED_TRACE(edge);
        controller.reset();
        QTest::qWait(40);
        auto* leaf = item("leaf_" + target);
        const bool horizontal = edge < 2;
        const bool leading = edge % 2 == 0;
        const QPointF local = horizontal ? QPointF(leading ? 4 : leaf->width() - 4, leaf->height() / 2)
                                         : QPointF(leaf->width() / 2, leading ? 4 : leaf->height() - 4);
        drag(center("panelHeader_" + source), leaf->mapToScene(local).toPoint());
        const auto moved = snapshot();
        const auto& split = moved.at("children").at(0);
        ASSERT_EQ(split.at("kind"), "split");
        EXPECT_EQ(split.at("orientation"), horizontal ? "horizontal" : "vertical");
        EXPECT_EQ(split.at("children").at(leading ? 0 : 1).at("panels").at(0).at("id"), source.toStdString());

        const auto firstId = QString::fromStdString(split.at("children").at(0).at("id"));
        auto* first = item("leaf_" + firstId);
        const auto divider = first
                                 ->mapToScene(horizontal ? QPointF(first->width() + 2, first->height() / 2)
                                                         : QPointF(first->width() / 2, first->height() + 2))
                                 .toPoint();
        drag(divider, divider + (horizontal ? QPoint(40, 0) : QPoint(0, 40)));
        EXPECT_FALSE(item("dockLabel")->isVisible());
        EXPECT_GT(snapshot().at("children").at(0).at("ratio").get<double>(), split.at("ratio").get<double>());
    }
    const auto layout = snapshot();
    auto* upper = item("leaf_" + QString::fromStdString(layout["children"][0]["children"][0]["id"]));
    auto* lower = item("leaf_" + QString::fromStdString(layout["children"][0]["children"][1]["id"]));
    const auto outerDivider = lower->mapToScene(QPointF(lower->width() / 2, lower->height() + 2)).toPoint();
    drag(outerDivider, QPoint(outerDivider.x(), 50));
    EXPECT_GE(upper->height(), item("panelHeader_" + source)->height());
    EXPECT_GE(lower->height(), item("panelHeader_" + other)->height());
}

TEST_F(WorkspaceDragTest, NestedPanelMenusSwitchTypeAndCloseViewer) {
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("panelType_" + other));
    QTest::qWait(30);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("panelTypeChoice_viewer_" + other));
    QTest::qWait(60);
    const auto switched = snapshot();
    const auto* leaf = containing(switched, other.toStdString());
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(leaf->at("panels").at(0).at("type"), "viewer");

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("panelType_" + other));
    QTest::qWait(30);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("panelClose_" + other));
    QTest::qWait(60);
    EXPECT_EQ(containing(snapshot(), other.toStdString()), nullptr);
}
TEST_F(WorkspaceDragTest, CatalogMenuCreatesRealNodesAndTimelineSeeks) {
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphToolsButton"));
    QTest::qWait(30);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("toolCategory_Generators"));
    QTest::qWait(30);
    const auto before = viewerController.graphNodes().size();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("toolNode_constcolor"));
    QTest::qWait(30);
    ASSERT_EQ(viewerController.graphNodes().size(), before + 1);
    EXPECT_EQ(viewerController.graphNodes().last().toMap().value("type").toString(), "constcolor");
    static_cast<void>(projectSession.undo(nemo::EditOptions{projectSession.revision(), {}}));
    EXPECT_EQ(viewerController.graphNodes().size(), before);

    viewerController.openSource("/tmp/nemo-interactive-command-source.mkv");
    QTest::qWait(30);
    auto* ruler = item("timelineRuler");
    const auto quarter = ruler->mapToScene(QPointF(ruler->width() / 4, 10)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, quarter);
    QTest::qWait(30);
    const auto quarterFrame = viewerController.frame();
    EXPECT_GT(quarterFrame, 0);
    const auto middle = ruler->mapToScene(QPointF(ruler->width() / 2, 10)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, quarter);
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(window, quarter + (middle - quarter) * step / 8, 2);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, middle);
    QTest::qWait(30);
    EXPECT_GT(viewerController.frame(), quarterFrame);
}

TEST_F(WorkspaceDragTest, LowZoomConnectedNodeBodiesRemainSelectable) {
    const auto network = viewerController.rootNetworkId();
    const auto source = viewerController.createGraphNode(network, "constcolor", "Source", 40, 40, {}, {});
    const auto target = viewerController.createGraphNode(network, "merge", "Target", 40, 240, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, source, 0, target, 0));
    auto* panel = item("graphPanel");
    const auto panelId = panel->property("panelId").toString();
    auto state = controller.panelState(panelId);
    auto views = state.value("graphViews").toMap();
    views[network] = QVariantMap{{"zoom", 0.5}, {"panX", 80.0}, {"panY", 80.0}};
    state["graphViews"] = views;
    controller.setPanelState(panelId, state);
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto revision = projectSession.revision();
    const auto edges = viewerController.graphEdges();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      graph->mapToScene(graphInteraction()->nodeRect(source).center()).toPoint());
    EXPECT_EQ(graphInteraction()->selectedNodeIds(), QStringList{source});
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier,
                      graph->mapToScene(graphInteraction()->nodeRect(target).center()).toPoint());
    EXPECT_EQ(graphInteraction()->selectedNodeIds(), (QStringList{source, target}));
    EXPECT_EQ(projectSession.revision(), revision);
    EXPECT_EQ(viewerController.graphEdges(), edges);
}

// Issue #84 slice 1. A wheel burst is one gesture: the accumulated deltas are
// applied once per event-loop turn, the view stays anchored under the pointer,
// and the final view is written once when the burst settles — never per input
// event. Navigation changes no document state and creates no history entry.
TEST_F(WorkspaceDragTest, GraphWheelBurstAppliesPerTurnAndPersistsOnceWhenItSettles) {
    const auto network = viewerController.rootNetworkId();
    const auto source = viewerController.createGraphNode(network, "constcolor", "Source", 40, 40, {}, {});
    const auto target = viewerController.createGraphNode(network, "merge", "Target", 40, 240, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, source, 0, target, 0));
    auto* panel = item("graphPanel");
    const auto panelId = panel->property("panelId").toString();

    // A known starting view, so the expected post-burst zoom is arithmetic the
    // test owns rather than a function of the panel's size.
    auto state = controller.panelState(panelId);
    auto views = state.value(QStringLiteral("graphViews")).toMap();
    views[network] =
        QVariantMap{{QStringLiteral("zoom"), 0.5}, {QStringLiteral("panX"), 80.0}, {QStringLiteral("panY"), 80.0}};
    state[QStringLiteral("graphViews")] = views;
    controller.setPanelState(panelId, state);
    QTest::qWait(40);
    ASSERT_DOUBLE_EQ(panel->property("zoom").toDouble(), 0.5);

    int writes = 0;
    // A local context object: the connection must not outlive this body, or a
    // state write from panel teardown would call into dead locals.
    QObject writesScope;
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::panelStateChanged, &writesScope,
                     [&](const QString& changed) {
                         if (changed == panelId)
                             ++writes;
                     });
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      graph->mapToScene(graphInteraction()->nodeRect(source).center()).toPoint());
    const auto selected = graphInteraction()->selectedNodeIds();
    ASSERT_EQ(selected.size(), 1);
    // Selecting is itself a gesture, so it writes its view once; the burst that
    // follows is measured against this baseline.
    QTest::qWait(20);
    const int baseline = writes;

    auto* surface = item("graphSurface");
    const QPoint anchorScene = surface->mapToScene(QPointF(surface->width() * 0.35, surface->height() * 0.4)).toPoint();
    // The event lands on integer window coordinates, so the anchor the panel
    // sees is that point mapped back into the surface.
    const QPointF anchor = surface->mapFromScene(QPointF(anchorScene));
    QTest::mouseMove(window, anchorScene);
    QTest::qWait(20);
    const auto anchoredBefore = scenePointAt(panel, anchor);
    const auto revision = projectSession.revision();

    // The gesture reaches the view once per event-loop turn, so a slow wheel
    // steps the view once for each notch, and a burst that lands in one turn
    // lands exactly where the accumulated deltas indicate.
    QJsonArray applied;
    for (int notch = 0; notch < 3; ++notch) {
        wheel(anchorScene, 120);
        QTest::qWait(20);
        applied.append(panel->property("zoom").toDouble());
    }
    ASSERT_EQ(applied.size(), 3);
    EXPECT_LT(applied.at(0).toDouble(), applied.at(1).toDouble());
    EXPECT_LT(applied.at(1).toDouble(), applied.at(2).toDouble());
    EXPECT_NEAR(applied.at(2).toDouble(), 0.5 * std::exp(53.0 * 0.002 * 3.0), 1e-9);
    EXPECT_EQ(writes, baseline) << "a wheel burst must not write the workspace mid-gesture";
    for (int notch = 0; notch < 5; ++notch)
        wheel(anchorScene, 120);
    QTest::qWait(20);
    const double expected = 0.5 * std::exp(53.0 * 0.002 * 8.0);
    EXPECT_NEAR(panel->property("zoom").toDouble(), expected, 1e-9);
    // The pixel under the pointer is the pixel that stays under the pointer.
    const auto anchoredAfter = scenePointAt(panel, anchor);
    EXPECT_NEAR(anchoredAfter.x(), anchoredBefore.x(), 1e-6);
    EXPECT_NEAR(anchoredAfter.y(), anchoredBefore.y(), 1e-6);
    // Navigation is not an edit: no history, no document revision, no selection
    // change, no wiring change.
    EXPECT_EQ(projectSession.revision(), revision);
    EXPECT_EQ(graphInteraction()->selectedNodeIds(), selected);

    // The burst settles: exactly one write, carrying the view the gesture ended
    // with.
    QTest::qWait(320);
    EXPECT_EQ(writes, baseline + 1);
    const auto stored = storedView(panelId, network);
    EXPECT_NEAR(stored.value(QStringLiteral("zoom")).toDouble(), panel->property("zoom").toDouble(), 1e-9);
    EXPECT_NEAR(stored.value(QStringLiteral("panX")).toDouble(), panel->property("panX").toDouble(), 1e-9);
    EXPECT_NEAR(stored.value(QStringLiteral("panY")).toDouble(), panel->property("panY").toDouble(), 1e-9);

    // A trackpad pixel delta and an equivalent angle delta are the same gesture,
    // and an opposite burst returns to where the artist started.
    const double zoomed = panel->property("zoom").toDouble();
    wheel(anchorScene, 0, QPoint(0, 265));
    QTest::qWait(20);
    EXPECT_NEAR(panel->property("zoom").toDouble(), zoomed * std::exp(53.0 * 0.002 * 5.0), 1e-9);
    QTest::qWait(320);
    EXPECT_EQ(writes, baseline + 2);
    wheel(anchorScene, -600);
    QTest::qWait(20);
    EXPECT_NEAR(panel->property("zoom").toDouble(), zoomed, 1e-9);
    QTest::qWait(320);
    EXPECT_EQ(writes, baseline + 3);

    // Frame all still works after the whole gesture sequence, and is itself a
    // settled gesture that writes once.
    const int settled = writes;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(60);
    const double framedZoom = panel->property("zoom").toDouble();
    EXPECT_GT(framedZoom, 0.0);
    EXPECT_EQ(writes, settled + 1);
    EXPECT_NEAR(storedView(panelId, network).value(QStringLiteral("zoom")).toDouble(), framedZoom, 1e-9);

    recordEvidence(QStringLiteral("zoom-burst"), QJsonObject{{QStringLiteral("sampled_notches"), 3},
                                                             {QStringLiteral("burst_notches_in_one_turn"), 5},
                                                             {QStringLiteral("applied_zoom_per_turn"), applied},
                                                             {QStringLiteral("persisted_writes"), writes - baseline},
                                                             {QStringLiteral("anchor_scene_x"), anchoredBefore.x()},
                                                             {QStringLiteral("anchor_scene_y"), anchoredBefore.y()},
                                                             {QStringLiteral("anchored_scene_x"), anchoredAfter.x()},
                                                             {QStringLiteral("anchored_scene_y"), anchoredAfter.y()},
                                                             {QStringLiteral("frame_all_zoom"), framedZoom}});
}

// A settled view is what a scope switch, a gesture release and a panel-state
// write all leave behind; a write by one panel must not touch another panel's
// view or its display model.
TEST_F(WorkspaceDragTest, GraphViewGestureWritesOnceAndOtherPanelsDoNotRewriteIt) {
    const auto network = viewerController.rootNetworkId();
    viewerController.createGraphNode(network, "constcolor", "Source", 40, 40, {}, {});
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* panel = item("graphPanel");
    const auto panelId = panel->property("panelId").toString();

    int graphWrites = 0;
    int viewerWrites = 0;
    QObject writesScope;
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::panelStateChanged, &writesScope,
                     [&](const QString& changed) {
                         if (changed == panelId)
                             ++graphWrites;
                         else
                             ++viewerWrites;
                     });
    // A middle-drag pan is one gesture: it applies every move and writes once,
    // when the button is released.
    auto* surface = item("graphSurface");
    const QPoint from = surface->mapToScene(QPointF(80, 80)).toPoint();
    QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, from);
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(window, from + QPoint(3 * step, 2 * step), 2);
    EXPECT_EQ(graphWrites, 0) << "a pan drag must not write the workspace mid-gesture";
    QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, from + QPoint(24, 16));
    QTest::qWait(60);
    EXPECT_EQ(graphWrites, 1);
    const auto panned = storedView(panelId, network);
    EXPECT_NEAR(panned.value(QStringLiteral("panX")).toDouble(), panel->property("panX").toDouble(), 1e-9);
    EXPECT_NEAR(panned.value(QStringLiteral("zoom")).toDouble(), panel->property("zoom").toDouble(), 1e-9);

    // Another panel writes its own state, as a viewer pan or an inspector
    // arrangement does. It must not reach this panel's view or state record.
    const auto zoomBefore = panel->property("zoom").toDouble();
    const auto panBefore = panel->property("panX").toDouble();
    const auto foreignPanel = otherPanelId(panelId);
    ASSERT_FALSE(foreignPanel.isEmpty());
    controller.setPanelState(foreignPanel, QVariantMap{{QStringLiteral("panX"), 120.0}});
    QTest::qWait(60);
    EXPECT_EQ(graphWrites, 1);
    EXPECT_EQ(viewerWrites, 1);
    EXPECT_DOUBLE_EQ(panel->property("zoom").toDouble(), zoomBefore);
    EXPECT_DOUBLE_EQ(panel->property("panX").toDouble(), panBefore);
    EXPECT_NEAR(storedView(panelId, network).value(QStringLiteral("panX")).toDouble(), panBefore, 1e-9);
    recordEvidence(QStringLiteral("independent-from-viewer-write"),
                   QJsonObject{{QStringLiteral("graph_writes"), graphWrites},
                               {QStringLiteral("viewer_writes"), viewerWrites},
                               {QStringLiteral("zoom_before_foreign_write"), zoomBefore},
                               {QStringLiteral("pan_before_foreign_write"), panBefore}});
}

// Slice 1 conforms to the prototype's zoom-independent navigation coverage at
// more than one zoom level: screen-space port acquisition and protection, drag
// mapping from screen to scene distance, a wire dropped glyph to glyph, and
// last-click placement in scene coordinates.
// The gesture boundaries write the view: a burst that has been applied but has
// not settled yet is still what the project records when the application closes.
TEST_F(WorkspaceDragTest, GraphViewInMotionIsPersistedWhenTheWindowCloses) {
    const auto network = viewerController.rootNetworkId();
    viewerController.createGraphNode(network, "constcolor", "Source", 40, 40, {}, {});
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* panel = item("graphPanel");
    const auto panelId = panel->property("panelId").toString();
    auto* surface = item("graphSurface");
    const QPoint anchorScene = surface->mapToScene(QPointF(surface->width() / 2, surface->height() / 2)).toPoint();
    QTest::mouseMove(window, anchorScene);
    QTest::qWait(20);

    for (int notch = 0; notch < 4; ++notch)
        wheel(anchorScene, 120);
    QTest::qWait(20);
    const double applied = panel->property("zoom").toDouble();
    EXPECT_NE(applied, 1.0) << "the burst must have been applied before the close";

    // No settle wait: the window closes with the burst still in flight.
    window->close();
    QTest::qWait(40);
    EXPECT_NEAR(storedView(panelId, network).value(QStringLiteral("zoom")).toDouble(), applied, 1e-9);
}

TEST_F(WorkspaceDragTest, GraphScreenSpaceHitTestingSurvivesEveryZoomLevel) {
    const auto network = viewerController.rootNetworkId();
    const auto a = viewerController.createGraphNode(network, "constcolor", "A", 40, 40, {}, {});
    const auto b = viewerController.createGraphNode(network, "merge", "B", 40, 200, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, a, 0, b, 0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* panel = item("graphPanel");
    auto* surface = item("graphSurface");
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    ASSERT_FALSE(graphInteraction()->nodeRect(a).isEmpty()) << "the fixture must address the node the panel displays";

    // Zoom anchored on A, so A stays under the pointer while the level changes.
    const auto zoomUntil = [&](bool in) {
        for (int step = 0; step < 40; ++step) {
            const double zoom = panel->property("zoom").toDouble();
            if (in ? zoom > 2.0 : zoom < 0.6)
                return zoom;
            const QPoint anchor = graph->mapToScene(graphInteraction()->nodeRect(a).center()).toPoint();
            QTest::mouseMove(window, anchor);
            QTest::qWait(10);
            wheel(anchor, in ? 120 : -120);
            QTest::qWait(20);
        }
        return panel->property("zoom").toDouble();
    };
    // Hover needs a delivered move before it reports anything; the first move
    // after a click only establishes the pointer.
    const auto hover = [&](const QPoint& point, int radius) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            QTest::mouseMove(window, point + QPoint(radius, 0));
            QTest::mouseMove(window, point);
            QTest::qWait(20);
        }
        return graphInteraction()->property("hoveredEndpoint").toMap();
    };

    for (const bool zoomedIn : {true, false}) {
        SCOPED_TRACE(zoomedIn ? "zoomed in" : "zoomed out");
        const double zoom = zoomUntil(zoomedIn);
        EXPECT_GT(zoom, zoomedIn ? 2.0 : 0.0);
        EXPECT_LT(zoom, zoomedIn ? 2.6 : 0.6);
        graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
        ASSERT_NE(graph, nullptr);

        // Acquisition: the glyph itself is acquired at every zoom, because the
        // radius is the screen-space one. Protection: 20 px away, nothing is.
        const QPoint output = graph->mapToScene(graphInteraction()->portPosition(a, 0, true)).toPoint();
        const auto acquired = hover(output, 6);
        EXPECT_EQ(acquired.value(QStringLiteral("node")).toString(), a);
        EXPECT_EQ(acquired.value(QStringLiteral("port")).toInt(), 0);
        EXPECT_EQ(acquired.value(QStringLiteral("direction")).toString(), QStringLiteral("output"));
        EXPECT_TRUE(hover(output - QPoint(20, 0), 6).isEmpty())
            << "port protection must stay screen-space at this zoom";

        // A screen-space drag maps to scene distance through the current zoom.
        const auto nodeBefore = prefixedNodeRecord(viewerController.graphNodes(), a);
        const QPoint center = graph->mapToScene(graphInteraction()->nodeRect(a).center()).toPoint();
        const QPoint delta(60, 40);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseMove(window, center + delta, 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, center + delta);
        QTest::qWait(40);
        const auto nodeAfter = prefixedNodeRecord(viewerController.graphNodes(), a);
        EXPECT_NEAR(nodeAfter.value(QStringLiteral("x")).toDouble() - nodeBefore.value(QStringLiteral("x")).toDouble(),
                    delta.x() / zoom, 1.0)
            << "screen motion must map to scene distance through the current zoom";
        EXPECT_NEAR(nodeAfter.value(QStringLiteral("y")).toDouble() - nodeBefore.value(QStringLiteral("y")).toDouble(),
                    delta.y() / zoom, 1.0);

        // Reroute dots stay hittable at this zoom: an Alt-click on the pipe
        // body inserts one, and the pointer then acquires that dot.
        const auto edge = viewerController.graphEdges().first().toMap();
        const auto edgeId = edge.value(QStringLiteral("id")).toString();
        const QPointF pipeStart = graphInteraction()->portPosition(a, 0, true);
        const QPointF pipeEnd = graphInteraction()->portPosition(b, 0, false);
        QTest::mouseClick(window, Qt::LeftButton, Qt::AltModifier,
                          graph->mapToScene((pipeStart + pipeEnd) / 2).toPoint());
        QTest::qWait(40);
        const auto routed = viewerController.graphEdges().first().toMap().value(QStringLiteral("route")).toList();
        ASSERT_EQ(routed.size(), 1);
        const auto dot = routed.first().toMap();
        const QPoint dotScene = graph
                                    ->mapToScene(QPointF(dot.value(QStringLiteral("x")).toDouble(),
                                                         dot.value(QStringLiteral("y")).toDouble()))
                                    .toPoint();
        QTest::mouseMove(window, dotScene + QPoint(4, 0));
        QTest::qWait(20);
        QTest::mouseMove(window, dotScene);
        QTest::qWait(20);
        const auto hoveredDot = graphInteraction()->property("hoveredReroute").toMap();
        EXPECT_EQ(hoveredDot.value(QStringLiteral("edge")).toString(), edgeId);
        EXPECT_EQ(hoveredDot.value(QStringLiteral("index")).toInt(), 0);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(20);
        ASSERT_EQ(viewerController.graphEdges().first().toMap().value(QStringLiteral("route")).toList().size(), 0);

        recordEvidence(
            zoomedIn ? QStringLiteral("zoomed-in-hit-testing") : QStringLiteral("zoomed-out-hit-testing"),
            QJsonObject{{QStringLiteral("acquired_port_node"), acquired.value(QStringLiteral("node")).toString()},
                        {QStringLiteral("acquired_port_index"), acquired.value(QStringLiteral("port")).toInt()},
                        {QStringLiteral("drag_screen_delta_x"), delta.x()},
                        {QStringLiteral("drag_screen_delta_y"), delta.y()},
                        {QStringLiteral("drag_scene_delta_x"), nodeAfter.value(QStringLiteral("x")).toDouble() -
                                                                   nodeBefore.value(QStringLiteral("x")).toDouble()},
                        {QStringLiteral("drag_scene_delta_y"), nodeAfter.value(QStringLiteral("y")).toDouble() -
                                                                   nodeBefore.value(QStringLiteral("y")).toDouble()},
                        {QStringLiteral("reroute_dot_hit_index"), hoveredDot.value(QStringLiteral("index")).toInt()}});
        if (zoomedIn)
            continue;

        // A wire dropped glyph to glyph still connects while zoomed out, where
        // the glyphs themselves are a couple of pixels wide.
        const QPoint source = graph->mapToScene(graphInteraction()->portPosition(a, 0, true)).toPoint();
        const QPoint destination = graph->mapToScene(graphInteraction()->portPosition(b, 1, false)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, source);
        QTest::mouseMove(window, destination, 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, destination);
        QTest::qWait(40);
        ASSERT_EQ(viewerController.graphEdges().size(), 2);
        EXPECT_EQ(viewerController.graphEdges().last().toMap().value("toNode").toString(), b);
        const auto connected = viewerController.graphEdges();
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(20);
        ASSERT_EQ(viewerController.graphEdges().size(), 1);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        EXPECT_EQ(viewerController.graphEdges(), connected);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);

        // Last-click placement is recorded in scene coordinates, so a creation
        // lands where the artist clicked, not where they happen to be zoomed.
        const QPoint empty = surface->mapToScene(QPointF(surface->width() * 0.2, surface->height() * 0.85)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, empty);
        QTest::qWait(20);
        ASSERT_TRUE(panel->property("lastClickValid").toBool());
        const auto lastClickX = panel->property("lastClickX").toDouble();
        const auto lastClickY = panel->property("lastClickY").toDouble();
        const auto count = viewerController.graphNodes().size();
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::qWait(30);
        item("graphSearchField")->setProperty("text", "Constant Color");
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(40);
        ASSERT_EQ(viewerController.graphNodes().size(), count + 1);
        const auto placed = viewerController.graphNodes().last().toMap();
        EXPECT_NEAR(placed.value(QStringLiteral("x")).toDouble(), lastClickX - 56.0, 1.0);
        EXPECT_NEAR(placed.value(QStringLiteral("y")).toDouble(), lastClickY - 14.0, 1.0);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(20);
    }
}

TEST_F(WorkspaceDragTest, EqualNodeIdsInDifferentScopesKeepTheirOwnPortGeometry) {
    const auto rootNetwork = viewerController.rootNetworkId();
    const auto rootNode = viewerController.createGraphNode(rootNetwork, "constcolor", "Root source", 40, 40, {}, {});
    auto child = std::make_shared<nemo::NetworkId>();
    ASSERT_TRUE(
        projectSession
            .submit(nemo::addNetworkCommand("Child", child), nemo::EditOptions{projectSession.revision(), "child"})
            .committed);
    const auto childNetwork = QString::number(*child);
    const auto childNode = viewerController.createGraphNode(childNetwork, "merge", "Child merge", 40, 40, {}, {});
    ASSERT_EQ(rootNode, childNode) << "The fixture must exercise equal network-local node identities";
    auto instance = std::make_shared<nemo::NetworkInstanceId>();
    ASSERT_TRUE(projectSession
                    .submit(nemo::addInstanceCommand(rootNetwork.toULongLong(), *child, "Child", instance),
                            nemo::EditOptions{projectSession.revision(), "instance"})
                    .committed);
    const auto subnet = QString::number(projectSession.document().instance(*instance)->node);
    ASSERT_TRUE(viewerController.commitGraphMove(rootNetwork,
                                                 QVariantList{QVariantMap{{"id", subnet}, {"x", 300}, {"y", 120}}}));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphEnterAffordance_" + subnet));
    QTest::qWait(40);
    ASSERT_EQ(item("graphPanel")->property("graphNetworkId").toString(), childNetwork);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto rectangle = graphInteraction()->nodeRect(childNode);
    const auto input = graphInteraction()->portPosition(childNode, 0, false);
    EXPECT_LT(input.x(), rectangle.center().x());
    EXPECT_GT(graphInteraction()->portPosition(childNode, 1, false).x(), rectangle.center().x());
    EXPECT_LT(input.y(), rectangle.top()) << "Input arrows protrude above their owning card";
}

TEST_F(WorkspaceDragTest, GraphDragPreviewCancellationAndGroupOffsets) {
    const auto network = viewerController.rootNetworkId();
    const auto a = viewerController.createGraphNode(network, "constcolor", "A", 40, 40, {}, {});
    const auto b = viewerController.createGraphNode(network, "constcolor", "B", 220, 40, {}, {});
    ASSERT_FALSE(a.isEmpty());
    ASSERT_FALSE(b.isEmpty());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto point = [&](const QString& id) {
        return graph->mapToScene(graphInteraction()->nodeRect(id).center()).toPoint();
    };
    zoomGraphIn(point(source), 1.5);
    const auto before = viewerController.graphNodes();
    const auto revision = projectSession.revision();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(a));
    EXPECT_EQ(projectSession.revision(), revision);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(a));
    QTest::mouseMove(window, point(a) + QPoint(27, 19), 20);
    EXPECT_EQ(viewerController.graphNodes(), before) << "Drag is presentation-only until release";
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton);
    QTest::qWait(20);
    EXPECT_EQ(viewerController.graphNodes(), before);
    EXPECT_EQ(projectSession.revision(), revision);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(a));
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, point(b));
    const auto offset = graphInteraction()->nodeRect(b).topLeft() - graphInteraction()->nodeRect(a).topLeft();
    const auto from = point(a);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(window, from + QPoint(31, 23), 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, from + QPoint(31, 23));
    QTest::qWait(30);
    EXPECT_NE(viewerController.graphNodes(), before);
    EXPECT_EQ(graphInteraction()->nodeRect(b).topLeft() - graphInteraction()->nodeRect(a).topLeft(), offset);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(viewerController.graphNodes(), before) << "One undo restores the entire group";
}

TEST_F(WorkspaceDragTest, GraphPipePullRetainsRoutesOnCancelAndDisconnectsOnRelease) {
    const auto network = viewerController.rootNetworkId();
    const auto a = viewerController.createGraphNode(network, "constcolor", "A", 40, 0, {}, {});
    const auto b = viewerController.createGraphNode(network, "merge", "B", 40, 240, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, a, 0, b, 0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto edge = viewerController.graphEdges().first().toMap();
    const auto edgeId = edge.value("id").toString();
    zoomGraphIn(graph->mapToScene(graphInteraction()->nodeRect(a).center()).toPoint(), 1.5);
    const auto start = graphInteraction()->portPosition(a, 0, true);
    const auto end = graphInteraction()->portPosition(b, 0, false);
    const auto pipe = graph->mapToScene((start + end) / 2).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::AltModifier, pipe);
    QTest::qWait(30);
    const auto routed = viewerController.graphEdges();
    ASSERT_EQ(routed.first().toMap().value("route").toList().size(), 1);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_TRUE(viewerController.graphEdges().first().toMap().value("route").toList().isEmpty());
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(viewerController.graphEdges(), routed);
    const auto body = graph->mapToScene(start * 0.7 + end * 0.3).toPoint();
    auto* surface = item("graphCanvasSurface");
    const auto empty = surface->mapToScene(QPointF(surface->width() - 15, surface->height() - 15)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, body);
    QTest::mouseMove(window, empty, 20);
    EXPECT_EQ(viewerController.graphEdges(), routed);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, empty);
    EXPECT_EQ(viewerController.graphEdges(), routed);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, body);
    QTest::mouseMove(window, empty, 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, empty);
    QTest::qWait(30);
    EXPECT_TRUE(viewerController.graphEdges().isEmpty());
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(viewerController.graphEdges(), routed);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("id").toString(), edgeId);
}

TEST_F(WorkspaceDragTest, GraphPipePullLocksSourceOrDestinationOnPress) {
    const auto network = viewerController.rootNetworkId();
    const auto a = viewerController.createGraphNode(network, "constcolor", "A", 40, 0, {}, {});
    const auto b = viewerController.createGraphNode(network, "merge", "B", 40, 240, {}, {});
    const auto c = viewerController.createGraphNode(network, "constcolor", "C", 240, 0, {}, {});
    const auto d = viewerController.createGraphNode(network, "merge", "D", 240, 240, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, a, 0, b, 0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(30);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto start = graphInteraction()->portPosition(a, 0, true);
    const auto end = graphInteraction()->portPosition(b, 0, false);
    const auto body = graph->mapToScene((start + end) / 2).toPoint();
    const auto sourceDrop = graph->mapToScene(graphInteraction()->portPosition(c, 0, true)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, body);
    QTest::mouseMove(window, sourceDrop, 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, sourceDrop);
    QTest::qWait(20);
    ASSERT_EQ(viewerController.graphEdges().size(), 1);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("fromNode").toString(), c);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("toNode").toString(), b);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(20);
    const auto destinationDrop = graph->mapToScene(graphInteraction()->portPosition(d, 0, false)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::ShiftModifier, body);
    QTest::mouseMove(window, destinationDrop, 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, destinationDrop);
    QTest::qWait(20);
    ASSERT_EQ(viewerController.graphEdges().size(), 1);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("fromNode").toString(), a);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("toNode").toString(), d);
}

TEST_F(WorkspaceDragTest, GraphSearchCreatesImmediatelyAndProtectsSelectionWhileEditingText) {
    const auto network = viewerController.rootNetworkId();
    const auto a = viewerController.createGraphNode(network, "constcolor", "Selected", 40, 40, {}, {});
    ASSERT_FALSE(a.isEmpty());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(30);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      graph->mapToScene(graphInteraction()->nodeRect(a).center()).toPoint());
    const auto before = viewerController.graphNodes();
    QTest::keyClick(window, Qt::Key_Tab);
    QTest::qWait(30);
    auto* field = item("graphSearchField");
    field->setProperty("text", "Merge");
    QTest::keyClick(window, Qt::Key_Home);
    QTest::keyClick(window, Qt::Key_Delete);
    EXPECT_EQ(viewerController.graphNodes(), before) << "Delete in search edits text, not selected nodes";
    field->setProperty("text", "Merge");
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(30);
    ASSERT_EQ(viewerController.graphNodes().size(), before.size() + 1);
    const auto created = viewerController.graphNodes().last().toMap();
    EXPECT_EQ(created.value("type").toString(), "merge");
    ASSERT_EQ(viewerController.graphEdges().size(), 1);
    const auto edge = viewerController.graphEdges().first().toMap();
    EXPECT_EQ(edge.value("fromNode").toString(), a);
    EXPECT_EQ(edge.value("toNode").toString(), created.value("id").toString());
    ASSERT_TRUE(projectSession.undo(nemo::EditOptions{projectSession.revision(), {}}).committed);
    EXPECT_EQ(viewerController.graphNodes(), before);
    EXPECT_TRUE(viewerController.graphEdges().isEmpty());
}

TEST_F(WorkspaceDragTest, SubnetNavigationShowsTypedTerminalsAndRestoresScopedView) {
    const auto network = viewerController.rootNetworkId();
    const auto external = viewerController.createGraphNode(network, "constcolor", "External", -180, 0, {}, {});
    const auto source = viewerController.createGraphNode(network, "constcolor", "Source", 0, 0, {}, {});
    const auto merge = viewerController.createGraphNode(network, "merge", "Merge", 0, 180, {}, {});
    ASSERT_FALSE(external.isEmpty());
    ASSERT_FALSE(source.isEmpty());
    ASSERT_FALSE(merge.isEmpty());

    auto rootNodes = viewerController.graphSnapshot(network).value(QStringLiteral("nodes")).toList();
    QString output;
    for (const auto& value : rootNodes) {
        const auto node = value.toMap();
        if (node.value(QStringLiteral("type")).toString() == QStringLiteral("output") &&
            !node.value(QStringLiteral("deletable")).toBool()) {
            output = node.value(QStringLiteral("id")).toString();
            break;
        }
    }
    ASSERT_FALSE(output.isEmpty());
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, external, 0, merge, 1));
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, source, 0, merge, 0));
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, merge, 0, output, 0));

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto point = [&](const QString& id) {
        return graph->mapToScene(graphInteraction()->nodeRect(id).center()).toPoint();
    };
    zoomGraphIn(point(source), 1.5);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(source));
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, point(merge));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(merge));
    QTest::qWait(20);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphCollapseSelection"));
    QTest::qWait(60);

    QString subnet;
    QString definition;
    for (const auto& value : viewerController.graphNodes()) {
        const auto node = value.toMap();
        if (!node.value(QStringLiteral("definition")).toString().isEmpty() &&
            !node.value(QStringLiteral("instance")).toString().isEmpty()) {
            subnet = node.value(QStringLiteral("id")).toString();
            definition = node.value(QStringLiteral("definition")).toString();
            break;
        }
    }
    ASSERT_FALSE(subnet.isEmpty());
    ASSERT_FALSE(definition.isEmpty());
    auto* panel = item("graphPanel");
    ASSERT_EQ(panel->property("graphNetworkId").toString(), network);
    ASSERT_TRUE(item("graphEnterAffordance_" + subnet)->isVisible());

    graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto rootRect = graphInteraction()->nodeRect(subnet);
    const auto rootScreen = graph->mapToScene(rootRect.topLeft());
    const auto revision = projectSession.revision();
    const auto enter = center("graphEnterAffordance_" + subnet);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, enter);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network);
    const auto outside = item("graphSurface")->mapToScene(QPointF(-12, -12)).toPoint();
    QTest::mouseMove(window, outside, 10);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, outside);
    QTest::qWait(40);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network);
    EXPECT_EQ(projectSession.revision(), revision);

    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, enter);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, enter);
    QTest::qWait(30);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network);
    EXPECT_EQ(projectSession.revision(), revision);

    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, enter);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network)
        << "navigation commits only when the affordance press is released";
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, enter);
    QTest::qWait(60);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), definition);
    ASSERT_TRUE(item("graphBreadcrumb_" + definition)->isVisible());
    EXPECT_EQ(projectSession.revision(), revision);

    graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    // The records the network projects, read from the controller's own
    // projection rather than from the paint item, which no longer publishes them.
    const auto childNodes = viewerController.graphSnapshot(definition).value(QStringLiteral("nodes")).toList();
    const auto input = prefixedNodeRecord(childNodes, QStringLiteral("input:"));
    const auto formalOutput = prefixedNodeRecord(childNodes, QStringLiteral("output:"));
    ASSERT_FALSE(input.isEmpty()) << "formal input must be projected as a visible graph record";
    ASSERT_FALSE(formalOutput.isEmpty()) << "formal output must be projected as a visible graph record";
    const auto inputPort = input.value(QStringLiteral("outputs")).toList();
    const auto outputPort = formalOutput.value(QStringLiteral("inputs")).toList();
    ASSERT_EQ(inputPort.size(), 1);
    ASSERT_EQ(outputPort.size(), 1);
    EXPECT_EQ(inputPort.front().toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("image"));
    EXPECT_EQ(outputPort.front().toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("image"));
    const auto inputId = input.value(QStringLiteral("id")).toString();
    const auto outputId = formalOutput.value(QStringLiteral("id")).toString();
    const auto inputRect = graphInteraction()->nodeRect(inputId);
    const auto outputRect = graphInteraction()->nodeRect(outputId);
    const auto surfaceRect = item("graphSurface");
    const auto inputTopLeft = surfaceRect->mapFromScene(graph->mapToScene(inputRect.topLeft()));
    const auto inputBottomRight = surfaceRect->mapFromScene(graph->mapToScene(inputRect.bottomRight()));
    const auto outputTopLeft = surfaceRect->mapFromScene(graph->mapToScene(outputRect.topLeft()));
    const auto outputBottomRight = surfaceRect->mapFromScene(graph->mapToScene(outputRect.bottomRight()));
    EXPECT_GT(inputRect.width(), 0);
    EXPECT_GT(inputRect.height(), 0);
    EXPECT_GT(outputRect.width(), 0);
    EXPECT_GT(outputRect.height(), 0);
    const QRectF viewport(0, 0, surfaceRect->width(), surfaceRect->height());
    EXPECT_TRUE(viewport.contains(inputTopLeft));
    EXPECT_TRUE(viewport.contains(inputBottomRight));
    EXPECT_TRUE(viewport.contains(outputTopLeft));
    EXPECT_TRUE(viewport.contains(outputBottomRight));
    bool inputWire = false;
    bool outputWire = false;
    for (const auto& value : viewerController.graphSnapshot(definition).value(QStringLiteral("edges")).toList()) {
        const auto edge = value.toMap();
        const auto id = edge.value(QStringLiteral("id")).toString();
        if (id.startsWith(QStringLiteral("input:"))) {
            inputWire = true;
            EXPECT_EQ(edge.value(QStringLiteral("fromNode")).toString(), inputId);
            EXPECT_FALSE(edge.value(QStringLiteral("toNode")).toString().isEmpty());
        } else if (id.startsWith(QStringLiteral("output:"))) {
            outputWire = true;
            EXPECT_FALSE(edge.value(QStringLiteral("fromNode")).toString().isEmpty());
            EXPECT_EQ(edge.value(QStringLiteral("toNode")).toString(), outputId);
        }
    }
    EXPECT_TRUE(inputWire);
    EXPECT_TRUE(outputWire);

    QString inner;
    for (const auto& value : childNodes) {
        const auto node = value.toMap();
        const auto id = node.value(QStringLiteral("id")).toString();
        if (!id.startsWith(QStringLiteral("input:")) && !id.startsWith(QStringLiteral("output:")) && id != output &&
            node.value(QStringLiteral("type")).toString() != QStringLiteral("output")) {
            inner = id;
            break;
        }
    }
    ASSERT_FALSE(inner.isEmpty());
    const auto childPoint = graph->mapToScene(graphInteraction()->nodeRect(inner).center()).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, childPoint);
    QTest::qWait(20);
    const auto childSelection = graphInteraction()->selectedNodeIds();
    ASSERT_EQ(childSelection.size(), 1);
    const auto childRect = graphInteraction()->nodeRect(inner);
    const auto childScreen = graph->mapToScene(childRect.topLeft());
    const auto childSurface = item("graphSurface");
    const auto panStart = childSurface->mapToScene(QPointF(80, 80)).toPoint();
    QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, panStart);
    QTest::mouseMove(window, panStart + QPoint(31, 19), 20);
    QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, panStart + QPoint(31, 19));
    const auto pannedChildRect = graphInteraction()->nodeRect(inner);
    const auto pannedChildScreen = graph->mapToScene(pannedChildRect.topLeft());
    EXPECT_NE(pannedChildScreen, childScreen);

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphBreadcrumb_" + network));
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network);
    ASSERT_TRUE(item("graphBreadcrumb_" + network)->isVisible());
    const auto rootSelection = graphInteraction()->selectedNodeIds();
    ASSERT_EQ(rootSelection.size(), 1);
    EXPECT_EQ(graph->mapToScene(graphInteraction()->nodeRect(subnet).topLeft()), rootScreen);

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphEnterAffordance_" + subnet));
    EXPECT_EQ(panel->property("graphNetworkId").toString(), definition);
    graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto reenteredSelection = graphInteraction()->selectedNodeIds();
    ASSERT_EQ(reenteredSelection.size(), 1);
    EXPECT_EQ(reenteredSelection.front(), inner);
    EXPECT_EQ(graph->mapToScene(graphInteraction()->nodeRect(inner).topLeft()), pannedChildScreen);
}

TEST_F(WorkspaceDragTest, RemovingActiveSubnetUnwindsToParentAndKeepsSharedDefinition) {
    const auto network = viewerController.rootNetworkId();
    const auto source = viewerController.createGraphNode(network, "constcolor", "PathSource", 0, 0, {}, {});
    const auto merge = viewerController.createGraphNode(network, "merge", "PathMerge", 0, 160, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, source, 0, merge, 0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    auto point = [&](const QString& id) {
        return graph->mapToScene(graphInteraction()->nodeRect(id).center()).toPoint();
    };
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(source));
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, point(merge));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(merge));
    QTest::qWait(20);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphCollapseSelection"));
    QTest::qWait(60);

    QString subnet;
    QString definition;
    QString instance;
    for (const auto& value : viewerController.graphNodes()) {
        const auto node = value.toMap();
        if (!node.value(QStringLiteral("instance")).toString().isEmpty()) {
            subnet = node.value(QStringLiteral("id")).toString();
            definition = node.value(QStringLiteral("definition")).toString();
            instance = node.value(QStringLiteral("instance")).toString();
            break;
        }
    }
    ASSERT_FALSE(subnet.isEmpty());
    ASSERT_FALSE(definition.isEmpty());
    ASSERT_FALSE(instance.isEmpty());
    auto sibling = std::make_shared<nemo::NetworkInstanceId>();
    ASSERT_TRUE(
        projectSession
            .submit(nemo::addInstanceCommand(network.toULongLong(), definition.toULongLong(), "SharedSibling", sibling),
                    nemo::EditOptions{projectSession.revision(), "shared-sibling"})
            .committed);
    ASSERT_TRUE(projectSession
                    .submit(nemo::setLayoutCommand(network.toULongLong(),
                                                   projectSession.document().instance(*sibling)->node, {300, 0}),
                            nemo::EditOptions{projectSession.revision(), "place-shared-sibling"})
                    .committed);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(30);

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphEnterAffordance_" + subnet));
    QTest::qWait(60);
    auto* panel = item("graphPanel");
    EXPECT_EQ(panel->property("graphNetworkId").toString(), definition);
    const auto revision = projectSession.revision();
    ASSERT_TRUE(viewerController.deleteGraphNodes(network, QVariantList{subnet}));
    QTest::qWait(80);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network);
    EXPECT_TRUE(viewerController.graphSnapshot(definition).value(QStringLiteral("available")).toBool());
    EXPECT_EQ(projectSession.document().instances().size(), 1U);
    EXPECT_GT(projectSession.revision(), revision);

    ASSERT_TRUE(projectSession.undo(nemo::EditOptions{projectSession.revision(), {}}).committed);
    QTest::qWait(60);
    EXPECT_EQ(panel->property("graphNetworkId").toString(), network);
    EXPECT_TRUE(viewerController.graphSnapshot(definition).value(QStringLiteral("available")).toBool());
    bool restored = false;
    for (const auto& value : viewerController.graphNodes())
        restored = restored || value.toMap().value(QStringLiteral("id")).toString() == subnet;
    EXPECT_TRUE(restored);
}

TEST_F(WorkspaceDragTest, CreatingWorkspaceThroughDialogActivatesAnIndependentCopy) {
    const auto original = controller.activeWorkspaceId();
    controller.setGroup(source, QStringLiteral("D"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("addWorkspaceButton"));
    QTest::qWait(30);
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(60);
    ASSERT_NE(controller.activeWorkspaceId(), original);
    const auto copied = snapshot();
    const auto& copiedPanel = copied["children"][0]["children"][0]["panels"][0];
    EXPECT_EQ(copiedPanel["group"], "D");
    const auto copiedId = QString::fromStdString(copiedPanel["id"]);
    EXPECT_NE(copiedId, source);
    controller.setGroup(copiedId, QStringLiteral("E"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("workspaceTab_" + original));
    QTest::qWait(30);
    EXPECT_EQ(controller.activeWorkspaceId(), original);
    EXPECT_EQ(snapshot()["children"][0]["children"][0]["panels"][0]["group"], "D");
}

TEST_F(WorkspaceDragTest, SubnetParameterPopoutExposesEditsAndReordersRows) {
    const auto network = viewerController.rootNetworkId();
    const auto source = viewerController.createGraphNode(network, "constcolor", "ExposeSource", 0, 0, {}, {});
    const auto merge = viewerController.createGraphNode(network, "merge", "ExposeMerge", 0, 160, {}, {});
    ASSERT_FALSE(source.isEmpty());
    ASSERT_FALSE(merge.isEmpty());
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, source, 0, merge, 0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto point = [&](const QString& id) {
        return graph->mapToScene(graphInteraction()->nodeRect(id).center()).toPoint();
    };
    zoomGraphIn(point(source), 1.5);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(source));
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, point(merge));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(merge));
    QTest::qWait(20);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphCollapseSelection"));
    QTest::qWait(60);

    QString subnet;
    QString definition;
    for (const auto& value : viewerController.graphNodes()) {
        const auto node = value.toMap();
        if (!node.value(QStringLiteral("instance")).toString().isEmpty()) {
            subnet = node.value(QStringLiteral("id")).toString();
            definition = node.value(QStringLiteral("definition")).toString();
            break;
        }
    }
    ASSERT_FALSE(subnet.isEmpty());
    ASSERT_FALSE(definition.isEmpty());

    graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(subnet));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(subnet));
    QTest::qWait(20);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphEditExposedParameters"));
    QTest::qWait(40);
    auto* popup = window->findChild<QQuickWindow*>(QStringLiteral("subnetParametersWindow"));
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(popup->isVisible());
    const auto editorItem = [popup](const QString& name) { return visual(popup->contentItem(), name); };
    const auto editorCenter = [&editorItem](const QString& name) {
        auto* found = editorItem(name);
        return found->mapToScene(QPointF(found->width() / 2, found->height() / 2)).toPoint();
    };
    EXPECT_TRUE(editorItem("subnetParametersHint")->isVisible());

    // Author an exposure through the searchable picker, not a direct call to
    // the controller that bypasses the real authoring UI.
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, editorCenter("subnetAddParameter"));
    auto* search = editorItem("subnetParameterSearch");
    ASSERT_NE(search, nullptr);
    search->setProperty("text", QStringLiteral("ExposeSource.color"));
    QTest::qWait(40);
    auto* candidates = editorItem("subnetParameterCandidates");
    ASSERT_NE(candidates, nullptr);
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, candidates->mapToScene(QPointF(60, 14)).toPoint());
    QTest::qWait(40);
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, editorCenter("subnetAddParameter"));
    QTest::qWait(40);
    const auto rows = viewerController.subnetExposure(network, subnet).value(QStringLiteral("rows")).toList();
    ASSERT_EQ(rows.size(), 1);
    const auto exposedId = rows.first().toMap().value(QStringLiteral("id")).toString();
    auto* label = editorItem("subnetExposedLabel_" + exposedId);
    ASSERT_NE(label, nullptr);

    // Renaming the exposed label is one undoable command.
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, editorCenter("subnetExposedLabel_" + exposedId));
    label->setProperty("text", QStringLiteral("Tint"));
    QTest::keyClick(popup, Qt::Key_Return);
    QTest::qWait(40);
    EXPECT_EQ(viewerController.subnetExposure(network, subnet)
                  .value(QStringLiteral("rows"))
                  .toList()
                  .first()
                  .toMap()
                  .value(QStringLiteral("name"))
                  .toString(),
              QStringLiteral("Tint"));
    ASSERT_TRUE(projectSession.undo(nemo::EditOptions{projectSession.revision(), {}}).committed);
    QTest::qWait(40);
    EXPECT_EQ(viewerController.subnetExposure(network, subnet)
                  .value(QStringLiteral("rows"))
                  .toList()
                  .first()
                  .toMap()
                  .value(QStringLiteral("name"))
                  .toString(),
              rows.first().toMap().value(QStringLiteral("name")).toString());

    // Removing the exposure leaves the definition parameter authored value and
    // the row disappears; the popout stays open with its hint.
    const auto* definitionNode =
        projectSession.document().network(definition.toULongLong()).graph().node(source.toULongLong());
    ASSERT_NE(definitionNode, nullptr);
    const auto authored = definitionNode->params.count("color");

    // Exercise the cross-window MIME receiver with native Qt drag/drop events.
    // Cancellation and wrong-scope/duplicate drops must not author anything.
    auto* dropArea = editorItem("subnetParametersDropArea");
    ASSERT_NE(dropArea, nullptr);
    auto dropPoint = dropArea->mapToScene(QPointF(30, 65));
    QMimeData mime;
    const auto setPayload = [&](const QString& scope) {
        mime.setData("application/x-nemo-parameter",
                     QJsonDocument(QJsonObject{{"networkId", scope}, {"nodeId", merge}, {"parameterKey", "operation"}})
                         .toJson(QJsonDocument::Compact));
    };
    const auto enterDrop = [&](Qt::DropActions actions = Qt::CopyAction) {
        QDragEnterEvent enter(dropPoint.toPoint(), actions, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(popup, &enter);
        return enter.isAccepted();
    };
    setPayload(network);
    EXPECT_FALSE(enterDrop());
    QDragLeaveEvent rejectedLeave;
    QCoreApplication::sendEvent(popup, &rejectedLeave);
    EXPECT_EQ(viewerController.subnetExposure(network, subnet).value("rows").toList().size(), 1);
    setPayload(definition);
    ASSERT_TRUE(enterDrop()) << popup->property("message").toString().toStdString();
    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(popup, &leave);
    EXPECT_EQ(viewerController.subnetExposure(network, subnet).value("rows").toList().size(), 1);
    ASSERT_TRUE(enterDrop()) << popup->property("message").toString().toStdString();
    QDropEvent drop(dropPoint, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(popup, &drop);
    EXPECT_TRUE(drop.isAccepted());
    QTest::qWait(40);
    const auto order = [&] {
        QVariantList ids;
        for (const auto& value :
             viewerController.subnetExposure(network, subnet).value(QStringLiteral("rows")).toList())
            ids.push_back(value.toMap().value(QStringLiteral("id")));
        return ids;
    };
    ASSERT_EQ(order().size(), 2);
    EXPECT_FALSE(enterDrop());
    QCoreApplication::sendEvent(popup, &rejectedLeave);
    EXPECT_EQ(order().size(), 2);
    const auto firstId = order().at(0).toString();
    // Reorder through the same window's native MIME receiver, not its command
    // function. Insertion below the last row moves the first control to the end.
    mime.clear();
    mime.setData(
        "application/x-nemo-exposure",
        QJsonDocument(QJsonObject{{"networkId", definition}, {"exposureId", firstId}}).toJson(QJsonDocument::Compact));
    dropPoint = dropArea->mapToScene(QPointF(30, dropArea->height() - 12));
    ASSERT_TRUE(enterDrop(Qt::MoveAction));
    QDropEvent reorder(dropPoint, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(popup, &reorder);
    EXPECT_TRUE(reorder.isAccepted());
    QTest::qWait(60);
    EXPECT_EQ(order().at(1).toString(), firstId);
    ASSERT_TRUE(projectSession.undo(nemo::EditOptions{projectSession.revision(), {}}).committed);
    QTest::qWait(40);
    EXPECT_EQ(order().at(0).toString(), firstId);

    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, editorCenter("subnetExposedRemove_" + firstId));
    QTest::qWait(40);
    ASSERT_EQ(order().size(), 1);
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                      editorCenter("subnetExposedRemove_" + order().at(0).toString()));
    QTest::qWait(40);
    EXPECT_TRUE(order().isEmpty());
    EXPECT_TRUE(popup->isVisible());
    EXPECT_TRUE(editorItem("subnetParametersHint")->isVisible());
    const auto* afterRemove =
        projectSession.document().network(definition.toULongLong()).graph().node(source.toULongLong());
    ASSERT_NE(afterRemove, nullptr);
    EXPECT_EQ(afterRemove->params.count("color"), authored);

    // Close the tool window before exercising the graph context menu.
    popup->close();
    QTest::qWait(40);
    EXPECT_FALSE(popup->isVisible());

    // The context menu duplicates a linked occurrence and detaches only the
    // selected one; the occurrence query reports the shared then local state.
    ASSERT_TRUE(viewerController.promoteParameter(definition, source, "color", ""));
    QTest::qWait(40);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(subnet));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(subnet));
    QTest::qWait(20);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphDuplicateLinked"));
    QTest::qWait(60);
    EXPECT_EQ(viewerController.subnetExposure(network, subnet).value(QStringLiteral("linkState")).toString(),
              QStringLiteral("shared"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(subnet));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(subnet));
    QTest::qWait(20);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphMakeIndependent"));
    QTest::qWait(60);
    EXPECT_EQ(viewerController.subnetExposure(network, subnet).value(QStringLiteral("linkState")).toString(),
              QStringLiteral("local"));
}

TEST(WorkspaceControllerTest, WorkspaceOrderMovementValidatesAndPersists) {
    QTemporaryDir directory;
    const QString path = directory.filePath("workspace.json");
    nemo::workspace::WorkspaceController controller(path);
    const QString first = controller.activeWorkspaceId();
    const QString second = controller.createWorkspace(QStringLiteral("Second"));
    const QString third = controller.createWorkspace(QStringLiteral("Third"));
    ASSERT_FALSE(second.isEmpty());
    ASSERT_FALSE(third.isEmpty());

    ASSERT_TRUE(controller.moveWorkspace(third, -2));
    ASSERT_EQ(controller.workspaces().size(), 3);
    EXPECT_EQ(controller.workspaces().at(0).toMap().value("id").toString(), third);
    EXPECT_EQ(controller.workspaces().at(1).toMap().value("id").toString(), first);
    EXPECT_EQ(controller.workspaces().at(2).toMap().value("id").toString(), second);

    const auto movedOrder = controller.workspaces();
    EXPECT_FALSE(controller.moveWorkspace(QStringLiteral("missing"), 1));
    EXPECT_FALSE(controller.moveWorkspace(third, -1));
    EXPECT_EQ(controller.workspaces(), movedOrder);
    EXPECT_FALSE(controller.error().isEmpty());

    ASSERT_TRUE(controller.save());
    nemo::workspace::WorkspaceController restored(path);
    ASSERT_EQ(restored.workspaces().size(), 3);
    EXPECT_EQ(restored.workspaces().at(0).toMap().value("id").toString(), third);
    EXPECT_EQ(restored.workspaces().at(1).toMap().value("id").toString(), first);
    EXPECT_EQ(restored.workspaces().at(2).toMap().value("id").toString(), second);
}

TEST(WorkspaceControllerTest, CategoryResetDoesNotResetOtherAppearanceSettings) {
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController controller(directory.filePath("workspace.json"));
    ASSERT_TRUE(controller.setAppearancePreset(QStringLiteral("Paper")));
    ASSERT_TRUE(controller.setAccentOverride(QStringLiteral("#123456")));
    const auto originalColors = controller.categoryColors();
    ASSERT_TRUE(controller.setCategoryColor(QStringLiteral("Compositing"), QStringLiteral("#abcdef")));

    controller.resetCategoryColors();

    EXPECT_EQ(controller.appearancePreset(), QStringLiteral("Paper"));
    EXPECT_EQ(controller.accentOverride(), QStringLiteral("#123456"));
    EXPECT_EQ(controller.categoryColors(), originalColors);
}

TEST(WorkspaceControllerTest, CategoryUpgradePreservesCustomColorsAndWorkspace) {
    QTemporaryDir directory;
    const auto path = directory.filePath("workspace.json");
    nemo::workspace::WorkspaceController original(path);
    const auto defaults = original.categoryColors();
    ASSERT_TRUE(original.setAppearancePreset("Paper"));
    const auto copy = original.duplicateWorkspace(original.activeWorkspaceId(), "Color work");
    ASSERT_TRUE(original.switchWorkspace(copy));
    auto saved = original.projectPresentation();
    saved["version"] = 2;
    saved["appearance"]["categoryColors"] = {{"Merge", "#abcdef"},   {"Filter", "#a96832"},  {"IO", "#123456"},
                                             {"Distort", "#54816b"}, {"Utility", "#59646f"}, {"Color", "#71608c"}};
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(saved.dump()));
    }
    nemo::workspace::WorkspaceController restored(path);
    ASSERT_TRUE(restored.error().isEmpty()) << restored.error().toStdString();
    EXPECT_EQ(restored.root(), original.root());
    EXPECT_EQ(restored.activeWorkspaceId(), copy);
    EXPECT_EQ(restored.appearancePreset(), original.appearancePreset());
    auto expected = defaults;
    expected.insert("Compositing", "#abcdef");
    expected.insert("I/O", "#123456");
    EXPECT_EQ(restored.categoryColors(), expected);
    ASSERT_TRUE(restored.save());
    nemo::workspace::WorkspaceController reopened(path);
    EXPECT_EQ(reopened.categoryColors(), expected);
    EXPECT_EQ(reopened.workspaces(), original.workspaces());
    EXPECT_EQ(reopened.activeWorkspaceId(), copy);
}

TEST_F(WorkspaceDragTest, ResetLayoutPreservesWorkspaceTabsAndAppearance) {
    const auto original = controller.activeWorkspaceId();
    controller.setGroup(source, QStringLiteral("D"));
    const auto scratch = controller.createWorkspace(QStringLiteral("Scratch"));
    ASSERT_TRUE(controller.switchWorkspace(scratch));
    ASSERT_TRUE(controller.setAppearancePreset(QStringLiteral("Paper")));
    ASSERT_TRUE(controller.setAccentOverride(QStringLiteral("#123456")));
    const auto initialLayout = snapshot();
    controller.setRatio(QString::fromStdString(snapshot()["id"]), 0.5);
    const auto presets = controller.workspaces();
    QTest::keyClick(window, Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(40);
    EXPECT_EQ(snapshot(), initialLayout);
    EXPECT_EQ(controller.workspaces(), presets);
    EXPECT_EQ(controller.activeWorkspaceId(), scratch);
    EXPECT_EQ(controller.appearancePreset(), QStringLiteral("Paper"));
    EXPECT_EQ(controller.accentOverride(), QStringLiteral("#123456"));
    ASSERT_TRUE(controller.switchWorkspace(original));
    EXPECT_EQ(snapshot()["children"][0]["children"][0]["panels"][0]["group"], "D");
}

TEST(WorkspaceControllerTest, PresetsAppearanceAndIndependentLayoutsPersistTogether) {
    QTemporaryDir directory;
    const QString path = directory.filePath("workspace.json");
    nemo::workspace::WorkspaceController controller(path);
    const auto panelId = [](const nemo::workspace::WorkspaceController& value) {
        const Json root = Json::parse(QJsonDocument::fromVariant(value.root()).toJson().toStdString());
        return QString::fromStdString(root["children"][0]["children"][0]["panels"][0]["id"]);
    };

    const QString first = controller.activeWorkspaceId();
    controller.setGroup(panelId(controller), QStringLiteral("C"));
    const QString scratch = controller.createWorkspace(QStringLiteral("Scratch"));
    ASSERT_TRUE(controller.switchWorkspace(scratch));
    controller.setGroup(panelId(controller), QStringLiteral("D"));
    const QString copy = controller.duplicateWorkspace(scratch, QStringLiteral("Scratch Copy"));
    ASSERT_TRUE(controller.switchWorkspace(copy));
    ASSERT_TRUE(controller.setAppearancePreset(QStringLiteral("Paper")));
    ASSERT_TRUE(controller.setAccentOverride(QStringLiteral("#123456")));
    ASSERT_TRUE(controller.save());

    nemo::workspace::WorkspaceController restored(path);
    EXPECT_EQ(restored.workspaces().size(), 3);
    EXPECT_EQ(restored.activeWorkspaceId(), copy);
    EXPECT_EQ(restored.appearancePreset(), QStringLiteral("Paper"));
    EXPECT_EQ(restored.accentOverride(), QStringLiteral("#123456"));
    ASSERT_TRUE(restored.switchWorkspace(first));
    EXPECT_EQ(Json::parse(QJsonDocument::fromVariant(restored.root()).toJson().toStdString())["children"][0]["children"]
                                                                                             [0]["panels"][0]["group"],
              "C");
    ASSERT_TRUE(restored.switchWorkspace(scratch));
    EXPECT_EQ(Json::parse(QJsonDocument::fromVariant(restored.root()).toJson().toStdString())["children"][0]["children"]
                                                                                             [0]["panels"][0]["group"],
              "D");
}

TEST(WorkspaceControllerTest, UnreadableWorkspaceIsPreservedUntilExplicitReset) {
    QTemporaryDir directory;
    const QString path = directory.filePath("workspace.json");
    const QByteArray invalid = QByteArrayLiteral("{not workspace json");
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(invalid), invalid.size());
    }

    nemo::workspace::WorkspaceController controller(path);
    EXPECT_FALSE(controller.error().isEmpty());
    const auto restoreError = controller.error();
    controller.registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"), QStringLiteral("ViewerPanel.qml"));
    ASSERT_TRUE(controller.setAppearancePreset(QStringLiteral("Paper")));
    EXPECT_EQ(controller.error(), restoreError);
    EXPECT_FALSE(controller.save());
    QFile preserved(path);
    ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
    EXPECT_EQ(preserved.readAll(), invalid);
    preserved.close();

    controller.reset();
    ASSERT_TRUE(controller.save());
    ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
    EXPECT_NE(preserved.readAll(), invalid);
}

TEST(WorkspaceControllerTest, PanelStateWriteNotifiesItsOwnerWithoutRebuildingTheLayout) {
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController controller(directory.filePath("workspace.json"));
    const QString panelId = [](const nemo::workspace::WorkspaceController& value) {
        const Json root = Json::parse(QJsonDocument::fromVariant(value.root()).toJson().toStdString());
        return QString::fromStdString(root["children"][0]["children"][0]["panels"][0]["id"]);
    }(controller);
    ASSERT_FALSE(panelId.isEmpty());
    const auto limitOf = [&] { return controller.panelState(panelId).value(QStringLiteral("limit")).toInt(); };

    QStringList notified;
    int roots = 0;
    int presentations = 0;
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::panelStateChanged, &controller,
                     [&](const QString& changed) { notified.append(changed); });
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::rootChanged, &controller, [&] { ++roots; });
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::presentationChanged, &controller,
                     [&] { ++presentations; });

    controller.setPanelState(panelId, QVariantMap{{QStringLiteral("limit"), 3}});

    // The owner is told; the layout is not re-delivered, because re-delivering
    // it makes every panel rebuild its display model per state write.
    EXPECT_EQ(notified, QStringList{panelId});
    EXPECT_EQ(roots, 0);
    // Persistence still observes the write, and the record still travels in the
    // layout payload the project stores.
    EXPECT_EQ(presentations, 1);
    EXPECT_EQ(limitOf(), 3);
    const Json root = Json::parse(QJsonDocument::fromVariant(controller.root()).toJson().toStdString());
    EXPECT_EQ(root["children"][0]["children"][0]["panels"][0]["state"]["limit"].get<int>(), 3);
}

TEST(WorkspaceControllerTest, NestedRootDeliveryIsCoalescedWithoutDroppingTheNestedChange) {
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController controller(directory.filePath("workspace.json"));
    const auto groupOf = [&] {
        const Json root = Json::parse(QJsonDocument::fromVariant(controller.root()).toJson().toStdString());
        return QString::fromStdString(root["children"][0]["children"][0]["panels"][0]["group"]);
    };
    const QString panelId = [&] {
        const Json root = Json::parse(QJsonDocument::fromVariant(controller.root()).toJson().toStdString());
        return QString::fromStdString(root["children"][0]["children"][0]["panels"][0]["id"]);
    }();
    ASSERT_FALSE(panelId.isEmpty());

    bool delivering = false;
    bool reentered = false;
    QStringList observed;
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::rootChanged, &controller, [&] {
        reentered = reentered || delivering;
        delivering = true;
        observed.append(groupOf());
        // An arrangement edit requested while the root notification is still
        // being delivered — a panel persisting during identity change or
        // teardown does exactly this.
        if (observed.last() != QStringLiteral("C"))
            controller.setGroup(panelId, QStringLiteral("C"));
        delivering = false;
    });

    controller.setGroup(panelId, QStringLiteral("B"));

    // Everything above runs without spinning an event loop: the coalesced
    // delivery is synchronous, before this call returns.
    // A delivery must never run inside another delivery: that re-notifies a QML
    // binding that is still updating and Qt reports it as a binding loop.
    EXPECT_FALSE(reentered);
    // The nested edit is not dropped: the delivery that follows the outer one
    // carries the latest snapshot, and the final arrangement keeps the edit.
    EXPECT_EQ(observed, (QStringList{QStringLiteral("B"), QStringLiteral("C")}));
    EXPECT_EQ(groupOf(), QStringLiteral("C"));
}

// Issue #100 rebuilt the graph panel on the interaction core; the cost
// contract is asserted as counts. Responsiveness is exactly one hit-test pass
// per pointer move. A gesture rasterises and uploads no label atlas and writes
// no panel state while it is active; a settled gesture records exactly one
// command and exactly one panel-state write; a cancelled gesture records
// nothing at all, and a marquee is not an edit.
TEST_F(WorkspaceDragTest, GraphGestureCostBudget) {
    const auto network = viewerController.rootNetworkId();
    const auto a = viewerController.createGraphNode(network, "constcolor", "A", 40, 40, {}, {});
    const auto b = viewerController.createGraphNode(network, "merge", "B", 40, 240, {}, {});
    ASSERT_TRUE(viewerController.connectOrReplaceGraph(network, a, 0, b, 0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* panel = item("graphPanel");
    const auto panelId = panel->property("panelId").toString();
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    auto* interaction = graphInteraction();
    ASSERT_NE(interaction, nullptr);

    int writes = 0;
    // A local context object: the connection must not outlive this body, or a
    // state write from panel teardown would call into dead locals.
    QObject writesScope;
    QObject::connect(&controller, &nemo::workspace::WorkspaceController::panelStateChanged, &writesScope,
                     [&](const QString& changed) {
                         if (changed == panelId)
                             ++writes;
                     });

    // A rendered frame first: the atlas identity is the label set, so this is
    // the cost the scene needs before any gesture starts.
    QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
    window->update();
    ASSERT_TRUE(rendered.wait(1000));
    // Frame all can leave the cards smaller than the screen-space port radius
    // (the root network carries a delivery card far from the two fixture nodes,
    // so the framed zoom is the 0.2 floor). At that size a card is 5.6 px tall
    // and the guarded middle band a card press must land in is sub-pixel, so an
    // integer window coordinate can acquire the card's own port instead. Wheel
    // in until a card is comfortably larger than the port radius, then let the
    // burst settle, so the gesture below is aimed at the card it names.
    zoomGraphIn(graph->mapToScene(interaction->nodeRect(a).center()).toPoint(), 1.0);

    // A rendered frame at the settled view: the atlas identity is the label set,
    // so this is the cost the scene needs before any gesture starts.
    rendered.clear();
    window->update();
    ASSERT_TRUE(rendered.wait(1000));
    const auto rasterizations = graph->labelAtlasesRasterized();
    const auto revision = projectSession.revision();
    const auto document = projectSession.snapshot();
    const auto undoLabel = std::string(projectSession.undoLabel());
    const int writesBefore = writes;

    // The pointer is established inside the surface before anything is counted:
    // entering the surface and the press each resolve their own target, and a
    // synthetic event on the first move must not be read as gesture work.
    const QPoint press = graph->mapToScene(interaction->nodeRect(a).center()).toPoint();
    QTest::mouseMove(window, press);
    QTest::qWait(20);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, press);
    const auto movesBefore = interaction->hitTestPasses();

    // Phase 1: a settled node drag. One hit-test pass per move, and no cost of
    // any other kind while the gesture is active.
    const int moves = 6;
    QJsonArray passesPerMove;
    for (int step = 1; step <= moves; ++step) {
        const auto passes = interaction->hitTestPasses();
        QTest::mouseMove(window, press + QPoint(9 * step, 6 * step), 2);
        passesPerMove.append(static_cast<qint64>(interaction->hitTestPasses() - passes));
        EXPECT_EQ(interaction->hitTestPasses(), movesBefore + static_cast<qulonglong>(step))
            << "one pointer move is exactly one hit-test pass";
        EXPECT_EQ(graph->labelAtlasesRasterized(), rasterizations)
            << "a gesture must neither rasterise nor upload a label atlas";
        EXPECT_EQ(projectSession.revision(), revision);
        EXPECT_EQ(writes, writesBefore) << "a drag must not write the workspace mid-gesture";
        EXPECT_TRUE(nemo::documentContentEquals(projectSession.document(), document))
            << "a drag is presentation-only until release";
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, press + QPoint(9 * moves, 6 * moves));
    QTest::qWait(40);
    EXPECT_EQ(projectSession.revision(), revision + 1) << "one gesture records exactly one command";
    EXPECT_EQ(writes, writesBefore + 1) << "one gesture settles into exactly one panel-state write";
    EXPECT_FALSE(nemo::documentContentEquals(projectSession.document(), document));

    // Phase 2: a cancelled gesture records nothing. Its release is the one write
    // boundary the replaced panel always had, so only the edit and the history
    // entry are pinned here.
    const auto cancelledRevision = projectSession.revision();
    const auto cancelledDocument = projectSession.snapshot();
    const auto cancelledLabel = std::string(projectSession.undoLabel());
    const QPoint held = press + QPoint(9 * moves, 6 * moves);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, held);
    QTest::mouseMove(window, held + QPoint(40, 30), 2);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, held + QPoint(40, 30));
    QTest::qWait(40);
    EXPECT_EQ(projectSession.revision(), cancelledRevision);
    EXPECT_TRUE(nemo::documentContentEquals(projectSession.document(), cancelledDocument))
        << "a cancelled gesture leaves the network exactly as it was";
    EXPECT_EQ(std::string(projectSession.undoLabel()), cancelledLabel)
        << "a cancelled gesture records no history entry";

    // Phase 3: a marquee is not an edit; it publishes the union of the nodes it
    // touched and the selection the gesture started with. The box is drawn from
    // clear of one node card to clear of the other, so it touches exactly the
    // two nodes and not the default card the root network carries.
    const auto marqueeRevision = projectSession.revision();
    const QRectF firstCard = graph->mapRectToScene(interaction->nodeRect(a));
    const QRectF secondCard = graph->mapRectToScene(interaction->nodeRect(b));
    const QPoint corner = (firstCard.topLeft() + QPointF(-8, -8)).toPoint();
    const QPoint empty = (secondCard.bottomRight() + QPointF(8, 8)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::ShiftModifier, empty);
    // Stepped motion, as the drag phase uses: one move helper call can be
    // compressed away by the platform's event loop, and a marquee that never
    // crossed its threshold publishes the base selection unchanged.
    for (int step = 1; step <= 4; ++step)
        QTest::mouseMove(window, empty + (corner - empty) * step / 4, 2);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::ShiftModifier, corner);
    QTest::qWait(40);
    EXPECT_EQ(projectSession.revision(), marqueeRevision) << "a marquee is not an edit";
    EXPECT_EQ(interaction->selectedNodeIds(), (QStringList{a, b}));

    // Phase 4: a wire pull costs one pass per move as well. The session reads
    // the same ordered pass the hover publishes instead of resolving a second
    // pick, so the count below is the whole cost of the gesture and a regression
    // to per-session re-querying shows up here.
    const QPoint wireStart = graph->mapToScene(interaction->portPosition(a, 0, true)).toPoint();
    const auto wireRevision = projectSession.revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, wireStart);
    const auto wireMovesBefore = interaction->hitTestPasses();
    for (int step = 1; step <= 4; ++step) {
        QTest::mouseMove(window, wireStart + (empty - wireStart) * step / 4, 2);
        EXPECT_EQ(interaction->hitTestPasses(), wireMovesBefore + static_cast<qulonglong>(step))
            << "a wire pull resolves exactly one hit-test pass per move";
        EXPECT_EQ(projectSession.revision(), wireRevision) << "a pull commits nothing until it is released";
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, empty);
    QTest::qWait(40);
    EXPECT_EQ(projectSession.revision(), wireRevision) << "a pull released on empty canvas changes no topology";

    recordEvidence(QStringLiteral("node-style"),
                   QJsonObject{{QStringLiteral("rasterizations"), static_cast<qint64>(rasterizations)},
                               {QStringLiteral("hit_test_passes"),
                                static_cast<qint64>(interaction->hitTestPasses() - movesBefore)},
                               {QStringLiteral("hit_test_passes_per_move"), passesPerMove},
                               {QStringLiteral("commands"), static_cast<qint64>(projectSession.revision() - revision)},
                               {QStringLiteral("panel_writes"), writes},
                               {QStringLiteral("selection_size"), interaction->selectedNodeIds().size()}});
}
// Story 5 of issue #100 — "a drag costs the same whether the network has ten
// nodes or a thousand" — as counts. The painter splits its vertices by change
// rate: the cards and their ports are a function of the network, the theme and
// the selection, and the pipes, the cards a gesture moves and the port under
// the pointer are rebuilt per frame. With sixty cards on screen, a drag step
// must build one card — not the network — and must not rebuild the static group
// after the move that takes the dragged card out of it.
TEST_F(WorkspaceDragTest, GraphFrameCostFollowsWhatMoves) {
    const auto network = viewerController.rootNetworkId();
    QStringList cards;
    for (int index = 0; index < 60; ++index) {
        cards.append(viewerController.createGraphNode(network, "constcolor", QStringLiteral("card%1").arg(index),
                                                      (index % 12) * 140, (index / 12) * 44, {}, {}));
    }
    ASSERT_EQ(cards.size(), 60);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphFrameAll"));
    QTest::qWait(40);
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    auto* interaction = graphInteraction();

    // The transient budget distinguishes one moved card from the dense static
    // group. Aim at a middle card: Frame All can leave the first card offscreen
    // at the minimum zoom on a narrow native window, where wheels hit a sibling
    // panel instead of the graph.
    const QString dragged = cards.at(cards.size() / 2);
    zoomGraphIn(graph->mapToScene(interaction->nodeRect(dragged).center()).toPoint(), 1.0);
    const auto painted = [this] {
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        window->update();
        return rendered.wait(1000);
    };
    // Selection is a content change, separate from the drag's transient work.
    const QPoint press = graph->mapToScene(interaction->nodeRect(dragged).center()).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, press);
    ASSERT_EQ(interaction->selectedNodeIds(), QStringList{dragged});
    ASSERT_TRUE(painted()) << "a frame must be painted before the gesture starts";
    const auto staticBefore = graph->staticGeometryRebuilds();
    const auto rastersBefore = graph->labelAtlasesRasterized();

    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, press);
    // The pointer is in place before anything is counted: the press resolves its
    // own target, and the count below is the moves'.
    const auto hitTestsBefore = interaction->hitTestPasses();
    const int steps = 6;
    for (int step = 1; step <= steps; ++step) {
        QTest::mouseMove(window, press + QPoint(7 * step, 4 * step), 2);
        ASSERT_TRUE(painted()) << "a drag step must reach a painted frame";
        EXPECT_LE(graph->staticGeometryRebuilds() - staticBefore, 1u)
            << "a drag step rebuilds no static geometry: the group is rebuilt once, when the dragged "
               "card leaves it, and not once per move";
        EXPECT_EQ(graph->labelAtlasesRasterized(), rastersBefore)
            << "a drag neither rasterises nor uploads a label atlas";
        EXPECT_GT(graph->transientVerticesBuilt(), 0u) << "the moved card must actually be drawn";
        EXPECT_LT(graph->transientVerticesBuilt(), 2000u)
            << "a drag step builds the card it moves, not the cards around it";
        EXPECT_EQ(interaction->hitTestPasses(), hitTestsBefore + static_cast<qulonglong>(step))
            << "one pointer move is still exactly one hit-test pass";
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, press + QPoint(7 * steps, 4 * steps));
    QTest::qWait(40);
    EXPECT_EQ(graph->staticGeometryRebuilds() - staticBefore, 2u)
        << "one rebuild takes the dragged card out of the static group and one puts it back";
    recordEvidence(
        QStringLiteral("frame-cost"),
        QJsonObject{
            {QStringLiteral("cards"), cards.size()},
            {QStringLiteral("drag_steps"), steps},
            {QStringLiteral("static_rebuilds"), static_cast<qint64>(graph->staticGeometryRebuilds() - staticBefore)},
            {QStringLiteral("transient_vertices"), static_cast<qint64>(graph->transientVerticesBuilt())},
            {QStringLiteral("label_atlases"), static_cast<qint64>(graph->labelAtlasesRasterized() - rastersBefore)}});
}

}  // namespace

int main(int argc, char** argv) {
    const bool nativeUi = qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") == 1;
    if (!nativeUi)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // The viewer destination scenario adopts the application-owned Vulkan
    // device on the real window (ViewerRuntime::attachToWindow), so that run
    // asks Qt for the Vulkan scene graph instead of OpenGL. CI keeps the
    // offscreen/Null configuration and the test skips without the opt-in.
    const bool viewerWindow = qEnvironmentVariableIntValue("NEMO_TEST_VIEWER_WINDOW") == 1;
    // Keep CI isolated even when the desktop exports a QPA fallback list.
    // Native acceptance opts in and uses the requested system platform.
    QQuickWindow::setSceneGraphBackend(QStringLiteral("rhi"));
    QQuickWindow::setGraphicsApi(nativeUi ? (viewerWindow ? QSGRendererInterface::Vulkan : QSGRendererInterface::OpenGL)
                                          : QSGRendererInterface::Null);
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterType<nemo::ui::ViewerItem>("Nemo", 1, 0, "ViewerItem");
    qmlRegisterType<nemo::ui::GraphItem>("Nemo", 1, 0, "GraphItem");
    // The graph panel instantiates its interaction core in QML; the harness
    // loads the panels as source files, so it registers the same types the
    // application's generated module registration publishes.
    qmlRegisterType<nemo::ui::GraphInteraction>("Nemo", 1, 0, "GraphInteraction");
    qmlRegisterType<nemo::ui::TimelineItem>("Nemo", 1, 0, "TimelineItem");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
