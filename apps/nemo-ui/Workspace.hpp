#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace nemo::workspace {

// A panel is presentation-only metadata. `metadata` contains every unknown
// field from the serialized record, while `state` is the optional panel-owned
// state object persisted by the workspace shell.
struct Panel {
    std::string id;
    std::string type;
    std::string group;
    nlohmann::json metadata = nlohmann::json::object();
    std::optional<nlohmann::json> state;
};

struct Node {
    std::string id;
    std::string kind;
    std::string orientation;
    double ratio{0.5};
    std::vector<Node> children;
    std::string active;
    std::vector<Panel> panels;
};

enum class Placement {
    Tabs,
    Left,
    Right,
    Top,
    Bottom,
};

struct MoveDestination {
    std::string leafId;
    Placement placement{Placement::Tabs};
    std::size_t tabIndex{0};
};

// Serializable, Qt-free workspace arrangement and panel-role state.
class Workspace {
public:
    static inline constexpr int kVersion = 1;

    Workspace();

    [[nodiscard]] static Workspace fromJson(const nlohmann::json& json);
    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] Workspace duplicateWithFreshIds() const;

    void split(const std::string& leafId, const std::string& orientation);
    void setRatio(const std::string& splitId, double ratio);
    // Pure Workspace accepts any non-empty stable panel type. Registry
    // validation belongs to WorkspaceController.
    void setPanelType(const std::string& panelId, const std::string& type);
    void setGroup(const std::string& panelId, const std::string& group);
    void activate(const std::string& leafId, const std::string& panelId);
    void addTab(const std::string& leafId, const std::string& type);
    [[nodiscard]] std::string createPanel(const std::string& leafId, const std::string& type,
                                          const std::string& group = "A");
    void setPanelState(const std::string& panelId, const nlohmann::json& state);
    [[nodiscard]] nlohmann::json panelState(const std::string& panelId) const;
    void closePanel(const std::string& panelId);
    [[nodiscard]] bool movePanel(const std::string& panelId, const MoveDestination& dest);

private:
    std::string newId(const char* prefix);

    Node root_;
    std::uint64_t nextId_{1};
    std::unordered_set<std::string> usedIds_;
};

}  // namespace nemo::workspace
