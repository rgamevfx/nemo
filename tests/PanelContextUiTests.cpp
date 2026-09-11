#include "PanelContextRouter.hpp"
#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QGuiApplication>
#include <QJsonDocument>
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

    QVariantMap context(const QString& panelId) const { return router.contextFor(panelId); }
};

TEST_F(PanelContextUiTest, BindingAndRoleMenusExposePresentationChoices) {
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    ASSERT_FALSE(viewer.isEmpty());
    const auto id = viewer.value(QStringLiteral("id")).toString();

    auto* binding = item(QStringLiteral("panelBinding_") + id);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      binding->mapToScene(QPointF(binding->width() / 2, binding->height() / 2)).toPoint());
    auto* menu = window->findChild<QObject*>(QStringLiteral("panelBindingMenu_") + id);
    ASSERT_NE(menu, nullptr);
    EXPECT_TRUE(menu->property("visible").toBool());
    auto* pinned = visual(window->contentItem(), QStringLiteral("panelBindingPinned_") + id);
    ASSERT_NE(pinned, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      pinned->mapToScene(QPointF(pinned->width() / 2, pinned->height() / 2)).toPoint());
    EXPECT_EQ(context(id).value(QStringLiteral("mode")).toString(), QStringLiteral("pinned"));
    QTest::keyClick(window, Qt::Key_Escape);

    auto* target = item(QStringLiteral("viewerTargetMenu_") + id);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
    QTest::qWait(20);
    auto* role = item(QStringLiteral("viewerRoleMedia_") + id);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      role->mapToScene(QPointF(role->width() / 2, role->height() / 2)).toPoint());
    QTest::qWait(20);
    EXPECT_EQ(context(id).value(QStringLiteral("viewerRole")).toString(), QStringLiteral("media"));
    EXPECT_FALSE(viewerController.hasSource()) << "Changing viewer role must not load media";
}

TEST_F(PanelContextUiTest, GroupClocksAreIsolatedAndPinnedContextStaysFixed) {
    const auto viewer = panelByType(root(), QStringLiteral("viewer"));
    const auto graph = panelByType(root(), QStringLiteral("nodegraph"));
    ASSERT_FALSE(viewer.isEmpty());
    ASSERT_FALSE(graph.isEmpty());
    const auto viewerId = viewer.value(QStringLiteral("id")).toString();
    const auto graphId = graph.value(QStringLiteral("id")).toString();

    router.setLinkMode(viewerId, QStringLiteral("group"));
    router.setGroup(viewerId, QStringLiteral("A"));
    router.setLinkMode(graphId, QStringLiteral("group"));
    router.setGroup(graphId, QStringLiteral("B"));
    router.setGroupContext(
        QStringLiteral("A"),
        QVariantMap{{QStringLiteral("sourceClock"), 11}, {QStringLiteral("sourceTarget"), QStringLiteral("source-a")}});
    router.setGroupContext(QStringLiteral("B"),
                           QVariantMap{{QStringLiteral("graphClock"), 27},
                                       {QStringLiteral("sourceClock"), 29},
                                       {QStringLiteral("sourceTarget"), QStringLiteral("source-b")}});
    EXPECT_EQ(context(viewerId).value(QStringLiteral("sourceClock")).toInt(), 11);
    EXPECT_EQ(context(graphId).value(QStringLiteral("sourceClock")).toInt(), 29);
    EXPECT_NE(context(viewerId).value(QStringLiteral("sourceTarget")).toString(),
              context(graphId).value(QStringLiteral("sourceTarget")).toString());

    router.setLinkMode(viewerId, QStringLiteral("pinned"));
    const auto pinned = context(viewerId);
    router.setGroupContext(
        QStringLiteral("A"),
        QVariantMap{{QStringLiteral("sourceClock"), 99}, {QStringLiteral("sourceTarget"), QStringLiteral("later-a")}});
    EXPECT_EQ(context(viewerId).value(QStringLiteral("sourceClock")), pinned.value(QStringLiteral("sourceClock")));
    EXPECT_EQ(context(viewerId).value(QStringLiteral("sourceTarget")), pinned.value(QStringLiteral("sourceTarget")));
    EXPECT_FALSE(context(viewerId).value(QStringLiteral("available")).toBool());
    EXPECT_FALSE(context(viewerId).value(QStringLiteral("unavailableReason")).toString().isEmpty());
}

}  // namespace
