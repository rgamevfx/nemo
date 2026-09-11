#include <gtest/gtest.h>

#include <limits>

#include "nemo/core/document/Serialization.hpp"

using namespace nemo;

namespace {

PortRef ref(const Document& doc, const std::string& name, std::uint32_t port = 0) {
    for (const auto& node : doc.graph.nodes()) {
        if (node.name == name) {
            return PortRef{node.id, port};
        }
    }
    return PortRef{kInvalidNode, port};
}

Document sampleDocument() {
    Document doc;
    doc.name = "sample";
    const NodeId plate = doc.graph.addNode("testpattern", "plate");
    doc.graph.setParam(plate, "gain", "1.5");
    const NodeId comp = doc.graph.addNode("merge", "comp");
    const NodeId out = doc.graph.addNode("output", "out");
    doc.graph.connect(PortRef{plate, 0}, PortRef{comp, 0});
    doc.graph.connect(PortRef{comp, 0}, PortRef{out, 0});
    return doc;
}

}  // namespace

TEST(PersistenceTest, SaveLoadRoundTripPreservesStructure) {
    const Document original = sampleDocument();
    const nlohmann::json saved = saveDocument(original);
    const LoadResult loaded = loadDocument(saved);

    EXPECT_EQ(loaded.document.name, original.name);
    EXPECT_TRUE(loaded.warnings.empty());
    ASSERT_EQ(loaded.document.graph.nodes().size(), original.graph.nodes().size());
    ASSERT_EQ(loaded.document.graph.edges().size(), original.graph.edges().size());

    // Persistence preserves the authored identities, including sparse IDs.
    for (const auto& node : original.graph.nodes()) {
        const Node* twin = nullptr;
        for (const auto& candidate : loaded.document.graph.nodes()) {
            if (candidate.name == node.name) {
                twin = &candidate;
                break;
            }
        }
        ASSERT_NE(twin, nullptr) << "missing node " << node.name;
        EXPECT_EQ(twin->type, node.type);
        EXPECT_EQ(twin->params, node.params);
    }
    // Connectivity: plate -> comp -> out preserved.
    const PortRef outLoaded = ref(loaded.document, "out");
    const auto& intoOut = loaded.document.graph.edgesInto(outLoaded.node);
    ASSERT_EQ(intoOut.size(), 1u);
    EXPECT_EQ(loaded.document.graph.node(intoOut.front().from.node)->name, "comp");
}

TEST(PersistenceTest, UnknownNodeTypeIsRetainedWithWarning) {
    Document doc;
    const NodeId legacy = doc.graph.addNode("legacyplugin", "old");
    EXPECT_NE(legacy, kInvalidNode);
    const LoadResult loaded = loadDocument(saveDocument(doc));
    ASSERT_EQ(loaded.document.graph.nodes().size(), 1u);
    EXPECT_EQ(loaded.document.graph.nodes().front().type, "legacyplugin");
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
    Document doc;
    const NodeId plate = doc.graph.addNode("testpattern", "plate");
    EXPECT_NE(plate, kInvalidNode);
    nlohmann::json saved = saveDocument(doc);
    saved["edges"].push_back({{"id", 99}, {"from", {{"node", 9999}, {"port", 0}}}, {"to", {{"node", 1}, {"port", 0}}}});
    const LoadResult loaded = loadDocument(saved);
    EXPECT_EQ(loaded.document.graph.edges().size(), 0u);
    ASSERT_EQ(loaded.warnings.size(), 1u);
}

TEST(PersistenceTest, ActiveCatalogRestoresContributedPortValidation) {
    NodeDescriptor descriptor;
    descriptor.type = "fixture.persisted";
    descriptor.displayName = "Persisted fixture";
    descriptor.outputs = {{PortKind::Color, "color"}};
    descriptor.capabilities.samplingScales = {1};
    descriptor.capabilities.qualityModes = {Quality::Full};
    descriptor.capabilities.channels = {"RGBA"};
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
    Document original(catalog);
    const auto source = original.graph.addNode(descriptor.type, "fixture");
    const auto output = original.graph.addNode("output", "out");
    original.graph.connect({source, 0}, {output, 0});

    const auto loaded = loadDocument(saveDocument(original), std::move(catalog));
    EXPECT_TRUE(loaded.warnings.empty());
    ASSERT_EQ(loaded.document.graph.edges().size(), 1u);
    const auto fixturePort = ref(loaded.document, "fixture", 1);
    const auto outputPort = ref(loaded.document, "out");
    const auto rejected = loaded.document.graph.validateEdge(fixturePort, outputPort);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->code, GraphError::PortType);
}

TEST(PersistenceTest, SparseNodeAndEdgeIdsRoundTripExactly) {
    Document original;
    const NodeId source = original.graph.addNodeWithId(17, "testpattern", "source", {{"future", "keep-me"}});
    const NodeId output = original.graph.addNodeWithId(500, "output", "output");
    const EdgeId edge = original.graph.connectWithId(900, {source, 0}, {output, 0});

    LoadResult loaded = loadDocument(saveDocument(original));
    ASSERT_TRUE(loaded.warnings.empty());
    ASSERT_EQ(loaded.document.graph.nodes().size(), 2u);
    ASSERT_EQ(loaded.document.graph.edges().size(), 1u);
    ASSERT_NE(loaded.document.graph.node(source), nullptr);
    ASSERT_NE(loaded.document.graph.node(output), nullptr);
    EXPECT_EQ(loaded.document.graph.node(source)->params.at("future"), "keep-me");
    EXPECT_EQ(loaded.document.graph.edges().front().id, edge);
    EXPECT_EQ(loaded.document.graph.edges().front().from.node, source);
    EXPECT_EQ(loaded.document.graph.edges().front().to.node, output);

    const NodeId newOutput = loaded.document.graph.addNode("output", "new-output");
    EXPECT_GT(newOutput, output);
    EXPECT_GT(loaded.document.graph.connect({source, 0}, {newOutput, 0}), edge);
}

TEST(PersistenceTest, DuplicateAndInvalidPersistedIdsAreDiagnostics) {
    nlohmann::json duplicateNode;
    duplicateNode["schema"] = Document::kSchemaVersion;
    duplicateNode["nodes"] = {{{"id", 7}, {"type", "testpattern"}, {"name", "a"}},
                              {{"id", 7}, {"type", "output"}, {"name", "b"}}};
    EXPECT_THROW(loadDocument(duplicateNode), DeserializeError);

    nlohmann::json invalidNode;
    invalidNode["schema"] = Document::kSchemaVersion;
    invalidNode["nodes"] = {{{"id", 0}, {"type", "testpattern"}, {"name", "a"}}};
    EXPECT_THROW(loadDocument(invalidNode), DeserializeError);

    nlohmann::json duplicateEdge;
    duplicateEdge["schema"] = Document::kSchemaVersion;
    duplicateEdge["nodes"] = {{{"id", 1}, {"type", "testpattern"}, {"name", "a"}},
                              {{"id", 2}, {"type", "output"}, {"name", "b"}}};
    duplicateEdge["edges"] = {{{"id", 8}, {"from", {{"node", 1}, {"port", 0}}}, {"to", {{"node", 2}, {"port", 0}}}},
                              {{"id", 8}, {"from", {{"node", 1}, {"port", 0}}}, {"to", {{"node", 2}, {"port", 1}}}}};
    EXPECT_THROW(loadDocument(duplicateEdge), DeserializeError);
}

TEST(PersistenceTest, LegacyEntriesWithoutIdsLoadWithMigrationWarnings) {
    nlohmann::json legacy;
    legacy["schema"] = Document::kSchemaVersion;
    legacy["nodes"] = {{{"type", "testpattern"}, {"name", "a"}, {"params", {{"future", "retained"}}}},
                       {{"type", "output"}, {"name", "b"}}};
    legacy["edges"] = {{{"from", {{"node", 1}, {"port", 0}}}, {"to", {{"node", 2}, {"port", 0}}}}};
    const LoadResult loaded = loadDocument(legacy);
    ASSERT_EQ(loaded.document.graph.nodes().size(), 2u);
    EXPECT_EQ(loaded.document.graph.node(1)->params.at("future"), "retained");
    ASSERT_EQ(loaded.document.graph.edges().size(), 1u);
    EXPECT_GE(loaded.warnings.size(), 2u);
}

TEST(PersistenceTest, LegacyGeneratedIdsAvoidDeclaredIds) {
    nlohmann::json legacy;
    legacy["schema"] = Document::kSchemaVersion;
    legacy["nodes"] = {{{"type", "testpattern"}, {"name", "legacy"}},
                       {{"id", 1}, {"type", "output"}, {"name", "declared"}}};

    const LoadResult loaded = loadDocument(legacy);
    EXPECT_EQ(loaded.document.graph.nodeByName("legacy")->id, 2u);
    EXPECT_EQ(loaded.document.graph.nodeByName("declared")->id, 1u);
}

TEST(PersistenceTest, DeletedIdentityHighWatermarkSurvivesRoundTrip) {
    Document original;
    const NodeId removed = original.graph.addNode("testpattern", "temporary");
    original.graph.removeNode(removed);

    LoadResult loaded = loadDocument(saveDocument(original));
    EXPECT_EQ(loaded.document.graph.nodes().size(), 0u);
    EXPECT_GT(loaded.document.graph.addNode("output", "fresh"), removed);
}

TEST(PersistenceTest, IdsAboveSignedRangeRoundTripExactly) {
    const NodeId high = std::numeric_limits<NodeId>::max() - 1;
    Document original;
    original.graph.addNodeWithId(high, "testpattern", "high");
    original.graph.addNodeWithId(1, "output", "out");
    const EdgeId highEdge = high - 1;
    original.graph.connectWithId(highEdge, {high, 0}, {1, 0});

    const LoadResult loaded = loadDocument(saveDocument(original));
    EXPECT_NE(loaded.document.graph.node(high), nullptr);
    EXPECT_EQ(loaded.document.graph.edges().front().id, highEdge);
}

TEST(PersistenceTest, SignedPortOverflowIsRejected) {
    nlohmann::json malformed;
    malformed["schema"] = Document::kSchemaVersion;
    malformed["nodes"] = {{{"id", 1}, {"type", "testpattern"}, {"name", "a"}},
                          {{"id", 2}, {"type", "output"}, {"name", "b"}}};
    malformed["edges"] = {{{"id", 3},
                           {"from", {{"node", 1}, {"port", std::numeric_limits<std::int64_t>::max()}}},
                           {"to", {{"node", 2}, {"port", 0}}}}};
    EXPECT_THROW(loadDocument(malformed), DeserializeError);
}
