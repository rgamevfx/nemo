#include "ParameterEditorRegistry.hpp"

namespace nemo::ui {
ParameterEditorRegistry::ParameterEditorRegistry(QObject* parent) : QObject(parent) {}

bool ParameterEditorRegistry::registerEditor(const QString& editorId, const QUrl& source) {
    const auto id = editorId.trimmed();
    if (id.isEmpty() || !id.contains(QLatin1Char('.')))
        return false;
    if (source.isEmpty() || !source.isValid())
        return false;
    const auto existing = editors_.find(id);
    if (existing != editors_.end() && existing->second == source)
        return true;
    editors_[id] = source;
    emit editorsChanged();
    return true;
}

bool ParameterEditorRegistry::unregisterEditor(const QString& editorId) {
    if (editors_.erase(editorId.trimmed()) == 0)
        return false;
    emit editorsChanged();
    return true;
}

QVariantMap ParameterEditorRegistry::editor(const QString& editorId) const {
    const auto found = editors_.find(editorId.trimmed());
    if (found == editors_.end())
        return QVariantMap{{QStringLiteral("available"), false},
                           {QStringLiteral("source"), QString{}},
                           {QStringLiteral("reason"), reason(editorId)}};
    return QVariantMap{{QStringLiteral("available"), true},
                       {QStringLiteral("source"), found->second},
                       {QStringLiteral("reason"), QString{}}};
}

QString ParameterEditorRegistry::reason(const QString& editorId) const {
    if (editors_.contains(editorId.trimmed()))
        return {};
    return QStringLiteral("No parameter editor registered for '%1'").arg(editorId);
}
}  // namespace nemo::ui
