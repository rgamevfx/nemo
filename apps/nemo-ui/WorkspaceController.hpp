#pragma once

#include "Workspace.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace nemo::workspace {

// Presentation-only state. All access is confined to the GUI thread.
class WorkspaceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap root READ root NOTIFY rootChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit WorkspaceController(QString path, QObject* parent = nullptr);
    [[nodiscard]] QVariantMap root() const;
    [[nodiscard]] QString error() const;

    Q_INVOKABLE void split(const QString& leafId, const QString& orientation);
    Q_INVOKABLE void setRatio(const QString& splitId, double ratio);
    Q_INVOKABLE void setPanelType(const QString& panelId, const QString& type);
    Q_INVOKABLE void setGroup(const QString& panelId, const QString& group);
    Q_INVOKABLE void activate(const QString& leafId, const QString& panelId);
    Q_INVOKABLE void addTab(const QString& leafId, const QString& type);
    Q_INVOKABLE void closePanel(const QString& panelId);
    Q_INVOKABLE bool movePanel(const QString& panelId, const QString& leafId, const QString& placement, int tabIndex);
    Q_INVOKABLE bool save();
    Q_INVOKABLE void reset();

signals:
    void rootChanged();
    void errorChanged();

private:
    void change(const std::function<void()>& operation, bool notify = true);
    void setError(QString message);

    Workspace workspace_;
    QString path_;
    QString error_;
    bool preserveUnreadableFile_ = false;
};

}  // namespace nemo::workspace
