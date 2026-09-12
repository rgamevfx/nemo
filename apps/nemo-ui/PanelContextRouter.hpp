#pragma once

#include "nemo/core/session/ProjectSession.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <map>
#include <optional>
#include <string>

namespace nemo::workspace {
class WorkspaceController;
}

namespace nemo::ui {

// Presentation-only routing state. This object deliberately owns no Document
// data: targets are stable string identities and source marks are read from
// the ProjectSession catalog whenever a context is observed.
class PanelContextRouter final : public QObject {
    Q_OBJECT
public:
    explicit PanelContextRouter(nemo::ProjectSession& session, QObject* parent = nullptr);

    // WorkspaceController is optional for headless/unit use. When present,
    // panel mode and viewer role are written to the panel's existing state.
    void setWorkspaceController(nemo::workspace::WorkspaceController* workspace);

    Q_INVOKABLE bool registerPanel(const QString& panelId, const QString& group, const QString& mode);
    Q_INVOKABLE bool removePanel(const QString& panelId);
    Q_INVOKABLE bool setActivePanel(const QString& panelId);
    Q_INVOKABLE bool setLinkMode(const QString& panelId, const QString& mode);
    Q_INVOKABLE bool setGroup(const QString& panelId, const QString& group);
    Q_INVOKABLE bool setViewerRole(const QString& panelId, const QString& role);
    Q_INVOKABLE QString viewerRole(const QString& panelId) const;
    Q_INVOKABLE QVariantMap contextFor(const QString& panelId) const;
    Q_INVOKABLE QVariantList availableTargets(const QString& panelId) const;
    // Target selection is explicit and group-scoped. These methods never
    // open decoders or mutate the Document.
    Q_INVOKABLE bool setGraphTarget(const QString& group, const QString& target);
    Q_INVOKABLE bool setTimelineTarget(const QString& group, const QString& target);
    Q_INVOKABLE bool openSource(const QString& group, const QString& source);
    Q_INVOKABLE bool setGroupContext(const QString& group, const QVariantMap& changes);
    // Presentation-only inspector relay: validates and re-broadcasts a
    // request to open a node's parameter inspector. No Document access.
    Q_INVOKABLE bool requestInspector(const QString& graphTarget, const QString& nodeId);

    [[nodiscard]] QString activePanel() const { return activePanel_; }

signals:
    void panelContextChanged(const QString& panelId);
    void groupContextChanged(const QString& group);
    void activePanelChanged(const QString& panelId);
    void panelBindingChanged(const QString& panelId);
    void inspectorRequested(const QString& graphTarget, const QString& nodeId);

private:
    struct GroupContext {
        QString graphTarget;
        QString timelineTarget;
        QString sourceTarget;
        double graphClock{0.0};
        double timelineClock{0.0};
        double sourceClock{0.0};
    };
    struct PanelBinding {
        QString group;
        QString mode;
        QString role;
        QString pinnedGroup;
        GroupContext pinned;
        bool hasPinned{false};
    };

    static int groupIndex(const QString& group) noexcept;
    static bool validMode(const QString& mode) noexcept;
    static bool validRole(const QString& role) noexcept;
    static QString normalized(const QString& value);
    [[nodiscard]] const GroupContext* resolvedGroup(const QString& panelId, QString* groupName = nullptr) const;
    [[nodiscard]] QVariantMap contextForBinding(const QString& panelId, const PanelBinding& binding) const;
    [[nodiscard]] QVariantMap groupMap(const QString& panelId, const QString& mode, const QString& group,
                                       const GroupContext& context) const;
    [[nodiscard]] bool targetAvailable(const QString& kind, const QString& target) const;
    [[nodiscard]] QVariantList sourceMarks(const QString& target) const;
    [[nodiscard]] static QVariantMap groupContextMap(const GroupContext& context);
    [[nodiscard]] static std::optional<GroupContext> groupContextFromMap(const QVariantMap& value);
    [[nodiscard]] bool setTarget(const QString& group, const QString& key, const QString& kind, const QString& target);
    [[nodiscard]] bool panelExists(const QString& panelId) const;
    void persist(const QString& panelId, const PanelBinding& binding);
    void synchronizeWorkspace();
    void documentChanged() noexcept;
    static void sessionChanged(void* context) noexcept;

    nemo::ProjectSession& session_;
    nemo::workspace::WorkspaceController* workspace_{nullptr};
    nemo::ProjectSession::Subscription sessionSubscription_;
    std::map<QString, PanelBinding> panels_;
    std::array<GroupContext, 5> groups_{};
    std::map<QString, QVariantMap> lastContexts_;
    QString activePanel_;
};

}  // namespace nemo::ui
