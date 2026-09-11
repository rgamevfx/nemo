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

#include <string>
#include <variant>

#include <stdexcept>

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
nemo::Graph& rootGraph(nemo::Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const nemo::Graph& rootGraph(const nemo::Document& document) {
    return document.network(document.rootNetworkId()).graph();
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
    EXPECT_NEAR(item("dockPreview")->width(), (leaf->width() - 4) / 2, 1);
    release(right);
    const auto split = snapshot();
    const auto* moved = containing(split, source.toStdString());
    const auto* remaining = containing(split, other.toStdString());
    ASSERT_NE(moved, nullptr);
    ASSERT_NE(remaining, nullptr);
    EXPECT_NE(moved->at("id"), remaining->at("id"));
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
    ASSERT_LT(narrow->width(), 244);
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
    EXPECT_GE(upper->height(), 80);
    EXPECT_GE(lower->height(), 80);
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
TEST_F(WorkspaceDragTest, InteractiveGraphAndTimelineUseCommandsWithIndependentClocks) {
    viewerController.openSource("/tmp/nemo-interactive-command-source.mkv");
    QTest::qWait(30);
    auto* name = item("graphAddName");
    name->forceActiveFocus();
    for (const char letter : std::string("extra"))
        QTest::keyClick(window, letter);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphAddButton"));
    QTest::qWait(30);
    const auto containsExtra = [&] {
        for (const auto& value : viewerController.graphNodes())
            if (value.toMap().value("name").toString() == "extra")
                return true;
        return false;
    };
    ASSERT_TRUE(containsExtra());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphUndo"));
    QTest::qWait(30);
    EXPECT_FALSE(containsExtra());

    auto* ruler = item("timelineRuler");
    const auto quarter = ruler->mapToScene(QPointF(ruler->width() / 4, 10)).toPoint();
    const auto beforeScrub = viewerController.frame();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, quarter);
    QTest::qWait(30);
    EXPECT_GT(viewerController.frame(), beforeScrub);
    const auto quarterFrame = viewerController.frame();
    const auto middle = ruler->mapToScene(QPointF(ruler->width() / 2, 10)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, quarter);
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(window, quarter + (middle - quarter) * step / 8, 2);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, middle);
    QTest::qWait(30);
    EXPECT_GT(viewerController.frame(), quarterFrame);
    EXPECT_EQ(item("graphPlayhead")->property("value").toInt(), 0);
    auto* slider = item("graphPlayhead");
    slider->forceActiveFocus();
    const auto graphBefore = slider->property("value").toInt();
    const auto previousPosition = ruler->property("playheadPosition").toReal();
    QTest::keyClick(window, Qt::Key_Right);
    QTest::qWait(30);
    EXPECT_GT(slider->property("value").toInt(), graphBefore);
    EXPECT_EQ(viewerController.frame(), slider->property("value").toInt());
    EXPECT_EQ(ruler->property("playheadPosition").toReal(), previousPosition);
    const auto slip = center("timelineSlipPlus");
    ASSERT_TRUE(QRect(QPoint(), window->size()).contains(slip)) << "Source timing controls must fit the tiled panel";
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, slip);
    QTest::qWait(30);
    EXPECT_EQ(viewerController.timelineClips().first().toMap().value("offset").toLongLong(), 1);
}

TEST_F(WorkspaceDragTest, RenamedNodeEditsReachBothPanelsAndUndoByIdentity) {
    constexpr nemo::NodeId highId = (nemo::NodeId{1} << 53) + 1;
    ASSERT_TRUE(projectSession
                    .submit(nemo::Command{"restore sparse identity",
                                          [](nemo::Document& document) {
                                              static_cast<void>(
                                                  rootGraph(document).addNodeWithId(highId, "testpattern", "source"));
                                          }},
                            {projectSession.revision()})
                    .committed);
    nemo::ui::ViewerRuntime secondRuntime;
    nemo::ui::ViewerController second(&secondRuntime, projectSession);
    const auto id = rootGraph(projectSession.document()).nodeByName("source")->id;
    const auto renamed =
        projectSession.submit(nemo::renameNodeCommand(projectSession.document().rootNetworkId(), id, "renamed"),
                              {projectSession.revision(), "rename-source"});
    ASSERT_TRUE(renamed.committed);
    viewerController.addGraphNode("testpattern", "source");
    QTest::qWait(30);
    const auto nodes = viewerController.graphNodes();
    int selected = -1;
    for (int index = 0; index < nodes.size(); ++index)
        if (nodes[index].toMap().value("id").toString() == QString::number(id))
            selected = index;
    ASSERT_GE(selected, 0);
    item("graphParameterNode")->setProperty("currentIndex", selected);
    for (const auto& field : {std::pair{"graphParameterKey", "note"}, std::pair{"graphParameterValue", "shared"}}) {
        item(field.first)->forceActiveFocus();
        for (const char letter : std::string(field.second))
            QTest::keyClick(window, letter);
    }
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphParameterApply"));
    ASSERT_TRUE(viewerController.error().isEmpty()) << viewerController.error().toStdString();
    ASSERT_TRUE(rootGraph(projectSession.document()).node(id)->params.contains("note"));
    ASSERT_EQ(std::get<std::string>(rootGraph(projectSession.document()).node(id)->params.at("note")), "shared");
    EXPECT_EQ(viewerController.graphNodes(), second.graphNodes());
    EXPECT_EQ(rootGraph(projectSession.document()).nodeByName("source")->params.count("note"), 0u);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center("graphUndo"));
    QTest::qWait(30);
    EXPECT_EQ(rootGraph(projectSession.document()).node(id)->params.count("note"), 0u);
    EXPECT_EQ(viewerController.graphNodes(), second.graphNodes());
    EXPECT_EQ(rootGraph(projectSession.document()).node(id)->name, "renamed");
}

TEST_F(WorkspaceDragTest, RegistryThemeAndWorkspacePresetsAreConsumerVisible) {
    const auto builtins = controller.panelTypes();
    ASSERT_EQ(builtins.size(), 3);
    EXPECT_EQ(controller.panelDescriptor(QStringLiteral("viewer")).value("title").toString(), "Viewer");
    EXPECT_EQ(controller.panelDescriptor(QStringLiteral("nodegraph")).value("title").toString(), "Nodegraph");

    controller.registerPanelType(QStringLiteral("extension-test"), QStringLiteral("Extension Test"),
                                 QStringLiteral("UnavailablePanel.qml"), QString());
    EXPECT_EQ(controller.panelTypes().size(), 4);
    EXPECT_EQ(controller.panelDescriptor(QStringLiteral("extension-test")).value("typeId").toString(),
              "extension-test");
    EXPECT_NE(item("workspaceSelector"), nullptr);
    EXPECT_NE(item("appearancePreset"), nullptr);
    const auto duplicateViewer = controller.createPanel(target, QStringLiteral("viewer"), QStringLiteral("A"));
    ASSERT_FALSE(duplicateViewer.isEmpty());
    EXPECT_NE(duplicateViewer, source);
    EXPECT_NE(item("appearanceAccent"), nullptr);

    EXPECT_TRUE(controller.setAppearancePreset(QStringLiteral("Paper")));
    EXPECT_EQ(controller.appearancePreset(), "Paper");
    EXPECT_FALSE(controller.setAppearancePreset(QStringLiteral("Unknown")));
    EXPECT_EQ(controller.appearancePreset(), "Paper");
    EXPECT_FALSE(controller.setAccentOverride(QStringLiteral("not-a-color")));
    EXPECT_TRUE(controller.accentOverride().isEmpty());
    EXPECT_TRUE(controller.setAccentOverride(QStringLiteral("#123456")));
    EXPECT_EQ(controller.accentOverride(), "#123456");
    EXPECT_FALSE(controller.setCategoryColor(QStringLiteral("Merge"), QStringLiteral("#xyzxyz")));
    EXPECT_TRUE(controller.setCategoryColor(QStringLiteral("Merge"), QStringLiteral("#60656b")));
    EXPECT_EQ(controller.categoryColors().value("Merge").toString(), "#60656b");
    controller.resetAppearance();
    EXPECT_EQ(controller.appearancePreset(), "Graphite");
    EXPECT_TRUE(controller.accentOverride().isEmpty());

    const auto firstId = controller.activeWorkspaceId();
    const auto createdId = controller.createWorkspace(QStringLiteral("Scratch"));
    ASSERT_FALSE(createdId.isEmpty());
    EXPECT_EQ(controller.activeWorkspaceId(), firstId);
    ASSERT_TRUE(controller.switchWorkspace(createdId));
    EXPECT_EQ(controller.activeWorkspaceId(), createdId);
    const auto duplicateId = controller.duplicateWorkspace(createdId, QStringLiteral("Scratch Copy"));
    ASSERT_FALSE(duplicateId.isEmpty());
    ASSERT_TRUE(controller.switchWorkspace(duplicateId));
    EXPECT_EQ(controller.activeWorkspaceId(), duplicateId);
    ASSERT_TRUE(controller.closeWorkspace(duplicateId));
    EXPECT_NE(controller.activeWorkspaceId(), duplicateId);
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
