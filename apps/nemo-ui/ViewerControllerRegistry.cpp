#include "ViewerControllerRegistry.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"

#include <QDebug>
#include <QQmlEngine>

#include <utility>

namespace nemo::ui {
ViewerControllerRegistry::ViewerControllerRegistry(ViewerRuntime* runtime, nemo::ProjectSession& session,
                                                   QObject* parent)
    : QObject(parent), runtime_(runtime), session_(session) {}

ViewerControllerRegistry::~ViewerControllerRegistry() {
    // Publication identity is retired before any controller is destroyed, so a
    // worker-side completion cannot land in a partially destroyed facade. Live
    // controllers then follow as members, and panels released during QML
    // teardown follow as QObject children once this body has run.
    for (auto& entry : controllers_) {
        if (entry.second.destination)
            runtime_->retireDestination(*entry.second.destination);
    }
}

QObject* ViewerControllerRegistry::controller(const QString& panelId) {
    if (const auto existing = sequenceByPanel_.find(panelId); existing != sequenceByPanel_.end())
        return controllers_.at(existing->second).controller.get();
    Entry entry;
    entry.panelId = panelId;
    entry.controller = std::make_unique<ViewerController>(runtime_, session_);
    entry.destination = runtime_->allocateDestination(panelId);
    entry.controller->setDestination(entry.destination);
    if (!entry.destination) {
        qWarning() << "nemo-ui: viewer destination table exhausted; panel" << panelId
                   << "runs without a render destination";
    }
    ViewerController* created = entry.controller.get();
    // The registry owns the controller. QML must never delete it through the
    // JavaScript collector when the borrowed wrapper is released.
    QQmlEngine::setObjectOwnership(created, QQmlEngine::CppOwnership);
    const std::uint64_t sequence = nextSequence_++;
    controllers_.emplace(sequence, std::move(entry));
    sequenceByPanel_.emplace(panelId, sequence);
    emit controllersChanged();
    return created;
}

void ViewerControllerRegistry::release(const QString& panelId) {
    const auto found = sequenceByPanel_.find(panelId);
    if (found == sequenceByPanel_.end())
        return;
    const auto entry = controllers_.find(found->second);
    sequenceByPanel_.erase(found);
    if (entry == controllers_.end())
        return;
    std::unique_ptr<ViewerController> released = std::move(entry->second.controller);
    // Clear the destination first: a controller without one is an inert
    // command/metadata facade, so this retiring instance can neither consume a
    // result from the mailbox while its destination is being reused by another
    // panel nor submit new work. Clearing also cancels its in-flight request.
    released->setDestination(std::nullopt);
    if (entry->second.destination)
        runtime_->retireDestination(*entry->second.destination);
    controllers_.erase(entry);
    emit controllersChanged();
    // The panel's ViewerItems are destroyed after this handler runs, and their
    // destructor detaches them from the borrowed controller — which is also
    // what triggered this release. Destroying the controller late keeps it
    // alive for the rest of that destruction sequence while the registry
    // remains its owner, so a shutdown that never resumes the event loop still
    // destroys it instead of leaking it.
    ViewerController* retired = released.release();
    retired->setParent(this);
    retired->deleteLater();
}

int ViewerControllerRegistry::activeCount() const {
    return static_cast<int>(controllers_.size());
}

QObject* ViewerControllerRegistry::primary() const {
    if (controllers_.empty())
        return nullptr;
    return controllers_.begin()->second.controller.get();
}
}  // namespace nemo::ui
