#include "ViewerController.hpp"
#include "ViewerItem.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

#include <QFileInfo>
#include <QQuickWindow>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
namespace nemo::ui {
namespace {
const char* parameterTypeName(nemo::ParameterType type) {
    switch (type) {
    case nemo::ParameterType::Boolean:
        return "boolean";
    case nemo::ParameterType::Integer:
        return "integer";
    case nemo::ParameterType::Float:
        return "float";
    case nemo::ParameterType::Color:
        return "color";
    case nemo::ParameterType::String:
        return "string";
    }
    return "string";
}
}  // namespace
ViewerController::ViewerController(ViewerRuntime* runtime, nemo::ProjectSession& session)
    : runtime_(runtime), session_(session), schedulerPoll_(this),
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
    sessionSubscription_ = session_.subscribe(this, &ViewerController::sessionDocumentChanged);
}

void ViewerController::sessionDocumentChanged(void* context) noexcept {
    auto* controller = static_cast<ViewerController*>(context);
    try {
        controller->documentChanged();
    } catch (const std::exception& error) {
        controller->fail(QString::fromUtf8(error.what()));
    }
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
    for (NodeId after = kInvalidNode;;) {
        const auto page = session_.queryNodes({}, 256, after);
        for (const auto& node : page) {
            const auto* descriptor = session_.document().graph.descriptor(node.type);
            if (descriptor && descriptor->isOutput)
                result.push_back(QString::fromStdString(node.name));
        }
        if (page.size() < 256)
            break;
        after = page.back().id;
    }
    return result;
}

QVariantList ViewerController::graphNodes() const {
    QVariantList result;
    for (NodeId after = kInvalidNode;;) {
        const auto page = session_.queryNodes({}, 256, after);
        for (const auto& node : page) {
            QVariantMap params;
            for (std::string keyAfter;;) {
                const auto values = session_.queryValues(node.id, {}, 256, keyAfter);
                for (const auto& value : values)
                    params.insert(QString::fromStdString(value.key), QString::fromStdString(value.value));
                if (values.size() < 256)
                    break;
                keyAfter = values.back().key;
            }
            result.push_back(QVariantMap{{QStringLiteral("id"), QString::number(node.id)},
                                         {QStringLiteral("type"), QString::fromStdString(node.type)},
                                         {QStringLiteral("name"), QString::fromStdString(node.name)},
                                         {QStringLiteral("params"), params}});
        }
        if (page.size() < 256)
            break;
        after = page.back().id;
    }
    return result;
}

QVariantList ViewerController::graphEdges() const {
    QVariantList result;
    for (EdgeId after = kInvalidEdge;;) {
        const auto page = session_.queryEdges(kInvalidNode, 256, after);
        for (const auto& edge : page)
            result.push_back(QVariantMap{{QStringLiteral("id"), QString::number(edge.id)},
                                         {QStringLiteral("fromNode"), QString::number(edge.from.node)},
                                         {QStringLiteral("fromPort"), static_cast<int>(edge.from.port)},
                                         {QStringLiteral("toNode"), QString::number(edge.to.node)},
                                         {QStringLiteral("toPort"), static_cast<int>(edge.to.port)}});
        if (page.size() < 256)
            break;
        after = page.back().id;
    }
    return result;
}

QVariantList ViewerController::nodeCatalog() const {
    QVariantList result;
    for (const auto& descriptor : session_.document().graph.catalog().descriptors()) {
        QVariantList inputs;
        for (const auto& port : descriptor.inputs)
            inputs.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(port.name)},
                                         {QStringLiteral("kind"), QStringLiteral("color")}});
        QVariantList outputs;
        for (const auto& port : descriptor.outputs)
            outputs.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(port.name)},
                                          {QStringLiteral("kind"), QStringLiteral("color")}});
        QVariantList parameters;
        for (const auto& parameter : descriptor.parameters) {
            QVariantMap value{{QStringLiteral("name"), QString::fromStdString(parameter.name)},
                              {QStringLiteral("type"), QString::fromLatin1(parameterTypeName(parameter.type))},
                              {QStringLiteral("defaultValue"), QString::fromStdString(parameter.defaultValue)}};
            if (parameter.minimum)
                value.insert(QStringLiteral("minimum"), *parameter.minimum);
            if (parameter.maximum)
                value.insert(QStringLiteral("maximum"), *parameter.maximum);
            QVariantList choices;
            for (const auto& choice : parameter.choices)
                choices.push_back(QString::fromStdString(choice));
            value.insert(QStringLiteral("choices"), choices);
            parameters.push_back(value);
        }
        QVariantList samplingScales;
        for (const int scale : descriptor.capabilities.samplingScales)
            samplingScales.push_back(scale);
        QStringList channels;
        for (const auto& channel : descriptor.capabilities.channels)
            channels.push_back(QString::fromStdString(channel));
        result.push_back(
            QVariantMap{{QStringLiteral("type"), QString::fromStdString(descriptor.type)},
                        {QStringLiteral("displayName"), QString::fromStdString(descriptor.displayName)},
                        {QStringLiteral("group"), QString::fromStdString(descriptor.group)},
                        {QStringLiteral("version"), QVariant::fromValue<qulonglong>(descriptor.implementationVersion)},
                        {QStringLiteral("inputs"), inputs},
                        {QStringLiteral("outputs"), outputs},
                        {QStringLiteral("parameters"), parameters},
                        {QStringLiteral("samplingScales"), samplingScales},
                        {QStringLiteral("channels"), channels},
                        {QStringLiteral("temporal"), descriptor.capabilities.temporal}});
    }
    return result;
}

void ViewerController::setOutputName(const QString& name) {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        fail(QStringLiteral("viewer output name must not be empty"));
        return;
    }
    const auto* node = session_.document().graph.nodeByName(trimmed.toStdString());
    const auto* descriptor = node ? session_.document().graph.descriptor(node->type) : nullptr;
    if (!descriptor || !descriptor->isOutput) {
        fail(QStringLiteral("viewer output '%1' is not an Output node").arg(trimmed));
        return;
    }
    if (outputNode_ == node->id) {
        if (outputName_ != trimmed) {
            outputName_ = trimmed;
            emit outputChanged();
        }
        return;
    }
    outputNode_ = node->id;
    outputName_ = trimmed;
    emit outputChanged();
    lastRequest_.reset();
    refreshRequest();
}

QVariantList ViewerController::timelineClips() const {
    QVariantList result;
    for (const auto& [key, source] : session_.document().sources) {
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
    const auto* selected = session_.document().graph.node(outputNode_);
    const auto* descriptor = selected ? session_.document().graph.descriptor(selected->type) : nullptr;
    if (!descriptor || !descriptor->isOutput) {
        const auto outputs = outputNames();
        const QString replacement = outputs.isEmpty() ? QStringLiteral("result") : outputs.front();
        const auto* replacementNode =
            outputs.isEmpty() ? nullptr : session_.document().graph.nodeByName(replacement.toStdString());
        const NodeId replacementId = replacementNode ? replacementNode->id : kInvalidNode;
        if (replacement != outputName_ || replacementId != outputNode_) {
            outputName_ = replacement;
            outputNode_ = replacementId;
            emit outputChanged();
        }
    } else if (outputName_ != QString::fromStdString(selected->name)) {
        outputName_ = QString::fromStdString(selected->name);
        emit outputChanged();
    }
    emit graphChanged();
    emit catalogChanged();
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

nemo::EditOptions ViewerController::editOptions() const {
    return nemo::EditOptions{.expectedRevision = session_.revision()};
}

bool ViewerController::applyEdit(const nemo::EditResult& result) {
    if (result.committed)
        return true;
    if (result.error)
        fail(QString::fromStdString(result.error->message));
    else
        fail(QStringLiteral("edit was rejected"));
    return false;
}

void ViewerController::buildGraph(const SourceReference& reference) {
    try {
        if (!session_.document().sources.empty()) {
            applyEdit(session_.submit(setSourceCommand("src", reference), editOptions()));
            return;
        }
        const auto result =
            session_.submit(Command{"open source",
                                    [reference](Document& document) {
                                        const auto source = std::make_shared<NodeId>();
                                        const auto background = std::make_shared<NodeId>();
                                        const auto merge = std::make_shared<NodeId>();
                                        const auto output = std::make_shared<NodeId>();
                                        addNodeCommand("source", "source", source).apply(document);
                                        addNodeCommand("constcolor", "background", background).apply(document);
                                        addNodeCommand("merge", "composite", merge).apply(document);
                                        addNodeCommand("output", "result", output).apply(document);
                                        setParamCommand(*source, "source", "src").apply(document);
                                        setParamCommand(*background, "color", "0 0 0 0").apply(document);
                                        connectCommand({*source, 0}, {*merge, 0}).apply(document);
                                        connectCommand({*background, 0}, {*merge, 1}).apply(document);
                                        connectCommand({*merge, 0}, {*output, 0}).apply(document);
                                        setSourceCommand("src", reference).apply(document);
                                    }},
                            editOptions());
        applyEdit(result);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::addGraphNode(const QString& type, const QString& name) {
    const auto trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        fail(QStringLiteral("graph node name must not be empty"));
        return;
    }
    try {
        applyEdit(session_.submit(addNodeCommand(type.toStdString(), trimmedName.toStdString()), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::connectGraphNodes(const QVariant& fromValue, int fromPort, const QVariant& toValue, int toPort) {
    bool fromOk = false, toOk = false;
    const auto fromNode = fromValue.toString().toULongLong(&fromOk);
    const auto toNode = toValue.toString().toULongLong(&toOk);
    if (!fromOk || !toOk) {
        fail(QStringLiteral("graph connection requires exact node IDs"));
        return;
    }
    if (fromPort < 0 || toPort < 0) {
        fail(QStringLiteral("graph ports must be non-negative"));
        return;
    }
    try {
        applyEdit(session_.submit(connectCommand({static_cast<NodeId>(fromNode), static_cast<std::uint32_t>(fromPort)},
                                                 {static_cast<NodeId>(toNode), static_cast<std::uint32_t>(toPort)}),
                                  editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::setNodeParameter(const QVariant& nodeValue, const QString& key, const QString& value) {
    // QML identities travel as decimal strings, not lossy JavaScript doubles.
    bool validId = false;
    const auto nodeId = nodeValue.toString().toULongLong(&validId);
    if (!validId || nodeId == static_cast<qulonglong>(kInvalidNode) || key.trimmed().isEmpty()) {
        fail(QStringLiteral("node parameter requires a node ID and key"));
        return;
    }
    const auto* node = session_.document().graph.node(static_cast<NodeId>(nodeId));
    if (!node) {
        fail(QStringLiteral("node parameter target does not exist"));
        return;
    }
    const auto it = node->params.find(key.toStdString());
    if (it != node->params.end() && it->second == value.toStdString())
        return;
    try {
        applyEdit(session_.submit(setParamCommand(static_cast<NodeId>(nodeId), key.toStdString(), value.toStdString()),
                                  editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::slipTimelineClip(const QString& source, int delta) {
    const auto key = source.trimmed().toStdString();
    const auto it = session_.document().sources.find(key);
    if (it == session_.document().sources.end()) {
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
        applyEdit(session_.submit(setSourceCommand(key, replacement), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void ViewerController::retimeTimelineClip(const QString& source, int step) {
    const auto key = source.trimmed().toStdString();
    const auto it = session_.document().sources.find(key);
    if (it == session_.document().sources.end()) {
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
        applyEdit(session_.submit(setSourceCommand(key, replacement), editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

bool ViewerController::undo() {
    if (!session_.canUndo())
        return false;
    try {
        return applyEdit(session_.undo(editOptions()));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ViewerController::redo() {
    if (!session_.canRedo())
        return false;
    try {
        return applyEdit(session_.redo(editOptions()));
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
    if (!runtime_->requestRange(session_.snapshot(), *lastRequest_, first, last, rangeGeneration_)) {
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
        if (const auto previous = session_.document().sources.find("src");
            previous != session_.document().sources.end()) {
            if (previous->second.revision == std::numeric_limits<std::uint64_t>::max())
                throw std::runtime_error("source revision exhausted");
            reference.revision = previous->second.revision + 1;
        }
        buildGraph(reference);
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
        probedSource_ = session_.document().sources.at("src");
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
        if (frame->requestId != generation_ || frame->revision != session_.document().stateRevision())
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
        // Capture one immutable project state for the whole request. The
        // session remains owner-thread-only; workers receive this snapshot.
        const Document document = session_.snapshot();
        const auto source = document.sources.find("src");
        if (source == document.sources.end()) {
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
            if (!runtime_->probe(document, "src", generation_))
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
        request.output = outputNode_;
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
        const auto revision = document.stateRevision();
        if (lastRequest_ && *lastRequest_ == request && lastRevision_ == revision)
            return;
        lastRequest_ = request;
        lastRevision_ = revision;
        const auto id = generation_ = ++nextRequestId_;
        if (!runtime_->submit(document, request, id))
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
