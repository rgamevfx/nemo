#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "Workspace.hpp"

namespace {

using nemo::workspace::MoveDestination;
using nemo::workspace::Placement;
using nemo::workspace::Workspace;

// Build a chain of `levels` splits that wraps a tabs leaf `targetId`, with a
// distinct branch leaf and panel at each level so ids are globally unique.
nlohmann::json makeChain(const std::string& prefix, const std::string& targetId, int levels) {
    nlohmann::json node = {
        {"id", targetId},
        {"kind", "tabs"},
        {"active", targetId + "-p"},
        {"panels", nlohmann::json::array({{{"id", targetId + "-p"}, {"type", "viewer"}, {"group", "A"}}})}};
    for (int i = 1; i <= levels; ++i) {
        const std::string branchId = prefix + "-branch-" + std::to_string(i);
        node = nlohmann::json{
            {"id", prefix + "-split-" + std::to_string(i)},
            {"kind", "split"},
            {"orientation", "horizontal"},
            {"ratio", 0.5},
            {"children", nlohmann::json::array(
                             {node,
                              {{"id", branchId},
                               {"kind", "tabs"},
                               {"active", branchId + "-p"},
                               {"panels", nlohmann::json::array(
                                              {{{"id", branchId + "-p"}, {"type", "viewer"}, {"group", "A"}}})}}})}};
    }
    return node;
}

const nlohmann::json* findNode(const nlohmann::json& node, const std::string& id) {
    if (node.is_object() && node.value("id", std::string{}) == id) {
        return &node;
    }
    if (node.is_object() && node.value("kind", std::string{}) == "split") {
        for (const auto& child : node.at("children")) {
            if (const nlohmann::json* r = findNode(child, id)) {
                return r;
            }
        }
    }
    return nullptr;
}

const nlohmann::json* findPanel(const nlohmann::json& node, const std::string& id) {
    if (node.is_object() && node.value("kind", std::string{}) == "tabs") {
        for (const auto& panel : node.at("panels")) {
            if (panel.value("id", std::string{}) == id) {
                return &panel;
            }
        }
        return nullptr;
    }
    if (node.is_object() && node.value("kind", std::string{}) == "split") {
        for (const auto& child : node.at("children")) {
            if (const nlohmann::json* r = findPanel(child, id)) {
                return r;
            }
        }
    }
    return nullptr;
}

// Returns the split node whose children directly contain nodeId, or nullptr.
const nlohmann::json* findParentSplit(const nlohmann::json& node, const std::string& nodeId) {
    if (!node.is_object() || node.value("kind", std::string{}) != "split") {
        return nullptr;
    }
    for (const auto& child : node.at("children")) {
        if (child.value("id", std::string{}) == nodeId) {
            return &node;
        }
        if (const nlohmann::json* r = findParentSplit(child, nodeId)) {
            return r;
        }
    }
    return nullptr;
}

}  // namespace

TEST(WorkspaceTest, NonDefaultRoundTripPreservesStructure) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");

    const std::string rootSplit = root.at("id");
    const std::string topSplit = root.at("children").at(0).at("id");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");

    // Nested split: split the viewer leaf horizontally, keeping it as the first
    // child and adding a fresh viewer tab as the second child.
    ws.split(viewerLeaf, "horizontal");
    // Add a second tab to the nodegraph leaf; addTab activates the new tab.
    ws.addTab(nodegraphLeaf, "timeline");
    // Retarget and relink the original viewer panel.
    ws.setPanelType(viewerPanel, "nodegraph");
    ws.setGroup(viewerPanel, "C");
    // Re-activate the original nodegraph tab after addTab.
    ws.activate(nodegraphLeaf, nodegraphPanel);
    ws.setRatio(topSplit, 0.35);
    ws.setRatio(rootSplit, 0.72);

    const nlohmann::json doc = ws.toJson();
    const Workspace loaded = Workspace::fromJson(doc);
    EXPECT_EQ(loaded.toJson(), doc);
}

TEST(WorkspaceTest, MalformedLayoutThrows) {
    EXPECT_THROW((void)Workspace::fromJson(nlohmann::json::array()), std::exception);
    EXPECT_THROW((void)Workspace::fromJson(nlohmann::json{{"root", nlohmann::json::object()}}), std::exception);
    EXPECT_THROW((void)Workspace::fromJson(nlohmann::json{{"version", 2}, {"root", nlohmann::json::object()}}),
                 std::exception);
    // A version exceeding int range must not truncate to 1 and pass as valid.
    EXPECT_THROW((void)Workspace::fromJson(nlohmann::json{{"version", 4294967297}, {"root", nlohmann::json::object()}}),
                 std::exception);
    EXPECT_THROW((void)Workspace::fromJson(nlohmann::json{{"version", "one"}, {"root", nlohmann::json::object()}}),
                 std::exception);
    EXPECT_THROW((void)Workspace::fromJson(nlohmann::json{{"version", 1}}), std::exception);
}

TEST(WorkspaceTest, MalformedNodeThrows) {
    const auto tabs = [](const std::string& id, const std::string& panelId, const std::string& type = "viewer",
                         const std::string& group = "A") {
        return nlohmann::json{{"id", id},
                              {"kind", "tabs"},
                              {"active", panelId},
                              {"panels", nlohmann::json::array({{{"id", panelId}, {"type", type}, {"group", group}}})}};
    };
    const auto split = [](const std::string& id, const std::string& orientation, double ratio, const nlohmann::json& a,
                          const nlohmann::json& b) {
        return nlohmann::json{{"id", id},
                              {"kind", "split"},
                              {"orientation", orientation},
                              {"ratio", ratio},
                              {"children", nlohmann::json::array({a, b})}};
    };
    const auto doc = [](const nlohmann::json& root) { return nlohmann::json{{"version", 1}, {"root", root}}; };

    // Missing id / empty id.
    EXPECT_THROW((void)Workspace::fromJson(doc(tabs("", "p"))), std::exception);
    EXPECT_THROW((void)Workspace::fromJson(
                     doc({{"kind", "tabs"},
                          {"active", "p"},
                          {"panels", nlohmann::json::array({{{"id", "p"}, {"type", "viewer"}, {"group", "A"}}})}})),
                 std::exception);

    // Duplicate node id across the tree.
    EXPECT_THROW((void)Workspace::fromJson(doc(split("s", "vertical", 0.5, tabs("leaf", "p"), tabs("leaf", "q")))),
                 std::exception);
    // Panel id collides with a node id.
    EXPECT_THROW((void)Workspace::fromJson(doc(split("s", "vertical", 0.5, tabs("leaf", "leaf"), tabs("t", "q")))),
                 std::exception);
    // Duplicate panel id in one leaf.
    EXPECT_THROW((void)Workspace::fromJson(
                     doc({{"id", "leaf"},
                          {"kind", "tabs"},
                          {"active", "p"},
                          {"panels", nlohmann::json::array({{{"id", "p"}, {"type", "viewer"}, {"group", "A"}},
                                                            {{"id", "p"}, {"type", "viewer"}, {"group", "A"}}})}})),
                 std::exception);

    // Bad orientation.
    EXPECT_THROW((void)Workspace::fromJson(doc(split("s", "diagonal", 0.5, tabs("a", "p"), tabs("b", "q")))),
                 std::exception);
    // Bad ratio.
    EXPECT_THROW((void)Workspace::fromJson(doc(split("s", "vertical", 0.0, tabs("a", "p"), tabs("b", "q")))),
                 std::exception);
    EXPECT_THROW((void)Workspace::fromJson(doc(split("s", "vertical", 1.0, tabs("a", "p"), tabs("b", "q")))),
                 std::exception);
    // Split must have exactly two children.
    EXPECT_THROW((void)Workspace::fromJson(doc({{"id", "s"},
                                                {"kind", "split"},
                                                {"orientation", "vertical"},
                                                {"ratio", 0.5},
                                                {"children", nlohmann::json::array({tabs("a", "p")})}})),
                 std::exception);
    // Tabs must have a non-empty panels array.
    EXPECT_THROW((void)Workspace::fromJson(
                     doc({{"id", "l"}, {"kind", "tabs"}, {"active", "p"}, {"panels", nlohmann::json::array()}})),
                 std::exception);
    // Tabs active panel must be among the panels.
    EXPECT_THROW((void)Workspace::fromJson(
                     doc({{"id", "leaf"},
                          {"kind", "tabs"},
                          {"active", "missing"},
                          {"panels", nlohmann::json::array({{{"id", "p"}, {"type", "viewer"}, {"group", "A"}}})}})),
                 std::exception);
    // Unknown non-empty types are recoverable extension metadata; empty types are malformed.
    EXPECT_THROW((void)Workspace::fromJson(doc(tabs("leaf", "p", "", "A"))), std::exception);
    // Invalid group.
    EXPECT_THROW((void)Workspace::fromJson(doc(tabs("leaf", "p", "viewer", "Z"))), std::exception);
    // Unknown kind.
    EXPECT_THROW((void)Workspace::fromJson(
                     doc({{"id", "n"},
                          {"kind", "curve"},
                          {"active", "p"},
                          {"panels", nlohmann::json::array({{{"id", "p"}, {"type", "viewer"}, {"group", "A"}}})}})),
                 std::exception);
}

TEST(WorkspaceTest, ExcessiveDepthRejected) {
    nlohmann::json node =
        nlohmann::json{{"id", "leaf-0"},
                       {"kind", "tabs"},
                       {"active", "p0"},
                       {"panels", nlohmann::json::array({{{"id", "p0"}, {"type", "viewer"}, {"group", "A"}}})}};
    for (int i = 1; i <= 70; ++i) {
        node = nlohmann::json{
            {"id", "split-" + std::to_string(i)},
            {"kind", "split"},
            {"orientation", "horizontal"},
            {"ratio", 0.5},
            {"children",
             nlohmann::json::array(
                 {node,
                  {{"id", "leaf-" + std::to_string(i)},
                   {"kind", "tabs"},
                   {"active", "p" + std::to_string(i)},
                   {"panels", nlohmann::json::array(
                                  {{{"id", "p" + std::to_string(i)}, {"type", "viewer"}, {"group", "A"}}})}}})}};
    }
    const nlohmann::json doc = {{"version", 1}, {"root", node}};
    EXPECT_THROW((void)Workspace::fromJson(doc), std::exception);
}

TEST(WorkspaceTest, SplitCreatesNestedSplitAndPreservesOriginalLeaf) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string topSplit = root.at("children").at(0).at("id");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");

    ws.split(viewerLeaf, "horizontal");
    const nlohmann::json doc = ws.toJson();

    // The original viewer leaf was replaced in place by a new split, so
    // the top split's first child is now the nested split.
    const nlohmann::json* top = findNode(doc.at("root"), topSplit);
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->at("kind"), "split");
    const nlohmann::json& nested = top->at("children").at(0);
    ASSERT_EQ(nested.at("kind"), "split");
    ASSERT_EQ(nested.at("children").size(), 2u);
    // The original viewer leaf survives as the first child, panel intact.
    EXPECT_EQ(nested.at("children").at(0).at("id"), viewerLeaf);
    EXPECT_EQ(nested.at("children").at(0).at("panels").at(0).at("id"), viewerPanel);
    // The new second child is a fresh leaf with a viewer tab.
    const nlohmann::json& newLeaf = nested.at("children").at(1);
    EXPECT_EQ(newLeaf.at("kind"), "tabs");
    ASSERT_EQ(newLeaf.at("panels").size(), 1u);
    EXPECT_EQ(newLeaf.at("panels").at(0).at("type"), "viewer");
}

TEST(WorkspaceTest, ClosePanelRemovesAndReactivates) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string timelineLeaf = root.at("children").at(1).at("children").at(1).at("id");
    const std::string timelinePanel = root.at("children").at(1).at("children").at(1).at("panels").at(0).at("id");

    ws.addTab(timelineLeaf, "nodegraph");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* leaf = findNode(added.at("root"), timelineLeaf);
    ASSERT_NE(leaf, nullptr);
    ASSERT_EQ(leaf->at("panels").size(), 2u);
    const std::string addedPanel = leaf->at("panels").at(1).at("id");
    // addTab activated the new panel.
    EXPECT_EQ(leaf->at("active"), addedPanel);

    ws.closePanel(addedPanel);
    const nlohmann::json doc = ws.toJson();
    const nlohmann::json* after = findNode(doc.at("root"), timelineLeaf);
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->at("panels").size(), 1u);
    EXPECT_EQ(after->at("panels").at(0).at("id"), timelinePanel);
    // Active switched back to the remaining panel.
    EXPECT_EQ(after->at("active"), timelinePanel);
}

TEST(WorkspaceTest, CloseLastPanelRejected) {
    // A single-panel workspace: the only panel is the final workspace panel.
    Workspace ws = Workspace::fromJson(
        nlohmann::json{{"version", 1},
                       {"root",
                        {{"id", "leaf"},
                         {"kind", "tabs"},
                         {"active", "p"},
                         {"panels", nlohmann::json::array({{{"id", "p"}, {"type", "viewer"}, {"group", "A"}}})}}}});
    const nlohmann::json before = ws.toJson();
    EXPECT_THROW(ws.closePanel("p"), std::exception);
    // The offending operation must not modify state.
    EXPECT_EQ(ws.toJson(), before);
}

TEST(WorkspaceTest, ClosingLeafLastPanelCollapsesSplit) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");
    const std::string timelinePanel = root.at("children").at(1).at("children").at(1).at("panels").at(0).at("id");

    // Closing the only panel of the viewer leaf is allowed (other panels
    // remain); the emptied leaf is removed and the top horizontal split
    // collapses, preserving nodegraph and timeline.
    ws.closePanel(viewerPanel);
    const nlohmann::json doc = ws.toJson();
    const nlohmann::json& r = doc.at("root");
    ASSERT_EQ(r.at("kind"), "split");
    ASSERT_EQ(r.at("orientation"), "vertical");
    ASSERT_EQ(r.at("children").size(), 2u);

    EXPECT_EQ(findNode(doc.at("root"), viewerLeaf), nullptr);
    const nlohmann::json* ng = findPanel(doc.at("root"), nodegraphPanel);
    ASSERT_NE(ng, nullptr);
    const nlohmann::json* tl = findPanel(doc.at("root"), timelinePanel);
    ASSERT_NE(tl, nullptr);
}

TEST(WorkspaceTest, SplitRespectsMaxDepth) {
    // Build a valid layout whose deepest leaf sits exactly at the parser
    // depth bound (64). Splitting it would exceed the bound and produce a
    // layout that could not be reloaded, so it must be rejected.
    constexpr int kDepth = 64;
    nlohmann::json node =
        nlohmann::json{{"id", "leaf-0"},
                       {"kind", "tabs"},
                       {"active", "p0"},
                       {"panels", nlohmann::json::array({{{"id", "p0"}, {"type", "viewer"}, {"group", "A"}}})}};
    for (int i = 1; i <= kDepth; ++i) {
        node = nlohmann::json{
            {"id", "split-" + std::to_string(i)},
            {"kind", "split"},
            {"orientation", "horizontal"},
            {"ratio", 0.5},
            {"children",
             nlohmann::json::array(
                 {node,
                  {{"id", "leaf-" + std::to_string(i)},
                   {"kind", "tabs"},
                   {"active", "p" + std::to_string(i)},
                   {"panels", nlohmann::json::array(
                                  {{{"id", "p" + std::to_string(i)}, {"type", "viewer"}, {"group", "A"}}})}}})}};
    }
    Workspace ws = Workspace::fromJson({{"version", 1}, {"root", node}});
    EXPECT_THROW(ws.split("leaf-0", "horizontal"), std::exception);
    // A shallow leaf in the same chain can still be split.
    ws.split("leaf-" + std::to_string(kDepth), "horizontal");
}

TEST(WorkspaceTest, InvalidOperationsDoNotModifyState) {
    Workspace ws;
    const nlohmann::json before = ws.toJson();
    const nlohmann::json& root = before.at("root");
    const std::string topSplit = root.at("children").at(0).at("id");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");

    // Split a split node (must be a tabs leaf).
    EXPECT_THROW(ws.split(topSplit, "horizontal"), std::exception);
    // setRatio on a tabs leaf.
    EXPECT_THROW(ws.setRatio(viewerLeaf, 0.5), std::exception);
    // setRatio out of bounds.
    EXPECT_THROW(ws.setRatio(topSplit, 0.0), std::exception);
    EXPECT_THROW(ws.setRatio(topSplit, 1.0), std::exception);
    // Empty panel types remain invalid; unknown non-empty types are recoverable.
    EXPECT_THROW(ws.setPanelType(viewerPanel, ""), std::exception);
    EXPECT_THROW(ws.setGroup(viewerPanel, "Z"), std::exception);
    // Activate a panel that is not in the leaf.
    EXPECT_THROW(ws.activate(viewerLeaf, nodegraphPanel), std::exception);
    // addTab with an empty type / on a split node.
    EXPECT_THROW(ws.addTab(viewerLeaf, ""), std::exception);
    EXPECT_THROW(ws.addTab(topSplit, "viewer"), std::exception);

    EXPECT_EQ(ws.toJson(), before);
}

TEST(WorkspaceTest, RatioTypeGroupAndActivationApply) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string topSplit = root.at("children").at(0).at("id");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");

    ws.setRatio(topSplit, 0.25);
    ws.setPanelType(viewerPanel, "timeline");
    ws.setGroup(viewerPanel, "E");

    const nlohmann::json doc = ws.toJson();
    const nlohmann::json* splitJson = findNode(doc.at("root"), topSplit);
    ASSERT_NE(splitJson, nullptr);
    EXPECT_EQ(splitJson->at("ratio").get<double>(), 0.25);

    const nlohmann::json* panelJson = findPanel(doc.at("root"), viewerPanel);
    ASSERT_NE(panelJson, nullptr);
    EXPECT_EQ(panelJson->at("type"), "timeline");
    EXPECT_EQ(panelJson->at("group"), "E");

    // Activate the same panel explicitly; the active field must reflect it.
    ws.activate(viewerLeaf, viewerPanel);
    const nlohmann::json activated = ws.toJson();
    const nlohmann::json* leafJson = findNode(activated.at("root"), viewerLeaf);
    ASSERT_NE(leafJson, nullptr);
    EXPECT_EQ(leafJson->at("active"), viewerPanel);
}

TEST(WorkspaceTest, MovePanelSameLeafReorderShiftsIndex) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");

    ws.addTab(nodegraphLeaf, "viewer");
    ws.addTab(nodegraphLeaf, "timeline");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* leaf = findNode(added.at("root"), nodegraphLeaf);
    ASSERT_NE(leaf, nullptr);
    ASSERT_EQ(leaf->at("panels").size(), 3u);
    const std::string p0 = leaf->at("panels").at(0).at("id");
    const std::string p1 = leaf->at("panels").at(1).at("id");
    const std::string p2 = leaf->at("panels").at(2).at("id");

    // Move the first tab to the position before the pre-move end (gap 3).
    EXPECT_TRUE(ws.movePanel(p0, MoveDestination{nodegraphLeaf, Placement::Tabs, 3}));

    const nlohmann::json doc = ws.toJson();
    const nlohmann::json* after = findNode(doc.at("root"), nodegraphLeaf);
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->at("panels").size(), 3u);
    EXPECT_EQ(after->at("panels").at(0).at("id"), p1);
    EXPECT_EQ(after->at("panels").at(1).at("id"), p2);
    EXPECT_EQ(after->at("panels").at(2).at("id"), p0);
    // The dragged tab becomes the active tab of its leaf.
    EXPECT_EQ(after->at("active"), p0);
}

TEST(WorkspaceTest, MovePanelSameLeafReorderNoOp) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");

    ws.addTab(nodegraphLeaf, "viewer");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* leaf = findNode(added.at("root"), nodegraphLeaf);
    ASSERT_NE(leaf, nullptr);
    ASSERT_EQ(leaf->at("panels").size(), 2u);
    const std::string p1 = leaf->at("panels").at(1).at("id");
    const nlohmann::json before = ws.toJson();

    // Dropping a tab onto its own slot (gap == source index) is a no-op.
    EXPECT_FALSE(ws.movePanel(p1, MoveDestination{nodegraphLeaf, Placement::Tabs, 1}));
    EXPECT_EQ(ws.toJson(), before);
    // Dropping it in the gap immediately after itself is also a no-op.
    EXPECT_FALSE(ws.movePanel(p1, MoveDestination{nodegraphLeaf, Placement::Tabs, 2}));
    EXPECT_EQ(ws.toJson(), before);
}

TEST(WorkspaceTest, MovePanelCrossLeafTabInsert) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");

    // Give the viewer leaf a second tab so moving the original out does not
    // empty (and collapse) the source leaf.
    ws.addTab(viewerLeaf, "timeline");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* viewerJson = findNode(added.at("root"), viewerLeaf);
    ASSERT_NE(viewerJson, nullptr);
    ASSERT_EQ(viewerJson->at("panels").size(), 2u);
    const std::string viewerSecond = viewerJson->at("panels").at(1).at("id");

    // Move the original viewer tab to the front of the nodegraph leaf.
    EXPECT_TRUE(ws.movePanel(viewerPanel, MoveDestination{nodegraphLeaf, Placement::Tabs, 0}));
    const nlohmann::json doc = ws.toJson();

    const nlohmann::json* ng = findNode(doc.at("root"), nodegraphLeaf);
    ASSERT_NE(ng, nullptr);
    ASSERT_EQ(ng->at("panels").size(), 2u);
    EXPECT_EQ(ng->at("panels").at(0).at("id"), viewerPanel);
    EXPECT_EQ(ng->at("panels").at(1).at("id"), nodegraphPanel);
    // The dropped tab becomes active in the destination leaf.
    EXPECT_EQ(ng->at("active"), viewerPanel);

    const nlohmann::json* vl = findNode(doc.at("root"), viewerLeaf);
    ASSERT_NE(vl, nullptr);
    ASSERT_EQ(vl->at("panels").size(), 1u);
    EXPECT_EQ(vl->at("panels").at(0).at("id"), viewerSecond);
    // The source leaf keeps its (unmoved) active tab.
    EXPECT_EQ(vl->at("active"), viewerSecond);
}

TEST(WorkspaceTest, MovePanelCrossLeafTabAppend) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");

    // Append the viewer tab to the end of the nodegraph leaf.
    EXPECT_TRUE(ws.movePanel(viewerPanel, MoveDestination{nodegraphLeaf, Placement::Tabs, 1}));
    const nlohmann::json doc = ws.toJson();
    const nlohmann::json* ng = findNode(doc.at("root"), nodegraphLeaf);
    ASSERT_NE(ng, nullptr);
    ASSERT_EQ(ng->at("panels").size(), 2u);
    EXPECT_EQ(ng->at("panels").at(0).at("id"), nodegraphPanel);
    EXPECT_EQ(ng->at("panels").at(1).at("id"), viewerPanel);
    EXPECT_EQ(ng->at("active"), viewerPanel);

    // Moving the viewer leaf's only panel out empties it; the parent split
    // collapses and no empty/orphan leaf survives.
    EXPECT_EQ(findNode(doc.at("root"), viewerLeaf), nullptr);
}

TEST(WorkspaceTest, MovePanelSoleOwnTileNoOp) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const nlohmann::json before = ws.toJson();

    // A sole panel dropped onto its own tile cannot be moved anywhere.
    EXPECT_FALSE(ws.movePanel(viewerPanel, MoveDestination{viewerLeaf, Placement::Tabs, 0}));
    EXPECT_FALSE(ws.movePanel(viewerPanel, MoveDestination{viewerLeaf, Placement::Left, 0}));
    EXPECT_FALSE(ws.movePanel(viewerPanel, MoveDestination{viewerLeaf, Placement::Bottom, 0}));
    EXPECT_EQ(ws.toJson(), before);
}

TEST(WorkspaceTest, MovePanelSourceLeafCollapse) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");
    const std::string timelineLeaf = root.at("children").at(1).at("children").at(1).at("id");
    const std::string timelinePanel = root.at("children").at(1).at("children").at(1).at("panels").at(0).at("id");

    // The nodegraph leaf holds a sole panel; moving it to the timeline leaf
    // empties it, the inner horizontal split collapses, and nodegraph is
    // appended to the timeline leaf.
    EXPECT_TRUE(ws.movePanel(nodegraphPanel, MoveDestination{timelineLeaf, Placement::Tabs, 1}));
    const nlohmann::json doc = ws.toJson();

    EXPECT_EQ(findNode(doc.at("root"), nodegraphLeaf), nullptr);
    const nlohmann::json* tl = findNode(doc.at("root"), timelineLeaf);
    ASSERT_NE(tl, nullptr);
    ASSERT_EQ(tl->at("panels").size(), 2u);
    EXPECT_EQ(tl->at("panels").at(0).at("id"), timelinePanel);
    EXPECT_EQ(tl->at("panels").at(1).at("id"), nodegraphPanel);
    EXPECT_EQ(tl->at("active"), nodegraphPanel);
    // The root keeps exactly two children (viewer leaf + timeline leaf).
    const nlohmann::json& r = doc.at("root");
    ASSERT_EQ(r.at("kind"), "split");
    ASSERT_EQ(r.at("children").size(), 2u);
}

TEST(WorkspaceTest, MovePanelEdgeSplitsAllDirections) {
    struct Case {
        Placement placement;
        std::string orientation;
        bool movedFirst;
    };
    const Case cases[] = {
        {Placement::Left, "horizontal", true},
        {Placement::Right, "horizontal", false},
        {Placement::Top, "vertical", true},
        {Placement::Bottom, "vertical", false},
    };

    for (const Case& c : cases) {
        Workspace ws;
        const nlohmann::json original = ws.toJson();
        const nlohmann::json& root = original.at("root");
        const std::string timelineLeaf = root.at("children").at(1).at("children").at(1).at("id");
        const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");
        const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");

        // Keep the source (timeline) leaf non-empty so it does not collapse.
        ws.addTab(timelineLeaf, "viewer");
        const nlohmann::json added = ws.toJson();
        const nlohmann::json* tl = findNode(added.at("root"), timelineLeaf);
        ASSERT_NE(tl, nullptr);
        const std::string moved = tl->at("panels").at(1).at("id");

        EXPECT_TRUE(ws.movePanel(moved, MoveDestination{nodegraphLeaf, c.placement, 0}));
        const nlohmann::json doc = ws.toJson();

        // The original nodegraph leaf survives, but is now a child of a fresh
        // split (which carries the same id it had as the leaf position).
        const nlohmann::json* split = findParentSplit(doc.at("root"), nodegraphLeaf);
        ASSERT_NE(split, nullptr);
        ASSERT_EQ(split->at("kind"), "split");
        ASSERT_EQ(split->at("children").size(), 2u);
        EXPECT_EQ(split->at("orientation"), c.orientation);
        EXPECT_EQ(split->at("ratio").get<double>(), 0.5);

        const auto childHasPanel = [](const nlohmann::json& child, const std::string& id) {
            if (child.value("kind", std::string{}) != "tabs") {
                return false;
            }
            for (const auto& p : child.at("panels")) {
                if (p.value("id", std::string{}) == id) {
                    return true;
                }
            }
            return false;
        };
        const std::size_t movedIdx = c.movedFirst ? 0 : 1;
        const std::size_t otherIdx = 1 - movedIdx;
        // The moved panel sits on the requested edge, the original leaf content
        // on the other side.
        EXPECT_TRUE(childHasPanel(split->at("children").at(movedIdx), moved));
        EXPECT_TRUE(childHasPanel(split->at("children").at(otherIdx), nodegraphPanel));
        // The original nodegraph leaf still holds only its original panel.
        const nlohmann::json& other = split->at("children").at(otherIdx);
        ASSERT_EQ(other.at("panels").size(), 1u);
        EXPECT_EQ(other.at("panels").at(0).at("id"), nodegraphPanel);
    }
}

TEST(WorkspaceTest, MovePanelSameLeafEdgeSplit) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");

    ws.addTab(viewerLeaf, "timeline");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* vl = findNode(added.at("root"), viewerLeaf);
    ASSERT_NE(vl, nullptr);
    const std::string viewerSecond = vl->at("panels").at(1).at("id");

    // A multi-panel leaf can split itself: dropping one tab on its own left
    // edge moves it into a fresh leaf on the left.
    EXPECT_TRUE(ws.movePanel(viewerPanel, MoveDestination{viewerLeaf, Placement::Left, 0}));
    const nlohmann::json doc = ws.toJson();
    const nlohmann::json* split = findParentSplit(doc.at("root"), viewerLeaf);
    ASSERT_NE(split, nullptr);
    ASSERT_EQ(split->at("kind"), "split");
    ASSERT_EQ(split->at("children").size(), 2u);
    EXPECT_EQ(split->at("orientation"), "horizontal");

    const nlohmann::json& left = split->at("children").at(0);
    ASSERT_EQ(left.at("kind"), "tabs");
    ASSERT_EQ(left.at("panels").size(), 1u);
    EXPECT_EQ(left.at("panels").at(0).at("id"), viewerPanel);
    EXPECT_EQ(left.at("active"), viewerPanel);

    const nlohmann::json& right = split->at("children").at(1);
    ASSERT_EQ(right.at("kind"), "tabs");
    ASSERT_EQ(right.at("panels").size(), 1u);
    EXPECT_EQ(right.at("panels").at(0).at("id"), viewerSecond);
    EXPECT_EQ(right.at("active"), viewerSecond);
}

TEST(WorkspaceTest, MovePanelInvalidRejectedNoPartialState) {
    Workspace ws;
    const nlohmann::json before = ws.toJson();
    const nlohmann::json& root = before.at("root");
    const std::string topSplit = root.at("children").at(0).at("id");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");

    EXPECT_THROW((void)ws.movePanel("missing", MoveDestination{viewerLeaf, Placement::Tabs, 0}), std::exception);
    EXPECT_THROW((void)ws.movePanel(viewerPanel, MoveDestination{"missing", Placement::Tabs, 0}), std::exception);
    // Target is a split node, not a tabs leaf.
    EXPECT_THROW((void)ws.movePanel(viewerPanel, MoveDestination{topSplit, Placement::Tabs, 0}), std::exception);
    // tabIndex outside the pre-move 0..size range.
    EXPECT_THROW((void)ws.movePanel(viewerPanel, MoveDestination{nodegraphLeaf, Placement::Tabs, 99}), std::exception);
    EXPECT_THROW((void)ws.movePanel(viewerPanel, MoveDestination{nodegraphLeaf, static_cast<Placement>(99), 0}),
                 std::exception);
    EXPECT_EQ(ws.toJson(), before);
}

TEST(WorkspaceTest, MovePanelDepthBoundRejected) {
    // A leaf at exactly depth 64 that is NOT promoted by the source collapse
    // (the source keeps a panel) must reject an edge split.
    const nlohmann::json doc = {
        {"version", 1},
        {"root",
         {{"id", "root"},
          {"kind", "split"},
          {"orientation", "horizontal"},
          {"ratio", 0.5},
          {"children",
           nlohmann::json::array(
               {makeChain("chain", "target", 63),
                {{"id", "src"},
                 {"kind", "tabs"},
                 {"active", "src-p1"},
                 {"panels", nlohmann::json::array({{{"id", "src-p1"}, {"type", "viewer"}, {"group", "A"}},
                                                   {{"id", "src-p2"}, {"type", "viewer"}, {"group", "A"}}})}}})}}}};
    Workspace ws = Workspace::fromJson(doc);
    const nlohmann::json before = ws.toJson();
    EXPECT_THROW((void)ws.movePanel("src-p1", MoveDestination{"target", Placement::Left, 0}), std::exception);
    EXPECT_EQ(ws.toJson(), before);
}

TEST(WorkspaceTest, MovePanelDepthBoundAfterSourceCollapseAllowed) {
    // A source leaf whose sole panel is moved collapses and promotes the
    // sibling subtree, dropping the target from depth 64 to 63 so the edge
    // split may proceed.
    const nlohmann::json doc = {
        {"version", 1},
        {"root",
         {{"id", "root"},
          {"kind", "split"},
          {"orientation", "horizontal"},
          {"ratio", 0.5},
          {"children",
           nlohmann::json::array(
               {makeChain("chain", "target", 63),
                {{"id", "src"},
                 {"kind", "tabs"},
                 {"active", "src-p1"},
                 {"panels", nlohmann::json::array({{{"id", "src-p1"}, {"type", "viewer"}, {"group", "A"}}})}}})}}}};
    Workspace ws = Workspace::fromJson(doc);
    EXPECT_TRUE(ws.movePanel("src-p1", MoveDestination{"target", Placement::Left, 0}));
    // The generated layout must reload: the split stays within the depth bound
    // and no empty leaf remains.
    const Workspace loaded = Workspace::fromJson(ws.toJson());
    EXPECT_EQ(loaded.toJson(), ws.toJson());
}

TEST(WorkspaceTest, MovePanelPreservesIdentityAndUnaffectedRatios) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string rootSplit = root.at("id");
    const std::string topSplit = root.at("children").at(0).at("id");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");

    // Tag the panel and record the unaffected ratios.
    ws.setPanelType(viewerPanel, "nodegraph");
    ws.setGroup(viewerPanel, "D");
    ws.setRatio(rootSplit, 0.71);
    ws.setRatio(topSplit, 0.33);

    ws.addTab(viewerLeaf, "timeline");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* vl = findNode(added.at("root"), viewerLeaf);
    const std::string viewerSecond = vl->at("panels").at(1).at("id");

    // Move the tagged panel to the nodegraph leaf; the source keeps a tab so
    // it does not collapse.
    EXPECT_TRUE(ws.movePanel(viewerPanel, MoveDestination{nodegraphLeaf, Placement::Tabs, 0}));
    const nlohmann::json doc = ws.toJson();

    const nlohmann::json* panel = findPanel(doc.at("root"), viewerPanel);
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->at("type"), "nodegraph");
    EXPECT_EQ(panel->at("group"), "D");

    // Unaffected split ratios must be preserved exactly.
    const nlohmann::json* rRoot = findNode(doc.at("root"), rootSplit);
    const nlohmann::json* rTop = findNode(doc.at("root"), topSplit);
    ASSERT_NE(rRoot, nullptr);
    ASSERT_NE(rTop, nullptr);
    EXPECT_EQ(rRoot->at("ratio").get<double>(), 0.71);
    EXPECT_EQ(rTop->at("ratio").get<double>(), 0.33);
}

TEST(WorkspaceTest, MovePanelMultipleMovesReload) {
    Workspace ws;
    const nlohmann::json original = ws.toJson();
    const nlohmann::json& root = original.at("root");
    const std::string viewerLeaf = root.at("children").at(0).at("children").at(0).at("id");
    const std::string nodegraphLeaf = root.at("children").at(0).at("children").at(1).at("id");
    const std::string timelineLeaf = root.at("children").at(1).at("children").at(1).at("id");
    const std::string viewerPanel = root.at("children").at(0).at("children").at(0).at("panels").at(0).at("id");
    const std::string nodegraphPanel = root.at("children").at(0).at("children").at(1).at("panels").at(0).at("id");

    ws.addTab(viewerLeaf, "timeline");
    const nlohmann::json added = ws.toJson();
    const nlohmann::json* vl = findNode(added.at("root"), viewerLeaf);
    const std::string viewerSecond = vl->at("panels").at(1).at("id");

    // A mixed sequence: reorder, cross-leaf tab insert, and an edge split.
    EXPECT_TRUE(ws.movePanel(viewerSecond, MoveDestination{viewerLeaf, Placement::Tabs, 0}));
    EXPECT_TRUE(ws.movePanel(nodegraphPanel, MoveDestination{timelineLeaf, Placement::Tabs, 1}));
    // The nodegraph leaf collapsed, so split the (surviving) timeline leaf.
    EXPECT_TRUE(ws.movePanel(viewerPanel, MoveDestination{timelineLeaf, Placement::Right, 0}));

    const nlohmann::json doc = ws.toJson();
    const Workspace loaded = Workspace::fromJson(doc);
    EXPECT_EQ(loaded.toJson(), doc);
}

TEST(WorkspaceTest, UnknownPanelTypeAndMetadataRoundTripForRecovery) {
    const nlohmann::json layout = {{"version", 1},
                                   {"root",
                                    {{"id", "leaf"},
                                     {"kind", "tabs"},
                                     {"active", "extension-panel"},
                                     {"panels", nlohmann::json::array({{{"id", "extension-panel"},
                                                                        {"type", "com.example.extension"},
                                                                        {"group", "C"},
                                                                        {"extensionVersion", 7},
                                                                        {"instanceMetadata", {{"mode", "demo"}}},
                                                                        {"state", {{"zoom", 1.25}}}}})}}}};

    const Workspace restored = Workspace::fromJson(layout);
    EXPECT_EQ(restored.toJson(), layout);
}

TEST(WorkspaceTest, DuplicateRemapsEveryIdentityAndPreservesPanelState) {
    Workspace workspace;
    const auto original = workspace.toJson();
    const auto duplicate = workspace.duplicateWithFreshIds().toJson();

    EXPECT_NE(duplicate.at("root").at("id"), original.at("root").at("id"));
    EXPECT_EQ(Workspace::fromJson(duplicate).toJson(), duplicate);
    EXPECT_EQ(duplicate.at("root").at("children").size(), original.at("root").at("children").size());
}
