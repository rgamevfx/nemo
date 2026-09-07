#include "Workspace.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <utility>

namespace nemo::workspace {

namespace {

constexpr int kMaxDepth = 64;

bool validOrientation(const std::string& s) {
    return s == "horizontal" || s == "vertical";
}

bool validPanelType(const std::string& s) {
    return s == "viewer" || s == "nodegraph" || s == "timeline";
}

bool validGroup(const std::string& s) {
    return s == "A" || s == "B" || s == "C" || s == "D" || s == "E";
}

bool validRatio(double r) {
    return r > 0.0 && r < 1.0;
}

Panel parsePanel(const nlohmann::json& j, std::set<std::string>& ids) {
    if (!j.is_object()) {
        throw std::runtime_error("workspace: panel entry is not an object");
    }
    Panel panel;
    if (!j.contains("id") || !j.at("id").is_string()) {
        throw std::runtime_error("workspace: panel has no string 'id'");
    }
    panel.id = j.at("id").get<std::string>();
    if (panel.id.empty()) {
        throw std::runtime_error("workspace: panel id must not be empty");
    }
    if (!ids.insert(panel.id).second) {
        throw std::runtime_error("workspace: duplicate id '" + panel.id + "'");
    }
    if (!j.contains("type") || !j.at("type").is_string()) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' has no string 'type'");
    }
    panel.type = j.at("type").get<std::string>();
    if (!validPanelType(panel.type)) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' has invalid type '" + panel.type + "'");
    }
    if (!j.contains("group") || !j.at("group").is_string()) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' has no string 'group'");
    }
    panel.group = j.at("group").get<std::string>();
    if (!validGroup(panel.group)) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' has invalid group '" + panel.group + "'");
    }
    return panel;
}

Node parseNode(const nlohmann::json& j, int depth, std::set<std::string>& ids) {
    if (depth > kMaxDepth) {
        throw std::runtime_error("workspace: layout exceeds maximum nesting depth");
    }
    if (!j.is_object()) {
        throw std::runtime_error("workspace: node is not an object");
    }
    Node node;
    if (!j.contains("id") || !j.at("id").is_string()) {
        throw std::runtime_error("workspace: node has no string 'id'");
    }
    node.id = j.at("id").get<std::string>();
    if (node.id.empty()) {
        throw std::runtime_error("workspace: node id must not be empty");
    }
    if (!ids.insert(node.id).second) {
        throw std::runtime_error("workspace: duplicate id '" + node.id + "'");
    }
    if (!j.contains("kind") || !j.at("kind").is_string()) {
        throw std::runtime_error("workspace: node '" + node.id + "' has no string 'kind'");
    }
    node.kind = j.at("kind").get<std::string>();

    if (node.kind == "split") {
        if (!j.contains("orientation") || !j.at("orientation").is_string()) {
            throw std::runtime_error("workspace: split '" + node.id + "' has no string 'orientation'");
        }
        node.orientation = j.at("orientation").get<std::string>();
        if (!validOrientation(node.orientation)) {
            throw std::runtime_error("workspace: split '" + node.id + "' has invalid orientation '" + node.orientation +
                                     "'");
        }
        if (!j.contains("ratio") || !j.at("ratio").is_number()) {
            throw std::runtime_error("workspace: split '" + node.id + "' has no numeric 'ratio'");
        }
        node.ratio = j.at("ratio").get<double>();
        if (!validRatio(node.ratio)) {
            throw std::runtime_error("workspace: split '" + node.id + "' ratio must be strictly between 0 and 1");
        }
        if (!j.contains("children") || !j.at("children").is_array() || j.at("children").size() != 2) {
            throw std::runtime_error("workspace: split '" + node.id + "' must have exactly two children");
        }
        for (const auto& child : j.at("children")) {
            node.children.push_back(parseNode(child, depth + 1, ids));
        }
    } else if (node.kind == "tabs") {
        if (!j.contains("panels") || !j.at("panels").is_array() || j.at("panels").empty()) {
            throw std::runtime_error("workspace: tabs '" + node.id + "' must have a non-empty 'panels' array");
        }
        for (const auto& panel : j.at("panels")) {
            node.panels.push_back(parsePanel(panel, ids));
        }
        if (!j.contains("active") || !j.at("active").is_string()) {
            throw std::runtime_error("workspace: tabs '" + node.id + "' has no string 'active'");
        }
        node.active = j.at("active").get<std::string>();
        const bool hasActive =
            std::any_of(node.panels.begin(), node.panels.end(), [&](const Panel& p) { return p.id == node.active; });
        if (!hasActive) {
            throw std::runtime_error("workspace: tabs '" + node.id + "' active panel '" + node.active +
                                     "' is not among its panels");
        }
    } else {
        throw std::runtime_error("workspace: node '" + node.id + "' has unknown kind '" + node.kind + "'");
    }
    return node;
}

nlohmann::json nodeToJson(const Node& node) {
    if (node.kind == "split") {
        nlohmann::json j = {
            {"id", node.id},
            {"kind", "split"},
            {"orientation", node.orientation},
            {"ratio", node.ratio},
            {"children", nlohmann::json::array()},
        };
        for (const auto& child : node.children) {
            j["children"].push_back(nodeToJson(child));
        }
        return j;
    }
    nlohmann::json panels = nlohmann::json::array();
    for (const auto& panel : node.panels) {
        panels.push_back({{"id", panel.id}, {"type", panel.type}, {"group", panel.group}});
    }
    return {{"id", node.id}, {"kind", "tabs"}, {"active", node.active}, {"panels", panels}};
}

Node* findNode(Node& node, const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    if (node.kind == "split") {
        for (auto& child : node.children) {
            if (Node* r = findNode(child, id)) {
                return r;
            }
        }
    }
    return nullptr;
}

Panel* findPanel(Node& node, const std::string& panelId) {
    if (node.kind == "tabs") {
        for (auto& panel : node.panels) {
            if (panel.id == panelId) {
                return &panel;
            }
        }
        return nullptr;
    }
    for (auto& child : node.children) {
        if (Panel* r = findPanel(child, panelId)) {
            return r;
        }
    }
    return nullptr;
}

Node* findLeafContaining(Node& node, const std::string& panelId) {
    if (node.kind == "tabs") {
        for (const auto& panel : node.panels) {
            if (panel.id == panelId) {
                return &node;
            }
        }
        return nullptr;
    }
    for (auto& child : node.children) {
        if (Node* r = findLeafContaining(child, panelId)) {
            return r;
        }
    }
    return nullptr;
}

// Returns the split node whose children directly contain nodeId, or nullptr
// when nodeId is the root (which has no parent).
Node* findParentSplit(Node& node, const std::string& nodeId) {
    if (node.kind != "split") {
        return nullptr;
    }
    for (auto& child : node.children) {
        if (child.id == nodeId) {
            return &node;
        }
        if (child.kind == "split") {
            if (Node* r = findParentSplit(child, nodeId)) {
                return r;
            }
        }
    }
    return nullptr;
}

// Returns the sibling of nodeId within its parent split, or nullptr when
// nodeId is the root. Collapsing an emptied leaf promotes exactly this
// sibling subtree.
Node* findSibling(Node& node, const std::string& nodeId) {
    Node* parent = findParentSplit(node, nodeId);
    if (!parent) {
        return nullptr;
    }
    for (std::size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].id == nodeId) {
            return &parent->children[1 - i];
        }
    }
    return nullptr;
}

void collapseEmpty(Node& node) {
    if (node.kind != "split") {
        return;
    }
    for (auto& child : node.children) {
        collapseEmpty(child);
    }
    node.children.erase(std::remove_if(node.children.begin(), node.children.end(),
                                       [](const Node& c) { return c.kind == "tabs" && c.panels.empty(); }),
                        node.children.end());
    if (node.children.size() == 1) {
        Node child = std::move(node.children.front());
        node = std::move(child);
    }
}

std::size_t countPanels(const Node& node) {
    if (node.kind == "tabs") {
        return node.panels.size();
    }
    std::size_t total = 0;
    for (const auto& child : node.children) {
        total += countPanels(child);
    }
    return total;
}

int nodeDepth(const Node& node, const std::string& id, int depth) {
    if (node.id == id) {
        return depth;
    }
    if (node.kind == "split") {
        for (const auto& child : node.children) {
            const int found = nodeDepth(child, id, depth + 1);
            if (found >= 0) {
                return found;
            }
        }
    }
    return -1;
}

}  // namespace

Workspace::Workspace() {
    Panel viewer;
    viewer.id = newId("panel");
    viewer.type = "viewer";
    viewer.group = "A";
    Node viewerLeaf;
    viewerLeaf.kind = "tabs";
    viewerLeaf.id = newId("leaf");
    viewerLeaf.panels.push_back(viewer);
    viewerLeaf.active = viewer.id;

    Panel nodegraph;
    nodegraph.id = newId("panel");
    nodegraph.type = "nodegraph";
    nodegraph.group = "A";
    Node nodegraphLeaf;
    nodegraphLeaf.kind = "tabs";
    nodegraphLeaf.id = newId("leaf");
    nodegraphLeaf.panels.push_back(nodegraph);
    nodegraphLeaf.active = nodegraph.id;

    Node topSplit;
    topSplit.kind = "split";
    topSplit.orientation = "horizontal";
    topSplit.ratio = 0.5;
    topSplit.id = newId("split");
    topSplit.children.push_back(std::move(viewerLeaf));
    topSplit.children.push_back(std::move(nodegraphLeaf));

    Panel timeline;
    timeline.id = newId("panel");
    timeline.type = "timeline";
    timeline.group = "A";
    Node timelineLeaf;
    timelineLeaf.kind = "tabs";
    timelineLeaf.id = newId("leaf");
    timelineLeaf.panels.push_back(timeline);
    timelineLeaf.active = timeline.id;

    root_.kind = "split";
    root_.orientation = "vertical";
    root_.ratio = 0.72;
    root_.id = newId("split");
    root_.children.push_back(std::move(topSplit));
    root_.children.push_back(std::move(timelineLeaf));
}

Workspace Workspace::fromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        throw std::runtime_error("workspace: root is not an object");
    }
    if (!json.contains("version") || !json.at("version").is_number_integer()) {
        throw std::runtime_error("workspace: 'version' must be an integer");
    }
    // Compare the JSON value directly rather than converting to int first:
    // converting a value that exceeds int range (e.g. 2^32 + 1) would
    // truncate and could pass a version check it must reject.
    if (json.at("version") != Workspace::kVersion) {
        throw std::runtime_error("workspace: unsupported version " + json.at("version").dump());
    }
    if (!json.contains("root")) {
        throw std::runtime_error("workspace: missing 'root'");
    }

    Workspace ws;
    std::set<std::string> ids;
    ws.root_ = parseNode(json.at("root"), 0, ids);
    ws.usedIds_.insert(ids.begin(), ids.end());

    // Advance the id counter past any numeric-suffixed id we just loaded so
    // freshly generated ids stay predictable; the used set is the hard
    // uniqueness guarantee regardless of the loaded id format.
    std::uint64_t maxSuffix = 0;
    for (const auto& id : ws.usedIds_) {
        std::size_t start = id.size();
        while (start > 0 && std::isdigit(static_cast<unsigned char>(id[start - 1]))) {
            --start;
        }
        if (start < id.size()) {
            try {
                const auto value = static_cast<std::uint64_t>(std::stoull(id.substr(start)));
                maxSuffix = std::max(maxSuffix, value);
            } catch (...) {
                // Non-numeric suffix; ignored.
            }
        }
    }
    ws.nextId_ = maxSuffix + 1;
    return ws;
}

nlohmann::json Workspace::toJson() const {
    return {{"version", Workspace::kVersion}, {"root", nodeToJson(root_)}};
}

void Workspace::split(const std::string& leafId, const std::string& orientation) {
    if (!validOrientation(orientation)) {
        throw std::runtime_error("split: invalid orientation '" + orientation + "'");
    }
    Node* leaf = findNode(root_, leafId);
    if (!leaf) {
        throw std::runtime_error("split: no node with id '" + leafId + "'");
    }
    if (leaf->kind != "tabs") {
        throw std::runtime_error("split: node '" + leafId + "' is not a tabs leaf");
    }
    // Splitting pushes the original leaf and the new leaf one level down, so
    // mirror the parser depth bound (kMaxDepth) to keep generated layouts
    // reloadable.
    const int depth = nodeDepth(root_, leafId, 0);
    if (depth + 1 > kMaxDepth) {
        throw std::runtime_error("split: would exceed maximum nesting depth");
    }

    Panel p;
    p.id = newId("panel");
    p.type = "viewer";
    p.group = "A";

    Node newLeaf;
    newLeaf.kind = "tabs";
    newLeaf.id = newId("leaf");
    newLeaf.panels.push_back(p);
    newLeaf.active = p.id;

    Node split;
    split.kind = "split";
    split.orientation = orientation;
    split.ratio = 0.5;
    split.id = newId("split");
    split.children.push_back(std::move(*leaf));
    split.children.push_back(std::move(newLeaf));

    *leaf = std::move(split);
    collapseEmpty(root_);
}

void Workspace::setRatio(const std::string& splitId, double ratio) {
    if (!validRatio(ratio)) {
        throw std::runtime_error("setRatio: ratio must be strictly between 0 and 1");
    }
    Node* node = findNode(root_, splitId);
    if (!node) {
        throw std::runtime_error("setRatio: no node with id '" + splitId + "'");
    }
    if (node->kind != "split") {
        throw std::runtime_error("setRatio: node '" + splitId + "' is not a split");
    }
    node->ratio = ratio;
}

void Workspace::setPanelType(const std::string& panelId, const std::string& type) {
    if (!validPanelType(type)) {
        throw std::runtime_error("setPanelType: invalid type '" + type + "'");
    }
    Panel* panel = findPanel(root_, panelId);
    if (!panel) {
        throw std::runtime_error("setPanelType: no panel with id '" + panelId + "'");
    }
    panel->type = type;
}

void Workspace::setGroup(const std::string& panelId, const std::string& group) {
    if (!validGroup(group)) {
        throw std::runtime_error("setGroup: invalid group '" + group + "'");
    }
    Panel* panel = findPanel(root_, panelId);
    if (!panel) {
        throw std::runtime_error("setGroup: no panel with id '" + panelId + "'");
    }
    panel->group = group;
}

void Workspace::activate(const std::string& leafId, const std::string& panelId) {
    Node* leaf = findNode(root_, leafId);
    if (!leaf) {
        throw std::runtime_error("activate: no node with id '" + leafId + "'");
    }
    if (leaf->kind != "tabs") {
        throw std::runtime_error("activate: node '" + leafId + "' is not a tabs leaf");
    }
    bool found = false;
    for (const auto& panel : leaf->panels) {
        if (panel.id == panelId) {
            found = true;
            break;
        }
    }
    if (!found) {
        throw std::runtime_error("activate: panel '" + panelId + "' is not in leaf '" + leafId + "'");
    }
    leaf->active = panelId;
}

void Workspace::addTab(const std::string& leafId, const std::string& type) {
    if (!validPanelType(type)) {
        throw std::runtime_error("addTab: invalid type '" + type + "'");
    }
    Node* leaf = findNode(root_, leafId);
    if (!leaf) {
        throw std::runtime_error("addTab: no node with id '" + leafId + "'");
    }
    if (leaf->kind != "tabs") {
        throw std::runtime_error("addTab: node '" + leafId + "' is not a tabs leaf");
    }
    Panel p;
    p.id = newId("panel");
    p.type = type;
    p.group = "A";
    leaf->panels.push_back(p);
    leaf->active = p.id;
}

void Workspace::closePanel(const std::string& panelId) {
    Node* leaf = findLeafContaining(root_, panelId);
    if (!leaf) {
        throw std::runtime_error("closePanel: no panel with id '" + panelId + "'");
    }
    // Only closing the last remaining panel of the whole workspace is
    // rejected. An emptied leaf is removed and its parent split collapses.
    if (countPanels(root_) <= 1) {
        throw std::runtime_error("closePanel: cannot close the last panel of the workspace");
    }
    auto it = std::find_if(leaf->panels.begin(), leaf->panels.end(), [&](const Panel& p) { return p.id == panelId; });
    usedIds_.erase(panelId);
    leaf->panels.erase(it);
    if (leaf->active == panelId && !leaf->panels.empty()) {
        leaf->active = leaf->panels.front().id;
    }
    collapseEmpty(root_);
}

bool Workspace::movePanel(const std::string& panelId, const MoveDestination& dest) {
    switch (dest.placement) {
    case Placement::Tabs:
    case Placement::Left:
    case Placement::Right:
    case Placement::Top:
    case Placement::Bottom:
        break;
    default:
        throw std::runtime_error("movePanel: invalid placement for leaf '" + dest.leafId + "'");
    }
    Node* srcLeaf = findLeafContaining(root_, panelId);
    if (!srcLeaf) {
        throw std::runtime_error("movePanel: no panel with id '" + panelId + "'");
    }
    Node* tgtLeaf = findNode(root_, dest.leafId);
    if (!tgtLeaf) {
        throw std::runtime_error("movePanel: no node with id '" + dest.leafId + "'");
    }
    if (tgtLeaf->kind != "tabs") {
        throw std::runtime_error("movePanel: node '" + dest.leafId + "' is not a tabs leaf");
    }

    const bool sameLeaf = (srcLeaf->id == tgtLeaf->id);

    // Validate the tab index for tab drops (edge placements ignore it).
    if (dest.placement == Placement::Tabs) {
        if (dest.tabIndex > tgtLeaf->panels.size()) {
            throw std::runtime_error("movePanel: tab insertion index " + std::to_string(dest.tabIndex) +
                                     " out of range for leaf '" + dest.leafId + "'");
        }
    }

    // A sole panel dropped onto its own tile cannot go anywhere: reject as a
    // no-op rather than splitting a leaf by draining its only content.
    if (sameLeaf && srcLeaf->panels.size() == 1) {
        return false;
    }

    // Edge drops create a split at the target leaf, pushing its children one
    // level deeper. The emptied source leaf may collapse first and promote the
    // sibling subtree, which can move the target leaf up one level; account
    // for that so we only reject genuinely too-deep layouts.
    if (dest.placement != Placement::Tabs) {
        int tgtDepth = nodeDepth(root_, dest.leafId, 0);
        if (!sameLeaf && srcLeaf->panels.size() == 1) {
            if (Node* sibling = findSibling(root_, srcLeaf->id)) {
                if (findNode(*sibling, dest.leafId)) {
                    --tgtDepth;
                }
            }
        }
        if (tgtDepth + 1 > kMaxDepth) {
            throw std::runtime_error("movePanel: would exceed maximum nesting depth");
        }
    }

    // Same-leaf tab drop: reorder. Inserting the panel is done after removing
    // it, so shift the insertion point toward the front when the source slot
    // was ahead of the gap.
    if (sameLeaf && dest.placement == Placement::Tabs) {
        auto it = std::find_if(srcLeaf->panels.begin(), srcLeaf->panels.end(),
                               [&](const Panel& p) { return p.id == panelId; });
        const std::size_t srcIndex = static_cast<std::size_t>(it - srcLeaf->panels.begin());
        const std::size_t insert = (dest.tabIndex > srcIndex) ? dest.tabIndex - 1 : dest.tabIndex;
        if (insert == srcIndex) {
            return false;
        }
        std::string active = panelId;
        Panel panel = std::move(*it);
        srcLeaf->panels.erase(it);
        srcLeaf->panels.insert(srcLeaf->panels.begin() + insert, std::move(panel));
        srcLeaf->active.swap(active);
        return true;
    }

    // Prepare allocations before extracting anything. The commit below uses
    // only moves into reserved storage, so a failed allocation cannot lose a panel.
    std::string targetActive = panelId;
    std::string sourceActive = srcLeaf->active;
    if (sourceActive == panelId && srcLeaf->panels.size() > 1) {
        sourceActive = srcLeaf->panels.front().id == panelId ? srcLeaf->panels[1].id : srcLeaf->panels.front().id;
    }
    Node panelLeaf;
    Node split;
    if (dest.placement == Placement::Tabs) {
        tgtLeaf->panels.reserve(tgtLeaf->panels.size() + 1);
    } else {
        panelLeaf.kind = "tabs";
        panelLeaf.id = newId("leaf");
        panelLeaf.panels.reserve(1);
        panelLeaf.active = panelId;
        split.kind = "split";
        split.orientation =
            (dest.placement == Placement::Left || dest.placement == Placement::Right) ? "horizontal" : "vertical";
        split.id = newId("split");
        split.children.reserve(2);
    }

    auto it =
        std::find_if(srcLeaf->panels.begin(), srcLeaf->panels.end(), [&](const Panel& p) { return p.id == panelId; });
    Panel panel = std::move(*it);
    srcLeaf->panels.erase(it);
    srcLeaf->active.swap(sourceActive);
    if (srcLeaf->panels.empty()) {
        collapseEmpty(root_);
    }

    Node* tgt = findNode(root_, dest.leafId);
    if (dest.placement == Placement::Tabs) {
        tgt->panels.insert(tgt->panels.begin() + dest.tabIndex, std::move(panel));
        tgt->active.swap(targetActive);
        return true;
    }

    panelLeaf.panels.push_back(std::move(panel));
    if (dest.placement == Placement::Left || dest.placement == Placement::Top) {
        split.children.push_back(std::move(panelLeaf));
        split.children.push_back(std::move(*tgt));
    } else {
        split.children.push_back(std::move(*tgt));
        split.children.push_back(std::move(panelLeaf));
    }
    *tgt = std::move(split);
    return true;
}

std::string Workspace::newId(const char* prefix) {
    std::string candidate;
    do {
        candidate = std::string(prefix) + "-" + std::to_string(nextId_++);
    } while (usedIds_.count(candidate) != 0);
    usedIds_.insert(candidate);
    return candidate;
}

}  // namespace nemo::workspace
