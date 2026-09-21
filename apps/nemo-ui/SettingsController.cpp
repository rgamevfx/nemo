#include "SettingsController.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <algorithm>
#include <limits>

#include "NativeFileChooser.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"

namespace nemo::ui {
namespace {
QString text(const std::string& value) {
    return QString::fromStdString(value);
}
QString joined(const std::vector<std::string>& values) {
    QStringList result;
    for (const auto& value : values)
        result.push_back(text(value));
    return result.join(QStringLiteral(", "));
}
QString pathText(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(value.data()), static_cast<qsizetype>(value.size()));
}
std::filesystem::path pathFromText(const QString& value) {
    return std::filesystem::path(value.toStdU16String());
}

[[nodiscard]] const extensions::PackageInfo* findPackageRecord(const std::vector<extensions::PackageInfo>& inventory,
                                                               const extensions::PackageInfo& package) {
    const auto found = std::find_if(inventory.begin(), inventory.end(), [&](const auto& entry) {
        return entry.id == package.id && entry.directory == package.directory;
    });
    return found == inventory.end() ? nullptr : &*found;
}

[[nodiscard]] bool activationChanged(const extensions::PackageInfo& requested,
                                     const extensions::PackageInfo* previous) {
    return requested.requestedEnabled != (previous && previous->requestedEnabled) ||
           (previous && previous->active && previous->version != requested.version);
}

QString statusName(extensions::PackageStatus status) {
    using Status = extensions::PackageStatus;
    switch (status) {
    case Status::Active:
        return QStringLiteral("Active");
    case Status::Disabled:
        return QStringLiteral("Disabled");
    case Status::MissingPackage:
        return QStringLiteral("Missing folder/package");
    case Status::MalformedManifest:
        return QStringLiteral("Malformed metadata");
    case Status::Incompatible:
        return QStringLiteral("Incompatible");
    case Status::DuplicateIdentity:
        return QStringLiteral("Duplicate identity");
    case Status::MissingDependency:
        return QStringLiteral("Missing dependency");
    case Status::DisabledDependency:
        return QStringLiteral("Disabled dependency");
    case Status::RefusedDependency:
        return QStringLiteral("Unavailable dependency");
    case Status::DependencyCycle:
        return QStringLiteral("Dependency cycle");
    case Status::FailedToLoad:
        return QStringLiteral("Failed to load");
    }
    return {};
}
}  // namespace

SettingsController::SettingsController(workspace::WorkspaceController& workspace, NativeFileChooser& chooser,
                                       const extensions::InstalledPackages& packages, ViewerRuntime& runtime,
                                       eval::ViewerCacheOptions effectiveCache, bool cacheDirectoryOverride,
                                       QObject* parent)
    : QObject(parent), workspace_(workspace), chooser_(chooser), startup_(packages), runtime_(runtime),
      effectiveCache_(std::move(effectiveCache)), cacheDirectoryOverride_(cacheDirectoryOverride) {
    refresh();
    pollUsage();
}

void SettingsController::setError(QString error) {
    error_ = std::move(error);
    emit changed();
}

void SettingsController::scan() {
    inventory_ = extensions::inspectInstalledPackages(preferences_);
    // Keep removed or changed active packages visible until process shutdown.
    for (const auto& active : startup_.inventory()) {
        if (!active.active)
            continue;
        if (!findPackageRecord(inventory_, active)) {
            auto retained = active;
            retained.requestedEnabled = preferences_.isEnabled(active.id, active.directory);
            retained.canEnable = false;
            inventory_.push_back(std::move(retained));
        }
    }
    emit changed();
}

void SettingsController::refresh() {
    const auto loaded = extensions::loadPackagePreferences(extensions::packagePreferencesPath());
    preferencesReadable_ = loaded.diagnostic.empty();
    preferences_ = loaded.preferences;
    error_ = text(loaded.diagnostic);
    scan();
}

QVariantList SettingsController::packages() const {
    QVariantList result;
    const bool trustedOverride = startup_.trustedOverride();
    for (const auto& info : inventory_) {
        const auto* atStartup = findPackageRecord(startup_.inventory(), info);
        const bool active = atStartup && atStartup->active;
        QString status = active ? QStringLiteral("Active now")
                                : (info.requestedEnabled && info.status == extensions::PackageStatus::Disabled
                                       ? QStringLiteral("Not active")
                                       : statusName(info.status));
        if (trustedOverride) {
            status += info.requestedEnabled ? QStringLiteral(" · Developer override; saved choice: enabled")
                                            : QStringLiteral(" · Developer override; saved choice: disabled");
        } else {
            status += info.requestedEnabled ? QStringLiteral(" · Enabled for next launch")
                                            : QStringLiteral(" · Disabled for next launch");
            if (activationChanged(info, atStartup))
                status += QStringLiteral(" · Restart required");
        }
        if (!info.diagnostic.empty())
            status += QStringLiteral("\n") + text(info.diagnostic);
        if (atStartup && !atStartup->diagnostic.empty() && atStartup->diagnostic != info.diagnostic)
            status +=
                QStringLiteral("\nAt startup — %1: %2").arg(statusName(atStartup->status), text(atStartup->diagnostic));
        const QString author = info.author.empty() ? QStringLiteral("Not provided") : text(info.author);
        QString details =
            QStringLiteral("Identity: %1\nAuthor / maintainer: %2\nLocation: %3\nNodes: %4\nEditors: %5\nPanels: "
                           "%6\nDependencies: %7")
                .arg(text(info.id), author, text(info.directory), joined(info.nodeTypes), joined(info.editors),
                     joined(info.panels),
                     info.dependencies.empty() ? QStringLiteral("None declared") : joined(info.dependencies));
        if (!info.description.empty())
            details.prepend(text(info.description) + QStringLiteral("\n\n"));
        result.push_back(QVariantMap{
            {QStringLiteral("id"), text(info.id)},
            {QStringLiteral("name"), text(info.name)},
            {QStringLiteral("version"), info.version ? QString::number(info.version) : QString()},
            {QStringLiteral("directory"), text(info.directory)},
            {QStringLiteral("details"), details},
            {QStringLiteral("status"), status},
            {QStringLiteral("requestedEnabled"), info.requestedEnabled},
            {QStringLiteral("canChange"),
             (info.requestedEnabled || info.canEnable) && preferencesReadable_ && !trustedOverride},
            {QStringLiteral("linked"), std::find(preferences_.linkedFolders.begin(), preferences_.linkedFolders.end(),
                                                 info.directory) != preferences_.linkedFolders.end()}});
    }
    return result;
}

bool SettingsController::restartRequired() const {
    if (requestedCacheDirectory() != pathText(effectiveCache_.directory) ||
        static_cast<std::uint64_t>(workspace_.cacheDiskMiB()) * 1024ULL * 1024ULL != effectiveCache_.maxDiskBytes)
        return true;
    if (startup_.trustedOverride())
        return false;
    for (const auto& info : inventory_) {
        if (activationChanged(info, findPackageRecord(startup_.inventory(), info)))
            return true;
    }
    return false;
}

QString SettingsController::extensionNotice() const {
    if (startup_.trustedOverride())
        return QStringLiteral("Developer override NEMO_EXTENSION_PATH is active. This process does not use saved "
                              "enablement; restart without that override to manage extensions normally.");
    if (inventory_.empty())
        return QStringLiteral("No local packages discovered. Newly discovered packages are disabled until you "
                              "explicitly trust and enable them.");
    return QStringLiteral("Requested enablement is saved immediately. Native activation and deactivation require a "
                          "normal restart; files are never uninstalled here.");
}

bool SettingsController::savePreferences(extensions::PackagePreferences preferences) {
    if (!preferencesReadable_) {
        setError(QStringLiteral("Extension preferences could not be read. Repair the reported file and Refresh before "
                                "changing activation."));
        return false;
    }
    std::string diagnostic;
    if (!extensions::savePackagePreferences(extensions::packagePreferencesPath(), preferences, diagnostic)) {
        setError(text(diagnostic));
        return false;
    }
    preferences_ = std::move(preferences);
    error_.clear();
    scan();
    return true;
}

void SettingsController::addExtensionFolder() {
    const bool accepted = chooser_.openFolder(
        this,
        [this](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled)
                return;
            if (outcome.status != NativeFileChooser::Outcome::Status::Chosen) {
                setError(outcome.message);
                return;
            }
            std::string canonical, reason;
            if (!extensions::canonicalPackageFolder(pathFromText(outcome.urls.constFirst().toLocalFile()), canonical,
                                                    reason)) {
                setError(text(reason));
                return;
            }
            auto next = preferences_;
            if (std::find(next.linkedFolders.begin(), next.linkedFolders.end(), canonical) == next.linkedFolders.end())
                next.linkedFolders.push_back(canonical);
            savePreferences(std::move(next));
        },
        QStringLiteral("Add Extension Folder — select the package containing manifest.json"));
    if (!accepted)
        setError(QStringLiteral("Another native file chooser is already open."));
}

void SettingsController::removeExtensionFolder(const QString& directory) {
    auto next = preferences_;
    next.removeLinkedFolder(directory.toStdString());
    savePreferences(std::move(next));
}

void SettingsController::setPackageEnabled(const QString& directory, const QString& id, const QString& version,
                                           bool enabled) {
    if (startup_.trustedOverride()) {
        setError(QStringLiteral("Restart without NEMO_EXTENSION_PATH before changing saved activation."));
        return;
    }
    auto next = preferences_;
    if (!enabled) {
        std::erase_if(next.enabled, [&](const auto& entry) { return entry.directory == directory.toStdString(); });
        savePreferences(std::move(next));
        return;
    }
    // Re-inspect before accepting the trust decision: the path may have changed
    // since the confirmation was opened. No native code runs on this path.
    const auto inspected = extensions::inspectInstalledPackages(preferences_);
    const auto found = std::find_if(inspected.begin(), inspected.end(),
                                    [&](const auto& info) { return info.directory == directory.toStdString(); });
    if (found == inspected.end() || !found->canEnable || found->id != id.toStdString() ||
        QString::number(found->version) != version) {
        setError(QStringLiteral("The inspected package changed or cannot be enabled. Refresh and inspect it again."));
        return;
    }
    next.setEnabled(found->id, found->directory, enabled);
    savePreferences(std::move(next));
}

QString SettingsController::changeExplanation(const QString& directory, bool enabled) const {
    QStringList lines;
    const auto found = std::find_if(inventory_.begin(), inventory_.end(),
                                    [&](const auto& info) { return info.directory == directory.toStdString(); });
    if (found == inventory_.end())
        return QStringLiteral("The package is no longer listed. Refresh before making changes.");
    lines.push_back(QStringLiteral("Package: %1").arg(text(found->id)));
    if (enabled && !found->dependencies.empty())
        lines.push_back(QStringLiteral("Requires: %1").arg(joined(found->dependencies)));
    if (!found->diagnostic.empty())
        lines.push_back(text(found->diagnostic));
    if (!enabled) {
        std::vector<std::string> affected{found->id};
        for (std::size_t cursor = 0; cursor < affected.size(); ++cursor) {
            for (const auto& info : inventory_) {
                if (std::find(affected.begin(), affected.end(), info.id) == affected.end() &&
                    std::find(info.dependencies.begin(), info.dependencies.end(), affected[cursor]) !=
                        info.dependencies.end())
                    affected.push_back(info.id);
            }
        }
        for (const auto& info : inventory_) {
            if (info.id != found->id && std::find(affected.begin(), affected.end(), info.id) != affected.end())
                lines.push_back(
                    QStringLiteral("Dependent package %1 at %2 requires this package, directly or transitively.")
                        .arg(text(info.id), text(info.directory)));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

void SettingsController::openFolder(const QString& directory) {
    if (!QDir().mkpath(directory)) {
        setError(QStringLiteral("Cannot create folder: %1").arg(directory));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
        setError(QStringLiteral("The desktop could not open folder: %1").arg(directory));
}

void SettingsController::openExtensionsFolder() {
    const auto roots = extensions::standardPackageRoots();
    if (roots.empty()) {
        setError(QStringLiteral("No absolute per-user data directory is available."));
        return;
    }
    openFolder(pathText(roots.front()));
}

QString SettingsController::requestedCacheDirectory() const {
    return workspace_.cacheDirectory().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                                 : workspace_.cacheDirectory();
}

bool SettingsController::setCacheStorage(const QString& directory, const QString& diskMiB) {
    bool ok = false;
    const int budget = diskMiB.toInt(&ok);
    if (!ok || budget <= 0) {
        setError(QStringLiteral("Disk budget must be a positive whole number of MiB (at most %1).")
                     .arg(std::numeric_limits<int>::max()));
        return false;
    }
    const QFileInfo location(directory.trimmed());
    if (!location.isAbsolute() || !location.isDir()) {
        setError(QStringLiteral("Choose an existing absolute cache directory."));
        return false;
    }
    QTemporaryFile probe(QDir(location.canonicalFilePath()).filePath(QStringLiteral(".nemo-write-check-XXXXXX")));
    if (!probe.open() || probe.write("nemo", 4) != 4 || !probe.flush()) {
        setError(QStringLiteral("Cache directory is not writable: %1 (%2)").arg(directory, probe.errorString()));
        return false;
    }
    if (!workspace_.setCacheStorage(location.canonicalFilePath(), budget)) {
        setError(workspace_.error());
        return false;
    }
    setError({});
    return true;
}

void SettingsController::browseCacheFolder() {
    if (!chooser_.openFolder(
            this,
            [this](NativeFileChooser::Outcome outcome) {
                if (outcome.status == NativeFileChooser::Outcome::Status::Chosen)
                    setCacheStorage(outcome.urls.constFirst().toLocalFile(),
                                    QString::number(workspace_.cacheDiskMiB()));
                else if (outcome.status == NativeFileChooser::Outcome::Status::Failed)
                    setError(outcome.message);
            },
            QStringLiteral("Choose Nemo viewer cache directory"), pathFromText(requestedCacheDirectory())))
        setError(QStringLiteral("Another native file chooser is already open."));
}

void SettingsController::openCacheFolder() {
    openFolder(requestedCacheDirectory());
}

void SettingsController::pollUsage() {
    const auto counts = runtime_.counts();
    const QString status =
        QStringLiteral(
            "Currently effective: %1\nDisk budget: %2 MiB\nViewer-cache disk usage: %3 MiB\nCompressed RAM hot "
            "set: %4 MiB\nBC7 GPU hot set: %5 MiB (%6 frames)\nActive cache frames: %7%8%9")
            .arg(pathText(effectiveCache_.directory))
            .arg(effectiveCache_.maxDiskBytes / (1024ULL * 1024ULL))
            .arg(static_cast<double>(counts.cacheDiskBytes) / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(static_cast<double>(counts.cacheCompressedRamBytes) / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(static_cast<double>(counts.cacheResidentBytes) / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(counts.cacheResidentFrames)
            .arg(counts.cacheActiveFrames)
            .arg(cacheDirectoryOverride_ ? QStringLiteral("\nDeveloper --viewer-cache-dir override is active.")
                                         : QString())
            .arg(counts.cacheError.empty() ? QString() : QStringLiteral("\nCache error: ") + text(counts.cacheError));
    if (status != cacheStatus_) {
        cacheStatus_ = status;
        emit usageChanged();
    }
}
}  // namespace nemo::ui
