#include <gtest/gtest.h>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

using namespace nemo;

namespace {

Network& root(Document& document) {
    return document.network(document.rootNetworkId());
}

}  // namespace

TEST(NetworkCommandsTest, CollapseClassifiesCrossingsAndUndoRestoresBothScopes) {
    auto catalog = std::make_shared<NodeCatalog>(std::vector<NodeDescriptor>{
        {.type = "test.mask",
         .displayName = "Mask",
         .group = "Test",
         .outputs = {{PortKind::Mask, "mask"}},
         .capabilities = {{1}, {Quality::Full}, {"RGBA"}}},
        {.type = "test.masked",
         .displayName = "Masked",
         .group = "Test",
         .inputs = {{PortKind::Image, "A"}, {PortKind::Image, "B"}, {PortKind::Mask, "mask"}},
         .outputs = {{PortKind::Image, "out"}},
         .capabilities = {{1}, {Quality::Full}, {"RGBA"}}}});
    Document document(catalog);
    const auto parentId = document.rootNetworkId();
    auto& graph = root(document).graph();
    const auto a = graph.addNode("constcolor", "A");
    const auto b = graph.addNode("constcolor", "B");
    const auto mask = graph.addNode("test.mask", "Mask");
    const auto p = graph.addNode("test.masked", "P");
    const auto q = graph.addNode("merge", "Q");
    const auto finish = graph.addNode("merge", "Finish");
    graph.setLayout(p, {220, 180});
    graph.setLayout(q, {420, 280});
    graph.connect({a, 0}, {p, 0});
    graph.connect({b, 0}, {p, 1});
    graph.connect({mask, 0}, {p, 2});
    graph.connect({a, 0}, {q, 1});
    const auto internal = graph.connect({p, 0}, {q, 0});
    const std::vector<LayoutPosition> route{{260, 240}, {380, 240}};
    graph.setRoute(internal, route);
    graph.connect({p, 0}, {finish, 0});
    graph.connect({q, 0}, {finish, 1});

    CommandStack history(document);
    auto created = std::make_shared<NetworkInstanceId>();
    history.push(collapseSelectionCommand(parentId, {p, q}, "collapsed", created));
    const auto occurrence = *document.instance(*created);
    const auto& child = document.network(occurrence.definition);
    ASSERT_EQ(child.inputs().size(), 3U);
    ASSERT_EQ(child.outputs().size(), 2U);
    EXPECT_EQ(child.graph().node(p)->layout, (LayoutPosition{220, 180}));
    EXPECT_EQ(child.graph().node(q)->layout, (LayoutPosition{420, 280}));
    ASSERT_EQ(child.graph().edges().size(), 1U);
    EXPECT_EQ(child.graph().edges().front().route, route);
    for (const auto& [terminal, source] : occurrence.inputBindings) {
        std::vector<PortRef> destinations;
        for (const auto& connection : child.inputConnections())
            if (connection.terminal == terminal)
                destinations.push_back(connection.node);
        if (source.node == a) {
            EXPECT_EQ(destinations, (std::vector<PortRef>{{p, 0}, {q, 1}}));
            EXPECT_EQ(child.input(terminal)->kind, PortKind::Image);
        } else if (source.node == b) {
            EXPECT_EQ(destinations, (std::vector<PortRef>{{p, 1}}));
        } else {
            EXPECT_EQ(source.node, mask);
            EXPECT_EQ(child.input(terminal)->kind, PortKind::Mask);
            EXPECT_EQ(destinations, (std::vector<PortRef>{{p, 2}}));
        }
    }
    ASSERT_EQ(occurrence.inputBindings.size(), 3U);
    const auto& parentEdges = document.network(parentId).graph().edgesInto(finish);
    ASSERT_EQ(parentEdges.size(), 2U);
    for (const auto& edge : parentEdges) {
        ASSERT_EQ(edge.from.node, occurrence.node);
        const auto terminal = child.outputs().at(edge.from.port).id;
        const auto output = std::find_if(child.outputConnections().begin(), child.outputConnections().end(),
                                         [terminal](const TerminalConnection& c) { return c.terminal == terminal; });
        ASSERT_NE(output, child.outputConnections().end());
        EXPECT_EQ(output->node, (PortRef{edge.to.port == 0 ? p : q, 0}));
    }
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(document.instance(*created), nullptr);
    const auto& restored = document.network(parentId).graph();
    EXPECT_EQ(restored.node(p)->layout, (LayoutPosition{220, 180}));
    EXPECT_EQ(restored.node(q)->layout, (LayoutPosition{420, 280}));
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(document.instance(*created)->definition, occurrence.definition);
    EXPECT_EQ(document.network(occurrence.definition).graph().edges().front().route, route);
}

TEST(NetworkCommandsTest, InvalidCollapseIsAtomic) {
    Document document;
    const auto parentId = document.rootNetworkId();
    const auto output = document.network(parentId).defaultOutput();
    const auto before = saveDocument(document);
    CommandStack history(document);

    EXPECT_THROW(history.push(collapseSelectionCommand(parentId, {output}, "invalid")), GraphException);
    EXPECT_EQ(saveDocument(document), before);
    EXPECT_EQ(history.depth(), 0u);
}

TEST(NetworkCommandsTest, PromotionIsMetadataOnlyAndIndependentCloneDetachesOneOccurrence) {
    Document document;
    const auto rootId = document.rootNetworkId();
    const auto definitionId = document.addNetwork("shared");
    auto& definition = document.network(definitionId);
    const auto color = definition.graph().addNode("constcolor", "color");
    const auto output = definition.addOutput("image", PortKind::Image);
    definition.connectOutput({color, 0}, output);
    const ColorValue authored{{0.2F, 0.4F, 0.6F, 1.0F}};
    definition.graph().setParam(color, "color", authored);
    auto exposed = std::make_shared<InterfacePortId>();
    CommandStack history(document);
    history.push(promoteParameterCommand(definitionId, color, "color", "Tint", exposed));
    const auto first = document.addInstance(rootId, definitionId, "first");
    const auto second = document.addInstance(rootId, definitionId, "second");
    const auto firstOutput = root(document).defaultOutput();
    const auto secondOutput = root(document).graph().addNode("output", "other");
    root(document).graph().connect({document.instance(first)->node, 0}, {firstOutput, 0});
    root(document).graph().connect({document.instance(second)->node, 0}, {secondOutput, 0});
    const auto pixel = [&](NodeId target) {
        return evaluateCpu(document, {.network = rootId, .output = target, .region = {0, 0, 1, 1}}).image.pixel(0, 0);
    };
    EXPECT_EQ(pixel(firstOutput), authored.value);
    EXPECT_EQ(pixel(secondOutput), authored.value);
    const ParameterAddress local{definitionId, color, "color", first};
    const ColorValue localValue{{.9F, .1F, .2F, 1.F}};
    history.push(setKeyframesCommand({KeyframeEdit{local, Keyframe{0, 0.0, localValue}}}));
    const auto channelId = document.animationChannel(local)->id;
    const auto keyId = document.animationChannel(local)->keys.front().id;
    EXPECT_EQ(pixel(firstOutput), localValue.value);
    EXPECT_EQ(pixel(secondOutput), authored.value);
    auto independent = std::make_shared<NetworkId>();
    history.push(makeIndependentCommand(first, independent));
    EXPECT_EQ(pixel(firstOutput), localValue.value);
    EXPECT_EQ(pixel(secondOutput), authored.value);
    const ParameterAddress detached{*independent, color, "color", first};
    ASSERT_NE(document.animationChannel(detached), nullptr);
    EXPECT_EQ(document.animationChannel(detached)->id, channelId);
    EXPECT_EQ(document.animationChannel(detached)->keys.front().id, keyId);
    const ColorValue changed{{0.F, 1.F, 0.F, 1.F}};
    history.push(setParamCommand(definitionId, color, "color", changed));
    EXPECT_EQ(pixel(firstOutput), localValue.value);
    EXPECT_EQ(pixel(secondOutput), changed.value);
    history.push(removeExposedParameterCommand(*independent, *exposed));
    EXPECT_EQ(pixel(firstOutput), localValue.value);
    EXPECT_EQ(document.animationChannel(detached)->keys.front().id, keyId);
}

TEST(NetworkCommandsTest, ViewerOnlyCollapseRoutesExternalInputThroughSubnetOutput) {
    Document document;
    const auto parentId = document.rootNetworkId();
    auto& parent = root(document);
    const auto pattern = parent.graph().addNode("constcolor", "plate");
    parent.graph().setParam(pattern, "color", ColorValue{{.25F, .5F, .75F, 1.F}});
    const auto viewer = parent.graph().addNode("viewer", "viewer");
    parent.graph().setPortContract(viewer, {PortSpec{PortKind::Image, "image"}}, {});
    parent.graph().connect({pattern, 0}, {viewer, 0});

    CommandStack history(document);
    auto created = std::make_shared<NetworkInstanceId>();
    history.push(collapseSelectionCommand(parentId, {viewer}, "viewer subnet", created));
    const auto* occurrence = document.instance(*created);
    ASSERT_NE(occurrence, nullptr);
    const auto childId = occurrence->definition;
    const auto& child = document.network(childId);
    ASSERT_EQ(child.inputs().size(), 2u);
    ASSERT_EQ(child.outputs().size(), 1u);
    ASSERT_EQ(child.outputInputBindings().size(), 1u);
    const auto passThrough = child.outputInputBindings().begin();
    EXPECT_EQ(child.input(passThrough->second)->kind, PortKind::Image);
    EXPECT_EQ(child.output(passThrough->first)->kind, PortKind::Image);

    auto& committedParent = document.network(parentId);
    committedParent.graph().connect({occurrence->node, 0}, {committedParent.defaultOutput(), 0});
    EvaluationRequest request;
    request.network = parentId;
    request.output = committedParent.defaultOutput();
    request.region = {0, 0, 8, 4};
    EXPECT_EQ(evaluateCpu(document, request).image.pixel(7, 0), (std::array<float, 4>{.25F, .5F, .75F, 1.F}));
    EXPECT_TRUE(history.undo());
    EXPECT_EQ(document.instance(*created), nullptr);
}

TEST(NetworkCommandsTest, OwnedLocalDefinitionIsRemovedWithoutTouchingLinkedDefinition) {
    Document document;
    const auto rootId = document.rootNetworkId();
    const auto shared = document.addNetwork("shared");
    document.network(shared).graph().addNode("testpattern", "pattern");
    const auto linked = document.addInstance(rootId, shared, "linked");
    const auto localDefinition = document.addNetwork("local");
    document.network(localDefinition).graph().addNode("testpattern", "pattern");
    const auto local = document.addInstance(rootId, localDefinition, "local");
    document.setInstanceOwnership(local, true);

    document.removeInstance(local);
    EXPECT_THROW(static_cast<void>(document.network(localDefinition)), std::out_of_range);
    EXPECT_NE(document.instance(linked), nullptr);
    EXPECT_NO_THROW(static_cast<void>(document.network(shared)));
}

TEST(NetworkCommandsTest, NonconvexCollapseRejectsParentCycleWithoutPublishing) {
    Document document;
    const auto network = document.rootNetworkId();
    auto& graph = root(document).graph();
    const auto a = graph.addNode("constcolor", "A");
    const auto b = graph.addNode("merge", "B");
    const auto c = graph.addNode("merge", "C");
    graph.connect({a, 0}, {b, 0});
    graph.connect({b, 0}, {c, 0});
    graph.connect({c, 0}, {root(document).defaultOutput(), 0});
    const auto before = saveDocument(document);
    CommandStack history(document);
    EXPECT_THROW(history.push(collapseSelectionCommand(network, {a, c}, "nonconvex")), GraphException);
    EXPECT_EQ(saveDocument(document), before);
    EXPECT_FALSE(history.canUndo());
}

TEST(NetworkCommandsTest, NestedCollapseRetainsExternalScopeAndUnpacksWithoutChangingPixels) {
    Document document;
    const auto network = document.rootNetworkId();
    const auto output = root(document).defaultOutput();
    auto& graph = root(document).graph();
    const auto foreground = graph.addNode("constcolor", "Foreground");
    const auto background = graph.addNode("constcolor", "Background");
    const auto merge = graph.addNode("merge", "Merge");
    graph.setParam(foreground, "color", ColorValue{{.8F, .2F, .1F, .5F}});
    graph.setParam(background, "color", ColorValue{{.1F, .3F, .9F, 1.F}});
    graph.connect({foreground, 0}, {merge, 1});
    graph.connect({background, 0}, {merge, 0});
    graph.connect({merge, 0}, {output, 0});
    const EvaluationRequest request{.network = network, .output = output, .region = {0, 0, 8, 4}};
    const auto original = evaluateCpu(document, request).image;
    EXPECT_NEAR(original.pixel(0, 0)[0], .45F, 1e-6F);
    EXPECT_NEAR(original.pixel(0, 0)[1], .25F, 1e-6F);
    EXPECT_NEAR(original.pixel(0, 0)[2], .5F, 1e-6F);
    const auto expected = cpuImageHash(original);
    CommandStack history(document);
    auto outer = std::make_shared<NetworkInstanceId>();
    history.push(collapseSelectionCommand(network, {merge}, "Outer", outer));
    auto wrapper = std::make_shared<NetworkInstanceId>();
    history.push(collapseSelectionCommand(network, {document.instance(*outer)->node}, "Wrapper", wrapper));
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
    const auto child = document.instance(*outer)->definition;
    auto inner = std::make_shared<NetworkInstanceId>();
    history.push(collapseSelectionCommand(child, {merge}, "Inner", inner));
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
    auto restored = loadDocument(saveDocument(document));
    EXPECT_EQ(cpuImageHash(evaluateCpu(restored.document, request).image), expected);
    history.push(unpackInstanceCommand(*inner));
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
    history.push(unpackInstanceCommand(*outer));
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
    history.push(unpackInstanceCommand(*wrapper));
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(cpuImageHash(evaluateCpu(document, request).image), expected);
}

TEST(NetworkCommandsTest, CopyDuplicatesLocalStructureButRetainsNestedExplicitLinks) {
    Document document;
    const auto rootId = document.rootNetworkId();
    const auto asset = document.addNetwork("Asset");
    const auto color = document.network(asset).graph().addNode("constcolor", "Color");
    const ColorValue blue{{0.F, 0.F, 1.F, 1.F}};
    document.network(asset).graph().setParam(color, "color", blue);
    const auto assetOutput = document.network(asset).addOutput("image", PortKind::Image);
    document.network(asset).connectOutput({color, 0}, assetOutput);
    const auto localDefinition = document.addNetwork("Local");
    const auto linkedAsset = document.addInstance(localDefinition, asset, "Linked asset");
    const auto localOutput = document.network(localDefinition).addOutput("image", PortKind::Image);
    document.network(localDefinition).connectOutput({document.instance(linkedAsset)->node, 0}, localOutput);
    const auto local = document.addInstance(rootId, localDefinition, "Local subnet");
    document.setInstanceOwnership(local, true);
    const auto localNode = document.instance(local)->node;
    auto copies = std::make_shared<std::vector<NodeId>>();
    CommandStack history(document);
    history.push(copySelectionCommand(rootId, {localNode}, rootId, {300, 0}, copies));
    ASSERT_EQ(copies->size(), 1U);
    const auto copyNode = copies->front();
    const auto firstOutput = root(document).defaultOutput();
    const auto otherOutput = root(document).graph().addNode("output", "other");
    root(document).graph().connect({localNode, 0}, {firstOutput, 0});
    root(document).graph().connect({copyNode, 0}, {otherOutput, 0});
    const auto pixel = [&](NodeId output) {
        return evaluateCpu(document, {.network = rootId, .output = output, .region = {0, 0, 1, 1}}).image.pixel(0, 0);
    };
    EXPECT_EQ(pixel(firstOutput), blue.value);
    EXPECT_EQ(pixel(otherOutput), blue.value);
    const auto greenNode = document.network(localDefinition).graph().addNode("constcolor", "Local color");
    const ColorValue green{{0.F, 1.F, 0.F, 1.F}};
    history.push(setParamCommand(localDefinition, greenNode, "color", green));
    history.push(replaceOutputConnectionCommand(localDefinition, localOutput, {greenNode, 0}));
    EXPECT_EQ(pixel(firstOutput), green.value);
    EXPECT_EQ(pixel(otherOutput), blue.value);
    const ColorValue red{{1.F, 0.F, 0.F, 1.F}};
    history.push(setParamCommand(asset, color, "color", red));
    EXPECT_EQ(pixel(firstOutput), green.value);
    EXPECT_EQ(pixel(otherOutput), red.value);
    history.push(createLinkedInstanceCommand(rootId, localDefinition, "Explicit linked use"));
    auto linkedCopies = std::make_shared<std::vector<NodeId>>();
    history.push(copySelectionCommand(rootId, {localNode}, rootId, {600, 0}, linkedCopies));
    ASSERT_EQ(linkedCopies->size(), 1U);
    const auto linkedOutput = root(document).graph().addNode("output", "linked output");
    root(document).graph().connect({linkedCopies->front(), 0}, {linkedOutput, 0});
    EXPECT_EQ(pixel(linkedOutput), green.value);
    const ColorValue yellow{{1.F, 1.F, 0.F, 1.F}};
    history.push(setParamCommand(localDefinition, greenNode, "color", yellow));
    EXPECT_EQ(pixel(firstOutput), yellow.value);
    EXPECT_EQ(pixel(linkedOutput), yellow.value);
    EXPECT_EQ(pixel(otherOutput), red.value);
}

TEST(NetworkCommandsTest, DisconnectedViewerCollapseRejectsWithoutInventingAnOutput) {
    Document document;
    const auto network = document.rootNetworkId();
    const auto viewer = root(document).graph().addNode("viewer", "Disconnected viewer");
    root(document).graph().setPortContract(viewer, {{PortKind::Image, "image"}}, {});
    const auto before = saveDocument(document);
    CommandStack history(document);
    EXPECT_THROW(history.push(collapseSelectionCommand(network, {viewer}, "Invalid")), GraphException);
    EXPECT_EQ(saveDocument(document), before);
    EXPECT_FALSE(history.canUndo());
    root(document).graph().setPortContract(viewer, {{PortKind::Image, "image"}, {PortKind::Mask, "mask"}}, {});
    const auto mask = root(document).graph().addNode("test.mask", "Mask only");
    root(document).graph().setPortContract(mask, {}, {{PortKind::Mask, "mask"}});
    root(document).graph().connect({mask, 0}, {viewer, 1});
    const auto maskOnly = saveDocument(document);
    EXPECT_THROW(history.push(collapseSelectionCommand(network, {viewer}, "Still invalid")), GraphException);
    EXPECT_EQ(saveDocument(document), maskOnly);
    EXPECT_FALSE(history.canUndo());
}

TEST(NetworkCommandsTest, ExposedParameterRejectsDuplicateSourceAndReorders) {
    Document document;
    const auto definitionId = document.addNetwork("shared");
    auto& definition = document.network(definitionId);
    const auto color = definition.graph().addNode("constcolor", "color");
    const auto merge = definition.graph().addNode("merge", "merge");
    CommandStack history(document);
    auto tint = std::make_shared<InterfacePortId>();
    history.push(promoteParameterCommand(definitionId, color, "color", "Tint", tint));
    auto mix = std::make_shared<InterfacePortId>();
    history.push(promoteParameterCommand(definitionId, merge, "operation", "Operation", mix));

    // The same source parameter is one authored control, and duplicate display
    // names would make the inspector rows ambiguous.
    const auto afterPromotion = saveDocument(document);
    EXPECT_THROW(history.push(promoteParameterCommand(definitionId, color, "color", "Tint Again")), GraphException);
    EXPECT_THROW(history.push(promoteParameterCommand(definitionId, merge, "operation", "Tint")), GraphException);
    EXPECT_EQ(saveDocument(document), afterPromotion);
    const auto exposedIds = [&document, definitionId] {
        std::vector<InterfacePortId> ids;
        for (const auto& exposed : document.network(definitionId).exposedParameters())
            ids.push_back(exposed.id);
        return ids;
    };
    EXPECT_EQ(exposedIds(), (std::vector<InterfacePortId>{*tint, *mix}));

    history.push(moveExposedParameterCommand(definitionId, *tint, 1));
    EXPECT_EQ(exposedIds(), (std::vector<InterfacePortId>{*mix, *tint}));
    // A destination past the end clamps to the last row instead of failing.
    history.push(moveExposedParameterCommand(definitionId, *mix, 99));
    EXPECT_EQ(exposedIds(), (std::vector<InterfacePortId>{*tint, *mix}));
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(exposedIds(), (std::vector<InterfacePortId>{*mix, *tint}));
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(exposedIds(), (std::vector<InterfacePortId>{*tint, *mix}));
}

TEST(NetworkCommandsTest, ParameterInsertionAndUndoAreOneAtomicEdit) {
    Document document;
    const auto network = document.addNetwork("Editable");
    auto& definition = document.network(network);
    const auto color = definition.graph().addNode("constcolor", "color");
    const auto transform = definition.graph().addNode("transform", "transform");
    CommandStack history(document);
    history.push(promoteParameterCommand(network, color, "color", "Tint"));
    history.push(promoteParameterCommand(network, transform, "rotate", "Angle"));
    const auto names = [&] {
        std::vector<std::string> result;
        for (const auto& parameter : document.network(network).exposedParameters())
            result.push_back(parameter.name);
        return result;
    };
    history.push(promoteParameterCommand(network, transform, "scale", "Size", {}, 1));
    EXPECT_EQ(names(), (std::vector<std::string>{"Tint", "Size", "Angle"}));
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(names(), (std::vector<std::string>{"Tint", "Angle"}));
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(names(), (std::vector<std::string>{"Tint", "Size", "Angle"}));
    const auto beforeRejection = saveDocument(document);
    EXPECT_THROW(history.push(promoteParameterCommand(network, transform, "scale", "Duplicate", {}, 0)),
                 GraphException);
    EXPECT_EQ(saveDocument(document), beforeRejection);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(names(), (std::vector<std::string>{"Tint", "Angle"}));
}

TEST(NetworkCommandsTest, ExposureInterfaceLayoutAndOwnershipSurviveSerialization) {
    Document document;
    const auto rootId = document.rootNetworkId();
    const auto definitionId = document.addNetwork("Inspectable");
    auto& definition = document.network(definitionId);
    const auto color = definition.graph().addNode("constcolor", "color");
    const auto image = definition.addInput("image", PortKind::Image);
    const auto result = definition.addOutput("result", PortKind::Image);
    definition.setFormalPortLayout(PortDirection::Input, image, {12.0, 34.0});
    definition.renameFormalPort(PortDirection::Input, image, "plate");
    const auto passed = definition.addOutput("passed", PortKind::Image);
    definition.connectOutputToInput(passed, image);
    definition.connectOutput({color, 0}, result);
    const auto exposed = definition.addExposedParameter(color, "color", "Tint");
    const auto owned = document.addInstance(rootId, definitionId, "Owned");
    const auto linked = document.addInstance(rootId, definitionId, "Linked");
    document.setInstanceOwnership(owned, true);

    const auto saved = saveDocument(document);
    auto restored = loadDocument(saved);
    EXPECT_EQ(saveDocument(restored.document), saved);
    const auto* restoredOwned = restored.document.instance(owned);
    ASSERT_NE(restoredOwned, nullptr);
    EXPECT_TRUE(restoredOwned->ownsDefinition);
    ASSERT_NE(restored.document.instance(linked), nullptr);
    EXPECT_FALSE(restored.document.instance(linked)->ownsDefinition);

    const auto& restoredDefinition = restored.document.network(definitionId);
    const auto* restoredPlate = restoredDefinition.input("plate");
    ASSERT_NE(restoredPlate, nullptr);
    EXPECT_EQ(restoredPlate->layout, (LayoutPosition{12.0, 34.0}));
    ASSERT_EQ(restoredDefinition.outputInputBindings().size(), 1U);
    const auto* restoredExposed = restoredDefinition.exposedParameter(exposed);
    ASSERT_NE(restoredExposed, nullptr);
    EXPECT_EQ(restoredExposed->name, "Tint");
    EXPECT_EQ(restoredExposed->key, "color");
    EXPECT_EQ(restoredExposed->node, color);
    EXPECT_EQ(restoredExposed->type, ParameterType::Color);
}

TEST(NetworkCommandsTest, LinkedDuplicateSharesDefinitionAndPlacesTheOccurrence) {
    Document document;
    const auto rootId = document.rootNetworkId();
    const auto definitionId = document.addNetwork("shared");
    document.network(definitionId).graph().addNode("testpattern", "pattern");
    const auto first = document.addInstance(rootId, definitionId, "First");
    document.setInstanceOwnership(first, true);

    CommandStack history(document);
    auto created = std::make_shared<NetworkInstanceId>();
    history.push(createLinkedInstanceCommand(rootId, definitionId, "Second", {320.0, 180.0}, created));
    const auto* second = document.instance(*created);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->definition, definitionId);
    EXPECT_FALSE(second->ownsDefinition);
    EXPECT_FALSE(document.instance(first)->ownsDefinition);
    const auto* secondNode = root(document).graph().node(second->node);
    ASSERT_NE(secondNode, nullptr);
    EXPECT_EQ(secondNode->layout, (LayoutPosition{320.0, 180.0}));
    EXPECT_NE(document.instance(first)->node, second->node);
}
