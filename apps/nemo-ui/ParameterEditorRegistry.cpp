#include "ParameterEditorRegistry.hpp"

namespace nemo::ui {
ParameterEditorRegistry::ParameterEditorRegistry(QObject* parent) : QObject(parent) {}

bool ParameterEditorRegistry::registerEditor(const QString& editorId, const QUrl& source, const QStringList& consumes,
                                             const QString& presentation) {
    const auto id = editorId.trimmed();
    if (id.isEmpty() || !id.contains(QLatin1Char('.')))
        return false;
    if (source.isEmpty() || !source.isValid())
        return false;
    const auto existing = editors_.find(id);
    const auto declared = presentation.trimmed();
    if (!declared.isEmpty() && declared != QStringLiteral("row") && declared != QStringLiteral("section")) {
        refusals_[id] =
            QStringLiteral("Parameter editor '%1' declares an unsupported presentation '%2'").arg(id, declared);
        return false;
    }
    const auto layout = declared.isEmpty() ? QStringLiteral("row") : declared;
    if (existing != editors_.end() && existing->second == source && consumes_.at(id) == consumes &&
        presentations_.at(id) == layout) {
        return true;
    }
    refusals_.erase(id);
    editors_[id] = source;
    consumes_[id] = consumes;
    presentations_[id] = layout;
    emit editorsChanged();
    return true;
}

bool ParameterEditorRegistry::unregisterEditor(const QString& editorId) {
    const auto id = editorId.trimmed();
    if (editors_.erase(id) == 0)
        return false;
    consumes_.erase(id);
    presentations_.erase(id);
    emit editorsChanged();
    return true;
}

QVariantMap ParameterEditorRegistry::editor(const QString& editorId) const {
    const auto found = editors_.find(editorId.trimmed());
    if (found == editors_.end()) {
        const auto refusal = refusals_.find(editorId.trimmed());
        return QVariantMap{{QStringLiteral("available"), false},
                           {QStringLiteral("source"), QString{}},
                           {QStringLiteral("consumes"), QStringList{}},
                           {QStringLiteral("presentation"), QStringLiteral("row")},
                           {QStringLiteral("reason"), refusal == refusals_.end() ? reason(editorId) : refusal->second}};
    }
    const auto declared = consumes_.find(found->first);
    const auto layout = presentations_.find(found->first);
    return QVariantMap{
        {QStringLiteral("available"), true},
        {QStringLiteral("source"), found->second},
        {QStringLiteral("consumes"), declared == consumes_.end() ? QStringList{} : declared->second},
        {QStringLiteral("presentation"), layout == presentations_.end() ? QStringLiteral("row") : layout->second},
        {QStringLiteral("reason"), QString{}}};
}

QString ParameterEditorRegistry::reason(const QString& editorId) const {
    if (editors_.contains(editorId.trimmed()))
        return {};
    return QStringLiteral("No parameter editor registered for '%1'").arg(editorId);
}
}  // namespace nemo::ui
