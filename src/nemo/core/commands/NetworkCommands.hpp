#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"

namespace nemo {

// Hierarchy edits are ordinary Command values so ProjectSession and the
// headless CLI publish one document/history transaction for each operation.
Command collapseSelectionCommand(NetworkId network, std::vector<NodeId> selected, std::string name,
                                 std::shared_ptr<NetworkInstanceId> created = {});
Command unpackInstanceCommand(NetworkInstanceId instance);

Command renameInterfaceCommand(NetworkId network, PortDirection direction, InterfacePortId port, std::string name);
Command promoteParameterCommand(NetworkId network, NodeId node, std::string key, std::string exposedName,
                                std::shared_ptr<InterfacePortId> created = {}, std::optional<std::size_t> index = {});
Command renameExposedParameterCommand(NetworkId network, InterfacePortId parameter, std::string name);
Command removeExposedParameterCommand(NetworkId network, InterfacePortId parameter);
Command moveExposedParameterCommand(NetworkId network, InterfacePortId parameter, std::size_t index);
Command promoteInterfaceCommand(NetworkId network, PortDirection direction, PortKind kind, std::string name,
                                std::shared_ptr<InterfacePortId> created = {});
Command setInterfaceLayoutCommand(NetworkId network, PortDirection direction, InterfacePortId port,
                                  LayoutPosition layout);
Command disconnectInputCommand(NetworkId network, InterfacePortId input, PortRef destination);
Command disconnectOutputCommand(NetworkId network, InterfacePortId output);
Command replaceInputConnectionCommand(NetworkId network, InterfacePortId input, PortRef destination);
Command replaceOutputConnectionCommand(NetworkId network, InterfacePortId output, PortRef source);
Command connectInputToOutputCommand(NetworkId network, InterfacePortId input, InterfacePortId output);
Command bindInstanceInputToParentTerminalCommand(NetworkInstanceId instance, InterfacePortId input,
                                                 InterfacePortId parentInput);
Command unbindInstanceInputCommand(NetworkInstanceId instance, InterfacePortId input);

Command createLinkedInstanceCommand(NetworkId parentNetwork, NetworkId definition, std::string name,
                                    LayoutPosition position = {}, std::shared_ptr<NetworkInstanceId> created = {});
Command makeIndependentCommand(NetworkInstanceId instance, std::shared_ptr<NetworkId> createdDefinition = {});

// Copies selected nodes and their wholly internal edges. Owned local subnets
// are copied independently; explicitly linked definitions and source media
// retain their references.
Command copySelectionCommand(NetworkId sourceNetwork, std::vector<NodeId> selected, NetworkId destinationNetwork,
                             LayoutPosition offset = {}, std::shared_ptr<std::vector<NodeId>> created = {});

}  // namespace nemo
