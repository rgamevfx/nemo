#include "WorkspaceController.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QUrl>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Nemo"));
    QCoreApplication::setApplicationName(QStringLiteral("nemo-ui"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    nemo::workspace::WorkspaceController workspace(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                                   QStringLiteral("/workspace.json"));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Nemo/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec();
}
