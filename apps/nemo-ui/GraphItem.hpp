#pragma once

#include "GraphInteraction.hpp"

#include <QtQml/qqmlregistration.h>

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QVariantMap>

namespace nemo::ui {

// Dense graph presentation item. It consumes the interaction core's scene, the
// geometry functions and the active session's transient preview; it computes no
// layout of its own and holds no interaction state. Everything it draws comes
// from records the core already resolved, so the port the painter draws is the
// port the pick acquires at every zoom level.
//
// The vertices are split by how often they change. Cards and their ports are a
// function of the network, the theme and the selection, so they are built once
// and kept; the pipes, the cards a gesture is moving and the port under the
// pointer are rebuilt per frame and cost what they move. Nothing is
// re-rasterised because a node moved: the label atlas identity is the label set
// and the device pixel ratio, never the view scale or a position, so a drag
// moves quads rather than re-uploading a texture.
class GraphItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(nemo::ui::GraphInteraction* interaction READ interaction WRITE setInteraction NOTIFY interactionChanged)
    Q_PROPERTY(QVariantMap categoryColors READ categoryColors WRITE setCategoryColors NOTIFY categoryColorsChanged)
    Q_PROPERTY(
        QVariantMap presentationStyle READ presentationStyle WRITE setPresentationStyle NOTIFY presentationStyleChanged)
    Q_PROPERTY(QRectF visibleRect READ visibleRect WRITE setVisibleRect NOTIFY visibleRectChanged)
    // Cost diagnostic: how many label atlases have been rasterised and uploaded.
    // The responsiveness contract is "zero label rasterisations per frame while a
    // gesture is active", and this is how a test observes it.
    Q_PROPERTY(qulonglong labelAtlasesRasterized READ labelAtlasesRasterized NOTIFY diagnosticsChanged)
    // Cost diagnostic: how many times the static geometry group has been
    // rebuilt, and how many vertices the last paint built in the transient
    // group. The responsiveness contract is "a gesture rebuilds no static
    // geometry and costs vertices proportional to what it moves, not to the
    // network", and these are how a test observes it.
    Q_PROPERTY(qulonglong staticGeometryRebuilds READ staticGeometryRebuilds NOTIFY diagnosticsChanged)
    Q_PROPERTY(qulonglong transientVerticesBuilt READ transientVerticesBuilt NOTIFY diagnosticsChanged)

public:
    explicit GraphItem(QQuickItem* parent = nullptr);
    ~GraphItem() override;

    [[nodiscard]] GraphInteraction* interaction() const { return interaction_; }
    void setInteraction(GraphInteraction* interaction);
    [[nodiscard]] QVariantMap categoryColors() const { return categoryColorsProperty_; }
    void setCategoryColors(const QVariantMap& colors);
    [[nodiscard]] QVariantMap presentationStyle() const { return presentationStyleProperty_; }
    void setPresentationStyle(const QVariantMap& style);
    [[nodiscard]] QRectF visibleRect() const { return visibleRect_; }
    void setVisibleRect(QRectF rect);
    [[nodiscard]] qulonglong labelAtlasesRasterized() const { return rasterizations_; }
    [[nodiscard]] qulonglong staticGeometryRebuilds() const { return staticRebuilds_; }
    [[nodiscard]] qulonglong transientVerticesBuilt() const { return transientVertices_; }

signals:
    void interactionChanged();
    void categoryColorsChanged();
    void presentationStyleChanged();
    void visibleRectChanged();
    void diagnosticsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*unused*/) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    // The item is transformed by the view scale, so the scale it draws at is the
    // scale its own transform carries; screen-space line weights are derived
    // from it rather than passed in beside it.
    [[nodiscard]] qreal viewScale() const;

    QPointer<GraphInteraction> interaction_;
    QVariantMap categoryColorsProperty_;
    QHash<QString, QColor> categoryColorRecords_;
    QVariantMap presentationStyleProperty_;
    QRectF visibleRect_;
    QColor accentColor_{QStringLiteral("#3485f6")};
    QColor mutedColor_{QStringLiteral("#979ea8")};
    int fontSize_{11};
    qulonglong rasterizations_{};
    qulonglong staticRebuilds_{};
    qulonglong transientVertices_{};
};

}  // namespace nemo::ui
