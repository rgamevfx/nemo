#include "GraphSessions.hpp"

#include "GraphCommandFacade.hpp"
#include "GraphGeometry.hpp"
#include "GraphHitTest.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace nemo::ui {
namespace {

// The prototype's move threshold. Strictly greater, so a pointer that sits an
// exact 3 px from the press is still a click, and a session that crossed it once
// stays moved even if the pointer comes back.
[[nodiscard]] bool leftMoveThreshold(bool alreadyMoved, QPointF press, QPointF pointer) {
    if (alreadyMoved)
        return true;
    const QPointF offset = pointer - press;
    return std::hypot(offset.x(), offset.y()) > kMoveThreshold;
}

// The prototype's nodeIsDisconnectedProcessing: a deletable processing node
// that declares both directions and that no edge in the scene touches can be
// dropped onto a pipe.
[[nodiscard]] bool disconnectedProcessingNode(const GraphScene& scene, const QString& nodeId) {
    const GraphNodeRecord* node = scene.node(nodeId);
    if (!node || !node->deletable || node->inputs.isEmpty() || node->outputs.isEmpty())
        return false;
    for (const GraphEdgeRecord& edge : scene.edges) {
        if (edge.from.node == nodeId || edge.to.node == nodeId)
            return false;
    }
    return true;
}

// One port's scene centre, which is the single geometry owner's answer and never
// a second copy of the port layout.
[[nodiscard]] QPointF portScenePosition(const GraphScene& scene, const QString& nodeId, int port, bool output) {
    const GraphNodeRecord* node = scene.node(nodeId);
    return node ? portPosition(*node, port, output) : QPointF{};
}

}  // namespace

MoveSession::MoveSession(std::shared_ptr<const GraphScene> scene, const QStringList& nodeIds, QPointF pressScreen)
    : scene_(std::move(scene)), ids_(nodeIds), pressScreen_(pressScreen) {
    for (const QString& id : ids_) {
        const GraphNodeRecord* node = scene_->node(id);
        if (!node)
            continue;
        // The authored card top-left is the drag start position.
        starts_.insert(id, cardRect(*node).topLeft());
    }
    disconnectedInsert_ = ids_.size() == 1 && disconnectedProcessingNode(*scene_, ids_.constFirst());
}

void MoveSession::update(const GraphSessionContext& context) {
    if (!leftMoveThreshold(moved_, pressScreen_, context.screen))
        return;
    moved_ = true;
    // The raw delta first; applySnap replaces it with the snapped one.
    delta_ = (context.screen - pressScreen_) / context.view.scale;
    applySnap(context.view);
    // One write per moved node, in place. A node that did not move keeps no
    // entry and nothing else is touched.
    for (const QString& id : ids_) {
        const auto start = starts_.constFind(id);
        if (start == starts_.constEnd())
            continue;
        preview_.positions.insert(id, start.value() + delta_);
    }
}

void MoveSession::applySnap(const GraphViewTransform& view) {
    const qreal threshold = kSnapTolerance / view.scale;
    // The prototype starts from the raw delta with a best distance just outside
    // the tolerance and accepts a candidate only on a strict `<`. Starting the
    // best distance at exactly the tolerance with the same strict `<` is that
    // gate. X and Y snap independently and share one running best across the
    // whole double loop, so the first candidate keeps a tie.
    qreal bestDistanceX = threshold;
    qreal bestDistanceY = threshold;
    qreal bestX = delta_.x();
    qreal bestY = delta_.y();
    for (const QString& movingId : ids_) {
        const auto start = starts_.constFind(movingId);
        if (start == starts_.constEnd())
            continue;
        const QPointF candidate = start.value() + delta_;
        for (const GraphNodeRecord& other : scene_->nodes) {
            if (ids_.contains(other.id))
                continue;
            const qreal x = other.position.x();
            const qreal y = other.position.y();
            const qreal distanceX = std::abs(x - candidate.x());
            if (distanceX < bestDistanceX) {
                bestDistanceX = distanceX;
                bestX = delta_.x() + x - candidate.x();
            }
            const qreal distanceY = std::abs(y - candidate.y());
            if (distanceY < bestDistanceY) {
                bestDistanceY = distanceY;
                bestY = delta_.y() + y - candidate.y();
            }
        }
    }
    delta_ = QPointF(bestX, bestY);
}

void MoveSession::cancel() {
    preview_ = GraphPreview{};
    moved_ = false;
}

GraphCommitResult MoveSession::commit(const GraphSessionContext& context) {
    GraphCommitResult result;
    result.settleView = true;
    if (!moved_)
        return result;

    QVector<QPair<QString, QPointF>> positions;
    positions.reserve(ids_.size());
    for (const QString& id : ids_) {
        const auto start = starts_.constFind(id);
        if (start == starts_.constEnd())
            continue;
        positions.append({id, start.value() + delta_});
    }
    if (positions.isEmpty())
        return result;

    // A lone disconnected processing node released inside the surface over a raw
    // pipe hit drops onto that pipe; only a refused insert falls back to the
    // move, so exactly one command is submitted at most.
    if (disconnectedInsert_ && context.insideViewport && !context.hits.pipe.isNull()) {
        if (context.commands.insertExistingNodeOnEdge(scene_->networkId, ids_.constFirst(), context.hits.pipe.edge,
                                                     positions.constFirst().second))
            return result;
    }
    context.commands.moveNodes(scene_->networkId, positions);
    return result;
}

void ToggleSession::update(const GraphSessionContext& context) {
    moved_ = leftMoveThreshold(moved_, pressScreen_, context.screen);
}

void ToggleSession::cancel() {
    // A toggle starts no candidate: the selection was already written at the
    // press, so there is nothing to drop.
}

GraphCommitResult ToggleSession::commit(const GraphSessionContext&) {
    // A toggle writes the selection at the press and starts no candidate, so its
    // release is the panel-state write boundary and nothing else.
    return {};
}

MarqueeSession::MarqueeSession(std::shared_ptr<const GraphScene> scene, const QStringList& baseSelection,
                               QPointF pressScreen)
    : scene_(std::move(scene)), base_(baseSelection), pressScreen_(pressScreen) {}

void MarqueeSession::update(const GraphSessionContext& context) {
    moved_ = leftMoveThreshold(moved_, pressScreen_, context.screen);
    // The candidate box is published on every pointer event, threshold or not:
    // the prototype draws a zero-size box from the press.
    preview_.marqueeVisible = true;
    preview_.marqueeScreen = QRectF(pressScreen_, context.screen).normalized();
}

void MarqueeSession::cancel() {
    preview_ = GraphPreview{};
    moved_ = false;
}

GraphCommitResult MarqueeSession::commit(const GraphSessionContext& context) {
    GraphCommitResult result;
    result.settleView = true;
    if (!moved_)
        return result;

    const QRectF box = QRectF(pressScreen_, context.screen).normalized();
    result.writesSelection = true;
    result.selection = base_;
    // The base list first, then the nodes the box touches in scene order, with
    // the prototype's inclusive screen-space test.
    for (const GraphNodeRecord& node : scene_->nodes) {
        if (result.selection.contains(node.id))
            continue;
        const QRectF card = cardRect(node);
        const QPointF screenTopLeft = context.view.toScreen(card.topLeft());
        if (screenTopLeft.x() <= box.right() && screenTopLeft.x() + card.width() * context.view.scale >= box.left() &&
            screenTopLeft.y() <= box.bottom() && screenTopLeft.y() + card.height() * context.view.scale >= box.top())
            result.selection.append(node.id);
    }
    return result;
}

ConnectSession::ConnectSession(std::shared_ptr<const GraphScene> scene, const ConnectBegin& begin)
    : scene_(std::move(scene)), begin_(begin) {}

GraphHit ConnectSession::candidate(const GraphSessionContext& context) const {
    // Only a port of the opposite direction can receive the pulled end: a fixed
    // output looks for an input and a fixed input looks for an output, exactly
    // as the prototype filtered its port query. Reading the per-direction winner
    // rather than the combined `port` keeps a same-direction port inside the
    // tolerance from shadowing a valid destination.
    const GraphHit& hit = begin_.fixed.output ? context.hits.portInput : context.hits.portOutput;
    if (hit.kind != GraphHitKind::Port)
        return {};
    return hit;
}

void ConnectSession::update(const GraphSessionContext& context) {
    moved_ = leftMoveThreshold(moved_, begin_.pressScreen, context.screen);

    // A pipe-body press publishes nothing until the pointer leaves the move
    // threshold, so a click and a sub-3 px jitter leave the pipe alone. The gate
    // never suppresses the commit test.
    if (begin_.pendingFromPipe && !moved_) {
        preview_ = GraphPreview{};
        return;
    }

    const GraphHit target = candidate(context);
    preview_.wireVisible = true;
    preview_.wireFixed = portScenePosition(*scene_, begin_.fixed.node, begin_.fixed.port, begin_.fixed.output);
    preview_.wireFree = target.isNull() ? context.view.toScene(context.screen)
                                        : portScenePosition(*scene_, target.node, target.port, target.output);
    preview_.wireHiddenEdge = begin_.existingEdge;
}

void ConnectSession::cancel() {
    preview_ = GraphPreview{};
    moved_ = false;
}

GraphCommitResult ConnectSession::commit(const GraphSessionContext& context) {
    GraphCommitResult result;
    result.settleView = true;
    // A click and a sub-3 px jitter are not a pull, and a release outside the
    // surface commits nothing.
    if (!moved_ || !context.insideViewport)
        return result;

    const GraphHit target = candidate(context);
    if (!target.isNull()) {
        // The fixed end keeps the end the artist grabbed; the wire travels from
        // the output to the input whichever end was pulled.
        GraphEndpointRecord from = begin_.fixed;
        GraphEndpointRecord to{target.node, target.port, target.output};
        if (!begin_.fixed.output)
            std::swap(from, to);
        if (begin_.existingEdge.isEmpty())
            context.commands.connectOrReplace(scene_->networkId, from, to);
        else
            context.commands.rewire(scene_->networkId, begin_.existingEdge, from, to);
        return result;
    }

    // No compatible destination: an existing pulled end released on empty canvas
    // disconnects, while a drop on a card preserves the topology untouched.
    if (begin_.existingEdge.isEmpty() || !context.hits.card.isNull())
        return result;
    context.commands.disconnect(scene_->networkId, begin_.existingEdge);
    return result;
}

RerouteSession::RerouteSession(std::shared_ptr<const GraphScene> scene, const GraphHit& dot, bool remove,
                               QPointF pressScreen)
    : scene_(std::move(scene)), edgeId_(dot.edge), pressScreen_(pressScreen), index_(dot.routeIndex),
      remove_(remove) {
    // The candidate starts as the authored route, exactly as the prototype
    // copies it before it grabs a dot. It lives in preview_ so a pointer move
    // updates one entry in place rather than rebuilding the vector.
    preview_.routeEdge = edgeId_;
    if (const GraphEdgeRecord* edge = scene_->edge(edgeId_))
        preview_.routePoints = edge->route;
}

RerouteSession::RerouteSession(std::shared_ptr<const GraphScene> scene, const GraphHit& pipeBody, QPointF pressScreen)
    : scene_(std::move(scene)), edgeId_(pipeBody.edge), pressScreen_(pressScreen), inserted_(true) {
    preview_.routeEdge = edgeId_;
    const GraphEdgeRecord* edge = scene_->edge(edgeId_);
    if (!edge)
        return;
    preview_.routePoints = edge->route;
    // The prototype splices at the polyline segment the Alt-click landed on,
    // clamped the way Array.splice clamps a past-the-end index.
    const int index = std::clamp(pipeBody.segment, 0, static_cast<int>(preview_.routePoints.size()));
    preview_.routePoints.insert(index, pipeBody.scene);
    index_ = index;
}

void RerouteSession::update(const GraphSessionContext& context) {
    // The threshold latch gates only the commit; the dot itself follows the
    // pointer on every event, sub-threshold motion included, exactly as the
    // prototype wrote the reroute point on each mouse move. One entry is
    // rewritten in place: no whole-vector copy.
    moved_ = leftMoveThreshold(moved_, pressScreen_, context.screen);
    if (index_ >= 0 && index_ < preview_.routePoints.size())
        preview_.routePoints[index_] = context.view.toScene(context.screen);
}

void RerouteSession::cancel() {
    preview_ = GraphPreview{};
    edgeId_.clear();
    index_ = -1;
    remove_ = false;
    inserted_ = false;
    moved_ = false;
}

GraphCommitResult RerouteSession::commit(const GraphSessionContext& context) {
    GraphCommitResult result;
    result.settleView = true;
    if (!scene_->edge(edgeId_))
        return result;

    // A click with no movement removes the dot it grabbed and nothing else.
    if (remove_ && !moved_ && index_ >= 0 && index_ < preview_.routePoints.size())
        preview_.routePoints.remove(index_);

    // Exactly the prototype's three commit conditions: a removal click, a moved
    // dot, or an inserted dot. Anything else leaves the authored route alone.
    if (!(remove_ && !moved_) && !moved_ && !inserted_)
        return result;
    context.commands.commitRoute(scene_->networkId, edgeId_, preview_.routePoints);
    return result;
}

ScopeEnterSession::ScopeEnterSession(QString nodeId) : nodeId_(std::move(nodeId)) {}

void ScopeEnterSession::update(const GraphSessionContext&) {
    // Scope entry is the one gesture whose release decides on its own hit: the
    // prototype enters when the release is still on the affordance the press
    // acquired, which the interaction re-resolves, not when the pointer stayed
    // within the move threshold.
}

void ScopeEnterSession::cancel() {
    nodeId_.clear();
}

GraphCommitResult ScopeEnterSession::commit(const GraphSessionContext&) {
    GraphCommitResult result;
    // The prototype navigates instead of writing the view record, so scope entry
    // is the one session that does not settle the view.
    result.settleView = false;
    result.scopeEntry = nodeId_;
    return result;
}

ViewSession::ViewSession(QObject* parent) : QObject(parent) {
    turnTimer_ = new QTimer(this);
    turnTimer_->setInterval(0);
    turnTimer_->setSingleShot(true);
    connect(turnTimer_, &QTimer::timeout, this, &ViewSession::applyZoomTarget);

    settleTimer_ = new QTimer(this);
    settleTimer_->setInterval(kZoomSettleMs);
    settleTimer_->setSingleShot(true);
    connect(settleTimer_, &QTimer::timeout, this, [this] {
        // One write per settled burst, and only for a burst that is still dirty:
        // an adopted view or an already cashed-in burst writes nothing.
        if (!burstDirty_)
            return;
        burstDirty_ = false;
        turnPending_ = false;
        emit settled();
    });
}

ViewSession::~ViewSession() = default;

void ViewSession::update(const GraphSessionContext& context) {
    if (!panning_)
        return;
    moved_ = leftMoveThreshold(moved_, pressScreen_, context.screen);
    const QPointF pan = panOrigin_.pan + (context.screen - pressScreen_);
    if (pan == transform_.pan)
        return;
    transform_.pan = pan;
    emit viewChanged();
}

void ViewSession::cancel() {
    // The pan candidate is dropped and the transform stays where it is: Escape
    // during a pan leaves the view in place. A queued burst is not touched; it
    // belongs to the wheel, not to the pan.
    panning_ = false;
}

GraphCommitResult ViewSession::commit(const GraphSessionContext&) {
    GraphCommitResult result;
    // A left click on empty canvas clears the selection; nothing else does.
    if (clearsSelectionOnClick_ && !moved_)
        result.writesSelection = true;
    // A release during a burst defers its write to the single settled boundary.
    result.settleView = !zoomInFlight();
    return result;
}

void ViewSession::beginPan(GraphViewTransform origin, QPointF pressScreen, bool clearsSelectionOnClick) {
    panOrigin_ = origin;
    pressScreen_ = pressScreen;
    clearsSelectionOnClick_ = clearsSelectionOnClick;
    panning_ = true;
    moved_ = false;
    // A burst already in flight is the same view session and is left alone.
}

void ViewSession::endPan() {
    panning_ = false;
}

void ViewSession::queueZoom(qreal factor, QPointF anchorScreen) {
    // One application per event-loop turn: a turn that is still pending absorbs
    // every wheel step that lands in it, and the first step of a new turn
    // accumulates from the scale the artist is looking at rather than from the
    // previous turn's target.
    if (!turnPending_) {
        zoomTarget_ = transform_.scale;
        turnPending_ = true;
        turnTimer_->start();
    }
    zoomTarget_ = clampedZoom(zoomTarget_ * factor);
    zoomAnchor_ = anchorScreen;
    // The burst is in flight until it settles, which is what defers a release's
    // panel-state write to the one settled boundary.
    burstDirty_ = true;
    settleTimer_->start();
}

void ViewSession::applyZoomTarget() {
    turnPending_ = false;
    const qreal next = clampedZoom(zoomTarget_);
    if (next == transform_.scale)
        return;
    // The scene point under the anchor stays under it.
    const QPointF scene = (zoomAnchor_ - transform_.pan) / transform_.scale;
    transform_.scale = next;
    transform_.pan = zoomAnchor_ - scene * next;
    emit viewChanged();
}

void ViewSession::adopt(GraphViewTransform view) {
    // A view the interaction sets itself wins over any queued burst, so a stale
    // target can never move it afterwards.
    turnTimer_->stop();
    burstDirty_ = false;
    turnPending_ = false;
    if (view.pan == transform_.pan && view.scale == transform_.scale)
        return;
    transform_ = view;
    emit viewChanged();
}

}  // namespace nemo::ui
