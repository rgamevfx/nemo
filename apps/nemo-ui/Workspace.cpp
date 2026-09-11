#include "Workspace.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace nemo::workspace {

namespace {

constexpr int kMaxDepth = 64;

bool validOrientation(const std::string& s) {
    return s == "horizontal" || s == "vertical";
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
    if (panel.type.empty()) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' type must not be empty");
    }
    if (!j.contains("group") || !j.at("group").is_string()) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' has no string 'group'");
    }
    panel.group = j.at("group").get<std::string>();
    if (!validGroup(panel.group)) {
        throw std::runtime_error("workspace: panel '" + panel.id + "' has invalid group '" + panel.group + "'");
    }

    panel.metadata = nlohmann::json::object();
    for (const auto& [key, value] : j.items()) {
        if (key != "id" && key != "type" && key != "group" && key != "state") {
            panel.metadata[key] = value;
        }
    }
    if (j.contains("state")) {
        panel.state = j.at("state");
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

nlohmann::json panelToJson(const Panel& panel) {
    nlohmann::json json = panel.metadata.is_object() ? panel.metadata : nlohmann::json::object();
    json["id"] = panel.id;
    json["type"] = panel.type;
    json["group"] = panel.group;
    if (panel.state.has_value()) {
        json["state"] = *panel.state;
    }
    return json;
}

nlohmann::json nodeToJson(const Node& node) {
    if (node.kind == "split") {
        nlohmann::json json = {{"id", node.id},
                               {"kind", "split"},
                               {"orientation", node.orientation},
                               {"ratio", node.ratio},
                               {"children", nlohmann::json::array()}};
        for (const auto& child : node.children) {
            json["children"].push_back(nodeToJson(child));
        }
        return json;
    }
    nlohmann::json panels = nlohmann::json::array();
    for (const auto& panel : node.panels) {
        panels.push_back(panelToJson(panel));
    }
    return {{"id", node.id}, {"kind", "tabs"}, {"active", node.active}, {"panels", panels}};
}

Node* findNode(Node& node, const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    if (node.kind == "split") {
        for (auto& child : node.children) {
            if (Node* result = findNode(child, id)) {
                return result;
            }
        }
    }
    return nullptr;
}

const Node* findNode(const Node& node, const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    if (node.kind == "split") {
        for (const auto& child : node.children) {
            if (const Node* result = findNode(child, id)) {
                return result;
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
        if (Panel* result = findPanel(child, panelId)) {
            return result;
        }
    }
    return nullptr;
}

const Panel* findPanel(const Node& node, const std::string& panelId) {
    if (node.kind == "tabs") {
        for (const auto& panel : node.panels) {
            if (panel.id == panelId) {
                return &panel;
            }
        }
        return nullptr;
    }
    for (const auto& child : node.children) {
        if (const Panel* result = findPanel(child, panelId)) {
            return result;
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
        if (Node* result = findLeafContaining(child, panelId)) {
            return result;
        }
    }
    return nullptr;
}

Node* findParentSplit(Node& node, const std::string& nodeId) {
    if (node.kind != "split") {
        return nullptr;
    }
    for (auto& child : node.children) {
        if (child.id == nodeId) {
            return &node;
        }
        if (child.kind == "split") {
            if (Node* result = findParentSplit(child, nodeId)) {
                return result;
            }
        }
    }
    return nullptr;
}

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
                                       [](const Node& child) { return child.kind == "tabs" && child.panels.empty(); }),
                        node.children.end());
    if (node.children.size() == 1) {
        Node replacement = std::move(node.children.front());
        node = std::move(replacement);
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

void remapIds(Node& node, std::function<std::string(const char*)> newId,
              std::unordered_map<std::string, std::string>& ids) {
    const std::string oldNodeId = node.id;
    node.id = newId("node");
    ids.emplace(oldNodeId, node.id);
    if (node.kind == "split") {
        for (auto& child : node.children) {
            remapIds(child, newId, ids);
        }
        return;
    }
    const std::string oldActive = node.active;
    for (auto& panel : node.panels) {
        const std::string oldPanelId = panel.id;
        panel.id = newId("panel");
        ids.emplace(oldPanelId, panel.id);
    }
    node.active = ids.at(oldActive);
}

}  // namespace

Workspace::Workspace() {
    Panel viewer{newId("panel"), "viewer", "A", nlohmann::json::object(), std::nullopt};
    Node viewerLeaf;
    viewerLeaf.kind = "tabs";
    viewerLeaf.id = newId("leaf");
    viewerLeaf.active = viewer.id;
    viewerLeaf.panels.push_back(std::move(viewer));

    Panel nodegraph{newId("panel"), "nodegraph", "A", nlohmann::json::object(), std::nullopt};
    Node nodegraphLeaf;
    nodegraphLeaf.kind = "tabs";
    nodegraphLeaf.id = newId("leaf");
    nodegraphLeaf.active = nodegraph.id;
    nodegraphLeaf.panels.push_back(std::move(nodegraph));

    Node topSplit;
    topSplit.kind = "split";
    topSplit.orientation = "horizontal";
    topSplit.ratio = 0.5;
    topSplit.id = newId("split");
    topSplit.children.push_back(std::move(viewerLeaf));
    topSplit.children.push_back(std::move(nodegraphLeaf));

    Panel timeline{newId("panel"), "timeline", "A", nlohmann::json::object(), std::nullopt};
    Node timelineLeaf;
    timelineLeaf.kind = "tabs";
    timelineLeaf.id = newId("leaf");
    timelineLeaf.active = timeline.id;
    timelineLeaf.panels.push_back(std::move(timeline));

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
    if (json.at("version") != Workspace::kVersion) {
        throw std::runtime_error("workspace: unsupported version " + json.at("version").dump());
    }
    if (!json.contains("root")) {
        throw std::runtime_error("workspace: missing 'root'");
    }

    Workspace workspace;
    std::set<std::string> ids;
    workspace.root_ = parseNode(json.at("root"), 0, ids);
    workspace.usedIds_.insert(ids.begin(), ids.end());
    std::uint64_t maxSuffix = 0;
    for (const auto& id : workspace.usedIds_) {
        std::size_t start = id.size();
        while (start > 0 && std::isdigit(static_cast<unsigned char>(id[start - 1]))) {
            --start;
        }
        if (start < id.size()) {
            try {
                maxSuffix = std::max(maxSuffix, static_cast<std::uint64_t>(std::stoull(id.substr(start))));
            } catch (...) {
            }
        }
    }
    workspace.nextId_ = maxSuffix + 1;
    return workspace;
}

nlohmann::json Workspace::toJson() const {
    return {{"version", Workspace::kVersion}, {"root", nodeToJson(root_)}};
}

Workspace Workspace::duplicateWithFreshIds() const {
    Workspace duplicate;
    duplicate.root_ = root_;
    duplicate.usedIds_.clear();
    duplicate.nextId_ = 1;
    std::unordered_map<std::string, std::string> ids;
    const auto allocate = [&duplicate](const char* prefix) { return duplicate.newId(prefix); };
    remapIds(duplicate.root_, allocate, ids);
    return duplicate;
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
    if (nodeDepth(root_, leafId, 0) + 1 > kMaxDepth) {
        throw std::runtime_error("split: would exceed maximum nesting depth");
    }
    Panel panel{newId("panel"), "viewer", "A", nlohmann::json::object(), std::nullopt};
    Node newLeaf;
    newLeaf.kind = "tabs";
    newLeaf.id = newId("leaf");
    newLeaf.active = panel.id;
    newLeaf.panels.push_back(std::move(panel));

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
    if (type.empty()) {
        throw std::runtime_error("setPanelType: type must not be empty");
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
    const bool found =
        std::any_of(leaf->panels.begin(), leaf->panels.end(), [&](const Panel& panel) { return panel.id == panelId; });
    if (!found) {
        throw std::runtime_error("activate: panel '" + panelId + "' is not in leaf '" + leafId + "'");
    }
    leaf->active = panelId;
}

void Workspace::addTab(const std::string& leafId, const std::string& type) {
    static_cast<void>(createPanel(leafId, type, "A"));
}

std::string Workspace::createPanel(const std::string& leafId, const std::string& type, const std::string& group) {
    if (type.empty()) {
        throw std::runtime_error("createPanel: type must not be empty");
    }
    if (!validGroup(group)) {
        throw std::runtime_error("createPanel: invalid group '" + group + "'");
    }
    Node* leaf = findNode(root_, leafId);
    if (!leaf) {
        throw std::runtime_error("createPanel: no node with id '" + leafId + "'");
    }
    if (leaf->kind != "tabs") {
        throw std::runtime_error("createPanel: node '" + leafId + "' is not a tabs leaf");
    }
    Panel panel{newId("panel"), type, group, nlohmann::json::object(), std::nullopt};
    const std::string id = panel.id;
    leaf->panels.push_back(std::move(panel));
    leaf->active = id;
    return id;
}

void Workspace::setPanelState(const std::string& panelId, const nlohmann::json& state) {
    Panel* panel = findPanel(root_, panelId);
    if (!panel) {
        throw std::runtime_error("setPanelState: no panel with id '" + panelId + "'");
    }
    nlohmann::json next = state;
    panel->state = std::move(next);
}

nlohmann::json Workspace::panelState(const std::string& panelId) const {
    const Panel* panel = findPanel(root_, panelId);
    if (!panel) {
        throw std::runtime_error("panelState: no panel with id '" + panelId + "'");
    }
    return panel->state.value_or(nlohmann::json::object());
}

void Workspace::closePanel(const std::string& panelId) {
    Node* leaf = findLeafContaining(root_, panelId);
    if (!leaf) {
        throw std::runtime_error("closePanel: no panel with id '" + panelId + "'");
    }
    if (countPanels(root_) <= 1) {
        throw std::runtime_error("closePanel: cannot close the last panel of the workspace");
    }
    auto it =
        std::find_if(leaf->panels.begin(), leaf->panels.end(), [&](const Panel& panel) { return panel.id == panelId; });
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
    const bool sameLeaf = srcLeaf->id == tgtLeaf->id;
    if (dest.placement == Placement::Tabs && dest.tabIndex > tgtLeaf->panels.size()) {
        throw std::runtime_error("movePanel: tab insertion index " + std::to_string(dest.tabIndex) +
                                 " out of range for leaf '" + dest.leafId + "'");
    }
    if (sameLeaf && srcLeaf->panels.size() == 1) {
        return false;
    }
    if (dest.placement != Placement::Tabs) {
        int targetDepth = nodeDepth(root_, dest.leafId, 0);
        if (!sameLeaf && srcLeaf->panels.size() == 1) {
            if (Node* sibling = findSibling(root_, srcLeaf->id); sibling && findNode(*sibling, dest.leafId)) {
                --targetDepth;
            }
        }
        if (targetDepth + 1 > kMaxDepth) {
            throw std::runtime_error("movePanel: would exceed maximum nesting depth");
        }
    }

    if (sameLeaf && dest.placement == Placement::Tabs) {
        auto it = std::find_if(srcLeaf->panels.begin(), srcLeaf->panels.end(),
                               [&](const Panel& panel) { return panel.id == panelId; });
        const std::size_t sourceIndex = static_cast<std::size_t>(it - srcLeaf->panels.begin());
        const std::size_t insertion = dest.tabIndex > sourceIndex ? dest.tabIndex - 1 : dest.tabIndex;
        if (insertion == sourceIndex) {
            return false;
        }
        Panel panel = std::move(*it);
        srcLeaf->panels.erase(it);
        srcLeaf->panels.insert(srcLeaf->panels.begin() + insertion, std::move(panel));
        srcLeaf->active = panelId;
        return true;
    }

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
        panelLeaf.active = panelId;
        panelLeaf.panels.reserve(1);
        split.kind = "split";
        split.orientation =
            dest.placement == Placement::Left || dest.placement == Placement::Right ? "horizontal" : "vertical";
        split.id = newId("split");
        split.children.reserve(2);
    }

    auto it = std::find_if(srcLeaf->panels.begin(), srcLeaf->panels.end(),
                           [&](const Panel& panel) { return panel.id == panelId; });
    Panel panel = std::move(*it);
    srcLeaf->panels.erase(it);
    srcLeaf->active = sourceActive;
    if (srcLeaf->panels.empty()) {
        collapseEmpty(root_);
    }

    Node* target = findNode(root_, dest.leafId);
    if (dest.placement == Placement::Tabs) {
        target->panels.insert(target->panels.begin() + dest.tabIndex, std::move(panel));
        target->active = targetActive;
        return true;
    }
    panelLeaf.panels.push_back(std::move(panel));
    if (dest.placement == Placement::Left || dest.placement == Placement::Top) {
        split.children.push_back(std::move(panelLeaf));
        split.children.push_back(std::move(*target));
    } else {
        split.children.push_back(std::move(*target));
        split.children.push_back(std::move(panelLeaf));
    }
    *target = std::move(split);
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
