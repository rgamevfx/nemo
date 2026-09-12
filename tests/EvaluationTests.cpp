#include <gtest/gtest.h>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/NativeEffects.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <algorithm>
#include <array>
#include <cmath>
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

// Acceptance for the interactive viewer: a request whose target is a
// processing node (not an Output) is valid and publishes that node's image.
TEST(EvaluationTest, NonOutputProcessingNodeIsAValidEvaluationTarget) {
    Document document =
        makeDocument({{"testpattern", "plate"}, {"constcolor", "backdrop"}, {"merge", "comp"}, {"output", "out"}});
    rootGraph(document).setParam(rootGraph(document).nodeByName("backdrop")->id, "color",
                                 ColorValue{{0.0F, 0.25F, 1.0F, 1.0F}});
    connect(rootGraph(document), "plate", "comp", 0, 0);
    connect(rootGraph(document), "backdrop", "comp", 0, 1);
    connect(rootGraph(document), "comp", "out");

    EvaluationRequest request = fullFrameRequest(document, 0);
    request.output = rootGraph(document).nodeByName("comp")->id;
    const CpuEvaluation evaluation = evaluateCpu(document, request);

    // Only the merge's own dependencies are scheduled; the Output node is not.
    ASSERT_EQ(evaluation.plan.steps.size(), 3u);
    EXPECT_EQ(evaluation.plan.result.contentHash, stepFor(evaluation.plan, "comp")->produced.contentHash);
    // The opaque backdrop wins the "over", so the viewer shows merge's image.
    const auto pixel = evaluation.image.pixel(4, 0);
    EXPECT_FLOAT_EQ(pixel[0], 0.0F);
    EXPECT_FLOAT_EQ(pixel[1], 0.25F);
    EXPECT_FLOAT_EQ(pixel[2], 1.0F);
    EXPECT_FLOAT_EQ(pixel[3], 1.0F);

    // The network's Output node still defines the consumption result.
    EvaluationRequest throughOutput = fullFrameRequest(document, 0);
    const CpuEvaluation published = evaluateCpu(document, throughOutput);
    EXPECT_EQ(published.plan.result.contentHash, stepFor(published.plan, "out")->produced.contentHash);
    EXPECT_EQ(published.plan.result.contentHash, evaluation.plan.result.contentHash);
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

TEST(EvaluationTest, AnimatedParametersReachCpuPlanAndPixelsWithoutMutatingDocument) {
    Document document = makeDocument({{"constcolor", "animated"}, {"output", "out"}});
    const NodeId animated = rootGraph(document).nodeByName("animated")->id;
    rootGraph(document).setParam(animated, "color", ColorValue{{0.0F, 0.0F, 0.0F, 1.0F}});
    connect(rootGraph(document), "animated", "out");

    const ParameterAddress address{document.rootNetworkId(), animated, "color", kInvalidNetworkInstance};
    CommandStack stack(document);
    stack.push(setKeyframesCommand({
        KeyframeEdit{address, Keyframe{0, 0.0, ColorValue{{1.0F, 0.0F, 0.0F, 1.0F}}}},
        KeyframeEdit{address, Keyframe{0, 2.0, ColorValue{{0.0F, 0.0F, 1.0F, 1.0F}}}},
    }));

    const EvaluationRequest request = fullFrameRequest(document, 1);
    const CpuEvaluation evaluation = evaluateCpu(document, request);
    const PlanStep* step = stepFor(evaluation.plan, "animated");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(std::get<ColorValue>(step->effectiveParams.at("color")).value,
              (std::array<float, 4>{0.5F, 0.0F, 0.5F, 1.0F}));
    EXPECT_EQ(evaluation.image.pixel(0, 0), (std::array<float, 4>{0.5F, 0.0F, 0.5F, 1.0F}));
    EXPECT_EQ(std::get<ColorValue>(rootGraph(document).node(animated)->params.at("color")).value,
              (std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F}));
}

TEST(EvaluationTest, NestedAnimationUsesDefinitionThenInstancePrecedence) {
    Document document;
    const NetworkId root = document.rootNetworkId();
    document.network(root).graph().removeNode(document.network(root).graph().nodeByName("Output")->id);
    const NetworkId definitionId = document.addNetwork("animated-definition");
    Network& definition = document.network(definitionId);
    const NodeId color = definition.graph().addNode("constcolor", "color");
    definition.graph().setParam(color, "color", ColorValue{{0.0F, 0.0F, 0.0F, 1.0F}});
    const InterfacePortId outputPort = definition.addOutput("out", PortKind::Image);
    definition.connectOutput({color, 0}, outputPort);

    const NetworkInstanceId occurrence = document.addInstance(root, definitionId, "occurrence");
    const NodeId output = document.network(root).graph().addNode("output", "out");
    document.network(root).graph().connect({document.instance(occurrence)->node, 0}, {output, 0});

    const ParameterAddress definitionAddress{definitionId, color, "color", kInvalidNetworkInstance};
    CommandStack stack(document);
    stack.push(setKeyframesCommand({
        KeyframeEdit{definitionAddress, Keyframe{0, 0.0, ColorValue{{1.0F, 0.0F, 0.0F, 1.0F}}}},
        KeyframeEdit{definitionAddress, Keyframe{0, 2.0, ColorValue{{0.0F, 0.0F, 1.0F, 1.0F}}}},
    }));

    const ParameterAddress instanceAddress{definitionId, color, "color", occurrence};
    EXPECT_EQ(animatedParameterValue(document, instanceAddress, 1.0),
              (ParameterValue{ColorValue{{0.5F, 0.0F, 0.5F, 1.0F}}}));

    const EvaluationRequest request = fullFrameRequest(document, 1);
    const CpuEvaluation definitionAnimation = evaluateCpu(document, request);
    EXPECT_EQ(definitionAnimation.image.pixel(0, 0), (std::array<float, 4>{0.5F, 0.0F, 0.5F, 1.0F}));

    stack.push(setInstanceParamCommand(occurrence, color, "color", ColorValue{{0.25F, 0.25F, 0.25F, 1.0F}}));
    EXPECT_EQ(animatedParameterValue(document, instanceAddress, 1.0),
              (ParameterValue{ColorValue{{0.25F, 0.25F, 0.25F, 1.0F}}}));
    EXPECT_EQ(evaluateCpu(document, request).image.pixel(0, 0), (std::array<float, 4>{0.25F, 0.25F, 0.25F, 1.0F}));

    stack.push(
        setKeyframesCommand({KeyframeEdit{instanceAddress, Keyframe{0, 0.0, ColorValue{{0.0F, 1.0F, 0.0F, 1.0F}}}},
                             KeyframeEdit{instanceAddress, Keyframe{0, 2.0, ColorValue{{0.0F, 1.0F, 0.0F, 1.0F}}}}}));
    const CpuEvaluation instanceAnimation = evaluateCpu(document, request);
    EXPECT_EQ(instanceAnimation.image.pixel(0, 0), (std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F}));

    const auto sibling = document.addInstance(root, definitionId, "sibling");
    const auto siblingOutput = document.network(root).graph().addNode("output", "sibling-out");
    document.network(root).graph().connect({document.instance(sibling)->node, 0}, {siblingOutput, 0});
    EvaluationRequest siblingRequest = request;
    siblingRequest.output = siblingOutput;
    EXPECT_EQ(evaluateCpu(document, siblingRequest).image.pixel(0, 0), (std::array<float, 4>{0.5F, 0.0F, 0.5F, 1.0F}));

    // An instance channel masks definition animation even if evaluating that
    // unused curve would overflow the parameter representation.
    stack.push(resetInstanceParamCommand(occurrence, color, "color"));
    auto maskedKey = document.animationChannel(definitionAddress)->keys.front();
    maskedKey.interpolation = KeyInterpolation::Bezier;
    maskedKey.tangentMode = TangentMode::Broken;
    maskedKey.outSlope[0] = 1e40;
    stack.push(setKeyframesCommand({{definitionAddress, maskedKey}}));
    EXPECT_EQ(animatedParameterValue(document, instanceAddress, 1.0),
              (ParameterValue{ColorValue{{0.0F, 1.0F, 0.0F, 1.0F}}}));
    EXPECT_EQ(evaluateCpu(document, request).image.pixel(0, 0), (std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F}));
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, siblingRequest)), GraphException);
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

// ---------------------------------------------------------------------------
// Native effect inventory (issue #34): Grade, Blur, Transform, and the shared
// mask/mix parameters. Expected pixels below are derived from the declared
// math (Foundry Grade controls, normalized separable Gaussian, Catmull-Rom),
// never from a previous run of the implementation.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] CpuImage solidImage(int width, int height, const std::array<float, 4>& color) {
    CpuImage image(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, color);
    return image;
}

[[nodiscard]] EvaluationRequest wholeRasterRequest(int width, int height) {
    EvaluationRequest request;
    request.network = 1;
    request.output = 1;
    request.region = {0, 0, width, height};
    request.fullWidth = width;
    request.fullHeight = height;
    return request;
}

// Drives the CPU pixel kernel directly with a synthetic raster: blur and
// transform need inputs no built-in generator produces.
[[nodiscard]] CpuImage applyNativeEffect(const char* type, ParameterValues params, const CpuImage& input,
                                         const CpuImage* mask = nullptr) {
    NodeInstance node;
    node.type = type;
    node.name = type;
    node.hasPortContract = true;
    node.params = std::move(params);
    return evaluateNativeEffect(builtinNodeCatalog(), node, node.params,
                                wholeRasterRequest(input.width(), input.height()), input, mask);
}

// Separable Gaussian weights for the declared blur: sigma = size/3, support
// ceil(size/scale), normalized once. Derived in double here so the pixel
// comparisons do not inherit the kernel's float rounding.
[[nodiscard]] std::vector<float> gaussianWeights(float size, float samplingScale = 1.0F) {
    const double sigma = static_cast<double>(size) / 3.0;
    const int support = static_cast<int>(std::ceil(static_cast<double>(size) / samplingScale));
    std::vector<double> weights(static_cast<std::size_t>(2 * support + 1));
    double total = 0.0;
    for (int i = -support; i <= support; ++i) {
        const double x = static_cast<double>(i) * samplingScale / sigma;
        weights[static_cast<std::size_t>(i + support)] = std::exp(-0.5 * x * x);
        total += weights[static_cast<std::size_t>(i + support)];
    }
    std::vector<float> result(weights.size());
    for (std::size_t i = 0; i < weights.size(); ++i)
        result[i] = static_cast<float>(weights[i] / total);
    return result;
}

// One declared Catmull-Rom (a = -0.5) tap weight `k` in [0, 3] for phase `t`.
[[nodiscard]] double catmullRomWeight(int k, double t) {
    switch (k) {
    case 0:
        return -0.5 * t * t * t + t * t - 0.5 * t;
    case 1:
        return 1.5 * t * t * t - 2.5 * t * t + 1.0;
    case 2:
        return -1.5 * t * t * t + 2.0 * t * t + 0.5 * t;
    default:
        return 0.5 * t * t * t - 0.5 * t * t;
    }
}

}  // namespace

TEST(NativeEffectTest, GradeForwardAppliesDeclaredControlsPerChannel) {
    Document document = makeDocument({{"constcolor", "plate"}, {"grade", "grade"}, {"output", "out"}});
    Graph& graph = rootGraph(document);
    const NodeId plate = graph.nodeByName("plate")->id;
    const NodeId grade = graph.nodeByName("grade")->id;
    graph.setParam(plate, "color", ColorValue{{0.25F, 0.25F, 0.25F, 0.5F}});
    // R exercises every control independently: with bp .125, wp .625,
    // lift .125, gain .625, multiply 2, offset .125 (and gamma 1) the ramp is
    // a = (gain-lift)*multiply/(wp-bp) = 2, b = lift+offset-bp*a = 0. G/B/A
    // keep their prior settings.
    graph.setParam(grade, "blackpoint", ColorValue{{0.125F, 0.0F, 0.0F, 0.0F}});
    graph.setParam(grade, "whitepoint", ColorValue{{0.625F, 1.0F, 1.0F, 1.0F}});
    graph.setParam(grade, "lift", ColorValue{{0.125F, 0.0F, 0.0F, 0.0F}});
    graph.setParam(grade, "gain", ColorValue{{0.625F, 1.0F, 1.0F, 2.0F}});
    graph.setParam(grade, "multiply", ColorValue{{2.0F, 1.0F, 1.0F, 1.0F}});
    graph.setParam(grade, "offset", ColorValue{{0.125F, 0.25F, 0.0F, 0.0F}});
    connect(graph, "plate", "grade");
    connect(graph, "grade", "out");

    // Animated gamma at an interpolated time: shared effective-parameter
    // resolution must reach the new native dispatch (frame 1 of the 2 -> 4
    // ramp is gamma 3 on G).
    const ParameterAddress gammaAddress{document.rootNetworkId(), grade, "gamma", kInvalidNetworkInstance};
    CommandStack stack(document);
    stack.push(setKeyframesCommand({
        KeyframeEdit{gammaAddress, Keyframe{0, 0.0, ColorValue{{1.0F, 2.0F, 1.0F, 1.0F}}}},
        KeyframeEdit{gammaAddress, Keyframe{0, 2.0, ColorValue{{1.0F, 4.0F, 1.0F, 1.0F}}}},
    }));

    const auto pixel = evaluateCpu(document, fullFrameRequest(document, 1, 2, 1)).image.pixel(0, 0);
    // R: a=2, b=0 => 2x. G: signedPow(x + 0.25, 1/3). B: x. The default RGB
    // channels choice leaves alpha untouched at 0.5.
    EXPECT_NEAR(pixel[0], 0.5F, 1e-6F);
    EXPECT_NEAR(pixel[1], std::pow(0.5F, 1.0F / 3.0F), 1e-5F);
    EXPECT_NEAR(pixel[2], 0.25F, 1e-6F);
    EXPECT_FLOAT_EQ(pixel[3], 0.5F);

    // Disabled channels are exact pass-through; None is the identity (frame 0
    // uses the authored keyframe gamma 2). The command push above publishes a
    // rebuilt document, so reacquire the graph instead of reusing the
    // pre-push reference.
    const EvaluationRequest request = fullFrameRequest(document, 0, 2, 1);
    const auto render = [&]() { return evaluateCpu(document, request).image.pixel(0, 0); };
    rootGraph(document).setParam(grade, "channels", ChoiceValue{"R"});
    EXPECT_EQ(render(), (std::array<float, 4>{0.5F, 0.25F, 0.25F, 0.5F}));
    rootGraph(document).setParam(grade, "channels", ChoiceValue{"Alpha"});
    EXPECT_EQ(render(), (std::array<float, 4>{0.25F, 0.25F, 0.25F, 1.0F}));
    rootGraph(document).setParam(grade, "channels", ChoiceValue{"None"});
    EXPECT_EQ(render(), (std::array<float, 4>{0.25F, 0.25F, 0.25F, 0.5F}));
}

TEST(NativeEffectTest, GradeReverseRoundTripsSignedHdrWithoutClamps) {
    Document document = makeDocument({{"constcolor", "plate"}, {"grade", "fwd"}, {"grade", "rev"}, {"output", "out"}});
    Graph& graph = rootGraph(document);
    const NodeId plate = graph.nodeByName("plate")->id;
    const NodeId forward = graph.nodeByName("fwd")->id;
    const NodeId reverse = graph.nodeByName("rev")->id;
    graph.setParam(plate, "color", ColorValue{{-3.0F, 0.25F, 7.0F, 1.0F}});
    for (const NodeId node : {forward, reverse}) {
        graph.setParam(node, "gain", ColorValue{{2.0F, 2.0F, 2.0F, 2.0F}});
        graph.setParam(node, "offset", ColorValue{{0.125F, 0.125F, 0.125F, 0.0F}});
        graph.setParam(node, "gamma", ColorValue{{1.0F, 2.0F, 1.0F, 1.0F}});
        graph.setParam(node, "clampBlack", false);
        graph.setParam(node, "clampWhite", false);
    }
    graph.setParam(reverse, "reverse", true);
    connect(graph, "plate", "fwd");
    connect(graph, "fwd", "rev");
    connect(graph, "rev", "out");

    EvaluationRequest forwardRequest = fullFrameRequest(document, 0, 2, 1);
    forwardRequest.output = forward;
    const auto hdr = evaluateCpu(document, forwardRequest).image.pixel(0, 0);
    // Clamps off: signed HDR values survive and the nonzero offset shifts the
    // ramp (R: 2x+0.125; G: signedPow(2x+0.125, 1/2); B: 2x+0.125).
    EXPECT_NEAR(hdr[0], -5.875F, 1e-5F);
    EXPECT_NEAR(hdr[1], std::sqrt(0.625F), 1e-5F);
    EXPECT_NEAR(hdr[2], 14.125F, 1e-5F);
    EXPECT_FLOAT_EQ(hdr[3], 1.0F);

    EvaluationRequest reverseRequest = fullFrameRequest(document, 0, 2, 1);
    reverseRequest.output = reverse;
    const auto roundTrip = evaluateCpu(document, reverseRequest).image.pixel(0, 0);
    EXPECT_NEAR(roundTrip[0], -3.0F, 1e-5F);
    EXPECT_NEAR(roundTrip[1], 0.25F, 1e-5F);
    EXPECT_NEAR(roundTrip[2], 7.0F, 1e-5F);
    EXPECT_FLOAT_EQ(roundTrip[3], 1.0F);
}

TEST(NativeEffectTest, GradeClampsAndRejectsInvalidCoefficients) {
    Document document = makeDocument({{"constcolor", "plate"}, {"grade", "grade"}, {"output", "out"}});
    Graph& graph = rootGraph(document);
    const NodeId plate = graph.nodeByName("plate")->id;
    const NodeId grade = graph.nodeByName("grade")->id;
    graph.setParam(plate, "color", ColorValue{{-0.5F, 0.5F, 1.5F, 1.0F}});
    graph.setParam(grade, "gain", ColorValue{{2.0F, 2.0F, 2.0F, 2.0F}});
    connect(graph, "plate", "grade");
    connect(graph, "grade", "out");
    const EvaluationRequest request = fullFrameRequest(document, 0, 1, 1);
    const auto render = [&]() { return evaluateCpu(document, request).image.pixel(0, 0); };

    // Foundry: black clamp sets output below 0 to 0, white clamp sets output
    // above 1 to 1; threshold is literal 0/1, not blackpoint/whitepoint.
    graph.setParam(grade, "clampBlack", true);
    graph.setParam(grade, "clampWhite", true);
    EXPECT_EQ(render(), (std::array<float, 4>{0.0F, 1.0F, 1.0F, 1.0F}));
    graph.setParam(grade, "clampBlack", false);
    graph.setParam(grade, "clampWhite", true);
    EXPECT_EQ(render(), (std::array<float, 4>{-1.0F, 1.0F, 1.0F, 1.0F}));
    graph.setParam(grade, "clampBlack", true);
    graph.setParam(grade, "clampWhite", false);
    EXPECT_EQ(render(), (std::array<float, 4>{0.0F, 1.0F, 3.0F, 1.0F}));
    graph.setParam(grade, "clampBlack", false);
    graph.setParam(grade, "clampWhite", false);
    EXPECT_EQ(render(), (std::array<float, 4>{-1.0F, 1.0F, 3.0F, 1.0F}));

    // Reverse needs an invertible ramp: a == 0 must be rejected.
    graph.setParam(grade, "reverse", true);
    graph.setParam(grade, "gain", ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}});
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, request)), EvaluationException);
    // Whitepoint == blackpoint is degenerate even forward.
    graph.setParam(grade, "reverse", false);
    graph.setParam(grade, "gain", ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}});
    graph.setParam(grade, "whitepoint", ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}});
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, request)), EvaluationException);
    // Gamma must be positive.
    graph.setParam(grade, "whitepoint", ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}});
    graph.setParam(grade, "gamma", ColorValue{{0.0F, 1.0F, 1.0F, 1.0F}});
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, request)), EvaluationException);
}

TEST(NativeEffectTest, EffectMaskSelectsChannelInvertsAndMixes) {
    Document document =
        makeDocument({{"constcolor", "plate"}, {"constcolor", "mask"}, {"grade", "grade"}, {"output", "out"}});
    Graph& graph = rootGraph(document);
    const NodeId plate = graph.nodeByName("plate")->id;
    const NodeId mask = graph.nodeByName("mask")->id;
    const NodeId grade = graph.nodeByName("grade")->id;
    graph.setParam(plate, "color", ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}});
    graph.setParam(mask, "color", ColorValue{{0.1F, 0.3F, 0.6F, 0.9F}});
    graph.setParam(grade, "gain", ColorValue{{2.0F, 2.0F, 2.0F, 2.0F}});
    connect(graph, "plate", "grade", 0, 0);
    connect(graph, "mask", "grade", 0, 1);  // Image output -> declared mask input
    connect(graph, "grade", "out");
    const EvaluationRequest request = fullFrameRequest(document, 0, 1, 1);
    const auto render = [&]() { return evaluateCpu(document, request).image.pixel(0, 0); };
    // Original 1.0, processed 2.0: output = original*(1 - w) + processed*w.

    graph.setParam(grade, "maskChannel", ChoiceValue{"A"});
    EXPECT_NEAR(render()[0], 1.9F, 1e-6F);
    graph.setParam(grade, "maskChannel", ChoiceValue{"R"});
    EXPECT_NEAR(render()[0], 1.1F, 1e-6F);
    graph.setParam(grade, "maskChannel", ChoiceValue{"G"});
    EXPECT_NEAR(render()[0], 1.3F, 1e-6F);
    graph.setParam(grade, "maskChannel", ChoiceValue{"B"});
    EXPECT_NEAR(render()[0], 1.6F, 1e-6F);
    graph.setParam(grade, "maskChannel", ChoiceValue{"A"});
    graph.setParam(grade, "invertMask", true);
    EXPECT_NEAR(render()[0], 1.1F, 1e-6F);
    graph.setParam(grade, "invertMask", false);
    graph.setParam(grade, "mix", 0.5);
    EXPECT_NEAR(render()[0], 1.45F, 1e-6F);
    graph.setParam(grade, "mix", 1.0);
    // No selected channel means full coverage, independent of inversion.
    graph.setParam(grade, "maskChannel", ChoiceValue{"none"});
    graph.setParam(grade, "invertMask", true);
    EXPECT_NEAR(render()[0], 2.0F, 1e-6F);
    // Coverage clamps the stored channel to [0, 1].
    graph.setParam(grade, "maskChannel", ChoiceValue{"A"});
    graph.setParam(grade, "invertMask", false);
    graph.setParam(mask, "color", ColorValue{{0.0F, 0.0F, 0.0F, 1.5F}});
    EXPECT_NEAR(render()[0], 2.0F, 1e-6F);
    graph.setParam(mask, "color", ColorValue{{0.0F, 0.0F, 0.0F, -0.5F}});
    EXPECT_NEAR(render()[0], 1.0F, 1e-6F);

    // The optional slot may be left unconnected: absent coverage is 1.
    Document unmasked = makeDocument({{"constcolor", "plate"}, {"grade", "grade"}, {"output", "out"}});
    Graph& unmaskedGraph = rootGraph(unmasked);
    const NodeId unmaskedGrade = unmaskedGraph.nodeByName("grade")->id;
    unmaskedGraph.setParam(unmaskedGraph.nodeByName("plate")->id, "color", ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}});
    unmaskedGraph.setParam(unmaskedGrade, "gain", ColorValue{{2.0F, 2.0F, 2.0F, 2.0F}});
    unmaskedGraph.setParam(unmaskedGrade, "invertMask", true);
    connect(unmaskedGraph, "plate", "grade");
    connect(unmaskedGraph, "grade", "out");
    EXPECT_NEAR(evaluateCpu(unmasked, fullFrameRequest(unmasked, 0, 1, 1)).image.pixel(0, 0)[0], 2.0F, 1e-6F);
}

TEST(NativeEffectTest, OptionalMaskSlotChangesResultIdentityAndCacheKey) {
    Document absent =
        makeDocument({{"constcolor", "plate"}, {"constcolor", "mask"}, {"grade", "grade"}, {"output", "out"}});
    Graph& absentGraph = rootGraph(absent);
    const NodeId absentGrade = absentGraph.nodeByName("grade")->id;
    absentGraph.setParam(absentGraph.nodeByName("plate")->id, "color", ColorValue{{0.5F, 0.25F, 0.75F, 1.0F}});
    absentGraph.setParam(absentGraph.nodeByName("mask")->id, "color", ColorValue{{0.0F, 0.0F, 0.0F, 0.25F}});
    absentGraph.setParam(absentGrade, "gain", ColorValue{{2.0F, 2.0F, 2.0F, 2.0F}});
    connect(absentGraph, "plate", "grade");
    connect(absentGraph, "grade", "out");

    Document connected = absent;
    connect(rootGraph(connected), "mask", "grade", 0, 1);

    const EvaluationRequest request = fullFrameRequest(absent, 0, 1, 1);
    ResultCache<CpuImage> cache;
    const CpuEvaluation absentEvaluation = evaluateCpu(absent, request, &cache);
    const CacheCounts afterAbsent = cache.counts();
    const CpuEvaluation connectedEvaluation = evaluateCpu(connected, request, &cache);

    const PlanStep* absentStep = stepFor(absentEvaluation.plan, "grade");
    const PlanStep* connectedStep = stepFor(connectedEvaluation.plan, "grade");
    ASSERT_NE(absentStep, nullptr);
    ASSERT_NE(connectedStep, nullptr);
    // Adding the mask changes the grade node's consumed input, so its result
    // identity changes and it must not be served from the absent-slot entry.
    EXPECT_NE(absentStep->produced.contentHash, connectedStep->produced.contentHash);
    EXPECT_EQ(absentEvaluation.image.pixel(0, 0)[0], 1.0F);
    // Original 0.5, processed 1.0, coverage 0.25 => 0.625.
    EXPECT_NEAR(connectedEvaluation.image.pixel(0, 0)[0], 0.625F, 1e-6F);
    EXPECT_GT(cache.counts().misses, afterAbsent.misses);
    // The unchanged upstream plate is still shared.
    EXPECT_GT(cache.counts().hits, afterAbsent.hits);
}

TEST(NativeEffectTest, BlurIsExactGaussianWithEdgeClampAndChannelSelection) {
    constexpr int kWidth = 9;
    CpuImage input = solidImage(kWidth, 1, {0.0F, 0.0F, 0.0F, 1.0F});
    input.setPixel(4, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    const ParameterValues rgb = {{"channels", ChoiceValue{"RGB"}}};
    ParameterValues impulseParams = rgb;
    impulseParams["size"] = 3.0;
    const CpuImage blurred = applyNativeEffect("blur", impulseParams, input);
    const std::vector<float> weights = gaussianWeights(3.0F);  // sigma 1, support 3
    ASSERT_EQ(weights.size(), 7u);
    for (int x = 0; x < kWidth; ++x) {
        const int tap = x - 4;
        const float expected = std::fabs(tap) <= 3 ? weights[static_cast<std::size_t>(tap + 3)] : 0.0F;
        EXPECT_NEAR(blurred.pixel(x, 0)[0], expected, 1e-5F) << "impulse x=" << x;
        // RGB filtering preserves the original alpha bit-exactly.
        EXPECT_FLOAT_EQ(blurred.pixel(x, 0)[3], 1.0F);
    }

    // An impulse on the border folds every negative tap onto the border pixel
    // (clamp-to-edge), which a zero-padded blur would miss.
    CpuImage edge = solidImage(5, 1, {0.0F, 0.0F, 0.0F, 1.0F});
    edge.setPixel(0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    ParameterValues edgeParams = rgb;
    edgeParams["size"] = 3.0;
    const CpuImage edgeBlurred = applyNativeEffect("blur", edgeParams, edge);
    for (int x = 0; x < 5; ++x) {
        float expected = 0.0F;
        for (int i = -3; i <= 3; ++i)
            if (std::clamp(x + i, 0, 4) == 0)
                expected += weights[static_cast<std::size_t>(i + 3)];
        EXPECT_NEAR(edgeBlurred.pixel(x, 0)[0], expected, 1e-5F) << "edge x=" << x;
    }

    // size 0 is an exact identity; Alpha filters alpha only, RGB filters RGB
    // only, so the untouched channels stay bit-exact.
    const CpuImage identity = applyNativeEffect("blur", ParameterValues{{"size", 0.0}}, input);
    for (int x = 0; x < kWidth; ++x)
        EXPECT_EQ(identity.pixel(x, 0), input.pixel(x, 0));
    CpuImage varying = solidImage(5, 1, {0.0F, 0.0F, 0.0F, 0.0F});
    for (int x = 0; x < 5; ++x)
        varying.setPixel(x, 0, {0.1F * x, 1.0F - 0.1F * x, 0.25F, 0.2F * x});
    ParameterValues alphaParams = {{"size", 2.0}, {"channels", ChoiceValue{"Alpha"}}};
    const CpuImage alphaOnly = applyNativeEffect("blur", alphaParams, varying);
    ParameterValues rgbParams = {{"size", 2.0}, {"channels", ChoiceValue{"RGB"}}};
    const CpuImage rgbOnly = applyNativeEffect("blur", rgbParams, varying);
    for (int x = 0; x < 5; ++x) {
        for (int c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(alphaOnly.pixel(x, 0)[c], varying.pixel(x, 0)[c]);
        EXPECT_FLOAT_EQ(rgbOnly.pixel(x, 0)[3], varying.pixel(x, 0)[3]);
    }

    // The shared mask weight applies to blur as well: half-coverage, full mix
    // blends the analytic blur with the original.
    CpuImage mask = solidImage(kWidth, 1, {0.0F, 0.0F, 0.0F, 0.5F});
    ParameterValues maskedParams = impulseParams;
    maskedParams["maskChannel"] = ChoiceValue{"A"};
    maskedParams["mix"] = 1.0;
    const CpuImage masked = applyNativeEffect("blur", maskedParams, input, &mask);
    for (int x = 0; x < kWidth; ++x) {
        const float expected = 0.5F * input.pixel(x, 0)[0] + 0.5F * blurred.pixel(x, 0)[0];
        EXPECT_NEAR(masked.pixel(x, 0)[0], expected, 1e-5F) << "masked blur x=" << x;
    }
}

TEST(NativeEffectTest, TransformSelectsFiltersAndMapsCoordinates) {
    // A six-pixel row with an impulse at index 4; output pixel 2 samples
    // input coordinate 3.0 and pixel 3 samples 4.0 (translateX -0.5).
    CpuImage input = solidImage(6, 1, {0.0F, 0.0F, 0.0F, 1.0F});
    input.setPixel(4, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    const auto render = [&](const char* filter, float translateX = -0.5F) {
        return applyNativeEffect("transform",
                                 ParameterValues{{"translateX", translateX},
                                                 {"scale", 1.0},
                                                 {"rotate", 0.0},
                                                 {"filter", ChoiceValue{filter}}},
                                 input);
    };
    const CpuImage nearest = render("Nearest");
    EXPECT_FLOAT_EQ(nearest.pixel(2, 0)[0], 0.0F);  // floor(3.0) selects the empty pixel 3
    EXPECT_FLOAT_EQ(nearest.pixel(3, 0)[0], 1.0F);  // floor(4.0) selects the impulse
    const CpuImage linear = render("Linear");
    EXPECT_NEAR(linear.pixel(2, 0)[0], 0.0F, 1e-6F);  // centers 2.5 and 3.5, both empty
    EXPECT_NEAR(linear.pixel(3, 0)[0], 0.5F, 1e-6F);  // midway between centers 3.5 and 4.5
    const CpuImage cubic = render("Cubic");
    constexpr double kPhase = 0.5;
    // Catmull-Rom a=-0.5: the impulse lands in the negative outer lobe for
    // pixel 2 and the positive second tap for pixel 3; neither is clamped.
    EXPECT_NEAR(cubic.pixel(2, 0)[0], catmullRomWeight(3, kPhase), 1e-6F);
    EXPECT_NEAR(cubic.pixel(3, 0)[0], catmullRomWeight(2, kPhase), 1e-6F);

    // Positive translateX moves content right and outside samples are
    // transparent black; the image center is the fixed pivot under scale.
    CpuImage row = solidImage(3, 1, {0.0F, 0.0F, 0.0F, 1.0F});
    row.setPixel(0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    row.setPixel(1, 0, {0.0F, 1.0F, 0.0F, 1.0F});
    row.setPixel(2, 0, {0.0F, 0.0F, 1.0F, 1.0F});
    const CpuImage shifted = applyNativeEffect(
        "transform",
        ParameterValues{{"translateX", 1.0}, {"scale", 1.0}, {"rotate", 0.0}, {"filter", ChoiceValue{"Nearest"}}}, row);
    EXPECT_EQ(shifted.pixel(0, 0), (std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F}));
    EXPECT_EQ(shifted.pixel(1, 0), row.pixel(0, 0));
    EXPECT_EQ(shifted.pixel(2, 0), row.pixel(1, 0));

    CpuImage square = solidImage(4, 4, {0.0F, 0.0F, 0.0F, 1.0F});
    square.setPixel(1, 1, {1.0F, 0.0F, 0.0F, 1.0F});
    square.setPixel(2, 2, {0.0F, 1.0F, 0.0F, 1.0F});
    const CpuImage zoomed =
        applyNativeEffect("transform", ParameterValues{{"scale", 2.0}, {"filter", ChoiceValue{"Nearest"}}}, square);
    EXPECT_EQ(zoomed.pixel(1, 1), square.pixel(1, 1));
    EXPECT_EQ(zoomed.pixel(2, 2), square.pixel(2, 2));
    EXPECT_EQ(zoomed.pixel(0, 0), square.pixel(1, 1));
    EXPECT_EQ(zoomed.pixel(3, 3), square.pixel(2, 2));

    // Positive angle is clockwise in the stored raster: a pixel north of
    // center lands east of center.
    CpuImage grid = solidImage(3, 3, {0.0F, 0.0F, 0.0F, 1.0F});
    grid.setPixel(1, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    const CpuImage rotated =
        applyNativeEffect("transform", ParameterValues{{"rotate", 90.0}, {"filter", ChoiceValue{"Nearest"}}}, grid);
    EXPECT_NEAR(rotated.pixel(2, 1)[0], 1.0F, 1e-5F);
    EXPECT_NEAR(rotated.pixel(0, 1)[0], 0.0F, 1e-5F);

    // Alpha-aware premultiplied interpolation: a 50/50 blend of opaque red
    // with a zero-alpha pixel stays red at half alpha rather than darkening.
    CpuImage pair = solidImage(2, 1, {0.0F, 0.0F, 0.0F, 0.0F});
    pair.setPixel(0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    pair.setPixel(1, 0, {0.0F, 1.0F, 0.0F, 0.0F});
    const CpuImage blended =
        applyNativeEffect("transform", ParameterValues{{"translateX", -0.5}, {"filter", ChoiceValue{"Linear"}}}, pair);
    EXPECT_NEAR(blended.pixel(0, 0)[0], 1.0F, 1e-5F);
    EXPECT_NEAR(blended.pixel(0, 0)[1], 0.0F, 1e-5F);
    EXPECT_NEAR(blended.pixel(0, 0)[3], 0.5F, 1e-5F);
    EXPECT_EQ(blended.pixel(1, 0), (std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F}));

    // The shared mask weight also applies to transform output.
    CpuImage mask = solidImage(6, 1, {0.0F, 0.0F, 0.0F, 0.5F});
    const CpuImage masked = applyNativeEffect("transform",
                                              ParameterValues{{"translateX", -1.0},
                                                              {"filter", ChoiceValue{"Nearest"}},
                                                              {"maskChannel", ChoiceValue{"A"}},
                                                              {"mix", 1.0}},
                                              input, &mask);
    for (int x = 0; x < 6; ++x) {
        const float processed = x + 1 < 6 ? input.pixel(x + 1, 0)[0] : 0.0F;
        EXPECT_NEAR(masked.pixel(x, 0)[0], 0.5F * input.pixel(x, 0)[0] + 0.5F * processed, 1e-6F)
            << "masked transform x=" << x;
    }
}
