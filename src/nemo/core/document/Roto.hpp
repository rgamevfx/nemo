#pragma once

#include "nemo/core/SharedContainers.hpp"
#include "nemo/core/document/Ids.hpp"
#include "nemo/core/document/ParameterValue.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Node-local authored Roto data (issue #93).
//
// A Roto node owns one immutable `RotoData` value: a nested set of closed
// Bezier/B-spline shapes and groups in full-resolution image coordinates
// (x right, y down). Identity is local to the owning node, exactly like node
// identities are local to a network, and it is never renumbered by an edit,
// undo/redo, a node copy or a reparent, so selection, animation channels and
// rendered geometry stay addressable across all of them.
//
// Published data is immutable (`std::shared_ptr<const RotoData>` on the node);
// every controlled edit publishes a new value that shares the untouched storage
// chunks. Animation and static values are resolved through the existing
// ParameterAddress/AnimationChannel machinery: the roto identity fields are
// appended to the ordinary address, so there is exactly one animation engine,
// one history and one parameter-gesture lifecycle.
//
// This header deliberately forward-declares Document, ParameterAddress and
// ParameterSpec so it can be included by Graph.hpp (which stores the shared
// pointer) without a cycle; the resolving entry points are defined in Roto.cpp.

namespace nemo {

struct Document;
struct ParameterAddress;
struct ParameterSpec;

using RotoElementId = std::uint64_t;
using RotoPointId = std::uint64_t;
inline constexpr RotoElementId kInvalidRotoElement = 0;
inline constexpr RotoPointId kInvalidRotoPoint = 0;

enum class RotoKind : std::uint8_t {
    // Smooth cubic Bezier through every authored point.
    Bezier,
    // Uniform cubic B-spline whose authored points are control points.
    BSpline,
    // A nested container: it carries no points and shapes its children.
    Group,
};

// How an element composes with the accumulated matte of its preceding siblings.
enum class RotoBlend : std::uint8_t { Combine, Intersect, Subtract };

enum class RotoFeatherProfile : std::uint8_t { Linear, Smooth };

// Static property keys. They are the persisted parameter keys the generic
// parameter surface addresses a roto element or point by, so authoring owners
// and the model cannot drift on their spelling.
inline constexpr std::string_view kRotoParamTranslation = "translation";
inline constexpr std::string_view kRotoParamScale = "scale";
inline constexpr std::string_view kRotoParamPivot = "pivot";
inline constexpr std::string_view kRotoParamRotation = "rotation";
inline constexpr std::string_view kRotoParamOpacity = "opacity";
inline constexpr std::string_view kRotoParamFeather = "feather";
inline constexpr std::string_view kRotoParamVisible = "visible";
inline constexpr std::string_view kRotoParamInverted = "inverted";
inline constexpr std::string_view kRotoParamFeatherEnabled = "featherEnabled";
inline constexpr std::string_view kRotoParamFeatherProfile = "featherProfile";
inline constexpr std::string_view kRotoParamFeatherFalloff = "featherFalloff";
inline constexpr std::string_view kRotoParamPosition = "position";
inline constexpr std::string_view kRotoParamInTangent = "inTangent";
inline constexpr std::string_view kRotoParamOutTangent = "outTangent";
inline constexpr std::string_view kRotoParamTension = "tension";

// One control point of a shape. Tangents are OFFSETS from `position`, in
// full-resolution pixels, and `feather` is a signed outward distance in the same
// pixels. `tension` 0 is a smooth uniform cubic and 1 is the cusp/control
// polygon.
struct RotoPoint {
    RotoPointId id{kInvalidRotoPoint};
    Vector2Value position{};
    Vector2Value inTangent{};
    Vector2Value outTangent{};
    double feather{0.0};
    double tension{0.0};
    // Authored fields of the persisted point this build does not model, retained
    // verbatim by the codec so a load/save cycle loses nothing.
    nlohmann::json extension{};

    friend bool operator==(const RotoPoint&, const RotoPoint&) = default;
};

// One authored shape or group. `parent` 0 is the root; a parent is always a
// Group. Storage order among siblings is the authored draw order, and `blend`
// composes with the accumulated matte of the preceding siblings.
struct RotoElement {
    RotoElementId id{kInvalidRotoElement};
    RotoElementId parent{kInvalidRotoElement};
    std::string name;
    RotoKind kind{RotoKind::Bezier};
    RotoBlend blend{RotoBlend::Combine};
    bool visible{true};
    bool locked{false};
    bool inverted{false};
    // Scale, then rotate around `pivot`, then translate, all in parent space;
    // nested parents apply after their children. `rotation` is in degrees and
    // physical-aspect aware.
    Vector2Value translation{};
    Vector2Value scale{{1.0F, 1.0F}};
    Vector2Value pivot{};
    double rotation{0.0};
    double opacity{1.0};
    // Shape-wide feather distance and its profile; a point's own signed feather
    // is added to this. Both are static geometry facts of the authored element.
    double feather{0.0};
    bool featherEnabled{true};
    RotoFeatherProfile featherProfile{RotoFeatherProfile::Linear};
    double featherFalloff{1.0};
    // Authored inclusive lifetime in document-local frames. Unbounded when
    // unset; an element outside its lifetime does not contribute.
    std::optional<double> firstFrame;
    std::optional<double> lastFrame;
    // Empty for a Group; at least three points for a closed shape.
    CowVector<RotoPoint> points;
    // Authored fields of the persisted element this build does not model,
    // retained verbatim by the codec so a load/save cycle loses nothing.
    nlohmann::json extension{};

    friend bool operator==(const RotoElement&, const RotoElement&) = default;
};

// The complete authored value of one Roto node. `nextElementId`/`nextPointId`
// are monotonic allocator watermarks: they are never lowered by a deletion, an
// undo or a restore, exactly like the document's other identity watermarks.
struct RotoData {
    CowVector<RotoElement> elements;
    RotoElementId nextElementId{1};
    RotoPointId nextPointId{1};
    // Authored fields of the persisted Roto record this build does not model,
    // retained verbatim by the codec so a load/save cycle loses nothing.
    nlohmann::json extension{};

    friend bool operator==(const RotoData&, const RotoData&) = default;
};

// ---------------------------------------------------------------------------
// Structure queries (never throw).

[[nodiscard]] const RotoElement* rotoElement(const RotoData& data, RotoElementId id) noexcept;
[[nodiscard]] std::size_t rotoElementIndex(const RotoData& data, RotoElementId id) noexcept;
[[nodiscard]] const RotoPoint* rotoPoint(const RotoElement& element, RotoPointId id) noexcept;
// True when `time` is inside the element's authored lifetime (unset bounds are
// unbounded). Element lifetimes are static; they are not animatable.
[[nodiscard]] bool rotoElementActiveAt(const RotoElement& element, double time) noexcept;

// ---------------------------------------------------------------------------
// Validation.
//
// `validateRotoData` is the complete structural contract for a published value:
// unique identities below the allocator watermarks, finite geometry, at least
// three points per closed shape, no points on a group, an existing Group parent
// with acyclic parentage, and every authored range. It never throws, so a
// controlled write and the persistence codec refuse malformed state with the
// same message.
[[nodiscard]] std::optional<std::string> validateRotoData(const RotoData& data);
// Whole-value edits preserve allocator watermarks and freeze locked elements;
// the only accepted change to a locked element is clearing its own lock.
// Returns the problem, or nullopt when the transition is allowed.
[[nodiscard]] std::optional<std::string> rotoTransitionProblem(const RotoData& previous, const RotoData& next);
// Validates one property write through a scoped roto address: the element (and
// point) must exist, the key must be a roto property of that scope, the value
// must be representable and in range, and a locked element refuses everything
// except clearing its lock. Returns the problem, or nullopt.
[[nodiscard]] std::optional<std::string> validateRotoEdit(const RotoData& data, const ParameterAddress& address,
                                                          const ParameterValue& value);
// The scope-and-lock half of `validateRotoEdit`, resolved against the node the
// address names. Every write that addresses a roto property - a static value, a
// new keyframe, a key removal or a key move - passes through this one rule, so
// "locked" means the same thing on every path. Returns the problem, or nullopt.
[[nodiscard]] std::optional<std::string> rotoWriteProblem(const Document& document, const ParameterAddress& address);
// The complete rule for a write that carries a value: the scope/lock rule above
// plus the property schema and range.
[[nodiscard]] std::optional<std::string> validateRotoWrite(const Document& document, const ParameterAddress& address,
                                                           const ParameterValue& value);
// The value-only half of `validateRotoEdit`: this build must know the key, and
// the value must have that property's shape and range. Used for a keyframe value
// as well as a static write, so a keyed property can never hold a value the
// static property refuses.
[[nodiscard]] std::optional<std::string> validateRotoValue(const ParameterAddress& address,
                                                           const ParameterValue& value);

// ---------------------------------------------------------------------------
// Identity and equality.

// Persisted-content equality: identical elements (including preserved unknown
// fields) and identical allocator watermarks. Shared storage compares by chunk
// without touching records.
[[nodiscard]] bool rotoContentEquals(const RotoData& left, const RotoData& right);
// Same, for the immutable node-owned handles; two null handles are equal.
[[nodiscard]] bool rotoSharedEquals(const std::shared_ptr<const RotoData>& left,
                                    const std::shared_ptr<const RotoData>& right);
// Content hash over every authored element and point property, stable across
// save/reopen. Allocator watermarks are excluded: they record no geometry, so a
// watermark change never invalidates an identity that renders identically.
[[nodiscard]] std::uint64_t rotoContentHash(const RotoData& data);

// ---------------------------------------------------------------------------
// Parameter surface (shared with the ordinary parameter/animation machinery).

// The static schema of a roto-scoped address, or null when `address` names no
// roto property. The returned spec is process-lifetime constant.
[[nodiscard]] const ParameterSpec* rotoParameterSpec(const ParameterAddress& address);
// The authored (never animated) value of a roto-scoped address. Throws
// GraphException when the address, element, point or key does not resolve.
[[nodiscard]] ParameterValue rotoParameterValue(const Document& document, const ParameterAddress& address);
// Applies one validated property write, preserving every identity. The caller
// validates first (validateRotoEdit); this never changes an unrelated property.
[[nodiscard]] RotoData applyRotoParameter(RotoData data, const ParameterAddress& address, const ParameterValue& value);

// ---------------------------------------------------------------------------
// Evaluation.

// The node's authored data with every property resolved at `time` through the
// existing animation sampler (static values for unanimated properties), and with
// elements whose lifetime excludes `time` - and their descendants - removed. A
// node with no roto data yields an empty value. A negative sample time is
// rejected exactly like every other animation query.
[[nodiscard]] RotoData evaluateRoto(const Document& document, NetworkId network, NodeId node, double time);

// ---------------------------------------------------------------------------
// Authoring helpers. Each appends to `data` and returns the new element
// identity, allocating element and point identities from the data's watermarks
// so a caller never invents one. `appendRotoElement` assigns the element
// identity and every point identity left at zero.

[[nodiscard]] RotoElementId appendRotoElement(RotoData& data, RotoElement element);
[[nodiscard]] RotoElementId appendRotoGroup(RotoData& data, RotoElementId parent, std::string name);
// A closed Bezier or B-spline path through `positions` (a Group has no points).
// Tangents are left at zero, so a pen tool owns its own tangent policy; the
// number of points a closed shape needs is enforced when the value is published,
// not while a draft is assembled.
[[nodiscard]] RotoElementId appendRotoPath(RotoData& data, RotoKind kind, RotoElementId parent, std::string name,
                                           const std::vector<Vector2Value>& positions);
// Closed four-corner rectangle with sharp (zero-tangent) corners.
[[nodiscard]] RotoElementId appendRotoRectangle(RotoData& data, RotoElementId parent, std::string name, double x,
                                                double y, double width, double height);
// Closed four-point ellipse with the circular-arc tangent approximation.
[[nodiscard]] RotoElementId appendRotoEllipse(RotoData& data, RotoElementId parent, std::string name, double centerX,
                                              double centerY, double radiusX, double radiusY);

}  // namespace nemo
