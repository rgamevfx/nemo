#include "ProjectFileController.hpp"

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace nemo::ui {
namespace {

QVariantList toVariantList(const std::vector<std::string>& values) {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) {
        list.push_back(QString::fromStdString(value));
    }
    return list;
}

// Filesystem paths cross QString as the platform's native encoding. On Windows
// std::filesystem::path::string() is the ANSI code page, so a narrow round-trip
// would mangle non-ASCII paths; use the wide form there.
QString pathToQString(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::filesystem::path pathFromQString(const QString& value) {
#if defined(_WIN32)
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

QString referenceStateText(nemo::ReferenceState state) {
    switch (state) {
    case nemo::ReferenceState::Present:
        return QStringLiteral("present");
    case nemo::ReferenceState::Missing:
        return QStringLiteral("missing");
    case nemo::ReferenceState::Unresolved:
        return QStringLiteral("unresolved");
    }
    return QStringLiteral("unresolved");
}

QVariantList toReferenceList(const std::vector<nemo::ExternalReference>& references) {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(references.size()));
    for (const auto& reference : references) {
        list.push_back(QVariantMap{{QStringLiteral("identity"), QString::fromStdString(reference.identity)},
                                   {QStringLiteral("storedPath"), QString::fromStdString(reference.storedPath)},
                                   {QStringLiteral("resolvedPath"), QString::fromStdString(reference.resolvedPath)},
                                   {QStringLiteral("state"), referenceStateText(reference.state)},
                                   {QStringLiteral("exists"), reference.state == nemo::ReferenceState::Present}});
    }
    return list;
}

QString errorText(const nemo::ProjectFileError& error, const std::filesystem::path& fallbackPath) {
    QString message;
    if (!error.message.empty()) {
        message = QString::fromStdString(error.message);
    } else {
        const QString path = pathToQString(fallbackPath);
        message = path.isEmpty() ? QStringLiteral("Project file operation failed.")
                                 : QStringLiteral("Project file operation failed for %1.").arg(path);
    }
    if (error.code == nemo::ProjectFileError::Code::UnsupportedTarget) {
        message += QStringLiteral(" Save to a different file name.");
    }
    return message;
}

bool isKnownPresentationEnvelope(const nlohmann::json& envelope) {
    if (!envelope.is_object()) {
        return false;
    }
    const auto version = envelope.find("version");
    return version != envelope.end() && version->is_number_integer() &&
           version->get<int>() == nemo::kPresentationEnvelopeVersion;
}

}  // namespace

ProjectFileController::ProjectFileController(nemo::ProjectSession& session,
                                             nemo::workspace::WorkspaceController& workspace,
                                             PanelContextRouter& router, NativeFileChooser& chooser, QObject* parent)
    : QObject(parent), session_(session), workspace_(workspace), router_(router), chooser_(chooser) {
    appDataDirectory_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    ioWorker_.moveToThread(&ioThread_);
    ioThread_.start();
    sessionSubscription_ = session_.subscribe(this, &ProjectFileController::sessionChanged);
    // Each choose* entry point hands the chooser a handler that carries that
    // request's purpose, so an outcome can only ever be read as the action this
    // workflow actually asked for.
    autosaveTimer_.setInterval(static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nemo::kDefaultAutosaveInterval).count()));
    autosaveTimer_.setSingleShot(false);
    connect(&autosaveTimer_, &QTimer::timeout, this, &ProjectFileController::autosave);
    // Panel/layout/context state is project presentation: a change there is a
    // project change even though the document revision does not move.
    connect(&workspace_, &nemo::workspace::WorkspaceController::rootChanged, this,
            &ProjectFileController::refreshPresentationDirty);
    connect(&workspace_, &nemo::workspace::WorkspaceController::presentationChanged, this,
            &ProjectFileController::refreshPresentationDirty);
    connect(&workspace_, &nemo::workspace::WorkspaceController::workspacesChanged, this,
            &ProjectFileController::refreshPresentationDirty);
    connect(&workspace_, &nemo::workspace::WorkspaceController::activeWorkspaceIdChanged, this,
            &ProjectFileController::refreshPresentationDirty);
    connect(&workspace_, &nemo::workspace::WorkspaceController::appearanceChanged, this,
            &ProjectFileController::refreshPresentationDirty);
    connect(&router_, &PanelContextRouter::groupContextChanged, this, &ProjectFileController::refreshPresentationDirty);
    refreshFromSession();
    // The state the app starts with is the baseline: a restored workspace.json
    // layout must not make a brand-new untitled project look unsaved.
    rebaselinePresentation();
    updateDirty();
}

ProjectFileController::~ProjectFileController() {
    autosaveTimer_.stop();
    ioThread_.quit();
    ioThread_.wait();
}

QString ProjectFileController::filePath() const {
    return pathToQString(session_.projectPath());
}

QString ProjectFileController::fileName() const {
    const auto& path = session_.projectPath();
    if (path.empty()) {
        return QStringLiteral("Untitled");
    }
    return pathToQString(path.filename());
}

QString ProjectFileController::windowTitle() const {
    return fileName() + (dirty_ ? QStringLiteral("*") : QString());
}

void ProjectFileController::sessionChanged(void* context) noexcept {
    static_cast<ProjectFileController*>(context)->scheduleRefresh();
}

void ProjectFileController::scheduleRefresh() {
    if (refreshScheduled_) {
        return;
    }
    refreshScheduled_ = true;
    // Observer callbacks run synchronously on the owner thread and must not
    // mutate or block; defer the (serializing) dirty query to the event loop.
    QMetaObject::invokeMethod(
        this,
        [this] {
            refreshScheduled_ = false;
            refreshFromSession();
        },
        Qt::QueuedConnection);
}

void ProjectFileController::refreshFromSession() {
    try {
        documentDirty_ = session_.isDirty();
    } catch (const std::exception&) {
        documentDirty_ = true;
    }
    updateDirty();
    const QString path = filePath();
    const bool recovered = session_.recovered();
    if (path != cachedPath_ || recovered != recoveredCached_) {
        cachedPath_ = path;
        recoveredCached_ = recovered;
        emit pathChanged();
        emit titleChanged();
    }
}

nlohmann::json ProjectFileController::composePresentation() {
    if (preservedEnvelope_.is_object() && !preservedEnvelope_.empty()) {
        return preservedEnvelope_;
    }
    nlohmann::json data = nlohmann::json::object();
    data["workspace"] = workspace_.projectPresentation();
    data["context"] = router_.contextPresentation();
    return nemo::makePresentationEnvelope(std::move(data));
}

void ProjectFileController::refreshPresentationDirty() {
    // Cheap relative to the session query: only the presentation payload is
    // composed, never the document.
    presentationDirty_ = presentationBaselineSet_ && !(composePresentation() == presentationBaseline_);
    updateDirty();
}

void ProjectFileController::rebaselinePresentation() {
    presentationBaseline_ = composePresentation();
    presentationBaselineSet_ = true;
    presentationDirty_ = false;
}

void ProjectFileController::updateDirty() {
    const bool dirty = documentDirty_ || presentationDirty_;
    if (dirty != dirty_) {
        dirty_ = dirty;
        emit dirtyChanged();
        emit titleChanged();
    }
    if (dirty_) {
        if (!autosaveTimer_.isActive()) {
            autosaveTimer_.start();
        }
    } else {
        autosaveTimer_.stop();
    }
}

void ProjectFileController::setError(QString message) {
    if (error_ == message) {
        return;
    }
    error_ = std::move(message);
    emit errorChanged();
}

void ProjectFileController::clearError() {
    setError(QString{});
}

void ProjectFileController::setBusy(bool busy) {
    if (busy_ == busy) {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void ProjectFileController::beginOperation() {
    ++inFlight_;
    setBusy(true);
}

void ProjectFileController::endOperation() {
    if (inFlight_ > 0) {
        --inFlight_;
    }
    setBusy(inFlight_ > 0);
}

std::filesystem::path ProjectFileController::localPath(const QUrl& url) const {
    if (!url.isLocalFile()) {
        return {};
    }
    const QString local = url.toLocalFile();
    if (local.isEmpty()) {
        return {};
    }
    return pathFromQString(local);
}

std::filesystem::path ProjectFileController::freshUntitledIdentity() const {
    // A random token per unsaved document keeps two app instances (or two
    // recovered projects) from sharing one bounded slot set. The identity is
    // only ever an autosave location; it is never presented as a project path.
    const QString base = appDataDirectory_.isEmpty() ? QDir::tempPath() + QStringLiteral("/nemo")
                                                     : appDataDirectory_ + QStringLiteral("/autosave");
    const QString name = QStringLiteral("untitled-%1.nemo").arg(QUuid::createUuid().toString(QUuid::Id128));
    return pathFromQString(base + QLatin1Char('/') + name);
}

std::filesystem::path ProjectFileController::autosaveIdentity() {
    if (!session_.projectPath().empty()) {
        return session_.projectPath();
    }
    if (untitledIdentity_.empty()) {
        untitledIdentity_ = freshUntitledIdentity();
    }
    return untitledIdentity_;
}

std::filesystem::path ProjectFileController::recoveryFolder() {
    if (!session_.projectPath().empty()) {
        return session_.projectPath().parent_path();
    }
    return autosaveIdentity().parent_path();
}

void ProjectFileController::ensurePresentation() {
    session_.setPresentation(composePresentation());
}

void ProjectFileController::chooseOpenProject() {
    startDialog(DialogPurpose::Open, QStringLiteral("Open project"), pathFromQString(QDir::homePath()), QString());
}

void ProjectFileController::chooseRecoveryProject() {
    QString suggested;
    if (!session_.projectPath().empty()) {
        nemo::AutosaveStore store(session_.projectPath());
        if (const auto latest = store.latest(); latest.has_value()) {
            suggested = pathToQString(latest->filename());
        }
        if (suggested.isEmpty()) {
            suggested = pathToQString(session_.projectPath().filename()) + QStringLiteral(".bak");
        }
    }
    std::filesystem::path folder = recoveryFolder();
    if (folder.empty()) {
        folder = pathFromQString(QDir::homePath());
    }
    startDialog(DialogPurpose::Recovery, QStringLiteral("Recover unsaved project"), folder, suggested);
}

void ProjectFileController::chooseSaveAs() {
    const QString suggested = session_.projectPath().empty() ? QStringLiteral("Untitled.nemo") : fileName();
    const std::filesystem::path folder =
        session_.projectPath().empty() ? pathFromQString(QDir::homePath()) : session_.projectPath().parent_path();
    startDialog(DialogPurpose::SaveAs, QStringLiteral("Save project as"), folder, suggested);
}

void ProjectFileController::failFileDialog(QString message) {
    setError(std::move(message));
    emit fileDialogFailed();
}

void ProjectFileController::startDialog(DialogPurpose purpose, const QString& title,
                                        const std::filesystem::path& folder, const QString& suggestedName) {
    // The purpose lives in this request's outcome handler, never in a member:
    // an outcome the chooser delivers here belongs to the dialog this call
    // started, and no stale purpose can exist. Every project dialog is
    // single-select, so a chosen outcome carries exactly one URL. The chooser
    // refuses a second request while one is outstanding; that refusal reaches
    // no one (this handler is not registered) and leaves the outstanding
    // request's owner untouched.
    NativeFileChooser::OutcomeHandler onOutcome = [this, purpose](NativeFileChooser::Outcome outcome) {
        switch (outcome.status) {
        case NativeFileChooser::Outcome::Status::Chosen:
            emitChosen(purpose, outcome.urls.constFirst());
            return;
        case NativeFileChooser::Outcome::Status::Cancelled:
            emit fileDialogCancelled();
            return;
        case NativeFileChooser::Outcome::Status::Failed:
            failFileDialog(std::move(outcome.message));
            return;
        }
    };
    switch (purpose) {
    case DialogPurpose::Open:
        static_cast<void>(chooser_.openFiles(this, std::move(onOutcome), title, false,
                                             {{QStringLiteral("Nemo project"), {QStringLiteral("*.nemo")}},
                                              {QStringLiteral("All files"), {QStringLiteral("*")}}},
                                             folder));
        return;
    case DialogPurpose::Recovery:
        // Keep autosave slots and previous-good backups reachable.
        static_cast<void>(
            chooser_.openFiles(this, std::move(onOutcome), title, false,
                               {{QStringLiteral("Nemo project or recovery copy"),
                                 {QStringLiteral("*.nemo"), QStringLiteral("*.autosave*"), QStringLiteral("*.bak")}},
                                {QStringLiteral("All files"), {QStringLiteral("*")}}},
                               folder, suggestedName));
        return;
    case DialogPurpose::SaveAs:
        static_cast<void>(chooser_.saveFile(this, std::move(onOutcome), title,
                                            {{QStringLiteral("Nemo project"), {QStringLiteral("*.nemo")}}}, folder,
                                            suggestedName, QStringLiteral("nemo")));
        return;
    }
}

void ProjectFileController::emitChosen(DialogPurpose purpose, const QUrl& url) {
    switch (purpose) {
    case DialogPurpose::Open:
        emit openFileChosen(url);
        break;
    case DialogPurpose::Recovery:
        emit recoveryFileChosen(url);
        break;
    case DialogPurpose::SaveAs:
        emit saveAsChosen(url);
        break;
    }
}

void ProjectFileController::openProject(const QUrl& url) {
    const auto path = localPath(url);
    if (path.empty()) {
        setError(QStringLiteral("No project file was selected."));
        emit projectOpenFailed();
        return;
    }
    beginRead(path, false);
}

void ProjectFileController::recoverProject(const QUrl& url) {
    const auto path = localPath(url);
    if (path.empty()) {
        setError(QStringLiteral("No recovery file was selected."));
        emit projectOpenFailed();
        return;
    }
    beginRead(path, true);
}

void ProjectFileController::beginRead(const std::filesystem::path& path, bool recovery) {
    if (loadInFlight_) {
        setError(QStringLiteral("A project is already being loaded."));
        emit projectOpenFailed();
        return;
    }
    clearError();
    loadInFlight_ = true;
    beginOperation();
    const std::uint64_t requestRevision = session_.revision();
    const std::uint64_t generation = ++loadGeneration_;
    QMetaObject::invokeMethod(
        &ioWorker_,
        [this, path, recovery, requestRevision, generation] {
            // Backend owns recovery semantics: one readRecovery path infers the
            // protected original for autosave slots and .bak alike, so its
            // recoveryOriginal/sourcePath are never overridden here.
            const nemo::ProjectReadResult result =
                recovery ? nemo::ProjectFile::readRecovery(path) : nemo::ProjectFile::read(path);
            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result), requestRevision, generation, recovery]() mutable {
                    completeOpen(std::move(result), requestRevision, generation, recovery);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ProjectFileController::completeOpen(nemo::ProjectReadResult result, std::uint64_t requestRevision,
                                         std::uint64_t generation, bool /*recovery*/) {
    loadInFlight_ = false;
    endOperation();
    if (generation != loadGeneration_) {
        return;  // a newer read superseded this one
    }
    if (!result.ok) {
        setError(errorText(result.error, result.sourcePath));
        emit projectOpenFailed();
        return;
    }
    if (session_.revision() != requestRevision) {
        // The user kept editing while the file was read. Refuse the replacement
        // rather than discard edits that are newer than the chosen file.
        setError(QStringLiteral("The project was not opened because the document changed while it was being read."));
        emit projectOpenFailed();
        return;
    }

    const nlohmann::json envelope = result.presentation;
    const bool knownEnvelope = isKnownPresentationEnvelope(envelope);
    const bool hasPresentation = knownEnvelope && !nemo::presentationData(envelope).is_null();
    // Presentation records this build cannot interpret are preserved verbatim
    // on every write; only a version we own is recomposed from live state.
    preservedEnvelope_ = (!knownEnvelope && envelope.is_object() && !envelope.empty()) ? envelope : nlohmann::json{};
    if (preservedEnvelope_.is_object()) {
        const bool alreadyWarned =
            std::any_of(result.warnings.begin(), result.warnings.end(),
                        [](const std::string& warning) { return warning.find("presentation") != std::string::npos; });
        if (!alreadyWarned) {
            result.warnings.push_back(
                "the project's presentation records come from a newer version; they are preserved unchanged");
        }
    }
    warnings_ = toVariantList(result.warnings);
    references_ = toReferenceList(result.references);
    pendingPresentation_ = knownEnvelope ? envelope : nlohmann::json{};
    pendingHasPresentation_ = hasPresentation;
    const bool recovered = result.recovered;

    const nemo::ProjectReplaceResult replaced = session_.open(std::move(result));
    if (!replaced.replaced) {
        setError(replaced.error ? QString::fromStdString(replaced.error->message)
                                : QStringLiteral("Cannot open the project document."));
        emit projectOpenFailed();
        return;
    }
    clearError();
    lastAutosaveError_.clear();
    if (session_.projectPath().empty()) {
        // A freshly opened untitled/recovered document owns a fresh bounded slot
        // set; earlier untitled autosaves for another document stay untouched.
        untitledIdentity_ = freshUntitledIdentity();
    }
    // Integer identities are only meaningful inside one project. Drop the old
    // document's target/playhead selections before anything can resolve them
    // against the replaced document; Restore re-applies the project's own
    // validated context afterwards, Keep leaves them cleared.
    router_.resetDocumentContexts();
    rebaselinePresentation();
    updateDirty();
    emit warningsChanged();
    emit pathChanged();
    refreshFromSession();
    emit projectOpened(pendingHasPresentation_, recovered);
}

void ProjectFileController::resolvePresentation(bool restore) {
    if (!pendingHasPresentation_) {
        return;
    }
    const nlohmann::json envelope = std::move(pendingPresentation_);
    pendingPresentation_ = nlohmann::json{};
    pendingHasPresentation_ = false;
    if (restore) {
        const nlohmann::json data = nemo::presentationData(envelope);
        if (data.is_object()) {
            if (const auto workspace = data.find("workspace"); workspace != data.end() && workspace->is_object()) {
                if (!workspace_.applyProjectPresentation(*workspace)) {
                    setError(workspace_.error());
                }
            }
            if (const auto context = data.find("context"); context != data.end() && context->is_object()) {
                // A malformed context record keeps the current routing rather
                // than applying a partial selection.
                (void)router_.applyContextPresentation(*context);
            }
        }
    }
    // Either choice defines the presentation this project now carries; a later
    // panel/layout change is what makes the project dirty, not this decision.
    rebaselinePresentation();
    updateDirty();
}

void ProjectFileController::save() {
    if (session_.projectPath().empty()) {
        setError(QStringLiteral("This project has no file yet; use Save As."));
        emit saveFinished(false);
        return;
    }
    // Save in place: an existing project keeps the exact name it was opened or
    // saved under, whatever its suffix.
    saveToPath(session_.projectPath());
}

void ProjectFileController::saveAs(const QUrl& url) {
    const auto path = localPath(url);
    if (path.empty()) {
        setError(QStringLiteral("No project file destination was selected."));
        emit saveFinished(false);
        return;
    }
    // The native chooser's destination is authoritative: never rewrite it to a
    // different path the user did not confirm replacing.
    saveToPath(path);
}

void ProjectFileController::saveToPath(std::filesystem::path target) {
    if (writeInFlight_) {
        setError(QStringLiteral("A save is already in progress."));
        emit saveFinished(false);
        return;
    }
    if (target.empty()) {
        setError(QStringLiteral("No project file destination is available."));
        emit saveFinished(false);
        return;
    }
    // The chosen path is written verbatim; only the native chooser may append
    // its own default extension (Windows SetDefaultExtension). The suffix is
    // never used as format validation and never rewritten here.
    clearError();
    ensurePresentation();
    nemo::ProjectWriteRequest request = session_.prepareSave(target, nemo::PathPolicy::RebaseRelative, true);
    writeInFlight_ = true;
    beginOperation();
    QMetaObject::invokeMethod(
        &ioWorker_,
        [this, request]() mutable {
            nemo::ProjectWriteResult result = nemo::ProjectFile::writeAtomic(request);
            QMetaObject::invokeMethod(
                this,
                [this, request = std::move(request), result = std::move(result)]() mutable {
                    completeSave(std::move(request), std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ProjectFileController::completeSave(nemo::ProjectWriteRequest request, nemo::ProjectWriteResult result) {
    writeInFlight_ = false;
    endOperation();
    if (!result.ok) {
        session_.setLastFileError(result.error.message);
        setError(errorText(result.error, result.target));
        emit saveFinished(false);
        return;
    }
    const nemo::EditResult committed = session_.commitSave(request, result);
    if (!committed.committed) {
        // A completion from an older project generation (the document was
        // replaced while the write ran) is reported by the session; surface its
        // diagnostic and let the refreshed session state stand.
        QString message = committed.error ? QString::fromStdString(committed.error->message) : QString{};
        if (message.isEmpty()) {
            message = QString::fromStdString(session_.lastFileError());
        }
        setError(message.isEmpty() ? QStringLiteral("The project was written but the session did not adopt it.")
                                   : message);
        refreshFromSession();
        emit saveFinished(false);
        return;
    }
    session_.setLastFileError({});
    clearError();
    // The written envelope is the baseline. Recompute against the live
    // presentation: layout/panel changes made while the write was in flight
    // must keep the project dirty rather than being silently marked saved.
    presentationBaseline_ = request.presentation;
    presentationBaselineSet_ = true;
    presentationDirty_ = !(composePresentation() == presentationBaseline_);
    lastAutosaveError_.clear();
    emit pathChanged();
    refreshFromSession();
    emit saveFinished(true);
}

void ProjectFileController::autosave() {
    if (!dirty_ || writeInFlight_ || autosaveInFlight_ || loadInFlight_) {
        return;
    }
    const std::filesystem::path identity = autosaveIdentity();
    ensurePresentation();
    nemo::ProjectWriteRequest request = session_.prepareSave(identity, nemo::PathPolicy::KeepStored, false);
    nemo::AutosaveStore store(identity, nemo::kDefaultAutosaveSlots);
    autosaveInFlight_ = true;
    QMetaObject::invokeMethod(
        &ioWorker_,
        [this, store = std::move(store), request]() mutable {
            const nemo::ProjectWriteResult result = store.write(request);
            QMetaObject::invokeMethod(
                this,
                [this, result] {
                    autosaveInFlight_ = false;
                    if (!result.ok) {
                        // Autosave failure is real project state: keep it in the
                        // session's file diagnostic and raise it through the
                        // existing error dialog once per distinct message.
                        session_.setLastFileError(result.error.message);
                        const QString message = errorText(result.error, result.target);
                        if (message != lastAutosaveError_) {
                            lastAutosaveError_ = message;
                            setError(message);
                            emit autosaveFailed();
                        }
                        refreshFromSession();
                    } else {
                        lastAutosaveError_.clear();
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

}  // namespace nemo::ui
