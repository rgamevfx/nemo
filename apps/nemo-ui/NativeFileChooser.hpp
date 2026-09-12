#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

class QThread;

namespace nemo::ui {

// Asynchronous native file chooser (issue #35 project workflow, issue #43 media
// import). One platform implementation per build: the xdg-desktop-portal
// FileChooser on Linux (the session's real chooser under Wayland), the Win32
// common item dialog on Windows, and an explicit failure everywhere else.
//
// Platform ownership stays inside: no DBus/Win32 type crosses this interface,
// the portal request/response wiring and the Win32 modal-loop thread are
// private, and a caller only describes a request and receives one outcome.
//
// Every request belongs to the client that starts it. The requester QObject
// identity is required, the outcome is delivered only through the handler that
// requester supplied, and there is no broadcast: a client can never observe an
// outcome belonging to another client's request. One request is outstanding at
// a time; an accepted request produces exactly one Outcome for its requester,
// while a refused request (one already outstanding) produces nothing and leaves
// the outstanding request — requester included — untouched. A requester
// destroyed before its outcome receives no action.
class NativeFileChooser final : public QObject {
    Q_OBJECT
public:
    // `label` names the chooser filter; `patterns` are glob patterns. The
    // platform layer never validates them: a suffix is a chooser convenience,
    // never a format check. An empty filter list leaves the platform default.
    struct Filter {
        QString label;
        QStringList patterns;
    };

    // The single outcome of an accepted request, in the initiator's own terms.
    struct Outcome {
        enum class Status { Chosen, Cancelled, Failed };
        Status status{Status::Failed};
        // Local file URLs, at least one, in the chooser's own order. Chosen only.
        QList<QUrl> urls;
        // The persistent diagnostic for the caller's error surface. Failed only.
        QString message;
    };
    // Delivered exactly once, and only to the requester that started the
    // request. Swept away with the requester, so it never outlives its client.
    using OutcomeHandler = std::function<void(Outcome)>;

    explicit NativeFileChooser(QObject* parent = nullptr);
    ~NativeFileChooser() override;

    // One existing-file dialog opening at `startFolder`. `requester` is the
    // owning client and receives this request's outcome through `onOutcome`;
    // both are required. `multiple` allows more than one selection. Returns
    // false, storing nothing, when a request is already outstanding.
    [[nodiscard]] bool openFiles(QObject* requester, OutcomeHandler onOutcome, const QString& title,
                                 bool multiple = false, const std::vector<Filter>& filters = {},
                                 const std::filesystem::path& startFolder = {}, const QString& suggestedName = {});
    // One save dialog. `defaultSuffix` ("nemo") is the extension the platform
    // appends when the user types none. Same requester/outcome contract.
    [[nodiscard]] bool saveFile(QObject* requester, OutcomeHandler onOutcome, const QString& title,
                                const std::vector<Filter>& filters = {}, const std::filesystem::path& startFolder = {},
                                const QString& suggestedName = {}, const QString& defaultSuffix = {});
    // True while a request is outstanding and its outcome is pending.
    [[nodiscard]] bool inFlight() const { return inFlight_; }

private slots:
    // XDG portal Request::Response(u,a{sv}); string-based connection so the
    // portal's own signature is matched without introspecting the handle.
    void onPortalResponse(uint response, const QVariantMap& results);

private:
    // The platform request. `startFolder`/`filters` are shared; `save`,
    // `multiple` and `defaultSuffix` select the platform behavior.
    struct Request {
        QString title;
        QString acceptLabel;
        QString suggestedName;
        QString defaultSuffix;
        std::filesystem::path startFolder;
        std::vector<Filter> filters;
        bool save{false};
        bool multiple{false};
    };

    [[nodiscard]] bool begin(QObject* requester, OutcomeHandler onOutcome, Request request);
    // The one outcome for the request `serial` belongs to. Delivered only to the
    // requester that started that request and dropped when that requester is
    // gone or the request has already ended, so a platform reply that outlives
    // its request can never end a later one; an empty selection is a
    // cancellation. Never touches the request state after handing the outcome
    // over.
    void finish(std::uint64_t serial, Outcome outcome);
#if defined(_WIN32)
    void startWindowsDialog(const Request& request, std::uint64_t serial);
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    void startPortalDialog(const Request& request, std::uint64_t serial);
#endif

    bool inFlight_{false};
    // Identity of the outstanding request, rising with every accepted request.
    std::uint64_t requestSerial_{0};
    Request request_;
    QPointer<QObject> requester_;
    OutcomeHandler onOutcome_;
    QMetaObject::Connection requesterDestroyed_;
#if defined(_WIN32)
    // Private thread hosting the Win32 modal dialog: its thread owns the modal
    // loop and message pump, so the GUI event loop stays free.
    QThread* blockingThread_{nullptr};
    QObject* blockingWorker_{nullptr};
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    QString portalHandle_;
    // The request the portal session belongs to; cleared once its response has
    // been consumed, so a late response cannot be read as a later request's.
    std::uint64_t portalSerial_{0};
    int portalTokenCounter_{0};
#endif
};

}  // namespace nemo::ui
