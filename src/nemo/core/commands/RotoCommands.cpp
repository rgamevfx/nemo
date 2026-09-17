#include "nemo/core/commands/RotoCommands.hpp"

#include <string>
#include <utility>

namespace nemo {

Command setRotoDataCommand(NetworkId network, NodeId node, RotoData data) {
    return Command{"set roto data on node " + std::to_string(node),
                   [network, node, data = std::move(data)](Document& document) {
                       auto& graph = document.network(network).graph();
                       const NodeInstance* current = graph.node(node);
                       if (current == nullptr)
                           throw GraphException(GraphError::UnknownNode,
                                                "cannot set roto data on unknown node " + std::to_string(node));
                       if (current->roto) {
                           if (const auto problem = rotoTransitionProblem(*current->roto, data))
                               throw GraphException(GraphError::InvalidRoto,
                                                    "node '" + current->name + "' roto data: " + *problem);
                       }
                       // The graph owns the complete structural validation, so
                       // exactly one validator decides what may be published.
                       graph.setRoto(node, std::move(data));
                   }};
}

}  // namespace nemo
