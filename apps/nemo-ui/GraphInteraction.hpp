#pragma once

#include "GraphCommandFacade.hpp"
#include "GraphGeometry.hpp"
#include "GraphHitTest.hpp"
#include "GraphScene.hpp"
#include "GraphSessions.hpp"
#include "ViewerController.hpp"

#include <QtQml/qqmlregistration.h>

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace nemo::ui {

// The graph interaction core: everything between the read-only network snapshot
// and the pointer. It owns the scene, the view transform, the selection, the
// hover result and the active gesture session, and it is the only object the
// panel and the paint item talk to. The panel keeps input plumbing, chrome,
// popups and shortcuts; the item keeps painting.
//
// Every gesture is an explicit session with begin/update/cancel/commit. A
// cancelled session never reaches the command facade. One gesture produces one
// command and therefore one history entry; marquee, pan, zoom and scope entry
// produce none.
class GraphInteraction : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(nemo::ui::ViewerController* controller READ controller WRITE setController NOTIFY controllerChanged)
    // Whether the snapshot has a network at all. A session begun against an
    // unavailable snapshot never exists.
    Q_PROPERTY(bool available READ available NOTIFY sceneChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(qreal panX READ panX NOTIFY viewChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY viewChanged)
    // Bounds of the graph content plus its margin. The panel sizes the paint
    // item from these; nothing is hard-coded.
    Q_PROPERTY(qreal sceneWidth READ sceneWidth NOTIFY sceneChanged)
    Q_PROPERTY(qreal sceneHeight READ sceneHeight NOTIFY sceneChanged)
    // The scene rectangle the paint item must cover, derived from the viewport
    // and the transform.
    Q_PROPERTY(QRectF visibleRect READ visibleRect NOTIFY viewChanged)
    Q_PROPERTY(QStringList selectedNodeIds READ selectedNodeIds NOTIFY selectionChanged)
    // A press gesture is down (including a pan).
    Q_PROPERTY(bool gestureActive READ gestureActive NOTIFY gestureChanged)
    // The active gesture can publish an authored edit, so the shared history
    // adapter routes Undo to it for the duration.
    Q_PROPERTY(bool historyGestureActive READ historyGestureActive NOTIFY gestureChanged)
    // Enter-subnet affordances in viewport coordinates, one record per subnet
    // card: id, x, y, size, linkState. The panel positions its chrome chip from
    // these records instead of computing card geometry. They move with the view
    // and with the scene, so both notify.
    Q_PROPERTY(QVariantList enterAffordances READ enterAffordances NOTIFY affordancesChanged)
    Q_PROPERTY(bool marqueeVisible READ marqueeVisible NOTIFY previewChanged)
    Q_PROPERTY(QRectF marqueeRect READ marqueeRect NOTIFY previewChanged)
    // Where a new node is placed: the last click inside the surface, in scene
    // coordinates, or the viewport centre when there has been none. The notify
    // is the view signal because the panel persists it as part of the view
    // record.
    Q_PROPERTY(bool lastClickValid READ lastClickValid NOTIFY viewChanged)
    Q_PROPERTY(qreal lastClickX READ lastClickX NOTIFY viewChanged)
    Q_PROPERTY(qreal lastClickY READ lastClickY NOTIFY viewChanged)
    // The element the pointer currently acquires, published for interaction
    // evidence.
    Q_PROPERTY(QVariantMap hoveredEndpoint READ hoveredEndpoint NOTIFY hoverChanged)
    Q_PROPERTY(QVariantMap hoveredReroute READ hoveredReroute NOTIFY hoverChanged)
    // Cost diagnostic: hit-test passes performed. The responsiveness contract is
    // a count — exactly one pass per pointer move — and this is how a test
    // observes it.
    Q_PROPERTY(qulonglong hitTestPasses READ hitTestPasses NOTIFY diagnosticsChanged)

public:
    explicit GraphInteraction(QObject* parent = nullptr);
    ~GraphInteraction() override;

    [[nodiscard]] ViewerController* controller() const { return controller_; }
    void setController(ViewerController* controller);

    [[nodiscard]] bool available() const { return scene_->available; }
    [[nodiscard]] qreal zoom() const { return view_->transform().scale; }
    [[nodiscard]] qreal panX() const { return view_->transform().pan.x(); }
    [[nodiscard]] qreal panY() const { return view_->transform().pan.y(); }
    [[nodiscard]] qreal sceneWidth() const;
    [[nodiscard]] qreal sceneHeight() const;
    [[nodiscard]] QRectF visibleRect() const;
    [[nodiscard]] QStringList selectedNodeIds() const { return selected_; }
    [[nodiscard]] bool gestureActive() const;
    [[nodiscard]] bool historyGestureActive() const;
    [[nodiscard]] QVariantList enterAffordances() const;
    [[nodiscard]] bool marqueeVisible() const;
    [[nodiscard]] QRectF marqueeRect() const;
    [[nodiscard]] bool lastClickValid() const { return lastClickValid_; }
    [[nodiscard]] qreal lastClickX() const { return lastClick_.x(); }
    [[nodiscard]] qreal lastClickY() const { return lastClick_.y(); }
    [[nodiscard]] QVariantMap hoveredEndpoint() const;
    [[nodiscard]] QVariantMap hoveredReroute() const;
    [[nodiscard]] qulonglong hitTestPasses() const { return hitTestPasses_; }

    // The scene the painter and the geometry queries read. Immutable; replaced
    // only by setSnapshot, which aborts live sessions first.
    [[nodiscard]] const std::shared_ptr<const GraphScene>& scene() const { return scene_; }
    // The active session's transient state, empty when no session is running.
    [[nodiscard]] const GraphPreview& preview() const;
    // The element the pointer currently acquires.
    [[nodiscard]] const GraphHover& hover() const { return hover_; }
    [[nodiscard]] const QStringList& selection() const { return selected_; }

    // The snapshot for one network, stamped with the document revision it was
    // projected from. A different revision aborts a live session; it never
    // rebases one.
    Q_INVOKABLE void setSnapshot(const QString& networkId, const QVariantMap& snapshot, qulonglong revision);
    // The surface rectangle the panel gives the graph, in viewport pixels.
    Q_INVOKABLE void setViewport(qreal width, qreal height);

    // A view record restored from panel state. Cancels any queued burst and a
    // pending frame, because a view that was just adopted is the view the
    // artist is looking at.
    Q_INVOKABLE void adoptView(qreal zoom, qreal panX, qreal panY, bool lastClickValid, qreal lastClickX,
                               qreal lastClickY);
    Q_INVOKABLE void adoptSelection(const QVariantList& nodeIds);
    Q_INVOKABLE void clearLastClick();
    // Frames the whole network now, and writes the view once.
    Q_INVOKABLE void frameAll();
    // Requests framing for a scope that has no stored view: applied as soon as
    // the surface has a size, unless a view is adopted or a gesture starts
    // first.
    Q_INVOKABLE void requestFrame();

    // Pointer input, in viewport coordinates. Modifiers and buttons are the Qt
    // values QML already has.
    Q_INVOKABLE void press(qreal x, qreal y, int button, int modifiers);
    Q_INVOKABLE void move(qreal x, qreal y, int modifiers);
    Q_INVOKABLE void release(qreal x, qreal y, int button, int modifiers);
    // Re-resolves the element under the pointer without moving it.
    Q_INVOKABLE void hover();
    // The pointer left the surface. Hover is dropped unless a gesture is down.
    Q_INVOKABLE void leave();
    Q_INVOKABLE void wheel(qreal pixelDeltaY, qreal angleDeltaY, qreal x, qreal y);
    Q_INVOKABLE void doubleClick(qreal x, qreal y);
    // Escape, a right press, or the shared history adapter taking Undo: the
    // session is dropped and the network is exactly as it was.
    Q_INVOKABLE void cancelGesture();

    // Panel-state-independent queries the panel's chrome and menus read. The
    // panel never iterates graph elements or resolves a pick itself.
    Q_INVOKABLE QVariantMap scenePoint(qreal x, qreal y) const;
    Q_INVOKABLE QVariantMap nodeInfo(const QString& nodeId) const;
    // The card rectangle and port centre as the node is currently displayed: an
    // active gesture's transient position is included, so a caller aiming at
    // what is on screen sees the screen. Authored values come from the scene
    // record, and the offset is the same one the painter applies.
    Q_INVOKABLE QRectF nodeRect(const QString& nodeId) const;
    Q_INVOKABLE QPointF portPosition(const QString& nodeId, int port, bool output) const;

    // Shared graph commands the panel's shortcuts and menus raise. They are the
    // panel's actions, not gestures, and they all travel the same command path.
    Q_INVOKABLE bool deleteSelection();
    Q_INVOKABLE bool copySelection();
    Q_INVOKABLE bool pasteSelection();
    Q_INVOKABLE bool collapseSelection();
    // Duplicate a subnet occurrence keeping its definition linked, placed clear
    // of the card it came from.
    Q_INVOKABLE bool duplicateLinked(const QString& nodeId);
    // Give a shared or linked occurrence its own copy of the definition.
    Q_INVOKABLE bool makeIndependent(const QString& instanceId);
    Q_INVOKABLE bool assignViewer(int viewerIndex);
    // Placement: the anchor and network are captured when the placement chrome
    // opens, and the node is planned and created from the descriptor the panel
    // already holds. Returns whether anything was created.
    Q_INVOKABLE void beginPlacement();
    Q_INVOKABLE bool createNode(const QVariantMap& descriptor);

signals:
    void controllerChanged();
    void sceneChanged();
    void affordancesChanged();
    void viewChanged();
    void previewChanged();
    void selectionChanged();
    void hoverChanged();
    void gestureChanged();
    void diagnosticsChanged();
    // One panel-state write boundary per settled gesture.
    void viewSettled();
    void contextMenuRequested(qreal x, qreal y, const QString& nodeId);
    void inspectorRequested(const QString& nodeId);
    void scopeEntryRequested(const QString& nodeId);

private:
    // One hit-test pass for this pointer event. Both the hover feedback and a
    // press resolve their target from the same result, and every pass is
    // counted because the responsiveness contract is one pass per pointer move.
    [[nodiscard]] GraphHitResult query(QPointF viewport);
    [[nodiscard]] GraphSessionContext context(QPointF viewport, const GraphHitResult& hits) const;
    void beginGesture(std::unique_ptr<GraphSession> session);
    void toggleSelection(const QString& nodeId);
    // Publishes a selection that is already filtered to this scene.
    void applySelection(const QStringList& requested);
    // Filters a candidate selection to the identities this scene owns and
    // publishes it when it changed. `requested` of nullptr re-filters the
    // current selection against the scene.
    void syncSelection(const QStringList* requested = nullptr);
    void publishHover(const GraphHitResult& hits, Qt::KeyboardModifiers modifiers);
    // Frames the whole network. `settle` marks the one write boundary; the
    // deferred scope frame is not a gesture and records nothing.
    void frame(bool settle);
    void maybeFrame();
    [[nodiscard]] bool hasViewport() const;
    [[nodiscard]] QPointF creationScenePoint() const;
    [[nodiscard]] bool nodeNamed(const QString& name) const;
    // The transient displacement of a node while a gesture is moving it; zero
    // for every other node and for every other gesture.
    [[nodiscard]] QPointF transientOffset(const QString& nodeId) const;

    ViewerController* controller_{};
    std::shared_ptr<const GraphScene> scene_;
    std::unique_ptr<ViewSession> view_;
    std::unique_ptr<GraphSession> gesture_;
    std::unique_ptr<GraphCommandFacade> commands_;
    // The one ordered pass per pointer event writes into these buffers, so a
    // move costs one hit test and no allocation for it.
    GraphHitScratch hitScratch_;
    GraphHover hover_;
    QStringList selected_;
    QPointF viewportSize_;
    QPointF pointer_;
    QPointF pressPointer_;
    QPointF lastClick_;
    bool lastClickValid_{};
    bool lastClickMoved_{};
    bool framePending_{};
    // Placement chrome captured when the search popup or a category menu opens.
    QString placementNetwork_;
    QString placementAnchor_;
    qulonglong hitTestPasses_{};
};

}  // namespace nemo::ui
