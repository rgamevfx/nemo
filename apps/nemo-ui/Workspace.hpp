#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace nemo::workspace {

// Panel role in a tabs leaf. Groups are the compact A-E link badges from
// spec section 6 ("Panel linkage"); types are the panel kinds this build
// knows how to instantiate. Layout state is separate from project
// processing state: closing a panel neither deletes a network nor changes
// its output (spec section 10.6).
struct Panel {
    std::string id;
    std::string type;   // "viewer" | "nodegraph" | "timeline"
    std::string group;  // "A".."E"
};

// One node of the workspace split tree. A node is either a split (divides
// its rectangle between exactly two children) or a tabs leaf (holds a stack
// of panels with one active). Node and panel ids are globally unique.
struct Node {
    std::string id;
    std::string kind;  // "split" | "tabs"
    // Split fields.
    std::string orientation;  // "horizontal" | "vertical"
    double ratio{0.5};        // strictly between 0 and 1
    std::vector<Node> children;
    // Tabs fields.
    std::string active;  // id of the active panel
    std::vector<Panel> panels;
};

// Where a dragged panel is dropped. Tabs adds the panel to an existing leaf
// (as a tab at tabIndex); the edge placements split the leaf and place the
// panel in a fresh leaf on that edge (ratio 0.5).
enum class Placement {
    Tabs,
    Left,
    Right,
    Top,
    Bottom,
};

// Destination of a movePanel call. For Placement::Tabs, tabIndex is the
// insertion gap in the target leaf's PRE-MOVE tab list (range 0..size).
// Edge placements ignore tabIndex.
struct MoveDestination {
    std::string leafId;
    Placement placement{Placement::Tabs};
    std::size_t tabIndex{0};
};

// Serializable workspace layout (JSON v1). Pure data: no Qt, no viewer, no
// evaluation dependency. Mutating methods validate their arguments and
// throw std::runtime_error (via std::exception) without modifying state when
// the operation is invalid.
class Workspace {
public:
    static inline constexpr int kVersion = 1;

    // Default layout: viewer and nodegraph beside one another above a
    // timeline, all linked to group A.
    Workspace();

    // Strictly parse a JSON v1 layout. Throws std::runtime_error on malformed
    // input (bad version, duplicate ids, invalid ratios/orientations, invalid
    // types/groups, a tabs leaf with no valid active panel, or nesting beyond
    // the depth bound).
    [[nodiscard]] static Workspace fromJson(const nlohmann::json& json);

    // Serialize the current layout as {"version":1,"root":<node>}.
    [[nodiscard]] nlohmann::json toJson() const;

    // Split a tabs leaf into a new split, keeping the existing leaf as the
    // first child and adding a fresh viewer tab as the second child.
    void split(const std::string& leafId, const std::string& orientation);
    // Set the ratio of a split node. Ratio must be strictly between 0 and 1.
    void setRatio(const std::string& splitId, double ratio);
    // Change a panel's type.
    void setPanelType(const std::string& panelId, const std::string& type);
    // Change a panel's link group (A..E).
    void setGroup(const std::string& panelId, const std::string& group);
    // Activate a panel inside a tabs leaf. The panel must belong to the leaf.
    void activate(const std::string& leafId, const std::string& panelId);
    // Add a new tab (group A) to a tabs leaf and activate it.
    void addTab(const std::string& leafId, const std::string& type);
    // Close a panel. Reject closing the last panel of the whole workspace;
    // an emptied leaf is removed and its parent split collapses.
    void closePanel(const std::string& panelId);
    // Move a panel to a destination leaf. Returns true when the layout
    // changes and false for a no-op (a reorder onto the panel's own slot or a
    // sole panel dropped on its own tile). Invalid arguments (unknown panel,
    // unknown/non-tabs destination, out-of-range tabIndex, or an edge split
    // that would exceed the depth bound) throw std::runtime_error without
    // modifying state.
    bool movePanel(const std::string& panelId, const MoveDestination& dest);

private:
    std::string newId(const char* prefix);

    Node root_;
    std::uint64_t nextId_{1};
    std::unordered_set<std::string> usedIds_;
};

}  // namespace nemo::workspace
