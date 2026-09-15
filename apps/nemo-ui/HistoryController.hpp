#pragma once

#include "nemo/core/session/ProjectSession.hpp"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <vector>

namespace nemo::ui {

// Owner-thread presentation over the application's explicitly supplied session.
// Windows borrow this adapter; gesture owners retain their own preview machines.
class HistoryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString undoText READ undoText NOTIFY changed)
    Q_PROPERTY(QString redoText READ redoText NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit HistoryController(ProjectSession& session, QObject* parent = nullptr);
    ~HistoryController() override;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoText() const;
    [[nodiscard]] QString redoText() const;
    [[nodiscard]] QString error() const { return error_; }
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE void registerWindow(QQuickWindow* window);
    Q_INVOKABLE void unregisterWindow(QQuickWindow* window);
    Q_INVOKABLE void beginMenu(QQuickWindow* window);
    Q_INVOKABLE void endMenu();
    Q_INVOKABLE void setGesture(QObject* owner, bool active);
signals:
    void changed();
private slots:
    void refresh();

private:
    enum class Kind { None, Text, Gesture, Document };
    struct Target {
        Kind kind{Kind::None};
        QPointer<QObject> owner;
    };
    struct Window {
        QPointer<QQuickWindow> window;
        QMetaObject::Connection focus;
        QMetaObject::Connection destroyed;
    };
    [[nodiscard]] bool registered(QQuickWindow* window) const;
    [[nodiscard]] Target resolve(QQuickWindow* window) const;
    [[nodiscard]] Target target() const;
    [[nodiscard]] bool available(const Target& target, bool redo) const;
    bool invoke(bool redo);
    bool eventFilter(QObject* watched, QEvent* event) override;
    static void sessionChanged(void* context) noexcept;
    ProjectSession& session_;
    ProjectSession::Subscription subscription_;
    std::vector<Window> windows_;
    std::vector<QPointer<QObject>> gestures_;
    std::vector<QMetaObject::Connection> textConnections_;
    QPointer<QObject> observedText_;
    bool menuActive_{false};
    Target menuTarget_;
    QPointer<QQuickWindow> menuWindow_;
    std::uint64_t menuGeneration_{};
    // A popup may dismiss on the trigger's press, before its clicked handler.
    // Keep that pre-focus target only for the current pointer invocation.
    Target pressTarget_;
    QPointer<QQuickWindow> pressWindow_;
    std::uint64_t pressEpoch_{};
    QString error_;
};
}  // namespace nemo::ui
