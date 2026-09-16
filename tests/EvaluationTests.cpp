#include <gtest/gtest.h>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
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

Document makeDocument(const std::vector<std::pair<std::string, std::string>>& typeAndName, int width = 8,
                      int height = 4) {
    Document document;
    CommandStack stack(document);
    stack.push(setNetworkFormatCommand(document.rootNetworkId(), ImageFormat{width, height, 1.0F}));
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
    EXPECT_EQ(merge->inputs[0], plate->node);
    EXPECT_EQ(merge->inputs[1], backdrop->node);
    EXPECT_EQ(merge->inputImages[1].contentHash, backdrop->produced.contentHash);
    const auto pixel = evaluation.image.pixel(4, 0);
    EXPECT_NEAR(pixel[0], 0.5F * (4.0F / 7.0F), 1e-6F);
    EXPECT_NEAR(pixel[2], 0.5F * 1.0F, 1e-6F);
    // Effective parameter state records the resolved operation.
    EXPECT_EQ(std::get<ChoiceValue>(merge->effectiveParams.at("operation")).value, "over");
}

// ---------------------------------------------------------------------------
// Merge operations, masking and A/B roles (issue #75). Every expectation in
// this block is an independently calculated pixel oracle, not a comparison
// with another executor.
// ---------------------------------------------------------------------------

namespace {

// Distinct per-channel RGB *and* distinct background/foreground alpha, so a
// swapped A/B binding changes every operation's result.
constexpr std::array<float, 4> kMergeBackground{0.2F, 0.4F, 0.6F, 0.4F};
constexpr std::array<float, 4> kMergeForeground{0.8F, 0.5F, 0.25F, 0.5F};
constexpr std::array<float, 4> kMergeMask{0.1F, 0.2F, 0.3F, 0.3F};

// Background (port A) and foreground (port B) constant colors plus a
// constant mask available for the optional third port. `operation` is left
// unauthored when null, exercising the descriptor's default.
Document mergeDocument(const std::array<float, 4>& background, const std::array<float, 4>& foreground,
                       const char* operation = nullptr, const std::array<float, 4>& mask = kMergeMask) {
    Document document = makeDocument({{"constcolor", "background"},
                                      {"constcolor", "foreground"},
                                      {"constcolor", "mask"},
                                      {"merge", "comp"},
                                      {"output", "out"}});
    rootGraph(document).setParam(rootGraph(document).nodeByName("background")->id, "color", ColorValue{background});
    rootGraph(document).setParam(rootGraph(document).nodeByName("foreground")->id, "color", ColorValue{foreground});
    rootGraph(document).setParam(rootGraph(document).nodeByName("mask")->id, "color", ColorValue{mask});
    if (operation != nullptr) {
        rootGraph(document).setParam(rootGraph(document).nodeByName("comp")->id, "operation",
                                     ChoiceValue{std::string{operation}});
    }
    connect(rootGraph(document), "background", "comp", 0, 0);
    connect(rootGraph(document), "foreground", "comp", 0, 1);
    connect(rootGraph(document), "comp", "out");
    return document;
}

void setMergeParam(Document& document, const char* key, ParameterValue value) {
    rootGraph(document).setParam(rootGraph(document).nodeByName("comp")->id, key, std::move(value));
}

[[nodiscard]] std::array<float, 4> mergePixel(const Document& document) {
    return evaluateCpu(document, fullFrameRequest(document, 0)).image.pixel(0, 0);
}

}  // namespace

// Issue specification story 33/40: the five operations form the documented
// blend target and interpolate by foreground alpha; alpha is
// operation-independent. The unmasked oracle is the issue's hand-computed
// table (background 0.2/0.4/0.6 with alpha 0.4, foreground 0.8/0.5/0.25 with
// alpha 0.5).
TEST(EvaluationTest, MergeOperationsMatchTheIndependentOracle) {
    struct Case {
        const char* operation;
        std::array<float, 3> rgb;
    };
    const Case cases[] = {
        {"over", {0.5F, 0.45F, 0.425F}},   {"plus", {0.6F, 0.65F, 0.725F}},       {"multiply", {0.18F, 0.3F, 0.375F}},
        {"screen", {0.52F, 0.55F, 0.65F}}, {"difference", {0.4F, 0.25F, 0.475F}},
    };
    for (const Case& expected : cases) {
        const Document document = mergeDocument(kMergeBackground, kMergeForeground, expected.operation);
        const std::array<float, 4> pixel = mergePixel(document);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            EXPECT_NEAR(pixel[channel], expected.rgb[channel], 1e-6F) << expected.operation << " channel " << channel;
        }
        // 0.5 + (1 - 0.5) * 0.4, the same alpha for every operation.
        EXPECT_NEAR(pixel[3], 0.7F, 1e-6F) << expected.operation;
    }

    // An unauthored document defaults to Over and keeps the historical
    // expression exactly (same operand order), not merely a close value.
    const Document defaulted = mergeDocument(kMergeBackground, kMergeForeground);
    const std::array<float, 4> pixel = mergePixel(defaulted);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float legacy =
            kMergeForeground[3] * kMergeForeground[channel] + (1.0F - kMergeForeground[3]) * kMergeBackground[channel];
        EXPECT_EQ(pixel[channel], legacy);
    }
    EXPECT_EQ(pixel[3], kMergeForeground[3] + (1.0F - kMergeForeground[3]) * kMergeBackground[3]);
}

// Story 40: scene-linear RGB is never clamped, for opaque or translucent
// foregrounds and for negative background values.
TEST(EvaluationTest, MergePreservesHdrAndNegativeRgbWithoutClamping) {
    const std::array<float, 4> background{-0.5F, 2.0F, -2.0F, 1.0F};
    const std::array<float, 4> foreground{2.5F, -1.0F, 0.5F, 1.0F};
    // Opaque foreground: Over selects the foreground exactly, above one and
    // below zero.
    EXPECT_EQ(mergePixel(mergeDocument(background, foreground, "over")),
              (std::array<float, 4>{2.5F, -1.0F, 0.5F, 1.0F}));
    EXPECT_EQ(mergePixel(mergeDocument(background, foreground, "multiply")),
              (std::array<float, 4>{-1.25F, -2.0F, -1.0F, 1.0F}));
    EXPECT_EQ(mergePixel(mergeDocument(background, foreground, "plus")),
              (std::array<float, 4>{2.0F, 1.0F, -1.5F, 1.0F}));
    EXPECT_EQ(mergePixel(mergeDocument(background, foreground, "difference")),
              (std::array<float, 4>{3.0F, 3.0F, 2.5F, 1.0F}));

    // Translucent foreground (alpha 0.25) keeps the interpolated negative
    // channel (0.25*0.5 + 0.75*-2 = -1.375) and needs no clamp to survive.
    const std::array<float, 4> translucentBackground{-0.5F, 2.0F, -2.0F, 0.5F};
    const std::array<float, 4> partial{2.5F, -1.0F, 0.5F, 0.25F};
    const std::array<float, 4> translucent = mergePixel(mergeDocument(translucentBackground, partial, "over"));
    EXPECT_FLOAT_EQ(translucent[0], 0.25F);
    EXPECT_FLOAT_EQ(translucent[1], 1.25F);
    EXPECT_FLOAT_EQ(translucent[2], -1.375F);
    EXPECT_FLOAT_EQ(translucent[3], 0.625F);
}

// Stories 36-39/41: the optional mask follows the shared mask contract, Mix
// works with no mask connected, and zero coverage or zero Mix returns the
// background exactly.
TEST(EvaluationTest, MergeMaskChannelInvertAndMixFollowTheSharedContract) {
    Document document = mergeDocument(kMergeBackground, kMergeForeground, "over");

    // Mix with no mask connected: half of the Over composite.
    setMergeParam(document, "mix", ParameterValue{0.5});
    std::array<float, 4> pixel = mergePixel(document);
    EXPECT_NEAR(pixel[0], 0.35F, 1e-6F);
    EXPECT_NEAR(pixel[1], 0.425F, 1e-6F);
    EXPECT_NEAR(pixel[2], 0.5125F, 1e-6F);
    EXPECT_NEAR(pixel[3], 0.55F, 1e-6F);

    // Zero Mix returns the background exactly, mask or no mask.
    setMergeParam(document, "mix", ParameterValue{0.0});
    EXPECT_EQ(mergePixel(document), kMergeBackground);

    // Absent mask (or channel none) gives full coverage regardless of Mix.
    connect(rootGraph(document), "mask", "comp", 0, 2);
    setMergeParam(document, "mix", ParameterValue{1.0});
    setMergeParam(document, "maskChannel", ParameterValue{ChoiceValue{"none"}});
    EXPECT_NEAR(mergePixel(document)[1], 0.45F, 1e-6F);
    setMergeParam(document, "maskChannel", ParameterValue{ChoiceValue{"A"}});

    // Fractional coverage: mask A = 0.3, Mix 0.5 -> weight 0.15, the issue's
    // hand-computed masked Over (0.245, 0.4075, 0.57375, 0.445).
    setMergeParam(document, "mix", ParameterValue{0.5});
    pixel = mergePixel(document);
    EXPECT_NEAR(pixel[0], 0.245F, 1e-6F);
    EXPECT_NEAR(pixel[1], 0.4075F, 1e-6F);
    EXPECT_NEAR(pixel[2], 0.57375F, 1e-6F);
    EXPECT_NEAR(pixel[3], 0.445F, 1e-6F);

    // Invert flips the coverage to 0.7 (weight 0.35); the channel choices
    // select the stored channel (R = 0.1, G = 0.2), so the results differ
    // from the alpha-masked one above.
    setMergeParam(document, "invertMask", ParameterValue{true});
    pixel = mergePixel(document);
    EXPECT_NEAR(pixel[0], 0.305F, 1e-6F);
    EXPECT_NEAR(pixel[3], 0.505F, 1e-6F);
    setMergeParam(document, "invertMask", ParameterValue{false});
    setMergeParam(document, "maskChannel", ParameterValue{ChoiceValue{"R"}});
    pixel = mergePixel(document);
    EXPECT_NEAR(pixel[0], 0.215F, 1e-6F);
    EXPECT_NEAR(pixel[3], 0.415F, 1e-6F);
    setMergeParam(document, "maskChannel", ParameterValue{ChoiceValue{"G"}});
    pixel = mergePixel(document);
    EXPECT_NEAR(pixel[0], 0.23F, 1e-6F);
    EXPECT_NEAR(pixel[3], 0.43F, 1e-6F);

    // The selected channel is clamped to [0, 1] before inversion: an HDR
    // mask selects full coverage, a negative one zero coverage.
    setMergeParam(document, "maskChannel", ParameterValue{ChoiceValue{"A"}});
    setMergeParam(document, "mix", ParameterValue{1.0});
    rootGraph(document).setParam(rootGraph(document).nodeByName("mask")->id, "color",
                                 ColorValue{{0.0F, 0.0F, 0.0F, 3.0F}});
    pixel = mergePixel(document);
    EXPECT_NEAR(pixel[0], 0.5F, 1e-6F);
    EXPECT_NEAR(pixel[1], 0.45F, 1e-6F);
    EXPECT_NEAR(pixel[2], 0.425F, 1e-6F);
    EXPECT_NEAR(pixel[3], 0.7F, 1e-6F);
    rootGraph(document).setParam(rootGraph(document).nodeByName("mask")->id, "color",
                                 ColorValue{{0.0F, 0.0F, 0.0F, -2.0F}});
    EXPECT_EQ(mergePixel(document), kMergeBackground);

    // Zero coverage returns the background even with full Mix.
    setMergeParam(document, "mix", ParameterValue{0.0});
    EXPECT_EQ(mergePixel(document), kMergeBackground);
}

// Story 34: A is the background and B the foreground, in both directions.
// The swapped oracle recomputes the same formulas with the roles exchanged;
// the alpha formula is symmetric, so the RGB differences are the evidence.
TEST(EvaluationTest, MergeKeepsPortRolesInBothDirections) {
    struct Case {
        const char* operation;
        std::array<float, 3> authored;
        std::array<float, 3> swapped;
    };
    const Case cases[] = {
        {"over", {0.5F, 0.45F, 0.425F}, {0.56F, 0.46F, 0.39F}},
        {"plus", {0.6F, 0.65F, 0.725F}, {0.88F, 0.66F, 0.49F}},
        {"multiply", {0.18F, 0.3F, 0.375F}, {0.544F, 0.38F, 0.21F}},
        {"screen", {0.52F, 0.55F, 0.65F}, {0.816F, 0.58F, 0.43F}},
        {"difference", {0.4F, 0.25F, 0.475F}, {0.72F, 0.34F, 0.29F}},
    };
    for (const Case& expected : cases) {
        const std::array<float, 4> authored =
            mergePixel(mergeDocument(kMergeBackground, kMergeForeground, expected.operation));
        const std::array<float, 4> swapped =
            mergePixel(mergeDocument(kMergeForeground, kMergeBackground, expected.operation));
        for (std::size_t channel = 0; channel < 3; ++channel) {
            EXPECT_NEAR(authored[channel], expected.authored[channel], 1e-6F)
                << expected.operation << " authored port order";
            EXPECT_NEAR(swapped[channel], expected.swapped[channel], 1e-6F)
                << expected.operation << " swapped port order";
        }
        EXPECT_NEAR(authored[3], 0.7F, 1e-6F);
        EXPECT_NEAR(swapped[3], 0.7F, 1e-6F);
    }
}

// Story 35: the atomic swap is the same edit the renderer sees — one command,
// one history entry, and the rendered sources exchange.
TEST(EvaluationTest, SwapInputsCommandExchangesTheRenderedSources) {
    Document document = mergeDocument(kMergeBackground, kMergeForeground, "multiply");
    EXPECT_NEAR(mergePixel(document)[0], 0.18F, 1e-6F);
    CommandStack history(document);
    history.push(swapInputsCommand(document.rootNetworkId(), rootGraph(document).nodeByName("comp")->id, 0, 1));
    ASSERT_EQ(history.depth(), 1u);
    EXPECT_NEAR(mergePixel(document)[0], 0.544F, 1e-6F);
    ASSERT_TRUE(history.undo());
    EXPECT_NEAR(mergePixel(document)[0], 0.18F, 1e-6F);
}

// Story 42: an unsupported operation value fails explicitly instead of
// silently falling back. The descriptor rejects it on authoring/deserialize;
// the registered implementation's typed interpretation rejects any value a
// document catalog could still admit, and never selects a fallback.
TEST(EvaluationTest, MergeUnknownOperationFailsExplicitlyInsteadOfFallingBack) {
    // The descriptor rejects an unknown value on authoring.
    Document document;
    const NodeId merge = rootGraph(document).addNode("merge", "comp");
    EXPECT_THROW(rootGraph(document).setParam(merge, "operation", ParameterValue{ChoiceValue{"average"}}),
                 GraphException);

    // The registered interpretation rejects any value a document catalog could
    // still admit (a fixture may declare a wider vocabulary), naming the
    // supported set instead of selecting a fallback.
    NodeInstance node;
    node.type = "merge";
    node.name = "comp";
    node.params.emplace("operation", ParameterValue{ChoiceValue{"average"}});
    ParameterValues effective = node.params;

    const auto problem = builtinNodeContributions()->validateParameters(builtinNodeCatalog(), node, effective);
    ASSERT_TRUE(problem.has_value());
    EXPECT_NE(problem->find("over, plus, multiply, screen, difference"), std::string::npos) << *problem;
    EXPECT_NE(problem->find("average"), std::string::npos) << *problem;
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
    channels.channels = {"depth"};
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, channels)), EvaluationException);

    EvaluationRequest quality = fullFrameRequest(document, 0);
    quality.quality = Quality::Draft;
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, quality)), EvaluationException);
}

TEST(RequestValidation, EnforcesEachDependencyCapabilityWithNodeContext) {
    const auto check = [](NodeCapabilities capabilities, const EvaluationRequest& request,
                          const std::string& expected) {
        auto catalog = std::make_shared<const NodeCatalog>(
            extendedBuiltinSchema(std::vector<NodeDescriptor>{capabilityFixture(std::move(capabilities))}));
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
    rgba.channels = {"R", "G", "B", "A"};
    check(NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"Y"}}, rgba,
          "channels");
    EvaluationRequest quality;
    quality.region = {0, 0, 2, 2};
    check(NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Draft}, .channels = {"RGBA"}}, quality,
          "quality");
    // A declared lack of region support is deliberately NOT one of these
    // rejections (issue #85): such a contribution is processed over the whole
    // image domain internally, and its consumers still receive their region.
    // That behavior is verified by rendering, not by a message.
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

// Drives the registered CPU pixel adapter directly with a synthetic raster:
// blur and transform need inputs no built-in generator produces.
[[nodiscard]] CpuImage applyNativeEffect(const char* type, ParameterValues params, const CpuImage& input,
                                         const CpuImage* mask = nullptr) {
    NodeInstance node;
    node.type = type;
    node.name = type;
    node.hasPortContract = true;
    node.params = std::move(params);
    const auto contributions = builtinNodeContributions();
    const NodeContribution* contribution = contributions->find(type);
    if (contribution == nullptr || !contribution->cpu)
        throw EvaluationException(std::string{"node type '"} + type + "' has no CPU implementation");
    const NodeCatalog& catalog = builtinNodeCatalog();
    const Document document(builtinNodeCatalogPtr());
    const EvaluationRequest request = wholeRasterRequest(input.width(), input.height());
    const std::array<const CpuImage*, 2> inputs{&input, mask};
    const std::span<const CpuImage* const> contextInputs(inputs.data(), inputs.size());
    const ImageDescription inputDescription{.format = request.region,
                                            .dataBounds = request.region,
                                            .pixelAspect = input.layout().pixelAspect,
                                            .color = input.layout().color};
    const ImageDescription maskDescription{.format = mask ? Region{0, 0, mask->width(), mask->height()} : Region{},
                                           .dataBounds = mask ? Region{0, 0, mask->width(), mask->height()} : Region{}};
    const std::array<const ImageDescription*, 2> descriptions{&inputDescription, mask ? &maskDescription : nullptr};
    const std::array<EvaluationRequest, 2> inputRequests{
        request, mask ? wholeRasterRequest(mask->width(), mask->height()) : EvaluationRequest{}};
    const ImageDescription description =
        contribution->describe
            ? contribution->describe(NodeDescriptionContext{document, catalog, node, 0, descriptions, inputDescription})
            : inputDescription;
    const CpuNodeContext context{document, catalog,       node,        request, node.params, contextInputs,
                                 nullptr,  inputRequests, description, nullptr, descriptions};
    return contribution->cpu->execute(context);
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

    // The archived 0.1..3 range is slider travel, not the equation domain: a
    // typed scale of 4 samples the expanded positive domain around the center
    // (4x4 source, pivot 1.5: dst 0 -> 1.125 rounds to 1, dst 3 -> 1.875 to 2).
    const CpuImage wide =
        applyNativeEffect("transform", ParameterValues{{"scale", 4.0}, {"filter", ChoiceValue{"Nearest"}}}, square);
    EXPECT_EQ(wide.pixel(0, 0), square.pixel(1, 1));
    EXPECT_EQ(wide.pixel(1, 1), square.pixel(1, 1));
    EXPECT_EQ(wide.pixel(2, 2), square.pixel(2, 2));
    EXPECT_EQ(wide.pixel(3, 3), square.pixel(2, 2));
    // A tiny positive scale whose reciprocal overflows has no representable
    // mapping and is rejected by the admissibility owner, as is zero.
    EXPECT_THROW(static_cast<void>(applyNativeEffect(
                     "transform", ParameterValues{{"scale", 1.0e-45}, {"filter", ChoiceValue{"Nearest"}}}, square)),
                 EvaluationException);
    EXPECT_THROW(static_cast<void>(applyNativeEffect(
                     "transform", ParameterValues{{"scale", 0.0}, {"filter", ChoiceValue{"Nearest"}}}, square)),
                 EvaluationException);

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

// ---------------------------------------------------------------------------
// Dependency-region evaluation (issue #85). A region-limited request renders the
// same samples as the matching window of the whole-image request, through the
// chains that need real dependency coverage: pointwise effects, Blur halos,
// Transform inverse bounds and filter footprint, masks and Mix, and a
// contribution that declares it can only process whole images.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] EvaluationRequest windowRequest(const Document& document, const std::string& outputName, Region region,
                                              int samplingScale, int domainWidth, int domainHeight) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = rootGraph(document).nodeByName(outputName)->id;
    request.region = region;
    request.samplingScale = samplingScale;
    request.fullWidth = domainWidth;
    request.fullHeight = domainHeight;
    return request;
}

// The region-limited raster must equal the matching window of the whole-image
// raster: same samples, same sampling lattice, same origin semantics.
void expectWindowMatchesWholeFrame(const CpuImage& window, const CpuImage& whole, Region normalized, int scale) {
    ASSERT_EQ(window.width(), scaledDimension(normalized.width, scale));
    ASSERT_EQ(window.height(), scaledDimension(normalized.height, scale));
    const int offsetX = normalized.x / scale;
    const int offsetY = normalized.y / scale;
    for (int y = 0; y < window.height(); ++y) {
        for (int x = 0; x < window.width(); ++x) {
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                EXPECT_FLOAT_EQ(window.pixel(x, y)[channel], whole.pixel(offsetX + x, offsetY + y)[channel])
                    << "sample (" << x << "," << y << ") channel " << channel;
            }
        }
    }
}

// Coverage satisfies the consumer's request on the common sampling lattice;
// raster padding never rewrites the authored logical format.
void expectCoverageInvariants(const EvaluationPlan& plan, Region requested, int scale, int domainWidth,
                              int domainHeight) {
    EXPECT_EQ(plan.request.region, regionOnLattice(requested, scale));
    EXPECT_EQ(plan.description.format, (Region{0, 0, domainWidth, domainHeight}));
    for (const PlanStep& step : plan.steps) {
        EXPECT_TRUE(regionContains(step.region, requested)) << "step " << step.name;
        EXPECT_EQ(step.region.x % scale, 0) << "step " << step.name;
        EXPECT_EQ(step.region.y % scale, 0) << "step " << step.name;
    }
}

// The TestPattern's declared math, derived here independently of any evaluator:
// R is a horizontal gradient over the full-resolution domain, G a vertical one,
// B a time-positioned bar, A opaque.
[[nodiscard]] std::array<float, 4> testPatternPixel(int fullX, int fullY, int fullWidth, int fullHeight) {
    const double u = fullWidth > 1 ? static_cast<double>(fullX) / (fullWidth - 1) : 0.0;
    const double v = fullHeight > 1 ? static_cast<double>(fullY) / (fullHeight - 1) : 0.0;
    const int barWidth = std::max(2, fullWidth / 16);
    const bool inBar = fullX >= 0 && fullX < barWidth;  // frame 0 puts the bar at x 0
    return {static_cast<float>(u), static_cast<float>(v), inBar ? 1.0F : 0.0F, 1.0F};
}

// A whole-frame-only contribution (supportsRegion=false) with a real CPU
// implementation: it can only produce the whole image, and its pixels depend on
// the whole frame's statistics, so a region-local evaluation could not
// reproduce them. Registering it proves the planner escalates such a node and
// its inputs instead of feeding it a partial region.
NodeDescriptor wholeFrameFixtureDescriptor() {
    return NodeDescriptor{.type = "fixture.wholeframe",
                          .displayName = "Whole Frame Fixture",
                          .group = "Tests",
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "image", false}},
                          .outputs = {{PortKind::Image, "out"}},
                          .capabilities = NodeCapabilities{.samplingScales = {1},
                                                           .qualityModes = {Quality::Full},
                                                           .channels = {"RGBA"},
                                                           .supportsRegion = false}};
}

CpuImage executeWholeFrameFixture(const CpuNodeContext& context) {
    const EvaluationRequest& request = context.request;
    if (request.region.x != 0 || request.region.y != 0 || request.region.width != request.imageWidth() ||
        request.region.height != request.imageHeight()) {
        failNode(context.node, "whole-frame implementation received a partial region");
    }
    const CpuImage& input = requiredImageInput(context, 0, "whole-frame fixture requires a connected input");
    double total = 0.0;
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); ++x)
            total += static_cast<double>(input.pixel(x, y)[0]);
    const float mean = static_cast<float>(total / (static_cast<double>(input.width()) * input.height()));
    CpuImage output(effectRasterLayout(context));
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            const std::array<float, kImageChannels> pixel = input.pixel(x, y);
            output.setPixel(x, y, {pixel[0] - mean + 0.5F, pixel[1], pixel[2], pixel[3]});
        }
    }
    return output;
}

}  // namespace

TEST(RegionalEvaluationTest, PointwiseChainRegionMatchesWholeFrameWindow) {
    Document document = makeDocument({{"testpattern", "plate"}, {"grade", "grade"}, {"output", "out"}}, 320, 160);
    Graph& graph = rootGraph(document);
    graph.setParam(graph.nodeByName("grade")->id, "gain", ColorValue{{1.5F, 1.5F, 1.5F, 2.0F}});
    connect(graph, "plate", "grade");
    connect(graph, "grade", "out");

    const Region region{80, 40, 40, 30};
    const auto wholeFrame = evaluateCpu(document, windowRequest(document, "out", {0, 0, 320, 160}, 1, 320, 160));
    const auto cropped = evaluateCpu(document, windowRequest(document, "out", region, 1, 320, 160));

    expectCoverageInvariants(cropped.plan, region, 1, 320, 160);
    expectWindowMatchesWholeFrame(cropped.image, wholeFrame.image, region, 1);
    // The pointwise dependency needs exactly the demanded coverage: no rule can
    // widen it, so the plate is planned at the padded block, not the whole frame.
    const PlanStep* plate = stepFor(cropped.plan, "plate");
    ASSERT_NE(plate, nullptr);
    EXPECT_NE(plate->region, (Region{0, 0, 320, 160}));
}

TEST(RegionalEvaluationTest, OddOriginRegionsMatchWholeFrameAtReducedScales) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}}, 320, 160);
    connect(rootGraph(document), "plate", "out");

    for (const int scale : {2, 4}) {
        // An origin that is not a multiple of the sampling scale is rounded
        // outward to the enclosing lattice cell, never inward.
        const Region requested{77, 41, 50, 26};
        const Region normalized = regionOnLattice(requested, scale);
        const auto wholeFrame =
            evaluateCpu(document, windowRequest(document, "out", {0, 0, 320, 160}, scale, 320, 160));
        const auto cropped = evaluateCpu(document, windowRequest(document, "out", requested, scale, 320, 160));

        EXPECT_EQ(cropped.plan.request.samplingScale, scale);
        expectCoverageInvariants(cropped.plan, requested, scale, 320, 160);
        expectWindowMatchesWholeFrame(cropped.image, wholeFrame.image, normalized, scale);
    }
}

TEST(RegionalEvaluationTest, ChainedBlurAndTransformWithMaskAndMixMatchWholeFrame) {
    Document document = makeDocument({{"testpattern", "plate"},
                                      {"testpattern", "mask"},
                                      {"blur", "soften"},
                                      {"transform", "move"},
                                      {"output", "out"}},
                                     320, 160);
    Graph& graph = rootGraph(document);
    graph.setParam(graph.nodeByName("soften")->id, "size", 5.0);
    graph.setParam(graph.nodeByName("move")->id, "translateX", 3.5);
    graph.setParam(graph.nodeByName("move")->id, "translateY", -2.25);
    graph.setParam(graph.nodeByName("move")->id, "rotate", 7.0);
    graph.setParam(graph.nodeByName("move")->id, "filter", ChoiceValue{"Cubic"});
    graph.setParam(graph.nodeByName("move")->id, "maskChannel", ChoiceValue{"G"});
    graph.setParam(graph.nodeByName("move")->id, "mix", 0.6);
    connect(graph, "plate", "soften", 0, 0);
    connect(graph, "soften", "move", 0, 0);
    connect(graph, "mask", "move", 0, 1);
    connect(graph, "move", "out");

    const Region region{101, 47, 60, 40};
    const auto wholeFrame = evaluateCpu(document, windowRequest(document, "out", {0, 0, 320, 160}, 1, 320, 160));
    const auto cropped = evaluateCpu(document, windowRequest(document, "out", region, 1, 320, 160));

    expectCoverageInvariants(cropped.plan, region, 1, 320, 160);
    expectWindowMatchesWholeFrame(cropped.image, wholeFrame.image, region, 1);

    // The rules are visible in the plan: the plate must cover the blur halo and
    // the transform's inverse-mapped footprint, so its coverage reaches strictly
    // beyond the requested window on every side.
    const PlanStep* plate = stepFor(cropped.plan, "plate");
    const PlanStep* soften = stepFor(cropped.plan, "soften");
    ASSERT_NE(plate, nullptr);
    ASSERT_NE(soften, nullptr);
    EXPECT_TRUE(regionContains(plate->region, soften->region));
    EXPECT_LT(plate->region.x, region.x);
    EXPECT_LT(plate->region.y, region.y);
    EXPECT_GT(plate->region.x + plate->region.width, region.x + region.width);
    EXPECT_GT(plate->region.y + plate->region.height, region.y + region.height);
    // The mask is read at the output coordinates: it needs nothing beyond the
    // transform's own region.
    const PlanStep* mask = stepFor(cropped.plan, "mask");
    ASSERT_NE(mask, nullptr);
    EXPECT_TRUE(regionContains(mask->region, region));
}

TEST(RegionalEvaluationTest, UnrepresentableTransformGeometryFailsByNodeInsteadOfInventingBounds) {
    Document document = makeDocument({{"testpattern", "plate"}, {"transform", "move"}, {"output", "out"}});
    Graph& graph = rootGraph(document);
    graph.setParam(graph.nodeByName("move")->id, "scale", 1e-20);
    graph.setParam(graph.nodeByName("move")->id, "translateX", 1e30);
    connect(graph, "plate", "move");
    connect(graph, "move", "out");
    try {
        static_cast<void>(evaluateCpu(document, windowRequest(document, "out", {80, 40, 40, 30}, 1, 320, 160)));
        FAIL() << "unrepresentable geometry must not produce fabricated bounds";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, graph.nodeByName("move")->id);
        EXPECT_NE(std::string(error.what()).find("representable image coordinates"), std::string::npos);
    }
}

TEST(RegionalEvaluationTest, BlurRegionMatchesDeclaredGaussianIncludingImageBorders) {
    Document document = makeDocument({{"testpattern", "plate"}, {"blur", "soften"}, {"output", "out"}}, 256, 192);
    Graph& graph = rootGraph(document);
    graph.setParam(graph.nodeByName("soften")->id, "size", 3.0);
    connect(graph, "plate", "soften");
    connect(graph, "soften", "out");

    constexpr int kDomainWidth = 256;
    constexpr int kDomainHeight = 192;
    const std::vector<float> weights = gaussianWeights(3.0F);  // sigma 1, support 3
    ASSERT_EQ(weights.size(), 7u);
    const auto blurredChannel = [&weights](int fullX, int fullY, int channel) {
        float total = 0.0F;
        for (int tap = -3; tap <= 3; ++tap) {
            const std::array<float, 4> pixel =
                testPatternPixel(std::clamp(fullX + tap, 0, kDomainWidth - 1),
                                 std::clamp(fullY + tap, 0, kDomainHeight - 1), kDomainWidth, kDomainHeight);
            total += weights[static_cast<std::size_t>(tap + 3)] * pixel[static_cast<std::size_t>(channel)];
        }
        return total;
    };

    // A region flush against the top-left image border: its halo is clipped by
    // the domain, so the taps fold onto the real border pixels — the clamp the
    // whole-image reference has always applied, not a clamp against the region.
    const Region edge{1, 0, 20, 8};
    const auto edgeEvaluation =
        evaluateCpu(document, windowRequest(document, "out", edge, 1, kDomainWidth, kDomainHeight));
    expectCoverageInvariants(edgeEvaluation.plan, edge, 1, kDomainWidth, kDomainHeight);
    for (int y = 0; y < edgeEvaluation.image.height(); ++y) {
        for (int x = 0; x < edgeEvaluation.image.width(); ++x) {
            for (const int channel : {0, 1}) {
                EXPECT_NEAR(edgeEvaluation.image.pixel(x, y)[static_cast<std::size_t>(channel)],
                            blurredChannel(edge.x + x, edge.y + y, channel), 1e-6F)
                    << "border sample (" << x << "," << y << ") channel " << channel;
            }
            EXPECT_FLOAT_EQ(edgeEvaluation.image.pixel(x, y)[3], 1.0F);
        }
    }

    // An interior region: every tap comes from a real neighbor, so the halo is
    // what makes the result identical to the whole-image blur.
    const Region interior{101, 64, 20, 10};
    const auto interiorEvaluation =
        evaluateCpu(document, windowRequest(document, "out", interior, 1, kDomainWidth, kDomainHeight));
    const auto wholeFrame = evaluateCpu(
        document, windowRequest(document, "out", {0, 0, kDomainWidth, kDomainHeight}, 1, kDomainWidth, kDomainHeight));
    for (int y = 0; y < interiorEvaluation.image.height(); ++y) {
        for (int x = 0; x < interiorEvaluation.image.width(); ++x) {
            for (const int channel : {0, 1, 2}) {
                EXPECT_NEAR(interiorEvaluation.image.pixel(x, y)[static_cast<std::size_t>(channel)],
                            blurredChannel(interior.x + x, interior.y + y, channel), 1e-6F)
                    << "interior sample (" << x << "," << y << ") channel " << channel;
            }
        }
    }
    expectWindowMatchesWholeFrame(interiorEvaluation.image, wholeFrame.image, interior, 1);
}

TEST(RegionalEvaluationTest, WholeFrameOnlyContributionEscalatesAndStillServesRegions) {
    auto catalog = std::make_shared<const NodeCatalog>(extendedBuiltinSchema({wholeFrameFixtureDescriptor()}));
    Document document(catalog);
    CommandStack stack(document);
    stack.push(setNetworkFormatCommand(document.rootNetworkId(), ImageFormat{320, 160, 1.0F}));
    Graph& graph = document.network(document.rootNetworkId()).graph();
    graph.removeNode(graph.nodeByName("Output")->id);
    const NodeId plate = graph.addNode("testpattern", "plate");
    const NodeId whole = graph.addNode("fixture.wholeframe", "whole");
    const NodeId out = graph.addNode("output", "out");
    static_cast<void>(graph.connect(PortRef{plate, 0}, PortRef{whole, 0}));
    static_cast<void>(graph.connect(PortRef{whole, 0}, PortRef{out, 0}));

    std::vector<NodeContribution> registrations = builtinContributions();
    registrations.push_back(NodeContribution{.descriptor = wholeFrameFixtureDescriptor(),
                                             .role = NodeRole::Image,
                                             .cpu = CpuImplementation{1, &executeWholeFrameFixture}});
    const auto contributions = std::make_shared<NodeContributions>(std::move(registrations));

    const Region region{80, 40, 40, 30};
    const auto wholeFrame = evaluateCpu(document, windowRequest(document, "out", {0, 0, 320, 160}, 1, 320, 160),
                                        nullptr, nullptr, contributions);
    const auto cropped =
        evaluateCpu(document, windowRequest(document, "out", region, 1, 320, 160), nullptr, nullptr, contributions);

    expectCoverageInvariants(cropped.plan, region, 1, 320, 160);
    const PlanStep* wholeStep = stepFor(cropped.plan, "whole");
    ASSERT_NE(wholeStep, nullptr);
    // The whole-frame-only node really processed the whole domain (its own
    // implementation would have failed otherwise, and its pixels carry the
    // whole-frame mean), and its consumer still received exactly its window.
    EXPECT_EQ(wholeStep->region, (Region{0, 0, 320, 160}));
    EXPECT_EQ(wholeStep->produced.layout.width, 320);
    EXPECT_EQ(cropped.image.width(), region.width);
    EXPECT_EQ(cropped.image.height(), region.height);
    expectWindowMatchesWholeFrame(cropped.image, wholeFrame.image, region, 1);

    ResultCache<CpuImage> cache;
    const auto cached =
        evaluateCpu(document, windowRequest(document, "out", region, 1, 320, 160), &cache, nullptr, contributions);
    const auto cachedAgain =
        evaluateCpu(document, windowRequest(document, "out", region, 1, 320, 160), &cache, nullptr, contributions);
    EXPECT_EQ(cache.counts().misses, 3u);  // plate + whole + out computed once
    EXPECT_EQ(cache.counts().hits, 3u);    // every step reused on the second pass
    ASSERT_EQ(cached.image.width(), cachedAgain.image.width());
    ASSERT_EQ(cached.image.height(), cachedAgain.image.height());
    for (int y = 0; y < cached.image.height(); ++y)
        for (int x = 0; x < cached.image.width(); ++x)
            EXPECT_EQ(cached.image.pixel(x, y), cachedAgain.image.pixel(x, y));
    EXPECT_EQ(cachedAgain.plan.result.contentHash, cached.plan.result.contentHash);
    for (const PlanStep& step : cachedAgain.plan.steps)
        EXPECT_TRUE(step.cacheReused) << "step " << step.name;
}
