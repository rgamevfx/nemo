#include "HistoryController.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQuickItem>
#include <QTimer>
#include <algorithm>
#include <exception>

namespace nemo::ui {
namespace {
QObject* textBuffer(QQuickWindow* window) {
    auto* item = window ? window->activeFocusItem() : nullptr;
    // TextInput/TextEdit (including Controls' fields and editable SpinBoxes)
    // expose this public Qt contract. No private Qt types or text-owner registry.
    if (item && item->isEnabled() && !item->property("readOnly").toBool() &&
        item->metaObject()->indexOfProperty("canUndo") >= 0 && item->metaObject()->indexOfProperty("canRedo") >= 0 &&
        item->metaObject()->indexOfMethod("undo()") >= 0 && item->metaObject()->indexOfMethod("redo()") >= 0)
        return item;
    return nullptr;
}
}  // namespace

HistoryController::HistoryController(ProjectSession& session, QObject* parent)
    : QObject(parent), session_(session), subscription_(session.subscribe(this, sessionChanged)) {
    qApp->installEventFilter(this);
    connect(qApp, &QGuiApplication::focusWindowChanged, this, &HistoryController::refresh);
}
HistoryController::~HistoryController() {
    qApp->removeEventFilter(this);
}
void HistoryController::sessionChanged(void* context) noexcept {
    static_cast<HistoryController*>(context)->refresh();
}
bool HistoryController::registered(QQuickWindow* window) const {
    return window && std::ranges::any_of(windows_, [window](const auto& entry) { return entry.window == window; });
}
void HistoryController::registerWindow(QQuickWindow* window) {
    if (!window || registered(window))
        return;
    windows_.push_back({window,
                        connect(window, &QQuickWindow::activeFocusItemChanged, this, &HistoryController::refresh),
                        connect(window, &QObject::destroyed, this, [this] {
                            std::erase_if(windows_, [](const auto& entry) { return !entry.window; });
                            refresh();
                        })});
    refresh();
}
void HistoryController::unregisterWindow(QQuickWindow* window) {
    for (const auto& entry : windows_)
        if (entry.window == window) {
            disconnect(entry.focus);
            disconnect(entry.destroyed);
        }
    std::erase_if(windows_, [window](const auto& entry) { return entry.window == window; });
    if (menuWindow_ == window) {
        menuActive_ = false;
        menuTarget_ = {};
        menuWindow_.clear();
    }
    refresh();
}
HistoryController::Target HistoryController::resolve(QQuickWindow* window) const {
    if (!registered(window))
        return {};
    if (auto* text = textBuffer(window))
        return {Kind::Text, text};
    for (auto it = gestures_.rbegin(); it != gestures_.rend(); ++it)
        if (auto* item = qobject_cast<QQuickItem*>(it->data()); item && item->window() == window)
            return {Kind::Gesture, item};
    return {Kind::Document, {}};
}
HistoryController::Target HistoryController::target() const {
    if (menuActive_)
        return menuGeneration_ == session_.projectGeneration() && registered(menuWindow_) ? menuTarget_ : Target{};
    return resolve(qobject_cast<QQuickWindow*>(QGuiApplication::focusWindow()));
}
bool HistoryController::available(const Target& target, bool redo) const {
    switch (target.kind) {
    case Kind::Text:
        return target.owner && target.owner->property(redo ? "canRedo" : "canUndo").toBool();
    case Kind::Gesture:
        return !redo && target.owner && std::ranges::find(gestures_, target.owner) != gestures_.end();
    case Kind::Document:
        return redo ? session_.canRedo() : session_.canUndo();
    case Kind::None:
        return false;
    }
    return false;
}
bool HistoryController::canUndo() const {
    return available(target(), false);
}
bool HistoryController::canRedo() const {
    return available(target(), true);
}
QString HistoryController::undoText() const {
    const auto current = target();
    if (current.kind == Kind::Gesture)
        return tr("Cancel edit");
    if (current.kind == Kind::Document && session_.canUndo()) {
        const auto label = session_.undoLabel();
        if (!label.empty())
            return tr("Undo %1").arg(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())));
    }
    return tr("Undo");
}
QString HistoryController::redoText() const {
    if (target().kind == Kind::Document && session_.canRedo()) {
        const auto label = session_.redoLabel();
        if (!label.empty())
            return tr("Redo %1").arg(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())));
    }
    return tr("Redo");
}
void HistoryController::beginMenu(QQuickWindow* window) {
    menuTarget_ = pressWindow_ == window ? pressTarget_ : resolve(window);
    menuWindow_ = window;
    menuGeneration_ = session_.projectGeneration();
    menuActive_ = true;
    refresh();
}
void HistoryController::endMenu() {
    // Menu dismissal can precede triggered(). Keep the captured target through
    // that event, including an exhausted or destroyed text buffer (no fallback).
    QTimer::singleShot(0, this, [this] {
        menuActive_ = false;
        menuTarget_ = {};
        menuWindow_.clear();
        refresh();
    });
}
void HistoryController::setGesture(QObject* owner, bool active) {
    if (!owner)
        return;
    std::erase_if(gestures_, [owner](const auto& item) { return !item || item == owner; });
    if (active)
        gestures_.push_back(owner);
    refresh();
}
void HistoryController::refresh() {
    auto current = target();
    auto* text = current.kind == Kind::Text ? current.owner.data() : nullptr;
    if (observedText_ != text) {
        for (const auto& connection : textConnections_)
            disconnect(connection);
        textConnections_.clear();
        observedText_ = text;
        if (text) {
            const auto slot = metaObject()->method(metaObject()->indexOfSlot("refresh()"));
            for (const auto* name : {"canUndo", "canRedo"}) {
                const auto property = text->metaObject()->property(text->metaObject()->indexOfProperty(name));
                if (property.hasNotifySignal())
                    textConnections_.push_back(connect(text, property.notifySignal(), this, slot));
            }
            textConnections_.push_back(connect(text, &QObject::destroyed, this, &HistoryController::refresh));
        }
    }
    emit changed();
}
bool HistoryController::invoke(bool redo) {
    const auto current = target();
    if (!available(current, redo))
        return false;
    error_.clear();
    bool ok = false;
    try {
        if (current.kind == Kind::Text) {
            ok = QMetaObject::invokeMethod(current.owner, redo ? "redo" : "undo", Qt::DirectConnection);
        } else if (current.kind == Kind::Gesture) {
            ok = QMetaObject::invokeMethod(current.owner, "cancelHistoryGesture", Qt::DirectConnection);
        } else if (current.kind == Kind::Document) {
            const auto result = redo ? session_.redo({.expectedRevision = session_.revision()})
                                     : session_.undo({.expectedRevision = session_.revision()});
            ok = result.committed;
            if (result.error)
                error_ = QString::fromStdString(result.error->message);
        }
    } catch (const std::exception& error) {
        error_ = QString::fromUtf8(error.what());
    }
    refresh();
    return ok;
}
bool HistoryController::undo() {
    return invoke(false);
}
bool HistoryController::redo() {
    return invoke(true);
}
bool HistoryController::eventFilter(QObject* watched, QEvent* event) {
    auto* window = qobject_cast<QQuickWindow*>(watched);
    if (!registered(window) || !window->isActive())
        return false;
    if (event->type() == QEvent::MouseButtonPress) {
        pressTarget_ = resolve(window);
        pressWindow_ = window;
        ++pressEpoch_;
    } else if (event->type() == QEvent::MouseButtonRelease) {
        const auto epoch = pressEpoch_;
        QTimer::singleShot(0, this, [this, epoch] {
            if (pressEpoch_ == epoch) {
                pressWindow_.clear();
                pressTarget_ = {};
            }
        });
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
        return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const bool redo = key->matches(QKeySequence::Redo);
    if (!redo && !key->matches(QKeySequence::Undo))
        return false;
    // Claim even unavailable operations: text exhaustion and gesture Redo
    // must not reach a competing shortcut or bubble to document history.
    key->accept();
    if (event->type() == QEvent::KeyPress)
        invoke(redo);
    return true;
}
}  // namespace nemo::ui
