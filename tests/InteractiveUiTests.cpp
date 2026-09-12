#include "ParameterEditorRegistry.hpp"
#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/gpu/Error.hpp"

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
    nemo::ui::ViewerController controller(&runtime, session);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    controller.setNodeParameter(namedNode(controller, "background").value("id").toString(), "color",
                                QVariantList{QVariant{0.2}, QVariant{0.3}, QVariant{0.4}, QVariant{1.0}});
    const auto color = namedNode(controller, "background").value("params").toMap().value("color").toList();
    ASSERT_EQ(color.size(), 4);
    EXPECT_FLOAT_EQ(color.at(0).toFloat(), 0.2F);
    EXPECT_FLOAT_EQ(color.at(1).toFloat(), 0.3F);
    EXPECT_FLOAT_EQ(color.at(2).toFloat(), 0.4F);
    EXPECT_FLOAT_EQ(color.at(3).toFloat(), 1.0F);
    ASSERT_TRUE(controller.undo());
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
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.graphEdges(), originalEdges);
    ASSERT_TRUE(controller.redo());
    EXPECT_NE(controller.graphEdges(), originalEdges);
}

TEST(Interactive, GraphSnapshotPublishesAuthoredPositionsPortsRoutesAndStableIds) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
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
    ASSERT_TRUE(controller.undo());
    EXPECT_DOUBLE_EQ(namedNode(controller, "stableSource").value("x").toDouble(), 120.5);
}

TEST(Interactive, GraphScopeEditsDoNotFallBackToRoot) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
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
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.graphSnapshot(other), otherBefore);
    const auto revision = session.revision();
    EXPECT_TRUE(controller.createGraphNode("999999", "constcolor", "Invalid", 0, 0, {}, {}).isEmpty());
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(controller.graphSnapshot(root), rootBefore);
}
TEST(Interactive, DisconnectedProcessingNodeDropsOntoWireAtomically) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
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
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.graphEdges().size(), 1);
    EXPECT_TRUE(namedNode(controller, "wireMerge").value("id").toString() == mergeId);
}

TEST(Interactive, GraphCreationUndoPreservesExistingConnections) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    const auto before = controller.graphEdges();
    const auto scope = QString::number(session.document().rootNetworkId());
    ASSERT_FALSE(controller.createGraphNode(scope, "output", "independentOutput", 0.0, 0.0, {}, {}).isEmpty());
    const auto output = namedNode(controller, "independentOutput");
    ASSERT_FALSE(output.isEmpty());
    EXPECT_TRUE(
        controller.connectOrReplaceGraph(scope, namedNode(controller, "source").value("id"), 0, output.value("id"), 0));
    ASSERT_TRUE(controller.error().isEmpty()) << controller.error().toStdString();
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.graphEdges(), before);
    ASSERT_TRUE(controller.undo());
    EXPECT_TRUE(namedNode(controller, "independentOutput").isEmpty());
    EXPECT_EQ(controller.graphEdges(), before);
}

TEST(Interactive, TimelineSlipAndRetimeUseSourceMappingAndUndoIndependently) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    controller.setFrame(3);
    controller.slipTimelineClip("src", 7);
    controller.retimeTimelineClip("src", 2);
    ASSERT_EQ(controller.timelineClips().size(), 1);
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 13);
    EXPECT_EQ(controller.frame(), 3) << "Source timing edits must not move the playhead";
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 10);
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 3);
    controller.retimeTimelineClip("src", 0);
    EXPECT_FALSE(controller.error().isEmpty());
    ASSERT_TRUE(controller.redo()) << "Invalid timing must preserve command history";
    EXPECT_EQ(controller.timelineClips().first().toMap().value("sourceFrame").toLongLong(), 10);
}

TEST(Interactive, UndoingSourceImportCancelsProbeAndRedoRequestsFreshMetadata) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    ASSERT_TRUE(controller.pending());
    ASSERT_TRUE(controller.undo());
    EXPECT_TRUE(controller.graphNodes().isEmpty());
    EXPECT_FALSE(controller.pending());
    EXPECT_FALSE(controller.hasSource());
    EXPECT_EQ(runtime.counts().queued, 0u);

    ASSERT_TRUE(controller.redo());
    EXPECT_TRUE(controller.pending());
    EXPECT_EQ(namedNode(controller, "source").value("params").toMap().value("source").toString(), "src");
    EXPECT_EQ(runtime.counts().queued, 1u);
}

TEST(Interactive, ViewerAssignmentAttachesTargetAndIsOneUndoableCommand) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
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
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.viewerCount(scope), 0);
    EXPECT_TRUE(controller.viewerTargetId().isEmpty());
    ASSERT_TRUE(controller.redo());
    EXPECT_EQ(controller.viewerTargetId(), mergeId);
}

TEST(Interactive, ViewerIndicesFollowNodeIdOrderAndActiveViewerSelectsTarget) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
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
    nemo::ui::ViewerController controller(&runtime, session);
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
    nemo::ui::ViewerController controller(&runtime, session);
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

TEST(Interactive, PresentationConsumersShareSessionHistoryAndLifetime) {
    nemo::ui::ViewerRuntime firstRuntime;
    nemo::ui::ViewerRuntime secondRuntime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController first(&firstRuntime, session);
    QSignalSpy firstGraph(&first, &nemo::ui::ViewerController::graphChanged);
    QSignalSpy firstTimeline(&first, &nemo::ui::ViewerController::timelineChanged);
    QSignalSpy firstHistory(&first, &nemo::ui::ViewerController::historyChanged);
    QSignalSpy firstCatalog(&first, &nemo::ui::ViewerController::catalogChanged);

    {
        nemo::ui::ViewerController second(&secondRuntime, session);
        QSignalSpy secondGraph(&second, &nemo::ui::ViewerController::graphChanged);
        QSignalSpy secondTimeline(&second, &nemo::ui::ViewerController::timelineChanged);
        QSignalSpy secondHistory(&second, &nemo::ui::ViewerController::historyChanged);
        QSignalSpy secondCatalog(&second, &nemo::ui::ViewerController::catalogChanged);

        first.openSource("/tmp/nemo-shared-session-source.mkv");
        EXPECT_EQ(firstGraph.count(), 1);
        EXPECT_EQ(secondGraph.count(), 1);
        EXPECT_EQ(firstTimeline.count(), 1);
        EXPECT_EQ(secondTimeline.count(), 1);
        EXPECT_EQ(firstHistory.count(), 1);
        EXPECT_EQ(secondHistory.count(), 1);
        EXPECT_EQ(firstCatalog.count(), 1);
        EXPECT_EQ(secondCatalog.count(), 1);
        ASSERT_TRUE(first.canUndo());

        firstGraph.clear();
        secondGraph.clear();
        firstTimeline.clear();
        secondTimeline.clear();
        firstHistory.clear();
        secondHistory.clear();
        firstCatalog.clear();
        secondCatalog.clear();
        ASSERT_TRUE(second.undo());
        EXPECT_TRUE(first.graphNodes().isEmpty());
        EXPECT_TRUE(second.graphNodes().isEmpty());
        EXPECT_EQ(firstGraph.count(), 1);
        EXPECT_EQ(secondGraph.count(), 1);
        EXPECT_EQ(firstTimeline.count(), 1);
        EXPECT_EQ(secondTimeline.count(), 1);
        EXPECT_EQ(firstHistory.count(), 1);
        EXPECT_EQ(secondHistory.count(), 1);
        EXPECT_EQ(firstCatalog.count(), 1);
        EXPECT_EQ(secondCatalog.count(), 1);

        firstGraph.clear();
        secondGraph.clear();
        firstTimeline.clear();
        secondTimeline.clear();
        firstHistory.clear();
        secondHistory.clear();
        firstCatalog.clear();
        secondCatalog.clear();
        static_cast<void>(
            session.submit(nemo::addNodeCommand(session.document().rootNetworkId(), "testpattern", "direct"),
                           nemo::EditOptions{.expectedRevision = session.revision()}));
        EXPECT_EQ(firstGraph.count(), 1);
        EXPECT_EQ(secondGraph.count(), 1);
        EXPECT_EQ(firstTimeline.count(), 1);
        EXPECT_EQ(secondTimeline.count(), 1);
        EXPECT_EQ(firstHistory.count(), 1);
        EXPECT_EQ(secondHistory.count(), 1);
        EXPECT_EQ(firstCatalog.count(), 1);
        EXPECT_EQ(secondCatalog.count(), 1);
        EXPECT_FALSE(first.graphNodes().isEmpty());
        EXPECT_FALSE(second.graphNodes().isEmpty());
    }

    // The surviving controller remains subscribed after the other consumer's
    // explicit lifetime ends.
    firstGraph.clear();
    firstTimeline.clear();
    firstHistory.clear();
    firstCatalog.clear();
    EXPECT_TRUE(first.undo());
    EXPECT_EQ(firstGraph.count(), 1);
    EXPECT_EQ(firstTimeline.count(), 1);
    EXPECT_EQ(firstHistory.count(), 1);
    EXPECT_EQ(firstCatalog.count(), 1);
    EXPECT_TRUE(first.graphNodes().isEmpty());
}
TEST(Interactive, IntegerTextEditsPreservePrecisionAndRejectOverflowAtomically) {
    nemo::NodeDescriptor descriptor{
        .type = "test.integer",
        .displayName = "Integer",
        .group = "Tests",
        .parameters = {{.name = "count", .type = nemo::ParameterType::Integer, .defaultValue = std::int64_t{0}}}};
    auto catalog = std::make_shared<nemo::NodeCatalog>(std::vector<nemo::NodeDescriptor>{descriptor});
    nemo::Document document(catalog);
    const auto network = document.rootNetworkId();
    const auto node = document.network(network).graph().addNode("test.integer", "control");
    nemo::ProjectSession session(std::move(document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ViewerController controller(&runtime, session);
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
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(std::get<std::int64_t>(session.queryValues(network, node).front().value), 0);
    EXPECT_FALSE(controller.canUndo());
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
    fixture.catalog = std::make_shared<nemo::NodeCatalog>(std::vector<nemo::NodeDescriptor>{descriptor});
    fixture.document = nemo::Document{fixture.catalog};
    const auto network = fixture.document.rootNetworkId();
    fixture.node = fixture.document.network(network).graph().addNode("test.inspector", "inspector");
    return fixture;
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
    nemo::ui::ViewerController controller(&runtime, session);
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
    nemo::ui::ViewerController controller(&runtime, session);
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
    nemo::ui::ViewerController controller(&runtime, session);
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

    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(controller.nodeParameterKeyStatus(network, node, QStringLiteral("gain")), QStringLiteral("animated"));
    ASSERT_TRUE(controller.redo());
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
    nemo::ui::ViewerController controller(&runtime, session);
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
    EXPECT_TRUE(controller.undo());
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

    // Only one gesture may be active at a time.
    const auto active = controller.beginNodeParameterEdit(network, node, QStringLiteral("count"));
    ASSERT_FALSE(active.isEmpty());
    EXPECT_TRUE(controller.beginNodeParameterEdit(network, node, QStringLiteral("count")).isEmpty());
    EXPECT_TRUE(controller.cancelNodeParameterEdit(active));

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

TEST(Interactive, JavaScriptArrayEditsConvertVectorAndColorParameters) {
    auto fixture = inspectorFixture();
    nemo::ProjectSession session(std::move(fixture.document));
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ViewerController controller(&runtime, session);
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

TEST(Interactive, ParameterEditorRegistryRegistersAndResolvesEditors) {
    nemo::ui::ParameterEditorRegistry registry;
    QSignalSpy changed(&registry, &nemo::ui::ParameterEditorRegistry::editorsChanged);

    const auto missing = registry.editor(QStringLiteral("nemo.missing"));
    EXPECT_FALSE(missing.value(QStringLiteral("available")).toBool());
    EXPECT_TRUE(missing.value(QStringLiteral("source")).toString().isEmpty());
    EXPECT_EQ(missing.value(QStringLiteral("reason")).toString(),
              QStringLiteral("No parameter editor registered for 'nemo.missing'"));
    EXPECT_EQ(registry.reason(QStringLiteral("nemo.missing")),
              QStringLiteral("No parameter editor registered for 'nemo.missing'"));

    EXPECT_FALSE(
        registry.registerEditor(QStringLiteral("unnamespaced"), QUrl(QStringLiteral("qrc:/Nemo/Unnamed.qml"))));
    EXPECT_FALSE(registry.registerEditor(QString{}, QUrl(QStringLiteral("qrc:/Nemo/Unnamed.qml"))));
    EXPECT_FALSE(registry.registerEditor(QStringLiteral("nemo.blank"), QUrl{}));
    EXPECT_EQ(changed.count(), 0);

    const QUrl source{QStringLiteral("qrc:/Nemo/Linear.qml")};
    EXPECT_TRUE(registry.registerEditor(QStringLiteral("nemo.linear"), source));
    EXPECT_EQ(changed.count(), 1);
    const auto registered = registry.editor(QStringLiteral("nemo.linear"));
    EXPECT_TRUE(registered.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(registered.value(QStringLiteral("source")).toUrl(), source);
    EXPECT_TRUE(registered.value(QStringLiteral("reason")).toString().isEmpty());
    EXPECT_TRUE(registry.reason(QStringLiteral("nemo.linear")).isEmpty());

    // Re-registering replaces the source and keeps the registration usable.
    const QUrl replacement{QStringLiteral("qrc:/Nemo/LinearV2.qml")};
    EXPECT_TRUE(registry.registerEditor(QStringLiteral("nemo.linear"), replacement));
    EXPECT_EQ(registry.editor(QStringLiteral("nemo.linear")).value(QStringLiteral("source")).toUrl(), replacement);
    EXPECT_EQ(changed.count(), 2);

    EXPECT_TRUE(registry.unregisterEditor(QStringLiteral("nemo.linear")));
    EXPECT_EQ(changed.count(), 3);
    EXPECT_FALSE(registry.editor(QStringLiteral("nemo.linear")).value(QStringLiteral("available")).toBool());
    EXPECT_FALSE(registry.unregisterEditor(QStringLiteral("nemo.linear")));
    EXPECT_EQ(changed.count(), 3);
}

}  // namespace
