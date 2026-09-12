#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <map>

namespace nemo::ui {
// Host registry for namespaced custom parameter editors. It owns presentation
// metadata only: QML resolves the registered source and instantiates the
// editor; no parameter or document model lives here.
class ParameterEditorRegistry final : public QObject {
    Q_OBJECT
public:
    explicit ParameterEditorRegistry(QObject* parent = nullptr);
    // Rejects an empty/unnamespaced id or an invalid/empty source. Registering
    // an id again replaces its source.
    Q_INVOKABLE bool registerEditor(const QString& editorId, const QUrl& source);
    Q_INVOKABLE bool unregisterEditor(const QString& editorId);
    // {available, source, reason}; a missing id reports the exact reason.
    Q_INVOKABLE QVariantMap editor(const QString& editorId) const;
    Q_INVOKABLE QString reason(const QString& editorId) const;

signals:
    void editorsChanged();

private:
    std::map<QString, QUrl> editors_;
};
}  // namespace nemo::ui
