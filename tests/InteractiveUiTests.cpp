#include "ParameterEditorRegistry.hpp"
#include "ParameterInteraction.hpp"
#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerItem.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/gpu/Error.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QJSEngine>
#include <QJSValue>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

nemo::Graph& rootGraph(nemo::Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const nemo::Graph& rootGraph(const nemo::Document& document) {
    return document.network(document.rootNetworkId()).graph();
}
nemo::Document emptyDocument() {
    nemo::Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    return document;
}
namespace {

QVariantMap namedNode(const nemo::ui::ViewerController& controller, const QString& name) {
    for (const auto& value : controller.graphNodes()) {
        const auto node = value.toMap();
        if (node.value("name").toString() == name)
            return node;
    }
    return {};
}
// No decoder is needed for command semantics: the unstarted runtime accepts
// immutable probe snapshots, but all observed edits use the real controller
// and its explicitly composed ProjectSession, exactly as QML does.
TEST(Interactive, GraphCommandsUndoAndReplaceOccupiedConnectionsAtomically) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    controller.setNodeParameter(namedNode(controller, "background").value("id").toString(), "color",
                                QVariantList{QVariant{0.2}, QVariant{0.3}, QVariant{0.4}, QVariant{1.0}});
    const auto color = namedNode(controller, "background").value("params").toMap().value("color").toList();
    ASSERT_EQ(color.size(), 4);
    EXPECT_FLOAT_EQ(color.at(0).toFloat(), 0.2F);
    EXPECT_FLOAT_EQ(color.at(1).toFloat(), 0.3F);
    EXPECT_FLOAT_EQ(color.at(2).toFloat(), 0.4F);
    EXPECT_FLOAT_EQ(color.at(3).toFloat(), 1.0F);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    const auto resetColor = namedNode(controller, "background").value("params").toMap().value("color").toList();
    ASSERT_EQ(resetColor.size(), 4);
    EXPECT_FLOAT_EQ(resetColor.at(0).toFloat(), 0.0F);
    EXPECT_FLOAT_EQ(resetColor.at(1).toFloat(), 0.0F);
    EXPECT_FLOAT_EQ(resetColor.at(2).toFloat(), 0.0F);
    EXPECT_FLOAT_EQ(resetColor.at(3).toFloat(), 0.0F);

    const auto originalEdges = controller.graphEdges();
    ASSERT_TRUE(controller.connectOrReplaceGraph(QString::number(session.document().rootNetworkId()),
                                                 namedNode(controller, "background").value("id").toString(), 0,
                                                 namedNode(controller, "composite").value("id").toString(), 0));
    EXPECT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    EXPECT_NE(controller.graphEdges(), originalEdges);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.graphEdges(), originalEdges);
    ASSERT_TRUE(session.redo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_NE(controller.graphEdges(), originalEdges);
}

TEST(Interactive, GraphSnapshotPublishesAuthoredPositionsPortsRoutesAndStableIds) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto snapshot = controller.graphSnapshot(scope);
    EXPECT_TRUE(snapshot.value("available").toBool());
    EXPECT_EQ(snapshot.value("networkId").toString(), scope);
    EXPECT_FALSE(controller.graphSnapshot("999999").value("available").toBool());
    const auto sourceId = controller.createGraphNode(scope, "source", "stableSource", 120.5, 240.25, {}, {});
    const auto outputId = controller.createGraphNode(scope, "output", "stableOutput", 420.0, 240.25, {}, {});
    ASSERT_FALSE(sourceId.isEmpty());
    ASSERT_FALSE(outputId.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, sourceId, 0, outputId, 0));

    const auto source = namedNode(controller, "stableSource");
    ASSERT_EQ(source.value("id").toString(), sourceId);
    EXPECT_DOUBLE_EQ(source.value("x").toDouble(), 120.5);
    EXPECT_DOUBLE_EQ(source.value("y").toDouble(), 240.25);

    ASSERT_EQ(controller.graphEdges().size(), 1);
    const auto edge = controller.graphEdges().first().toMap();
    const auto edgeId = edge.value("id").toString();
    ASSERT_TRUE(controller.commitGraphRoute(
        scope, edgeId, QVariantList{QVariantMap{{"x", 260.0}, {"y", 180.0}}, QVariantMap{{"x", 320.0}, {"y", 300.0}}}));
    ASSERT_TRUE(
        controller.commitGraphMove(scope, QVariantList{QVariantMap{{"id", sourceId}, {"x", 140.0}, {"y", 260.0}}}));
    EXPECT_EQ(namedNode(controller, "stableSource").value("id").toString(), sourceId);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_DOUBLE_EQ(namedNode(controller, "stableSource").value("x").toDouble(), 120.5);
}

TEST(Interactive, GraphScopeEditsDoNotFallBackToRoot) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto root = controller.rootNetworkId();
    const auto rootBefore = controller.graphSnapshot(root);
    const auto created = std::make_shared<nemo::NetworkId>();
    ASSERT_TRUE(session
                    .submit(nemo::addNetworkCommand("other", created),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);
    const auto other = QString::number(*created);
    const auto otherBefore = controller.graphSnapshot(other);
    const auto node = controller.createGraphNode(other, "constcolor", "Scoped", -40, 60, {}, {});
    ASSERT_FALSE(node.isEmpty());
    EXPECT_EQ(controller.graphSnapshot(root), rootBefore);
    EXPECT_NE(controller.graphSnapshot(other), otherBefore);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.graphSnapshot(other), otherBefore);
    const auto revision = session.revision();
    EXPECT_TRUE(controller.createGraphNode("999999", "constcolor", "Invalid", 0, 0, {}, {}).isEmpty());
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(controller.graphSnapshot(root), rootBefore);
}

TEST(Interactive, CollapseSelectionReturnsCommittedInstanceNodeAndRestoresAtomically) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto source = controller.createGraphNode(scope, "source", "collapseSource", 0.0, 0.0, {}, {});
    const auto merge = controller.createGraphNode(scope, "merge", "collapseMerge", 140.0, 0.0, {}, {});
    const auto output = controller.createGraphNode(scope, "output", "collapseOutput", 280.0, 0.0, {}, {});
    ASSERT_FALSE(source.isEmpty());
    ASSERT_FALSE(merge.isEmpty());
    ASSERT_FALSE(output.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, source, 0, merge, 0));
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, merge, 0, output, 0));

    const auto subnet = controller.collapseSelection(scope, QVariantList{source, merge}, "Collapsed");
    ASSERT_FALSE(subnet.isEmpty()) << controller.error().toStdString();
    const auto parentNode = namedNode(controller, "Collapsed");
    ASSERT_EQ(parentNode.value("id").toString(), subnet);
    ASSERT_FALSE(parentNode.value("definition").toString().isEmpty());
    ASSERT_FALSE(parentNode.value("instance").toString().isEmpty());
    const auto instanceId = parentNode.value("instance").toString().toULongLong();
    const auto* instance = session.document().instance(instanceId);
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->node, subnet.toULongLong());
    EXPECT_TRUE(controller.graphSnapshot(parentNode.value("definition").toString()).value("available").toBool());

    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_TRUE(namedNode(controller, "Collapsed").isEmpty());
    EXPECT_EQ(session.document().instances().size(), 0U);
    ASSERT_TRUE(session.redo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(namedNode(controller, "Collapsed").value("id").toString(), subnet);
}
TEST(Interactive, DisconnectedProcessingNodeDropsOntoWireAtomically) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto sourceId = controller.createGraphNode(scope, "source", "wireSource", 0.0, 0.0, {}, {});
    const auto outputId = controller.createGraphNode(scope, "output", "wireOutput", 300.0, 0.0, {}, {});
    const auto mergeId = controller.createGraphNode(scope, "merge", "wireMerge", 140.0, 120.0, {}, {});
    ASSERT_FALSE(sourceId.isEmpty());
    ASSERT_FALSE(outputId.isEmpty());
    ASSERT_FALSE(mergeId.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, sourceId, 0, outputId, 0));
    ASSERT_EQ(controller.graphEdges().size(), 1);
    const auto originalEdge = controller.graphEdges().first().toMap();
    ASSERT_TRUE(
        controller.insertExistingGraphNodeOnEdge(scope, mergeId, originalEdge.value("id").toString(), 140.0, 0.0));
    ASSERT_EQ(controller.graphEdges().size(), 2);
    const auto inserted = namedNode(controller, "wireMerge");
    EXPECT_DOUBLE_EQ(inserted.value("x").toDouble(), 140.0);
    EXPECT_DOUBLE_EQ(inserted.value("y").toDouble(), 0.0);
    EXPECT_EQ(controller.graphEdges().first().toMap().value("fromNode").toString(), sourceId);
    EXPECT_EQ(controller.graphEdges().last().toMap().value("toNode").toString(), outputId);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.graphEdges().size(), 1);
    EXPECT_TRUE(namedNode(controller, "wireMerge").value("id").toString() == mergeId);
}

TEST(Interactive, GraphCreationUndoPreservesExistingConnections) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    const auto before = controller.graphEdges();
    const auto scope = QString::number(session.document().rootNetworkId());
    ASSERT_FALSE(controller.createGraphNode(scope, "output", "independentOutput", 0.0, 0.0, {}, {}).isEmpty());
    const auto output = namedNode(controller, "independentOutput");
    ASSERT_FALSE(output.isEmpty());
    EXPECT_TRUE(
        controller.connectOrReplaceGraph(scope, namedNode(controller, "source").value("id"), 0, output.value("id"), 0));
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.graphEdges(), before);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_TRUE(namedNode(controller, "independentOutput").isEmpty());
    EXPECT_EQ(controller.graphEdges(), before);
}

TEST(Interactive, TimelineSlipAndRetimeUseSourceMappingAndUndoIndependently) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    controller.setFrame(3);
    controller.slipTimelineClip("src", 7);
    controller.retimeTimelineClip("src", 2);
    ASSERT_EQ(controller.timelineClips().size(), 1);
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 13);
    EXPECT_EQ(controller.frame(), 3) << "Source timing edits must not move the playhead";
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 10);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 3);
    controller.retimeTimelineClip("src", 0);
    EXPECT_FALSE(controller.error().isEmpty());
    ASSERT_TRUE(session.redo(nemo::EditOptions{.expectedRevision = session.revision()}).committed)
        << "Invalid timing must preserve command history";
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 10);
}

TEST(Interactive, UndoingSourceImportCancelsProbeAndRedoRequestsFreshMetadata) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    // A probe/render consumer owns a scheduler destination (issue #47); an
    // unassigned controller is a command-only facade and never probes.
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    ASSERT_TRUE(controller.pending());
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_TRUE(controller.graphNodes().isEmpty());
    EXPECT_FALSE(controller.pending());
    EXPECT_FALSE(controller.hasSource());
    EXPECT_EQ(runtime.counts().queued, 0u);

    ASSERT_TRUE(session.redo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_TRUE(controller.pending());
    EXPECT_EQ(namedNode(controller, "source").value("params").toMap().value("source").toString(), "src");
    EXPECT_EQ(runtime.counts().queued, 1u);
}

TEST(Interactive, ViewerAssignmentAttachesTargetAndIsOneUndoableCommand) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto colorId = controller.createGraphNode(scope, "constcolor", "viewerColor", 0.0, 0.0, {}, {});
    const auto mergeId = controller.createGraphNode(scope, "merge", "viewerMerge", 120.0, 0.0, {}, {});
    ASSERT_FALSE(colorId.isEmpty());
    ASSERT_FALSE(mergeId.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, colorId, 0, mergeId, 0));

    EXPECT_EQ(controller.viewerCount(scope), 0);
    EXPECT_TRUE(controller.viewerTargetId().isEmpty());
    ASSERT_TRUE(controller.assignViewer(scope, 0, mergeId));
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    EXPECT_EQ(controller.viewerCount(scope), 1);
    const auto attachment = controller.viewerAttachment(scope, 0);
    EXPECT_EQ(attachment.value(QStringLiteral("index")).toInt(), 0);
    EXPECT_EQ(attachment.value(QStringLiteral("viewerName")).toString(), QStringLiteral("Viewer1"));
    EXPECT_FALSE(attachment.value(QStringLiteral("viewerId")).toString().isEmpty());
    EXPECT_EQ(attachment.value(QStringLiteral("attachedId")).toString(), mergeId);
    EXPECT_EQ(attachment.value(QStringLiteral("attachedName")).toString(), QStringLiteral("viewerMerge"));
    // The default active viewer is index 0 of the root network, so the shared
    // controller renders the attached node without an explicit activation.
    EXPECT_EQ(controller.viewerTargetId(), mergeId);
    EXPECT_EQ(controller.viewerTargetName(), QStringLiteral("viewerMerge"));

    // Creation and attachment are one command: undo removes the viewer again.
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.viewerCount(scope), 0);
    EXPECT_TRUE(controller.viewerTargetId().isEmpty());
    ASSERT_TRUE(session.redo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.viewerTargetId(), mergeId);
}

TEST(Interactive, RoutedMediaCanvasDoesNotReuseGraphScopeFormat) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native viewer evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    auto document = emptyDocument();
    document.network(document.rootNetworkId()).setFormat({1280, 720, 1.0F});
    const auto child = document.addNetwork("Anamorphic");
    document.network(child).setFormat({2048, 858, 1.5F});
    const auto color = document.network(child).graph().addNode("constcolor", "Color");
    nemo::assignViewerCommand(child, 0, color).apply(document);
    document.sources["plate"].path = "not-probed.exr";
    nemo::ui::ViewerRuntime runtime;
    nemo::eval::ViewerCacheOptions options;
    options.directory = directory.path().toStdString();
    options.encoding.codec = "libx264-cpu";
    try {
        runtime.bootstrap({"VK_KHR_surface"}, NEMO_SLANG_SPV_DIR, options);
    } catch (const nemo::gpu::GpuException& error) {
        if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }
    nemo::ProjectSession session{std::move(document)};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto awaitFormat = [&](QSizeF format) {
        QElapsedTimer deadline;
        deadline.start();
        while (controller.compositionSize() != format && deadline.elapsed() < 60000)
            QTest::qWait(10);
        return controller.compositionSize() == format;
    };

    controller.setActiveViewer(QString::number(child), 0);
    ASSERT_TRUE(awaitFormat(QSizeF(2048, 858)));
    // No viewport is attached, so only descriptions run. An unavailable
    // routed source must not borrow either network's format.
    controller.setViewerContext(QStringLiteral("media"), QStringLiteral("plate"), 0);
    EXPECT_FALSE(controller.compositionSize().isValid());
    QElapsedTimer deadline;
    deadline.start();
    while (controller.error().isEmpty() && deadline.elapsed() < 60000)
        QTest::qWait(10);
    ASSERT_FALSE(controller.error().isEmpty());
    EXPECT_TRUE(controller.error().contains(QStringLiteral("not-probed.exr")));
    EXPECT_FALSE(controller.compositionSize().isValid());
    controller.setViewerContext(QStringLiteral("graph"), {}, 0);
    ASSERT_TRUE(awaitFormat(QSizeF(2048, 858)));
#endif
}

TEST(Interactive, ViewerIndicesFollowNodeIdOrderAndActiveViewerSelectsTarget) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto colorId = controller.createGraphNode(scope, "constcolor", "orderedColor", 0.0, 0.0, {}, {});
    const auto mergeId = controller.createGraphNode(scope, "merge", "orderedMerge", 120.0, 0.0, {}, {});
    ASSERT_FALSE(colorId.isEmpty());
    ASSERT_FALSE(mergeId.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, colorId, 0, mergeId, 0));

    // Assigning index 1 creates Viewer1 and Viewer2 in order; only the second
    // carries the attachment.
    ASSERT_TRUE(controller.assignViewer(scope, 1, mergeId));
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    EXPECT_EQ(controller.viewerCount(scope), 2);
    EXPECT_TRUE(controller.viewerAttachment(scope, 0).value(QStringLiteral("attachedId")).toString().isEmpty());
    EXPECT_EQ(controller.viewerAttachment(scope, 1).value(QStringLiteral("attachedId")).toString(), mergeId);
    EXPECT_TRUE(controller.viewerTargetId().isEmpty()) << "Active viewer 0 has no attachment";

    controller.setActiveViewer(scope, 1);
    EXPECT_EQ(controller.viewerTargetId(), mergeId);
    EXPECT_EQ(controller.viewerTargetName(), QStringLiteral("orderedMerge"));

    controller.setActiveViewer(scope, 0);
    EXPECT_TRUE(controller.viewerTargetId().isEmpty());
    EXPECT_TRUE(controller.viewerTargetName().isEmpty());
}

TEST(Interactive, ViewerDetachAndNodeDeletionLeaveEmptyWithoutOutputFallback) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto colorId = controller.createGraphNode(scope, "constcolor", "detachColor", 0.0, 0.0, {}, {});
    const auto outputId = controller.createGraphNode(scope, "output", "detachOutput", 220.0, 0.0, {}, {});
    ASSERT_FALSE(colorId.isEmpty());
    ASSERT_FALSE(outputId.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, colorId, 0, outputId, 0));
    ASSERT_TRUE(controller.assignViewer(scope, 0, colorId));
    ASSERT_EQ(controller.viewerTargetId(), colorId);

    // An empty node identity detaches the viewer. The Output node still exists
    // and is the network's consumption result; it must not become the target.
    ASSERT_TRUE(controller.assignViewer(scope, 0, QString()));
    EXPECT_TRUE(controller.viewerTargetId().isEmpty());
    EXPECT_TRUE(controller.viewerTargetName().isEmpty());
    EXPECT_TRUE(controller.viewerAttachment(scope, 0).value(QStringLiteral("attachedId")).toString().isEmpty());
    EXPECT_TRUE(controller.compositionSize().isEmpty());
    EXPECT_EQ(controller.status(), QStringLiteral("No viewer target"));

    // Re-attach, then delete the attached node: incident-edge removal empties
    // the viewer with the same explicit state.
    ASSERT_TRUE(controller.assignViewer(scope, 0, colorId));
    ASSERT_EQ(controller.viewerTargetId(), colorId);
    ASSERT_TRUE(controller.deleteGraphNodes(scope, QVariantList{colorId}));
    EXPECT_TRUE(controller.viewerTargetId().isEmpty());
    EXPECT_TRUE(controller.compositionSize().isEmpty());
    EXPECT_EQ(controller.status(), QStringLiteral("No viewer target"));
}

TEST(Interactive, MediaFreeViewerRendersAttachedComposite) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native viewer evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    nemo::ui::ViewerRuntime runtime;
    nemo::eval::ViewerCacheOptions options;
    options.directory = directory.path().toStdString();
    options.encoding.codec = "libx264-cpu";
    options.chunkFrames = 1;
    try {
        runtime.bootstrap({"VK_KHR_surface"}, NEMO_SLANG_SPV_DIR, options);
    } catch (const nemo::gpu::GpuException& error) {
        if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }
    // A media-free graph: no source is imported, only generators and a merge.
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    // The render consumer owns a scheduler destination (issue #47).
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto colorA = controller.createGraphNode(scope, "constcolor", "canvasA", 0.0, 0.0, {}, {});
    const auto colorB = controller.createGraphNode(scope, "constcolor", "canvasB", 0.0, 80.0, {}, {});
    const auto mergeId = controller.createGraphNode(scope, "merge", "canvasMerge", 140.0, 40.0, {}, {});
    ASSERT_FALSE(colorA.isEmpty());
    ASSERT_FALSE(colorB.isEmpty());
    ASSERT_FALSE(mergeId.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, colorA, 0, mergeId, 0));
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, colorB, 0, mergeId, 1));
    ASSERT_FALSE(controller.hasSource());
    ASSERT_TRUE(controller.assignViewer(scope, 0, mergeId));
    controller.setResolutionMode("quarter");
    controller.viewportChanged(QSizeF(320.0, 240.0));

    QElapsedTimer deadline;
    deadline.start();
    while (!controller.presentation() && controller.error().isEmpty() && deadline.elapsed() < 60000)
        QTest::qWait(10);
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.presentation());
    EXPECT_EQ(controller.presentation()->request.output, static_cast<nemo::NodeId>(mergeId.toULongLong()));
    EXPECT_EQ(controller.presentation()->request.imageWidth(), 1920);
    EXPECT_EQ(controller.presentation()->request.imageHeight(), 1080);
    EXPECT_FALSE(controller.presentation()->frame.width <= 0);
#endif
}

// Playback produces one request per DISPLAYED frame. Nothing consumes the
// submitted request here (the runtime is not bootstrapped, so no worker runs),
// which is exactly the condition the free-running tick mishandled: it submitted
// a fresh frame every interval, superseding the frame still being computed, so
// completed frames were discarded instead of shown. The transport must instead
// leave one frame outstanding and wait for it.
TEST(Interactive, PlaybackWaitsForTheFrameItSubmitted) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto color = controller.createGraphNode(scope, "constcolor", "playbackColor", 0.0, 0.0, {}, {});
    ASSERT_FALSE(color.isEmpty());
    ASSERT_TRUE(controller.assignViewer(scope, 0, color));
    controller.viewportChanged(QSizeF(320.0, 240.0));

    controller.setFrameRate(25.0);
    const int start = controller.frame();
    controller.play();
    // Five playback intervals at 25 fps: a clock-driven transport would have
    // issued five requests and discarded four of them inside this window.
    QTest::qWait(200);

    const auto counts = runtime.counts(nemo::eval::ViewerDestination::Interactive);
    // The frame being computed was never superseded by its own successor...
    EXPECT_EQ(counts.dropped, 0u) << "playback discarded a frame it had submitted";
    // ...so the transport waited for it instead of running ahead: five playback
    // intervals of wall clock produced no further submission, and the panel
    // stayed on the frame it is waiting to display.
    EXPECT_EQ(counts.queued, 1u) << "a second frame was submitted while one was outstanding";
    EXPECT_EQ(controller.frame(), start) << "the transport ran ahead of the frame being displayed";
    controller.pause();
}

// The other half of the same contract, on a real device: frames that finish do
// reach the viewer, and the transport advances one frame per DISPLAYED frame
// rather than one per timer tick. Skips without a usable Vulkan device.
TEST(Interactive, PlaybackPublishesEveryFrameItRenders) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native viewer evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    nemo::eval::ViewerCacheOptions options;
    options.directory = directory.path().toStdString();
    options.encoding.codec = "libx264-cpu";
    options.chunkFrames = 4;
    nemo::ui::ViewerRuntime runtime;
    try {
        runtime.bootstrap({"VK_KHR_surface"}, NEMO_SLANG_SPV_DIR, options);
    } catch (const nemo::gpu::GpuException& error) {
        if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto color = controller.createGraphNode(scope, "constcolor", "playbackColor", 0.0, 0.0, {}, {});
    ASSERT_FALSE(color.isEmpty());
    ASSERT_TRUE(controller.assignViewer(scope, 0, color));
    // A small raster so an uncached frame can fit inside a playback interval.
    controller.setResolutionMode("quarter");
    controller.viewportChanged(QSizeF(320.0, 240.0));

    QElapsedTimer deadline;
    deadline.start();
    while (!controller.presentation() && controller.error().isEmpty() && deadline.elapsed() < 60000)
        QTest::qWait(10);
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.presentation());

    QSignalSpy arrived(&controller, &nemo::ui::ViewerController::frameArrived);
    ASSERT_TRUE(arrived.isValid());
    controller.setFrameRate(24.0);
    const int start = controller.frame();
    controller.play();
    while (arrived.count() < 5 && deadline.restart() < 60000 && controller.error().isEmpty())
        QTest::qWait(10);
    controller.pause();

    const auto counts = runtime.counts(nemo::eval::ViewerDestination::Interactive);
    EXPECT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    // Five frames were computed, published and displayed across five intervals.
    EXPECT_GE(arrived.count(), 5);
    // None was thrown away because its own successor reached the scheduler.
    EXPECT_EQ(counts.staleRejected, 0u);
    EXPECT_EQ(counts.dropped, 0u);
    // The transport advanced once per displayed frame, not once per tick.
    EXPECT_EQ(controller.frame(), start + arrived.count());
#endif
}

TEST(Interactive, CacheRangeSupersedingDescriptionDoesNotStallViewer) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native viewer evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    nemo::eval::ViewerCacheOptions options;
    options.directory = directory.path().toStdString();
    options.encoding.codec = "libx264-cpu";
    options.chunkFrames = 1;
    nemo::ui::ViewerRuntime runtime;
    try {
        runtime.bootstrap({"VK_KHR_surface"}, NEMO_SLANG_SPV_DIR, options);
    } catch (const nemo::gpu::GpuException& error) {
        if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto scope = QString::number(session.document().rootNetworkId());
    const auto color = controller.createGraphNode(scope, "constcolor", "rangeColor", 0.0, 0.0, {}, {});
    ASSERT_FALSE(color.isEmpty());
    ASSERT_TRUE(controller.assignViewer(scope, 0, color));
    controller.setResolutionMode("quarter");
    controller.viewportChanged(QSizeF(320.0, 240.0));
    QElapsedTimer deadline;
    deadline.start();
    while (!controller.presentation() && controller.error().isEmpty() && deadline.elapsed() < 10000)
        QTest::qWait(10);
    ASSERT_TRUE(controller.presentation()) << controller.error().toStdString();
    ASSERT_EQ(controller.presentation()->request.localTime, 0);

    // No GUI event delivery between these calls: frame 1's description has
    // not been consumed when the range supersedes it. A later view refresh
    // must request the metadata again instead of waiting for the lost answer.
    controller.setFrame(1);
    controller.requestRange(0, 0);
    controller.setResolutionMode("half");
    deadline.restart();
    while (controller.presentation()->request.localTime != 1 && controller.error().isEmpty() &&
           deadline.elapsed() < 10000)
        QTest::qWait(10);
    EXPECT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    EXPECT_EQ(controller.presentation()->request.localTime, 1);
#endif
}

// Issue #47's focused multi-destination scenario: two viewer panels submit
// different targets through one runtime, a result for destination A cannot
// replace B's display, rapid supersession keeps only the newest request per
// destination, and retiring A leaves B rendering.
TEST(Interactive, TwoViewerDestinationsRenderIndependentlyAndSurviveRetirement) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native multi-destination evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    nemo::ui::ViewerRuntime runtime;
    nemo::eval::ViewerCacheOptions options;
    options.directory = directory.path().toStdString();
    options.encoding.codec = "libx264-cpu";
    options.chunkFrames = 1;
    try {
        runtime.bootstrap({"VK_KHR_surface"}, NEMO_SLANG_SPV_DIR, options);
    } catch (const nemo::gpu::GpuException& error) {
        if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }

    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController viewerA(&runtime, session, interaction);
    nemo::ui::ViewerController viewerB(&runtime, session, interaction);
    const auto destinationA = runtime.allocateDestination(QStringLiteral("scenario-a"));
    const auto destinationB = runtime.allocateDestination(QStringLiteral("scenario-b"));
    ASSERT_TRUE(destinationA.has_value());
    ASSERT_TRUE(destinationB.has_value());
    EXPECT_NE(*destinationA, *destinationB);
    // Both destinations are panel instances, never the reserved defaults.
    EXPECT_NE(*destinationA, nemo::eval::ViewerDestination::Interactive);
    EXPECT_NE(*destinationA, nemo::eval::ViewerDestination::Cache);
    viewerA.setDestination(*destinationA);
    viewerB.setDestination(*destinationB);

    const auto scope = QString::number(session.document().rootNetworkId());
    const auto colorA = viewerA.createGraphNode(scope, "constcolor", "destColorA", 0.0, 0.0, {}, {});
    const auto colorB = viewerA.createGraphNode(scope, "constcolor", "destColorB", 0.0, 80.0, {}, {});
    const auto viewerNodeA = viewerA.createGraphNode(scope, "viewer", "DestViewerA", 0.0, 160.0, {}, {});
    const auto viewerNodeB = viewerA.createGraphNode(scope, "viewer", "DestViewerB", 0.0, 240.0, {}, {});
    ASSERT_FALSE(colorA.isEmpty());
    ASSERT_FALSE(colorB.isEmpty());
    ASSERT_FALSE(viewerNodeA.isEmpty());
    ASSERT_FALSE(viewerNodeB.isEmpty());
    ASSERT_TRUE(viewerA.connectOrReplaceGraph(scope, colorA, 0, viewerNodeA, 0));
    ASSERT_TRUE(viewerA.connectOrReplaceGraph(scope, colorB, 0, viewerNodeB, 0));
    // Viewer nodes are addressed in ascending NodeId order, so panel A renders
    // the first viewer's upstream node and panel B the second's.
    viewerA.setActiveViewer(scope, 0);
    viewerB.setActiveViewer(scope, 1);
    viewerA.setResolutionMode("quarter");
    viewerB.setResolutionMode("quarter");
    viewerA.viewportChanged(QSizeF(320.0, 240.0));
    viewerB.viewportChanged(QSizeF(200.0, 160.0));

    const auto settle = [](nemo::ui::ViewerController& viewer, int frame) {
        QElapsedTimer deadline;
        deadline.start();
        while ((!viewer.presentation() || viewer.presentation()->request.localTime != frame) &&
               viewer.error().isEmpty() && deadline.elapsed() < 60000)
            QTest::qWait(10);
    };
    settle(viewerA, 0);
    settle(viewerB, 0);
    ASSERT_TRUE(viewerA.error().isEmpty()) << viewerA.error().toStdString();
    ASSERT_TRUE(viewerB.error().isEmpty()) << viewerB.error().toStdString();
    ASSERT_TRUE(viewerA.presentation());
    ASSERT_TRUE(viewerB.presentation());
    EXPECT_EQ(viewerA.presentation()->request.output, static_cast<nemo::NodeId>(colorA.toULongLong()));
    EXPECT_EQ(viewerB.presentation()->request.output, static_cast<nemo::NodeId>(colorB.toULongLong()));
    const auto bRequestId = viewerB.presentation()->requestId;

    // Rapid supersession on one destination must leave the other untouched and
    // publish only the newest request for the edited destination.
    viewerA.setFrame(1);
    viewerA.setFrame(2);
    viewerA.setFrame(3);
    settle(viewerA, 3);
    ASSERT_TRUE(viewerA.presentation());
    EXPECT_EQ(viewerA.presentation()->request.localTime, 3);
    ASSERT_TRUE(viewerB.presentation());
    EXPECT_EQ(viewerB.presentation()->requestId, bRequestId);

    // Closing panel A retires only its destination: B's presentation survives
    // and B can still render a new frame afterwards.
    ASSERT_TRUE(runtime.retireDestination(*destinationA));
    viewerA.setDestination(std::nullopt);
    ASSERT_TRUE(viewerB.presentation());
    // Retiring A's destination must not disturb B's current presentation.
    EXPECT_EQ(viewerB.presentation()->requestId, bRequestId);
    viewerB.setFrame(1);
    settle(viewerB, 1);
    ASSERT_TRUE(viewerB.error().isEmpty()) << viewerB.error().toStdString();
    ASSERT_TRUE(viewerB.presentation());
    EXPECT_EQ(viewerB.presentation()->request.localTime, 1);
    EXPECT_NE(viewerB.presentation()->requestId, bRequestId);
#endif
}

TEST(Interactive, ViewerControllerRegistryAllocatesIndependentDestinations) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerControllerRegistry registry(&runtime, session, interaction);
    auto* first = qobject_cast<nemo::ui::ViewerController*>(registry.controller(QStringLiteral("panel-a")));
    auto* second = qobject_cast<nemo::ui::ViewerController*>(registry.controller(QStringLiteral("panel-b")));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first, second);
    EXPECT_EQ(registry.activeCount(), 2);
    // The same panel always resolves to the same controller.
    EXPECT_EQ(registry.controller(QStringLiteral("panel-a")), first);
    ASSERT_TRUE(first->destination().has_value());
    ASSERT_TRUE(second->destination().has_value());
    EXPECT_NE(*first->destination(), *second->destination());
    // Panel destinations are distinct from the reserved scheduler streams.
    EXPECT_NE(*first->destination(), nemo::eval::ViewerDestination::Interactive);
    EXPECT_NE(*first->destination(), nemo::eval::ViewerDestination::Cache);
    EXPECT_NE(*second->destination(), nemo::eval::ViewerDestination::Interactive);
    EXPECT_NE(*second->destination(), nemo::eval::ViewerDestination::Cache);
    EXPECT_EQ(registry.primary(), first);

    // Retiring one panel leaves the other's controller and destination live.
    registry.release(QStringLiteral("panel-a"));
    EXPECT_EQ(registry.activeCount(), 1);
    EXPECT_EQ(registry.primary(), second);
    EXPECT_FALSE(first->destination().has_value());
    EXPECT_TRUE(second->destination().has_value());

    registry.release(QStringLiteral("panel-b"));
    EXPECT_EQ(registry.activeCount(), 0);
    EXPECT_EQ(registry.primary(), nullptr);
    EXPECT_FALSE(second->destination().has_value());
}

// A panel's ViewerItem borrows the controller the registry owns, and
// ViewerPanel.qml releases that controller from Component.onDestruction. The
// registry retires it with deleteLater(), so the item can outlive the
// controller by a deferred-delete pass; destroying it afterwards must not
// dereference the retired controller. Before the borrowed pointer became weak,
// ~ViewerItem crashed here while detaching from the freed controller.
TEST(Interactive, ViewerItemOutlivesItsReleasedControllerWithoutDereferencingIt) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerControllerRegistry registry(&runtime, session, interaction);
    auto* controller = qobject_cast<nemo::ui::ViewerController*>(registry.controller(QStringLiteral("panel-a")));
    ASSERT_TRUE(controller);
    ASSERT_EQ(registry.activeCount(), 1);

    auto* item = new nemo::ui::ViewerItem;
    item->setController(controller);
    ASSERT_EQ(item->controller(), controller);

    // The panel is removed while its item still exists: the controller is
    // retired and destroyed on the next deferred-delete pass.
    registry.release(QStringLiteral("panel-a"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    EXPECT_EQ(item->controller(), nullptr);
    delete item;
}

TEST(Interactive, PresentationConsumersShareSessionHistoryAndLifetime) {
    nemo::ui::ViewerRuntime firstRuntime;
    nemo::ui::ViewerRuntime secondRuntime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController first(&firstRuntime, session, interaction);
    QSignalSpy firstGraph(&first, &nemo::ui::ViewerController::graphChanged);
    QSignalSpy firstTimeline(&first, &nemo::ui::ViewerController::timelineChanged);
    QSignalSpy firstCatalog(&first, &nemo::ui::ViewerController::catalogChanged);

    {
        nemo::ui::ViewerController second(&secondRuntime, session, interaction);
        QSignalSpy secondGraph(&second, &nemo::ui::ViewerController::graphChanged);
        QSignalSpy secondTimeline(&second, &nemo::ui::ViewerController::timelineChanged);
        QSignalSpy secondCatalog(&second, &nemo::ui::ViewerController::catalogChanged);

        first.openSource("/tmp/nemo-shared-session-source.mkv");
        EXPECT_EQ(firstGraph.count(), 1);
        EXPECT_EQ(secondGraph.count(), 1);
        EXPECT_EQ(firstTimeline.count(), 1);
        EXPECT_EQ(secondTimeline.count(), 1);
        EXPECT_EQ(secondCatalog.count(), 1);
        ASSERT_TRUE(session.canUndo());

        firstGraph.clear();
        secondGraph.clear();
        firstTimeline.clear();
        secondTimeline.clear();
        firstCatalog.clear();
        secondCatalog.clear();
        ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
        EXPECT_TRUE(first.graphNodes().isEmpty());
        EXPECT_TRUE(second.graphNodes().isEmpty());
        EXPECT_EQ(firstGraph.count(), 1);
        EXPECT_EQ(secondGraph.count(), 1);
        EXPECT_EQ(firstTimeline.count(), 1);
        EXPECT_EQ(secondTimeline.count(), 1);
        EXPECT_EQ(secondCatalog.count(), 1);

        firstGraph.clear();
        secondGraph.clear();
        firstTimeline.clear();
        secondTimeline.clear();
        firstCatalog.clear();
        secondCatalog.clear();
        static_cast<void>(
            session.submit(nemo::addNodeCommand(session.document().rootNetworkId(), "testpattern", "direct"),
                           nemo::EditOptions{.expectedRevision = session.revision()}));
        EXPECT_EQ(firstGraph.count(), 1);
        EXPECT_EQ(secondGraph.count(), 1);
        EXPECT_EQ(firstTimeline.count(), 1);
        EXPECT_EQ(secondTimeline.count(), 1);
        EXPECT_EQ(secondCatalog.count(), 1);
        EXPECT_FALSE(first.graphNodes().isEmpty());
        EXPECT_FALSE(second.graphNodes().isEmpty());
    }

    // The surviving controller remains subscribed after the other consumer's
    // explicit lifetime ends.
    firstGraph.clear();
    firstTimeline.clear();
    firstCatalog.clear();
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(firstGraph.count(), 1);
    EXPECT_EQ(firstTimeline.count(), 1);
    EXPECT_EQ(firstCatalog.count(), 1);
    EXPECT_TRUE(first.graphNodes().isEmpty());
}
TEST(Interactive, IntegerTextEditsPreservePrecisionAndRejectOverflowAtomically) {
    nemo::NodeDescriptor descriptor{
        .type = "test.integer",
        .displayName = "Integer",
        .group = "Tests",
        .parameters = {{.name = "count", .type = nemo::ParameterType::Integer, .defaultValue = std::int64_t{0}}}};
    auto catalog =
        std::make_shared<nemo::NodeCatalog>(nemo::extendedBuiltinSchema(std::vector<nemo::NodeDescriptor>{descriptor}));
    nemo::Document document(catalog);
    const auto network = document.rootNetworkId();
    const auto node = document.network(network).graph().addNode("test.integer", "control");
    nemo::ProjectSession session(std::move(document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto id = QString::number(node);
    controller.setNodeParameterText(id, "count", "9223372036854775807");
    ASSERT_EQ(session.revision(), 2u);
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(std::get<std::int64_t>(session.queryValues(network, node).front().value), maximum);
    EXPECT_EQ(namedNode(controller, "control").value("params").toMap().value("count").toLongLong(), maximum);
    controller.setNodeParameterText(id, "count", "9223372036854775808");
    EXPECT_EQ(session.revision(), 2u);
    controller.setNodeParameter(id, "count", QVariant::fromValue<qulonglong>(std::numeric_limits<qulonglong>::max()));
    EXPECT_EQ(session.revision(), 2u);
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(std::get<std::int64_t>(session.queryValues(network, node).front().value), 0);
    EXPECT_FALSE(session.canUndo());
}

TEST(Interactive, CacheRangeReportsAsynchronousDiskAdmissionFailure) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native cache-range evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    nemo::ProjectSession session{emptyDocument()};
    auto color = std::make_shared<nemo::NodeId>();
    auto output = std::make_shared<nemo::NodeId>();
    static_cast<void>(
        session.submit(nemo::addNodeCommand(session.document().rootNetworkId(), "constcolor", "color", color),
                       nemo::EditOptions{.expectedRevision = session.revision()}));
    static_cast<void>(
        session.submit(nemo::addNodeCommand(session.document().rootNetworkId(), "output", "result", output),
                       nemo::EditOptions{.expectedRevision = session.revision()}));
    static_cast<void>(
        session.submit(nemo::connectCommand(session.document().rootNetworkId(), {*color, 0}, {*output, 0}),
                       nemo::EditOptions{.expectedRevision = session.revision()}));
    nemo::EvaluationRequest request;
    request.network = session.document().rootNetworkId();
    request.output = *output;
    request.region = {0, 0, 64, 48};
    nemo::eval::ViewerCacheOptions options;
    options.directory = directory.path().toStdString();
    options.encoding.codec = "libx264-cpu";
    options.chunkFrames = 1;
    options.maxDiskBytes = 1;
    nemo::ui::ViewerRuntime runtime;
    try {
        runtime.bootstrap({"VK_KHR_surface"}, NEMO_SLANG_SPV_DIR, options);
    } catch (const nemo::gpu::GpuException& error) {
        if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
            GTEST_SKIP() << error.what();
        throw;
    }
    ASSERT_TRUE(runtime.requestRange(session.snapshot(), request, 0, 0, 1));
    QElapsedTimer deadline;
    deadline.start();
    while (runtime.counts().cacheErrors == 0 && deadline.elapsed() < 60000)
        QTest::qWait(10);
    const auto counts = runtime.counts();
    EXPECT_GT(counts.cacheErrors, 0u);
    EXPECT_GT(counts.cacheDropped, 0u);
    EXPECT_EQ(counts.cachePublished, 0u);
    EXPECT_FALSE(counts.cacheError.empty()) << "Asynchronous failure must remain observable after evaluation";
#endif
}

struct InspectorFixture {
    std::shared_ptr<nemo::NodeCatalog> catalog;
    nemo::Document document;
    nemo::NodeId node{nemo::kInvalidNode};
};

// One schema with every parameter type; the inspector must describe each.
InspectorFixture inspectorFixture() {
    nemo::NodeDescriptor descriptor{
        .type = "test.inspector",
        .displayName = "Inspector Fixture",
        .group = "Tests",
        .parameters = {
            {.name = "enabled",
             .type = nemo::ParameterType::Boolean,
             .defaultValue = nemo::ParameterValue{true},
             .label = "Enabled",
             .section = "General"},
            {.name = "count",
             .type = nemo::ParameterType::Integer,
             .defaultValue = nemo::ParameterValue{std::int64_t{0}},
             .minimum = 0.0,
             .maximum = 10.0,
             .step = 1.0},
            {.name = "gain",
             .type = nemo::ParameterType::Float,
             .defaultValue = nemo::ParameterValue{1.0},
             .minimum = 0.0,
             .maximum = 4.0,
             .step = 0.1},
            {.name = "mode",
             .type = nemo::ParameterType::Choice,
             .defaultValue = nemo::ParameterValue{nemo::ChoiceValue{"linear"}},
             .choices = {"linear", "constant"}},
            {.name = "offset",
             .type = nemo::ParameterType::Vector2,
             .defaultValue = nemo::ParameterValue{nemo::Vector2Value{{0.0F, 0.0F}}}},
            {.name = "position",
             .type = nemo::ParameterType::Vector3,
             .defaultValue = nemo::ParameterValue{nemo::Vector3Value{{0.0F, 0.0F, 0.0F}}}},
            {.name = "tint",
             .type = nemo::ParameterType::Color,
             .defaultValue = nemo::ParameterValue{nemo::ColorValue{{0.0F, 0.0F, 0.0F, 1.0F}}}},
            {.name = "note",
             .type = nemo::ParameterType::String,
             .defaultValue = nemo::ParameterValue{std::string{}},
             .editor = "nemo.text"},
        }};
    InspectorFixture fixture;
    fixture.catalog =
        std::make_shared<nemo::NodeCatalog>(nemo::extendedBuiltinSchema(std::vector<nemo::NodeDescriptor>{descriptor}));
    fixture.document = nemo::Document{fixture.catalog};
    const auto network = fixture.document.rootNetworkId();
    fixture.node = fixture.document.network(network).graph().addNode("test.inspector", "inspector");
    return fixture;
}

// The first parameter row of a target's inspector: the handle a subnet
// occurrence presents, whatever identity the implementation encodes it with.
QVariantMap firstInspectorRow(const nemo::ui::ViewerController& controller, const QString& network,
                              const QString& node) {
    const auto inspector = controller.parameterInspector(network, node);
    if (!inspector.value(QStringLiteral("available")).toBool())
        return {};
    const auto sections = inspector.value(QStringLiteral("sections")).toList();
    if (sections.isEmpty())
        return {};
    const auto rows = sections.first().toMap().value(QStringLiteral("parameters")).toList();
    return rows.isEmpty() ? QVariantMap{} : rows.first().toMap();
}

QVariantMap inspectorRow(const QVariantMap& inspector, const QString& key) {
    for (const auto& section : inspector.value(QStringLiteral("sections")).toList()) {
        for (const auto& parameter : section.toMap().value(QStringLiteral("parameters")).toList()) {
            const auto row = parameter.toMap();
            if (row.value(QStringLiteral("key")).toString() == key)
                return row;
        }
    }
    return {};
}

QStringList inspectorSectionNames(const QVariantMap& inspector) {
    QStringList names;
    for (const auto& section : inspector.value(QStringLiteral("sections")).toList())
        names.push_back(section.toMap().value(QStringLiteral("name")).toString());
    return names;
}

QVariantMap inspectorCatalogEntry(const nemo::ui::ViewerController& controller, const QString& type) {
    for (const auto& value : controller.nodeCatalog()) {
        if (value.toMap().value(QStringLiteral("type")).toString() == type)
            return value.toMap();
    }
    return {};
}

TEST(Interactive, ParameterInspectorPublishesSchemaMetadataAndValues) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto network = QString::number(session.document().rootNetworkId());
    const auto node = QString::number(fixture.node);

    const auto inspector = controller.parameterInspector(network, node);
    ASSERT_TRUE(inspector.value(QStringLiteral("available")).toBool())
        << inspector.value(QStringLiteral("reason")).toString().toStdString();
    EXPECT_EQ(inspector.value(QStringLiteral("networkId")).toString(), network);
    EXPECT_EQ(inspector.value(QStringLiteral("nodeId")).toString(), node);
    EXPECT_EQ(inspector.value(QStringLiteral("instanceId")).toString(), QStringLiteral("0"));
    EXPECT_EQ(inspector.value(QStringLiteral("name")).toString(), QStringLiteral("inspector"));
    EXPECT_EQ(inspector.value(QStringLiteral("type")).toString(), QStringLiteral("test.inspector"));
    EXPECT_EQ(inspector.value(QStringLiteral("category")).toString(), QStringLiteral("Tests"));
    EXPECT_EQ(inspectorSectionNames(inspector), QStringList({QStringLiteral("General"), QStringLiteral("Properties")}));

    const auto enabled = inspectorRow(inspector, QStringLiteral("enabled"));
    EXPECT_EQ(enabled.value(QStringLiteral("type")).toString(), QStringLiteral("boolean"));
    EXPECT_EQ(enabled.value(QStringLiteral("kind")).toString(), QStringLiteral("toggle"));
    EXPECT_EQ(enabled.value(QStringLiteral("label")).toString(), QStringLiteral("Enabled"));
    EXPECT_EQ(enabled.value(QStringLiteral("value")).metaType().id(), QMetaType::Bool);
    EXPECT_TRUE(enabled.value(QStringLiteral("value")).toBool());
    EXPECT_FALSE(enabled.contains(QStringLiteral("minimum")));
    EXPECT_FALSE(enabled.contains(QStringLiteral("maximum")));
    EXPECT_FALSE(enabled.contains(QStringLiteral("step")));
    EXPECT_FALSE(enabled.value(QStringLiteral("animated")).toBool());
    EXPECT_FALSE(enabled.value(QStringLiteral("keyed")).toBool());
    EXPECT_TRUE(enabled.value(QStringLiteral("editor")).toString().isEmpty());
    EXPECT_TRUE(enabled.value(QStringLiteral("choices")).toList().isEmpty());

    const auto count = inspectorRow(inspector, QStringLiteral("count"));
    EXPECT_EQ(count.value(QStringLiteral("kind")).toString(), QStringLiteral("number"));
    EXPECT_EQ(count.value(QStringLiteral("label")).toString(), QStringLiteral("Count"));
    EXPECT_EQ(count.value(QStringLiteral("value")).metaType().id(), QMetaType::LongLong);
    EXPECT_EQ(count.value(QStringLiteral("value")).toLongLong(), 0);
    EXPECT_DOUBLE_EQ(count.value(QStringLiteral("minimum")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(count.value(QStringLiteral("maximum")).toDouble(), 10.0);
    EXPECT_DOUBLE_EQ(count.value(QStringLiteral("step")).toDouble(), 1.0);

    const auto gain = inspectorRow(inspector, QStringLiteral("gain"));
    EXPECT_EQ(gain.value(QStringLiteral("kind")).toString(), QStringLiteral("number"));
    EXPECT_EQ(gain.value(QStringLiteral("label")).toString(), QStringLiteral("Gain"));
    EXPECT_EQ(gain.value(QStringLiteral("value")).metaType().id(), QMetaType::Double);
    EXPECT_DOUBLE_EQ(gain.value(QStringLiteral("value")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(gain.value(QStringLiteral("step")).toDouble(), 0.1);

    const auto mode = inspectorRow(inspector, QStringLiteral("mode"));
    EXPECT_EQ(mode.value(QStringLiteral("kind")).toString(), QStringLiteral("choice"));
    EXPECT_EQ(mode.value(QStringLiteral("type")).toString(), QStringLiteral("choice"));
    EXPECT_EQ(mode.value(QStringLiteral("value")).toString(), QStringLiteral("linear"));
    EXPECT_EQ(mode.value(QStringLiteral("choices")).toList(),
              QVariantList({QVariant(QStringLiteral("linear")), QVariant(QStringLiteral("constant"))}));

    const auto offset = inspectorRow(inspector, QStringLiteral("offset"));
    EXPECT_EQ(offset.value(QStringLiteral("kind")).toString(), QStringLiteral("vector2"));
    ASSERT_EQ(offset.value(QStringLiteral("value")).toList().size(), 2);
    EXPECT_FLOAT_EQ(offset.value(QStringLiteral("value")).toList().at(0).toFloat(), 0.0F);

    const auto position = inspectorRow(inspector, QStringLiteral("position"));
    EXPECT_EQ(position.value(QStringLiteral("kind")).toString(), QStringLiteral("vector3"));
    ASSERT_EQ(position.value(QStringLiteral("value")).toList().size(), 3);

    const auto tint = inspectorRow(inspector, QStringLiteral("tint"));
    EXPECT_EQ(tint.value(QStringLiteral("kind")).toString(), QStringLiteral("color"));
    ASSERT_EQ(tint.value(QStringLiteral("value")).toList().size(), 4);
    EXPECT_FLOAT_EQ(tint.value(QStringLiteral("value")).toList().at(3).toFloat(), 1.0F);

    const auto note = inspectorRow(inspector, QStringLiteral("note"));
    EXPECT_EQ(note.value(QStringLiteral("kind")).toString(), QStringLiteral("string"));
    EXPECT_EQ(note.value(QStringLiteral("editor")).toString(), QStringLiteral("nemo.text"));
    EXPECT_EQ(note.value(QStringLiteral("value")).toString(), QString{});

    // The same metadata is published through the catalog snapshot, with the
    // step only present when the schema declares one.
    const auto catalogEntry = inspectorCatalogEntry(controller, QStringLiteral("test.inspector"));
    ASSERT_FALSE(catalogEntry.isEmpty());
    for (const auto& parameter : catalogEntry.value(QStringLiteral("parameters")).toList()) {
        const auto map = parameter.toMap();
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("enabled")) {
            EXPECT_EQ(map.value(QStringLiteral("label")).toString(), QStringLiteral("Enabled"));
            EXPECT_EQ(map.value(QStringLiteral("section")).toString(), QStringLiteral("General"));
            EXPECT_FALSE(map.contains(QStringLiteral("step")));
            EXPECT_TRUE(map.value(QStringLiteral("editor")).toString().isEmpty());
        }
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("gain")) {
            EXPECT_TRUE(map.value(QStringLiteral("label")).toString().isEmpty());
            EXPECT_TRUE(map.value(QStringLiteral("section")).toString().isEmpty());
            EXPECT_DOUBLE_EQ(map.value(QStringLiteral("step")).toDouble(), 0.1);
        }
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("note"))
            EXPECT_EQ(map.value(QStringLiteral("editor")).toString(), QStringLiteral("nemo.text"));
    }
}

TEST(Interactive, ParameterInspectorReportsUnavailableTargets) {
    auto fixture = inspectorFixture();
    const auto networkId = fixture.document.rootNetworkId();
    // A node whose type has no descriptor is retained as data; the inspector
    // must report the missing schema instead of throwing.
    const auto orphan = fixture.document.network(networkId).graph().addNode("test.unregistered", "orphan");
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto network = QString::number(networkId);

    const auto missingNetwork = controller.parameterInspector(QStringLiteral("424242"), QString::number(fixture.node));
    EXPECT_FALSE(missingNetwork.value(QStringLiteral("available")).toBool());
    EXPECT_FALSE(missingNetwork.value(QStringLiteral("reason")).toString().isEmpty());
    EXPECT_EQ(missingNetwork.value(QStringLiteral("networkId")).toString(), QStringLiteral("424242"));

    const auto missingNode = controller.parameterInspector(network, QStringLiteral("424242"));
    EXPECT_FALSE(missingNode.value(QStringLiteral("available")).toBool());
    EXPECT_FALSE(missingNode.value(QStringLiteral("reason")).toString().isEmpty());
    EXPECT_EQ(missingNode.value(QStringLiteral("nodeId")).toString(), QStringLiteral("424242"));
    EXPECT_TRUE(missingNode.value(QStringLiteral("sections")).toList().isEmpty());

    const auto unknownType = controller.parameterInspector(network, QString::number(orphan));
    EXPECT_FALSE(unknownType.value(QStringLiteral("available")).toBool());
    EXPECT_FALSE(unknownType.value(QStringLiteral("reason")).toString().isEmpty());
    EXPECT_TRUE(unknownType.value(QStringLiteral("sections")).toList().isEmpty());
}

TEST(Interactive, NodeParameterKeyingUpsertsRemovesAndReportsStatus) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);
    const nemo::ParameterAddress gainAddress{networkId, fixture.node, "gain", nemo::kInvalidNetworkInstance};

    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("none"));
    ASSERT_TRUE(session
                    .submit(nemo::setKeyframesCommand({nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 0.0, 2.0}},
                                                       nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 10.0, 4.0}}}),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);

    controller.setFrame(0);
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("key"));
    auto gain = inspectorRow(controller.parameterInspector(network, node), QStringLiteral("gain"));
    EXPECT_TRUE(gain.value(QStringLiteral("animated")).toBool());
    EXPECT_TRUE(gain.value(QStringLiteral("keyed")).toBool());
    EXPECT_DOUBLE_EQ(gain.value(QStringLiteral("value")).toDouble(), 2.0);

    controller.setFrame(5);
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("animated"));
    gain = inspectorRow(controller.parameterInspector(network, node), QStringLiteral("gain"));
    EXPECT_TRUE(gain.value(QStringLiteral("animated")).toBool());
    EXPECT_FALSE(gain.value(QStringLiteral("keyed")).toBool());
    EXPECT_DOUBLE_EQ(gain.value(QStringLiteral("value")).toDouble(), 3.0);

    // Upserting a key uses the value evaluated at the current frame and is
    // idempotent while that value is already keyed.
    const auto revision = session.revision();
    EXPECT_TRUE(controller.keyNodeParameter(network, node, QStringLiteral("gain")));
    EXPECT_EQ(session.revision(), revision + 1);
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("key"));
    gain = inspectorRow(controller.parameterInspector(network, node), QStringLiteral("gain"));
    EXPECT_TRUE(gain.value(QStringLiteral("keyed")).toBool());
    EXPECT_DOUBLE_EQ(gain.value(QStringLiteral("value")).toDouble(), 3.0);
    EXPECT_TRUE(controller.keyNodeParameter(network, node, QStringLiteral("gain")));
    EXPECT_EQ(session.revision(), revision + 1);

    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("animated"));
    ASSERT_TRUE(session.redo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("key"));

    EXPECT_TRUE(controller.removeNodeParameterKey(network, node, QStringLiteral("gain")));
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("animated"));
    gain = inspectorRow(controller.parameterInspector(network, node), QStringLiteral("gain"));
    EXPECT_TRUE(gain.value(QStringLiteral("animated")).toBool());
    EXPECT_FALSE(gain.value(QStringLiteral("keyed")).toBool());
    EXPECT_FALSE(controller.removeNodeParameterKey(network, node, QStringLiteral("gain")));

    // Keying requires a schema parameter.
    EXPECT_FALSE(controller.keyNodeParameter(network, node, QStringLiteral("missing")));
}

TEST(Interactive, ParameterEditsDefineSingleUndoEntryAndRespectKeying) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);

    // A continuous unkeyed edit is one history entry and creates no channel.
    const auto startRevision = session.revision();
    const auto token = controller.beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(token.isEmpty()) << controller.error().toStdString();
    EXPECT_TRUE(controller.updateNodeParameterEdit(token, 2.0));
    EXPECT_TRUE(controller.updateNodeParameterEdit(token, 3.5));
    EXPECT_TRUE(controller.commitNodeParameterEdit(token));
    EXPECT_EQ(session.revision(), startRevision + 1);
    auto values = session.queryValues(networkId, fixture.node, "gain");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{3.5});
    EXPECT_TRUE(session.queryAnimationChannels().empty());
    EXPECT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    values = session.queryValues(networkId, fixture.node, "gain");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{1.0});

    // Cancel leaves the document and history untouched.
    const auto revision = session.revision();
    const auto cancelled = controller.beginNodeParameterEdit(network, node, QStringLiteral("count"));
    ASSERT_FALSE(cancelled.isEmpty());
    EXPECT_TRUE(controller.updateNodeParameterEdit(cancelled, 4));
    EXPECT_TRUE(controller.cancelNodeParameterEdit(cancelled));
    EXPECT_EQ(session.revision(), revision);
    values = session.queryValues(networkId, fixture.node, "count");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{std::int64_t{0}});

    // Starting another interaction hands the session gesture over (issue #102):
    // the previous edit is retired with its own token instead of locking the
    // session, and the retired token cannot touch the replacement.
    const auto active = controller.beginNodeParameterEdit(network, node, QStringLiteral("count"));
    ASSERT_FALSE(active.isEmpty());
    EXPECT_TRUE(controller.updateNodeParameterEdit(active, 5));
    const auto replacement = controller.beginNodeParameterEdit(network, node, QStringLiteral("count"));
    ASSERT_FALSE(replacement.isEmpty()) << controller.error().toStdString();
    EXPECT_NE(replacement, active);
    EXPECT_FALSE(controller.updateNodeParameterEdit(active, 6))
        << "an ended gesture's token must not drive the replacement's preview";
    EXPECT_FALSE(controller.commitNodeParameterEdit(active)) << "a superseded token publishes nothing";
    EXPECT_EQ(session.revision(), revision) << "neither the retired preview nor the retired commit may author";
    EXPECT_TRUE(controller.updateNodeParameterEdit(replacement, 7));
    EXPECT_TRUE(controller.commitNodeParameterEdit(replacement));
    EXPECT_EQ(session.revision(), revision + 1);
    values = session.queryValues(networkId, fixture.node, "count");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{std::int64_t{7}});

    // A key already at the current frame is updated in place.
    const nemo::ParameterAddress gainAddress{networkId, fixture.node, "gain", nemo::kInvalidNetworkInstance};
    ASSERT_TRUE(session
                    .submit(nemo::setKeyframesCommand({nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 0.0, 1.0}}}),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);
    controller.setFrame(0);
    const auto keyedRevision = session.revision();
    const auto keyed = controller.beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(keyed.isEmpty()) << controller.error().toStdString();
    EXPECT_TRUE(controller.updateNodeParameterEdit(keyed, 3.25));
    EXPECT_TRUE(controller.commitNodeParameterEdit(keyed));
    EXPECT_EQ(session.revision(), keyedRevision + 1);
    const auto* channel = session.document().animationChannel(gainAddress);
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(channel->keys.size(), 1U);
    EXPECT_EQ(channel->keys.front().value, nemo::ParameterValue{3.25});
}

// Issue #102: ONE presentation interaction per session. Every controller of a
// session shares the injected owner, so starting an edit in another panel
// retires the previous panel's unfinished edit instead of locking the session:
// the retired preview never lands, its token can neither update nor commit the
// replacement, and the replacement stays one undoable history entry.
TEST(Interactive, ParameterGestureOwnershipMovesBetweenSessionControllers) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerControllerRegistry registry(&runtime, session, interaction);
    auto* first = qobject_cast<nemo::ui::ViewerController*>(registry.controller(QStringLiteral("panel-a")));
    auto* second = qobject_cast<nemo::ui::ViewerController*>(registry.controller(QStringLiteral("panel-b")));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(first, second);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);

    // Panel A previews 2.0 without committing anything.
    const auto revision = session.revision();
    const auto firstToken = first->beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(firstToken.isEmpty()) << first->error().toStdString();
    ASSERT_TRUE(first->updateNodeParameterEdit(firstToken, 2.0));

    // Panel B starts its own parameter interaction. A's edit ends first, with
    // its own token and synchronously, and its preview is discarded.
    second->prepareParameterInteraction();
    EXPECT_EQ(session.revision(), revision) << "a retired preview publishes nothing";
    EXPECT_FALSE(first->updateNodeParameterEdit(firstToken, 3.0));
    EXPECT_FALSE(first->commitNodeParameterEdit(firstToken));
    EXPECT_EQ(session.revision(), revision);

    const auto secondToken = second->beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(secondToken.isEmpty()) << second->error().toStdString();
    ASSERT_TRUE(second->updateNodeParameterEdit(secondToken, 3.5));
    // A's ended token cannot reach B's live gesture, neither by cancelling it
    // nor by committing on its behalf.
    EXPECT_FALSE(first->cancelNodeParameterEdit(firstToken));
    EXPECT_FALSE(first->commitNodeParameterEdit(firstToken));
    EXPECT_TRUE(second->updateNodeParameterEdit(secondToken, 3.5));
    ASSERT_TRUE(second->commitNodeParameterEdit(secondToken)) << second->error().toStdString();
    EXPECT_EQ(session.revision(), revision + 1) << "the replacement is exactly one history entry";
    auto values = session.queryValues(networkId, fixture.node, "gain");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{3.5});

    // One undo removes the committed edit and nothing of the retired one.
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    values = session.queryValues(networkId, fixture.node, "gain");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{1.0});
}

TEST(Interactive, NotificationCancellationCannotOrphanOrCancelTheNextGesture) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController first(&runtime, session, interaction);
    nemo::ui::ViewerController second(&runtime, session, interaction);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);
    const auto token = first.beginNodeParameterEdit(network, node, "gain");
    ASSERT_FALSE(token.isEmpty());
    ASSERT_TRUE(first.updateNodeParameterEdit(token, 2.0));
    struct Cancellation {
        nemo::ui::ViewerController* controller;
        QString token;
    } cancellation{&first, token};
    {
        auto subscription = session.subscribe(&cancellation, [](void* context) noexcept {
            const auto* pending = static_cast<Cancellation*>(context);
            static_cast<void>(pending->controller->cancelNodeParameterEdit(pending->token));
        });
        ASSERT_TRUE(session
                        .submit(nemo::setParamCommand(networkId, fixture.node, "count", std::int64_t{5}),
                                {.expectedRevision = session.revision()})
                        .committed);
    }
    const auto afterChange = session.revision();
    const auto next = second.beginNodeParameterEdit(network, node, "gain");
    ASSERT_FALSE(next.isEmpty()) << second.error().toStdString();
    QCoreApplication::sendPostedEvents(&first, QEvent::MetaCall);
    ASSERT_TRUE(second.updateNodeParameterEdit(next, 3.5));
    ASSERT_TRUE(second.commitNodeParameterEdit(next));
    EXPECT_EQ(session.revision(), afterChange + 1);
    ASSERT_TRUE(session.undo({.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(session.queryValues(networkId, fixture.node, "gain").front().value, nemo::ParameterValue{1.0});
    EXPECT_EQ(session.queryValues(networkId, fixture.node, "count").front().value,
              nemo::ParameterValue{std::int64_t{5}});
}

// A controller destroyed while it owns a live gesture must release the shared
// interaction: the session is never left locked, its preview publishes nothing,
// and the next controller can begin, preview and commit normally. A released
// participant is never cancelled afterwards, which is what lets a destroyed
// controller stay unreachable.
TEST(Interactive, DestroyedControllerReleasesTheSessionInteraction) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);
    const auto revision = session.revision();

    auto owning = std::make_unique<nemo::ui::ViewerController>(&runtime, session, interaction);
    const auto token = owning->beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(token.isEmpty()) << owning->error().toStdString();
    ASSERT_TRUE(owning->updateNodeParameterEdit(token, 3.25));
    owning.reset();
    EXPECT_EQ(session.revision(), revision) << "the destroyed controller's preview is discarded, not committed";

    // The owner holds no participant after that destruction: cancelling the
    // session interaction must not reach the destroyed controller, and a probe
    // owner is the only one a later cancel can reach.
    interaction.cancel();
    int cancellations = 0;
    interaction.acquire(&cancellations, [](void* owner) { ++*static_cast<int*>(owner); });
    interaction.release(&cancellations);
    interaction.cancel();
    EXPECT_EQ(cancellations, 0) << "a released participant is never cancelled";

    auto replacement = std::make_unique<nemo::ui::ViewerController>(&runtime, session, interaction);
    const auto next = replacement->beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(next.isEmpty()) << replacement->error().toStdString();
    ASSERT_TRUE(replacement->updateNodeParameterEdit(next, 0.75));
    ASSERT_TRUE(replacement->commitNodeParameterEdit(next)) << replacement->error().toStdString();
    EXPECT_EQ(session.revision(), revision + 1);
    const auto values = session.queryValues(networkId, fixture.node, "gain");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{0.75});
}

TEST(Interactive, JavaScriptArrayEditsConvertVectorAndColorParameters) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);

    // QML delivers a JS array as a QJSValue, not a QVariantList: both must
    // convert identically, or every vector/color control silently rejects edits.
    QJSEngine engine;
    const QJSValue tint = engine.evaluate("[0.25, 0.5, 0.75, 1]");
    ASSERT_TRUE(tint.isArray());
    controller.setNodeParameter(node, QStringLiteral("tint"), QVariant::fromValue(tint));
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    const auto colors = session.queryValues(networkId, fixture.node, "tint");
    ASSERT_FALSE(colors.empty());
    EXPECT_EQ(std::get<nemo::ColorValue>(colors.front().value), (nemo::ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}));

    // The one-undo-step gesture path converts the same JS array shape.
    const auto token = controller.beginNodeParameterEdit(network, node, QStringLiteral("position"));
    ASSERT_FALSE(token.isEmpty()) << controller.error().toStdString();
    const QJSValue position = engine.evaluate("[1, 2, 3]");
    ASSERT_TRUE(position.isArray());
    EXPECT_TRUE(controller.updateNodeParameterEdit(token, QVariant::fromValue(position)));
    EXPECT_TRUE(controller.commitNodeParameterEdit(token));
    const auto positions = session.queryValues(networkId, fixture.node, "position");
    ASSERT_FALSE(positions.empty());
    EXPECT_EQ(std::get<nemo::Vector3Value>(positions.front().value), (nemo::Vector3Value{{1.0F, 2.0F, 3.0F}}));
}

// Issue #43: a Media Bin reference is explicitly viewable through the shared
// source-fill path even when no authored source node addresses it, and the
// explicit open authors nothing.
TEST(Interactive, MediaRoleViewsCatalogReferenceWithoutAuthoringAGraphNode) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    // A media import authors exactly this: one Document source reference plus
    // one catalog entry, never a graph node.
    nemo::SourceReference reference;
    reference.path = "/media/nemo-media-role-still.exr";
    reference.revision = 5;
    ASSERT_TRUE(session
                    .submit(nemo::setSourceCommand("media-role-still", reference),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);
    auto entry = std::make_shared<nemo::MediaSourceId>();
    ASSERT_TRUE(session
                    .submit(nemo::importMediaReferenceCommand("media-role-still", nemo::kInvalidMediaBin, {}, entry),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);

    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    controller.setDestination(nemo::eval::ViewerDestination::Interactive);
    const auto network = session.document().rootNetworkId();
    // The fixture authors exactly the media import result: a source reference
    // and a catalog entry, no graph node at all.
    ASSERT_TRUE(session.document().network(network).graph().nodes().empty());
    const auto revisionBefore = session.revision();
    const bool canUndoBefore = session.canUndo();

    // The catalog reference alone reaches the source-fill path: the routed
    // media role probes the referenced source, not the graph role's "src".
    controller.setViewerContext(QStringLiteral("media"), QStringLiteral("media-role-still"), 0);
    EXPECT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    EXPECT_EQ(controller.renderState(), QStringLiteral("pending"));
    EXPECT_EQ(runtime.counts().queued, 1u);

    // Explicit viewing is presentation-only: no node, no history entry, and the
    // source is not counted as used media.
    EXPECT_TRUE(session.document().network(network).graph().nodes().empty());
    EXPECT_TRUE(controller.graphNodes().isEmpty());
    EXPECT_EQ(session.revision(), revisionBefore);
    EXPECT_EQ(session.canUndo(), canUndoBefore);
    EXPECT_FALSE(session.document().mediaCatalog().sourceUsed(session.document(), "media-role-still"));

    // A decimal catalog entry id addresses the same reference; an unknown
    // target still reports an explicit unavailable media context.
    controller.setViewerContext(QStringLiteral("media"), QString::number(static_cast<qulonglong>(*entry)), 0);
    EXPECT_EQ(controller.renderState(), QStringLiteral("pending"));
    controller.setViewerContext(QStringLiteral("media"), QStringLiteral("media-role-missing"), 0);
    EXPECT_EQ(controller.renderState(), QStringLiteral("unavailable"));
}

TEST(Interactive, ParameterEditorRegistryRegistersAndResolvesEditors) {
    nemo::ui::ParameterEditorRegistry registry;

    const auto missing = registry.editor(QStringLiteral("nemo.missing"));
    EXPECT_FALSE(missing.value(QStringLiteral("available")).toBool());
    EXPECT_TRUE(missing.value(QStringLiteral("source")).toString().isEmpty());

    EXPECT_FALSE(
        registry.registerEditor(QStringLiteral("unnamespaced"), QUrl(QStringLiteral("qrc:/Nemo/Unnamed.qml"))));
    EXPECT_FALSE(registry.registerEditor(QString{}, QUrl(QStringLiteral("qrc:/Nemo/Unnamed.qml"))));
    EXPECT_FALSE(registry.registerEditor(QStringLiteral("nemo.blank"), QUrl{}));

    const QUrl source{QStringLiteral("qrc:/Nemo/Linear.qml")};
    EXPECT_TRUE(registry.registerEditor(QStringLiteral("nemo.linear"), source));
    const auto registered = registry.editor(QStringLiteral("nemo.linear"));
    EXPECT_TRUE(registered.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(registered.value(QStringLiteral("source")).toUrl(), source);
    EXPECT_TRUE(registered.value(QStringLiteral("reason")).toString().isEmpty());
    EXPECT_TRUE(registry.reason(QStringLiteral("nemo.linear")).isEmpty());

    // Re-registering replaces the source and keeps the registration usable.
    const QUrl replacement{QStringLiteral("qrc:/Nemo/LinearV2.qml")};
    EXPECT_TRUE(registry.registerEditor(QStringLiteral("nemo.linear"), replacement));
    EXPECT_EQ(registry.editor(QStringLiteral("nemo.linear")).value(QStringLiteral("source")).toUrl(), replacement);

    EXPECT_TRUE(registry.unregisterEditor(QStringLiteral("nemo.linear")));
    EXPECT_FALSE(registry.editor(QStringLiteral("nemo.linear")).value(QStringLiteral("available")).toBool());

    // An unsupported presentation must not leave a usable registration behind.
    EXPECT_TRUE(registry.registerEditor(QStringLiteral("nemo.layout"), replacement, {}, QStringLiteral("section")));
    // A refused replacement is an atomic rejection: the previously valid
    // registration survives untouched (same source, consumes and presentation).
    EXPECT_FALSE(registry.registerEditor(QStringLiteral("nemo.layout"), replacement, {}, QStringLiteral("panel")));
    const auto preserved = registry.editor(QStringLiteral("nemo.layout"));
    EXPECT_TRUE(preserved.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(preserved.value(QStringLiteral("presentation")).toString(), QStringLiteral("section"));
    EXPECT_EQ(preserved.value(QStringLiteral("source")).toUrl(), replacement);
    // An id that was never validly registered reports unavailable with the
    // offending presentation named.
    EXPECT_FALSE(registry.registerEditor(QStringLiteral("nemo.badlayout"), replacement, {}, QStringLiteral("panel")));
    const auto refused = registry.editor(QStringLiteral("nemo.badlayout"));
    EXPECT_FALSE(refused.value(QStringLiteral("available")).toBool());
    EXPECT_TRUE(refused.value(QStringLiteral("reason")).toString().contains(QStringLiteral("panel")));
    EXPECT_FALSE(registry.unregisterEditor(QStringLiteral("nemo.linear")));
}

TEST(Interactive, SubnetExposurePublishesTypedControlsWithInstanceLocalEdits) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = controller.rootNetworkId();
    const auto source = controller.createGraphNode(scope, "constcolor", "exposeSource", 0.0, 0.0, {}, {});
    const auto merge = controller.createGraphNode(scope, "merge", "exposeMerge", 160.0, 0.0, {}, {});
    const auto output = controller.createGraphNode(scope, "output", "exposeOutput", 320.0, 0.0, {}, {});
    ASSERT_FALSE(source.isEmpty());
    ASSERT_FALSE(merge.isEmpty());
    ASSERT_FALSE(output.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, source, 0, merge, 0));
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, merge, 0, output, 0));
    const auto subnet = controller.collapseSelection(scope, QVariantList{source, merge}, "Exposed");
    ASSERT_FALSE(subnet.isEmpty()) << controller.error().toStdString();
    const auto node = namedNode(controller, "Exposed");
    const auto definition = node.value("definition").toString();
    const auto instance = node.value("instance").toString();
    ASSERT_FALSE(definition.isEmpty());
    ASSERT_FALSE(instance.isEmpty());

    ASSERT_TRUE(controller.promoteParameter(definition, source, "color", "")) << controller.error().toStdString();
    const auto exposure = controller.subnetExposure(scope, subnet);
    ASSERT_TRUE(exposure.value("available").toBool()) << exposure.value("reason").toString().toStdString();
    EXPECT_EQ(exposure.value("linkState").toString(), QStringLiteral("local"));
    const auto rows = exposure.value("rows").toList();
    ASSERT_EQ(rows.size(), 1);
    const auto row = rows.first().toMap();
    EXPECT_EQ(row.value("key").toString(), QStringLiteral("color"));
    EXPECT_EQ(row.value("node").toString(), source);
    EXPECT_EQ(row.value("nodeName").toString(), QStringLiteral("exposeSource"));
    EXPECT_EQ(row.value("source").toString(), QStringLiteral("exposeSource.color"));
    const auto exposedId = row.value("id").toString();

    // A subnet's ordinary inspector presents the exposed control, addressed by
    // exposed identity rather than the editable label.
    const auto inspector = controller.parameterInspector(scope, subnet);
    ASSERT_TRUE(inspector.value("available").toBool()) << inspector.value("reason").toString().toStdString();
    const auto sections = inspector.value("sections").toList();
    ASSERT_EQ(sections.size(), 1);
    const auto controls = sections.first().toMap().value("parameters").toList();
    ASSERT_EQ(controls.size(), 1);
    const auto control = controls.first().toMap();
    EXPECT_EQ(control.value("key").toString(), QStringLiteral("exposed:") + exposedId);
    EXPECT_EQ(control.value("kind").toString(), QStringLiteral("color"));

    // Editing the control is one command gesture and an instance-local override:
    // the definition-authored value of the child node stays untouched.
    const auto token = controller.beginNodeParameterEdit(scope, subnet, control.value("key").toString());
    ASSERT_FALSE(token.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdit(token, QVariantList{0.25, 0.5, 0.75, 1.0}));
    ASSERT_TRUE(controller.commitNodeParameterEdit(token));
    const auto edited = controller.parameterInspector(scope, subnet)
                            .value("sections")
                            .toList()
                            .first()
                            .toMap()
                            .value("parameters")
                            .toList()
                            .first()
                            .toMap()
                            .value("value")
                            .toList();
    ASSERT_EQ(edited.size(), 4);
    EXPECT_FLOAT_EQ(edited.at(0).toFloat(), 0.25F);
    EXPECT_FLOAT_EQ(edited.at(2).toFloat(), 0.75F);
    const auto* definitionNode =
        session.document().network(definition.toULongLong()).graph().node(source.toULongLong());
    ASSERT_NE(definitionNode, nullptr);
    EXPECT_EQ(definitionNode->params.count("color"), 0U);
    const auto* firstInstance = session.document().instance(instance.toULongLong());
    ASSERT_NE(firstInstance, nullptr);
    EXPECT_TRUE(firstInstance->params.contains(source.toULongLong()));

    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    const auto* restoredInstance = session.document().instance(instance.toULongLong());
    ASSERT_NE(restoredInstance, nullptr);
    EXPECT_FALSE(restoredInstance->params.contains(source.toULongLong()));

    ASSERT_TRUE(controller.renameExposedParameter(definition, exposedId, "Tint"));
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    EXPECT_EQ(controller.subnetExposure(scope, subnet).value("rows").toList().first().toMap().value("name").toString(),
              QStringLiteral("Tint"));

    // Duplicating shares the definition; Make Independent detaches only the
    // selected occurrence.
    const auto copyNode = controller.duplicateLinkedInstance(scope, subnet, 420.0, 80.0);
    ASSERT_FALSE(copyNode.isEmpty()) << controller.error().toStdString();
    EXPECT_EQ(controller.subnetExposure(scope, subnet).value("linkState").toString(), QStringLiteral("shared"));
    EXPECT_EQ(namedNode(controller, "Exposed").value("linkState").toString(), QStringLiteral("shared"));
    ASSERT_TRUE(controller.makeIndependent(instance));
    EXPECT_EQ(controller.subnetExposure(scope, subnet).value("linkState").toString(), QStringLiteral("local"));
    EXPECT_EQ(controller.subnetExposure(scope, copyNode).value("linkState").toString(), QStringLiteral("linked"));
}

// Issue #75: an authored animation channel owns the value. Editing between keys
// must author the current-frame key instead of writing a static value the
// channel would shadow, and every such edit stays one undo step.
TEST(Interactive, BetweenKeyEditAuthorsTheCurrentFrameKey) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);
    const nemo::ParameterAddress gainAddress{networkId, fixture.node, "gain", nemo::kInvalidNetworkInstance};
    ASSERT_TRUE(session
                    .submit(nemo::setKeyframesCommand({nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 0.0, 2.0}},
                                                       nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 10.0, 4.0}}}),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);
    controller.setFrame(5);
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("animated"));

    const auto revision = session.revision();
    const auto token = controller.beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(token.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdit(token, 3.5));
    ASSERT_TRUE(controller.commitNodeParameterEdit(token));
    EXPECT_EQ(session.revision(), revision + 1);
    const auto* channel = session.document().animationChannel(gainAddress);
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(channel->keys.size(), 3U);
    const auto atFrame = std::find_if(channel->keys.begin(), channel->keys.end(),
                                      [](const nemo::Keyframe& key) { return key.time == 5.0; });
    ASSERT_NE(atFrame, channel->keys.end());
    EXPECT_EQ(atFrame->value, nemo::ParameterValue{3.5});

    // Cancelling a between-keys edit leaves the channel exactly as it was.
    const auto before = session.document().animationChannels();
    const auto cancelled = controller.beginNodeParameterEdit(network, node, QStringLiteral("gain"));
    ASSERT_FALSE(cancelled.isEmpty());
    ASSERT_TRUE(controller.updateNodeParameterEdit(cancelled, 2.5));
    ASSERT_TRUE(controller.cancelNodeParameterEdit(cancelled));
    EXPECT_EQ(session.document().animationChannels(), before);
}

// Issue #75 story 19: reset restores the schema default at the same edit scope
// and never removes the channel or its other keys.
TEST(Interactive, ResetRestoresDefaultAtTheAuthoredScope) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto networkId = session.document().rootNetworkId();
    const auto network = QString::number(networkId);
    const auto node = QString::number(fixture.node);

    // A static override resets in one history entry; a second reset is a no-op.
    controller.setNodeParameters(QVariantList{QVariantMap{{QStringLiteral("nodeId"), node},
                                                          {QStringLiteral("key"), QStringLiteral("gain")},
                                                          {QStringLiteral("value"), 3.25}}});
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    const auto changed = session.revision();
    EXPECT_TRUE(controller.resetNodeParameterEdit(network, node, QStringLiteral("gain")));
    EXPECT_EQ(session.revision(), changed + 1);
    const auto values = session.queryValues(networkId, fixture.node, "gain");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(values.front().value, nemo::ParameterValue{1.0});
    EXPECT_TRUE(controller.resetNodeParameterEdit(network, node, QStringLiteral("gain")));
    EXPECT_EQ(session.revision(), changed + 1);

    // An animated parameter authors the default at the current frame; the
    // channel and its other keys survive.
    const nemo::ParameterAddress gainAddress{networkId, fixture.node, "gain", nemo::kInvalidNetworkInstance};
    ASSERT_TRUE(session
                    .submit(nemo::setKeyframesCommand({nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 0.0, 2.0}},
                                                       nemo::KeyframeEdit{gainAddress, nemo::Keyframe{0, 10.0, 4.0}}}),
                            nemo::EditOptions{.expectedRevision = session.revision()})
                    .committed);
    controller.setFrame(5);
    const auto animated = session.revision();
    EXPECT_TRUE(controller.resetNodeParameterEdit(network, node, QStringLiteral("gain")));
    EXPECT_EQ(session.revision(), animated + 1);
    const auto* channel = session.document().animationChannel(gainAddress);
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(channel->keys.size(), 3U);
    const auto atFrame = std::find_if(channel->keys.begin(), channel->keys.end(),
                                      [](const nemo::Keyframe& key) { return key.time == 5.0; });
    ASSERT_NE(atFrame, channel->keys.end());
    EXPECT_EQ(atFrame->value, nemo::ParameterValue{1.0});
    for (const auto& key : channel->keys) {
        if (key.time == 0.0)
            EXPECT_EQ(key.value, nemo::ParameterValue{2.0});
        if (key.time == 10.0)
            EXPECT_EQ(key.value, nemo::ParameterValue{4.0});
    }

    // An unknown parameter is rejected without touching document or history.
    const auto unchanged = session.revision();
    EXPECT_FALSE(controller.resetNodeParameterEdit(network, node, QStringLiteral("missing")));
    EXPECT_FALSE(controller.error().isEmpty());
    EXPECT_EQ(session.revision(), unchanged);
}

// Issue #75 story 22/76: the identity-scoped reset resolves a subnet occurrence
// and an exposed control, never falling back to the root network.
TEST(Interactive, ResetResolvesOccurrenceAndExposedIdentity) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = controller.rootNetworkId();
    // A collapsible selection is a connected image chain of ordinary nodes: the
    // network's formal Output terminal stays outside the selection.
    const auto source = controller.createGraphNode(scope, "constcolor", "resetSource", 0.0, 0.0, {}, {});
    const auto merge = controller.createGraphNode(scope, "merge", "resetMerge", 160.0, 0.0, {}, {});
    const auto output = controller.createGraphNode(scope, "output", "resetOutput", 320.0, 0.0, {}, {});
    ASSERT_FALSE(source.isEmpty());
    ASSERT_FALSE(merge.isEmpty());
    ASSERT_FALSE(output.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, source, 0, merge, 0));
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, merge, 0, output, 0));
    const auto subnet = controller.collapseSelection(scope, QVariantList{source, merge}, "ResetScope");
    ASSERT_FALSE(subnet.isEmpty()) << controller.error().toStdString();
    const auto node = namedNode(controller, "ResetScope");
    const auto definition = node.value("definition").toString();
    const auto instance = node.value("instance").toString();
    ASSERT_FALSE(definition.isEmpty());
    ASSERT_FALSE(instance.isEmpty());
    ASSERT_TRUE(controller.promoteParameter(definition, source, "color", "")) << controller.error().toStdString();
    const auto exposure = controller.subnetExposure(scope, subnet);
    ASSERT_TRUE(exposure.value("available").toBool());
    // The edit target is the inspector's exposed identity, not the child key:
    // the resolver addresses an occurrence control through "exposed:<id>".
    const auto inspector = controller.parameterInspector(scope, subnet);
    ASSERT_TRUE(inspector.value("available").toBool()) << inspector.value("reason").toString().toStdString();
    const auto control =
        inspector.value("sections").toList().first().toMap().value("parameters").toList().first().toMap();
    const auto exposedKey = control.value("key").toString();
    ASSERT_FALSE(exposedKey.isEmpty());

    // An edit through the exposed inspector row is an occurrence-local
    // override: that occurrence's own value changes while the definition and a
    // sibling occurrence of the same definition stay as they were.
    const auto edit = controller.beginNodeParameterEdit(scope, subnet, exposedKey);
    ASSERT_FALSE(edit.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdit(edit, QVariantList{0.25, 0.5, 0.75, 1.0}))
        << controller.error().toStdString();
    ASSERT_TRUE(controller.commitNodeParameterEdit(edit));
    const auto editedRow = firstInspectorRow(controller, scope, subnet);
    ASSERT_FALSE(editedRow.isEmpty());
    EXPECT_EQ(editedRow.value(QStringLiteral("value")).toList(), QVariantList({0.25, 0.5, 0.75, 1.0}));

    // The definition's child keeps its own value: the override belongs to the
    // occurrence, not to the shared definition.
    const auto definitionRow = inspectorRow(controller.parameterInspector(definition, source), QStringLiteral("color"));
    ASSERT_FALSE(definitionRow.isEmpty());
    EXPECT_EQ(definitionRow.value(QStringLiteral("value")).toList(), QVariantList({1.0, 1.0, 1.0, 1.0}));

    // A sibling occurrence of the same definition carries its own value and
    // must not be affected by the first occurrence's reset.
    const auto sibling = controller.duplicateLinkedInstance(scope, subnet, 420.0, 80.0);
    ASSERT_FALSE(sibling.isEmpty()) << controller.error().toStdString();
    const auto siblingKey = firstInspectorRow(controller, scope, sibling).value(QStringLiteral("key")).toString();
    const auto siblingEdit = controller.beginNodeParameterEdit(scope, sibling, siblingKey);
    ASSERT_FALSE(siblingEdit.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdit(siblingEdit, QVariantList{0.125, 0.25, 0.5, 1.0}))
        << controller.error().toStdString();
    ASSERT_TRUE(controller.commitNodeParameterEdit(siblingEdit));

    // Reset resolves the intended occurrence: its value returns to the schema
    // default in one history entry, the definition child and the sibling
    // occurrence keep their own values.
    const auto revision = session.revision();
    EXPECT_TRUE(controller.resetNodeParameterEdit(scope, subnet, exposedKey)) << controller.error().toStdString();
    EXPECT_EQ(session.revision(), revision + 1);
    const auto resetRow = firstInspectorRow(controller, scope, subnet);
    ASSERT_FALSE(resetRow.isEmpty());
    EXPECT_EQ(resetRow.value(QStringLiteral("value")).toList(), QVariantList({1.0, 1.0, 1.0, 1.0}));
    EXPECT_EQ(firstInspectorRow(controller, scope, sibling).value(QStringLiteral("value")).toList(),
              QVariantList({0.125, 0.25, 0.5, 1.0}))
        << "resetting one occurrence must not touch another occurrence of the same definition";
    EXPECT_EQ(inspectorRow(controller.parameterInspector(definition, source), QStringLiteral("color"))
                  .value(QStringLiteral("value"))
                  .toList(),
              QVariantList({1.0, 1.0, 1.0, 1.0}));
    // Already at the default: a second reset is a no-op.
    EXPECT_TRUE(controller.resetNodeParameterEdit(scope, subnet, exposedKey));
    EXPECT_EQ(session.revision(), revision + 1);

    // A parameter the target scope does not own is rejected without touching
    // document or history.
    const auto unchanged = session.revision();
    EXPECT_FALSE(controller.resetNodeParameterEdit(scope, QString::number(fixture.node), QStringLiteral("missing")));
    EXPECT_FALSE(controller.error().isEmpty());
    EXPECT_EQ(session.revision(), unchanged);
}

// Issue #75 story 33-35/41: the controller exposes the declared input occupancy
// and refuses a meaningless swap without touching history.
TEST(Interactive, NodeInputOccupancyGuardsTheAtomicSwap) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = controller.rootNetworkId();
    const auto first = controller.createGraphNode(scope, "constcolor", "first", 0.0, 0.0, {}, {});
    const auto second = controller.createGraphNode(scope, "constcolor", "second", 0.0, 80.0, {}, {});
    const auto merge = controller.createGraphNode(scope, "merge", "swapMerge", 160.0, 0.0, {}, {});
    ASSERT_FALSE(first.isEmpty());
    ASSERT_FALSE(second.isEmpty());
    ASSERT_FALSE(merge.isEmpty());

    auto occupancy = controller.nodeInputOccupancy(scope, merge);
    ASSERT_TRUE(occupancy.value(QStringLiteral("available")).toBool());
    auto ports = occupancy.value(QStringLiteral("ports")).toList();
    ASSERT_GE(ports.size(), 3);
    EXPECT_FALSE(ports.at(0).toMap().value(QStringLiteral("connected")).toBool());
    EXPECT_FALSE(ports.at(1).toMap().value(QStringLiteral("connected")).toBool());
    EXPECT_TRUE(ports.at(2).toMap().value(QStringLiteral("optional")).toBool());
    EXPECT_EQ(ports.at(2).toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("mask"));

    // Nothing connected: the swap is refused with an explanation and no entry.
    const auto emptyRevision = session.revision();
    EXPECT_FALSE(controller.swapNodeInputs(scope, merge, 0, 1));
    EXPECT_FALSE(controller.error().isEmpty());
    EXPECT_EQ(session.revision(), emptyRevision);

    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, first, 0, merge, 0));
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, second, 0, merge, 1));
    occupancy = controller.nodeInputOccupancy(scope, merge);
    ports = occupancy.value(QStringLiteral("ports")).toList();
    EXPECT_TRUE(ports.at(0).toMap().value(QStringLiteral("connected")).toBool());
    EXPECT_TRUE(ports.at(1).toMap().value(QStringLiteral("connected")).toBool());
    EXPECT_NE(ports.at(0).toMap().value(QStringLiteral("source")).toString(),
              ports.at(1).toMap().value(QStringLiteral("source")).toString());

    const auto swapRevision = session.revision();
    const auto edgesBefore = controller.graphEdges();
    EXPECT_TRUE(controller.swapNodeInputs(scope, merge, 0, 1)) << controller.error().toStdString();
    EXPECT_EQ(session.revision(), swapRevision + 1);
    EXPECT_NE(controller.graphEdges(), edgesBefore);
    // The mask slot is retained by the swap and undo restores both edges.
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    EXPECT_EQ(controller.graphEdges(), edgesBefore);

    // Two edges from one source cannot be swapped.
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, first, 0, merge, 1));
    const auto sameSourceRevision = session.revision();
    EXPECT_FALSE(controller.swapNodeInputs(scope, merge, 0, 1));
    EXPECT_EQ(session.revision(), sameSourceRevision);
}

// Issue #75 stories 9/42: soft presentation travel never replaces a semantic
// constraint, a generic parameter edit cannot author an invalid mapping, and
// the gesture guard rejects what the executor would reject while preserving
// state.
TEST(Interactive, SemanticBoundsStayAuthoritativeWhilePresentationTravelIsSoft) {
    nemo::Document document;
    const auto network = document.rootNetworkId();
    auto& graph = document.network(network).graph();
    const auto transform = graph.addNode("transform", "motion");
    const auto read = graph.addNode("source", "reader");

    // Valid large and negative typed translation/rotation are accepted: the
    // slider travel is a presentation range, not a legal-value bound.
    EXPECT_NO_THROW(graph.setParam(transform, "translateX", nemo::ParameterValue{-1500.0}));
    EXPECT_NO_THROW(graph.setParam(transform, "translateX", nemo::ParameterValue{4096.0}));
    EXPECT_NO_THROW(graph.setParam(transform, "rotate", nemo::ParameterValue{-720.0}));
    // A finite nonzero scale may mirror the image; zero has no inverse mapping.
    EXPECT_THROW(graph.setParam(transform, "scale", nemo::ParameterValue{0.0}), nemo::GraphException);
    graph.setParam(transform, "scale", nemo::ParameterValue{-4.0});
    // Mix can extrapolate; its 0..1 slider travel is not a legal-value bound.
    graph.setParam(transform, "mix", nemo::ParameterValue{2.0});
    // A zero source step would create an invalid mapping; the catalog's nonzero
    // constraint rejects it on the generic edit rather than at evaluation.
    EXPECT_THROW(graph.setParam(read, "frameStep", nemo::ParameterValue{std::int64_t{0}}), nemo::GraphException);
    EXPECT_NO_THROW(graph.setParam(read, "frameStep", nemo::ParameterValue{std::int64_t{-2}}));

    // The inspector publishes the exact authored text so an Integer edit never
    // round-trips through a JavaScript double.
    nemo::ProjectSession session(std::move(document));
    EXPECT_EQ(session.queryValues(network, transform, "mix").front().value, nemo::ParameterValue{2.0});
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto row = inspectorRow(controller.parameterInspector(QString::number(network), QString::number(read)),
                                  QStringLiteral("frameStep"));
    ASSERT_FALSE(row.isEmpty());
    EXPECT_EQ(row.value(QStringLiteral("valueText")).toString(), QStringLiteral("-2"));

    // The parameter gesture guard uses the executor's admissibility owner: an
    // invalid enabled value is refused and the document stays as it was.
    const auto scaleRow = inspectorRow(
        controller.parameterInspector(QString::number(network), QString::number(transform)), QStringLiteral("scale"));
    ASSERT_FALSE(scaleRow.isEmpty());
    const auto guardRevision = session.revision();
    const auto rejected = controller.beginNodeParameterEdit(QString::number(network), QString::number(transform),
                                                            QStringLiteral("scale"));
    ASSERT_FALSE(rejected.isEmpty()) << controller.error().toStdString();
    EXPECT_FALSE(controller.updateNodeParameterEdit(rejected, 0.0));
    EXPECT_FALSE(controller.error().isEmpty());
    EXPECT_FALSE(controller.commitNodeParameterEdit(rejected));
    EXPECT_EQ(session.revision(), guardRevision);
    const auto scaleValues = session.queryValues(network, transform, "scale");
    ASSERT_FALSE(scaleValues.empty());
    EXPECT_EQ(scaleValues.front().value, nemo::ParameterValue{-4.0});

    const auto accepted = controller.beginNodeParameterEdit(QString::number(network), QString::number(transform),
                                                            QStringLiteral("scale"));
    ASSERT_FALSE(accepted.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdit(accepted, 0.05)) << controller.error().toStdString();
    ASSERT_TRUE(controller.commitNodeParameterEdit(accepted));
    EXPECT_EQ(session.revision(), guardRevision + 1);

    // A typed gesture carries the exact text through the catalog parser: a
    // 64-bit value beyond the safe double range stays exact.
    nemo::Document exact;
    const auto exactNetwork = exact.rootNetworkId();
    const auto exactNode = exact.network(exactNetwork).graph().addNode("source", "exact");
    nemo::ProjectSession exactSession(std::move(exact));
    nemo::ui::ParameterInteraction exactInteraction;
    nemo::ui::ViewerController exactController(&runtime, exactSession, exactInteraction);
    const auto token = exactController.beginNodeParameterEdit(QString::number(exactNetwork), QString::number(exactNode),
                                                              QStringLiteral("frameOffset"));
    ASSERT_FALSE(token.isEmpty()) << exactController.error().toStdString();
    EXPECT_TRUE(exactController.updateNodeParameterEdit(token, QStringLiteral("9007199254740993")));
    EXPECT_TRUE(exactController.commitNodeParameterEdit(token)) << exactController.error().toStdString();
    const auto exactValues = exactSession.queryValues(exactNetwork, exactNode, "frameOffset");
    ASSERT_FALSE(exactValues.empty());
    EXPECT_EQ(exactValues.front().value, nemo::ParameterValue{std::int64_t{9007199254740993}});
}

// Issue #75: one atomic batch may mix an animated address with a static
// companion. The core owner decides per address (existing channel -> key,
// unanimated -> static), the batch is one history entry, and no channel is
// created for the static field.
TEST(Interactive, MixedAnimatedAndStaticBatchIsOneAtomicEdit) {
    nemo::Document document;
    const auto network = document.rootNetworkId();
    const auto node = document.network(network).graph().addNode("source", "Read");
    const nemo::ParameterAddress choiceAddress{network, node, "inputTransform", nemo::kInvalidNetworkInstance};
    nemo::Keyframe authoredChoice{0, 0.0, nemo::ParameterValue{nemo::ChoiceValue{"auto"}}};
    authoredChoice.interpolation = nemo::KeyInterpolation::Hold;
    nemo::setKeyframesCommand({nemo::KeyframeEdit{choiceAddress, authoredChoice}}).apply(document);
    nemo::ProjectSession session(std::move(document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = QString::number(network);
    const auto id = QString::number(node);
    controller.setFrame(5);
    const auto inputSpaceBefore = session.queryValues(network, node, "inputColorSpace");
    ASSERT_EQ(inputSpaceBefore.size(), 1U);

    const auto token = controller.beginNodeParameterEdits(
        scope, id, QStringList{QStringLiteral("inputTransform"), QStringLiteral("inputColorSpace")});
    ASSERT_FALSE(token.isEmpty()) << controller.error().toStdString();
    const auto revision = session.revision();
    ASSERT_TRUE(controller.updateNodeParameterEdits(
        token, QVariantMap{{QStringLiteral("inputTransform"), QStringLiteral("explicit")},
                           {QStringLiteral("inputColorSpace"), QStringLiteral("sRGB - Texture")}}))
        << controller.error().toStdString();
    ASSERT_TRUE(controller.commitNodeParameterEdit(token)) << controller.error().toStdString();
    EXPECT_EQ(session.revision(), revision + 1) << "one batch is one history entry";

    // The animated address authored/updated the current-frame key; its other key
    // is untouched.
    const auto* channel = session.document().animationChannel(choiceAddress);
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(channel->keys.size(), 2U);
    const auto authored = std::find_if(channel->keys.begin(), channel->keys.end(),
                                       [](const nemo::Keyframe& key) { return key.time == 5.0; });
    ASSERT_NE(authored, channel->keys.end());
    EXPECT_EQ(authored->value, (nemo::ParameterValue{nemo::ChoiceValue{"explicit"}}));
    const auto original = std::find_if(channel->keys.begin(), channel->keys.end(),
                                       [](const nemo::Keyframe& key) { return key.time == 0.0; });
    ASSERT_NE(original, channel->keys.end());
    EXPECT_EQ(original->value, (nemo::ParameterValue{nemo::ChoiceValue{"auto"}}));

    // The static companion took the static value and created no channel.
    const nemo::ParameterAddress spaceAddress{network, node, "inputColorSpace", nemo::kInvalidNetworkInstance};
    EXPECT_EQ(session.document().animationChannel(spaceAddress), nullptr)
        << "an unanimated address must not gain a channel";
    const auto values = session.queryValues(network, node, "inputColorSpace");
    ASSERT_FALSE(values.empty());
    EXPECT_EQ(std::get<std::string>(values.front().value), "sRGB - Texture");

    // Undo restores both addresses together.
    ASSERT_TRUE(session.undo(nemo::EditOptions{.expectedRevision = session.revision()}).committed);
    const auto inputSpaceAfterUndo = session.queryValues(network, node, "inputColorSpace");
    ASSERT_EQ(inputSpaceAfterUndo.size(), 1U);
    EXPECT_EQ(inputSpaceAfterUndo.front().value, inputSpaceBefore.front().value);
    const auto* restored = session.document().animationChannel(choiceAddress);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->keys.size(), 1U);

    // The batch applies again after the undo ...
    const auto replayRevision = session.revision();
    const auto replay = controller.beginNodeParameterEdits(
        scope, id, QStringList{QStringLiteral("inputTransform"), QStringLiteral("inputColorSpace")});
    ASSERT_FALSE(replay.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdits(
        replay, QVariantMap{{QStringLiteral("inputTransform"), QStringLiteral("explicit")},
                            {QStringLiteral("inputColorSpace"), QStringLiteral("sRGB - Texture")}}))
        << controller.error().toStdString();
    ASSERT_TRUE(controller.commitNodeParameterEdit(replay)) << controller.error().toStdString();
    EXPECT_EQ(session.revision(), replayRevision + 1);

    // ... and repeating the identical batch is a completed no-op: the owner
    // publishes nothing, so the commit reports success with no revision change
    // and no invented error.
    const auto noopRevision = session.revision();
    const auto noop = controller.beginNodeParameterEdits(
        scope, id, QStringList{QStringLiteral("inputTransform"), QStringLiteral("inputColorSpace")});
    ASSERT_FALSE(noop.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdits(
        noop, QVariantMap{{QStringLiteral("inputTransform"), QStringLiteral("explicit")},
                          {QStringLiteral("inputColorSpace"), QStringLiteral("sRGB - Texture")}}))
        << controller.error().toStdString();
    EXPECT_TRUE(controller.commitNodeParameterEdit(noop)) << controller.error().toStdString();
    EXPECT_EQ(session.revision(), noopRevision) << "an unchanged batch publishes nothing";
    EXPECT_TRUE(controller.error().isEmpty());

    // A refused candidate latches the gesture: commit cannot publish the
    // previous preview.
    const auto guard = session.revision();
    const auto refused = controller.beginNodeParameterEdits(scope, id, QStringList{QStringLiteral("inputTransform")});
    ASSERT_FALSE(refused.isEmpty()) << controller.error().toStdString();
    EXPECT_FALSE(controller.updateNodeParameterEdits(
        refused, QVariantMap{{QStringLiteral("inputTransform"), QStringLiteral("not-a-mode")}}));
    EXPECT_FALSE(controller.commitNodeParameterEdit(refused));
    EXPECT_EQ(session.revision(), guard);
}

// Issue #75: two Reads inside one definition may expose the SAME parameter key.
// The host must publish a distinct resolved target per exposure row, and a
// gesture through one row must change only that child — no cross-target write,
// no fallback for a key the occurrence does not expose.
TEST(Interactive, OccurrenceExposuresResolveTheirOwnChildTarget) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ParameterInteraction interaction;
    nemo::ui::ViewerController controller(&runtime, session, interaction);
    const auto scope = controller.rootNetworkId();
    const auto readA = controller.createGraphNode(scope, "source", "ReadA", 0.0, 0.0, {}, {});
    const auto readB = controller.createGraphNode(scope, "source", "ReadB", 0.0, 120.0, {}, {});
    const auto merge = controller.createGraphNode(scope, "merge", "PairMerge", 200.0, 60.0, {}, {});
    ASSERT_FALSE(readA.isEmpty());
    ASSERT_FALSE(readB.isEmpty());
    ASSERT_FALSE(merge.isEmpty());
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, readA, 0, merge, 0));
    ASSERT_TRUE(controller.connectOrReplaceGraph(scope, readB, 0, merge, 1));
    const auto subnet = controller.collapseSelection(scope, QVariantList{readA, readB, merge}, "PairSubnet");
    ASSERT_FALSE(subnet.isEmpty()) << controller.error().toStdString();
    const auto node = namedNode(controller, "PairSubnet");
    const auto definition = node.value("definition").toString();
    ASSERT_FALSE(definition.isEmpty());
    ASSERT_TRUE(controller.promoteParameter(definition, readA, "frameOffset", "")) << controller.error().toStdString();
    ASSERT_TRUE(controller.promoteParameter(definition, readB, "frameOffset", "")) << controller.error().toStdString();

    const auto inspector = controller.parameterInspector(scope, subnet);
    ASSERT_TRUE(inspector.value(QStringLiteral("available")).toBool())
        << inspector.value(QStringLiteral("reason")).toString().toStdString();
    const auto rows = inspector.value(QStringLiteral("sections"))
                          .toList()
                          .first()
                          .toMap()
                          .value(QStringLiteral("parameters"))
                          .toList();
    ASSERT_EQ(rows.size(), 2);
    const auto first = rows.at(0).toMap();
    const auto second = rows.at(1).toMap();
    // One child key, two different resolved targets and two different handles.
    EXPECT_EQ(first.value(QStringLiteral("targetKey")).toString(), QStringLiteral("frameOffset"));
    EXPECT_EQ(second.value(QStringLiteral("targetKey")).toString(), QStringLiteral("frameOffset"));
    EXPECT_NE(first.value(QStringLiteral("targetNode")).toString(),
              second.value(QStringLiteral("targetNode")).toString())
        << "a shared child key must not collapse two Reads onto one target";
    EXPECT_NE(first.value(QStringLiteral("key")).toString(), second.value(QStringLiteral("key")).toString());
    EXPECT_EQ(first.value(QStringLiteral("targetNetwork")).toString(), definition);
    EXPECT_EQ(second.value(QStringLiteral("targetNetwork")).toString(), definition);
    // The accepted exposed-only scope: a key this occurrence does not expose has
    // no row at all, so no fallback mutation is possible through it.
    for (const auto& row : rows)
        EXPECT_NE(row.toMap().value(QStringLiteral("targetKey")).toString(), QStringLiteral("inputColorSpace"));

    // Editing the FIRST exposure writes only its own child; the sibling Read's
    // occurrence-effective value is unchanged.
    const auto token = controller.beginNodeParameterEdit(scope, subnet, first.value(QStringLiteral("key")).toString());
    ASSERT_FALSE(token.isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.updateNodeParameterEdit(token, 3)) << controller.error().toStdString();
    ASSERT_TRUE(controller.commitNodeParameterEdit(token)) << controller.error().toStdString();
    const auto after = controller.parameterInspector(scope, subnet)
                           .value(QStringLiteral("sections"))
                           .toList()
                           .first()
                           .toMap()
                           .value(QStringLiteral("parameters"))
                           .toList();
    ASSERT_EQ(after.size(), 2);
    EXPECT_EQ(after.at(0).toMap().value(QStringLiteral("value")).toLongLong(), 3);
    EXPECT_EQ(after.at(1).toMap().value(QStringLiteral("value")).toLongLong(), 0)
        << "editing one occurrence exposure must not write the sibling Read";

    // A definition-hosted Read publishes the same identity block with itself as
    // the target, so one contract covers both hosting modes.
    const auto definitionRow =
        inspectorRow(controller.parameterInspector(definition, readA), QStringLiteral("frameOffset"));
    ASSERT_FALSE(definitionRow.isEmpty());
    EXPECT_EQ(definitionRow.value(QStringLiteral("targetNetwork")).toString(), definition);
    EXPECT_EQ(definitionRow.value(QStringLiteral("targetNode")).toString(), readA);
    EXPECT_EQ(definitionRow.value(QStringLiteral("targetKey")).toString(), QStringLiteral("frameOffset"));
}
}  // namespace
