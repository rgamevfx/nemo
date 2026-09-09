#pragma once

#include <QQuickItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QSGGeometryNode;

namespace nemo::ui {

// A bounded scene-graph surface for the timeline's dense ruler and source
// references. The GUI thread derives plain layout records in setClips(); the
// render-thread callback only consumes those records while Qt has blocked the
// GUI thread. No source decoder or persistent Document object crosses this
// presentation seam.
class TimelineItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList clips READ clips WRITE setClips NOTIFY clipsChanged)
    Q_PROPERTY(int frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(int frameCount READ frameCount WRITE setFrameCount NOTIFY frameCountChanged)
    Q_PROPERTY(qreal viewportY READ viewportY WRITE setViewportY NOTIFY viewportYChanged)
    Q_PROPERTY(qreal viewportHeight READ viewportHeight WRITE setViewportHeight NOTIFY viewportHeightChanged)
    Q_PROPERTY(QString selectedSource READ selectedSource WRITE setSelectedSource NOTIFY selectedSourceChanged)
    Q_PROPERTY(qreal playheadPosition READ playheadPosition NOTIFY playheadPositionChanged)
public:
    explicit TimelineItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QVariantList clips() const { return clips_; }
    void setClips(const QVariantList& clips);
    [[nodiscard]] int frame() const { return frame_; }
    void setFrame(int frame);
    [[nodiscard]] int frameCount() const { return frameCount_; }
    void setFrameCount(int frameCount);
    [[nodiscard]] qreal viewportY() const { return viewportY_; }
    void setViewportY(qreal viewportY);
    [[nodiscard]] qreal viewportHeight() const { return viewportHeight_; }
    void setViewportHeight(qreal viewportHeight);
    [[nodiscard]] QString selectedSource() const { return selectedSource_; }
    void setSelectedSource(const QString& source);
    [[nodiscard]] qreal playheadPosition() const;

signals:
    void clipsChanged();
    void frameChanged();
    void frameCountChanged();
    void viewportYChanged();
    void viewportHeightChanged();
    void selectedSourceChanged();
    void playheadPositionChanged();
    void frameSelected(int frame);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    struct ClipRecord {
        QString id;
        QString source;
        qreal start{};
        qreal end{};
        qlonglong offset{};
        qlonglong step{1};
        qlonglong sourceFrame{-1};
    };
    struct VisibleArea {
        qreal top{};
        qreal bottom{};
        int firstRow{};
        int lastRow{-1};
        bool ruler{};
    };

    [[nodiscard]] VisibleArea visibleArea() const;
    void drawFills(QSGGeometryNode* node, const VisibleArea& view) const;
    void drawLines(QSGGeometryNode* node, const VisibleArea& view) const;
    void drawPlayhead(QSGGeometryNode* node, const VisibleArea& view) const;

    void rebuildRecords();
    void selectAndScrub(const QPointF& position);
    [[nodiscard]] int frameAt(qreal x) const;
    [[nodiscard]] int rowAt(qreal y) const;

    QVariantList clips_;
    std::vector<ClipRecord> records_;
    int frame_{};
    int frameCount_{-1};
    qreal viewportY_{};
    qreal viewportHeight_{-1};
    QString selectedSource_;
    bool dragging_{};
};

}  // namespace nemo::ui
