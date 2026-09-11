#include <gtest/gtest.h>

#include <limits>

#include "nemo/core/document/Serialization.hpp"

using namespace nemo;

namespace {

Network& root(Document& document) {
    return document.network(document.rootNetworkId());
}
const Network& root(const Document& document) {
    return document.network(document.rootNetworkId());
}
PortRef ref(const Document& document, const std::string& name, std::uint32_t port = 0) {
    for (const auto& node : root(document).graph().nodes())
        if (node.name == name)
            return PortRef{node.id, port};
    return PortRef{kInvalidNode, port};
}

Document sampleDocument() {
    Document document;
    document.name = "sample";
    auto& network = root(document);
    const NodeId plate = network.graph().addNode("testpattern", "plate");
    network.graph().setParam(plate, "future", "1.5");
    const NodeId comp = network.graph().addNode("merge", "comp");
    const NodeId output = network.graph().nodeByName("Output")->id;
    network.graph().renameNode(output, "out");
    network.graph().connect({plate, 0}, {comp, 0});
    network.graph().connect({comp, 0}, {output, 0});
    network.setDefaultOutput(output);
    return document;
}

}  // namespace

TEST(PersistenceTest, SaveLoadRoundTripPreservesStructure) {
    const Document original = sampleDocument();
    const nlohmann::json saved = saveDocument(original);
    const LoadResult loaded = loadDocument(saved);
    EXPECT_EQ(loaded.document.name, original.name);
    EXPECT_TRUE(loaded.warnings.empty());
    ASSERT_EQ(loaded.document.networks().size(), 1u);
    const auto& originalNetwork = root(original);
    const auto& loadedNetwork = root(loaded.document);
    ASSERT_EQ(loadedNetwork.graph().nodes().size(), originalNetwork.graph().nodes().size());
    ASSERT_EQ(loadedNetwork.graph().edges().size(), originalNetwork.graph().edges().size());
    EXPECT_EQ(loadedNetwork.defaultOutput(), originalNetwork.defaultOutput());
    for (const auto& node : originalNetwork.graph().nodes()) {
        const NodeInstance* twin = loadedNetwork.graph().nodeByName(node.name);
        ASSERT_NE(twin, nullptr) << "missing node " << node.name;
        EXPECT_EQ(twin->type, node.type);
        EXPECT_EQ(twin->params, node.params);
        EXPECT_EQ(twin->layout, node.layout);
    }
    const PortRef output = ref(loaded.document, "out");
    const auto& intoOutput = loadedNetwork.graph().edgesInto(output.node);
    ASSERT_EQ(intoOutput.size(), 1u);
    EXPECT_EQ(loadedNetwork.graph().node(intoOutput.front().from.node)->name, "comp");
}

TEST(PersistenceTest, UnknownNodeTypeIsRetainedWithWarning) {
    Document document;
    auto& network = root(document);
    const NodeId unknown = network.graph().addNode("legacyplugin", "old");
    EXPECT_NE(unknown, kInvalidNode);
    const LoadResult loaded = loadDocument(saveDocument(document));
    ASSERT_EQ(root(loaded.document).graph().nodes().size(), 2u);  // automatic Output remains
    EXPECT_NE(root(loaded.document).graph().nodeByName("old"), nullptr);
    ASSERT_EQ(loaded.warnings.size(), 1u);
    EXPECT_NE(loaded.warnings.front().find("legacyplugin"), std::string::npos);
}

TEST(PersistenceTest, NewerSchemaIsRejectedNotSilentlyMisread) {
    nlohmann::json future = saveDocument(sampleDocument());
    future["schema"] = Document::kSchemaVersion + 1;
    EXPECT_THROW(loadDocument(future), DeserializeError);
}

TEST(PersistenceTest, MalformedRootRejected) {
    EXPECT_THROW(loadDocument(nlohmann::json::array()), DeserializeError);
    EXPECT_THROW(loadDocument(nlohmann::json{{"name", "no schema"}}), DeserializeError);
}

TEST(PersistenceTest, EdgeToMissingNodeDroppedWithWarning) {
    Document document;
    auto& network = root(document);
    const NodeId plate = network.graph().addNode("testpattern", "plate");
    nlohmann::json saved = saveDocument(document);
    saved["networks"][0]["edges"].push_back(
        {{"id", 99}, {"from", {{"node", 9999}, {"port", 0}}}, {"to", {{"node", plate}, {"port", 0}}}});
    const LoadResult loaded = loadDocument(saved);
    EXPECT_EQ(root(loaded.document).graph().edges().size(), 0u);
    ASSERT_FALSE(loaded.warnings.empty());
}

TEST(PersistenceTest, ActiveCatalogRestoresContributedPortValidation) {
    NodeDescriptor descriptor;
    descriptor.type = "fixture.persisted";
    descriptor.displayName = "Persisted fixture";
    descriptor.outputs = {{PortKind::Image, "color"}};
    descriptor.capabilities.samplingScales = {1};
    descriptor.capabilities.qualityModes = {Quality::Full};
    descriptor.capabilities.channels = {"RGBA"};
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
    Document original(catalog);
    auto& network = root(original);
    const auto source = network.graph().addNode(descriptor.type, "fixture");
    const auto output = network.graph().nodeByName("Output")->id;
    network.graph().connect({source, 0}, {output, 0});
    const auto loaded = loadDocument(saveDocument(original), std::move(catalog));
    EXPECT_TRUE(loaded.warnings.empty());
    ASSERT_EQ(root(loaded.document).graph().edges().size(), 1u);
    const auto fixturePort = ref(loaded.document, "fixture", 1);
    const auto outputPort = ref(loaded.document, "Output");
    const auto rejected = root(loaded.document).graph().validateEdge(fixturePort, outputPort);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->code, GraphError::PortType);
}

TEST(PersistenceTest, SparseNodeAndEdgeIdsRoundTripExactly) {
    Document original;
    auto& network = root(original);
    network.graph().removeNode(network.graph().nodeByName("Output")->id);
    const NodeId source = network.graph().addNodeWithId(17, "testpattern", "source", {{"future", "keep-me"}});
    const NodeId output = network.graph().addNodeWithId(500, "output", "output");
    const EdgeId edge = network.graph().connectWithId(900, {source, 0}, {output, 0});
    LoadResult loaded = loadDocument(saveDocument(original));
    ASSERT_TRUE(loaded.warnings.empty());
    auto& graph = root(loaded.document).graph();
    ASSERT_EQ(graph.nodes().size(), 2u);
    ASSERT_EQ(graph.edges().size(), 1u);
    EXPECT_NE(graph.node(source), nullptr);
    EXPECT_NE(graph.node(output), nullptr);
    EXPECT_EQ(graph.node(source)->params.at("future"), "keep-me");
    EXPECT_EQ(graph.edges().front().id, edge);
    EXPECT_GT(graph.addNode("output", "new-output"), output);
    EXPECT_GT(graph.connect({source, 0}, {graph.nodeByName("new-output")->id, 0}), edge);
}

TEST(PersistenceTest, DuplicateAndInvalidPersistedIdsAreDiagnostics) {
    nlohmann::json duplicateNode{
        {"schema", Document::kSchemaVersion},
        {"networks",
         {{{"id", 1},
           {"name", "root"},
           {"nodes",
            {{{"id", 7}, {"type", "testpattern"}, {"name", "a"}}, {{"id", 7}, {"type", "output"}, {"name", "b"}}}}}}}};
    EXPECT_THROW(loadDocument(duplicateNode), DeserializeError);
    nlohmann::json invalidNode{
        {"schema", Document::kSchemaVersion},
        {"networks", {{{"id", 1}, {"nodes", {{{"id", 0}, {"type", "testpattern"}, {"name", "a"}}}}}}}};
    EXPECT_THROW(loadDocument(invalidNode), DeserializeError);
}

TEST(PersistenceTest, LegacyEntriesWithoutIdsLoadWithMigrationWarnings) {
    nlohmann::json legacy{{"schema", 1},
                          {"nodes",
                           {{{"type", "testpattern"}, {"name", "a"}, {"params", {{"future", "retained"}}}},
                            {{"type", "output"}, {"name", "b"}}}},
                          {"edges", {{{"from", {{"node", 1}, {"port", 0}}}, {"to", {{"node", 2}, {"port", 0}}}}}}};
    const LoadResult loaded = loadDocument(legacy);
    EXPECT_EQ(root(loaded.document).graph().nodes().size(), 2u);
    EXPECT_FALSE(loaded.warnings.empty());
}

TEST(PersistenceTest, DeletedIdentityHighWatermarkSurvivesRoundTrip) {
    Document original;
    auto& network = root(original);
    const NodeId removed = network.graph().addNode("testpattern", "temporary");
    network.graph().removeNode(removed);
    LoadResult loaded = loadDocument(saveDocument(original));
    EXPECT_GT(root(loaded.document).graph().addNode("output", "fresh"), removed);
}

TEST(PersistenceTest, LayoutInterfacesInstancesAndWatermarksRoundTrip) {
    NodeDescriptor mask;
    mask.type = "mask.source";
    mask.displayName = "Mask";
    mask.outputs = {{PortKind::Mask, "mask"}};
    const auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{mask});
    Document original(catalog);
    auto& rootNetwork = root(original);
    const auto rootSource = rootNetwork.graph().addNode("mask.source", "parent-mask");
    const auto child = original.addNetwork("shared");
    auto& childNetwork = original.network(child);
    childNetwork.graph().setLayout(childNetwork.graph().nodeByName("Output")->id, {11.5, -2.0});
    const auto input = childNetwork.addInput("mask", PortKind::Mask, 41);
    const auto output = childNetwork.addOutput("result", PortKind::Image, 42);
    childNetwork.setDefaultOutput(childNetwork.graph().nodeByName("Output")->id);
    const auto instance = original.addInstance(original.rootNetworkId(), child, "use-shared");
    original.bindInstanceInput(instance, input, {rootSource, 0});
    original.restoreIdentityHighWatermarks(99, 88);
    const auto loaded = loadDocument(saveDocument(original), catalog);
    ASSERT_EQ(loaded.document.networks().size(), 2u);
    EXPECT_EQ(loaded.document.nextNetworkId(), 99u);
    EXPECT_EQ(loaded.document.nextInstanceId(), 88u);
    ASSERT_EQ(loaded.document.instances().size(), 1u);
    EXPECT_EQ(loaded.document.instances().front().inputBindings.at(input).port, 0u);
    EXPECT_EQ(loaded.document.network(child).inputs().front().kind, PortKind::Mask);
    EXPECT_EQ(loaded.document.network(child).outputs().front().id, output);
    EXPECT_EQ(loaded.document.network(child).graph().nodeByName("Output")->layout, LayoutPosition(11.5, -2.0));
}
TEST(PersistenceTest, SignedPortOverflowIsRejected) {
    auto malformed = saveDocument(sampleDocument());
    malformed["networks"][0]["edges"][0]["from"]["port"] = std::numeric_limits<std::int64_t>::max();
    EXPECT_THROW(loadDocument(malformed), DeserializeError);
}

TEST(PersistenceTest, SelectedNonInitialRootRoundTripsWithoutGhostNetworks) {
    Document original;
    const auto initial = original.rootNetworkId();
    const auto selected = original.addNetwork("delivery");
    original.setRootNetworkId(selected);
    original.removeNetwork(initial);
    const auto loaded = loadDocument(saveDocument(original));
    EXPECT_EQ(loaded.document.rootNetworkId(), selected);
    ASSERT_EQ(loaded.document.networks().size(), 1u);
    EXPECT_EQ(loaded.document.network(selected).name(), "delivery");
}

TEST(PersistenceTest, IncompleteNetworkWithoutOutputRemainsSaveable) {
    Document original;
    auto& network = root(original);
    network.graph().removeNode(network.defaultOutput());
    const auto source = network.graph().addNode("testpattern", "source");
    const auto loaded = loadDocument(saveDocument(original));
    EXPECT_EQ(root(loaded.document).defaultOutput(), kInvalidNode);
    EXPECT_NE(root(loaded.document).graph().node(source), nullptr);
}

TEST(PersistenceTest, MissingInstanceRecordIsRejectedWithNodeRelationship) {
    Document original;
    const auto definition = original.addNetwork("shared");
    (void)original.addInstance(original.rootNetworkId(), definition, "use");
    auto malformed = saveDocument(original);
    malformed["instances"] = nlohmann::json::array();
    EXPECT_THROW(loadDocument(malformed), DeserializeError);
}
