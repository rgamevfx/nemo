#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/document/ParameterValueJson.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

using namespace nemo;
namespace {
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
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
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
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{fixtureDescriptor()});
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
    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{invalid})), std::invalid_argument);

    invalid = fixtureDescriptor();
    invalid.parameters = {{.name = "value",
                           .type = ParameterType::Float,
                           .defaultValue = ParameterValue{3.0},
                           .minimum = 0.0,
                           .maximum = 2.0}};

    invalid = fixtureDescriptor();
    invalid.capabilities.samplingScales = {1, 1};
    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{invalid})), std::invalid_argument);

    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{fixtureDescriptor(), fixtureDescriptor()})),
                 std::invalid_argument);
}

TEST(CatalogTest, ColorEditsRespectDeclaredBoundsAndFloatStorageAtomically) {
    NodeDescriptor descriptor = fixtureDescriptor();
    descriptor.parameters = {{.name = "color",
                              .type = ParameterType::Color,
                              .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 1.0F}}},
                              .minimum = 0.0,
                              .maximum = 1.0}};
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
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
        static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{inspectorParameters(parameter)}));
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
    auto catalog =
        NodeCatalog(std::vector<NodeDescriptor>{inspectorParameters(ParameterSpec{.name = "value",
                                                                                  .type = ParameterType::Float,
                                                                                  .defaultValue = ParameterValue{0.0},
                                                                                  .minimum = -1.0,
                                                                                  .maximum = 1.0,
                                                                                  .step = 0.25,
                                                                                  .label = "Exposure",
                                                                                  .section = "Tone Map",
                                                                                  .editor = "nemo.exposure"})});
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

    auto catalog = std::make_shared<const NodeCatalog>(
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
                                                                      .nonzero = true})});
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
