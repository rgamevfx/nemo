#include "GraphInteraction.hpp"

#include "GraphCommandFacade.hpp"
#include "ViewerController.hpp"

#include <QRegularExpression>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace nemo::ui {
namespace {

const GraphPreview kEmptyPreview;

GraphEndpointRecord endpointOf(const GraphEdgeRecord& edge, bool output) {
    return output ? edge.from : edge.to;
}

// The pointer feedback the panel and the painter read is a derived view of the
// same query a press resolves its target with; it is published only when it
// actually changed, so a still pointer costs no repaint.
bool sameHit(const GraphHit& left, const GraphHit& right) {
    return left.kind == right.kind && left.node == right.node && left.edge == right.edge && left.port == right.port &&
           left.output == right.output && left.routeIndex == right.routeIndex;
}

bool sameHover(const GraphHover& left, const GraphHover& right) {
    return left.edge == right.edge && sameHit(left.endpoint, right.endpoint) && sameHit(left.reroute, right.reroute);
}

QString graphId(const QVariant& value) {
    const auto text = value.toString();
    return text == QStringLiteral("0") ? QString{} : text;
}

void appendIdentity(QStringList* ids, const QString& id) {
    if (!id.isEmpty() && !ids->contains(id))
        ids->push_back(id);
}

double finiteOr(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

}  // namespace

GraphInteraction::GraphInteraction(QObject* parent)
    : QObject(parent), scene_(std::make_shared<const GraphScene>()), view_(std::make_unique<ViewSession>()) {
    connect(view_.get(), &ViewSession::viewChanged, this, [this] {
        emit viewChanged();
        emit affordancesChanged();
    });
    connect(view_.get(), &ViewSession::settled, this, &GraphInteraction::viewSettled);
}

GraphInteraction::~GraphInteraction() = default;

void GraphInteraction::setController(ViewerController* controller) {
    if (controller_ == controller)
        return;
    controller_ = controller;
    commands_ = controller_ ? std::make_unique<GraphCommandFacade>(*controller_) : nullptr;
    emit controllerChanged();
}

const GraphPreview& GraphInteraction::preview() const {
    return gesture_ ? gesture_->preview() : kEmptyPreview;
}

qreal GraphInteraction::sceneWidth() const {
    const QRectF bounds = contentBounds(*scene_);
    return bounds.isNull() ? 1.0 : bounds.right() + kContentMargin;
}

qreal GraphInteraction::sceneHeight() const {
    const QRectF bounds = contentBounds(*scene_);
    return bounds.isNull() ? 1.0 : bounds.bottom() + kContentMargin;
}

QRectF GraphInteraction::visibleRect() const {
    const auto transform = view_->transform();
    if (transform.scale <= 0.0 || viewportSize_.x() <= 0.0 || viewportSize_.y() <= 0.0)
        return {};
    return QRectF(-transform.pan.x() / transform.scale, -transform.pan.y() / transform.scale,
                  viewportSize_.x() / transform.scale, viewportSize_.y() / transform.scale);
}

bool GraphInteraction::gestureActive() const {
    return gesture_ != nullptr || view_->panning();
}

bool GraphInteraction::historyGestureActive() const {
    return gesture_ && gesture_->historyGesture();
}

QVariantList GraphInteraction::enterAffordances() const {
    const auto transform = view_->transform();
    QVariantList records;
    records.reserve(scene_->nodes.size());
    for (const auto& node : scene_->nodes) {
        if (!node.hasChildScope())
            continue;
        const QRectF chip = affordanceRect(node);
        const QPointF corner = transform.toScreen(chip.topLeft());
        records.push_back(QVariantMap{{QStringLiteral("id"), node.id},
                                      {QStringLiteral("x"), corner.x()},
                                      {QStringLiteral("y"), corner.y()},
                                      {QStringLiteral("size"), chip.width() * transform.scale},
                                      {QStringLiteral("linkState"),
                                       node.linkState.isEmpty() ? QStringLiteral("local") : node.linkState}});
    }
    return records;
}

bool GraphInteraction::marqueeVisible() const {
    return gesture_ && gesture_->preview().marqueeVisible;
}

QRectF GraphInteraction::marqueeRect() const {
    return gesture_ ? gesture_->preview().marqueeScreen : QRectF();
}

QVariantMap GraphInteraction::hoveredEndpoint() const {
    const auto& endpoint = hover_.endpoint;
    if (endpoint.isNull())
        return {};
    QVariantMap map{{QStringLiteral("node"), endpoint.node},
                    {QStringLiteral("port"), endpoint.port},
                    {QStringLiteral("direction"),
                     endpoint.output ? QStringLiteral("output") : QStringLiteral("input")}};
    if (!endpoint.edge.isEmpty())
        map.insert(QStringLiteral("edge"), endpoint.edge);
    return map;
}

QVariantMap GraphInteraction::hoveredReroute() const {
    if (hover_.reroute.isNull())
        return {};
    // "Dragging" is the reroute candidate naming this edge, which is exactly the
    // dot the gesture is moving. A plain hover enlarges nothing.
    const bool dragging = !preview().routeEdge.isEmpty() && preview().routeEdge == hover_.reroute.edge;
    return QVariantMap{{QStringLiteral("edge"), hover_.reroute.edge},
                       {QStringLiteral("index"), hover_.reroute.routeIndex},
                       {QStringLiteral("dragging"), dragging}};
}

bool GraphInteraction::hasViewport() const {
    return viewportSize_.x() > 0.0 && viewportSize_.y() > 0.0;
}

void GraphInteraction::setSnapshot(const QString& networkId, const QVariantMap& snapshot, qulonglong revision) {
    auto built = buildGraphScene(snapshot);
    if (built.networkId.isEmpty())
        built.networkId = networkId;
    built.revision = revision;
    auto next = std::make_shared<const GraphScene>(std::move(built));
    const bool changed = next->revision != scene_->revision || next->networkId != scene_->networkId;
    // A superseded snapshot aborts a live gesture; it never rebases one. The
    // commit path detaches its session before it commands, so a document change
    // caused by a commit cannot cancel the commit that caused it.
    if (gesture_ && changed) {
        gesture_->cancel();
        gesture_.reset();
        emit gestureChanged();
        emit previewChanged();
    }
    if (changed) {
        hover_ = GraphHover{};
        emit hoverChanged();
    }
    scene_ = std::move(next);
    syncSelection();
    emit sceneChanged();
    emit affordancesChanged();
    maybeFrame();
}

void GraphInteraction::setViewport(qreal width, qreal height) {
    const QPointF size(finiteOr(width, 0.0), finiteOr(height, 0.0));
    if (viewportSize_ == size)
        return;
    viewportSize_ = size;
    emit viewChanged();
    maybeFrame();
}

void GraphInteraction::adoptView(qreal zoom, qreal panX, qreal panY, bool lastClickValid, qreal lastClickX,
                                 qreal lastClickY) {
    const auto current = view_->transform();
    const qreal nextZoom = clampedZoom(zoom);
    // A view that was just adopted is the view the artist is looking at, so it
    // cancels a pending scope frame and any queued burst.
    framePending_ = false;
    if (nextZoom != current.scale || panX != current.pan.x() || panY != current.pan.y())
        view_->adopt(GraphViewTransform{QPointF(panX, panY), nextZoom});
    lastClickValid_ = lastClickValid;
    lastClick_ = QPointF(finiteOr(lastClickX, 0.0), finiteOr(lastClickY, 0.0));
    emit viewChanged();
}

void GraphInteraction::adoptSelection(const QVariantList& nodeIds) {
    QStringList next;
    for (const auto& value : nodeIds)
        appendIdentity(&next, graphId(value));
    applySelection(next);
}

void GraphInteraction::clearLastClick() {
    if (!lastClickValid_ && lastClick_.isNull())
        return;
    lastClickValid_ = false;
    lastClick_ = QPointF();
    emit viewChanged();
}

void GraphInteraction::frameAll() {
    frame(true);
}

void GraphInteraction::requestFrame() {
    framePending_ = true;
    maybeFrame();
}

void GraphInteraction::maybeFrame() {
    if (!framePending_ || !scene_->available || !hasViewport())
        return;
    framePending_ = false;
    // A deferred scope frame is not a gesture: it records nothing, and the next
    // boundary writes the view the artist ends up looking at.
    frame(false);
}

void GraphInteraction::frame(bool settle) {
    if (scene_->nodes.isEmpty() || viewportSize_.x() < 3.0 || viewportSize_.y() < 3.0)
        return;
    const QRectF bounds = contentBounds(*scene_);
    const qreal width = std::max(kCardWidth, bounds.width());
    const qreal height = std::max(kCardHeight, bounds.height());
    const qreal next = clampedZoom(std::min((viewportSize_.x() - 42.0) / width, (viewportSize_.y() - 42.0) / height));
    view_->adopt(GraphViewTransform{QPointF((viewportSize_.x() - bounds.width() * next) / 2.0 - bounds.left() * next,
                                            (viewportSize_.y() - bounds.height() * next) / 2.0 - bounds.top() * next),
                                    next});
    if (settle)
        emit viewSettled();
}

GraphHitResult GraphInteraction::query(QPointF viewport) {
    GraphHitResult hits = hitTestGraph(*scene_, view_->transform(), viewport);
    ++hitTestPasses_;
    emit diagnosticsChanged();
    return hits;
}

GraphSessionContext GraphInteraction::context(QPointF viewport, const GraphHitResult& hits) const {
    const bool inside = viewport.x() >= 0.0 && viewport.y() >= 0.0 && viewport.x() <= viewportSize_.x() &&
                        viewport.y() <= viewportSize_.y();
    return GraphSessionContext{*commands_, view_->transform(), hits, viewport, inside};
}

void GraphInteraction::beginGesture(std::unique_ptr<GraphSession> session) {
    if (gesture_)
        gesture_->cancel();
    gesture_ = std::move(session);
    emit gestureChanged();
    emit previewChanged();
}

void GraphInteraction::press(qreal x, qreal y, int button, int modifiers) {
    const QPointF viewport(finiteOr(x, 0.0), finiteOr(y, 0.0));
    const auto mods = Qt::KeyboardModifiers(modifiers);
    pointer_ = viewport;
    const auto hits = query(viewport);
    publishHover(hits, mods);
    const auto mouseButton = static_cast<Qt::MouseButton>(button);
    if (mouseButton == Qt::RightButton) {
        // A right press while a gesture is running only cancels it. Otherwise it
        // offers the menu for the card it landed on, selecting that card first
        // when it was not selected.
        const bool hadGesture = gestureActive();
        cancelGesture();
        if (hadGesture)
            return;
        if (!hits.card.isNull() && !selected_.contains(hits.card.node))
            applySelection(QStringList{hits.card.node});
        emit contextMenuRequested(viewport.x(), viewport.y(), hits.card.node);
        return;
    }
    if (!commands_)
        return;
    pressPointer_ = viewport;
    if (mouseButton == Qt::MiddleButton) {
        view_->beginPan(view_->transform(), pressPointer_, false);
        emit gestureChanged();
        return;
    }
    if (mouseButton != Qt::LeftButton)
        return;
    const auto& primary = hits.primary();
    switch (primary.kind) {
    case GraphHitKind::Affordance:
        beginGesture(std::make_unique<ScopeEnterSession>(primary.node));
        break;
    case GraphHitKind::Port:
        if (!selected_.contains(primary.node))
            applySelection(QStringList{primary.node});
        beginGesture(std::make_unique<ConnectSession>(
            scene_, ConnectBegin{GraphEndpointRecord{primary.node, primary.port, primary.output}, {}, false, pressPointer_}));
        break;
    case GraphHitKind::Endpoint:
        beginGesture(std::make_unique<ConnectSession>(
            scene_,
            ConnectBegin{GraphEndpointRecord{primary.node, primary.port, primary.output}, primary.edge, false,
                        pressPointer_}));
        break;
    case GraphHitKind::Reroute:
        beginGesture(std::make_unique<RerouteSession>(scene_, primary, mods.testFlag(Qt::AltModifier), pressPointer_));
        break;
    case GraphHitKind::Card:
        if (mods.testFlag(Qt::ShiftModifier)) {
            toggleSelection(primary.node);
            beginGesture(std::make_unique<ToggleSession>(pressPointer_));
        } else {
            if (!selected_.contains(primary.node))
                applySelection(QStringList{primary.node});
            beginGesture(std::make_unique<MoveSession>(scene_, selected_, pressPointer_));
        }
        break;
    case GraphHitKind::PipeBody: {
        const auto* edge = scene_->edge(primary.edge);
        if (edge == nullptr)
            break;
        // Alt on a pipe inserts a reroute dot at the projection point; held and
        // dragged, that one gesture inserts and moves it.
        if (mods.testFlag(Qt::AltModifier)) {
            beginGesture(std::make_unique<RerouteSession>(scene_, primary, pressPointer_));
            break;
        }
        // Shift locks the destination end and pulls the source; without it the
        // destination is what moves. The choice is captured on press.
        const bool lockDestination = mods.testFlag(Qt::ShiftModifier);
        beginGesture(std::make_unique<ConnectSession>(
            scene_, ConnectBegin{endpointOf(*edge, lockDestination), primary.edge, true, pressPointer_}));
        break;
    }
    case GraphHitKind::None:
        if (mods.testFlag(Qt::ShiftModifier)) {
            beginGesture(std::make_unique<MarqueeSession>(scene_, selected_, pressPointer_));
        } else {
            view_->beginPan(view_->transform(), pressPointer_, true);
            emit gestureChanged();
        }
        break;
    }
}

void GraphInteraction::move(qreal x, qreal y, int modifiers) {
    const QPointF viewport(finiteOr(x, 0.0), finiteOr(y, 0.0));
    const auto mods = Qt::KeyboardModifiers(modifiers);
    pointer_ = viewport;
    const auto hits = query(viewport);
    publishHover(hits, mods);
    // A gesture only exists while a button is down, so a plain hover never
    // reaches a session.
    if (gesture_) {
        gesture_->update(context(viewport, hits));
        emit previewChanged();
    } else if (view_->panning()) {
        view_->update(context(viewport, hits));
    }
}

void GraphInteraction::release(qreal x, qreal y, int button, int modifiers) {
    const QPointF viewport(finiteOr(x, 0.0), finiteOr(y, 0.0));
    const auto mods = Qt::KeyboardModifiers(modifiers);
    pointer_ = viewport;
    const auto mouseButton = static_cast<Qt::MouseButton>(button);
    const auto hits = query(viewport);
    publishHover(hits, mods);
    if (mouseButton != Qt::LeftButton && mouseButton != Qt::MiddleButton)
        return;
    const bool panning = view_->panning();
    const bool moved = gesture_ ? gesture_->moved() : (panning && view_->moved());
    lastClickMoved_ = moved;
    // Only a left release places the next creation, and a pan that moved is a
    // navigation rather than a click.
    bool lastClickChanged = false;
    if (mouseButton == Qt::LeftButton && (!panning || !moved)) {
        const QPointF click = view_->transform().toScene(pressPointer_);
        lastClickChanged = !lastClickValid_ || lastClick_ != click;
        lastClickValid_ = true;
        lastClick_ = click;
    }
    // The session leaves the gesture slot before it commits: a command publishes
    // a document change synchronously, and the panel re-reads the snapshot from
    // inside that notification.
    auto session = std::move(gesture_);
    const auto sessionContext = context(viewport, hits);
    // A left or middle release is always a panel-state write boundary — the
    // replaced panel wrote once at every release, including the release after a
    // cancelled gesture. A session narrows that only when it has a reason to: a
    // scope entry navigates instead of recording, and a release during a zoom
    // burst defers to the one settled write.
    bool settle = true;
    if (session) {
        const auto result = session->commit(sessionContext);
        if (result.writesSelection)
            applySelection(result.selection);
        // Scope entry happens only when the release is still on the affordance
        // the press acquired; released anywhere else it does nothing at all. The
        // release's own ordered pass answers that, so no second query is made.
        if (!result.scopeEntry.isEmpty() && hits.affordance.kind == GraphHitKind::Affordance &&
            hits.affordance.node == result.scopeEntry)
            emit scopeEntryRequested(result.scopeEntry);
        settle = result.settleView;
        session.reset();
    } else if (panning) {
        const auto result = view_->commit(sessionContext);
        view_->endPan();
        if (result.writesSelection)
            applySelection(result.selection);
        settle = result.settleView;
    }
    emit gestureChanged();
    emit previewChanged();
    // The last-click record is view state the panel persists, so its notify runs
    // before the write boundary that reads it.
    if (lastClickChanged)
        emit viewChanged();
    if (settle)
        emit viewSettled();
}

void GraphInteraction::hover() {
    const auto hits = query(pointer_);
    publishHover(hits, Qt::KeyboardModifiers());
}

void GraphInteraction::leave() {
    if (gesture_ || view_->panning())
        return;
    const GraphHover cleared;
    if (sameHover(hover_, cleared))
        return;
    hover_ = cleared;
    emit hoverChanged();
}

void GraphInteraction::wheel(qreal pixelDeltaY, qreal angleDeltaY, qreal x, qreal y) {
    // The accepted rule is the wire drag alone: the view must not shift under a
    // connection the artist is making.
    if (gesture_ && !gesture_->acceptsWheel())
        return;
    const qreal pixels = pixelDeltaY != 0.0 ? pixelDeltaY : (angleDeltaY / 120.0) * kWheelPixelsPerNotch;
    if (pixels == 0.0)
        return;
    view_->queueZoom(std::exp(pixels * kWheelZoomExponent), QPointF(finiteOr(x, 0.0), finiteOr(y, 0.0)));
}

void GraphInteraction::doubleClick(qreal x, qreal y) {
    if (lastClickMoved_)
        return;
    const auto hits = query(QPointF(finiteOr(x, 0.0), finiteOr(y, 0.0)));
    if (!hits.card.isNull())
        emit inspectorRequested(hits.card.node);
}

void GraphInteraction::cancelGesture() {
    if (!gesture_ && !view_->panning())
        return;
    if (gesture_) {
        gesture_->cancel();
        gesture_.reset();
    }
    // Cancellation drops the pan candidate and resets the move threshold, so the
    // release that follows is a click and never a commit. A queued zoom burst is
    // not a gesture and keeps running.
    view_->cancel();
    lastClickMoved_ = false;
    emit gestureChanged();
    emit previewChanged();
}

QVariantMap GraphInteraction::scenePoint(qreal x, qreal y) const {
    const QPointF scene = view_->transform().toScene(QPointF(x, y));
    return QVariantMap{{QStringLiteral("x"), scene.x()}, {QStringLiteral("y"), scene.y()}};
}

QVariantMap GraphInteraction::nodeInfo(const QString& nodeId) const {
    const auto* node = scene_->node(nodeId);
    if (node == nullptr)
        return QVariantMap{{QStringLiteral("exists"), false}};
    return QVariantMap{{QStringLiteral("exists"), true},
                       {QStringLiteral("id"), node->id},
                       {QStringLiteral("name"), node->name},
                       {QStringLiteral("type"), node->type},
                       {QStringLiteral("category"), node->category},
                       {QStringLiteral("isSubnet"), node->hasChildScope()},
                       {QStringLiteral("definition"), node->definition},
                       {QStringLiteral("instance"), node->instance},
                       {QStringLiteral("linkState"), node->linkState},
                       {QStringLiteral("deletable"), node->deletable},
                       {QStringLiteral("terminal"), node->terminal},
                       {QStringLiteral("x"), node->position.x()},
                       {QStringLiteral("y"), node->position.y()}};
}

QPointF GraphInteraction::transientOffset(const QString& nodeId) const {
    const auto* node = scene_->node(nodeId);
    if (node == nullptr)
        return {};
    const auto& positions = preview().positions;
    const auto moved = positions.constFind(nodeId);
    return moved == positions.constEnd() ? QPointF{} : *moved - node->position;
}

QRectF GraphInteraction::nodeRect(const QString& nodeId) const {
    const auto* node = scene_->node(nodeId);
    return node == nullptr ? QRectF() : cardRect(*node).translated(transientOffset(nodeId));
}

QPointF GraphInteraction::portPosition(const QString& nodeId, int port, bool output) const {
    const auto* node = scene_->node(nodeId);
    if (node == nullptr)
        return {};
    return nemo::ui::portPosition(*node, port, output) + transientOffset(nodeId);
}

QPointF GraphInteraction::creationScenePoint() const {
    const QPointF scene =
        lastClickValid_ ? lastClick_
                        : view_->transform().toScene(QPointF(viewportSize_.x() / 2.0, viewportSize_.y() / 2.0));
    return QPointF(scene.x() - kCardWidth / 2.0, scene.y() - kCardHeight / 2.0);
}

void GraphInteraction::applySelection(const QStringList& requested) {
    QStringList next;
    next.reserve(requested.size());
    for (const auto& id : requested) {
        if (scene_->node(id) != nullptr)
            appendIdentity(&next, id);
    }
    if (next == selected_)
        return;
    selected_ = std::move(next);
    emit selectionChanged();
}

void GraphInteraction::syncSelection(const QStringList* requested) {
    applySelection(requested ? *requested : selected_);
}

void GraphInteraction::toggleSelection(const QString& nodeId) {
    QStringList next = selected_;
    if (const auto at = next.indexOf(nodeId); at >= 0)
        next.removeAt(at);
    else
        next.push_back(nodeId);
    applySelection(next);
}

bool GraphInteraction::deleteSelection() {
    if (!commands_)
        return false;
    QStringList ids;
    for (const auto& id : selected_) {
        const auto* node = scene_->node(id);
        if (node != nullptr && node->deletable)
            ids.push_back(id);
    }
    if (ids.isEmpty())
        return false;
    if (!commands_->deleteNodes(scene_->networkId, ids))
        return false;
    applySelection(QStringList{});
    return true;
}

bool GraphInteraction::copySelection() {
    if (!commands_ || selected_.isEmpty())
        return false;
    return commands_->copySelection(scene_->networkId, selected_);
}

bool GraphInteraction::pasteSelection() {
    if (!commands_)
        return false;
    const QStringList created = commands_->pasteSelection(scene_->networkId, creationScenePoint());
    if (created.isEmpty())
        return false;
    applySelection(created);
    return true;
}

bool GraphInteraction::collapseSelection() {
    if (!commands_ || selected_.isEmpty())
        return false;
    const QString created = commands_->collapseSelection(scene_->networkId, selected_);
    if (created.isEmpty())
        return false;
    applySelection(QStringList{created});
    return true;
}

bool GraphInteraction::duplicateLinked(const QString& nodeId) {
    if (!commands_)
        return false;
    const auto* node = scene_->node(nodeId);
    if (node == nullptr)
        return false;
    const QRectF card = cardRect(*node);
    return !commands_->duplicateLinked(scene_->networkId, nodeId, QPointF(card.right() + 24.0, card.y())).isEmpty();
}

bool GraphInteraction::makeIndependent(const QString& instanceId) {
    return commands_ && commands_->makeIndependent(instanceId);
}

bool GraphInteraction::assignViewer(int viewerIndex) {
    if (!commands_ || selected_.size() != 1)
        return false;
    return commands_->assignViewer(scene_->networkId, viewerIndex, selected_.first());
}

void GraphInteraction::beginPlacement() {
    placementNetwork_ = scene_->networkId;
    placementAnchor_ = selected_.isEmpty() ? QString{} : selected_.last();
}

bool GraphInteraction::createNode(const QVariantMap& descriptor) {
    if (!commands_ || !scene_->available || scene_->networkId.isEmpty() || descriptor.isEmpty())
        return false;
    if (placementNetwork_ != scene_->networkId)
        beginPlacement();
    const QString type = descriptor.value(QStringLiteral("type")).toString();
    if (type.isEmpty())
        return false;
    // The name is unique inside the network: the descriptor's display name with
    // every space removed, then the first free numeric suffix.
    QString base = descriptor.value(QStringLiteral("displayName")).toString();
    if (base.isEmpty())
        base = type;
    base.remove(QRegularExpression(QStringLiteral("\\s+")));
    QString name = base;
    for (int suffix = 1; nodeNamed(name); ++suffix)
        name = base + QString::number(suffix);

    QPointF point = creationScenePoint();
    QString anchor;
    QVector<QPair<QString, QPointF>> shifted;
    const auto* anchorNode = placementAnchor_.isEmpty() ? nullptr : scene_->node(placementAnchor_);
    const auto inputs = descriptor.value(QStringLiteral("inputs")).toList();
    const auto outputs = descriptor.value(QStringLiteral("outputs")).toList();
    const auto firstKind = [](const QVariantList& ports) {
        return ports.isEmpty() ? QString{} : ports.first().toMap().value(QStringLiteral("kind")).toString();
    };
    if (anchorNode != nullptr && !inputs.isEmpty() && !anchorNode->outputs.isEmpty() &&
        anchorNode->outputs.first().kind == firstKind(inputs)) {
        // The anchor can only be bridged when every consumer of its first output
        // accepts the new node's first output kind.
        bool valid = true;
        if (!outputs.isEmpty()) {
            for (const auto& edge : scene_->edges) {
                if (edge.from.node != anchorNode->id || edge.from.port != 0)
                    continue;
                const auto* destination = scene_->node(edge.to.node);
                const auto* port =
                    destination != nullptr && edge.to.port >= 0 && edge.to.port < destination->inputs.size()
                        ? &destination->inputs.at(edge.to.port)
                        : nullptr;
                if (port == nullptr || port->kind != firstKind(outputs)) {
                    valid = false;
                    break;
                }
            }
        }
        if (valid) {
            anchor = anchorNode->id;
            point = QPointF(anchorNode->position.x(), anchorNode->position.y() + 95.0);
            // The anchor's first output drives the chain that has to make room.
            QSet<QString> downstream;
            QVector<QString> pending;
            for (const auto& edge : scene_->edges)
                if (edge.from.node == anchorNode->id && edge.from.port == 0)
                    pending.push_back(edge.to.node);
            while (!pending.isEmpty()) {
                const QString id = pending.takeLast();
                if (downstream.contains(id))
                    continue;
                downstream.insert(id);
                for (const auto& edge : scene_->edges)
                    if (edge.from.node == id)
                        pending.push_back(edge.to.node);
            }
            const qreal bottom = point.y() + kCardHeight + 12.0;
            qreal delta = 0.0;
            for (const auto& node : scene_->nodes) {
                if (!downstream.contains(node.id))
                    continue;
                if (node.position.x() < point.x() + kCardWidth && node.position.x() + kCardWidth > point.x() &&
                    node.position.y() < bottom)
                    delta = std::max(delta, bottom - node.position.y());
            }
            if (delta > 0.0) {
                for (const auto& node : scene_->nodes) {
                    if (downstream.contains(node.id))
                        shifted.append({node.id, QPointF(node.position.x(), node.position.y() + delta)});
                }
            }
        }
    }
    const QString created = commands_->createNode(scene_->networkId, type, name, point, anchor, shifted);
    if (created.isEmpty())
        return false;
    applySelection(QStringList{created});
    return true;
}

bool GraphInteraction::nodeNamed(const QString& name) const {
    return std::ranges::any_of(scene_->nodes, [&name](const GraphNodeRecord& node) { return node.name == name; });
}

void GraphInteraction::publishHover(const GraphHitResult& hits, Qt::KeyboardModifiers modifiers) {
    GraphHover next;
    next.edge = hits.pipe.edge;
    if (!hits.port.isNull()) {
        next.endpoint = hits.port;
    } else if (!hits.pipe.isNull() && hits.card.isNull()) {
        // On neither a port nor a card: the feedback names the pipe end a pull
        // would grab, which is the end the active session has locked away from.
        if (const auto* edge = scene_->edge(hits.pipe.edge)) {
            const bool output =
                gesture_ ? gesture_->preferOutputEndpoint(modifiers) : !modifiers.testFlag(Qt::ShiftModifier);
            next.endpoint = GraphHit{GraphHitKind::Endpoint,
                                     output ? edge->from.node : edge->to.node,
                                     edge->id,
                                     output ? edge->from.port : edge->to.port,
                                     output,
                                     -1,
                                     -1,
                                     hits.pipe.screen,
                                     hits.pipe.scene,
                                     hits.pipe.distance};
        }
    }
    next.reroute = hits.reroute;
    if (sameHover(hover_, next))
        return;
    hover_ = std::move(next);
    emit hoverChanged();
}

}  // namespace nemo::ui
