#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
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

bool finitePosition(double x, double y) {
    return std::isfinite(x) && std::isfinite(y);
}
std::optional<nemo::NetworkId> networkIdentity(const QString& value) {
    const auto id = graphIdentity(value);
    return id ? std::optional<nemo::NetworkId>{static_cast<nemo::NetworkId>(*id)} : std::nullopt;
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
        result.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(index)},
                                     {QStringLiteral("name"), QString::fromStdString(port.name)},
                                     {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))}});
    }
    return result;
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
    : runtime_(runtime), session_(session), schedulerPoll_(this),
      presentationState_(std::make_unique<WindowPresentationState>()) {
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
    schedulerPoll_.setInterval(200);
    connect(&schedulerPoll_, &QTimer::timeout, this, &ViewerController::pollScheduler);
    schedulerPoll_.start();
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

QString ViewerController::renderState() const {
    if (!error_.isEmpty())
        return QStringLiteral("failed");
    if (pending_)
        return QStringLiteral("pending");
    if (outdated_)
        return QStringLiteral("outdated");
    if (presentation_)
        return QStringLiteral("current");
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

QVariantMap ViewerController::graphSnapshot(const QString& networkValue) const {
    const auto identity = graphIdentity(networkValue);
    if (!identity)
        return QVariantMap{{QStringLiteral("networkId"), networkValue},
                           {QStringLiteral("available"), false},
                           {QStringLiteral("nodes"), QVariantList{}},
                           {QStringLiteral("edges"), QVariantList{}}};
    try {
        const auto network = static_cast<NetworkId>(*identity);
        const auto& graph = session_.document().network(network).graph();
        QVariantList nodes;
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
                nodes.push_back(QVariantMap{
                    {QStringLiteral("id"), QString::number(node->id)},
                    {QStringLiteral("type"), QString::fromStdString(node->type)},
                    {QStringLiteral("name"), QString::fromStdString(node->name)},
                    {QStringLiteral("params"), params},
                    {QStringLiteral("x"), node->layout.x},
                    {QStringLiteral("y"), node->layout.y},
                    {QStringLiteral("inputs"), portSnapshot(graph.inputPorts(node->id))},
                    {QStringLiteral("outputs"), portSnapshot(graph.outputPorts(node->id))},
                    {QStringLiteral("category"), group},
                    {QStringLiteral("group"), group},
                    {QStringLiteral("deletable"), node->id != session_.document().network(network).defaultOutput()},
                });
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
                edges.push_back(QVariantMap{
                    {QStringLiteral("id"), QString::number(edge.id)},
                    {QStringLiteral("fromNode"), QString::number(edge.from.node)},
                    {QStringLiteral("fromPort"), static_cast<int>(edge.from.port)},
                    {QStringLiteral("toNode"), QString::number(edge.to.node)},
                    {QStringLiteral("toPort"), static_cast<int>(edge.to.port)},
                    {QStringLiteral("route"), routeSnapshot(edge.route)},
                });
            }
            if (page.size() < 256)
                break;
            after = page.back().edge.id;
        }
        return QVariantMap{{QStringLiteral("networkId"), QString::number(network)},
                           {QStringLiteral("available"), true},
                           {QStringLiteral("nodes"), nodes},
                           {QStringLiteral("edges"), edges}};
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
                                         {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))}});
        QVariantList outputs;
        for (const auto& port : descriptor.outputs)
            outputs.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(port.name)},
                                          {QStringLiteral("kind"), QString::fromLatin1(portKindName(port.kind))}});
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
    auto counts = runtime_->counts();
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
    runtime_->cancel(generation_);
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
        if (!applyEdit(session_.submit(
                addNodeCommand(*network, trimmedType.toStdString(), trimmedName.toStdString(), id, LayoutPosition{x, y},
                               anchor ? static_cast<NodeId>(*anchor) : kInvalidNode, std::move(shifted)),
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
    const auto from = graphIdentity(fromValue);
    const auto to = graphIdentity(toValue);
    if (!network || !from || !to || fromPort < 0 || toPort < 0) {
        fail(QStringLiteral("graph connection requires a valid network, node IDs, and non-negative ports"));
        return false;
    }
    try {
        const PortRef fromRef{static_cast<NodeId>(*from), static_cast<std::uint32_t>(fromPort)};
        const PortRef toRef{static_cast<NodeId>(*to), static_cast<std::uint32_t>(toPort)};
        const auto& graph = session_.document().network(*network).graph();
        const auto occupied = std::find_if(graph.edges().cbegin(), graph.edges().cend(),
                                           [toRef](const Edge& edge) { return edge.to == toRef; });
        if (occupied != graph.edges().cend() && occupied->from == fromRef) {
            clearError();
            return true;
        }
        const auto command = occupied == graph.edges().cend() ? connectCommand(*network, fromRef, toRef)
                                                              : replaceInputCommand(*network, fromRef, toRef);
        return applyEdit(session_.submit(command, editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}
bool ViewerController::rewireGraphEdge(const QString& networkValue, const QVariant& edgeValue,
                                       const QVariant& fromValue, int fromPort, const QVariant& toValue, int toPort) {
    const auto network = networkIdentity(networkValue);
    const auto edge = graphIdentity(edgeValue);
    const auto from = graphIdentity(fromValue);
    const auto to = graphIdentity(toValue);
    if (!network || !edge || !from || !to || fromPort < 0 || toPort < 0) {
        fail(QStringLiteral("graph rewiring requires a valid network, edge, node IDs, and non-negative ports"));
        return false;
    }
    try {
        const PortRef fromRef{static_cast<NodeId>(*from), static_cast<std::uint32_t>(fromPort)};
        const PortRef toRef{static_cast<NodeId>(*to), static_cast<std::uint32_t>(toPort)};
        const auto& graph = session_.document().network(*network).graph();
        const auto existing = std::find_if(graph.edges().cbegin(), graph.edges().cend(),
                                           [edge](const Edge& candidate) { return candidate.id == *edge; });
        if (existing != graph.edges().cend() && existing->from == fromRef && existing->to == toRef) {
            clearError();
            return true;
        }
        return applyEdit(session_.submit(rewireGraphEdgeCommand(*network, static_cast<EdgeId>(*edge), fromRef, toRef),
                                         editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::disconnectGraphEdge(const QString& networkValue, const QVariant& edgeValue) {
    const auto network = networkIdentity(networkValue);
    const auto edge = graphIdentity(edgeValue);
    if (!network || !edge) {
        fail(QStringLiteral("graph disconnection requires valid network and edge IDs"));
        return false;
    }
    try {
        return applyEdit(session_.submit(disconnectCommand(*network, static_cast<EdgeId>(*edge)), editOptions()));
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
        const auto& graph = session_.document().network(*network).graph();
        std::vector<LayoutEdit> edits;
        edits.reserve(static_cast<std::size_t>(positions.size()));
        std::unordered_set<NodeId> unique;
        bool changed = false;
        for (const auto& value : positions) {
            const auto map = value.toMap();
            const auto id = graphIdentity(map.value(QStringLiteral("id")));
            double x = 0.0;
            double y = 0.0;
            if (!id || !graph.node(static_cast<NodeId>(*id)) || !mapPosition(map, x, y) ||
                !unique.insert(static_cast<NodeId>(*id)).second) {
                fail(QStringLiteral("graph move requires distinct existing IDs and finite x/y positions"));
                return false;
            }
            const auto* node = graph.node(static_cast<NodeId>(*id));
            changed = changed || node->layout != LayoutPosition{x, y};
            edits.push_back(LayoutEdit{static_cast<NodeId>(*id), {x, y}});
        }
        if (!changed) {
            clearError();
            return true;
        }
        return applyEdit(session_.submit(setLayoutsCommand(*network, std::move(edits)), editOptions()));
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
    if (!runtime_->requestRange(session_.snapshot(), *lastRequest_, first, last, rangeGeneration_)) {
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
    auto result = runtime_->takeResult();
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
        probedSource_ = session_.document().sources.at("src");
        pending_ = false;
        sourceSize_ = QSizeF(info.width, info.height);
        pixelAspect_ = info.pixelAspect;
        frameCount_ = static_cast<int>(std::min<std::int64_t>(info.frameCount, std::numeric_limits<int>::max()));
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
        if (frame->requestId != generation_ || frame->revision != session_.document().stateRevision())
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
    try {
        // Capture one immutable project state for the whole request. The
        // session remains owner-thread-only; workers receive this snapshot.
        const Document document = session_.snapshot();
        // No attachment means an explicit empty viewer, never an Output
        // fallback: the Output node still defines network consumption, but it
        // is not what the interactive viewer displays.
        if (viewerTargetNode_ == kInvalidNode) {
            const bool hadPresentation = static_cast<bool>(presentation_);
            presentation_.reset();
            lastRequest_.reset();
            pending_ = false;
            outdated_ = false;
            status_ = QStringLiteral("No viewer target");
            emit statusChanged();
            if (hadPresentation)
                emit frameArrived();
            return;
        }
        const auto source = document.sources.find("src");
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
                if (!runtime_->probe(document, "src", generation_))
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
        request.network = activeViewerNetwork_ != kInvalidNetwork ? activeViewerNetwork_ : document.rootNetworkId();
        request.output = viewerTargetNode_;
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
        const auto revision = document.stateRevision();
        if (lastRequest_ && *lastRequest_ == request && lastRevision_ == revision)
            return;
        lastRequest_ = request;
        lastRevision_ = revision;
        const auto id = generation_ = ++nextRequestId_;
        if (!runtime_->submit(document, request, id))
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
    value = std::max(value, 0);
    if (frameCount_ > 0)
        value = std::min(value, frameCount_ - 1);
    if (frame_ == value)
        return;
    frame_ = value;
    emit frameChanged();
    emit timelineChanged();
    refreshRequest();
}
void ViewerController::viewportChanged(QSizeF pixels) {
    if (viewport_ == pixels)
        return;
    viewport_ = pixels;
    refreshRequest();
}
void ViewerController::attachWindow(QQuickWindow* window) {
    const auto error = runtime_->attachToWindow(window);
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    connect(
        window, &QQuickWindow::beforeRendering, this,
        [this, window] { presentationState_->beginFrame(window, runtime_->presentationDevice()); },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            const auto result = presentationState_->takeNewPresentedFrame();
            if (!result)
                return;
            const double elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - result->requestedAt)
                    .count();
            emit framePresented(static_cast<int>(result->request.localTime), result->frame.width, result->frame.height,
                                result->cacheHit, elapsed);
        },
        Qt::DirectConnection);
    window->show();
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
