#include "WorkspaceController.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <exception>
#include <stdexcept>
#include <utility>

namespace nemo::workspace {

WorkspaceController::WorkspaceController(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {
    QFile file(path_);
    if (!file.exists()) {
        return;
    }
    preserveUnreadableFile_ = true;
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("Cannot read workspace %1: %2").arg(path_, file.errorString()));
        return;
    }
    constexpr qint64 maxLayoutBytes = 1024 * 1024;
    if (file.size() > maxLayoutBytes) {
        setError(QStringLiteral("Workspace %1 exceeds the 1 MiB layout limit.").arg(path_));
        return;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(QStringLiteral("Cannot read workspace %1: %2").arg(path_, file.errorString()));
        return;
    }
    try {
        workspace_ = Workspace::fromJson(nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size()));
        preserveUnreadableFile_ = false;
    } catch (const std::exception& exception) {
        setError(
            QStringLiteral("Cannot restore workspace %1: %2. The file is preserved; Reset layout allows replacing it.")
                .arg(path_, QString::fromUtf8(exception.what())));
    }
}

QVariantMap WorkspaceController::root() const {
    const auto json = workspace_.toJson().at("root").dump();
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().toVariantMap();
}

QString WorkspaceController::error() const {
    return error_;
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

void WorkspaceController::split(const QString& leafId, const QString& orientation) {
    change([&] { workspace_.split(leafId.toStdString(), orientation.toStdString()); });
}

void WorkspaceController::setRatio(const QString& splitId, double ratio) {
    change([&] { workspace_.setRatio(splitId.toStdString(), ratio); }, false);
}

void WorkspaceController::setPanelType(const QString& panelId, const QString& type) {
    change([&] { workspace_.setPanelType(panelId.toStdString(), type.toStdString()); });
}

void WorkspaceController::setGroup(const QString& panelId, const QString& group) {
    change([&] { workspace_.setGroup(panelId.toStdString(), group.toStdString()); });
}

void WorkspaceController::activate(const QString& leafId, const QString& panelId) {
    change([&] { workspace_.activate(leafId.toStdString(), panelId.toStdString()); });
}

void WorkspaceController::addTab(const QString& leafId, const QString& type) {
    change([&] { workspace_.addTab(leafId.toStdString(), type.toStdString()); });
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

bool WorkspaceController::save() {
    if (preserveUnreadableFile_) {
        setError(
            QStringLiteral("Workspace %1 was not overwritten because restore failed. Use Reset layout to replace it.")
                .arg(path_));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        setError(QStringLiteral("Cannot create workspace directory for %1.").arg(path_));
        return false;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("Cannot save workspace %1: %2").arg(path_, file.errorString()));
        return false;
    }
    const auto data = workspace_.toJson().dump(2);
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
    preserveUnreadableFile_ = false;
    setError({});
    emit rootChanged();
}

}  // namespace nemo::workspace
