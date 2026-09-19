#pragma once

#include "GraphGeometry.hpp"
#include "GraphScene.hpp"

#include <QPointF>
#include <QString>

namespace nemo::ui {

enum class GraphHitKind {
    None,
    // The enter-subnet affordance on the topmost card: it wins over everything,
    // including the card's own ports, exactly as the prototype acquired it.
    Affordance,
    // A port glyph.
    Port,
    // A connection end sitting on a port.
    Endpoint,
    // A reroute dot on a pipe.
    Reroute,
    // A card body.
    Card,
    // A pipe body.
    PipeBody,
};

struct GraphHit {
    GraphHitKind kind{GraphHitKind::None};
    QString node;
    QString edge;
    int port{-1};
    bool output{};
    // Reroute dots address the authored route array, not the polyline.
    int routeIndex{-1};
    // The polyline segment the hit landed on.
    int segment{-1};
    QPointF screen;
    QPointF scene;
    qreal distance{};

    [[nodiscard]] bool isNull() const { return kind == GraphHitKind::None; }
};

// One ordered pass over the scene resolving every class the pointer can acquire,
// so hover, press and pipe-body gating all read a single query. Within a class
// the topmost element wins: a later candidate wins a tie, which is the paint
// order the items are drawn in. A port wins over its own card, and the card of a
// subnet wins nothing over its own affordance.
struct GraphHitResult {
    GraphHit affordance;
    GraphHit port;
    // The winning port of each direction, in screen-space distance order. The
    // combined `port` above is one of these two.
    GraphHit portOutput;
    GraphHit portInput;
    GraphHit endpoint;
    GraphHit reroute;
    GraphHit card;
    // Raw pipe-body proximity, which is what a grab and the hover feedback use.
    GraphHit pipe;

    // Port, connection endpoint, reroute dot, card, pipe body.
    [[nodiscard]] const GraphHit& primary() const;
};

// The buffers one ordered pass writes into, owned by the caller so a pointer
// move reuses one set of allocations instead of minting a polyline per edge and
// a screen rectangle per node. The pass clears what it uses, so a scratch that
// has been through the pass once allocates nothing; nothing in the result
// points into this storage.
struct GraphHitScratch {
    QVector<QRectF> screenCards;
    QVector<QPointF> polyline;
    QVector<QPointF> screenPolyline;
};

[[nodiscard]] GraphHitResult hitTestGraph(const GraphScene& scene, const GraphViewTransform& view, QPointF screen,
                                          GraphHitScratch& scratch);

// What the pointer currently acquires, published for the panel's feedback and
// for the painter's highlight. Derived from the same ordered pass a press
// resolves its target with, so feedback can never disagree with the pick.
struct GraphHover {
    // A card is highlighted only when it wins over its ports and affordance.
    QString card;
    // The pipe body under the pointer.
    QString edge;
    // The port that would be grabbed, or the end of the pipe under the pointer
    // when no card and no port is there.
    GraphHit endpoint;
    // The reroute dot under the pointer.
    GraphHit reroute;
};

}  // namespace nemo::ui
