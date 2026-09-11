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
constexpr std::array<QStringView, 3> kModes{QStringView(u"follow"), QStringView(u"group"), QStringView(u"pinned")};
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

bool PanelContextRouter::validMode(const QString& mode) noexcept {
    return std::any_of(kModes.begin(), kModes.end(), [&](QStringView value) { return mode == value; });
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
    state.insert(QStringLiteral("linkMode"), binding.mode);
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

bool PanelContextRouter::registerPanel(const QString& rawPanelId, const QString& rawGroup, const QString& rawMode) {
    const auto panelId = normalized(rawPanelId);
    const auto group = normalized(rawGroup);
    const auto mode = normalized(rawMode);
    if (panelId.isEmpty() || groupIndex(group) < 0 || !validMode(mode))
        return false;

    auto found = panels_.find(panelId);
    if (found != panels_.end()) {
        auto& binding = found->second;
        binding.group = group;
        if (binding.mode != mode) {
            if (mode == QStringLiteral("pinned")) {
                binding.pinned = groups_[groupIndex(group)];
                binding.pinnedGroup = group;
                binding.hasPinned = true;
            } else {
                binding.pinnedGroup.clear();
                binding.hasPinned = false;
            }
            binding.mode = mode;
        }
        lastContexts_[panelId] = contextFor(panelId);
        return true;
    }

    PanelBinding binding;
    binding.group = group;
    binding.mode = mode;
    binding.role = QStringLiteral("graph");
    if (workspace_) {
        const auto state = workspace_->panelState(panelId);
        const auto savedMode = state.value(QStringLiteral("linkMode")).toString();
        const auto savedRole = state.value(QStringLiteral("viewerRole")).toString();
        if (validMode(savedMode))
            binding.mode = savedMode;
        if (validRole(savedRole))
            binding.role = savedRole;
    }
    if (binding.mode == QStringLiteral("pinned")) {
        binding.pinned = groups_[groupIndex(group)];
        binding.pinnedGroup = group;
        binding.hasPinned = true;
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
    for (auto& [id, context] : lastContexts_) {
        const auto before = context;
        const auto now = contextFor(id);
        context = now;
        if (now != before)
            emit panelContextChanged(id);
    }
    emit activePanelChanged(activePanel_);
    return true;
}

bool PanelContextRouter::setLinkMode(const QString& panelId, const QString& rawMode) {
    const auto mode = normalized(rawMode);
    auto found = panels_.find(panelId);
    if (found == panels_.end() || !validMode(mode))
        return false;
    auto& binding = found->second;
    if (binding.mode == mode)
        return true;
    if (mode == QStringLiteral("pinned")) {
        QString resolvedGroupName;
        const auto* resolved = resolvedGroup(panelId, &resolvedGroupName);
        if (!resolved) {
            const auto index = groupIndex(binding.group);
            if (index < 0)
                return false;
            resolved = &groups_[index];
            resolvedGroupName = binding.group;
        }
        binding.pinned = *resolved;
        binding.pinnedGroup = resolvedGroupName;
        binding.hasPinned = true;
    } else {
        binding.hasPinned = false;
        binding.pinnedGroup.clear();
    }
    binding.mode = mode;
    persist(panelId, binding);
    lastContexts_[panelId] = contextFor(panelId);
    emit panelBindingChanged(panelId);
    emit panelContextChanged(panelId);
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
    for (const auto& [id, candidate] : panels_) {
        if (candidate.mode == QStringLiteral("pinned"))
            continue;
        const auto before = lastContexts_.at(id);
        const auto now = contextFor(id);
        lastContexts_[id] = now;
        QString resolved;
        if (resolvedGroup(id, &resolved) && resolved == group && now != before)
            emit panelContextChanged(id);
    }
    emit panelBindingChanged(panelId);
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

const PanelContextRouter::GroupContext* PanelContextRouter::resolvedGroup(const QString& panelId,
                                                                          QString* groupName) const {
    const auto found = panels_.find(panelId);
    if (found == panels_.end())
        return nullptr;
    const auto& binding = found->second;
    if (binding.mode == QStringLiteral("pinned")) {
        if (!binding.hasPinned)
            return nullptr;
        if (groupName)
            *groupName = binding.pinnedGroup;
        return &binding.pinned;
    }
    QString selected = binding.group;
    if (binding.mode == QStringLiteral("follow")) {
        const auto active = panels_.find(activePanel_);
        if (active == panels_.end())
            return nullptr;
        selected = active->second.group;
    }
    const auto index = groupIndex(selected);
    if (index < 0)
        return nullptr;
    if (groupName)
        *groupName = selected;
    return &groups_[index];
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
    // Graph/timeline targets are presentation identities. Known document
    // identities receive strict deletion checks; opaque plugin IDs remain
    // valid until their owner changes the group selection.
    if (kind == QStringLiteral("graph") && target.startsWith(QStringLiteral("network:"))) {
        std::uint64_t id = 0;
        if (!parseUnsigned(target.mid(8), &id))
            return false;
        return std::any_of(session_.document().networks().begin(), session_.document().networks().end(),
                           [id](const auto& network) { return network.id() == static_cast<NetworkId>(id); });
    }
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

QVariantMap PanelContextRouter::groupMap(const QString& panelId, const QString& mode, const QString& group,
                                         const GroupContext& context) const {
    const auto& role = panels_.at(panelId).role;
    QVariantMap result{{QStringLiteral("panelId"), panelId},
                       {QStringLiteral("mode"), mode},
                       {QStringLiteral("group"), panels_.at(panelId).group},
                       {QStringLiteral("resolvedGroup"), group},
                       {QStringLiteral("viewerRole"), role},
                       {QStringLiteral("graphTarget"), context.graphTarget},
                       {QStringLiteral("timelineTarget"), context.timelineTarget},
                       {QStringLiteral("sourceTarget"), context.sourceTarget},
                       {QStringLiteral("graphClock"), context.graphClock},
                       {QStringLiteral("timelineClock"), context.timelineClock},
                       {QStringLiteral("sourceClock"), context.sourceClock}};
    result.insert(QStringLiteral("sourceMarks"), sourceMarks(context.sourceTarget));
    const QString kind = role == QStringLiteral("media") ? QStringLiteral("source") : role;
    const QString target = role == QStringLiteral("graph")      ? context.graphTarget
                           : role == QStringLiteral("timeline") ? context.timelineTarget
                                                                : context.sourceTarget;
    const bool available = targetAvailable(kind, target);
    result.insert(QStringLiteral("available"), available);
    result.insert(QStringLiteral("unavailableReason"), available ? QString{} : unavailableReason(kind));
    return result;
}

QVariantMap PanelContextRouter::contextForBinding(const QString& panelId, const PanelBinding& binding) const {
    QString group;
    const auto* context = resolvedGroup(panelId, &group);
    if (!context) {
        return {{QStringLiteral("panelId"), panelId},
                {QStringLiteral("mode"), binding.mode},
                {QStringLiteral("group"), binding.group},
                {QStringLiteral("resolvedGroup"), QString{}},
                {QStringLiteral("viewerRole"), binding.role},
                {QStringLiteral("available"), false},
                {QStringLiteral("unavailableReason"), binding.mode == QStringLiteral("follow")
                                                          ? QStringLiteral("no active panel")
                                                          : QStringLiteral("pinned snapshot unavailable")}};
    }
    return groupMap(panelId, binding.mode, group, *context);
}

QVariantMap PanelContextRouter::contextFor(const QString& panelId) const {
    const auto found = panels_.find(panelId);
    if (found == panels_.end())
        return {{QStringLiteral("panelId"), panelId},
                {QStringLiteral("available"), false},
                {QStringLiteral("unavailableReason"), QStringLiteral("unknown panel")}};
    return contextForBinding(panelId, found->second);
}

QVariantList PanelContextRouter::availableTargets(const QString& panelId) const {
    QVariantList result;
    const auto context = contextFor(panelId);
    const auto add = [&](const QString& kind, const QString& target, const QString& label) {
        if (target.isEmpty())
            return;
        result.push_back(QVariantMap{{QStringLiteral("kind"), kind},
                                     {QStringLiteral("id"), target},
                                     {QStringLiteral("label"), label.isEmpty() ? target : label},
                                     {QStringLiteral("available"), targetAvailable(kind, target)}});
    };
    add(QStringLiteral("graph"), context.value(QStringLiteral("graphTarget")).toString(),
        QStringLiteral("Current graph"));
    add(QStringLiteral("timeline"), context.value(QStringLiteral("timelineTarget")).toString(),
        QStringLiteral("Current timeline"));
    for (const auto& entry : session_.document().mediaCatalog.entries()) {
        const auto id = QString::fromStdString(entry.sourceKey);
        add(QStringLiteral("source"), id, QString::fromStdString(entry.metadata.userName).trimmed());
    }
    return result;
}

bool PanelContextRouter::setGroupContext(const QString& rawGroup, const QVariantMap& changes) {
    const auto group = normalized(rawGroup);
    const auto index = groupIndex(group);
    if (index < 0)
        return false;
    GroupContext next = groups_[index];
    const auto setTarget = [&](const QString& key, QString* output) {
        if (!changes.contains(key))
            return true;
        if (!changes.value(key).canConvert<QString>())
            return false;
        *output = normalized(changes.value(key).toString());
        return true;
    };
    const auto setClock = [&](const QString& key, double* output) {
        if (!changes.contains(key))
            return true;
        bool ok = false;
        const auto value = changes.value(key).toDouble(&ok);
        if (!ok || !std::isfinite(value))
            return false;
        *output = value;
        return true;
    };
    if (!setTarget(QStringLiteral("graphTarget"), &next.graphTarget) ||
        !setTarget(QStringLiteral("timelineTarget"), &next.timelineTarget) ||
        !setTarget(QStringLiteral("sourceTarget"), &next.sourceTarget) ||
        !setClock(QStringLiteral("graphClock"), &next.graphClock) ||
        !setClock(QStringLiteral("timelineClock"), &next.timelineClock) ||
        !setClock(QStringLiteral("sourceClock"), &next.sourceClock))
        return false;
    if (next.graphTarget == groups_[index].graphTarget && next.timelineTarget == groups_[index].timelineTarget &&
        next.sourceTarget == groups_[index].sourceTarget && next.graphClock == groups_[index].graphClock &&
        next.timelineClock == groups_[index].timelineClock && next.sourceClock == groups_[index].sourceClock)
        return true;
    groups_[index] = std::move(next);
    emit groupContextChanged(group);
    for (const auto& [panelId, binding] : panels_) {
        const auto before = lastContexts_.at(panelId);
        const auto now = contextFor(panelId);
        lastContexts_[panelId] = now;
        QString resolved;
        if (binding.mode != QStringLiteral("pinned") && resolvedGroup(panelId, &resolved) && resolved == group &&
            now != before)
            emit panelContextChanged(panelId);
    }
    return true;
}

void PanelContextRouter::sessionChanged(void* context) noexcept {
    static_cast<PanelContextRouter*>(context)->documentChanged();
}

void PanelContextRouter::documentChanged() noexcept {
    for (const auto& [panelId, binding] : panels_) {
        const auto before = lastContexts_.at(panelId);
        const auto now = contextForBinding(panelId, binding);
        lastContexts_[panelId] = now;
        if (now != before)
            emit panelContextChanged(panelId);
    }
}

}  // namespace nemo::ui
