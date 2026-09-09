#include "ViewerController.hpp"

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

}  // namespace
