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

TEST(GraphTest, ReachabilityFollowsFanOutAndReconvergentPaths) {
    Graph g;
    const NodeId root = g.addNode("testpattern", "root");
    const NodeId left = g.addNode("merge", "left");
    const NodeId right = g.addNode("merge", "right");
    const NodeId join = g.addNode("merge", "join");
    const NodeId delivery = g.addNode("output", "delivery");
    g.connect(out(root), out(left, 0));
    g.connect(out(root), out(right, 0));
    g.connect(out(left), out(join, 0));
    g.connect(out(right), out(join, 1));
    g.connect(out(join), out(delivery, 0));

    // Fan-out: both branches leave the same source output.
    EXPECT_TRUE(g.reachable(root, left));
    EXPECT_TRUE(g.reachable(root, right));
    // Reconvergence: two distinct paths reach join and then delivery.
    EXPECT_TRUE(g.reachable(left, join));
    EXPECT_TRUE(g.reachable(right, join));
    EXPECT_TRUE(g.reachable(root, join));
    EXPECT_TRUE(g.reachable(root, delivery));
    EXPECT_TRUE(g.reachable(left, delivery));
    EXPECT_TRUE(g.reachable(right, delivery));
    // Origin equal to target stays reachable.
    EXPECT_TRUE(g.reachable(join, join));
    // Directed connectivity is not reversed by a converging merge.
    EXPECT_FALSE(g.reachable(delivery, join));
    EXPECT_FALSE(g.reachable(join, left));
    EXPECT_FALSE(g.reachable(left, root));
}

TEST(GraphTest, ReconvergentPathsDecideCycleCandidates) {
    Graph g;
    const NodeId first = g.addNode("merge", "first");
    const NodeId second = g.addNode("merge", "second");
    const NodeId third = g.addNode("merge", "third");
    const NodeId sink = g.addNode("merge", "sink");
    g.connect(out(first), out(second, 0));
    g.connect(out(second), out(third, 0));
    g.connect(out(third), out(sink, 0));
    // A second, shorter route reconverges first -> sink.
    g.connect(out(first), out(sink, 1));
    EXPECT_TRUE(g.reachable(second, sink));
    EXPECT_TRUE(g.reachable(first, sink));

    // Direct candidate: third already feeds sink, so sink -> third closes a loop.
    const auto direct = g.validateEdge(out(sink), out(third, 1));
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(direct->code, GraphError::Cycle);
    EXPECT_NE(direct->message.find(std::to_string(third)), std::string::npos);

    // Transitive candidate: second reaches sink through third.
    const auto transitive = g.validateEdge(out(sink), out(second, 1));
    ASSERT_TRUE(transitive.has_value());
    EXPECT_EQ(transitive->code, GraphError::Cycle);
    EXPECT_NE(transitive->message.find(std::to_string(second)), std::string::npos);

    // The reconvergent route blocks sink -> first as well.
    const auto reconvergent = g.validateEdge(out(sink), out(first, 0));
    ASSERT_TRUE(reconvergent.has_value());
    EXPECT_EQ(reconvergent->code, GraphError::Cycle);

    // Unrelated free inputs stay connectable: no false cycle.
    const NodeId island = g.addNode("merge", "island");
    EXPECT_FALSE(g.validateEdge(out(sink), out(island, 0)).has_value());
    EXPECT_FALSE(g.validateEdge(out(island), out(second, 1)).has_value());
}

TEST(GraphTest, ReachabilitySeparatesComponentsAndUnknownIdentities) {
    Graph g;
    const NodeId leftSource = g.addNode("testpattern", "left-source");
    const NodeId leftSink = g.addNode("output", "left-sink");
    const NodeId rightSource = g.addNode("testpattern", "right-source");
    const NodeId rightSink = g.addNode("output", "right-sink");
    g.connect(out(leftSource), out(leftSink));
    g.connect(out(rightSource), out(rightSink));

    EXPECT_TRUE(g.reachable(leftSource, leftSink));
    EXPECT_TRUE(g.reachable(rightSource, rightSink));
    EXPECT_FALSE(g.reachable(leftSource, rightSink));
    EXPECT_FALSE(g.reachable(rightSource, leftSink));

    // Self-reachability holds even for an identity this graph never saw.
    constexpr NodeId unknown = 4242;
    EXPECT_TRUE(g.reachable(unknown, unknown));
    EXPECT_TRUE(g.reachable(kInvalidNode, kInvalidNode));
    EXPECT_FALSE(g.reachable(unknown, leftSink));
    EXPECT_FALSE(g.reachable(leftSource, unknown));

    const auto unknownTarget = g.validateEdge(out(leftSource), out(unknown, 0));
    ASSERT_TRUE(unknownTarget.has_value());
    EXPECT_EQ(unknownTarget->code, GraphError::UnknownNode);
}

TEST(GraphTest, SparseDeletionAndRestorationPreserveReachabilityAndWatermarks) {
    // Identities far above the discovery count defend that traversal state is
    // sized by visited nodes, not by an identity high-water mark.
    constexpr NodeId base = 1ULL << 40;
    Graph g;
    const NodeId source = g.addNodeWithId(base, "testpattern", "source");
    const NodeId middle = g.addNodeWithId(base + 5, "merge", "middle");
    const NodeId delivery = g.addNodeWithId(base + 9, "output", "delivery");
    const EdgeId upstream = g.connect(out(source), out(middle, 0));
    const EdgeId downstream = g.connect(out(middle), out(delivery, 0));
    ASSERT_TRUE(g.reachable(source, delivery));

    g.removeNode(middle);
    // Removal invalidates the cached incoming adjacency of the survivors.
    EXPECT_FALSE(g.reachable(source, delivery));
    EXPECT_TRUE(g.edgesInto(delivery).empty());
    EXPECT_TRUE(g.edgesInto(middle).empty());

    // Restoration keeps the exact identities and monotonic watermarks.
    EXPECT_EQ(g.addNodeWithId(middle, "merge", "middle"), middle);
    EXPECT_EQ(g.restoreEdgeWithId(upstream, out(source), out(middle, 0)), upstream);
    EXPECT_EQ(g.restoreEdgeWithId(downstream, out(middle), out(delivery, 0)), downstream);
    EXPECT_TRUE(g.reachable(source, delivery));
    EXPECT_TRUE(g.reachable(source, middle));
    EXPECT_TRUE(g.reachable(middle, delivery));
    EXPECT_GT(g.nextNodeId(), middle);
    EXPECT_GT(g.nextEdgeId(), downstream);

    // Fresh identities are allocated above the preserved high-water marks.
    EXPECT_GT(g.addNode("testpattern", "fresh"), middle);
}

TEST(GraphTest, RestoreEdgeWithIdRejectsCyclesWithoutAdvancingIdentity) {
    Graph g;
    const NodeId first = g.addNode("merge", "first");
    const NodeId second = g.addNode("merge", "second");
    const NodeId third = g.addNode("merge", "third");
    g.connect(out(first), out(second, 0));
    g.connect(out(second), out(third, 0));
    const auto edgeCount = g.edges().size();
    const auto watermark = g.nextEdgeId();

    // Direct restored cycle: second already receives from first.
    try {
        g.restoreEdgeWithId(1000, out(second), out(first, 0));
        FAIL() << "expected a direct cycle rejection from restoration";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::Cycle);
        EXPECT_NE(std::string(error.what()).find(std::to_string(first)), std::string::npos);
    }
    // Transitive restored cycle: third depends on first through second.
    EXPECT_THROW(g.restoreEdgeWithId(1001, out(third), out(first, 0)), GraphException);

    // A rejected restoration publishes nothing and never moves the watermark.
    EXPECT_EQ(g.edges().size(), edgeCount);
    EXPECT_EQ(g.nextEdgeId(), watermark);
    EXPECT_TRUE(g.edgesInto(first).empty());
    EXPECT_TRUE(g.reachable(first, third));
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
TEST(GraphTest, MaskInputsAcceptImageOrMaskButMaskCannotFeedImage) {
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

    // Mask outputs satisfy mask inputs.
    EXPECT_FALSE(g.validateEdge(PortRef{maskSource, 0}, PortRef{maskSink, 0}).has_value());
    // An Image output is also a valid mask source: any stored channel can be
    // addressed as coverage, so this direction is accepted.
    EXPECT_FALSE(g.validateEdge(PortRef{imageSource, 0}, PortRef{maskSink, 0}).has_value());

    // The reverse is not symmetric: a Mask output can never stand in for an
    // Image input.
    const NodeId imageSink = g.addNode("output", "image-sink");
    const auto incompatible = g.validateEdge(PortRef{maskSource, 0}, PortRef{imageSink, 0});
    ASSERT_TRUE(incompatible.has_value());
    EXPECT_EQ(incompatible->code, GraphError::PortType);
}

TEST(GraphTest, ViewerAcceptsOneImageInputAndProvidesNoOutput) {
    Graph g;
    const NodeId plate = g.addNode("testpattern", "plate");
    const NodeId viewer = g.addNode("viewer", "Viewer1");
    ASSERT_NE(g.node(viewer), nullptr);
    ASSERT_EQ(g.inputPorts(viewer).size(), 1u);
    EXPECT_EQ(g.inputPorts(viewer).front().kind, PortKind::Image);
    EXPECT_EQ(g.inputPorts(viewer).front().name, "color");
    EXPECT_TRUE(g.outputPorts(viewer).empty());
    const EdgeId edge = g.connect(PortRef{plate, 0}, PortRef{viewer, 0});
    ASSERT_EQ(g.edgesInto(viewer).size(), 1u);
    EXPECT_EQ(g.edgesInto(viewer).front().id, edge);

    // A display sink has no output to fan out to a processing node.
    const auto rejected = g.validateEdge(PortRef{viewer, 0}, PortRef{plate, 0});
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->code, GraphError::PortType);
}
