#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/gpu/Error.hpp"

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
TEST(Interactive, GraphCommandsUndoAndRejectOccupiedConnectionsAtomically) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    controller.setNodeParameter(namedNode(controller, "background").value("id").toULongLong(), "color",
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

    const auto edges = controller.graphEdges();
    controller.connectGraphNodes(namedNode(controller, "background").value("id").toULongLong(), 0,
                                 namedNode(controller, "composite").value("id").toULongLong(), 0);
    EXPECT_FALSE(controller.error().isEmpty());
    EXPECT_EQ(controller.graphEdges(), edges);
    ASSERT_TRUE(controller.redo()) << "A rejected connection must not destroy the redo branch";
    const auto redoColor = namedNode(controller, "background").value("params").toMap().value("color").toList();
    ASSERT_EQ(redoColor.size(), 4);
    EXPECT_FLOAT_EQ(redoColor.at(0).toFloat(), 0.2F);
    EXPECT_FLOAT_EQ(redoColor.at(1).toFloat(), 0.3F);
    EXPECT_FLOAT_EQ(redoColor.at(2).toFloat(), 0.4F);
    EXPECT_FLOAT_EQ(redoColor.at(3).toFloat(), 1.0F);
}

TEST(Interactive, GraphCreationUndoPreservesExistingConnections) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ProjectSession session{emptyDocument()};
    nemo::ui::ViewerController controller(&runtime, session);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    const auto before = controller.graphEdges();
    controller.addGraphNode("output", "independentOutput");
    const auto output = namedNode(controller, "independentOutput");
    ASSERT_FALSE(output.isEmpty());
    controller.connectGraphNodes(namedNode(controller, "source").value("id").toULongLong(), 0,
                                 output.value("id").toULongLong(), 0);
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

TEST(Interactive, OutputSelectionUsesCatalogDeclaration) {
    auto catalog = std::make_shared<nemo::NodeCatalog>(std::vector<nemo::NodeDescriptor>{
        nemo::NodeDescriptor{.type = "fixture.output",
                             .displayName = "Fixture Output",
                             .group = "I/O",
                             .isOutput = true,
                             .inputs = {{nemo::PortKind::Image, "color"}},
                             .capabilities = nemo::NodeCapabilities{.samplingScales = {1},
                                                                    .qualityModes = {nemo::Quality::Full},
                                                                    .channels = {"RGBA"}}}});
    nemo::Document document{std::move(catalog)};
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    nemo::ProjectSession session{std::move(document)};
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ViewerController controller(&runtime, session);

    static_cast<void>(
        session.submit(nemo::addNodeCommand(session.document().rootNetworkId(), "fixture.output", "declared"),
                       nemo::EditOptions{.expectedRevision = session.revision()}));
    ASSERT_EQ(controller.outputNames(), (QStringList{"declared"}));
    controller.setOutputName("declared");
    EXPECT_EQ(controller.outputName(), "declared");
    const nemo::NodeId declared = rootGraph(session.document()).nodeByName("declared")->id;
    const auto renamed =
        session.submit(nemo::renameNodeCommand(session.document().rootNetworkId(), declared, "renamed"),
                       nemo::EditOptions{.expectedRevision = session.revision()});
    ASSERT_TRUE(renamed.committed);
    EXPECT_EQ(controller.outputName(), "renamed");
    EXPECT_EQ(namedNode(controller, "renamed").value("id").toULongLong(), static_cast<qulonglong>(declared));
    const auto parameter = session.submit(
        nemo::setParamCommand(session.document().rootNetworkId(), declared, "marker", std::string{"stable"}),
        nemo::EditOptions{.expectedRevision = session.revision()});
    ASSERT_TRUE(parameter.committed);
    EXPECT_EQ(namedNode(controller, "renamed").value("params").toMap().value("marker").toString(), "stable");
    EXPECT_TRUE(controller.error().isEmpty());
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

}  // namespace
