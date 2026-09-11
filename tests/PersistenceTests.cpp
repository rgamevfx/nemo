#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <variant>

#include "nemo/core/commands/AnimationCommands.hpp"
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
    network.graph().setParam(plate, "future", ParameterValue{std::string{"1.5"}});
    const NodeId comp = network.graph().addNode("merge", "comp");
    const NodeId output = network.graph().nodeByName("Output")->id;
    network.graph().renameNode(output, "out");
    network.graph().connect({plate, 0}, {comp, 0});
    network.graph().connect({comp, 0}, {output, 0});
    network.setDefaultOutput(output);
    return document;
}

Document animatedDocument() {
    Document document;
    const auto node = root(document).graph().addNode("constcolor", "animated");
    const ParameterAddress address{document.rootNetworkId(), node, "color"};
    Keyframe first{0, 0.25, ColorValue{{-2.F, 4.F, 8.F, 1.F}}};
    first.interpolation = KeyInterpolation::Bezier;
    first.tangentMode = TangentMode::Broken;
    first.inSlope = {-0.25, 0.5, 0, 0};
    first.outSlope = {0.75, -0.5, 0, 0};
    Keyframe last{0, 10.75, ColorValue{{6.F, -4.F, 2.F, 1.F}}};
    last.interpolation = KeyInterpolation::Hold;
    setKeyframesCommand({{address, first}, {address, last}}).apply(document);
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

TEST(PersistenceTest, TypedParametersUseTaggedSchemaAndRoundTrip) {
    Document original;
    auto& network = root(original);
    const NodeId node = network.graph().addNode("constcolor", "grade");
    const ColorValue color{std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F}};
    network.graph().setParam(node, "color", ParameterValue{color});

    const nlohmann::json saved = saveDocument(original);
    EXPECT_EQ(saved.at("schema"), Document::kSchemaVersion);
    const auto& nodes = saved.at("networks").at(0).at("nodes");
    const auto gradeIt = std::find_if(nodes.begin(), nodes.end(),
                                      [](const auto& entry) { return entry.value("name", std::string{}) == "grade"; });
    ASSERT_NE(gradeIt, nodes.end());
    EXPECT_EQ(gradeIt->at("params").at("color").at("type"), "color");
    EXPECT_EQ(gradeIt->at("params").at("color").at("value"), nlohmann::json::array({0.1F, 0.2F, 0.3F, 1.0F}));

    const LoadResult loaded = loadDocument(saved);
    ASSERT_TRUE(loaded.warnings.empty());
    const auto* restored = root(loaded.document).graph().nodeByName("grade");
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(std::holds_alternative<ColorValue>(restored->params.at("color")));
    EXPECT_EQ(std::get<ColorValue>(restored->params.at("color")), color);
}

TEST(PersistenceTest, TypedInstanceOverridesRoundTrip) {
    Document original;
    const NetworkId definition = original.addNetwork("shared");
    auto& shared = original.network(definition);
    const NodeId target = shared.graph().addNode("constcolor", "grade");
    const NetworkInstanceId instance = original.addInstance(original.rootNetworkId(), definition, "use-shared");
    const ColorValue color{std::array<float, 4>{0.4F, 0.5F, 0.6F, 1.0F}};
    original.setInstanceParam(instance, target, "color", ParameterValue{color});

    const LoadResult loaded = loadDocument(saveDocument(original));
    const auto* restored = loaded.document.instance(instance);
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(std::holds_alternative<ColorValue>(restored->params.at(target).at("color")));
    EXPECT_EQ(std::get<ColorValue>(restored->params.at(target).at("color")), color);
}

TEST(PersistenceTest, LegacyKnownParameterTextMigratesThroughCatalog) {
    nlohmann::json legacy = saveDocument(sampleDocument());
    legacy["schema"] = 2;
    legacy.erase("animationChannels");
    legacy.erase("nextAnimationChannelId");
    legacy.erase("nextKeyframeId");
    auto& nodes = legacy["networks"].at(0)["nodes"];
    auto node = std::find_if(nodes.begin(), nodes.end(),
                             [](const auto& entry) { return entry.value("name", std::string{}) == "plate"; });
    ASSERT_NE(node, nodes.end());
    node->at("type") = "constcolor";
    node->at("params") = {{"color", "0.1 0.2 0.3 1"}};

    const LoadResult loaded = loadDocument(legacy);
    const auto* restored = root(loaded.document).graph().nodeByName("plate");
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(std::holds_alternative<ColorValue>(restored->params.at("color")));
    EXPECT_EQ(std::get<ColorValue>(restored->params.at("color")),
              (ColorValue{std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F}}));
}

TEST(PersistenceTest, ParameterMigrationFailuresIdentifyLocation) {
    nlohmann::json legacy = saveDocument(sampleDocument());
    legacy["schema"] = 2;
    legacy.erase("animationChannels");
    legacy.erase("nextAnimationChannelId");
    legacy.erase("nextKeyframeId");
    auto& legacyNodes = legacy["networks"].at(0)["nodes"];
    auto legacyNode = std::find_if(legacyNodes.begin(), legacyNodes.end(),
                                   [](const auto& entry) { return entry.value("name", std::string{}) == "plate"; });
    ASSERT_NE(legacyNode, legacyNodes.end());
    legacyNode->at("type") = "constcolor";
    legacyNode->at("params") = {{"color", "not-a-color"}};

    try {
        (void)loadDocument(legacy);
        FAIL() << "invalid legacy parameter should be rejected";
    } catch (const DeserializeError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("network"), std::string::npos);
        EXPECT_NE(message.find("node"), std::string::npos);
        EXPECT_NE(message.find("key 'color'"), std::string::npos);
    }

    nlohmann::json malformed = saveDocument(sampleDocument());
    auto& malformedNodes = malformed["networks"].at(0)["nodes"];
    auto malformedNode = std::find_if(malformedNodes.begin(), malformedNodes.end(),
                                      [](const auto& entry) { return entry.value("name", std::string{}) == "plate"; });
    ASSERT_NE(malformedNode, malformedNodes.end());
    malformedNode->at("type") = "constcolor";
    malformedNode->at("params") = {{"color", {{"type", "color"}, {"value", 7}}}};
    EXPECT_THROW(loadDocument(malformed), DeserializeError);
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
    const NodeId source = network.graph().addNodeWithId(17, "testpattern", "source",
                                                        {{"future", ParameterValue{std::string{"keep-me"}}}});
    const NodeId output = network.graph().addNodeWithId(500, "output", "output");
    const EdgeId edge = network.graph().connectWithId(900, {source, 0}, {output, 0});
    LoadResult loaded = loadDocument(saveDocument(original));
    ASSERT_TRUE(loaded.warnings.empty());
    auto& graph = root(loaded.document).graph();
    ASSERT_EQ(graph.nodes().size(), 2u);
    ASSERT_EQ(graph.edges().size(), 1u);
    EXPECT_NE(graph.node(source), nullptr);
    EXPECT_NE(graph.node(output), nullptr);
    ASSERT_TRUE(std::holds_alternative<std::string>(graph.node(source)->params.at("future")));
    EXPECT_EQ(std::get<std::string>(graph.node(source)->params.at("future")), "keep-me");
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

TEST(PersistenceTest, AnimationRoundTripPreservesCurvesAndScopedInstanceOverrides) {
    auto original = animatedDocument();
    const auto definition = original.addNetwork("shared-animation");
    const auto node = original.network(definition).graph().addNode("constcolor", "color");
    const auto instance = original.addInstance(original.rootNetworkId(), definition, "occurrence");
    const ParameterAddress occurrence{definition, node, "color", instance};
    setKeyframesCommand({{occurrence, Keyframe{0, 3.5, ColorValue{{0.25F, 0.5F, 0.75F, 1.F}}}}}).apply(original);
    const auto encoded = saveDocument(original);
    const auto loaded = loadDocument(encoded);
    EXPECT_EQ(loaded.document.animationChannels(), original.animationChannels());
    EXPECT_EQ(saveDocument(loaded.document), encoded);
    const auto address = original.animationChannels().front().address;
    EXPECT_EQ(animatedParameterValue(loaded.document, address, 5.25), animatedParameterValue(original, address, 5.25));
    EXPECT_EQ(animatedParameterValue(loaded.document, occurrence, 3.5),
              (ParameterValue{ColorValue{{0.25F, 0.5F, 0.75F, 1.F}}}));
}

TEST(PersistenceTest, AnimationRetiredIdentitiesSurviveSaveLoadAndCannotBeReused) {
    auto document = animatedDocument();
    const auto oldChannel = document.animationChannels().front();
    removeKeyframesCommand({{oldChannel.id, oldChannel.keys[0].id}, {oldChannel.id, oldChannel.keys[1].id}})
        .apply(document);
    auto loaded = loadDocument(saveDocument(document)).document;
    setKeyframesCommand({{oldChannel.address, Keyframe{0, 1, ColorValue{{0, 0, 0, 1}}}}}).apply(loaded);
    ASSERT_EQ(loaded.animationChannels().size(), 1U);
    EXPECT_GT(loaded.animationChannels().front().id, oldChannel.id);
    EXPECT_GT(loaded.animationChannels().front().keys.front().id, oldChannel.keys.back().id);
}

TEST(PersistenceTest, SchemaThreeMigratesWithoutInventingAnimation) {
    auto encoded = saveDocument(sampleDocument());
    encoded["schema"] = 3;
    encoded.erase("animationChannels");
    encoded.erase("nextAnimationChannelId");
    encoded.erase("nextKeyframeId");
    const auto loaded = loadDocument(encoded);
    EXPECT_TRUE(loaded.document.animationChannels().empty());
    EXPECT_EQ(root(loaded.document).graph().nodeByName("plate")->params.at("future"),
              ParameterValue{std::string{"1.5"}});
}

TEST(PersistenceTest, AnimationRejectsConflictingIdentitiesTimesAndAddresses) {
    const auto encoded = saveDocument(animatedDocument());
    auto duplicateTime = encoded;
    duplicateTime["animationChannels"][0]["keys"][1]["time"] = duplicateTime["animationChannels"][0]["keys"][0]["time"];
    EXPECT_THROW(loadDocument(duplicateTime), DeserializeError);
    auto duplicateKey = encoded;
    duplicateKey["animationChannels"][0]["keys"][1]["id"] = duplicateKey["animationChannels"][0]["keys"][0]["id"];
    EXPECT_THROW(loadDocument(duplicateKey), DeserializeError);
    auto duplicateAddress = encoded;
    auto channel = duplicateAddress["animationChannels"][0];
    channel["id"] = 100;
    channel["keys"][0]["id"] = 100;
    channel["keys"][1]["id"] = 101;
    duplicateAddress["animationChannels"].push_back(channel);
    EXPECT_THROW(loadDocument(duplicateAddress), DeserializeError);
    auto missingNode = encoded;
    missingNode["animationChannels"][0]["address"]["node"] = 100;
    EXPECT_THROW(loadDocument(missingNode), DeserializeError);
}

TEST(PersistenceTest, AnimationRejectsMalformedValuesAndTangentsInsteadOfDroppingChannels) {
    const auto encoded = saveDocument(animatedDocument());
    auto nonfinite = encoded;
    nonfinite["animationChannels"][0]["keys"][0]["time"] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(loadDocument(nonfinite), DeserializeError);
    auto slopes = encoded;
    slopes["animationChannels"][0]["keys"][0]["tangentMode"] = "smooth";
    EXPECT_THROW(loadDocument(slopes), DeserializeError);
    auto type = encoded;
    type["animationChannels"][0]["keys"][0]["value"] = {{"type", "boolean"}, {"value", true}};
    EXPECT_THROW(loadDocument(type), DeserializeError);
    auto interpolation = encoded;
    interpolation["animationChannels"][0]["keys"][0]["interpolation"] = "unsupported";
    EXPECT_THROW(loadDocument(interpolation), DeserializeError);
    auto unavailable = encoded;
    unavailable["animationChannels"][0]["address"]["key"] = "missing-parameter";
    EXPECT_THROW(loadDocument(unavailable), DeserializeError);
}
