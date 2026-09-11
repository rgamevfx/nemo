#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectSession.hpp"

using namespace nemo;

namespace {
Document colorDocument(NodeId* node) {
    Document document;
    auto& graph = document.network(document.rootNetworkId()).graph();
    *node = graph.addNode("constcolor", "animated-color");
    return document;
}

ParameterAddress colorAddress(const Document& document, NodeId node) {
    return ParameterAddress{document.rootNetworkId(), node, "color"};
}

ColorValue color(float r, float g, float b) {
    return ColorValue{{r, g, b, 1.0F}};
}
}  // namespace

TEST(AnimationTest, LinearKeyframesEvaluateAtIntermediateAndEndpointFrames) {
    NodeId node{};
    Document document = colorDocument(&node);
    const auto address = colorAddress(document, node);
    const auto set = setKeyframesCommand({
        KeyframeEdit{address, Keyframe{0, 0.0, color(0.0F, 0.0F, 0.0F)}},
        KeyframeEdit{address, Keyframe{0, 10.0, color(1.0F, 1.0F, 1.0F)}},
    });
    set.apply(document);

    const auto middle = animatedParameterValue(document, address, 5.0);
    ASSERT_TRUE(std::holds_alternative<ColorValue>(middle));
    EXPECT_FLOAT_EQ(std::get<ColorValue>(middle).value[0], 0.5F);
    EXPECT_EQ(animatedParameterValue(document, address, -10.0), ParameterValue{color(0.0F, 0.0F, 0.0F)});
    EXPECT_EQ(animatedParameterValue(document, address, 20.0), ParameterValue{color(1.0F, 1.0F, 1.0F)});
}

TEST(AnimationTest, KeyedGestureUpdatesExistingKeyAndUndoesCompleteDragWithoutPublishingPreview) {
    NodeId node{};
    ProjectSession session(colorDocument(&node));
    const auto address = colorAddress(session.document(), node);
    ASSERT_TRUE(
        session.submit(setKeyframesCommand({{address, Keyframe{0, 12.0, color(0, 0, 0)}}}), {session.revision(), {}})
            .committed);
    const auto original = *session.document().animationChannel(address);
    const auto revision = session.revision();
    const auto begin = session.beginKeyedParameterGesture(12.0, {{address, color(0.25F, 0.5F, 0.75F)}}, {revision, {}});
    ASSERT_NE(begin.snapshot, nullptr);
    const auto update = session.updateKeyedParameterGesture(begin.token, {{address, color(1, 0, 0)}});
    ASSERT_NE(update.snapshot, nullptr);
    EXPECT_EQ(*session.document().animationChannel(address), original);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(animatedParameterValue(*update.snapshot, address, 12.0), ParameterValue{color(1, 0, 0)});
    ASSERT_TRUE(session.commitParameterGesture(begin.token, {revision, {}}).committed);
    const auto committed = *session.document().animationChannel(address);
    ASSERT_EQ(committed.keys.size(), 1U);
    EXPECT_EQ(committed.keys.front().id, original.keys.front().id);
    EXPECT_EQ(committed.keys.front().value, ParameterValue{color(1, 0, 0)});
    ASSERT_TRUE(session.undo({session.revision(), {}}).committed);
    EXPECT_EQ(*session.document().animationChannel(address), original);
    ASSERT_TRUE(session.redo({session.revision(), {}}).committed);
    EXPECT_EQ(*session.document().animationChannel(address), committed);
    const auto cancelled =
        session.beginKeyedParameterGesture(12.0, {{address, color(0, 1, 0)}}, {session.revision(), {}});
    ASSERT_NE(cancelled.snapshot, nullptr);
    EXPECT_FALSE(session.cancelParameterGesture(cancelled.token).error);
    EXPECT_EQ(*session.document().animationChannel(address), committed);
}

TEST(AnimationTest, HoldInterpolationAndDiscreteValuesDoNotBlend) {
    NodeId node{};
    Document document = colorDocument(&node);
    const auto address = colorAddress(document, node);
    auto first = Keyframe{0, 0.0, color(1.0F, 0.0F, 0.0F)};
    first.interpolation = KeyInterpolation::Hold;
    ASSERT_NO_THROW(setKeyframesCommand({KeyframeEdit{address, first},
                                         KeyframeEdit{address, Keyframe{0, 10.0, color(0.0F, 0.0F, 1.0F)}}})
                        .apply(document));
    EXPECT_EQ(animatedParameterValue(document, address, 5.0), ParameterValue{color(1.0F, 0.0F, 0.0F)});

    auto catalog = std::make_shared<NodeCatalog>(std::vector<NodeDescriptor>{NodeDescriptor{
        .type = "discrete.fixture",
        .displayName = "Discrete Fixture",
        .parameters = {{.name = "choice",
                        .type = ParameterType::Choice,
                        .defaultValue = ParameterValue{ChoiceValue{"A"}},
                        .choices = {"A", "B"}},
                       {.name = "toggle", .type = ParameterType::Boolean, .defaultValue = ParameterValue{false}}}}});
    Document discreteDocument(catalog);
    auto& discreteGraph = discreteDocument.network(discreteDocument.rootNetworkId()).graph();
    const auto discreteNode = discreteGraph.addNode("discrete.fixture", "discrete");
    const ParameterAddress choice{discreteDocument.rootNetworkId(), discreteNode, "choice"};
    const ParameterAddress toggle{discreteDocument.rootNetworkId(), discreteNode, "toggle"};
    auto choiceFirst = Keyframe{0, 0.0, ParameterValue{ChoiceValue{"A"}}};
    auto choiceSecond = Keyframe{0, 10.0, ParameterValue{ChoiceValue{"B"}}};
    choiceFirst.interpolation = KeyInterpolation::Hold;
    choiceSecond.interpolation = KeyInterpolation::Hold;
    auto toggleFirst = Keyframe{0, 0.0, ParameterValue{false}};
    auto toggleSecond = Keyframe{0, 10.0, ParameterValue{true}};
    toggleFirst.interpolation = KeyInterpolation::Hold;
    toggleSecond.interpolation = KeyInterpolation::Hold;
    ASSERT_NO_THROW(setKeyframesCommand({KeyframeEdit{choice, choiceFirst}, KeyframeEdit{choice, choiceSecond},
                                         KeyframeEdit{toggle, toggleFirst}, KeyframeEdit{toggle, toggleSecond}})
                        .apply(discreteDocument));
    EXPECT_EQ(animatedParameterValue(discreteDocument, choice, 5.0), ParameterValue{ChoiceValue{"A"}});
    EXPECT_EQ(animatedParameterValue(discreteDocument, toggle, 5.0), ParameterValue{false});
    EXPECT_EQ(animatedParameterValue(discreteDocument, toggle, 10.0), ParameterValue{true});
}

TEST(AnimationTest, BezierInsertionPreservesNonlinearCurveAndEndpointExtrapolation) {
    NodeId node{};
    Document document = colorDocument(&node);
    const auto address = colorAddress(document, node);
    auto left = Keyframe{0, 0.0, color(0.0F, 0.0F, 0.0F)};
    left.interpolation = KeyInterpolation::Bezier;
    auto right = Keyframe{0, 10.0, color(1.0F, 1.0F, 1.0F)};
    right.interpolation = KeyInterpolation::Bezier;
    setKeyframesCommand({{address, left}, {address, right}}).apply(document);
    const auto checkCurve = [&] {
        // Zero endpoint slopes give the known smoothstep polynomial 3u² - 2u³.
        for (const auto [time, expected] : std::array<std::pair<double, float>, 7>{{{-5.0, 0.0F},
                                                                                    {0.0, 0.0F},
                                                                                    {2.5, 0.15625F},
                                                                                    {5.0, 0.5F},
                                                                                    {7.5, 0.84375F},
                                                                                    {10.0, 1.0F},
                                                                                    {15.0, 1.0F}}})
            EXPECT_NEAR(std::get<ColorValue>(animatedParameterValue(document, address, time)).value[0], expected, 1e-6F)
                << time;
    };
    checkCurve();
    insertKeyframeCommand(address, 5.0).apply(document);
    checkCurve();
    const auto& inserted = document.animationChannel(address)->keys[1];
    EXPECT_NEAR(inserted.inSlope[0], 0.15, 1e-12);
    EXPECT_EQ(inserted.inSlope, inserted.outSlope);
    insertKeyframeCommand(address, -5.0).apply(document);
    insertKeyframeCommand(address, 15.0).apply(document);
    checkCurve();
}
TEST(AnimationTest, ContinuousValuesUseActualUnitsForScalarVectorAndColor) {
    auto catalog = std::make_shared<NodeCatalog>(std::vector<NodeDescriptor>{NodeDescriptor{
        .type = "numeric.fixture",
        .displayName = "Numeric Fixture",
        .parameters = {{.name = "scalar", .type = ParameterType::Float, .defaultValue = ParameterValue{0.0}},
                       {.name = "vector",
                        .type = ParameterType::Vector2,
                        .defaultValue = ParameterValue{Vector2Value{{0.0F, 0.0F}}}},
                       {.name = "color",
                        .type = ParameterType::Color,
                        .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}}}}}});
    Document document(catalog);
    auto& graph = document.network(document.rootNetworkId()).graph();
    const auto node = graph.addNode("numeric.fixture", "numeric");
    const auto scalar = ParameterAddress{document.rootNetworkId(), node, "scalar"};
    const auto vector = ParameterAddress{document.rootNetworkId(), node, "vector"};
    const auto colorAddressValue = ParameterAddress{document.rootNetworkId(), node, "color"};
    setKeyframesCommand(
        {KeyframeEdit{scalar, Keyframe{0, 0.0, ParameterValue{2.0}}},
         KeyframeEdit{scalar, Keyframe{0, 4.0, ParameterValue{6.0}}},
         KeyframeEdit{vector, Keyframe{0, 0.0, ParameterValue{Vector2Value{{10.0F, -4.0F}}}}},
         KeyframeEdit{vector, Keyframe{0, 4.0, ParameterValue{Vector2Value{{18.0F, 4.0F}}}}},
         KeyframeEdit{colorAddressValue, Keyframe{0, 0.0, ParameterValue{ColorValue{{2.0F, 4.0F, 6.0F, 8.0F}}}}},
         KeyframeEdit{colorAddressValue, Keyframe{0, 4.0, ParameterValue{ColorValue{{6.0F, 8.0F, 10.0F, 12.0F}}}}}})
        .apply(document);
    EXPECT_EQ(animatedParameterValue(document, scalar, 2.0), ParameterValue{4.0});
    EXPECT_EQ(animatedParameterValue(document, vector, 2.0), (ParameterValue{Vector2Value{{14.0F, 0.0F}}}));
    EXPECT_EQ(animatedParameterValue(document, colorAddressValue, 2.0),
              (ParameterValue{ColorValue{{4.0F, 6.0F, 8.0F, 10.0F}}}));
}

TEST(AnimationTest, KeyedGestureRejectsResetValuesAndPreservesTransientPublication) {
    NodeId node{};
    ProjectSession session(colorDocument(&node));
    const auto address = colorAddress(session.document(), node);
    const auto result = session.beginKeyedParameterGesture(1.0, {ParameterEdit{address, std::nullopt}},
                                                           EditOptions{session.revision(), {}});
    EXPECT_FALSE(result.result.committed);
    EXPECT_NE(result.result.error, std::nullopt);
    EXPECT_TRUE(session.queryAnimationChannels().empty());
    EXPECT_FALSE(session.canUndo());
}

TEST(AnimationTest, KeyedBeginRejectsStaleRevisionAndGroupUpdatesShareOneGesture) {
    NodeId firstNode{};
    Document document = colorDocument(&firstNode);
    const auto secondNode = document.network(document.rootNetworkId()).graph().addNode("constcolor", "second-color");
    ProjectSession session(std::move(document));
    const auto firstAddress = colorAddress(session.document(), firstNode);
    const auto secondAddress = colorAddress(session.document(), secondNode);
    ASSERT_TRUE(session
                    .submit(setParamCommand(session.document().rootNetworkId(), firstNode, "color",
                                            ParameterValue{color(0.1F, 0.1F, 0.1F)}),
                            EditOptions{session.revision(), {}})
                    .committed);
    const auto stale = session.beginKeyedParameterGesture(
        3.0, {ParameterEdit{firstAddress, ParameterValue{color(1.0F, 0.0F, 0.0F)}}}, EditOptions{1, {}});
    ASSERT_NE(stale.result.error, std::nullopt);
    EXPECT_EQ(stale.result.error->code, EditErrorCode::RevisionConflict);

    const auto begin =
        session.beginKeyedParameterGesture(3.0, {{firstAddress, color(1.0F, 0.0F, 0.0F)}}, {session.revision(), {}});
    ASSERT_NE(begin.snapshot, nullptr);
    const auto update = session.updateKeyedParameterGesture(
        begin.token, {{firstAddress, color(0.0F, 0.0F, 1.0F)}, {secondAddress, color(0.0F, 1.0F, 0.0F)}});
    ASSERT_NE(update.snapshot, nullptr);
    EXPECT_TRUE(session.queryAnimationChannels().empty());
    const auto previewChannels = update.snapshot->animationChannels();
    ASSERT_TRUE(session.commitParameterGesture(begin.token, {session.revision(), {}}).committed);
    EXPECT_EQ(session.document().animationChannels(), previewChannels);
    EXPECT_EQ(animatedParameterValue(session.document(), firstAddress, 3.0), ParameterValue{color(0, 0, 1)});
    EXPECT_EQ(animatedParameterValue(session.document(), secondAddress, 3.0), ParameterValue{color(0, 1, 0)});
    ASSERT_TRUE(session.undo({session.revision(), {}}).committed);
    EXPECT_TRUE(session.queryAnimationChannels().empty());
    ASSERT_TRUE(session.redo({session.revision(), {}}).committed);
    EXPECT_EQ(session.document().animationChannels(), previewChannels);
}

TEST(AnimationTest, KeyQueryCursorUsesStableIdsRatherThanMutableTimeOrder) {
    NodeId node{};
    ProjectSession session(colorDocument(&node));
    const auto address = colorAddress(session.document(), node);
    ASSERT_TRUE(session
                    .submit(setKeyframesCommand({{address, Keyframe{0, 10.0, color(1, 0, 0)}},
                                                 {address, Keyframe{0, 0.0, color(0, 1, 0)}},
                                                 {address, Keyframe{0, 5.0, color(0, 0, 1)}}}),
                            {session.revision(), {}})
                    .committed);
    const auto channels = session.queryAnimationChannels(1);
    ASSERT_EQ(channels.size(), 1U);
    EXPECT_TRUE(session.queryAnimationChannels(0).empty());
    EXPECT_TRUE(session.queryAnimationChannels(1, channels.front().id).empty());
    const auto first = session.queryAnimationKeys(channels.front().id, 1);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first.front().key.time, 10.0);
    const auto second = session.queryAnimationKeys(channels.front().id, 1, first.front().key.id);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second.front().key.time, 0.0);
    const auto third = session.queryAnimationKeys(channels.front().id, 1, second.front().key.id);
    ASSERT_EQ(third.size(), 1U);
    EXPECT_EQ(third.front().key.time, 5.0);
    EXPECT_TRUE(session.queryAnimationKeys(channels.front().id, 1, third.front().key.id).empty());
}

TEST(AnimationTest, CollidingGroupMoveIsAtomicAndValidMoveIsOneHistoryEntry) {
    NodeId node{};
    ProjectSession session(colorDocument(&node));
    const auto address = colorAddress(session.document(), node);
    ASSERT_TRUE(session
                    .submit(setKeyframesCommand({KeyframeEdit{address, Keyframe{0, 0.0, color(0, 0, 0)}},
                                                 KeyframeEdit{address, Keyframe{0, 10.0, color(1, 1, 1)}}}),
                            EditOptions{session.revision(), {}})
                    .committed);
    const auto channel = session.queryAnimationChannels().front();
    const auto keys = session.queryAnimationKeys(channel.id);
    ASSERT_EQ(keys.size(), 2U);
    const auto revision = session.revision();
    const auto collision = session.submit(moveKeyframesCommand({KeyframeRef{channel.id, keys[1].key.id}}, -10.0),
                                          EditOptions{revision, {}});
    EXPECT_FALSE(collision.committed);
    EXPECT_EQ(collision.error->code, EditErrorCode::InvalidArgument);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(session.queryAnimationKeys(channel.id)[0].key.time, 0.0);
    EXPECT_EQ(session.queryAnimationKeys(channel.id)[1].key.time, 10.0);

    const auto moved = session.submit(
        moveKeyframesCommand({KeyframeRef{channel.id, keys[0].key.id}, KeyframeRef{channel.id, keys[1].key.id}}, 1.0),
        EditOptions{revision, {}});
    ASSERT_TRUE(moved.committed);
    EXPECT_EQ(session.queryAnimationKeys(channel.id)[0].key.time, 1.0);
    EXPECT_EQ(session.queryAnimationKeys(channel.id)[1].key.time, 11.0);
    ASSERT_TRUE(session.undo({session.revision(), {}}).committed);
    EXPECT_EQ(session.queryAnimationKeys(channel.id)[0].key, keys[0].key);
    EXPECT_EQ(session.queryAnimationKeys(channel.id)[1].key, keys[1].key);
}

TEST(AnimationTest, ExactBatchSwapsTimesAndRejectsEveryFieldOnTangentOrTargetConflicts) {
    NodeId node{};
    ProjectSession session(colorDocument(&node));
    const auto address = colorAddress(session.document(), node);
    ASSERT_TRUE(session
                    .submit(setKeyframesCommand({{address, Keyframe{0, 0, color(0, 0, 0)}},
                                                 {address, Keyframe{0, 10, color(1, 1, 1)}}}),
                            {session.revision(), {}})
                    .committed);
    const auto before = *session.document().animationChannel(address);
    auto first = before.keys[0];
    auto second = before.keys[1];
    first.time = 10;
    first.value = color(2, 3, 4);
    first.interpolation = KeyInterpolation::Bezier;
    first.tangentMode = TangentMode::Broken;
    first.inSlope[0] = -0.5;
    first.outSlope[0] = 0.25;
    second.time = 0;
    second.inSlope[0] = 1;
    const auto revision = session.revision();
    const auto rejected = session.submit(setKeyframesCommand({{address, first}, {address, second}}), {revision, {}});
    ASSERT_TRUE(rejected.error);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(*session.document().animationChannel(address), before);
    second.outSlope[0] = 1;
    ASSERT_TRUE(session.submit(setKeyframesCommand({{address, first}, {address, second}}), {revision, {}}).committed);
    EXPECT_EQ(session.document().animationChannel(address)->keys, (std::vector<Keyframe>{second, first}));
    const auto changed = *session.document().animationChannel(address);
    auto byTime = first;
    byTime.id = 0;
    EXPECT_FALSE(
        session.submit(setKeyframesCommand({{address, first}, {address, byTime}}), {session.revision(), {}}).committed);
    EXPECT_EQ(*session.document().animationChannel(address), changed);
    ASSERT_TRUE(session.undo({session.revision(), {}}).committed);
    EXPECT_EQ(*session.document().animationChannel(address), before);
    ASSERT_TRUE(session.undo({session.revision(), {}}).committed);
    EXPECT_TRUE(session.document().animationChannels().empty());
}
TEST(AnimationTest, NodeDeletionPrunesAnimationQueriesAndUndoRestoresIdentity) {
    NodeId node{};
    ProjectSession session(colorDocument(&node));
    const auto address = colorAddress(session.document(), node);
    ASSERT_TRUE(session
                    .submit(setKeyframesCommand({KeyframeEdit{address, Keyframe{0, 0.0, color(0, 0, 0)}}}),
                            EditOptions{session.revision(), {}})
                    .committed);
    const auto channelId = session.queryAnimationChannels().front().id;
    const auto keyId = session.queryAnimationKeys(channelId).front().key.id;
    const auto network = session.document().rootNetworkId();
    const auto deleted = session.submit(
        Command{"delete node",
                [network, node](Document& document) { document.network(network).graph().removeNode(node); }},
        EditOptions{session.revision(), {}});
    ASSERT_TRUE(deleted.committed);
    EXPECT_EQ(deleted.changedAnimationChannelIds, (std::vector<AnimationChannelId>{channelId}));
    EXPECT_EQ(deleted.changedAnimationKeyIds, (std::vector<KeyframeRef>{{channelId, keyId}}));
    EXPECT_TRUE(session.queryAnimationKeys(channelId).empty());
    EXPECT_TRUE(session.queryAnimationChannels().empty());
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    const auto restored = session.queryAnimationChannels();
    ASSERT_EQ(restored.size(), 1U);
    EXPECT_EQ(restored.front().id, channelId);
    EXPECT_EQ(session.queryAnimationKeys(channelId).front().key.id, keyId);
}
