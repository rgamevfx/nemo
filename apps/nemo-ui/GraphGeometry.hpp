#pragma once

#include "GraphScene.hpp"

#include <QPointF>
#include <QRectF>
#include <QVector>
#include <array>
#include <limits>

namespace nemo::ui {

// The single owner of card rectangle, port position, port hit region, route
// polyline and point-to-segment projection. The painter and the picker both
// call these functions; a second implementation of any of them is a defect.
//
// Gesture tolerances remain the issue #100 baseline. The compact card and
// outward arrow geometry are shared by painting, wires and acquisition.
inline constexpr qreal kCardWidth = 112.0;
inline constexpr qreal kCardHeight = 28.0;
inline constexpr qreal kPortArrowLength = 8.0;
inline constexpr qreal kPortArrowHalfWidth = 5.0;
inline constexpr qreal kMaskArrowLength = 6.0;
inline constexpr qreal kMaskArrowHalfWidth = 3.0;
// Screen-space tolerances: every tolerance is expressed in screen pixels and
// scaled by the view transform, so picking feels identical at every zoom.
inline constexpr qreal kPortHitRadius = 14.0;
inline constexpr qreal kPipeHitTolerance = 12.0;
inline constexpr qreal kRerouteHitTolerance = 9.0;
inline constexpr qreal kMoveThreshold = 3.0;
inline constexpr qreal kSnapTolerance = 6.0;
// The half of the card a port can be acquired in. The guarded side is the one
// the port sits on, so an input cannot be grabbed from below its own card.
inline constexpr qreal kPortGuardFraction = 0.45;
inline constexpr qreal kZoomMinimum = 0.2;
inline constexpr qreal kZoomMaximum = 2.5;
inline constexpr qreal kWheelPixelsPerNotch = 53.0;
inline constexpr qreal kWheelZoomExponent = 0.002;
inline constexpr int kZoomSettleMs = 200;
inline constexpr qreal kContentMargin = 16.0;
// Card-relative box of the enter-subnet affordance. The painted chip and the
// region that acquires it are the prototype's two slightly different boxes;
// both are preserved as they were.
inline constexpr qreal kAffordancePaintedLeft = kCardWidth - 24.0;
inline constexpr qreal kAffordancePaintedTop = 4.0;
inline constexpr qreal kAffordancePaintedSize = 20.0;
inline constexpr qreal kAffordanceHitLeft = kCardWidth - 24.0;
inline constexpr qreal kAffordanceHitTop = 3.0;
inline constexpr qreal kAffordanceHitRight = kCardWidth - 3.0;
inline constexpr qreal kAffordanceHitBottom = 24.0;

enum class PortSide { Top, Right, Bottom };

// Outputs are always on the bottom edge; a mask input is on the right edge and
// every other input on the top edge, as the prototype places them.
[[nodiscard]] PortSide portSide(const GraphPortRecord& port, bool output);

// Screen and scene spaces of one panel. Tolerances are screen-space and scene
// distances are converted through the same transform, so the two can never
// disagree.
struct GraphViewTransform {
    QPointF pan;
    qreal scale{1.0};

    [[nodiscard]] QPointF toScreen(QPointF scene) const { return pan + scene * scale; }
    [[nodiscard]] QPointF toScene(QPointF screen) const;
};

[[nodiscard]] qreal clampedZoom(qreal zoom);

// Card rectangle in scene coordinates.
[[nodiscard]] QRectF cardRect(const GraphNodeRecord& node);

// Port centre in scene coordinates, half an arrow outside the card. The
// painted connector, wire anchor and acquisition centre always agree.
[[nodiscard]] QPointF portPosition(const GraphNodeRecord& node, int port, bool output);

// Triangle in scene coordinates, pointing into an input or away from an output.
[[nodiscard]] std::array<QPointF, 3> portArrow(const GraphNodeRecord& node, int port, bool output);

// The painted affordance chip, in scene coordinates relative to the scene
// origin.
[[nodiscard]] QRectF affordanceRect(const GraphNodeRecord& node);

// The region that acquires the enter-subnet affordance, in scene coordinates.
[[nodiscard]] bool withinAffordance(const GraphNodeRecord& node, QPointF scenePoint);

// The half-plane guard for a port of this side, in screen space: the pointer
// must be on the port's own side of its card to acquire it.
[[nodiscard]] bool portGuardSatisfied(PortSide side, QRectF screenCard, QPointF screenPoint);

struct PolylineProjection {
    qreal distance{std::numeric_limits<qreal>::max()};
    int segment{-1};
    QPointF point;
};

// Source port, authored route points, target port, appended into `out`. `out`
// is the caller's storage, so one interaction walks every edge into one buffer
// instead of minting a vector per edge: the pick pass runs on every pointer
// move and the painter runs every frame, and neither may ask the allocator for
// a polyline per edge. Left empty when either endpoint is not a node of this
// scene, which is how terminal bindings stay unpainted and unpiecked.
void routePolyline(const GraphScene& scene, const GraphEdgeRecord& edge, QVector<QPointF>& out);

// Closest point on a polyline, first segment winning a tie.
[[nodiscard]] PolylineProjection projectOnPolyline(const QVector<QPointF>& polyline, QPointF point);

// Bounds of every card and route point, before the content margin.
[[nodiscard]] QRectF contentBounds(const GraphScene& scene);

}  // namespace nemo::ui
