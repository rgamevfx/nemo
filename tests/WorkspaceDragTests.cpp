#include "GraphItem.hpp"
#include "PanelContextRouter.hpp"
#include "TimelineItem.hpp"
#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

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

class WorkspaceDragTest : public testing::Test {
protected:
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController controller{directory.filePath("workspace.json")};
    nemo::ui::ViewerRuntime viewerRuntime;
    nemo::ProjectSession projectSession;
    nemo::ui::PanelContextRouter panelContextRouter{projectSession};
    // Context bindings persist through the same workspace presentation state.
    // Rendering and document ownership remain in their existing objects.
    nemo::ui::ViewerController viewerController{&viewerRuntime, projectSession};
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
        engine.rootContext()->setContextProperty("panelContextRouter", &panelContextRouter);
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
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("toolCategory_Utility"));
    QTest::qWait(30);
    const auto before = viewerController.graphNodes().size();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("toolNode_constcolor"));
    QTest::qWait(30);
    ASSERT_EQ(viewerController.graphNodes().size(), before + 1);
    EXPECT_EQ(viewerController.graphNodes().last().toMap().value("type").toString(), "constcolor");
    viewerController.undo();
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
    const auto point = [&](const QString& id) { return graph->mapToScene(graph->nodeRect(id).center()).toPoint(); };
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
    const auto offset = graph->nodeRect(b).topLeft() - graph->nodeRect(a).topLeft();
    const auto from = point(a);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(window, from + QPoint(31, 23), 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, from + QPoint(31, 23));
    QTest::qWait(30);
    EXPECT_NE(viewerController.graphNodes(), before);
    EXPECT_EQ(graph->nodeRect(b).topLeft() - graph->nodeRect(a).topLeft(), offset);
    ASSERT_TRUE(viewerController.undo());
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
    const auto start = graph->portPosition(a, 0, true);
    const auto end = graph->portPosition(b, 0, false);
    const auto pipe = graph->mapToScene((start + end) / 2).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::AltModifier, pipe);
    QTest::qWait(30);
    const auto routed = viewerController.graphEdges();
    ASSERT_EQ(routed.first().toMap().value("route").toList().size(), 1);
    const auto body = graph->mapToScene(start * 0.7 + end * 0.3).toPoint();
    auto* surface = item("graphCanvasSurface");
    const auto empty = surface->mapToScene(QPointF(surface->width() - 15, surface->height() - 15)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, body);
    QTest::mouseMove(window, empty, 20);
    EXPECT_EQ(viewerController.graphEdges(), routed);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, empty);
    EXPECT_EQ(viewerController.graphEdges(), routed);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, body);
    QTest::mouseMove(window, empty, 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, empty);
    QTest::qWait(30);
    EXPECT_TRUE(viewerController.graphEdges().isEmpty());
    ASSERT_TRUE(viewerController.undo());
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
    const auto body =
        graph->mapToScene((graph->portPosition(a, 0, true) + graph->portPosition(b, 0, false)) / 2).toPoint();
    const auto sourceDrop = graph->mapToScene(graph->portPosition(c, 0, true)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, body);
    QTest::mouseMove(window, sourceDrop, 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, sourceDrop);
    QTest::qWait(20);
    ASSERT_EQ(viewerController.graphEdges().size(), 1);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("fromNode").toString(), c);
    EXPECT_EQ(viewerController.graphEdges().first().toMap().value("toNode").toString(), b);
    ASSERT_TRUE(viewerController.undo());
    QTest::qWait(20);
    const auto destinationDrop = graph->mapToScene(graph->portPosition(d, 0, false)).toPoint();
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
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, graph->mapToScene(graph->nodeRect(a).center()).toPoint());
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
    ASSERT_TRUE(viewerController.undo());
    EXPECT_EQ(viewerController.graphNodes(), before);
    EXPECT_TRUE(viewerController.graphEdges().isEmpty());
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
    ASSERT_TRUE(controller.setCategoryColor(QStringLiteral("Merge"), QStringLiteral("#abcdef")));

    controller.resetCategoryColors();

    EXPECT_EQ(controller.appearancePreset(), QStringLiteral("Paper"));
    EXPECT_EQ(controller.accentOverride(), QStringLiteral("#123456"));
    EXPECT_EQ(controller.categoryColors().value("Merge").toString(), QStringLiteral("#60656b"));
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
}  // namespace

int main(int argc, char** argv) {
    const bool nativeUi = qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") == 1;
    if (!nativeUi)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Keep CI isolated even when the desktop exports a QPA fallback list.
    // Native acceptance opts in and uses the requested system platform.
    QQuickWindow::setSceneGraphBackend(QStringLiteral("rhi"));
    QQuickWindow::setGraphicsApi(nativeUi ? QSGRendererInterface::OpenGL : QSGRendererInterface::Null);
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterType<nemo::ui::ViewerItem>("Nemo", 1, 0, "ViewerItem");
    qmlRegisterType<nemo::ui::GraphItem>("Nemo", 1, 0, "GraphItem");
    qmlRegisterType<nemo::ui::TimelineItem>("Nemo", 1, 0, "TimelineItem");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
