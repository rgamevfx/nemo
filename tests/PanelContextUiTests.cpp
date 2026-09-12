#include "PanelContextRouter.hpp"
#include "ProjectFileController.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <stdexcept>

namespace {

QQuickItem* visual(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visual(child, name))
            return found;
    }
    return nullptr;
}

QVariantMap panelByType(const QVariantMap& node, const QString& type) {
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("tabs")) {
        for (const auto& value : node.value(QStringLiteral("panels")).toList()) {
            const auto panel = value.toMap();
            if (panel.value(QStringLiteral("type")).toString() == type)
                return panel;
        }
        return {};
    }
    for (const auto& value : node.value(QStringLiteral("children")).toList()) {
        const auto found = panelByType(value.toMap(), type);
        if (!found.isEmpty())
            return found;
    }
    return {};
}

QVariantList panels(const QVariantMap& node) {
    QVariantList result;
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("tabs")) {
        for (const auto& value : node.value(QStringLiteral("panels")).toList()) {
            const auto panel = value.toMap();
            if (!panel.isEmpty())
                result.append(panel);
        }
        return result;
    }
    for (const auto& value : node.value(QStringLiteral("children")).toList())
        result.append(panels(value.toMap()));
    return result;
}

bool waitForSaveSignal(QSignalSpy& spy) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (spy.isEmpty() && std::chrono::steady_clock::now() < deadline) {
        QTest::qWait(10);
    }
    return !spy.isEmpty();
}

QString firstSplitId(const QVariantMap& node) {
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("split")) {
        const auto id = node.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            return id;
    }
    for (const auto& value : node.value(QStringLiteral("children")).toList()) {
        const auto found = firstSplitId(value.toMap());
        if (!found.isEmpty())
            return found;
    }
    return {};
}

}  // namespace

class PanelContextUiTest : public testing::Test {
protected:
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController workspace{directory.filePath(QStringLiteral("workspace.json"))};
    nemo::ProjectSession projectSession;
    nemo::ui::ViewerRuntime viewerRuntime;
    nemo::ui::ViewerController viewerController{&viewerRuntime, projectSession};
    nemo::ui::PanelContextRouter router{projectSession};
    // Main.qml reads the project file state; the harness injects the same
    // adapter the application composes.
    nemo::ui::ProjectFileController projectFile{projectSession, workspace, router};
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;

    void SetUp() override {
        workspace.registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"),
                                    QStringLiteral("ViewerPanel.qml"));
        workspace.registerPanelType(QStringLiteral("nodegraph"), QStringLiteral("Nodegraph"),
                                    QStringLiteral("GraphPanel.qml"));
        workspace.registerPanelType(QStringLiteral("timeline"), QStringLiteral("Timeline"),
                                    QStringLiteral("TimelinePanel.qml"));
        workspace.registerPanelType(QStringLiteral("parameters"), QStringLiteral("Parameters"),
                                    QStringLiteral("ParametersPanel.qml"));
        router.setWorkspaceController(&workspace);
        engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace);
        engine.rootContext()->setContextProperty(QStringLiteral("viewerController"), &viewerController);
        engine.rootContext()->setContextProperty(QStringLiteral("panelContextRouter"), &router);
        engine.rootContext()->setContextProperty(QStringLiteral("projectFile"), &projectFile);
        engine.load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine.rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        ASSERT_NE(window, nullptr);
        window->show();
        window->requestActivate();
        QTest::qWait(50);
    }

    void TearDown() override {
        if (window)
            window->close();
    }

    QVariantMap root() const { return workspace.root(); }

    QQuickItem* item(const QString& name) const {
        auto* found = visual(window->contentItem(), name);
        if (!found)
            throw std::runtime_error("Missing QML item " + name.toStdString());
        return found;
    }

    QPoint center(const QString& name) const {
        auto* found = item(name);
        return found->mapToScene(QPointF(found->width() / 2, found->height() / 2)).toPoint();
    }

    QQuickItem* panelBody(const QString& objectName, const QString& panelId) const {
        std::function<QQuickItem*(QQuickItem*)> walk = [&](QQuickItem* node) -> QQuickItem* {
            if (node->objectName() == objectName && node->property("panelId").toString() == panelId)
                return node;
            for (auto* child : node->childItems())
                if (auto* found = walk(child))
                    return found;
            return nullptr;
        };
        return walk(window->contentItem());
    }

    QVariantMap context(const QString& panelId) const { return router.contextFor(panelId); }
};

TEST_F(PanelContextUiTest, RetiredBindingChromeIsGoneFromPanelHeaders) {
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    ASSERT_FALSE(viewer.isEmpty());
    const auto id = viewer.value(QStringLiteral("id")).toString();

    EXPECT_NE(item(QStringLiteral("panelType_") + id), nullptr);
    EXPECT_EQ(visual(window->contentItem(), QStringLiteral("panelBinding_") + id), nullptr);
    EXPECT_EQ(visual(window->contentItem(), QStringLiteral("panelBindingMenu_") + id), nullptr);
}

TEST_F(PanelContextUiTest, GroupBadgeMenuRetargetsRoutingWithoutTouchingOtherGroups) {
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    const auto graph = panelByType(root(), QStringLiteral("nodegraph"));
    ASSERT_FALSE(viewer.isEmpty());
    ASSERT_FALSE(graph.isEmpty());
    const auto viewerId = viewer.value(QStringLiteral("id")).toString();
    const auto graphId = graph.value(QStringLiteral("id")).toString();
    ASSERT_EQ(viewer.value(QStringLiteral("group")).toString(), QStringLiteral("A"));
    ASSERT_EQ(graph.value(QStringLiteral("group")).toString(), QStringLiteral("A"));

    // Every contextual header carries the visible A-E badge with the
    // prototype's compact 26x24 geometry.
    auto* badge = item(QStringLiteral("panelGroup_") + viewerId);
    EXPECT_TRUE(badge->isVisible());
    EXPECT_EQ(badge->property("text").toString(), QStringLiteral("A"));
    EXPECT_EQ(badge->implicitWidth(), 26.0);
    EXPECT_EQ(badge->implicitHeight(), 24.0);
    EXPECT_GE(badge->width(), 26.0);

    // Select the group through the real opened menu, not through the API.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(QStringLiteral("panelGroup_") + viewerId));
    QTest::qWait(30);
    auto* menu = window->findChild<QObject*>(QStringLiteral("panelGroupMenu_") + viewerId);
    ASSERT_NE(menu, nullptr);
    EXPECT_TRUE(menu->property("visible").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(QStringLiteral("panelGroupChoice_C_") + viewerId));
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (item(QStringLiteral("panelGroup_") + viewerId)->property("text").toString() == QStringLiteral("C"))
            break;
        QTest::qWait(25);
    }

    // The visible letter, the persisted workspace group and the routed panel
    // context all follow the selection...
    EXPECT_EQ(item(QStringLiteral("panelGroup_") + viewerId)->property("text").toString(), QStringLiteral("C"));
    EXPECT_EQ(panelByType(root(), QStringLiteral("viewer")).value(QStringLiteral("group")).toString(),
              QStringLiteral("C"));
    EXPECT_EQ(context(viewerId).value(QStringLiteral("group")).toString(), QStringLiteral("C"));

    // ... unrelated panels keep their own group and their own badge letter ...
    EXPECT_EQ(panelByType(root(), QStringLiteral("nodegraph")).value(QStringLiteral("group")).toString(),
              QStringLiteral("A"));
    EXPECT_EQ(context(graphId).value(QStringLiteral("group")).toString(), QStringLiteral("A"));
    for (const auto& value : panels(root())) {
        const auto entry = value.toMap();
        const auto id = entry.value(QStringLiteral("id")).toString();
        EXPECT_EQ(item(QStringLiteral("panelGroup_") + id)->property("text").toString(),
                  entry.value(QStringLiteral("group")).toString())
            << "panel " << id.toStdString() << " must show its own group letter";
    }

    // ... and a group-scoped context write reaches only panels that selected it.
    ASSERT_TRUE(router.setGroupContext(QStringLiteral("C"),
                                       QVariantMap{{QStringLiteral("sourceTarget"), QStringLiteral("source-c")}}));
    EXPECT_EQ(context(viewerId).value(QStringLiteral("sourceTarget")).toString(), QStringLiteral("source-c"));
    EXPECT_TRUE(context(graphId).value(QStringLiteral("sourceTarget")).toString().isEmpty());

    // The selection is what the existing workspace file persists.
    ASSERT_TRUE(workspace.save());
    nemo::workspace::WorkspaceController restored(directory.filePath(QStringLiteral("workspace.json")));
    const auto restoredViewer = panelByType(restored.root(), QStringLiteral("viewer"));
    ASSERT_FALSE(restoredViewer.isEmpty());
    EXPECT_EQ(restoredViewer.value(QStringLiteral("id")).toString(), viewerId);
    EXPECT_EQ(restoredViewer.value(QStringLiteral("group")).toString(), QStringLiteral("C"));

    // Narrowing the window into the viewer's compact layout must not cost the
    // header its group badge.
    window->resize(960, 700);
    QTest::qWait(150);
    auto* header = item(QStringLiteral("panelHeader_") + viewerId);
    ASSERT_TRUE(badge->isVisible());
    EXPECT_LT(header->width(), 530.0) << "the narrowed viewer must use its compact header allocation";
    EXPECT_GE(badge->width(), 26.0);
    const auto badgeRect = QRectF(badge->mapToItem(header, QPointF(0, 0)), badge->size());
    EXPECT_GE(badgeRect.left(), 0.0);
    EXPECT_LE(badgeRect.right(), header->width());
}

TEST_F(PanelContextUiTest, GroupContextsStayIsolatedWithoutLinkModes) {
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    const auto graph = panelByType(root(), QStringLiteral("nodegraph"));
    ASSERT_FALSE(viewer.isEmpty());
    ASSERT_FALSE(graph.isEmpty());
    const auto viewerId = viewer.value(QStringLiteral("id")).toString();
    const auto graphId = graph.value(QStringLiteral("id")).toString();
    ASSERT_TRUE(router.setGroup(viewerId, QStringLiteral("A")));
    ASSERT_TRUE(router.setGroup(graphId, QStringLiteral("B")));

    router.setGroupContext(
        QStringLiteral("A"),
        QVariantMap{{QStringLiteral("sourceClock"), 11}, {QStringLiteral("sourceTarget"), QStringLiteral("source-a")}});
    router.setGroupContext(
        QStringLiteral("B"),
        QVariantMap{{QStringLiteral("sourceClock"), 29}, {QStringLiteral("sourceTarget"), QStringLiteral("source-b")}});
    EXPECT_EQ(context(viewerId).value(QStringLiteral("sourceClock")).toInt(), 11);
    EXPECT_EQ(context(graphId).value(QStringLiteral("sourceClock")).toInt(), 29);
    EXPECT_NE(context(viewerId).value(QStringLiteral("sourceTarget")).toString(),
              context(graphId).value(QStringLiteral("sourceTarget")).toString());
    EXPECT_FALSE(context(viewerId).contains(QStringLiteral("resolvedGroup")));
    EXPECT_FALSE(context(viewerId).contains(QStringLiteral("mode")));
}

TEST_F(PanelContextUiTest, InspectorRequestsAreGroupScopedAndMediaFree) {
    const auto parameters = panelByType(root(), QStringLiteral("parameters"));
    ASSERT_FALSE(parameters.isEmpty());
    const auto groupAId = parameters.value(QStringLiteral("id")).toString();

    // Convert the timeline leaf into a second live parameters panel in group B
    // so both group receivers are instantiated and visible.
    const auto timeline = panelByType(root(), QStringLiteral("timeline"));
    ASSERT_FALSE(timeline.isEmpty());
    const auto groupBId = timeline.value(QStringLiteral("id")).toString();
    workspace.setPanelType(groupBId, QStringLiteral("parameters"));
    workspace.setGroup(groupBId, QStringLiteral("B"));

    const auto waitForPanel = [&](const QString& id) -> QQuickItem* {
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (auto* found = panelBody(QStringLiteral("parametersPanel"), id))
                return found;
            QTest::qWait(25);
        }
        return nullptr;
    };
    auto* panelA = waitForPanel(groupAId);
    auto* panelB = waitForPanel(groupBId);
    ASSERT_NE(panelA, nullptr);
    ASSERT_NE(panelB, nullptr);
    EXPECT_EQ(panelA->property("panelGroup").toString(), QStringLiteral("A"));
    EXPECT_EQ(panelB->property("panelGroup").toString(), QStringLiteral("B"));

    const auto revision = projectSession.revision();
    ASSERT_TRUE(router.requestInspector(QStringLiteral("A"), QStringLiteral("7"), QStringLiteral("42")));
    QTest::qWait(30);
    ASSERT_EQ(panelA->property("inspectors").toList().size(), 1);
    EXPECT_TRUE(panelB->property("inspectors").toList().isEmpty())
        << "a group-A request must not reach the group-B parameters panel";
    const auto opened = panelA->property("inspectors").toList().front().toMap();
    EXPECT_EQ(opened.value(QStringLiteral("network")).toString(), QStringLiteral("7"));
    EXPECT_EQ(opened.value(QStringLiteral("node")).toString(), QStringLiteral("42"));

    ASSERT_TRUE(router.requestInspector(QStringLiteral("B"), QStringLiteral("9"), QStringLiteral("11")));
    QTest::qWait(30);
    EXPECT_EQ(panelA->property("inspectors").toList().size(), 1);
    ASSERT_EQ(panelB->property("inspectors").toList().size(), 1);

    // The relay is media-free and never mutates the document.
    EXPECT_EQ(projectSession.revision(), revision);
    EXPECT_TRUE(projectSession.document().mediaCatalog.entries().empty());
}

TEST_F(PanelContextUiTest, PresentationEditRacingProjectWriteKeepsProjectDirtyAndFileUnchangedByIt) {
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    ASSERT_FALSE(viewer.isEmpty());
    const auto viewerId = viewer.value(QStringLiteral("id")).toString();
    const QString file = directory.filePath(QStringLiteral("race.nemo"));

    // saveAs captures the owned document snapshot and presentation
    // synchronously; its worker completion is queued and cannot publish before
    // the presentation edit below runs.
    const nlohmann::json preEditWorkspace = workspace.projectPresentation();
    QSignalSpy firstSave(&projectFile, &nemo::ui::ProjectFileController::saveFinished);
    projectFile.saveAs(QUrl::fromLocalFile(file));
    workspace.setGroup(viewerId, QStringLiteral("B"));
    ASSERT_TRUE(workspace.error().isEmpty()) << workspace.error().toStdString();

    ASSERT_TRUE(waitForSaveSignal(firstSave));
    EXPECT_TRUE(firstSave.takeLast().at(0).toBool()) << projectFile.error().toStdString();
    // The edit that raced the write was not saved: still dirty, and the file
    // carries exactly the presentation the write captured.
    EXPECT_TRUE(projectFile.dirty());
    QFile written(file);
    ASSERT_TRUE(written.open(QIODevice::ReadOnly));
    const auto writtenJson = nlohmann::json::parse(written.readAll().constData());
    const auto writtenData = nemo::presentationData(writtenJson.at("presentation"));
    ASSERT_TRUE(writtenData.is_object());
    EXPECT_EQ(writtenData.at("workspace"), preEditWorkspace);

    // A following save records the current presentation and returns clean.
    QSignalSpy secondSave(&projectFile, &nemo::ui::ProjectFileController::saveFinished);
    projectFile.saveAs(QUrl::fromLocalFile(file));
    ASSERT_TRUE(waitForSaveSignal(secondSave));
    EXPECT_TRUE(secondSave.takeLast().at(0).toBool()) << projectFile.error().toStdString();
    EXPECT_FALSE(projectFile.dirty());
}

TEST_F(PanelContextUiTest, SplitRatioChangeAfterSaveMarksProjectDirtyAndSecondSaveClearsIt) {
    const QString file = directory.filePath(QStringLiteral("ratio.nemo"));
    QSignalSpy firstSave(&projectFile, &nemo::ui::ProjectFileController::saveFinished);
    projectFile.saveAs(QUrl::fromLocalFile(file));
    ASSERT_TRUE(waitForSaveSignal(firstSave));
    EXPECT_TRUE(firstSave.takeLast().at(0).toBool()) << projectFile.error().toStdString();
    EXPECT_FALSE(projectFile.dirty());

    const QString splitId = firstSplitId(root());
    ASSERT_FALSE(splitId.isEmpty());
    // A splitter drag persists a ratio and deliberately skips rootChanged so
    // the QML tree is not rebuilt; the project must still become dirty.
    workspace.setRatio(splitId, 0.371);
    ASSERT_TRUE(workspace.error().isEmpty()) << workspace.error().toStdString();
    EXPECT_TRUE(projectFile.dirty());

    QSignalSpy secondSave(&projectFile, &nemo::ui::ProjectFileController::saveFinished);
    projectFile.saveAs(QUrl::fromLocalFile(file));
    ASSERT_TRUE(waitForSaveSignal(secondSave));
    EXPECT_TRUE(secondSave.takeLast().at(0).toBool()) << projectFile.error().toStdString();
    EXPECT_FALSE(projectFile.dirty());
}

TEST_F(PanelContextUiTest, SaveAsToExtensionlessPathNeverRewritesAnExistingSiblingProject) {
    const QString target = directory.filePath(QStringLiteral("shot"));
    const QString sibling = target + QStringLiteral(".nemo");

    // A real, valid sibling project the native chooser never confirmed
    // replacing: saving to the exact extensionless path must not rewrite it.
    QSignalSpy siblingSave(&projectFile, &nemo::ui::ProjectFileController::saveFinished);
    projectFile.saveAs(QUrl::fromLocalFile(sibling));
    ASSERT_TRUE(waitForSaveSignal(siblingSave));
    ASSERT_TRUE(siblingSave.takeLast().at(0).toBool()) << projectFile.error().toStdString();
    QFile siblingFile(sibling);
    ASSERT_TRUE(siblingFile.open(QIODevice::ReadOnly));
    const QByteArray siblingBytes = siblingFile.readAll();
    ASSERT_FALSE(siblingBytes.isEmpty());

    // Change the presentation so the next write's content differs from the
    // sibling's; a suffix rewrite would visibly clobber it.
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    ASSERT_FALSE(viewer.isEmpty());
    workspace.setGroup(viewer.value(QStringLiteral("id")).toString(), QStringLiteral("B"));
    ASSERT_TRUE(workspace.error().isEmpty()) << workspace.error().toStdString();

    QSignalSpy targetSave(&projectFile, &nemo::ui::ProjectFileController::saveFinished);
    projectFile.saveAs(QUrl::fromLocalFile(target));
    ASSERT_TRUE(waitForSaveSignal(targetSave));
    EXPECT_TRUE(targetSave.takeLast().at(0).toBool()) << projectFile.error().toStdString();

    // The exact chosen path was written, not <path>.nemo...
    EXPECT_TRUE(QFile::exists(target));
    EXPECT_EQ(projectFile.filePath(), target);
    // ...and the sibling project is byte-identical.
    QFile unchanged(sibling);
    ASSERT_TRUE(unchanged.open(QIODevice::ReadOnly));
    EXPECT_EQ(unchanged.readAll(), siblingBytes);
}
