#pragma once

#include <QColor>
#include <QHash>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace nemo::ui {

// Dense graph presentation item. QVariant properties are copied and parsed on
// the GUI thread into immutable-for-the-frame records. updatePaintNode only
// consumes those records while Qt has blocked the GUI thread: it never reads
// Document/controller state. The scene graph owns all QSG resources.
//
// Node snapshot keys: id (decimal string), type, name, params, category,
// authored x/y (top-left position), inputs and outputs (descriptor maps with
// index/name/kind and optional local x/y), and deletable. Edge snapshot keys:
// id, fromNode, fromPort, toNode, toPort, and route (maps with x/y).
class GraphItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList nodes READ nodes WRITE setNodes NOTIFY nodesChanged)
    Q_PROPERTY(QVariantList edges READ edges WRITE setEdges NOTIFY edgesChanged)
    Q_PROPERTY(QVariantMap categoryColors READ categoryColors WRITE setCategoryColors NOTIFY categoryColorsChanged)
    Q_PROPERTY(
        QVariantMap presentationStyle READ presentationStyle WRITE setPresentationStyle NOTIFY presentationStyleChanged)
    Q_PROPERTY(qreal viewScale READ viewScale WRITE setViewScale NOTIFY viewScaleChanged)
    Q_PROPERTY(QRectF visibleRect READ visibleRect WRITE setVisibleRect NOTIFY visibleRectChanged)
    Q_PROPERTY(QStringList selectedNodeIds READ selectedNodeIds WRITE setSelectedNodeIds NOTIFY selectedNodeIdsChanged)
    Q_PROPERTY(QString hoveredNodeId READ hoveredNodeId WRITE setHoveredNodeId NOTIFY hoveredNodeIdChanged)
    Q_PROPERTY(QString hoveredEdgeId READ hoveredEdgeId WRITE setHoveredEdgeId NOTIFY hoveredEdgeIdChanged)
    Q_PROPERTY(QVariantMap hoveredEndpoint READ hoveredEndpoint WRITE setHoveredEndpoint NOTIFY hoveredEndpointChanged)
    Q_PROPERTY(QVariantMap hoveredReroute READ hoveredReroute WRITE setHoveredReroute NOTIFY hoveredRerouteChanged)
    Q_PROPERTY(QVariantMap wirePreview READ wirePreview WRITE setWirePreview NOTIFY wirePreviewChanged)
public:
    explicit GraphItem(QQuickItem* parent = nullptr);
    ~GraphItem() override;

    [[nodiscard]] QVariantList nodes() const { return nodesProperty_; }
    void setNodes(const QVariantList& nodes);
    [[nodiscard]] QVariantList edges() const { return edgesProperty_; }
    void setEdges(const QVariantList& edges);
    [[nodiscard]] QVariantMap categoryColors() const { return categoryColorsProperty_; }
    void setCategoryColors(const QVariantMap& colors);
    [[nodiscard]] QVariantMap presentationStyle() const { return presentationStyleProperty_; }
    void setPresentationStyle(const QVariantMap& style);
    [[nodiscard]] qreal viewScale() const { return viewScaleProperty_; }
    void setViewScale(qreal scale);
    [[nodiscard]] QRectF visibleRect() const { return visibleRect_; }
    void setVisibleRect(QRectF rect);

    [[nodiscard]] QStringList selectedNodeIds() const { return selectedNodeIdsProperty_; }
    void setSelectedNodeIds(const QStringList& ids);
    [[nodiscard]] QString hoveredNodeId() const { return hoveredNodeIdProperty_; }
    void setHoveredNodeId(const QString& id);
    [[nodiscard]] QString hoveredEdgeId() const { return hoveredEdgeIdProperty_; }
    void setHoveredEdgeId(const QString& id);
    [[nodiscard]] QVariantMap hoveredEndpoint() const { return hoveredEndpointProperty_; }
    void setHoveredEndpoint(const QVariantMap& endpoint);
    [[nodiscard]] QVariantMap hoveredReroute() const { return hoveredRerouteProperty_; }
    void setHoveredReroute(const QVariantMap& reroute);
    [[nodiscard]] QVariantMap wirePreview() const { return wirePreviewProperty_; }
    void setWirePreview(const QVariantMap& preview);

    // Geometry queries expose the same snapshot used for drawing. GraphPanel
    // owns screen-space hit testing and gesture policy.
    Q_INVOKABLE QRectF nodeRect(const QVariant& nodeId) const;
    Q_INVOKABLE QPointF portPosition(const QVariant& nodeId, const QVariant& portId, bool output) const;

signals:
    void nodesChanged();
    void edgesChanged();
    void categoryColorsChanged();
    void presentationStyleChanged();
    void viewScaleChanged();
    void visibleRectChanged();
    void selectedNodeIdsChanged();
    void hoveredNodeIdChanged();
    void hoveredEdgeIdChanged();
    void hoveredEndpointChanged();
    void hoveredRerouteChanged();
    void wirePreviewChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

    struct PortRecord {
        QString id;
        QString name;
        QString kind;
        QPointF localPosition;
        bool hasPosition{};
    };
    struct NodeRecord {
        quint64 id{};
        QString name;
        QString type;
        QString category;
        QPointF position;
        QRectF rectangle;
        QVector<PortRecord> inputs;
        QVector<PortRecord> outputs;
        bool deletable{true};
    };
    struct EndpointRecord {
        quint64 node{};
        QString portId;
        int portIndex{-1};
        bool output{};
    };
    struct EdgeRecord {
        quint64 id{};
        EndpointRecord from;
        EndpointRecord to;
        QVector<QPointF> route;
    };
    struct RerouteRecord {
        quint64 edge{};
        int index{-1};
        QPointF position;
        bool selected{};
        bool hovered{};
    };

    void rebuildNodeRecords();
    void rebuildEdgeRecords();
    void rebuildInteractionRecords();
    void updateImplicitSize();
    [[nodiscard]] const NodeRecord* nodeRecord(quint64 id) const;
    [[nodiscard]] QPointF portPoint(const NodeRecord& node, const QString& portId, int portIndex, bool output) const;

    QVariantList nodesProperty_;
    QVariantList edgesProperty_;
    QVariantMap categoryColorsProperty_;
    QVariantMap presentationStyleProperty_;
    qreal viewScaleProperty_{1.0};
    QRectF visibleRect_;
    QStringList selectedNodeIdsProperty_;
    QString hoveredNodeIdProperty_;
    QString hoveredEdgeIdProperty_;
    QVariantMap hoveredEndpointProperty_;
    QVariantMap hoveredRerouteProperty_;
    QVariantMap wirePreviewProperty_;

    QVector<quint64> selectedNodeIds_;
    QVector<NodeRecord> nodeRecords_;
    QVector<EdgeRecord> edgeRecords_;
    QVector<RerouteRecord> rerouteRecords_;
    quint64 hoveredNodeId_{};
    quint64 hoveredEdgeId_{};
    QHash<quint64, qsizetype> nodeIndex_;
    QHash<QString, QColor> categoryColorRecords_;
    QColor accentColor_{QStringLiteral("#3485f6")};
    QColor borderColor_{QStringLiteral("#30343a")};
    QColor mutedColor_{QStringLiteral("#979ea8")};
    QColor panelColor_{QStringLiteral("#1e2023")};
    QColor nodeColor_{QStringLiteral("#2a2e33")};
    int fontSize_{11};
    EndpointRecord wirePreviewFrom_;
    EndpointRecord wirePreviewTo_;
    QPointF wirePreviewPointer_;
    quint64 wirePreviewHiddenEdge_{};
    bool wirePreviewFromInput_{};
    bool wirePreviewValid_{};
    EndpointRecord hoveredEndpoint_;
    quint64 hoveredEndpointEdge_{};
    quint64 hoveredRerouteEdge_{};
    int hoveredRerouteIndex_{-1};
    QSizeF contentSize_;
};

}  // namespace nemo::ui
