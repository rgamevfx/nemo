#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/session/ProjectSession.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVulkanFunctions>

#include <vulkan/vulkan.h>

#ifndef NEMO_SLANG_SPV_DIR
#define NEMO_SLANG_SPV_DIR ""
#endif

#include <algorithm>
#include <cmath>
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
    const QCommandLineOption cacheDirectoryOption("viewer-cache-dir", "Viewer cache storage directory.", "path",
                                                  QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const QCommandLineOption cacheCodecOption("viewer-cache-codec", "Provisional viewer replay encoder.", "codec",
                                              "h264-nvenc");
    const QCommandLineOption cacheChunksOption("viewer-cache-chunk-frames", "Maximum requested frames per chunk.",
                                               "count", "12");
    const QCommandLineOption cacheBitrateOption("viewer-cache-bitrate-kbps", "Provisional replay bitrate.", "kbps",
                                                "8000");
    const QCommandLineOption benchmarkOption("cache-benchmark-frames",
                                             "Explicitly visit N frames and report request-to-Qt-frameSwapped latency.",
                                             "count", "0");
    parser.addOptions({cacheDirectoryOption, cacheCodecOption, cacheChunksOption, cacheBitrateOption, benchmarkOption});
    parser.addOption(frameOption);
    parser.process(app);
    bool chunkOk = false, bitrateOk = false, benchmarkOk = false;
    const int chunkFrames = parser.value(cacheChunksOption).toInt(&chunkOk);
    const int bitrate = parser.value(cacheBitrateOption).toInt(&bitrateOk);
    const int benchmarkFrames = parser.value(benchmarkOption).toInt(&benchmarkOk);
    if (!chunkOk || chunkFrames < 1 || chunkFrames > 48 || !bitrateOk || bitrate < 1 || !benchmarkOk ||
        benchmarkFrames < 0 || (benchmarkFrames > 0 && parser.value(sourceOption).isEmpty())) {
        std::cerr
            << "nemo-ui: invalid cache options; chunk frames 1..48, positive bitrate, benchmark requires source\n";
        return 2;
    }
    nemo::eval::ViewerCacheOptions cacheOptions;
    cacheOptions.directory = parser.value(cacheDirectoryOption).toStdString();
    cacheOptions.encoding.codec = parser.value(cacheCodecOption).toStdString();
    cacheOptions.encoding.bitrateKbps = bitrate;
    cacheOptions.chunkFrames = static_cast<std::size_t>(chunkFrames);
    cacheOptions.maxPendingFrames = static_cast<std::size_t>(chunkFrames);

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
        runtime.bootstrap(extensions.value(), NEMO_SLANG_SPV_DIR, cacheOptions);
    } catch (const std::exception& error) {
        std::cerr << "nemo-ui: gpu bootstrap failed: " << error.what() << '\n';
        return 1;
    }

    // The application composes one project owner; presentation facades may
    // come and go without taking the document or shared history with them.
    nemo::ProjectSession projectSession;
    nemo::ui::PanelContextRouter panelContextRouter(projectSession);
    nemo::ui::ViewerController viewerController(&runtime, projectSession);
    QObject::connect(&viewerController, &nemo::ui::ViewerController::sourceChanged, &panelContextRouter, [&] {
        if (!viewerController.hasSource())
            return;
        const auto source = QStringLiteral("src");
        const auto network = projectSession.document().rootNetworkId();
        panelContextRouter.openSource(QStringLiteral("A"), source);
        panelContextRouter.setGraphTarget(QStringLiteral("A"),
                                          QStringLiteral("network:%1").arg(static_cast<qulonglong>(network)));
        panelContextRouter.setTimelineTarget(QStringLiteral("A"), QStringLiteral("source:%1").arg(source));
    });
    std::vector<double> swapLatencies;
    if (benchmarkFrames > 0) {
        viewerController.setResolutionMode("half");
        QObject::connect(&viewerController, &nemo::ui::ViewerController::statusChanged, &app, [&] {
            if (!viewerController.error().isEmpty()) {
                std::cerr << "cache benchmark failed: " << viewerController.error().toStdString() << '\n';
                app.exit(1);
            }
        });
        QObject::connect(
            &viewerController, &nemo::ui::ViewerController::framePresented, &app,
            [&](int frame, int width, int height, bool cacheHit, double latency) {
                const QJsonObject sample{{"event", "viewer_frame_swapped"},
                                         {"frame", frame},
                                         {"width", width},
                                         {"height", height},
                                         {"cache_hit", cacheHit},
                                         {"request_to_swap_ms", latency}};
                std::cout << QJsonDocument(sample).toJson(QJsonDocument::Compact).constData() << std::endl;
                swapLatencies.push_back(latency);
                if (static_cast<int>(swapLatencies.size()) >= benchmarkFrames) {
                    std::sort(swapLatencies.begin(), swapLatencies.end());
                    const auto p95 = static_cast<std::size_t>(std::ceil(swapLatencies.size() * 0.95)) - 1;
                    const QJsonObject summary{{"event", "viewer_latency_summary"},
                                              {"samples", benchmarkFrames},
                                              {"p95_ms", swapLatencies[p95]},
                                              {"boundary", "Qt frameSwapped; window-system handoff, not scanout"}};
                    std::cout << QJsonDocument(summary).toJson(QJsonDocument::Compact).constData() << std::endl;
                    app.quit();
                } else if (viewerController.frameCount() > 0 && frame + 1 >= viewerController.frameCount()) {
                    std::cerr << "cache benchmark failed: source has fewer frames than requested\n";
                    app.exit(1);
                } else {
                    viewerController.setFrame(frame + 1);
                }
            },
            Qt::QueuedConnection);
        QTimer::singleShot(600'000, &app, [&] {
            std::cerr << "cache benchmark failed: timed out waiting for rendered presentation\n";
            app.exit(1);
        });
    }

    nemo::workspace::WorkspaceController workspace(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                                   QStringLiteral("/workspace.json"));
    panelContextRouter.setWorkspaceController(&workspace);
    // Production panels register their presentation descriptors before QML is
    // loaded; the shared shell never switches on panel type.
    workspace.registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"), QStringLiteral("ViewerPanel.qml"),
                                QString());
    workspace.registerPanelType(QStringLiteral("nodegraph"), QStringLiteral("Node graph"),
                                QStringLiteral("GraphPanel.qml"), QString());
    workspace.registerPanelType(QStringLiteral("timeline"), QStringLiteral("Timeline"),
                                QStringLiteral("TimelinePanel.qml"), QString());
    workspace.registerPanelType(QStringLiteral("parameters"), QStringLiteral("Parameters"),
                                QStringLiteral("ParametersPanel.qml"), QString());
    nemo::ui::ParameterEditorRegistry parameterEditors;
    int result = 0;
    {
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace);
        engine.rootContext()->setContextProperty(QStringLiteral("panelContextRouter"), &panelContextRouter);
        engine.rootContext()->setContextProperty(QStringLiteral("viewerController"), &viewerController);
        engine.rootContext()->setContextProperty(QStringLiteral("parameterEditors"), &parameterEditors);
        // Wayland Vulkan renders our QML chrome, not Qt's client decorations.
        // Set the window policy before creation so input and pixels share an origin.
        engine.setInitialProperties(
            {{QStringLiteral("frameless"), QGuiApplication::platformName() == QStringLiteral("wayland")}});
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
