#include "AnimationViewModel.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ProjectFileController.hpp"
#include "ViewerController.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJSEngine>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <gtest/gtest.h>

namespace {
using namespace nemo;
Document animatedDocument() {
    Document document;
    auto& graph = document.network(document.rootNetworkId()).graph();
    const auto node = graph.addNode("constcolor", "Color");
    setLayoutCommand(document.rootNetworkId(), node, {-140, 0}).apply(document);
    const ParameterAddress address{document.rootNetworkId(), node, "color"};
    setKeyframesCommand({{address, Keyframe{0, 10, ColorValue{{0.0F, 0.25F, 0.5F, 1.0F}}}},
                         {address, Keyframe{0, 30, ColorValue{{1.0F, 0.5F, 0.75F, 1.0F}}}}})
        .apply(document);
    return document;
}
QVariantMap nodeTarget(NetworkId network, NodeId node) {
    return {{"network", QString::number(network)}, {"node", QString::number(node)}};
}
QVariantList rootAnimationTargets(const Document& document) {
    QVariantList targets;
    for (const auto& channel : document.animationChannels()) {
        const auto* occurrence = document.instance(channel.address.instance);
        const auto network = occurrence ? occurrence->parentNetwork : channel.address.network;
        const auto node = occurrence ? occurrence->node : channel.address.node;
        const auto target = nodeTarget(network, node);
        if (network == document.rootNetworkId() && !targets.contains(target))
            targets.push_back(target);
    }
    return targets;
}
TEST(AnimationUi, TargetUnionKeepsNetworkIdentityAndRejectsFilteredEdits) {
    auto document = animatedDocument();
    const auto original = document.animationChannels().front();
    const auto occurrenceId = std::make_shared<NetworkInstanceId>();
    collapseSelectionCommand(document.rootNetworkId(), {original.address.node}, "Subnet", occurrenceId).apply(document);
    const auto definition = document.instance(*occurrenceId)->definition;
    const auto child = document.network(definition).graph().nodeByName("Color")->id;
    const auto other = document.network(document.rootNetworkId()).graph().addNode("constcolor", "Other");
    setKeyframesCommand({{ParameterAddress{document.rootNetworkId(), other, "color"},
                          Keyframe{0, 12, ColorValue{{0.2F, 0.3F, 0.4F, 1.0F}}}}})
        .apply(document);
    ProjectSession session(document);
    ui::AnimationViewModel model(session);
    EXPECT_TRUE(model.channels().isEmpty());
    auto red = nodeTarget(definition, child);
    red.insert("parameter", "color");
    red.insert("component", 0);
    model.setTargets({red, nodeTarget(document.rootNetworkId(), other), red, nodeTarget(999999, child)});
    ASSERT_EQ(model.channels().size(), 5);
    const auto projected = model.channels().front().toMap();
    EXPECT_EQ(projected.value("networkId").toString(), QString::number(definition));
    EXPECT_EQ(projected.value("nodeId").toString(), QString::number(child));
    const auto selectedKey = projected.value("keys").toList().front().toMap().value("id").toString();
    ASSERT_TRUE(model.beginGesture());
    const auto revision = session.revision();
    model.setTargets({nodeTarget(document.rootNetworkId(), other)});
    EXPECT_FALSE(model.moveKeys({selectedKey}, 4, 1));
    EXPECT_EQ(session.revision(), revision);
    EXPECT_FALSE(session.canUndo());
    EXPECT_EQ(session.document().animationChannels(), document.animationChannels());
}

TEST(AnimationUi, ParameterTargetSurvivesLastKeyRemovalAndRecreation) {
    ProjectSession session(animatedDocument());
    ui::AnimationViewModel model(session);
    const auto address = session.document().animationChannels().front().address;
    auto green = nodeTarget(address.network, address.node);
    green.insert("parameter", "color");
    green.insert("component", 1);
    model.setTargets({green});
    ASSERT_EQ(model.channels().size(), 1);
    QStringList keys;
    for (const auto& value : model.channels().front().toMap().value("keys").toList())
        keys.push_back(value.toMap().value("id").toString());
    ASSERT_TRUE(model.removeKeys(keys));
    EXPECT_TRUE(model.channels().isEmpty());
    ASSERT_TRUE(session
                    .submit(setKeyframesCommand({{address, Keyframe{0, 20, ColorValue{{0.1F, 0.7F, 0.3F, 1.0F}}}}}),
                            {session.revision(), {}})
                    .committed);
    ASSERT_EQ(model.channels().size(), 1);
    const auto values = model.channels().front().toMap().value("keys").toList();
    ASSERT_EQ(values.size(), 1);
    EXPECT_FLOAT_EQ(values.front().toMap().value("value").toFloat(), 0.7F);
}
QString key(const ui::AnimationViewModel& model, int component, int index) {
    return model.channels().at(component).toMap().value("keys").toList().at(index).toMap().value("id").toString();
}
TEST(AnimationUi, ComponentMovesCoalesceTimeAndPreserveUnselectedValues) {
    ProjectSession session(animatedDocument());
    ui::AnimationViewModel model(session);
    model.setTargets(rootAnimationTargets(session.document()));
    const auto before = session.document().animationChannels().front();
    ASSERT_TRUE(model.beginGesture());
    ASSERT_TRUE(model.moveKeys({key(model, 0, 0), key(model, 1, 0)}, 2, 0.25));
    model.cancelGesture();
    const auto moved = session.document().animationChannels().front();
    EXPECT_DOUBLE_EQ(moved.keys[0].time, 12);
    EXPECT_EQ(moved.keys[0].value, (ParameterValue{ColorValue{{0.25F, 0.5F, 0.5F, 1.0F}}}));
    ASSERT_TRUE(model.undo());
    EXPECT_EQ(session.document().animationChannels().front(), before);
    ASSERT_TRUE(model.redo());
    EXPECT_EQ(session.document().animationChannels().front(), moved);
    ASSERT_TRUE(model.undo());
    ASSERT_TRUE(model.editKey(key(model, 0, 0), 11, 0.5, 0, 0));
    EXPECT_FALSE(model.redo());
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[0].time, 11);
}
TEST(AnimationUi, CollisionRejectsEveryExactFieldAndDoesNotConsumeHistory) {
    ProjectSession session(animatedDocument());
    ui::AnimationViewModel model(session);
    model.setTargets(rootAnimationTargets(session.document()));
    const auto before = session.document().animationChannels().front();
    const auto revision = session.revision();
    ASSERT_TRUE(model.beginGesture());
    EXPECT_FALSE(model.editKey(key(model, 0, 0), 30, 0.75, 2, 2));
    EXPECT_FALSE(model.error().isEmpty());
    EXPECT_EQ(session.document().animationChannels().front(), before);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_FALSE(session.canUndo());
    ASSERT_TRUE(model.editKey(key(model, 0, 0), 12, 0.75, 2, 2));
    model.cancelGesture();
    const auto& edited = session.document().animationChannels().front().keys.front();
    EXPECT_DOUBLE_EQ(edited.time, 12);
    EXPECT_FLOAT_EQ(std::get<ColorValue>(edited.value).value[0], 0.75F);
    EXPECT_DOUBLE_EQ(edited.inSlope[0], 2);
    EXPECT_DOUBLE_EQ(edited.outSlope[0], 2);
    ASSERT_TRUE(model.undo());
    EXPECT_EQ(session.document().animationChannels().front(), before);
}
TEST(AnimationUi, StaleGestureCannotOverwriteAnotherEditOrReplacement) {
    ProjectSession session(animatedDocument());
    ui::AnimationViewModel model(session);
    model.setTargets(rootAnimationTargets(session.document()));
    const auto id = key(model, 0, 0);
    ASSERT_TRUE(model.beginGesture());
    const auto& original = session.document().animationChannels().front();
    auto changed = original.keys[0];
    changed.time = 11;
    ASSERT_TRUE(session.submit(setKeyframesCommand({{original.address, changed}}), {session.revision(), {}}).committed);
    EXPECT_FALSE(model.moveKeys({id}, 3, 0));
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[0].time, 11);
    model.cancelGesture();
    ASSERT_TRUE(model.beginGesture());
    ASSERT_TRUE(session.replaceDocument(animatedDocument()).replaced);
    EXPECT_FALSE(model.editKey(id, 12, 0.5, 0, 0));
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[0].time, 10);
}
TEST(AnimationUi, TangentsAndInsertionUseSharedCurveSemantics) {
    ProjectSession session(animatedDocument());
    ui::AnimationViewModel model(session);
    model.setTargets(rootAnimationTargets(session.document()));
    const auto id = key(model, 0, 0);
    ASSERT_TRUE(model.setInterpolation({id}, "bezier"));
    ASSERT_TRUE(model.setTangent(id, "out", 0.1));
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[0].inSlope[0], 0.1);
    ASSERT_TRUE(model.setTangentMode({id}, "broken"));
    ASSERT_TRUE(model.setTangent(id, "out", 0.2));
    const auto broken = session.document().animationChannels().front();
    ASSERT_TRUE(model.undo());
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[0].outSlope[0], 0.1);
    ASSERT_TRUE(model.redo());
    EXPECT_EQ(session.document().animationChannels().front(), broken);
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[0].inSlope[0], 0.1);
    const auto address = session.document().animationChannels().front().address;
    const auto middle = animatedParameterValue(session.document(), address, 20);
    const auto channel = model.channels()[0].toMap().value("id").toString();
    const auto inserted = model.insertKey(channel, 20);
    ASSERT_FALSE(inserted.isEmpty());
    EXPECT_EQ(animatedParameterValue(session.document(), address, 20), middle);
    const auto revision = session.revision();
    EXPECT_EQ(model.insertKey(channel, 20), inserted);
    EXPECT_EQ(session.revision(), revision);
    ASSERT_TRUE(model.undo());
    EXPECT_EQ(session.document().animationChannels().front().keys.size(), 2U);
    ASSERT_TRUE(model.redo());
    EXPECT_EQ(key(model, 0, 1), inserted);
    EXPECT_EQ(animatedParameterValue(session.document(), address, 20), middle);
}

QQuickItem* visual(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = visual(child, name))
            return found;
    return nullptr;
}
QVariantMap panelByType(const QVariantMap& node, const QString& type) {
    for (const auto& value : node.value("panels").toList())
        if (value.toMap().value("type").toString() == type)
            return value.toMap();
    for (const auto& child : node.value("children").toList()) {
        const auto found = panelByType(child.toMap(), type);
        if (!found.isEmpty())
            return found;
    }
    return {};
}
class AnimationSurface : public testing::Test {
protected:
    QTemporaryDir directory;
    workspace::WorkspaceController workspace{directory.filePath("workspace.json")};
    ProjectSession session{animatedDocument()};
    ui::ViewerRuntime runtime;
    ui::ViewerController controller{&runtime, session};
    ui::PanelContextRouter router{session};
    ui::NativeFileChooser chooser;
    ui::ProjectFileController file{session, workspace, router, chooser};
    ui::ParameterEditorRegistry editors;
    QQmlApplicationEngine engine;
    QSignalSpy warnings{&engine, &QQmlEngine::warnings};
    QQuickWindow* window{};
    QString panelId;
    void SetUp() override {
        for (const auto& entry : std::vector<std::pair<QString, QString>>{{"viewer", "ViewerPanel.qml"},
                                                                          {"nodegraph", "GraphPanel.qml"},
                                                                          {"parameters", "ParametersPanel.qml"},
                                                                          {"animation", "AnimationPanel.qml"},
                                                                          {"timeline", "TimelinePanel.qml"}})
            workspace.registerPanelType(entry.first,
                                        entry.first == "nodegraph" ? QString("Node graph")
                                                                   : entry.first.left(1).toUpper() + entry.first.mid(1),
                                        entry.second);
        const auto timeline = panelByType(workspace.root(), "timeline");
        panelId = timeline.value("id").toString();
        if (panelId.isEmpty()) {
            const auto viewer = panelByType(workspace.root(), "viewer");
            panelId = viewer.value("id").toString();
        }
        workspace.setPanelType(panelId, "animation");
        router.setWorkspaceController(&workspace);
        engine.rootContext()->setContextProperty("workspace", &workspace);
        engine.rootContext()->setContextProperty("viewerController", &controller);
        engine.rootContext()->setContextProperty("panelContextRouter", &router);
        engine.rootContext()->setContextProperty("projectFile", &file);
        engine.rootContext()->setContextProperty("parameterEditors", &editors);
        engine.load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine.rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        ASSERT_NE(window, nullptr);
        window->show();
        window->requestActivate();
        QTest::qWait(100);
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        window->requestUpdate();
        ASSERT_TRUE(rendered.wait(2000));
        for (const auto& target : rootAnimationTargets(session.document()))
            ASSERT_TRUE(router.requestInspector("A", target.toMap().value("network").toString(),
                                                target.toMap().value("node").toString()));
        QTest::qWait(30);
    }
    void TearDown() override {
        if (window)
            window->close();
        EXPECT_EQ(warnings.count(), 0);
    }
    QQuickItem* item(const QString& name) { return visual(window->contentItem(), name); }
    void click(const QString& name, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        auto* target = item(name);
        ASSERT_NE(target, nullptr) << name.toStdString();
        QTest::mouseClick(window, Qt::LeftButton, modifiers,
                          target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
        QTest::qWait(20);
    }
    QVariant js(const QString& expression) {
        engine.globalObject().setProperty("animation", engine.newQObject(item("animationPanel")));
        const auto result = engine.evaluate(expression);
        EXPECT_FALSE(result.isError()) << result.toString().toStdString();
        return result.toVariant();
    }
    QPoint point(double time, double value) {
        const auto x = js(QString("animation.timeToX(%1)").arg(time)).toDouble();
        const auto y = js(QString("animation.valueToY(%1)").arg(value)).toDouble();
        return item("animationSurface")->mapToScene(QPointF(x, y)).toPoint();
    }
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool cancel = false,
              Qt::MouseButton button = Qt::LeftButton) {
        QTest::mousePress(window, button, modifiers, from);
        for (int step = 1; step <= 6; ++step) {
            const QPointF position = QPointF(from) + QPointF(to - from) * (step / 6.0);
            QMouseEvent event(QEvent::MouseMove, position, window->mapToGlobal(position.toPoint()), Qt::NoButton,
                              button, modifiers);
            QGuiApplication::sendEvent(window, &event);
        }
        if (cancel)
            QTest::keyClick(window, Qt::Key_Escape);
        QTest::mouseRelease(window, button, modifiers, to);
        QTest::qWait(20);
    }
    void clickPin(const QString& name) {
        auto* target = item(name);
        ASSERT_NE(target, nullptr);
        QTest::mouseMove(window, target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
        QTest::qWait(30);
        click(name);
    }
    void enter(const QString& name, const QString& text) {
        click(name);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const QChar character : text)
            QTest::keyClick(window, character.toLatin1());
    }
    void capture(const QString& name) {
        const auto dir = qEnvironmentVariable("NEMO_ANIMATION_CAPTURE_DIR");
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);
        QTest::qWait(100);
        auto* header = item("panelHeader_" + panelId);
        if (header) {
            auto* panel = header->parentItem()->parentItem();
            const auto origin = panel->mapToScene(QPointF{});
            const QRect crop = QRectF(origin, QSizeF(panel->width(), panel->height())).toAlignedRect();
            EXPECT_TRUE(window->grabWindow().copy(crop).save(dir + '/' + name + "-panel.png"));
        }
        auto* popup = window->findChild<QObject*>("animationEditKeyPopup");
        if (popup && popup->property("visible").toBool()) {
            const QRect bounds(popup->property("x").toInt(), popup->property("y").toInt(),
                               popup->property("width").toInt(), popup->property("height").toInt());
            EXPECT_TRUE(window->grabWindow().copy(bounds).save(dir + '/' + name + "-popup.png"));
        }
        if (name == "dense-track" || name == "inspector-pins") {
            session.setPresentation(makePresentationEnvelope(
                {{"workspace", workspace.projectPresentation()}, {"context", router.contextPresentation()}}));
            const auto filename = name == "dense-track" ? "/dense.nemo" : "/inspector-pins.nemo";
            EXPECT_TRUE(ProjectFile::writeAtomic(session.prepareSave((dir + filename).toStdString())).ok);
        }
        EXPECT_TRUE(window->grabWindow().save(dir + '/' + name + ".png"));
    }
};
TEST_F(AnimationSurface, HeaderSelectionAndHierarchyNeverWriteAnimation) {
    ASSERT_NE(item("animationPanel"), nullptr);
    const auto before = session.document().animationChannels();
    const auto revision = session.revision();
    capture("track");
    click("animationCurvesView");
    EXPECT_EQ(item("animationPanel")->property("viewMode").toString(), "curves");
    EXPECT_EQ(js("animation.channels.length").toInt(), 4);
    const auto start = item("animationPanel")->property("viewStart");
    const auto end = item("animationPanel")->property("viewEnd");
    const auto node = QString::number(session.document().animationChannels().front().address.node);
    click("animationNodeRow_" + controller.rootNetworkId() + "_" + node);
    EXPECT_EQ(item("animationPanel")->property("viewStart"), start);
    EXPECT_EQ(item("animationPanel")->property("viewEnd"), end);
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 4);
    capture("curves");
    EXPECT_EQ(session.document().animationChannels(), before);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_FALSE(session.canUndo());
}

TEST_F(AnimationSurface, NativeInsertionExactCollisionAndCancellableConstrainedDrag) {
    window->setMaximumSize(QSize(1274, 640));
    window->resize(1274, 640);
    QTest::qWait(60);
    click("animationCurvesView");
    ASSERT_EQ(js("animation.viewMode").toString(), "curves");
    click("animationFrameAll");
    const auto before = session.document().animationChannels().front();
    const auto clock = controller.frame();
    const auto start = js("animation.viewStart");
    const auto end = js("animation.viewEnd");
    QTest::mouseClick(window, Qt::LeftButton, Qt::AltModifier, point(20, 0.5));
    QTest::qWait(20);
    ASSERT_EQ(session.document().animationChannels().front().keys.size(), 3U);
    const auto inserted = session.document().animationChannels().front().keys[1];
    EXPECT_DOUBLE_EQ(inserted.time, 20);
    EXPECT_FLOAT_EQ(std::get<ColorValue>(inserted.value).value[0], 0.5);
    EXPECT_EQ(controller.frame(), clock);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    ASSERT_EQ(session.document().animationChannels().front(), before);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    ASSERT_EQ(session.document().animationChannels().front().keys[1].id, inserted.id);
    // Right-click the inserted red component and use the actual contextual editor.
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point(20, 0.5));
    QTest::qWait(20);
    click("animationEditKey");
    capture("key-edit");
    enter("animationKeyTime", "30");
    enter("animationKeyValue", "0.8");
    const auto revision = session.revision();
    const auto unchanged = session.document().animationChannels().front();
    click("animationKeyApply");
    auto* popup = window->findChild<QObject*>("animationEditKeyPopup");
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(popup->property("visible").toBool());
    EXPECT_FALSE(popup->property("errorMessage").toString().isEmpty());
    EXPECT_EQ(session.document().animationChannels().front(), unchanged);
    EXPECT_EQ(session.revision(), revision);
    // The popup's controls must be inside the actual native window even when
    // its owning dock is shorter than the collision text.
    auto* apply = item("animationKeyApply");
    ASSERT_NE(apply, nullptr);
    EXPECT_LE(apply->mapToScene(QPointF(0, apply->height())).y(), window->height());
    capture("collision");
    enter("animationKeyTime", "22");
    click("animationKeyApply");
    EXPECT_FALSE(popup->property("visible").toBool());
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[1].time, 22);
    EXPECT_FLOAT_EQ(std::get<ColorValue>(session.document().animationChannels().front().keys[1].value).value[0], 0.8F);
    // Shift locks to time. A cancelled second drag never publishes.
    const auto from = point(22, 0.8);
    drag(from, from + QPoint(28, 8), Qt::ShiftModifier);
    const auto moved = session.document().animationChannels().front();
    EXPECT_NE(moved.keys[1].time, 22);
    EXPECT_FLOAT_EQ(std::get<ColorValue>(moved.keys[1].value).value[0], 0.8F);
    const auto afterDrag = session.revision();
    drag(point(moved.keys[1].time, 0.8), point(moved.keys[1].time, 0.8) + QPoint(18, 16), Qt::NoModifier, true);
    EXPECT_EQ(session.revision(), afterDrag);
    EXPECT_EQ(session.document().animationChannels().front(), moved);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[1].time, 22);
    const auto exact = session.document().animationChannels().front();
    drag(point(22, 0.8), point(22, 0.8) + QPoint(3, -14), Qt::ShiftModifier);
    EXPECT_DOUBLE_EQ(session.document().animationChannels().front().keys[1].time, 22);
    EXPECT_NE(session.document().animationChannels().front().keys[1].value, exact.keys[1].value);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(session.document().animationChannels().front(), exact);
    drag(point(22, 0.8), point(22, 0.8) + QPoint(17, 0), Qt::AltModifier);
    const auto fractionalTime = session.document().animationChannels().front().keys[1].time;
    EXPECT_GT(std::abs(fractionalTime - std::round(fractionalTime)), 0.01);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(session.document().animationChannels().front(), exact);
    EXPECT_EQ(controller.frame(), clock);
    EXPECT_EQ(js("animation.viewStart"), start);
    EXPECT_EQ(js("animation.viewEnd"), end);
    const auto viewRevision = session.revision();
    QTest::keyClick(window, Qt::Key_F);
    EXPECT_LT(js("animation.viewEnd").toDouble() - js("animation.viewStart").toDouble(),
              end.toDouble() - start.toDouble());
    QTest::keyClick(window, Qt::Key_Home);
    EXPECT_EQ(js("animation.viewStart"), start);
    click("animationFrameAll", Qt::ShiftModifier);
    EXPECT_LT(js("animation.viewEnd").toDouble() - js("animation.viewStart").toDouble(),
              end.toDouble() - start.toDouble());
    EXPECT_EQ(session.revision(), viewRevision);
}

TEST_F(AnimationSurface, VisibilityFramingAndGroupRestorationRemainPresentationOnly) {
    click("animationCurvesView");
    click("animationFrameAll");
    const auto revision = session.revision();
    const auto start = js("animation.viewStart");
    const auto end = js("animation.viewEnd");
    const auto node = QString::number(session.document().animationChannels().front().address.node);
    ASSERT_TRUE(router.requestInspector("A", controller.rootNetworkId(), node));
    QTest::qWait(20);
    EXPECT_EQ(js("animation.targetNodeId").toString(), node);
    // The tree's explicit context menu owns isolation; selection alone does not.
    auto* row = item("animationParameterRow_" + controller.rootNetworkId() + "_" + node + "_color.R");
    ASSERT_NE(row, nullptr);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
    QTest::qWait(20);
    click("animationIsolateCurves");
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 1);
    click("animationNodeRow_" + controller.rootNetworkId() + "_" + node);
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 1);
    EXPECT_EQ(js("animation.viewStart"), start);
    EXPECT_EQ(js("animation.viewEnd"), end);
    capture("isolated");
    row = item("animationNodeRow_" + controller.rootNetworkId() + "_" + node);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, row->mapToScene(QPointF(10, 11)).toPoint());
    EXPECT_EQ(js("animation.rows.length").toInt(), 1);
    EXPECT_EQ(js("animation.channels.length").toInt(), 4);
    row = item("animationNodeRow_" + controller.rootNetworkId() + "_" + node);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
    QTest::qWait(20);
    click("animationShowAllCurves");
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 4);
    row = item("animationNodeRow_" + controller.rootNetworkId() + "_" + node);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, row->mapToScene(QPointF(10, 11)).toPoint());
    row = item("animationParameterRow_" + controller.rootNetworkId() + "_" + node + "_color.R");
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
    QTest::qWait(20);
    click("animationHideCurves");
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 3);
    workspace.setGroup(panelId, "B");
    QTest::qWait(40);
    EXPECT_EQ(js("animation.networkId").toString(), controller.rootNetworkId());
    EXPECT_EQ(js("animation.targetNodeId").toString(), node);
    EXPECT_EQ(js("animation.viewStart"), start);
    EXPECT_EQ(js("animation.viewEnd"), end);
    const auto originalWorkspace = workspace.activeWorkspaceId();
    const auto other = workspace.createWorkspace("Other");
    ASSERT_FALSE(other.isEmpty());
    ASSERT_TRUE(workspace.switchWorkspace(other));
    QTest::qWait(60);
    ASSERT_TRUE(workspace.switchWorkspace(originalWorkspace));
    QTest::qWait(50);
    EXPECT_EQ(js("animation.targetNodeId").toString(), node);
    EXPECT_EQ(js("animation.viewStart"), start);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_FALSE(session.canUndo());
}

TEST(AnimationUi, ExposedOccurrenceChannelsDoNotEditTheirDefinition) {
    auto document = animatedDocument();
    const auto original = document.animationChannels().front();
    const auto occurrenceId = std::make_shared<NetworkInstanceId>();
    collapseSelectionCommand(document.rootNetworkId(), {original.address.node}, "Subnet", occurrenceId).apply(document);
    const auto occurrence = *document.instance(*occurrenceId);
    const auto& definition = document.network(occurrence.definition);
    const auto child = definition.graph().nodeByName("Color")->id;
    promoteParameterCommand(occurrence.definition, child, "color", "Tint").apply(document);
    const ParameterAddress address{occurrence.definition, child, "color", occurrence.id};
    setKeyframesCommand({{address, Keyframe{0, 12, ColorValue{{0.5F, 0.25F, 0.75F, 1.0F}}}}}).apply(document);
    ProjectSession session(document);
    ui::AnimationViewModel model(session);
    model.setTargets(rootAnimationTargets(session.document()));
    ASSERT_EQ(model.channels().size(), 4);
    EXPECT_EQ(model.channels()[0].toMap().value("nodeId").toString(), QString::number(occurrence.node));
    EXPECT_EQ(model.channels()[0].toMap().value("label").toString(), "Tint.R");
    const auto definitionBefore = session.document().animationChannels().front();
    ASSERT_TRUE(model.editKey(key(model, 0, 0), 15, 0.8, 0, 0));
    EXPECT_EQ(session.document().animationChannels().front(), definitionBefore);
    EXPECT_FLOAT_EQ(std::get<ColorValue>(animatedParameterValue(session.document(), address, 15)).value[0], 0.8F);
    ASSERT_TRUE(model.undo());
    EXPECT_DOUBLE_EQ(session.document().animationChannel(address)->keys[0].time, 12);
    model.setTargets({nodeTarget(999999, occurrence.node)});
    EXPECT_FALSE(model.available());
    EXPECT_TRUE(model.channels().isEmpty());
    EXPECT_FALSE(model.moveKeys({"1:0/1"}, 2, 0));
}

TEST_F(AnimationSurface, GraphDoubleClickAndKeyedSliderShareInspectorTargetAndHistory) {
    const auto scope = controller.rootNetworkId();
    const auto node = controller.createGraphNode(scope, "transform", "Motion", 20, 60, {}, {});
    ASSERT_FALSE(node.isEmpty());
    controller.setFrame(12);
    ASSERT_TRUE(controller.keyNodeParameter(scope, node, "translateX"));
    QTest::qWait(30);
    auto* graph = item("graphItem");
    ASSERT_NE(graph, nullptr);
    QRectF rectangle;
    ASSERT_TRUE(
        QMetaObject::invokeMethod(graph, "nodeRect", Q_RETURN_ARG(QRectF, rectangle), Q_ARG(QVariant, QVariant(node))));
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, graph->mapToScene(rectangle.center()).toPoint());
    QTest::qWait(30);
    EXPECT_EQ(js("animation.networkId").toString(), scope);
    EXPECT_EQ(js("animation.targetNodeId").toString(), node);
    auto* slider = item("slider_" + node + "_translateX");
    ASSERT_NE(slider, nullptr);
    const auto start = js("animation.viewStart");
    const auto address = ParameterAddress{scope.toULongLong(), node.toULongLong(), "translateX"};
    const auto before = *session.document().animationChannel(address);
    const auto revision = session.revision();
    const auto from = slider->mapToScene(QPointF(slider->width() / 2, slider->height() / 2)).toPoint();
    drag(from, from + QPoint(28, 0));
    EXPECT_EQ(session.revision(), revision + 1);
    const auto edited = *session.document().animationChannel(address);
    EXPECT_NE(edited.keys[0].value, before.keys[0].value);
    click("animationTrackView");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(*session.document().animationChannel(address), before);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(*session.document().animationChannel(address), edited);
    EXPECT_EQ(controller.frame(), 12);
    EXPECT_EQ(js("animation.viewStart"), start);
    capture("inspector-workflow");
}

TEST_F(AnimationSurface, DenseTrackCurvesTangentsAndCompactDock) {
    Document document;
    const auto network = document.rootNetworkId();
    std::vector<KeyframeEdit> edits;
    for (int n = 0; n < 9; ++n) {
        const auto node = document.network(network).graph().addNode(n < 2 ? "transform" : "blur",
                                                                    "Animated " + std::to_string(n + 1));
        setLayoutCommand(network, node, {double(n % 3) * 150, double(n / 3) * 90}).apply(document);
        const std::vector<std::string> parameters =
            n == 0   ? std::vector<std::string>{"translateX", "translateY", "rotate", "scale"}
            : n == 1 ? std::vector<std::string>{"translateX", "rotate"}
                     : std::vector<std::string>{"size", "mix"};
        for (const auto& parameter : parameters) {
            for (int k = 0; k < 8; ++k) {
                const double value = parameter == "mix"     ? 0.15 + (k % 4) * 0.2
                                     : parameter == "scale" ? 0.5 + (k % 3) * 0.5
                                     : parameter == "size"  ? 2.0 + (k % 4) * 3
                                                            : double((k * 7 + n * 3) % 50) - 25;
                Keyframe frame{0, 1001.0 + k * 12, value};
                frame.interpolation = k % 2 == 0 ? KeyInterpolation::Bezier : KeyInterpolation::Linear;
                edits.push_back({{network, node, parameter}, frame});
            }
        }
    }
    setKeyframesCommand(std::move(edits)).apply(document);
    ASSERT_TRUE(session.replaceDocument(std::move(document)).replaced);
    // This legacy dense gesture fixture explicitly pins its nodes before
    // removing the inspector dock; filtering itself is exercised separately.
    ASSERT_TRUE(item("animationPanel")->setProperty("pinnedTargets", rootAnimationTargets(session.document())));
    QTest::qWait(30);
    const auto originalLayout = workspace.projectPresentation();
    for (const auto& type : {"viewer", "parameters", "nodegraph"}) {
        const auto other = panelByType(workspace.root(), type).value("id").toString();
        if (!other.isEmpty())
            workspace.closePanel(other);
    }
    window->setMaximumSize(QSize(1274, 640));
    window->resize(1274, 640);
    QTest::qWait(60);
    click("animationTrackView");
    click("animationFrameAll");
    ASSERT_EQ(js("animation.channels.length").toInt(), 20);
    EXPECT_EQ(js("animation.channels.reduce((n,c) => n+c.keys.length,0)").toInt(), 160);
    capture("dense-track");
    click("animationCurvesView");
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 20);
    capture("dense-curves");
    const auto denseStart = js("animation.viewStart");
    const auto denseEnd = js("animation.viewEnd");
    auto* firstNodeRow =
        item("animationNodeRow_" + controller.rootNetworkId() + "_" + js("animation.channels[0].nodeId").toString());
    const auto nodePoint = firstNodeRow->mapToScene(QPointF(70, 11)).toPoint();
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, nodePoint);
    QTest::qWait(20);
    click("animationIsolateCurves");
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 4);
    capture("dense-isolated");
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, nodePoint);
    QTest::qWait(20);
    click("animationShowAllCurves");
    auto* scroll = item("animationScroll");
    QQuickItem* scrollBar = nullptr;
    for (auto* child : scroll->childItems())
        if (child->inherits("QQuickScrollBar"))
            scrollBar = child;
    ASSERT_NE(scrollBar, nullptr);
    const auto halfThumb = scrollBar->property("size").toDouble() * scrollBar->height() / 2;
    const auto scrollFrom = scrollBar->mapToScene(QPointF(scrollBar->width() / 2, halfThumb)).toPoint();
    const auto scrollTo =
        scrollBar->mapToScene(QPointF(scrollBar->width() / 2, scrollBar->height() - halfThumb)).toPoint();
    drag(scrollFrom, scrollTo);
    EXPECT_GT(js("animation.scrollY").toDouble(), 0);
    EXPECT_EQ(js("animation.viewStart"), denseStart);
    EXPECT_EQ(js("animation.viewEnd"), denseEnd);
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 20);
    drag(scrollTo, scrollFrom);
    // Select a middle Bezier key with incoming/outgoing neighboring segments.
    auto* surface = item("animationSurface");
    const auto location = js("animation.keyPositions()[2]").toMap();
    const auto keyPoint =
        surface->mapToScene(QPointF(location.value("x").toDouble(), location.value("y").toDouble())).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, keyPoint);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, keyPoint);
    QTest::qWait(20);
    const auto chooseInterpolation = [&](const QString& name) {
        engine.globalObject().setProperty("keyMenu",
                                          engine.newQObject(window->findChild<QObject*>("animationKeyContextMenu")));
        auto* submenu = qobject_cast<QQuickItem*>(engine.evaluate("keyMenu.itemAt(1)").toQObject());
        ASSERT_NE(submenu, nullptr);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          submenu->mapToScene(QPointF(submenu->width() / 2, submenu->height() / 2)).toPoint());
        QTest::qWait(30);
        click(name);
    };
    chooseInterpolation("animationLinear");
    EXPECT_EQ(session.document().animationChannels()[0].keys[2].interpolation, KeyInterpolation::Linear);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, keyPoint);
    QTest::qWait(20);
    chooseInterpolation("animationBezier");
    EXPECT_EQ(session.document().animationChannels()[0].keys[2].interpolation, KeyInterpolation::Bezier);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, keyPoint);
    QTest::qWait(20);
    click("animationSmooth");
    const auto handles = js("animation.tangentPositions()").toList();
    ASSERT_FALSE(handles.isEmpty());
    const auto handle = handles.last().toMap();
    const auto handlePoint =
        surface->mapToScene(QPointF(handle.value("x").toDouble(), handle.value("y").toDouble())).toPoint();
    const auto before = session.document().animationChannels();
    drag(handlePoint, handlePoint + QPoint(0, -14));
    const auto changed = session.document().animationChannels();
    EXPECT_NE(changed, before);
    for (const auto& channel : changed)
        for (const auto& frame : channel.keys)
            if (frame.tangentMode == TangentMode::Smooth)
                EXPECT_EQ(frame.inSlope, frame.outSlope);
    const auto revision = session.revision();
    const auto nextHandle = js("animation.tangentPositions().slice(-1)[0]").toMap();
    const auto nextPoint =
        surface->mapToScene(QPointF(nextHandle.value("x").toDouble(), nextHandle.value("y").toDouble())).toPoint();
    drag(nextPoint, nextPoint + QPoint(0, 12), Qt::NoModifier, true);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(session.document().animationChannels(), changed);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, keyPoint);
    QTest::qWait(20);
    click("animationBroken");
    capture("dense-tangents");
    const auto brokenBefore = session.document().animationChannels();
    const auto brokenHandle = js("animation.tangentPositions().slice(-1)[0]").toMap();
    drag(surface->mapToScene(QPointF(brokenHandle.value("x").toDouble(), brokenHandle.value("y").toDouble())).toPoint(),
         surface->mapToScene(QPointF(brokenHandle.value("x").toDouble(), brokenHandle.value("y").toDouble() - 10))
             .toPoint());
    const auto brokenAfter = session.document().animationChannels();
    EXPECT_NE(brokenAfter, brokenBefore);
    for (std::size_t c = 0; c < brokenBefore.size(); ++c)
        for (std::size_t k = 0; k < brokenBefore[c].keys.size(); ++k)
            if (brokenAfter[c].keys[k] != brokenBefore[c].keys[k]) {
                EXPECT_EQ(brokenAfter[c].keys[k].inSlope, brokenBefore[c].keys[k].inSlope);
                EXPECT_NE(brokenAfter[c].keys[k].outSlope, brokenBefore[c].keys[k].outSlope);
            }
    drag(keyPoint, keyPoint + QPoint(18, 4), Qt::ShiftModifier);
    EXPECT_NE(session.document().animationChannels(), brokenAfter);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(session.document().animationChannels(), brokenAfter);
    // Restore two real docked panels, then position Animation in a narrow right tile.
    ASSERT_TRUE(workspace.applyProjectPresentation(originalLayout));
    QTest::qWait(40);
    for (const auto& type : {"viewer", "parameters"}) {
        const auto other = panelByType(workspace.root(), type).value("id").toString();
        if (!other.isEmpty())
            workspace.closePanel(other);
    }
    const auto graphId = panelByType(workspace.root(), "nodegraph").value("id").toString();
    const auto leafFor = [&](auto&& self, const QVariantMap& node) -> QString {
        for (const auto& panel : node.value("panels").toList())
            if (panel.toMap().value("id").toString() == graphId)
                return node.value("id").toString();
        for (const auto& child : node.value("children").toList()) {
            const auto found = self(self, child.toMap());
            if (!found.isEmpty())
                return found;
        }
        return {};
    };
    ASSERT_TRUE(workspace.movePanel(panelId, leafFor(leafFor, workspace.root()), "right", -1));
    QTest::qWait(40);
    auto* graphLeaf = item("leaf_" + leafFor(leafFor, workspace.root()));
    ASSERT_NE(graphLeaf, nullptr);
    const auto divider = graphLeaf->mapToScene(QPointF(graphLeaf->width() + 4, graphLeaf->height() / 2)).toPoint();
    drag(divider, QPoint(window->width() - 335, divider.y()));
    QTest::qWait(50);
    click("animationCurvesView");
    click("animationFrameAll");
    EXPECT_GE(item("animationSurface")->width(), 150);
    EXPECT_LE(item("animationPanel")->width(), 340);
    EXPECT_TRUE(item("animationEditorMenu")->isVisible());
    capture("narrow");
    ASSERT_TRUE(workspace.setAppearancePreset("Paper"));
    capture("narrow-paper");
}

TEST_F(AnimationSurface, CollapsedTrackBandMovesHiddenDescendantsAsOneEdit) {
    click("animationTrackView");
    click("animationFrameAll");
    const auto node = QString::number(session.document().animationChannels().front().address.node);
    auto* row = item("animationNodeRow_" + controller.rootNetworkId() + "_" + node);
    ASSERT_NE(row, nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, row->mapToScene(QPointF(10, 11)).toPoint());
    ASSERT_EQ(js("animation.rows.length").toInt(), 1);
    const auto before = session.document().animationChannels().front();
    auto* surface = item("animationSurface");
    const auto from =
        surface->mapToScene(QPointF(js("animation.timeToX(20)").toDouble(), js("animation.rowY(0)").toDouble()))
            .toPoint();
    const auto revision = session.revision();
    drag(from, from + QPoint(32, 0));
    const auto moved = session.document().animationChannels().front();
    EXPECT_EQ(session.revision(), revision + 1);
    EXPECT_NE(moved.keys[0].time, before.keys[0].time);
    EXPECT_DOUBLE_EQ(moved.keys[1].time - moved.keys[0].time, 20);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(session.document().animationChannels().front(), before);
}

TEST_F(AnimationSurface, BoxSelectionPanZoomAndTreeResizeDoNotCommit) {
    click("animationCurvesView");
    click("animationFrameAll");
    const auto revision = session.revision();
    const auto clock = controller.frame();
    drag(point(8.7, 1.1), point(31.3, -0.1));
    EXPECT_EQ(js("animation.selectedKeys().length").toInt(), 8);
    const auto start = js("animation.viewStart").toDouble();
    const auto from = point(20, 0.5);
    drag(from, from + QPoint(24, 10), Qt::NoModifier, false, Qt::MiddleButton);
    EXPECT_NE(js("animation.viewStart").toDouble(), start);
    auto* surface = item("animationSurface");
    const auto local = QPointF(surface->width() * 0.35, surface->height() * 0.5);
    const auto position = surface->mapToScene(local);
    const auto anchor = js(QString("animation.xToTime(%1)").arg(local.x())).toDouble();
    QWheelEvent wheel(position, window->mapToGlobal(position.toPoint()), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(window, &wheel);
    EXPECT_NEAR(js(QString("animation.xToTime(%1)").arg(local.x())).toDouble(), anchor, 1e-5);
    auto* divider = item("animationTreeDivider");
    const auto dividerStart = divider->mapToScene(QPointF(2, 40)).toPoint();
    const auto treeWidth = js("animation.treeWidth").toDouble();
    drag(dividerStart, dividerStart + QPoint(20, 0));
    EXPECT_NE(js("animation.treeWidth").toDouble(), treeWidth);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(controller.frame(), clock);
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 4);
}

TEST_F(AnimationSurface, InspectorFollowingPinsAndMasksSurviveGroupAndWorkspaceChanges) {
    click("closeAllInspectors");
    EXPECT_EQ(js("animation.channels.length").toInt(), 0);
    for (const auto& type : {"viewer", "nodegraph"}) {
        const auto id = panelByType(workspace.root(), type).value("id").toString();
        if (!id.isEmpty())
            workspace.closePanel(id);
    }
    window->setMaximumSize(QSize(1274, 640));
    window->resize(1274, 640);
    QTest::qWait(60);
    const auto scope = controller.rootNetworkId();
    const auto grade = controller.createGraphNode(scope, "grade", "Grade", 0, 0, {}, {});
    const auto blur = controller.createGraphNode(scope, "blur", "Blur", 160, 0, {}, {});
    ASSERT_FALSE(grade.isEmpty());
    ASSERT_FALSE(blur.isEmpty());
    ASSERT_TRUE(controller.keyNodeParameter(scope, grade, "multiply"));
    ASSERT_TRUE(controller.keyNodeParameter(scope, blur, "size"));
    ASSERT_TRUE(controller.keyNodeParameter(scope, blur, "mix"));
    EXPECT_EQ(js("animation.channels.length").toInt(), 0);
    ASSERT_TRUE(router.requestInspector("A", scope, grade));
    ASSERT_TRUE(router.requestInspector("A", scope, blur));
    QTest::qWait(40);
    ASSERT_EQ(js("animation.channels.length").toInt(), 6);
    EXPECT_EQ(js("animation.rows.filter(r => r.parent).length").toInt(), 2);
    click("animationCurvesView");
    click("animationFrameAll");
    const auto start = js("animation.viewStart");
    const auto end = js("animation.viewEnd");
    const auto revision = session.revision();
    const auto before = session.document().animationChannels();
    capture("follow-inspectors");
    click("collapse_" + grade);
    EXPECT_EQ(js("animation.channels.length").toInt(), 6);
    const auto red =
        js(QString(
               "animation.channels.find(c => c.nodeId === '%1' && c.parameter === 'multiply' && c.component === 0).id")
               .arg(grade))
            .toString();
    clickPin("animationPinChannel_" + red);
    click("close_" + grade);
    ASSERT_EQ(js("animation.channels.length").toInt(), 3);
    EXPECT_EQ(js(QString("animation.channels.filter(c => c.nodeId === '%1').length").arg(grade)).toInt(), 1);
    capture("inspector-pins");
    auto* row = item("animationParameterRow_" + scope + "_" + grade + "_multiply.R");
    ASSERT_NE(row, nullptr);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      row->mapToScene(QPointF(45, row->height() / 2)).toPoint());
    QTest::qWait(20);
    click("animationHideCurves");
    EXPECT_FALSE(js(QString("animation.curveVisible('%1')").arg(red)).toBool());
    clickPin("animationPinNode_" + scope + "_" + blur);
    click("close_" + blur);
    ASSERT_EQ(js("animation.channels.length").toInt(), 3);
    EXPECT_EQ(js("animation.channels.filter(c => animation.curveVisible(c.id)).length").toInt(), 2);
    capture("only-pins");
    workspace.setGroup(panelId, "B");
    QTest::qWait(40);
    EXPECT_EQ(js("animation.channels.length").toInt(), 0);
    workspace.setGroup(panelId, "A");
    QTest::qWait(40);
    EXPECT_EQ(js("animation.channels.length").toInt(), 3);
    const auto original = workspace.activeWorkspaceId();
    const auto other = workspace.createWorkspace("Other");
    ASSERT_FALSE(other.isEmpty());
    ASSERT_TRUE(workspace.switchWorkspace(other));
    QTest::qWait(60);
    ASSERT_TRUE(workspace.switchWorkspace(original));
    QTest::qWait(100);
    ASSERT_EQ(js("animation.channels.length").toInt(), 3);
    EXPECT_FALSE(js(QString("animation.curveVisible('%1')").arg(red)).toBool());
    clickPin("animationPinChannel_" + red);
    EXPECT_EQ(js("animation.channels.length").toInt(), 2);
    clickPin("animationPinNode_" + scope + "_" + blur);
    EXPECT_EQ(js("animation.channels.length").toInt(), 0);
    EXPECT_EQ(js("animation.viewStart"), start);
    EXPECT_EQ(js("animation.viewEnd"), end);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(session.document().animationChannels(), before);
}
TEST_F(AnimationSurface, RestoringPresentationRetainsInspectorMembershipAndPinnedChannels) {
    const auto red = js("animation.channels.find(c => c.component === 0).id").toString();
    clickPin("animationPinChannel_" + red);
    const auto saved = workspace.projectPresentation();
    ASSERT_TRUE(QMetaObject::invokeMethod(item("parametersPanel"), "closeAllInspectors"));
    QTest::qWait(30);
    clickPin("animationPinChannel_" + red);
    ASSERT_EQ(js("animation.channels.length").toInt(), 0);

    ASSERT_TRUE(workspace.applyProjectPresentation(saved));
    QTest::qWait(100);
    ASSERT_EQ(item("parametersPanel")->property("inspectors").toList().size(), 1);
    ASSERT_EQ(js("animation.channels.length").toInt(), 4);
    ASSERT_TRUE(QMetaObject::invokeMethod(item("parametersPanel"), "closeAllInspectors"));
    QTest::qWait(30);
    EXPECT_EQ(js("animation.channels.length").toInt(), 1);
    EXPECT_EQ(js("animation.channels[0].id").toString(), red);
}
}  // namespace
