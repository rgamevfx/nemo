#pragma once

#include "PanelContextRouter.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <cstdint>
#include <filesystem>

namespace nemo::ui {

// Owns the application's project file workflow over the shared ProjectSession.
// It adds no document model: every read/write goes through the core ProjectFile
// codec (the single serializer/owner), and the live Document/history stays in
// the composed ProjectSession. File I/O runs on a private worker thread with an
// owned immutable snapshot; completion is delivered to the GUI thread through
// queued invocations, so the event loop never blocks on the filesystem.
//
// Presentation (workspace layout + context records) is composed by this class
// from the existing WorkspaceController / PanelContextRouter owners and travels
// inside the core presentation envelope, which headless callers never read.
class ProjectFileController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasPath READ hasPath NOTIFY pathChanged)
    Q_PROPERTY(bool recovered READ recovered NOTIFY pathChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY pathChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY pathChanged)
    Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY titleChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantList warnings READ warnings NOTIFY warningsChanged)
    Q_PROPERTY(QVariantList references READ references NOTIFY warningsChanged)

public:
    // All three owners must outlive this controller; main.cpp declares them
    // before the QML engine and this object after them.
    ProjectFileController(nemo::ProjectSession& session, nemo::workspace::WorkspaceController& workspace,
                          PanelContextRouter& router, QObject* parent = nullptr);
    ~ProjectFileController() override;

    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool hasPath() const { return !session_.projectPath().empty(); }
    [[nodiscard]] bool recovered() const { return session_.recovered(); }
    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QString fileName() const;
    [[nodiscard]] QString windowTitle() const;
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QVariantList warnings() const { return warnings_; }
    [[nodiscard]] QVariantList references() const { return references_; }

    // Native chooser entry points (xdg-desktop-portal FileChooser, so the dialog
    // is the session's real file chooser on Wayland). Each emits exactly one of
    // *Chosen / fileDialogFailed. QML owns the pending-action prompt so the
    // destructive decision is made against the file the user actually picked.
    Q_INVOKABLE void chooseOpenProject();
    Q_INVOKABLE void chooseRecoveryProject();
    Q_INVOKABLE void chooseSaveAs();

    // Operations. openProject/recoverProject read on the worker then replace the
    // published document; save/saveAs snapshot on this thread, write on the
    // worker, and commit the outcome here.
    Q_INVOKABLE void openProject(const QUrl& url);
    Q_INVOKABLE void recoverProject(const QUrl& url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs(const QUrl& url);
    // Resolves the open/recover presentation prompt. Keep-current (false) leaves
    // the current workspace untouched; the replaced document already revalidated
    // routing through the session observer.
    Q_INVOKABLE void resolvePresentation(bool restore);
    Q_INVOKABLE void clearError();

signals:
    void dirtyChanged();
    void busyChanged();
    void pathChanged();
    void titleChanged();
    void errorChanged();
    // Emitted only when the autosave failure message changes, so the existing
    // error dialog is shown once per distinct failure instead of every tick.
    void autosaveFailed();
    void warningsChanged();
    void openFileChosen(const QUrl& url);
    void recoveryFileChosen(const QUrl& url);
    void saveAsChosen(const QUrl& url);
    // Raised after failFileDialog sets `error`, so QML surfaces the diagnostic
    // through the existing error dialog.
    void fileDialogFailed();
    void fileDialogCancelled();
    // Replacement succeeded. hasPresentation requests the restore/keep prompt.
    void projectOpened(bool hasPresentation, bool recovered);
    void projectOpenFailed();
    void saveFinished(bool ok);

private:
    enum class DialogPurpose { Open, Recovery, SaveAs };

    static void sessionChanged(void* context) noexcept;

    void scheduleRefresh();
    void refreshFromSession();
    void setError(QString message);
    void setBusy(bool busy);
    void beginOperation();
    void endOperation();

    void startDialog(DialogPurpose purpose, const QString& title, const std::filesystem::path& folder,
                     const QString& suggestedName);
    // Sets the persistent error diagnostic before emitting fileDialogFailed so
    // the existing error dialog shows the real reason.
    void failFileDialog(QString message);
#if defined(_WIN32)
    void startWindowsDialog(DialogPurpose purpose, const QString& title, const std::filesystem::path& folder,
                            const QString& suggestedName);
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    void startPortalDialog(DialogPurpose purpose, const QString& title, const std::filesystem::path& folder,
                           const QString& suggestedName);
#endif
    void emitChosen(DialogPurpose purpose, const QUrl& url);

    void beginRead(const std::filesystem::path& path, bool recovery);
    void completeOpen(nemo::ProjectReadResult result, std::uint64_t requestRevision, std::uint64_t generation,
                      bool recovery);
    void saveToPath(std::filesystem::path target);
    void completeSave(nemo::ProjectWriteRequest request, nemo::ProjectWriteResult result);
    void autosave();

    void ensurePresentation();
    // Live project presentation (workspace records + context records), or the
    // preserved envelope when this build cannot interpret the stored one.
    [[nodiscard]] nlohmann::json composePresentation();
    // Panel/layout changes are project state too: recompute the presentation
    // dirty flag without re-serializing the document.
    void refreshPresentationDirty();
    void updateDirty();
    void rebaselinePresentation();
    [[nodiscard]] std::filesystem::path localPath(const QUrl& url) const;
    // Untitled documents get a unique identity per unsaved document so two app
    // instances (or two recovered projects) never share the same three slots.
    [[nodiscard]] std::filesystem::path freshUntitledIdentity() const;
    std::filesystem::path autosaveIdentity();
    std::filesystem::path recoveryFolder();

private slots:
    // XDG portal Request::Response(u,a{sv}); string-based connection so the
    // portal's own signature is matched without introspecting the handle.
    void onPortalResponse(uint response, const QVariantMap& results);

private:
    nemo::ProjectSession& session_;
    nemo::workspace::WorkspaceController& workspace_;
    PanelContextRouter& router_;
    nemo::ProjectSession::Subscription sessionSubscription_;

    QThread ioThread_;
    QObject ioWorker_;
    QTimer autosaveTimer_;

    QString error_;
    QString lastAutosaveError_;
    QVariantList warnings_;
    QVariantList references_;
    QString cachedPath_;
    bool dirty_{false};
    // Document dirty comes from the session; presentation dirty is tracked here
    // because panel/layout state changes do not publish a session revision.
    bool documentDirty_{false};
    bool presentationDirty_{false};
    nlohmann::json presentationBaseline_;
    bool presentationBaselineSet_{false};
    bool busy_{false};
    bool recoveredCached_{false};
    int inFlight_{0};
    bool writeInFlight_{false};
    bool autosaveInFlight_{false};
    bool refreshScheduled_{false};
    std::uint64_t loadGeneration_{0};
    bool loadInFlight_{false};

    nlohmann::json pendingPresentation_;
    bool pendingHasPresentation_{false};
    // A presentation envelope this build does not understand (newer version)
    // is retained verbatim on every write instead of being rewritten from the
    // current workspace, so an older build never destroys newer records.
    nlohmann::json preservedEnvelope_;

    QString appDataDirectory_;
    std::filesystem::path untitledIdentity_;
    bool dialogInFlight_{false};
    DialogPurpose chooserPurpose_{DialogPurpose::Open};
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    QString portalHandle_;
    int portalTokenCounter_{0};
#endif
};

}  // namespace nemo::ui
