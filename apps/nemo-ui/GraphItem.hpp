#pragma once

#include <QHash>
#include <QPair>
#include <QQuickItem>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

namespace nemo::ui {

// Dense graph presentation item. The QVariant properties are received and
// parsed on the GUI thread into immutable-for-the-frame layout records. The
// scene graph owns its QSGGeometry/material/texture children and is rebuilt or
// updated only from updatePaintNode while Qt has blocked the GUI thread. No
// Document or controller state is read from the render thread.
// The host supplies its viewport in visibleRect; an empty viewport draws no
// content, including during minimization or an intermediate workspace layout.
class GraphItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList nodes READ nodes WRITE setNodes NOTIFY nodesChanged)
    Q_PROPERTY(QVariantList edges READ edges WRITE setEdges NOTIFY edgesChanged)
    Q_PROPERTY(QRectF visibleRect READ visibleRect WRITE setVisibleRect NOTIFY visibleRectChanged)

public:
    explicit GraphItem(QQuickItem* parent = nullptr);
    ~GraphItem() override;
    [[nodiscard]] QVariantList nodes() const { return nodesProperty_; }
    void setNodes(const QVariantList& nodes);
    [[nodiscard]] QVariantList edges() const { return edgesProperty_; }
    void setEdges(const QVariantList& edges);
    [[nodiscard]] QRectF visibleRect() const { return visibleRect_; }
    void setVisibleRect(QRectF rect);

signals:
    void nodesChanged();
    void edgesChanged();
    void visibleRectChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    struct NodeRecord {
        quint64 id{};
        QString name;
        QString type;
        QString header;
        QString detail;
        QVector<QPair<QString, QString>> parameters;
        QRectF rectangle;
    };
    struct EdgeRecord {
        quint64 id{};
        quint64 fromNode{};
        int fromPort{};
        quint64 toNode{};
        int toPort{};
    };

    void rebuildNodeRecords();
    void rebuildEdgeRecords();
    void updateImplicitSize();

    QVariantList nodesProperty_;
    QVariantList edgesProperty_;
    QRectF visibleRect_;
    QVector<NodeRecord> nodeRecords_;
    QVector<EdgeRecord> edgeRecords_;
    QHash<quint64, qsizetype> nodeIndex_;
    QSizeF contentSize_;
};

}  // namespace nemo::ui
