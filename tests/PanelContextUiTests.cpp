#include "PanelContextRouter.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

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

}  // namespace

class PanelContextUiTest : public testing::Test {
protected:
    QTemporaryDir directory;
    nemo::workspace::WorkspaceController workspace{directory.filePath(QStringLiteral("workspace.json"))};
    nemo::ProjectSession projectSession;
    nemo::ui::ViewerRuntime viewerRuntime;
    nemo::ui::ViewerController viewerController{&viewerRuntime, projectSession};
    nemo::ui::PanelContextRouter router{projectSession};
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
