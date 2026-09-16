#include "nemo/nodes/Builtins.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace nemo {
namespace {

// The single explicit list, assembled once from the per-node factories.
[[nodiscard]] std::vector<NodeContribution> assembleBuiltins() {
    std::vector<NodeContribution> contributions;
    contributions.reserve(10);
#define NEMO_NODE(name) contributions.push_back(nodes::name##Contribution());
#include "nemo/nodes/BuiltinNodes.inc"
#undef NEMO_NODE
    return contributions;
}

}  // namespace

std::vector<NodeContribution> builtinContributions() {
    return assembleBuiltins();
}

std::shared_ptr<const NodeContributions> builtinNodeContributions() {
    // Built once, immutable thereafter; the schema catalog projection is the
    // authoritative built-in inventory (see builtinNodeCatalogPtr).
    static const auto contributions = std::make_shared<const NodeContributions>(assembleBuiltins());
    return contributions;
}

}  // namespace nemo
