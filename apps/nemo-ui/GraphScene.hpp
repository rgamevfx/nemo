#pragma once

#include <QHash>
#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <QVector>

namespace nemo::ui {

// The read-only network snapshot projected into immutable records with an
// identity index and a revision stamp. The stamp is supplied by the caller that
// knows the document revision (`ViewerController::graphRevision()`); the
// projection itself does not carry it, so a snapshot's content is exactly the
// network it describes. Built on the GUI thread from
// ViewerController::graphSnapshot(); it holds no Qt Quick types and no
// transient state, and nothing downstream mutates it. Every consumer — the
// paint item, the hit test and the gesture sessions — reads this one record
// set, so a snapshot is parsed exactly once per document change.
//
// A session retains the scene it began from, and the interaction aborts live
// sessions when a snapshot carries a different revision, so a superseded scene
// can never address the wrong node.

struct GraphPortRecord {
    // A port is addressed by its declared index, which is the identity the
    // snapshot publishes and the identity a pick reports.
    int index{};
    // image | mask | ..., which decides the port's side and its appearance.
    QString kind;
    QString name;
};

struct GraphEndpointRecord {
    QString node;
    // Port identity is the declared index, which is the identity the snapshot
    // publishes (`id` is that index as a string).
    int port{-1};
    bool output{};
};

struct GraphNodeRecord {
    QString id;
    QString name;
    QString type;
    QString category;
    // Authored position, card top-left, in scene coordinates.
    QPointF position;
    bool deletable{true};
    // A formal network terminal (a child network's input or output port drawn
    // as a card in its own network).
    bool terminal{};
    // Child network identity when this node is a subnet occurrence.
    QString definition;
    QString instance;
    // local | linked | shared, as the snapshot names it.
    QString linkState;
    QVector<GraphPortRecord> inputs;
    QVector<GraphPortRecord> outputs;

    [[nodiscard]] bool hasChildScope() const { return !definition.isEmpty(); }
};

struct GraphEdgeRecord {
    QString id;
    GraphEndpointRecord from;
    GraphEndpointRecord to;
    // Authored reroute points, in scene coordinates.
    QVector<QPointF> route;
};

struct GraphScene {
    bool available{};
    quint64 revision{};
    QString networkId;
    QVector<GraphNodeRecord> nodes;
    QVector<GraphEdgeRecord> edges;
    QHash<QString, qsizetype> nodeIndex;
    QHash<QString, qsizetype> edgeIndex;

    [[nodiscard]] const GraphNodeRecord* node(const QString& id) const;
    [[nodiscard]] const GraphEdgeRecord* edge(const QString& id) const;
};

// Projects one snapshot map. The caller stamps `revision` afterwards. An
// unavailable snapshot yields an empty scene, so the panel can switch networks
// and still abort a live session on a revision change.
[[nodiscard]] GraphScene buildGraphScene(const QVariantMap& snapshot);

}  // namespace nemo::ui
