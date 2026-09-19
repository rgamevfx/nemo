#include "GraphScene.hpp"

#include <QVariantList>
#include <QVariantMap>

#include <utility>

namespace nemo::ui {
namespace {

// The snapshot publishes an invalid identity as the literal "0". Every other
// value is an identity, including the empty string.
QString idFromVariant(const QVariant& value) {
    const auto text = value.toString();
    return text == QStringLiteral("0") ? QString{} : text;
}

// A position authored as a QPointF, as an {x, y} map, or as a two-element list.
// Node positions and route points are read through this one reader, and any
// other shape is unparsable: it is skipped rather than normalised onto a point
// at the origin the snapshot never authored.
QPointF pointFromVariant(const QVariant& value, bool& valid) {
    // A point itself — QPointF, or the integer QPoint a caller may hand over.
    // No container converts to a point, so this cannot swallow a malformed one.
    if (value.canConvert<QPointF>()) {
        valid = true;
        return value.toPointF();
    }
    const auto type = value.metaType().id();
    if (type == QMetaType::QVariantMap || type == QMetaType::QVariantHash) {
        const auto map = value.toMap();
        if (map.contains(QStringLiteral("x")) && map.contains(QStringLiteral("y"))) {
            valid = true;
            return {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()};
        }
    } else if (type == QMetaType::QVariantList || type == QMetaType::QStringList) {
        const auto list = value.toList();
        if (list.size() >= 2) {
            valid = true;
            return {list.at(0).toDouble(), list.at(1).toDouble()};
        }
    }
    valid = false;
    return {};
}

// Authored x/y win; otherwise a `position` key in either of the three shapes;
// otherwise the origin.
QPointF nodePosition(const QVariantMap& map) {
    if (map.contains(QStringLiteral("x")) && map.contains(QStringLiteral("y"))) {
        return {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()};
    }
    if (map.contains(QStringLiteral("position"))) {
        bool valid = false;
        const auto point = pointFromVariant(map.value(QStringLiteral("position")), valid);
        if (valid)
            return point;
    }
    return {};
}

QVector<GraphPortRecord> portRecords(const QVariant& value) {
    const auto list = value.toList();
    QVector<GraphPortRecord> ports;
    ports.reserve(list.size());
    for (qsizetype position = 0; position < list.size(); ++position) {
        const auto map = list.at(position).toMap();
        GraphPortRecord port;
        port.index = map.contains(QStringLiteral("index")) ? map.value(QStringLiteral("index")).toInt()
                                                           : static_cast<int>(position);
        port.kind = map.value(QStringLiteral("kind")).toString();
        port.name = map.value(QStringLiteral("name")).toString();
        ports.push_back(std::move(port));
    }
    return ports;
}

GraphEndpointRecord endpointRecord(const QVariantMap& map, const QString& nodeKey, const QString& portKey,
                                   bool output) {
    GraphEndpointRecord endpoint;
    endpoint.node = idFromVariant(map.value(nodeKey));
    // An absent port key is not port zero: it is the endpoint's own invalid
    // index, which names no declared port.
    if (map.contains(portKey))
        endpoint.port = map.value(portKey).toInt();
    endpoint.output = output;
    return endpoint;
}

QVector<QPointF> routePoints(const QVariant& value) {
    const auto list = value.toList();
    QVector<QPointF> route;
    route.reserve(list.size());
    for (const auto& entry : list) {
        bool valid = false;
        const auto point = pointFromVariant(entry, valid);
        if (valid)
            route.push_back(point);
    }
    return route;
}

}  // namespace

const GraphNodeRecord* GraphScene::node(const QString& id) const {
    if (id.isEmpty())
        return nullptr;
    const auto found = nodeIndex.constFind(id);
    if (found == nodeIndex.constEnd())
        return nullptr;
    const auto index = *found;
    return index >= 0 && index < nodes.size() ? &nodes.at(index) : nullptr;
}

const GraphEdgeRecord* GraphScene::edge(const QString& id) const {
    if (id.isEmpty())
        return nullptr;
    const auto found = edgeIndex.constFind(id);
    if (found == edgeIndex.constEnd())
        return nullptr;
    const auto index = *found;
    return index >= 0 && index < edges.size() ? &edges.at(index) : nullptr;
}

GraphScene buildGraphScene(const QVariantMap& snapshot) {
    GraphScene scene;
    scene.available = snapshot.value(QStringLiteral("available")).toBool();
    scene.networkId = snapshot.value(QStringLiteral("networkId")).toString();
    // An unavailable snapshot projects no records; the caller still stamps the
    // revision, so the panel aborts a live session on a revision change while it
    // displays nothing for a network that is not there.
    if (!scene.available)
        return scene;

    const auto nodes = snapshot.value(QStringLiteral("nodes")).toList();
    scene.nodes.reserve(nodes.size());
    for (const auto& value : nodes) {
        const auto map = value.toMap();
        GraphNodeRecord record;
        record.id = idFromVariant(map.value(QStringLiteral("id")));
        record.name = map.value(QStringLiteral("name")).toString();
        record.type = map.value(QStringLiteral("type")).toString();
        record.category = map.value(QStringLiteral("category")).toString();
        record.position = nodePosition(map);
        record.deletable = map.contains(QStringLiteral("deletable"))
                               ? map.value(QStringLiteral("deletable")).toBool()
                               : map.value(QStringLiteral("canDelete"), true).toBool();
        record.terminal = map.value(QStringLiteral("terminal")).toBool();
        record.definition = map.value(QStringLiteral("definition")).toString();
        record.instance = map.value(QStringLiteral("instance")).toString();
        record.linkState = map.value(QStringLiteral("linkState")).toString();
        record.inputs = portRecords(map.value(QStringLiteral("inputs")));
        record.outputs = portRecords(map.value(QStringLiteral("outputs")));
        if (!record.id.isEmpty())
            scene.nodeIndex.insert(record.id, scene.nodes.size());
        scene.nodes.push_back(std::move(record));
    }

    const auto edges = snapshot.value(QStringLiteral("edges")).toList();
    scene.edges.reserve(edges.size());
    for (const auto& value : edges) {
        const auto map = value.toMap();
        GraphEdgeRecord record;
        record.id = idFromVariant(map.value(QStringLiteral("id")));
        record.from = endpointRecord(map, QStringLiteral("fromNode"), QStringLiteral("fromPort"), true);
        record.to = endpointRecord(map, QStringLiteral("toNode"), QStringLiteral("toPort"), false);
        record.route = routePoints(map.value(QStringLiteral("route")));
        if (!record.id.isEmpty())
            scene.edgeIndex.insert(record.id, scene.edges.size());
        scene.edges.push_back(std::move(record));
    }
    return scene;
}

}  // namespace nemo::ui
