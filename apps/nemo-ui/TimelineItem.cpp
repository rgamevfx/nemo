#include "TimelineItem.hpp"

#include <QColor>
#include <QMouseEvent>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <cmath>
#include <memory>

namespace nemo::ui {
namespace {
constexpr qreal kRulerHeight = 28.0;
constexpr qreal kRowsTop = 30.0;
constexpr qreal kStripHeight = 38.0;
constexpr qreal kRowGap = 2.0;
constexpr qreal kRowPitch = kStripHeight + kRowGap;
constexpr qreal kLabelWidth = 132.0;
constexpr int kDefaultTimelineLength = 240;

void appendRect(QSGGeometry::ColoredPoint2D*& vertices, const QRectF& rect, const QColor& color) {
    const auto r = static_cast<unsigned char>(color.red());
    const auto g = static_cast<unsigned char>(color.green());
    const auto b = static_cast<unsigned char>(color.blue());
    const auto a = static_cast<unsigned char>(color.alpha());
    vertices[0].set(static_cast<float>(rect.left()), static_cast<float>(rect.top()), r, g, b, a);
    vertices[1].set(static_cast<float>(rect.right()), static_cast<float>(rect.top()), r, g, b, a);
    vertices[2].set(static_cast<float>(rect.right()), static_cast<float>(rect.bottom()), r, g, b, a);
    vertices[3].set(static_cast<float>(rect.left()), static_cast<float>(rect.top()), r, g, b, a);
    vertices[4].set(static_cast<float>(rect.right()), static_cast<float>(rect.bottom()), r, g, b, a);
    vertices[5].set(static_cast<float>(rect.left()), static_cast<float>(rect.bottom()), r, g, b, a);
    vertices += 6;
}

void appendLine(QSGGeometry::Point2D*& vertices, const QPointF& from, const QPointF& to) {
    vertices[0].set(static_cast<float>(from.x()), static_cast<float>(from.y()));
    vertices[1].set(static_cast<float>(to.x()), static_cast<float>(to.y()));
    vertices += 2;
}

std::unique_ptr<QSGGeometryNode> makeColoredNode() {
    auto node = std::make_unique<QSGGeometryNode>();
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node->setGeometry(geometry.release());
    node->setFlag(QSGNode::OwnsGeometry);
    auto material = std::make_unique<QSGVertexColorMaterial>();
    node->setMaterial(material.release());
    node->setFlag(QSGNode::OwnsMaterial);
    node->setFlag(QSGNode::OwnedByParent);
    return node;
}

std::unique_ptr<QSGGeometryNode> makeLineNode(const QColor& color) {
    auto node = std::make_unique<QSGGeometryNode>();
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawLines);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node->setGeometry(geometry.release());
    node->setFlag(QSGNode::OwnsGeometry);
    auto material = std::make_unique<QSGFlatColorMaterial>();
    material->setColor(color);
    node->setMaterial(material.release());
    node->setFlag(QSGNode::OwnsMaterial);
    node->setFlag(QSGNode::OwnedByParent);
    return node;
}

QSGGeometry* prepareGeometry(QSGGeometryNode* node, int vertices) {
    auto* geometry = node->geometry();
    if (geometry->vertexCount() != vertices) {
        geometry->allocate(vertices);
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
    return geometry;
}

}  // namespace

TimelineItem::TimelineItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setKeepMouseGrab(true);
    setImplicitHeight(kRowsTop + kRowPitch + 6.0);
}

void TimelineItem::setClips(const QVariantList& clips) {
    if (clips_ == clips) {
        return;
    }
    clips_ = clips;
    rebuildRecords();
    emit clipsChanged();
    update();
}

void TimelineItem::rebuildRecords() {
    records_.clear();
    records_.reserve(clips_.size());
    for (const auto& value : clips_) {
        const auto map = value.toMap();
        ClipRecord record;
        record.id = map.value(QStringLiteral("id")).toString();
        record.source = map.value(QStringLiteral("source")).toString();
        if (record.source.isEmpty()) {
            record.source = record.id;
        }
        record.start = map.value(QStringLiteral("start"), 0).toDouble();
        record.end = map.value(QStringLiteral("end"), -1).toDouble();
        record.offset = map.value(QStringLiteral("offset"), 0).toLongLong();
        record.step = map.value(QStringLiteral("step"), 1).toLongLong();
        record.sourceFrame = map.value(QStringLiteral("sourceFrame"), -1).toLongLong();
        records_.push_back(std::move(record));
    }

    const auto selected = std::find_if(records_.cbegin(), records_.cend(),
                                       [&](const ClipRecord& record) { return record.source == selectedSource_; });
    if (records_.empty()) {
        if (!selectedSource_.isEmpty()) {
            selectedSource_.clear();
            emit selectedSourceChanged();
        }
    } else if (selected == records_.cend()) {
        selectedSource_ = records_.front().source;
        emit selectedSourceChanged();
    }
    setImplicitHeight(kRowsTop + static_cast<qreal>(std::max<std::size_t>(1, records_.size())) * kRowPitch + 6.0);
}

void TimelineItem::setFrame(int frame) {
    frame = std::max(0, frame);
    if (frameCount_ > 0) {
        frame = std::min(frame, frameCount_ - 1);
    }
    if (frame == frame_) {
        return;
    }
    frame_ = frame;
    emit frameChanged();
    emit playheadPositionChanged();
    update();
}

void TimelineItem::setFrameCount(int frameCount) {
    frameCount = frameCount > 0 ? frameCount : -1;
    if (frameCount == frameCount_) {
        return;
    }
    frameCount_ = frameCount;
    if (frameCount_ > 0 && frame_ >= frameCount_) {
        setFrame(frameCount_ - 1);
    }
    emit frameCountChanged();
    emit playheadPositionChanged();
    update();
}

void TimelineItem::setViewportY(qreal viewportY) {
    viewportY = std::max<qreal>(0.0, viewportY);
    if (qFuzzyCompare(viewportY_, viewportY)) {
        return;
    }
    viewportY_ = viewportY;
    emit viewportYChanged();
    update();
}

void TimelineItem::setViewportHeight(qreal viewportHeight) {
    if (qFuzzyCompare(viewportHeight_, viewportHeight)) {
        return;
    }
    viewportHeight_ = viewportHeight;
    emit viewportHeightChanged();
    update();
}

void TimelineItem::setSelectedSource(const QString& source) {
    if (source == selectedSource_) {
        return;
    }
    if (!source.isEmpty()) {
        const auto it = std::find_if(records_.cbegin(), records_.cend(),
                                     [&](const ClipRecord& record) { return record.source == source; });
        if (it == records_.cend()) {
            return;
        }
    }
    selectedSource_ = source;
    emit selectedSourceChanged();
    update();
}

void TimelineItem::setBackgroundColor(const QColor& color) {
    if (backgroundColor_ == color)
        return;
    backgroundColor_ = color;
    emit colorsChanged();
    update();
}

void TimelineItem::setPanelColor(const QColor& color) {
    if (panelColor_ == color)
        return;
    panelColor_ = color;
    emit colorsChanged();
    update();
}

void TimelineItem::setHeaderColor(const QColor& color) {
    if (headerColor_ == color)
        return;
    headerColor_ = color;
    emit colorsChanged();
    update();
}

void TimelineItem::setBorderColor(const QColor& color) {
    if (borderColor_ == color)
        return;
    borderColor_ = color;
    emit colorsChanged();
    update();
}

void TimelineItem::setAccentColor(const QColor& color) {
    if (accentColor_ == color)
        return;
    accentColor_ = color;
    emit colorsChanged();
    update();
}

void TimelineItem::setRaisedColor(const QColor& color) {
    if (raisedColor_ == color)
        return;
    raisedColor_ = color;
    emit colorsChanged();
    update();
}

qreal TimelineItem::playheadPosition() const {
    const int length = frameCount_ > 1 ? frameCount_ : kDefaultTimelineLength;
    if (width() <= kLabelWidth) {
        return std::max<qreal>(0.0, width());
    }
    const qreal timelineWidth = width() - kLabelWidth;
    return kLabelWidth +
           timelineWidth * static_cast<qreal>(std::clamp(frame_, 0, length - 1)) / static_cast<qreal>(length - 1);
}

int TimelineItem::frameAt(qreal x) const {
    const int length = frameCount_ > 1 ? frameCount_ : kDefaultTimelineLength;
    if (width() <= kLabelWidth) {
        return 0;
    }
    const qreal normalized = std::clamp((x - kLabelWidth) / (width() - kLabelWidth), 0.0, 1.0);
    return std::clamp(static_cast<int>(std::llround(normalized * static_cast<qreal>(length - 1))), 0, length - 1);
}

int TimelineItem::rowAt(qreal y) const {
    if (y < kRowsTop) {
        return -1;
    }
    const auto row = static_cast<int>(std::floor((y - kRowsTop) / kRowPitch));
    if (row < 0 || static_cast<std::size_t>(row) >= records_.size()) {
        return -1;
    }
    const qreal rowTop = kRowsTop + row * kRowPitch;
    return y <= rowTop + kStripHeight ? row : -1;
}

void TimelineItem::selectAndScrub(const QPointF& position) {
    const int row = rowAt(position.y());
    if (row >= 0) {
        setSelectedSource(records_[static_cast<std::size_t>(row)].source);
    }
    emit frameSelected(frameAt(position.x()));
}

void TimelineItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    dragging_ = true;
    setKeepMouseGrab(true);
    selectAndScrub(event->position());
    event->accept();
}

void TimelineItem::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        event->ignore();
        return;
    }
    selectAndScrub(event->position());
    event->accept();
}

void TimelineItem::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        setKeepMouseGrab(false);
        event->accept();
        return;
    }
    event->ignore();
}

void TimelineItem::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    emit playheadPositionChanged();
    update();
}

TimelineItem::VisibleArea TimelineItem::visibleArea() const {
    VisibleArea view;
    if (width() <= 0.0 || height() <= 0.0) {
        return view;
    }
    const qreal contentHeight = implicitHeight();
    view.top = std::clamp(viewportY_, 0.0, contentHeight);
    view.bottom = contentHeight;
    if (viewportHeight_ >= 0.0) {
        view.bottom = std::clamp(viewportY_ + viewportHeight_, view.top, contentHeight);
    }
    view.ruler = view.top < kRulerHeight && view.bottom > 0.0;
    if (view.bottom > kRowsTop && !records_.empty()) {
        view.firstRow = std::max(0, static_cast<int>(std::floor((view.top - kRowsTop) / kRowPitch)) - 1);
        view.lastRow = std::min(static_cast<int>(records_.size()) - 1,
                                static_cast<int>(std::ceil((view.bottom - kRowsTop) / kRowPitch)));
    }
    return view;
}

void TimelineItem::drawFills(QSGGeometryNode* node, const VisibleArea& view) const {
    const int rows = std::max(0, view.lastRow - view.firstRow + 1);
    const int rectangles = static_cast<int>(view.ruler) + rows * 3;
    auto* geometry = prepareGeometry(node, rectangles * 6);
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    if (view.ruler)
        appendRect(vertices, QRectF(0.0, 0.0, width(), kRulerHeight), headerColor_);

    const int length = frameCount_ > 0 ? frameCount_ : kDefaultTimelineLength;
    const qreal labelWidth = std::min(kLabelWidth, width());
    const qreal timelineWidth = std::max<qreal>(1.0, width() - labelWidth);
    for (int row = view.firstRow; row <= view.lastRow; ++row) {
        const auto& record = records_[static_cast<std::size_t>(row)];
        const qreal y = kRowsTop + row * kRowPitch;
        const bool selected = record.source == selectedSource_;
        appendRect(vertices, QRectF(0.0, y, labelWidth, kStripHeight), selected ? raisedColor_ : panelColor_);
        appendRect(vertices, QRectF(labelWidth, y, timelineWidth, kStripHeight), backgroundColor_);

        qreal left = labelWidth;
        qreal right = width();
        QColor coverage = selected ? accentColor_ : raisedColor_;
        if (record.end > record.start && frameCount_ > 0) {
            left = labelWidth +
                   timelineWidth * std::clamp(record.start / static_cast<qreal>(std::max(1, length - 1)), 0.0, 1.0);
            right = labelWidth + timelineWidth * std::clamp(record.end / static_cast<qreal>(length), 0.0, 1.0);
            right = std::max(left + 1.0, right);
        }
        appendRect(vertices, QRectF(left, y + 8.0, std::max<qreal>(0.0, std::min(width(), right) - left), 22.0),
                   coverage);
    }
}

void TimelineItem::drawLines(QSGGeometryNode* node, const VisibleArea& view) const {
    const int rows = std::max(0, view.lastRow - view.firstRow + 1);
    const int count = (view.ruler ? 10 : 0) + rows * 4;
    auto* vertices = prepareGeometry(node, count * 2)->vertexDataAsPoint2D();
    if (view.ruler) {
        for (int tick = 0; tick < 9; ++tick) {
            const qreal x = kLabelWidth + (width() - kLabelWidth) * tick / 8.0;
            appendLine(vertices, QPointF(x, 0.0), QPointF(x, kRulerHeight));
        }
        appendLine(vertices, QPointF(kLabelWidth, 0.0), QPointF(kLabelWidth, height()));
    }
    for (int row = view.firstRow; row <= view.lastRow; ++row) {
        const qreal y = kRowsTop + row * kRowPitch;
        appendLine(vertices, QPointF(0.0, y), QPointF(width(), y));
        appendLine(vertices, QPointF(0.0, y + kStripHeight), QPointF(width(), y + kStripHeight));
        appendLine(vertices, QPointF(0.0, y), QPointF(0.0, y + kStripHeight));
        appendLine(vertices, QPointF(width(), y), QPointF(width(), y + kStripHeight));
    }
}

void TimelineItem::drawPlayhead(QSGGeometryNode* node, const VisibleArea& view) const {
    const qreal playhead = playheadPosition();
    const bool visible = playhead >= kLabelWidth && playhead <= width() && view.bottom > view.top;
    auto* vertices = prepareGeometry(node, visible ? 2 : 0)->vertexDataAsPoint2D();
    if (visible)
        appendLine(vertices, QPointF(playhead, view.top), QPointF(playhead, view.bottom));
}

QSGNode* TimelineItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) {
    // Three persistent batches; only vertex storage changes with the viewport.
    // An unfinished first construction remains RAII-owned.
    auto* root = old;
    if (root == nullptr) {
        auto created = std::make_unique<QSGNode>();
        created->appendChildNode(makeColoredNode().release());
        created->appendChildNode(makeLineNode(borderColor_).release());
        created->appendChildNode(makeLineNode(accentColor_).release());
        root = created.release();
    }
    const auto view = visibleArea();
    auto* fills = static_cast<QSGGeometryNode*>(root->firstChild());
    auto* lines = static_cast<QSGGeometryNode*>(fills->nextSibling());
    auto* playhead = static_cast<QSGGeometryNode*>(lines->nextSibling());
    static_cast<QSGFlatColorMaterial*>(lines->material())->setColor(borderColor_);
    static_cast<QSGFlatColorMaterial*>(playhead->material())->setColor(accentColor_);
    lines->markDirty(QSGNode::DirtyMaterial);
    playhead->markDirty(QSGNode::DirtyMaterial);
    drawFills(fills, view);
    drawLines(lines, view);
    drawPlayhead(playhead, view);
    return root;
}

}  // namespace nemo::ui
