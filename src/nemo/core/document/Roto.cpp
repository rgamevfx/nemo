#include "nemo/core/document/Roto.hpp"

#include "nemo/core/Hashing.hpp"
#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nemo {
namespace {

[[noreturn]] void reject(std::string message, GraphError code = GraphError::InvalidRoto) {
    throw GraphException(code, "roto: " + std::move(message));
}

// An unresolvable roto parameter address is reported exactly like every other
// unresolvable animation address, so callers handle one error kind.
[[noreturn]] void addressProblem(std::string message) {
    throw GraphException(GraphError::ParameterValue, "roto address " + std::move(message));
}

[[nodiscard]] bool finite(const Vector2Value& value) noexcept {
    return std::isfinite(value.value[0]) && std::isfinite(value.value[1]);
}

[[nodiscard]] std::string numberText(double value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

[[nodiscard]] const char* parameterTypeName(ParameterType type) {
    switch (type) {
    case ParameterType::Boolean:
        return "boolean";
    case ParameterType::Integer:
        return "integer";
    case ParameterType::Float:
        return "float";
    case ParameterType::Choice:
        return "choice";
    case ParameterType::Vector2:
        return "vector2";
    case ParameterType::Vector3:
        return "vector3";
    case ParameterType::Color:
        return "color";
    case ParameterType::String:
        return "string";
    }
    return "value";
}

// ---------------------------------------------------------------------------
// The roto parameter schema. One table serves lookup, defaulting, range
// validation and the shared animation machinery, so a key cannot drift between
// the model, the persistence codec and the presentation layer.
//
// Exactly three properties declare a hard range, each because the value feeds
// an operation whose documented algebra or representation requires it — never
// because a slider looks better that way (issue #103):
//
//   * `opacity` [0,1]. An element's (or group's) opacity multiplies its own
//     value BEFORE the sibling fold, so it is an operand of the documented
//     coverage algebra (ADR-0008: Combine `a + b - a*b`, Intersect `a*b`,
//     Subtract `a*(1-b)`). That algebra is the coverage union/intersection only
//     for operands inside [0,1]: two coincident shapes at opacity 2 would fold
//     to `2 + 2 - 4 = 0`, a hole instead of a doubled matte. Leaving the domain
//     would require a NEW fold convention (clamping the operands, or an
//     out-of-domain blend), which is an effect design decision rather than a
//     removed limit. The NODE's own `opacity` parameter is applied after the
//     fold and therefore declares no range at all.
//   * `tension` [0,1]. The endpoints are definitional: 0 is the smooth uniform
//     cubic and 1 the cusp/control polygon (see RotoPoint). Extrapolated tension
//     is a different curve convention, not a larger value of this one.
//   * `featherFalloff` (0, ...). The ramp is raised to `1 / falloff`, so the
//     reciprocal is the requirement: `denorm_min` is this table's spelling of
//     "strictly positive" (ADR-0008 states `falloff > 0`). Zero has no
//     reciprocal and a negative falloff would invert the ramp direction, which
//     is another convention.
//
// Everything else here — translation, scale, pivot, rotation, feather, point
// tangents, lifetime — declares no bound: negative scale mirrors a shape and
// negative feather points inward, both already meaningful.
struct RotoParameterRow {
    ParameterSpec spec;
    // False for an element property; true for a property of one of its points.
    bool pointScope{false};
};

const std::vector<RotoParameterRow>& rotoParameterRows() {
    static const std::vector<RotoParameterRow> rows = [] {
        std::vector<RotoParameterRow> table;
        const auto add = [&table](bool pointScope, std::string_view key, ParameterType type, ParameterValue value,
                                  std::optional<double> minimum = std::nullopt,
                                  std::optional<double> maximum = std::nullopt, std::vector<std::string> choices = {},
                                  std::optional<double> step = std::nullopt) {
            ParameterSpec spec;
            spec.name = std::string(key);
            spec.type = type;
            spec.defaultValue = std::move(value);
            spec.minimum = minimum;
            spec.maximum = maximum;
            spec.choices = std::move(choices);
            spec.step = step;
            table.push_back(RotoParameterRow{std::move(spec), pointScope});
        };
        add(false, kRotoParamTranslation, ParameterType::Vector2, Vector2Value{}, {}, {}, {}, 1.0);
        add(false, kRotoParamScale, ParameterType::Vector2, Vector2Value{{1.0F, 1.0F}}, {}, {}, {}, 0.01);
        add(false, kRotoParamPivot, ParameterType::Vector2, Vector2Value{}, {}, {}, {}, 1.0);
        add(false, kRotoParamRotation, ParameterType::Float, 0.0, {}, {}, {}, 1.0);
        add(false, kRotoParamOpacity, ParameterType::Float, 1.0, 0.0, 1.0, {}, 0.01);
        add(false, kRotoParamFeather, ParameterType::Float, 0.0, {}, {}, {}, 1.0);
        add(false, kRotoParamVisible, ParameterType::Boolean, true);
        add(false, kRotoParamInverted, ParameterType::Boolean, false);
        add(false, kRotoParamFeatherEnabled, ParameterType::Boolean, true);
        add(false, kRotoParamFeatherProfile, ParameterType::Choice, ChoiceValue{"linear"}, {}, {},
            {"linear", "smooth"});
        add(false, kRotoParamFeatherFalloff, ParameterType::Float, 1.0, std::numeric_limits<double>::denorm_min(),
            std::nullopt, {}, 0.01);
        add(true, kRotoParamPosition, ParameterType::Vector2, Vector2Value{}, {}, {}, {}, 1.0);
        add(true, kRotoParamInTangent, ParameterType::Vector2, Vector2Value{}, {}, {}, {}, 1.0);
        add(true, kRotoParamOutTangent, ParameterType::Vector2, Vector2Value{}, {}, {}, {}, 1.0);
        // Feather is a signed outward distance at both element and point scope.
        add(true, kRotoParamFeather, ParameterType::Float, 0.0, {}, {}, {}, 1.0);
        add(true, kRotoParamTension, ParameterType::Float, 0.0, 0.0, 1.0, {}, 0.01);
        return table;
    }();
    return rows;
}

[[nodiscard]] const ParameterSpec* rotoSpecForKey(bool pointScope, std::string_view key) {
    for (const auto& row : rotoParameterRows())
        if (row.pointScope == pointScope && row.spec.name == key)
            return &row.spec;
    return nullptr;
}

[[nodiscard]] Vector2Value asVector(const ParameterValue& value, std::string_view key) {
    if (const auto* vector = std::get_if<Vector2Value>(&value))
        return *vector;
    reject("property '" + std::string(key) + "' did not resolve to a vector2 value");
}

[[nodiscard]] double asNumber(const ParameterValue& value, std::string_view key) {
    if (const auto* number = std::get_if<double>(&value))
        return *number;
    reject("property '" + std::string(key) + "' did not resolve to a numeric value");
}

[[nodiscard]] bool asBoolean(const ParameterValue& value, std::string_view key) {
    if (const auto* boolean = std::get_if<bool>(&value))
        return *boolean;
    reject("property '" + std::string(key) + "' did not resolve to a boolean value");
}

[[nodiscard]] RotoFeatherProfile asFeatherProfile(const ParameterValue& value) {
    const auto* choice = std::get_if<ChoiceValue>(&value);
    if (choice == nullptr)
        reject("property '" + std::string(kRotoParamFeatherProfile) + "' did not resolve to a choice value");
    if (choice->value == "linear")
        return RotoFeatherProfile::Linear;
    if (choice->value == "smooth")
        return RotoFeatherProfile::Smooth;
    reject("'" + choice->value + "' is not a feather profile");
}

void assignElementParameter(RotoElement& element, std::string_view key, const ParameterValue& value) {
    if (key == kRotoParamTranslation)
        element.translation = asVector(value, key);
    else if (key == kRotoParamScale)
        element.scale = asVector(value, key);
    else if (key == kRotoParamPivot)
        element.pivot = asVector(value, key);
    else if (key == kRotoParamRotation)
        element.rotation = asNumber(value, key);
    else if (key == kRotoParamOpacity)
        element.opacity = asNumber(value, key);
    else if (key == kRotoParamFeather)
        element.feather = asNumber(value, key);
    else if (key == kRotoParamVisible)
        element.visible = asBoolean(value, key);
    else if (key == kRotoParamInverted)
        element.inverted = asBoolean(value, key);
    else if (key == kRotoParamFeatherEnabled)
        element.featherEnabled = asBoolean(value, key);
    else if (key == kRotoParamFeatherProfile)
        element.featherProfile = asFeatherProfile(value);
    else if (key == kRotoParamFeatherFalloff)
        element.featherFalloff = asNumber(value, key);
    else
        reject("unknown element property '" + std::string(key) + "'");
}

void assignPointParameter(RotoPoint& point, std::string_view key, const ParameterValue& value) {
    if (key == kRotoParamPosition)
        point.position = asVector(value, key);
    else if (key == kRotoParamInTangent)
        point.inTangent = asVector(value, key);
    else if (key == kRotoParamOutTangent)
        point.outTangent = asVector(value, key);
    else if (key == kRotoParamFeather)
        point.feather = asNumber(value, key);
    else if (key == kRotoParamTension)
        point.tension = asNumber(value, key);
    else
        reject("unknown point property '" + std::string(key) + "'");
}

[[nodiscard]] ParameterValue elementParameter(const RotoElement& element, std::string_view key) {
    if (key == kRotoParamTranslation)
        return element.translation;
    if (key == kRotoParamScale)
        return element.scale;
    if (key == kRotoParamPivot)
        return element.pivot;
    if (key == kRotoParamRotation)
        return element.rotation;
    if (key == kRotoParamOpacity)
        return element.opacity;
    if (key == kRotoParamFeather)
        return element.feather;
    if (key == kRotoParamVisible)
        return element.visible;
    if (key == kRotoParamInverted)
        return element.inverted;
    if (key == kRotoParamFeatherEnabled)
        return element.featherEnabled;
    if (key == kRotoParamFeatherProfile)
        return ChoiceValue{element.featherProfile == RotoFeatherProfile::Smooth ? "smooth" : "linear"};
    if (key == kRotoParamFeatherFalloff)
        return element.featherFalloff;
    addressProblem("references unknown element property '" + std::string(key) + "'");
}

[[nodiscard]] ParameterValue pointParameter(const RotoPoint& point, std::string_view key) {
    if (key == kRotoParamPosition)
        return point.position;
    if (key == kRotoParamInTangent)
        return point.inTangent;
    if (key == kRotoParamOutTangent)
        return point.outTangent;
    if (key == kRotoParamFeather)
        return point.feather;
    if (key == kRotoParamTension)
        return point.tension;
    addressProblem("references unknown point property '" + std::string(key) + "'");
}

// The element addressed by `address`, or a diagnostic. Shared by the authored
// query, the write validator and the writer so they cannot disagree about what
// resolves.
[[nodiscard]] const RotoElement& addressedElement(const RotoData& data, const ParameterAddress& address) {
    if (address.rotoElement == kInvalidRotoElement)
        addressProblem("requires an element scope");
    const RotoElement* element = rotoElement(data, address.rotoElement);
    if (element == nullptr)
        addressProblem("references unknown roto element " + std::to_string(address.rotoElement));
    if (address.rotoPoint != kInvalidRotoPoint) {
        if (element->kind == RotoKind::Group)
            addressProblem("references point " + std::to_string(address.rotoPoint) + " of group element " +
                           std::to_string(address.rotoElement) + ", which has no points");
        if (rotoPoint(*element, address.rotoPoint) == nullptr)
            addressProblem("references unknown point " + std::to_string(address.rotoPoint) + " of element " +
                           std::to_string(address.rotoElement));
    }
    return *element;
}

// The scope-and-lock rule, applied to an already-resolved authored value. Every
// write that addresses a roto property passes through it, so "locked" means the
// same thing for a static value, a keyframe, a key removal and a key move.
[[nodiscard]] std::optional<std::string> rotoScopeProblem(const RotoData& data, const ParameterAddress& address) {
    if (address.rotoElement == kInvalidRotoElement)
        return "a roto property edit requires an element scope";
    const RotoElement* element = rotoElement(data, address.rotoElement);
    if (element == nullptr)
        return "unknown roto element " + std::to_string(address.rotoElement);
    if (address.rotoPoint != kInvalidRotoPoint) {
        if (element->kind == RotoKind::Group)
            return "group element " + std::to_string(element->id) + " has no points";
        if (rotoPoint(*element, address.rotoPoint) == nullptr)
            return "element " + std::to_string(element->id) + " has no point " + std::to_string(address.rotoPoint);
    }
    if (element->locked)
        return "element " + std::to_string(element->id) + " is locked; unlock it before editing";
    return std::nullopt;
}

// Resolves the authored value one scoped write addresses. An address that names
// no roto property at all is not this rule's business: `data` is left null and
// the caller keeps its ordinary behavior.
[[nodiscard]] std::optional<std::string> rotoWriteScope(const Document& document, const ParameterAddress& address,
                                                        const RotoData*& data) {
    static const RotoData kNoRotoData{};
    if (address.rotoElement == kInvalidRotoElement && address.rotoPoint == kInvalidRotoPoint) {
        data = nullptr;
        return std::nullopt;
    }
    if (address.rotoElement == kInvalidRotoElement)
        return "a roto point property requires its element scope";
    if (address.instance != kInvalidNetworkInstance)
        return "roto properties are node-local; an occurrence cannot override them";
    const auto* node = document.network(address.network).graph().node(address.node);
    if (node == nullptr)
        return "unknown node " + std::to_string(address.node);
    data = node->roto ? node->roto.get() : &kNoRotoData;
    return std::nullopt;
}

[[nodiscard]] RotoElementId allocateElementId(RotoData& data) {
    if (data.nextElementId == kInvalidRotoElement || data.nextElementId == std::numeric_limits<RotoElementId>::max())
        reject("element identity space is exhausted", GraphError::InvalidId);
    return data.nextElementId++;
}

[[nodiscard]] RotoPointId allocatePointId(RotoData& data) {
    if (data.nextPointId == kInvalidRotoPoint || data.nextPointId == std::numeric_limits<RotoPointId>::max())
        reject("point identity space is exhausted", GraphError::InvalidId);
    return data.nextPointId++;
}

// ---------------------------------------------------------------------------
// Content identity.

void hashVector(std::uint64_t& hash, const Vector2Value& value) {
    for (const float component : value.value)
        hashMixWord(hash, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(component)));
}

void hashNumber(std::uint64_t& hash, double value) {
    hashMixWord(hash, std::bit_cast<std::uint64_t>(value));
}

void hashOptionalNumber(std::uint64_t& hash, const std::optional<double>& value) {
    hashMixWord(hash, value ? 1 : 0);
    if (value)
        hashNumber(hash, *value);
}

void hashRotoPoint(std::uint64_t& hash, const RotoPoint& point) {
    hashMixWord(hash, point.id);
    hashVector(hash, point.position);
    hashVector(hash, point.inTangent);
    hashVector(hash, point.outTangent);
    hashNumber(hash, point.feather);
    hashNumber(hash, point.tension);
    hashMixText(hash, point.extension.dump());
}

void hashRotoElement(std::uint64_t& hash, const RotoElement& element) {
    hashMixWord(hash, element.id);
    hashMixWord(hash, element.parent);
    hashMixText(hash, element.name);
    hashMixWord(hash, static_cast<std::uint64_t>(element.kind));
    hashMixWord(hash, static_cast<std::uint64_t>(element.blend));
    hashMixWord(hash, element.visible ? 1 : 0);
    hashMixWord(hash, element.locked ? 1 : 0);
    hashMixWord(hash, element.inverted ? 1 : 0);
    hashVector(hash, element.translation);
    hashVector(hash, element.scale);
    hashVector(hash, element.pivot);
    hashNumber(hash, element.rotation);
    hashNumber(hash, element.opacity);
    hashNumber(hash, element.feather);
    hashMixWord(hash, element.featherEnabled ? 1 : 0);
    hashMixWord(hash, static_cast<std::uint64_t>(element.featherProfile));
    hashNumber(hash, element.featherFalloff);
    hashOptionalNumber(hash, element.firstFrame);
    hashOptionalNumber(hash, element.lastFrame);
    hashMixText(hash, element.extension.dump());
    hashMixWord(hash, static_cast<std::uint64_t>(element.points.size()));
    for (const auto& point : element.points)
        hashRotoPoint(hash, point);
}

}  // namespace

const RotoElement* rotoElement(const RotoData& data, RotoElementId id) noexcept {
    const std::size_t index = rotoElementIndex(data, id);
    return index == data.elements.size() ? nullptr : &data.elements[index];
}

std::size_t rotoElementIndex(const RotoData& data, RotoElementId id) noexcept {
    return data.elements.indexOf([id](const RotoElement& element) { return element.id == id; });
}

const RotoPoint* rotoPoint(const RotoElement& element, RotoPointId id) noexcept {
    const std::size_t index = element.points.indexOf([id](const RotoPoint& point) { return point.id == id; });
    return index == element.points.size() ? nullptr : &element.points[index];
}

bool rotoElementActiveAt(const RotoElement& element, double time) noexcept {
    if (element.firstFrame && time < *element.firstFrame)
        return false;
    if (element.lastFrame && time > *element.lastFrame)
        return false;
    return true;
}

std::optional<std::string> validateRotoData(const RotoData& data) {
    if (data.nextElementId == kInvalidRotoElement || data.nextElementId == std::numeric_limits<RotoElementId>::max())
        return "element identity watermark must be nonzero and below the identity limit";
    if (data.nextPointId == kInvalidRotoPoint || data.nextPointId == std::numeric_limits<RotoPointId>::max())
        return "point identity watermark must be nonzero and below the identity limit";
    const std::size_t count = data.elements.size();
    std::set<RotoPointId> pointIds;
    for (std::size_t index = 0; index < count; ++index) {
        const RotoElement& element = data.elements[index];
        const std::string prefix = "element " + std::to_string(element.id) + ": ";
        if (element.id == kInvalidRotoElement || element.id == std::numeric_limits<RotoElementId>::max())
            return prefix + "identity must be nonzero and below the identity limit";
        if (element.id >= data.nextElementId)
            return prefix + "identity is not below the allocator watermark " + std::to_string(data.nextElementId);
        for (std::size_t other = index + 1; other < count; ++other)
            if (data.elements[other].id == element.id)
                return "duplicate element identity " + std::to_string(element.id);
        if (element.kind != RotoKind::Bezier && element.kind != RotoKind::BSpline && element.kind != RotoKind::Group)
            return prefix + "unknown element kind";
        if (element.blend != RotoBlend::Combine && element.blend != RotoBlend::Intersect &&
            element.blend != RotoBlend::Subtract)
            return prefix + "unknown blend mode";
        if (element.featherProfile != RotoFeatherProfile::Linear &&
            element.featherProfile != RotoFeatherProfile::Smooth)
            return prefix + "unknown feather profile";
        if (!finite(element.translation) || !finite(element.scale) || !finite(element.pivot) ||
            !std::isfinite(element.rotation) || !std::isfinite(element.opacity) || !std::isfinite(element.feather) ||
            !std::isfinite(element.featherFalloff))
            return prefix + "numeric properties must be finite";
        if (element.opacity < 0.0 || element.opacity > 1.0)
            return prefix + "opacity must be between 0 and 1";
        if (element.featherFalloff <= 0.0)
            return prefix + "feather falloff must be positive";
        if (element.firstFrame && !std::isfinite(*element.firstFrame))
            return prefix + "first frame must be finite";
        if (element.lastFrame && !std::isfinite(*element.lastFrame))
            return prefix + "last frame must be finite";
        if (element.firstFrame && element.lastFrame && *element.firstFrame > *element.lastFrame)
            return prefix + "lifetime first frame must not exceed its last frame";
        if (element.parent != kInvalidRotoElement) {
            const RotoElement* parent = rotoElement(data, element.parent);
            if (parent == nullptr)
                return prefix + "unknown parent element " + std::to_string(element.parent);
            if (parent->kind != RotoKind::Group)
                return prefix + "parent element " + std::to_string(element.parent) + " is not a group";
            // A walk bounded by the element count terminates unless the
            // parentage closes into a cycle.
            RotoElementId ancestor = element.parent;
            for (std::size_t steps = 0; ancestor != kInvalidRotoElement; ++steps) {
                if (steps >= count)
                    return prefix + "element parentage must be acyclic";
                const RotoElement* step = rotoElement(data, ancestor);
                if (step == nullptr)
                    break;
                ancestor = step->parent;
            }
        }
        if (element.kind == RotoKind::Group) {
            if (!element.points.empty())
                return prefix + "a group element must not carry points";
        } else if (element.points.size() < 3) {
            return prefix + "a closed shape requires at least three points";
        }
        for (std::size_t pointIndex = 0; pointIndex < element.points.size(); ++pointIndex) {
            const RotoPoint& point = element.points[pointIndex];
            const std::string pointPrefix = prefix + "point " + std::to_string(point.id) + ": ";
            if (point.id == kInvalidRotoPoint || point.id == std::numeric_limits<RotoPointId>::max())
                return pointPrefix + "identity must be nonzero and below the identity limit";
            if (point.id >= data.nextPointId)
                return pointPrefix + "identity is not below the allocator watermark " +
                       std::to_string(data.nextPointId);
            if (!pointIds.insert(point.id).second)
                return "duplicate point identity " + std::to_string(point.id);
            if (!finite(point.position) || !finite(point.inTangent) || !finite(point.outTangent))
                return pointPrefix + "geometry must be finite";
            if (!std::isfinite(point.feather) || !std::isfinite(point.tension))
                return pointPrefix + "numeric properties must be finite";
            if (point.tension < 0.0 || point.tension > 1.0)
                return pointPrefix + "tension must be between 0 and 1";
        }
    }
    return std::nullopt;
}

std::optional<std::string> rotoTransitionProblem(const RotoData& previous, const RotoData& next) {
    if (next.nextElementId < previous.nextElementId)
        return "the next element identity cannot move backwards";
    if (next.nextPointId < previous.nextPointId)
        return "the next point identity cannot move backwards";
    for (const auto& element : previous.elements) {
        if (!element.locked)
            continue;
        const RotoElement* replacement = rotoElement(next, element.id);
        if (replacement == nullptr)
            return "element " + std::to_string(element.id) + " is locked and cannot be removed";
        // Compare against the frozen content with its own lock restored, so the
        // only difference a locked element accepts is clearing that lock.
        RotoElement frozen = *replacement;
        frozen.locked = element.locked;
        if (frozen == element)
            continue;
        return "element " + std::to_string(element.id) + " is locked; unlock it before editing";
    }
    return std::nullopt;
}

std::optional<std::string> validateRotoValue(const ParameterAddress& address, const ParameterValue& value) {
    const ParameterSpec* spec = rotoParameterSpec(address);
    if (spec == nullptr)
        return "property '" + address.key + "' is not a " +
               (address.rotoPoint != kInvalidRotoPoint ? std::string{"roto point property"}
                                                       : std::string{"roto element property"});
    if (const auto problem = validateParameterValueRepresentation(value))
        return "property '" + address.key + "' " + *problem;
    if (value.index() != spec->defaultValue.index())
        return "property '" + address.key + "' requires a " + parameterTypeName(spec->type) + " value";
    if (const auto* number = std::get_if<double>(&value)) {
        if (spec->minimum && *number < *spec->minimum)
            return "property '" + address.key + "' must be at least " + numberText(*spec->minimum);
        if (spec->maximum && *number > *spec->maximum)
            return "property '" + address.key + "' must be at most " + numberText(*spec->maximum);
    } else if (const auto* choice = std::get_if<ChoiceValue>(&value)) {
        if (std::find(spec->choices.begin(), spec->choices.end(), choice->value) == spec->choices.end())
            return "'" + choice->value + "' is not a valid " + address.key + " choice";
    }
    return std::nullopt;
}

std::optional<std::string> validateRotoEdit(const RotoData& data, const ParameterAddress& address,
                                            const ParameterValue& value) {
    if (const auto problem = rotoScopeProblem(data, address))
        return problem;
    return validateRotoValue(address, value);
}

std::optional<std::string> rotoWriteProblem(const Document& document, const ParameterAddress& address) {
    const RotoData* data = nullptr;
    if (const auto problem = rotoWriteScope(document, address, data))
        return problem;
    return data == nullptr ? std::nullopt : rotoScopeProblem(*data, address);
}

std::optional<std::string> validateRotoWrite(const Document& document, const ParameterAddress& address,
                                             const ParameterValue& value) {
    const RotoData* data = nullptr;
    if (const auto problem = rotoWriteScope(document, address, data))
        return problem;
    if (data == nullptr)
        return std::nullopt;
    if (const auto problem = rotoScopeProblem(*data, address))
        return problem;
    return validateRotoValue(address, value);
}

bool rotoContentEquals(const RotoData& left, const RotoData& right) {
    return left.nextElementId == right.nextElementId && left.nextPointId == right.nextPointId &&
           left.extension == right.extension && left.elements == right.elements;
}

bool rotoSharedEquals(const std::shared_ptr<const RotoData>& left, const std::shared_ptr<const RotoData>& right) {
    if (left == right)
        return true;
    if (!left || !right)
        return false;
    return rotoContentEquals(*left, *right);
}

std::uint64_t rotoContentHash(const RotoData& data) {
    std::uint64_t hash = kFnv1a64Basis;
    hashMixText(hash, "roto-v1");
    hashMixText(hash, data.extension.dump());
    hashMixWord(hash, static_cast<std::uint64_t>(data.elements.size()));
    for (const auto& element : data.elements)
        hashRotoElement(hash, element);
    return hash;
}

const ParameterSpec* rotoParameterSpec(const ParameterAddress& address) {
    if (address.rotoElement == kInvalidRotoElement)
        return nullptr;
    return rotoSpecForKey(address.rotoPoint != kInvalidRotoPoint, address.key);
}

ParameterValue rotoParameterValue(const Document& document, const ParameterAddress& address) {
    if (address.rotoElement == kInvalidRotoElement)
        addressProblem("requires an element scope");
    if (address.instance != kInvalidNetworkInstance)
        addressProblem("is node-local; an occurrence cannot address roto properties");
    const auto* node = document.network(address.network).graph().node(address.node);
    if (node == nullptr)
        addressProblem("references unknown node " + std::to_string(address.node));
    if (!node->roto)
        addressProblem("references node " + std::to_string(address.node) + ", which has no roto data");
    const RotoElement& element = addressedElement(*node->roto, address);
    if (address.rotoPoint != kInvalidRotoPoint)
        return pointParameter(*rotoPoint(element, address.rotoPoint), address.key);
    return elementParameter(element, address.key);
}

RotoData applyRotoParameter(RotoData data, const ParameterAddress& address, const ParameterValue& value) {
    const std::size_t index = rotoElementIndex(data, address.rotoElement);
    if (index == data.elements.size())
        reject("unknown roto element " + std::to_string(address.rotoElement) + " for property '" + address.key + "'");
    RotoElement& element = data.elements[index];
    if (address.rotoPoint == kInvalidRotoPoint) {
        assignElementParameter(element, address.key, value);
        return data;
    }
    const std::size_t pointIndex =
        element.points.indexOf([&address](const RotoPoint& candidate) { return candidate.id == address.rotoPoint; });
    if (pointIndex == element.points.size())
        reject("element " + std::to_string(address.rotoElement) + " has no point " + std::to_string(address.rotoPoint));
    assignPointParameter(element.points[pointIndex], address.key, value);
    return data;
}

RotoData evaluateRoto(const Document& document, NetworkId network, NodeId node, double time) {
    if (!std::isfinite(time))
        reject("evaluation time must be finite", GraphError::ParameterValue);
    const NodeInstance* authored = document.network(network).graph().node(node);
    if (authored == nullptr)
        reject("evaluation references unknown node " + std::to_string(node), GraphError::UnknownNode);
    if (!authored->roto)
        return {};
    const RotoData& source = *authored->roto;
    RotoData result = source;
    // Static records stay shared. Only inactive subtrees and actually changed
    // animated properties detach storage; sampling never rewrites every point.
    if (std::any_of(source.elements.begin(), source.elements.end(),
                    [time](const RotoElement& element) { return !rotoElementActiveAt(element, time); })) {
        result.elements.eraseIf([&](const RotoElement& element) {
            const RotoElement* ancestor = &element;
            for (std::size_t steps = 0; ancestor && steps < source.elements.size(); ++steps) {
                if (!rotoElementActiveAt(*ancestor, time))
                    return true;
                ancestor = rotoElement(source, ancestor->parent);
            }
            return false;
        });
    }
    for (const auto& channel : document.animationChannels()) {
        const auto& address = channel.address;
        if (address.network != network || address.node != node || address.instance != kInvalidNetworkInstance ||
            address.rotoElement == kInvalidRotoElement)
            continue;
        const auto* element = rotoElement(result, address.rotoElement);
        if (!element)
            continue;
        if (std::any_of(channel.keys.begin(), channel.keys.end(),
                        [](const Keyframe& keyframe) { return !keyframe.opaqueValue.is_null(); }))
            reject("animation channel " + std::to_string(channel.id) + " property '" + address.key +
                       "' has values this build cannot interpret",
                   GraphError::ParameterValue);
        const ParameterSpec* spec = rotoSpecForKey(address.rotoPoint != kInvalidRotoPoint, address.key);
        if (spec == nullptr)
            reject("animation channel " + std::to_string(channel.id) + " property '" + address.key +
                       "' is not a roto property",
                   GraphError::ParameterValue);
        const ParameterValue value = evaluateAnimationChannel(channel, spec->type, time);
        if (const auto problem = validateRotoValue(address, value))
            reject("animation channel " + std::to_string(channel.id) + ": " + *problem, GraphError::ParameterValue);
        const auto* point = address.rotoPoint != kInvalidRotoPoint ? rotoPoint(*element, address.rotoPoint) : nullptr;
        if (address.rotoPoint != kInvalidRotoPoint && !point)
            reject("animation channel " + std::to_string(channel.id) + " references unknown point " +
                       std::to_string(address.rotoPoint),
                   GraphError::ParameterValue);
        const ParameterValue current =
            point ? pointParameter(*point, address.key) : elementParameter(*element, address.key);
        if (current != value)
            result = applyRotoParameter(std::move(result), address, value);
    }
    return result;
}

RotoElementId appendRotoElement(RotoData& data, RotoElement element) {
    element.id = allocateElementId(data);
    for (std::size_t index = 0; index < element.points.size(); ++index) {
        RotoPoint& point = element.points[index];
        if (point.id == kInvalidRotoPoint)
            point.id = allocatePointId(data);
    }
    const RotoElementId id = element.id;
    data.elements.push_back(std::move(element));
    return id;
}

RotoElementId appendRotoGroup(RotoData& data, RotoElementId parent, std::string name) {
    RotoElement element;
    element.parent = parent;
    element.name = std::move(name);
    element.kind = RotoKind::Group;
    return appendRotoElement(data, std::move(element));
}

RotoElementId appendRotoPath(RotoData& data, RotoKind kind, RotoElementId parent, std::string name,
                             const std::vector<Vector2Value>& positions) {
    if (kind == RotoKind::Group)
        reject("a path element cannot be a group");
    if (positions.empty())
        reject("a path element requires at least one point");
    // The number of points a *closed* shape needs is a property of the published
    // value, not of this allocator: a caller assembling a draft can add points
    // one at a time and only the command that publishes the value requires a
    // closed shape.
    RotoElement element;
    element.parent = parent;
    element.name = std::move(name);
    element.kind = kind;
    for (const Vector2Value& position : positions) {
        RotoPoint point;
        point.position = position;
        element.points.push_back(point);
    }
    return appendRotoElement(data, std::move(element));
}

RotoElementId appendRotoRectangle(RotoData& data, RotoElementId parent, std::string name, double x, double y,
                                  double width, double height) {
    const auto corner = [](double px, double py) {
        return Vector2Value{{static_cast<float>(px), static_cast<float>(py)}};
    };
    // Sharp corners: the authored tangents stay zero, so the pen tool's own
    // tangent policy is never pre-empted.
    return appendRotoPath(data, RotoKind::Bezier, parent, std::move(name),
                          {corner(x, y), corner(x + width, y), corner(x + width, y + height), corner(x, y + height)});
}

RotoElementId appendRotoEllipse(RotoData& data, RotoElementId parent, std::string name, double centerX, double centerY,
                                double radiusX, double radiusY) {
    // Cubic approximation of the four elliptical quadrants.
    constexpr double kKappa = 0.5522847498307936;
    const double tangentX = kKappa * radiusX;
    const double tangentY = kKappa * radiusY;
    const auto pointAt = [](double px, double py, double inX, double inY, double outX, double outY) {
        RotoPoint point;
        point.position = Vector2Value{{static_cast<float>(px), static_cast<float>(py)}};
        point.inTangent = Vector2Value{{static_cast<float>(inX), static_cast<float>(inY)}};
        point.outTangent = Vector2Value{{static_cast<float>(outX), static_cast<float>(outY)}};
        return point;
    };
    RotoElement element;
    element.parent = parent;
    element.name = std::move(name);
    element.kind = RotoKind::Bezier;
    element.points.push_back(pointAt(centerX + radiusX, centerY, 0.0, -tangentY, 0.0, tangentY));
    element.points.push_back(pointAt(centerX, centerY + radiusY, tangentX, 0.0, -tangentX, 0.0));
    element.points.push_back(pointAt(centerX - radiusX, centerY, 0.0, tangentY, 0.0, -tangentY));
    element.points.push_back(pointAt(centerX, centerY - radiusY, -tangentX, 0.0, tangentX, 0.0));
    return appendRotoElement(data, std::move(element));
}

}  // namespace nemo
