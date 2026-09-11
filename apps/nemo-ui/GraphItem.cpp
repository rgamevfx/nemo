#include "GraphItem.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGVertexColorMaterial>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace nemo::ui {
namespace {

constexpr int kMaxVisibleLabels = 256;
constexpr int kAtlasWidth = 1008;
constexpr int kAtlasCellWidth = 112;
constexpr int kAtlasCellHeight = 24;
constexpr qreal kGraphMargin = 16.0;
constexpr qreal kCardWidth = 112.0;
constexpr qreal kCardHeight = 28.0;
constexpr qreal kPortRadius = 4.0;

quint64 idFromVariant(const QVariant& value) {
    bool ok = false;
    const auto text = value.toString();
    if (!text.isEmpty()) {
        const auto id = text.toULongLong(&ok);
        if (ok)
            return id;
    }
    return value.toULongLong(&ok);
}

QPointF pointFromVariant(const QVariant& value, bool* valid = nullptr) {
    if (value.canConvert<QPointF>()) {
        if (valid)
            *valid = true;
        return value.toPointF();
    }
    const auto map = value.toMap();
    if (map.contains(QStringLiteral("x")) && map.contains(QStringLiteral("y"))) {
        if (valid)
            *valid = true;
        return {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()};
    }
    const auto list = value.toList();
    if (list.size() >= 2) {
        if (valid)
            *valid = true;
        return {list.at(0).toDouble(), list.at(1).toDouble()};
    }
    if (valid)
        *valid = false;
    return {};
}

struct EndpointValues {
    quint64 node{};
    QString portId;
    int portIndex{-1};
};

EndpointValues readEndpoint(const QVariantMap& map, const QString& nodeKey, const QString& portKey) {
    EndpointValues endpoint;
    endpoint.node = idFromVariant(map.value(nodeKey));
    const auto port = map.value(portKey);
    endpoint.portIndex = port.toInt();
    endpoint.portId = port.toString();
    if (endpoint.portId.isEmpty() && endpoint.portIndex >= 0)
        endpoint.portId = QString::number(endpoint.portIndex);
    return endpoint;
}

QSGGeometryNode* makeColoredGeometryNode() {
    auto node = std::make_unique<QSGGeometryNode>();
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node->setGeometry(geometry.release());
    node->setFlag(QSGNode::OwnsGeometry);
    node->setFlag(QSGNode::OwnedByParent);
    auto material = std::make_unique<QSGVertexColorMaterial>();
    node->setMaterial(material.release());
    node->setFlag(QSGNode::OwnsMaterial);
    return node.release();
}

void appendColoredCircle(QVector<QSGGeometry::ColoredPoint2D>& vertices, QPointF center, qreal radius,
                         const QColor& color) {
    constexpr int segments = 12;
    const QRgb rgba = qPremultiply(color.rgba());
    for (int i = 0; i < segments; ++i) {
        const auto a = (2.0 * 3.14159265358979323846 * i) / segments;
        const auto b = (2.0 * 3.14159265358979323846 * (i + 1)) / segments;
        const auto base = vertices.size();
        vertices.resize(base + 3);
        const auto set = [rgba](QSGGeometry::ColoredPoint2D& vertex, QPointF point) {
            vertex.set(float(point.x()), float(point.y()), qRed(rgba), qGreen(rgba), qBlue(rgba), qAlpha(rgba));
        };
        set(vertices[base + 0], center);
        set(vertices[base + 1], {center.x() + std::cos(a) * radius, center.y() + std::sin(a) * radius});
        set(vertices[base + 2], {center.x() + std::cos(b) * radius, center.y() + std::sin(b) * radius});
    }
}
qreal relativeLuminanceChannel(int channel) {
    const qreal value = channel / 255.0;
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

QColor nodeTextColor(const QColor& fill) {
    const qreal luminance = 0.2126 * relativeLuminanceChannel(fill.red()) +
                            0.7152 * relativeLuminanceChannel(fill.green()) +
                            0.0722 * relativeLuminanceChannel(fill.blue());
    constexpr qreal darkLuminance = 0.2126 * (0x17 / 255.0) + 0.7152 * (0x1a / 255.0) + 0.0722 * (0x1f / 255.0);
    const qreal whiteContrast = 1.05 / (luminance + 0.05);
    const qreal darkContrast =
        (std::max(luminance, darkLuminance) + 0.05) / (std::min(luminance, darkLuminance) + 0.05);
    return whiteContrast >= darkContrast ? QColor(QStringLiteral("#ffffff")) : QColor(QStringLiteral("#171a1f"));
}
QVector<QPointF> roundedPolygon(QRectF rectangle, qreal radius) {
    rectangle = rectangle.normalized();
    radius = std::min({radius, rectangle.width() * 0.5, rectangle.height() * 0.5});
    constexpr int segments = 4;
    QVector<QPointF> points;
    points.reserve(segments * 4);
    const QPointF centers[] = {{rectangle.right() - radius, rectangle.top() + radius},
                               {rectangle.right() - radius, rectangle.bottom() - radius},
                               {rectangle.left() + radius, rectangle.bottom() - radius},
                               {rectangle.left() + radius, rectangle.top() + radius}};
    for (int corner = 0; corner < 4; ++corner) {
        const qreal start = -3.14159265358979323846 * 0.5 + corner * 3.14159265358979323846 * 0.5;
        for (int segment = 0; segment < segments; ++segment) {
            const qreal angle = start + segment * (3.14159265358979323846 * 0.5) / (segments - 1);
            points.push_back(centers[corner] + QPointF(std::cos(angle) * radius, std::sin(angle) * radius));
        }
    }
    return points;
}

void appendRoundedRect(QVector<QSGGeometry::ColoredPoint2D>& vertices, QRectF rectangle, qreal radius,
                       const QColor& color) {
    const auto points = roundedPolygon(rectangle, radius);
    const QPointF center = rectangle.center();
    for (int i = 0; i < points.size(); ++i) {
        const auto append = [&vertices, &color](QPointF point) {
            vertices.push_back({});
            vertices.back().set(float(point.x()), float(point.y()), color.red(), color.green(), color.blue(),
                                color.alpha());
        };
        append(center);
        append(points.at(i));
        append(points.at((i + 1) % points.size()));
    }
}

void appendRoundedBorder(QVector<QSGGeometry::ColoredPoint2D>& vertices, QRectF rectangle, qreal thickness,
                         qreal radius, const QColor& color) {
    const auto outer = roundedPolygon(rectangle, radius);
    const auto inner = roundedPolygon(rectangle.adjusted(thickness, thickness, -thickness, -thickness),
                                      std::max<qreal>(0.0, radius - thickness));
    if (outer.size() != inner.size())
        return;
    for (int i = 0; i < outer.size(); ++i) {
        const auto next = (i + 1) % outer.size();
        const auto append = [&vertices, &color](QPointF point) {
            vertices.push_back({});
            vertices.back().set(float(point.x()), float(point.y()), color.red(), color.green(), color.blue(),
                                color.alpha());
        };
        append(outer.at(i));
        append(outer.at(next));
        append(inner.at(next));
        append(outer.at(i));
        append(inner.at(next));
        append(inner.at(i));
    }
}

void appendCircleBorder(QVector<QSGGeometry::ColoredPoint2D>& vertices, QPointF center, qreal radius, qreal thickness,
                        const QColor& color) {
    constexpr int segments = 16;
    const qreal innerRadius = std::max<qreal>(0.0, radius - thickness);
    for (int i = 0; i < segments; ++i) {
        const qreal a = (2.0 * 3.14159265358979323846 * i) / segments;
        const qreal b = (2.0 * 3.14159265358979323846 * (i + 1)) / segments;
        const QPointF outerA = center + QPointF(std::cos(a) * radius, std::sin(a) * radius);
        const QPointF outerB = center + QPointF(std::cos(b) * radius, std::sin(b) * radius);
        const QPointF innerA = center + QPointF(std::cos(a) * innerRadius, std::sin(a) * innerRadius);
        const QPointF innerB = center + QPointF(std::cos(b) * innerRadius, std::sin(b) * innerRadius);
        const auto append = [&vertices, &color](QPointF point) {
            vertices.push_back({});
            vertices.back().set(float(point.x()), float(point.y()), color.red(), color.green(), color.blue(),
                                color.alpha());
        };
        append(outerA);
        append(outerB);
        append(innerB);
        append(outerA);
        append(innerB);
        append(innerA);
    }
}

void appendColoredLine(QVector<QSGGeometry::ColoredPoint2D>& vertices, QPointF from, QPointF to, qreal width,
                       const QColor& color) {
    const QPointF difference = to - from;
    const qreal length = std::hypot(difference.x(), difference.y());
    if (length < 0.001)
        return;
    const QPointF normal(-difference.y() * width / (2.0 * length), difference.x() * width / (2.0 * length));
    const QRgb rgba = qPremultiply(color.rgba());
    const auto append = [&vertices, rgba](QPointF point) {
        vertices.push_back({});
        vertices.back().set(float(point.x()), float(point.y()), qRed(rgba), qGreen(rgba), qBlue(rgba), qAlpha(rgba));
    };
    append(from + normal);
    append(to + normal);
    append(to - normal);
    append(from + normal);
    append(to - normal);
    append(from - normal);
}

void appendBorder(QVector<QSGGeometry::ColoredPoint2D>& vertices, QRectF rectangle, qreal thickness,
                  const QColor& color) {
    appendRoundedBorder(vertices, rectangle, thickness, 3.0, color);
}

void updateColoredGeometry(QSGGeometryNode* node, const QVector<QSGGeometry::ColoredPoint2D>& vertices) {
    auto* geometry = node->geometry();
    if (geometry->vertexCount() != vertices.size())
        geometry->allocate(static_cast<int>(vertices.size()));
    if (!vertices.isEmpty())
        std::copy(vertices.cbegin(), vertices.cend(), geometry->vertexDataAsColoredPoint2D());
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
}

struct LabelSpec {
    QString key;
    QString text;
    QPointF position;
    QColor color;
    int fontSize{11};
    int rasterScale{1};
    friend bool operator==(const LabelSpec&, const LabelSpec&) = default;
};

class LabelAtlasNode final : public QSGNode {
public:
    explicit LabelAtlasNode(std::unique_ptr<QSGTexture> texture)
        : texture_(std::move(texture)), geometryNode_(std::make_unique<QSGGeometryNode>()) {
        auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
        geometryNode_->setGeometry(geometry.release());
        geometryNode_->setFlag(QSGNode::OwnsGeometry);
        geometryNode_->setFlag(QSGNode::OwnedByParent, false);
        auto material = std::make_unique<QSGTextureMaterial>();
        material->setTexture(texture_.get());
        material->setFiltering(QSGTexture::Linear);
        geometryNode_->setMaterial(material.release());
        geometryNode_->setFlag(QSGNode::OwnsMaterial);
        appendChildNode(geometryNode_.get());
    }
    ~LabelAtlasNode() override { removeChildNode(geometryNode_.get()); }
    [[nodiscard]] QSGGeometryNode* geometryNode() const { return geometryNode_.get(); }

private:
    std::unique_ptr<QSGTexture> texture_;
    std::unique_ptr<QSGGeometryNode> geometryNode_;
};

struct GraphFrame {
    QVector<QSGGeometry::ColoredPoint2D> edges;
    QVector<QSGGeometry::ColoredPoint2D> edgeHighlights;
    QVector<QSGGeometry::ColoredPoint2D> bodies;
    QVector<QSGGeometry::ColoredPoint2D> outlines;
    QVector<LabelSpec> labels;
};

class GraphSceneNode final : public QSGNode {
public:
    GraphSceneNode() {
        edges_ = makeColoredGeometryNode();
        appendChildNode(edges_);
        edgeHighlights_ = makeColoredGeometryNode();
        appendChildNode(edgeHighlights_);
        bodies_ = makeColoredGeometryNode();
        appendChildNode(bodies_);
        outlines_ = makeColoredGeometryNode();
        appendChildNode(outlines_);
        frame_.labels.reserve(kMaxVisibleLabels);
    }
    GraphFrame& beginFrame() {
        frame_.edges.clear();
        frame_.edgeHighlights.clear();
        frame_.bodies.clear();
        frame_.outlines.clear();
        frame_.labels.clear();
        return frame_;
    }
    QSGGeometryNode* edges() const { return edges_; }
    QSGGeometryNode* edgeHighlights() const { return edgeHighlights_; }
    QSGGeometryNode* bodies() const { return bodies_; }
    QSGGeometryNode* outlines() const { return outlines_; }
    const QVector<LabelSpec>& cachedLabels() const { return cachedLabels_; }
    void replaceLabels(std::unique_ptr<LabelAtlasNode> next, const QVector<LabelSpec>& labels) {
        const std::unique_ptr<LabelAtlasNode> previous(labels_);
        if (previous != nullptr)
            removeChildNode(previous.get());
        labels_ = next.release();
        if (labels_ != nullptr) {
            labels_->setFlag(QSGNode::OwnedByParent);
            appendChildNode(labels_);
        }
        cachedLabels_ = labels;
    }

private:
    QSGGeometryNode* edges_{};
    QSGGeometryNode* edgeHighlights_{};
    QSGGeometryNode* bodies_{};
    QSGGeometryNode* outlines_{};
    LabelAtlasNode* labels_{};
    QVector<LabelSpec> cachedLabels_;
    GraphFrame frame_;
};
QImage renderLabelAtlas(const QVector<LabelSpec>& labels, QVector<QRectF>* atlasRects, QSize* atlasSize) {
    if (labels.isEmpty())
        return {};
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(std::max(1, labels.constFirst().fontSize));
    font.setWeight(QFont::Medium);
    const QFontMetrics metrics(font);
    const int columns = kAtlasWidth / kAtlasCellWidth;
    const int rows = (static_cast<int>(labels.size()) + columns - 1) / columns;
    const int rasterScale = labels.constFirst().rasterScale;
    *atlasSize = QSize(kAtlasWidth * rasterScale, rows * kAtlasCellHeight * rasterScale);
    QImage image(*atlasSize, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    atlasRects->clear();
    atlasRects->reserve(labels.size());
    QPainter painter(&image);
    painter.scale(rasterScale, rasterScale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);
    for (int i = 0; i < labels.size(); ++i) {
        const auto& label = labels.at(i);
        const int column = i % columns;
        const int row = i / columns;
        const int y = row * kAtlasCellHeight;
        const QString text = metrics.elidedText(label.text, Qt::ElideRight, kAtlasCellWidth - 12);
        const int advance = std::max(1, metrics.horizontalAdvance(text));
        const int centeredX = column * kAtlasCellWidth + (kAtlasCellWidth - advance) / 2;
        painter.setPen(label.color);
        painter.drawText(QPointF(centeredX, y + (kAtlasCellHeight - metrics.height()) / 2 + metrics.ascent()), text);
        atlasRects->push_back(QRectF(column * kAtlasCellWidth * rasterScale, y * rasterScale,
                                     kAtlasCellWidth * rasterScale, kAtlasCellHeight * rasterScale));
    }
    painter.end();
    return image;
}

std::unique_ptr<LabelAtlasNode> makeLabelAtlas(QQuickWindow* window, const QVector<LabelSpec>& labels) {
    if (window == nullptr || labels.isEmpty())
        return nullptr;
    QVector<QRectF> atlasRects;
    QSize atlasSize;
    const QImage image = renderLabelAtlas(labels, &atlasRects, &atlasSize);
    std::unique_ptr<QSGTexture> texture(window->createTextureFromImage(image));
    if (texture == nullptr)
        return nullptr;
    texture->setFiltering(QSGTexture::Linear);
    auto atlas = std::make_unique<LabelAtlasNode>(std::move(texture));
    auto* geometry = atlas->geometryNode()->geometry();
    geometry->allocate(static_cast<int>(labels.size()) * 6);
    auto* vertices = geometry->vertexDataAsTexturedPoint2D();
    for (qsizetype i = 0; i < labels.size(); ++i) {
        const auto& label = labels.at(i);
        const QRectF source = atlasRects.at(i);
        const auto u0 = float(source.left() / atlasSize.width());
        const auto u1 = float(source.right() / atlasSize.width());
        const auto v0 = float(source.top() / atlasSize.height());
        const auto v1 = float(source.bottom() / atlasSize.height());
        auto* out = vertices + i * 6;
        out[0].set(float(label.position.x()), float(label.position.y()), u0, v0);
        out[1].set(float(label.position.x() + kAtlasCellWidth), float(label.position.y()), u1, v0);
        out[2].set(float(label.position.x() + kAtlasCellWidth), float(label.position.y() + kAtlasCellHeight), u1, v1);
        out[3].set(float(label.position.x()), float(label.position.y()), u0, v0);
        out[4].set(float(label.position.x() + kAtlasCellWidth), float(label.position.y() + kAtlasCellHeight), u1, v1);
        out[5].set(float(label.position.x()), float(label.position.y() + kAtlasCellHeight), u0, v1);
    }
    geometry->markVertexDataDirty();
    atlas->geometryNode()->markDirty(QSGNode::DirtyGeometry);
    return atlas;
}

}  // namespace

GraphItem::GraphItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setImplicitSize(520.0, 260.0);
}

GraphItem::~GraphItem() = default;

void GraphItem::setNodes(const QVariantList& nodes) {
    if (nodesProperty_ == nodes)
        return;
    nodesProperty_ = nodes;
    rebuildNodeRecords();
    emit nodesChanged();
    update();
}

void GraphItem::setEdges(const QVariantList& edges) {
    if (edgesProperty_ == edges)
        return;
    edgesProperty_ = edges;
    rebuildEdgeRecords();
    updateImplicitSize();
    emit edgesChanged();
    update();
}

void GraphItem::setCategoryColors(const QVariantMap& colors) {
    if (categoryColorsProperty_ == colors)
        return;
    categoryColorsProperty_ = colors;
    categoryColorRecords_.clear();
    categoryColorRecords_.reserve(colors.size());
    for (auto it = colors.cbegin(); it != colors.cend(); ++it) {
        QColor color = it.value().value<QColor>();
        if (!color.isValid())
            color = QColor(it.value().toString());
        if (color.isValid())
            categoryColorRecords_.insert(it.key(), color);
    }
    emit categoryColorsChanged();
    update();
}
void GraphItem::setPresentationStyle(const QVariantMap& style) {
    if (presentationStyleProperty_ == style)
        return;
    presentationStyleProperty_ = style;
    const auto readColor = [&style](const QString& key, const QColor& fallback) {
        QColor color = style.value(key).value<QColor>();
        if (!color.isValid())
            color = QColor(style.value(key).toString());
        return color.isValid() ? color : fallback;
    };
    accentColor_ = readColor(QStringLiteral("accent"), accentColor_);
    borderColor_ = readColor(QStringLiteral("border"), borderColor_);
    mutedColor_ = readColor(QStringLiteral("muted"), mutedColor_);
    panelColor_ = readColor(QStringLiteral("panel"), panelColor_);
    nodeColor_ = readColor(QStringLiteral("node"), nodeColor_);
    const int fontSize = style.value(QStringLiteral("fontSize")).toInt();
    if (fontSize > 0)
        fontSize_ = fontSize;
    emit presentationStyleChanged();
    update();
}

void GraphItem::setViewScale(qreal scale) {
    scale = std::isfinite(scale) && scale > 0.0001 ? scale : 1.0;
    if (qFuzzyCompare(viewScaleProperty_, scale))
        return;
    viewScaleProperty_ = scale;
    emit viewScaleChanged();
    update();
}

void GraphItem::setVisibleRect(QRectF rect) {
    rect = rect.normalized();
    if (visibleRect_ == rect)
        return;
    visibleRect_ = rect;
    emit visibleRectChanged();
    update();
}

void GraphItem::setSelectedNodeIds(const QStringList& ids) {
    if (selectedNodeIdsProperty_ == ids)
        return;
    selectedNodeIdsProperty_ = ids;
    rebuildInteractionRecords();
    emit selectedNodeIdsChanged();
    update();
}

void GraphItem::setHoveredNodeId(const QString& id) {
    if (hoveredNodeIdProperty_ == id)
        return;
    hoveredNodeIdProperty_ = id;
    rebuildInteractionRecords();
    emit hoveredNodeIdChanged();
    update();
}

void GraphItem::setHoveredEdgeId(const QString& id) {
    if (hoveredEdgeIdProperty_ == id)
        return;
    hoveredEdgeIdProperty_ = id;
    rebuildInteractionRecords();
    emit hoveredEdgeIdChanged();
    update();
}

void GraphItem::setHoveredEndpoint(const QVariantMap& endpoint) {
    if (hoveredEndpointProperty_ == endpoint)
        return;
    hoveredEndpointProperty_ = endpoint;
    rebuildInteractionRecords();
    emit hoveredEndpointChanged();
    update();
}

void GraphItem::setHoveredReroute(const QVariantMap& reroute) {
    if (hoveredRerouteProperty_ == reroute)
        return;
    hoveredRerouteProperty_ = reroute;
    rebuildInteractionRecords();
    emit hoveredRerouteChanged();
    update();
}

void GraphItem::setWirePreview(const QVariantMap& preview) {
    if (wirePreviewProperty_ == preview)
        return;
    wirePreviewProperty_ = preview;
    rebuildInteractionRecords();
    emit wirePreviewChanged();
    update();
}

void GraphItem::rebuildNodeRecords() {
    nodeRecords_.clear();
    nodeRecords_.reserve(nodesProperty_.size());
    nodeIndex_.clear();
    nodeIndex_.reserve(nodesProperty_.size());
    for (const auto& value : nodesProperty_) {
        const auto map = value.toMap();
        NodeRecord record;
        record.id = idFromVariant(map.value(QStringLiteral("id")));
        record.name = map.value(QStringLiteral("name")).toString();
        record.type = map.value(QStringLiteral("type")).toString();
        record.category = map.value(QStringLiteral("category")).toString();
        record.position = {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()};
        bool positionValid = map.contains(QStringLiteral("x")) && map.contains(QStringLiteral("y"));
        if (!positionValid && map.contains(QStringLiteral("position")))
            record.position = pointFromVariant(map.value(QStringLiteral("position")), &positionValid);
        if (!positionValid)
            record.position = {};
        record.rectangle = QRectF(record.position, QSizeF(kCardWidth, kCardHeight));
        record.deletable = map.contains(QStringLiteral("deletable"))
                               ? map.value(QStringLiteral("deletable")).toBool()
                               : map.value(QStringLiteral("canDelete"), true).toBool();
        const auto readPorts = [](const QVariant& portsValue, QVector<PortRecord>* ports) {
            const auto portsList = portsValue.toList();
            ports->reserve(portsList.size());
            for (const auto& portValue : portsList) {
                const auto portMap = portValue.toMap();
                PortRecord port;
                port.id = portMap.value(QStringLiteral("id")).toString();
                if (port.id.isEmpty())
                    port.id = QString::number(portMap.value(QStringLiteral("index")).toInt());
                port.name = portMap.value(QStringLiteral("name")).toString();
                port.kind = portMap.value(QStringLiteral("kind")).toString();
                if (portMap.contains(QStringLiteral("x")) && portMap.contains(QStringLiteral("y"))) {
                    port.localPosition = {portMap.value(QStringLiteral("x")).toDouble(),
                                          portMap.value(QStringLiteral("y")).toDouble()};
                    port.hasPosition = true;
                }
                ports->push_back(std::move(port));
            }
        };
        readPorts(map.value(QStringLiteral("inputs")), &record.inputs);
        readPorts(map.value(QStringLiteral("outputs")), &record.outputs);
        nodeIndex_.insert(record.id, nodeRecords_.size());
        nodeRecords_.push_back(std::move(record));
    }
    rebuildInteractionRecords();
    updateImplicitSize();
}

void GraphItem::rebuildEdgeRecords() {
    edgeRecords_.clear();
    edgeRecords_.reserve(edgesProperty_.size());
    for (const auto& value : edgesProperty_) {
        const auto map = value.toMap();
        EdgeRecord record;
        record.id = idFromVariant(map.value(QStringLiteral("id")));
        const auto from = readEndpoint(map, QStringLiteral("fromNode"), QStringLiteral("fromPort"));
        record.from = {from.node, from.portId, from.portIndex, true};
        const auto to = readEndpoint(map, QStringLiteral("toNode"), QStringLiteral("toPort"));
        record.to = {to.node, to.portId, to.portIndex, false};
        const auto route = map.value(QStringLiteral("route")).toList();
        record.route.reserve(route.size());
        for (const auto& valuePoint : route) {
            bool valid = false;
            const auto point = pointFromVariant(valuePoint, &valid);
            if (valid)
                record.route.push_back(point);
        }
        edgeRecords_.push_back(std::move(record));
    }
    rebuildInteractionRecords();
}

void GraphItem::rebuildInteractionRecords() {
    selectedNodeIds_.clear();
    selectedNodeIds_.reserve(selectedNodeIdsProperty_.size());
    for (const auto& id : selectedNodeIdsProperty_) {
        const auto parsed = idFromVariant(id);
        if (parsed != 0)
            selectedNodeIds_.push_back(parsed);
    }
    hoveredNodeId_ = idFromVariant(hoveredNodeIdProperty_);
    hoveredEdgeId_ = idFromVariant(hoveredEdgeIdProperty_);
    hoveredEndpoint_ = {};
    hoveredEndpointEdge_ = idFromVariant(hoveredEndpointProperty_.value(QStringLiteral("edge")));
    hoveredEndpoint_.node = idFromVariant(hoveredEndpointProperty_.value(QStringLiteral("node")));
    hoveredEndpoint_.portIndex = std::max(0, hoveredEndpointProperty_.value(QStringLiteral("port")).toInt());
    hoveredEndpoint_.portId = hoveredEndpointProperty_.value(QStringLiteral("port")).toString();
    hoveredEndpoint_.output =
        hoveredEndpointProperty_.value(QStringLiteral("direction")).toString() == QStringLiteral("output");
    if (hoveredEndpoint_.portId.isEmpty())
        hoveredEndpoint_.portId = QString::number(hoveredEndpoint_.portIndex);

    hoveredRerouteEdge_ = idFromVariant(hoveredRerouteProperty_.value(QStringLiteral("edge")));
    hoveredRerouteIndex_ = hoveredRerouteProperty_.value(QStringLiteral("index")).toInt();
    rerouteRecords_.clear();
    for (const auto& edge : edgeRecords_) {
        for (int index = 0; index < edge.route.size(); ++index) {
            const bool hovered = edge.id == hoveredRerouteEdge_ && index == hoveredRerouteIndex_ &&
                                 hoveredRerouteProperty_.value(QStringLiteral("dragging")).toBool();
            rerouteRecords_.push_back({edge.id, index, edge.route.at(index), false, hovered});
        }
    }

    const auto previewFrom = readEndpoint(wirePreviewProperty_, QStringLiteral("fromNode"), QStringLiteral("fromPort"));
    wirePreviewFrom_ = {previewFrom.node, previewFrom.portId, previewFrom.portIndex, true};
    const auto previewTo = readEndpoint(wirePreviewProperty_, QStringLiteral("toNode"), QStringLiteral("toPort"));
    wirePreviewTo_ = {previewTo.node, previewTo.portId, previewTo.portIndex, false};
    wirePreviewFromInput_ = wirePreviewProperty_.value(QStringLiteral("fromInput")).toBool();
    wirePreviewPointer_ = {wirePreviewProperty_.value(QStringLiteral("x")).toDouble(),
                           wirePreviewProperty_.value(QStringLiteral("y")).toDouble()};
    wirePreviewHiddenEdge_ = idFromVariant(wirePreviewProperty_.value(QStringLiteral("hiddenEdge")));
    wirePreviewValid_ = wirePreviewFromInput_ ? wirePreviewTo_.node != 0 : wirePreviewFrom_.node != 0;
}
void GraphItem::updateImplicitSize() {
    QSizeF next(520.0, 260.0);
    for (const auto& record : nodeRecords_) {
        next.setWidth(std::max(next.width(), record.rectangle.right() + kGraphMargin));
        next.setHeight(std::max(next.height(), record.rectangle.bottom() + kGraphMargin));
    }
    for (const auto& edge : edgeRecords_) {
        for (const auto& point : edge.route) {
            next.setWidth(std::max(next.width(), point.x() + kGraphMargin));
            next.setHeight(std::max(next.height(), point.y() + kGraphMargin));
        }
    }
    if (contentSize_ != next) {
        contentSize_ = next;
        setImplicitSize(contentSize_.width(), contentSize_.height());
    }
}
const GraphItem::NodeRecord* GraphItem::nodeRecord(quint64 id) const {
    const auto nodeIndex = nodeIndex_.value(id, -1);
    return nodeIndex >= 0 && nodeIndex < nodeRecords_.size() ? &nodeRecords_.at(nodeIndex) : nullptr;
}

QPointF GraphItem::portPoint(const NodeRecord& node, const QString& portId, int portIndex, bool output) const {
    const auto& ports = output ? node.outputs : node.inputs;
    int index = portIndex;
    for (int i = 0; i < ports.size(); ++i) {
        if ((!portId.isEmpty() && ports.at(i).id == portId) || (portId.isEmpty() && i == portIndex)) {
            index = i;
            if (ports.at(i).hasPosition)
                return node.position + ports.at(i).localPosition;
            break;
        }
    }
    if (index < 0 || index >= ports.size())
        index = std::clamp(index, 0, std::max(0, static_cast<int>(ports.size()) - 1));
    if (index >= 0 && index < ports.size() && ports.at(index).hasPosition)
        return node.position + ports.at(index).localPosition;

    const bool right = !output && index >= 0 && index < ports.size() &&
                       ports.at(index).kind.compare(QStringLiteral("mask"), Qt::CaseInsensitive) == 0;
    int sideIndex = 0;
    int sideCount = 0;
    for (int i = 0; i < ports.size(); ++i) {
        const bool sameSide =
            output || (ports.at(i).kind.compare(QStringLiteral("mask"), Qt::CaseInsensitive) == 0) == right;
        if (!sameSide)
            continue;
        if (i == index)
            sideIndex = sideCount;
        ++sideCount;
    }
    const qreal fraction = sideCount <= 1 ? 0.5 : (sideIndex + 1.0) / (sideCount + 1.0);
    if (right)
        return {node.rectangle.right(), node.rectangle.top() + node.rectangle.height() * fraction};
    if (output)
        return {node.rectangle.left() + node.rectangle.width() * fraction, node.rectangle.bottom()};
    return {node.rectangle.left() + node.rectangle.width() * fraction, node.rectangle.top()};
}

QRectF GraphItem::nodeRect(const QVariant& nodeId) const {
    const auto* node = nodeRecord(idFromVariant(nodeId));
    return node == nullptr ? QRectF() : node->rectangle;
}

QPointF GraphItem::portPosition(const QVariant& nodeId, const QVariant& portId, bool output) const {
    const auto* node = nodeRecord(idFromVariant(nodeId));
    if (node == nullptr)
        return {};
    return portPoint(*node, portId.toString(), portId.toInt(), output);
}

QSGNode* GraphItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) {
    std::unique_ptr<GraphSceneNode> created;
    auto* scene = static_cast<GraphSceneNode*>(old);
    if (scene == nullptr) {
        created = std::make_unique<GraphSceneNode>();
        scene = created.get();
    }
    auto& frame = scene->beginFrame();
    const QRectF clip = visibleRect_;
    const qreal inverseScale = 1.0 / std::max<qreal>(viewScaleProperty_, 0.0001);
    const auto alphaColor = [](QColor color, int alpha) {
        color.setAlpha(std::clamp(alpha, 0, 255));
        return color;
    };
    if (!clip.isEmpty()) {
        const auto appendPath = [&](QVector<QSGGeometry::ColoredPoint2D>& vertices, const EdgeRecord& edge, qreal width,
                                    const QColor& color) {
            const auto* fromNode = nodeRecord(edge.from.node);
            const auto* toNode = nodeRecord(edge.to.node);
            if (fromNode == nullptr || toNode == nullptr)
                return;
            QPointF previous = portPoint(*fromNode, edge.from.portId, edge.from.portIndex, true);
            for (const auto& point : edge.route) {
                appendColoredLine(vertices, previous, point, width * inverseScale, color);
                previous = point;
            }
            appendColoredLine(vertices, previous, portPoint(*toNode, edge.to.portId, edge.to.portIndex, false),
                              width * inverseScale, color);
        };
        const auto pathBounds = [&](const EdgeRecord& edge) {
            const auto* fromNode = nodeRecord(edge.from.node);
            const auto* toNode = nodeRecord(edge.to.node);
            if (fromNode == nullptr || toNode == nullptr)
                return QRectF();
            const QPointF source = portPoint(*fromNode, edge.from.portId, edge.from.portIndex, true);
            QPointF minimum = source;
            QPointF maximum = source;
            const auto include = [&minimum, &maximum](QPointF point) {
                minimum.setX(std::min(minimum.x(), point.x()));
                minimum.setY(std::min(minimum.y(), point.y()));
                maximum.setX(std::max(maximum.x(), point.x()));
                maximum.setY(std::max(maximum.y(), point.y()));
            };
            for (const auto& point : edge.route)
                include(point);
            include(portPoint(*toNode, edge.to.portId, edge.to.portIndex, false));
            return QRectF(minimum, maximum)
                .normalized()
                .adjusted(-8.0 * inverseScale, -8.0 * inverseScale, 8.0 * inverseScale, 8.0 * inverseScale);
        };
        for (const auto& edge : edgeRecords_) {
            if (wirePreviewValid_ && edge.id == wirePreviewHiddenEdge_)
                continue;
            if (!pathBounds(edge).intersects(clip))
                continue;
            const bool highlighted = edge.id == hoveredEdgeId_ || edge.id == hoveredEndpointEdge_;
            const bool active =
                std::find(selectedNodeIds_.cbegin(), selectedNodeIds_.cend(), edge.from.node) !=
                    selectedNodeIds_.cend() ||
                std::find(selectedNodeIds_.cbegin(), selectedNodeIds_.cend(), edge.to.node) != selectedNodeIds_.cend();
            if (highlighted || active) {
                const qreal width = highlighted ? 4.0 : 2.0;
                appendPath(frame.edgeHighlights, edge, width, alphaColor(accentColor_, 242));
            } else {
                appendPath(frame.edges, edge, 1.35, alphaColor(mutedColor_, 179));
            }
        }

        for (const auto& reroute : rerouteRecords_) {
            if (wirePreviewValid_ && reroute.edge == wirePreviewHiddenEdge_)
                continue;
            if (!clip.adjusted(-8.0 * inverseScale, -8.0 * inverseScale, 8.0 * inverseScale, 8.0 * inverseScale)
                     .contains(reroute.position))
                continue;
            const qreal radius = (reroute.hovered ? 6.0 : 4.0) * inverseScale;
            appendColoredCircle(frame.bodies, reroute.position, radius, alphaColor(accentColor_, 242));
        }

        if (wirePreviewValid_) {
            QPointF fixed;
            QPointF other = wirePreviewPointer_;
            bool fixedValid = false;
            if (wirePreviewFromInput_) {
                const auto* node = nodeRecord(wirePreviewTo_.node);
                if (node != nullptr) {
                    fixed = portPoint(*node, wirePreviewTo_.portId, wirePreviewTo_.portIndex, false);
                    fixedValid = true;
                }
                if (const auto* source = nodeRecord(wirePreviewFrom_.node); source != nullptr)
                    other = portPoint(*source, wirePreviewFrom_.portId, wirePreviewFrom_.portIndex, true);
            } else {
                const auto* node = nodeRecord(wirePreviewFrom_.node);
                if (node != nullptr) {
                    fixed = portPoint(*node, wirePreviewFrom_.portId, wirePreviewFrom_.portIndex, true);
                    fixedValid = true;
                }
                if (const auto* target = nodeRecord(wirePreviewTo_.node); target != nullptr)
                    other = portPoint(*target, wirePreviewTo_.portId, wirePreviewTo_.portIndex, false);
            }
            if (fixedValid)
                appendColoredLine(frame.edgeHighlights, fixed, other, 2.2 * inverseScale,
                                  alphaColor(accentColor_, 230));
        }
        const int rasterScale = std::max(
            1,
            static_cast<int>(std::ceil(viewScaleProperty_ * (window() ? window()->effectiveDevicePixelRatio() : 1.0))));
        const auto appendLabel = [&frame, &clip, this, rasterScale](QString key, QString text, QPointF position,
                                                                    const QColor& color) {
            if (frame.labels.size() < kMaxVisibleLabels &&
                QRectF(position, QSizeF(kAtlasCellWidth, kAtlasCellHeight)).intersects(clip))
                frame.labels.push_back({std::move(key), std::move(text), position, color, fontSize_, rasterScale});
        };
        for (const auto& node : nodeRecords_) {
            if (!node.rectangle.intersects(clip))
                continue;
            const QColor fill = categoryColorRecords_.value(node.category, QColor(QStringLiteral("#59646f")));
            appendRoundedRect(frame.bodies, node.rectangle, 3.0, fill);
            const bool selected =
                std::find(selectedNodeIds_.cbegin(), selectedNodeIds_.cend(), node.id) != selectedNodeIds_.cend();
            appendBorder(frame.outlines, node.rectangle, selected ? 2.0 : 1.0, selected ? accentColor_ : borderColor_);
            const QColor text = nodeTextColor(fill);
            const auto header = node.name.isEmpty() ? node.type : node.name;
            appendLabel(QStringLiteral("node:%1").arg(node.id), header, node.rectangle.topLeft() + QPointF(0, 2), text);

            const auto appendPort = [&](const PortRecord& port, int index, bool output) {
                const QPointF center = portPoint(node, port.id, index, output);
                const bool hovered = hoveredEndpoint_.node == node.id && hoveredEndpoint_.output == output &&
                                     (hoveredEndpoint_.portId == port.id || hoveredEndpoint_.portIndex == index);
                const bool mask = port.kind.compare(QStringLiteral("mask"), Qt::CaseInsensitive) == 0;
                QColor portFill = output ? (mask ? panelColor_ : fill) : (mask ? panelColor_ : mutedColor_);
                QColor portBorder = output ? nodeColor_ : (mask ? mutedColor_ : nodeColor_);
                if (hovered)
                    portFill = accentColor_;
                appendColoredCircle(frame.outlines, center, kPortRadius, portFill);
                appendCircleBorder(frame.outlines, center, kPortRadius, 1.0, portBorder);
            };
            for (int i = 0; i < node.inputs.size(); ++i)
                appendPort(node.inputs.at(i), i, false);
            for (int i = 0; i < node.outputs.size(); ++i)
                appendPort(node.outputs.at(i), i, true);
        }

        if (!wirePreviewValid_ && hoveredEndpointEdge_ != 0 && hoveredEndpoint_.node != 0) {
            const auto* node = nodeRecord(hoveredEndpoint_.node);
            if (node != nullptr) {
                const auto center =
                    portPoint(*node, hoveredEndpoint_.portId, hoveredEndpoint_.portIndex, hoveredEndpoint_.output);
                appendCircleBorder(frame.edgeHighlights, center, 7.0 * inverseScale, 2.0 * inverseScale, accentColor_);
            }
        }
    }
    updateColoredGeometry(scene->edges(), frame.edges);
    updateColoredGeometry(scene->edgeHighlights(), frame.edgeHighlights);
    updateColoredGeometry(scene->bodies(), frame.bodies);
    updateColoredGeometry(scene->outlines(), frame.outlines);
    if (frame.labels != scene->cachedLabels()) {
        auto nextLabels = makeLabelAtlas(window(), frame.labels);
        if (nextLabels != nullptr || frame.labels.isEmpty())
            scene->replaceLabels(std::move(nextLabels), frame.labels);
    }
    return created != nullptr ? created.release() : scene;
}

void GraphItem::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size())
        update();
}

}  // namespace nemo::ui
