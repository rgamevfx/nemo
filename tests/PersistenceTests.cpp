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

TEST(PersistenceTest, MediaLibraryBinsEntriesAndWatermarksRoundTrip) {
    Document original;
    auto& catalog = original.mediaCatalog;
    const auto shots = catalog.addBin("Shots");
    const auto video = catalog.addBin("Video", shots, MediaQueryDescriptor{.text = "hero", .kind = MediaKind::Video});
    MediaMetadata metadata;
    metadata.userName = "Hero";
    metadata.description = "plate";
    metadata.tags = {"hero", "plate"};
    metadata.label = "blue";
    metadata.offline = true;
    metadata.kind = MediaKind::Video;
    MediaProbeMetadata probe;
    probe.width = 1920;
    probe.height = 1080;
    probe.duration = 240;
    probe.codec = "prores";
    probe.colorPrimaries = "bt709";
    probe.status = MediaProbeStatus::Ready;
    metadata.committedProbe = probe;
    MediaMarkRange mark;
    mark.inFrame = 0;
    mark.outFrame = 12;
    const auto entry = catalog.addEntry("plate.mov", video, metadata, {mark});
    const auto removed = catalog.addEntry("old.exr", shots, MediaMetadata{});
    catalog.removeEntry(removed);

    const auto encoded = saveDocument(original);
    ASSERT_TRUE(encoded.contains("format"));
    EXPECT_EQ(encoded.at("format"), "nemo");
    const auto loaded = loadDocument(encoded);
    ASSERT_TRUE(loaded.warnings.empty());
    EXPECT_EQ(loaded.document.mediaCatalog.bins(), original.mediaCatalog.bins());
    EXPECT_EQ(loaded.document.mediaCatalog.entries(), original.mediaCatalog.entries());
    EXPECT_EQ(loaded.document.mediaCatalog.entry(entry)->marks, std::vector<MediaMarkRange>{mark});
    EXPECT_EQ(loaded.document.nextMediaSourceId(), original.nextMediaSourceId());
    EXPECT_EQ(loaded.document.nextMediaBinId(), original.nextMediaBinId());
    EXPECT_EQ(saveDocument(loaded.document), encoded);
}

TEST(PersistenceTest, UnknownExtensionDataSurvivesRoundTripWithoutResurrection) {
    Document document;
    auto& network = root(document);
    const NodeId plate = network.graph().addNode("testpattern", "plate");
    const NodeId output = network.graph().nodeByName("Output")->id;

    nlohmann::json saved = saveDocument(document);
    auto& nodes = saved["networks"][0]["nodes"];
    nodes.push_back({{"id", 77},
                     {"type", "futurenode"},
                     {"name", "future"},
                     {"params", {{"mesh", {{"type", "mesh"}, {"value", {1, 2, 3}}}}}},
                     {"futureOnly", {{"marker", 4242}}}});
    auto plateNode = std::find_if(nodes.begin(), nodes.end(),
                                  [](const auto& entry) { return entry.value("name", std::string{}) == "plate"; });
    ASSERT_NE(plateNode, nodes.end());
    (*plateNode)["mesh"] = {{"vertices", 3}};
    (*plateNode)["implementation"] = {{"version", 2}};
    saved["networks"][0]["futureNetwork"] = true;
    saved["networks"][0]["edges"].push_back({{"id", 500},
                                             {"from", {{"node", plate}, {"port", 0}}},
                                             {"to", {{"node", 77}, {"port", 0}}},
                                             {"style", "dashed"}});
    saved["networks"][0]["edges"].push_back(
        {{"id", 501}, {"from", {{"node", 77}, {"port", 0}}}, {"to", {{"node", output}, {"port", 0}}}});
    saved["futureTop"] = {{"a", 1}};

    const auto first = loadDocument(saved);
    const auto* future = root(first.document).graph().nodeByName("future");
    ASSERT_NE(future, nullptr);
    // Connections authored against the unavailable node type survive exactly.
    EXPECT_EQ(root(first.document).graph().edges().size(), 2u);
    ASSERT_TRUE(future->opaqueParams.is_object());
    EXPECT_EQ(future->opaqueParams.at("mesh").at("type"), "mesh");
    bool warned = false;
    for (const auto& warning : first.warnings)
        warned = warned || warning.find("futurenode") != std::string::npos;
    EXPECT_TRUE(warned);

    const auto roundTripped = saveDocument(first.document);
    EXPECT_EQ(roundTripped.at("futureTop").at("a"), 1);
    EXPECT_TRUE(roundTripped.at("networks").at(0).at("futureNetwork").get<bool>());
    const auto& encodedNodes = roundTripped.at("networks").at(0).at("nodes");
    const auto plateIt = std::find_if(encodedNodes.begin(), encodedNodes.end(),
                                      [](const auto& entry) { return entry.value("name", std::string{}) == "plate"; });
    ASSERT_NE(plateIt, encodedNodes.end());
    EXPECT_EQ(plateIt->at("mesh").at("vertices"), 3);
    EXPECT_EQ(plateIt->at("implementation").at("version"), 2);
    const auto futureIt = std::find_if(encodedNodes.begin(), encodedNodes.end(), [](const auto& entry) {
        return entry.value("name", std::string{}) == "future";
    });
    ASSERT_NE(futureIt, encodedNodes.end());
    EXPECT_EQ(futureIt->at("futureOnly").at("marker"), 4242);
    EXPECT_EQ(futureIt->at("params").at("mesh").at("type"), "mesh");
    const auto& encodedEdges = roundTripped.at("networks").at(0).at("edges");
    const auto styledIt = std::find_if(encodedEdges.begin(), encodedEdges.end(), [](const auto& entry) {
        return entry.is_object() && entry.value("id", std::uint64_t{0}) == 500;
    });
    ASSERT_NE(styledIt, encodedEdges.end());
    EXPECT_EQ(styledIt->at("style"), "dashed");

    // A second load/save is byte-stable and proves nothing was preserved twice.
    EXPECT_EQ(saveDocument(loadDocument(roundTripped).document), roundTripped);

    // Deleting the record drops its preserved data instead of resurrecting it.
    Document trimmed = first.document;
    root(trimmed).graph().removeNode(future->id);
    EXPECT_EQ(saveDocument(trimmed).dump().find("4242"), std::string::npos);

    // Animation channel/key extension fields survive the typed codec as well.
    auto animated = saveDocument(animatedDocument());
    animated["animationChannels"][0]["futureChannel"] = true;
    animated["animationChannels"][0]["keys"][0]["futureKey"] = 7;
    const auto animatedSaved = saveDocument(loadDocument(animated).document);
    EXPECT_TRUE(animatedSaved.at("animationChannels").at(0).at("futureChannel").get<bool>());
    EXPECT_EQ(animatedSaved.at("animationChannels").at(0).at("keys").at(0).at("futureKey"), 7);
}

TEST(PersistenceTest, ProjectFormatAndRequiredFeaturesAreEnforced) {
    const auto encoded = saveDocument(sampleDocument());
    ASSERT_TRUE(encoded.contains("format"));
    EXPECT_EQ(encoded.at("format"), "nemo");
    ASSERT_TRUE(encoded.at("requiredFeatures").is_array());

    auto wrongFormat = encoded;
    wrongFormat["format"] = "zip";
    EXPECT_THROW(loadDocument(wrongFormat), DeserializeError);

    auto unknownFeature = encoded;
    unknownFeature["requiredFeatures"].push_back({{"id", "timeline"}, {"version", 1}});
    try {
        (void)loadDocument(unknownFeature);
        FAIL() << "an unknown required feature must be rejected";
    } catch (const DeserializeError& error) {
        EXPECT_NE(std::string(error.what()).find("timeline"), std::string::npos);
    }

    auto newerFeature = encoded;
    newerFeature["requiredFeatures"] = {{{"id", "networks"}, {"version", 2}}};
    EXPECT_THROW(loadDocument(newerFeature), DeserializeError);

    auto malformedFeature = encoded;
    malformedFeature["requiredFeatures"] = nlohmann::json::array({nlohmann::json{{"id", "networks"}}});
    EXPECT_THROW(loadDocument(malformedFeature), DeserializeError);

    auto nonArray = encoded;
    nonArray["requiredFeatures"] = 5;
    EXPECT_THROW(loadDocument(nonArray), DeserializeError);

    // Legacy files carry no discriminator and still migrate through schema.
    auto legacy = encoded;
    legacy.erase("format");
    legacy.erase("requiredFeatures");
    EXPECT_TRUE(loadDocument(legacy).warnings.empty());
}

TEST(PersistenceTest, MediaIdentityWatermarksSurviveRoundTrip) {
    Document original;
    auto& catalog = original.mediaCatalog;
    const auto shots = catalog.addBin("Shots");
    const auto scratch = catalog.addBin("Scratch");
    catalog.removeBin(scratch, false);
    const auto removed = catalog.addEntry("old.exr", shots, MediaMetadata{});
    catalog.removeEntry(removed);

    auto loaded = loadDocument(saveDocument(original));
    EXPECT_EQ(loaded.document.nextMediaBinId(), original.nextMediaBinId());
    EXPECT_EQ(loaded.document.nextMediaSourceId(), original.nextMediaSourceId());
    EXPECT_GT(loaded.document.mediaCatalog.addBin("fresh"), scratch);
    EXPECT_GT(loaded.document.mediaCatalog.addEntry("new.exr", shots, MediaMetadata{}), removed);
}

TEST(PersistenceTest, UnavailableNodeAnimationIsRetainedAndRecoversWithCatalog) {
    nlohmann::json saved = saveDocument(Document{});
    const auto rootId = saved.at("rootNetworkId").get<std::uint64_t>();
    saved["networks"][0]["nodes"].push_back({{"id", 5},
                                             {"type", "futuregrade"},
                                             {"name", "future"},
                                             {"params", {{"gain", {{"type", "float"}, {"value", 1.0}}}}}});
    const nlohmann::json key = {{"id", 1},
                                {"time", 0.0},
                                {"value", {{"type", "float"}, {"value", 2.0}}},
                                {"interpolation", "linear"},
                                {"tangentMode", "smooth"},
                                {"inSlope", {0, 0, 0, 0}},
                                {"outSlope", {0, 0, 0, 0}}};
    const nlohmann::json last = {{"id", 2},
                                 {"time", 10.0},
                                 {"value", {{"type", "float"}, {"value", 6.0}}},
                                 {"interpolation", "hold"},
                                 {"tangentMode", "smooth"},
                                 {"inSlope", {0, 0, 0, 0}},
                                 {"outSlope", {0, 0, 0, 0}}};
    const nlohmann::json opaqueKey = {{"id", 3},
                                      {"time", 0.0},
                                      {"value", {{"type", "futurecurve"}, {"value", {{"samples", {1, 2, 3}}}}}},
                                      {"interpolation", "hold"},
                                      {"tangentMode", "smooth"},
                                      {"inSlope", {0, 0, 0, 0}},
                                      {"outSlope", {0, 0, 0, 0}}};
    saved["animationChannels"] = nlohmann::json::array(
        {{{"id", 1}, {"address", {{"network", rootId}, {"node", 5}, {"key", "gain"}}}, {"keys", {key, last}}},
         {{"id", 2}, {"address", {{"network", rootId}, {"node", 5}, {"key", "shape"}}}, {"keys", {opaqueKey}}}});
    saved["nextAnimationChannelId"] = 3;
    saved["nextKeyframeId"] = 4;
    saved["requiredFeatures"].push_back({{"id", "animation"}, {"version", 1}});

    // Without the node's implementation the authored channel is retained as
    // disabled data instead of rejecting the whole project.
    const auto unavailable = loadDocument(saved);
    ASSERT_NE(root(unavailable.document).graph().nodeByName("future"), nullptr);
    ASSERT_EQ(unavailable.document.animationChannels().size(), 2u);
    const auto* typed = unavailable.document.animationChannel(ParameterAddress{rootId, 5, "gain"});
    ASSERT_NE(typed, nullptr);
    ASSERT_EQ(typed->keys.size(), 2u);
    EXPECT_EQ(typed->keys.front().value, ParameterValue{2.0});
    const auto* opaque = unavailable.document.animationChannel(ParameterAddress{rootId, 5, "shape"});
    ASSERT_NE(opaque, nullptr);
    ASSERT_EQ(opaque->keys.size(), 1u);
    EXPECT_FALSE(opaque->keys.front().opaqueValue.is_null());
    EXPECT_FALSE(unavailable.warnings.empty());

    const auto retained = saveDocument(unavailable.document);
    const auto reopened = loadDocument(retained);
    ASSERT_EQ(reopened.document.animationChannels().size(), 2u);
    EXPECT_FALSE(
        reopened.document.animationChannel(ParameterAddress{rootId, 5, "shape"})->keys.front().opaqueValue.is_null());
    EXPECT_EQ(saveDocument(reopened.document), retained);

    // Restoring the implementation recovers the authored animation.
    NodeDescriptor descriptor;
    descriptor.type = "futuregrade";
    descriptor.displayName = "Future grade";
    descriptor.outputs = {{PortKind::Image, "out"}};
    ParameterSpec gain;
    gain.name = "gain";
    gain.type = ParameterType::Float;
    gain.defaultValue = 1.0;
    descriptor.parameters.push_back(gain);
    const auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
    const auto recovered = loadDocument(retained, catalog);
    const auto* channel = recovered.document.animationChannel(ParameterAddress{rootId, 5, "gain"});
    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(animatedParameterValue(recovered.document, ParameterAddress{rootId, 5, "gain"}, 0.0),
              ParameterValue{2.0});
    EXPECT_EQ(animatedParameterValue(recovered.document, ParameterAddress{rootId, 5, "gain"}, 10.0),
              ParameterValue{6.0});
    const auto* recoveredOpaque = recovered.document.animationChannel(ParameterAddress{rootId, 5, "shape"});
    ASSERT_NE(recoveredOpaque, nullptr);
    EXPECT_FALSE(recoveredOpaque->keys.front().opaqueValue.is_null());
}

TEST(PersistenceTest, OpaqueAnimatedValueMakesOnlyItsOwnEvaluationUnavailable) {
    Document document;
    auto& rootNetwork = root(document);
    const auto opaqueRoot = rootNetwork.graph().addNode("constcolor", "opaque");
    const auto clean = rootNetwork.graph().addNode("constcolor", "clean");
    setKeyframesCommand({{ParameterAddress{document.rootNetworkId(), opaqueRoot, "color"},
                          Keyframe{0, 0.0, ColorValue{{1.F, 0.F, 0.F, 1.F}}}}})
        .apply(document);
    setKeyframesCommand({{ParameterAddress{document.rootNetworkId(), clean, "color"},
                          Keyframe{0, 0.0, ColorValue{{0.F, 0.F, 1.F, 1.F}}}}})
        .apply(document);

    const auto definition = document.addNetwork("shared-opaque");
    const auto definitionNode = document.network(definition).graph().addNode("constcolor", "grade");
    const auto instance = document.addInstance(document.rootNetworkId(), definition, "use-opaque");
    setKeyframesCommand(
        {{ParameterAddress{definition, definitionNode, "color"}, Keyframe{0, 0.0, ColorValue{{1.F, 0.F, 0.F, 1.F}}}}})
        .apply(document);
    setKeyframesCommand({{ParameterAddress{definition, definitionNode, "color", instance},
                          Keyframe{0, 0.0, ColorValue{{0.F, 1.F, 0.F, 1.F}}}}})
        .apply(document);

    // Only the authored values of the two definition-level channels become
    // uninterpretable; the occurrence override and the independent node stay typed.
    auto encoded = saveDocument(document);
    const auto rootId = document.rootNetworkId();
    for (auto& channel : encoded["animationChannels"]) {
        const auto& address = channel.at("address");
        if (address.contains("instance"))
            continue;
        const auto network = address.at("network").get<std::uint64_t>();
        const auto node = address.at("node").get<std::uint64_t>();
        if ((network == rootId && node == opaqueRoot) || (network == definition && node == definitionNode))
            channel["keys"][0]["value"] = {{"type", "futurecolor"}, {"value", {1, 2, 3, 4}}};
    }
    const auto loaded = loadDocument(encoded);
    ASSERT_EQ(loaded.document.animationChannels().size(), 4u);

    // The channel is still saved and reopened losslessly.
    EXPECT_EQ(saveDocument(loadDocument(saveDocument(loaded.document)).document), saveDocument(loaded.document));

    ParameterValues values;
    EXPECT_THROW(animatedParameterValue(loaded.document, ParameterAddress{rootId, opaqueRoot, "color"}, 1.0),
                 GraphException);
    EXPECT_THROW(applyAnimationParameters(loaded.document, rootId, opaqueRoot, kInvalidNetworkInstance, 1.0, values),
                 GraphException);
    EXPECT_THROW(
        applyAnimationParameters(loaded.document, definition, definitionNode, kInvalidNetworkInstance, 1.0, values),
        GraphException);
    // An independent node and an overridden occurrence still evaluate.
    ASSERT_NO_THROW(applyAnimationParameters(loaded.document, rootId, clean, kInvalidNetworkInstance, 1.0, values));
    EXPECT_EQ(values.at("color"), (ParameterValue{ColorValue{{0.F, 0.F, 1.F, 1.F}}}));
    ASSERT_NO_THROW(applyAnimationParameters(loaded.document, definition, definitionNode, instance, 1.0, values));
    EXPECT_EQ(values.at("color"), (ParameterValue{ColorValue{{0.F, 1.F, 0.F, 1.F}}}));
}
