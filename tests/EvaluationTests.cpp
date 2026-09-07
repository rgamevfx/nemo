#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

using namespace nemo;

namespace {

Document makeDocument(const std::vector<std::pair<std::string, std::string>>& typeAndName) {
    Document document;
    for (const auto& [type, name] : typeAndName) {
        document.graph.addNode(type, name);
    }
    return document;
}
void connect(Graph& graph, const std::string& from, const std::string& to, std::uint32_t fromPort = 0,
             std::uint32_t toPort = 0) {
    const Node* fromNode = graph.nodeByName(from);
    const Node* toNode = graph.nodeByName(to);
    ASSERT_NE(fromNode, nullptr);
    ASSERT_NE(toNode, nullptr);
    static_cast<void>(graph.connect(PortRef{fromNode->id, fromPort}, PortRef{toNode->id, toPort}));
}

EvaluationRequest fullFrameRequest(const Document& document, std::int64_t frame, int width = 8, int height = 4) {
    EvaluationRequest request;
    request.output = resolveOutput(document);
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
    connect(document.graph, "plate", "out");

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
    document.graph.nodeByName("backdrop")->params["color"] = "0 0 1 0.5";
    connect(document.graph, "plate", "comp", 0, 0);     // port A: over base
    connect(document.graph, "backdrop", "comp", 0, 1);  // port B: over source
    connect(document.graph, "comp", "out");

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
    ASSERT_EQ(merge->effectiveParams.count("operation"), 1);
    EXPECT_EQ(merge->effectiveParams.at("operation"), "over");
}

// Acceptance example 2: no Output node -> evaluation error naming the
// document, surfaced verbatim by the CLI as {"errors": [...]} with exit 1.
TEST(EvaluationTest, MissingOutputNodeFailsWithClearError) {
    Document document = makeDocument({{"testpattern", "plate"}});
    try {
        static_cast<void>(resolveOutput(document));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& e) {
        EXPECT_NE(std::string(e.what()).find("no Output node"), std::string::npos);
    }
}

TEST(EvaluationTest, MultipleOutputNodesRequireDisambiguation) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "a"}, {"output", "b"}});
    connect(document.graph, "plate", "a");
    connect(document.graph, "plate", "b");
    try {
        static_cast<void>(resolveOutput(document));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& e) {
        EXPECT_NE(std::string(e.what()).find("multiple Output nodes"), std::string::npos);
    }
    // Naming one of them resolves the request.
    EvaluationRequest request;
    request.output = resolveOutput(document, "b");
    EXPECT_EQ(document.graph.node(request.output)->name, "b");
}

// Invalid-type connections are rejected by Graph at edit time.
TEST(EvaluationTest, InvalidTypeConnectionRejectedByGraph) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    const NodeId plate = document.graph.nodeByName("plate")->id;
    const NodeId out = document.graph.nodeByName("out")->id;
    // The Output node declares no output ports: anything fed from it is a
    // type error, not a silent dangling edge.
    const auto fromOutput = document.graph.validateEdge(PortRef{out, 0}, PortRef{plate, 0});
    ASSERT_TRUE(fromOutput.has_value());
    EXPECT_EQ(fromOutput->code, GraphError::PortType);
    EXPECT_THROW(static_cast<void>(document.graph.connect(PortRef{out, 0}, PortRef{plate, 0})), GraphException);
    // testpattern declares no input ports either.
    const auto intoPattern = document.graph.validateEdge(PortRef{plate, 0}, PortRef{plate, 0});
    ASSERT_TRUE(intoPattern.has_value());
    EXPECT_EQ(intoPattern->code, GraphError::PortType);
}

// Acceptance example 3 (core side): same document + same frame produce an
// identical plan and identical image; a different frame changes the recorded
// image identity.
TEST(EvaluationTest, RepeatedRequestsAreDeterministicAndFrameSensitive) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(document.graph, "plate", "out");

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
    connect(document.graph, "comp", "out");
    try {
        static_cast<void>(evaluateCpu(document, fullFrameRequest(document, 0)));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& e) {
        EXPECT_NE(std::string(e.what()).find("node 'comp'"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("input port 0 ('A') is not connected"), std::string::npos);
        EXPECT_TRUE(e.hasNode());
    }
}

TEST(EvaluationTest, UnknownTypeInDependencyChainIsReportedNotSilent) {
    Document document = makeDocument({{"grail", "mystery"}, {"output", "out"}});
    connect(document.graph, "mystery", "out");
    try {
        static_cast<void>(evaluateCpu(document, fullFrameRequest(document, 0)));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& e) {
        EXPECT_NE(std::string(e.what()).find("no CPU reference implementation"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("'mystery'"), std::string::npos);
    }
}

TEST(EvaluationTest, RequestValidationRejectsUnsupportedChannelsAndQuality) {
    Document document = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(document.graph, "plate", "out");

    EvaluationRequest channels = fullFrameRequest(document, 0);
    channels.channels = "depth";
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, channels)), EvaluationException);

    EvaluationRequest quality = fullFrameRequest(document, 0);
    quality.quality = Quality::Draft;
    EXPECT_THROW(static_cast<void>(evaluateCpu(document, quality)), EvaluationException);
}
