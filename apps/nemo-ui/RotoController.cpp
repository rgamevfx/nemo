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
#include <set>
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
        if (view == owner)
            return;
    views_.push_back(QPointer<QObject>(owner));
    // A presenter that dies without detaching still releases its view, so the
    // shared selection is never kept alive by a closed panel.
    connect(owner, &QObject::destroyed, this, [this, owner] { detachView(owner); });
}

void RotoController::detachView(QObject* owner) {
    std::erase_if(views_, [owner](const QPointer<QObject>& view) { return !view || view == owner; });
    if (std::any_of(views_.begin(), views_.end(), [](const QPointer<QObject>& view) { return !view.isNull(); }))
        return;
    // The last presenter is gone: this is transient state, never document
    // state, so it is dropped with its views — including a live session
    // preview, which is discarded rather than left owning the one gesture.
    cancelHistoryGesture();
    if (selectedElement_ != 0 || !selectedPoints_.empty()) {
        selectedElement_ = 0;
        selectedPoints_.clear();
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
    if (!additive)
        selectedPoints_.clear();
    if (selectedElement_ == *element && !additive)
        return true;
    // Selecting requires clearing the draft's preview: the selected element is
    // a different authoring context than the shape being drawn.
    cancelDraft();
    selectedElement_ = *element;
    selectedPoints_.clear();
    emit selectionChanged();
    refresh();
    return true;
}

bool RotoController::selectPoint(const QString& pointId, bool additive) {
    const auto point = identity(pointId);
    if (!point || selectedElement_ == 0)
        return false;
    const auto* data = authoredData();
    if (!data)
        return false;
    const auto* owner = findPointElement(*data, *point);
    if (!owner || owner->id != selectedElement_)
        return false;
    if (!additive)
        selectedPoints_.clear();
    if (std::find(selectedPoints_.begin(), selectedPoints_.end(), *point) == selectedPoints_.end())
        selectedPoints_.push_back(*point);
    emit selectionChanged();
    // The published records carry the per-point selection flag, so the
    // presenters that draw handles from them are re-stated.
    refresh();
    return true;
}

void RotoController::clearSelection() {
    if (selectedElement_ == 0 && selectedPoints_.empty())
        return;
    selectedElement_ = 0;
    selectedPoints_.clear();
    emit selectionChanged();
    refresh();
}

bool RotoController::pointSelected(const QString& pointId) const {
    const auto point = identity(pointId);
    return point && std::find(selectedPoints_.begin(), selectedPoints_.end(), *point) != selectedPoints_.end();
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
        Keyframe replacement;
        replacement.time = frame;
        replacement.value = *current;
        if (channel) {
            const auto found = std::find_if(channel->keys.begin(), channel->keys.end(),
                                            [frame](const Keyframe& item) { return item.time == frame; });
            if (found == channel->keys.end())
                return fail(QStringLiteral("the Roto animation channel changed under this edit"));
            if (found->value == *current) {
                clearError();
                return true;
            }
            replacement = *found;
        } else {
            replacement.interpolation =
                animation_detail::componentCount(spec->type) == 0 ? KeyInterpolation::Hold : KeyInterpolation::Linear;
        }
        const auto result = session_.submit(setKeyframesCommand({KeyframeEdit{*address, std::move(replacement)}}),
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
    if (removed.find(selectedElement_) != removed.end()) {
        selectedElement_ = 0;
        selectedPoints_.clear();
        emit selectionChanged();
    }
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
    const auto* record = evaluatedData() ? findElement(*evaluatedData(), *element) : nullptr;
    if (!record)
        record = findElement(*authoredData(), *element);
    if (!record)
        return fail(QStringLiteral("the element no longer exists"));
    if (record->kind == RotoKind::Group)
        return fail(QStringLiteral("a group has no points"));
    // No explicit point list means the whole path, which is what a shape-level
    // action means.
    std::vector<RotoPointId> points;
    if (pointIds.isEmpty()) {
        for (std::size_t i = 0; i < record->points.size(); ++i)
            points.push_back(record->points[i].id);
    } else {
        for (const auto& text : pointIds) {
            const auto parsed = identity(text);
            if (!parsed)
                return fail(QStringLiteral("the point identity is invalid"));
            points.push_back(*parsed);
        }
    }
    if (points.empty())
        return false;
    QVariantList targets;
    QVariantList values;
    const bool bspline = record->kind == RotoKind::BSpline;
    for (const auto point : points) {
        const auto pointText = identityText(point);
        if (bspline) {
            targets.push_back(QVariantMap{{QStringLiteral("element"), identityText(*element)},
                                          {QStringLiteral("point"), pointText},
                                          {QStringLiteral("key"), QStringLiteral("tension")}});
            values.push_back(smooth ? 0.0 : 1.0);
            continue;
        }
        const auto index = record->points.indexOf([point](const RotoPoint& item) { return item.id == point; });
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
                targets.push_back(QVariantMap{{QStringLiteral("element"), identityText(*element)},
                                              {QStringLiteral("point"), pointText},
                                              {QStringLiteral("key"), QString::fromLatin1(key)}});
                values.push_back(QVariantList{sign * tangent.value[0], sign * tangent.value[1]});
            }
        } else {
            // A cusp is a corner: both handles sit on the point, so the
            // incoming and outgoing chords disagree in direction.
            for (const auto* key : {"inTangent", "outTangent"}) {
                targets.push_back(QVariantMap{{QStringLiteral("element"), identityText(*element)},
                                              {QStringLiteral("point"), pointText},
                                              {QStringLiteral("key"), QString::fromLatin1(key)}});
                values.push_back(QVariantList{0.0, 0.0});
            }
        }
    }
    const auto token = beginGesture(targets);
    if (token.isEmpty())
        return false;
    if (!updateGesture(token, values) || !commitGesture(token)) {
        if (!token.isEmpty())
            static_cast<void>(cancelGesture(token));
        return false;
    }
    return true;
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
        return !created.isEmpty();
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
    return true;
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
                       {QStringLiteral("selected"), element.id == selectedElement_}};
}

QVariantMap RotoController::geometryRecord(const RotoElement& element, const RotoData& data) const {
    const auto placement = placementOf(data, element.id);
    QVariantMap record{{QStringLiteral("id"), identityText(element.id)},
                       {QStringLiteral("parent"), identityText(element.parent)},
                       {QStringLiteral("kind"), kindName(element.kind)},
                       {QStringLiteral("group"), element.kind == RotoKind::Group},
                       {QStringLiteral("inverted"), element.inverted},
                       {QStringLiteral("locked"), element.locked},
                       {QStringLiteral("selected"), element.id == selectedElement_},
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
        if (selectedElement_ != 0) {
            const auto* record = findElement(authored_, selectedElement_);
            if (!record) {
                selectedElement_ = 0;
                selectedPoints_.clear();
                emit selectionChanged();
            } else {
                const RotoElement* display = nullptr;
                if (const auto* evaluated = evaluatedData())
                    display = findElement(*evaluated, selectedElement_);
                const auto& source = display ? *display : *record;
                points = pointRecords(source);
                // A point of another element can never stay selected: the
                // selection is one element's points, always.
                const auto before = selectedPoints_.size();
                std::erase_if(selectedPoints_, [&source](RotoPointId id) {
                    return std::none_of(source.points.begin(), source.points.end(),
                                        [id](const RotoPoint& point) { return point.id == id; });
                });
                if (selectedPoints_.size() != before)
                    emit selectionChanged();
            }
        } else if (!selectedPoints_.empty()) {
            selectedPoints_.clear();
            emit selectionChanged();
        }
    } else {
        if (selectedElement_ != 0 || !selectedPoints_.empty()) {
            selectedElement_ = 0;
            selectedPoints_.clear();
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
        emit gestureChanged();
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
