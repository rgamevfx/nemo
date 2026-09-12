#include "PanelContextRouter.hpp"

#include "WorkspaceController.hpp"

#include <QVariant>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <string_view>

namespace nemo::ui {
namespace {
constexpr std::array<QStringView, 3> kRoles{QStringView(u"graph"), QStringView(u"timeline"), QStringView(u"media")};

QString unavailableReason(const QString& kind) {
    return QStringLiteral("%1 target is unavailable").arg(kind);
}

bool parseUnsigned(const QString& value, std::uint64_t* result) {
    const auto bytes = value.toLatin1();
    if (bytes.isEmpty())
        return false;
    std::uint64_t parsed = 0;
    const auto* first = bytes.constData();
    const auto* last = first + bytes.size();
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last)
        return false;
    *result = parsed;
    return true;
}

void collectPanelIds(const QVariantMap& node, std::set<QString>* ids) {
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("tabs")) {
        for (const auto& panel : node.value(QStringLiteral("panels")).toList()) {
            const auto id = panel.toMap().value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                ids->insert(id);
        }
        return;
    }
    for (const auto& child : node.value(QStringLiteral("children")).toList())
        collectPanelIds(child.toMap(), ids);
}
}  // namespace

PanelContextRouter::PanelContextRouter(nemo::ProjectSession& session, QObject* parent)
    : QObject(parent), session_(session),
      sessionSubscription_(session.subscribe(this, &PanelContextRouter::sessionChanged)) {}

void PanelContextRouter::setWorkspaceController(nemo::workspace::WorkspaceController* workspace) {
    if (workspace_ == workspace)
        return;
    if (workspace_)
        disconnect(workspace_, nullptr, this, nullptr);
    workspace_ = workspace;
    if (workspace_) {
        connect(workspace_, &nemo::workspace::WorkspaceController::rootChanged, this,
                &PanelContextRouter::synchronizeWorkspace);
        synchronizeWorkspace();
    }
}

QString PanelContextRouter::normalized(const QString& value) {
    return value.trimmed();
}

int PanelContextRouter::groupIndex(const QString& group) noexcept {
    if (group.size() != 1)
        return -1;
    const auto index = group.at(0).unicode() - QChar(u'A').unicode();
    return index >= 0 && index < 5 ? index : -1;
}

bool PanelContextRouter::validRole(const QString& role) noexcept {
    return std::any_of(kRoles.begin(), kRoles.end(), [&](QStringView value) { return role == value; });
}

bool PanelContextRouter::panelExists(const QString& panelId) const {
    return !panelId.isEmpty() && panels_.find(panelId) != panels_.end();
}

void PanelContextRouter::persist(const QString& panelId, const PanelBinding& binding) {
    if (!workspace_)
        return;
    auto state = workspace_->panelState(panelId);
    state.insert(QStringLiteral("viewerRole"), binding.role);
    workspace_->setPanelState(panelId, state);
}

void PanelContextRouter::synchronizeWorkspace() {
    if (!workspace_)
        return;
    std::set<QString> current;
    collectPanelIds(workspace_->root(), &current);
    for (auto panel = panels_.begin(); panel != panels_.end();) {
        if (current.contains(panel->first)) {
            ++panel;
            continue;
        }
        const auto removed = panel->first;
        lastContexts_.erase(removed);
        panel = panels_.erase(panel);
        if (activePanel_ == removed) {
            activePanel_.clear();
            emit activePanelChanged(activePanel_);
        }
        emit panelContextChanged(removed);
    }
}

bool PanelContextRouter::registerPanel(const QString& rawPanelId, const QString& rawGroup) {
    const auto panelId = normalized(rawPanelId);
    const auto group = normalized(rawGroup);
    if (panelId.isEmpty() || groupIndex(group) < 0)
        return false;

    auto found = panels_.find(panelId);
    if (found != panels_.end()) {
        found->second.group = group;
        lastContexts_[panelId] = contextFor(panelId);
        return true;
    }

    PanelBinding binding;
    binding.group = group;
    binding.role = QStringLiteral("graph");
    if (workspace_) {
        const auto state = workspace_->panelState(panelId);
        const auto savedRole = state.value(QStringLiteral("viewerRole")).toString();
        if (validRole(savedRole))
            binding.role = savedRole;
    }
    panels_[panelId] = binding;
    lastContexts_[panelId] = contextFor(panelId);
    return true;
}

bool PanelContextRouter::removePanel(const QString& panelId) {
    const auto found = panels_.find(panelId);
    if (found == panels_.end())
        return false;
    const bool wasActive = activePanel_ == panelId;
    panels_.erase(found);
    lastContexts_.erase(panelId);
    if (wasActive) {
        activePanel_.clear();
        emit activePanelChanged(activePanel_);
    }
    emit panelContextChanged(panelId);
    return true;
}

bool PanelContextRouter::setActivePanel(const QString& panelId) {
    if (!panelExists(panelId) || activePanel_ == panelId)
        return panelExists(panelId);
    activePanel_ = panelId;
    emit activePanelChanged(activePanel_);
    return true;
}

bool PanelContextRouter::setGroup(const QString& panelId, const QString& rawGroup) {
    const auto group = normalized(rawGroup);
    auto found = panels_.find(panelId);
    if (found == panels_.end() || groupIndex(group) < 0)
        return false;
    if (found->second.group == group)
        return true;
    found->second.group = group;
    if (workspace_)
        workspace_->setGroup(panelId, group);
    const auto before = lastContexts_.at(panelId);
    const auto now = contextFor(panelId);
    lastContexts_[panelId] = now;
    if (now != before)
        emit panelContextChanged(panelId);
    return true;
}

bool PanelContextRouter::setViewerRole(const QString& panelId, const QString& rawRole) {
    const auto role = normalized(rawRole);
    auto found = panels_.find(panelId);
    if (found == panels_.end() || !validRole(role))
        return false;
    if (found->second.role == role)
        return true;
    found->second.role = role;
    persist(panelId, found->second);
    lastContexts_[panelId] = contextFor(panelId);
    emit panelContextChanged(panelId);
    return true;
}

QString PanelContextRouter::viewerRole(const QString& panelId) const {
    const auto found = panels_.find(panelId);
    return found == panels_.end() ? QString{} : found->second.role;
}

bool PanelContextRouter::targetAvailable(const QString& kind, const QString& target) const {
    if (target.isEmpty())
        return false;
    if (kind == QStringLiteral("source")) {
        for (const auto& entry : session_.document().mediaCatalog.entries()) {
            if (target == QString::fromStdString(entry.sourceKey) ||
                target == QString::number(static_cast<qulonglong>(entry.id)))
                return true;
        }
        for (const auto& source : session_.document().sources) {
            if (target == QString::fromStdString(source.first))
                return true;
        }
        return false;
    }
    if (kind == QStringLiteral("timeline") && target.startsWith(QStringLiteral("source:"))) {
        const auto source = target.mid(7).toStdString();
        return session_.document().sources.contains(source);
    }
    // Timeline targets are presentation identities. Known document identities
    // receive strict deletion checks; opaque plugin IDs remain valid until
    // their owner changes the group selection.
    if (kind == QStringLiteral("timeline") && target.startsWith(QStringLiteral("channel:"))) {
        std::uint64_t id = 0;
        if (!parseUnsigned(target.mid(8), &id))
            return false;
        return session_.document().animationChannel(static_cast<AnimationChannelId>(id)) != nullptr;
    }
    return true;
}

QVariantList PanelContextRouter::sourceMarks(const QString& target) const {
    for (const auto& entry : session_.document().mediaCatalog.entries()) {
        if (target != QString::fromStdString(entry.sourceKey) &&
            target != QString::number(static_cast<qulonglong>(entry.id)))
            continue;
        QVariantList marks;
        for (const auto& mark : entry.marks) {
            QVariantMap value;
            if (mark.inFrame)
                value.insert(QStringLiteral("inFrame"), QVariant::fromValue<qlonglong>(*mark.inFrame));
            if (mark.outFrame)
                value.insert(QStringLiteral("outFrame"), QVariant::fromValue<qlonglong>(*mark.outFrame));
            marks.push_back(value);
        }
        return marks;
    }
    return {};
}

QVariantMap PanelContextRouter::groupMap(const QString& panelId, const GroupContext& context) const {
    const auto& binding = panels_.at(panelId);
    QVariantMap result{{QStringLiteral("panelId"), panelId},
                       {QStringLiteral("group"), binding.group},
                       {QStringLiteral("viewerRole"), binding.role},
                       {QStringLiteral("timelineTarget"), context.timelineTarget},
                       {QStringLiteral("sourceTarget"), context.sourceTarget},
                       {QStringLiteral("timelineClock"), context.timelineClock},
                       {QStringLiteral("sourceClock"), context.sourceClock}};
    result.insert(QStringLiteral("sourceMarks"), sourceMarks(context.sourceTarget));
    const QString kind = binding.role == QStringLiteral("media") ? QStringLiteral("source") : binding.role;
    const QString target = binding.role == QStringLiteral("timeline") ? context.timelineTarget
                           : binding.role == QStringLiteral("media")  ? context.sourceTarget
                                                                      : QString{};
    // The graph role carries no image gate: the viewer's attached upstream
    // node is owned by the viewer controller, not by this router.
    const bool available = binding.role != QStringLiteral("graph") && targetAvailable(kind, target);
    result.insert(QStringLiteral("available"), available);
    result.insert(QStringLiteral("unavailableReason"), available ? QString{} : unavailableReason(kind));
    return result;
}

QVariantMap PanelContextRouter::contextFor(const QString& panelId) const {
    const auto found = panels_.find(panelId);
    if (found == panels_.end())
        return {{QStringLiteral("panelId"), panelId},
                {QStringLiteral("available"), false},
                {QStringLiteral("unavailableReason"), QStringLiteral("unknown panel")}};
    const auto index = groupIndex(found->second.group);
    if (index < 0)
        return {{QStringLiteral("panelId"), panelId},
                {QStringLiteral("group"), found->second.group},
                {QStringLiteral("viewerRole"), found->second.role},
                {QStringLiteral("available"), false},
                {QStringLiteral("unavailableReason"), QStringLiteral("panel group is unavailable")}};
    return groupMap(panelId, groups_[index]);
}

bool PanelContextRouter::setTarget(const QString& rawGroup, const QString& key, const QString& kind,
                                   const QString& rawTarget) {
    const auto group = normalized(rawGroup);
    const auto target = normalized(rawTarget);
    if (groupIndex(group) < 0 || (!target.isEmpty() && !targetAvailable(kind, target)))
        return false;
    return setGroupContext(group, {{key, target}});
}

bool PanelContextRouter::setTimelineTarget(const QString& group, const QString& target) {
    return setTarget(group, QStringLiteral("timelineTarget"), QStringLiteral("timeline"), target);
}

bool PanelContextRouter::openSource(const QString& group, const QString& source) {
    const auto target = normalized(source);
    if (target.isEmpty())
        return false;
    return setTarget(group, QStringLiteral("sourceTarget"), QStringLiteral("source"), target);
}

bool PanelContextRouter::setGroupContext(const QString& rawGroup, const QVariantMap& changes) {
    const auto group = normalized(rawGroup);
    const auto index = groupIndex(group);
    if (index < 0)
        return false;
    GroupContext next = groups_[index];
    const auto applyTarget = [&](const QString& key, QString* output) {
        if (!changes.contains(key))
            return true;
        if (!changes.value(key).canConvert<QString>())
            return false;
        *output = normalized(changes.value(key).toString());
        return true;
    };
    const auto applyClock = [&](const QString& key, double* output) {
        if (!changes.contains(key))
            return true;
        bool ok = false;
        const auto value = changes.value(key).toDouble(&ok);
        if (!ok || !std::isfinite(value))
            return false;
        *output = value;
        return true;
    };
    if (!applyTarget(QStringLiteral("timelineTarget"), &next.timelineTarget) ||
        !applyTarget(QStringLiteral("sourceTarget"), &next.sourceTarget) ||
        !applyClock(QStringLiteral("timelineClock"), &next.timelineClock) ||
        !applyClock(QStringLiteral("sourceClock"), &next.sourceClock))
        return false;
    if (next.timelineTarget == groups_[index].timelineTarget && next.sourceTarget == groups_[index].sourceTarget &&
        next.timelineClock == groups_[index].timelineClock && next.sourceClock == groups_[index].sourceClock)
        return true;
    groups_[index] = std::move(next);
    emit groupContextChanged(group);
    for (const auto& [panelId, binding] : panels_) {
        if (binding.group != group)
            continue;
        const auto before = lastContexts_.at(panelId);
        const auto now = contextFor(panelId);
        lastContexts_[panelId] = now;
        if (now != before)
            emit panelContextChanged(panelId);
    }
    return true;
}

bool PanelContextRouter::requestInspector(const QString& rawGroup, const QString& rawNetwork,
                                          const QString& rawNodeId) {
    const auto group = normalized(rawGroup);
    const auto network = normalized(rawNetwork);
    const auto node = normalized(rawNodeId);
    if (group.isEmpty() || network.isEmpty() || node.isEmpty())
        return false;
    emit inspectorRequested(group, network, node);
    return true;
}

nlohmann::json PanelContextRouter::contextPresentation() const {
    nlohmann::json groups = nlohmann::json::object();
    for (int index = 0; index < 5; ++index) {
        const GroupContext& context = groups_[static_cast<std::size_t>(index)];
        if (context.timelineTarget.isEmpty() && context.sourceTarget.isEmpty() && context.timelineClock == 0.0 &&
            context.sourceClock == 0.0) {
            continue;
        }
        const auto letter = QString(QChar(u'A' + index)).toStdString();
        groups[letter] = {{"timelineTarget", context.timelineTarget.toStdString()},
                          {"sourceTarget", context.sourceTarget.toStdString()},
                          {"timelineClock", context.timelineClock},
                          {"sourceClock", context.sourceClock}};
    }
    return {{"groups", std::move(groups)}};
}

void PanelContextRouter::resetDocumentContexts() {
    for (int index = 0; index < 5; ++index) {
        GroupContext& context = groups_[static_cast<std::size_t>(index)];
        if (context.timelineTarget.isEmpty() && context.sourceTarget.isEmpty() && context.timelineClock == 0.0 &&
            context.sourceClock == 0.0) {
            continue;
        }
        context = GroupContext{};
        const QString letter = QString(QChar(u'A' + index));
        emit groupContextChanged(letter);
        for (const auto& [panelId, binding] : panels_) {
            if (binding.group != letter) {
                continue;
            }
            const auto before = lastContexts_.at(panelId);
            const auto now = contextFor(panelId);
            lastContexts_[panelId] = now;
            if (now != before) {
                emit panelContextChanged(panelId);
            }
        }
    }
}

bool PanelContextRouter::applyContextPresentation(const nlohmann::json& presentation) {
    if (!presentation.is_object()) {
        return false;
    }
    const auto groups = presentation.find("groups");
    if (groups == presentation.end() || !groups->is_object()) {
        return false;
    }
    for (int index = 0; index < 5; ++index) {
        const QString letter = QString(QChar(u'A' + index));
        const auto entry = groups->find(letter.toStdString());
        if (entry == groups->end() || !entry->is_object()) {
            continue;
        }
        GroupContext next = groups_[static_cast<std::size_t>(index)];
        const auto readTarget = [&](const char* key, const QString& kind, QString* output) {
            const auto value = entry->find(key);
            if (value == entry->end() || !value->is_string()) {
                return;
            }
            const QString target = normalized(QString::fromStdString(value->get<std::string>()));
            // A restored target the replaced document cannot address is a stale
            // project-context identity: clear it rather than display or resolve
            // against a different document's identity.
            *output = target.isEmpty() || targetAvailable(kind, target) ? target : QString{};
        };
        readTarget("timelineTarget", QStringLiteral("timeline"), &next.timelineTarget);
        readTarget("sourceTarget", QStringLiteral("source"), &next.sourceTarget);
        const auto readClock = [&](const char* key, double* output) {
            const auto value = entry->find(key);
            if (value == entry->end() || !value->is_number()) {
                return;
            }
            const double parsed = value->get<double>();
            if (std::isfinite(parsed)) {
                *output = parsed;
            }
        };
        readClock("timelineClock", &next.timelineClock);
        readClock("sourceClock", &next.sourceClock);
        GroupContext& current = groups_[static_cast<std::size_t>(index)];
        if (next.timelineTarget == current.timelineTarget && next.sourceTarget == current.sourceTarget &&
            next.timelineClock == current.timelineClock && next.sourceClock == current.sourceClock) {
            continue;
        }
        current = std::move(next);
        emit groupContextChanged(letter);
        for (const auto& [panelId, binding] : panels_) {
            if (binding.group != letter) {
                continue;
            }
            const auto before = lastContexts_.at(panelId);
            const auto now = contextFor(panelId);
            lastContexts_[panelId] = now;
            if (now != before) {
                emit panelContextChanged(panelId);
            }
        }
    }
    return true;
}

void PanelContextRouter::sessionChanged(void* context) noexcept {
    static_cast<PanelContextRouter*>(context)->documentChanged();
}

void PanelContextRouter::documentChanged() noexcept {
    for (const auto& [panelId, binding] : panels_) {
        const auto before = lastContexts_.at(panelId);
        const auto now = contextFor(panelId);
        lastContexts_[panelId] = now;
        if (now != before)
            emit panelContextChanged(panelId);
    }
}

}  // namespace nemo::ui
