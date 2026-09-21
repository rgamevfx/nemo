#pragma once

#include "Workspace.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <vector>

namespace nemo::workspace {

class WorkspaceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap root READ root NOTIFY rootChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantList panelTypes READ panelTypes NOTIFY panelTypesChanged)
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(QString activeWorkspaceId READ activeWorkspaceId NOTIFY activeWorkspaceIdChanged)
    Q_PROPERTY(QString appearancePreset READ appearancePreset NOTIFY appearanceChanged)
    Q_PROPERTY(QString accentOverride READ accentOverride NOTIFY appearanceChanged)
    Q_PROPERTY(QVariantMap categoryColors READ categoryColors NOTIFY appearanceChanged)
    // Application preferences: the last Settings section, the Settings popout
    // size and the cache/storage choices. They are user state, not composition
    // state, so they never travel in project presentation.
    Q_PROPERTY(int settingsSection READ settingsSection NOTIFY settingsChanged)
    Q_PROPERTY(int settingsWidth READ settingsWidth NOTIFY settingsChanged)
    Q_PROPERTY(int settingsHeight READ settingsHeight NOTIFY settingsChanged)
    Q_PROPERTY(QString cacheDirectory READ cacheDirectory NOTIFY settingsChanged)
    Q_PROPERTY(int cacheDiskMiB READ cacheDiskMiB NOTIFY settingsChanged)

public:
    explicit WorkspaceController(QString path, QObject* parent = nullptr);

    [[nodiscard]] QVariantMap root() const;
    [[nodiscard]] QString error() const;
    [[nodiscard]] QVariantList panelTypes() const;
    [[nodiscard]] QVariantList workspaces() const;
    [[nodiscard]] QString activeWorkspaceId() const;
    [[nodiscard]] QString appearancePreset() const;
    [[nodiscard]] QString accentOverride() const;
    [[nodiscard]] QVariantMap categoryColors() const;
    [[nodiscard]] int settingsSection() const;
    [[nodiscard]] int settingsWidth() const;
    [[nodiscard]] int settingsHeight() const;
    // Empty means the runtime platform default; the consumer resolves it.
    [[nodiscard]] QString cacheDirectory() const;
    [[nodiscard]] int cacheDiskMiB() const;

    Q_INVOKABLE void registerPanelType(const QString& typeId, const QString& title, const QString& qmlSource,
                                       const QString& headerSource = {});
    Q_INVOKABLE QVariantMap panelDescriptor(const QString& typeId) const;
    Q_INVOKABLE QString createPanel(const QString& leafId, const QString& typeId,
                                    const QString& group = QStringLiteral("A"));
    Q_INVOKABLE QVariantMap panelState(const QString& panelId) const;
    Q_INVOKABLE void setPanelState(const QString& panelId, const QVariantMap& state);

    Q_INVOKABLE void split(const QString& leafId, const QString& orientation);
    Q_INVOKABLE void setRatio(const QString& splitId, double ratio);
    Q_INVOKABLE void setPanelType(const QString& panelId, const QString& type);
    Q_INVOKABLE void setGroup(const QString& panelId, const QString& group);
    Q_INVOKABLE void activate(const QString& leafId, const QString& panelId);
    Q_INVOKABLE void addTab(const QString& leafId, const QString& type);
    Q_INVOKABLE void closePanel(const QString& panelId);
    Q_INVOKABLE bool movePanel(const QString& panelId, const QString& leafId, const QString& placement, int tabIndex);

    Q_INVOKABLE QString createWorkspace(const QString& name);
    Q_INVOKABLE bool renameWorkspace(const QString& id, const QString& name);
    Q_INVOKABLE QString duplicateWorkspace(const QString& id, const QString& name);
    Q_INVOKABLE bool closeWorkspace(const QString& id);
    Q_INVOKABLE bool switchWorkspace(const QString& id);
    Q_INVOKABLE bool moveWorkspace(const QString& id, int offset);

    Q_INVOKABLE bool setAppearancePreset(const QString& preset);
    Q_INVOKABLE bool setAccentOverride(const QString& color);
    Q_INVOKABLE bool setCategoryColor(const QString& category, const QString& color);
    Q_INVOKABLE void resetCategoryColors();
    Q_INVOKABLE void resetAppearance();

    // Remembered Settings popout section and size. Section 0..2 selects the
    // App Settings / UI Settings / Extensions pane; the size is the resizable
    // popout's accepted inner size. Validation mirrors what the popout may
    // present, and a rejected or unsaved value leaves the last accepted one.
    Q_INVOKABLE bool setSettingsWindow(int section, int width, int height);
    // Application cache/storage preference. An empty directory keeps the
    // runtime's platform default; a non-empty one must be absolute. The
    // caller owns writability/effective-state diagnosis; this owner only
    // validates the shape of the request and that it was persisted.
    Q_INVOKABLE bool setCacheStorage(const QString& directory, int diskMiB);

    Q_INVOKABLE bool save();
    Q_INVOKABLE void reset();

    // Project-file presentation boundary. The project envelope stores the same
    // versioned workspace records the standalone workspace file uses, so layout
    // and panel state (including unavailable-panel metadata) round-trip without
    // a second arrangement model. It deliberately carries no appearance or
    // settings record: those are application preferences stored in the
    // workspace file, and a project must never overwrite them. Qt/QML never
    // sees this payload; only the project file adapter reads and applies it.
    [[nodiscard]] nlohmann::json projectPresentation();
    [[nodiscard]] bool applyProjectPresentation(const nlohmann::json& presentation);

signals:
    void rootChanged();
    // A panel-state write. Panel state is presentation state that the writing
    // panel already holds, so the record change is delivered only to its owner
    // (and to persistence consumers through presentationChanged). Rebuilding
    // the workspace root here would re-deliver every panel's state and make
    // every panel rebuild its display model once per input event — the cost a
    // view gesture must not pay.
    void panelStateChanged(const QString& panelId);
    // Persistence-only notification: the arrangement/panel payload changed but
    // the QML tree must not be rebuilt. Splitter ratio drags emit this and
    // deliberately skip rootChanged, so project dirty/autosave consumers must
    // observe it without disturbing the existing gesture.
    void presentationChanged();
    void errorChanged();
    void panelTypesChanged();
    void workspacesChanged();
    void activeWorkspaceIdChanged();
    void appearanceChanged();
    // Any persisted application preference (Settings section/size, cache
    // directory, disk budget) changed. Application preferences never emit
    // presentationChanged or rootChanged: they are not project state.
    void settingsChanged();

private:
    struct PanelDescriptor {
        QString typeId;
        QString title;
        QString source;
        QString headerSource;
    };
    struct Preset {
        QString id;
        QString name;
        Workspace workspace;
    };

    void change(const std::function<void()>& operation, bool notify = true);
    // Sole owner helper for rootChanged. WorkspaceController owns the root
    // snapshot and its notification; QML handlers reached by an in-flight
    // delivery (panels persist synchronously on identity change/teardown) can
    // request another root change while that delivery is still running.
    // Emitting nested would re-notify a QML binding that is mid-update (Qt
    // reports that as a binding loop), so nested requests are coalesced and the
    // latest snapshot is delivered synchronously once the outer delivery
    // returns. Requests made while no delivery is active emit immediately.
    void notifyRootChanged();
    void setError(QString message);
    void snapshotActiveWorkspace();
    [[nodiscard]] int workspaceIndex(const QString& id) const;
    [[nodiscard]] QString nextWorkspaceId() const;
    static bool validColor(const QString& value);
    static bool validPreset(const QString& value);
    static QString normalizedName(const QString& value);
    // Validate and commit one appearance edit. The value becomes visible only
    // after the preference store has accepted it: a failed write restores the
    // last accepted value and keeps the write diagnosis visible.
    bool applyAppearance(const std::function<void()>& mutate);
    // Same contract for the application preferences.
    bool applySettings(const std::function<void()>& mutate);
    // includeApplicationState is true only for the workspace file, which is the
    // application preference store. A project presentation restores layout
    // records only: an older project's appearance/settings must not overwrite
    // the user's choices. restoreFromJson is transactional either way.
    void restoreFromJson(const nlohmann::json& json, bool includeApplicationState);
    [[nodiscard]] nlohmann::json layoutJson() const;
    [[nodiscard]] nlohmann::json persistenceJson() const;

    Workspace workspace_;
    QString path_;
    QString error_;
    bool preserveUnreadableFile_ = false;
    // True while notifyRootChanged() is executing its signal emission; a
    // nested request sets rootChangePending_ instead of re-entering.
    bool deliveringRootChanged_ = false;
    bool rootChangePending_ = false;
    std::vector<PanelDescriptor> panelDescriptors_;
    std::vector<Preset> presets_;
    QString activeWorkspaceId_;
    QString appearancePreset_ = QStringLiteral("Graphite");
    QString accentOverride_;
    QVariantMap categoryColors_;
    int settingsSection_ = 0;
    int settingsWidth_ = 800;
    int settingsHeight_ = 600;
    QString cacheDirectory_;
    int cacheDiskMiB_ = 2048;
};

}  // namespace nemo::workspace
