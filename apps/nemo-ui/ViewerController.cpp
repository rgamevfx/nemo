#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

#include <QFileInfo>
#include <QJSValue>
#include <QMetaType>
#include <QQuickWindow>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
namespace nemo::ui {
namespace {
const char* parameterTypeName(nemo::ParameterType type) {
    switch (type) {
    case nemo::ParameterType::Boolean:
        return "boolean";
    case nemo::ParameterType::Integer:
        return "integer";
    case nemo::ParameterType::Float:
        return "float";
    case nemo::ParameterType::Choice:
        return "choice";
    case nemo::ParameterType::Vector2:
        return "vector2";
    case nemo::ParameterType::Vector3:
        return "vector3";
    case nemo::ParameterType::Color:
        return "color";
    case nemo::ParameterType::String:
        return "string";
    }
    return "string";
}

const char* portKindName(nemo::PortKind kind) {
    switch (kind) {
    case nemo::PortKind::Image:
        return "image";
    case nemo::PortKind::Mask:
        return "mask";
    case nemo::PortKind::Media:
        return "media";
    }
    return "image";
}

QVariant parameterValueVariant(const nemo::ParameterValue& value) {
    return std::visit(
        [](const auto& current) -> QVariant {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, bool>)
                return QVariant{current};
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return QVariant::fromValue<qlonglong>(static_cast<qlonglong>(current));
            else if constexpr (std::is_same_v<T, double>)
                return QVariant{current};
            else if constexpr (std::is_same_v<T, std::string>)
                return QString::fromStdString(current);
            else if constexpr (std::is_same_v<T, nemo::ChoiceValue>)
                return QString::fromStdString(current.value);
            else {
                QVariantList list;
                for (const float component : current.value)
                    list.push_back(component);
                return list;
            }
        },
        value);
}

bool isIntegerVariant(const QVariant& value) {
    switch (value.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

bool isRealVariant(const QVariant& value) {
    switch (value.metaType().id()) {
    case QMetaType::Float:
    case QMetaType::Double:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

std::optional<nemo::ParameterValue> parameterValueFromVariant(const nemo::NodeCatalog& catalog,
                                                              const nemo::NodeDescriptor* descriptor,
                                                              std::string_view key, const QVariant& value,
                                                              QString& error) {
    if (value.metaType().id() == QMetaType::ULongLong &&
        value.toULongLong() > static_cast<qulonglong>(std::numeric_limits<std::int64_t>::max())) {
        error = QStringLiteral("integer value is outside the signed 64-bit range");
        return std::nullopt;
    }
    const auto* spec = descriptor ? catalog.parameterSpec(descriptor->type, key) : nullptr;
    if (!spec) {
        switch (value.metaType().id()) {
        case QMetaType::Bool:
            return nemo::ParameterValue{value.toBool()};
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            return nemo::ParameterValue{static_cast<std::int64_t>(value.toLongLong())};
        case QMetaType::Float:
        case QMetaType::Double:
            return nemo::ParameterValue{value.toDouble()};
        case QMetaType::QString:
            return nemo::ParameterValue{value.toString().toStdString()};
        default:
            error = QStringLiteral("unsupported value type for unknown parameter");
            return std::nullopt;
        }
    }

    nemo::ParameterValue converted;
    switch (spec->type) {
    case nemo::ParameterType::Boolean:
        if (value.metaType().id() != QMetaType::Bool) {
            error = QStringLiteral("boolean parameter requires a boolean value");
            return std::nullopt;
        }
        converted = value.toBool();
        break;
    case nemo::ParameterType::Integer:
        if (!isIntegerVariant(value)) {
            if (value.metaType().id() != QMetaType::Double) {
                error = QStringLiteral("integer parameter requires an integer value");
                return std::nullopt;
            }
            const double number = value.toDouble();
            if (!std::isfinite(number) || std::trunc(number) != number ||
                number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                number >= 9223372036854775808.0) {
                error = QStringLiteral("integer parameter requires an integer value");
                return std::nullopt;
            }
            converted = static_cast<std::int64_t>(number);
        } else {
            converted = static_cast<std::int64_t>(value.toLongLong());
        }
        break;
    case nemo::ParameterType::Float:
        if (!isRealVariant(value)) {
            error = QStringLiteral("float parameter requires a numeric value");
            return std::nullopt;
        }
        converted = value.toDouble();
        break;
    case nemo::ParameterType::Choice:
        if (value.metaType().id() != QMetaType::QString) {
            error = QStringLiteral("choice parameter requires a string value");
            return std::nullopt;
        }
        converted = nemo::ChoiceValue{value.toString().toStdString()};
        break;
    case nemo::ParameterType::Vector2:
    case nemo::ParameterType::Vector3:
    case nemo::ParameterType::Color: {
        // QML passes JS arrays as QJSValue; accept both that and a native
        // QVariantList so scripted and presentation callers behave identically.
        QVariantList list;
        if (value.metaType().id() == QMetaType::QVariantList) {
            list = value.toList();
        } else if (value.metaType().id() == qMetaTypeId<QJSValue>() && value.value<QJSValue>().isArray()) {
            list = value.value<QJSValue>().toVariant().toList();
        } else {
            error = QStringLiteral("vector and color parameters require a numeric list");
            return std::nullopt;
        }
        const int expected = spec->type == nemo::ParameterType::Vector2   ? 2
                             : spec->type == nemo::ParameterType::Vector3 ? 3
                                                                          : 4;
        if (list.size() != expected || std::any_of(list.cbegin(), list.cend(), [](const QVariant& component) {
                return !isRealVariant(component) || !std::isfinite(component.toDouble());
            })) {
            error = QStringLiteral("vector and color parameters require the exact finite component count");
            return std::nullopt;
        }
        if (expected == 2)
            converted = nemo::Vector2Value{
                {static_cast<float>(list.at(0).toDouble()), static_cast<float>(list.at(1).toDouble())}};
        else if (expected == 3)
            converted = nemo::Vector3Value{{static_cast<float>(list.at(0).toDouble()),
                                            static_cast<float>(list.at(1).toDouble()),
                                            static_cast<float>(list.at(2).toDouble())}};
        else
            converted = nemo::ColorValue{
                {static_cast<float>(list.at(0).toDouble()), static_cast<float>(list.at(1).toDouble()),
                 static_cast<float>(list.at(2).toDouble()), static_cast<float>(list.at(3).toDouble())}};
        break;
    }
    case nemo::ParameterType::String:
        if (value.metaType().id() != QMetaType::QString) {
            error = QStringLiteral("string parameter requires a string value");
            return std::nullopt;
        }
        converted = value.toString().toStdString();
        break;
    }
    if (const auto problem = catalog.validateParameter(descriptor->type, key, converted)) {
        error = QString::fromStdString(*problem);
        return std::nullopt;
    }
    return converted;
}

std::optional<std::uint64_t> graphIdentity(const QVariant& value) {
    bool ok = false;
    const QString text = value.toString().trimmed();
    if (text.isEmpty())
        return std::nullopt;
    const auto id = text.toULongLong(&ok);
    if (!ok || id == 0)
        return std::nullopt;
    return id;
}
std::optional<nemo::NetworkId> networkIdentity(const QString& value) {
    const auto id = graphIdentity(value);
    return id ? std::optional<nemo::NetworkId>{static_cast<nemo::NetworkId>(*id)} : std::nullopt;
}

bool finitePosition(double x, double y) {
    return std::isfinite(x) && std::isfinite(y);
}
enum class EndpointKind { Graph, InputTerminal, OutputTerminal };
struct Endpoint {
    EndpointKind kind{EndpointKind::Graph};
    nemo::NodeId node{nemo::kInvalidNode};
    nemo::InterfacePortId terminal{nemo::kInvalidInterfacePort};
    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

std::optional<std::uint64_t> unsignedText(const QString& value) {
    bool ok = false;
    const auto id = value.toULongLong(&ok);
    return ok && id != 0 ? std::optional<std::uint64_t>{id} : std::nullopt;
}

std::optional<Endpoint> endpointIdentity(const QVariant& value) {
    const auto text = value.toString().trimmed();
    if (text.startsWith(QStringLiteral("input:"))) {
        const auto id = unsignedText(text.mid(6));
        return id ? std::optional<Endpoint>{{EndpointKind::InputTerminal, nemo::kInvalidNode,
                                             static_cast<nemo::InterfacePortId>(*id)}}
                  : std::nullopt;
    }
    if (text.startsWith(QStringLiteral("output:"))) {
        const auto id = unsignedText(text.mid(7));
        return id ? std::optional<Endpoint>{{EndpointKind::OutputTerminal, nemo::kInvalidNode,
                                             static_cast<nemo::InterfacePortId>(*id)}}
                  : std::nullopt;
    }
    const auto id = graphIdentity(value);
    return id ? std::optional<Endpoint>{{EndpointKind::Graph, static_cast<nemo::NodeId>(*id),
                                         nemo::kInvalidInterfacePort}}
              : std::nullopt;
}

struct BindingEdge {
    enum class Kind { Input, Output, Instance };
    Kind kind;
    nemo::InterfacePortId terminal{nemo::kInvalidInterfacePort};
    nemo::NetworkInstanceId instance{nemo::kInvalidNetworkInstance};
    nemo::PortRef node;
};

std::optional<BindingEdge> bindingEdgeIdentity(const QVariant& value) {
    const auto parts = value.toString().trimmed().split(QLatin1Char(':'));
    if (parts.size() < 2)
        return std::nullopt;
    bool ok = false;
    const auto terminal = parts.at(1).toULongLong(&ok);
    if (!ok || terminal == 0)
        return std::nullopt;
    if (parts.at(0) == QStringLiteral("output") && parts.size() == 2)
        return BindingEdge{BindingEdge::Kind::Output,
                           static_cast<nemo::InterfacePortId>(terminal),
                           nemo::kInvalidNetworkInstance,
                           {}};
    if (parts.at(0) == QStringLiteral("input") && parts.size() == 4) {
        bool nodeOk = false;
        bool portOk = false;
        const auto node = parts.at(2).toULongLong(&nodeOk);
        const auto port = parts.at(3).toUInt(&portOk);
        if (nodeOk && portOk && node != 0)
            return BindingEdge{BindingEdge::Kind::Input,
                               static_cast<nemo::InterfacePortId>(terminal),
                               nemo::kInvalidNetworkInstance,
                               {static_cast<nemo::NodeId>(node), port}};
    }
    if (parts.at(0) == QStringLiteral("instance") && parts.size() == 3) {
        bool instanceOk = false;
        const auto instance = parts.at(1).toULongLong(&instanceOk);
        const auto childTerminal = parts.at(2).toULongLong(&ok);
        if (instanceOk && ok && instance != 0 && childTerminal != 0)
            return BindingEdge{BindingEdge::Kind::Instance,
                               static_cast<nemo::InterfacePortId>(childTerminal),
                               static_cast<nemo::NetworkInstanceId>(instance),
                               {}};
    }
    return std::nullopt;
}

std::optional<std::pair<nemo::PortDirection, nemo::InterfacePortId>> terminalNodeIdentity(const QVariant& value) {
    const auto endpoint = endpointIdentity(value);
    if (!endpoint || endpoint->kind == EndpointKind::Graph)
        return std::nullopt;
    return std::pair{endpoint->kind == EndpointKind::InputTerminal ? nemo::PortDirection::Input
                                                                   : nemo::PortDirection::Output,
                     endpoint->terminal};
}

bool mapPosition(const QVariantMap& map, double& x, double& y) {
    const auto xValue = map.value(QStringLiteral("x"));
    const auto yValue = map.value(QStringLiteral("y"));
    if (!xValue.isValid() || !yValue.isValid() || !isRealVariant(xValue) || !isRealVariant(yValue))
        return false;
    x = xValue.toDouble();
    y = yValue.toDouble();
    return finitePosition(x, y);
}

QVariantList portSnapshot(const std::vector<nemo::PortSpec>& ports) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(ports.size()));
    for (qsizetype index = 0; index < static_cast<qsizetype>(ports.size()); ++index) {
        const auto& port = ports.at(static_cast<std::size_t>(index));
        result.push_back(QVariantMap{{QStringLiteral("id"), QString::number(index)},
                                     {QStringLiteral("index"), static_cast<int>(index)},
                                     {QStringLiteral("name"), QString::fromStdString(port.name)},
                                     {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))},
                                     {QStringLiteral("optional"), port.optional}});
    }
    return result;
}

QVariantMap formalPortSnapshot(const nemo::FormalPort& port, bool input) {
    const QVariantMap terminal{{QStringLiteral("id"), QString::number(port.id)},
                               {QStringLiteral("index"), 0},
                               {QStringLiteral("name"), QString::fromStdString(port.name)},
                               {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))},
                               {QStringLiteral("allowFanOut"), port.allowFanOut}};
    return QVariantMap{{QStringLiteral("id"), input ? QStringLiteral("input:") + QString::number(port.id)
                                                    : QStringLiteral("output:") + QString::number(port.id)},
                       {QStringLiteral("type"), input ? QStringLiteral("input") : QStringLiteral("output")},
                       {QStringLiteral("name"), QString::fromStdString(port.name)},
                       {QStringLiteral("x"), port.layout.x},
                       {QStringLiteral("y"), port.layout.y},
                       {QStringLiteral("inputs"), input ? QVariantList{} : QVariantList{terminal}},
                       {QStringLiteral("outputs"), input ? QVariantList{terminal} : QVariantList{}},
                       {QStringLiteral("category"), QStringLiteral("IO")},
                       {QStringLiteral("group"), QStringLiteral("I/O")},
                       {QStringLiteral("terminal"), true},
                       {QStringLiteral("direction"), input ? QStringLiteral("input") : QStringLiteral("output")},
                       {QStringLiteral("terminalId"), QString::number(port.id)},
                       {QStringLiteral("deletable"), false}};
}

QVariantMap formalMetadataSnapshot(const nemo::FormalPort& port) {
    return QVariantMap{{QStringLiteral("id"), QString::number(port.id)},
                       {QStringLiteral("name"), QString::fromStdString(port.name)},
                       {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))},
                       {QStringLiteral("allowFanOut"), port.allowFanOut},
                       {QStringLiteral("x"), port.layout.x},
                       {QStringLiteral("y"), port.layout.y}};
}

struct PresentedConnection {
    QString id;
    Endpoint from;
    int fromPort{};
    Endpoint to;
    int toPort{};
    Command disconnect;
};

PresentedConnection resolveConnection(const Document& document, NetworkId network, const QString& id) {
    const auto& scoped = document.network(network);
    if (const auto edgeId = graphIdentity(id)) {
        const auto& edges = scoped.graph().edges();
        const auto found =
            std::find_if(edges.begin(), edges.end(), [&](const Edge& edge) { return edge.id == *edgeId; });
        if (found != edges.end())
            return {id,
                    {EndpointKind::Graph, found->from.node, {}},
                    static_cast<int>(found->from.port),
                    {EndpointKind::Graph, found->to.node, {}},
                    static_cast<int>(found->to.port),
                    disconnectCommand(network, *edgeId)};
    } else if (const auto binding = bindingEdgeIdentity(id)) {
        if (binding->kind == BindingEdge::Kind::Input) {
            for (const auto& connection : scoped.inputConnections())
                if (connection.terminal == binding->terminal && connection.node == binding->node)
                    return {id,
                            {EndpointKind::InputTerminal, {}, binding->terminal},
                            0,
                            {EndpointKind::Graph, binding->node.node, {}},
                            static_cast<int>(binding->node.port),
                            disconnectInputCommand(network, binding->terminal, binding->node)};
        } else if (binding->kind == BindingEdge::Kind::Output) {
            if (const auto input = scoped.outputInputBindings().find(binding->terminal);
                input != scoped.outputInputBindings().end())
                return {id, {EndpointKind::InputTerminal, {}, input->second},
                        0,  {EndpointKind::OutputTerminal, {}, binding->terminal},
                        0,  disconnectOutputCommand(network, binding->terminal)};
            for (const auto& connection : scoped.outputConnections())
                if (connection.terminal == binding->terminal)
                    return {id,
                            {EndpointKind::Graph, connection.node.node, {}},
                            static_cast<int>(connection.node.port),
                            {EndpointKind::OutputTerminal, {}, binding->terminal},
                            0,
                            disconnectOutputCommand(network, binding->terminal)};
        } else if (const auto* instance = document.instance(binding->instance);
                   instance && instance->parentNetwork == network) {
            const auto& inputs = document.network(instance->definition).inputs();
            const auto input = std::find_if(inputs.begin(), inputs.end(),
                                            [&](const FormalPort& port) { return port.id == binding->terminal; });
            if (input != inputs.end()) {
                const int index = static_cast<int>(std::distance(inputs.begin(), input));
                if (const auto source = instance->inputBindings.find(binding->terminal);
                    source != instance->inputBindings.end())
                    return {id,
                            {EndpointKind::Graph, source->second.node, {}},
                            static_cast<int>(source->second.port),
                            {EndpointKind::Graph, instance->node, {}},
                            index,
                            unbindInstanceInputCommand(instance->id, binding->terminal)};
            }
        }
    }
    throw GraphException(GraphError::UnknownEdge,
                         "connection " + id.toStdString() + " does not exist in network " + std::to_string(network));
}

std::optional<PresentedConnection> occupiedConnection(const Document& document, NetworkId network,
                                                      const Endpoint& destination, int port) {
    const auto& scoped = document.network(network);
    if (destination.kind == EndpointKind::OutputTerminal) {
        if (scoped.outputInputBindings().contains(destination.terminal))
            return resolveConnection(document, network,
                                     QStringLiteral("output:") + QString::number(destination.terminal));
        for (const auto& connection : scoped.outputConnections())
            if (connection.terminal == destination.terminal)
                return resolveConnection(document, network,
                                         QStringLiteral("output:") + QString::number(destination.terminal));
        return std::nullopt;
    }
    const PortRef target{destination.node, static_cast<std::uint32_t>(port)};
    if (const auto* node = scoped.graph().node(destination.node); node && node->instance != kInvalidNetworkInstance) {
        const auto* instance = document.instance(node->instance);
        const auto& inputs = document.network(instance->definition).inputs();
        if (static_cast<std::size_t>(port) < inputs.size()) {
            const auto terminal = inputs[static_cast<std::size_t>(port)].id;
            if (instance->inputBindings.contains(terminal))
                return resolveConnection(document, network,
                                         QStringLiteral("instance:") + QString::number(instance->id) +
                                             QLatin1Char(':') + QString::number(terminal));
        }
    }
    for (const auto& edge : scoped.graph().edges())
        if (edge.to == target)
            return resolveConnection(document, network, QString::number(edge.id));
    for (const auto& connection : scoped.inputConnections())
        if (connection.node == target)
            return resolveConnection(document, network,
                                     QStringLiteral("input:") + QString::number(connection.terminal) +
                                         QLatin1Char(':') + QString::number(target.node) + QLatin1Char(':') +
                                         QString::number(target.port));
    return std::nullopt;
}

Command connectPresentedPorts(const Document& document, NetworkId network, const Endpoint& from, int fromPort,
                              const Endpoint& to, int toPort) {
    if (fromPort < 0 || toPort < 0 || from.kind == EndpointKind::OutputTerminal ||
        to.kind == EndpointKind::InputTerminal || (from.kind == EndpointKind::InputTerminal && fromPort != 0) ||
        (to.kind == EndpointKind::OutputTerminal && toPort != 0))
        throw GraphException(GraphError::PortType, "connection has an invalid source or destination port");
    if (to.kind == EndpointKind::OutputTerminal) {
        if (from.kind == EndpointKind::InputTerminal)
            return connectInputToOutputCommand(network, from.terminal, to.terminal);
        return connectOutputCommand(network, {from.node, static_cast<std::uint32_t>(fromPort)}, to.terminal);
    }
    const auto* node = document.network(network).graph().node(to.node);
    if (!node)
        throw GraphException(GraphError::UnknownNode,
                             "connection destination node " + std::to_string(to.node) + " is unavailable");
    if (node->instance != kInvalidNetworkInstance) {
        const auto* instance = document.instance(node->instance);
        if (!instance || instance->parentNetwork != network)
            throw GraphException(GraphError::InvalidInstance,
                                 "connection destination instance is outside this network");
        const auto& inputs = document.network(instance->definition).inputs();
        if (static_cast<std::size_t>(toPort) >= inputs.size())
            throw GraphException(GraphError::PortType, "connection destination instance input is out of range");
        const auto terminal = inputs[static_cast<std::size_t>(toPort)].id;
        return from.kind == EndpointKind::InputTerminal
                   ? bindInstanceInputToParentTerminalCommand(instance->id, terminal, from.terminal)
                   : bindInstanceInputCommand(instance->id, terminal,
                                              {from.node, static_cast<std::uint32_t>(fromPort)});
    }
    const PortRef destination{to.node, static_cast<std::uint32_t>(toPort)};
    return from.kind == EndpointKind::InputTerminal
               ? connectInputCommand(network, from.terminal, destination)
               : connectCommand(network, {from.node, static_cast<std::uint32_t>(fromPort)}, destination);
}

QVariantMap bindingSnapshot(QString id, QString fromNode, int fromPort, QString toNode, int toPort, QString binding,
                            nemo::PortKind kind) {
    return QVariantMap{{QStringLiteral("id"), std::move(id)},
                       {QStringLiteral("fromNode"), std::move(fromNode)},
                       {QStringLiteral("fromPort"), fromPort},
                       {QStringLiteral("toNode"), std::move(toNode)},
                       {QStringLiteral("toPort"), toPort},
                       {QStringLiteral("route"), QVariantList{}},
                       {QStringLiteral("binding"), std::move(binding)},
                       {QStringLiteral("kind"), QString::fromLatin1(portKindName(kind))}};
}

QVariantList routeSnapshot(const std::vector<nemo::LayoutPosition>& route) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(route.size()));
    for (const auto& point : route)
        result.push_back(QVariantMap{{QStringLiteral("x"), point.x}, {QStringLiteral("y"), point.y}});
    return result;
}

// The inspector control family for a schema type. It is deliberately a
// presentation fact, not a new parameter model.
const char* parameterKindName(nemo::ParameterType type) {
    switch (type) {
    case nemo::ParameterType::Boolean:
        return "toggle";
    case nemo::ParameterType::Integer:
    case nemo::ParameterType::Float:
        return "number";
    case nemo::ParameterType::Choice:
        return "choice";
    case nemo::ParameterType::Vector2:
        return "vector2";
    case nemo::ParameterType::Vector3:
        return "vector3";
    case nemo::ParameterType::Color:
        return "color";
    case nemo::ParameterType::String:
        return "string";
    }
    return "string";
}

// Empty schema labels fall back to a readable form of the persisted key:
// underscores/dots become word breaks and camelCase gains a space.
[[nodiscard]] QString humanizedParameterLabel(std::string_view name) {
    std::string result;
    result.reserve(name.size() + 4);
    bool capitalize = true;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        if (character == '_' || character == '-' || character == '.') {
            if (!result.empty() && result.back() != ' ')
                result.push_back(' ');
            capitalize = true;
            continue;
        }
        if (capitalize) {
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
            capitalize = false;
            continue;
        }
        const auto previous = static_cast<unsigned char>(name[index - 1]);
        if (std::isupper(static_cast<unsigned char>(character)) && (std::islower(previous) || std::isdigit(previous))) {
            result.push_back(' ');
            result.push_back(character);
            continue;
        }
        result.push_back(character);
    }
    return QString::fromStdString(result);
}

// A shared definition may be instantiated many times; only the occurrence name
// must be unique inside its parent graph.
[[nodiscard]] std::string uniqueOccurrenceName(const nemo::Graph& graph, std::string base) {
    if (!graph.nodeByName(base))
        return base;
    for (std::size_t suffix = 2;; ++suffix) {
        const std::string candidate = base + " " + std::to_string(suffix);
        if (!graph.nodeByName(candidate))
            return candidate;
    }
}

// Default label for a newly exposed control, derived from the source schema so
// the popout never invents a name. A collision gains a numeric suffix.
[[nodiscard]] std::string uniqueExposedName(const nemo::Network& definition, const nemo::ParameterSpec& spec,
                                            const std::string& key) {
    const std::string base = spec.label.empty() ? humanizedParameterLabel(key).toStdString() : spec.label;
    const auto taken = [&definition](const std::string& candidate) {
        return std::any_of(definition.exposedParameters().begin(), definition.exposedParameters().end(),
                           [&candidate](const nemo::ExposedParameter& exposed) { return exposed.name == candidate; });
    };
    if (!taken(base))
        return base;
    for (std::size_t suffix = 2;; ++suffix) {
        const std::string candidate = base + " " + std::to_string(suffix);
        if (!taken(candidate))
            return candidate;
    }
}

// A subnet inspector row addresses its definition parameter through the stable
// exposed identity, never through the editable display name (which may repeat
// a source key or be renamed at any time).
QString exposedParameterToken(nemo::InterfacePortId id) {
    return QStringLiteral("exposed:") + QString::number(id);
}

std::optional<nemo::InterfacePortId> exposedParameterIdentity(const QString& key) {
    const auto prefix = QStringLiteral("exposed:");
    if (!key.startsWith(prefix))
        return std::nullopt;
    bool valid = false;
    const auto id = key.mid(prefix.size()).toULongLong(&valid);
    return valid && id != 0 ? std::optional<nemo::InterfacePortId>{static_cast<nemo::InterfacePortId>(id)}
                            : std::nullopt;
}

struct InspectorTarget {
    nemo::NetworkId network{nemo::kInvalidNetwork};
    nemo::NodeId node{nemo::kInvalidNode};
    const nemo::NodeInstance* instance{};
    const nemo::NodeDescriptor* descriptor{};
    const nemo::ParameterSpec* spec{};
    nemo::ParameterAddress address{};
};

// Resolves a network/node (and optionally a parameter key) to live document
// objects. `error` always names the relationship that failed.
std::optional<InspectorTarget> resolveInspectorTarget(const nemo::Document& document, const QString& networkValue,
                                                      const QVariant& nodeValue, std::string_view key, QString& error) {
    const auto network = networkIdentity(networkValue);
    if (!network) {
        error = QStringLiteral("network '%1' is not a valid identity").arg(networkValue);
        return std::nullopt;
    }
    const auto node = graphIdentity(nodeValue);
    if (!node) {
        error = QStringLiteral("node '%1' is not a valid identity").arg(nodeValue.toString());
        return std::nullopt;
    }
    const nemo::Network* resolved = nullptr;
    try {
        resolved = &document.network(*network);
    } catch (const std::exception&) {
        error = QStringLiteral("network '%1' does not exist").arg(QString::number(*network));
        return std::nullopt;
    }
    const auto id = static_cast<nemo::NodeId>(*node);
    const auto* instance = resolved->graph().node(id);
    if (!instance) {
        error = QStringLiteral("node '%1' does not exist in network '%2'")
                    .arg(QString::number(id), QString::number(*network));
        return std::nullopt;
    }
    if (instance->instance != nemo::kInvalidNetworkInstance) {
        // A subnet occurrence has no catalog descriptor of its own; its
        // inspector and edit target are the definition's exposed parameters.
        const auto* occurrence = document.instance(instance->instance);
        if (!occurrence || occurrence->parentNetwork != *network) {
            error = QStringLiteral("node '%1' is not a live subnet occurrence in network '%2'")
                        .arg(QString::number(id), QString::number(*network));
            return std::nullopt;
        }
        const auto& definition = document.network(occurrence->definition);
        InspectorTarget target{
            occurrence->definition,
            nemo::kInvalidNode,
            instance,
            nullptr,
            nullptr,
            nemo::ParameterAddress{occurrence->definition, nemo::kInvalidNode, std::string(key), occurrence->id}};
        if (!key.empty()) {
            const auto exposedId =
                exposedParameterIdentity(QString::fromUtf8(key.data(), static_cast<int>(key.size())));
            const auto* exposed = exposedId ? definition.exposedParameter(*exposedId) : nullptr;
            const auto* node = exposed ? definition.graph().node(exposed->node) : nullptr;
            const auto* spec = node ? definition.graph().catalog().parameterSpec(node->type, exposed->key) : nullptr;
            if (!exposed || !node || !spec) {
                error = QStringLiteral("subnet '%1' does not expose parameter '%2'")
                            .arg(QString::fromStdString(instance->name), QString::fromStdString(std::string(key)));
                return std::nullopt;
            }
            target.node = exposed->node;
            target.spec = spec;
            target.address.node = exposed->node;
            target.address.key = exposed->key;
        }
        return target;
    }
    const auto* descriptor = resolved->graph().descriptor(instance->type);
    if (!descriptor) {
        error = QStringLiteral("node '%1' has no descriptor for type '%2'")
                    .arg(QString::fromStdString(instance->name), QString::fromStdString(instance->type));
        return std::nullopt;
    }
    InspectorTarget target{*network, id,
                           instance, descriptor,
                           nullptr,  nemo::ParameterAddress{*network, id, std::string(key), instance->instance}};
    if (!key.empty()) {
        target.spec = resolved->graph().catalog().parameterSpec(instance->type, key);
        if (!target.spec) {
            error = QStringLiteral("node '%1' has no parameter '%2'")
                        .arg(QString::fromStdString(instance->name), QString::fromStdString(std::string(key)));
            return std::nullopt;
        }
    }
    return target;
}

struct ParameterKeyState {
    nemo::AnimationChannelId channel{nemo::kInvalidAnimationChannel};
    std::size_t keyCount{};
    nemo::KeyframeId keyAtFrame{nemo::kInvalidKeyframe};
    bool animated{false};
    bool keyed{false};
};

[[nodiscard]] ParameterKeyState parameterKeyState(const nemo::Document& document, const nemo::ParameterAddress& address,
                                                  double frame) {
    ParameterKeyState state;
    const auto* channel = document.animationChannel(address);
    if (!channel)
        return state;
    state.channel = channel->id;
    state.keyCount = channel->keys.size();
    state.animated = !channel->keys.empty();
    const auto found = std::find_if(channel->keys.begin(), channel->keys.end(),
                                    [frame](const nemo::Keyframe& key) { return key.time == frame; });
    if (found != channel->keys.end()) {
        state.keyAtFrame = found->id;
        state.keyed = true;
    }
    return state;
}
}  // namespace
ViewerController::ViewerController(ViewerRuntime* runtime, nemo::ProjectSession& session)
    : runtime_(runtime), session_(session), schedulerPoll_(this), playback_(this) {
    connect(runtime_, &ViewerRuntime::resultReady, this, &ViewerController::receive, Qt::QueuedConnection);
    connect(
        runtime_, &ViewerRuntime::rangeFailed, this,
        [this](const QString& message, qulonglong id) {
            if (id != rangeGeneration_)
                return;
            rangeError_ = message;
            emit schedulerChanged();
        },
        Qt::QueuedConnection);
    // Presentation is destination-scoped at the runtime: a panel re-emits only
    // the frames its own destination presented. The runtime emits this from
    // Qt's frameSwapped boundary on the render thread, so the relay is direct
    // and only re-emits.
    connect(
        runtime_, &ViewerRuntime::framePresented, this,
        [this](eval::ViewerDestination presented, int frame, int width, int height, bool cacheHit, double elapsed) {
            if (!destination_ || *destination_ != presented)
                return;
            emit framePresented(frame, width, height, cacheHit, elapsed);
        },
        Qt::DirectConnection);
    schedulerPoll_.setInterval(200);
    connect(&schedulerPoll_, &QTimer::timeout, this, &ViewerController::pollScheduler);
    schedulerPoll_.start();
    connect(&playback_, &QTimer::timeout, this, &ViewerController::playbackTick);
    sessionSubscription_ = session_.subscribe(this, &ViewerController::sessionDocumentChanged);
}

void ViewerController::sessionDocumentChanged(void* context) noexcept {
    auto* controller = static_cast<ViewerController*>(context);
    try {
        controller->documentChanged();
    } catch (const std::exception& error) {
        controller->fail(QString::fromUtf8(error.what()));
    }
}

ViewerController::~ViewerController() = default;

void ViewerController::setDestination(std::optional<eval::ViewerDestination> destination) {
    if (destination_ == destination)
        return;
    // Stop publishing to the destination this panel is leaving: retirement is
    // the runtime's, but its in-flight work must not become this panel's state.
    if (destination_)
        runtime_->cancel(generation_, *destination_);
    destination_ = std::move(destination);
    emit destinationChanged();
    if (!destination_)
        return;
    refreshViewerTarget();
    refreshContextTarget();
    invalidateRequest();
    refreshRequest();
}

qulonglong ViewerController::destinationId() const {
    return destination_ ? static_cast<qulonglong>(*destination_) : 0ULL;
}

QString ViewerController::renderState() const {
    if (!error_.isEmpty())
        return QStringLiteral("failed");
    if (pending_)
        return QStringLiteral("pending");
    if (outdated_)
        return QStringLiteral("outdated");
    if (presentation_)
        return QStringLiteral("current");
    // A routed context with no render target is unavailable, not idle: the
    // panel must not present another group's output in its place.
    if (!contextUnavailable_.isEmpty())
        return QStringLiteral("unavailable");
    return QStringLiteral("idle");
}

namespace {
// Viewer nodes are the network's nodes whose descriptor type is "viewer",
// ordered by ascending NodeId. This is the presentation ordering the panel
// selector and assignViewerCommand agree on.
std::vector<NodeId> viewerNodeIds(const nemo::Document& document, NetworkId network) {
    try {
        const auto& graph = document.network(network).graph();
        std::vector<NodeId> viewers;
        for (const auto& node : graph.nodes()) {
            const auto* descriptor = graph.descriptor(node.type);
            if (descriptor && descriptor->type == "viewer")
                viewers.push_back(node.id);
        }
        std::sort(viewers.begin(), viewers.end());
        return viewers;
    } catch (const std::exception&) {
        return {};
    }
}
}  // namespace

namespace {
// Resolves a routed media target to its decodable Document source key. A
// decimal target is a catalog entry id and resolves through the entry's
// persistent source key; anything else is the source key itself, matching
// PanelContextRouter::targetAvailable. Empty when the target names no
// Document source reference at all — a catalog entry whose source reference
// was removed is not viewable.
[[nodiscard]] std::string mediaSourceKey(const nemo::Document& document, const QString& target) {
    const auto trimmed = target.trimmed();
    if (trimmed.isEmpty())
        return {};
    std::string key;
    bool decimal = false;
    const auto entryId = trimmed.toULongLong(&decimal);
    if (decimal) {
        const auto* entry = document.mediaCatalog().entry(static_cast<MediaSourceId>(entryId));
        if (entry == nullptr)
            return {};
        key = entry->sourceKey;
    } else {
        key = trimmed.toStdString();
    }
    return document.sources.find(key) == document.sources.end() ? std::string{} : key;
}

// Resolves the root network's source node addressing `key`. The lowest
// matching NodeId wins so one target always resolves to one node. `reason`
// names the failed relationship when no such node exists; the caller decides
// whether that is an unavailable panel or a request-owned snapshot.
[[nodiscard]] NodeId mediaSourceNode(const nemo::Document& document, const std::string& key, QString& reason) {
    NodeId found = kInvalidNode;
    try {
        const auto& graph = document.network(document.rootNetworkId()).graph();
        for (const auto& node : graph.nodes()) {
            if (node.type != "source")
                continue;
            const auto parameter = node.params.find("source");
            if (parameter == node.params.end())
                continue;
            const auto* value = std::get_if<std::string>(&parameter->second);
            if (!value || *value != key)
                continue;
            if (found == kInvalidNode || node.id < found)
                found = node.id;
        }
    } catch (const std::exception&) {
        reason = QStringLiteral("Media source '%1' cannot be resolved: the root network is unavailable")
                     .arg(QString::fromStdString(key));
        return kInvalidNode;
    }
    if (found == kInvalidNode)
        reason =
            QStringLiteral("Media source '%1' has no source node in the root network").arg(QString::fromStdString(key));
    return found;
}
}  // namespace

int ViewerController::viewerCount(const QString& networkValue) const {
    const auto network = networkIdentity(networkValue);
    if (!network)
        return 0;
    return static_cast<int>(viewerNodeIds(session_.document(), *network).size());
}

QVariantMap ViewerController::viewerAttachment(const QString& networkValue, int viewerIndex) const {
    QVariantMap result{{QStringLiteral("index"), viewerIndex},
                       {QStringLiteral("viewerId"), QString()},
                       {QStringLiteral("viewerName"), QString()},
                       {QStringLiteral("attachedId"), QString()},
                       {QStringLiteral("attachedName"), QString()}};
    const auto network = networkIdentity(networkValue);
    if (!network || viewerIndex < 0)
        return result;
    const auto& document = session_.document();
    const auto viewers = viewerNodeIds(document, *network);
    if (static_cast<std::size_t>(viewerIndex) >= viewers.size())
        return result;
    const NodeId viewer = viewers[static_cast<std::size_t>(viewerIndex)];
    try {
        const auto& graph = document.network(*network).graph();
        if (const auto* instance = graph.node(viewer)) {
            result.insert(QStringLiteral("viewerId"), QString::number(viewer));
            result.insert(QStringLiteral("viewerName"), QString::fromStdString(instance->name));
        }
        for (const auto& edge : graph.edgesInto(viewer)) {
            if (edge.to.port != 0)
                continue;
            result.insert(QStringLiteral("attachedId"), QString::number(edge.from.node));
            if (const auto* upstream = graph.node(edge.from.node))
                result.insert(QStringLiteral("attachedName"), QString::fromStdString(upstream->name));
            break;
        }
    } catch (const std::exception&) {
    }
    return result;
}

QString ViewerController::rootNetworkId() const {
    return QString::number(session_.document().rootNetworkId());
}

QVariantMap ViewerController::graphScope(const QString& rootValue, const QVariantList& instancePath) const {
    QVariantList path;
    QVariantList breadcrumbs;
    const auto root = networkIdentity(rootValue);
    if (!root)
        return {{QStringLiteral("available"), false},
                {QStringLiteral("path"), path},
                {QStringLiteral("breadcrumbs"), breadcrumbs}};
    NetworkId active = *root;
    try {
        const auto& document = session_.document();
        const auto& rootNetwork = document.network(active);
        breadcrumbs.push_back(QVariantMap{{QStringLiteral("networkId"), QString::number(active)},
                                          {QStringLiteral("name"), QString::fromStdString(rootNetwork.name())}});
        for (const auto& value : instancePath) {
            const auto entry = value.toMap();
            const auto id = graphIdentity(entry.value(QStringLiteral("instanceId")));
            const auto* instance = id ? document.instance(*id) : nullptr;
            if (!instance || instance->parentNetwork != active ||
                entry.value(QStringLiteral("networkId")).toString() != QString::number(instance->definition))
                break;
            const auto* node = document.network(active).graph().node(instance->node);
            if (!node || node->instance != instance->id)
                break;
            path.push_back(QVariantMap{{QStringLiteral("parentNetworkId"), QString::number(active)},
                                       {QStringLiteral("networkId"), QString::number(instance->definition)},
                                       {QStringLiteral("instanceId"), QString::number(instance->id)},
                                       {QStringLiteral("nodeId"), QString::number(instance->node)},
                                       {QStringLiteral("name"), QString::fromStdString(node->name)}});
            active = instance->definition;
            breadcrumbs.push_back(QVariantMap{{QStringLiteral("networkId"), QString::number(active)},
                                              {QStringLiteral("name"), QString::fromStdString(node->name)}});
        }
        return {{QStringLiteral("available"), true},
                {QStringLiteral("networkId"), QString::number(active)},
                {QStringLiteral("path"), path},
                {QStringLiteral("breadcrumbs"), breadcrumbs}};
    } catch (const std::exception&) {
        return {{QStringLiteral("available"), false},
                {QStringLiteral("networkId"), rootValue},
                {QStringLiteral("path"), QVariantList{}},
                {QStringLiteral("breadcrumbs"), QVariantList{}}};
    }
}

QVariantMap ViewerController::graphSnapshot(const QString& networkValue) const {
    const auto identity = graphIdentity(networkValue);
    if (!identity)
        return QVariantMap{{QStringLiteral("networkId"), networkValue}, {QStringLiteral("available"), false},
                           {QStringLiteral("nodes"), QVariantList{}},   {QStringLiteral("edges"), QVariantList{}},
                           {QStringLiteral("inputs"), QVariantList{}},  {QStringLiteral("outputs"), QVariantList{}}};
    try {
        const auto network = static_cast<NetworkId>(*identity);
        const auto& scoped = session_.document().network(network);
        const auto& graph = scoped.graph();
        QVariantList nodes;
        QVariantList formalInputs;
        QVariantList formalOutputs;
        for (const auto& port : scoped.inputs()) {
            nodes.push_back(formalPortSnapshot(port, true));
            formalInputs.push_back(formalMetadataSnapshot(port));
        }
        for (const auto& port : scoped.outputs()) {
            nodes.push_back(formalPortSnapshot(port, false));
            formalOutputs.push_back(formalMetadataSnapshot(port));
        }
        for (NodeId after = kInvalidNode;;) {
            const auto page = session_.queryNodes(network, {}, 256, after);
            for (const auto& query : page) {
                const auto* node = graph.node(query.id);
                if (!node)
                    continue;
                QVariantMap params;
                for (std::string keyAfter;;) {
                    const auto values = session_.queryValues(network, node->id, {}, 256, keyAfter);
                    for (const auto& value : values)
                        params.insert(QString::fromStdString(value.key), parameterValueVariant(value.value));
                    if (values.size() < 256)
                        break;
                    keyAfter = values.back().key;
                }
                const auto descriptor = graph.descriptor(node->type);
                const auto group = descriptor ? QString::fromStdString(descriptor->group) : QStringLiteral("Utility");
                QVariantMap snapshot{{QStringLiteral("id"), QString::number(node->id)},
                                     {QStringLiteral("type"), QString::fromStdString(node->type)},
                                     {QStringLiteral("name"), QString::fromStdString(node->name)},
                                     {QStringLiteral("params"), params},
                                     {QStringLiteral("x"), node->layout.x},
                                     {QStringLiteral("y"), node->layout.y},
                                     {QStringLiteral("inputs"), portSnapshot(graph.inputPorts(node->id))},
                                     {QStringLiteral("outputs"), portSnapshot(graph.outputPorts(node->id))},
                                     {QStringLiteral("category"), group},
                                     {QStringLiteral("group"), group},
                                     {QStringLiteral("deletable"), node->id != scoped.defaultOutput()}};
                if (node->definition != kInvalidNetwork)
                    snapshot.insert(QStringLiteral("definition"), QString::number(node->definition));
                if (node->instance != kInvalidNetworkInstance) {
                    snapshot.insert(QStringLiteral("instance"), QString::number(node->instance));
                    if (const auto* occurrence = session_.document().instance(node->instance)) {
                        std::size_t references = 0;
                        for (const auto& other : session_.document().instances())
                            if (other.definition == occurrence->definition)
                                ++references;
                        snapshot.insert(QStringLiteral("linkState"), references > 1 ? QStringLiteral("shared")
                                                                     : occurrence->ownsDefinition
                                                                         ? QStringLiteral("local")
                                                                         : QStringLiteral("linked"));
                        snapshot.insert(
                            QStringLiteral("exposedParameterCount"),
                            static_cast<int>(
                                session_.document().network(occurrence->definition).exposedParameters().size()));
                    }
                }
                nodes.push_back(std::move(snapshot));
            }
            if (page.size() < 256)
                break;
            after = page.back().id;
        }
        QVariantList edges;
        for (EdgeId after = kInvalidEdge;;) {
            const auto page = session_.queryEdges(network, kInvalidNode, 256, after);
            for (const auto& query : page) {
                const auto& edge = query.edge;
                edges.push_back(QVariantMap{{QStringLiteral("id"), QString::number(edge.id)},
                                            {QStringLiteral("fromNode"), QString::number(edge.from.node)},
                                            {QStringLiteral("fromPort"), static_cast<int>(edge.from.port)},
                                            {QStringLiteral("toNode"), QString::number(edge.to.node)},
                                            {QStringLiteral("toPort"), static_cast<int>(edge.to.port)},
                                            {QStringLiteral("route"), routeSnapshot(edge.route)},
                                            {QStringLiteral("binding"), QStringLiteral("graph")}});
            }
            if (page.size() < 256)
                break;
            after = page.back().edge.id;
        }
        for (const auto& connection : scoped.inputConnections()) {
            const auto* formal = scoped.input(connection.terminal);
            if (!formal)
                continue;
            edges.push_back(bindingSnapshot(
                QStringLiteral("input:") + QString::number(connection.terminal) + QLatin1Char(':') +
                    QString::number(connection.node.node) + QLatin1Char(':') + QString::number(connection.node.port),
                QStringLiteral("input:") + QString::number(connection.terminal), 0,
                QString::number(connection.node.node), static_cast<int>(connection.node.port),
                QStringLiteral("networkInput"), formal->kind));
        }
        for (const auto& connection : scoped.outputConnections()) {
            const auto* formal = scoped.output(connection.terminal);
            if (!formal)
                continue;
            edges.push_back(bindingSnapshot(QStringLiteral("output:") + QString::number(connection.terminal),
                                            QString::number(connection.node.node),
                                            static_cast<int>(connection.node.port),
                                            QStringLiteral("output:") + QString::number(connection.terminal), 0,
                                            QStringLiteral("networkOutput"), formal->kind));
        }
        for (const auto& [output, input] : scoped.outputInputBindings()) {
            const auto* formal = scoped.output(output);
            if (formal)
                edges.push_back(bindingSnapshot(QStringLiteral("output:") + QString::number(output),
                                                QStringLiteral("input:") + QString::number(input), 0,
                                                QStringLiteral("output:") + QString::number(output), 0,
                                                QStringLiteral("terminalPassThrough"), formal->kind));
        }
        for (const auto& occurrence : session_.document().instances()) {
            if (occurrence.parentNetwork != network || !graph.node(occurrence.node))
                continue;
            const auto& child = session_.document().network(occurrence.definition);
            for (const auto& [terminal, source] : occurrence.inputBindings) {
                const auto* formal = child.input(terminal);
                const auto index =
                    std::find_if(child.inputs().cbegin(), child.inputs().cend(),
                                 [terminal](const FormalPort& candidate) { return candidate.id == terminal; });
                if (!formal || index == child.inputs().cend())
                    continue;
                edges.push_back(bindingSnapshot(QStringLiteral("instance:") + QString::number(occurrence.id) +
                                                    QLatin1Char(':') + QString::number(terminal),
                                                QString::number(source.node), static_cast<int>(source.port),
                                                QString::number(occurrence.node),
                                                static_cast<int>(std::distance(child.inputs().cbegin(), index)),
                                                QStringLiteral("instanceInput"), formal->kind));
            }
        }
        return QVariantMap{{QStringLiteral("networkId"), QString::number(network)},
                           {QStringLiteral("networkName"), QString::fromStdString(scoped.name())},
                           {QStringLiteral("available"), true},
                           {QStringLiteral("nodes"), nodes},
                           {QStringLiteral("edges"), edges},
                           {QStringLiteral("inputs"), formalInputs},
                           {QStringLiteral("outputs"), formalOutputs}};
    } catch (const std::exception&) {
        return QVariantMap{{QStringLiteral("networkId"), networkValue},
                           {QStringLiteral("available"), false},
                           {QStringLiteral("nodes"), QVariantList{}},
                           {QStringLiteral("edges"), QVariantList{}}};
    }
}

QVariantList ViewerController::graphNodes() const {
    return graphSnapshot(rootNetworkId()).value(QStringLiteral("nodes")).toList();
}

QVariantList ViewerController::graphEdges() const {
    return graphSnapshot(rootNetworkId()).value(QStringLiteral("edges")).toList();
}

QVariantList ViewerController::nodeCatalog() const {
    QVariantList result;
    for (const auto& descriptor :
         session_.document().network(session_.document().rootNetworkId()).graph().catalog().descriptors()) {
        QVariantList inputs;
        for (const auto& port : descriptor.inputs)
            inputs.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(port.name)},
                                         {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))},
                                         {QStringLiteral("optional"), port.optional}});
        QVariantList outputs;
        for (const auto& port : descriptor.outputs)
            outputs.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(port.name)},
                                          {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))},
                                          {QStringLiteral("optional"), port.optional}});
        QVariantList parameters;
        for (const auto& parameter : descriptor.parameters) {
            QVariantMap value{{QStringLiteral("name"), QString::fromStdString(parameter.name)},
                              {QStringLiteral("type"), QString::fromLatin1(parameterTypeName(parameter.type))},
                              {QStringLiteral("defaultValue"), parameterValueVariant(parameter.defaultValue)},
                              {QStringLiteral("label"), QString::fromStdString(parameter.label)},
                              {QStringLiteral("section"), QString::fromStdString(parameter.section)},
                              {QStringLiteral("editor"), QString::fromStdString(parameter.editor)}};
            if (parameter.minimum)
                value.insert(QStringLiteral("minimum"), *parameter.minimum);
            if (parameter.maximum)
                value.insert(QStringLiteral("maximum"), *parameter.maximum);
            if (parameter.step)
                value.insert(QStringLiteral("step"), *parameter.step);
            QVariantList choices;
            for (const auto& choice : parameter.choices)
                choices.push_back(QString::fromStdString(choice));
            value.insert(QStringLiteral("choices"), choices);
            parameters.push_back(value);
        }
        QVariantList samplingScales;
        for (const int scale : descriptor.capabilities.samplingScales)
            samplingScales.push_back(scale);
        QStringList channels;
        for (const auto& channel : descriptor.capabilities.channels)
            channels.push_back(QString::fromStdString(channel));
        result.push_back(
            QVariantMap{{QStringLiteral("type"), QString::fromStdString(descriptor.type)},
                        {QStringLiteral("displayName"), QString::fromStdString(descriptor.displayName)},
                        {QStringLiteral("group"), QString::fromStdString(descriptor.group)},
                        {QStringLiteral("version"), QVariant::fromValue<qulonglong>(descriptor.implementationVersion)},
                        {QStringLiteral("inputs"), inputs},
                        {QStringLiteral("outputs"), outputs},
                        {QStringLiteral("parameters"), parameters},
                        {QStringLiteral("samplingScales"), samplingScales},
                        {QStringLiteral("channels"), channels},
                        {QStringLiteral("temporal"), descriptor.capabilities.temporal}});
    }
    return result;
}

void ViewerController::setActiveViewer(const QString& networkValue, int viewerIndex) {
    const auto network = networkIdentity(networkValue);
    if (!network || viewerIndex < 0) {
        fail(QStringLiteral("active viewer requires a valid network ID and nonnegative index"));
        return;
    }
    if (activeViewerNetwork_ == *network && activeViewerIndex_ == viewerIndex)
        return;
    activeViewerNetwork_ = *network;
    activeViewerIndex_ = viewerIndex;
    refreshViewerTarget();
    invalidateRequest();
    refreshRequest();
}

bool ViewerController::assignViewer(const QString& networkValue, int viewerIndex, const QVariant& nodeValue) {
    const auto network = networkIdentity(networkValue);
    if (!network || viewerIndex < 0) {
        fail(QStringLiteral("viewer assignment requires a valid network ID and nonnegative index"));
        return false;
    }
    // An empty or invalid node identity detaches the viewer.
    const auto source = graphIdentity(nodeValue);
    try {
        static_cast<void>(session_.document().network(*network));
        return applyEdit(session_.submit(assignViewerCommand(*network, static_cast<std::size_t>(viewerIndex),
                                                             source ? static_cast<NodeId>(*source) : kInvalidNode),
                                         editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

QString ViewerController::viewerTargetId() const {
    return viewerTargetNode_ == kInvalidNode ? QString{} : QString::number(viewerTargetNode_);
}

void ViewerController::refreshViewerTarget() {
    const auto& document = session_.document();
    NetworkId network = activeViewerNetwork_;
    if (network == kInvalidNetwork)
        network = document.rootNetworkId();
    NodeId target = kInvalidNode;
    QString name;
    const auto viewers = viewerNodeIds(document, network);
    if (activeViewerIndex_ >= 0 && static_cast<std::size_t>(activeViewerIndex_) < viewers.size()) {
        try {
            const auto& graph = document.network(network).graph();
            const NodeId viewer = viewers[static_cast<std::size_t>(activeViewerIndex_)];
            for (const auto& edge : graph.edgesInto(viewer)) {
                if (edge.to.port != 0)
                    continue;
                target = edge.from.node;
                break;
            }
            if (const auto* upstream = graph.node(target))
                name = QString::fromStdString(upstream->name);
        } catch (const std::exception&) {
            target = kInvalidNode;
            name.clear();
        }
    }
    if (target == viewerTargetNode_ && name == viewerTargetName_)
        return;
    viewerTargetNode_ = target;
    viewerTargetName_ = name;
    emit viewerTargetChanged();
}

void ViewerController::setViewerContext(const QString& role, const QString& target, int clock) {
    const auto name = role.trimmed().toLower();
    const ContextRole nextRole = name == QStringLiteral("media")      ? ContextRole::Media
                                 : name == QStringLiteral("timeline") ? ContextRole::Timeline
                                                                      : ContextRole::Graph;
    const auto nextTarget = target.trimmed();
    const bool contextChanged = nextRole != contextRole_ || nextTarget != contextTarget_;
    contextRole_ = nextRole;
    contextTarget_ = nextTarget;
    if (contextRole_ == ContextRole::Graph) {
        // The graph role ignores the routed target: the controller renders its
        // own Viewer attachment (the panel's active viewer index).
        refreshViewerTarget();
    }
    refreshContextTarget();
    bool frameMoved = false;
    if (contextRole_ != ContextRole::Timeline) {
        // The routed clock is this context's own time; the timeline role has no
        // render target yet, so it has no frame to apply.
        const int requested = clampFrame(clock);
        if (requested != frame_) {
            frame_ = requested;
            emit frameChanged();
            emit timelineChanged();
            frameMoved = true;
        }
    }
    if (!contextChanged && !frameMoved)
        return;
    invalidateRequest();
    refreshRequest();
}

void ViewerController::refreshContextTarget() {
    contextTargetNode_ = kInvalidNode;
    contextUnavailable_.clear();
    contextSourceKey_.clear();
    switch (contextRole_) {
    case ContextRole::Media: {
        // The catalog reference alone must be viewable: a media target is a
        // valid context as soon as it names a Document source reference, even
        // when no authored graph node addresses it. refreshRequest then runs
        // the source-fill plan over a request-owned snapshot instead of
        // authoring a node.
        contextSourceKey_ = mediaSourceKey(session_.document(), contextTarget_);
        if (contextSourceKey_.empty()) {
            contextUnavailable_ =
                contextTarget_.isEmpty()
                    ? QStringLiteral("Media source is unavailable: the panel context has no source target")
                    : QStringLiteral("Media source '%1' is unavailable: no catalog source reference")
                          .arg(contextTarget_);
            return;
        }
        contextTargetNode_ = mediaSourceNode(session_.document(), contextSourceKey_, contextUnavailable_);
        // No authored source node is not a failure for this role: the
        // reference is still rendered from the private snapshot above.
        if (contextTargetNode_ == kInvalidNode)
            contextUnavailable_.clear();
        return;
    }
    case ContextRole::Timeline:
        // Editorial timeline target wiring belongs to issue #54. This panel
        // reports the deferral instead of presenting another group's output.
        contextUnavailable_ =
            contextTarget_.isEmpty()
                ? QStringLiteral("Timeline viewer target is unavailable: issue #54 owns timeline viewer wiring")
                : QStringLiteral("Timeline target '%1' is unavailable: issue #54 owns timeline viewer wiring")
                      .arg(contextTarget_);
        return;
    case ContextRole::Graph:
        return;
    }
}

NodeId ViewerController::renderTargetNode() const {
    switch (contextRole_) {
    case ContextRole::Media:
        return contextTargetNode_;
    case ContextRole::Timeline:
        return kInvalidNode;
    case ContextRole::Graph:
        return viewerTargetNode_;
    }
    return kInvalidNode;
}

QString ViewerController::unavailableStatus() const {
    return contextUnavailable_.isEmpty() ? QStringLiteral("No viewer target") : contextUnavailable_;
}

int ViewerController::clampFrame(int frame) const {
    frame = std::max(frame, 0);
    if (frameCount_ > 0)
        frame = std::min(frame, frameCount_ - 1);
    return frame;
}

int ViewerController::frameDomainEnd() const {
    return frameCount_ > 0 ? frameCount_ - 1 : kDefaultFrameCount - 1;
}

void ViewerController::applyFrameCount(int frameCount) {
    frameCount_ = frameCount;
    if (marksAuthored_) {
        inFrame_ = std::clamp(inFrame_, 0, frameDomainEnd());
        outFrame_ = std::clamp(outFrame_, inFrame_, frameDomainEnd());
    } else {
        // Unauthored marks cover the whole probed range.
        inFrame_ = 0;
        outFrame_ = frameDomainEnd();
    }
    const int clamped = clampFrame(frame_);
    if (clamped != frame_) {
        frame_ = clamped;
        emit frameChanged();
        emit timelineChanged();
    }
    emit marksChanged();
}

QVariantList ViewerController::timelineClips() const {
    QVariantList result;
    for (const auto& [key, source] : session_.document().sources) {
        const auto local = static_cast<std::int64_t>(frame_);
        qlonglong sourceFrame = -1;
        try {
            sourceFrame = static_cast<qlonglong>(source.frameAt(local));
        } catch (const std::exception&) {
            // Keep the strip inspectable while an out-of-coverage mapping is
            // being edited; evaluation reports the precise source error.
        }
        QVariantMap clip{{QStringLiteral("id"), QString::fromStdString(key)},
                         {QStringLiteral("source"), QString::fromStdString(key)},
                         {QStringLiteral("start"), 0},
                         {QStringLiteral("end"), frameCount_},
                         {QStringLiteral("offset"), QVariant::fromValue<qlonglong>(source.frameOffset)},
                         {QStringLiteral("step"), QVariant::fromValue<qlonglong>(source.frameStep)},
                         {QStringLiteral("sourceFrame"), QVariant::fromValue<qlonglong>(sourceFrame)}};
        result.push_back(std::move(clip));
    }
    return result;
}

qulonglong ViewerController::queued() const {
    return schedulerCounts_.queued;
}

qulonglong ViewerController::dropped() const {
    return schedulerCounts_.dropped;
}

qulonglong ViewerController::staleRejected() const {
    return schedulerCounts_.staleRejected;
}

qulonglong ViewerController::completed() const {
    return schedulerCounts_.completed;
}

void ViewerController::pollScheduler() {
    // Counters are destination-scoped: a panel reports only its own queued,
    // dropped, stale-rejected and completed work, with the global cache
    // counters beside them. A destination-less facade reports nothing rather
    // than another destination's activity.
    if (!destination_)
        return;
    auto counts = runtime_->counts(*destination_);
    if (counts == schedulerCounts_)
        return;
    schedulerCounts_ = std::move(counts);
    emit schedulerChanged();
}

void ViewerController::documentChanged() {
    error_.clear();
    pending_ = false;
    outdated_ = static_cast<bool>(presentation_);
    refreshViewerTarget();
    // A source node addressed by the routed media target may have appeared or
    // disappeared with this revision.
    refreshContextTarget();
    emit graphChanged();
    emit catalogChanged();
    emit timelineChanged();
    emit historyChanged();
    emit statusChanged();
    invalidateRequest();
    refreshRequest();
}

void ViewerController::invalidateRequest() {
    lastRequest_.reset();
    pending_ = false;
    outdated_ = static_cast<bool>(presentation_);
    generation_ = ++nextRequestId_;
    rangeGeneration_ = 0;
    const bool hadRangeError = !rangeError_.isEmpty();
    rangeError_.clear();
    // Destination-scoped: this panel drops only its own queued work and never
    // moves the global cancel watermark, so sibling panels keep rendering.
    if (destination_)
        runtime_->cancel(generation_, *destination_);
    pollScheduler();
    if (hadRangeError)
        emit schedulerChanged();
}

nemo::EditOptions ViewerController::editOptions() const {
    return nemo::EditOptions{.expectedRevision = session_.revision()};
}

bool ViewerController::applyEdit(const nemo::EditResult& result) {
    if (result.committed)
        return true;
    if (result.error)
        fail(QString::fromStdString(result.error->message));
    else
        fail(QStringLiteral("edit was rejected"));
    return false;
}
void ViewerController::clearError() {
    if (error_.isEmpty())
        return;
    error_.clear();
    emit statusChanged();
}

void ViewerController::buildGraph(const SourceReference& reference) {
    try {
        const auto network = session_.document().rootNetworkId();
        if (!session_.document().sources.empty()) {
            applyEdit(session_.submit(setSourceCommand("src", reference), editOptions()));
            return;
        }
        const auto result = session_.submit(
            Command{
                "open source",
                [reference, network](Document& document) {
                    const auto source = std::make_shared<NodeId>();
                    const auto background = std::make_shared<NodeId>();
                    const auto merge = std::make_shared<NodeId>();
                    const auto output = std::make_shared<NodeId>(document.network(network).defaultOutput());
                    // The default media workflow displays the composite through
                    // a Viewer node rather than relying on an Output name.
                    const auto viewer = std::make_shared<NodeId>();
                    addNodeCommand(network, "source", "source", source, LayoutPosition{60.0, 20.0}).apply(document);
                    addNodeCommand(network, "constcolor", "background", background, LayoutPosition{240.0, 20.0})
                        .apply(document);
                    addNodeCommand(network, "merge", "composite", merge, LayoutPosition{150.0, 80.0}).apply(document);
                    if (*output == kInvalidNode)
                        addNodeCommand(network, "output", "result", output, LayoutPosition{150.0, 140.0})
                            .apply(document);
                    else
                        setLayoutCommand(network, *output, LayoutPosition{150.0, 140.0}).apply(document);
                    addNodeCommand(network, "viewer", "Viewer1", viewer, LayoutPosition{150.0, 200.0}).apply(document);
                    setParamCommand(network, *source, "source", std::string{"src"}).apply(document);
                    setParamCommand(network, *background, "color", ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}})
                        .apply(document);
                    connectCommand(network, {*source, 0}, {*merge, 0}).apply(document);
                    connectCommand(network, {*background, 0}, {*merge, 1}).apply(document);
                    connectCommand(network, {*merge, 0}, {*output, 0}).apply(document);
                    connectCommand(network, {*merge, 0}, {*viewer, 0}).apply(document);
                    setDefaultOutputCommand(network, *output).apply(document);
                    setSourceCommand("src", reference).apply(document);
                }},
            editOptions());
        applyEdit(result);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

QString ViewerController::createGraphNode(const QString& networkValue, const QString& type, const QString& name,
                                          double x, double y, const QVariant& anchorValue,
                                          const QVariantList& shiftedValues) {
    const auto trimmedType = type.trimmed();
    const auto trimmedName = name.trimmed();
    if (trimmedType.isEmpty() || trimmedName.isEmpty()) {
        fail(QStringLiteral("graph node creation requires a type and name"));
        return {};
    }
    if (!finitePosition(x, y)) {
        fail(QStringLiteral("graph node position must be finite"));
        return {};
    }
    const auto network = networkIdentity(networkValue);
    if (!network) {
        fail(QStringLiteral("graph node creation requires a valid network ID"));
        return {};
    }
    std::vector<LayoutEdit> shifted;
    shifted.reserve(static_cast<std::size_t>(shiftedValues.size()));
    for (const auto& value : shiftedValues) {
        const auto map = value.toMap();
        const auto id = graphIdentity(map.value(QStringLiteral("id")));
        double shiftedX = 0.0;
        double shiftedY = 0.0;
        if (!id || !mapPosition(map, shiftedX, shiftedY)) {
            fail(QStringLiteral("graph node shifts require existing IDs and finite x/y coordinates"));
            return {};
        }
        shifted.push_back(LayoutEdit{static_cast<NodeId>(*id), {shiftedX, shiftedY}});
    }
    const auto anchor = graphIdentity(anchorValue);
    const auto id = std::make_shared<NodeId>();
    try {
        static_cast<void>(session_.document().network(*network));
        if (!applyEdit(
                session_.submit(addNodeCommand(*network, trimmedType.toStdString(), trimmedName.toStdString(), id,
                                               LayoutPosition{x, y}, anchor.value_or(kInvalidNode), std::move(shifted)),
                                editOptions())))
            return {};
        return QString::number(*id);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return {};
    }
}

QString ViewerController::insertGraphNode(const QString& networkValue, const QVariant& edgeValue, const QString& type,
                                          const QString& name, double x, double y) {
    const auto edge = graphIdentity(edgeValue);
    const auto trimmedType = type.trimmed();
    const auto trimmedName = name.trimmed();
    const auto network = networkIdentity(networkValue);
    if (!network || !edge || trimmedType.isEmpty() || trimmedName.isEmpty()) {
        fail(QStringLiteral("graph insertion requires a network, edge, type, and name"));
        return {};
    }
    if (!finitePosition(x, y)) {
        fail(QStringLiteral("graph node position must be finite"));
        return {};
    }
    const auto id = std::make_shared<NodeId>();
    try {
        const auto& graph = session_.document().network(*network).graph();
        const auto existing = std::find_if(graph.edges().cbegin(), graph.edges().cend(),
                                           [edge](const Edge& candidate) { return candidate.id == *edge; });
        if (existing == graph.edges().cend()) {
            fail(QStringLiteral("graph insertion edge does not exist"));
            return {};
        }
        if (!applyEdit(
                session_.submit(insertNodeOnEdgeCommand(*network, static_cast<EdgeId>(*edge), trimmedType.toStdString(),
                                                        trimmedName.toStdString(), LayoutPosition{x, y}, id),
                                editOptions())))
            return {};
        return QString::number(*id);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return {};
    }
}

bool ViewerController::insertExistingGraphNodeOnEdge(const QString& networkValue, const QVariant& nodeValue,
                                                     const QVariant& edgeValue, double x, double y) {
    const auto network = networkIdentity(networkValue);
    const auto node = graphIdentity(nodeValue);
    const auto edge = graphIdentity(edgeValue);
    if (!network || !node || !edge || !finitePosition(x, y)) {
        fail(QStringLiteral("graph insertion requires valid network, node, edge IDs, and a finite position"));
        return false;
    }
    try {
        const auto& graph = session_.document().network(*network).graph();
        if (!graph.node(static_cast<NodeId>(*node))) {
            fail(QStringLiteral("graph insertion node does not exist"));
            return false;
        }
        const auto existing = std::find_if(graph.edges().cbegin(), graph.edges().cend(),
                                           [edge](const Edge& candidate) { return candidate.id == *edge; });
        if (existing == graph.edges().cend()) {
            fail(QStringLiteral("graph insertion edge does not exist"));
            return false;
        }
        return applyEdit(
            session_.submit(insertExistingNodeOnEdgeCommand(*network, static_cast<EdgeId>(*edge),
                                                            static_cast<NodeId>(*node), LayoutPosition{x, y}),
                            editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::deleteGraphNodes(const QString& networkValue, const QVariantList& nodeValues) {
    const auto network = networkIdentity(networkValue);
    if (!network || nodeValues.isEmpty()) {
        if (!network)
            fail(QStringLiteral("graph deletion requires a valid network ID"));
        return false;
    }
    try {
        const auto& graph = session_.document().network(*network).graph();
        std::vector<NodeId> ids;
        ids.reserve(static_cast<std::size_t>(nodeValues.size()));
        std::unordered_set<NodeId> unique;
        for (const auto& value : nodeValues) {
            const auto id = graphIdentity(value);
            if (!id || !graph.node(static_cast<NodeId>(*id)) || !unique.insert(static_cast<NodeId>(*id)).second) {
                fail(QStringLiteral("graph deletion requires distinct existing node IDs"));
                return false;
            }
            ids.push_back(static_cast<NodeId>(*id));
        }
        std::vector<Command> commands;
        commands.reserve(ids.size());
        for (const auto id : ids)
            commands.push_back(removeNodeCommand(*network, id));
        return applyEdit(session_.submit(transactionCommand("delete graph nodes", std::move(commands)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::connectOrReplaceGraph(const QString& networkValue, const QVariant& fromValue, int fromPort,
                                             const QVariant& toValue, int toPort) {
    const auto network = networkIdentity(networkValue);
    const auto from = endpointIdentity(fromValue);
    const auto to = endpointIdentity(toValue);
    if (!network || !from || !to || fromPort < 0 || toPort < 0) {
        fail(QStringLiteral("graph connection requires valid endpoints and non-negative ports"));
        return false;
    }
    try {
        auto connect = connectPresentedPorts(session_.document(), *network, *from, fromPort, *to, toPort);
        const auto occupied = occupiedConnection(session_.document(), *network, *to, toPort);
        if (occupied && occupied->from == *from && occupied->fromPort == fromPort) {
            clearError();
            return true;
        }
        std::vector<Command> edits;
        if (occupied)
            edits.push_back(occupied->disconnect);
        edits.push_back(std::move(connect));
        return applyEdit(session_.submit(transactionCommand("connect graph ports", std::move(edits)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}
bool ViewerController::rewireGraphEdge(const QString& networkValue, const QVariant& edgeValue,
                                       const QVariant& fromValue, int fromPort, const QVariant& toValue, int toPort) {
    const auto network = networkIdentity(networkValue);
    const auto from = endpointIdentity(fromValue);
    const auto to = endpointIdentity(toValue);
    if (!network || !from || !to || fromPort < 0 || toPort < 0) {
        fail(QStringLiteral("graph rewiring requires valid endpoints and non-negative ports"));
        return false;
    }
    try {
        const auto existing = resolveConnection(session_.document(), *network, edgeValue.toString());
        if (existing.from == *from && existing.to == *to && existing.fromPort == fromPort &&
            existing.toPort == toPort) {
            clearError();
            return true;
        }
        auto connect = connectPresentedPorts(session_.document(), *network, *from, fromPort, *to, toPort);
        const auto occupied = occupiedConnection(session_.document(), *network, *to, toPort);
        std::vector<Command> edits;
        if (occupied && occupied->id != existing.id)
            edits.push_back(occupied->disconnect);
        const auto edgeId = graphIdentity(existing.id);
        const auto* target =
            to->kind == EndpointKind::Graph ? session_.document().network(*network).graph().node(to->node) : nullptr;
        if (edgeId && from->kind == EndpointKind::Graph && target && target->instance == kInvalidNetworkInstance) {
            edits.push_back(rewireGraphEdgeCommand(*network, *edgeId,
                                                   {from->node, static_cast<std::uint32_t>(fromPort)},
                                                   {to->node, static_cast<std::uint32_t>(toPort)}));
        } else {
            edits.push_back(existing.disconnect);
            edits.push_back(std::move(connect));
        }
        return applyEdit(session_.submit(transactionCommand("rewire graph ports", std::move(edits)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::disconnectGraphEdge(const QString& networkValue, const QVariant& edgeValue) {
    const auto network = networkIdentity(networkValue);
    if (!network) {
        fail(QStringLiteral("graph disconnection requires a valid network ID"));
        return false;
    }
    try {
        return applyEdit(session_.submit(
            resolveConnection(session_.document(), *network, edgeValue.toString()).disconnect, editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::commitGraphMove(const QString& networkValue, const QVariantList& positions) {
    const auto network = networkIdentity(networkValue);
    if (!network || positions.isEmpty()) {
        if (!network)
            fail(QStringLiteral("graph move requires a valid network ID"));
        return false;
    }
    try {
        const auto& scoped = session_.document().network(*network);
        std::vector<Command> commands;
        commands.reserve(static_cast<std::size_t>(positions.size()));
        std::unordered_set<std::string> unique;
        bool changed = false;
        for (const auto& value : positions) {
            const auto map = value.toMap();
            const auto idValue = map.value(QStringLiteral("id"));
            const auto idText = idValue.toString().trimmed();
            double x = 0.0;
            double y = 0.0;
            if (idText.isEmpty() || !mapPosition(map, x, y) || !unique.insert(idText.toStdString()).second) {
                fail(QStringLiteral("graph move requires distinct IDs and finite x/y positions"));
                return false;
            }
            if (const auto terminal = terminalNodeIdentity(idValue)) {
                const auto* formal = terminal->first == PortDirection::Input ? scoped.input(terminal->second)
                                                                             : scoped.output(terminal->second);
                if (!formal) {
                    fail(QStringLiteral("graph move terminal does not exist"));
                    return false;
                }
                changed = changed || formal->layout != LayoutPosition{x, y};
                commands.push_back(setInterfaceLayoutCommand(*network, terminal->first, terminal->second, {x, y}));
                continue;
            }
            const auto id = graphIdentity(idValue);
            if (!id || !scoped.graph().node(static_cast<NodeId>(*id))) {
                fail(QStringLiteral("graph move requires existing graph or formal terminal IDs"));
                return false;
            }
            const auto* node = scoped.graph().node(static_cast<NodeId>(*id));
            changed = changed || node->layout != LayoutPosition{x, y};
            commands.push_back(setLayoutCommand(*network, static_cast<NodeId>(*id), {x, y}));
        }
        if (!changed) {
            clearError();
            return true;
        }
        return applyEdit(session_.submit(transactionCommand("move graph items", std::move(commands)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::commitGraphRoute(const QString& networkValue, const QVariant& edgeValue,
                                        const QVariantList& points) {
    const auto network = networkIdentity(networkValue);
    const auto edge = graphIdentity(edgeValue);
    if (!network || !edge) {
        fail(QStringLiteral("graph route requires valid network and edge IDs"));
        return false;
    }
    std::vector<LayoutPosition> route;
    route.reserve(static_cast<std::size_t>(points.size()));
    for (const auto& value : points) {
        double x = 0.0;
        double y = 0.0;
        if (!mapPosition(value.toMap(), x, y)) {
            fail(QStringLiteral("graph route points require finite x/y coordinates"));
            return false;
        }
        route.push_back(LayoutPosition{x, y});
    }
    try {
        const auto& graph = session_.document().network(*network).graph();
        const auto existing = std::find_if(graph.edges().cbegin(), graph.edges().cend(),
                                           [edge](const Edge& candidate) { return candidate.id == *edge; });
        if (existing != graph.edges().cend() && existing->route == route) {
            clearError();
            return true;
        }
        return applyEdit(
            session_.submit(setRouteCommand(*network, static_cast<EdgeId>(*edge), std::move(route)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::insertGraphRoutePoint(const QString& networkValue, const QVariant& edgeValue, int index,
                                             double x, double y) {
    const auto network = networkIdentity(networkValue);
    const auto edge = graphIdentity(edgeValue);
    if (!network || !edge || index < 0 || !finitePosition(x, y)) {
        fail(QStringLiteral("graph route insertion requires a valid network, edge, index, and position"));
        return false;
    }
    try {
        return applyEdit(session_.submit(
            insertRoutePointCommand(*network, static_cast<EdgeId>(*edge), static_cast<std::size_t>(index), {x, y}),
            editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::moveGraphRoutePoint(const QString& networkValue, const QVariant& edgeValue, int index, double x,
                                           double y) {
    const auto network = networkIdentity(networkValue);
    const auto edge = graphIdentity(edgeValue);
    if (!network || !edge || index < 0 || !finitePosition(x, y)) {
        fail(QStringLiteral("graph route move requires a valid network, edge, index, and position"));
        return false;
    }
    try {
        return applyEdit(session_.submit(
            moveRoutePointCommand(*network, static_cast<EdgeId>(*edge), static_cast<std::size_t>(index), {x, y}),
            editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::removeGraphRoutePoint(const QString& networkValue, const QVariant& edgeValue, int index) {
    const auto network = networkIdentity(networkValue);
    const auto edge = graphIdentity(edgeValue);
    if (!network || !edge || index < 0) {
        fail(QStringLiteral("graph route removal requires a valid network, edge, and index"));
        return false;
    }
    try {
        return applyEdit(session_.submit(
            removeRoutePointCommand(*network, static_cast<EdgeId>(*edge), static_cast<std::size_t>(index)),
            editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}
QString ViewerController::collapseSelection(const QString& networkValue, const QVariantList& nodeValues,
                                            const QString& nameValue) {
    const auto network = networkIdentity(networkValue);
    if (!network || nodeValues.isEmpty()) {
        fail(QStringLiteral("collapse requires a valid network ID and at least one selected node"));
        return {};
    }
    std::vector<NodeId> nodes;
    nodes.reserve(static_cast<std::size_t>(nodeValues.size()));
    std::unordered_set<NodeId> unique;
    try {
        const auto& graph = session_.document().network(*network).graph();
        for (const auto& value : nodeValues) {
            const auto id = graphIdentity(value);
            if (!id || !graph.node(static_cast<NodeId>(*id)) || !unique.insert(static_cast<NodeId>(*id)).second) {
                fail(QStringLiteral("collapse requires distinct existing node IDs"));
                return {};
            }
            nodes.push_back(static_cast<NodeId>(*id));
        }
        const auto name = nameValue.trimmed();
        if (name.isEmpty()) {
            fail(QStringLiteral("collapse requires a non-empty subnet name"));
            return {};
        }
        const auto createdInstance = std::make_shared<NetworkInstanceId>();
        if (!applyEdit(session_.submit(
                collapseSelectionCommand(*network, std::move(nodes), name.toStdString(), createdInstance),
                editOptions())))
            return {};

        // The command writes the occurrence identity into createdInstance. Read
        // the node identity from the committed document, not from a QML guess.
        const auto* instance = session_.document().instance(*createdInstance);
        if (!instance || instance->parentNetwork != *network || instance->node == kInvalidNode) {
            fail(QStringLiteral("collapse committed without a readable subnet instance"));
            return {};
        }
        clearError();
        return QString::number(instance->node);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return {};
    }
}

bool ViewerController::unpackInstance(const QString& instanceValue) {
    const auto identity = graphIdentity(instanceValue);
    if (!identity) {
        fail(QStringLiteral("unpack requires a valid network instance ID"));
        return false;
    }
    try {
        return applyEdit(
            session_.submit(unpackInstanceCommand(static_cast<NetworkInstanceId>(*identity)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

QVariantMap ViewerController::subnetExposure(const QString& networkValue, const QVariant& nodeValue) const {
    const auto failure = [&networkValue, &nodeValue](const QString& reason) {
        return QVariantMap{
            {QStringLiteral("available"), false},        {QStringLiteral("reason"), reason},
            {QStringLiteral("networkId"), networkValue}, {QStringLiteral("nodeId"), nodeValue.toString()},
            {QStringLiteral("instanceId"), QString{}},   {QStringLiteral("definition"), QString{}},
            {QStringLiteral("name"), QString{}},         {QStringLiteral("linkState"), QString{}},
            {QStringLiteral("rows"), QVariantList{}}};
    };
    const auto network = networkIdentity(networkValue);
    const auto node = graphIdentity(nodeValue);
    if (!network || !node)
        return failure(QStringLiteral("subnet exposure requires a network and a subnet node"));
    try {
        const auto& document = session_.document();
        const auto& parent = document.network(*network);
        const auto* occurrenceNode = parent.graph().node(static_cast<NodeId>(*node));
        if (!occurrenceNode || occurrenceNode->instance == kInvalidNetworkInstance)
            return failure(QStringLiteral("selected node is not a subnet occurrence"));
        const auto* occurrence = document.instance(occurrenceNode->instance);
        if (!occurrence)
            return failure(QStringLiteral("subnet occurrence is unavailable"));
        const auto& definition = document.network(occurrence->definition);
        std::size_t references = 0;
        for (const auto& other : document.instances())
            if (other.definition == occurrence->definition)
                ++references;
        QVariantList rows;
        for (const auto& exposed : definition.exposedParameters()) {
            const auto* child = definition.graph().node(exposed.node);
            if (!child)
                continue;
            const auto* spec = definition.graph().catalog().parameterSpec(child->type, exposed.key);
            if (!spec)
                continue;
            rows.push_back(
                QVariantMap{{QStringLiteral("id"), QString::number(exposed.id)},
                            {QStringLiteral("node"), QString::number(exposed.node)},
                            {QStringLiteral("nodeName"), QString::fromStdString(child->name)},
                            {QStringLiteral("key"), QString::fromStdString(exposed.key)},
                            {QStringLiteral("name"), QString::fromStdString(exposed.name)},
                            {QStringLiteral("label"), spec->label.empty() ? humanizedParameterLabel(exposed.key)
                                                                          : QString::fromStdString(spec->label)},
                            {QStringLiteral("type"), QString::fromLatin1(parameterTypeName(spec->type))},
                            {QStringLiteral("kind"), QString::fromLatin1(parameterKindName(spec->type))},
                            {QStringLiteral("source"), QString::fromStdString(child->name + "." + exposed.key)}});
        }
        const QString linkState = references > 1               ? QStringLiteral("shared")
                                  : occurrence->ownsDefinition ? QStringLiteral("local")
                                                               : QStringLiteral("linked");
        return QVariantMap{{QStringLiteral("available"), true},
                           {QStringLiteral("reason"), QString{}},
                           {QStringLiteral("networkId"), QString::number(*network)},
                           {QStringLiteral("nodeId"), QString::number(static_cast<NodeId>(*node))},
                           {QStringLiteral("instanceId"), QString::number(occurrence->id)},
                           {QStringLiteral("definition"), QString::number(occurrence->definition)},
                           {QStringLiteral("name"), QString::fromStdString(occurrenceNode->name)},
                           {QStringLiteral("definitionName"), QString::fromStdString(definition.name())},
                           {QStringLiteral("linkState"), linkState},
                           {QStringLiteral("rows"), rows}};
    } catch (const std::exception& error) {
        return failure(QString::fromUtf8(error.what()));
    }
}

bool ViewerController::promoteParameter(const QString& networkValue, const QVariant& nodeValue, const QString& keyValue,
                                        const QString& nameValue, int index) {
    const auto network = networkIdentity(networkValue);
    const auto node = graphIdentity(nodeValue);
    const auto key = keyValue.trimmed();
    if (!network || !node || key.isEmpty() || index < -1) {
        fail(QStringLiteral(
            "promote requires a definition network, a node, a parameter key and a valid insertion index"));
        return false;
    }
    try {
        const auto& definition = session_.document().network(*network);
        const auto* source = definition.graph().node(static_cast<NodeId>(*node));
        const auto* spec =
            source ? definition.graph().catalog().parameterSpec(source->type, key.toStdString()) : nullptr;
        if (!source || !spec) {
            fail(QStringLiteral("promote target has no parameter '%1'").arg(key));
            return false;
        }
        const auto requested = nameValue.trimmed();
        const auto name = requested.isEmpty()
                              ? QString::fromStdString(uniqueExposedName(definition, *spec, key.toStdString()))
                              : requested;
        return applyEdit(session_.submit(
            promoteParameterCommand(*network, static_cast<NodeId>(*node), key.toStdString(), name.toStdString(), {},
                                    index < 0 ? std::nullopt : std::optional<std::size_t>(index)),
            editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::renameExposedParameter(const QString& networkValue, const QVariant& parameterValue,
                                              const QString& nameValue) {
    const auto network = networkIdentity(networkValue);
    const auto parameter = graphIdentity(parameterValue);
    const auto name = nameValue.trimmed();
    if (!network || !parameter || name.isEmpty()) {
        fail(QStringLiteral("rename requires a definition network, an exposed parameter and a name"));
        return false;
    }
    try {
        if (!session_.document().network(*network).exposedParameter(static_cast<InterfacePortId>(*parameter))) {
            fail(
                QStringLiteral("network %1 does not expose parameter %2").arg(networkValue, parameterValue.toString()));
            return false;
        }
        return applyEdit(session_.submit(
            renameExposedParameterCommand(*network, static_cast<InterfacePortId>(*parameter), name.toStdString()),
            editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::removeExposedParameter(const QString& networkValue, const QVariant& parameterValue) {
    const auto network = networkIdentity(networkValue);
    const auto parameter = graphIdentity(parameterValue);
    if (!network || !parameter) {
        fail(QStringLiteral("remove requires a definition network and an exposed parameter"));
        return false;
    }
    try {
        if (!session_.document().network(*network).exposedParameter(static_cast<InterfacePortId>(*parameter))) {
            fail(
                QStringLiteral("network %1 does not expose parameter %2").arg(networkValue, parameterValue.toString()));
            return false;
        }
        return applyEdit(session_.submit(
            removeExposedParameterCommand(*network, static_cast<InterfacePortId>(*parameter)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::moveExposedParameter(const QString& networkValue, const QVariant& parameterValue, int index) {
    const auto network = networkIdentity(networkValue);
    const auto parameter = graphIdentity(parameterValue);
    if (!network || !parameter || index < 0) {
        fail(QStringLiteral("reorder requires a definition network, an exposed parameter and a row index"));
        return false;
    }
    try {
        const auto& definition = session_.document().network(*network);
        if (!definition.exposedParameter(static_cast<InterfacePortId>(*parameter))) {
            fail(
                QStringLiteral("network %1 does not expose parameter %2").arg(networkValue, parameterValue.toString()));
            return false;
        }
        return applyEdit(session_.submit(moveExposedParameterCommand(*network, static_cast<InterfacePortId>(*parameter),
                                                                     static_cast<std::size_t>(index)),
                                         editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

QString ViewerController::duplicateLinkedInstance(const QString& networkValue, const QVariant& nodeValue, double x,
                                                  double y) {
    const auto network = networkIdentity(networkValue);
    const auto node = graphIdentity(nodeValue);
    if (!network || !node || !finitePosition(x, y)) {
        fail(QStringLiteral("duplicate requires a network, a subnet node and a finite position"));
        return {};
    }
    try {
        const auto& document = session_.document();
        const auto& parent = document.network(*network);
        const auto* occurrenceNode = parent.graph().node(static_cast<NodeId>(*node));
        if (!occurrenceNode || occurrenceNode->instance == kInvalidNetworkInstance) {
            fail(QStringLiteral("duplicate requires a selected subnet occurrence"));
            return {};
        }
        const auto* occurrence = document.instance(occurrenceNode->instance);
        if (!occurrence) {
            fail(QStringLiteral("subnet occurrence is unavailable"));
            return {};
        }
        const auto name = uniqueOccurrenceName(parent.graph(), occurrenceNode->name + " Copy");
        const auto created = std::make_shared<NetworkInstanceId>();
        if (!applyEdit(session_.submit(
                createLinkedInstanceCommand(*network, occurrence->definition, name, LayoutPosition{x, y}, created),
                editOptions())))
            return {};
        const auto* createdInstance = session_.document().instance(*created);
        if (!createdInstance) {
            fail(QStringLiteral("duplicate committed without a readable instance"));
            return {};
        }
        clearError();
        return QString::number(createdInstance->node);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return {};
    }
}

bool ViewerController::makeIndependent(const QString& instanceValue) {
    const auto identity = graphIdentity(instanceValue);
    if (!identity) {
        fail(QStringLiteral("make independent requires a valid network instance ID"));
        return false;
    }
    try {
        return applyEdit(
            session_.submit(makeIndependentCommand(static_cast<NetworkInstanceId>(*identity)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::copyGraphSelection(const QString& networkValue, const QVariantList& nodeValues) {
    const auto network = networkIdentity(networkValue);
    if (!network || nodeValues.isEmpty()) {
        fail(QStringLiteral("copy requires a network and a non-empty selection"));
        return false;
    }
    std::vector<NodeId> nodes;
    nodes.reserve(static_cast<std::size_t>(nodeValues.size()));
    std::unordered_set<NodeId> unique;
    try {
        const auto& graph = session_.document().network(*network).graph();
        for (const auto& value : nodeValues) {
            const auto id = graphIdentity(value);
            if (!id || !graph.node(static_cast<NodeId>(*id)) || !unique.insert(static_cast<NodeId>(*id)).second) {
                fail(QStringLiteral("copy requires distinct existing node IDs"));
                return false;
            }
            nodes.push_back(static_cast<NodeId>(*id));
        }
        clipboardNetwork_ = *network;
        clipboardNodes_ = std::move(nodes);
        clearError();
        return true;
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

QString ViewerController::pasteGraphSelection(const QString& networkValue, double x, double y) {
    if (!clipboardNetwork_ || clipboardNodes_.empty()) {
        fail(QStringLiteral("nothing has been copied"));
        return {};
    }
    const auto network = networkIdentity(networkValue);
    if (!network || !finitePosition(x, y)) {
        fail(QStringLiteral("paste requires a network and a finite position"));
        return {};
    }
    try {
        const auto& graph = session_.document().network(*clipboardNetwork_).graph();
        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        for (const auto id : clipboardNodes_) {
            const auto* node = graph.node(id);
            if (!node)
                continue;
            minX = std::min(minX, node->layout.x);
            minY = std::min(minY, node->layout.y);
        }
        if (minX == std::numeric_limits<double>::max()) {
            fail(QStringLiteral("copied selection no longer exists"));
            return {};
        }
        const auto created = std::make_shared<std::vector<NodeId>>();
        if (!applyEdit(session_.submit(copySelectionCommand(*clipboardNetwork_, clipboardNodes_, *network,
                                                            LayoutPosition{x - minX, y - minY}, created),
                                       editOptions())))
            return {};
        QStringList ids;
        ids.reserve(static_cast<qsizetype>(created->size()));
        for (const auto id : *created)
            ids.push_back(QString::number(id));
        clearError();
        return ids.join(QLatin1Char(','));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return {};
    }
}

void ViewerController::setNodeParameter(const QVariant& nodeValue, const QString& keyValue, const QVariant& value) {
    // QML identities travel as decimal strings, not lossy JavaScript doubles.
    bool validId = false;
    const auto nodeId = nodeValue.toString().toULongLong(&validId);
    const auto key = keyValue.trimmed();
    if (!validId || nodeId == static_cast<qulonglong>(kInvalidNode) || key.isEmpty()) {
        fail(QStringLiteral("node parameter requires a node ID and key"));
        return;
    }
    const auto network = session_.document().rootNetworkId();
    try {
        const auto& graph = session_.document().network(network).graph();
        const auto* node = graph.node(static_cast<NodeId>(nodeId));
        if (!node) {
            fail(QStringLiteral("node parameter target does not exist"));
            return;
        }
        QString conversionError;
        const auto converted = parameterValueFromVariant(graph.catalog(), graph.descriptor(node->type),
                                                         key.toStdString(), value, conversionError);
        if (!converted) {
            fail(conversionError);
            return;
        }
        const auto it = node->params.find(key.toStdString());
        if (it != node->params.end() && it->second == *converted) {
            clearError();
            return;
        }
        applyEdit(session_.submit(setParamCommand(network, static_cast<NodeId>(nodeId), key.toStdString(), *converted),
                                  editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::setNodeParameterText(const QVariant& nodeValue, const QString& keyValue, const QString& text) {
    bool validId = false;
    const auto nodeId = nodeValue.toString().toULongLong(&validId);
    const auto key = keyValue.trimmed().toStdString();
    const auto network = session_.document().rootNetworkId();
    if (!validId) {
        fail(QStringLiteral("node parameter text requires an existing node ID and key"));
        return;
    }
    try {
        const auto& graph = session_.document().network(network).graph();
        const auto* node = graph.node(static_cast<NodeId>(nodeId));
        if (!node || key.empty()) {
            fail(QStringLiteral("node parameter text requires an existing node ID and key"));
            return;
        }
        const auto value = graph.catalog().parseParameterText(node->type, key, text.toStdString());
        setNodeParameter(nodeValue, keyValue, parameterValueVariant(value));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::resetNodeParameter(const QVariant& nodeValue, const QString& keyValue) {
    bool validId = false;
    const auto nodeId = nodeValue.toString().toULongLong(&validId);
    const auto key = keyValue.trimmed();
    if (!validId || nodeId == static_cast<qulonglong>(kInvalidNode) || key.isEmpty()) {
        fail(QStringLiteral("node parameter reset requires a node ID and key"));
        return;
    }
    const auto network = session_.document().rootNetworkId();
    try {
        const auto& graph = session_.document().network(network).graph();
        const auto* node = graph.node(static_cast<NodeId>(nodeId));
        if (!node) {
            fail(QStringLiteral("node parameter target does not exist"));
            return;
        }
        if (!node->params.contains(key.toStdString())) {
            clearError();
            return;
        }
        applyEdit(
            session_.submit(resetParamCommand(network, static_cast<NodeId>(nodeId), key.toStdString()), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::setNodeParameters(const QVariantList& edits) {
    if (edits.isEmpty()) {
        fail(QStringLiteral("parameter batch requires at least one edit"));
        return;
    }
    const auto network = session_.document().rootNetworkId();
    try {
        const auto& graph = session_.document().network(network).graph();
        std::vector<nemo::ParameterEdit> converted;
        converted.reserve(static_cast<std::size_t>(edits.size()));
        for (const auto& entry : edits) {
            const auto map = entry.toMap();
            bool validId = false;
            const auto nodeId = map.value(QStringLiteral("nodeId")).toString().toULongLong(&validId);
            const auto key = map.value(QStringLiteral("key")).toString().trimmed();
            if (!validId || nodeId == static_cast<qulonglong>(kInvalidNode) || key.isEmpty()) {
                fail(QStringLiteral("parameter batch entries require a node ID and key"));
                return;
            }
            const auto* node = graph.node(static_cast<NodeId>(nodeId));
            if (!node) {
                fail(QStringLiteral("node parameter target does not exist"));
                return;
            }
            std::optional<nemo::ParameterValue> value;
            if (map.contains(QStringLiteral("value")) && map.value(QStringLiteral("value")).isValid()) {
                QString conversionError;
                value = parameterValueFromVariant(graph.catalog(), graph.descriptor(node->type), key.toStdString(),
                                                  map.value(QStringLiteral("value")), conversionError);
                if (!value) {
                    fail(conversionError);
                    return;
                }
            }
            converted.push_back(nemo::ParameterEdit{
                nemo::ParameterAddress{network, static_cast<NodeId>(nodeId), key.toStdString()}, std::move(value)});
        }
        applyEdit(session_.submit(setParametersCommand(std::move(converted)), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

QVariantMap ViewerController::subnetInspector(nemo::NetworkId network, nemo::NodeId node) const {
    const auto unavailable = [network, node](const QString& reason) {
        return QVariantMap{{QStringLiteral("available"), false},
                           {QStringLiteral("reason"), reason},
                           {QStringLiteral("networkId"), QString::number(network)},
                           {QStringLiteral("nodeId"), QString::number(node)},
                           {QStringLiteral("instanceId"), QString{}},
                           {QStringLiteral("name"), QString{}},
                           {QStringLiteral("type"), QString{}},
                           {QStringLiteral("category"), QString{}},
                           {QStringLiteral("linkState"), QString{}},
                           {QStringLiteral("sections"), QVariantList{}}};
    };
    try {
        const auto& document = session_.document();
        const auto& parent = document.network(network);
        const auto* occurrenceNode = parent.graph().node(node);
        const auto* occurrence = occurrenceNode && occurrenceNode->instance != nemo::kInvalidNetworkInstance
                                     ? document.instance(occurrenceNode->instance)
                                     : nullptr;
        if (!occurrence)
            return unavailable(QStringLiteral("node '%1' is not a live subnet occurrence").arg(node));
        const auto& definition = document.network(occurrence->definition);
        std::size_t references = 0;
        for (const auto& other : document.instances())
            if (other.definition == occurrence->definition)
                ++references;
        const QString linkState = references > 1               ? QStringLiteral("shared")
                                  : occurrence->ownsDefinition ? QStringLiteral("local")
                                                               : QStringLiteral("linked");
        const auto frame = static_cast<double>(frame_);
        QVariantList rows;
        for (const auto& exposed : definition.exposedParameters()) {
            const auto* child = definition.graph().node(exposed.node);
            const auto* spec = child ? definition.graph().catalog().parameterSpec(child->type, exposed.key) : nullptr;
            if (!child || !spec)
                continue;
            const nemo::ParameterAddress address{occurrence->definition, exposed.node, exposed.key, occurrence->id};
            const auto keyState = parameterKeyState(document, address, frame);
            QVariantMap row{{QStringLiteral("key"), exposedParameterToken(exposed.id)},
                            {QStringLiteral("label"), QString::fromStdString(exposed.name)},
                            {QStringLiteral("source"), QString::fromStdString(child->name + "." + exposed.key)},
                            {QStringLiteral("type"), QString::fromLatin1(parameterTypeName(spec->type))},
                            {QStringLiteral("kind"), QString::fromLatin1(parameterKindName(spec->type))},
                            {QStringLiteral("value"),
                             parameterValueVariant(nemo::animatedParameterValue(document, address, frame))},
                            {QStringLiteral("animated"), keyState.animated},
                            {QStringLiteral("keyed"), keyState.keyed},
                            {QStringLiteral("editor"), QString::fromStdString(spec->editor)}};
            if (spec->minimum)
                row.insert(QStringLiteral("minimum"), *spec->minimum);
            if (spec->maximum)
                row.insert(QStringLiteral("maximum"), *spec->maximum);
            if (spec->step)
                row.insert(QStringLiteral("step"), *spec->step);
            QVariantList choices;
            for (const auto& choice : spec->choices)
                choices.push_back(QString::fromStdString(choice));
            row.insert(QStringLiteral("choices"), choices);
            rows.push_back(std::move(row));
        }
        QVariantList sections;
        sections.push_back(QVariantMap{{QStringLiteral("name"), QStringLiteral("Exposed Parameters")},
                                       {QStringLiteral("parameters"), rows}});
        return QVariantMap{
            {QStringLiteral("available"), true},
            {QStringLiteral("reason"), QString{}},
            {QStringLiteral("networkId"), QString::number(network)},
            {QStringLiteral("nodeId"), QString::number(node)},
            {QStringLiteral("instanceId"), QString::number(occurrence->id)},
            {QStringLiteral("name"), QString::fromStdString(occurrenceNode->name)},
            {QStringLiteral("type"),
             QStringLiteral("Subnet \u00b7 ") + (linkState == QStringLiteral("local")    ? QStringLiteral("local")
                                                 : linkState == QStringLiteral("shared") ? QStringLiteral("shared")
                                                                                         : QStringLiteral("linked"))},
            {QStringLiteral("category"), QStringLiteral("Subnet")},
            {QStringLiteral("linkState"), linkState},
            {QStringLiteral("sections"), sections}};
    } catch (const std::exception& failure) {
        return unavailable(QString::fromUtf8(failure.what()));
    }
}

QVariantMap ViewerController::parameterInspector(const QString& networkValue, const QVariant& nodeValue) const {
    const auto unavailable = [&networkValue, &nodeValue](const QString& reason) {
        return QVariantMap{
            {QStringLiteral("available"), false},        {QStringLiteral("reason"), reason},
            {QStringLiteral("networkId"), networkValue}, {QStringLiteral("nodeId"), nodeValue.toString()},
            {QStringLiteral("instanceId"), QString{}},   {QStringLiteral("name"), QString{}},
            {QStringLiteral("type"), QString{}},         {QStringLiteral("category"), QString{}},
            {QStringLiteral("sections"), QVariantList{}}};
    };
    QString error;
    // A subnet occurrence presents its definition's exposed controls rather
    // than the synthetic network.instance schema, which has no parameters.
    if (const auto network = networkIdentity(networkValue)) {
        if (const auto node = graphIdentity(nodeValue)) {
            const nemo::NodeInstance* candidate = nullptr;
            try {
                candidate = session_.document().network(*network).graph().node(static_cast<NodeId>(*node));
            } catch (const std::exception&) {
                candidate = nullptr;
            }
            if (candidate && candidate->instance != nemo::kInvalidNetworkInstance)
                return subnetInspector(*network, static_cast<NodeId>(*node));
        }
    }
    const auto target = resolveInspectorTarget(session_.document(), networkValue, nodeValue, {}, error);
    if (!target)
        return unavailable(error);
    try {
        const auto frame = static_cast<double>(frame_);
        const auto allValues = session_.queryValues(target->network, target->node);
        std::vector<std::pair<QString, QVariantList>> sections;
        for (const auto& spec : target->descriptor->parameters) {
            const auto sectionName =
                spec.section.empty() ? QStringLiteral("Properties") : QString::fromStdString(spec.section);
            auto section = std::find_if(sections.begin(), sections.end(),
                                        [&sectionName](const auto& entry) { return entry.first == sectionName; });
            if (section == sections.end()) {
                sections.emplace_back(sectionName, QVariantList{});
                section = std::prev(sections.end());
            }
            const nemo::ParameterAddress address{target->network, target->node, spec.name, target->instance->instance};
            const auto keyState = parameterKeyState(session_.document(), address, frame);
            QVariant value;
            if (keyState.animated) {
                value = parameterValueVariant(nemo::animatedParameterValue(session_.document(), address, frame));
            } else {
                const auto authored = std::find_if(allValues.begin(), allValues.end(),
                                                   [&spec](const auto& entry) { return entry.key == spec.name; });
                value = authored != allValues.end() ? parameterValueVariant(authored->value)
                                                    : parameterValueVariant(spec.defaultValue);
            }
            QVariantMap row{{QStringLiteral("key"), QString::fromStdString(spec.name)},
                            {QStringLiteral("label"), spec.label.empty() ? humanizedParameterLabel(spec.name)
                                                                         : QString::fromStdString(spec.label)},
                            {QStringLiteral("type"), QString::fromLatin1(parameterTypeName(spec.type))},
                            {QStringLiteral("kind"), QString::fromLatin1(parameterKindName(spec.type))},
                            {QStringLiteral("value"), value},
                            {QStringLiteral("animated"), keyState.animated},
                            {QStringLiteral("keyed"), keyState.keyed},
                            {QStringLiteral("editor"), QString::fromStdString(spec.editor)}};
            if (spec.minimum)
                row.insert(QStringLiteral("minimum"), *spec.minimum);
            if (spec.maximum)
                row.insert(QStringLiteral("maximum"), *spec.maximum);
            if (spec.step)
                row.insert(QStringLiteral("step"), *spec.step);
            QVariantList choices;
            for (const auto& choice : spec.choices)
                choices.push_back(QString::fromStdString(choice));
            row.insert(QStringLiteral("choices"), choices);
            section->second.push_back(row);
        }
        QVariantList sectionList;
        sectionList.reserve(static_cast<qsizetype>(sections.size()));
        for (auto& section : sections)
            sectionList.push_back(
                QVariantMap{{QStringLiteral("name"), section.first}, {QStringLiteral("parameters"), section.second}});
        return QVariantMap{{QStringLiteral("available"), true},
                           {QStringLiteral("reason"), QString{}},
                           {QStringLiteral("networkId"), QString::number(target->network)},
                           {QStringLiteral("nodeId"), QString::number(target->node)},
                           {QStringLiteral("instanceId"), QString::number(target->instance->instance)},
                           {QStringLiteral("name"), QString::fromStdString(target->instance->name)},
                           {QStringLiteral("type"), QString::fromStdString(target->instance->type)},
                           {QStringLiteral("category"), QString::fromStdString(target->descriptor->group)},
                           {QStringLiteral("sections"), sectionList}};
    } catch (const std::exception& failure) {
        return unavailable(QString::fromUtf8(failure.what()));
    }
}

QString ViewerController::nodeParameterKeyStatus(const QString& networkValue, const QVariant& nodeValue,
                                                 const QString& keyValue) const {
    QString error;
    const auto target =
        resolveInspectorTarget(session_.document(), networkValue, nodeValue, keyValue.trimmed().toStdString(), error);
    if (!target)
        return QStringLiteral("none");
    const auto state = parameterKeyState(session_.document(), target->address, static_cast<double>(frame_));
    if (!state.animated)
        return QStringLiteral("none");
    return state.keyed ? QStringLiteral("key") : QStringLiteral("animated");
}

bool ViewerController::keyNodeParameter(const QString& networkValue, const QVariant& nodeValue,
                                        const QString& keyValue) {
    QString error;
    const auto target =
        resolveInspectorTarget(session_.document(), networkValue, nodeValue, keyValue.trimmed().toStdString(), error);
    if (!target) {
        fail(error);
        return false;
    }
    if (!target->spec) {
        fail(QStringLiteral("node parameter key must not be empty"));
        return false;
    }
    try {
        const auto frame = static_cast<double>(frame_);
        const auto state = parameterKeyState(session_.document(), target->address, frame);
        const auto current = nemo::animatedParameterValue(session_.document(), target->address, frame);
        nemo::Keyframe replacement;
        replacement.time = frame;
        replacement.value = current;
        if (state.keyed) {
            const auto keys = session_.queryAnimationKeys(state.channel);
            const auto existing =
                std::find_if(keys.begin(), keys.end(),
                             [frame](const nemo::AnimationKeyQueryResult& value) { return value.key.time == frame; });
            if (existing == keys.end()) {
                fail(QStringLiteral("animation channel '%1' lost its key at frame %2").arg(state.channel).arg(frame));
                return false;
            }
            if (existing->key.value == current) {
                clearError();
                return true;
            }
            replacement = existing->key;
        } else {
            replacement.interpolation = nemo::animation_detail::componentCount(target->spec->type) == 0
                                            ? nemo::KeyInterpolation::Hold
                                            : nemo::KeyInterpolation::Linear;
        }
        return applyEdit(session_.submit(
            nemo::setKeyframesCommand({nemo::KeyframeEdit{target->address, std::move(replacement)}}), editOptions()));
    } catch (const std::exception& failure) {
        fail(QString::fromUtf8(failure.what()));
        return false;
    }
}

bool ViewerController::removeNodeParameterKey(const QString& networkValue, const QVariant& nodeValue,
                                              const QString& keyValue) {
    QString error;
    const auto target =
        resolveInspectorTarget(session_.document(), networkValue, nodeValue, keyValue.trimmed().toStdString(), error);
    if (!target) {
        fail(error);
        return false;
    }
    if (!target->spec) {
        fail(QStringLiteral("node parameter key must not be empty"));
        return false;
    }
    try {
        const auto frame = static_cast<double>(frame_);
        const auto state = parameterKeyState(session_.document(), target->address, frame);
        if (!state.keyed) {
            clearError();
            return false;
        }
        const auto keys = session_.queryAnimationKeys(state.channel);
        const auto existing =
            std::find_if(keys.begin(), keys.end(),
                         [frame](const nemo::AnimationKeyQueryResult& value) { return value.key.time == frame; });
        if (existing == keys.end()) {
            fail(QStringLiteral("animation channel '%1' lost its key at frame %2").arg(state.channel).arg(frame));
            return false;
        }
        return applyEdit(session_.submit(
            nemo::removeKeyframesCommand({nemo::KeyframeRef{state.channel, existing->key.id}}), editOptions()));
    } catch (const std::exception& failure) {
        fail(QString::fromUtf8(failure.what()));
        return false;
    }
}

QString ViewerController::beginNodeParameterEdit(const QString& networkValue, const QVariant& nodeValue,
                                                 const QString& keyValue) {
    if (parameterGestureToken_ != 0) {
        fail(QStringLiteral("a node parameter edit is already in progress"));
        return {};
    }
    QString error;
    const auto target =
        resolveInspectorTarget(session_.document(), networkValue, nodeValue, keyValue.trimmed().toStdString(), error);
    if (!target) {
        fail(error);
        return {};
    }
    if (!target->spec) {
        fail(QStringLiteral("node parameter edit requires a parameter key"));
        return {};
    }
    try {
        const auto frame = static_cast<double>(frame_);
        const auto state = parameterKeyState(session_.document(), target->address, frame);
        const nemo::ParameterEdit edit{target->address,
                                       nemo::animatedParameterValue(session_.document(), target->address, frame)};
        const auto gesture = state.keyed ? session_.beginKeyedParameterGesture(frame, {edit}, editOptions())
                                         : session_.beginParameterGesture({edit}, editOptions());
        if (gesture.token == 0) {
            applyEdit(gesture.result);
            return {};
        }
        parameterGestureAddress_ = target->address;
        parameterGestureToken_ = gesture.token;
        parameterGestureKeyed_ = state.keyed;
        clearError();
        return QString::number(gesture.token);
    } catch (const std::exception& failure) {
        fail(QString::fromUtf8(failure.what()));
        return {};
    }
}

bool ViewerController::updateNodeParameterEdit(const QString& tokenValue, const QVariant& value) {
    bool valid = false;
    const auto token = tokenValue.trimmed().toULongLong(&valid);
    if (!valid || token == 0 || !parameterGestureAddress_ || token != parameterGestureToken_) {
        fail(QStringLiteral("node parameter edit update requires the active gesture token"));
        return false;
    }
    try {
        const auto& graph = session_.document().network(parameterGestureAddress_->network).graph();
        const auto* node = graph.node(parameterGestureAddress_->node);
        if (!node) {
            fail(QStringLiteral("node parameter edit target no longer exists"));
            return false;
        }
        QString conversionError;
        const auto converted = parameterValueFromVariant(graph.catalog(), graph.descriptor(node->type),
                                                         parameterGestureAddress_->key, value, conversionError);
        if (!converted) {
            fail(conversionError);
            return false;
        }
        const std::vector<nemo::ParameterEdit> edits{{*parameterGestureAddress_, *converted}};
        const auto gesture = parameterGestureKeyed_
                                 ? session_.updateKeyedParameterGesture(parameterGestureToken_, edits)
                                 : session_.updateParameterGesture(parameterGestureToken_, edits);
        if (gesture.token == 0) {
            applyEdit(gesture.result);
            return false;
        }
        clearError();
        return true;
    } catch (const std::exception& failure) {
        fail(QString::fromUtf8(failure.what()));
        return false;
    }
}

bool ViewerController::commitNodeParameterEdit(const QString& tokenValue) {
    bool valid = false;
    const auto token = tokenValue.trimmed().toULongLong(&valid);
    if (!valid || token == 0 || !parameterGestureAddress_ || token != parameterGestureToken_) {
        fail(QStringLiteral("node parameter edit commit requires the active gesture token"));
        return false;
    }
    const auto result = session_.commitParameterGesture(parameterGestureToken_, editOptions());
    if (!result.committed && result.error)
        // A conflict leaves the session gesture registered; release it so the
        // next gesture is not blocked by a stale preview.
        static_cast<void>(session_.cancelParameterGesture(parameterGestureToken_));
    parameterGestureAddress_.reset();
    parameterGestureToken_ = 0;
    parameterGestureKeyed_ = false;
    return applyEdit(result);
}

bool ViewerController::cancelNodeParameterEdit(const QString& tokenValue) {
    bool valid = false;
    const auto token = tokenValue.trimmed().toULongLong(&valid);
    if (!valid || token == 0 || !parameterGestureAddress_ || token != parameterGestureToken_) {
        fail(QStringLiteral("node parameter edit cancel requires the active gesture token"));
        return false;
    }
    const auto result = session_.cancelParameterGesture(parameterGestureToken_);
    parameterGestureAddress_.reset();
    parameterGestureToken_ = 0;
    parameterGestureKeyed_ = false;
    if (result.error) {
        fail(QString::fromStdString(result.error->message));
        return false;
    }
    clearError();
    return true;
}

void ViewerController::slipTimelineClip(const QString& source, int delta) {
    const auto key = source.trimmed().toStdString();
    const auto it = session_.document().sources.find(key);
    if (it == session_.document().sources.end()) {
        fail(QStringLiteral("timeline source '%1' is unavailable").arg(source));
        return;
    }
    if (delta == 0)
        return;
    SourceReference replacement = it->second;
    if ((delta > 0 && replacement.frameOffset > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && replacement.frameOffset < std::numeric_limits<std::int64_t>::min() - delta)) {
        fail(QStringLiteral("timeline slip exceeds source timing range"));
        return;
    }
    replacement.frameOffset += delta;
    try {
        // Slip changes the source local-time interval without moving the
        // parent placement. SourceReference is the persistent timing mapping.
        applyEdit(session_.submit(setSourceCommand(key, replacement), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::retimeTimelineClip(const QString& source, int step) {
    const auto key = source.trimmed().toStdString();
    const auto it = session_.document().sources.find(key);
    if (it == session_.document().sources.end()) {
        fail(QStringLiteral("timeline source '%1' is unavailable").arg(source));
        return;
    }
    if (step == 0) {
        fail(QStringLiteral("timeline source frame step must not be zero"));
        return;
    }
    SourceReference replacement = it->second;
    if (replacement.frameStep == step)
        return;
    replacement.frameStep = step;
    try {
        // Retime is source timing: each composition frame advances `step`
        // source frames before downstream graph processing.
        applyEdit(session_.submit(setSourceCommand(key, replacement), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

bool ViewerController::undo() {
    if (!session_.canUndo())
        return false;
    try {
        return applyEdit(session_.undo(editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::redo() {
    if (!session_.canRedo())
        return false;
    try {
        return applyEdit(session_.redo(editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

void ViewerController::cancelRender() {
    invalidateRequest();
    status_ = presentation_ ? QStringLiteral("Cancelled; displayed frame is outdated")
                            : QStringLiteral("Cancelled; no frame is displayed");
    emit statusChanged();
    pollScheduler();
}

void ViewerController::requestRange(int first, int last) {
    if (first > last) {
        fail(QStringLiteral("cache range start must not exceed end"));
        return;
    }
    if (!destination_) {
        fail(QStringLiteral("cache range requires a viewer destination"));
        return;
    }
    if (!lastRequest_) {
        fail(QStringLiteral("cache range requires a current viewer request"));
        return;
    }
    if (frameCount_ > 0) {
        first = std::clamp(first, 0, frameCount_ - 1);
        last = std::clamp(last, 0, frameCount_ - 1);
    } else {
        first = std::max(0, first);
        last = std::max(first, last);
    }
    rangeGeneration_ = ++nextRequestId_;
    rangeError_.clear();
    emit schedulerChanged();
    // Range work is represented as one lazy range per destination, so a panel
    // range must not collide with the global Cache stream.
    if (!runtime_->requestRange(session_.snapshot(), *lastRequest_, first, last, rangeGeneration_, *destination_,
                                session_.colorConfigPath())) {
        status_ = QStringLiteral("Cache range admission rejected; see scheduler drop count");
        emit statusChanged();
        pollScheduler();
        return;
    }
    status_ = QStringLiteral("Caching requested range %1–%2; viewer identity unchanged").arg(first).arg(last);
    emit statusChanged();
    pollScheduler();
}

void ViewerController::openSource(const QString& path) {
    if (path.trimmed().isEmpty()) {
        fail(QStringLiteral("source path is empty"));
        return;
    }
    try {
        SourceReference reference;
        reference.path = QFileInfo(path).absoluteFilePath().toStdString();
        if (const auto previous = session_.document().sources.find("src");
            previous != session_.document().sources.end()) {
            if (previous->second.revision == std::numeric_limits<std::uint64_t>::max())
                throw std::runtime_error("source revision exhausted");
            reference.revision = previous->second.revision + 1;
        }
        buildGraph(reference);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::receive() {
    // Without a destination the controller never submitted work: consuming a
    // result here would steal the destination owner's publication.
    if (!destination_)
        return;
    auto result = runtime_->takeResult(*destination_);
    if (!result)
        return;
    pollScheduler();
    if (auto* failure = std::get_if<ViewerFailure>(&*result)) {
        if (failure->requestId == generation_ || failure->requestId == 0)
            fail(QString::fromStdString(failure->message));
    } else if (auto* probe = std::get_if<SourceProbeResult>(&*result)) {
        if (probe->requestId != generation_)
            return;
        const auto& info = probe->source.info;
        // Publish exactly the reference that was probed: the graph role keeps
        // "src", the media role its routed catalog source. A reference removed
        // before the result arrived is not republished as current media.
        const auto reference = session_.document().sources.find(probeSourceKey_);
        if (reference == session_.document().sources.end())
            return;
        probedSource_ = reference->second;
        pending_ = false;
        sourceSize_ = QSizeF(info.width, info.height);
        pixelAspect_ = info.pixelAspect;
        applyFrameCount(static_cast<int>(std::min<std::int64_t>(info.frameCount, std::numeric_limits<int>::max())));
        // Playback cadence follows the probed media rate; an unknown rate keeps
        // the 24 fps default rather than inventing a timebase.
        const double rate = info.frameRate > 0.0 ? info.frameRate : 24.0;
        if (rate != frameRate_) {
            frameRate_ = rate;
            if (playing_)
                playback_.setInterval(playbackInterval());
            emit frameRateChanged();
        }
        sourceDescription_ =
            QStringLiteral("%1x%2 %3; decode selection: %4")
                .arg(info.width)
                .arg(info.height)
                .arg(QString::fromStdString(info.codecName))
                .arg(probe->source.decision.hardware ? QStringLiteral("Vulkan")
                                                     : QString::fromStdString(probe->source.decision.reason));
        emit sourceChanged();
        emit timelineChanged();
        refreshRequest();
    } else {
        auto frame = std::get<std::shared_ptr<const ViewerResult>>(std::move(*result));
        // The result must belong to the request this panel still owns and carry
        // the snapshot it was rendered from; generation_ already rejects work
        // issued before the authored document changed.
        if (frame->requestId != generation_ || frame->revision != submittedRevision_)
            return;
        presentation_ = std::move(frame);
        pending_ = false;
        outdated_ = false;
        effectiveScale_ = presentation_->request.samplingScale;
        error_.clear();
        status_ = QStringLiteral("Displayed %1x%2, 1:%3, frame %4; %5; %6")
                      .arg(presentation_->frame.width)
                      .arg(presentation_->frame.height)
                      .arg(effectiveScale_)
                      .arg(presentation_->request.localTime)
                      .arg(presentation_->cacheHit ? QStringLiteral("compressed cache") : QStringLiteral("live render"))
                      .arg(sourceDescription_);
        emit effectiveScaleChanged();
        emit frameArrived();
        emit statusChanged();
    }
}

namespace {
// A media-free composition has no probed source to derive pixel dimensions
// from. The interactive viewer still renders the attached target, evaluated
// against this default composition canvas so a graph-only workflow displays
// a result without an Output node.
constexpr int kDefaultCompositionWidth = 1920;
constexpr int kDefaultCompositionHeight = 1080;
}  // namespace

void ViewerController::refreshRequest() {
    // Without a destination this controller is a pure command/metadata facade:
    // it may not probe, submit, or cancel another panel's destination.
    if (!destination_)
        return;
    try {
        // Capture one immutable project state for the whole request. The
        // session remains owner-thread-only; workers receive this snapshot.
        // The media role adds one request-owned node below, so this copy stays
        // writable while the authored document and its history are untouched.
        Document document = session_.snapshot();
        // The revision a rendered result must still match is the authored
        // document's: the request-owned node below must not make this panel's
        // own result look stale.
        const auto revision = document.stateRevision();
        const bool mediaContext = contextRole_ == ContextRole::Media;
        // The graph role keeps the command-line "src" reference; the media role
        // resolves the routed catalog reference.
        const std::string sourceKey = mediaContext ? contextSourceKey_ : std::string{"src"};
        // An authored source node addressing the media key wins. Without one,
        // the routed catalog reference is still viewable: a temporary source
        // node is inserted into this request-owned snapshot only, so the same
        // source-fill plan runs as for an authored node. Its result identity is
        // the reference itself (Reuse canonicalSource: key, path, mapping,
        // revision, interpretation), so it is cache-equivalent to an authored
        // node and follows relink revisions, while no node, used-media mark or
        // history entry is ever persisted for a catalog open.
        NodeId target = renderTargetNode();
        const bool privateMediaSource = mediaContext && target == kInvalidNode && !contextSourceKey_.empty();
        if (privateMediaSource) {
            const auto created = std::make_shared<NodeId>();
            addNodeCommand(document.rootNetworkId(), "source", "source", created, {}).apply(document);
            setParamCommand(document.rootNetworkId(), *created, "source", contextSourceKey_).apply(document);
            target = *created;
        }
        // No render target means an explicit empty viewer, never an Output
        // fallback and never another group's target: the Output node still
        // defines network consumption, but it is not what this panel displays.
        if (target == kInvalidNode) {
            const bool hadPresentation = static_cast<bool>(presentation_);
            presentation_.reset();
            lastRequest_.reset();
            pending_ = false;
            outdated_ = false;
            status_ = unavailableStatus();
            emit statusChanged();
            if (hadPresentation)
                emit frameArrived();
            return;
        }
        const auto source = document.sources.find(sourceKey);
        bool mediaReady = false;
        if (source == document.sources.end()) {
            const bool hadMedia = !sourceSize_.isEmpty() || !probedSource_.path.empty() || pixelAspect_ != 1.0;
            sourceSize_ = {};
            probedSource_ = {};
            pixelAspect_ = 1.0;
            frameCount_ = -1;
            if (hadMedia)
                emit sourceChanged();
        } else {
            const auto& reference = source->second;
            mediaReady = !sourceSize_.isEmpty() && reference.path == probedSource_.path &&
                         reference.revision == probedSource_.revision &&
                         reference.interpretation == probedSource_.interpretation;
            if (!mediaReady) {
                // Probe and interactive render share one scheduler slot, so a
                // probe in flight must be the only queued work. The render
                // resumes from the probe result at the real media size.
                sourceSize_ = {};
                frameCount_ = -1;
                pending_ = true;
                status_ = QStringLiteral("Probing %1").arg(QString::fromStdString(reference.path));
                emit sourceChanged();
                emit statusChanged();
                generation_ = ++nextRequestId_;
                probeSourceKey_ = sourceKey;
                if (!runtime_->probe(document, sourceKey, generation_, *destination_, session_.colorConfigPath()))
                    fail(QStringLiteral("Source probe admission rejected"));
                return;
            }
        }
        if (viewport_.isEmpty())
            return;
        const int width = mediaReady ? static_cast<int>(sourceSize_.width()) : kDefaultCompositionWidth;
        const int height = mediaReady ? static_cast<int>(sourceSize_.height()) : kDefaultCompositionHeight;
        const auto mode = mode_ == "full"      ? ViewerResolution::Full
                          : mode_ == "half"    ? ViewerResolution::Half
                          : mode_ == "quarter" ? ViewerResolution::Quarter
                                               : ViewerResolution::Auto;
        EvaluationRequest request;
        // The media role addresses a source node in the root network; the graph
        // role follows the active Viewer attachment's network.
        request.network =
            !mediaContext && activeViewerNetwork_ != kInvalidNetwork ? activeViewerNetwork_ : document.rootNetworkId();
        request.output = target;
        request.localTime = frame_;
        request.samplingScale =
            policy_.resolve(mode, width, height, pixelAspect_, viewport_.width(), viewport_.height(), zoom_);
        const auto fit = aspectFit(width, height, pixelAspect_, viewport_.width(), viewport_.height());
        const double sx = fit.width / width * zoom_;
        const double sy = fit.height / height * zoom_;
        const double visibleWidth = std::min<double>(width, viewport_.width() / sx);
        const double visibleHeight = std::min<double>(height, viewport_.height() / sy);
        const double centerX = std::clamp(width / 2.0 + pan_.x(), visibleWidth / 2.0, width - visibleWidth / 2.0);
        const double centerY = std::clamp(height / 2.0 + pan_.y(), visibleHeight / 2.0, height - visibleHeight / 2.0);
        const int x = std::max(0, static_cast<int>(std::floor(centerX - visibleWidth / 2)));
        const int y = std::max(0, static_cast<int>(std::floor(centerY - visibleHeight / 2)));
        const int right = std::min(width, static_cast<int>(std::ceil(centerX + visibleWidth / 2)));
        const int bottom = std::min(height, static_cast<int>(std::ceil(centerY + visibleHeight / 2)));
        request.region = {x, y, right - x, bottom - y};
        request.fullWidth = width;
        request.fullHeight = height;
        if (lastRequest_ && *lastRequest_ == request && lastRevision_ == revision)
            return;
        lastRequest_ = request;
        lastRevision_ = revision;
        const auto id = generation_ = ++nextRequestId_;
        // A published result carries the revision of the snapshot it was
        // rendered from, so remember exactly what was submitted.
        submittedRevision_ = document.stateRevision();
        if (!runtime_->submit(document, request, id, *destination_, viewerChannel_, session_.colorConfigPath()))
            pending_ = true;
        outdated_ = static_cast<bool>(presentation_);
        error_.clear();
        status_ = presentation_ ? QStringLiteral("Pending 1:%1; previous frame is outdated").arg(request.samplingScale)
                                : QStringLiteral("Rendering 1:%1").arg(request.samplingScale);
        emit statusChanged();
        pollScheduler();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

QRectF ViewerController::presentedRegion() const {
    if (!presentation_)
        return {};
    const auto& region = presentation_->request.region;
    return QRectF(region.x, region.y, region.width, region.height);
}

QSizeF ViewerController::compositionSize() const {
    if (presentation_)
        return QSizeF(presentation_->request.imageWidth(), presentation_->request.imageHeight());
    if (hasSource())
        return sourceSize_;
    if (viewerTargetNode_ != kInvalidNode)
        return QSizeF(kDefaultCompositionWidth, kDefaultCompositionHeight);
    return {};
}
void ViewerController::setResolutionMode(const QString& mode) {
    if (mode != "auto" && mode != "full" && mode != "half" && mode != "quarter") {
        fail(QStringLiteral("unknown viewer resolution: %1").arg(mode));
        return;
    }
    if (mode_ == mode)
        return;
    mode_ = mode;
    emit resolutionChanged();
    refreshRequest();
}
void ViewerController::setZoom(double value) {
    if (!std::isfinite(value))
        return;
    value = std::clamp(value, 0.05, 32.0);
    if (value == zoom_)
        return;
    zoom_ = value;
    emit zoomChanged();
    refreshRequest();
}
void ViewerController::zoomBy(double factor) {
    if (factor > 0)
        setZoom(zoom_ * factor);
}
void ViewerController::setPan(QPointF value) {
    if (!std::isfinite(value.x()) || !std::isfinite(value.y()) || value == pan_)
        return;
    pan_ = value;
    emit panChanged();
    refreshRequest();
}
void ViewerController::resetView() {
    pan_ = {};
    zoom_ = 1.0;
    emit panChanged();
    emit zoomChanged();
    refreshRequest();
}
void ViewerController::setFrame(int value) {
    value = clampFrame(value);
    if (frame_ == value)
        return;
    frame_ = value;
    emit frameChanged();
    emit timelineChanged();
    refreshRequest();
}
void ViewerController::play() {
    // Starting outside the marked range enters at the in mark, as the prototype
    // does; every tick then advances exactly one frame.
    if (frame_ < inFrame_ || frame_ > outFrame_)
        setFrame(inFrame_);
    if (playing_)
        return;
    playing_ = true;
    playback_.setInterval(playbackInterval());
    playback_.start();
    emit playbackChanged();
}
void ViewerController::pause() {
    if (!playing_)
        return;
    playing_ = false;
    playback_.stop();
    emit playbackChanged();
}
void ViewerController::togglePlay() {
    if (playing_)
        pause();
    else
        play();
}
void ViewerController::stop() {
    pause();
    setFrame(inFrame_);
}
void ViewerController::stepBy(int delta) {
    setFrame(frame_ + delta);
}
void ViewerController::seekToIn() {
    setFrame(inFrame_);
}
void ViewerController::seekToOut() {
    setFrame(outFrame_);
}
void ViewerController::setMarkIn() {
    setMarkInFrame(frame_);
}
void ViewerController::setMarkOut() {
    setMarkOutFrame(frame_);
}
void ViewerController::setMarkInFrame(int frame) {
    marksAuthored_ = true;
    const int mark = std::clamp(clampFrame(frame), 0, std::min(outFrame_, frameDomainEnd()));
    if (mark != inFrame_) {
        inFrame_ = mark;
        emit marksChanged();
    }
    if (frame_ < inFrame_)
        setFrame(inFrame_);
}
void ViewerController::setMarkOutFrame(int frame) {
    marksAuthored_ = true;
    const int mark = std::clamp(clampFrame(frame), inFrame_, frameDomainEnd());
    if (mark != outFrame_) {
        outFrame_ = mark;
        emit marksChanged();
    }
    if (frame_ > outFrame_)
        setFrame(outFrame_);
}
void ViewerController::setChannel(const QString& channel) {
    // Case-insensitive display names; values are the presentation-only gpu
    // isolation applied in the presentation copy.
    const auto name = channel.trimmed().toUpper();
    gpu::ViewerChannel selection = gpu::ViewerChannel::RGBA;
    if (name == QStringLiteral("RGBA"))
        selection = gpu::ViewerChannel::RGBA;
    else if (name == QStringLiteral("R"))
        selection = gpu::ViewerChannel::Red;
    else if (name == QStringLiteral("G"))
        selection = gpu::ViewerChannel::Green;
    else if (name == QStringLiteral("B"))
        selection = gpu::ViewerChannel::Blue;
    else if (name == QStringLiteral("A"))
        selection = gpu::ViewerChannel::Alpha;
    else {
        fail(QStringLiteral("unknown viewer display channel '%1'").arg(channel));
        return;
    }
    if (name == channel_)
        return;
    channel_ = name;
    viewerChannel_ = selection;
    emit displayChanged();
    // Channel isolation is presentation-only: the evaluated request is
    // byte-identical, so the identity early-return in refreshRequest would skip
    // the resubmit that re-runs the presentation copy. Forget it deliberately.
    lastRequest_.reset();
    refreshRequest();
}
void ViewerController::setLayer(const QString& layer) {
    const auto name = layer.trimmed().toLower();
    if (name == layer_)
        return;
    if (name == QStringLiteral("depth")) {
        // The runtime presents the display-referred composite only. Reporting
        // the selection as unavailable is honest; applying it is not.
        status_ = QStringLiteral("Display layer 'depth' is unavailable: the viewer presents the display-referred "
                                 "RGB composite");
        emit statusChanged();
        return;
    }
    fail(QStringLiteral("unknown viewer display layer '%1'").arg(layer));
}
QString ViewerController::timecode() const {
    return timecodeForFrame(frame_);
}
QString ViewerController::timecodeForFrame(int frame) const {
    const double rate = frameRate_ > 0.0 ? frameRate_ : 24.0;
    const int elapsed = clampFrame(frame);
    const int totalSeconds = static_cast<int>(std::floor(static_cast<double>(elapsed) / rate));
    const int frames = static_cast<int>(std::floor(static_cast<double>(elapsed) - totalSeconds * rate));
    const int seconds = totalSeconds % 60;
    const int totalMinutes = totalSeconds / 60;
    const auto pad = [](int value) { return value < 10 ? QStringLiteral("0%1").arg(value) : QString::number(value); };
    return QStringLiteral("%1:%2:%3:%4").arg(pad(totalMinutes / 60), pad(totalMinutes % 60), pad(seconds), pad(frames));
}
int ViewerController::frameForTimecode(const QString& text) const {
    const auto parts = text.trimmed().split(QStringLiteral(":"));
    if (parts.size() != 4)
        return frame_;
    const double rate = frameRate_ > 0.0 ? frameRate_ : 24.0;
    int values[4] = {0, 0, 0, 0};
    for (int index = 0; index < 4; ++index) {
        bool parsed = false;
        values[index] = parts.at(index).toInt(&parsed);
        if (!parsed)
            return frame_;
    }
    if (values[0] < 0 || values[1] < 0 || values[1] > 59 || values[2] < 0 || values[2] > 59 || values[3] < 0 ||
        static_cast<double>(values[3]) >= rate)
        return frame_;
    const auto wholeHours = static_cast<double>((values[0] * 60 + values[1]) * 60 + values[2]);
    const double total = wholeHours * rate + static_cast<double>(values[3]);
    const auto bounded = std::llround(std::min(total, static_cast<double>(std::numeric_limits<int>::max())));
    return clampFrame(static_cast<int>(bounded));
}
int ViewerController::playbackInterval() const {
    const double rate = frameRate_ > 0.0 ? frameRate_ : 24.0;
    return std::max(1, static_cast<int>(std::lround(1000.0 / rate)));
}
void ViewerController::playbackTick() {
    // One frame per tick, one request per displayed frame: playback never
    // queues work ahead of the panel.
    if (frame_ >= outFrame_)
        setFrame(inFrame_);
    else
        setFrame(frame_ + 1);
}
void ViewerController::viewportChanged(QSizeF pixels) {
    if (viewport_ == pixels)
        return;
    viewport_ = pixels;
    refreshRequest();
}
void ViewerController::attachWindow(QQuickWindow* window) {
    // The runtime owns the single window presentation host shared by every
    // panel; this panel only surfaces the attach error.
    const auto error = runtime_->attachToWindow(window);
    if (!error.isEmpty())
        fail(error);
}
void ViewerController::attachViewerItem(ViewerItem* item) {
    if (!items_.contains(item))
        items_.append(item);
    if (!primary_)
        setPrimaryViewerItem(item);
}
void ViewerController::detachViewerItem(ViewerItem* item) {
    items_.removeAll(item);
    if (primary_ == item) {
        primary_.clear();
        for (const auto& remaining : items_) {
            if (remaining && remaining->isVisible()) {
                setPrimaryViewerItem(remaining);
                break;
            }
        }
    }
}
void ViewerController::setPrimaryViewerItem(ViewerItem* item) {
    primary_ = item;
    for (const auto& viewer : items_)
        if (viewer)
            viewer->setPrimary(viewer == item);
}
void ViewerController::fail(QString message) {
    error_ = std::move(message);
    pending_ = false;
    outdated_ = static_cast<bool>(presentation_);
    status_ = presentation_ ? QStringLiteral("Failed; displayed frame is outdated") : QStringLiteral("Failed");
    emit statusChanged();
}
}  // namespace nemo::ui
