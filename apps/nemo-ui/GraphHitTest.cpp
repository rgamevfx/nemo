#include "GraphHitTest.hpp"

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <cmath>

namespace nemo::ui {
namespace {

// The reference primary() hands back when the pointer acquires nothing. It has
// static storage duration, so the reference never dangles and no caller can
// observe a locally constructed hit.
const GraphHit kNullHit{};

// The port record an endpoint guard reads when an edge names a port the node
// does not declare. The prototype indexed the exported port array directly, so
// an absent record behaved as an empty one: no kind, which puts an input on the
// top edge and an output on the bottom edge.
const GraphPortRecord& portRecordOrFallback(const QVector<GraphPortRecord>& ports, int port) {
    static const GraphPortRecord kFallback{};
    return port >= 0 && port < ports.size() ? ports.at(port) : kFallback;
}

[[nodiscard]] qreal screenDistance(QPointF a, QPointF b) {
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

// One direction's running best. A port is addressed by its position in the
// declared array, which is the identity portPosition() takes, the identity the
// pick reports and the identity the snapshot publishes as `index`.
void considerPorts(const GraphNodeRecord& node, const QVector<GraphPortRecord>& ports, bool output,
                   const QRectF& screenCard, QPointF screen, const GraphViewTransform& view, qreal& bestDistance,
                   GraphHit& best) {
    for (int port = 0; port < ports.size(); ++port) {
        // A port on the far half of its own card does not compete at all.
        if (!portGuardSatisfied(portSide(ports.at(port), output), screenCard, screen))
            continue;
        const QPointF scenePoint = portPosition(node, port, output);
        const QPointF screenPoint = view.toScreen(scenePoint);
        const qreal distance = screenDistance(screen, screenPoint);
        // A later candidate wins a tie, which is the order the cards paint in.
        if (distance <= bestDistance) {
            bestDistance = distance;
            best.kind = GraphHitKind::Port;
            best.node = node.id;
            best.port = port;
            best.output = output;
            best.screen = screenPoint;
            best.scene = scenePoint;
            best.distance = distance;
        }
    }
}

// A node together with the screen card the node walk cached for it. The edge
// walk needs both the declared ports and the very rectangle the card test used,
// and resolving them together keeps the pass to one identity lookup.
struct ScreenNode {
    const GraphNodeRecord* node{};
    const QRectF* card{};
};

[[nodiscard]] ScreenNode screenNodeAt(const GraphScene& scene, const QVector<QRectF>& screenCards,
                                      const QString& id) {
    if (id.isEmpty())
        return {};
    const auto found = scene.nodeIndex.constFind(id);
    if (found == scene.nodeIndex.constEnd())
        return {};
    const qsizetype index = *found;
    if (index < 0 || index >= screenCards.size())
        return {};
    return {&scene.nodes.at(index), &screenCards.at(index)};
}

}  // namespace

const GraphHit& GraphHitResult::primary() const {
    if (!affordance.isNull())
        return affordance;
    if (!port.isNull())
        return port;
    if (!endpoint.isNull())
        return endpoint;
    if (!reroute.isNull())
        return reroute;
    if (!card.isNull())
        return card;
    if (!pipe.isNull())
        return pipe;
    return kNullHit;
}

GraphHitResult hitTestGraph(const GraphScene& scene, const GraphViewTransform& view, QPointF screen) {
    GraphHitResult result;

    // One forward pass over the nodes resolves the cards, both directions of
    // ports and, from the topmost card, the enter-subnet affordance. The screen
    // cards are kept because the edge walk guards an endpoint against the card
    // of the node it belongs to.
    QVector<QRectF> screenCards;
    screenCards.reserve(scene.nodes.size());
    GraphHit outputBest;
    GraphHit inputBest;
    qreal outputDistance = kPortHitRadius;
    qreal inputDistance = kPortHitRadius;
    const GraphNodeRecord* topCard = nullptr;

    for (const GraphNodeRecord& node : scene.nodes) {
        const QRectF card = cardRect(node);
        const QRectF screenCard =
            QRectF(view.toScreen(card.topLeft()), view.toScreen(card.bottomRight())).normalized();
        screenCards.append(screenCard);

        // Inclusive bounds, and a later card replaces the earlier one, so the
        // card the pass ends on is the topmost one.
        if (screenCard.left() <= screen.x() && screen.x() <= screenCard.right() && screenCard.top() <= screen.y() &&
            screen.y() <= screenCard.bottom()) {
            result.card.kind = GraphHitKind::Card;
            result.card.node = node.id;
            result.card.screen = screen;
            result.card.scene = view.toScene(screen);
            topCard = &node;
        }

        considerPorts(node, node.outputs, true, screenCard, screen, view, outputDistance, outputBest);
        considerPorts(node, node.inputs, false, screenCard, screen, view, inputDistance, inputBest);
    }

    // Each direction's own winner is published even when the combined pick goes
    // the other way, so a direction-specific consumer reads the field it needs.
    result.portOutput = outputBest;
    result.portInput = inputBest;

    // A port wins over its own card, and of the two directions the output wins
    // a tie: a direction with no candidate inside the tolerance loses.
    if (!outputBest.isNull() && (inputBest.isNull() || outputDistance <= inputDistance))
        result.port = outputBest;
    else if (!inputBest.isNull())
        result.port = inputBest;

    // The affordance is derived from the topmost card alone, so a card lying
    // above a subnet wins the pick and with it the affordance.
    if (topCard != nullptr && topCard->hasChildScope()) {
        const QPointF scenePoint = view.toScene(screen);
        if (withinAffordance(*topCard, scenePoint)) {
            result.affordance.kind = GraphHitKind::Affordance;
            result.affordance.node = topCard->id;
            result.affordance.screen = screen;
            result.affordance.scene = scenePoint;
        }
    }

    // One forward pass over the edges resolves connection endpoints, reroute
    // dots and pipe bodies. The projected polyline is built once per edge and
    // reused by every class, so no candidate allocates.
    GraphHit endpointBest;
    GraphHit rerouteBest;
    GraphHit pipeBest;
    qreal endpointDistance = kPortHitRadius;
    qreal rerouteDistance = kRerouteHitTolerance;
    qreal pipeDistance = kPipeHitTolerance;
    QVector<QPointF> screenPolyline;

    for (const GraphEdgeRecord& edge : scene.edges) {
        const QVector<QPointF> polyline = routePolyline(scene, edge);
        if (polyline.size() < 2)
            continue;
        screenPolyline.clear();
        screenPolyline.reserve(polyline.size());
        for (const QPointF& point : polyline)
            screenPolyline.append(view.toScreen(point));

        // Source first, then target: within one edge the target wins a tie, and
        // a later edge wins a tie against both.
        const ScreenNode source = screenNodeAt(scene, screenCards, edge.from.node);
        if (source.node != nullptr) {
            const qreal distance = screenDistance(screen, screenPolyline.first());
            const GraphPortRecord& port = portRecordOrFallback(source.node->outputs, edge.from.port);
            if (distance <= endpointDistance && portGuardSatisfied(portSide(port, true), *source.card, screen)) {
                endpointDistance = distance;
                endpointBest.kind = GraphHitKind::Endpoint;
                endpointBest.node = edge.from.node;
                endpointBest.edge = edge.id;
                endpointBest.port = edge.from.port;
                endpointBest.output = true;
                endpointBest.screen = screenPolyline.first();
                endpointBest.scene = polyline.first();
                endpointBest.distance = distance;
            }
        }

        const ScreenNode target = screenNodeAt(scene, screenCards, edge.to.node);
        if (target.node != nullptr) {
            const qreal distance = screenDistance(screen, screenPolyline.last());
            const GraphPortRecord& port = portRecordOrFallback(target.node->inputs, edge.to.port);
            if (distance <= endpointDistance && portGuardSatisfied(portSide(port, false), *target.card, screen)) {
                endpointDistance = distance;
                endpointBest.kind = GraphHitKind::Endpoint;
                endpointBest.node = edge.to.node;
                endpointBest.edge = edge.id;
                endpointBest.port = edge.to.port;
                endpointBest.output = false;
                endpointBest.screen = screenPolyline.last();
                endpointBest.scene = polyline.last();
                endpointBest.distance = distance;
            }
        }

        // Reroute dots are the interior polyline points, addressing the
        // authored route array rather than the polyline they produced.
        for (qsizetype point = 1; point + 1 < screenPolyline.size(); ++point) {
            const qreal distance = screenDistance(screen, screenPolyline.at(point));
            if (distance <= rerouteDistance) {
                rerouteDistance = distance;
                rerouteBest.kind = GraphHitKind::Reroute;
                rerouteBest.edge = edge.id;
                rerouteBest.routeIndex = static_cast<int>(point) - 1;
                rerouteBest.screen = screenPolyline.at(point);
                rerouteBest.scene = view.toScene(screenPolyline.at(point));
                rerouteBest.distance = distance;
            }
        }

        // The pipe body is the projection of the pointer onto the projected
        // polyline; its scene point is that same point converted back, never a
        // separately projected scene point.
        const PolylineProjection projection = projectOnPolyline(screenPolyline, screen);
        if (projection.distance <= pipeDistance) {
            pipeDistance = projection.distance;
            pipeBest.kind = GraphHitKind::PipeBody;
            pipeBest.edge = edge.id;
            pipeBest.segment = projection.segment;
            pipeBest.screen = projection.point;
            pipeBest.scene = view.toScene(projection.point);
            pipeBest.distance = projection.distance;
        }
    }

    result.endpoint = endpointBest;
    result.reroute = rerouteBest;
    result.pipe = pipeBest;
    return result;
}

}  // namespace nemo::ui
