#include "GraphInteraction.hpp"
#include "GraphItem.hpp"
#include "HistoryController.hpp"
#include "MediaLibraryModel.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ParameterInteraction.hpp"
#include "ProjectFileController.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

namespace {
QQuickItem* historyItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = historyItem(child, name))
            return found;
    return nullptr;
}
class HistorySurface : public testing::Test {
protected:
    QTemporaryDir directory;
    nemo::ProjectSession session;
    nemo::ui::HistoryController history{session};
    nemo::workspace::WorkspaceController workspace{directory.filePath("workspace.json")};
    nemo::ui::ViewerRuntime runtime;
    // One presentation interaction per project (issue #102): every controller
    // for this session shares it, and it outlives them.
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller{&runtime, session, interaction};
    nemo::ui::PanelContextRouter router{session};
    nemo::ui::NativeFileChooser chooser;
    nemo::ui::ProjectFileController file{session, workspace, router, chooser};
    nemo::ui::ParameterEditorRegistry editors;
    nemo::media::MediaImportService importer;
    nemo::ui::MediaLibraryModel media{session, importer, &router, &workspace};
    QQmlApplicationEngine engine;
    QSignalSpy warnings{&engine, &QQmlEngine::warnings};
    QQuickWindow* window{};

    void SetUp() override {
        for (const auto& entry : std::vector<std::pair<QString, QString>>{{"viewer", "ViewerPanel.qml"},
                                                                          {"nodegraph", "GraphPanel.qml"},
                                                                          {"parameters", "ParametersPanel.qml"},
                                                                          {"animation", "AnimationPanel.qml"},
                                                                          {"timeline", "TimelinePanel.qml"}})
            workspace.registerPanelType(entry.first, entry.first, entry.second);
        router.setWorkspaceController(&workspace);
        engine.rootContext()->setContextProperty("workspace", &workspace);
        engine.rootContext()->setContextProperty("viewerController", &controller);
        engine.rootContext()->setContextProperty("panelContextRouter", &router);
        workspace.registerPanelType("media", "Media Bin", "MediaBinPanel.qml");
        engine.rootContext()->setContextProperty("projectFile", &file);
        engine.rootContext()->setContextProperty("parameterEditors", &editors);
        engine.rootContext()->setContextProperty("mediaLibrary", &media);
        engine.rootContext()->setContextProperty("historyController", &history);
        engine.load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine.rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        ASSERT_NE(window, nullptr);
        window->show();
        window->requestActivate();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
        QTest::qWait(100);
        ASSERT_TRUE(window->isActive());
    }
    void TearDown() override {
        if (window) {
            window->setProperty("closeOverride", true);
            window->close();
        }
        EXPECT_EQ(warnings.count(), 0);
    }
    QQuickItem* item(const QString& name, QQuickWindow* host = nullptr) {
        return historyItem((host ? host : window)->contentItem(), name);
    }
    // The interaction core the graph panel drives; the paint item is a consumer
    // of it, so geometry, selection and hover are asked of the core.
    nemo::ui::GraphInteraction* graphInteraction() {
        auto* found = window->findChild<QObject*>(QStringLiteral("graphInteraction"));
        EXPECT_NE(found, nullptr) << "the graph panel must own its interaction core";
        return qobject_cast<nemo::ui::GraphInteraction*>(found);
    }
    QPoint center(QQuickItem* target) {
        return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
    }
    void click(const QString& name, QQuickWindow* host = nullptr) {
        auto* target = item(name, host);
        ASSERT_NE(target, nullptr) << name.toStdString();
        QTest::mouseClick(host ? host : window, Qt::LeftButton, Qt::NoModifier, center(target));
        QTest::qWait(30);
    }
    void key(bool redo = false, QQuickWindow* host = nullptr) {
        QTest::keyClick(host ? host : window, Qt::Key_Z,
                        redo ? Qt::ControlModifier | Qt::ShiftModifier : Qt::ControlModifier);
        QTest::qWait(30);
    }
    void text(const QString& value) {
        for (auto character : value)
            QTest::keyClick(window, character.toLatin1());
    }
    void graphFocus() { click("graphCanvasSurface"); }
    void movePressed(QPointF point) {
        QMouseEvent event(QEvent::MouseMove, point, window->mapToGlobal(point.toPoint()), Qt::NoButton, Qt::LeftButton,
                          Qt::NoModifier);
        QGuiApplication::sendEvent(window, &event);
    }
    void moveHeld(QPoint from, QPoint to) {
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 6; ++i) {
            const QPointF point = QPointF(from) + QPointF(to - from) * (i / 6.0);
            movePressed(point);
        }
        QTest::qWait(20);
    }
    QString create(const QString& type = "Constant") {
        graphFocus();
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::qWait(30);
        text(type);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(50);
        const auto selection = graphInteraction()->selectedNodeIds();
        return selection.isEmpty() ? QString{} : selection.front();
    }
    void capture(const QString& name, QQuickWindow* host = nullptr) {
        const auto output = qEnvironmentVariable("NEMO_HISTORY_CAPTURE_DIR");
        if (output.isEmpty())
            return;
        QDir().mkpath(output);
        QTest::qWait(80);
        const auto image = (host ? host : window)->grabWindow();
        ASSERT_FALSE(image.isNull());
        EXPECT_TRUE(image.save(output + '/' + name + ".png"));
    }
};

TEST_F(HistorySurface, GraphKeyboardAndEditMenuRestoreOneChronologicalTransition) {
    graphFocus();
    EXPECT_FALSE(history.canUndo());
    click("editMenuButton");
    ASSERT_NE(item("editUndoAction"), nullptr);
    EXPECT_FALSE(item("editUndoAction")->isEnabled());
    capture("empty-menu");
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(30);
    const auto node = create();
    ASSERT_FALSE(node.isEmpty());
    ASSERT_FALSE(controller.hasSource());
    const auto authored = session.snapshot();
    const auto revision = session.revision();
    key();
    EXPECT_EQ(session.revision(), revision + 1);
    EXPECT_EQ(session.document().network(session.document().rootNetworkId()).graph().node(node.toULongLong()), nullptr);
    click("editMenuButton");
    capture("redo-create-menu");
    click("editRedoAction");
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), authored));
    EXPECT_EQ(session.revision(), revision + 2);

    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto start = graph->mapToScene(graphInteraction()->nodeRect(node).center()).toPoint();
    moveHeld(start, start + QPoint(45, 20));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(45, 20));
    QTest::qWait(40);
    ASSERT_FALSE(nemo::documentContentEquals(session.document(), authored));
    key();
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), authored));
    key(true);
    const auto moved = session.snapshot();
    QTest::keyClick(window, Qt::Key_Delete);
    QTest::qWait(40);
    EXPECT_EQ(session.document().network(session.document().rootNetworkId()).graph().node(node.toULongLong()), nullptr);
    key();
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), moved));
    capture("graph-restored");
}

TEST_F(HistorySurface, ExhaustedTextHistoryNeverFallsThroughKeyboardOrMenu) {
    ASSERT_FALSE(create().isEmpty());
    const auto revision = session.revision();
    QTest::keyClick(window, Qt::Key_Tab);
    QTest::qWait(30);
    auto* input = item("graphSearchField");
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->hasActiveFocus());
    text("blur");
    key();
    EXPECT_EQ(input->property("text").toString(), "");
    key();
    EXPECT_EQ(session.revision(), revision);
    click("editMenuButton");
    ASSERT_NE(item("editUndoAction"), nullptr);
    EXPECT_FALSE(item("editUndoAction")->isEnabled());
    EXPECT_TRUE(item("editRedoAction")->isEnabled());
    capture("text-exhausted-menu");
    click("editRedoAction");
    EXPECT_EQ(input->property("text").toString(), "blur");
    EXPECT_EQ(session.revision(), revision);
    click("editMenuButton");
    click("editUndoAction");
    EXPECT_EQ(input->property("text").toString(), "");
    EXPECT_EQ(session.revision(), revision);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(30);
    key();
    EXPECT_EQ(session.revision(), revision + 1);
}

TEST_F(HistorySurface, NumericTextMenuDoesNotCommitOrUndoDocument) {
    const auto node = create("transform");
    ASSERT_FALSE(node.isEmpty());
    auto* panel = item("parametersPanel");
    ASSERT_NE(panel, nullptr);
    ASSERT_TRUE(router.requestInspector(panel->property("panelGroup").toString(), controller.rootNetworkId(), node));
    QTest::qWait(50);
    click("param_" + node + "_translateX");
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    text("12.5");
    const auto revision = session.revision();
    auto* input = window->activeFocusItem();
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->property("text").toString(), "12.5");
    click("editMenuButton");
    EXPECT_EQ(session.revision(), revision) << "opening Edit must not commit numeric text";
    click("editUndoAction");
    EXPECT_NE(input->property("text").toString(), "12.5");
    EXPECT_EQ(session.revision(), revision);
    click("editMenuButton");
    click("editRedoAction");
    EXPECT_EQ(input->property("text").toString(), "12.5");
    EXPECT_EQ(session.revision(), revision);
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(30);
    EXPECT_EQ(session.revision(), revision + 1);
    EXPECT_EQ(session.queryValues(session.document().rootNetworkId(), node.toULongLong(), "translateX").front().value,
              nemo::ParameterValue{12.5});
    graphFocus();
    key();
    EXPECT_EQ(session.revision(), revision + 2);
    EXPECT_EQ(session.queryValues(session.document().rootNetworkId(), node.toULongLong(), "translateX").front().value,
              nemo::ParameterValue{0.0});
    capture("numeric-text-history");
}

TEST_F(HistorySurface, UndoCancelsGraphAndParameterPreviewAndReleaseCannotCommit) {
    const auto node = create("transform");
    ASSERT_FALSE(node.isEmpty());
    const auto before = session.snapshot();
    const auto revision = session.revision();
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    const auto originalRect = graphInteraction()->nodeRect(node);
    const auto start = graph->mapToScene(graphInteraction()->nodeRect(node).center()).toPoint();
    const auto end = start + QPoint(45, 20);
    moveHeld(start, end);
    ASSERT_NE(graphInteraction()->nodeRect(node), originalRect);
    EXPECT_FALSE(history.canRedo());
    key(true);
    EXPECT_EQ(session.revision(), revision);
    key();
    EXPECT_EQ(graphInteraction()->nodeRect(node), originalRect);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    QTest::qWait(30);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), before));

    auto* panel = item("parametersPanel");
    ASSERT_NE(panel, nullptr);
    ASSERT_TRUE(router.requestInspector(panel->property("panelGroup").toString(), controller.rootNetworkId(), node));
    QTest::qWait(50);
    auto* field = item("param_" + node + "_translateX");
    ASSERT_NE(field, nullptr);
    const auto displayedBefore = field->property("displayedText");
    const auto from = center(field);
    moveHeld(from, from + QPoint(35, 0));
    ASSERT_NE(field->property("displayedText"), displayedBefore);
    EXPECT_FALSE(history.canRedo());
    key(true);
    EXPECT_EQ(session.revision(), revision);
    key();
    EXPECT_EQ(field->property("displayedText"), displayedBefore);
    movePressed(from + QPoint(50, 0));
    QTest::qWait(20);
    EXPECT_EQ(field->property("displayedText"), displayedBefore);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, from + QPoint(35, 0));
    QTest::qWait(30);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), before));
    auto* columns = item("columnToggle");
    ASSERT_NE(columns, nullptr);
    if (columns->property("checked").toBool())
        click("columnToggle");
    QTest::qWait(30);
    auto* slider = item("slider_" + node + "_translateX");
    ASSERT_NE(slider, nullptr);
    ASSERT_TRUE(slider->isVisible());
    const auto sliderBefore = slider->property("displayValue");
    const auto sliderStart = center(slider);
    moveHeld(sliderStart, sliderStart + QPoint(30, 0));
    ASSERT_NE(slider->property("displayValue"), sliderBefore);
    EXPECT_FALSE(history.canRedo());
    key(true);
    EXPECT_EQ(session.revision(), revision);
    key();
    EXPECT_EQ(slider->property("displayValue"), sliderBefore);
    movePressed(sliderStart + QPoint(50, 0));
    QTest::qWait(20);
    EXPECT_EQ(slider->property("displayValue"), sliderBefore);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, sliderStart + QPoint(50, 0));
    QTest::qWait(30);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), before));
    moveHeld(sliderStart, sliderStart + QPoint(30, 0));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, sliderStart + QPoint(30, 0));
    QTest::qWait(30);
    EXPECT_EQ(session.revision(), revision + 1);
    EXPECT_NE(slider->property("displayValue"), sliderBefore);
    key();
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), before));
    capture("cancelled-previews");
    graphFocus();
    key();
    EXPECT_EQ(session.revision(), revision + 3);
    EXPECT_EQ(session.document().network(session.document().rootNetworkId()).graph().node(node.toULongLong()), nullptr);
}

TEST_F(HistorySurface, CompoundHistorySurvivesToolWindowReopeningAndScopedNavigation) {
    const auto node = create();
    ASSERT_FALSE(node.isEmpty());
    const auto network = controller.rootNetworkId();
    const auto beforeNodes = controller.graphNodes();
    const auto beforeEdges = controller.graphEdges();
    auto* graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
    ASSERT_NE(graph, nullptr);
    auto point = graph->mapToScene(graphInteraction()->nodeRect(node).center()).toPoint();
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point);
    QTest::qWait(30);
    click("graphCollapseSelection");
    ASSERT_EQ(session.document().instances().size(), 1U);
    const auto occurrence = session.document().instances().front();
    const auto subnet = QString::number(occurrence.node);
    const auto collapsed = session.snapshot();
    const auto collapsedRevision = session.revision();
    click("graphEnterAffordance_" + subnet);
    EXPECT_NE(item("graphPanel")->property("graphNetworkId").toString(), network);
    key();
    EXPECT_EQ(controller.graphNodes(), beforeNodes);
    EXPECT_EQ(controller.graphEdges(), beforeEdges);
    EXPECT_EQ(item("graphPanel")->property("graphNetworkId").toString(), network);
    EXPECT_EQ(session.revision(), collapsedRevision + 1);
    key(true);
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), collapsed));

    for (int iteration = 0; iteration < 3; ++iteration) {
        window->requestActivate();
        QTest::qWait(30);
        click("graphFrameAll");
        graph = qobject_cast<nemo::ui::GraphItem*>(item("graphItem"));
        point = graph->mapToScene(graphInteraction()->nodeRect(subnet).center()).toPoint();
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point);
        QTest::qWait(30);
        click("graphEditExposedParameters");
        auto* tool = window->findChild<QQuickWindow*>("subnetParametersWindow");
        ASSERT_NE(tool, nullptr);
        ASSERT_TRUE(tool->isVisible());
        ASSERT_TRUE(QTest::qWaitForWindowExposed(tool));
        tool->requestActivate();
        QTest::qWait(40);
        ASSERT_TRUE(tool->isActive());
        const auto revision = session.revision();
        click("editMenuButton", tool);
        if (iteration == 0)
            capture("tool-edit-menu", tool);
        click("editUndoAction", tool);
        EXPECT_EQ(session.revision(), revision + 1);
        EXPECT_EQ(controller.graphNodes(), beforeNodes);
        EXPECT_EQ(controller.graphEdges(), beforeEdges);
        key(true, tool);
        EXPECT_EQ(session.revision(), revision + 2);
        EXPECT_TRUE(nemo::documentContentEquals(session.document(), collapsed));
        tool->close();
        QTest::qWait(30);
    }
    QQuickWindow unrelated;
    unrelated.resize(200, 100);
    unrelated.show();
    unrelated.requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&unrelated));
    QTest::qWait(40);
    const auto revision = session.revision();
    key(false, &unrelated);
    EXPECT_EQ(session.revision(), revision);
    unrelated.close();
}

TEST_F(HistorySurface, InterleavedPanelEditsFollowDocumentChronologyAcrossFocus) {
    const auto node = create("transform");
    ASSERT_FALSE(node.isEmpty());
    auto* panel = item("parametersPanel");
    ASSERT_NE(panel, nullptr);
    ASSERT_TRUE(router.requestInspector(panel->property("panelGroup").toString(), controller.rootNetworkId(), node));
    QTest::qWait(40);
    click("param_" + node + "_translateX");
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    text("7");
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(40);
    auto* viewer = item("viewerPanel");
    ASSERT_NE(viewer, nullptr);
    workspace.setPanelType(viewer->property("panelId").toString(), "media");
    QTest::qWait(50);
    click("mediaAddBinButton");
    text("HistoryBin");
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(40);
    ASSERT_EQ(session.queryMediaBins().size(), 1U);
    const auto bin = session.queryMediaBins().front();
    const auto allEdits = session.snapshot();
    graphFocus();
    key();
    EXPECT_TRUE(session.queryMediaBins().empty());
    EXPECT_EQ(session.queryValues(session.document().rootNetworkId(), node.toULongLong(), "translateX").front().value,
              nemo::ParameterValue{7.0});
    key();
    EXPECT_EQ(session.queryValues(session.document().rootNetworkId(), node.toULongLong(), "translateX").front().value,
              nemo::ParameterValue{0.0});
    click("mediaTreeView");
    key(true);
    EXPECT_EQ(session.queryValues(session.document().rootNetworkId(), node.toULongLong(), "translateX").front().value,
              nemo::ParameterValue{7.0});
    key(true);
    ASSERT_EQ(session.queryMediaBins().size(), 1U);
    EXPECT_EQ(session.queryMediaBins().front().id, bin.id);
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), allEdits));
    key();
    const auto revision = session.revision();
    QTest::keyClick(window, Qt::Key_Z, Qt::ShiftModifier);
    EXPECT_EQ(session.revision(), revision) << "bare Shift+Z is not Redo";
    capture("interleaved-history");
}

TEST_F(HistorySurface, SavedIndicatorTracksNativeHistoryAndReplacement) {
    // The same native smoke can consume the existing #69/#72 seed workload;
    // this is not a second workload builder or a performance threshold.
    const auto seed = qEnvironmentVariable("NEMO_HISTORY_PROJECT");
    if (!seed.isEmpty()) {
        auto loaded = nemo::ProjectFile::read(seed.toStdString());
        ASSERT_TRUE(loaded.ok) << loaded.error.message;
        ASSERT_TRUE(session.replaceDocument(std::move(loaded.document)).replaced);
        click("graphFrameAll");
    }
    const auto node = create("transform");
    ASSERT_FALSE(node.isEmpty());
    auto* panel = item("parametersPanel");
    ASSERT_NE(panel, nullptr);
    ASSERT_TRUE(router.requestInspector(panel->property("panelGroup").toString(), controller.rootNetworkId(), node));
    QTest::qWait(50);
    const auto savedContent = session.snapshot();
    file.saveAs(QUrl::fromLocalFile(directory.filePath("history.nemo")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !file.busy(); }, 5000));
    ASSERT_TRUE(file.error().isEmpty()) << file.error().toStdString();
    ASSERT_FALSE(file.dirty());
    EXPECT_TRUE(history.canUndo());
    const auto savedTitle = window->title();
    click("param_" + node + "_translateX");
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    text("4.5");
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(30);
    EXPECT_TRUE(file.dirty());
    EXPECT_NE(window->title(), savedTitle);
    const auto authored = session.snapshot();
    key();
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), savedContent));
    EXPECT_FALSE(file.dirty());
    EXPECT_EQ(window->title(), savedTitle);
    key(true);
    EXPECT_TRUE(nemo::documentContentEquals(session.document(), authored));
    EXPECT_TRUE(file.dirty());
    capture(seed.isEmpty() ? "unsaved-indicator" : "large-project-history");
    ASSERT_TRUE(session.replaceDocument(nemo::Document{}).replaced);
    graphFocus();
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
    const auto revision = session.revision();
    key();
    key(true);
    EXPECT_EQ(session.revision(), revision);
}
}  // namespace
