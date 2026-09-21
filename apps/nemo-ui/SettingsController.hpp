#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <vector>

#include "nemo/eval/ViewerCache.hpp"
#include "nemo/extensions/InstalledPackages.hpp"

namespace nemo::workspace {
class WorkspaceController;
}

namespace nemo::ui {
class NativeFileChooser;
class ViewerRuntime;

// GUI-thread preference adapter. The immutable startup inventory remains owned
// by InstalledPackages; editing requested activation never changes live code.
class SettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList packages READ packages NOTIFY changed)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed)
    Q_PROPERTY(QString extensionNotice READ extensionNotice NOTIFY changed)
    Q_PROPERTY(QString requestedCacheDirectory READ requestedCacheDirectory NOTIFY changed)
    Q_PROPERTY(QString cacheStatus READ cacheStatus NOTIFY usageChanged)
public:
    SettingsController(workspace::WorkspaceController& workspace, NativeFileChooser& chooser,
                       const extensions::InstalledPackages& packages, ViewerRuntime& runtime,
                       eval::ViewerCacheOptions effectiveCache, bool cacheDirectoryOverride, QObject* parent = nullptr);

    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QVariantList packages() const;
    [[nodiscard]] bool restartRequired() const;
    [[nodiscard]] QString extensionNotice() const;
    [[nodiscard]] QString requestedCacheDirectory() const;
    [[nodiscard]] QString cacheStatus() const { return cacheStatus_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void addExtensionFolder();
    Q_INVOKABLE void removeExtensionFolder(const QString& directory);
    Q_INVOKABLE void setPackageEnabled(const QString& directory, const QString& id, const QString& version,
                                       bool enabled);
    Q_INVOKABLE QString changeExplanation(const QString& directory, bool enabled) const;
    Q_INVOKABLE void openExtensionsFolder();
    Q_INVOKABLE void browseCacheFolder();
    Q_INVOKABLE bool setCacheStorage(const QString& directory, const QString& diskMiB);
    Q_INVOKABLE void openCacheFolder();
    Q_INVOKABLE void pollUsage();
signals:
    void changed();
    void usageChanged();

private:
    void setError(QString error);
    bool savePreferences(extensions::PackagePreferences preferences);
    void scan();
    void openFolder(const QString& directory);
    workspace::WorkspaceController& workspace_;
    NativeFileChooser& chooser_;
    const extensions::InstalledPackages& startup_;
    ViewerRuntime& runtime_;
    const eval::ViewerCacheOptions effectiveCache_;
    const bool cacheDirectoryOverride_;
    extensions::PackagePreferences preferences_;
    std::vector<extensions::PackageInfo> inventory_;
    bool preferencesReadable_{false};
    QString error_;
    QString cacheStatus_;
};
}  // namespace nemo::ui
