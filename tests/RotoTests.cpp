#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/commands/RotoCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Roto.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"

// Roto (issue #93) acceptance at the approved public session seam: the authored
// value is published, undone and persisted through ProjectSession and the
// project file codec, and its properties are animated through the existing
// keyed/gesture machinery. Nothing here reaches into a private model path: a
// test edits exactly what the UI and the CLI edit.

namespace {

namespace fs = std::filesystem;
using namespace nemo;

constexpr std::string_view kRotoType = "roto";

// A group with a nested shape, plus a sibling shape, authored through the
// model's own helpers so every identity comes from the value's watermarks
// exactly as a caller's would.
RotoData authoredShapes() {
    RotoData data;
    const RotoElementId group = appendRotoGroup(data, kInvalidRotoElement, "Group 1");
    appendRotoRectangle(data, kInvalidRotoElement, "Rect", 10.0, 20.0, 100.0, 50.0);
    appendRotoEllipse(data, group, "Ellipse", 64.0, 64.0, 32.0, 16.0);
    return data;
}

ParameterAddress elementAddress(NetworkId network, NodeId node, std::string_view key, RotoElementId element) {
    return ParameterAddress{network, node, std::string(key), kInvalidNetworkInstance, element, kInvalidRotoPoint};
}

ParameterAddress pointAddress(NetworkId network, NodeId node, std::string_view key, RotoElementId element,
                              RotoPointId point) {
    return ParameterAddress{network, node, std::string(key), kInvalidNetworkInstance, element, point};
}

ParameterValue vector2(float x, float y) {
    return ParameterValue{Vector2Value{{x, y}}};
}

class RotoTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        dir_ = fs::temp_directory_path(ec) /
               ("nemo-roto-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(counter_++));
        fs::remove_all(dir_, ec);
        ASSERT_TRUE(fs::create_directories(dir_, ec) || !ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // Creates the roto node every test edits and returns its identity.
    static NodeId addRotoNode(ProjectSession& session, std::string name = "roto") {
        auto created = std::make_shared<NodeId>();
        const EditResult result = session.submit(
            addNodeCommand(session.document().rootNetworkId(), std::string{kRotoType}, std::move(name), created),
            EditOptions{session.revision(), {}});
        EXPECT_TRUE(result.committed) << (result.error ? result.error->message : std::string{"add rejected"});
        return *created;
    }

    static const RotoData* published(const ProjectSession& session, NodeId node) {
        const auto* instance = session.document().network(session.document().rootNetworkId()).graph().node(node);
        return instance == nullptr || !instance->roto ? nullptr : instance->roto.get();
    }

    static bool publish(ProjectSession& session, NodeId node, const RotoData& data) {
        return session
            .submit(setRotoDataCommand(session.document().rootNetworkId(), node, data),
                    EditOptions{session.revision(), {}})
            .committed;
    }

    fs::path dir_;

private:
    static int counter_;
};

int RotoTest::counter_ = 0;

TEST_F(RotoTest, TopologyCommandPublishesValueAndSurvivesUndoRedoWithIdentities) {
    ProjectSession session;
    const NodeId node = addRotoNode(session);
    const RotoData authored = authoredShapes();
    const RotoElementId groupId = authored.elements[0].id;
    const RotoElementId rectangleId = authored.elements[1].id;
    const RotoPointId pointId = authored.elements[1].points[0].id;

    const EditResult result = session.submit(setRotoDataCommand(session.document().rootNetworkId(), node, authored),
                                             EditOptions{session.revision(), {}});
    ASSERT_TRUE(result.committed) << result.error->message;
    ASSERT_EQ(result.changedNodeIds.size(), 1U);
    EXPECT_EQ(result.changedNodeIds.front().id, node);

    const RotoData* live = published(session, node);
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(rotoContentEquals(*live, authored));
    EXPECT_EQ(live->elements[1].parent, kInvalidRotoElement);
    ASSERT_NE(rotoElement(*live, groupId), nullptr);
    EXPECT_EQ(rotoElement(*live, authored.elements[2].id)->parent, groupId);
    ASSERT_NE(rotoPoint(*rotoElement(*live, rectangleId), pointId), nullptr);

    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    EXPECT_EQ(published(session, node), nullptr);
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    live = published(session, node);
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(rotoContentEquals(*live, authored));
    EXPECT_EQ(live->nextElementId, authored.nextElementId);
    EXPECT_EQ(live->nextPointId, authored.nextPointId);
    EXPECT_EQ(rotoElement(*live, rectangleId)->points[0].id, pointId);
}

TEST_F(RotoTest, RejectedTopologyIsAtomicAndLeavesNoHistoryEntry) {
    ProjectSession session;
    const NodeId node = addRotoNode(session);
    const NetworkId network = session.document().rootNetworkId();
    const RotoData baseline = authoredShapes();
    ASSERT_TRUE(publish(session, node, baseline));

    const RotoElementId groupId = baseline.elements[0].id;
    const RotoElementId rectangleId = baseline.elements[1].id;
    const RotoElementId ellipseId = baseline.elements[2].id;
    const RotoPointId pointId = baseline.elements[1].points[0].id;

    std::vector<std::pair<std::string, RotoData>> rejected;
    {
        RotoData duplicate = baseline;
        duplicate.elements[1].id = groupId;
        rejected.emplace_back("duplicate element identity", std::move(duplicate));
    }
    {
        RotoData orphan = baseline;
        orphan.elements[1].parent = 4242;
        rejected.emplace_back("unknown parent", std::move(orphan));
    }
    {
        RotoData nonGroup = baseline;
        nonGroup.elements[0].parent = rectangleId;
        rejected.emplace_back("non-group parent", std::move(nonGroup));
    }
    {
        RotoData cyclic = baseline;
        const RotoElementId nested = appendRotoGroup(cyclic, groupId, "Group 2");
        cyclic.elements[rotoElementIndex(cyclic, groupId)].parent = nested;
        rejected.emplace_back("cyclic parentage", std::move(cyclic));
    }
    {
        RotoData nonFinite = baseline;
        nonFinite.elements[1].points[0].position = Vector2Value{{std::numeric_limits<float>::quiet_NaN(), 0.0F}};
        rejected.emplace_back("non-finite geometry", std::move(nonFinite));
    }
    {
        RotoData unclosed = baseline;
        RotoElement& rectangle = unclosed.elements[1];
        rectangle.points.erase(2);
        rectangle.points.erase(2);
        rejected.emplace_back("unclosed shape", std::move(unclosed));
    }
    {
        RotoData groupWithPoints = baseline;
        RotoPoint stray;
        groupWithPoints.elements[0].points.push_back(stray);
        rejected.emplace_back("points on a group", std::move(groupWithPoints));
    }
    {
        RotoData duplicatePoints = baseline;
        duplicatePoints.elements[rotoElementIndex(duplicatePoints, ellipseId)].points[0].id = pointId;
        rejected.emplace_back("duplicate point identity", std::move(duplicatePoints));
    }
    {
        RotoData outOfRange = baseline;
        outOfRange.elements[1].opacity = 1.5;
        rejected.emplace_back("opacity range", std::move(outOfRange));
    }
    {
        RotoData outOfRange = baseline;
        outOfRange.elements[1].points[0].tension = 2.0;
        rejected.emplace_back("tension range", std::move(outOfRange));
    }
    {
        RotoData watermark = baseline;
        watermark.nextElementId = 1;
        rejected.emplace_back("element identity watermark", std::move(watermark));
    }
    {
        RotoData inverted = baseline;
        inverted.elements[1].firstFrame = 20.0;
        inverted.elements[1].lastFrame = 4.0;
        rejected.emplace_back("inverted lifetime", std::move(inverted));
    }

    for (const auto& [reason, candidate] : rejected) {
        const std::uint64_t revision = session.revision();
        const bool canUndo = session.canUndo();
        const Document before = session.document();
        const EditResult result =
            session.submit(setRotoDataCommand(network, node, candidate), EditOptions{revision, {}});
        ASSERT_FALSE(result.committed) << "accepted " << reason;
        ASSERT_TRUE(result.error.has_value()) << reason;
        EXPECT_EQ(result.error->graphError, GraphError::InvalidRoto) << reason;
        EXPECT_EQ(session.revision(), revision) << reason;
        EXPECT_EQ(session.canUndo(), canUndo) << reason;
        EXPECT_TRUE(documentContentEquals(session.document(), before)) << reason;
    }

    // A valid successor still commits: the refusals left a usable document
    // behind instead of a poisoned candidate.
    RotoData moved = baseline;
    moved.elements[1].points[0].position = Vector2Value{{15.0F, 25.0F}};
    ASSERT_TRUE(publish(session, node, moved));
    EXPECT_FLOAT_EQ(rotoElement(*published(session, node), rectangleId)->points[0].position.value[0], 15.0F);
}

TEST_F(RotoTest, LockedElementRefusesValueAndTopologyWritesUntilUnlocked) {
    ProjectSession session;
    const NodeId node = addRotoNode(session);
    const NetworkId network = session.document().rootNetworkId();
    RotoData authored = authoredShapes();
    const RotoElementId groupId = authored.elements[0].id;
    const RotoElementId rectangleId = authored.elements[1].id;
    const RotoPointId pointId = authored.elements[1].points[0].id;
    authored.elements[1].locked = true;
    ASSERT_TRUE(publish(session, node, authored));

    const ParameterAddress position = pointAddress(network, node, kRotoParamPosition, rectangleId, pointId);
    const ParameterEdit draggedPoint{position, vector2(1.0F, 2.0F)};
    const EditResult valueEdit =
        session.submit(setParametersCommand({draggedPoint}), EditOptions{session.revision(), {}});
    ASSERT_FALSE(valueEdit.committed);
    EXPECT_EQ(valueEdit.error->graphError, GraphError::InvalidRoto);

    RotoData dragged = authored;
    dragged.elements[1].points[0].position = Vector2Value{{1.0F, 2.0F}};
    const EditResult topologyEdit =
        session.submit(setRotoDataCommand(network, node, dragged), EditOptions{session.revision(), {}});
    ASSERT_FALSE(topologyEdit.committed);
    EXPECT_EQ(topologyEdit.error->graphError, GraphError::InvalidRoto);

    // A keyed write is the same kind of write, so the shared gesture path
    // refuses it too and publishes no channel.
    const ParameterGestureResult keyed =
        session.beginKeyedParameterGesture(0.0, {{position, vector2(5.0F, 6.0F)}}, EditOptions{session.revision(), {}});
    EXPECT_EQ(keyed.token, 0U);
    ASSERT_TRUE(keyed.result.error.has_value());
    EXPECT_EQ(keyed.result.error->graphError, GraphError::InvalidRoto);
    EXPECT_EQ(session.document().animationChannel(position), nullptr);

    // A locked shape cannot be removed either, through the same transition rule.
    RotoData pruned = authored;
    pruned.elements.erase(1);
    EXPECT_FALSE(publish(session, node, pruned));

    // Clearing the lock is the one accepted write, and it preserves the rest.
    RotoData unlocked = authored;
    unlocked.elements[1].locked = false;
    ASSERT_TRUE(publish(session, node, unlocked));
    ASSERT_NE(published(session, node), nullptr);
    EXPECT_FALSE(rotoElement(*published(session, node), rectangleId)->locked);
    ASSERT_NE(rotoElement(*published(session, node), groupId), nullptr);
    const EditResult afterUnlock =
        session.submit(setParametersCommand({draggedPoint}), EditOptions{session.revision(), {}});
    ASSERT_TRUE(afterUnlock.committed) << afterUnlock.error->message;
    EXPECT_EQ(animatedParameterValue(session.document(), position, 0.0), vector2(1.0F, 2.0F));
    EXPECT_FLOAT_EQ(rotoElement(*published(session, node), rectangleId)->points[0].position.value[0], 1.0F);
}

TEST_F(RotoTest, KeyedPointValuesResolveThroughTheSharedAnimationSampler) {
    ProjectSession session;
    const NodeId node = addRotoNode(session);
    const NetworkId network = session.document().rootNetworkId();
    const RotoData authored = authoredShapes();
    const RotoElementId rectangleId = authored.elements[1].id;
    const RotoPointId pointId = authored.elements[1].points[0].id;
    ASSERT_TRUE(publish(session, node, authored));
    const ParameterAddress position = pointAddress(network, node, kRotoParamPosition, rectangleId, pointId);

    const auto key = [&](double time, float x, float y) {
        const ParameterGestureResult begin =
            session.beginKeyedParameterGesture(time, {{position, vector2(x, y)}}, EditOptions{session.revision(), {}});
        EXPECT_NE(begin.token, 0U) << (begin.result.error ? begin.result.error->message : std::string{});
        EXPECT_TRUE(session.commitParameterGesture(begin.token, EditOptions{session.revision(), {}}).committed);
    };
    key(0.0, 0.0F, 0.0F);
    ASSERT_NE(session.document().animationChannel(position), nullptr);
    key(10.0, 100.0F, 50.0F);

    const RotoData atHalf = evaluateRoto(session.document(), network, node, 5.0);
    const RotoElement* evaluated = rotoElement(atHalf, rectangleId);
    ASSERT_NE(evaluated, nullptr);
    ASSERT_NE(rotoPoint(*evaluated, pointId), nullptr);
    EXPECT_FLOAT_EQ(rotoPoint(*evaluated, pointId)->position.value[0], 50.0F);
    EXPECT_FLOAT_EQ(rotoPoint(*evaluated, pointId)->position.value[1], 25.0F);
    // The authored value is not rewritten by a sampled query, and element
    // properties that were never keyed keep their authored value.
    EXPECT_FLOAT_EQ(rotoElement(*published(session, node), rectangleId)->points[0].position.value[0], 10.0F);
    EXPECT_FLOAT_EQ(evaluated->opacity, 1.0F);

    // The mixed value gesture routes an address that now has a channel to a key
    // at the gesture frame, so a numeric point drag reuses the same owner.
    const ParameterGestureResult mixed = session.beginValueParameterGesture(10.0, {{position, vector2(7.0F, 9.0F)}},
                                                                            EditOptions{session.revision(), {}});
    ASSERT_NE(mixed.token, 0U) << mixed.result.error->message;
    ASSERT_TRUE(session.commitParameterGesture(mixed.token, EditOptions{session.revision(), {}}).committed);
    const RotoData moved = evaluateRoto(session.document(), network, node, 10.0);
    EXPECT_FLOAT_EQ(rotoPoint(*rotoElement(moved, rectangleId), pointId)->position.value[0], 7.0F);

    // Retiring the shape retires the property's channel with it.
    RotoData withoutRectangle = authored;
    withoutRectangle.elements.erase(1);
    ASSERT_TRUE(publish(session, node, withoutRectangle));
    EXPECT_EQ(session.document().animationChannel(position), nullptr);
}

TEST_F(RotoTest, LifetimeHidesInactiveHierarchyFromEvaluation) {
    ProjectSession session;
    const NodeId node = addRotoNode(session);
    const NetworkId network = session.document().rootNetworkId();
    RotoData authored = authoredShapes();
    const RotoElementId groupId = authored.elements[0].id;
    const RotoElementId rectangleId = authored.elements[1].id;
    const RotoElementId ellipseId = authored.elements[2].id;
    authored.elements[rotoElementIndex(authored, groupId)].firstFrame = 24.0;
    ASSERT_TRUE(publish(session, node, authored));

    // The ellipse is inside its own lifetime at frame 12, but its group is not:
    // an absent group cannot contribute, so neither element is evaluated.
    const RotoData early = evaluateRoto(session.document(), network, node, 12.0);
    EXPECT_EQ(rotoElement(early, ellipseId), nullptr);
    EXPECT_EQ(rotoElement(early, groupId), nullptr);
    EXPECT_NE(rotoElement(early, rectangleId), nullptr);

    const RotoData late = evaluateRoto(session.document(), network, node, 30.0);
    EXPECT_NE(rotoElement(late, ellipseId), nullptr);
    EXPECT_NE(rotoElement(late, groupId), nullptr);
    EXPECT_EQ(late.nextElementId, authored.nextElementId);
    EXPECT_EQ(late.nextPointId, authored.nextPointId);

    // A node that authored no roto content evaluates to the empty value.
    const NodeId empty = addRotoNode(session, "Empty");
    EXPECT_TRUE(evaluateRoto(session.document(), network, empty, 0.0).elements.empty());
}

TEST_F(RotoTest, SaveAndReopenKeepIdentitiesGeometryChannelsAndCleanDirtyState) {
    ProjectSession session;
    const NodeId node = addRotoNode(session, "Roto");
    const NetworkId network = session.document().rootNetworkId();
    const RotoData authored = authoredShapes();
    const RotoElementId groupId = authored.elements[0].id;
    const RotoElementId rectangleId = authored.elements[1].id;
    const RotoPointId pointId = authored.elements[1].points[0].id;
    ASSERT_TRUE(publish(session, node, authored));

    ASSERT_TRUE(session
                    .submit(setParametersCommand(
                                {ParameterEdit{elementAddress(network, node, kRotoParamOpacity, rectangleId),
                                               ParameterValue{0.25}},
                                 ParameterEdit{pointAddress(network, node, kRotoParamTension, rectangleId, pointId),
                                               ParameterValue{0.5}}}),
                            EditOptions{session.revision(), {}})
                    .committed);

    const ParameterAddress position = pointAddress(network, node, kRotoParamPosition, rectangleId, pointId);
    const ParameterGestureResult keyed =
        session.beginKeyedParameterGesture(0.0, {{position, vector2(3.0F, 4.0F)}}, EditOptions{session.revision(), {}});
    ASSERT_NE(keyed.token, 0U) << keyed.result.error->message;
    ASSERT_TRUE(session.commitParameterGesture(keyed.token, EditOptions{session.revision(), {}}).committed);

    const RotoData beforeSave = *published(session, node);
    const std::uint64_t contentHash = rotoContentHash(beforeSave);
    const ParameterValue sampled = animatedParameterValue(session.document(), position, 0.0);

    const fs::path target = dir_ / "roto.nemo";
    const ProjectWriteRequest request = session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(request);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(request, written).committed);
    EXPECT_FALSE(session.isDirty());

    const ProjectReadResult reopened = ProjectFile::read(target);
    ASSERT_TRUE(reopened.ok) << reopened.error.message;
    const auto* instance = reopened.document.network(network).graph().node(node);
    ASSERT_NE(instance, nullptr);
    ASSERT_NE(instance->roto, nullptr);
    EXPECT_TRUE(rotoContentEquals(*instance->roto, beforeSave));
    EXPECT_EQ(rotoContentHash(*instance->roto), contentHash);
    EXPECT_EQ(instance->roto->nextElementId, beforeSave.nextElementId);
    EXPECT_EQ(instance->roto->nextPointId, beforeSave.nextPointId);
    ASSERT_NE(rotoElement(*instance->roto, groupId), nullptr);
    const RotoElement* storedRectangle = rotoElement(*instance->roto, rectangleId);
    ASSERT_NE(storedRectangle, nullptr);
    EXPECT_EQ(storedRectangle->opacity, 0.25);
    ASSERT_NE(rotoPoint(*storedRectangle, pointId), nullptr);
    EXPECT_EQ(rotoPoint(*storedRectangle, pointId)->tension, 0.5);
    EXPECT_NE(reopened.document.animationChannel(position), nullptr);
    EXPECT_EQ(animatedParameterValue(reopened.document, position, 0.0), sampled);
    EXPECT_EQ(rotoContentHash(evaluateRoto(reopened.document, network, node, 3.0)),
              rotoContentHash(evaluateRoto(session.document(), network, node, 3.0)));

    // The stored record is the same typed record the codec writes.
    EXPECT_EQ(rotoDataToJson(*instance->roto).at("version"), 1);
}

TEST_F(RotoTest, UnknownRotoFieldsAndFutureVersionsRoundTripOrAreRefused) {
    // A record from a newer build keeps its unknown fields at every level, so a
    // load/save cycle through this build loses nothing. The record states only
    // the fields it needs; every other property takes the model's default.
    const nlohmann::json stored = nlohmann::json::parse(R"json(
        {"version": 1,
         "nextElementId": 3,
         "nextPointId": 5,
         "futureRecordField": {"a": 1},
         "elements": [
           {"id": 1,
            "name": "Shape",
            "kind": "bezier",
            "featherProfile": "smooth",
            "futureElementField": "kept",
            "points": [
              {"id": 1, "position": [1.0, 2.0], "futurePointField": 7},
              {"id": 2, "position": [3.0, 4.0]},
              {"id": 3, "position": [5.0, 6.0]}]}]})json");

    const RotoData parsed = rotoDataFromJson(stored, "test roto");
    ASSERT_EQ(parsed.elements.size(), 1U);
    EXPECT_EQ(parsed.elements[0].kind, RotoKind::Bezier);
    EXPECT_EQ(parsed.elements[0].featherProfile, RotoFeatherProfile::Smooth);
    EXPECT_EQ(parsed.elements[0].blend, RotoBlend::Combine);
    EXPECT_TRUE(parsed.elements[0].visible);
    EXPECT_EQ(parsed.elements[0].points.size(), 3U);
    EXPECT_EQ(parsed.elements[0].extension.at("futureElementField"), "kept");
    EXPECT_EQ(parsed.extension.at("futureRecordField").at("a"), 1);
    EXPECT_EQ(parsed.elements[0].points[0].extension.at("futurePointField"), 7);

    const nlohmann::json encoded = rotoDataToJson(parsed);
    EXPECT_EQ(encoded.at("futureRecordField").at("a"), 1);
    EXPECT_EQ(encoded.at("elements").at(0).at("futureElementField"), "kept");
    EXPECT_EQ(encoded.at("elements").at(0).at("points").at(0).at("futurePointField"), 7);
    EXPECT_TRUE(rotoContentEquals(rotoDataFromJson(encoded, "test roto"), parsed));

    // A version this build does not model is refused rather than guessed, and a
    // structurally invalid record is refused with the model's own diagnostic.
    nlohmann::json future = encoded;
    future["version"] = 2;
    EXPECT_THROW(rotoDataFromJson(future, "test roto"), DeserializeError);
    nlohmann::json unclosed = encoded;
    unclosed["elements"].at(0)["points"].erase(1);
    unclosed["elements"].at(0)["points"].erase(1);
    EXPECT_THROW(rotoDataFromJson(unclosed, "test roto"), DeserializeError);
}

TEST_F(RotoTest, CopiedNodeKeepsRotoIdentitiesAndReparentingKeepsPointIdentities) {
    ProjectSession session;
    const NodeId node = addRotoNode(session, "Roto");
    const NetworkId network = session.document().rootNetworkId();
    const RotoData authored = authoredShapes();
    const RotoElementId groupId = authored.elements[0].id;
    const RotoElementId rectangleId = authored.elements[1].id;
    const RotoPointId pointId = authored.elements[1].points[0].id;
    ASSERT_TRUE(publish(session, node, authored));

    auto created = std::make_shared<std::vector<NodeId>>();
    const EditResult copy =
        session.submit(copySelectionCommand(network, {node}, network, LayoutPosition{40.0, 0.0}, created),
                       EditOptions{session.revision(), {}});
    ASSERT_TRUE(copy.committed) << copy.error->message;
    ASSERT_EQ(created->size(), 1U);
    const RotoData* copyRoto = published(session, created->front());
    ASSERT_NE(copyRoto, nullptr);
    EXPECT_TRUE(rotoContentEquals(*copyRoto, *published(session, node)));
    ASSERT_NE(rotoElement(*copyRoto, rectangleId), nullptr);
    EXPECT_EQ(rotoElement(*copyRoto, rectangleId)->points[0].id, pointId);

    // Reparenting an existing shape under the group preserves every identity, so
    // selection and animation channels survive the move.
    RotoData reparented = authored;
    reparented.elements[rotoElementIndex(reparented, rectangleId)].parent = groupId;
    ASSERT_TRUE(publish(session, node, reparented));
    const RotoData* live = published(session, node);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(rotoElement(*live, rectangleId)->parent, groupId);
    EXPECT_EQ(rotoElement(*live, rectangleId)->points[0].id, pointId);
    EXPECT_EQ(live->nextElementId, authored.nextElementId);
    EXPECT_EQ(live->nextPointId, authored.nextPointId);

    // Folding the node into a subnet carries the authored value into the new
    // definition network with its identities intact.
    auto collapseInstance = std::make_shared<NetworkInstanceId>();
    const RotoData beforeCollapse = *live;
    ASSERT_TRUE(session
                    .submit(collapseSelectionCommand(network, {node}, "Roto Subnet", collapseInstance),
                            EditOptions{session.revision(), {}})
                    .committed);
    const auto* folded = session.document().instance(*collapseInstance);
    ASSERT_NE(folded, nullptr);
    // The definition keeps the moved node's identity, so the folded shape is
    // addressed exactly as before the fold.
    const auto* foldedNode = session.document().network(folded->definition).graph().node(node);
    ASSERT_NE(foldedNode, nullptr);
    ASSERT_NE(foldedNode->roto, nullptr);
    EXPECT_TRUE(rotoContentEquals(*foldedNode->roto, beforeCollapse));
    EXPECT_EQ(foldedNode->roto->nextPointId, beforeCollapse.nextPointId);
}

TEST_F(RotoTest, ShapeHelpersAllocateEveryIdentityFromTheValueWatermarks) {
    RotoData data;
    const RotoElementId group = appendRotoGroup(data, kInvalidRotoElement, "Group");
    const RotoElementId path =
        appendRotoPath(data, RotoKind::BSpline, group, "Path",
                       {Vector2Value{{0.0F, 0.0F}}, Vector2Value{{10.0F, 0.0F}}, Vector2Value{{10.0F, 10.0F}}});
    const RotoElementId rectangle = appendRotoRectangle(data, group, "Rect", 0.0, 0.0, 8.0, 8.0);
    const RotoElementId ellipse = appendRotoEllipse(data, kInvalidRotoElement, "Ellipse", 4.0, 4.0, 2.0, 1.0);
    ASSERT_EQ(data.elements.size(), 4U);
    EXPECT_EQ(rotoElement(data, path)->kind, RotoKind::BSpline);
    EXPECT_EQ(rotoElement(data, path)->points.size(), 3U);
    EXPECT_EQ(rotoElement(data, rectangle)->points.size(), 4U);
    EXPECT_EQ(rotoElement(data, ellipse)->points.size(), 4U);
    EXPECT_TRUE(rotoElement(data, group)->points.empty());
    // The ellipse carries the arc tangent approximation; a rectangle keeps the
    // pen tool's zero tangents.
    EXPECT_NE(rotoElement(data, ellipse)->points[0].outTangent.value[1], 0.0F);
    EXPECT_FLOAT_EQ(rotoElement(data, rectangle)->points[0].inTangent.value[0], 0.0F);
    EXPECT_EQ(validateRotoData(data), std::nullopt);
    EXPECT_GT(data.nextElementId, ellipse);

    // Every allocated identity is unique and below the watermark it advanced.
    for (const auto& element : data.elements) {
        EXPECT_LT(element.id, data.nextElementId);
        for (const auto& point : element.points)
            EXPECT_LT(point.id, data.nextPointId);
    }
    EXPECT_THROW(appendRotoPath(data, RotoKind::Group, kInvalidRotoElement, "Bad", {}), GraphException);
    // A draft may be assembled point by point; the closed-shape requirement is
    // enforced when the value is published, not by the allocator.
    const RotoElementId draft =
        appendRotoPath(data, RotoKind::Bezier, kInvalidRotoElement, "Draft", {Vector2Value{}, Vector2Value{}});
    EXPECT_EQ(rotoElement(data, draft)->points.size(), 2U);
    EXPECT_NE(validateRotoData(data), std::nullopt);
    EXPECT_THROW(appendRotoPath(data, RotoKind::Bezier, kInvalidRotoElement, "Empty", {}), GraphException);
}

TEST_F(RotoTest, DeletingShapesCannotRewindIdentityAllocationThroughReplacement) {
    ProjectSession session;
    const auto node = addRotoNode(session);
    auto data = authoredShapes();
    ASSERT_TRUE(publish(session, node, data));
    data.elements.eraseIf([](const RotoElement&) { return true; });
    ASSERT_TRUE(publish(session, node, data));
    auto rewound = data;
    --rewound.nextElementId;
    EXPECT_FALSE(publish(session, node, rewound));
    rewound = data;
    --rewound.nextPointId;
    EXPECT_FALSE(publish(session, node, rewound));
    const auto id = appendRotoRectangle(data, 0, "New", 0, 0, 8, 8);
    ASSERT_TRUE(publish(session, node, data));
    EXPECT_EQ(id, rewound.nextElementId);
    EXPECT_GT(published(session, node)->elements[0].points[0].id, rewound.nextPointId);
}

TEST_F(RotoTest, KeyedFalloffRejectsTheSameZeroBoundaryAsStaticAuthoring) {
    ProjectSession session;
    const auto node = addRotoNode(session);
    const auto network = session.document().rootNetworkId();
    const auto shapes = authoredShapes();
    ASSERT_TRUE(publish(session, node, shapes));
    const auto address = elementAddress(network, node, "featherFalloff", shapes.elements[1].id);
    const auto revision = session.revision();
    const auto staticEdit = session.submit(setParametersCommand({{address, 0.0}}), {session.revision(), {}});
    EXPECT_FALSE(staticEdit.committed);
    ASSERT_TRUE(staticEdit.error);
    const auto keyedEdit =
        session.submit(setKeyframesCommand({{address, Keyframe{.time = 0, .value = 0.0}}}), {session.revision(), {}});
    EXPECT_FALSE(keyedEdit.committed);
    ASSERT_TRUE(keyedEdit.error);
    EXPECT_EQ(session.revision(), revision);
    EXPECT_EQ(session.document().animationChannel(address), nullptr);
}

}  // namespace
