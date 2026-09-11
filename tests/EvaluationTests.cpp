#include <gtest/gtest.h>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace nemo;

namespace {
Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

Document makeDocument(const std::vector<std::pair<std::string, std::string>>& typeAndName) {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    for (const auto& [type, name] : typeAndName) {
        rootGraph(document).addNode(type, name);
    }
    return document;
}

NodeDescriptor capabilityFixture(NodeCapabilities capabilities) {
    return NodeDescriptor{.type = "fixture.capability",
                          .displayName = "Capability Fixture",
                          .group = "Tests",
                          .inputs = {},
                          .outputs = {{PortKind::Image, "color"}},
                          .capabilities = std::move(capabilities)};
}
void connect(Graph& graph, const std::string& from, const std::string& to, std::uint32_t fromPort = 0,
             std::uint32_t toPort = 0) {
    const NodeInstance* fromNode = graph.nodeByName(from);
    const NodeInstance* toNode = graph.nodeByName(to);
    ASSERT_NE(fromNode, nullptr);
    ASSERT_NE(toNode, nullptr);
    static_cast<void>(graph.connect(PortRef{fromNode->id, fromPort}, PortRef{toNode->id, toPort}));
}

EvaluationRequest fullFrameRequest(const Document& document, std::int64_t frame, int width = 8, int height = 4) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = resolveOutput(document, request.network);
    request.localTime = frame;
    request.region = {0, 0, width, height};
    return request;
}

const PlanStep* stepFor(const EvaluationPlan& plan, const std::string& name) {
    for (const auto& step : plan.steps) {
        if (step.name == name) {
            return &step;
        }
    }
    return nullptr;
}

}  // namespace

// Acceptance example 1a: `plate -> output` renders the plate pattern.
TEST(EvaluationTest, LinearChainRendersPatternThroughOutput) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(document), "plate", "out");

    const CpuEvaluation evaluation = evaluateCpu(document, fullFrameRequest(document, 0));

    ASSERT_EQ(evaluation.plan.steps.size(), 2);
    // Dependencies first: the plate is scheduled before the output.
    EXPECT_EQ(evaluation.plan.steps[0].name, "plate");
    EXPECT_EQ(evaluation.plan.steps[1].name, "out");
    EXPECT_EQ(evaluation.plan.steps[1].inputs, (std::vector<NodeId>{evaluation.plan.steps[0].node}));
    // Pattern: red gradient reaches 1.0 at the right edge; the output node
    // passes its input through unchanged.
    const auto lastPixel = evaluation.image.pixel(evaluation.image.width() - 1, 0);
    EXPECT_FLOAT_EQ(lastPixel[0], 1.0F);
    EXPECT_FLOAT_EQ(lastPixel[1], 0.0F);
    EXPECT_FLOAT_EQ(lastPixel[3], 1.0F);
    EXPECT_EQ(evaluation.plan.result.contentHash, evaluation.plan.steps[1].produced.contentHash);
    EXPECT_EQ(evaluation.plan.result.residency, Residency::HostCpuReference);
}

// Acceptance example 1b: `plate -> merge(over, constcolor) -> output` renders
// the pattern composited over a solid color.
TEST(EvaluationTest, MergeCompositesPatternOverConstColor) {
    Document document =
        makeDocument({{"testpattern", "plate"}, {"constcolor", "backdrop"}, {"merge", "comp"}, {"output", "out"}});
    rootGraph(document).setParam(rootGraph(document).nodeByName("backdrop")->id, "color",
                                 ColorValue{{0.0F, 0.0F, 1.0F, 0.5F}});
    connect(rootGraph(document), "plate", "comp", 0, 0);     // port A: over base
    connect(rootGraph(document), "backdrop", "comp", 0, 1);  // port B: over source
    connect(rootGraph(document), "comp", "out");

    const CpuEvaluation evaluation = evaluateCpu(document, fullFrameRequest(document, 0));

    const PlanStep* merge = stepFor(evaluation.plan, "comp");
    const PlanStep* plate = stepFor(evaluation.plan, "plate");
    const PlanStep* backdrop = stepFor(evaluation.plan, "backdrop");
    ASSERT_NE(merge, nullptr);
    ASSERT_NE(plate, nullptr);
    ASSERT_NE(backdrop, nullptr);
    ASSERT_EQ(merge->inputs.size(), 2);
    EXPECT_EQ(merge->inputs[0], plate->node);
    EXPECT_EQ(merge->inputs[1], backdrop->node);
    EXPECT_EQ(merge->inputImages[1].contentHash, backdrop->produced.contentHash);
    const auto pixel = evaluation.image.pixel(4, 0);
    EXPECT_NEAR(pixel[0], 0.5F * (4.0F / 7.0F), 1e-6F);
    EXPECT_NEAR(pixel[2], 0.5F * 1.0F, 1e-6F);
    // Effective parameter state records the resolved operation.
    EXPECT_EQ(std::get<ChoiceValue>(merge->effectiveParams.at("operation")).value, "over");
}

// Acceptance example 2: no Output node -> evaluation error naming the
// document, surfaced verbatim by the CLI as {"errors": [...]} with exit 1.
TEST(EvaluationTest, MissingOutputNodeFailsWithClearError) {
    Document document = makeDocument({{"testpattern", "plate"}});
    try {
        static_cast<void>(resolveOutput(document, document.rootNetworkId()));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& e) {
        EXPECT_NE(std::string(e.what()).find("no Output node"), std::string::npos);
    }
}

TEST(EvaluationTest, ExplicitOutputSelectionUsesNetworkScope) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "a"}, {"output", "b"}});
    connect(rootGraph(document), "plate", "a");
    connect(rootGraph(document), "plate", "b");
    const NetworkId network = document.rootNetworkId();
    EXPECT_EQ(resolveOutput(document, network, "b"), rootGraph(document).nodeByName("b")->id);
    EvaluationRequest request;
    request.network = network;
    request.output = resolveOutput(document, network, "b");
    EXPECT_EQ(rootGraph(document).node(request.output)->name, "b");
}

// Invalid-type connections are rejected by Graph at edit time.
TEST(EvaluationTest, InvalidTypeConnectionRejectedByGraph) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    const NodeId plate = rootGraph(document).nodeByName("plate")->id;
    const NodeId out = rootGraph(document).nodeByName("out")->id;
    // The Output node declares no output ports: anything fed from it is a
    // type error, not a silent dangling edge.
    const auto fromOutput = rootGraph(document).validateEdge(PortRef{out, 0}, PortRef{plate, 0});
    ASSERT_TRUE(fromOutput.has_value());
    EXPECT_EQ(fromOutput->code, GraphError::PortType);
    EXPECT_THROW(static_cast<void>(rootGraph(document).connect(PortRef{out, 0}, PortRef{plate, 0})), GraphException);
    // testpattern declares no input ports either.
    const auto intoPattern = rootGraph(document).validateEdge(PortRef{plate, 0}, PortRef{plate, 0});
    ASSERT_TRUE(intoPattern.has_value());
    EXPECT_EQ(intoPattern->code, GraphError::PortType);
}

// Acceptance example 3 (core side): same document + same frame produce an
// identical plan and identical image; a different frame changes the recorded
// image identity.
TEST(EvaluationTest, RepeatedRequestsAreDeterministicAndFrameSensitive) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(document), "plate", "out");

    const CpuEvaluation first = evaluateCpu(document, fullFrameRequest(document, 3));
    const CpuEvaluation second = evaluateCpu(document, fullFrameRequest(document, 3));
    const CpuEvaluation other = evaluateCpu(document, fullFrameRequest(document, 4));

    EXPECT_EQ(first.plan.result.contentHash, second.plan.result.contentHash);
    EXPECT_EQ(first.image.data()[0], second.image.data()[0]);
    EXPECT_NE(first.plan.result.contentHash, other.plan.result.contentHash);
    EXPECT_EQ(planToJson(first.plan), planToJson(second.plan));
}

TEST(EvaluationTest, UnconnectedRequiredInputIdentifiesTheNode) {
    Document document = makeDocument({{"merge", "comp"}, {"output", "out"}});
    connect(rootGraph(document), "comp", "out");
    try {
        static_cast<void>(evaluateCpu(document, fullFrameRequest(document, 0)));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& e) {
        EXPECT_EQ(e.node, rootGraph(document).nodeByName("comp")->id);
        EXPECT_TRUE(e.hasNode());
    }
}

TEST(EvaluationTest, UnknownTypeHasNoFabricatedContractAndFailsExplicitly) {
    Document document = makeDocument({{"grail", "mystery"}, {"output", "out"}});
    const NodeInstance* mystery = rootGraph(document).nodeByName("mystery");
    ASSERT_NE(mystery, nullptr);
    EXPECT_FALSE(mystery->hasPortContract);
    EvaluationRequest request = fullFrameRequest(document, 0);
    request.output = mystery->id;
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, request)), EvaluationException);
}

TEST(EvaluationTest, RequestValidationRejectsUnsupportedChannelsAndQuality) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(document), "plate", "out");

    EvaluationRequest channels = fullFrameRequest(document, 0);
    channels.channels = "depth";
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, channels)), EvaluationException);

    EvaluationRequest quality = fullFrameRequest(document, 0);
    quality.quality = Quality::Draft;
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, quality)), EvaluationException);
}

TEST(RequestValidation, EnforcesEachDependencyCapabilityWithNodeContext) {
    const auto check = [](NodeCapabilities capabilities, const EvaluationRequest& request,
                          const std::string& expected) {
        auto catalog = std::make_shared<const NodeCatalog>(
            std::vector<NodeDescriptor>{capabilityFixture(std::move(capabilities))});
        Document document(catalog);
        const NodeId fixture = rootGraph(document).addNode("fixture.capability", "fixture");
        const NodeId output = rootGraph(document).addNode("output", "out");
        rootGraph(document).connect(PortRef{fixture, 0}, PortRef{output, 0});
        EvaluationRequest contextual = request;
        contextual.network = document.rootNetworkId();
        contextual.output = output;
        try {
            validateRequest(document, contextual);
            FAIL() << "expected capability rejection";
        } catch (const EvaluationException& error) {
            EXPECT_TRUE(error.hasNode());
            EXPECT_EQ(error.nodeName, "fixture");
            EXPECT_NE(std::string(error.what()).find(expected), std::string::npos);
        }
    };

    EvaluationRequest reduced;
    reduced.region = {0, 0, 2, 2};
    reduced.samplingScale = 2;
    check(NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"RGBA"}}, reduced,
          "sampling scale");

    EvaluationRequest rgba;
    rgba.region = {0, 0, 2, 2};
    check(NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"Y"}}, rgba,
          "channels");
    EvaluationRequest quality;
    quality.region = {0, 0, 2, 2};
    check(NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Draft}, .channels = {"RGBA"}}, quality,
          "quality");
    EvaluationRequest cropped;
    cropped.region = {1, 0, 2, 2};
    cropped.fullWidth = 4;
    cropped.fullHeight = 2;
    check(NodeCapabilities{.samplingScales = {1},
                           .qualityModes = {Quality::Full},
                           .channels = {"RGBA"},
                           .supportsRegion = false},
          cropped, "region-of-interest");
}

TEST(EvaluationTest, NestedSharedInstancesKeepScopedOverridesAndLazyInputs) {
    Document document;
    const NetworkId root = document.rootNetworkId();

    const NetworkId innerDefinition = document.addNetwork("inner");
    auto& inner = document.network(innerDefinition);
    const InterfacePortId innerInput = inner.addInput("in", PortKind::Image);
    const InterfacePortId innerOutput = inner.addOutput("out", PortKind::Image);
    const NodeId innerMerge = inner.graph().addNode("merge", "inner-merge");
    const NodeId innerBackground = inner.graph().addNode("constcolor", "inner-background");
    inner.graph().setParam(innerBackground, "color", ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}});
    inner.connectInput(innerInput, {innerMerge, 0});
    inner.graph().connect({innerBackground, 0}, {innerMerge, 1});
    inner.connectOutput({innerMerge, 0}, innerOutput);

    const NetworkId sharedDefinition = document.addNetwork("shared");
    auto& shared = document.network(sharedDefinition);
    const InterfacePortId unusedInput = shared.addInput("unused", PortKind::Image);
    const InterfacePortId sharedOutput = shared.addOutput("out", PortKind::Image);
    const NodeId color = shared.graph().addNode("constcolor", "color");
    const NetworkInstanceId innerInstance = document.addInstance(sharedDefinition, innerDefinition, "inner");
    document.bindInstanceInput(innerInstance, innerInput, {color, 0});
    shared.connectOutput({document.instance(innerInstance)->node, 0}, sharedOutput);

    const NetworkInstanceId first = document.addInstance(root, sharedDefinition, "first");
    const NetworkInstanceId second = document.addInstance(root, sharedDefinition, "second");
    document.setInstanceParam(first, color, "color", ColorValue{{1.0F, 0.0F, 0.0F, 1.0F}});
    document.setInstanceParam(second, color, "color", ColorValue{{0.0F, 1.0F, 0.0F, 1.0F}});

    auto& rootGraphRef = document.network(root).graph();
    const NodeId firstOutput = rootGraphRef.addNode("output", "first-output");
    const NodeId secondOutput = rootGraphRef.addNode("output", "second-output");
    rootGraphRef.connect({document.instance(first)->node, 0}, {firstOutput, 0});
    rootGraphRef.connect({document.instance(second)->node, 0}, {secondOutput, 0});
    const NodeId branches = rootGraphRef.addNode("merge", "branches");
    const NodeId combinedOutput = rootGraphRef.addNode("output", "combined-output");
    rootGraphRef.connect({document.instance(first)->node, 0}, {branches, 0});
    rootGraphRef.connect({document.instance(second)->node, 0}, {branches, 1});
    rootGraphRef.connect({branches, 0}, {combinedOutput, 0});

    const NodeId missingSource = rootGraphRef.addNode("source", "unused-missing");
    rootGraphRef.setParam(missingSource, "source", std::string("missing"));
    document.bindInstanceInput(first, unusedInput, {missingSource, 0});

    const auto nodesBefore = shared.graph().nodes().size();
    const auto edgesBefore = shared.graph().edges().size();
    ResultCache<CpuImage> cache;
    EvaluationRequest firstRequest;
    firstRequest.network = root;
    firstRequest.output = firstOutput;
    firstRequest.region = {0, 0, 2, 2};
    const CpuEvaluation firstEvaluation = evaluateCpu(document, firstRequest, &cache);
    const CacheCounts afterFirst = cache.counts();
    EvaluationRequest secondRequest = firstRequest;
    secondRequest.output = secondOutput;
    const CpuEvaluation secondEvaluation = evaluateCpu(document, secondRequest, &cache);
    EvaluationRequest combinedRequest = firstRequest;
    combinedRequest.output = combinedOutput;
    const CpuEvaluation combinedEvaluation = evaluateCpu(document, combinedRequest, &cache);

    EXPECT_EQ(firstEvaluation.image.pixel(0, 0), (std::array<float, 4>{1, 0, 0, 1}));
    EXPECT_EQ(secondEvaluation.image.pixel(0, 0), (std::array<float, 4>{0, 1, 0, 1}));
    EXPECT_NE(firstEvaluation.plan.result.contentHash, secondEvaluation.plan.result.contentHash);
    EXPECT_EQ(combinedEvaluation.image.pixel(0, 0), (std::array<float, 4>{0, 1, 0, 1}));
    bool firstBranchSeen = false;
    bool secondBranchSeen = false;
    for (const auto& step : combinedEvaluation.plan.steps) {
        if (step.name != "color")
            continue;
        if (step.path == std::vector<NetworkInstanceId>{first}) {
            EXPECT_EQ(std::get<ColorValue>(step.effectiveParams.at("color")).value,
                      (std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F}));
            firstBranchSeen = true;
        } else if (step.path == std::vector<NetworkInstanceId>{second}) {
            EXPECT_EQ(std::get<ColorValue>(step.effectiveParams.at("color")).value,
                      (std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F}));
            secondBranchSeen = true;
        }
    }
    EXPECT_TRUE(firstBranchSeen);
    EXPECT_TRUE(secondBranchSeen);
    EXPECT_GT(cache.counts().misses, afterFirst.misses);
    for (const auto& step : firstEvaluation.plan.steps)
        EXPECT_NE(step.name, "unused-missing");
    EXPECT_EQ(shared.graph().nodes().size(), nodesBefore);
    EXPECT_EQ(shared.graph().edges().size(), edgesBefore);
}
