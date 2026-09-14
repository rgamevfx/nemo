#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
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
    // an id again replaces its source and its declared parameter set. `consumes`
    // declares the node parameter keys this one editor owns; the inspector then
    // omits their generic rows so exactly one control renders each setting. An
    // unavailable editor consumes nothing and the generic rows stay usable.
    // `presentation` is the host layout the editor needs: "row" (the default
    // when omitted) renders the editor beside the ordinary label/key cells,
    // while "section" renders it full width with no outer label/key wrapper, for
    // an aggregate editor that presents several consumed parameters itself. An
    // explicitly unsupported presentation is refused rather than silently
    // treated as "row", and `editor(id)` then reports why; the error is cleared
    // by a successful registration.
    Q_INVOKABLE bool registerEditor(const QString& editorId, const QUrl& source,
                                    const QStringList& consumes = QStringList{},
                                    const QString& presentation = QStringLiteral("row"));
    Q_INVOKABLE bool unregisterEditor(const QString& editorId);
    // {available, source, consumes, presentation, reason}; a missing id reports
    // the exact reason.
    Q_INVOKABLE QVariantMap editor(const QString& editorId) const;
    Q_INVOKABLE QString reason(const QString& editorId) const;

signals:
    void editorsChanged();

private:
    std::map<QString, QUrl> editors_;
    std::map<QString, QStringList> consumes_;
    std::map<QString, QString> presentations_;
    // Reason a registration id was refused (unsupported presentation), reported
    // by editor() so a misconfigured contribution is not masked.
    std::map<QString, QString> refusals_;
};
}  // namespace nemo::ui
