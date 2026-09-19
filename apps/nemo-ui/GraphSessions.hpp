#pragma once

#include "GraphGeometry.hpp"
#include "GraphHitTest.hpp"
#include "GraphScene.hpp"

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <Qt>

#include <memory>

class QTimer;

namespace nemo::ui {

class GraphCommandFacade;

// The immutable per-frame view of the active gesture's transient state. The
// painter reads this and nothing else about a gesture; nothing here reaches the
// model, and a cancelled gesture drops it without a trace.
struct GraphPreview {
    // Node move: transient card positions, one entry per moved node and no
    // entry for any unmoved node. Replaced in place on every pointer move, so a
    // move of N selected nodes costs N writes and re-parses nothing.
    QHash<QString, QPointF> positions;
    // Marquee: candidate rectangle in viewport (screen) space, because it is
    // drawn as overlay chrome rather than in the graph's own transform.
    bool marqueeVisible{};
    QRectF marqueeScreen;
    // Connect and disconnect: the preview route between the fixed end and the
    // free end, both in scene coordinates.
    bool wireVisible{};
    QPointF wireFixed;
    QPointF wireFree;
    // The edge hidden while its own end is being pulled, so the stale pipe is
    // not drawn beside the candidate.
    QString wireHiddenEdge;
    // Reroute: the candidate polyline for one edge, in scene coordinates.
    QString routeEdge;
    QVector<QPointF> routePoints;
};

// Everything a session may consult while it is live. The scene is not here: a
// session retains the scene it began from, and the interaction aborts live
// sessions when a snapshot carries a different revision, so a session can never
// observe a scene other than its own. `hits` is the single ordered pass the
// interaction made for this pointer event: a session reads the element it needs
// from it rather than resolving a second pick, so feedback and the pick can
// never disagree and a pointer move costs one pass whatever gesture is running.
struct GraphSessionContext {
    GraphCommandFacade& commands;
    GraphViewTransform view;
    const GraphHitResult& hits;
    QPointF screen;
    bool insideViewport{};
};

struct GraphCommitResult {
    // The release is a panel-state write boundary. Scope entry writes through
    // its own navigation instead, so it is the one session that answers false.
    bool settleView{true};
    // The gesture writes the panel's selection (marquee union, click-to-clear).
    bool writesSelection{};
    QStringList selection;
    // The affordance the release landed on, for scope entry.
    QString scopeEntry;
};

// begin / update / cancel / commit. Cancellation is dropping the session: it
// never reaches the command facade and never produces a history entry.
class GraphSession {
public:
    virtual ~GraphSession() = default;
    // A session whose release can publish an authored edit; the shared history
    // adapter routes Undo to this session for the duration.
    [[nodiscard]] virtual bool historyGesture() const = 0;
    // The wheel is ignored while a wire is being pulled, so the view cannot
    // shift under the connection being made.
    [[nodiscard]] virtual bool acceptsWheel() const { return true; }
    // Whether the pointer left the 3 px move threshold since the press.
    [[nodiscard]] virtual bool moved() const = 0;
    [[nodiscard]] virtual const GraphPreview& preview() const = 0;
    // Which end of a pipe the hover feedback should prefer when the pointer is
    // on neither port: the output/source end unless Shift says otherwise, and
    // the opposite end while a pipe pull has locked the other one.
    [[nodiscard]] virtual bool preferOutputEndpoint(Qt::KeyboardModifiers modifiers) const {
        return !modifiers.testFlag(Qt::ShiftModifier);
    }
    virtual void update(const GraphSessionContext& context) = 0;
    virtual void cancel() = 0;
    virtual GraphCommitResult commit(const GraphSessionContext& context) = 0;
};

// Node move: one card, or the whole selection when the press landed inside one.
// The candidate is a per-node transient position written in place on every
// move; the commit is one move command covering every moved node, and a drag
// that never left the move threshold submits nothing.
class MoveSession final : public GraphSession {
public:
    MoveSession(std::shared_ptr<const GraphScene> scene, const QStringList& nodeIds, QPointF pressScreen);
    [[nodiscard]] bool historyGesture() const override { return true; }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext& context) override;

private:
    void applySnap(const GraphViewTransform& view);

    std::shared_ptr<const GraphScene> scene_;
    QStringList ids_;
    QHash<QString, QPointF> starts_;
    QPointF pressScreen_;
    QPointF delta_;
    // A single selected processing node with no connections drops onto a pipe.
    bool disconnectedInsert_{};
    bool moved_{};
    GraphPreview preview_;
};

// A Shift press on a card toggles the selection and starts no candidate. It is
// still an exclusive gesture: nothing else may begin while the button is down,
// and its release is the panel-state write boundary.
class ToggleSession final : public GraphSession {
public:
    explicit ToggleSession(QPointF pressScreen) : pressScreen_(pressScreen) {}
    [[nodiscard]] bool historyGesture() const override { return false; }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext&) override;

private:
    QPointF pressScreen_;
    bool moved_{};
    GraphPreview preview_;
};

// Shift press on empty canvas: the candidate rectangle the union is taken from.
class MarqueeSession final : public GraphSession {
public:
    MarqueeSession(std::shared_ptr<const GraphScene> scene, const QStringList& baseSelection, QPointF pressScreen);
    [[nodiscard]] bool historyGesture() const override { return false; }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext& context) override;

private:
    std::shared_ptr<const GraphScene> scene_;
    QStringList base_;
    QPointF pressScreen_;
    bool moved_{};
    GraphPreview preview_;
};

// How a connect gesture began. The fixed end is captured on press, never
// recomputed, which is the prototype's press-time endpoint lock: the end the
// artist grabbed stays the end that moves.
struct ConnectBegin {
    GraphEndpointRecord fixed;
    // The edge whose end is being reconnected; empty for a new wire, which
    // replaces an occupied destination input instead.
    QString existingEdge;
    // A pipe-body press shows nothing until the pointer leaves the move
    // threshold, so a click and a sub-3 px jitter leave the pipe alone.
    bool pendingFromPipe{};
    // Where the gesture began, so the 3 px threshold is measured from the press
    // rather than from a port that may be nowhere near it.
    QPointF pressScreen;
};

// Connect, disconnect and reconnect are one session: press a port or a
// connection end, pull, and release on a compatible destination. An invalid or
// outside drop leaves the topology untouched, and a drop on empty canvas
// disconnects an existing pulled end.
class ConnectSession final : public GraphSession {
public:
    ConnectSession(std::shared_ptr<const GraphScene> scene, const ConnectBegin& begin);
    [[nodiscard]] bool historyGesture() const override { return true; }
    // The wheel is ignored while a wire is actually being pulled. A pipe pull
    // that has not left the move threshold is still the prototype's "pipe"
    // state, which accepted the wheel.
    [[nodiscard]] bool acceptsWheel() const override { return begin_.pendingFromPipe && !moved_; }
    // A pipe highlight marks the end that would move, which is the end this
    // gesture did not lock on press. A direct port or connection-end press has
    // no lock, so the live Shift state decides.
    [[nodiscard]] bool preferOutputEndpoint(Qt::KeyboardModifiers modifiers) const override {
        return begin_.pendingFromPipe && !moved_ ? !begin_.fixed.output : !modifiers.testFlag(Qt::ShiftModifier);
    }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext& context) override;

private:
    std::shared_ptr<const GraphScene> scene_;
    ConnectBegin begin_;
    bool moved_{};
    GraphPreview preview_;

    // The compatible destination of this event's own ordered pass, or a null hit.
    [[nodiscard]] GraphHit candidate(const GraphSessionContext& context) const;
};

// Reroute: grab a dot, or Alt-click a pipe body to insert one. A click with no
// movement removes the dot it grabbed and nothing else.
class RerouteSession final : public GraphSession {
public:
    // Grab an existing dot; `remove` is Alt held at the press.
    RerouteSession(std::shared_ptr<const GraphScene> scene, const GraphHit& dot, bool remove, QPointF pressScreen);
    // Alt-click on a pipe body: insert a dot at the projection point.
    RerouteSession(std::shared_ptr<const GraphScene> scene, const GraphHit& pipeBody, QPointF pressScreen);
    [[nodiscard]] bool historyGesture() const override { return true; }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext& context) override;

private:
    std::shared_ptr<const GraphScene> scene_;
    QString edgeId_;
    QPointF pressScreen_;
    int index_{-1};
    bool remove_{};
    bool inserted_{};
    bool moved_{};
    GraphPreview preview_;
};

// Scope entry: press on a subnet card's affordance, release on the same one.
// One panel-state scope change and no history entry; released anywhere else it
// does nothing at all.
class ScopeEnterSession final : public GraphSession {
public:
    explicit ScopeEnterSession(QString nodeId);
    [[nodiscard]] bool historyGesture() const override { return false; }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext&) override;

private:
    QString nodeId_;
    bool moved_{};
    GraphPreview preview_;
};

// Pan and zoom are one session and they own the view transform. A middle drag
// or an empty-canvas drag moves the view every move; a wheel burst accumulates
// a target zoom, applies it at most once per event-loop turn, and settles into
// exactly one panel-state write. The settled write is the only one the gesture
// produces: nothing is written mid-gesture.
//
// The session outlives a single press — a burst may begin with no button down —
// so a wheel that arrives during another gesture is still the same view
// session.
class ViewSession final : public QObject, public GraphSession {
    Q_OBJECT
public:
    explicit ViewSession(QObject* parent = nullptr);
    ~ViewSession() override;

    [[nodiscard]] bool historyGesture() const override { return false; }
    [[nodiscard]] bool moved() const override { return moved_; }
    [[nodiscard]] const GraphPreview& preview() const override { return preview_; }
    void update(const GraphSessionContext& context) override;
    void cancel() override;
    GraphCommitResult commit(const GraphSessionContext& context) override;

    // A drag on empty canvas, or a middle drag. `origin` is the view at the
    // press; only a left drag clears the selection on a click.
    void beginPan(GraphViewTransform origin, QPointF pressScreen, bool clearsSelectionOnClick);
    [[nodiscard]] bool panning() const { return panning_; }
    void endPan();

    // Accumulate one wheel step. `factor` is exp(delta * 0.002) with the
    // prototype's 53 px per notch; the target is clamped 0.2-2.5 and anchored
    // at the pointer, so the pixel under the pointer stays under it.
    void queueZoom(qreal factor, QPointF anchorScreen);
    [[nodiscard]] bool zoomInFlight() const { return burstDirty_; }

    // A view the interaction sets itself (a restored record, frame all) wins
    // over any queued burst, so a stale target can never move it afterwards.
    void adopt(GraphViewTransform view);
    [[nodiscard]] GraphViewTransform transform() const { return transform_; }

signals:
    // The transform moved and the graph must repaint.
    void viewChanged();
    // One panel-state write boundary, once per settled burst.
    void settled();

private:
    void applyZoomTarget();

    GraphViewTransform transform_;
    // Pan.
    bool panning_{};
    bool clearsSelectionOnClick_{};
    GraphViewTransform panOrigin_;
    QPointF pressScreen_;
    bool moved_{};
    // Zoom burst.
    qreal zoomTarget_{1.0};
    QPointF zoomAnchor_;
    bool turnPending_{};
    // A burst is queued or applied and has not settled yet; this is what defers
    // the release's write and what the settle timer cashes in exactly once.
    bool burstDirty_{};
    QTimer* turnTimer_{};
    QTimer* settleTimer_{};
    GraphPreview preview_;
};

}  // namespace nemo::ui
