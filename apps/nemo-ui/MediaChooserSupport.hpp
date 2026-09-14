#pragma once

// Shared media-file chooser support for the panel adapters that browse for
// media (the Media Bin import/relink workflow, issue #43, and the Read node's
// node-local file control, issue #61). The dialog itself belongs to
// NativeFileChooser; this header only carries the one filter list and the
// local-path translation both requesters must agree on, so the application
// never grows a second media filter table.

#include "NativeFileChooser.hpp"

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <vector>

namespace nemo::ui {

[[nodiscard]] inline const std::vector<NativeFileChooser::Filter>& mediaChooserFilters() {
    static const std::vector<NativeFileChooser::Filter> filters{
        NativeFileChooser::Filter{QStringLiteral("Media files"),
                                  {QStringLiteral("*.exr"), QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                   QStringLiteral("*.jpeg"), QStringLiteral("*.tif"), QStringLiteral("*.tiff"),
                                   QStringLiteral("*.dpx"), QStringLiteral("*.mov"), QStringLiteral("*.mp4"),
                                   QStringLiteral("*.mkv"), QStringLiteral("*.wav")}},
        NativeFileChooser::Filter{QStringLiteral("All files"), {QStringLiteral("*")}}};
    return filters;
}

// The chooser reports local files only. A non-local URL or an empty selection
// is a failure the requester must see, never a path any adapter may store;
// `failure` is set, and the returned list left empty, when that happens.
inline QStringList chooserLocalPaths(const QList<QUrl>& urls, QString& failure) {
    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl& url : urls) {
        const QString path = url.toLocalFile();
        if (path.isEmpty()) {
            failure = QStringLiteral("The file dialog returned a non-local path.");
            return {};
        }
        paths.push_back(path);
    }
    if (paths.isEmpty()) {
        failure = QStringLiteral("The file dialog returned no file path.");
    }
    return paths;
}

}  // namespace nemo::ui
