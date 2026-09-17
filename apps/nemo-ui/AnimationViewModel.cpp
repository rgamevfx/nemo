#include "AnimationViewModel.hpp"

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/document/Roto.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace nemo::ui {
namespace {
QString componentId(AnimationChannelId channel, std::size_t component) {
    return QString::number(channel) + ':' + QString::number(component);
}
QString keyId(const QString& component, KeyframeId key) {
    return component + '/' + QString::number(key);
}
const Keyframe& findKey(const AnimationChannel& channel, KeyframeId id) {
    const auto found =
        std::find_if(channel.keys.begin(), channel.keys.end(), [id](const Keyframe& key) { return key.id == id; });
    if (found == channel.keys.end())
        throw std::invalid_argument("animation channel " + std::to_string(channel.id) + " has no key " +
                                    std::to_string(id));
    return *found;
}
QVariant componentValue(const ParameterValue& value, std::size_t index) {
    return std::visit(
        [index](const auto& item) -> QVariant {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, double> || std::is_same_v<T, bool>)
                return item;
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return QVariant::fromValue<qlonglong>(item);
            else if constexpr (std::is_same_v<T, std::string>)
                return QString::fromStdString(item);
            else if constexpr (std::is_same_v<T, ChoiceValue>)
                return QString::fromStdString(item.value);
            else
                return item.value[index];
        },
        value);
}
void setComponent(ParameterValue& value, std::size_t index, double number) {
    if (!std::isfinite(number))
        throw std::invalid_argument("animation value must be finite");
    std::visit(
        [index, number](auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, double>)
                item = number;
            else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (std::trunc(number) != number || number < -0x1p63 || number >= 0x1p63)
                    throw std::invalid_argument("animation integer value must be an exact signed 64-bit integer");
                item = static_cast<std::int64_t>(number);
            } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
                                 std::is_same_v<T, ChoiceValue>) {
                throw std::invalid_argument("discrete animation values are edited in Parameters");
            } else
                item.value[index] = number;
        },
        value);
}
QString interpolationName(KeyInterpolation mode) {
    return mode == KeyInterpolation::Bezier ? "bezier" : mode == KeyInterpolation::Hold ? "hold" : "linear";
}
// Presentation name of one Roto channel: the authored element, the point's
// position in its path, and the property's own label. Identity stays in the
// address; this is display text only.
QString rotoChannelLabel(const NodeInstance& node, const ParameterAddress& address, const ParameterSpec& spec) {
    const auto property = QString::fromStdString(spec.label.empty() ? spec.name : spec.label);
    const auto* element =
        node.roto
            ? node.roto->elements.find([&address](const RotoElement& item) { return item.id == address.rotoElement; })
            : nullptr;
    if (!element)
        return property;
    auto elementName = QString::fromStdString(element->name);
    if (elementName.isEmpty())
        elementName = QStringLiteral("Element");
    if (address.rotoPoint == 0)
        return QStringLiteral("%1 · %2").arg(elementName, property);
    std::size_t index = 0;
    for (std::size_t i = 0; i < element->points.size(); ++i) {
        if (element->points[i].id == address.rotoPoint) {
            index = i + 1;
            break;
        }
    }
    return QStringLiteral("%1 · Point %2 · %3").arg(elementName).arg(index).arg(property);
}
}  // namespace

AnimationViewModel::AnimationViewModel(ProjectSession& session, QObject* parent)
    : QObject(parent), session_(session), generation_(session.projectGeneration()),
      subscription_(session.subscribe(this, changed)) {
    refresh();
}
void AnimationViewModel::changed(void* context) noexcept {
    static_cast<AnimationViewModel*>(context)->refresh();
}
void AnimationViewModel::setTargets(const QVariantList& targets) {
    if (targets_ == targets)
        return;
    cancelGesture();
    targets_ = targets;
    emit targetsChanged();
    refresh();
}
void AnimationViewModel::refresh() {
    QVariantList records;
    const auto generation = session_.projectGeneration();
    const bool replaced = observedGeneration_ != 0 && observedGeneration_ != generation;
    observedGeneration_ = generation;
    if (!gestureRevision_)
        generation_ = generation;
    components_.clear();
    const bool wasAvailable = available_;
    available_ = targets_.empty();
    try {
        const auto& document = session_.document();
        struct Selection {
            bool all{};
            std::vector<std::pair<QString, int>> parameters;
        };
        std::map<std::pair<NetworkId, NodeId>, Selection> selected;
        for (const auto& value : targets_) {
            const auto target = value.toMap();
            bool validNetwork = false, validNode = false;
            const auto network = target.value("network").toString().toULongLong(&validNetwork);
            const auto node = target.value("node").toString().toULongLong(&validNode);
            if (!validNetwork || !validNode)
                continue;
            try {
                if (!document.network(network).graph().node(node))
                    continue;
            } catch (const std::exception&) {
                continue;  // A deleted/restored presentation target is not a root fallback.
            }
            available_ = true;
            auto& selection = selected[{network, node}];
            const auto parameter = target.value("parameter").toString();
            if (parameter.isEmpty()) {
                selection.all = true;
                continue;
            }
            int component = -1;
            if (target.contains("component")) {
                bool validComponent = false;
                component = target.value("component").toInt(&validComponent);
                if (!validComponent || component < 0 || component > 3)
                    continue;
            }
            selection.parameters.emplace_back(parameter, component);
        }
        if (!selected.empty()) {
            for (const auto& channel : document.animationChannels()) {
                const auto* occurrence = channel.address.instance == kInvalidNetworkInstance
                                             ? nullptr
                                             : document.instance(channel.address.instance);
                if (channel.address.instance != kInvalidNetworkInstance && !occurrence)
                    continue;
                const auto scope = occurrence ? occurrence->parentNetwork : channel.address.network;
                const auto nodeId = occurrence ? occurrence->node : channel.address.node;
                const auto selection = selected.find({scope, nodeId});
                if (selection == selected.end() || channel.keys.empty())
                    continue;
                const auto& definition = document.network(channel.address.network);
                const auto* node = definition.graph().node(channel.address.node);
                // A Roto channel carries the element (and point) identity in its
                // address, and its schema is the Roto owner's, not the catalog's
                // (the element/point properties are not node parameters). Every
                // ordinary channel keeps the catalog route unchanged.
                const bool rotoScoped = channel.address.rotoElement != 0;
                const auto* spec =
                    rotoScoped ? rotoParameterSpec(channel.address)
                               : (node ? definition.graph().catalog().parameterSpec(node->type, channel.address.key)
                                       : nullptr);
                const auto* displayNode = occurrence ? document.network(scope).graph().node(nodeId) : node;
                if (!spec || !displayNode)
                    continue;
                QString parameterKey = QString::fromStdString(channel.address.key);
                QString label;
                if (rotoScoped) {
                    label = rotoChannelLabel(*node, channel.address, *spec);
                } else if (!occurrence) {
                    label = QString::fromStdString(spec->label.empty() ? spec->name : spec->label);
                } else {
                    for (const auto& exposed : definition.exposedParameters()) {
                        if (exposed.node == channel.address.node && exposed.key == channel.address.key) {
                            label = QString::fromStdString(exposed.name);
                            parameterKey = "exposed:" + QString::number(exposed.id);
                            break;
                        }
                    }
                    if (label.isEmpty())
                        continue;
                }
                const auto count = animation_detail::componentCount(spec->type);
                const QString kind = spec->type == ParameterType::Boolean ? "toggle"
                                     : spec->type == ParameterType::Choice || spec->type == ParameterType::String
                                         ? "choice"
                                     : spec->type == ParameterType::Integer ? "integer"
                                                                            : "number";
                const auto networkId = QString::number(scope);
                const auto displayId = QString::number(nodeId);
                for (std::size_t component = 0; component < std::max<std::size_t>(1, count); ++component) {
                    if (!selection->second.all &&
                        !std::any_of(selection->second.parameters.begin(), selection->second.parameters.end(),
                                     [&](const auto& parameter) {
                                         return parameter.first == parameterKey &&
                                                (parameter.second < 0 ||
                                                 static_cast<std::size_t>(parameter.second) == component);
                                     }))
                        continue;
                    const auto id = componentId(channel.id, component);
                    const QString suffix =
                        count > 1 ? QString(".%1").arg((spec->type == ParameterType::Color ? "RGBA" : "XYZ")[component])
                                  : QString{};
                    QVariantList keys;
                    for (const auto& key : channel.keys) {
                        keys.push_back(
                            QVariantMap{{"id", keyId(id, key.id)},
                                        {"keyframeId", QString::number(key.id)},
                                        {"time", key.time},
                                        {"value", componentValue(key.value, component)},
                                        {"interpolation", interpolationName(key.interpolation)},
                                        {"tangentMode", key.tangentMode == TangentMode::Smooth ? "smooth" : "broken"},
                                        {"inSlope", key.inSlope[component]},
                                        {"outSlope", key.outSlope[component]}});
                    }
                    records.push_back(QVariantMap{
                        {"id", id},
                        {"networkId", networkId},
                        {"nodeId", displayId},
                        {"nodeKey", networkId + "_" + displayId},
                        {"nodeName", QString::fromStdString(displayNode->name)},
                        {"parameter", parameterKey},
                        {"component", static_cast<int>(component)},
                        {"parameterKey", parameterKey + suffix},
                        {"label", label + suffix},
                        {"kind", kind},
                        {"continuous", count != 0},
                        // Roto scope of this channel, empty for an
                        // ordinary node parameter. The identity is
                        // display-visible so a presenter can name the
                        // shape a key belongs to.
                        {"rotoElement",
                         channel.address.rotoElement != 0 ? QString::number(channel.address.rotoElement) : QString{}},
                        {"rotoPoint",
                         channel.address.rotoPoint != 0 ? QString::number(channel.address.rotoPoint) : QString{}},
                        {"keys", keys}});
                    components_.emplace(id, Component{channel.id, component, spec->type});
                }
            }
        }
    } catch (const std::exception&) {
        records.clear();
        components_.clear();
        available_ = false;
    }
    if (records != channels_ || wasAvailable != available_) {
        channels_ = std::move(records);
        emit channelsChanged();
    }
    if (replaced)
        emit projectChanged();
}
bool AnimationViewModel::fail(const QString& message) {
    error_ = message;
    emit errorChanged();
    return false;
}
bool AnimationViewModel::beginGesture() {
    if (!available_)
        return fail("Animation targets are unavailable");
    gestureRevision_ = session_.revision();
    generation_ = session_.projectGeneration();
    return true;
}
void AnimationViewModel::cancelGesture() {
    gestureRevision_.reset();
    generation_ = session_.projectGeneration();
}
bool AnimationViewModel::checkRevision() {
    if (generation_ != session_.projectGeneration() || (gestureRevision_ && *gestureRevision_ != session_.revision()))
        return fail("The project changed during this animation edit. Cancel and retry.");
    return available_ || fail("Animation targets are unavailable");
}
AnimationViewModel::Target AnimationViewModel::resolve(const QString& id) const {
    const auto slash = id.lastIndexOf('/');
    bool valid = false;
    const auto key = id.mid(slash + 1).toULongLong(&valid);
    const auto component = components_.find(id.left(slash));
    if (slash < 0 || !valid || key == 0 || component == components_.end())
        throw std::invalid_argument("animation key '" + id.toStdString() + "' is unavailable in this network");
    return {component->second, key};
}
bool AnimationViewModel::submit(Command command) {
    if (!checkRevision())
        return false;
    const auto result =
        session_.submit(std::move(command), {.expectedRevision = gestureRevision_.value_or(session_.revision())});
    if (result.error)
        return fail(QString::fromStdString(result.error->message));
    error_.clear();
    emit errorChanged();
    return true;
}
bool AnimationViewModel::edit(const QStringList& ids, Operation operation, double time, double value, double inSlope,
                              double outSlope, const QString& mode) {
    if (!checkRevision())
        return false;
    try {
        std::map<KeyframeId, KeyframeEdit> edits;
        std::set<QString> seen;
        for (const auto& id : ids) {
            if (!seen.insert(id).second)
                continue;
            const auto target = resolve(id);
            const auto* channel = session_.document().animationChannel(target.component.channel);
            if (!channel)
                throw std::invalid_argument("animation channel was removed");
            const auto& original = findKey(*channel, target.key);
            auto found = edits.find(target.key);
            const bool inserted = found == edits.end();
            if (inserted)
                found = edits.emplace(target.key, KeyframeEdit{channel->address, original}).first;
            auto& key = found->second.key;
            const auto component = target.component.index;
            const bool numeric = animation_detail::componentCount(target.component.type) != 0;
            switch (operation) {
            case Operation::Move:
                if (inserted)
                    key.time += time;
                if (value != 0)
                    setComponent(key.value, component, componentValue(original.value, component).toDouble() + value);
                break;
            case Operation::Exact:
                key.time = time;
                setComponent(key.value, component, value);
                key.inSlope[component] = inSlope;
                key.outSlope[component] = key.tangentMode == TangentMode::Smooth ? inSlope : outSlope;
                break;
            case Operation::Interpolation:
                if (mode != "hold" && mode != "linear" && mode != "bezier")
                    throw std::invalid_argument("unknown animation interpolation");
                if (!numeric && mode != "hold")
                    throw std::invalid_argument("discrete animation channels require Hold interpolation");
                key.interpolation = mode == "hold"     ? KeyInterpolation::Hold
                                    : mode == "bezier" ? KeyInterpolation::Bezier
                                                       : KeyInterpolation::Linear;
                break;
            case Operation::TangentMode:
                if (!numeric || (mode != "smooth" && mode != "broken"))
                    throw std::invalid_argument("tangent modes require a continuous numeric channel");
                key.tangentMode = mode == "smooth" ? TangentMode::Smooth : TangentMode::Broken;
                if (key.tangentMode == TangentMode::Smooth)
                    key.outSlope = key.inSlope;
                break;
            case Operation::InSlope:
            case Operation::OutSlope:
                if (!numeric)
                    throw std::invalid_argument("tangent editing requires a continuous numeric channel");
                if (operation == Operation::InSlope || key.tangentMode == TangentMode::Smooth)
                    key.inSlope[component] = value;
                if (operation == Operation::OutSlope || key.tangentMode == TangentMode::Smooth)
                    key.outSlope[component] = value;
                break;
            }
        }
        std::vector<KeyframeEdit> changedKeys;
        for (auto& [id, edit] : edits) {
            const auto* channel = session_.document().animationChannel(edit.address);
            if (edit.key != findKey(*channel, id))
                changedKeys.push_back(std::move(edit));
        }
        if (changedKeys.empty())
            return true;
        return submit(setKeyframesCommand(std::move(changedKeys)));
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}
bool AnimationViewModel::moveKeys(const QStringList& ids, double dt, double dv) {
    return edit(ids, Operation::Move, dt, dv);
}
bool AnimationViewModel::editKey(const QString& id, double time, double value, double inSlope, double outSlope) {
    return edit({id}, Operation::Exact, time, value, inSlope, outSlope);
}
bool AnimationViewModel::setInterpolation(const QStringList& ids, const QString& mode) {
    return edit(ids, Operation::Interpolation, 0, 0, 0, 0, mode);
}
bool AnimationViewModel::setTangentMode(const QStringList& ids, const QString& mode) {
    return edit(ids, Operation::TangentMode, 0, 0, 0, 0, mode);
}
bool AnimationViewModel::setTangent(const QString& id, const QString& side, double slope) {
    if (side != "in" && side != "out")
        return fail("Unknown tangent side '" + side + "'");
    return edit({id}, side == "in" ? Operation::InSlope : Operation::OutSlope, 0, slope);
}
QString AnimationViewModel::insertKey(const QString& id, double time) {
    if (!checkRevision())
        return {};
    try {
        const auto found = components_.find(id);
        if (found == components_.end())
            throw std::invalid_argument("animation curve '" + id.toStdString() + "' is unavailable");
        const auto channelId = found->second.channel;
        const auto* channel = session_.document().animationChannel(channelId);
        if (!channel)
            throw std::invalid_argument("animation channel was removed");
        for (const auto& key : channel->keys)
            if (key.time == time)
                return keyId(id, key.id);
        if (!submit(insertKeyframeCommand(channel->address, time)))
            return {};
        for (const auto& key : session_.document().animationChannel(channelId)->keys)
            if (key.time == time)
                return keyId(id, key.id);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
    return {};
}
bool AnimationViewModel::removeKeys(const QStringList& ids) {
    if (!checkRevision())
        return false;
    try {
        std::set<KeyframeId> seen;
        std::vector<KeyframeRef> refs;
        for (const auto& id : ids) {
            const auto target = resolve(id);
            if (seen.insert(target.key).second)
                refs.push_back({target.component.channel, target.key});
        }
        return refs.empty() || submit(removeKeyframesCommand(std::move(refs)));
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}
}  // namespace nemo::ui
