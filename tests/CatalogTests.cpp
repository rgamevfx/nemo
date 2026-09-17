#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/document/ParameterValueJson.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectSession.hpp"

using namespace nemo;
namespace {
namespace fs = std::filesystem;

Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

NodeDescriptor fixtureDescriptor() {
    return NodeDescriptor{
        .type = "fixture.catalog",
        .displayName = "Catalog Fixture",
        .group = "Tests",
        .implementationVersion = 7,
        .inputs = {},
        .outputs = {{PortKind::Image, "color"}},
        .parameters = {},
        .capabilities = NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"RGBA"}}};
}
}  // namespace

TEST(CatalogTest, RegisteredFixtureSnapshotIsDiscoverableAndTyped) {
    auto descriptor = fixtureDescriptor();
    descriptor.outputs.push_back({PortKind::Image, "aux"});
    auto catalog = std::make_shared<const NodeCatalog>(extendedBuiltinSchema(std::vector<NodeDescriptor>{descriptor}));
    ASSERT_NE(catalog->find("fixture.catalog"), nullptr);
    const auto& ports = catalog->outputPorts("fixture.catalog");
    const auto aux = std::find_if(ports.begin(), ports.end(), [](const PortSpec& port) { return port.name == "aux"; });
    ASSERT_NE(aux, ports.end());
    const auto portIndex = static_cast<std::uint32_t>(std::distance(ports.begin(), aux));
    EXPECT_TRUE(catalog->outputPorts("output").empty());
    EXPECT_TRUE(catalog->outputPorts("missing.fixture").empty());

    Graph graph(catalog);
    const NodeId fixture = graph.addNode("fixture.catalog", "fixture");
    const NodeId output = graph.addNode("output", "out");
    const auto edge = graph.connect(PortRef{fixture, portIndex}, PortRef{output, 0});
    ASSERT_EQ(graph.edgesInto(output).size(), 1u);
    EXPECT_EQ(graph.edgesInto(output).front().id, edge);
    EXPECT_EQ(graph.edgesInto(output).front().from, (PortRef{fixture, portIndex}));
    const auto invalidPort = static_cast<std::uint32_t>(ports.size());
    const auto rejected = graph.validateEdge(PortRef{fixture, invalidPort}, PortRef{output, 0});
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->code, GraphError::PortType);
}

TEST(CatalogTest, DeclaredFixtureReportsUnavailableExecutor) {
    auto catalog =
        std::make_shared<const NodeCatalog>(extendedBuiltinSchema(std::vector<NodeDescriptor>{fixtureDescriptor()}));
    Document document(catalog);
    const NodeId fixture = rootGraph(document).addNode("fixture.catalog", "fixture");
    const NodeId output = rootGraph(document).addNode("output", "out");
    rootGraph(document).connect(PortRef{fixture, 0}, PortRef{output, 0});

    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = output;
    request.region = {0, 0, 2, 2};
    try {
        static_cast<void>(evaluateCpu(document, request));
        FAIL() << "expected unavailable fixture executor";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("declared node type"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("executor unavailable"), std::string::npos);
    }
}

TEST(CatalogTest, InvalidDeclaredParametersIdentifyNodeAndPreserveState) {
    Document document;
    const NodeId color = rootGraph(document).addNode("constcolor", "background");
    EXPECT_THROW(rootGraph(document).setParam(color, "color", ParameterValue{std::string{"not-a-color"}}),
                 GraphException);
    EXPECT_TRUE(rootGraph(document).node(color)->params.empty());

    const NodeId merge = rootGraph(document).addNode("merge", "composite");
    try {
        rootGraph(document).setParam(merge, "operation", ParameterValue{ChoiceValue{"replace"}});
        FAIL() << "expected invalid declared choice";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::ParameterValue);
        EXPECT_NE(std::string(error.what()).find("composite"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("operation"), std::string::npos);
    }
    EXPECT_TRUE(rootGraph(document).node(merge)->params.empty());

    EXPECT_NO_THROW(rootGraph(document).setParam(color, "futureParameter", ParameterValue{std::string{"preserve-me"}}));
    EXPECT_EQ(std::get<std::string>(rootGraph(document).node(color)->params.at("futureParameter")), "preserve-me");
}

TEST(CatalogTest, InvalidDescriptorSchemaIsRejectedBeforeSnapshotPublication) {
    NodeDescriptor invalid = fixtureDescriptor();
    invalid.type.clear();
    EXPECT_THROW(static_cast<void>(NodeCatalog(extendedBuiltinSchema(std::vector<NodeDescriptor>{invalid}))),
                 std::invalid_argument);

    invalid = fixtureDescriptor();
    invalid.parameters = {{.name = "value",
                           .type = ParameterType::Float,
                           .defaultValue = ParameterValue{3.0},
                           .minimum = 0.0,
                           .maximum = 2.0}};

    invalid = fixtureDescriptor();
    invalid.capabilities.samplingScales = {1, 1};
    EXPECT_THROW(static_cast<void>(NodeCatalog(extendedBuiltinSchema(std::vector<NodeDescriptor>{invalid}))),
                 std::invalid_argument);

    EXPECT_THROW(static_cast<void>(NodeCatalog(
                     extendedBuiltinSchema(std::vector<NodeDescriptor>{fixtureDescriptor(), fixtureDescriptor()}))),
                 std::invalid_argument);
}

TEST(CatalogTest, ColorEditsRespectDeclaredBoundsAndFloatStorageAtomically) {
    NodeDescriptor descriptor = fixtureDescriptor();
    descriptor.parameters = {{.name = "color",
                              .type = ParameterType::Color,
                              .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 1.0F}}},
                              .minimum = 0.0,
                              .maximum = 1.0}};
    auto catalog = std::make_shared<const NodeCatalog>(extendedBuiltinSchema(std::vector<NodeDescriptor>{descriptor}));
    Document document(catalog);
    const NodeId node = rootGraph(document).addNode("fixture.catalog", "fixture");
    const auto revision = rootGraph(document).revision();

    EXPECT_THROW(rootGraph(document).setParam(node, "color", ParameterValue{ColorValue{{-1.0F, 0.0F, 0.0F, 1.0F}}}),
                 GraphException);
    EXPECT_EQ(rootGraph(document).revision(), revision);
    EXPECT_TRUE(rootGraph(document).node(node)->params.empty());
    EXPECT_THROW(rootGraph(document).setParam(
                     node, "color", ParameterValue{ColorValue{{std::numeric_limits<float>::max(), 0.0F, 0.0F, 1.0F}}}),
                 GraphException);
    EXPECT_EQ(rootGraph(document).revision(), revision);
    EXPECT_TRUE(rootGraph(document).node(node)->params.empty());
}

TEST(CatalogTest, UnboundedColorPreservesHdrAndNegativeValues) {
    Document document;
    const NodeId color = rootGraph(document).addNode("constcolor", "hdr");
    EXPECT_NO_THROW(
        rootGraph(document).setParam(color, "color", ParameterValue{ColorValue{{-2.0F, 3.5F, 100.0F, 1.0F}}}));
    EXPECT_EQ(rootGraph(document).node(color)->params.at("color"),
              (ParameterValue{ColorValue{{-2.0F, 3.5F, 100.0F, 1.0F}}}));
}

TEST(CatalogTest, TaggedIntegerRejectsUnsignedOverflowAndNonfiniteValues) {
    const auto tooLarge = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    const nlohmann::json integer{{"type", "integer"}, {"value", tooLarge}};
    EXPECT_THROW(static_cast<void>(parameterValueFromJson(integer)), std::invalid_argument);

    const ParameterValue maxFloat{static_cast<double>(std::numeric_limits<float>::max())};
    EXPECT_EQ(parameterValueFromJson(parameterValueToJson(maxFloat)), maxFloat);
    const double beyondFloat = 2.0 * static_cast<double>(std::numeric_limits<float>::max());
    EXPECT_THROW(static_cast<void>(parameterValueToJson(ParameterValue{beyondFloat})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parameterValueToJson(ParameterValue{std::numeric_limits<double>::quiet_NaN()})),
                 std::invalid_argument);
}

namespace {
NodeDescriptor inspectorParameters(ParameterSpec parameter) {
    NodeDescriptor descriptor = fixtureDescriptor();
    descriptor.parameters = {std::move(parameter)};
    return descriptor;
}

[[nodiscard]] bool rejectsInspectorParameter(const ParameterSpec& parameter) {
    try {
        static_cast<void>(
            NodeCatalog(extendedBuiltinSchema(std::vector<NodeDescriptor>{inspectorParameters(parameter)})));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
}  // namespace

TEST(CatalogTest, InspectorMetadataIsValidatedBeforeSnapshotPublication) {
    // A step is numeric-only and must be finite and positive.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::String,
                                                        .defaultValue = ParameterValue{std::string{}},
                                                        .step = 0.5}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .step = 0.0}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .step = std::numeric_limits<double>::quiet_NaN()}));

    // Labels and sections allow spaces but never control characters.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .label = std::string{"Bad\nLabel"}}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .section = std::string{"Bad\tsection"}}));

    // Custom editor ids are namespaced and free of whitespace/control characters.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .editor = "nemo"}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .editor = "nemo.linear slider"}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .editor = std::string{"nemo.linear\t"}}));

    // A fully specified numeric parameter is accepted with its metadata intact.
    auto catalog = NodeCatalog(extendedBuiltinSchema(
        std::vector<NodeDescriptor>{inspectorParameters(ParameterSpec{.name = "value",
                                                                      .type = ParameterType::Float,
                                                                      .defaultValue = ParameterValue{0.0},
                                                                      .minimum = -1.0,
                                                                      .maximum = 1.0,
                                                                      .step = 0.25,
                                                                      .label = "Exposure",
                                                                      .section = "Tone Map",
                                                                      .editor = "nemo.exposure"})}));
    const auto* spec = catalog.parameterSpec("fixture.catalog", "value");
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(spec->step.has_value());
    EXPECT_DOUBLE_EQ(*spec->step, 0.25);
    EXPECT_EQ(spec->label, "Exposure");
    EXPECT_EQ(spec->section, "Tone Map");
    EXPECT_EQ(spec->editor, "nemo.exposure");
}

TEST(CatalogTest, ViewerDescriptorIsDisplayOnlyAndNeverANetworkOutput) {
    const auto& catalog = builtinNodeCatalog();
    const auto* viewer = catalog.find("viewer");
    ASSERT_NE(viewer, nullptr);
    EXPECT_EQ(viewer->type, "viewer");
    EXPECT_EQ(viewer->displayName, "Viewer");
    EXPECT_EQ(viewer->group, "I/O");
    EXPECT_FALSE(viewer->isOutput);
    EXPECT_EQ(viewer->implementationVersion, 1u);
    ASSERT_EQ(viewer->inputs.size(), 1u);
    EXPECT_EQ(viewer->inputs.front().kind, PortKind::Image);
    EXPECT_EQ(viewer->inputs.front().name, "color");
    EXPECT_TRUE(viewer->outputs.empty());
    EXPECT_TRUE(viewer->parameters.empty());
    EXPECT_FALSE(viewer->capabilities.temporal);
    EXPECT_TRUE(catalog.outputPorts("viewer").empty());

    // Being display-only, a viewer can never become the network's result.
    Document document;
    const NodeId viewerNode = rootGraph(document).addNode("viewer", "Viewer1");
    EXPECT_THROW(document.network(document.rootNetworkId()).setDefaultOutput(viewerNode), GraphException);
    EXPECT_EQ(document.network(document.rootNetworkId()).defaultOutput(), rootGraph(document).nodeByName("Output")->id);
}

TEST(CatalogTest, PresentationMetadataRejectsInvalidDeclarations) {
    // Soft travel is interaction metadata: it must be finite, ordered, inside
    // any declared hard range, numeric-only, and never a legal-value bound.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::String,
                                                        .defaultValue = ParameterValue{std::string{}},
                                                        .softMinimum = 0.0}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .softMinimum = 1.0,
                                                        .softMaximum = 0.0}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .minimum = 0.0,
                                                        .softMinimum = -1.0}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .maximum = 1.0,
                                                        .softMaximum = 2.0}));
    // Display decimals are a bounded presentation hint for numeric types only.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .displayDecimals = -1}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .displayDecimals = 12}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::String,
                                                        .defaultValue = ParameterValue{std::string{}},
                                                        .displayDecimals = 2}));
    // A presentation row is display text and never carries control characters.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .row = std::string{"Bad\nRow"}}));
    // Linked channel semantics only apply to typed tuples, and a nonzero
    // constraint only to numeric parameters.
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::Float,
                                                        .defaultValue = ParameterValue{0.0},
                                                        .channels = ChannelHint{ChannelLink::Additive, false}}));
    EXPECT_TRUE(rejectsInspectorParameter(ParameterSpec{.name = "value",
                                                        .type = ParameterType::String,
                                                        .defaultValue = ParameterValue{std::string{}},
                                                        .nonzero = true}));

    auto catalog = std::make_shared<const NodeCatalog>(extendedBuiltinSchema(
        std::vector<NodeDescriptor>{inspectorParameters(ParameterSpec{.name = "value",
                                                                      .type = ParameterType::Float,
                                                                      .defaultValue = ParameterValue{1.0},
                                                                      .minimum = 0.0,
                                                                      .step = 0.5,
                                                                      .label = "Value",
                                                                      .section = "Tone",
                                                                      .editor = {},
                                                                      .softMinimum = 0.1,
                                                                      .softMaximum = 3.0,
                                                                      .displayDecimals = 2,
                                                                      .row = "Pair",
                                                                      .nonzero = true})}));
    const auto* spec = catalog->parameterSpec("fixture.catalog", "value");
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(spec->softMinimum.has_value());
    ASSERT_TRUE(spec->softMaximum.has_value());
    EXPECT_DOUBLE_EQ(*spec->softMinimum, 0.1);
    EXPECT_DOUBLE_EQ(*spec->softMaximum, 3.0);
    EXPECT_EQ(spec->row, "Pair");
    EXPECT_TRUE(spec->nonzero);

    // The nonzero constraint is authoritative for a generic parameter edit.
    Document document(catalog);
    const auto node = rootGraph(document).addNode("fixture.catalog", "nonzero");
    EXPECT_THROW(rootGraph(document).setParam(node, "value", ParameterValue{0.0}), GraphException);
    EXPECT_NO_THROW(rootGraph(document).setParam(node, "value", ParameterValue{2.5}));
    EXPECT_EQ(rootGraph(document).node(node)->params.at("value"), ParameterValue{2.5});
}

// ---------------------------------------------------------------------------
// Write: the delivery sink (issue #94, stories 72-73).
// ---------------------------------------------------------------------------

namespace {

NodeDescriptor deliveryFixtureDescriptor(bool isDeliverySink) {
    NodeDescriptor descriptor = fixtureDescriptor();
    descriptor.type = "fixture.delivery";
    descriptor.displayName = "Delivery Fixture";
    descriptor.outputs.clear();
    descriptor.isDeliverySink = isDeliverySink;
    return descriptor;
}

[[nodiscard]] NodeContribution deliveryFixtureContribution(bool isDeliverySink, NodeRole role) {
    NodeContribution contribution;
    contribution.descriptor = deliveryFixtureDescriptor(isDeliverySink);
    contribution.role = role;
    contribution.cpu =
        CpuImplementation{contribution.descriptor.implementationVersion, [](const CpuNodeContext& context) {
                              return requiredImageInput(context, 0, "fixture delivery needs an input");
                          }};
    return contribution;
}

// A network whose only sink is the Write node: the default Output node is
// removed before the session owns the document, so `plate -> write` is the
// whole graph and the delivery request is the only thing aimed at it.
struct WriteChain {
    WriteChain() : session(withoutDefaultOutput()) {}

    static Document withoutDefaultOutput() {
        Document document;
        rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
        return document;
    }

    ProjectSession session;
};

[[nodiscard]] EditResult submit(ProjectSession& session, Command command) {
    EditResult result = session.submit(std::move(command), EditOptions{session.revision(), {}});
    EXPECT_FALSE(result.error.has_value()) << (result.error ? result.error->message : std::string{});
    return result;
}

[[nodiscard]] const PlanStep* stepFor(const EvaluationPlan& plan, const std::string& name) {
    for (const PlanStep& step : plan.steps) {
        if (step.name == name)
            return &step;
    }
    return nullptr;
}

}  // namespace

TEST(CatalogTest, DeliveryRoleAndDeliverySinkFlagMustAgree) {
    const auto expectRejected = [](NodeContribution contribution, const char* relationship) {
        std::shared_ptr<const NodeContributions> candidate;
        try {
            candidate =
                std::make_shared<const NodeContributions>(std::vector<NodeContribution>{std::move(contribution)});
            ADD_FAILURE() << "an inconsistent delivery declaration was published";
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find("fixture.delivery"), std::string::npos) << error.what();
            EXPECT_NE(std::string(error.what()).find(relationship), std::string::npos) << error.what();
        }
        EXPECT_EQ(candidate, nullptr);
    };

    // A delivery sink role whose schema does not declare the sink...
    expectRejected(deliveryFixtureContribution(false, NodeRole::Delivery), "delivery-sink");
    // ...and a schema that declares a sink no role implements.
    expectRejected(deliveryFixtureContribution(true, NodeRole::Image), "delivery-sink");

    // The matching pair is accepted, so the check is consistency, not refusal.
    EXPECT_NO_THROW(static_cast<void>(
        NodeContributions(std::vector<NodeContribution>{deliveryFixtureContribution(true, NodeRole::Delivery)})));
}

TEST(CatalogTest, WriteEvaluationDeliversItsInputWithoutTouchingDisk) {
    std::error_code error;
    const fs::path dir =
        fs::temp_directory_path(error) / ("nemo-write-" + std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(dir, error);
    ASSERT_TRUE(fs::create_directories(dir, error) || !error);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(dir, ignored);
        }
    } cleanup{dir};

    WriteChain chain;
    ProjectSession& session = chain.session;
    const NetworkId network = session.document().rootNetworkId();
    submit(session, setNetworkFormatCommand(network, ImageFormat{8, 4, 1.0F}));
    submit(session, addNodeCommand(network, "testpattern", "plate"));
    submit(session, addNodeCommand(network, "write", "delivery"));
    const Graph& graph = rootGraph(session.document());
    const NodeId plate = graph.nodeByName("plate")->id;
    const NodeId write = graph.nodeByName("delivery")->id;
    submit(session, connectCommand(network, PortRef{plate, 0}, PortRef{write, 0}));
    const fs::path file = dir / "render.exr";
    submit(session, setParamCommand(network, write, "file", ParameterValue{file.string()}));

    EvaluationRequest request;
    request.network = network;
    request.output = write;
    request.region = {0, 0, 8, 4};
    const CpuEvaluation delivered = evaluateCpu(session.document(), request);

    // The delivery request is an ordinary evaluation: the same plate is
    // scheduled, and the delivered image is the plate's image unchanged.
    EvaluationRequest upstream = request;
    upstream.output = plate;
    const CpuEvaluation source = evaluateCpu(session.document(), upstream);
    ASSERT_EQ(delivered.plan.steps.size(), 2u);
    const PlanStep* plateStep = stepFor(delivered.plan, "plate");
    const PlanStep* deliveryStep = stepFor(delivered.plan, "delivery");
    ASSERT_NE(plateStep, nullptr);
    ASSERT_NE(deliveryStep, nullptr);
    ASSERT_EQ(source.image.width(), delivered.image.width());
    ASSERT_EQ(source.image.height(), delivered.image.height());
    for (int y = 0; y < source.image.height(); ++y) {
        for (int x = 0; x < source.image.width(); ++x) {
            EXPECT_EQ(delivered.image.pixel(x, y), source.image.pixel(x, y)) << "pixel (" << x << "," << y << ")";
        }
    }
    EXPECT_EQ(delivered.plan.result.contentHash, source.plan.result.contentHash);
    EXPECT_EQ(deliveryStep->produced.contentHash, plateStep->produced.contentHash);
    EXPECT_EQ(deliveryStep->inputs, (std::vector<NodeId>{plate}));

    // Story 73: evaluating a Write node never writes anything — no output file,
    // no temporary file, nothing at all in the authored directory.
    EXPECT_FALSE(fs::exists(file));
    EXPECT_TRUE(fs::is_empty(dir, error));
    // The authored request itself is document state and is left alone.
    EXPECT_EQ(session.document().network(network).graph().node(write)->params.at("file"),
              ParameterValue{file.string()});
}

TEST(CatalogTest, WriteSinkNeverBecomesTheNetworkResult) {
    Document document;
    const NodeId write = rootGraph(document).addNode("write", "delivery");
    // The network's result is defined by an Output node alone, so a delivery
    // sink can neither be selected as the default output...
    EXPECT_THROW(document.network(document.rootNetworkId()).setDefaultOutput(write), GraphException);

    // ...nor satisfy a network-result evaluation. With the default Output node
    // gone and a connected Write node present, the result is still missing.
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    rootGraph(document).addNode("testpattern", "plate");
    const NodeId plate = rootGraph(document).nodeByName("plate")->id;
    static_cast<void>(rootGraph(document).connect(PortRef{plate, 0}, PortRef{write, 0}));
    try {
        static_cast<void>(resolveOutput(document, document.rootNetworkId()));
        FAIL() << "expected EvaluationException";
    } catch (const EvaluationException& failure) {
        EXPECT_NE(std::string(failure.what()).find("no Output node"), std::string::npos);
    }
}
