#include <gtest/gtest.h>

#include "nemo/core/document/Graph.hpp"

using namespace nemo;

namespace {

PortRef out(NodeId node, std::uint32_t port = 0) { return PortRef{node, port}; }

} // namespace

TEST(GraphTest, AddNodeAssignsDistinctIncreasingIds) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    const NodeId b = g.addNode("output", "b");
    EXPECT_NE(a, b);
    ASSERT_NE(g.node(a), nullptr);
    EXPECT_EQ(g.node(a)->type, "testpattern");
    EXPECT_EQ(g.node(b)->name, "b");
}

TEST(GraphTest, DuplicateNodeNameRejected) {
    Graph g;
    g.addNode("testpattern", "dup");
    EXPECT_THROW(g.addNode("output", "dup"), GraphException);
}

TEST(GraphTest, ConnectRejectsUnknownNode) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    const auto problem = g.validateEdge(out(a), out(999));
    ASSERT_TRUE(problem.has_value());
    EXPECT_EQ(problem->code, GraphError::UnknownNode);
    EXPECT_THROW(g.connect(out(a), out(999)), GraphException);
}

TEST(GraphTest, ConnectRejectsDirectAndTransitiveCycles) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    const NodeId b = g.addNode("output", "b");
    g.connect(out(a), out(b));

    // Direct cycle b -> a.
    auto direct = g.validateEdge(out(b), out(a));
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(direct->code, GraphError::Cycle);
    // The message must identify the offending relationship.
    EXPECT_NE(direct->message.find(std::to_string(b)), std::string::npos);
    EXPECT_THROW(g.connect(out(b), out(a)), GraphException);

    // Transitive cycle through a third node c: a -> c -> b would close a loop.
    const NodeId c = g.addNode("testpattern", "c");
    g.connect(out(c), out(b)); // c feeds b; a -> c -> b
    auto transitive = g.validateEdge(out(b), out(c));
    ASSERT_TRUE(transitive.has_value());
    EXPECT_EQ(transitive->code, GraphError::Cycle);
}

TEST(GraphTest, InputPortOccupiedBySingleEdge) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    const NodeId b = g.addNode("testpattern", "b");
    const NodeId c = g.addNode("output", "c");
    g.connect(out(a), out(c));
    auto occupied = g.validateEdge(out(b), out(c, 0));
    ASSERT_TRUE(occupied.has_value());
    EXPECT_EQ(occupied->code, GraphError::PortOccupied);
    // A different input port is free.
    EXPECT_FALSE(g.validateEdge(out(b), out(c, 1)).has_value());
}

TEST(GraphTest, RemoveNodeDropsTouchingEdges) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    const NodeId b = g.addNode("output", "b");
    g.connect(out(a), out(b));
    g.removeNode(a);
    EXPECT_EQ(g.edges().size(), 0u);
    EXPECT_EQ(g.nodes().size(), 1u);
    EXPECT_THROW(g.removeNode(a), GraphException);
}

TEST(GraphTest, DisconnectUnknownEdgeExplains) {
    Graph g;
    EXPECT_THROW(g.disconnect(42), GraphException);
    try {
        g.disconnect(42);
    } catch (const GraphException& e) {
        EXPECT_EQ(e.errorCode(), GraphError::UnknownEdge);
    }
}

TEST(GraphTest, RemovedIdsAreNeverReused) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    g.removeNode(a);
    const NodeId fresh = g.addNode("output", "fresh");
    EXPECT_GT(fresh, a);
    EXPECT_EQ(g.node(a), nullptr);
}
