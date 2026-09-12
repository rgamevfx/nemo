#pragma once

#include "nemo/eval/ViewerDestination.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace nemo {
class ProjectSession;
}  // namespace nemo

namespace nemo::ui {
class ViewerController;
class ViewerRuntime;

// Panel-instance ownership of viewer renderers. Every viewer panel asks for its
// own ViewerController, which renders through its own scheduler destination;
// closing the panel retires that destination and destroys the controller. A
// destination is a bounded runtime handle, so an exhausted allocation leaves
// the controller as a command/metadata facade (visible through its
// hasDestination status) rather than fabricating an id.
class ViewerControllerRegistry final : public QObject {
    Q_OBJECT
public:
    ViewerControllerRegistry(ViewerRuntime* runtime, nemo::ProjectSession& session, QObject* parent = nullptr);
    ~ViewerControllerRegistry() override;

    // Borrowed by QML; the registry owns every controller it creates, and
    // repeated calls for one panel return the same controller.
    Q_INVOKABLE QObject* controller(const QString& panelId);
    // Retires the panel's destination, then destroys its controller. Requests
    // already in flight for the retired destination are rejected at
    // publication instead of reaching a destroyed controller.
    Q_INVOKABLE void release(const QString& panelId);
    Q_INVOKABLE int activeCount() const;
    // The oldest live controller, so the primary viewer is deterministic
    // regardless of panel-id ordering or panel reopen order. Null when no
    // panel holds a controller.
    Q_INVOKABLE QObject* primary() const;

signals:
    // Emitted when a panel acquires or releases its controller, so startup
    // wiring can wait for the first viewer panel instead of polling.
    void controllersChanged();

private:
    struct Entry {
        QString panelId;
        std::optional<eval::ViewerDestination> destination;
        std::unique_ptr<ViewerController> controller;
    };

    ViewerRuntime* runtime_;
    nemo::ProjectSession& session_;
    // Keyed by creation sequence: ordered iteration makes primary() the
    // first-created live controller.
    std::map<std::uint64_t, Entry> controllers_;
    std::map<QString, std::uint64_t> sequenceByPanel_;
    std::uint64_t nextSequence_{};
};
}  // namespace nemo::ui
