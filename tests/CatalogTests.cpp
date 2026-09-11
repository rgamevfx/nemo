#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

using namespace nemo;

NodeDescriptor fixtureDescriptor() {
    return NodeDescriptor{
        .type = "fixture.catalog",
        .displayName = "Catalog Fixture",
        .group = "Tests",
        .implementationVersion = 7,
        .inputs = {},
        .outputs = {{PortKind::Color, "color"}},
        .parameters = {},
        .capabilities = NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"RGBA"}}};
}

TEST(CatalogTest, RegisteredFixtureSnapshotIsDiscoverableAndTyped) {
    auto descriptor = fixtureDescriptor();
    descriptor.outputs.push_back({PortKind::Color, "aux"});
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
    const NodeId fixture = document.graph.addNode("fixture.catalog", "fixture");
    const NodeId output = document.graph.addNode("output", "out");
    document.graph.connect(PortRef{fixture, 0}, PortRef{output, 0});

    EvaluationRequest request;
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
    const NodeId color = document.graph.addNode("constcolor", "background");
    EXPECT_THROW(document.graph.setParam(color, "color", "not-a-color"), GraphException);
    EXPECT_TRUE(document.graph.node(color)->params.empty());

    const NodeId merge = document.graph.addNode("merge", "composite");
    try {
        document.graph.setParam(merge, "operation", "replace");
        FAIL() << "expected invalid declared choice";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::ParameterValue);
        EXPECT_NE(std::string(error.what()).find("composite"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("operation"), std::string::npos);
    }
    EXPECT_TRUE(document.graph.node(merge)->params.empty());

    EXPECT_NO_THROW(document.graph.setParam(color, "futureParameter", "preserve-me"));
    EXPECT_EQ(document.graph.node(color)->params.at("futureParameter"), "preserve-me");
}

TEST(CatalogTest, InvalidDescriptorSchemaIsRejectedBeforeSnapshotPublication) {
    NodeDescriptor invalid = fixtureDescriptor();
    invalid.type.clear();
    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{invalid})), std::invalid_argument);

    invalid = fixtureDescriptor();
    invalid.parameters = {
        {.name = "value", .type = ParameterType::Float, .defaultValue = "3", .minimum = 0.0, .maximum = 2.0}};
    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{invalid})), std::invalid_argument);

    invalid = fixtureDescriptor();
    invalid.capabilities.samplingScales = {1, 1};
    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{invalid})), std::invalid_argument);

    EXPECT_THROW(static_cast<void>(NodeCatalog(std::vector<NodeDescriptor>{fixtureDescriptor(), fixtureDescriptor()})),
                 std::invalid_argument);
}

TEST(CatalogTest, ColorEditsRespectDeclaredBoundsAndFloatStorageAtomically) {
    NodeDescriptor descriptor = fixtureDescriptor();
    descriptor.parameters = {
        {.name = "color", .type = ParameterType::Color, .defaultValue = "0 0 0 1", .minimum = 0.0, .maximum = 1.0}};
    auto catalog = std::make_shared<const NodeCatalog>(std::vector<NodeDescriptor>{descriptor});
    Document document(catalog);
    const NodeId node = document.graph.addNode("fixture.catalog", "fixture");
    const auto revision = document.graph.revision();

    EXPECT_THROW(document.graph.setParam(node, "color", "-1 0 0 1"), GraphException);
    EXPECT_EQ(document.graph.revision(), revision);
    EXPECT_TRUE(document.graph.node(node)->params.empty());
    EXPECT_THROW(document.graph.setParam(node, "color", "3.4028236e38 0 0 1"), GraphException);
    EXPECT_EQ(document.graph.revision(), revision);
    EXPECT_TRUE(document.graph.node(node)->params.empty());
}

TEST(CatalogTest, UnboundedColorPreservesHdrAndNegativeValues) {
    Document document;
    const NodeId color = document.graph.addNode("constcolor", "hdr");
    EXPECT_NO_THROW(document.graph.setParam(color, "color", "-2 3.5 100 1"));
    EXPECT_EQ(document.graph.node(color)->params.at("color"), "-2 3.5 100 1");
}
