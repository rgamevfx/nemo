#include "GraphItem.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
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
constexpr qreal kNodeWidth = 112.0;
constexpr qreal kNodeHeight = 28.0;
constexpr qreal kNodeColumnGap = 24.0;
constexpr qreal kNodeRowGap = 32.0;

QString boundedText(QString text, int maxCharacters) {
    text.replace('\n', ' ');
    text.replace('\r', ' ');
    if (text.size() <= maxCharacters) {
        return text;
    }
    return text.left(std::max(1, maxCharacters - 3)) + QStringLiteral("...");
}

QSGGeometryNode* makeGeometryNode(const QColor& color) {
    auto node = std::make_unique<QSGGeometryNode>();
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node->setGeometry(geometry.release());
    node->setFlag(QSGNode::OwnsGeometry);
    node->setFlag(QSGNode::OwnedByParent);
    auto material = std::make_unique<QSGFlatColorMaterial>();
    material->setColor(color);
    node->setMaterial(material.release());
    node->setFlag(QSGNode::OwnsMaterial);
    return node.release();
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

void appendColoredQuad(QVector<QSGGeometry::ColoredPoint2D>& vertices, const QRectF& rectangle, const QColor& color) {
    const auto topLeft = rectangle.topLeft();
    const auto topRight = rectangle.topRight();
    const auto bottomLeft = rectangle.bottomLeft();
    const auto bottomRight = rectangle.bottomRight();
    const auto append = [&vertices, &color](QPointF point) {
        vertices.push_back({});
        vertices.back().set(float(point.x()), float(point.y()), color.red(), color.green(), color.blue(),
                            color.alpha());
    };
    append(topLeft);
    append(topRight);
    append(bottomRight);
    append(topLeft);
    append(bottomRight);
    append(bottomLeft);
}

void updateColoredGeometry(QSGGeometryNode* node, const QVector<QSGGeometry::ColoredPoint2D>& vertices) {
    auto* geometry = node->geometry();
    if (geometry->vertexCount() != vertices.size())
        geometry->allocate(static_cast<int>(vertices.size()));
    if (!vertices.isEmpty()) {
        auto* destination = geometry->vertexDataAsColoredPoint2D();
        std::copy(vertices.cbegin(), vertices.cend(), destination);
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
}

void appendQuad(QVector<QSGGeometry::Point2D>& vertices, const QRectF& rectangle) {
    const auto topLeft = rectangle.topLeft();
    const auto topRight = rectangle.topRight();
    const auto bottomLeft = rectangle.bottomLeft();
    const auto bottomRight = rectangle.bottomRight();
    vertices.push_back({});
    vertices.back().set(float(topLeft.x()), float(topLeft.y()));
    vertices.push_back({});
    vertices.back().set(float(topRight.x()), float(topRight.y()));
    vertices.push_back({});
    vertices.back().set(float(bottomRight.x()), float(bottomRight.y()));
    vertices.push_back({});
    vertices.back().set(float(topLeft.x()), float(topLeft.y()));
    vertices.push_back({});
    vertices.back().set(float(bottomRight.x()), float(bottomRight.y()));
    vertices.push_back({});
    vertices.back().set(float(bottomLeft.x()), float(bottomLeft.y()));
}

void appendBorder(QVector<QSGGeometry::Point2D>& vertices, QRectF rectangle, qreal thickness) {
    rectangle = rectangle.normalized();
    appendQuad(vertices, QRectF(rectangle.left(), rectangle.top(), rectangle.width(), thickness));
    appendQuad(vertices, QRectF(rectangle.left(), rectangle.bottom() - thickness, rectangle.width(), thickness));
    appendQuad(vertices, QRectF(rectangle.left(), rectangle.top() + thickness, thickness,
                                std::max<qreal>(0.0, rectangle.height() - thickness * 2.0)));
    appendQuad(vertices, QRectF(rectangle.right() - thickness, rectangle.top() + thickness, thickness,
                                std::max<qreal>(0.0, rectangle.height() - thickness * 2.0)));
}

void appendLine(QVector<QSGGeometry::Point2D>& vertices, QPointF from, QPointF to, qreal width) {
    const QPointF difference = to - from;
    const qreal length = std::hypot(difference.x(), difference.y());
    if (length < 0.001) {
        return;
    }
    const QPointF normal(-difference.y() * width / (2.0 * length), difference.x() * width / (2.0 * length));
    const QPointF a = from + normal;
    const QPointF b = to + normal;
    const QPointF c = to - normal;
    const QPointF d = from - normal;
    const auto offset = vertices.size();
    vertices.resize(offset + 6);
    vertices[offset + 0].set(float(a.x()), float(a.y()));
    vertices[offset + 1].set(float(b.x()), float(b.y()));
    vertices[offset + 2].set(float(c.x()), float(c.y()));
    vertices[offset + 3].set(float(a.x()), float(a.y()));
    vertices[offset + 4].set(float(c.x()), float(c.y()));
    vertices[offset + 5].set(float(d.x()), float(d.y()));
}

void updateGeometry(QSGGeometryNode* node, const QVector<QSGGeometry::Point2D>& vertices) {
    auto* geometry = node->geometry();
    if (geometry->vertexCount() != vertices.size()) {
        geometry->allocate(static_cast<int>(vertices.size()));
    }
    if (!vertices.isEmpty()) {
        auto* destination = geometry->vertexDataAsPoint2D();
        std::copy(vertices.cbegin(), vertices.cend(), destination);
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
}

struct LabelSpec {
    QString key;
    QString text;
    QPointF position;
    QColor color;
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
    // Reverse destruction releases geometry/material before their texture.
    std::unique_ptr<QSGTexture> texture_;
    std::unique_ptr<QSGGeometryNode> geometryNode_;
};

struct GraphFrame {
    QVector<QSGGeometry::Point2D> edges;
    QVector<QSGGeometry::ColoredPoint2D> bodies;
    QVector<QSGGeometry::Point2D> outlines;
    QVector<LabelSpec> labels;
};

class GraphSceneNode final : public QSGNode {
public:
    GraphSceneNode() {
        edges_ = makeGeometryNode(QColor(151, 158, 168, 150));
        appendChildNode(edges_);
        bodies_ = makeColoredGeometryNode();
        appendChildNode(bodies_);
        outlines_ = makeGeometryNode(QColor(98, 108, 124));
        appendChildNode(outlines_);
        frame_.labels.reserve(kMaxVisibleLabels);
    }
    GraphFrame& beginFrame() {
        frame_.edges.clear();
        frame_.bodies.clear();
        frame_.outlines.clear();
        frame_.labels.clear();
        return frame_;
    }
    [[nodiscard]] QSGGeometryNode* edges() const { return edges_; }
    [[nodiscard]] QSGGeometryNode* bodies() const { return bodies_; }
    [[nodiscard]] QSGGeometryNode* outlines() const { return outlines_; }
    [[nodiscard]] const QVector<LabelSpec>& cachedLabels() const { return cachedLabels_; }
    void replaceLabels(std::unique_ptr<LabelAtlasNode> next, const QVector<LabelSpec>& labels) {
        const std::unique_ptr<LabelAtlasNode> previous(labels_);
        if (previous != nullptr) {
            removeChildNode(previous.get());
        }
        labels_ = next.release();
        if (labels_ != nullptr) {
            labels_->setFlag(QSGNode::OwnedByParent);
            appendChildNode(labels_);
        }
        cachedLabels_ = labels;
    }

private:
    QSGGeometryNode* edges_{};
    QSGGeometryNode* bodies_{};
    QSGGeometryNode* outlines_{};
    LabelAtlasNode* labels_{};
    QVector<LabelSpec> cachedLabels_;
    GraphFrame frame_;
};

[[nodiscard]] QPointF pointForPort(const QRectF& rectangle, int port, bool output) {
    const qreal x = rectangle.left() + rectangle.width() * 0.5;
    return {x + (output ? 0.0 : port * 8.0), output ? rectangle.bottom() : rectangle.top()};
}

QImage renderLabelAtlas(const QVector<LabelSpec>& labels, QVector<QRectF>* atlasRects, QSize* atlasSize) {
    if (labels.isEmpty()) {
        return {};
    }

    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(11);
    const QFontMetrics metrics(font);
    const int columns = kAtlasWidth / kAtlasCellWidth;
    const int rows = (static_cast<int>(labels.size()) + columns - 1) / columns;
    *atlasSize = QSize(kAtlasWidth, rows * kAtlasCellHeight);
    QImage image(*atlasSize, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    atlasRects->clear();
    atlasRects->reserve(labels.size());

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < labels.size(); ++i) {
        const auto& label = labels.at(i);
        painter.setFont(font);
        const int column = i % columns;
        const int row = i / columns;
        const int y = row * kAtlasCellHeight;
        const int textWidth = static_cast<int>(kNodeWidth) - 12;
        const QString text = metrics.elidedText(label.text, Qt::ElideRight, textWidth);
        const int advance = std::max(1, metrics.horizontalAdvance(text));
        const int centeredX = column * kAtlasCellWidth + (kAtlasCellWidth - advance) / 2;
        painter.setPen(label.color);
        painter.drawText(QPointF(centeredX, y + (kAtlasCellHeight - metrics.height()) / 2 + metrics.ascent()), text);
        atlasRects->push_back(QRectF(column * kAtlasCellWidth, y, kAtlasCellWidth, kAtlasCellHeight));
    }
    painter.end();
    return image;
}

std::unique_ptr<LabelAtlasNode> makeLabelAtlas(QQuickWindow* window, const QVector<LabelSpec>& labels) {
    if ((window == nullptr) || labels.isEmpty()) {
        return nullptr;
    }

    QVector<QRectF> atlasRects;
    QSize atlasSize;
    const QImage image = renderLabelAtlas(labels, &atlasRects, &atlasSize);
    if (image.isNull()) {
        return nullptr;
    }
    std::unique_ptr<QSGTexture> texture(window->createTextureFromImage(image));
    if (texture == nullptr) {
        return nullptr;
    }
    texture->setFiltering(QSGTexture::Linear);
    auto atlas = std::make_unique<LabelAtlasNode>(std::move(texture));

    auto* geometry = atlas->geometryNode()->geometry();
    geometry->allocate(static_cast<int>(labels.size()) * 6);
    auto* vertices = geometry->vertexDataAsTexturedPoint2D();
    for (qsizetype i = 0; i < labels.size(); ++i) {
        const auto& label = labels.at(i);
        const QRectF source = atlasRects.at(i);
        const QSizeF size = source.size();
        const auto u0 = float(source.left() / atlasSize.width());
        const auto u1 = float(source.right() / atlasSize.width());
        const auto v0 = float(source.top() / atlasSize.height());
        const auto v1 = float(source.bottom() / atlasSize.height());
        auto* out = vertices + i * 6;
        out[0].set(float(label.position.x()), float(label.position.y()), u0, v0);
        out[1].set(float(label.position.x() + size.width()), float(label.position.y()), u1, v0);
        out[2].set(float(label.position.x() + size.width()), float(label.position.y() + size.height()), u1, v1);
        out[3].set(float(label.position.x()), float(label.position.y()), u0, v0);
        out[4].set(float(label.position.x() + size.width()), float(label.position.y() + size.height()), u1, v1);
        out[5].set(float(label.position.x()), float(label.position.y() + size.height()), u0, v1);
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
    if (nodesProperty_ == nodes) {
        return;
    }
    nodesProperty_ = nodes;
    rebuildNodeRecords();
    emit nodesChanged();
    update();
}

void GraphItem::setEdges(const QVariantList& edges) {
    if (edgesProperty_ == edges) {
        return;
    }
    edgesProperty_ = edges;
    rebuildEdgeRecords();
    emit edgesChanged();
    update();
}

void GraphItem::setCategoryColors(const QVariantMap& colors) {
    if (categoryColors_ == colors) {
        return;
    }
    categoryColors_ = colors;
    emit categoryColorsChanged();
    update();
}

void GraphItem::setVisibleRect(QRectF rect) {
    rect = rect.normalized();
    if (visibleRect_ == rect) {
        return;
    }
    visibleRect_ = rect;
    emit visibleRectChanged();
    update();
}

void GraphItem::rebuildNodeRecords() {
    nodeRecords_.clear();
    nodeRecords_.reserve(nodesProperty_.size());
    nodeIndex_.clear();
    for (const auto& value : nodesProperty_) {
        const QVariantMap map = value.toMap();
        NodeRecord record;
        record.id = map.value(QStringLiteral("id")).toULongLong();
        record.name = boundedText(map.value(QStringLiteral("name")).toString(), 48);
        record.type = boundedText(map.value(QStringLiteral("type")).toString(), 32);
        record.category = map.value(QStringLiteral("category")).toString();
        record.header = record.name.isEmpty() ? record.type : record.name;
        record.inputs = map.value(QStringLiteral("inputs")).toInt();
        record.outputs = map.value(QStringLiteral("outputs")).toInt();

        nodeIndex_.insert(record.id, nodeRecords_.size());
        nodeRecords_.push_back(std::move(record));
    }

    const int columns =
        nodeRecords_.isEmpty() ? 1 : std::max(1, std::min(8, int(std::ceil(std::sqrt(nodeRecords_.size())))));
    for (int i = 0; i < nodeRecords_.size(); ++i) {
        auto& record = nodeRecords_[i];
        const int column = i % columns;
        const int row = i / columns;
        record.rectangle = QRectF(kGraphMargin + column * (kNodeWidth + kNodeColumnGap),
                                  kGraphMargin + row * (kNodeHeight + kNodeRowGap), kNodeWidth, kNodeHeight);
    }
    updateImplicitSize();
}

void GraphItem::rebuildEdgeRecords() {
    edgeRecords_.clear();
    edgeRecords_.reserve(edgesProperty_.size());
    for (const auto& value : edgesProperty_) {
        const QVariantMap map = value.toMap();
        EdgeRecord record;
        record.id = map.value(QStringLiteral("id")).toULongLong();
        record.fromNode = map.value(QStringLiteral("fromNode")).toULongLong();
        record.fromPort = std::max(0, map.value(QStringLiteral("fromPort")).toInt());
        record.toNode = map.value(QStringLiteral("toNode")).toULongLong();
        record.toPort = std::max(0, map.value(QStringLiteral("toPort")).toInt());
        edgeRecords_.push_back(record);
    }
}

void GraphItem::updateImplicitSize() {
    QSizeF next(520.0, 260.0);
    for (const auto& record : nodeRecords_) {
        next.setWidth(std::max(next.width(), record.rectangle.right() + kGraphMargin));
        next.setHeight(std::max(next.height(), record.rectangle.bottom() + kGraphMargin));
    }
    if (contentSize_ == next) {
        return;
    }
    contentSize_ = next;
    setImplicitSize(contentSize_.width(), contentSize_.height());
}

QSGNode* GraphItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) {
    std::unique_ptr<GraphSceneNode> created;
    auto* scene = static_cast<GraphSceneNode*>(old);
    if (scene == nullptr) {
        created = std::make_unique<GraphSceneNode>();
        scene = created.get();
    }
    const QRectF clip = visibleRect_.intersected(QRectF(0.0, 0.0, contentSize_.width(), contentSize_.height()));
    auto& frame = scene->beginFrame();
    for (qsizetype index = 0; index < edgeRecords_.size(); ++index) {
        const auto& edge = edgeRecords_.at(index);
        const auto from = nodeIndex_.value(edge.fromNode, -1);
        const auto to = nodeIndex_.value(edge.toNode, -1);
        if (from < 0 || to < 0) {
            continue;
        }
        const auto source = pointForPort(nodeRecords_.at(from).rectangle, edge.fromPort, true);
        const auto destination = pointForPort(nodeRecords_.at(to).rectangle, edge.toPort, false);
        const QRectF bounds(source, destination);
        if (!bounds.normalized().adjusted(-6.0, -6.0, 6.0, 6.0).intersects(clip)) {
            continue;
        }
        appendLine(frame.edges, source, destination, 1.25);
    }
    auto appendLabel = [&frame, &clip](QString key, QString text, QPointF position, QColor color) {
        if (frame.labels.size() < kMaxVisibleLabels &&
            QRectF(position, QSizeF(kAtlasCellWidth, kAtlasCellHeight)).intersects(clip)) {
            frame.labels.push_back({std::move(key), std::move(text), position, color});
        }
    };
    for (qsizetype i = 0; i < nodeRecords_.size(); ++i) {
        const auto& node = nodeRecords_.at(i);
        if (!node.rectangle.intersects(clip)) {
            continue;
        }
        QColor fill(categoryColors_.value(node.category).toString());
        if (!fill.isValid())
            fill = QColor(QStringLiteral("#59646f"));
        const QColor text =
            fill.lightnessF() > 0.62F ? QColor(QStringLiteral("#1c2025")) : QColor(QStringLiteral("#f1f4f7"));
        appendColoredQuad(frame.bodies, node.rectangle, fill);
        appendBorder(frame.outlines, node.rectangle, 1.0);
        const auto appendPorts = [&](int count, bool output) {
            static constexpr QPointF ring[] = {{3, 0},  {2.12, 2.12},   {0, 3},  {-2.12, 2.12},
                                               {-3, 0}, {-2.12, -2.12}, {0, -3}, {2.12, -2.12}};
            for (int port = 0; port < count; ++port) {
                const auto center = pointForPort(node.rectangle, port, output);
                for (int segment = 0; segment < 8; ++segment)
                    appendLine(frame.outlines, center + ring[segment], center + ring[(segment + 1) % 8], 1.0);
            }
        };
        appendPorts(node.inputs, false);
        appendPorts(node.outputs, true);
        appendLabel(QStringLiteral("node-header:%1").arg(i), node.header, node.rectangle.topLeft() + QPointF(0.0, 2.0),
                    text);
    }
    updateGeometry(scene->edges(), frame.edges);
    updateColoredGeometry(scene->bodies(), frame.bodies);
    updateGeometry(scene->outlines(), frame.outlines);
    if (frame.labels != scene->cachedLabels()) {
        auto nextLabels = makeLabelAtlas(window(), frame.labels);
        if (nextLabels != nullptr || frame.labels.isEmpty()) {
            scene->replaceLabels(std::move(nextLabels), frame.labels);
        }
    }
    return created != nullptr ? created.release() : scene;
}

void GraphItem::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        update();
    }
}

}  // namespace nemo::ui
