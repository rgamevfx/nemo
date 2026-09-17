#pragma once

// Roto's shared geometry preparation (issue #93, stories 58-63, 65, 67).
//
// This header owns the one thing the two executors must agree on exactly: what
// the authored Roto data means as WORLD GEOMETRY. It turns a resolved
// `RotoData` snapshot plus the node's physical aspect into a flat, ordered
// program — groups opened and closed, leaf contours tessellated — stated in
// full-resolution image pixels (x right, y DOWN), which both the CPU adapter
// and the native preparation then rasterize INDEPENDENTLY (ADR-0004). Nothing
// here is pixel math: there is no coverage, no feather ramp and no blending
// arithmetic, so agreement between the two executors stays evidence about the
// ramp and the compositor rather than a shared mistake.
//
// Frozen geometry policy (the reference documents the controls, not these
// details; the ones it leaves open are Nemo's, recorded here):
//
//   * One element's transform, applied to a local point p in LOGICAL
//     (stored-raster) coordinates, is scale -> rotate about pivot -> translate,
//     exactly the Transform node's published contract
//     (src/nemo/nodes/transform/Contribution.cpp):
//
//         d      = (p - pivot) * (scale.x, scale.y)          [logical scale]
//         (X, Y) = (d.x * par, d.y)                          [to physical]
//         (X', Y') = (c*X - s*Y, s*X + c*Y)                  [rotate by rotation]
//         world  = pivot + (X' / par, Y') + translation      [back, then translate]
//
//     `par` is the OWNING network's saved format pixel aspect, so authored
//     handle geometry and the rasterized contour agree under the same viewer
//     mapping. A node without a network scope (a direct adapter call) passes the
//     described output aspect instead. `rotation` is in DEGREES and is positive
//     clockwise as displayed in the stored raster, which is what that matrix
//     means in x-right/y-down coordinates.
//   * Nesting: an element's own transform is applied first and each ancestor's
//     afterwards, root last — a child moves with its parent. Composing the
//     per-element affine down the tree makes that one 2x3 matrix per element.
//   * Feather is a SIGNED distance in full-resolution pixels, positive outward
//     (away from the interior), interpolated along the tessellated contour.
//     Polygon winding never changes its sign. The world feather of a vertex of
//     leaf L is
//
//         acc(L) * (point.feather + L.ownFeather)  +  sum over ancestor groups G of acc(G) * G.ownFeather
//
//     where acc(e) is the product of `sqrt(|scale.x * scale.y|)` over e and all
//     its ancestors (the uniform-scale approximation a rotated, non-uniformly
//     scaled shape's soft edge needs to stay a scalar width), and an element's
//     ownFeather is its `feather` when `featherEnabled`, else 0. A group's
//     feather therefore biases every contour beneath it.
//   * Contours are CLOSED. A Bezier segment from p_i to p_{i+1} is the cubic
//     with control points p_i + outTangent_i and p_{i+1} + inTangent_{i+1};
//     zero tangents are exactly a straight segment. A B-spline is the periodic
//     uniform cubic B-spline through the control points, and `tension` blends it
//     toward the control polygon (0 = smooth B-spline, 1 = the polygon itself);
//     tangents are ignored.
//   * Tessellation checks quarter-, mid- and three-quarter points against the
//     chord, so an inflection cannot hide at the midpoint. Tolerance accounts
//     for the accumulated transform and stays in full-resolution pixels.
//     Vertex feather values use the same basis as the position (a Bezier
//     segment's feather is linear in its parameter, a B-spline's follows its own
//     basis). Exceeding `kMaxRotoVerticesPerContour`, `kMaxRotoItemsPerSample`
//     or `kMaxRotoGroupDepth` is a node-identifying FAILURE, never a silently
//     truncated contour — an unrepresentable shape is reported where it was
//     authored.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nemo/core/document/Roto.hpp"
#include "nemo/core/evaluation/Params.hpp"

namespace nemo::nodes {

// Frozen work bounds (issue #93). They exist so a dense scene is refused with a
// node-identifying message instead of consuming unbounded time or memory; they
// are generous enough that ordinary authored shapes never meet them.
inline constexpr std::size_t kMaxRotoVerticesPerContour = 4096;
// Largest tessellated vertex pool ONE sample's whole hierarchy may hold. Exceeding
// it is refused like the per-contour cap, so a dense scene is reported instead of
// packed.
inline constexpr std::size_t kMaxRotoVerticesPerSample = 1u << 16;
// Largest retained geometry program, in `uint` words: the header, every sample's
// table entry, item records and vertices together. The executor still bounds the
// buffer by the device's storage-buffer range; this is the node's own, earlier,
// explicit refusal.
inline constexpr std::size_t kMaxRotoGeometryWords = 1u << 22;
inline constexpr std::size_t kMaxRotoItemsPerSample = 1024;
inline constexpr std::size_t kMaxRotoGroupDepth = 32;
// Largest coverage scratch the CPU reference holds at once: one coverage row per
// shape that reaches the current scanline, times the raster width. A request that
// would need more is refused, so a dense scene is reported instead of consuming
// unbounded memory.
inline constexpr std::size_t kMaxRotoRasterScratchSamples = 1u << 22;
// Flatness tolerance for adaptive subdivision, in full-resolution pixels.
inline constexpr double kRotoFlatnessTolerance = 0.25;
// Hard bisection depth for one sub-interval (2^16 leaves), so a pathological
// curve is bounded by construction; the vertex cap below is what reports a
// contour that really is too dense.
inline constexpr int kRotoMaxSubdivisionDepth = 16;

// One tessellated contour vertex: world position and signed world feather, both
// in full-resolution pixels.
struct RotoVertex {
    float x{0.0F};
    float y{0.0F};
    float feather{0.0F};
};

// One step of the flattened evaluation program. `Shape` carries a run of
// vertices; `GroupBegin`/`GroupEnd` bracket a group's children, whose combined
// matte the group then inverts, scales by its opacity and blends with its
// siblings.
struct RotoItem {
    enum class Kind : std::uint32_t { Shape = 0, GroupBegin = 1, GroupEnd = 2 };

    Kind kind{Kind::Shape};
    RotoBlend blend{RotoBlend::Combine};
    float opacity{1.0F};
    bool inverted{false};
    // Shape only.
    std::uint32_t firstVertex{0};
    std::uint32_t vertexCount{0};
    RotoFeatherProfile profile{RotoFeatherProfile::Linear};
    float falloff{1.0F};
    // Shape only: the world bounding box, grown by the vertices' own feather.
    float bounds[4]{0.0F, 0.0F, 0.0F, 0.0F};
};

// The flattened program plus the vertex pool it indexes. `items` is in
// evaluation order: storage order among siblings, a group's children between its
// own markers, at every level. Invisible elements (and their whole subtree) are
// absent, and elements outside their lifetime were already removed by
// `evaluateRoto`.
struct RotoGeometry {
    std::vector<RotoVertex> vertices;
    std::vector<RotoItem> items;

    [[nodiscard]] bool empty() const { return items.empty(); }
};

// One element's own 2x3 affine, world = R*p + T (see the policy above).
struct RotoAffine {
    float r[2][2]{{1.0F, 0.0F}, {0.0F, 1.0F}};
    float t[2]{0.0F, 0.0F};
};

[[nodiscard]] inline bool rotoFinite(float value) {
    return std::isfinite(value);
}

// The pixel aspect every authored shape is measured through: the owning
// network's saved format, which is also what the authoring overlay maps handles
// in. A node invoked without a network scope falls back to the described output
// aspect. A non-finite or non-positive aspect is refused rather than becoming a
// silent identity.
[[nodiscard]] inline float rotoPhysicalAspect(const NodeInstance& node, const ImageFormat* owningFormat,
                                              float describedAspect) {
    const float aspect = owningFormat != nullptr ? owningFormat->pixelAspect : describedAspect;
    if (!rotoFinite(aspect) || !(aspect > 0.0F)) {
        failNode(node, "cannot place Roto geometry: the physical pixel aspect is " + std::to_string(aspect) +
                           " (a finite, positive aspect is required)");
    }
    return aspect;
}

[[nodiscard]] inline float rotoVector2(const Vector2Value& value, std::size_t index) {
    return value.value[index];
}

// sqrt(|scale.x * scale.y|): the uniform scale factor a signed feather width
// travels with (see the policy above).
[[nodiscard]] inline float rotoUniformScale(const RotoElement& element) {
    const float x = rotoVector2(element.scale, 0);
    const float y = rotoVector2(element.scale, 1);
    return static_cast<float>(std::sqrt(std::abs(static_cast<double>(x) * y)));
}

// One element's own affine in the owning network's canvas space.
[[nodiscard]] inline RotoAffine rotoElementAffine(const RotoElement& element, float pixelAspect) {
    const float sx = rotoVector2(element.scale, 0);
    const float sy = rotoVector2(element.scale, 1);
    const float px = rotoVector2(element.pivot, 0);
    const float py = rotoVector2(element.pivot, 1);
    const float tx = rotoVector2(element.translation, 0);
    const float ty = rotoVector2(element.translation, 1);
    // Radians and trigonometry in double, rounded once to float: the same
    // residual-free conversion the Transform node's own forward map publishes.
    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    const double radians = static_cast<double>(element.rotation) * kDegreesToRadians;
    const float cosine = static_cast<float>(std::cos(radians));
    const float sine = static_cast<float>(std::sin(radians));

    RotoAffine affine;
    affine.r[0][0] = cosine * sx;
    affine.r[0][1] = -sine * sy / pixelAspect;
    affine.r[1][0] = sine * pixelAspect * sx;
    affine.r[1][1] = cosine * sy;
    // The pivot is the centre of rotation, not a translation: world = R*(p -
    // pivot) + pivot + translation.
    affine.t[0] = px + tx - (affine.r[0][0] * px + affine.r[0][1] * py);
    affine.t[1] = py + ty - (affine.r[1][0] * px + affine.r[1][1] * py);
    return affine;
}

// parent then child: an ancestor's transform is applied to its descendant's
// result (root last).
[[nodiscard]] inline RotoAffine rotoComposeAffine(const RotoAffine& parent, const RotoAffine& child) {
    RotoAffine composed;
    composed.r[0][0] = parent.r[0][0] * child.r[0][0] + parent.r[0][1] * child.r[1][0];
    composed.r[0][1] = parent.r[0][0] * child.r[0][1] + parent.r[0][1] * child.r[1][1];
    composed.r[1][0] = parent.r[1][0] * child.r[0][0] + parent.r[1][1] * child.r[1][0];
    composed.r[1][1] = parent.r[1][0] * child.r[0][1] + parent.r[1][1] * child.r[1][1];
    composed.t[0] = parent.r[0][0] * child.t[0] + parent.r[0][1] * child.t[1] + parent.t[0];
    composed.t[1] = parent.r[1][0] * child.t[0] + parent.r[1][1] * child.t[1] + parent.t[1];
    return composed;
}

[[nodiscard]] inline RotoVertex rotoApplyAffine(const RotoAffine& affine, float x, float y) {
    return RotoVertex{affine.r[0][0] * x + affine.r[0][1] * y + affine.t[0],
                      affine.r[1][0] * x + affine.r[1][1] * y + affine.t[1], 0.0F};
}

// ---------------------------------------------------------------------------
// Contour tessellation (local coordinates; the caller transforms).
// ---------------------------------------------------------------------------

namespace rotoDetail {

// A parametric point with its feather, in the element's local space.
struct CurveSample {
    double x{0.0};
    double y{0.0};
    double feather{0.0};
};

[[nodiscard]] inline CurveSample bezierPoint(const RotoPoint& from, const RotoPoint& to, double u) {
    const double cx0 = rotoVector2(from.position, 0) + rotoVector2(from.outTangent, 0);
    const double cy0 = rotoVector2(from.position, 1) + rotoVector2(from.outTangent, 1);
    const double cx1 = rotoVector2(to.position, 0) + rotoVector2(to.inTangent, 0);
    const double cy1 = rotoVector2(to.position, 1) + rotoVector2(to.inTangent, 1);
    const double px0 = rotoVector2(from.position, 0);
    const double py0 = rotoVector2(from.position, 1);
    const double px1 = rotoVector2(to.position, 0);
    const double py1 = rotoVector2(to.position, 1);
    const double v = 1.0 - u;
    const double a = v * v * v;
    const double b = 3.0 * v * v * u;
    const double c = 3.0 * v * u * u;
    const double d = u * u * u;
    CurveSample sample;
    sample.x = a * px0 + b * cx0 + c * cx1 + d * px1;
    sample.y = a * py0 + b * cy0 + c * cy1 + d * py1;
    // The feather interpolates along the segment exactly as the chord does, so a
    // straight run of equal feathers keeps a uniform width.
    sample.feather = (1.0 - u) * from.feather + u * to.feather;
    return sample;
}

// Periodic uniform cubic B-spline basis for control points p_{j-1}..p_{j+2},
// evaluated at u in [0,1] over the segment p_j -> p_{j+1}. Tension is per point,
// so the segment's tension is the linear blend of its two endpoints' values and
// blends the curve toward that control polygon (0 = smooth, 1 = the polygon).
[[nodiscard]] inline CurveSample bsplinePoint(const RotoElement& element, std::size_t count, std::size_t index,
                                              double u) {
    const auto point = [&](std::size_t offset) -> const RotoPoint& {
        // Periodic wrap: a closed contour has no end control point.
        return element.points[(index + count + offset) % count];
    };
    const RotoPoint& previous = point(count - 1);
    const RotoPoint& current = point(0);
    const RotoPoint& next = point(1);
    const RotoPoint& following = point(2);
    const double tension = std::clamp((1.0 - u) * current.tension + u * next.tension, 0.0, 1.0);
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double basis[4] = {
        (-u3 + 3.0 * u2 - 3.0 * u + 1.0) / 6.0,
        (3.0 * u3 - 6.0 * u2 + 4.0) / 6.0,
        (-3.0 * u3 + 3.0 * u2 + 3.0 * u + 1.0) / 6.0,
        u3 / 6.0,
    };
    const RotoPoint* controls[4] = {&previous, &current, &next, &following};
    CurveSample sample;
    for (std::size_t control = 0; control < 4; ++control) {
        sample.x += basis[control] * rotoVector2(controls[control]->position, 0);
        sample.y += basis[control] * rotoVector2(controls[control]->position, 1);
        sample.feather += basis[control] * controls[control]->feather;
    }
    const double chordFeather = (1.0 - u) * current.feather + u * next.feather;
    const double px = rotoVector2(current.position, 0);
    const double py = rotoVector2(current.position, 1);
    sample.x = (1.0 - tension) * sample.x + tension * ((1.0 - u) * px + u * rotoVector2(next.position, 0));
    sample.y = (1.0 - tension) * sample.y + tension * ((1.0 - u) * py + u * rotoVector2(next.position, 1));
    sample.feather = (1.0 - tension) * sample.feather + tension * chordFeather;
    return sample;
}

// Quarter samples expose inflections that a midpoint-only test misses.
template <typename Evaluate>
void emitSegment(const NodeInstance& node, Evaluate&& evaluate, double u0, const CurveSample& start, double u1,
                 const CurveSample& end, double tolerance, int depth, std::vector<RotoVertex>& out) {
    const double midpoint = 0.5 * (u0 + u1);
    const CurveSample middle = evaluate(midpoint);
    double deviation = 0.0;
    for (const double fraction : {0.25, 0.5, 0.75}) {
        const auto sample = fraction == 0.5 ? middle : evaluate(u0 + fraction * (u1 - u0));
        const double error = std::hypot(sample.x - (start.x + fraction * (end.x - start.x)),
                                        sample.y - (start.y + fraction * (end.y - start.y)));
        if (!std::isfinite(error))
            failNode(node, "Roto contour cannot be represented in image coordinates");
        deviation = std::max(deviation, error);
    }
    if (deviation <= tolerance) {
        if (out.size() >= kMaxRotoVerticesPerContour)
            failNode(node,
                     "Roto contour exceeds " + std::to_string(kMaxRotoVerticesPerContour) + " tessellated vertices");
        out.push_back(
            RotoVertex{static_cast<float>(start.x), static_cast<float>(start.y), static_cast<float>(start.feather)});
        return;
    }
    if (depth >= kRotoMaxSubdivisionDepth)
        failNode(node, "Roto contour exceeds the subdivision limit at the required image accuracy");
    emitSegment(node, evaluate, u0, start, midpoint, middle, tolerance, depth + 1, out);
    emitSegment(node, evaluate, midpoint, middle, u1, end, tolerance, depth + 1, out);
}

}  // namespace rotoDetail

// Tessellate one closed Bezier/B-spline contour into local vertices with local
// feather values (the element's own feather is added by the caller, which knows
// the accumulated scale). Throws a node-identifying failure when the contour
// needs more than `kMaxRotoVerticesPerContour` vertices.
inline void tessellateRotoContour(const NodeInstance& node, const RotoElement& element, std::vector<RotoVertex>& out,
                                  double tolerance) {
    const std::size_t count = element.points.size();
    if (count < 2) {
        return;  // no interior: a shape needs at least two points to enclose area
    }
    const bool spline = element.kind == RotoKind::BSpline;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t next = (index + 1) % count;
        const auto evaluate = [&](double u) {
            if (spline) {
                return rotoDetail::bsplinePoint(element, count, index, u);
            }
            return rotoDetail::bezierPoint(element.points[index], element.points[next], u);
        };
        const rotoDetail::CurveSample from = evaluate(0.0);
        const rotoDetail::CurveSample to = evaluate(1.0);
        rotoDetail::emitSegment(node, evaluate, 0.0, from, 1.0, to, tolerance, 0, out);
    }
}

// ---------------------------------------------------------------------------
// Hierarchy flattening.
// ---------------------------------------------------------------------------

// Build the evaluation program for one resolved snapshot. `elements` are walked
// in storage order, which is sibling order; a group's children follow it.
[[nodiscard]] inline RotoGeometry buildRotoGeometry(const NodeInstance& node, const RotoData& data, float pixelAspect) {
    RotoGeometry geometry;
    const std::size_t count = data.elements.size();
    if (count == 0) {
        return geometry;
    }
    // Root-level order is storage order; a child belongs to the first element
    // that names it as its parent. The index is built once so a deep hierarchy
    // stays linear in the element count.
    std::map<RotoElementId, std::vector<std::size_t>> indexByParent;
    for (std::size_t index = 0; index < count; ++index) {
        indexByParent[data.elements[index].parent].push_back(index);
    }
    const auto childrenOf = [&](RotoElementId parent) -> const std::vector<std::size_t>& {
        static const std::vector<std::size_t> none;
        const auto found = indexByParent.find(parent);
        return found == indexByParent.end() ? none : found->second;
    };

    std::vector<RotoVertex> local;
    std::size_t depth = 0;
    // Recursive descent: `affine` is the element's accumulated world transform,
    // `scale` the accumulated uniform scale, `bias` the accumulated ancestor
    // group feather in world pixels.
    const auto visit = [&](auto&& self, std::size_t index, const RotoAffine& parentAffine, float parentScale,
                           float bias) -> void {
        const RotoElement& element = data.elements[index];
        if (!element.visible) {
            return;  // an invisible group hides its whole subtree
        }
        if (geometry.items.size() >= kMaxRotoItemsPerSample)
            failNode(node, "Roto hierarchy exceeds " + std::to_string(kMaxRotoItemsPerSample) + " evaluation steps");
        const RotoAffine own = rotoElementAffine(element, pixelAspect);
        const RotoAffine affine = rotoComposeAffine(parentAffine, own);
        const float scale = parentScale * rotoUniformScale(element);
        const float ownFeather = element.featherEnabled ? static_cast<float>(element.feather) : 0.0F;

        if (element.kind == RotoKind::Group) {
            if (depth + 1 > kMaxRotoGroupDepth) {
                failNode(node, "Roto group '" + element.name + "' (id " + std::to_string(element.id) +
                                   ") nests deeper than " + std::to_string(kMaxRotoGroupDepth) +
                                   " levels; the hierarchy is refused rather than flattened");
            }
            RotoItem item;
            item.kind = RotoItem::Kind::GroupBegin;
            item.blend = element.blend;
            item.opacity = static_cast<float>(element.opacity);
            item.inverted = element.inverted;
            geometry.items.push_back(item);
            ++depth;
            const float childScale = scale;
            const float childBias = bias + scale * ownFeather;
            for (const std::size_t child : childrenOf(element.id)) {
                self(self, child, affine, childScale, childBias);
            }
            --depth;
            geometry.items.push_back(RotoItem{.kind = RotoItem::Kind::GroupEnd});
            return;
        }

        local.clear();
        const double normOne = std::max(std::abs(static_cast<double>(affine.r[0][0])) + std::abs(affine.r[1][0]),
                                        std::abs(static_cast<double>(affine.r[0][1])) + std::abs(affine.r[1][1]));
        const double normInfinity = std::max(std::abs(static_cast<double>(affine.r[0][0])) + std::abs(affine.r[0][1]),
                                             std::abs(static_cast<double>(affine.r[1][0])) + std::abs(affine.r[1][1]));
        const double stretch = std::sqrt(normOne * normInfinity);
        if (!std::isfinite(stretch))
            failNode(node, "Roto transform cannot be represented in image coordinates");
        tessellateRotoContour(node, element, local, kRotoFlatnessTolerance / (stretch > 0.0 ? stretch : 1.0));
        if (local.size() > kMaxRotoVerticesPerSample - geometry.vertices.size())
            failNode(node,
                     "Roto hierarchy exceeds " + std::to_string(kMaxRotoVerticesPerSample) + " tessellated vertices");
        if (local.size() < 3) {
            return;
        }
        RotoItem item;
        item.kind = RotoItem::Kind::Shape;
        item.blend = element.blend;
        item.opacity = static_cast<float>(element.opacity);
        item.inverted = element.inverted;
        item.firstVertex = static_cast<std::uint32_t>(geometry.vertices.size());
        item.vertexCount = static_cast<std::uint32_t>(local.size());
        item.profile = element.featherProfile;
        item.falloff = static_cast<float>(element.featherFalloff);
        float minX = 0.0F;
        float minY = 0.0F;
        float maxX = 0.0F;
        float maxY = 0.0F;
        for (std::size_t vertex = 0; vertex < local.size(); ++vertex) {
            const float pointFeather = element.featherEnabled ? local[vertex].feather : 0.0F;
            RotoVertex world = rotoApplyAffine(affine, local[vertex].x, local[vertex].y);
            // The point's own feather travels with the shape's uniform scale; the
            // element's own feather with the element's and the ancestors' scale.
            world.feather = scale * (pointFeather + ownFeather) + bias;
            geometry.vertices.push_back(world);
            const float reach = std::fabs(world.feather);
            if (vertex == 0) {
                minX = world.x - reach;
                minY = world.y - reach;
                maxX = world.x + reach;
                maxY = world.y + reach;
            } else {
                minX = std::min(minX, world.x - reach);
                minY = std::min(minY, world.y - reach);
                maxX = std::max(maxX, world.x + reach);
                maxY = std::max(maxY, world.y + reach);
            }
        }
        item.bounds[0] = minX;
        item.bounds[1] = minY;
        item.bounds[2] = maxX;
        item.bounds[3] = maxY;
        geometry.items.push_back(item);
    };

    for (const std::size_t index : childrenOf(0)) {
        visit(visit, index, RotoAffine{}, 1.0F, 0.0F);
    }
    if (geometry.items.size() > kMaxRotoItemsPerSample) {
        failNode(node, "Roto hierarchy needs more than " + std::to_string(kMaxRotoItemsPerSample) +
                           " evaluation steps; it is refused rather than evaluated partially");
    }
    if (geometry.vertices.size() > kMaxRotoVerticesPerSample) {
        failNode(node, "the Roto hierarchy tessellates to more than " + std::to_string(kMaxRotoVerticesPerSample) +
                           " vertices at once; reduce the contour detail (the geometry is refused, not truncated)");
    }
    return geometry;
}

// World bounding box of everything a program contributes, or false when it
// contributes nothing at all (every shape already carries its own feather reach,
// but the box is taken over the vertices so an empty contour adds nothing).
[[nodiscard]] inline bool rotoGeometryBounds(const RotoGeometry& geometry, float out[4]) {
    bool any = false;
    float minX = 0.0F;
    float minY = 0.0F;
    float maxX = 0.0F;
    float maxY = 0.0F;
    for (const RotoItem& item : geometry.items) {
        if (item.kind != RotoItem::Kind::Shape) {
            continue;
        }
        if (!any) {
            minX = item.bounds[0];
            minY = item.bounds[1];
            maxX = item.bounds[2];
            maxY = item.bounds[3];
            any = true;
        } else {
            minX = std::min(minX, item.bounds[0]);
            minY = std::min(minY, item.bounds[1]);
            maxX = std::max(maxX, item.bounds[2]);
            maxY = std::max(maxY, item.bounds[3]);
        }
    }
    if (any) {
        out[0] = minX;
        out[1] = minY;
        out[2] = maxX;
        out[3] = maxY;
    }
    return any;
}

}  // namespace nemo::nodes
