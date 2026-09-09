#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"

#include <QFileInfo>
#include <QQuickWindow>
#include <algorithm>
#include <cmath>
#include <limits>

namespace nemo::ui {
ViewerController::ViewerController(ViewerRuntime* runtime)
    : runtime_(runtime), commands_(document_), presentationState_(std::make_unique<WindowPresentationState>()) {
    connect(runtime_, &ViewerRuntime::resultReady, this, &ViewerController::receive, Qt::QueuedConnection);
}
ViewerController::~ViewerController() = default;

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
        sourceSize_ = {};
        presentation_.reset();
        lastRequest_.reset();
        frameCount_ = -1;
        error_.clear();
        status_ = QStringLiteral("Probing %1").arg(path);
        emit sourceChanged();
        emit frameArrived();
        emit statusChanged();
        runtime_->probe(document_, "src", ++generation_);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::receive() {
    auto result = runtime_->takeResult();
    if (!result)
        return;
    if (auto* failure = std::get_if<ViewerFailure>(&*result)) {
        if (failure->requestId == generation_ || failure->requestId == 0)
            fail(QString::fromStdString(failure->message));
    } else if (auto* probe = std::get_if<SourceProbeResult>(&*result)) {
        if (probe->requestId != generation_)
            return;
        const auto& info = probe->source.info;
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
        refreshRequest();
    } else {
        auto frame = std::get<std::shared_ptr<const ViewerResult>>(std::move(*result));
        if (frame->requestId != generation_ || frame->revision != document_.stateRevision())
            return;
        presentation_ = std::move(frame);
        effectiveScale_ = presentation_->request.samplingScale;
        error_.clear();
        status_ = QStringLiteral("Displayed %1x%2, 1:%3, frame %4; %5")
                      .arg(presentation_->frame.width)
                      .arg(presentation_->frame.height)
                      .arg(effectiveScale_)
                      .arg(presentation_->request.localTime)
                      .arg(sourceDescription_);
        emit effectiveScaleChanged();
        emit frameArrived();
        emit statusChanged();
    }
}

void ViewerController::refreshRequest() {
    if (!hasSource() || viewport_.isEmpty())
        return;
    try {
        const int width = static_cast<int>(sourceSize_.width());
        const int height = static_cast<int>(sourceSize_.height());
        const auto mode = mode_ == "full"      ? ViewerResolution::Full
                          : mode_ == "half"    ? ViewerResolution::Half
                          : mode_ == "quarter" ? ViewerResolution::Quarter
                                               : ViewerResolution::Auto;
        EvaluationRequest request;
        request.output = resolveOutput(document_, "result");
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
        runtime_->submit(document_, request, ++generation_);
        error_.clear();
        status_ = presentation_ ? QStringLiteral("Pending 1:%1; previous frame is outdated").arg(request.samplingScale)
                                : QStringLiteral("Rendering 1:%1").arg(request.samplingScale);
        emit statusChanged();
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
    status_ = presentation_ ? QStringLiteral("Failed; displayed frame is outdated") : QStringLiteral("Failed");
    emit statusChanged();
}
}  // namespace nemo::ui
