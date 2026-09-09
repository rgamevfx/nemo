#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>
#include <QVulkanFunctions>

#include <vulkan/vulkan.h>

#ifndef NEMO_SLANG_SPV_DIR
#define NEMO_SLANG_SPV_DIR ""
#endif

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

// The platform surface extensions the adopted VkInstance must carry. Qt does
// not enable extensions on an adopted instance (it only checks them), so the
// required set is derived from the running platform and verified against the
// loader BEFORE the nemo gpu::Instance is created — a missing extension is a
// precise startup failure, not a swapchain-creation surprise.
[[nodiscard]] std::optional<std::vector<std::string>> requiredSurfaceExtensions() {
    const std::string platform = QGuiApplication::platformName().toStdString();
    std::vector<std::string> required{VK_KHR_SURFACE_EXTENSION_NAME};
    if (platform == "wayland") {
        required.push_back("VK_KHR_wayland_surface");
    } else if (platform == "xcb") {
        required.push_back("VK_KHR_xcb_surface");
    } else if (platform == "windows") {
        required.push_back("VK_KHR_win32_surface");
    } else {
        return std::nullopt;  // no presentation-capable windowing system integration
    }
    std::vector<VkExtensionProperties> available;
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
        return std::nullopt;
    }
    available.resize(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()) != VK_SUCCESS) {
        return std::nullopt;
    }
    for (const std::string& name : required) {
        const bool found = std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& extension) {
            return name == extension.extensionName;
        });
        if (!found) {
            std::cerr << "nemo-ui: Vulkan instance extension '" << name << "' required for platform '" << platform
                      << "' is not available from the loader/driver\n";
            return std::nullopt;
        }
    }
    return required;
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Nemo"));
    QCoreApplication::setApplicationName(QStringLiteral("nemo-ui"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Nemo workspace shell with the native Vulkan viewer (issue #11).\n"
                                                    "Pass --source <media> for a reproducible manual scenario."));
    parser.addHelpOption();
    const QCommandLineOption sourceOption({"s", "source"}, QStringLiteral("Load tagged source media on startup."),
                                          QStringLiteral("source"));
    const QCommandLineOption frameOption({"f", "frame"}, QStringLiteral("Initial composition frame."),
                                         QStringLiteral("frame"), QStringLiteral("0"));
    parser.addOption(sourceOption);
    parser.addOption(frameOption);
    parser.process(app);

    // Qt renders on the app-owned presentation device, separate from execution.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);

    const auto extensions = requiredSurfaceExtensions();
    if (!extensions.has_value()) {
        std::cerr << "nemo-ui: no presentation-capable Vulkan surface integration for platform '"
                  << QGuiApplication::platformName().toStdString()
                  << "'; the viewer requires Wayland, X11, or Windows surface support\n";
        return 1;
    }

    // App-owned GPU stack, declared before the engine so it is destroyed
    // after every Qt window/scene graph (and can be quiesced before that
    // teardown frees adopted presentation images).
    nemo::ui::ViewerRuntime runtime;
    try {
        runtime.bootstrap(extensions.value(), NEMO_SLANG_SPV_DIR);
    } catch (const std::exception& error) {
        std::cerr << "nemo-ui: gpu bootstrap failed: " << error.what() << '\n';
        return 1;
    }

    nemo::ui::ViewerController viewerController(&runtime);

    nemo::workspace::WorkspaceController workspace(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                                   QStringLiteral("/workspace.json"));
    int result = 0;
    {
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace);
        engine.rootContext()->setContextProperty(QStringLiteral("viewerController"), &viewerController);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Nemo/qml/Main.qml")));
        if (engine.rootObjects().isEmpty()) {
            return 1;
        }

        // Attach the presentation-only Vulkan device to the invisible window,
        // validate surface support, then show it. Qt's device-wide waits
        // cannot race the independent execution device.
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        viewerController.attachWindow(window);
        if (!viewerController.error().isEmpty()) {
            std::cerr << "nemo-ui: presentation attach failed: " << viewerController.error().toStdString() << '\n';
            return 1;
        }

        // Reproducible manual scenario from the command line.
        const QString sourcePath = parser.value(sourceOption);
        if (!sourcePath.isEmpty()) {
            QMetaObject::invokeMethod(
                &viewerController, [&viewerController, sourcePath] { viewerController.openSource(sourcePath); },
                Qt::QueuedConnection);
        }
        bool frameOk = false;
        const int initialFrame = parser.value(frameOption).toInt(&frameOk);
        if (frameOk && initialFrame != 0) {
            QMetaObject::invokeMethod(
                &viewerController, [&viewerController, initialFrame] { viewerController.setFrame(initialFrame); },
                Qt::QueuedConnection);
        }

        result = app.exec();
        runtime.stopWorker();
    }  // Qt stops its render thread and destroys its queue resources first.

    // The controller's frame-slot pins outlive Qt's engine/window. Drain
    // before releasing those final image/semaphore owners.
    runtime.quiesceForTeardown();
    return result;
}
