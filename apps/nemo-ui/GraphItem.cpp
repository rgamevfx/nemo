#include "GraphItem.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QMetaType>
#include <QPainter>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QVariantMap>

#include <QStringList>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace nemo::ui {
namespace {

constexpr int kMaxParametersPerNode = 8;
constexpr int kMaxVisibleLabels = 256;
constexpr int kAtlasWidth = 1008;
constexpr int kAtlasCellWidth = 336;
constexpr int kAtlasCellHeight = 24;
constexpr qreal kGraphMargin = 16.0;
constexpr qreal kNodeWidth = 232.0;
constexpr qreal kNodeHeaderHeight = 56.0;
constexpr qreal kNodeParameterHeight = 17.0;
constexpr qreal kNodeColumnGap = 56.0;
constexpr qreal kNodeRowHeight = 240.0;
constexpr qreal kNodeRowGap = 48.0;

QString boundedText(QString text, int maxCharacters) {
    text.replace('\n', ' ');
    text.replace('\r', ' ');
    if (text.size() <= maxCharacters) {
        return text;
    }
    return text.left(std::max(1, maxCharacters - 3)) + QStringLiteral("...");
}

QString parameterDisplayText(const QVariant& value) {
    if (value.metaType().id() == QMetaType::QVariantList) {
        QStringList components;
        for (const auto& component : value.toList())
            components.push_back(component.toString());
        return components.join(QLatin1Char(' '));
    }
    if (value.metaType().id() == QMetaType::Bool)
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return value.toString();
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

struct VisibleEdge {
    qsizetype edge{};
    qsizetype from{};
    qsizetype to{};
    QPointF source;
    QPointF destination;
};

struct GraphFrame {
    QVector<QSGGeometry::Point2D> edges;
    QVector<QSGGeometry::Point2D> bodies;
    QVector<QSGGeometry::Point2D> outlines;
    QVector<VisibleEdge> visibleEdges;
    QVector<LabelSpec> labels;
};

class GraphSceneNode final : public QSGNode {
public:
    GraphSceneNode() {
        edges_ = makeGeometryNode(QColor(111, 159, 197));
        appendChildNode(edges_);
        bodies_ = makeGeometryNode(QColor(52, 58, 72));
        appendChildNode(bodies_);
        outlines_ = makeGeometryNode(QColor(98, 108, 124));
        appendChildNode(outlines_);
        frame_.labels.reserve(kMaxVisibleLabels);
    }
    GraphFrame& beginFrame() {
        frame_.edges.clear();
        frame_.bodies.clear();
        frame_.outlines.clear();
        frame_.visibleEdges.clear();
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
    const qreal top = rectangle.top() + kNodeHeaderHeight - 10.0;
    const qreal bottom = rectangle.bottom() - 8.0;
    const qreal y = std::clamp(top + std::max(0, port) * kNodeParameterHeight, top, bottom);
    return {output ? rectangle.right() : rectangle.left(), y};
}

QImage renderLabelAtlas(const QVector<LabelSpec>& labels, QVector<QRectF>* atlasRects, QSize* atlasSize) {
    if (labels.isEmpty()) {
        return {};
    }

    const QFont headerFont(QStringLiteral("Sans Serif"), 13, QFont::Bold);
    const QFont detailFont(QStringLiteral("Sans Serif"), 11);
    const QFontMetrics headerMetrics(headerFont);
    const QFontMetrics detailMetrics(detailFont);
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
        const bool header = label.key.startsWith(QStringLiteral("node-header"));
        const QFont& font = header ? headerFont : detailFont;
        const QFontMetrics& metrics = header ? headerMetrics : detailMetrics;
        const int column = i % columns;
        const int row = i / columns;
        const int x = column * kAtlasCellWidth + 5;
        const int y = row * kAtlasCellHeight;
        const int textWidth =
            label.key.startsWith(QStringLiteral("edge:")) ? kAtlasCellWidth - 10 : static_cast<int>(kNodeWidth) - 20;
        const QString text = metrics.elidedText(label.text, Qt::ElideRight, textWidth);
        const int advance = std::max(1, metrics.horizontalAdvance(text));
        painter.setFont(font);
        painter.setPen(label.color);
        painter.drawText(QPointF(x, y + metrics.ascent() + 2), text);
        atlasRects->push_back(QRectF(x, y, advance, kAtlasCellHeight));
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
        record.header = record.name.isEmpty() ? QStringLiteral("node #%1").arg(record.id) : record.name;
        record.detail = QStringLiteral("%1  (#%2)").arg(record.type, QString::number(record.id));

        const QVariantMap parameters = map.value(QStringLiteral("params")).toMap();
        int parameterCount = 0;
        for (auto it = parameters.cbegin(); it != parameters.cend() && parameterCount < kMaxParametersPerNode;
             ++it, ++parameterCount) {
            record.parameters.push_back({boundedText(it.key(), 24), boundedText(parameterDisplayText(it.value()), 42)});
        }
        if (parameters.size() > kMaxParametersPerNode) {
            record.parameters.push_back({QStringLiteral("..."), QStringLiteral("more parameters")});
        }
        record.rectangle = QRectF();
        nodeIndex_.insert(record.id, nodeRecords_.size());
        nodeRecords_.push_back(std::move(record));
    }

    const int columns =
        nodeRecords_.isEmpty() ? 1 : std::max(1, std::min(4, int(std::ceil(std::sqrt(nodeRecords_.size())))));
    for (int i = 0; i < nodeRecords_.size(); ++i) {
        auto& record = nodeRecords_[i];
        const int column = i % columns;
        const int row = i / columns;
        const qreal height = kNodeHeaderHeight + static_cast<qreal>(record.parameters.size()) * kNodeParameterHeight;
        record.rectangle = QRectF(kGraphMargin + column * (kNodeWidth + kNodeColumnGap),
                                  kGraphMargin + row * (kNodeRowHeight + kNodeRowGap), kNodeWidth, height);
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
        appendLine(frame.edges, source, destination, 3.0);
        frame.visibleEdges.push_back({index, from, to, source, destination});
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
        appendQuad(frame.bodies, node.rectangle);
        appendBorder(frame.outlines, node.rectangle, 1.0);
        appendLabel(QStringLiteral("node-header:%1").arg(i), node.header, node.rectangle.topLeft() + QPointF(10.0, 5.0),
                    QColor(240, 242, 245));
        appendLabel(QStringLiteral("node-detail:%1").arg(i), node.detail,
                    node.rectangle.topLeft() + QPointF(10.0, 26.0), QColor(174, 183, 197));
        for (int parameter = 0; parameter < node.parameters.size(); ++parameter) {
            const auto& pair = node.parameters.at(parameter);
            appendLabel(QStringLiteral("node-parameter:%1:%2").arg(i).arg(parameter),
                        QStringLiteral("%1: %2").arg(pair.first, pair.second),
                        node.rectangle.topLeft() + QPointF(10.0, 45.0 + parameter * kNodeParameterHeight),
                        QColor(192, 200, 210));
        }
    }
    for (const auto& visible : frame.visibleEdges) {
        const auto& edge = edgeRecords_.at(visible.edge);
        const QString text = QStringLiteral("#%1  %2:%3 -> %4:%5")
                                 .arg(edge.id)
                                 .arg(nodeRecords_.at(visible.from).name)
                                 .arg(edge.fromPort)
                                 .arg(nodeRecords_.at(visible.to).name)
                                 .arg(edge.toPort);
        appendLabel(QStringLiteral("edge:%1").arg(edge.id), boundedText(text, 80),
                    (visible.source + visible.destination) * 0.5 + QPointF(5.0, -8.0), QColor(182, 212, 232));
    }
    updateGeometry(scene->edges(), frame.edges);
    updateGeometry(scene->bodies(), frame.bodies);
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
