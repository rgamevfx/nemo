#include "ViewportPicker.hpp"

#include "ViewerRuntime.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QVariantMap>

#include <cmath>
#include <utility>

namespace nemo::ui {
namespace {
QVariantMap inspectorRow(const QVariantMap& inspector, const QString& key) {
    const QVariantList sections = inspector.value(QStringLiteral("sections")).toList();
    for (const QVariant& sectionValue : sections) {
        const QVariantList rows = sectionValue.toMap().value(QStringLiteral("parameters")).toList();
        for (const QVariant& rowValue : rows) {
            const QVariantMap row = rowValue.toMap();
            if (row.value(QStringLiteral("key")).toString() == key)
                return row;
        }
    }
    return {};
}
}  // namespace

ViewportPicker::ViewportPicker(ViewerRuntime& runtime, ViewerController& controller, nemo::ProjectSession& session,
                               QObject* parent)
    : QObject(parent), runtime_(runtime), controller_(controller), session_(session) {
    connect(&runtime, &ViewerRuntime::resultReady, this, &ViewportPicker::receive, Qt::QueuedConnection);
    connect(&controller, &ViewerController::frameChanged, this, &ViewportPicker::cancel);
    sessionSubscription_ = session.subscribe(this, &ViewportPicker::sessionChanged);
}

ViewportPicker::~ViewportPicker() {
    release();
    if (destination_)
        runtime_.retireDestination(*destination_);
}

void ViewportPicker::sessionChanged(void* context) noexcept {
    auto* picker = static_cast<ViewportPicker*>(context);
    try {
        picker->documentChanged();
    } catch (...) {
        // Never propagate through ProjectSession publication. A later result
        // must still pass the revision and gesture validity checks in receive().
    }
}

void ViewportPicker::setStatus(const QString& text) {
    if (status_ == text)
        return;
    status_ = text;
    emit statusChanged();
}

void ViewportPicker::documentChanged() {
    if (!active() || (session_.document().stateRevision() == armedRevision_ &&
                      session_.projectGeneration() == armedProjectGeneration_))
        return;
    release();
    setStatus(QStringLiteral("The project changed while the picker was armed; nothing was authored"));
}

bool ViewportPicker::begin(const QString& networkId, const QVariant& nodeId, const QString& parameterKey) {
    const QString key = parameterKey.trimmed();
    const QVariantMap inspector = controller_.parameterInspector(networkId, nodeId);
    if (!inspector.value(QStringLiteral("available")).toBool()) {
        const QString reason = inspector.value(QStringLiteral("reason")).toString();
        setStatus(reason.isEmpty() ? QStringLiteral("That node parameter is unavailable") : reason);
        return false;
    }
    const QVariantMap row = inspectorRow(inspector, key);
    const QVariantList captured = row.value(QStringLiteral("value")).toList();
    if (row.value(QStringLiteral("type")).toString() != QStringLiteral("color") || captured.size() != 4) {
        setStatus(QStringLiteral("Parameter '%1' is not an available color value").arg(key));
        return false;
    }
    controller_.parameterInteraction().acquire(this,
                                               [](void* owner) { static_cast<ViewportPicker*>(owner)->cancel(); });
    target_ = Target{networkId, nodeId, key, captured};
    armedRevision_ = session_.document().stateRevision();
    armedProjectGeneration_ = session_.projectGeneration();
    setStatus(QStringLiteral("Click a viewer to sample %1; Escape cancels").arg(key));
    emit activeChanged();
    return true;
}

void ViewportPicker::cancel() {
    release();
    setStatus({});
}

void ViewportPicker::release() {
    const bool wasActive = active();
    target_.reset();
    armedRevision_ = 0;
    armedProjectGeneration_ = 0;
    controller_.parameterInteraction().release(this);
    dropOutstanding();
    if (wasActive)
        emit activeChanged();
}

void ViewportPicker::dropOutstanding() {
    if (outstanding_ == 0)
        return;
    runtime_.cancel(++nextRequestId_, *destination_);
    outstanding_ = 0;
    submittedRevision_ = 0;
    displayed_ = {};
    QObject::disconnect(viewerDestruction_);
    viewerDestruction_ = {};
    sampledViewer_.clear();
    emit pickingChanged();
}

std::optional<eval::ViewerDestination> ViewportPicker::sampleDestination() {
    if (!destination_)
        destination_ = runtime_.allocateDestination(QStringLiteral("viewport-picker"));
    return destination_;
}

bool ViewportPicker::sample(QObject* viewerController, const double imageX, const double imageY) {
    if (!active() || outstanding_ != 0) {
        setStatus(active() ? QStringLiteral("A viewport sample is already in flight")
                           : QStringLiteral("No viewport pick is armed"));
        return false;
    }
    auto* viewer = qobject_cast<ViewerController*>(viewerController);
    if (!viewer || !viewer->displayedFrameIdentity()) {
        setStatus(QStringLiteral("This viewer has no current displayed frame to sample"));
        return false;
    }
    auto demand = viewer->workingSampleRequest(imageX, imageY);
    if (!demand) {
        setStatus(QStringLiteral("That point is outside the displayed image"));
        return false;
    }
    const auto destination = sampleDestination();
    if (!destination) {
        setStatus(QStringLiteral("Viewport sampling is unavailable: every viewer destination is in use"));
        return false;
    }
    const auto id = ++nextRequestId_;
    const auto revision = session_.document().stateRevision();
    if (!runtime_.sample(std::move(demand->document), demand->request, id, *destination, session_.colorConfigPath())) {
        setStatus(QStringLiteral("Viewport sample admission was rejected"));
        return false;
    }
    outstanding_ = id;
    submittedRevision_ = revision;
    displayed_ = demand->displayed;
    sampledViewer_ = viewer;
    viewerDestruction_ = connect(viewer, &QObject::destroyed, this, &ViewportPicker::cancel);
    setStatus(QStringLiteral("Sampling working RGB; Escape cancels"));
    emit pickingChanged();
    return true;
}

void ViewportPicker::receive() {
    if (!destination_)
        return;
    auto result = runtime_.takeResult(*destination_);
    if (!result)
        return;
    if (auto* failure = std::get_if<ViewerFailure>(&*result)) {
        if (failure->requestId == outstanding_) {
            dropOutstanding();
            setStatus(QString::fromStdString(failure->message));
        }
        return;
    }
    const auto* sample = std::get_if<ViewerWorkingSample>(&*result);
    if (!sample || sample->requestId != outstanding_)
        return;
    const auto displayed = displayed_;
    const auto revision = submittedRevision_;
    const QPointer<ViewerController> viewer = sampledViewer_;
    dropOutstanding();
    const auto current = viewer ? viewer->displayedFrameIdentity() : std::nullopt;
    if (!current || *current != displayed || session_.document().stateRevision() != revision) {
        release();
        setStatus(QStringLiteral("The viewer or project changed; the sample was discarded"));
        return;
    }
    for (const float component : sample->rgba) {
        if (!std::isfinite(component)) {
            setStatus(QStringLiteral("The sampled pixel is not a finite color"));
            return;
        }
    }
    if (!target_)
        return;
    auto target = std::move(*target_);
    for (int index = 0; index < 3; ++index)
        target.value[index] = static_cast<double>(sample->rgba[static_cast<std::size_t>(index)]);

    // No transaction is reserved while waiting for a click or a worker. Only
    // this accepted result acquires the ordinary one-undo parameter gesture.
    release();
    const QString token = controller_.beginNodeParameterEdit(target.network, target.node, target.key);
    if (token.isEmpty()) {
        setStatus(controller_.error());
        return;
    }
    if (!controller_.updateNodeParameterEdit(token, target.value)) {
        const QString error = controller_.error();
        static_cast<void>(controller_.cancelNodeParameterEdit(token));
        setStatus(error);
        return;
    }
    setStatus(controller_.commitNodeParameterEdit(token) ? QString{} : controller_.error());
}
}  // namespace nemo::ui
