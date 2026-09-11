#include <gtest/gtest.h>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

using namespace nemo;

namespace {

PortRef out(NodeId node, std::uint32_t port = 0) {
    return PortRef{node, port};
}

}  // namespace

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
    const NodeId a = g.addNode("merge", "a");
    const NodeId b = g.addNode("merge", "b");
    g.connect(out(a), out(b, 0));

    // Direct cycle b -> a through valid merge image ports.
    auto direct = g.validateEdge(out(b), out(a, 0));
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(direct->code, GraphError::Cycle);
    EXPECT_NE(direct->message.find(std::to_string(b)), std::string::npos);

    // Transitive cycle through a third merge node.
    const NodeId c = g.addNode("merge", "c");
    g.connect(out(b), out(c, 0));
    auto transitive = g.validateEdge(out(c), out(a, 0));
    ASSERT_TRUE(transitive.has_value());
    EXPECT_EQ(transitive->code, GraphError::Cycle);

    // A non-cyclic connection to another free input remains legal.
    EXPECT_FALSE(g.validateEdge(out(a), out(c, 1)).has_value());
}

TEST(GraphTest, InputPortOccupiedBySingleEdge) {
    Graph g;
    const NodeId a = g.addNode("testpattern", "a");
    const NodeId b = g.addNode("testpattern", "b");
    const NodeId c = g.addNode("merge", "c");
    g.connect(out(a), out(c, 0));
    auto occupied = g.validateEdge(out(b), out(c, 0));
    ASSERT_TRUE(occupied.has_value());
    EXPECT_EQ(occupied->code, GraphError::PortOccupied);
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
TEST(GraphTest, LayoutAndRoutesArePersistentGraphData) {
    Graph g;
    const NodeId source = g.addNode("testpattern", "source");
    const NodeId output = g.addNode("output", "output");
    const EdgeId edge = g.connect(out(source), out(output));
    g.setLayout(source, LayoutPosition{12.5, -3.0});
    g.setRoute(edge, {LayoutPosition{1.0, 2.0}, LayoutPosition{3.0, 4.0}});

    ASSERT_NE(g.node(source), nullptr);
    EXPECT_EQ(g.node(source)->layout, (LayoutPosition{12.5, -3.0}));
    ASSERT_EQ(g.edges().size(), 1u);
    EXPECT_EQ(g.edges().front().route, (std::vector<LayoutPosition>{{1.0, 2.0}, {3.0, 4.0}}));
}

TEST(GraphTest, UnknownNodeHasNoFabricatedPortContract) {
    Graph g;
    const NodeId unknown = g.addNode("missing.effect", "unknown");
    EXPECT_TRUE(g.inputPorts(unknown).empty());
    EXPECT_TRUE(g.outputPorts(unknown).empty());
    const NodeId output = g.addNode("output", "output");
    const auto problem = g.validateEdge(out(unknown), out(output));
    ASSERT_TRUE(problem.has_value());
    EXPECT_EQ(problem->code, GraphError::PortType);
    EXPECT_NE(problem->message.find("no port contract"), std::string::npos);
}

TEST(GraphTest, FormalInputReservationRejectsBypassAndCanBeReleased) {
    Document document;
    Network& network = document.network(document.rootNetworkId());
    const InterfacePortId input = network.addInput("plate", PortKind::Image);
    const NodeId merge = network.graph().addNode("merge", "merge");
    const NodeId source = network.graph().addNode("testpattern", "source");

    network.connectInput(input, PortRef{merge, 0});
    const auto bypass = network.graph().validateEdge(PortRef{source, 0}, PortRef{merge, 0});
    ASSERT_TRUE(bypass.has_value());
    EXPECT_EQ(bypass->code, GraphError::PortOccupied);
    network.disconnectInput(input, PortRef{merge, 0});
    EXPECT_FALSE(network.graph().validateEdge(PortRef{source, 0}, PortRef{merge, 0}).has_value());
}
TEST(GraphTest, ImageAndMaskPortsRejectIncompatibleConnections) {
    NodeDescriptor descriptor{
        .type = "mask.fixture",
        .displayName = "Mask Fixture",
        .group = "Tests",
        .inputs = {{PortKind::Mask, "mask"}},
        .outputs = {{PortKind::Mask, "mask"}},
        .capabilities = NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"RGBA"}}};
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
    Graph g(catalog);
    const NodeId maskSource = g.addNode("mask.fixture", "mask-source");
    const NodeId maskSink = g.addNode("mask.fixture", "mask-sink");
    const NodeId imageSource = g.addNode("testpattern", "image-source");
    EXPECT_FALSE(g.validateEdge(PortRef{maskSource, 0}, PortRef{maskSink, 0}).has_value());
    const auto incompatible = g.validateEdge(PortRef{imageSource, 0}, PortRef{maskSink, 0});
    ASSERT_TRUE(incompatible.has_value());
    EXPECT_EQ(incompatible->code, GraphError::PortType);
    EXPECT_NE(incompatible->message.find("does not match"), std::string::npos);
}
