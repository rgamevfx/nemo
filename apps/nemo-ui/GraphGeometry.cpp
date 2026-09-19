#include "GraphGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace nemo::ui {

PortSide portSide(const GraphPortRecord& port, bool output) {
    if (output)
        return PortSide::Bottom;
    return port.kind.compare(QStringLiteral("mask"), Qt::CaseInsensitive) == 0 ? PortSide::Right : PortSide::Top;
}

QPointF GraphViewTransform::toScene(QPointF screen) const {
    // A degenerate scale would make the two spaces disagree; the identity scale
    // keeps the transform invertible.
    const qreal divisor = scale > 0.0 ? scale : 1.0;
    return (screen - pan) / divisor;
}

qreal clampedZoom(qreal zoom) {
    // A zoom that states nothing (non-finite or zero) is the identity before the
    // clamp, so a wheel delta of none can never collapse the view.
    const qreal value = std::isfinite(zoom) && zoom != 0.0 ? zoom : 1.0;
    return std::clamp(value, kZoomMinimum, kZoomMaximum);
}

QRectF cardRect(const GraphNodeRecord& node) {
    return QRectF(node.position, QSizeF(kCardWidth, kCardHeight));
}

QPointF portPosition(const GraphNodeRecord& node, int port, bool output) {
    const auto& ports = output ? node.outputs : node.inputs;
    const qsizetype size = ports.size();
    const qsizetype last = size > 0 ? size - 1 : 0;
    const qsizetype index = std::clamp(static_cast<qsizetype>(port), qsizetype{0}, last);
    // An empty port list has no declared kind to read; the unknown-kind side is
    // the output's bottom or the input's top.
    const PortSide side = index < size ? portSide(ports.at(index), output) : portSide(GraphPortRecord{}, output);

    // Every port of this side is laid out in list order, and the whole side's
    // count is the denominator, so a port that moved sides moved the others too.
    qsizetype sideIndex = 0;
    qsizetype sideCount = 0;
    for (qsizetype candidate = 0; candidate < size; ++candidate) {
        if (portSide(ports.at(candidate), output) != side)
            continue;
        if (candidate == index)
            sideIndex = sideCount;
        ++sideCount;
    }
    const qreal fraction = sideCount <= 1 ? 0.5 : static_cast<qreal>(sideIndex + 1) / static_cast<qreal>(sideCount + 1);

    const QRectF card = cardRect(node);
    if (side == PortSide::Right)
        return {card.right(), card.top() + card.height() * fraction};
    if (side == PortSide::Bottom)
        return {card.left() + card.width() * fraction, card.bottom()};
    return {card.left() + card.width() * fraction, card.top()};
}

QRectF affordanceRect(const GraphNodeRecord& node) {
    return QRectF(node.position + QPointF(kAffordancePaintedLeft, kAffordancePaintedTop),
                  QSizeF(kAffordancePaintedSize, kAffordancePaintedSize));
}

bool withinAffordance(const GraphNodeRecord& node, QPointF scenePoint) {
    const QRectF card = cardRect(node);
    const qreal x = scenePoint.x() - card.left();
    const qreal y = scenePoint.y() - card.top();
    return x >= kAffordanceHitLeft && x <= kAffordanceHitRight && y >= kAffordanceHitTop && y <= kAffordanceHitBottom;
}

bool portGuardSatisfied(PortSide side, QRectF screenCard, QPointF screenPoint) {
    if (side == PortSide::Right)
        return screenPoint.x() >= screenCard.left() + screenCard.width() * (1.0 - kPortGuardFraction);
    if (side == PortSide::Bottom)
        return screenPoint.y() >= screenCard.top() + screenCard.height() * (1.0 - kPortGuardFraction);
    return screenPoint.y() <= screenCard.top() + screenCard.height() * kPortGuardFraction;
}

void routePolyline(const GraphScene& scene, const GraphEdgeRecord& edge, QVector<QPointF>& out) {
    out.clear();
    const auto* source = scene.node(edge.from.node);
    const auto* target = scene.node(edge.to.node);
    if (source == nullptr || target == nullptr)
        return;
    out.reserve(edge.route.size() + 2);
    out.push_back(portPosition(*source, edge.from.port, true));
    out.append(edge.route);
    out.push_back(portPosition(*target, edge.to.port, false));
}

PolylineProjection projectOnPolyline(const QVector<QPointF>& polyline, QPointF point) {
    PolylineProjection closest;
    if (polyline.size() < 2)
        return closest;
    for (qsizetype index = 0; index + 1 < polyline.size(); ++index) {
        const QPointF start = polyline.at(index);
        const QPointF span = polyline.at(index + 1) - start;
        const qreal lengthSquared = span.x() * span.x() + span.y() * span.y();
        const QPointF offset = point - start;
        // A zero-length segment has no interior: it resolves to its first point,
        // which is also what clamping the parameter produces.
        const qreal parameter =
            lengthSquared > 0.0 ? std::clamp((offset.x() * span.x() + offset.y() * span.y()) / lengthSquared, 0.0, 1.0)
                                : 0.0;
        const QPointF candidate = start + span * parameter;
        const QPointF delta = point - candidate;
        const qreal distance = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
        if (distance < closest.distance) {
            closest.distance = distance;
            closest.segment = static_cast<int>(index);
            closest.point = candidate;
        }
    }
    return closest;
}

QRectF contentBounds(const GraphScene& scene) {
    QRectF bounds;
    bool started = false;
    const auto include = [&bounds, &started](QPointF point) {
        if (!started) {
            bounds = QRectF(point.x(), point.y(), 0.0, 0.0);
            started = true;
            return;
        }
        bounds.setCoords(std::min(bounds.left(), point.x()), std::min(bounds.top(), point.y()),
                         std::max(bounds.right(), point.x()), std::max(bounds.bottom(), point.y()));
    };
    for (const auto& node : scene.nodes) {
        const QRectF card = cardRect(node);
        include(card.topLeft());
        include(card.bottomRight());
    }
    for (const auto& edge : scene.edges) {
        for (const auto& point : edge.route)
            include(point);
    }
    return bounds;
}

}  // namespace nemo::ui
