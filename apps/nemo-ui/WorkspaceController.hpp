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

    Q_INVOKABLE bool save();
    Q_INVOKABLE void reset();

    // Project-file presentation boundary. The project envelope stores the same
    // versioned workspace records the standalone workspace file uses, so layout,
    // panel state (including unavailable-panel metadata) and appearance
    // round-trip without a second arrangement model. Qt/QML never sees this
    // payload; only the project file adapter reads and applies it.
    [[nodiscard]] nlohmann::json projectPresentation();
    [[nodiscard]] bool applyProjectPresentation(const nlohmann::json& presentation);

signals:
    void rootChanged();
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
    void setError(QString message);
    void snapshotActiveWorkspace();
    [[nodiscard]] int workspaceIndex(const QString& id) const;
    [[nodiscard]] QString nextWorkspaceId() const;
    static bool validColor(const QString& value);
    static bool validPreset(const QString& value);
    static QString normalizedName(const QString& value);
    void restoreFromJson(const nlohmann::json& json);
    [[nodiscard]] nlohmann::json persistenceJson() const;

    Workspace workspace_;
    QString path_;
    QString error_;
    bool preserveUnreadableFile_ = false;
    std::vector<PanelDescriptor> panelDescriptors_;
    std::vector<Preset> presets_;
    QString activeWorkspaceId_;
    QString appearancePreset_ = QStringLiteral("Graphite");
    QString accentOverride_;
    QVariantMap categoryColors_;
};

}  // namespace nemo::workspace
