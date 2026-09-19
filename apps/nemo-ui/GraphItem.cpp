// The graph headers carry QML_ELEMENT, so the registration macro must be
// visible before them; QQuickItem would otherwise define it too late.
#include <QtQml/qqml.h>

#include "GraphItem.hpp"

#include "GraphGeometry.hpp"
#include "GraphHitTest.hpp"
#include "GraphScene.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QSGVertexColorMaterial>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

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
constexpr int kAtlasColumns = kAtlasWidth / kAtlasCellWidth;
// Screen-space stroke weights, converted by the view transform at the call site.
constexpr qreal kHighlightStrokeWidth = 4.0;
constexpr qreal kActiveStrokeWidth = 2.0;
constexpr qreal kOrdinaryStrokeWidth = 1.35;
constexpr qreal kWireStrokeWidth = 2.2;
constexpr qreal kEndpointRingRadius = 7.0;
constexpr qreal kEndpointRingThickness = 2.0;
constexpr qreal kRerouteRadius = 4.0;
constexpr qreal kRerouteHoverRadius = 6.0;
// How far outside the viewport a path is culled: enough that a stroke whose
// vertices sit just outside still covers the edge of the clip.
constexpr qreal kClipSlack = 8.0;

// The atlas identity: what the rasterised texture actually depends on. It holds
// no position and no identity of its own, so a moved card, a new transient
// position or a zoom step moves quads and never re-rasterises text.
struct LabelKey {
    QString text;
    QColor color;
    int fontSize{11};
    int rasterScale{1};
    int leftInset{6};
    int rightInset{6};

    friend bool operator==(const LabelKey&, const LabelKey&) = default;
};

[[nodiscard]] int atlasRows(qsizetype labelCount) {
    return static_cast<int>((labelCount + kAtlasColumns - 1) / kAtlasColumns);
}

// The one owner of the atlas cell grid, in device pixels at `rasterScale`.
[[nodiscard]] QRectF atlasCellRect(qsizetype index, int rasterScale) {
    const qsizetype column = index % kAtlasColumns;
    const qsizetype row = index / kAtlasColumns;
    return {static_cast<qreal>(column * kAtlasCellWidth * rasterScale),
            static_cast<qreal>(row * kAtlasCellHeight * rasterScale), kAtlasCellWidth * qreal(rasterScale),
            kAtlasCellHeight * qreal(rasterScale)};
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

// The uploaded label texture. Its texture coordinates are authored once, with
// the atlas; its geometry is a quad per label that every frame moves to the
// label's current position.
class LabelAtlasNode final : public QSGNode {
public:
    LabelAtlasNode(std::unique_ptr<QSGTexture> texture, int quads, int rasterScale)
        : texture_(std::move(texture)), rasterScale_(rasterScale), quads_(quads),
          geometryNode_(std::make_unique<QSGGeometryNode>()) {
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
        setQuadUvs();
    }
    ~LabelAtlasNode() override { removeChildNode(geometryNode_.get()); }

    [[nodiscard]] QSGGeometryNode* geometryNode() const { return geometryNode_.get(); }
    [[nodiscard]] int quadCount() const { return quads_; }

private:
    void setQuadUvs() {
        auto* geometry = geometryNode_->geometry();
        geometry->allocate(quads_ * 6);
        auto* vertices = geometry->vertexDataAsTexturedPoint2D();
        const qreal atlasWidth = kAtlasWidth * qreal(rasterScale_);
        const qreal atlasHeight = atlasRows(quads_) * kAtlasCellHeight * qreal(rasterScale_);
        for (int i = 0; i < quads_; ++i) {
            const QRectF source = atlasCellRect(i, rasterScale_);
            const auto u0 = float(source.left() / atlasWidth);
            const auto u1 = float(source.right() / atlasWidth);
            const auto v0 = float(source.top() / atlasHeight);
            const auto v1 = float(source.bottom() / atlasHeight);
            auto* out = vertices + i * 6;
            out[0].set(0.0f, 0.0f, u0, v0);
            out[1].set(float(kAtlasCellWidth), 0.0f, u1, v0);
            out[2].set(float(kAtlasCellWidth), float(kAtlasCellHeight), u1, v1);
            out[3].set(0.0f, 0.0f, u0, v0);
            out[4].set(float(kAtlasCellWidth), float(kAtlasCellHeight), u1, v1);
            out[5].set(0.0f, float(kAtlasCellHeight), u0, v1);
        }
        geometry->markVertexDataDirty();
        geometryNode_->markDirty(QSGNode::DirtyGeometry);
    }

    std::unique_ptr<QSGTexture> texture_;
    int rasterScale_{1};
    int quads_{};
    std::unique_ptr<QSGGeometryNode> geometryNode_;
};

struct GraphFrame {
    QVector<QSGGeometry::ColoredPoint2D> edges;
    QVector<QSGGeometry::ColoredPoint2D> edgeHighlights;
    QVector<QSGGeometry::ColoredPoint2D> bodies;
    QVector<QSGGeometry::ColoredPoint2D> outlines;
    // Visible labels as (identity, position): the keys decide whether the atlas
    // is re-rasterised, the positions only move the quads.
    QVector<LabelKey> labelKeys;
    QVector<QPointF> labelPositions;
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
        frame_.labelKeys.reserve(kMaxVisibleLabels);
        frame_.labelPositions.reserve(kMaxVisibleLabels);
    }
    GraphFrame& beginFrame() {
        frame_.edges.clear();
        frame_.edgeHighlights.clear();
        frame_.bodies.clear();
        frame_.outlines.clear();
        frame_.labelKeys.clear();
        frame_.labelPositions.clear();
        return frame_;
    }
    QSGGeometryNode* edges() const { return edges_; }
    QSGGeometryNode* edgeHighlights() const { return edgeHighlights_; }
    QSGGeometryNode* bodies() const { return bodies_; }
    QSGGeometryNode* outlines() const { return outlines_; }
    [[nodiscard]] LabelAtlasNode* labelNode() const { return labels_; }
    [[nodiscard]] const QVector<LabelKey>& cachedLabelKeys() const { return cachedLabelKeys_; }
    void replaceLabels(std::unique_ptr<LabelAtlasNode> next, const QVector<LabelKey>& keys) {
        const std::unique_ptr<LabelAtlasNode> previous(labels_);
        if (previous != nullptr)
            removeChildNode(previous.get());
        labels_ = next.release();
        if (labels_ != nullptr) {
            labels_->setFlag(QSGNode::OwnedByParent);
            appendChildNode(labels_);
        }
        cachedLabelKeys_ = keys;
    }

private:
    QSGGeometryNode* edges_{};
    QSGGeometryNode* edgeHighlights_{};
    QSGGeometryNode* bodies_{};
    QSGGeometryNode* outlines_{};
    LabelAtlasNode* labels_{};
    QVector<LabelKey> cachedLabelKeys_;
    GraphFrame frame_;
};
QImage renderLabelAtlas(const QVector<LabelKey>& keys, QSize* atlasSize) {
    const int rasterScale = keys.constFirst().rasterScale;
    *atlasSize = QSize(kAtlasWidth * rasterScale, atlasRows(keys.size()) * kAtlasCellHeight * rasterScale);
    QImage image(*atlasSize, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(std::max(1, keys.constFirst().fontSize));
    font.setWeight(QFont::Medium);
    const QFontMetrics metrics(font);
    QPainter painter(&image);
    painter.scale(rasterScale, rasterScale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);
    for (qsizetype i = 0; i < keys.size(); ++i) {
        const auto& key = keys.at(i);
        const QRectF cell = atlasCellRect(i, 1);
        const int contentWidth = kAtlasCellWidth - key.leftInset - key.rightInset;
        const QString text = metrics.elidedText(key.text, Qt::ElideRight, contentWidth);
        const int advance = std::max(1, metrics.horizontalAdvance(text));
        const auto centeredX = static_cast<int>(cell.left()) + key.leftInset + (contentWidth - advance) / 2;
        painter.setPen(key.color);
        painter.drawText(QPointF(centeredX, cell.top() + (kAtlasCellHeight - metrics.height()) / 2 + metrics.ascent()),
                         text);
    }
    painter.end();
    return image;
}

std::unique_ptr<LabelAtlasNode> makeLabelAtlas(QQuickWindow* window, const QVector<LabelKey>& keys) {
    if (window == nullptr || keys.isEmpty())
        return nullptr;
    QSize atlasSize;
    const QImage image = renderLabelAtlas(keys, &atlasSize);
    std::unique_ptr<QSGTexture> texture(window->createTextureFromImage(image));
    if (texture == nullptr)
        return nullptr;
    texture->setFiltering(QSGTexture::Linear);
    return std::make_unique<LabelAtlasNode>(std::move(texture), static_cast<int>(keys.size()),
                                            keys.constFirst().rasterScale);
}

// Moves every quad to the position its label is drawn at this frame. The
// texture coordinates authored with the atlas are untouched.
void syncLabelQuads(LabelAtlasNode* atlas, const QVector<QPointF>& positions) {
    if (atlas == nullptr || positions.size() != atlas->quadCount())
        return;
    auto* geometry = atlas->geometryNode()->geometry();
    auto* vertices = geometry->vertexDataAsTexturedPoint2D();
    for (qsizetype i = 0; i < positions.size(); ++i) {
        const QPointF position = positions.at(i);
        const auto x0 = float(position.x());
        const auto y0 = float(position.y());
        const auto x1 = float(position.x() + kAtlasCellWidth);
        const auto y1 = float(position.y() + kAtlasCellHeight);
        auto* out = vertices + i * 6;
        out[0].x = x0;
        out[0].y = y0;
        out[1].x = x1;
        out[1].y = y0;
        out[2].x = x1;
        out[2].y = y1;
        out[3].x = x0;
        out[3].y = y0;
        out[4].x = x1;
        out[4].y = y1;
        out[5].x = x0;
        out[5].y = y1;
    }
    geometry->markVertexDataDirty();
    atlas->geometryNode()->markDirty(QSGNode::DirtyGeometry);
}

}  // namespace

GraphItem::GraphItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setImplicitSize(520.0, 260.0);
}

GraphItem::~GraphItem() = default;

void GraphItem::setInteraction(GraphInteraction* interaction) {
    if (interaction_ == interaction)
        return;
    if (interaction_ != nullptr)
        disconnect(interaction_, nullptr, this, nullptr);
    interaction_ = interaction;
    if (interaction_ != nullptr) {
        const auto repaint = [this] { update(); };
        connect(interaction_, &GraphInteraction::sceneChanged, this, repaint);
        connect(interaction_, &GraphInteraction::viewChanged, this, repaint);
        connect(interaction_, &GraphInteraction::previewChanged, this, repaint);
        connect(interaction_, &GraphInteraction::selectionChanged, this, repaint);
        connect(interaction_, &GraphInteraction::hoverChanged, this, repaint);
    }
    emit interactionChanged();
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

void GraphItem::setVisibleRect(QRectF rect) {
    rect = rect.normalized();
    if (visibleRect_ == rect)
        return;
    visibleRect_ = rect;
    emit visibleRectChanged();
    update();
}

qreal GraphItem::viewScale() const {
    const qreal value = scale();
    return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

QSGNode* GraphItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) {
    std::unique_ptr<GraphSceneNode> created;
    auto* node = static_cast<GraphSceneNode*>(old);
    if (node == nullptr) {
        created = std::make_unique<GraphSceneNode>();
        node = created.get();
    }
    auto& frame = node->beginFrame();
    const GraphInteraction* interaction = interaction_;
    const GraphScene* scene = interaction == nullptr ? nullptr : interaction->scene().get();
    if (scene == nullptr) {
        // Nothing to draw and nothing to keep: an empty frame, and no stale
        // label atlas left over the empty scene.
        const QVector<QSGGeometry::ColoredPoint2D> none;
        updateColoredGeometry(node->edges(), none);
        updateColoredGeometry(node->edgeHighlights(), none);
        updateColoredGeometry(node->bodies(), none);
        updateColoredGeometry(node->outlines(), none);
        if (!node->cachedLabelKeys().isEmpty())
            node->replaceLabels(nullptr, {});
        return created != nullptr ? created.release() : node;
    }
    const GraphPreview& preview = interaction->preview();
    const GraphHover& hover = interaction->hover();
    const QStringList& selection = interaction->selection();
    const QRectF clip = visibleRect_;
    const qreal inverseScale = 1.0 / viewScale();
    const auto alphaColor = [](QColor color, int alpha) {
        color.setAlpha(std::clamp(alpha, 0, 255));
        return color;
    };
    // A moved card and its ports move together: the geometry stays in the
    // authored record and one offset is applied to every point of that node.
    const auto offsetFor = [&preview](const GraphNodeRecord& record) {
        const auto moved = preview.positions.constFind(record.id);
        return moved == preview.positions.constEnd() ? QPointF{} : *moved - record.position;
    };
    if (!clip.isEmpty()) {
        const auto appendSegments = [&](QVector<QSGGeometry::ColoredPoint2D>& vertices,
                                        const QVector<QPointF>& points, qreal width, const QColor& color) {
            for (qsizetype i = 1; i < points.size(); ++i)
                appendColoredLine(vertices, points.at(i - 1), points.at(i), width * inverseScale, color);
        };
        // The reroute candidate: the source port, the preview points, the
        // target port, in place of the authored route.
        const auto appendCandidate = [&](QVector<QSGGeometry::ColoredPoint2D>& vertices,
                                         const QVector<QPointF>& polyline, qreal width, const QColor& color) {
            QPointF previous = polyline.constFirst();
            for (const QPointF& point : preview.routePoints) {
                appendColoredLine(vertices, previous, point, width * inverseScale, color);
                previous = point;
            }
            appendColoredLine(vertices, previous, polyline.constLast(), width * inverseScale, color);
        };
        const auto pathBounds = [inverseScale](const QVector<QPointF>& points) {
            QPointF minimum = points.constFirst();
            QPointF maximum = minimum;
            for (const QPointF& point : points) {
                minimum.setX(std::min(minimum.x(), point.x()));
                minimum.setY(std::min(minimum.y(), point.y()));
                maximum.setX(std::max(maximum.x(), point.x()));
                maximum.setY(std::max(maximum.y(), point.y()));
            }
            const qreal slack = kClipSlack * inverseScale;
            return QRectF(minimum, maximum).normalized().adjusted(-slack, -slack, slack, slack);
        };
        for (const auto& edge : scene->edges) {
            if (preview.wireVisible && edge.id == preview.wireHiddenEdge)
                continue;
            const QVector<QPointF> polyline = routePolyline(*scene, edge);
            if (polyline.isEmpty())
                continue;
            if (!pathBounds(polyline).intersects(clip))
                continue;
            const bool highlighted = edge.id == hover.edge || edge.id == hover.endpoint.edge;
            const bool active = selection.contains(edge.from.node) || selection.contains(edge.to.node);
            QVector<QSGGeometry::ColoredPoint2D>& vertices =
                highlighted || active ? frame.edgeHighlights : frame.edges;
            if (highlighted || active) {
                const qreal width = highlighted ? kHighlightStrokeWidth : kActiveStrokeWidth;
                const QColor color = alphaColor(accentColor_, 242);
                if (preview.routeEdge == edge.id && !preview.routePoints.isEmpty())
                    appendCandidate(vertices, polyline, width, color);
                else
                    appendSegments(vertices, polyline, width, color);
            } else {
                appendSegments(vertices, polyline, kOrdinaryStrokeWidth, alphaColor(mutedColor_, 179));
            }
        }

        const qreal slack = kClipSlack * inverseScale;
        const QRectF dotClip = clip.adjusted(-slack, -slack, slack, slack);
        for (const auto& edge : scene->edges) {
            // A pulled pipe takes its dots with it: the replaced painter hid
            // the whole stale route, dots included.
            if (preview.wireVisible && edge.id == preview.wireHiddenEdge)
                continue;
            for (qsizetype index = 0; index < edge.route.size(); ++index) {
                const bool candidate = preview.routeEdge == edge.id && index < preview.routePoints.size();
                const QPointF position = candidate ? preview.routePoints.at(index) : edge.route.at(index);
                if (!dotClip.contains(position))
                    continue;
                // Only the dot being edited is enlarged: the replaced painter
                // required an active reroute gesture on that same dot, and the
                // candidate names the edge whose route the gesture edits.
                const bool dotHovered = hover.reroute.edge == edge.id && hover.reroute.routeIndex == index &&
                                        preview.routeEdge == edge.id;
                appendColoredCircle(frame.bodies, position,
                                    (dotHovered ? kRerouteHoverRadius : kRerouteRadius) * inverseScale,
                                    alphaColor(accentColor_, 242));
            }
        }

        if (preview.wireVisible) {
            appendColoredLine(frame.edgeHighlights, preview.wireFixed, preview.wireFree,
                              kWireStrokeWidth * inverseScale, alphaColor(accentColor_, 230));
        }

        const int rasterScale =
            std::max(1, static_cast<int>(std::ceil(window() ? window()->effectiveDevicePixelRatio() : 1.0)));
        for (const auto& record : scene->nodes) {
            const QPointF offset = offsetFor(record);
            const QRectF card = cardRect(record).translated(offset);
            if (!card.intersects(clip))
                continue;
            const QColor fill = categoryColorRecords_.value(record.category, QColor(QStringLiteral("#59646f")));
            appendRoundedRect(frame.bodies, card, 3.0, fill);
            const bool selected = selection.contains(record.id);
            appendBorder(frame.outlines, card, selected ? 2.0 : 1.0, selected ? accentColor_ : borderColor_);
            // Labels are gathered in node order, and only their identity and
            // position are kept: the atlas rebuild reads the identity alone.
            const QPointF labelPosition = card.topLeft() + QPointF(0, 2);
            if (frame.labelKeys.size() < kMaxVisibleLabels &&
                QRectF(labelPosition, QSizeF(kAtlasCellWidth, kAtlasCellHeight)).intersects(clip)) {
                const bool childScope = record.hasChildScope();
                frame.labelKeys.push_back({record.name.isEmpty() ? record.type : record.name, nodeTextColor(fill),
                                           fontSize_, rasterScale, childScope ? 7 : 6, childScope ? 27 : 6});
                frame.labelPositions.push_back(labelPosition);
            }

            const auto appendPort = [&](const GraphPortRecord& port, bool output) {
                const QPointF center = portPosition(record, port.index, output) + offset;
                // A port hover and a connection-end hover both light the port:
                // the replaced painter matched node, port and direction with no
                // kind test, and a pipe body resolves to one of its two ends.
                const bool portHovered = (hover.endpoint.kind == GraphHitKind::Port ||
                                          hover.endpoint.kind == GraphHitKind::Endpoint) &&
                                         hover.endpoint.node == record.id && hover.endpoint.output == output &&
                                         hover.endpoint.port == port.index;
                const bool mask = port.kind.compare(QStringLiteral("mask"), Qt::CaseInsensitive) == 0;
                const QColor portFill = output ? (mask ? panelColor_ : fill) : (mask ? panelColor_ : mutedColor_);
                const QColor portBorder = output ? nodeColor_ : (mask ? mutedColor_ : nodeColor_);
                appendColoredCircle(frame.outlines, center, kPortGlyphRadius, portHovered ? accentColor_ : portFill);
                appendCircleBorder(frame.outlines, center, kPortGlyphRadius, 1.0, portBorder);
            };
            for (const auto& port : record.inputs)
                appendPort(port, false);
            for (const auto& port : record.outputs)
                appendPort(port, true);
        }

        if (!preview.wireVisible && !hover.endpoint.edge.isEmpty() && !hover.endpoint.node.isEmpty()) {
            if (const GraphNodeRecord* record = scene->node(hover.endpoint.node); record != nullptr) {
                const QPointF center =
                    portPosition(*record, hover.endpoint.port, hover.endpoint.output) + offsetFor(*record);
                appendCircleBorder(frame.edgeHighlights, center, kEndpointRingRadius * inverseScale,
                                   kEndpointRingThickness * inverseScale, accentColor_);
            }
        }
    }
    updateColoredGeometry(node->edges(), frame.edges);
    updateColoredGeometry(node->edgeHighlights(), frame.edgeHighlights);
    updateColoredGeometry(node->bodies(), frame.bodies);
    updateColoredGeometry(node->outlines(), frame.outlines);
    if (frame.labelKeys == node->cachedLabelKeys()) {
        syncLabelQuads(node->labelNode(), frame.labelPositions);
    } else {
        const qulonglong previous = rasterizations_;
        std::unique_ptr<LabelAtlasNode> rebuilt;
        if (!frame.labelKeys.isEmpty()) {
            ++rasterizations_;
            rebuilt = makeLabelAtlas(window(), frame.labelKeys);
        }
        if (rebuilt != nullptr || frame.labelKeys.isEmpty()) {
            node->replaceLabels(std::move(rebuilt), frame.labelKeys);
            syncLabelQuads(node->labelNode(), frame.labelPositions);
        }
        if (rasterizations_ != previous)
            emit diagnosticsChanged();
    }
    return created != nullptr ? created.release() : node;
}

void GraphItem::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size())
        update();
}

}  // namespace nemo::ui
