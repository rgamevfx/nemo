#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "nemo/gpu/Error.hpp"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

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
// the immutable probe snapshot, but all observed edits use the real controller
// and CommandStack, exactly as QML does.
TEST(Interactive, GraphCommandsUndoAndRejectOccupiedConnectionsAtomically) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ViewerController controller(&runtime);
    controller.openSource("/tmp/nemo-interactive-command-source.mkv");
    controller.setNodeParameter("background", "color", "0.2 0.3 0.4 1");
    EXPECT_EQ(namedNode(controller, "background").value("params").toMap().value("color").toString(), "0.2 0.3 0.4 1");
    ASSERT_TRUE(controller.undo());
    EXPECT_EQ(namedNode(controller, "background").value("params").toMap().value("color").toString(), "0 0 0 0");

    const auto edges = controller.graphEdges();
    controller.connectGraphNodes(namedNode(controller, "background").value("id").toULongLong(), 0,
                                 namedNode(controller, "composite").value("id").toULongLong(), 0);
    EXPECT_FALSE(controller.error().isEmpty());
    EXPECT_EQ(controller.graphEdges(), edges);
    ASSERT_TRUE(controller.redo()) << "A rejected connection must not destroy the redo branch";
    EXPECT_EQ(namedNode(controller, "background").value("params").toMap().value("color").toString(), "0.2 0.3 0.4 1");
}

TEST(Interactive, GraphCreationUndoPreservesExistingConnections) {
    nemo::ui::ViewerRuntime runtime;
    nemo::ui::ViewerController controller(&runtime);
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
    nemo::ui::ViewerController controller(&runtime);
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
    nemo::ui::ViewerController controller(&runtime);
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

TEST(Interactive, CacheRangeReportsAsynchronousDiskAdmissionFailure) {
#ifndef NEMO_SLANG_SPV_DIR
    GTEST_SKIP() << "Native cache-range evidence requires compiled Slang shaders";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                        "docs/evidence/issue12-view.ocio";
    const nemo::test::ScopedEnvironment ocio("OCIO", config.string());
    nemo::Document document;
    nemo::CommandStack commands(document);
    auto color = std::make_shared<nemo::NodeId>();
    auto output = std::make_shared<nemo::NodeId>();
    commands.push(nemo::addNodeCommand("constcolor", "color", color));
    commands.push(nemo::addNodeCommand("output", "result", output));
    commands.push(nemo::connectCommand({*color, 0}, {*output, 0}));
    nemo::EvaluationRequest request;
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
    ASSERT_TRUE(runtime.requestRange(document, request, 0, 0, 1));
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
