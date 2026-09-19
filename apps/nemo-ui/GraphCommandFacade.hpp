#pragma once

#include "GraphScene.hpp"

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace nemo::ui {

class ViewerController;

// The only place in the interaction layer that calls the graph command API.
// Every commit path goes through this facade, so there is exactly one call site
// to audit and UI and automation keep sharing one command path.
//
// A command that can be refused returns whether it was accepted (invalid
// endpoints, a cycle, a vanished node). A refusal is visible through the
// document: an edit that nothing acts on — a gesture commit whose topology the
// artist can see — returns nothing rather than a verdict the caller would
// discard.
class GraphCommandFacade {
public:
    explicit GraphCommandFacade(ViewerController& controller) : controller_(&controller) {}

    void moveNodes(const QString& networkId, const QVector<QPair<QString, QPointF>>& positions);
    void connectOrReplace(const QString& networkId, const GraphEndpointRecord& from, const GraphEndpointRecord& to);
    void rewire(const QString& networkId, const QString& edgeId, const GraphEndpointRecord& from,
                const GraphEndpointRecord& to);
    void disconnect(const QString& networkId, const QString& edgeId);
    void commitRoute(const QString& networkId, const QString& edgeId, const QVector<QPointF>& points);
    [[nodiscard]] bool insertExistingNodeOnEdge(const QString& networkId, const QString& nodeId, const QString& edgeId,
                                               QPointF position);
    [[nodiscard]] QString createNode(const QString& networkId, const QString& type, const QString& name, QPointF position,
                                     const QString& anchorId, const QVector<QPair<QString, QPointF>>& shiftedNodes);
    [[nodiscard]] bool deleteNodes(const QString& networkId, const QStringList& nodeIds);
    [[nodiscard]] bool assignViewer(const QString& networkId, int viewerIndex, const QString& nodeId);
    [[nodiscard]] QString duplicateLinked(const QString& networkId, const QString& nodeId, QPointF position);
    [[nodiscard]] bool makeIndependent(const QString& instanceId);
    [[nodiscard]] QString collapseSelection(const QString& networkId, const QStringList& nodeIds);
    [[nodiscard]] bool copySelection(const QString& networkId, const QStringList& nodeIds);
    [[nodiscard]] QStringList pasteSelection(const QString& networkId, QPointF position);

private:
    ViewerController* controller_;
};

}  // namespace nemo::ui
