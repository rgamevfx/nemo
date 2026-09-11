#include <array>
#include <exception>
#include <gtest/gtest.h>
#include <thread>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

using namespace nemo;

namespace {

Network& root(Document& document) {
    return document.network(document.rootNetworkId());
}

std::shared_ptr<const NodeCatalog> typedCatalog() {
    NodeDescriptor pass;
    pass.type = "fixture.pass";
    pass.displayName = "Pass";
    pass.inputs = {{PortKind::Image, "in"}};
    pass.outputs = {{PortKind::Image, "out"}};
    NodeDescriptor mask;
    mask.type = "fixture.mask";
    mask.displayName = "Mask";
    mask.outputs = {{PortKind::Mask, "mask"}};
    NodeDescriptor maskSink;
    maskSink.type = "fixture.maskSink";
    maskSink.displayName = "Mask Sink";
    maskSink.inputs = {{PortKind::Mask, "mask"}};
    return std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{pass, mask, maskSink});
}

}  // namespace

TEST(NetworkTest, FreshDocumentHasRootNetworkAndDefaultOutput) {
    Document document;
    ASSERT_NE(document.rootNetworkId(), kInvalidNetwork);
    auto& network = root(document);
    ASSERT_NE(network.defaultOutput(), kInvalidNode);
    ASSERT_NE(network.graph().node(network.defaultOutput()), nullptr);
    EXPECT_TRUE(network.graph().descriptor(network.graph().node(network.defaultOutput())->type)->isOutput);

    const auto second = network.graph().addNode("output", "delivery");
    network.setDefaultOutput(second);
    EXPECT_EQ(network.defaultOutput(), second);
}

TEST(NetworkTest, NetworkScopedIdentitiesUndoAndRedo) {
    Document document;
    CommandStack history(document);
    const auto network = document.rootNetworkId();
    auto created = std::make_shared<NodeId>();
    history.push(addNodeCommand(network, "testpattern", "source", created));
    ASSERT_NE(document.network(network).graph().node(*created), nullptr);
    EXPECT_TRUE(history.undo());
    EXPECT_EQ(document.network(network).graph().node(*created), nullptr);
    EXPECT_TRUE(history.redo());
    EXPECT_NE(document.network(network).graph().node(*created), nullptr);
}

TEST(NetworkTest, UndoingNetworkCreationDoesNotLoseRetiredNodeWatermarks) {
    Document document;
    CommandStack history(document);
    auto network = std::make_shared<NetworkId>();
    auto original = std::make_shared<NodeId>();
    history.push(addNetworkCommand("shared", network));
    history.push(addNodeCommand(*network, "testpattern", "original", original));
    ASSERT_TRUE(history.undo());
    ASSERT_TRUE(history.undo());
    ASSERT_TRUE(history.redo());
    auto replacement = std::make_shared<NodeId>();
    history.push(addNodeCommand(*network, "testpattern", "replacement", replacement));
    EXPECT_GT(*replacement, *original);
    EXPECT_EQ(document.network(*network).graph().node(*original), nullptr);
    EXPECT_FALSE(history.canRedo());
}

TEST(NetworkTest, ImageAndMaskBoundariesAndNestedCyclesAreRejectedAtomically) {
    Document document(typedCatalog());
    const auto parentId = document.rootNetworkId();
    const auto image = document.network(parentId).graph().addNode("testpattern", "image");
    const auto mask = document.network(parentId).graph().addNode("fixture.mask", "mask");
    const auto definitionId = document.addNetwork("child");
    auto& definition = document.network(definitionId);
    const auto imageInput = definition.addInput("image", PortKind::Image, 51);
    const auto maskInput = definition.addInput("mask", PortKind::Mask, 52);
    const auto beauty = definition.addOutput("beauty", PortKind::Image, 53);
    const auto matte = definition.addOutput("matte", PortKind::Mask, 54);
    const auto passA = definition.graph().addNode("fixture.pass", "a");
    const auto passB = definition.graph().addNode("fixture.pass", "b");
    const auto maskSink = definition.graph().addNode("fixture.maskSink", "mask-in");
    const auto maskSource = definition.graph().addNode("fixture.mask", "mask-out");
    definition.connectInput(imageInput, {passA, 0});
    definition.connectInput(imageInput, {passB, 0});
    definition.connectInput(maskInput, {maskSink, 0});
    definition.connectOutput({passA, 0}, beauty);
    definition.connectOutput({maskSource, 0}, matte);
    const auto use = document.addInstance(parentId, definitionId, "child-use");
    const auto node = document.instance(use)->node;
    CommandStack history(document);
    history.push(bindInstanceInputCommand(use, imageInput, {image, 0}));
    history.push(bindInstanceInputCommand(use, maskInput, {mask, 0}));
    const auto before = saveDocument(document);
    const auto depth = history.depth();
    EXPECT_THROW(history.push(bindInstanceInputCommand(use, maskInput, {image, 0})), GraphException);
    EXPECT_THROW(history.push(bindInstanceInputCommand(use, imageInput, {mask, 0})), GraphException);
    EXPECT_THROW(history.push(addInstanceCommand(definitionId, parentId, "cycle")), GraphException);
    EXPECT_EQ(saveDocument(document), before);
    EXPECT_EQ(history.depth(), depth);
    EXPECT_EQ(document.instance(use)->inputBindings.at(imageInput), (PortRef{image, 0}));
    EXPECT_EQ(document.instance(use)->inputBindings.at(maskInput), (PortRef{mask, 0}));
    EXPECT_EQ(document.network(definitionId).inputConnections().size(), 3u);
    const auto& outputs = document.network(parentId).graph().outputPorts(node);
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].kind, PortKind::Image);
    EXPECT_EQ(outputs[1].kind, PortKind::Mask);
}

TEST(NetworkTest, SharedDefinitionHasIndependentInstanceBindingsAndValues) {
    Document document;
    const auto definition = document.addNetwork("shared");
    auto& shared = document.network(definition);
    const auto input = shared.addInput("source", PortKind::Image, 31);
    const auto target = shared.graph().addNode("source", "control");
    const auto source = root(document).graph().addNode("testpattern", "parent-source");
    const auto first = document.addInstance(document.rootNetworkId(), definition, "first");
    const auto second = document.addInstance(document.rootNetworkId(), definition, "second");
    document.bindInstanceInput(first, input, {source, 0});
    document.setInstanceParam(first, target, "source", "first.mov");
    document.setInstanceParam(second, target, "source", "second.mov");
    ASSERT_EQ(document.instances().size(), 2u);
    EXPECT_EQ(document.instance(first)->definition, definition);
    EXPECT_EQ(document.instance(second)->definition, definition);
    EXPECT_EQ(std::get<std::string>(document.instance(first)->params.at(target).at("source")), "first.mov");
    EXPECT_EQ(std::get<std::string>(document.instance(second)->params.at(target).at("source")), "second.mov");
    EXPECT_TRUE(document.instance(second)->inputBindings.empty());
}

TEST(NetworkTest, TransactionCreatesNativeNodesAndSelectsOutputAsOneUndoStep) {
    Document document;
    CommandStack history(document);
    constexpr NetworkId createdNetwork = 42;
    history.push(transactionCommand(
        "build network", {
                             Command{"create network and native graph",
                                     [=](Document& document) {
                                         auto& network =
                                             document.network(document.addNetworkWithId(createdNetwork, "transaction"));
                                         const auto source = network.graph().addNode("testpattern", "source");
                                         const auto merge = network.graph().addNode("merge", "merge");
                                         network.graph().connect({source, 0}, {merge, 0});
                                         const auto output = network.defaultOutput();
                                         network.graph().connect({merge, 0}, {output, 0});
                                         network.setDefaultOutput(output);
                                     }},
                         }));
    EXPECT_EQ(history.depth(), 1u);
    ASSERT_EQ(document.networks().size(), 2u);
    EXPECT_EQ(document.network(createdNetwork).graph().edges().size(), 2u);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(document.networks().size(), 1u);
    EXPECT_TRUE(history.redo());
    EXPECT_EQ(document.network(createdNetwork).graph().edges().size(), 2u);
}

TEST(NetworkTest, NetworkSchemaRoundTripPreservesInterfacesAndInstances) {
    Document original;
    const auto definition = original.addNetwork("tool");
    auto& network = original.network(definition);
    const auto input = network.addInput("matte", PortKind::Mask, 7);
    const auto output = network.addOutput("beauty", PortKind::Image, 8);
    const auto source = network.graph().addNode("source", "source");
    network.graph().setLayout(network.defaultOutput(), {42.0, 9.0});
    network.connectOutput({source, 0}, output);
    const auto instance = original.addInstance(original.rootNetworkId(), definition, "tool-use");
    original.setInstanceParam(instance, source, "source", "media.mov");
    const auto loaded = loadDocument(saveDocument(original));
    ASSERT_EQ(loaded.document.networks().size(), 2u);
    EXPECT_EQ(loaded.document.network(definition).input(7)->kind, PortKind::Mask);
    EXPECT_EQ(loaded.document.network(definition).output(8)->kind, PortKind::Image);
    EXPECT_EQ(std::get<std::string>(loaded.document.instances().front().params.at(source).at("source")), "media.mov");
    EXPECT_EQ(loaded.document.network(definition).graph().node(network.defaultOutput())->layout,
              LayoutPosition(42.0, 9.0));
}

TEST(NetworkTest, SharedConstSnapshotEvaluatesNestedNetworksConcurrently) {
    Document document;
    const auto rootId = document.rootNetworkId();
    const auto definition = document.addNetwork("shared");
    auto& child = document.network(definition);
    const auto color = child.graph().addNode("constcolor", "color");
    child.graph().setParam(color, "color", ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}});
    const auto beauty = child.addOutput("beauty", PortKind::Image);
    child.connectOutput({color, 0}, beauty);
    const auto occurrence = document.addInstance(rootId, definition, "use");
    const auto instanceNode = document.instance(occurrence)->node;
    CommandStack history(document);
    history.push(connectCommand(rootId, {instanceNode, 0}, {document.network(rootId).defaultOutput(), 0}));
    const Document snapshot = document;
    const auto revision = snapshot.stateRevision();
    EvaluationRequest request;
    request.network = rootId;
    request.output = snapshot.network(rootId).defaultOutput();
    request.region = {0, 0, 1, 1};
    std::array<std::exception_ptr, 4> failures{};
    std::array<float, 4> red{};
    {
        std::vector<std::jthread> workers;
        for (std::size_t i = 0; i < failures.size(); ++i) {
            workers.emplace_back([&, i] {
                try {
                    for (int iteration = 0; iteration < 8; ++iteration) {
                        red[i] = evaluateCpu(snapshot, request).image.pixel(0, 0)[0];
                        if (snapshot.stateRevision() != revision)
                            throw std::runtime_error("snapshot query changed authored state");
                    }
                } catch (...) {
                    failures[i] = std::current_exception();
                }
            });
        }
    }
    for (std::size_t i = 0; i < failures.size(); ++i) {
        EXPECT_EQ(failures[i], nullptr);
        EXPECT_FLOAT_EQ(red[i], 0.25F);
    }
}
