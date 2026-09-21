#pragma once

#include "ViewerController.hpp"
#include "nemo/eval/ViewerDestination.hpp"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <cstdint>
#include <optional>

namespace nemo::ui {
class ViewerRuntime;

// GUI-thread owner of one armed pick, not a document gesture. A viewer click
// queues one working-space pixel on a dedicated bounded runtime destination.
// Only a current result commits; cancel, stale results and failures author nothing.
// Runtime, facade and session are injected and must outlive this object.
class ViewportPicker final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool picking READ picking NOTIFY pickingChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
public:
    ViewportPicker(ViewerRuntime& runtime, ViewerController& controller, nemo::ProjectSession& session,
                   QObject* parent = nullptr);
    ~ViewportPicker() override;

    [[nodiscard]] bool active() const { return target_.has_value(); }
    [[nodiscard]] bool picking() const { return outstanding_ != 0; }
    [[nodiscard]] QString status() const { return status_; }

    // Uses the inspector's resolved address and animated value; alpha is retained.
    Q_INVOKABLE bool begin(const QString& networkId, const QVariant& nodeId, const QString& parameterKey);
    Q_INVOKABLE void cancel();
    // Full-resolution image coordinates from ViewerPanel's existing mapping.
    // A refused click leaves the picker armed and states the reason in status.
    Q_INVOKABLE bool sample(QObject* viewerController, double imageX, double imageY);

signals:
    void activeChanged();
    void pickingChanged();
    void statusChanged();

private:
    void receive();
    static void sessionChanged(void* context) noexcept;
    void documentChanged();
    void setStatus(const QString& text);
    void release();
    void dropOutstanding();
    [[nodiscard]] std::optional<eval::ViewerDestination> sampleDestination();

    ViewerRuntime& runtime_;
    ViewerController& controller_;
    nemo::ProjectSession& session_;
    struct Target {
        QString network;
        QVariant node;
        QString key;
        QVariantList value;
    };
    std::optional<Target> target_;
    std::uint64_t armedRevision_{};
    std::uint64_t armedProjectGeneration_{};
    std::uint64_t outstanding_{};
    std::uint64_t nextRequestId_{};
    std::uint64_t submittedRevision_{};
    ViewerController::DisplayedFrameIdentity displayed_;
    QPointer<ViewerController> sampledViewer_;
    QMetaObject::Connection viewerDestruction_;
    std::optional<eval::ViewerDestination> destination_;
    nemo::ProjectSession::Subscription sessionSubscription_;
    QString status_;
};
}  // namespace nemo::ui
