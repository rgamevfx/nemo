#include <gtest/gtest.h>

#include "nemo/core/document/Document.hpp"

using namespace nemo;

TEST(CommandStackTest, PushAppliesImmediately) {
    Document doc;
    CommandStack stack(doc);
    doc.graph.addNode("testpattern", "plate");
    stack.push(setParamCommand("plate", "gain", "2.0"));
    ASSERT_NE(doc.graph.node(1), nullptr);
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "2.0");
    EXPECT_EQ(stack.depth(), 1u);
}

TEST(CommandStackTest, UndoRestoresAndRedoReapplies) {
    Document doc;
    CommandStack stack(doc);
    doc.graph.addNode("testpattern", "plate");

    stack.push(setParamCommand("plate", "gain", "2.0"));
    stack.push(setParamCommand("plate", "gain", "4.0"));
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "4.0");

    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "2.0");
    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(doc.graph.node(1)->params.count("gain"), 0u);
    EXPECT_FALSE(stack.undo());

    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "2.0");
    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "4.0");
    EXPECT_FALSE(stack.redo());
}

TEST(CommandStackTest, NewCommandDropsRedoBranch) {
    Document doc;
    CommandStack stack(doc);
    doc.graph.addNode("testpattern", "plate");
    stack.push(setParamCommand("plate", "gain", "2.0"));
    ASSERT_TRUE(stack.undo());

    stack.push(setParamCommand("plate", "gain", "8.0"));
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "8.0");
}

TEST(CommandStackTest, FailingApplyLeavesDocumentUntouched) {
    Document doc;
    CommandStack stack(doc);
    doc.graph.addNode("testpattern", "plate");
    const std::size_t nodeCountBefore = doc.graph.nodes().size();
    EXPECT_THROW(stack.push(setParamCommand("missing", "gain", "1.0")), std::runtime_error);
    EXPECT_EQ(doc.graph.nodes().size(), nodeCountBefore);
    EXPECT_EQ(stack.depth(), 0u);
}

TEST(CommandStackTest, HistoryBoundedByCapacity) {
    Document doc;
    CommandStack stack(doc, /*capacity=*/2);
    doc.graph.addNode("testpattern", "plate");
    stack.push(setParamCommand("plate", "gain", "1.0"));
    stack.push(setParamCommand("plate", "gain", "2.0"));
    stack.push(setParamCommand("plate", "gain", "3.0"));
    EXPECT_EQ(stack.depth(), 2u);
    // Oldest command fell off: only two undos possible.
    EXPECT_TRUE(stack.undo());
    EXPECT_TRUE(stack.undo());
    EXPECT_FALSE(stack.undo());
    EXPECT_EQ(doc.graph.node(1)->params.at("gain"), "1.0");
}
