#include "WorkspaceController.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

namespace nemo::workspace {

namespace {

constexpr int kPersistenceVersion = 2;
constexpr qint64 kMaxLayoutBytes = 1024 * 1024;

QVariantMap defaultCategoryColors() {
    return {
        {QStringLiteral("Merge"), QStringLiteral("#60656b")},   {QStringLiteral("Filter"), QStringLiteral("#a96832")},
        {QStringLiteral("IO"), QStringLiteral("#386b91")},      {QStringLiteral("Color"), QStringLiteral("#71608c")},
        {QStringLiteral("Distort"), QStringLiteral("#54816b")}, {QStringLiteral("Utility"), QStringLiteral("#59646f")}};
}

nlohmann::json variantMapToJson(const QVariantMap& value) {
    const auto document = QJsonDocument::fromVariant(value);
    return nlohmann::json::parse(document.toJson(QJsonDocument::Compact).constData());
}

QVariantMap jsonObjectToVariantMap(const nlohmann::json& value) {
    if (!value.is_object()) {
        return {};
    }
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(value.dump()));
    return document.object().toVariantMap();
}

}  // namespace

WorkspaceController::WorkspaceController(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {
    categoryColors_ = defaultCategoryColors();
    presets_.push_back({QStringLiteral("workspace-1"), QStringLiteral("Default"), workspace_});
    activeWorkspaceId_ = presets_.front().id;

    QFile file(path_);
    if (!file.exists()) {
        return;
    }
    preserveUnreadableFile_ = true;
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("Cannot read workspace %1: %2").arg(path_, file.errorString()));
        return;
    }
    if (file.size() > kMaxLayoutBytes) {
        setError(QStringLiteral("Workspace %1 exceeds the 1 MiB layout limit.").arg(path_));
        return;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(QStringLiteral("Cannot read workspace %1: %2").arg(path_, file.errorString()));
        return;
    }
    try {
        restoreFromJson(nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size()));
        preserveUnreadableFile_ = false;
    } catch (const std::exception& exception) {
        setError(
            QStringLiteral("Cannot restore workspace %1: %2. The file is preserved; Reset layout allows replacing it.")
                .arg(path_, QString::fromUtf8(exception.what())));
    }
}

QVariantMap WorkspaceController::root() const {
    return jsonObjectToVariantMap(workspace_.toJson().at("root"));
}

QString WorkspaceController::error() const {
    return error_;
}

QVariantList WorkspaceController::panelTypes() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(panelDescriptors_.size()));
    for (const auto& descriptor : panelDescriptors_) {
        result.push_back(QVariantMap{{QStringLiteral("typeId"), descriptor.typeId},
                                     {QStringLiteral("title"), descriptor.title},
                                     {QStringLiteral("source"), descriptor.source},
                                     {QStringLiteral("headerSource"), descriptor.headerSource}});
    }
    return result;
}

QVariantMap WorkspaceController::panelDescriptor(const QString& typeId) const {
    const auto found = std::find_if(panelDescriptors_.begin(), panelDescriptors_.end(),
                                    [&](const PanelDescriptor& descriptor) { return descriptor.typeId == typeId; });
    if (found == panelDescriptors_.end()) {
        return {};
    }
    return {{QStringLiteral("typeId"), found->typeId},
            {QStringLiteral("title"), found->title},
            {QStringLiteral("source"), found->source},
            {QStringLiteral("headerSource"), found->headerSource}};
}

QVariantList WorkspaceController::workspaces() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(presets_.size()));
    for (const auto& preset : presets_) {
        result.push_back(QVariantMap{{QStringLiteral("id"), preset.id}, {QStringLiteral("name"), preset.name}});
    }
    return result;
}

QString WorkspaceController::activeWorkspaceId() const {
    return activeWorkspaceId_;
}

QString WorkspaceController::appearancePreset() const {
    return appearancePreset_;
}

QString WorkspaceController::accentOverride() const {
    return accentOverride_;
}

QVariantMap WorkspaceController::categoryColors() const {
    return categoryColors_;
}

void WorkspaceController::setError(QString message) {
    if (error_ != message) {
        error_ = std::move(message);
        emit errorChanged();
    }
}

void WorkspaceController::change(const std::function<void()>& operation, bool notify) {
    try {
        operation();
        if (!preserveUnreadableFile_) {
            setError({});
        }
        if (notify) {
            emit rootChanged();
        }
    } catch (const std::exception& exception) {
        setError(QString::fromUtf8(exception.what()));
    }
}

void WorkspaceController::registerPanelType(const QString& typeId, const QString& title, const QString& qmlSource,
                                            const QString& headerSource) {
    const auto id = typeId.trimmed();
    if (id.isEmpty() || title.trimmed().isEmpty() || qmlSource.trimmed().isEmpty()) {
        setError(QStringLiteral("registerPanelType: typeId, title and source must not be empty"));
        return;
    }
    if (std::any_of(panelDescriptors_.begin(), panelDescriptors_.end(),
                    [&](const PanelDescriptor& descriptor) { return descriptor.typeId == id; })) {
        setError(QStringLiteral("registerPanelType: type '%1' is already registered").arg(id));
        return;
    }
    panelDescriptors_.push_back({id, title.trimmed(), qmlSource.trimmed(), headerSource.trimmed()});
    setError({});
    emit panelTypesChanged();
}

QString WorkspaceController::createPanel(const QString& leafId, const QString& typeId, const QString& group) {
    try {
        if (panelDescriptor(typeId).isEmpty()) {
            throw std::runtime_error("createPanel: unregistered panel type '" + typeId.toStdString() + "'");
        }
        const auto id = workspace_.createPanel(leafId.toStdString(), typeId.toStdString(), group.toStdString());
        if (!preserveUnreadableFile_) {
            setError({});
        }
        emit rootChanged();
        return QString::fromStdString(id);
    } catch (const std::exception& exception) {
        setError(QString::fromUtf8(exception.what()));
        return {};
    }
}

QVariantMap WorkspaceController::panelState(const QString& panelId) const {
    try {
        return jsonObjectToVariantMap(workspace_.panelState(panelId.toStdString()));
    } catch (const std::exception&) {
        return {};
    }
}

void WorkspaceController::setPanelState(const QString& panelId, const QVariantMap& state) {
    try {
        workspace_.setPanelState(panelId.toStdString(), variantMapToJson(state));
        if (!preserveUnreadableFile_) {
            setError({});
        }
        emit rootChanged();
    } catch (const std::exception& exception) {
        setError(QString::fromUtf8(exception.what()));
    }
}

void WorkspaceController::split(const QString& leafId, const QString& orientation) {
    change([&] { workspace_.split(leafId.toStdString(), orientation.toStdString()); });
}

void WorkspaceController::setRatio(const QString& splitId, double ratio) {
    change([&] { workspace_.setRatio(splitId.toStdString(), ratio); }, false);
}

void WorkspaceController::setPanelType(const QString& panelId, const QString& type) {
    change([&] {
        if (panelDescriptor(type).isEmpty()) {
            throw std::runtime_error("setPanelType: unregistered panel type '" + type.toStdString() + "'");
        }
        workspace_.setPanelType(panelId.toStdString(), type.toStdString());
    });
}

void WorkspaceController::setGroup(const QString& panelId, const QString& group) {
    change([&] { workspace_.setGroup(panelId.toStdString(), group.toStdString()); });
}

void WorkspaceController::activate(const QString& leafId, const QString& panelId) {
    change([&] { workspace_.activate(leafId.toStdString(), panelId.toStdString()); });
}

void WorkspaceController::addTab(const QString& leafId, const QString& type) {
    createPanel(leafId, type, QStringLiteral("A"));
}

void WorkspaceController::closePanel(const QString& panelId) {
    change([&] { workspace_.closePanel(panelId.toStdString()); });
}

bool WorkspaceController::movePanel(const QString& panelId, const QString& leafId, const QString& placement,
                                    int tabIndex) {
    try {
        Placement position;
        if (placement == QStringLiteral("tabs")) {
            position = Placement::Tabs;
        } else if (placement == QStringLiteral("left")) {
            position = Placement::Left;
        } else if (placement == QStringLiteral("right")) {
            position = Placement::Right;
        } else if (placement == QStringLiteral("top")) {
            position = Placement::Top;
        } else if (placement == QStringLiteral("bottom")) {
            position = Placement::Bottom;
        } else {
            throw std::runtime_error("movePanel: unknown drop placement '" + placement.toStdString() + "'");
        }
        if (position == Placement::Tabs && tabIndex < 0) {
            throw std::runtime_error("movePanel: tab insertion index must not be negative");
        }
        const bool changed = workspace_.movePanel(
            panelId.toStdString(),
            {leafId.toStdString(), position, position == Placement::Tabs ? static_cast<std::size_t>(tabIndex) : 0});
        if (!preserveUnreadableFile_) {
            setError({});
        }
        if (changed) {
            emit rootChanged();
        }
        return true;
    } catch (const std::exception& exception) {
        setError(QString::fromUtf8(exception.what()));
        return false;
    }
}

QString WorkspaceController::normalizedName(const QString& value) {
    return value.trimmed();
}

int WorkspaceController::workspaceIndex(const QString& id) const {
    for (std::size_t i = 0; i < presets_.size(); ++i) {
        if (presets_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QString WorkspaceController::nextWorkspaceId() const {
    int suffix = 1;
    QString candidate;
    do {
        candidate = QStringLiteral("workspace-%1").arg(suffix++);
    } while (workspaceIndex(candidate) >= 0);
    return candidate;
}

void WorkspaceController::snapshotActiveWorkspace() {
    const int index = workspaceIndex(activeWorkspaceId_);
    if (index >= 0) {
        presets_[static_cast<std::size_t>(index)].workspace = workspace_;
    }
}

QString WorkspaceController::createWorkspace(const QString& name) {
    const auto normalized = normalizedName(name);
    if (normalized.isEmpty()) {
        setError(QStringLiteral("createWorkspace: name must not be empty"));
        return {};
    }
    snapshotActiveWorkspace();
    const QString id = nextWorkspaceId();
    presets_.push_back({id, normalized, Workspace{}});
    emit workspacesChanged();
    return id;
}

bool WorkspaceController::renameWorkspace(const QString& id, const QString& name) {
    const auto normalized = normalizedName(name);
    if (normalized.isEmpty()) {
        setError(QStringLiteral("renameWorkspace: name must not be empty"));
        return false;
    }
    const int index = workspaceIndex(id);
    if (index < 0) {
        setError(QStringLiteral("renameWorkspace: unknown workspace '%1'").arg(id));
        return false;
    }
    presets_[static_cast<std::size_t>(index)].name = normalized;
    setError({});
    emit workspacesChanged();
    return true;
}

QString WorkspaceController::duplicateWorkspace(const QString& id, const QString& name) {
    const auto normalized = normalizedName(name);
    if (normalized.isEmpty()) {
        setError(QStringLiteral("duplicateWorkspace: name must not be empty"));
        return {};
    }
    const int index = workspaceIndex(id);
    if (index < 0) {
        setError(QStringLiteral("duplicateWorkspace: unknown workspace '%1'").arg(id));
        return {};
    }
    snapshotActiveWorkspace();
    const QString newId = nextWorkspaceId();
    presets_.push_back(
        {newId, normalized, presets_[static_cast<std::size_t>(index)].workspace.duplicateWithFreshIds()});
    emit workspacesChanged();
    return newId;
}

bool WorkspaceController::closeWorkspace(const QString& id) {
    const int index = workspaceIndex(id);
    if (index < 0) {
        setError(QStringLiteral("closeWorkspace: unknown workspace '%1'").arg(id));
        return false;
    }
    if (presets_.size() <= 1) {
        setError(QStringLiteral("closeWorkspace: cannot close the last workspace"));
        return false;
    }
    snapshotActiveWorkspace();
    const bool closingActive = id == activeWorkspaceId_;
    presets_.erase(presets_.begin() + index);
    if (closingActive) {
        const int replacement = std::min(index, static_cast<int>(presets_.size()) - 1);
        activeWorkspaceId_ = presets_[static_cast<std::size_t>(replacement)].id;
        workspace_ = presets_[static_cast<std::size_t>(replacement)].workspace;
        emit activeWorkspaceIdChanged();
        emit rootChanged();
    }
    setError({});
    emit workspacesChanged();
    return true;
}

bool WorkspaceController::switchWorkspace(const QString& id) {
    const int index = workspaceIndex(id);
    if (index < 0) {
        setError(QStringLiteral("switchWorkspace: unknown workspace '%1'").arg(id));
        return false;
    }
    if (id == activeWorkspaceId_) {
        return true;
    }
    Workspace target = presets_[static_cast<std::size_t>(index)].workspace;
    snapshotActiveWorkspace();
    workspace_ = std::move(target);
    activeWorkspaceId_ = id;
    setError({});
    emit activeWorkspaceIdChanged();
    emit rootChanged();
    return true;
}

bool WorkspaceController::validPreset(const QString& value) {
    return value == QStringLiteral("Graphite") || value == QStringLiteral("Slate") || value == QStringLiteral("Paper");
}

bool WorkspaceController::validColor(const QString& value) {
    static const QRegularExpression expression(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return expression.match(value).hasMatch();
}

bool WorkspaceController::setAppearancePreset(const QString& preset) {
    if (!validPreset(preset)) {
        setError(QStringLiteral("setAppearancePreset: invalid preset '%1'").arg(preset));
        return false;
    }
    appearancePreset_ = preset;
    setError({});
    emit appearanceChanged();
    return true;
}

bool WorkspaceController::setAccentOverride(const QString& color) {
    if (!color.isEmpty() && !validColor(color)) {
        setError(QStringLiteral("setAccentOverride: expected #RRGGBB or empty"));
        return false;
    }
    accentOverride_ = color;
    setError({});
    emit appearanceChanged();
    return true;
}

bool WorkspaceController::setCategoryColor(const QString& category, const QString& color) {
    if (!categoryColors_.contains(category)) {
        setError(QStringLiteral("setCategoryColor: unknown category '%1'").arg(category));
        return false;
    }
    if (!validColor(color)) {
        setError(QStringLiteral("setCategoryColor: expected #RRGGBB"));
        return false;
    }
    categoryColors_.insert(category, color);
    setError({});
    emit appearanceChanged();
    return true;
}

void WorkspaceController::resetAppearance() {
    appearancePreset_ = QStringLiteral("Graphite");
    accentOverride_.clear();
    categoryColors_ = defaultCategoryColors();
    setError({});
    emit appearanceChanged();
}

nlohmann::json WorkspaceController::persistenceJson() const {
    nlohmann::json workspaces = nlohmann::json::array();
    for (const auto& preset : presets_) {
        workspaces.push_back({{"id", preset.id.toStdString()},
                              {"name", preset.name.toStdString()},
                              {"layout", preset.workspace.toJson()}});
    }
    nlohmann::json colors = nlohmann::json::object();
    for (auto it = categoryColors_.cbegin(); it != categoryColors_.cend(); ++it) {
        colors[it.key().toStdString()] = it.value().toString().toStdString();
    }
    return {{"version", kPersistenceVersion},
            {"activeWorkspaceId", activeWorkspaceId_.toStdString()},
            {"workspaces", workspaces},
            {"appearance",
             {{"preset", appearancePreset_.toStdString()},
              {"accentOverride", accentOverride_.toStdString()},
              {"categoryColors", colors}}}};
}

void WorkspaceController::restoreFromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("version") || !json.at("version").is_number_integer()) {
        throw std::runtime_error("workspace: persistence root must contain an integer 'version'");
    }

    std::vector<Preset> nextPresets;
    QString nextActive;
    QString nextPreset = QStringLiteral("Graphite");
    QString nextAccent;
    QVariantMap nextColors = defaultCategoryColors();
    const int version = json.at("version").get<int>();
    if (version == Workspace::kVersion) {
        nextPresets.push_back({QStringLiteral("workspace-1"), QStringLiteral("Default"), Workspace::fromJson(json)});
        nextActive = nextPresets.front().id;
    } else if (version == kPersistenceVersion) {
        if (!json.contains("workspaces") || !json.at("workspaces").is_array() || json.at("workspaces").empty()) {
            throw std::runtime_error("workspace: persistence must contain at least one workspace");
        }
        std::set<std::string> ids;
        for (const auto& entry : json.at("workspaces")) {
            if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string() || !entry.contains("name") ||
                !entry.at("name").is_string()) {
                throw std::runtime_error("workspace: preset requires string id and name");
            }
            const auto id = entry.at("id").get<std::string>();
            const auto name = entry.at("name").get<std::string>();
            if (id.empty() || name.empty()) {
                throw std::runtime_error("workspace: preset id and name must not be empty");
            }
            if (!ids.insert(id).second) {
                throw std::runtime_error("workspace: duplicate preset id '" + id + "'");
            }
            const auto& layout = entry.contains("layout") ? entry.at("layout") : entry;
            nextPresets.push_back(
                {QString::fromStdString(id), QString::fromStdString(name), Workspace::fromJson(layout)});
        }
        if (!json.contains("activeWorkspaceId") || !json.at("activeWorkspaceId").is_string()) {
            throw std::runtime_error("workspace: persistence has no string 'activeWorkspaceId'");
        }
        nextActive = QString::fromStdString(json.at("activeWorkspaceId").get<std::string>());
        if (std::none_of(nextPresets.begin(), nextPresets.end(),
                         [&](const Preset& preset) { return preset.id == nextActive; })) {
            throw std::runtime_error("workspace: activeWorkspaceId does not identify a workspace");
        }
        if (json.contains("appearance")) {
            const auto& appearance = json.at("appearance");
            if (!appearance.is_object()) {
                throw std::runtime_error("workspace: appearance must be an object");
            }
            if (appearance.contains("preset")) {
                if (!appearance.at("preset").is_string() ||
                    !validPreset(QString::fromStdString(appearance.at("preset").get<std::string>()))) {
                    throw std::runtime_error("workspace: appearance has invalid preset");
                }
                nextPreset = QString::fromStdString(appearance.at("preset").get<std::string>());
            }
            if (appearance.contains("accentOverride")) {
                if (!appearance.at("accentOverride").is_string()) {
                    throw std::runtime_error("workspace: appearance accentOverride must be a string");
                }
                nextAccent = QString::fromStdString(appearance.at("accentOverride").get<std::string>());
                if (!nextAccent.isEmpty() && !validColor(nextAccent)) {
                    throw std::runtime_error("workspace: appearance accentOverride is not #RRGGBB");
                }
            }
            if (appearance.contains("categoryColors")) {
                if (!appearance.at("categoryColors").is_object()) {
                    throw std::runtime_error("workspace: appearance categoryColors must be an object");
                }
                for (const auto& [category, value] : appearance.at("categoryColors").items()) {
                    const QString key = QString::fromStdString(category);
                    if (!nextColors.contains(key) || !value.is_string() ||
                        !validColor(QString::fromStdString(value.get<std::string>()))) {
                        throw std::runtime_error("workspace: appearance has invalid category color '" + category + "'");
                    }
                    nextColors.insert(key, QString::fromStdString(value.get<std::string>()));
                }
            }
        }
    } else {
        throw std::runtime_error("workspace: unsupported persistence version " + std::to_string(version));
    }

    auto activeIndex = std::find_if(nextPresets.begin(), nextPresets.end(),
                                    [&](const Preset& preset) { return preset.id == nextActive; });
    workspace_ = activeIndex->workspace;
    presets_ = std::move(nextPresets);
    activeWorkspaceId_ = nextActive;
    appearancePreset_ = nextPreset;
    accentOverride_ = nextAccent;
    categoryColors_ = std::move(nextColors);
}

bool WorkspaceController::save() {
    if (preserveUnreadableFile_) {
        setError(
            QStringLiteral("Workspace %1 was not overwritten because restore failed. Use Reset layout to replace it.")
                .arg(path_));
        return false;
    }
    snapshotActiveWorkspace();
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        setError(QStringLiteral("Cannot create workspace directory for %1.").arg(path_));
        return false;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("Cannot save workspace %1: %2").arg(path_, file.errorString()));
        return false;
    }
    const auto data = persistenceJson().dump(2);
    if (file.write(data.data(), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size()) ||
        !file.commit()) {
        setError(QStringLiteral("Cannot save workspace %1: %2").arg(path_, file.errorString()));
        return false;
    }
    setError({});
    return true;
}

void WorkspaceController::reset() {
    workspace_ = Workspace{};
    presets_.clear();
    presets_.push_back({QStringLiteral("workspace-1"), QStringLiteral("Default"), workspace_});
    activeWorkspaceId_ = presets_.front().id;
    resetAppearance();
    preserveUnreadableFile_ = false;
    setError({});
    emit workspacesChanged();
    emit activeWorkspaceIdChanged();
    emit rootChanged();
}

}  // namespace nemo::workspace
