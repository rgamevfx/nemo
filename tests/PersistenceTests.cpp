#include <gtest/gtest.h>

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
    doc.graph.node(plate)->params["gain"] = "1.5";
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

    // Structure comparison goes through names/params, not raw ids: ids are
    // remapped on load by design.
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
