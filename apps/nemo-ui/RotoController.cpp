#include "RotoController.hpp"

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/RotoCommands.hpp"
#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace nemo::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] QString identityText(std::uint64_t id) {
    return QString::number(static_cast<qulonglong>(id));
}

// The core publishes the roto property keys as constants so an authoring owner
// cannot drift on their spelling; every key this adapter accepts is compared
// through them rather than through a repeated literal.
[[nodiscard]] bool keyIs(const QString& key, std::string_view name) {
    return key == QLatin1String(name.data(), static_cast<qsizetype>(name.size()));
}

[[nodiscard]] std::optional<std::uint64_t> identity(const QString& text) {
    bool ok = false;
    const auto value = text.trimmed().toULongLong(&ok);
    if (!ok || value == 0)
        return std::nullopt;
    return value;
}

[[nodiscard]] QString kindName(RotoKind kind) {
    switch (kind) {
    case RotoKind::Bezier:
        return QStringLiteral("bezier");
    case RotoKind::BSpline:
        return QStringLiteral("bspline");
    case RotoKind::Group:
        return QStringLiteral("group");
    }
    return QStringLiteral("bezier");
}

[[nodiscard]] std::optional<RotoKind> kindFromName(const QString& name) {
    if (name == QLatin1String("bezier"))
        return RotoKind::Bezier;
    if (name == QLatin1String("bspline"))
        return RotoKind::BSpline;
    if (name == QLatin1String("group"))
        return RotoKind::Group;
    return std::nullopt;
}

[[nodiscard]] QString blendName(RotoBlend blend) {
    switch (blend) {
    case RotoBlend::Combine:
        return QStringLiteral("combine");
    case RotoBlend::Intersect:
        return QStringLiteral("intersect");
    case RotoBlend::Subtract:
        return QStringLiteral("subtract");
    }
    return QStringLiteral("combine");
}

[[nodiscard]] std::optional<RotoBlend> blendFromName(const QString& name) {
    if (name == QLatin1String("combine"))
        return RotoBlend::Combine;
    if (name == QLatin1String("intersect"))
        return RotoBlend::Intersect;
    if (name == QLatin1String("subtract"))
        return RotoBlend::Subtract;
    return std::nullopt;
}

[[nodiscard]] QString profileName(RotoFeatherProfile profile) {
    return profile == RotoFeatherProfile::Smooth ? QStringLiteral("smooth") : QStringLiteral("linear");
}

[[nodiscard]] std::optional<RotoFeatherProfile> profileFromName(const QString& name) {
    if (name == QLatin1String("linear"))
        return RotoFeatherProfile::Linear;
    if (name == QLatin1String("smooth"))
        return RotoFeatherProfile::Smooth;
    return std::nullopt;
}

[[nodiscard]] QVariantList vectorVariant(const Vector2Value& value) {
    return QVariantList{static_cast<double>(value.value[0]), static_cast<double>(value.value[1])};
}

[[nodiscard]] QVariant valueVariant(const ParameterValue& value) {
    return std::visit(
        [](const auto& item) -> QVariant {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                return item;
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return QVariant::fromValue<qlonglong>(static_cast<qlonglong>(item));
            else if constexpr (std::is_same_v<T, double>)
                return item;
            else if constexpr (std::is_same_v<T, std::string>)
                return QString::fromStdString(item);
            else if constexpr (std::is_same_v<T, ChoiceValue>)
                return QString::fromStdString(item.value);
            else if constexpr (std::is_same_v<T, Vector2Value>)
                return vectorVariant(item);
            else if constexpr (std::is_same_v<T, Vector3Value>)
                return QVariantList{static_cast<double>(item.value[0]), static_cast<double>(item.value[1]),
                                    static_cast<double>(item.value[2])};
            else
                return QVariantList{static_cast<double>(item.value[0]), static_cast<double>(item.value[1]),
                                    static_cast<double>(item.value[2]), static_cast<double>(item.value[3])};
        },
        value);
}

[[nodiscard]] QString kindOfSpec(ParameterType type) {
    switch (type) {
    case ParameterType::Boolean:
        return QStringLiteral("boolean");
    case ParameterType::Integer:
        return QStringLiteral("integer");
    case ParameterType::Choice:
        return QStringLiteral("choice");
    case ParameterType::String:
        return QStringLiteral("string");
    case ParameterType::Vector2:
        return QStringLiteral("vector2");
    case ParameterType::Vector3:
        return QStringLiteral("vector3");
    case ParameterType::Color:
        return QStringLiteral("color");
    case ParameterType::Float:
        break;
    }
    return QStringLiteral("float");
}

[[nodiscard]] std::optional<double> numberFrom(const QVariant& value) {
    switch (value.metaType().id()) {
    case QMetaType::Bool:
        return value.toBool() ? 1.0 : 0.0;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return value.toDouble();
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Vector2Value> vectorFrom(const QVariant& value) {
    const auto list = value.toList();
    if (list.size() == 2) {
        const auto x = numberFrom(list[0]);
        const auto y = numberFrom(list[1]);
        if (x && y)
            return Vector2Value{{static_cast<float>(*x), static_cast<float>(*y)}};
        return std::nullopt;
    }
    if (value.canConvert<QPointF>()) {
        const auto point = value.toPointF();
        return Vector2Value{{static_cast<float>(point.x()), static_cast<float>(point.y())}};
    }
    return std::nullopt;
}

// The two tangential neighbours of one point of a closed path, wrapping.
[[nodiscard]] std::pair<const RotoPoint&, const RotoPoint&> neighbours(const RotoElement& element, std::size_t index) {
    const auto count = element.points.size();
    const std::size_t previous = (index + count - 1) % count;
    const std::size_t next = (index + 1) % count;
    return {element.points[previous], element.points[next]};
}

// One static property key as the presenters spell it.
[[nodiscard]] QString keyText(std::string_view name) {
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

// The point channels that state one contour's GEOMETRY: a Bezier is its
// position, its two tangents and its feather; a B-spline is its control points,
// its tension and its feather. Neither kind ever authors the other kind's
// unused pair, so a keyed row states exactly the shape it draws.
[[nodiscard]] std::span<const std::string_view> geometryKeys(RotoKind kind) {
    static constexpr std::string_view spline[] = {kRotoParamPosition, kRotoParamTension, kRotoParamFeather};
    static constexpr std::string_view bezier[] = {kRotoParamPosition, kRotoParamInTangent, kRotoParamOutTangent,
                                                  kRotoParamFeather};
    return kind == RotoKind::BSpline ? std::span<const std::string_view>(spline)
                                     : std::span<const std::string_view>(bezier);
}

// A node type that carries the pixel grid of its input through unchanged, so a
// Roto's shapes are still stated in the target's coordinate space. Every other
// type - transform, reformat, crop, a nested occurrence, a type this build does
// not model - states a mapping this authoring owner cannot prove, so it is
// reported instead of guessed.
[[nodiscard]] bool gridPreservingType(std::string_view type) {
    // viewer/output are terminals: they display their input's grid unchanged.
    return type == "grade" || type == "blur" || type == "merge" || type == "shuffle" || type == "roto" ||
           type == "viewer" || type == "output";
}

// One node's answer to "can the viewed target be reached from here, and does
// every route keep the pixel grid?".
struct OverlayProbe {
    // Some route from the node to the target exists at all; false means the node
    // is unrelated to the target and no reason is owed.
    bool reaches{false};
    // A route exists whose every node carries the grid through unchanged.
    bool safeRoute{false};
    // A route exists that passes through a node whose mapping is not proven.
    bool unsafeRoute{false};
    // Display names of the nodes whose mapping is not proven and that lie on
    // some route to the target.
    std::set<std::string> blockers;
};

// ONE forward traversal of the graph answers both overlay questions: whether a
// route from `origin` to `target` exists, whether any of them passes through a
// node whose mapping is not proven, and which nodes those are. Following only
// the origin's own fan-out is what keeps a composite's other input out of the
// answer - a Merge states one coordinate space for A and B alike - and the memo
// makes each node expand once. Two routes that disagree (one proven, one not)
// are reported as an unsafe route as well, so a caller never picks the
// convenient path out of an ambiguous graph.
[[nodiscard]] OverlayProbe probeOverlay(const Graph& graph, NodeId origin, NodeId target) {
    std::map<NodeId, std::vector<NodeId>> successors;
    for (std::size_t index = 0; index < graph.edges().size(); ++index)
        successors[graph.edges()[index].from.node].push_back(graph.edges()[index].to.node);
    std::map<NodeId, OverlayProbe> probes;
    std::function<OverlayProbe(NodeId)> probe = [&](NodeId id) -> OverlayProbe {
        const auto cached = probes.find(id);
        if (cached != probes.end())
            return cached->second;
        // Provisional entry, so a malformed cyclic graph can never recurse
        // forever; the real answer replaces it below.
        probes.emplace(id, OverlayProbe{});
        const auto* instance = graph.node(id);
        const bool proven = instance && instance->definition == kInvalidNetwork && gridPreservingType(instance->type);
        OverlayProbe result;
        if (id == target) {
            result.reaches = true;
            result.safeRoute = proven;
            result.unsafeRoute = !proven;
        } else {
            const auto found = successors.find(id);
            if (found != successors.end())
                for (const auto next : found->second) {
                    const auto child = probe(next);
                    result.reaches = result.reaches || child.reaches;
                    result.safeRoute = result.safeRoute || child.safeRoute;
                    result.unsafeRoute = result.unsafeRoute || child.unsafeRoute;
                }
            if (!proven) {
                // This node states a mapping this owner cannot prove, so every
                // route through it is unsafe and none of them is proven.
                result.safeRoute = false;
                result.unsafeRoute = result.reaches;
            }
        }
        probes[id] = result;
        return result;
    };
    auto root = probe(origin);
    if (!root.reaches)
        return root;
    // Every node without a proven mapping that still lies on some route to the
    // target, so a refusal names what it cannot see through. Only nodes that
    // reach the target are visited, so a side branch that never arrives is not
    // blamed for a mapping the viewer never goes through.
    std::set<NodeId> visited;
    std::function<void(NodeId)> collect = [&](NodeId id) {
        if (!visited.insert(id).second || !graph.node(id))
            return;
        const auto seen = probes.find(id);
        if (seen == probes.end() || !seen->second.reaches)
            return;
        const auto* instance = graph.node(id);
        if (instance->definition != kInvalidNetwork || !gridPreservingType(instance->type))
            root.blockers.insert(instance->type);
        const auto found = successors.find(id);
        if (found == successors.end())
            return;
        for (const auto next : found->second)
            collect(next);
    };
    collect(origin);
    return root;
}

}  // namespace

RotoController::RotoController(ProjectSession& session, NetworkId network, NodeId node, QObject* parent)
    : QObject(parent), session_(session), network_(network), node_(node),
      subscription_(session.subscribe(this, &RotoController::sessionChanged)) {
    refresh();
}

void RotoController::sessionChanged(void* context) noexcept {
    auto* controller = static_cast<RotoController*>(context);
    try {
        controller->refresh();
    } catch (const std::exception& error) {
        controller->fail(QString::fromUtf8(error.what()));
    }
}

QString RotoController::networkId() const {
    return network_ == kInvalidNetwork ? QString{} : identityText(network_);
}

QString RotoController::nodeId() const {
    return node_ == kInvalidNode ? QString{} : identityText(node_);
}

QString RotoController::selectedElement() const {
    return selectedElement_ == 0 ? QString{} : identityText(selectedElement_);
}

QStringList RotoController::selectedPoints() const {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(selectedPoints_.size()));
    for (const auto point : selectedPoints_)
        ids.push_back(identityText(point));
    return ids;
}

QVariantList RotoController::draftPoints() const {
    QVariantList points;
    points.reserve(static_cast<qsizetype>(draft_.size()) + 1);
    for (const auto& point : draft_)
        points.push_back(vectorVariant(point));
    if (draftLive_)
        points.push_back(vectorVariant(*draftLive_));
    return points;
}

void RotoController::setFrame(int frame) {
    if (frame_ == frame)
        return;
    // A frame change invalidates a live preview: the frozen frame the gesture
    // was seeded at no longer matches what the panel evaluates, so the gesture
    // is discarded instead of committing through it.
    if (gestureToken_ != 0)
        static_cast<void>(cancelGesture(QString::number(gestureToken_)));
    cancelDraft();
    frame_ = frame;
    refresh();
    emit frameChanged();
}

void RotoController::attachView(QObject* owner) {
    if (!owner)
        return;
    for (auto& view : views_)
        if (view.owner == owner)
            return;
    views_.push_back({QPointer<QObject>(owner), false});
    // A presenter that dies without detaching still releases its view, so the
    // shared selection is never kept alive by a closed panel.
    connect(owner, &QObject::destroyed, this, [this, owner] { detachView(owner); });
}

bool RotoController::viewerAttached() const {
    return std::any_of(views_.begin(), views_.end(), [](const AttachedView& view) { return view.viewer; });
}

void RotoController::setViewerFrame(QObject* owner, int frame) {
    const auto view = std::find_if(views_.begin(), views_.end(),
                                   [owner](const AttachedView& attached) { return attached.owner == owner; });
    if (!owner || view == views_.end())
        return;
    const bool hadViewer = viewerAttached();
    view->viewer = true;
    setFrame(frame);
    if (!hadViewer)
        emit viewerAttachedChanged();
}

void RotoController::detachView(QObject* owner) {
    const bool hadViewer = viewerAttached();
    std::erase_if(views_, [owner](const AttachedView& view) { return !view.owner || view.owner == owner; });
    if (hadViewer != viewerAttached())
        emit viewerAttachedChanged();
    if (!views_.empty())
        return;
    // The last presenter is gone: this is transient state, never document
    // state, so it is dropped with its views — including a live session
    // preview, which is discarded rather than left owning the one gesture.
    cancelHistoryGesture();
    if (selectedElement_ != 0 || !selectedPoints_.empty() || !selectedElements_.empty()) {
        selectedElement_ = 0;
        selectedPoints_.clear();
        selectedElements_.clear();
        emit selectionChanged();
    }
}

bool RotoController::fail(const QString& message) {
    if (error_ == message)
        return false;
    error_ = message;
    emit errorChanged();
    return false;
}

void RotoController::clearError() {
    if (error_.isEmpty())
        return;
    error_.clear();
    emit errorChanged();
}

const RotoData* RotoController::evaluatedData() const {
    return evaluated_ ? &*evaluated_ : nullptr;
}

const RotoElement* RotoController::findElement(const RotoData& data, RotoElementId id) const {
    return rotoElement(data, id);
}

const RotoElement* RotoController::findPointElement(const RotoData& data, RotoPointId id) const {
    for (std::size_t i = 0; i < data.elements.size(); ++i)
        if (rotoPoint(data.elements[i], id))
            return &data.elements[i];
    return nullptr;
}

bool RotoController::elementLocked(RotoElementId id) const {
    const auto* data = authoredData();
    if (!data)
        return false;
    const auto* element = findElement(*data, id);
    return element && element->locked;
}

std::optional<ParameterAddress> RotoController::addressFor(const Scope& scope, const QString& key) const {
    if (!available_ || key.isEmpty())
        return std::nullopt;
    ParameterAddress address;
    address.network = network_;
    address.node = node_;
    address.key = key.toStdString();
    // Roto is node-local: there is no per-occurrence override, so the address
    // never names an instance.
    address.instance = kInvalidNetworkInstance;
    address.rotoElement = scope.element;
    address.rotoPoint = scope.point;
    if (!rotoParameterSpec(address))
        return std::nullopt;
    return address;
}

std::optional<ParameterValue> RotoController::currentValue(const Scope& scope, const QString& key) const {
    const RotoData* data = nullptr;
    if (const auto* evaluated = evaluatedData())
        if (findElement(*evaluated, scope.element))
            data = evaluated;
    if (!data)
        data = authoredData();
    if (!data)
        return std::nullopt;
    const auto* element = findElement(*data, scope.element);
    if (!element)
        return std::nullopt;
    if (scope.point != 0) {
        const auto* point = rotoPoint(*element, scope.point);
        if (!point)
            return std::nullopt;
        if (keyIs(key, kRotoParamPosition))
            return ParameterValue{point->position};
        if (keyIs(key, kRotoParamInTangent))
            return ParameterValue{point->inTangent};
        if (keyIs(key, kRotoParamOutTangent))
            return ParameterValue{point->outTangent};
        if (keyIs(key, kRotoParamFeather))
            return ParameterValue{point->feather};
        if (keyIs(key, kRotoParamTension))
            return ParameterValue{point->tension};
        return std::nullopt;
    }
    if (keyIs(key, kRotoParamTranslation))
        return ParameterValue{element->translation};
    if (keyIs(key, kRotoParamScale))
        return ParameterValue{element->scale};
    if (keyIs(key, kRotoParamPivot))
        return ParameterValue{element->pivot};
    if (keyIs(key, kRotoParamRotation))
        return ParameterValue{element->rotation};
    if (keyIs(key, kRotoParamOpacity))
        return ParameterValue{element->opacity};
    if (keyIs(key, kRotoParamFeather))
        return ParameterValue{element->feather};
    if (keyIs(key, kRotoParamFeatherFalloff))
        return ParameterValue{element->featherFalloff};
    if (keyIs(key, kRotoParamVisible))
        return ParameterValue{element->visible};
    if (keyIs(key, kRotoParamInverted))
        return ParameterValue{element->inverted};
    if (keyIs(key, kRotoParamFeatherEnabled))
        return ParameterValue{element->featherEnabled};
    if (keyIs(key, kRotoParamFeatherProfile))
        return ParameterValue{ChoiceValue{profileName(element->featherProfile).toStdString()}};
    return std::nullopt;
}

std::optional<ParameterValue> RotoController::convert(const ParameterSpec& spec, const QVariant& value,
                                                      QString& error) {
    switch (spec.type) {
    case ParameterType::Boolean:
        if (value.metaType().id() != QMetaType::Bool) {
            error = QStringLiteral("'%1' requires a boolean value").arg(QString::fromStdString(spec.name));
            return std::nullopt;
        }
        return ParameterValue{value.toBool()};
    case ParameterType::Float: {
        const auto number = numberFrom(value);
        if (!number || !std::isfinite(*number)) {
            error = QStringLiteral("'%1' requires a numeric value").arg(QString::fromStdString(spec.name));
            return std::nullopt;
        }
        return ParameterValue{*number};
    }
    case ParameterType::Integer: {
        const auto number = numberFrom(value);
        if (!number || !std::isfinite(*number) || std::trunc(*number) != *number) {
            error = QStringLiteral("'%1' requires an integer value").arg(QString::fromStdString(spec.name));
            return std::nullopt;
        }
        return ParameterValue{static_cast<std::int64_t>(*number)};
    }
    case ParameterType::Vector2: {
        const auto vector = vectorFrom(value);
        if (!vector) {
            error = QStringLiteral("'%1' requires a two-number value").arg(QString::fromStdString(spec.name));
            return std::nullopt;
        }
        return ParameterValue{*vector};
    }
    case ParameterType::Choice: {
        const auto name = value.toString().trimmed().toStdString();
        const auto found = std::find(spec.choices.begin(), spec.choices.end(), name);
        if (found == spec.choices.end()) {
            error = QStringLiteral("'%1' does not accept '%2'")
                        .arg(QString::fromStdString(spec.name), QString::fromStdString(name));
            return std::nullopt;
        }
        return ParameterValue{ChoiceValue{name}};
    }
    case ParameterType::String:
        return ParameterValue{value.toString().toStdString()};
    case ParameterType::Vector3:
    case ParameterType::Color:
        break;
    }
    error = QStringLiteral("'%1' does not accept that value").arg(QString::fromStdString(spec.name));
    return std::nullopt;
}

QVariantMap RotoController::parameterState(const QString& elementId, const QString& pointId, const QString& key) const {
    const auto element = identity(elementId);
    if (!available_ || !element)
        return QVariantMap{{QStringLiteral("available"), false}};
    Scope scope;
    scope.element = *element;
    if (!pointId.isEmpty()) {
        const auto point = identity(pointId);
        if (!point)
            return QVariantMap{{QStringLiteral("available"), false}};
        scope.point = *point;
    }
    const auto address = addressFor(scope, key);
    const auto* spec = address ? rotoParameterSpec(*address) : nullptr;
    if (!spec)
        return QVariantMap{{QStringLiteral("available"), false}};
    const auto value = currentValue(scope, key);
    if (!value)
        return QVariantMap{{QStringLiteral("available"), false}};
    QVariantMap state{{QStringLiteral("available"), true},
                      {QStringLiteral("kind"), kindOfSpec(spec->type)},
                      {QStringLiteral("value"), valueVariant(*value)},
                      {QStringLiteral("valueText"), QString::fromStdString(parameterValueText(*value))},
                      {QStringLiteral("label"), QString::fromStdString(spec->label.empty() ? spec->name : spec->label)},
                      {QStringLiteral("keyStatus"), QStringLiteral("none")},
                      {QStringLiteral("animated"), false},
                      {QStringLiteral("keyed"), false},
                      {QStringLiteral("hasMinimum"), false},
                      {QStringLiteral("hasMaximum"), false},
                      {QStringLiteral("step"), 0.0}};
    if (spec->minimum)
        state.insert(QStringLiteral("minimum"), *spec->minimum);
    if (spec->maximum)
        state.insert(QStringLiteral("maximum"), *spec->maximum);
    state.insert(QStringLiteral("hasMinimum"), spec->minimum.has_value());
    state.insert(QStringLiteral("hasMaximum"), spec->maximum.has_value());
    state.insert(QStringLiteral("step"), spec->step.value_or(0.0));
    QVariantList choices;
    for (const auto& choice : spec->choices)
        choices.push_back(QString::fromStdString(choice));
    state.insert(QStringLiteral("choices"), choices);
    if (const auto* channel = session_.document().animationChannel(*address)) {
        state.insert(QStringLiteral("animated"), !channel->keys.empty());
        const auto frame = static_cast<double>(frame_);
        const auto keyed = std::any_of(channel->keys.begin(), channel->keys.end(),
                                       [frame](const Keyframe& key) { return key.time == frame; });
        state.insert(QStringLiteral("keyed"), keyed);
        state.insert(QStringLiteral("keyStatus"), channel->keys.empty() ? QStringLiteral("none")
                                                  : keyed               ? QStringLiteral("key")
                                                                        : QStringLiteral("animated"));
    }
    return state;
}

bool RotoController::setTool(const QString& tool) {
    const auto name = tool.trimmed().toLower();
    if (name != QLatin1String("select") && name != QLatin1String("bezier") && name != QLatin1String("bspline") &&
        name != QLatin1String("rectangle") && name != QLatin1String("ellipse"))
        return fail(QStringLiteral("'%1' is not a Roto tool").arg(tool));
    if (tool_ == name)
        return true;
    cancelDraft();
    tool_ = name;
    clearError();
    emit toolChanged();
    return true;
}

bool RotoController::selectElement(const QString& elementId, bool additive) {
    const auto element = identity(elementId);
    if (!available_ || !element)
        return false;
    const auto* data = authoredData();
    if (!data || !findElement(*data, *element))
        return false;
    // Selecting requires clearing the draft's preview: the selected element is
    // a different authoring context than the shape being drawn.
    cancelDraft();
    if (!additive) {
        if (selectedElements_.size() == 1 && selectedElements_.front() == *element && selectedPoints_.empty()) {
            selectedElement_ = *element;
            return true;
        }
        selectedElements_ = {*element};
        selectedElement_ = *element;
        selectedPoints_.clear();
    } else {
        const auto at = std::find(selectedElements_.begin(), selectedElements_.end(), *element);
        if (at == selectedElements_.end()) {
            selectedElements_.push_back(*element);
            selectedElement_ = *element;
        } else {
            // Additive selection toggles: the same shape under Shift leaves the
            // selection, and the primary falls back to the last remaining one.
            selectedElements_.erase(at);
            selectedElement_ = selectedElements_.empty() ? kInvalidRotoElement : selectedElements_.back();
        }
        selectedPoints_.clear();
    }
    emit selectionChanged();
    refresh();
    return true;
}

bool RotoController::selectPoint(const QString& pointId, bool additive) {
    const auto point = identity(pointId);
    if (!available_ || !point)
        return false;
    const auto* data = authoredData();
    if (!data)
        return false;
    const auto* owner = findPointElement(*data, *point);
    if (!owner)
        return false;
    const auto at = std::find(selectedPoints_.begin(), selectedPoints_.end(), *point);
    if (!additive) {
        selectedPoints_ = {*point};
        selectedElements_ = {owner->id};
        selectedElement_ = owner->id;
    } else if (at != selectedPoints_.end()) {
        selectedPoints_.erase(at);
    } else {
        selectedPoints_.push_back(*point);
        // A point's own shape is part of the selection, so the primary element
        // addresses it and its handles are the ones a presenter draws.
        selectElementSilently(owner->id);
    }
    emit selectionChanged();
    // The published records carry the per-point selection flag, so the
    // presenters that draw handles from them are re-stated.
    refresh();
    return true;
}

void RotoController::selectElementSilently(RotoElementId id) {
    if (id == 0)
        return;
    if (std::find(selectedElements_.begin(), selectedElements_.end(), id) == selectedElements_.end())
        selectedElements_.push_back(id);
    selectedElement_ = id;
}

bool RotoController::elementSelected(const QString& elementId) const {
    const auto element = identity(elementId);
    if (!element)
        return false;
    return std::find(selectedElements_.begin(), selectedElements_.end(), *element) != selectedElements_.end();
}

QStringList RotoController::selectedElements() const {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(selectedElements_.size()));
    for (const auto element : selectedElements_)
        ids.push_back(identityText(element));
    return ids;
}

bool RotoController::setPointSelection(const QStringList& pointIds, bool additive) {
    if (!available_ || !authoredData())
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_);
    std::vector<RotoPointId> points;
    std::vector<RotoElementId> owners;
    points.reserve(static_cast<std::size_t>(pointIds.size()));
    owners.reserve(static_cast<std::size_t>(pointIds.size()));
    for (const auto& text : pointIds) {
        const auto parsed = identity(text);
        if (!parsed)
            return fail(QStringLiteral("the point identity is invalid"));
        const auto* owner = findPointElement(*authoredData(), *parsed);
        if (!owner)
            return fail(QStringLiteral("the point no longer exists"));
        points.push_back(*parsed);
        owners.push_back(owner->id);
    }
    if (!additive) {
        selectedPoints_.clear();
        selectedElements_.clear();
        selectedElement_ = kInvalidRotoElement;
    }
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (std::find(selectedPoints_.begin(), selectedPoints_.end(), points[index]) == selectedPoints_.end())
            selectedPoints_.push_back(points[index]);
        // Later shapes win the primary: the last identity in the list is what
        // the caller stated last, exactly like a click sequence.
        selectElementSilently(owners[index]);
    }
    clearError();
    emit selectionChanged();
    refresh();
    return true;
}

bool RotoController::selectAllPoints() {
    const auto* data = authoredData();
    if (!available_ || !data)
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_);
    selectedPoints_.clear();
    selectedElements_.clear();
    selectedElement_ = kInvalidRotoElement;
    const auto ordered = orderedElements(*data);
    for (const auto* element : ordered) {
        if (element->kind == RotoKind::Group || element->points.empty())
            continue;
        if (elementEffectivelyLocked(*data, element->id))
            continue;
        selectedElements_.push_back(element->id);
        for (std::size_t index = 0; index < element->points.size(); ++index)
            selectedPoints_.push_back(element->points[index].id);
        if (selectedElement_ == kInvalidRotoElement)
            selectedElement_ = element->id;
    }
    if (selectedElements_.empty())
        return fail(QStringLiteral("no editable shape is available"));
    clearError();
    emit selectionChanged();
    refresh();
    return true;
}

void RotoController::clearSelection() {
    if (selectedElement_ == 0 && selectedPoints_.empty() && selectedElements_.empty())
        return;
    selectedElement_ = 0;
    selectedPoints_.clear();
    selectedElements_.clear();
    emit selectionChanged();
    refresh();
}

bool RotoController::pointSelected(const QString& pointId) const {
    const auto point = identity(pointId);
    return point && std::find(selectedPoints_.begin(), selectedPoints_.end(), *point) != selectedPoints_.end();
}

bool RotoController::elementEffectivelyLocked(const RotoData& data, RotoElementId id) const {
    std::set<RotoElementId> visited;
    for (const RotoElement* element = findElement(data, id); element != nullptr;) {
        if (element->locked)
            return true;
        if (element->parent == 0 || !visited.insert(element->id).second)
            break;
        element = findElement(data, element->parent);
    }
    return false;
}

const RotoData& RotoController::displayData(RotoElementId id) const {
    if (const auto* evaluated = evaluatedData())
        if (findElement(*evaluated, id))
            return *evaluated;
    return authored_;
}

bool RotoController::shapeSelected(RotoElementId id) const {
    if (selectedElements_.empty())
        return false;
    const auto* data = authoredData();
    if (!data)
        return false;
    std::set<RotoElementId> visited;
    for (const RotoElement* element = findElement(*data, id); element != nullptr;) {
        if (std::find(selectedElements_.begin(), selectedElements_.end(), element->id) != selectedElements_.end())
            return true;
        if (element->parent == 0 || !visited.insert(element->id).second)
            break;
        element = findElement(*data, element->parent);
    }
    return false;
}

void RotoController::collectScopes(const RotoData& data, RotoElementId id, std::set<RotoPointId>& seen,
                                   std::vector<Scope>& scopes) const {
    const auto* element = findElement(data, id);
    if (!element)
        return;
    if (elementEffectivelyLocked(data, id))
        return;
    if (element->kind != RotoKind::Group) {
        for (std::size_t index = 0; index < element->points.size(); ++index) {
            const auto point = element->points[index].id;
            // A point reached through two selected groups - or a shape selected
            // alongside its own group - is still addressed exactly once.
            if (seen.insert(point).second)
                scopes.push_back(Scope{element->id, point});
        }
        return;
    }
    for (std::size_t index = 0; index < data.elements.size(); ++index)
        if (data.elements[index].parent == id)
            collectScopes(data, data.elements[index].id, seen, scopes);
}

std::vector<RotoController::Scope> RotoController::selectionScopes() const {
    std::vector<Scope> scopes;
    const auto* data = authoredData();
    if (!available_ || !data)
        return scopes;
    if (!selectedPoints_.empty()) {
        // An explicit point selection is addressed exactly, even inside a shape
        // that is not in `selectedElements_` after a cross-shape union.
        for (const auto point : selectedPoints_) {
            const auto* owner = findPointElement(*data, point);
            if (!owner)
                continue;
            if (elementEffectivelyLocked(*data, owner->id))
                continue;
            scopes.push_back(Scope{owner->id, point});
        }
        return scopes;
    }
    std::set<RotoPointId> seen;
    for (const auto element : selectedElements_)
        collectScopes(*data, element, seen, scopes);
    return scopes;
}

QString RotoController::beginGesture(const QVariantList& targets) {
    if (gestureToken_ != 0)
        return fail(QStringLiteral("a Roto edit is already in progress")), QString{};
    if (targets.isEmpty())
        return fail(QStringLiteral("a Roto edit requires at least one target")), QString{};
    try {
        std::vector<ParameterEdit> edits;
        std::vector<ParameterAddress> addresses;
        std::vector<ParameterSpec> specs;
        edits.reserve(static_cast<std::size_t>(targets.size()));
        addresses.reserve(static_cast<std::size_t>(targets.size()));
        specs.reserve(static_cast<std::size_t>(targets.size()));
        for (const auto& entry : targets) {
            const auto target = entry.toMap();
            const auto element = identity(target.value(QStringLiteral("element")).toString());
            if (!element)
                return fail(QStringLiteral("a Roto edit requires an element identity")), QString{};
            Scope scope;
            scope.element = *element;
            const auto pointText = target.value(QStringLiteral("point")).toString();
            if (!pointText.isEmpty()) {
                const auto point = identity(pointText);
                if (!point)
                    return fail(QStringLiteral("a Roto edit requires a valid point identity")), QString{};
                scope.point = *point;
            }
            const auto key = target.value(QStringLiteral("key")).toString().trimmed();
            const auto address = addressFor(scope, key);
            if (!address)
                return fail(QStringLiteral("'%1' is not an editable Roto property").arg(key)), QString{};
            const auto* authored = authoredData();
            const auto* record = authored ? findElement(*authored, *element) : nullptr;
            if (!record)
                return fail(QStringLiteral("the element no longer exists")), QString{};
            if (scope.point != 0 && !rotoPoint(*record, scope.point))
                return fail(QStringLiteral("the point no longer exists")), QString{};
            // A locked element is an authored refusal from the model: the
            // presenter states it instead of publishing a rejected preview.
            if (record->locked)
                return fail(QStringLiteral("the element is locked")), QString{};
            const auto* spec = rotoParameterSpec(*address);
            const auto value = currentValue(scope, key);
            if (!spec || !value)
                return fail(QStringLiteral("'%1' is not an editable Roto property").arg(key)), QString{};
            addresses.push_back(*address);
            specs.push_back(*spec);
            edits.push_back(ParameterEdit{*address, *value});
        }
        const auto gesture = session_.beginValueParameterGesture(static_cast<double>(frame_), edits,
                                                                 {.expectedRevision = session_.revision()});
        if (gesture.token == 0) {
            if (gesture.result.error)
                return fail(QString::fromStdString(gesture.result.error->message)), QString{};
            return fail(QStringLiteral("the Roto edit was refused")), QString{};
        }
        gestureToken_ = gesture.token;
        gestureAddresses_ = std::move(addresses);
        gestureSpecs_ = std::move(specs);
        gestureInvalid_ = false;
        preview_ = gesture.snapshot;
        clearError();
        emit gestureChanged();
        return QString::number(static_cast<qulonglong>(gesture.token));
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what())), QString{};
    }
}

bool RotoController::updateGesture(const QString& token, const QVariantList& values) {
    bool valid = false;
    const auto parsed = token.trimmed().toULongLong(&valid);
    if (!valid || parsed == 0 || parsed != gestureToken_)
        return fail(QStringLiteral("the Roto edit update requires the active gesture token"));
    if (values.size() != static_cast<qsizetype>(gestureAddresses_.size()))
        return fail(QStringLiteral("the Roto edit update requires one value per addressed property"));
    try {
        std::vector<ParameterEdit> edits;
        edits.reserve(gestureAddresses_.size());
        for (std::size_t index = 0; index < gestureAddresses_.size(); ++index) {
            QString conversionError;
            const auto converted =
                convert(gestureSpecs_[index], values[static_cast<qsizetype>(index)], conversionError);
            if (!converted) {
                gestureInvalid_ = true;
                return fail(conversionError);
            }
            edits.push_back(ParameterEdit{gestureAddresses_[index], *converted});
        }
        const auto gesture = session_.updateParameterGesture(gestureToken_, edits);
        if (gesture.token == 0) {
            gestureInvalid_ = true;
            if (gesture.result.error)
                return fail(QString::fromStdString(gesture.result.error->message));
            return fail(QStringLiteral("the Roto edit preview was refused"));
        }
        gestureInvalid_ = false;
        clearError();
        // The preview the session now holds is what the panel states: the
        // presenters read the live geometry from the published records instead
        // of keeping a second preview of their own.
        preview_ = gesture.snapshot;
        refreshPreviewGeometry();
        return true;
    } catch (const std::exception& error) {
        gestureInvalid_ = true;
        return fail(QString::fromUtf8(error.what()));
    }
}

bool RotoController::commitGesture(const QString& token) {
    bool valid = false;
    const auto parsed = token.trimmed().toULongLong(&valid);
    if (!valid || parsed == 0 || parsed != gestureToken_)
        return fail(QStringLiteral("the Roto edit commit requires the active gesture token"));
    if (gestureInvalid_) {
        const auto problem = error_.isEmpty() ? QStringLiteral("the Roto edit was rejected") : error_;
        static_cast<void>(session_.cancelParameterGesture(gestureToken_));
        gestureToken_ = 0;
        gestureAddresses_.clear();
        gestureSpecs_.clear();
        gestureInvalid_ = false;
        preview_.reset();
        transformTargets_.clear();
        emit gestureChanged();
        return fail(problem);
    }
    const auto result = session_.commitParameterGesture(gestureToken_, {.expectedRevision = session_.revision()});
    if (!result.committed && result.error)
        static_cast<void>(session_.cancelParameterGesture(gestureToken_));
    gestureToken_ = 0;
    gestureAddresses_.clear();
    gestureSpecs_.clear();
    gestureInvalid_ = false;
    preview_.reset();
    transformTargets_.clear();
    emit gestureChanged();
    if (!result.committed && !result.error) {
        // A batch whose every address is unchanged publishes nothing; that is a
        // completed no-op, not a rejection.
        clearError();
        return true;
    }
    if (result.error)
        return fail(QString::fromStdString(result.error->message));
    clearError();
    refresh();
    return true;
}

bool RotoController::cancelGesture(const QString& token) {
    bool valid = false;
    const auto parsed = token.trimmed().toULongLong(&valid);
    if (!valid || parsed == 0 || parsed != gestureToken_)
        return fail(QStringLiteral("the Roto edit cancel requires the active gesture token"));
    const auto rejected = gestureInvalid_;
    const auto result = session_.cancelParameterGesture(gestureToken_);
    gestureToken_ = 0;
    gestureAddresses_.clear();
    gestureSpecs_.clear();
    gestureInvalid_ = false;
    preview_.reset();
    transformTargets_.clear();
    emit gestureChanged();
    if (result.error)
        return fail(QString::fromStdString(result.error->message));
    if (rejected)
        return false;
    clearError();
    refresh();
    return true;
}

void RotoController::cancelHistoryGesture() {
    cancelDraft();
    if (gestureToken_ != 0)
        static_cast<void>(cancelGesture(QString::number(static_cast<qulonglong>(gestureToken_))));
}

bool RotoController::keyAtFrame(const QString& elementId, const QString& pointId, const QString& key) {
    const auto element = identity(elementId);
    if (!element)
        return fail(QStringLiteral("keying requires an element identity"));
    Scope scope;
    scope.element = *element;
    if (!pointId.isEmpty()) {
        const auto point = identity(pointId);
        if (!point)
            return fail(QStringLiteral("keying requires a valid point identity"));
        scope.point = *point;
    }
    const auto address = addressFor(scope, key);
    if (!address)
        return fail(QStringLiteral("'%1' is not a keyable Roto property").arg(key));
    const auto* spec = rotoParameterSpec(*address);
    if (!spec)
        return fail(QStringLiteral("'%1' is not a keyable Roto property").arg(key));
    try {
        const auto frame = static_cast<double>(frame_);
        const auto current = currentValue(scope, key);
        if (!current)
            return fail(QStringLiteral("the Roto property is unavailable"));
        const auto* channel = session_.document().animationChannel(*address);
        const Keyframe* existing = nullptr;
        if (channel) {
            const auto found = std::find_if(channel->keys.begin(), channel->keys.end(),
                                            [frame](const Keyframe& item) { return item.time == frame; });
            if (found != channel->keys.end())
                existing = &*found;
        }
        Command command = {};
        if (existing == nullptr) {
            // A channel that holds no key at THIS frame is not a conflict: the
            // existing insertion factory creates the channel when the property
            // was never keyed and inserts a key with the segment's continuity
            // when it was, so "key the value I see" works at every frame.
            command = insertKeyframeCommand(*address, frame);
        } else {
            if (existing->value == *current) {
                clearError();
                return true;
            }
            Keyframe replacement = *existing;
            replacement.value = *current;
            command = setKeyframesCommand({KeyframeEdit{*address, std::move(replacement)}});
        }
        const auto result = session_.submit(std::move(command), {.expectedRevision = session_.revision()});
        if (result.error)
            return fail(QString::fromStdString(result.error->message));
        clearError();
        refresh();
        return true;
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}

bool RotoController::removeKeyAtFrame(const QString& elementId, const QString& pointId, const QString& key) {
    const auto element = identity(elementId);
    if (!element)
        return fail(QStringLiteral("key removal requires an element identity"));
    Scope scope;
    scope.element = *element;
    if (!pointId.isEmpty()) {
        const auto point = identity(pointId);
        if (!point)
            return fail(QStringLiteral("key removal requires a valid point identity"));
        scope.point = *point;
    }
    const auto address = addressFor(scope, key);
    if (!address)
        return fail(QStringLiteral("'%1' is not a keyable Roto property").arg(key));
    try {
        const auto frame = static_cast<double>(frame_);
        const auto* channel = session_.document().animationChannel(*address);
        if (!channel)
            return false;
        const auto found = std::find_if(channel->keys.begin(), channel->keys.end(),
                                        [frame](const Keyframe& item) { return item.time == frame; });
        if (found == channel->keys.end())
            return false;
        const auto result = session_.submit(removeKeyframesCommand({KeyframeRef{channel->id, found->id}}),
                                            {.expectedRevision = session_.revision()});
        if (result.error)
            return fail(QString::fromStdString(result.error->message));
        clearError();
        refresh();
        return true;
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}

bool RotoController::commitData(RotoData data) {
    try {
        const auto result = session_.submit(setRotoDataCommand(network_, node_, std::move(data)),
                                            {.expectedRevision = session_.revision()});
        if (result.error)
            return fail(QString::fromStdString(result.error->message));
        clearError();
        refresh();
        return true;
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}

QString RotoController::uniqueName(const RotoData& data, const QString& base) const {
    std::set<QString> taken;
    for (std::size_t i = 0; i < data.elements.size(); ++i)
        taken.insert(QString::fromStdString(data.elements[i].name));
    for (std::uint64_t suffix = data.nextElementId;; ++suffix) {
        const auto candidate = QStringLiteral("%1 %2").arg(base).arg(static_cast<qulonglong>(suffix));
        if (taken.find(candidate) == taken.end())
            return candidate;
    }
}

RotoElementId RotoController::creationParent() const {
    if (selectedElement_ == 0)
        return 0;
    const auto* data = authoredData();
    const auto* element = data ? findElement(*data, selectedElement_) : nullptr;
    return element && element->kind == RotoKind::Group ? element->id : 0;
}

QString RotoController::createShape(const QString& kind, double x0, double y0, double x1, double y1) {
    if (!available_)
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_), QString{};
    const auto name = kind.trimmed().toLower();
    if (name != QLatin1String("rectangle") && name != QLatin1String("ellipse"))
        return fail(QStringLiteral("'%1' is not a Roto shape").arg(kind)), QString{};
    RotoData data = authoredData() ? *authoredData() : RotoData{};
    // A click without a drag still authors a usable shape: the box is expanded
    // around the pressed corner instead of publishing a degenerate one.
    constexpr double kMinimumExtent = 2.0;
    double left = std::min(x0, x1);
    double top = std::min(y0, y1);
    double width = std::abs(x1 - x0);
    double height = std::abs(y1 - y0);
    if (width < kMinimumExtent) {
        left = std::min(x0, x1) - 50.0;
        width = 100.0;
    }
    if (height < kMinimumExtent) {
        top = std::min(y0, y1) - 50.0;
        height = 100.0;
    }
    RotoElementId created = 0;
    if (name == QLatin1String("rectangle"))
        created =
            appendRotoRectangle(data, creationParent(), uniqueName(data, QStringLiteral("Rectangle")).toStdString(),
                                left, top, width, height);
    else
        created = appendRotoEllipse(data, creationParent(), uniqueName(data, QStringLiteral("Ellipse")).toStdString(),
                                    left + width / 2.0, top + height / 2.0, width / 2.0, height / 2.0);
    if (created == 0)
        return fail(QStringLiteral("the Roto shape could not be created")), QString{};
    if (!commitData(std::move(data)))
        return QString{};
    static_cast<void>(selectElement(identityText(created)));
    return identityText(created);
}

QString RotoController::addGroup(const QString& parentId) {
    if (!available_)
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_), QString{};
    RotoElementId parent = 0;
    if (!parentId.trimmed().isEmpty()) {
        const auto parsed = identity(parentId);
        if (!parsed)
            return fail(QStringLiteral("the parent identity is invalid")), QString{};
        const auto* data = authoredData();
        const auto* element = data ? findElement(*data, *parsed) : nullptr;
        if (!element || element->kind != RotoKind::Group)
            return fail(QStringLiteral("only a group can own elements")), QString{};
        parent = *parsed;
    }
    RotoData data = authoredData() ? *authoredData() : RotoData{};
    const auto created = appendRotoGroup(data, parent, uniqueName(data, QStringLiteral("Group")).toStdString());
    if (created == 0)
        return fail(QStringLiteral("the Roto group could not be created")), QString{};
    if (!commitData(std::move(data)))
        return QString{};
    static_cast<void>(selectElement(identityText(created)));
    return identityText(created);
}

bool RotoController::removeElements(const QStringList& elementIds) {
    if (!available_ || !authoredData())
        return fail(QStringLiteral("the Roto node has no authored shapes"));
    std::set<RotoElementId> removed;
    for (const auto& text : elementIds) {
        const auto parsed = identity(text);
        if (!parsed)
            return fail(QStringLiteral("the element identity is invalid"));
        removed.insert(*parsed);
    }
    if (removed.empty())
        return false;
    RotoData data = *authoredData();
    // Removing a group removes everything it owns: a reparented orphan would be
    // a shape the author never asked to keep.
    bool grew = true;
    while (grew) {
        grew = false;
        for (std::size_t i = 0; i < data.elements.size(); ++i) {
            const auto& element = data.elements[i];
            if (removed.find(element.id) != removed.end())
                continue;
            if (element.parent != 0 && removed.find(element.parent) != removed.end()) {
                removed.insert(element.id);
                grew = true;
            }
        }
    }
    data.elements.eraseIf([&removed](const RotoElement& element) { return removed.find(element.id) != removed.end(); });
    std::erase_if(selectedElements_, [&removed](RotoElementId id) { return removed.find(id) != removed.end(); });
    std::erase_if(selectedPoints_, [&removed](RotoPointId id) { return removed.find(id) != removed.end(); });
    if (removed.find(selectedElement_) != removed.end() || selectedElement_ == kInvalidRotoElement)
        selectedElement_ = selectedElements_.empty() ? kInvalidRotoElement : selectedElements_.front();
    emit selectionChanged();
    return commitData(std::move(data));
}

bool RotoController::renameElement(const QString& elementId, const QString& name) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return fail(QStringLiteral("an element name must not be empty"));
    if (elementLocked(*element))
        return fail(QStringLiteral("the element is locked"));
    RotoData data = *authoredData();
    const auto* found = findElement(data, *element);
    if (!found)
        return fail(QStringLiteral("the element no longer exists"));
    const auto position = data.elements.indexOf([&element](const RotoElement& item) { return item.id == *element; });
    data.elements[position].name = trimmed.toStdString();
    return commitData(std::move(data));
}

bool RotoController::setElementProperty(const QString& elementId, const QString& key, const QVariant& value) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    RotoData data = *authoredData();
    const auto position = data.elements.indexOf([&element](const RotoElement& item) { return item.id == *element; });
    if (position == data.elements.size())
        return fail(QStringLiteral("the element no longer exists"));
    auto& record = data.elements[position];
    // The model's lock rule: a locked element accepts no write except its own
    // unlock, so the presenter never publishes a rejected edit.
    if (record.locked && !keyIs(key, "locked"))
        return fail(QStringLiteral("the element is locked"));
    if (keyIs(key, "locked")) {
        if (value.metaType().id() != QMetaType::Bool)
            return fail(QStringLiteral("'locked' requires a boolean value"));
        record.locked = value.toBool();
    } else if (keyIs(key, "blend")) {
        const auto blend = blendFromName(value.toString().trimmed().toLower());
        if (!blend)
            return fail(QStringLiteral("'%1' is not a blend operation").arg(value.toString()));
        record.blend = *blend;
    } else if (keyIs(key, "featherProfile")) {
        const auto profile = profileFromName(value.toString().trimmed().toLower());
        if (!profile)
            return fail(QStringLiteral("'%1' is not a feather profile").arg(value.toString()));
        record.featherProfile = *profile;
    } else if (keyIs(key, "firstFrame") || keyIs(key, "lastFrame")) {
        std::optional<double> bound;
        if (!value.isNull() && value.isValid() && value.metaType().id() != QMetaType::UnknownType) {
            const auto number = numberFrom(value);
            if (!number || !std::isfinite(*number))
                return fail(QStringLiteral("a lifetime bound requires a numeric value"));
            bound = *number;
        }
        if (keyIs(key, "firstFrame"))
            record.firstFrame = bound;
        else
            record.lastFrame = bound;
    } else {
        return fail(QStringLiteral("'%1' is not a Roto element property").arg(key));
    }
    return commitData(std::move(data));
}

bool RotoController::clearLifetime(const QString& elementId) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    RotoData data = *authoredData();
    const auto position = data.elements.indexOf([&element](const RotoElement& item) { return item.id == *element; });
    if (position == data.elements.size())
        return fail(QStringLiteral("the element no longer exists"));
    data.elements[position].firstFrame.reset();
    data.elements[position].lastFrame.reset();
    return commitData(std::move(data));
}

bool RotoController::moveElement(const QString& elementId, int delta) {
    const auto element = identity(elementId);
    if (!element || !authoredData() || delta == 0)
        return false;
    RotoData data = *authoredData();
    const auto* found = findElement(data, *element);
    if (!found)
        return fail(QStringLiteral("the element no longer exists"));
    if (found->locked)
        return fail(QStringLiteral("the element is locked"));
    std::vector<RotoElement> order;
    order.reserve(data.elements.size());
    for (std::size_t i = 0; i < data.elements.size(); ++i)
        order.push_back(data.elements[i]);
    std::vector<std::size_t> siblings;
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i].parent == found->parent)
            siblings.push_back(i);
    const auto at = std::find_if(siblings.begin(), siblings.end(),
                                 [&order, &element](std::size_t index) { return order[index].id == *element; });
    if (at == siblings.end())
        return fail(QStringLiteral("the element is not a sibling of itself"));
    const auto position = static_cast<std::ptrdiff_t>(std::distance(siblings.begin(), at));
    const auto target =
        std::clamp(position + delta, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(siblings.size()) - 1);
    if (target == position)
        return true;
    const auto from = siblings[static_cast<std::size_t>(position)];
    const auto to = siblings[static_cast<std::size_t>(target)];
    std::swap(order[from], order[to]);
    data.elements = decltype(data.elements)(std::move(order));
    return commitData(std::move(data));
}

bool RotoController::reparentElement(const QString& elementId, const QString& parentId, int index) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    RotoData data = *authoredData();
    const auto* found = findElement(data, *element);
    if (!found)
        return fail(QStringLiteral("the element no longer exists"));
    if (found->locked)
        return fail(QStringLiteral("the element is locked"));
    RotoElementId parent = 0;
    if (!parentId.trimmed().isEmpty()) {
        const auto parsed = identity(parentId);
        if (!parsed)
            return fail(QStringLiteral("the parent identity is invalid"));
        parent = *parsed;
    }
    // Only a group owns elements, and a group never owns itself or one of its
    // own ancestors.
    if (parent != 0) {
        const auto* owner = findElement(data, parent);
        if (!owner || owner->kind != RotoKind::Group)
            return fail(QStringLiteral("only a group can own elements"));
        for (RotoElementId walk = parent; walk != 0;) {
            if (walk == *element)
                return fail(QStringLiteral("a group cannot own itself"));
            const auto* step = findElement(data, walk);
            walk = step ? step->parent : 0;
        }
    }
    std::vector<RotoElement> order;
    order.reserve(data.elements.size());
    for (std::size_t i = 0; i < data.elements.size(); ++i)
        order.push_back(data.elements[i]);
    const auto at =
        std::find_if(order.begin(), order.end(), [&element](const RotoElement& item) { return item.id == *element; });
    RotoElement moved = *at;
    order.erase(at);
    moved.parent = parent;
    std::size_t insertAt = order.size();
    if (index < 0) {
        // Append after the parent's existing children (or at the end for root).
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i].parent == parent)
                insertAt = i + 1;
    } else {
        std::size_t seen = 0;
        insertAt = order.size();
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i].parent != parent)
                continue;
            if (seen == static_cast<std::size_t>(index)) {
                insertAt = i;
                break;
            }
            ++seen;
        }
    }
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(insertAt), moved);
    data.elements = decltype(data.elements)(std::move(order));
    return commitData(std::move(data));
}

bool RotoController::addPoint(const QString& elementId, int index, double x, double y) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    RotoData data = *authoredData();
    const auto position = data.elements.indexOf([&element](const RotoElement& item) { return item.id == *element; });
    if (position == data.elements.size())
        return fail(QStringLiteral("the element no longer exists"));
    auto& record = data.elements[position];
    if (record.locked)
        return fail(QStringLiteral("the element is locked"));
    if (record.kind == RotoKind::Group)
        return fail(QStringLiteral("a group has no points"));
    if (!std::isfinite(x) || !std::isfinite(y))
        return fail(QStringLiteral("a point position must be finite"));
    RotoPoint point;
    point.id = data.nextPointId++;
    point.position = Vector2Value{{static_cast<float>(x), static_cast<float>(y)}};
    const auto count = static_cast<std::ptrdiff_t>(record.points.size());
    const auto at = index < 0 ? count : std::clamp<std::ptrdiff_t>(index, 0, count);
    record.points.insert(static_cast<std::size_t>(at), point);
    const auto created = point.id;
    if (!commitData(std::move(data)))
        return false;
    if (selectedElement_ == *element)
        selectedPoints_ = {created};
    emit selectionChanged();
    refresh();
    return true;
}

bool RotoController::removePoints(const QString& elementId, const QStringList& pointIds) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    std::set<RotoPointId> removed;
    for (const auto& text : pointIds) {
        const auto parsed = identity(text);
        if (!parsed)
            return fail(QStringLiteral("the point identity is invalid"));
        removed.insert(*parsed);
    }
    if (removed.empty())
        return false;
    RotoData data = *authoredData();
    const auto position = data.elements.indexOf([&element](const RotoElement& item) { return item.id == *element; });
    if (position == data.elements.size())
        return fail(QStringLiteral("the element no longer exists"));
    auto& record = data.elements[position];
    if (record.locked)
        return fail(QStringLiteral("the element is locked"));
    if (record.kind == RotoKind::Group)
        return fail(QStringLiteral("a group has no points"));
    std::size_t keeps = 0;
    for (std::size_t i = 0; i < record.points.size(); ++i)
        if (removed.find(record.points[i].id) == removed.end())
            ++keeps;
    if (keeps < 3)
        return fail(QStringLiteral("a closed shape keeps at least three points"));
    record.points.eraseIf([&removed](const RotoPoint& point) { return removed.find(point.id) != removed.end(); });
    std::erase_if(selectedPoints_, [&removed](RotoPointId id) { return removed.find(id) != removed.end(); });
    emit selectionChanged();
    return commitData(std::move(data));
}

bool RotoController::setSmooth(const QString& elementId, const QStringList& pointIds, bool smooth) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    const auto* record = findElement(displayData(*element), *element);
    if (!record)
        return fail(QStringLiteral("the element no longer exists"));
    if (record->kind == RotoKind::Group)
        return fail(QStringLiteral("a group has no points"));
    // No explicit point list means the whole path, which is what a shape-level
    // action means.
    std::vector<Scope> scopes;
    if (pointIds.isEmpty()) {
        for (std::size_t i = 0; i < record->points.size(); ++i)
            scopes.push_back(Scope{*element, record->points[i].id});
    } else {
        for (const auto& text : pointIds) {
            const auto parsed = identity(text);
            if (!parsed)
                return fail(QStringLiteral("the point identity is invalid"));
            scopes.push_back(Scope{*element, *parsed});
        }
    }
    if (scopes.empty())
        return false;
    return smoothScopes(scopes, smooth);
}

bool RotoController::smoothScopes(const std::vector<Scope>& scopes, bool smooth) {
    const auto* data = authoredData();
    if (!data)
        return fail(QStringLiteral("the Roto node has no authored shapes"));
    QVariantList targets;
    QVariantList values;
    for (const auto& scope : scopes) {
        const auto* record = findElement(displayData(scope.element), scope.element);
        if (!record)
            return fail(QStringLiteral("the element no longer exists"));
        const auto elementText = identityText(scope.element);
        const auto pointText = identityText(scope.point);
        if (record->kind == RotoKind::BSpline) {
            targets.push_back(QVariantMap{{QStringLiteral("element"), elementText},
                                          {QStringLiteral("point"), pointText},
                                          {QStringLiteral("key"), QStringLiteral("tension")}});
            values.push_back(smooth ? 0.0 : 1.0);
            continue;
        }
        if (record->kind == RotoKind::Group)
            return fail(QStringLiteral("a group has no points"));
        const auto index = record->points.indexOf([&scope](const RotoPoint& item) { return item.id == scope.point; });
        if (index == record->points.size())
            return fail(QStringLiteral("a selected point no longer exists"));
        const auto& source = record->points[index];
        if (smooth) {
            Vector2Value tangent{{-source.inTangent.value[0], -source.inTangent.value[1]}};
            if (tangent == Vector2Value{})
                tangent = source.outTangent;
            if (tangent == Vector2Value{}) {
                const auto& previous = record->points[(index + record->points.size() - 1) % record->points.size()];
                const auto& next = record->points[(index + 1) % record->points.size()];
                tangent = Vector2Value{{(next.position.value[0] - previous.position.value[0]) / 6.0F,
                                        (next.position.value[1] - previous.position.value[1]) / 6.0F}};
            }
            for (const auto* key : {"inTangent", "outTangent"}) {
                const double sign = key == std::string_view("inTangent") ? -1.0 : 1.0;
                targets.push_back(QVariantMap{{QStringLiteral("element"), elementText},
                                              {QStringLiteral("point"), pointText},
                                              {QStringLiteral("key"), QString::fromLatin1(key)}});
                values.push_back(QVariantList{sign * tangent.value[0], sign * tangent.value[1]});
            }
        } else {
            // A cusp is a corner: both handles sit on the point, so the
            // incoming and outgoing chords disagree in direction.
            for (const auto* key : {"inTangent", "outTangent"}) {
                targets.push_back(QVariantMap{{QStringLiteral("element"), elementText},
                                              {QStringLiteral("point"), pointText},
                                              {QStringLiteral("key"), QString::fromLatin1(key)}});
                values.push_back(QVariantList{0.0, 0.0});
            }
        }
    }
    if (targets.isEmpty())
        return fail(QStringLiteral("no point is selected to refine"));
    const auto token = beginGesture(targets);
    if (token.isEmpty())
        return false;
    if (!updateGesture(token, values) || !commitGesture(token)) {
        static_cast<void>(cancelGesture(token));
        return false;
    }
    return true;
}

bool RotoController::smoothSelection(bool smooth) {
    const auto scopes = selectionScopes();
    if (scopes.empty())
        return fail(QStringLiteral("select points or shapes to refine"));
    return smoothScopes(scopes, smooth);
}

bool RotoController::deleteSelection() {
    const auto* authored = authoredData();
    if (!available_ || !authored)
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_);
    if (!selectedPoints_.empty()) {
        RotoData data = *authored;
        // Group the deleted points by the shape that owns them, so a point
        // selection that spans shapes is still ONE topology command.
        std::vector<std::pair<RotoElementId, std::set<RotoPointId>>> byOwner;
        for (const auto point : selectedPoints_) {
            const auto* owner = findPointElement(*authored, point);
            if (!owner)
                continue;
            const auto found = std::find_if(byOwner.begin(), byOwner.end(),
                                            [owner](const auto& entry) { return entry.first == owner->id; });
            if (found == byOwner.end())
                byOwner.push_back({owner->id, {point}});
            else
                found->second.insert(point);
        }
        if (byOwner.empty())
            return fail(QStringLiteral("the selected points no longer exist"));
        for (const auto& group : byOwner) {
            const auto element = group.first;
            const auto& points = group.second;
            const auto position =
                data.elements.indexOf([element](const RotoElement& item) { return item.id == element; });
            if (position == data.elements.size())
                return fail(QStringLiteral("the element no longer exists"));
            if (elementEffectivelyLocked(data, element))
                return fail(QStringLiteral("the element is locked"));
            std::size_t keeps = 0;
            for (std::size_t index = 0; index < data.elements[position].points.size(); ++index)
                if (points.find(data.elements[position].points[index].id) == points.end())
                    ++keeps;
            // The model's structural rule: a closed shape keeps at least three
            // points, so a deletion that would break one is refused whole.
            if (keeps < 3)
                return fail(QStringLiteral("a closed shape keeps at least three points"));
            data.elements[position].points.eraseIf(
                [&points](const RotoPoint& point) { return points.find(point.id) != points.end(); });
            std::erase_if(selectedPoints_, [&points](RotoPointId id) { return points.find(id) != points.end(); });
        }
        const bool committed = commitData(std::move(data));
        if (committed)
            emit selectionChanged();
        return committed;
    }
    if (selectedElements_.empty())
        return fail(QStringLiteral("nothing is selected to delete"));
    for (const auto element : selectedElements_)
        if (authoredData() && elementEffectivelyLocked(*authoredData(), element))
            return fail(QStringLiteral("the element is locked"));
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(selectedElements_.size()));
    for (const auto element : selectedElements_)
        ids.push_back(identityText(element));
    selectedElements_.clear();
    selectedPoints_.clear();
    selectedElement_ = kInvalidRotoElement;
    const auto removed = removeElements(ids);
    emit selectionChanged();
    refresh();
    return removed;
}

bool RotoController::keySelection(bool remove) {
    const auto scopes = selectionScopes();
    if (!available_ || !authoredData())
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_);
    if (scopes.empty())
        return fail(QStringLiteral("select points or shapes to key"));
    const auto frame = static_cast<double>(frame_);
    std::vector<KeyframeEdit> edits;
    std::vector<KeyframeRef> refs;
    for (const auto& scope : scopes) {
        const auto* record = findElement(displayData(scope.element), scope.element);
        if (!record)
            continue;
        for (const auto& name : geometryKeys(record->kind)) {
            const auto key = keyText(name);
            const auto address = addressFor(scope, key);
            if (!address)
                continue;
            const auto* channel = session_.document().animationChannel(*address);
            const Keyframe* existing = nullptr;
            if (channel)
                for (const auto& candidate : channel->keys)
                    if (candidate.time == frame)
                        existing = &candidate;
            if (remove) {
                if (channel && existing)
                    refs.push_back(KeyframeRef{channel->id, existing->id});
                continue;
            }
            const auto current = currentValue(scope, key);
            if (!current)
                continue;
            if (existing && existing->value == *current)
                continue;
            edits.push_back(KeyframeEdit{*address, keyframeForParameterEdit(session_.document(), nullptr,
                                                                            ParameterEdit{*address, *current}, frame)});
        }
    }
    if (remove ? refs.empty() : edits.empty()) {
        clearError();
        return true;
    }
    try {
        const auto result =
            remove ? session_.submit(removeKeyframesCommand(std::move(refs)), {.expectedRevision = session_.revision()})
                   : session_.submit(setKeyframesCommand(std::move(edits)), {.expectedRevision = session_.revision()});
        if (result.error)
            return fail(QString::fromStdString(result.error->message));
        clearError();
        refresh();
        return true;
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
}

QVariantList RotoController::keyTimesFor(const std::vector<Scope>& scopes) const {
    std::set<double> times;
    for (const auto& scope : scopes) {
        const auto* record = findElement(displayData(scope.element), scope.element);
        if (!record)
            continue;
        for (const auto& name : geometryKeys(record->kind)) {
            const auto address = addressFor(scope, keyText(name));
            if (!address)
                continue;
            const auto* channel = session_.document().animationChannel(*address);
            if (!channel)
                continue;
            for (const auto& key : channel->keys)
                times.insert(key.time);
        }
    }
    QVariantList list;
    list.reserve(static_cast<qsizetype>(times.size()));
    for (const auto time : times)
        list.push_back(time);
    return list;
}

bool RotoController::insertCurvePoint(const QString& elementId, int segment, double t) {
    const auto element = identity(elementId);
    if (!element || !authoredData())
        return fail(QStringLiteral("the element identity is invalid"));
    if (!(t >= 0.0 && t <= 1.0) || !std::isfinite(t))
        return fail(QStringLiteral("a curve position must be a fraction between 0 and 1"));
    RotoData data = *authoredData();
    const auto position = data.elements.indexOf([&element](const RotoElement& item) { return item.id == *element; });
    if (position == data.elements.size())
        return fail(QStringLiteral("the element no longer exists"));
    auto& record = data.elements[position];
    if (record.locked || elementEffectivelyLocked(data, record.id))
        return fail(QStringLiteral("the element is locked"));
    if (record.kind == RotoKind::Group)
        return fail(QStringLiteral("a group has no points"));
    const auto count = record.points.size();
    if (count < 3)
        return fail(QStringLiteral("a closed shape keeps at least three points"));
    if (segment < 0 || static_cast<std::size_t>(segment) >= count)
        return fail(QStringLiteral("the segment does not exist"));
    const auto from = static_cast<std::size_t>(segment);
    const auto to = (from + 1) % count;
    // Hit testing addressed the curve at the current frame, not its unkeyed
    // backing values. Keep the topology edit and any split tangent keys atomic.
    const auto* sampled = findElement(displayData(*element), *element);
    const RotoElement source = sampled ? *sampled : record;
    RotoPoint created;
    created.id = data.nextPointId++;
    created.feather = (1.0 - t) * source.points[from].feather + t * source.points[to].feather;
    if (record.kind == RotoKind::BSpline) {
        // A B-spline's authored points ARE its control polygon, so there is no
        // curve-preserving single-point insertion: the new control point is the
        // curve's own sample, which is what "insert a point here" states for a
        // spline, and its tension is the segment's blend so the softened corner
        // keeps the contour's character.
        const auto at = [&source, from, count](std::size_t offset) -> const RotoPoint& {
            return source.points[(from + count + offset) % count];
        };
        const auto& p0 = at(count - 1);
        const auto& p1 = at(0);
        const auto& p2 = at(1);
        const auto& p3 = at(2);
        const double u2 = t * t;
        const double u3 = u2 * t;
        const double basis[4] = {(-u3 + 3.0 * u2 - 3.0 * t + 1.0) / 6.0, (3.0 * u3 - 6.0 * u2 + 4.0) / 6.0,
                                 (-3.0 * u3 + 3.0 * u2 + 3.0 * t + 1.0) / 6.0, u3 / 6.0};
        const RotoPoint* controls[4] = {&p0, &p1, &p2, &p3};
        double x = 0.0;
        double y = 0.0;
        for (std::size_t control = 0; control < 4; ++control) {
            x += basis[control] * controls[control]->position.value[0];
            y += basis[control] * controls[control]->position.value[1];
        }
        const double tension = (1.0 - t) * p1.tension + t * p2.tension;
        const double chordX = (1.0 - t) * p1.position.value[0] + t * p2.position.value[0];
        const double chordY = (1.0 - t) * p1.position.value[1] + t * p2.position.value[1];
        created.position = Vector2Value{{static_cast<float>((1.0 - tension) * x + tension * chordX),
                                         static_cast<float>((1.0 - tension) * y + tension * chordY)}};
        created.tension = tension;
    } else {
        // De Casteljau preserves the displayed segment at the edited frame.
        const auto& start = source.points[from];
        const auto& end = source.points[to];
        const double p0x = start.position.value[0];
        const double p0y = start.position.value[1];
        const double p3x = end.position.value[0];
        const double p3y = end.position.value[1];
        const double p1x = p0x + start.outTangent.value[0];
        const double p1y = p0y + start.outTangent.value[1];
        const double p2x = p3x + end.inTangent.value[0];
        const double p2y = p3y + end.inTangent.value[1];
        const auto mix = [t](double a, double b) { return a + (b - a) * t; };
        const double ax = mix(p0x, p1x);
        const double ay = mix(p0y, p1y);
        const double bx = mix(p1x, p2x);
        const double by = mix(p1y, p2y);
        const double cx = mix(p2x, p3x);
        const double cy = mix(p2y, p3y);
        const double dx = mix(ax, bx);
        const double dy = mix(ay, by);
        const double ex = mix(bx, cx);
        const double ey = mix(by, cy);
        const double fx = mix(dx, ex);
        const double fy = mix(dy, ey);
        created.position = Vector2Value{{static_cast<float>(fx), static_cast<float>(fy)}};
        created.inTangent = Vector2Value{{static_cast<float>(dx - fx), static_cast<float>(dy - fy)}};
        created.outTangent = Vector2Value{{static_cast<float>(ex - fx), static_cast<float>(ey - fy)}};
        record.points[from].outTangent = Vector2Value{{static_cast<float>(ax - p0x), static_cast<float>(ay - p0y)}};
        record.points[to].inTangent = Vector2Value{{static_cast<float>(cx - p3x), static_cast<float>(cy - p3y)}};
    }
    const auto createdId = created.id;
    std::vector<ParameterEdit> keyed;
    const auto queueKey = [&](RotoPointId point, std::string_view name, ParameterValue value) {
        keyed.push_back({ParameterAddress{network_, node_, std::string(name), 0, *element, point}, std::move(value)});
    };
    bool animated = false;
    for (const auto& point : source.points)
        for (const auto name : geometryKeys(source.kind))
            animated = animated || session_.document().animationChannel(
                                       ParameterAddress{network_, node_, std::string(name), 0, *element, point.id});
    if (animated) {
        queueKey(createdId, kRotoParamPosition, created.position);
        queueKey(createdId, kRotoParamFeather, created.feather);
        if (source.kind == RotoKind::BSpline) {
            queueKey(createdId, kRotoParamTension, created.tension);
        } else {
            queueKey(createdId, kRotoParamInTangent, created.inTangent);
            queueKey(createdId, kRotoParamOutTangent, created.outTangent);
            for (const auto& edit : {ParameterEdit{ParameterAddress{network_, node_, std::string(kRotoParamOutTangent),
                                                                    0, *element, record.points[from].id},
                                                   record.points[from].outTangent},
                                     ParameterEdit{ParameterAddress{network_, node_, std::string(kRotoParamInTangent),
                                                                    0, *element, record.points[to].id},
                                                   record.points[to].inTangent}})
                if (session_.document().animationChannel(edit.address))
                    keyed.push_back(edit);
        }
    }
    record.points.insert(from + 1, std::move(created));
    if (keyed.empty()) {
        if (!commitData(std::move(data)))
            return false;
    } else {
        try {
            const auto result = session_.submit(
                transactionCommand("insert Roto curve point",
                                   {setRotoDataCommand(network_, node_, std::move(data)),
                                    parameterValueCommand(session_.document(), nullptr, frame_, keyed, {})}),
                {.expectedRevision = session_.revision()});
            if (result.error)
                return fail(QString::fromStdString(result.error->message));
            clearError();
        } catch (const std::exception& error) {
            return fail(QString::fromUtf8(error.what()));
        }
    }
    // The new point is what the author just placed: it is the point mode's
    // content, and its own shape is the primary element.
    selectElementSilently(*element);
    selectedPoints_.push_back(createdId);
    emit selectionChanged();
    refresh();
    return true;
}

QString RotoController::beginSelectionTransform() {
    if (gestureToken_ != 0)
        return fail(QStringLiteral("a Roto edit is already in progress")), QString{};
    const auto* authored = authoredData();
    if (!available_ || !authored)
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_), QString{};
    const auto scopes = selectionScopes();
    if (scopes.empty())
        return fail(QStringLiteral("select points or shapes to move")), QString{};
    transformTargets_.clear();
    transformTargets_.reserve(scopes.size());
    QVariantList targets;
    for (const auto& scope : scopes) {
        // The ORIGINAL placement and the ORIGINAL geometry, frozen once: every
        // update states the selection against this sample, so a transform never
        // accumulates, and a release publishes the last sample's answer.
        const auto placement = placementOf(displayData(scope.element), scope.element);
        if (!placement)
            return fail(QStringLiteral("the element no longer exists")), QString{};
        const auto position = currentValue(scope, keyText(kRotoParamPosition));
        const auto inTangent = currentValue(scope, keyText(kRotoParamInTangent));
        const auto outTangent = currentValue(scope, keyText(kRotoParamOutTangent));
        // A placement that cannot be inverted names no image point (a zero scale
        // flattens the shape onto a line), so the selection refuses instead of
        // writing a coordinate the author could never name again.
        const double determinant = placement->a * placement->d - placement->b * placement->c;
        if (!(std::abs(determinant) > 1e-12))
            return fail(QStringLiteral("the shape's transform cannot be inverted, so the selection cannot be moved")),
                   QString{};
        if (!position || !inTangent || !outTangent)
            return fail(QStringLiteral("the selection's geometry is unavailable")), QString{};
        const auto* positionValue = std::get_if<Vector2Value>(&*position);
        const auto* inValue = std::get_if<Vector2Value>(&*inTangent);
        const auto* outValue = std::get_if<Vector2Value>(&*outTangent);
        if (!positionValue || !inValue || !outValue)
            return fail(QStringLiteral("the selection's geometry is unavailable")), QString{};
        TransformTarget target;
        target.element = scope.element;
        target.point = scope.point;
        target.position = *positionValue;
        target.inTangent = *inValue;
        target.outTangent = *outValue;
        target.placement = *placement;
        transformTargets_.push_back(target);
        const auto elementText = identityText(scope.element);
        const auto pointText = identityText(scope.point);
        for (const auto& name : {kRotoParamPosition, kRotoParamInTangent, kRotoParamOutTangent})
            targets.push_back(QVariantMap{{QStringLiteral("element"), elementText},
                                          {QStringLiteral("point"), pointText},
                                          {QStringLiteral("key"), keyText(name)}});
    }
    const auto token = beginGesture(targets);
    if (token.isEmpty()) {
        transformTargets_.clear();
        return token;
    }
    return token;
}

bool RotoController::updateSelectionTransform(const QString& token, double a, double b, double c, double d, double e,
                                              double f) {
    const auto finite = [](double value) { return std::isfinite(value); };
    if (!finite(a) || !finite(b) || !finite(c) || !finite(d) || !finite(e) || !finite(f))
        return fail(QStringLiteral("a selection transform must be finite"));
    bool valid = false;
    const auto parsed = token.trimmed().toULongLong(&valid);
    if (!valid || parsed == 0 || parsed != gestureToken_)
        return fail(QStringLiteral("the selection transform requires the active gesture token"));
    if (transformTargets_.empty())
        return fail(QStringLiteral("no live selection transform is frozen"));
    const double determinant = a * d - b * c;
    if (!(std::abs(determinant) > 1e-12))
        return fail(QStringLiteral("the selection transform is not invertible"));
    QVariantList values;
    values.reserve(static_cast<qsizetype>(transformTargets_.size()) * 3);
    for (const auto& target : transformTargets_) {
        const auto& placement = target.placement;
        const double placementDeterminant = placement.a * placement.d - placement.b * placement.c;
        // Image pixel of the frozen point, then the affine, then back into the
        // element's own coordinates through the frozen placement.
        const double imageX =
            placement.a * target.position.value[0] + placement.c * target.position.value[1] + placement.e;
        const double imageY =
            placement.b * target.position.value[0] + placement.d * target.position.value[1] + placement.f;
        const double movedX = a * imageX + c * imageY + e;
        const double movedY = b * imageX + d * imageY + f;
        const double localX =
            (placement.d * (movedX - placement.e) - placement.c * (movedY - placement.f)) / placementDeterminant;
        const double localY =
            (-placement.b * (movedX - placement.e) + placement.a * (movedY - placement.f)) / placementDeterminant;
        // A tangent is a VECTOR: the affine's translation does not touch it, and
        // its direction is composed through the frozen placement's linear part
        // and back, so a rotated/scaled selection keeps its handles attached.
        const auto mappedTangent = [&](double x, double y) {
            const double placedX = placement.a * x + placement.c * y;
            const double placedY = placement.b * x + placement.d * y;
            const double tangentX = a * placedX + c * placedY;
            const double tangentY = b * placedX + d * placedY;
            return QPointF((placement.d * tangentX - placement.c * tangentY) / placementDeterminant,
                           (-placement.b * tangentX + placement.a * tangentY) / placementDeterminant);
        };
        const auto inVector = mappedTangent(target.inTangent.value[0], target.inTangent.value[1]);
        const auto outVector = mappedTangent(target.outTangent.value[0], target.outTangent.value[1]);
        values.push_back(QVariantList{localX, localY});
        values.push_back(QVariantList{inVector.x(), inVector.y()});
        values.push_back(QVariantList{outVector.x(), outVector.y()});
    }
    return updateGesture(token, values);
}

bool RotoController::beginDraft(const QString& kind, double x, double y) {
    const auto name = kind.trimmed().toLower();
    const auto parsed = kindFromName(name);
    if (name != QLatin1String("rectangle") && name != QLatin1String("ellipse") &&
        (!parsed || *parsed == RotoKind::Group))
        return fail(QStringLiteral("'%1' is not a drawable Roto kind").arg(kind));
    if (!available_)
        return fail(reason_.isEmpty() ? QStringLiteral("the Roto node is unavailable") : reason_);
    if (!std::isfinite(x) || !std::isfinite(y))
        return fail(QStringLiteral("a shape point must be finite"));
    const auto parent = creationParent();
    if (parent != 0 && elementLocked(parent))
        return fail(QStringLiteral("the group is locked"));
    draftKind_ = name;
    draftActive_ = true;
    draft_.clear();
    draft_.push_back(Vector2Value{{static_cast<float>(x), static_cast<float>(y)}});
    // A rectangle/ellipse is one drag, so its second corner is the live point
    // from the start; a path commits each click and shows the rubber band.
    draftLive_ = draft_.front();
    clearError();
    emit draftChanged();
    return true;
}

bool RotoController::updateDraft(double x, double y) {
    if (!draftActive_)
        return false;
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    draftLive_ = Vector2Value{{static_cast<float>(x), static_cast<float>(y)}};
    emit draftChanged();
    return true;
}

bool RotoController::addDraftPoint(double x, double y) {
    if (!draftActive_)
        return false;
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    if (draftKind_ == QLatin1String("rectangle") || draftKind_ == QLatin1String("ellipse"))
        return false;
    draft_.push_back(Vector2Value{{static_cast<float>(x), static_cast<float>(y)}});
    draftLive_ = draft_.back();
    emit draftChanged();
    return true;
}

bool RotoController::commitDraft() {
    if (!draftActive_)
        return false;
    if (draftKind_ == QLatin1String("rectangle") || draftKind_ == QLatin1String("ellipse")) {
        const auto anchor = draft_.front();
        const auto corner = draftLive_ ? *draftLive_ : anchor;
        const auto created =
            createShape(draftKind_, anchor.value[0], anchor.value[1], corner.value[0], corner.value[1]);
        cancelDraft();
        if (created.isEmpty())
            return false;
        finishDraftTool();
        return true;
    }
    const auto kind = kindFromName(draftKind_);
    if (!kind)
        return fail(QStringLiteral("the draft kind is unavailable"));
    if (draft_.size() < 3) {
        cancelDraft();
        return fail(QStringLiteral("a closed shape needs at least three points"));
    }
    RotoData data = authoredData() ? *authoredData() : RotoData{};
    const auto base = draftKind_ == QLatin1String("bspline") ? QStringLiteral("B-spline") : QStringLiteral("Bezier");
    const auto created = appendRotoPath(data, *kind, creationParent(), uniqueName(data, base).toStdString(), draft_);
    if (created == 0) {
        cancelDraft();
        return fail(QStringLiteral("the Roto path could not be created"));
    }
    if (draftKind_ == QLatin1String("bezier")) {
        // The pen tool authors smooth handles: a Catmull-Rom-style tangent
        // through each point, so a clicked outline is a curve and not a
        // polygon. Dragging a handle afterwards takes over from this default.
        const auto position = data.elements.indexOf([created](const RotoElement& item) { return item.id == created; });
        auto& record = data.elements[position];
        const auto count = record.points.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto& previous = record.points[(i + count - 1) % count];
            const auto& next = record.points[(i + 1) % count];
            const Vector2Value tangent{{(next.position.value[0] - previous.position.value[0]) / 6.0F,
                                        (next.position.value[1] - previous.position.value[1]) / 6.0F}};
            record.points[i].outTangent = tangent;
            record.points[i].inTangent = Vector2Value{{-tangent.value[0], -tangent.value[1]}};
        }
    }
    cancelDraft();
    if (!commitData(std::move(data)))
        return false;
    static_cast<void>(selectElement(identityText(created)));
    finishDraftTool();
    return true;
}

void RotoController::finishDraftTool() {
    // A finished shape returns the viewport to Select: the pen is a mode, and
    // leaving it armed after the shape closes is how a second stray shape gets
    // drawn. Escape and a cancelled draft deliberately keep the tool.
    if (tool_ == QLatin1String("select"))
        return;
    tool_ = QStringLiteral("select");
    emit toolChanged();
}

void RotoController::cancelDraft() {
    if (!draftActive_ && draft_.empty() && !draftLive_)
        return;
    draftActive_ = false;
    draftKind_.clear();
    draft_.clear();
    draftLive_.reset();
    emit draftChanged();
}

std::vector<const RotoElement*> RotoController::orderedElements(const RotoData& data) const {
    std::vector<const RotoElement*> ordered;
    ordered.reserve(data.elements.size());
    std::set<RotoElementId> visited;
    std::function<void(RotoElementId)> visit = [&](RotoElementId parent) {
        for (std::size_t i = 0; i < data.elements.size(); ++i) {
            const auto& element = data.elements[i];
            if (element.parent != parent)
                continue;
            if (!visited.insert(element.id).second)
                continue;
            ordered.push_back(&element);
            visit(element.id);
        }
    };
    visit(0);
    // An element whose parent is no longer there stays visible in the authored
    // list instead of disappearing from the author's view.
    for (std::size_t i = 0; i < data.elements.size(); ++i) {
        const auto& element = data.elements[i];
        if (visited.insert(element.id).second)
            ordered.push_back(&element);
    }
    return ordered;
}

QVariantMap RotoController::elementRecord(const RotoElement& element, std::size_t depth) const {
    const RotoElement* display = nullptr;
    if (const auto* evaluated = evaluatedData())
        display = findElement(*evaluated, element.id);
    if (!display)
        display = &element;
    return QVariantMap{{QStringLiteral("id"), identityText(element.id)},
                       {QStringLiteral("parent"), identityText(element.parent)},
                       {QStringLiteral("name"), QString::fromStdString(element.name)},
                       {QStringLiteral("kind"), kindName(element.kind)},
                       {QStringLiteral("blend"), blendName(display->blend)},
                       {QStringLiteral("depth"), static_cast<int>(depth)},
                       {QStringLiteral("visible"), display->visible},
                       {QStringLiteral("locked"), display->locked},
                       {QStringLiteral("inverted"), display->inverted},
                       {QStringLiteral("featherEnabled"), display->featherEnabled},
                       {QStringLiteral("group"), element.kind == RotoKind::Group},
                       {QStringLiteral("pointCount"), static_cast<int>(element.points.size())},
                       {QStringLiteral("active"), display != &element},
                       {QStringLiteral("hasLifetime"), element.firstFrame.has_value() || element.lastFrame.has_value()},
                       // Authored lifetime bounds; absent means unbounded, which
                       // the presenter states as no value rather than a frame.
                       {QStringLiteral("firstFrame"), element.firstFrame ? QVariant(*element.firstFrame) : QVariant{}},
                       {QStringLiteral("lastFrame"), element.lastFrame ? QVariant(*element.lastFrame) : QVariant{}},
                       {QStringLiteral("selected"), elementSelected(identityText(element.id))}};
}

QVariantMap RotoController::geometryRecord(const RotoElement& element, const RotoData& data) const {
    const auto placement = placementOf(data, element.id);
    QVariantMap record{{QStringLiteral("id"), identityText(element.id)},
                       {QStringLiteral("parent"), identityText(element.parent)},
                       {QStringLiteral("kind"), kindName(element.kind)},
                       {QStringLiteral("group"), element.kind == RotoKind::Group},
                       {QStringLiteral("inverted"), element.inverted},
                       {QStringLiteral("locked"), element.locked},
                       // A selected group states every shape inside it as
                       // selected, so a presenter highlights what the selection
                       // actually addresses.
                       {QStringLiteral("selected"), shapeSelected(element.id)},
                       {QStringLiteral("a"), placement ? placement->a : 1.0},
                       {QStringLiteral("b"), placement ? placement->b : 0.0},
                       {QStringLiteral("c"), placement ? placement->c : 0.0},
                       {QStringLiteral("d"), placement ? placement->d : 1.0},
                       {QStringLiteral("e"), placement ? placement->e : 0.0},
                       {QStringLiteral("f"), placement ? placement->f : 0.0}};
    // Effective visibility and feather bias are inherited from the group chain.
    bool visible = true;
    double bias = 0.0;
    double featherScale = 1.0;
    for (const RotoElement* walk = &element; walk != nullptr;) {
        visible = visible && walk->visible;
        const double scale = std::sqrt(std::abs(static_cast<double>(walk->scale.value[0]) * walk->scale.value[1]));
        bias = scale * (bias + (walk->featherEnabled ? walk->feather : 0.0));
        featherScale *= scale;
        walk = walk->parent == 0 ? nullptr : findElement(data, walk->parent);
    }
    if (!element.featherEnabled)
        featherScale = 0.0;
    record.insert(QStringLiteral("visible"), visible);
    record.insert(QStringLiteral("featherBias"), bias);
    record.insert(QStringLiteral("featherScale"), featherScale);
    const auto count = element.points.size();
    bool cusp = false;
    if (count > 0) {
        double area = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& current = element.points[i].position.value;
            const auto& next = element.points[(i + 1) % count].position.value;
            area += static_cast<double>(current[0]) * next[1] - static_cast<double>(next[0]) * current[1];
        }
        const double determinant = placement ? placement->a * placement->d - placement->b * placement->c : 1.0;
        cusp = area * determinant < 0.0;
    }
    QVariantList points;
    points.reserve(static_cast<qsizetype>(count));
    const Placement world = placement ? *placement : Placement{};
    const auto transform = [&world](double x, double y) {
        return QPointF(world.a * x + world.c * y + world.e, world.b * x + world.d * y + world.f);
    };
    const auto linear = [&world](double x, double y) {
        return QPointF(world.a * x + world.c * y, world.b * x + world.d * y);
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto& point = element.points[i];
        const auto& [previous, next] = neighbours(element, i);
        const auto origin = transform(point.position.value[0], point.position.value[1]);
        const auto inHandle = linear(point.inTangent.value[0], point.inTangent.value[1]);
        const auto outHandle = linear(point.outTangent.value[0], point.outTangent.value[1]);
        // The outward direction of a point's feather: perpendicular to the
        // local path direction, on the side away from the interior. The
        // winding-independent choice comes from the polygon's signed area.
        const auto& direction = [&]() -> QPointF {
            const QPointF tangent = outHandle - inHandle;
            if (std::abs(tangent.x()) > 1e-9 || std::abs(tangent.y()) > 1e-9)
                return tangent;
            const auto ahead = transform(next.position.value[0], next.position.value[1]);
            const auto behind = transform(previous.position.value[0], previous.position.value[1]);
            QPointF chord = ahead - behind;
            if (std::abs(chord.x()) < 1e-9 && std::abs(chord.y()) < 1e-9)
                chord = QPointF(1.0, 0.0);
            return chord;
        }();
        const QPointF outward = cusp ? QPointF(-direction.y(), direction.x()) : QPointF(direction.y(), -direction.x());
        const double length = std::sqrt(outward.x() * outward.x() + outward.y() * outward.y());
        const QPointF unit = length > 1e-9 ? QPointF(outward.x() / length, outward.y() / length) : QPointF(0.0, -1.0);
        const double effective = featherScale * point.feather + bias;
        const auto tip = origin + unit * effective;
        points.push_back(QVariantMap{{QStringLiteral("id"), identityText(point.id)},
                                     {QStringLiteral("index"), static_cast<int>(i)},
                                     {QStringLiteral("x"), origin.x()},
                                     {QStringLiteral("y"), origin.y()},
                                     {QStringLiteral("inX"), inHandle.x()},
                                     {QStringLiteral("inY"), inHandle.y()},
                                     {QStringLiteral("outX"), outHandle.x()},
                                     {QStringLiteral("outY"), outHandle.y()},
                                     // The outward unit normal of this point's feather, in
                                     // image space, and the point's own signed feather.
                                     {QStringLiteral("nx"), unit.x()},
                                     {QStringLiteral("ny"), unit.y()},
                                     {QStringLiteral("feather"), static_cast<double>(point.feather)},
                                     {QStringLiteral("effectiveFeather"), effective},
                                     {QStringLiteral("tipX"), tip.x()},
                                     {QStringLiteral("tipY"), tip.y()},
                                     {QStringLiteral("tension"), static_cast<double>(point.tension)},
                                     {QStringLiteral("selected"), pointSelected(identityText(point.id))}});
    }
    record.insert(QStringLiteral("points"), points);
    return record;
}

QVariantList RotoController::pointRecords(const RotoElement& element) const {
    QVariantList records;
    records.reserve(static_cast<qsizetype>(element.points.size()));
    for (std::size_t i = 0; i < element.points.size(); ++i) {
        const auto& point = element.points[i];
        records.push_back(QVariantMap{{QStringLiteral("id"), identityText(point.id)},
                                      {QStringLiteral("index"), static_cast<int>(i)},
                                      {QStringLiteral("x"), static_cast<double>(point.position.value[0])},
                                      {QStringLiteral("y"), static_cast<double>(point.position.value[1])},
                                      {QStringLiteral("inX"), static_cast<double>(point.inTangent.value[0])},
                                      {QStringLiteral("inY"), static_cast<double>(point.inTangent.value[1])},
                                      {QStringLiteral("outX"), static_cast<double>(point.outTangent.value[0])},
                                      {QStringLiteral("outY"), static_cast<double>(point.outTangent.value[1])},
                                      {QStringLiteral("feather"), static_cast<double>(point.feather)},
                                      {QStringLiteral("tension"), static_cast<double>(point.tension)},
                                      {QStringLiteral("selected"), pointSelected(identityText(point.id))}});
    }
    return records;
}

double RotoController::networkPixelAspect() const {
    try {
        const auto aspect = static_cast<double>(session_.document().network(network_).format().pixelAspect);
        return aspect > 0.0 && std::isfinite(aspect) ? aspect : 1.0;
    } catch (const std::exception&) {
        return 1.0;
    }
}

RotoController::Placement RotoController::localPlacement(const RotoElement& element, double pixelAspect) const {
    const double radians = element.rotation * kPi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double scaleX = static_cast<double>(element.scale.value[0]);
    const double scaleY = static_cast<double>(element.scale.value[1]);
    const double pivotX = static_cast<double>(element.pivot.value[0]);
    const double pivotY = static_cast<double>(element.pivot.value[1]);
    Placement placement;
    placement.a = cosine * scaleX;
    placement.b = sine * pixelAspect * scaleX;
    placement.c = -sine * scaleY / pixelAspect;
    placement.d = cosine * scaleY;
    placement.e =
        pivotX + static_cast<double>(element.translation.value[0]) - placement.a * pivotX - placement.c * pivotY;
    placement.f =
        pivotY + static_cast<double>(element.translation.value[1]) - placement.b * pivotX - placement.d * pivotY;
    return placement;
}

std::optional<RotoController::Placement> RotoController::placementOf(const RotoData& data, RotoElementId id) const {
    std::vector<const RotoElement*> chain;
    for (const RotoElement* element = findElement(data, id); element != nullptr;) {
        chain.push_back(element);
        if (element->parent == 0)
            break;
        element = findElement(data, element->parent);
    }
    if (chain.empty())
        return std::nullopt;
    const double pixelAspect = networkPixelAspect();
    // A child's own transform runs first, then every ancestor's, root last.
    Placement total = localPlacement(*chain.front(), pixelAspect);
    for (std::size_t i = 1; i < chain.size(); ++i) {
        const auto outer = localPlacement(*chain[i], pixelAspect);
        Placement composed;
        composed.a = outer.a * total.a + outer.c * total.b;
        composed.b = outer.b * total.a + outer.d * total.b;
        composed.c = outer.a * total.c + outer.c * total.d;
        composed.d = outer.b * total.c + outer.d * total.d;
        composed.e = outer.a * total.e + outer.c * total.f + outer.e;
        composed.f = outer.b * total.e + outer.d * total.f + outer.f;
        total = composed;
    }
    return total;
}

void RotoController::refreshPreviewGeometry() {
    if (gestureToken_ == 0 || !preview_ || !available_ || !hasAuthored_)
        return;
    std::optional<RotoData> evaluated;
    try {
        evaluated = evaluateRoto(*preview_, network_, node_, static_cast<double>(frame_));
    } catch (const std::exception&) {
        return;
    }
    QVariantList geometry;
    QVariantList points;
    for (const auto* element : orderedElements(*evaluated)) {
        if (element->kind == RotoKind::Group)
            continue;
        geometry.push_back(geometryRecord(*element, *evaluated));
    }
    if (selectedElement_ != 0)
        if (const auto* display = findElement(*evaluated, selectedElement_))
            points = pointRecords(*display);
    const bool changed = geometry_ != geometry || points_ != points;
    geometry_ = std::move(geometry);
    points_ = std::move(points);
    if (changed)
        emit dataChanged();
}

bool RotoController::canOverlayViewer(const QString& target) const {
    const auto targetId = identity(target);
    if (!available_ || node_ == kInvalidNode || !targetId)
        return false;
    if (*targetId == node_)
        return true;
    const Network* network = nullptr;
    try {
        network = &session_.document().network(network_);
    } catch (const std::exception&) {
        return false;
    }
    const auto& graph = network->graph();
    if (!graph.node(node_) || !graph.node(*targetId))
        return false;
    // Self, or a target this node reaches with EVERY route carrying the pixel
    // grid: a second route through a mapping this owner cannot prove is an
    // ambiguity, not a shortcut.
    const auto probe = probeOverlay(graph, node_, *targetId);
    return probe.reaches && probe.safeRoute && !probe.unsafeRoute;
}

QString RotoController::overlayReason(const QString& target) const {
    const auto targetId = identity(target);
    if (!available_ || node_ == kInvalidNode || !targetId)
        return {};
    if (*targetId == node_)
        return {};
    const Network* network = nullptr;
    try {
        network = &session_.document().network(network_);
    } catch (const std::exception&) {
        return {};
    }
    const auto& graph = network->graph();
    if (!graph.node(node_) || !graph.node(*targetId))
        return {};
    const auto probe = probeOverlay(graph, node_, *targetId);
    // An unrelated target owes no reason: a presenter probing candidates keeps
    // looking instead of reporting a refusal about a node it never feeds.
    if (!probe.reaches)
        return {};
    QStringList names;
    for (const auto& type : probe.blockers) {
        const auto text = QString::fromStdString(type);
        if (!names.contains(text))
            names.push_back(text);
    }
    if (names.isEmpty())
        return QStringLiteral("the overlay cannot prove the coordinate mapping to this target");
    const auto list = names.join(QStringLiteral(", "));
    if (probe.unsafeRoute && probe.safeRoute)
        return QStringLiteral("the Roto reaches this target through more than one path (%1), so the overlay cannot "
                              "state one coordinate mapping")
            .arg(list);
    return QStringLiteral("the coordinate mapping through %1 is not proven, so the overlay stays off").arg(list);
}

void RotoController::refresh() {
    const bool hadGesture = gestureToken_ != 0;
    const bool hadDraft = draftActive_;
    const auto revision = session_.revision();
    QVariantList elements;
    QVariantList geometry;
    QVariantList points;
    QString reason;
    bool available = false;
    try {
        const auto& document = session_.document();
        const Network* network = nullptr;
        try {
            network = &document.network(network_);
        } catch (const std::exception&) {
            reason = QStringLiteral("the Roto network no longer exists");
        }
        const NodeInstance* instance = network ? network->graph().node(node_) : nullptr;
        const NodeDescriptor* descriptor = nullptr;
        if (network && !instance)
            reason = QStringLiteral("the Roto node no longer exists");
        if (instance)
            descriptor = instance->type.empty() ? nullptr : network->graph().descriptor(instance->type);
        if (instance && descriptor && descriptor->type != "roto")
            reason = QStringLiteral("the node is not a Roto node");
        if (instance && descriptor && descriptor->type == "roto") {
            available = true;
            hasAuthored_ = static_cast<bool>(instance->roto);
            authored_ = instance->roto ? *instance->roto : RotoData{};
        } else {
            hasAuthored_ = false;
            authored_ = RotoData{};
        }
    } catch (const std::exception& error) {
        reason = QString::fromUtf8(error.what());
        hasAuthored_ = false;
    }
    evaluated_.reset();
    if (available && hasAuthored_) {
        try {
            evaluated_ = evaluateRoto(session_.document(), network_, node_, static_cast<double>(frame_));
        } catch (const std::exception&) {
            // An element with an inadmissible lifetime still leaves the
            // authored hierarchy authorable, so the snapshot is simply the
            // authored structure without an evaluated counterpart.
            evaluated_.reset();
        }
    }
    if (available) {
        const auto ordered = orderedElements(authored_);
        elements.reserve(static_cast<qsizetype>(ordered.size()));
        for (const auto* element : ordered) {
            std::size_t depth = 0;
            for (const RotoElement* walk = element; walk->parent != 0;) {
                const auto* parent = findElement(authored_, walk->parent);
                if (!parent)
                    break;
                ++depth;
                walk = parent;
            }
            elements.push_back(elementRecord(*element, depth));
        }
        if (const auto* evaluated = evaluatedData()) {
            const auto drawn = orderedElements(*evaluated);
            geometry.reserve(static_cast<qsizetype>(drawn.size()));
            for (const auto* element : drawn) {
                if (element->kind == RotoKind::Group)
                    continue;
                geometry.push_back(geometryRecord(*element, *evaluated));
            }
        }
        // The selection is transient, but it can outlive the records it names:
        // an element that was undone away, or a point the same session removed,
        // is dropped here so no presenter is left addressing dead identities.
        const auto selectionBefore = selectedElements_.size() + selectedPoints_.size();
        std::erase_if(selectedElements_, [this](RotoElementId id) { return findElement(authored_, id) == nullptr; });
        std::erase_if(selectedPoints_, [this](RotoPointId id) { return findPointElement(authored_, id) == nullptr; });
        if (selectedElements_.empty()) {
            selectedElement_ = kInvalidRotoElement;
        } else if (std::find(selectedElements_.begin(), selectedElements_.end(), selectedElement_) ==
                   selectedElements_.end()) {
            selectedElement_ = selectedElements_.front();
        }
        if (selectedElement_ != 0) {
            const auto* record = findElement(authored_, selectedElement_);
            const RotoElement* display = nullptr;
            if (const auto* evaluated = evaluatedData())
                display = findElement(*evaluated, selectedElement_);
            if (const RotoElement* source = display ? display : record)
                points = pointRecords(*source);
        }
        if (selectedElements_.size() + selectedPoints_.size() != selectionBefore)
            emit selectionChanged();
    } else {
        if (selectedElement_ != 0 || !selectedPoints_.empty() || !selectedElements_.empty()) {
            selectedElement_ = 0;
            selectedPoints_.clear();
            selectedElements_.clear();
            emit selectionChanged();
        }
        if (hadDraft)
            cancelDraft();
    }
    // A target that vanished under a live gesture can never be committed: the
    // preview is discarded and nothing is published.
    if (hadGesture && (!available || !hasAuthored_)) {
        static_cast<void>(session_.cancelParameterGesture(gestureToken_));
        gestureToken_ = 0;
        gestureAddresses_.clear();
        gestureSpecs_.clear();
        gestureInvalid_ = false;
        preview_.reset();
        transformTargets_.clear();
        emit gestureChanged();
    }
    const auto keyTimes = available ? keyTimesFor(selectionScopes()) : QVariantList{};
    if (keyTimes_ != keyTimes) {
        keyTimes_ = keyTimes;
        emit keyTimesChanged();
    }
    const bool availabilityChanged = available_ != available || reason_ != reason;
    const bool recordsChanged = elements_ != elements || geometry_ != geometry || points_ != points ||
                                availabilityChanged || revision_ != revision;
    available_ = available;
    reason_ = reason;
    revision_ = revision;
    elements_ = std::move(elements);
    geometry_ = std::move(geometry);
    points_ = std::move(points);
    if (recordsChanged)
        emit dataChanged();
}

}  // namespace nemo::ui
