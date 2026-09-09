#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

#include <QFileInfo>
#include <QQuickWindow>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
namespace nemo::ui {
ViewerController::ViewerController(ViewerRuntime* runtime)
    : runtime_(runtime), commands_(document_), schedulerPoll_(this),
      presentationState_(std::make_unique<WindowPresentationState>()) {
    connect(runtime_, &ViewerRuntime::resultReady, this, &ViewerController::receive, Qt::QueuedConnection);
    connect(
        runtime_, &ViewerRuntime::rangeFailed, this,
        [this](const QString& message, qulonglong id) {
            if (id != rangeGeneration_)
                return;
            rangeError_ = message;
            emit schedulerChanged();
        },
        Qt::QueuedConnection);
    schedulerPoll_.setInterval(200);
    connect(&schedulerPoll_, &QTimer::timeout, this, &ViewerController::pollScheduler);
    schedulerPoll_.start();
}
ViewerController::~ViewerController() = default;

QString ViewerController::renderState() const {
    if (!error_.isEmpty())
        return QStringLiteral("failed");
    if (pending_)
        return QStringLiteral("pending");
    if (outdated_)
        return QStringLiteral("outdated");
    if (presentation_)
        return QStringLiteral("current");
    return QStringLiteral("idle");
}

QStringList ViewerController::outputNames() const {
    QStringList result;
    for (const auto& node : document_.graph.nodes()) {
        if (node.type == "output")
            result.push_back(QString::fromStdString(node.name));
    }
    return result;
}

QVariantList ViewerController::graphNodes() const {
    QVariantList result;
    for (const auto& node : document_.graph.nodes()) {
        QVariantMap params;
        for (const auto& [key, value] : node.params)
            params.insert(QString::fromStdString(key), QString::fromStdString(value));
        result.push_back(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(node.id)},
                                     {QStringLiteral("type"), QString::fromStdString(node.type)},
                                     {QStringLiteral("name"), QString::fromStdString(node.name)},
                                     {QStringLiteral("params"), params}});
    }
    return result;
}

QVariantList ViewerController::graphEdges() const {
    QVariantList result;
    for (const auto& edge : document_.graph.edges()) {
        result.push_back(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(edge.id)},
                                     {QStringLiteral("fromNode"), QVariant::fromValue<qulonglong>(edge.from.node)},
                                     {QStringLiteral("fromPort"), static_cast<int>(edge.from.port)},
                                     {QStringLiteral("toNode"), QVariant::fromValue<qulonglong>(edge.to.node)},
                                     {QStringLiteral("toPort"), static_cast<int>(edge.to.port)}});
    }
    return result;
}

void ViewerController::setOutputName(const QString& name) {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        fail(QStringLiteral("viewer output name must not be empty"));
        return;
    }
    const auto* node = document_.graph.nodeByName(trimmed.toStdString());
    if (!node || node->type != "output") {
        fail(QStringLiteral("viewer output '%1' is not an Output node").arg(trimmed));
        return;
    }
    if (outputName_ == trimmed)
        return;
    outputName_ = trimmed;
    emit outputChanged();
    lastRequest_.reset();
    refreshRequest();
}

QVariantList ViewerController::timelineClips() const {
    QVariantList result;
    for (const auto& [key, source] : document_.sources) {
        const auto local = static_cast<std::int64_t>(frame_);
        qlonglong sourceFrame = -1;
        try {
            sourceFrame = static_cast<qlonglong>(source.frameAt(local));
        } catch (const std::exception&) {
            // Keep the strip inspectable while an out-of-coverage mapping is
            // being edited; evaluation reports the precise source error.
        }
        QVariantMap clip{{QStringLiteral("id"), QString::fromStdString(key)},
                         {QStringLiteral("source"), QString::fromStdString(key)},
                         {QStringLiteral("start"), 0},
                         {QStringLiteral("end"), frameCount_},
                         {QStringLiteral("offset"), QVariant::fromValue<qlonglong>(source.frameOffset)},
                         {QStringLiteral("step"), QVariant::fromValue<qlonglong>(source.frameStep)},
                         {QStringLiteral("sourceFrame"), QVariant::fromValue<qlonglong>(sourceFrame)}};
        result.push_back(std::move(clip));
    }
    return result;
}

qulonglong ViewerController::queued() const {
    return schedulerCounts_.queued;
}

qulonglong ViewerController::dropped() const {
    return schedulerCounts_.dropped;
}

qulonglong ViewerController::staleRejected() const {
    return schedulerCounts_.staleRejected;
}

qulonglong ViewerController::completed() const {
    return schedulerCounts_.completed;
}

void ViewerController::pollScheduler() {
    auto counts = runtime_->counts();
    if (counts == schedulerCounts_)
        return;
    schedulerCounts_ = std::move(counts);
    emit schedulerChanged();
}

void ViewerController::documentChanged() {
    error_.clear();
    pending_ = false;
    outdated_ = static_cast<bool>(presentation_);
    const auto* selected = document_.graph.nodeByName(outputName_.toStdString());
    if (!selected || selected->type != "output") {
        const auto outputs = outputNames();
        const QString replacement = outputs.isEmpty() ? QStringLiteral("result") : outputs.front();
        if (replacement != outputName_) {
            outputName_ = replacement;
            emit outputChanged();
        }
    }
    emit graphChanged();
    emit timelineChanged();
    emit historyChanged();
    emit statusChanged();
    invalidateRequest();
    refreshRequest();
}

void ViewerController::invalidateRequest() {
    lastRequest_.reset();
    pending_ = false;
    outdated_ = static_cast<bool>(presentation_);
    generation_ = ++nextRequestId_;
    rangeGeneration_ = 0;
    const bool hadRangeError = !rangeError_.isEmpty();
    rangeError_.clear();
    runtime_->cancel(generation_);
    pollScheduler();
    if (hadRangeError)
        emit schedulerChanged();
}

void ViewerController::buildGraph(const SourceReference& reference) {
    if (!document_.sources.empty()) {
        commands_.push(setSourceCommand("src", reference));
        return;
    }
    const auto before = std::make_shared<Document>(document_);
    commands_.push(Command{"open source",
                           [reference](Document& document) {
                               Document next = document;
                               const auto source = std::make_shared<NodeId>();
                               const auto background = std::make_shared<NodeId>();
                               const auto merge = std::make_shared<NodeId>();
                               const auto output = std::make_shared<NodeId>();
                               addNodeCommand("source", "source", source).apply(next);
                               addNodeCommand("constcolor", "background", background).apply(next);
                               addNodeCommand("merge", "composite", merge).apply(next);
                               addNodeCommand("output", "result", output).apply(next);
                               setParamCommand("source", "source", "src").apply(next);
                               setParamCommand("background", "color", "0 0 0 0").apply(next);
                               connectCommand({*source, 0}, {*merge, 0}).apply(next);
                               connectCommand({*background, 0}, {*merge, 1}).apply(next);
                               connectCommand({*merge, 0}, {*output, 0}).apply(next);
                               setSourceCommand("src", reference).apply(next);
                               document = std::move(next);
                           },
                           [before](Document& document) { document = *before; }});
}
void ViewerController::addGraphNode(const QString& type, const QString& name) {
    const auto trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        fail(QStringLiteral("graph node name must not be empty"));
        return;
    }
    try {
        commands_.push(addNodeCommand(type.toStdString(), trimmedName.toStdString()));
        documentChanged();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::connectGraphNodes(qulonglong fromNode, int fromPort, qulonglong toNode, int toPort) {
    if (fromPort < 0 || toPort < 0) {
        fail(QStringLiteral("graph ports must be non-negative"));
        return;
    }
    try {
        commands_.push(connectCommand({static_cast<NodeId>(fromNode), static_cast<std::uint32_t>(fromPort)},
                                      {static_cast<NodeId>(toNode), static_cast<std::uint32_t>(toPort)}));
        documentChanged();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::setNodeParameter(const QString& nodeName, const QString& key, const QString& value) {
    if (nodeName.trimmed().isEmpty() || key.trimmed().isEmpty()) {
        fail(QStringLiteral("node parameter requires a node and key"));
        return;
    }
    const auto* node = document_.graph.nodeByName(nodeName.toStdString());
    if (node) {
        const auto it = node->params.find(key.toStdString());
        if (it != node->params.end() && it->second == value.toStdString())
            return;
    }
    try {
        commands_.push(setParamCommand(nodeName.toStdString(), key.toStdString(), value.toStdString()));
        documentChanged();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::slipTimelineClip(const QString& source, int delta) {
    const auto key = source.trimmed().toStdString();
    const auto it = document_.sources.find(key);
    if (it == document_.sources.end()) {
        fail(QStringLiteral("timeline source '%1' is unavailable").arg(source));
        return;
    }
    if (delta == 0)
        return;
    SourceReference replacement = it->second;
    if ((delta > 0 && replacement.frameOffset > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && replacement.frameOffset < std::numeric_limits<std::int64_t>::min() - delta)) {
        fail(QStringLiteral("timeline slip exceeds source timing range"));
        return;
    }
    replacement.frameOffset += delta;
    try {
        // Slip changes the source local-time interval without moving the
        // parent placement. SourceReference is the persistent timing mapping.
        commands_.push(setSourceCommand(key, replacement));
        documentChanged();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::retimeTimelineClip(const QString& source, int step) {
    const auto key = source.trimmed().toStdString();
    const auto it = document_.sources.find(key);
    if (it == document_.sources.end()) {
        fail(QStringLiteral("timeline source '%1' is unavailable").arg(source));
        return;
    }
    if (step == 0) {
        fail(QStringLiteral("timeline source frame step must not be zero"));
        return;
    }
    SourceReference replacement = it->second;
    if (replacement.frameStep == step)
        return;
    replacement.frameStep = step;
    try {
        // Retime is source timing: each composition frame advances `step`
        // source frames before downstream graph processing.
        commands_.push(setSourceCommand(key, replacement));
        documentChanged();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

bool ViewerController::undo() {
    if (!commands_.canUndo())
        return false;
    try {
        if (!commands_.undo())
            return false;
        documentChanged();
        return true;
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::redo() {
    if (!commands_.canRedo())
        return false;
    try {
        if (!commands_.redo())
            return false;
        documentChanged();
        return true;
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

void ViewerController::cancelRender() {
    invalidateRequest();
    status_ = presentation_ ? QStringLiteral("Cancelled; displayed frame is outdated")
                            : QStringLiteral("Cancelled; no frame is displayed");
    emit statusChanged();
    pollScheduler();
}

void ViewerController::requestRange(int first, int last) {
    if (first > last) {
        fail(QStringLiteral("cache range start must not exceed end"));
        return;
    }
    if (!lastRequest_) {
        fail(QStringLiteral("cache range requires a current viewer request"));
        return;
    }
    if (frameCount_ > 0) {
        first = std::clamp(first, 0, frameCount_ - 1);
        last = std::clamp(last, 0, frameCount_ - 1);
    } else {
        first = std::max(0, first);
        last = std::max(first, last);
    }
    rangeGeneration_ = ++nextRequestId_;
    rangeError_.clear();
    emit schedulerChanged();
    if (!runtime_->requestRange(document_, *lastRequest_, first, last, rangeGeneration_)) {
        status_ = QStringLiteral("Cache range admission rejected; see scheduler drop count");
        emit statusChanged();
        pollScheduler();
        return;
    }
    status_ = QStringLiteral("Caching requested range %1–%2; viewer identity unchanged").arg(first).arg(last);
    emit statusChanged();
    pollScheduler();
}

void ViewerController::openSource(const QString& path) {
    if (path.trimmed().isEmpty()) {
        fail(QStringLiteral("source path is empty"));
        return;
    }
    try {
        SourceReference reference;
        reference.path = QFileInfo(path).absoluteFilePath().toStdString();
        if (const auto previous = document_.sources.find("src"); previous != document_.sources.end()) {
            if (previous->second.revision == std::numeric_limits<std::uint64_t>::max())
                throw std::runtime_error("source revision exhausted");
            reference.revision = previous->second.revision + 1;
        }
        buildGraph(reference);
        documentChanged();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::receive() {
    auto result = runtime_->takeResult();
    if (!result)
        return;
    pollScheduler();
    if (auto* failure = std::get_if<ViewerFailure>(&*result)) {
        if (failure->requestId == generation_ || failure->requestId == 0)
            fail(QString::fromStdString(failure->message));
    } else if (auto* probe = std::get_if<SourceProbeResult>(&*result)) {
        if (probe->requestId != generation_)
            return;
        const auto& info = probe->source.info;
        probedSource_ = document_.sources.at("src");
        pending_ = false;
        sourceSize_ = QSizeF(info.width, info.height);
        pixelAspect_ = info.pixelAspect;
        frameCount_ = static_cast<int>(std::min<std::int64_t>(info.frameCount, std::numeric_limits<int>::max()));
        sourceDescription_ =
            QStringLiteral("%1x%2 %3; decode selection: %4")
                .arg(info.width)
                .arg(info.height)
                .arg(QString::fromStdString(info.codecName))
                .arg(probe->source.decision.hardware ? QStringLiteral("Vulkan")
                                                     : QString::fromStdString(probe->source.decision.reason));
        emit sourceChanged();
        emit timelineChanged();
        refreshRequest();
    } else {
        auto frame = std::get<std::shared_ptr<const ViewerResult>>(std::move(*result));
        if (frame->requestId != generation_ || frame->revision != document_.stateRevision())
            return;
        presentation_ = std::move(frame);
        pending_ = false;
        outdated_ = false;
        effectiveScale_ = presentation_->request.samplingScale;
        error_.clear();
        status_ = QStringLiteral("Displayed %1x%2, 1:%3, frame %4; %5; %6")
                      .arg(presentation_->frame.width)
                      .arg(presentation_->frame.height)
                      .arg(effectiveScale_)
                      .arg(presentation_->request.localTime)
                      .arg(presentation_->cacheHit ? QStringLiteral("compressed cache") : QStringLiteral("live render"))
                      .arg(sourceDescription_);
        emit effectiveScaleChanged();
        emit frameArrived();
        emit statusChanged();
    }
}

void ViewerController::refreshRequest() {
    try {
        const auto source = document_.sources.find("src");
        if (source == document_.sources.end()) {
            sourceSize_ = {};
            probedSource_ = {};
            frameCount_ = -1;
            presentation_.reset();
            pending_ = false;
            outdated_ = false;
            status_ = QStringLiteral("No source");
            emit sourceChanged();
            emit frameArrived();
            emit statusChanged();
            return;
        }
        const auto& reference = source->second;
        if (!hasSource() || reference.path != probedSource_.path || reference.revision != probedSource_.revision ||
            reference.interpretation != probedSource_.interpretation) {
            sourceSize_ = {};
            frameCount_ = -1;
            pending_ = true;
            status_ = QStringLiteral("Probing %1").arg(QString::fromStdString(reference.path));
            emit sourceChanged();
            emit statusChanged();
            generation_ = ++nextRequestId_;
            if (!runtime_->probe(document_, "src", generation_))
                fail(QStringLiteral("Source probe admission rejected"));
            return;
        }
        if (viewport_.isEmpty())
            return;
        const int width = static_cast<int>(sourceSize_.width());
        const int height = static_cast<int>(sourceSize_.height());
        const auto mode = mode_ == "full"      ? ViewerResolution::Full
                          : mode_ == "half"    ? ViewerResolution::Half
                          : mode_ == "quarter" ? ViewerResolution::Quarter
                                               : ViewerResolution::Auto;
        EvaluationRequest request;
        request.output = resolveOutput(document_, outputName_.toStdString());
        request.localTime = frame_;
        request.samplingScale =
            policy_.resolve(mode, width, height, pixelAspect_, viewport_.width(), viewport_.height(), zoom_);
        const auto fit = aspectFit(width, height, pixelAspect_, viewport_.width(), viewport_.height());
        const double sx = fit.width / width * zoom_;
        const double sy = fit.height / height * zoom_;
        const double visibleWidth = std::min<double>(width, viewport_.width() / sx);
        const double visibleHeight = std::min<double>(height, viewport_.height() / sy);
        const double centerX = std::clamp(width / 2.0 + pan_.x(), visibleWidth / 2.0, width - visibleWidth / 2.0);
        const double centerY = std::clamp(height / 2.0 + pan_.y(), visibleHeight / 2.0, height - visibleHeight / 2.0);
        const int x = std::max(0, static_cast<int>(std::floor(centerX - visibleWidth / 2)));
        const int y = std::max(0, static_cast<int>(std::floor(centerY - visibleHeight / 2)));
        const int right = std::min(width, static_cast<int>(std::ceil(centerX + visibleWidth / 2)));
        const int bottom = std::min(height, static_cast<int>(std::ceil(centerY + visibleHeight / 2)));
        request.region = {x, y, right - x, bottom - y};
        request.fullWidth = width;
        request.fullHeight = height;
        const auto revision = document_.stateRevision();
        if (lastRequest_ && *lastRequest_ == request && lastRevision_ == revision)
            return;
        lastRequest_ = request;
        lastRevision_ = revision;
        const auto id = generation_ = ++nextRequestId_;
        if (!runtime_->submit(document_, request, id))
            throw std::runtime_error("Viewer request admission rejected");
        pending_ = true;
        outdated_ = static_cast<bool>(presentation_);
        error_.clear();
        status_ = presentation_ ? QStringLiteral("Pending 1:%1; previous frame is outdated").arg(request.samplingScale)
                                : QStringLiteral("Rendering 1:%1").arg(request.samplingScale);
        emit statusChanged();
        pollScheduler();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

QRectF ViewerController::presentedRegion() const {
    if (!presentation_)
        return {};
    const auto& region = presentation_->request.region;
    return QRectF(region.x, region.y, region.width, region.height);
}
void ViewerController::setResolutionMode(const QString& mode) {
    if (mode != "auto" && mode != "full" && mode != "half" && mode != "quarter") {
        fail(QStringLiteral("unknown viewer resolution: %1").arg(mode));
        return;
    }
    if (mode_ == mode)
        return;
    mode_ = mode;
    emit resolutionChanged();
    refreshRequest();
}
void ViewerController::setZoom(double value) {
    if (!std::isfinite(value))
        return;
    value = std::clamp(value, 0.05, 32.0);
    if (value == zoom_)
        return;
    zoom_ = value;
    emit zoomChanged();
    refreshRequest();
}
void ViewerController::zoomBy(double factor) {
    if (factor > 0)
        setZoom(zoom_ * factor);
}
void ViewerController::setPan(QPointF value) {
    if (!std::isfinite(value.x()) || !std::isfinite(value.y()) || value == pan_)
        return;
    pan_ = value;
    emit panChanged();
    refreshRequest();
}
void ViewerController::resetView() {
    pan_ = {};
    zoom_ = 1.0;
    emit panChanged();
    emit zoomChanged();
    refreshRequest();
}
void ViewerController::setFrame(int value) {
    value = std::max(value, 0);
    if (frameCount_ > 0)
        value = std::min(value, frameCount_ - 1);
    if (frame_ == value)
        return;
    frame_ = value;
    emit frameChanged();
    emit timelineChanged();
    refreshRequest();
}
void ViewerController::viewportChanged(QSizeF pixels) {
    if (viewport_ == pixels)
        return;
    viewport_ = pixels;
    refreshRequest();
}
void ViewerController::attachWindow(QQuickWindow* window) {
    const auto error = runtime_->attachToWindow(window);
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    connect(
        window, &QQuickWindow::beforeRendering, this,
        [this, window] { presentationState_->beginFrame(window, runtime_->presentationDevice()); },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            const auto result = presentationState_->takeNewPresentedFrame();
            if (!result)
                return;
            const double elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - result->requestedAt)
                    .count();
            emit framePresented(static_cast<int>(result->request.localTime), result->frame.width, result->frame.height,
                                result->cacheHit, elapsed);
        },
        Qt::DirectConnection);
    window->show();
}
void ViewerController::attachViewerItem(ViewerItem* item) {
    if (!items_.contains(item))
        items_.append(item);
    if (!primary_)
        setPrimaryViewerItem(item);
}
void ViewerController::detachViewerItem(ViewerItem* item) {
    items_.removeAll(item);
    if (primary_ == item) {
        primary_.clear();
        for (const auto& remaining : items_) {
            if (remaining && remaining->isVisible()) {
                setPrimaryViewerItem(remaining);
                break;
            }
        }
    }
}
void ViewerController::setPrimaryViewerItem(ViewerItem* item) {
    primary_ = item;
    for (const auto& viewer : items_)
        if (viewer)
            viewer->setPrimary(viewer == item);
}
void ViewerController::fail(QString message) {
    error_ = std::move(message);
    pending_ = false;
    outdated_ = static_cast<bool>(presentation_);
    status_ = presentation_ ? QStringLiteral("Failed; displayed frame is outdated") : QStringLiteral("Failed");
    emit statusChanged();
}
}  // namespace nemo::ui
