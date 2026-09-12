#pragma once

#include "nemo/core/session/ProjectSession.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace nemo::workspace {
class WorkspaceController;
}

namespace nemo::ui {

// Presentation-only routing state. This object deliberately owns no Document
// data: targets are stable string identities and source marks are read from
// the ProjectSession catalog whenever a context is observed. A panel is always
// bound to its own A-E group.
class PanelContextRouter final : public QObject {
    Q_OBJECT
public:
    explicit PanelContextRouter(nemo::ProjectSession& session, QObject* parent = nullptr);

    // WorkspaceController is optional for headless/unit use. When present, the
    // panel viewer role is written to the panel's existing state.
    void setWorkspaceController(nemo::workspace::WorkspaceController* workspace);

    Q_INVOKABLE bool registerPanel(const QString& panelId, const QString& group);
    Q_INVOKABLE bool removePanel(const QString& panelId);
    Q_INVOKABLE bool setActivePanel(const QString& panelId);
    Q_INVOKABLE bool setGroup(const QString& panelId, const QString& group);
    Q_INVOKABLE bool setViewerRole(const QString& panelId, const QString& role);
    Q_INVOKABLE QString viewerRole(const QString& panelId) const;
    Q_INVOKABLE QVariantMap contextFor(const QString& panelId) const;
    // Target selection is explicit and group-scoped. These methods never
    // open decoders or mutate the Document.
    Q_INVOKABLE bool setTimelineTarget(const QString& group, const QString& target);
    Q_INVOKABLE bool openSource(const QString& group, const QString& source);
    Q_INVOKABLE bool setGroupContext(const QString& group, const QVariantMap& changes);
    // Presentation-only inspector relay: validates and re-broadcasts a
    // group-scoped request to open a node's parameter inspector. No Document
    // access and no media lookup.
    Q_INVOKABLE bool requestInspector(const QString& group, const QString& network, const QString& nodeId);

    [[nodiscard]] QString activePanel() const { return activePanel_; }

    // Project-associated context records: the group-scoped target selections
    // that are not already carried by panel state. Applying validates every
    // restored identity against the replaced document, so a target that no
    // longer exists is dropped instead of fabricating a stale reference.
    [[nodiscard]] nlohmann::json contextPresentation() const;
    [[nodiscard]] bool applyContextPresentation(const nlohmann::json& presentation);
    // Drops every document-bound selection (timeline/source targets and both
    // playheads). Called when the published document is replaced: integer
    // identities are only comparable within one project, so retaining an old
    // target that happens to share an ID would route to the wrong context.
    // Panel group/role bindings are presentation preferences and stay.
    void resetDocumentContexts();

signals:
    void panelContextChanged(const QString& panelId);
    void groupContextChanged(const QString& group);
    void activePanelChanged(const QString& panelId);
    void inspectorRequested(const QString& group, const QString& network, const QString& nodeId);

private:
    struct GroupContext {
        QString timelineTarget;
        QString sourceTarget;
        double timelineClock{0.0};
        double sourceClock{0.0};
    };
    struct PanelBinding {
        QString group;
        QString role;
    };

    static int groupIndex(const QString& group) noexcept;
    static bool validRole(const QString& role) noexcept;
    static QString normalized(const QString& value);
    [[nodiscard]] QVariantMap groupMap(const QString& panelId, const GroupContext& context) const;
    [[nodiscard]] bool targetAvailable(const QString& kind, const QString& target) const;
    [[nodiscard]] QVariantList sourceMarks(const QString& target) const;
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
