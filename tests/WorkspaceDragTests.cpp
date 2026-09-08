#include "WorkspaceController.hpp"

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
    QQmlApplicationEngine engine;
    QSignalSpy warnings{&engine, &QQmlEngine::warnings};
    QQuickWindow* window = nullptr;
    QString source;
    QString other;
    QString target;

    void SetUp() override {
        engine.rootContext()->setContextProperty("workspace", &controller);
        engine.load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine.rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        ASSERT_NE(window, nullptr);
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
        // Observe a rendered result, not just updated QML properties. Qt's
        // software scene graph also retires destroyed image textures at sync.
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

    drag(center("panelGroup_" + source), to);
    EXPECT_EQ(snapshot(), before);
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
    QTest::qWait(30);
    ASSERT_FALSE(window->isActive());
    release(to);
    EXPECT_EQ(snapshot(), before);
    EXPECT_FALSE(item("dockLabel")->isVisible());
    anotherWindow.hide();
    window->requestActivate();
    QTest::qWait(30);

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
}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
