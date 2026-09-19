#include "GraphCommandFacade.hpp"

#include "ViewerController.hpp"

#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <utility>

namespace nemo::ui {
namespace {

// The controller's own wire shapes: positions and route points cross as
// {x, y} maps, node identities as the decimal strings the snapshot publishes.
[[nodiscard]] QVariantList positionList(const QVector<QPair<QString, QPointF>>& positions) {
    QVariantList list;
    list.reserve(positions.size());
    for (const auto& [id, position] : positions) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), id);
        entry.insert(QStringLiteral("x"), position.x());
        entry.insert(QStringLiteral("y"), position.y());
        list.append(entry);
    }
    return list;
}

[[nodiscard]] QVariantList pointList(const QVector<QPointF>& points) {
    QVariantList list;
    list.reserve(points.size());
    for (const QPointF& point : points) {
        QVariantMap entry;
        entry.insert(QStringLiteral("x"), point.x());
        entry.insert(QStringLiteral("y"), point.y());
        list.append(entry);
    }
    return list;
}

[[nodiscard]] QVariantList identityList(const QStringList& ids) {
    QVariantList list;
    list.reserve(ids.size());
    for (const QString& id : ids)
        list.append(id);
    return list;
}

}  // namespace

void GraphCommandFacade::moveNodes(const QString& networkId, const QVector<QPair<QString, QPointF>>& positions) {
    controller_->commitGraphMove(networkId, positionList(positions));
}

void GraphCommandFacade::connectOrReplace(const QString& networkId, const GraphEndpointRecord& from,
                                          const GraphEndpointRecord& to) {
    controller_->connectOrReplaceGraph(networkId, from.node, from.port, to.node, to.port);
}

void GraphCommandFacade::rewire(const QString& networkId, const QString& edgeId, const GraphEndpointRecord& from,
                                const GraphEndpointRecord& to) {
    controller_->rewireGraphEdge(networkId, edgeId, from.node, from.port, to.node, to.port);
}

void GraphCommandFacade::disconnect(const QString& networkId, const QString& edgeId) {
    controller_->disconnectGraphEdge(networkId, edgeId);
}

void GraphCommandFacade::commitRoute(const QString& networkId, const QString& edgeId, const QVector<QPointF>& points) {
    controller_->commitGraphRoute(networkId, edgeId, pointList(points));
}

bool GraphCommandFacade::insertExistingNodeOnEdge(const QString& networkId, const QString& nodeId,
                                                  const QString& edgeId, QPointF position) {
    return controller_->insertExistingGraphNodeOnEdge(networkId, nodeId, edgeId, position.x(), position.y());
}

QString GraphCommandFacade::createNode(const QString& networkId, const QString& type, const QString& name,
                                       QPointF position, const QString& anchorId,
                                       const QVector<QPair<QString, QPointF>>& shiftedNodes) {
    // The anchor crosses as the identity string as it stands, empty included:
    // the controller reads an unparsable identity as "no anchor".
    return controller_->createGraphNode(networkId, type, name, position.x(), position.y(), QVariant(anchorId),
                                        positionList(shiftedNodes));
}

bool GraphCommandFacade::deleteNodes(const QString& networkId, const QStringList& nodeIds) {
    return controller_->deleteGraphNodes(networkId, identityList(nodeIds));
}

bool GraphCommandFacade::assignViewer(const QString& networkId, int viewerIndex, const QString& nodeId) {
    return controller_->assignViewer(networkId, viewerIndex, nodeId);
}

QString GraphCommandFacade::collapseSelection(const QString& networkId, const QStringList& nodeIds) {
    return controller_->collapseSelection(networkId, identityList(nodeIds), QStringLiteral("Subnet"));
}

bool GraphCommandFacade::copySelection(const QString& networkId, const QStringList& nodeIds) {
    return controller_->copyGraphSelection(networkId, identityList(nodeIds));
}

QStringList GraphCommandFacade::pasteSelection(const QString& networkId, QPointF position) {
    return controller_->pasteGraphSelection(networkId, position.x(), position.y())
        .split(QLatin1Char(','), Qt::SkipEmptyParts);
}

QString GraphCommandFacade::duplicateLinked(const QString& networkId, const QString& nodeId, QPointF position) {
    return controller_->duplicateLinkedInstance(networkId, nodeId, position.x(), position.y());
}

bool GraphCommandFacade::makeIndependent(const QString& instanceId) {
    return controller_->makeIndependent(instanceId);
}

}  // namespace nemo::ui
