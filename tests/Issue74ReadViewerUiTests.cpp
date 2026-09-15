// Read-to-Viewer native regression (issue #74).
//
// #61 delivered the Read node and its node-local media control, but the real
// browse-to-visible-image workflow was reported blank in the configured native
// application. The prior art (ViewerDestinationSurface, issue #47) proves the
// real window and per-destination presentation with synthetic `constcolor`
// nodes only, so it never exercised a Read/media source through the attached
// Viewer. This file extends that same production seam: the real Read control
// registers an image, the command-owned Document publishes the graph, and the
// panel-local ViewerController/ViewerRuntime must present recognizable pixels
// on the actual QQuickWindow.

#include "MediaLibraryModel.hpp"
#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ProjectFileController.hpp"
#include "ReadSourceController.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerItem.hpp"
#include "ViewerRuntime.hpp"
#include "Workspace.hpp"
#include "WorkspaceController.hpp"

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/MediaImportService.hpp"
#include "nemo/media/ViewingTransform.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {

using nemo::CpuImage;
using nemo::NodeId;
using nemo::ProjectSession;
using nemo::ui::ViewerController;

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 120000) {
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QTest::qWait(10);
    }
    return predicate();
}

// A real, decodable PNG of a distinctive size and color. Agents and CI write
// this in a temporary directory; no private user media is required.
std::filesystem::path writePng(const std::filesystem::path& directory, const std::string& name, int width, int height,
                               const std::array<float, 4>& rgba) {
    CpuImage image(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, rgba);
    const auto path = directory / (name + ".png");
    nemo::media::writeImage(path.string(), image, nemo::media::OutputPrecision::Float32);
    return path;
}

// The encoded sample of the config-freshness plate. Chromatic, so an
// expectation cannot be satisfied by neutral interface chrome.
inline constexpr float kConfigSampleR = 0.75F;
inline constexpr float kConfigSampleG = 0.25F;
inline constexpr float kConfigSampleB = 0.5F;

// A temp config whose only nonlinear op is the input space exponent, and whose
// display equation is a known matrix (R,G,B x 1,1,0.5). Rewriting the exponent
// at the same path is therefore a pure, independently computable change.
void writeGammaConfig(const std::filesystem::path& config, const std::string& gamma) {
    std::string text;
    text += "ocio_profile_version: 2\n";
    text += "search_path: \"\"\n";
    text += "roles:\n  default: linear\n  scene_linear: working_rec709\n";
    text += "colorspaces:\n";
    text += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
    text += "  - !<ColorSpace>\n    name: working_rec709\n    allocation: linear\n";
    text += "  - !<ColorSpace>\n    name: rec709_texture\n";
    text += "    to_reference: !<ExponentTransform> {value: [" + gamma + ", " + gamma + ", " + gamma + ", 1.0]}\n";
    text += "  - !<ColorSpace>\n    name: display_view\n";
    text += "    from_reference: !<MatrixTransform> {matrix: [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, "
            "0.5, 0.0, 0.0, 0.0, 0.0, 1.0]}\n";
    text += "displays:\n  sRGB:\n    - !<View> {name: rec709, colorspace: display_view}\n";
    std::ofstream out(config, std::ios::trunc);
    out << text;
}

// Expected 8-bit display color derived only from the encoded sample bytes and
// the config above: input --pow(sample, gamma)--> working --matrix--> display.
// No production read/resolve/transform code participates.
[[nodiscard]] std::array<int, 3> expectedGammaDisplay(double gamma) {
    return {static_cast<int>(std::lround(std::pow(kConfigSampleR, gamma) * 255.0)),
            static_cast<int>(std::lround(std::pow(kConfigSampleG, gamma) * 255.0)),
            static_cast<int>(std::lround(std::pow(kConfigSampleB, gamma) * 0.5 * 255.0))};
}

QQuickItem* visualByName(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = visualByName(child, name))
            return found;
    return nullptr;
}

QQuickItem* panelRootFor(QQuickItem* content, const QString& panelId) {
    QQuickItem* field = visualByName(content, QStringLiteral("viewerFrame_") + panelId);
    while (field && field->objectName() != QStringLiteral("viewerPanel"))
        field = field->parentItem();
    return field;
}

class ReadViewerSurface : public testing::Test {
protected:
    QTemporaryDir directory_;
    std::unique_ptr<nemo::ui::ViewerRuntime> runtime_;
    std::unique_ptr<ProjectSession> session_;
    std::unique_ptr<nemo::media::MediaImportService> importer_;
    std::unique_ptr<nemo::ui::MediaLibraryModel> media_;
    std::unique_ptr<nemo::ui::NativeFileChooser> chooser_;
    std::unique_ptr<nemo::ui::ReadSourceController> readSource_;
    std::unique_ptr<nemo::ui::PanelContextRouter> router_;
    std::unique_ptr<nemo::ui::ViewerController> facade_;
    std::unique_ptr<nemo::ui::ViewerControllerRegistry> registry_;
    std::unique_ptr<nemo::workspace::WorkspaceController> workspace_;
    // Runs first, after the session exists and before the runtime, import
    // worker, media model or read source resolve any OCIO configuration, so a
    // scenario can install its own config before those owners capture one.
    std::function<void()> prepareConfig;
    // Runs after the session/media/read-source owners exist and before any
    // observing ViewerController is constructed. Empty for every existing test.
    std::function<void()> prepareSession;
    // Runs before the workspace file is written, so a scenario can install a
    // layout and persisted panel state of its own. It returns the workspace
    // document to write; empty keeps the single-panel default below. The
    // confirmed-application path (NEMO74_USER_WORKSPACE) ignores it: copying the
    // owner's real workspace is a distinct scenario.
    std::function<QJsonObject()> prepareWorkspaceDocument;
    // Session revision captured at that same boundary (after the preparation
    // hook, before any observing controller). A scenario that needs the first
    // session notification to be its own replacement compares against this.
    std::uint64_t constructionBoundaryRevision{0};
    std::unique_ptr<nemo::ui::ProjectFileController> projectFile_;
    std::unique_ptr<nemo::ui::ParameterEditorRegistry> editors_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    std::unique_ptr<QSignalSpy> warnings_;
    QQuickWindow* window_{};
    ViewerController* controller_{};
    QString panel_{QStringLiteral("panel-a")};
    std::filesystem::path colorConfig_;
    QString evidenceDirectory_;

    void SetUp() override {
        if (qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") != 1 ||
            qEnvironmentVariableIntValue("NEMO_TEST_VIEWER_WINDOW") != 1) {
            GTEST_SKIP() << "native Read-to-Viewer evidence requires NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1";
        }
        if (std::string(NEMO_SLANG_SPV_DIR).empty())
            GTEST_SKIP() << "native Read-to-Viewer evidence requires compiled Slang shaders";
        const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                            "docs/evidence/issue12-view.ocio";
        colorConfig_ = config;
        qputenv("OCIO", config.string().c_str());
        evidenceDirectory_ = qEnvironmentVariable("NEMO74_EVIDENCE_DIR");

        // The session owns the document/color reference and has no config,
        // media or GPU dependency, so it exists before every config-dependent
        // owner: a scenario can install its configuration before the runtime,
        // the import worker or the read source ever resolve one.
        session_ = std::make_unique<ProjectSession>();
        if (prepareConfig)
            prepareConfig();

        runtime_ = std::make_unique<nemo::ui::ViewerRuntime>();
        nemo::eval::ViewerCacheOptions cacheOptions;
        cacheOptions.directory = directory_.filePath(QStringLiteral("cache")).toStdString();
        cacheOptions.encoding.codec = "libx264-cpu";
        cacheOptions.chunkFrames = 1;
        std::vector<std::string> extensions{"VK_KHR_surface"};
        if (QGuiApplication::platformName() == QStringLiteral("wayland"))
            extensions.push_back("VK_KHR_wayland_surface");
        else if (QGuiApplication::platformName() == QStringLiteral("xcb"))
            extensions.push_back("VK_KHR_xcb_surface");
        try {
            runtime_->bootstrap(extensions, NEMO_SLANG_SPV_DIR, cacheOptions);
        } catch (const nemo::gpu::GpuException& error) {
            if (error.errorCode() == nemo::gpu::GpuError::NoDevice)
                GTEST_SKIP() << error.what();
            throw;
        }

        importer_ = std::make_unique<nemo::media::MediaImportService>();
        media_ = std::make_unique<nemo::ui::MediaLibraryModel>(*session_, *importer_);
        chooser_ = std::make_unique<nemo::ui::NativeFileChooser>();
        readSource_ = std::make_unique<nemo::ui::ReadSourceController>(*session_, *media_, *chooser_);
        router_ = std::make_unique<nemo::ui::PanelContextRouter>(*session_);
        // A scenario that must make the session replacement the FIRST
        // notification prepares its document and configuration here, before any
        // observing ViewerController exists (facade_ below, and the panel
        // controllers created when Main.qml loads). Other tests leave this
        // unset and keep the default lifecycle.
        if (prepareSession)
            prepareSession();
        // The exact construction boundary: recorded after the prepared project
        // exists and before ANY observing ViewerController is constructed, so a
        // scenario can prove that nothing mutated the document across window
        // creation, event pumping and the baseline render.
        constructionBoundaryRevision = session_->revision();
        facade_ = std::make_unique<ViewerController>(runtime_.get(), *session_);
        registry_ = std::make_unique<nemo::ui::ViewerControllerRegistry>(runtime_.get(), *session_);

        const auto workspacePath = directory_.filePath(QStringLiteral("workspace.json"));
        // A confirmation run may point at the application's real workspace
        // file so the panel layout, group bindings and viewer state are the
        // owner's, not a synthetic single-panel shell.
        const auto userWorkspace = qEnvironmentVariable("NEMO74_USER_WORKSPACE");
        if (!userWorkspace.isEmpty()) {
            ASSERT_TRUE(QFile::copy(userWorkspace, workspacePath)) << userWorkspace.toStdString();
        } else {
            const QJsonObject panel{{QStringLiteral("id"), panel_},
                                    {QStringLiteral("type"), QStringLiteral("viewer")},
                                    {QStringLiteral("group"), QStringLiteral("A")},
                                    {QStringLiteral("state"), QJsonObject{{QStringLiteral("viewerIndex"), 0}}}};
            const QJsonObject leaf{{QStringLiteral("id"), QStringLiteral("leaf-a")},
                                   {QStringLiteral("kind"), QStringLiteral("tabs")},
                                   {QStringLiteral("active"), panel_},
                                   {QStringLiteral("panels"), QJsonArray{panel}}};
            const QJsonObject layout{{QStringLiteral("version"), 1}, {QStringLiteral("root"), leaf}};
            const QJsonObject document{{QStringLiteral("version"), 2},
                                       {QStringLiteral("activeWorkspaceId"), QStringLiteral("workspace-1")},
                                       {QStringLiteral("workspaces"),
                                        QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-1")},
                                                               {QStringLiteral("name"), QStringLiteral("Read viewer")},
                                                               {QStringLiteral("layout"), layout}}}}};
            const QJsonObject workspace = prepareWorkspaceDocument ? prepareWorkspaceDocument() : document;
            QFile workspaceFile(workspacePath);
            ASSERT_TRUE(workspaceFile.open(QIODevice::WriteOnly));
            workspaceFile.write(QJsonDocument(workspace).toJson());
            workspaceFile.close();
        }
        workspace_ = std::make_unique<nemo::workspace::WorkspaceController>(workspacePath);
        ASSERT_TRUE(workspace_->error().isEmpty()) << workspace_->error().toStdString();
        workspace_->registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"),
                                      QStringLiteral("ViewerPanel.qml"));
        workspace_->registerPanelType(QStringLiteral("nodegraph"), QStringLiteral("Node graph"),
                                      QStringLiteral("GraphPanel.qml"));
        workspace_->registerPanelType(QStringLiteral("parameters"), QStringLiteral("Parameters"),
                                      QStringLiteral("ParametersPanel.qml"));
        workspace_->registerPanelType(QStringLiteral("animation"), QStringLiteral("Animation"),
                                      QStringLiteral("AnimationPanel.qml"));
        workspace_->registerPanelType(QStringLiteral("timeline"), QStringLiteral("Timeline"),
                                      QStringLiteral("TimelinePanel.qml"));
        workspace_->registerPanelType(QStringLiteral("media"), QStringLiteral("Media Bin"),
                                      QStringLiteral("MediaBinPanel.qml"));
        router_->setWorkspaceController(workspace_.get());
        facade_->setDestination(runtime_->allocateDestination(QStringLiteral("shared-facade")));
        if (userWorkspace.isEmpty()) {
            controller_ = qobject_cast<ViewerController*>(registry_->controller(panel_));
            ASSERT_NE(controller_, nullptr);
        }

        projectFile_ = std::make_unique<nemo::ui::ProjectFileController>(*session_, *workspace_, *router_, *chooser_);
        editors_ = std::make_unique<nemo::ui::ParameterEditorRegistry>();

        engine_ = std::make_unique<QQmlApplicationEngine>();
        warnings_ = std::make_unique<QSignalSpy>(engine_.get(), &QQmlEngine::warnings);
        engine_->rootContext()->setContextProperty(QStringLiteral("workspace"), workspace_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("panelContextRouter"), router_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("projectFile"), projectFile_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewerController"), facade_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewerControllers"), registry_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("parameterEditors"), editors_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("readSourceController"), readSource_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("mediaLibrary"), media_.get());
        engine_->load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine_->rootObjects().isEmpty());
        window_ = qobject_cast<QQuickWindow*>(engine_->rootObjects().constFirst());
        ASSERT_NE(window_, nullptr);

        const QString attachError = runtime_->attachToWindow(window_);
        ASSERT_TRUE(attachError.isEmpty()) << attachError.toStdString();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window_));
    }

    void TearDown() override {
        if (runtime_)
            runtime_->stopWorker();
        warnings_.reset();
        engine_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (runtime_)
            runtime_->quiesceForTeardown();
        editors_.reset();
        projectFile_.reset();
        readSource_.reset();
        chooser_.reset();
        media_.reset();
        importer_.reset();
        registry_.reset();
        facade_.reset();
        workspace_.reset();
        router_.reset();
        session_.reset();
        runtime_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    [[nodiscard]] QString rootNetwork() const { return controller_->rootNetworkId(); }

    // Independent oracle for a still's display color: the shared image adapter
    // (scene-linear decode) through the same OCIO CPU policy the viewer applies
    // on the GPU. The native capture must land on those bytes within the
    // pinned transform tolerance, so the assertion never reuses the viewer's
    // own output as its expectation.
    [[nodiscard]] std::array<int, 3> expectedDisplayRgb(const std::filesystem::path& still) const {
        nemo::SourceReference reference;
        reference.path = still.string();
        nemo::CpuImage image = nemo::media::readImageFrame(reference, 0, "viewer color oracle").image;
        nemo::media::applyViewingTransformCpu(image, colorConfig_.string(), nemo::ColorPolicy{});
        const auto pixel = image.pixel(0, 0);
        return {static_cast<int>(std::lround(pixel[0] * 255.0F)), static_cast<int>(std::lround(pixel[1] * 255.0F)),
                static_cast<int>(std::lround(pixel[2] * 255.0F))};
    }

    [[nodiscard]] static int countPixelsNear(const QImage& image, const std::array<int, 3>& rgb, int tolerance) {
        int matches = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                if (std::abs(pixel.red() - rgb[0]) <= tolerance && std::abs(pixel.green() - rgb[1]) <= tolerance &&
                    std::abs(pixel.blue() - rgb[2]) <= tolerance)
                    ++matches;
            }
        return matches;
    }

    // The project's own panel ids only exist after QML created the panels.
    [[nodiscard]] ViewerController* controllerFor(const QString& panelId, int timeoutMs = 20000) {
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < timeoutMs) {
            if (auto* found = qobject_cast<ViewerController*>(registry_->controller(panelId)))
                return found;
            QTest::qWait(20);
        }
        return nullptr;
    }

    // Crops the panel body out of the real window grab. A blank viewer is the
    // panel background; a displayed image introduces non-background pixels. The
    // pointer is parked in a corner before grabbing unless the observation IS
    // the hover state, so routine evidence never shows interaction chrome.
    [[nodiscard]] QImage grabPanel(const QString& panelId = {}, bool preservePointer = false) {
        auto* panel = panelRootFor(window_->contentItem(), panelId.isEmpty() ? panel_ : panelId);
        if (!panel)
            return {};
        if (!preservePointer) {
            QTest::mouseMove(window_, QPoint(4, 4));
            QTest::qWait(50);
        }
        const QRect crop =
            QRectF(panel->mapToScene(QPointF{}), QSizeF(panel->width(), panel->height())).toAlignedRect();
        return window_->grabWindow().copy(crop).convertToFormat(QImage::Format_RGB32);
    }

    // Crops only the media surface, excluding transport and ruler chrome, so a
    // content check cannot be satisfied by panel controls.
    [[nodiscard]] QImage grabImageArea(const QString& panelId = {}) {
        auto* item = visualByName(window_->contentItem(),
                                  QStringLiteral("viewerItem_") + (panelId.isEmpty() ? panel_ : panelId));
        if (!item)
            return {};
        QTest::mouseMove(window_, QPoint(4, 4));
        QTest::qWait(50);
        const QRect crop = QRectF(item->mapToScene(QPointF{}), QSizeF(item->width(), item->height())).toAlignedRect();
        return window_->grabWindow().copy(crop).convertToFormat(QImage::Format_RGB32);
    }

    // Standard deviation of the image area's luminance. A viewer that shows
    // nothing is a flat surround; displayed media is not.
    [[nodiscard]] static double luminanceSpread(const QImage& image) {
        if (image.isNull())
            return 0.0;
        double sum = 0.0;
        double sumSquares = 0.0;
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                const double luminance = 0.299 * pixel.red() + 0.587 * pixel.green() + 0.114 * pixel.blue();
                sum += luminance;
                sumSquares += luminance * luminance;
                ++count;
            }
        if (count == 0)
            return 0.0;
        const double mean = sum / count;
        return std::sqrt(std::max(0.0, sumSquares / count - mean * mean));
    }

    // Native capture for the recorded evidence directory, when one is set,
    // beside the environment and the view it was taken in. `preservePointer`
    // keeps the pointer where the scenario put it, which is the only way the
    // hover state of a control can appear in a capture.
    void capture(const QString& name, const QString& panelId = {}, bool preservePointer = false,
                 bool includeWindowChrome = false) {
        if (evidenceDirectory_.isEmpty())
            return;
        QDir().mkpath(evidenceDirectory_);
        const QString id = panelId.isEmpty() ? panel_ : panelId;
        auto* controller = panelController(id);
        auto* item = viewerItem(id);
        const QJsonObject environment{{QStringLiteral("platform"), QGuiApplication::platformName()},
                                      {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
                                      {QStringLiteral("device_pixel_ratio"), window_->devicePixelRatio()},
                                      {QStringLiteral("window_width"), window_->width()},
                                      {QStringLiteral("window_height"), window_->height()},
                                      {QStringLiteral("appearance_preset"), workspace_->appearancePreset()},
                                      {QStringLiteral("panel"), id},
                                      {QStringLiteral("capture"), name},
                                      {QStringLiteral("media_surface_width"), item ? item->width() : 0.0},
                                      {QStringLiteral("media_surface_height"), item ? item->height() : 0.0},
                                      {QStringLiteral("displayed_scale"), sampleView(id).scale()},
                                      {QStringLiteral("controller_zoom"), controller ? controller->zoom() : 0.0},
                                      {QStringLiteral("pan_x"), controller ? controller->pan().x() : 0.0},
                                      {QStringLiteral("pan_y"), controller ? controller->pan().y() : 0.0},
                                      {QStringLiteral("sampling_scale"), presentedRequest(id).samplingScale},
                                      {QStringLiteral("region_x"), presentedRequest(id).region.x},
                                      {QStringLiteral("region_y"), presentedRequest(id).region.y},
                                      {QStringLiteral("region_width"), presentedRequest(id).region.width},
                                      {QStringLiteral("region_height"), presentedRequest(id).region.height},
                                      {QStringLiteral("stated_zoom"), statedZoom(id)}};
        QFile environmentFile(evidenceDirectory_ + QStringLiteral("/environment-") + name + QStringLiteral(".json"));
        EXPECT_TRUE(environmentFile.open(QIODevice::WriteOnly));
        environmentFile.write(QJsonDocument(environment).toJson(QJsonDocument::Indented));
        const QImage image = includeWindowChrome ? window_->grabWindow() : grabPanel(panelId, preservePointer);
        EXPECT_TRUE(image.save(evidenceDirectory_ + '/' + name + ".png"));
    }

    // Qt 6.4's QtTest has no wheel helper, so the harness constructs the wheel
    // event and delivers it to the window — the real input path, with no
    // test-only handler in production code.
    void wheel(const QPoint& position, int angleDelta, const QPoint& pixelDelta = QPoint()) {
        QWheelEvent event(QPointF(position), QPointF(window_->mapToGlobal(position)), pixelDelta, QPoint(0, angleDelta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window_, &event);
    }

    void drag(const QPoint& from, const QPoint& to, Qt::MouseButton button = Qt::LeftButton, int steps = 8) {
        QTest::mousePress(window_, button, Qt::NoModifier, from);
        for (int step = 1; step <= steps; ++step)
            QTest::mouseMove(window_, from + (to - from) * step / steps, 2);
        QTest::mouseRelease(window_, button, Qt::NoModifier, to);
        QTest::qWait(30);
    }

    [[nodiscard]] QQuickItem* viewerItem(const QString& panelId = {}) {
        return visualByName(window_->contentItem(),
                            QStringLiteral("viewerItem_") + (panelId.isEmpty() ? panel_ : panelId));
    }

    // The panel's display transform for the presented frame, in panel pixels:
    // the surface the artist sees the media mapped onto.
    [[nodiscard]] QRectF displayedRect(const QString& panelId = {}) {
        auto* item = viewerItem(panelId);
        return item ? item->property("displayRect").toRectF() : QRectF();
    }

    [[nodiscard]] QQuickItem* zoomControl(const QString& panelId = {}) {
        return visualByName(window_->contentItem(),
                            QStringLiteral("viewerZoomMenu_") + (panelId.isEmpty() ? panel_ : panelId));
    }

    [[nodiscard]] ViewerController* panelController(const QString& panelId = {}) {
        return panelId.isEmpty() ? controller_ : controllerFor(panelId);
    }

    // The request the controller submitted for the currently presented frame.
    [[nodiscard]] nemo::EvaluationRequest presentedRequest(const QString& panelId = {}) {
        auto* controller = panelController(panelId);
        const auto presentation = controller ? controller->presentation() : nullptr;
        return presentation ? presentation->request : nemo::EvaluationRequest{};
    }

    // Waits for the presented frame to carry a request the current view
    // produces, so region and sampling can be observed without pixel inspection.
    bool waitForRequest(const std::function<bool(const nemo::EvaluationRequest&)>& predicate,
                        const QString& panelId = {}, int timeoutMs = 60000) {
        return waitFor(
            [&] {
                auto* controller = panelController(panelId);
                const auto presentation = controller ? controller->presentation() : nullptr;
                return presentation && predicate(presentation->request);
            },
            timeoutMs);
    }

    // Picks a preset from the shared combo's own popup, by the real input path:
    // click the control to open its menu, then click the entry it shows.
    void pickZoomPreset(QQuickItem* control, int index) {
        // The arrow beside the value is the preset menu's target on a typeable
        // control; the body of the control belongs to the field.
        const QPoint arrow = control->mapToScene(QPointF(control->width() - 12, control->height() / 2)).toPoint();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, arrow);
        QTest::qWait(80);
        auto* popup = control->property("popup").value<QObject*>();
        ASSERT_NE(popup, nullptr);
        auto* list = popup->property("contentItem").value<QQuickItem*>();
        ASSERT_NE(list, nullptr) << "the zoom control's preset menu must show its entries";
        QQuickItem* entry = nullptr;
        ASSERT_TRUE(
            QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, index)));
        ASSERT_NE(entry, nullptr) << "the preset menu shows entry " << index;
        const QPointF entryCenter(entry->width() / 2, entry->height() / 2);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, entry->mapToScene(entryCenter).toPoint());
        QTest::qWait(80);
    }

    // One observation of the view: the region the presented frame carries, and
    // the panel rect the display transform draws it into. Scale and image-point
    // mapping are derived from those two alone, so no assertion repeats the
    // panel's own view arithmetic.
    struct ViewSample {
        nemo::Region region;
        QRectF rect;
        [[nodiscard]] double scale() const {
            return region.width > 0 ? rect.width() / static_cast<double>(region.width) : 0.0;
        }
        [[nodiscard]] QPointF panelPoint(double imageX, double imageY) const {
            const double sx = scale();
            const double sy = region.height > 0 ? rect.height() / static_cast<double>(region.height) : 0.0;
            return QPointF(rect.x() + (imageX - region.x) * sx, rect.y() + (imageY - region.y) * sy);
        }
    };

    [[nodiscard]] ViewSample sampleView(const QString& panelId = {}) {
        auto* controller = panelController(panelId);
        const auto presentation = controller ? controller->presentation() : nullptr;
        if (!presentation)
            return {};
        return ViewSample{presentation->request.region, displayedRect(panelId)};
    }

    // The scale the zoom control states: the value it edits, which is also what
    // it renders while the artist is not typing.
    [[nodiscard]] QString statedZoom(const QString& panelId = {}) {
        auto* control = zoomControl(panelId);
        return control ? control->property("editText").toString() : QString();
    }

    // The percentage text the control states for a scale, computed the way the
    // panel states it.
    [[nodiscard]] static QString percentText(double scale) {
        const double percent = std::round(scale * 1000.0) / 10.0;
        const bool integral = percent == std::round(percent);
        return (integral ? QString::number(static_cast<long long>(std::llround(percent)))
                         : QString::number(percent, 'f', 1)) +
               QStringLiteral("%");
    }
};

TEST_F(ReadViewerSurface, PngSelectedInReadAppearsInItsAttachedViewer) {
    const auto png = writePng(directory_.path().toStdString(), "figure", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});

    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    ASSERT_FALSE(read.isEmpty());
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(png.string())))
        << readSource_->error().toStdString();
    ASSERT_TRUE(waitFor([&] {
        return readSource_->info(rootNetwork(), read).value(QStringLiteral("state")).toString() ==
               QStringLiteral("ready");
    })) << readSource_->info(rootNetwork(), read).value(QStringLiteral("error")).toString().toStdString();

    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_FALSE(viewer.isEmpty());
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));

    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; }))
        << controller_->error().toStdString() << " status=" << controller_->status().toStdString();
    const auto presentation = controller_->presentation();
    ASSERT_TRUE(presentation);
    EXPECT_EQ(presentation->frame.width, 96);
    EXPECT_EQ(presentation->frame.height, 64);
    EXPECT_EQ(controller_->sourceSize(), QSizeF(96, 64));
    EXPECT_EQ(controller_->frameCount(), 1);

    const auto image = grabPanel();
    ASSERT_FALSE(image.isNull());
    // The native capture must show the still's own display color over a large
    // area. Panel chrome and background are neutral and far from it, so a
    // matching run is the media, never a control.
    const auto expected = expectedDisplayRgb(png);
    const int distinct = countPixelsNear(image, expected, 12);
    EXPECT_GT(distinct, 300) << "viewer panel showed no image pixels; status=" << controller_->status().toStdString()
                             << " error=" << controller_->error().toStdString() << " expected rgb " << expected[0]
                             << ',' << expected[1] << ',' << expected[2];
    capture(QStringLiteral("png-through-read"));
    EXPECT_EQ(warnings_->count(), 0);
}

// The verified integration gap: the graph viewer probed the command-line
// fixture key "src" instead of the reference its attached Read names. A
// document that has both must render the Read's media with the Read's
// dimensions, duration and pixels.
TEST_F(ReadViewerSurface, GraphViewerUsesTheAttachedTargetsReferenceNotTheLegacySrcKey) {
    const auto legacy = writePng(directory_.path().toStdString(), "legacy", 2, 2, {0.0F, 1.0F, 0.0F, 1.0F});
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    nemo::SourceReference legacyReference;
    legacyReference.path = legacy.string();
    ASSERT_TRUE(
        session_->submit(nemo::setSourceCommand("src", legacyReference), {.expectedRevision = session_->revision()})
            .committed);

    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    ASSERT_FALSE(read.isEmpty());
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())))
        << readSource_->error().toStdString();
    ASSERT_TRUE(waitFor([&] {
        return readSource_->info(rootNetwork(), read).value(QStringLiteral("state")).toString() ==
               QStringLiteral("ready");
    }));
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));

    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; }))
        << controller_->error().toStdString() << " status=" << controller_->status().toStdString();
    const auto presentation = controller_->presentation();
    ASSERT_TRUE(presentation);
    // The legacy "src" still is 2x2; the attached Read's plate is 96x64.
    EXPECT_EQ(presentation->frame.width, 96);
    EXPECT_EQ(presentation->frame.height, 64);
    EXPECT_EQ(controller_->sourceSize(), QSizeF(96, 64));
    EXPECT_EQ(controller_->frameCount(), 1);
    const auto expected = expectedDisplayRgb(plate);
    EXPECT_GT(countPixelsNear(grabPanel(), expected, 12), 300)
        << "viewer showed the legacy src reference instead of the attached Read's media";
}

// A source change must refresh both the displayed image and its metadata
// without recreating the panel, and undo must restore the visible result.
TEST_F(ReadViewerSurface, ReplacingTheReadPathUpdatesTheViewerAndUndoRestoresIt) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto other = writePng(directory_.path().toStdString(), "other", 40, 20, {0.9F, 0.1F, 0.1F, 1.0F});

    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    ASSERT_FALSE(read.isEmpty());
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->frame.width == 96;
    })) << controller_->error().toStdString();

    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(other.string())));
    ASSERT_TRUE(waitFor([&] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->frame.width == 40 && controller_->sourceSize() == QSizeF(40, 20);
    })) << controller_->error().toStdString();

    ASSERT_TRUE(controller_->undo());
    ASSERT_TRUE(waitFor([&] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->frame.width == 96 && controller_->sourceSize() == QSizeF(96, 64);
    })) << controller_->error().toStdString();
}

// Clearing Read removes the previous image and reports an explicit empty
// state; stale media is never mistaken for current output.
TEST_F(ReadViewerSurface, ClearingTheReadShowsTheExplicitEmptyState) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; }));

    // Clearing is an ordinary value edit of the File control through the shared
    // parameter gesture (the Read adapter no longer owns a static mutation API).
    const QString sourceKey = QStringLiteral("source");
    const QString clearToken = controller_->beginNodeParameterEdits(rootNetwork(), read, QStringList{sourceKey});
    ASSERT_FALSE(clearToken.isEmpty()) << controller_->error().toStdString();
    ASSERT_TRUE(controller_->updateNodeParameterEdits(clearToken, QVariantMap{{sourceKey, QString()}}))
        << controller_->error().toStdString();
    ASSERT_TRUE(controller_->commitNodeParameterEdit(clearToken)) << controller_->error().toStdString();
    ASSERT_TRUE(waitFor([&] { return controller_->renderState() == QStringLiteral("empty"); }))
        << controller_->renderState().toStdString();
    EXPECT_FALSE(controller_->hasPresentation());
    EXPECT_TRUE(controller_->sourceSize().isEmpty());
    EXPECT_EQ(controller_->frameCount(), -1);
    EXPECT_TRUE(controller_->error().isEmpty());
    EXPECT_NE(controller_->status().indexOf(QStringLiteral("Read1")), -1) << controller_->status().toStdString();

    // The panel shows the message where the image would be, not a blank area.
    auto* message = visualByName(panelRootFor(window_->contentItem(), panel_), "viewerUnavailable_" + panel_);
    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->isVisible());
    EXPECT_NE(message->property("text").toString().indexOf(QStringLiteral("Read1")), -1);
    capture(QStringLiteral("cleared-read-empty"));
}

// An offline reference reports an actionable failure naming the path on the
// viewer itself, rather than an empty image area with no explanation, and
// relinking recovers.
TEST_F(ReadViewerSurface, OfflineReadReportsItsPathOnTheViewer) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto missing = directory_.filePath(QStringLiteral("absent.png"));
    nemo::SourceReference absentReference;
    absentReference.path = missing.toStdString();
    ASSERT_TRUE(
        session_->submit(nemo::setSourceCommand("absent", absentReference), {.expectedRevision = session_->revision()})
            .committed);
    nemo::SourceReference plateReference;
    plateReference.path = plate.string();
    ASSERT_TRUE(
        session_->submit(nemo::setSourceCommand("plate", plateReference), {.expectedRevision = session_->revision()})
            .committed);

    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    controller_->setNodeParameter(read, QStringLiteral("source"), QStringLiteral("absent"));

    ASSERT_TRUE(waitFor([&] { return !controller_->error().isEmpty(); }))
        << "offline media reported nothing; status=" << controller_->status().toStdString();
    EXPECT_EQ(controller_->renderState(), QStringLiteral("failed"));
    EXPECT_NE(controller_->error().indexOf(QStringLiteral("absent.png")), -1) << controller_->error().toStdString();
    auto* message = visualByName(panelRootFor(window_->contentItem(), panel_), "viewerUnavailable_" + panel_);
    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->isVisible());
    EXPECT_NE(message->property("text").toString().indexOf(QStringLiteral("absent.png")), -1);

    // Relinking the same reference recovers the attached viewer with no panel
    // re-creation.
    controller_->setNodeParameter(read, QStringLiteral("source"), QStringLiteral("plate"));
    ASSERT_TRUE(waitFor([&] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->frame.width == 96 && controller_->error().isEmpty();
    })) << controller_->error().toStdString();
}

// A downstream target keeps the domain of the single media source it depends
// on, so an effect on a Read preserves the Read's raster and transport instead
// of falling back to the default canvas.
TEST_F(ReadViewerSurface, DownstreamEffectKeepsItsSingleMediaSourcesDomain) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto grade = controller_->createGraphNode(rootNetwork(), QStringLiteral("grade"), QStringLiteral("Grade1"),
                                                    40.0, 80.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 160.0, {}, {});
    ASSERT_FALSE(read.isEmpty());
    ASSERT_FALSE(grade.isEmpty());
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, grade, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), grade, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] {
        const auto presentation = controller_->presentation();
        return presentation && presentation->frame.width == 96;
    })) << controller_->error().toStdString();
    EXPECT_EQ(controller_->sourceSize(), QSizeF(96, 64));
    EXPECT_EQ(controller_->frameCount(), 1);
    EXPECT_EQ(controller_->presentation()->request.output, static_cast<nemo::NodeId>(grade.toULongLong()));
}

// Several media sources have no defined composition format, so the viewer keeps
// the established default canvas rather than picking one source arbitrarily.
TEST_F(ReadViewerSurface, SeveralMediaSourcesKeepTheDefaultCompositionCanvas) {
    const auto first = writePng(directory_.path().toStdString(), "first", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto second = writePng(directory_.path().toStdString(), "second", 40, 20, {0.9F, 0.1F, 0.1F, 1.0F});
    const auto readA = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("ReadA"),
                                                    0.0, 0.0, {}, {});
    const auto readB = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("ReadB"),
                                                    0.0, 40.0, {}, {});
    const auto merge = controller_->createGraphNode(rootNetwork(), QStringLiteral("merge"), QStringLiteral("Merge1"),
                                                    40.0, 80.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 160.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), readA, 0, merge, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), readB, 0, merge, 1));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), merge, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), readA, QString::fromStdString(first.string())));
    // One probe is outstanding per control: a second selection supersedes the
    // first, so each node is committed before the next is chosen.
    ASSERT_TRUE(waitFor([&] {
        return readSource_->info(rootNetwork(), readA).value(QStringLiteral("state")).toString() ==
               QStringLiteral("ready");
    }));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), readB, QString::fromStdString(second.string())));
    ASSERT_TRUE(waitFor([&] {
        return readSource_->info(rootNetwork(), readB).value(QStringLiteral("state")).toString() ==
               QStringLiteral("ready");
    }));
    ASSERT_TRUE(
        waitFor([&] { return controller_->presentation() != nullptr || !controller_->error().isEmpty(); }, 30000))
        << controller_->error().toStdString();
    ASSERT_TRUE(controller_->presentation())
        << "status=" << controller_->status().toStdString() << " error=" << controller_->error().toStdString();
    EXPECT_TRUE(controller_->sourceSize().isEmpty());
    EXPECT_EQ(controller_->frameCount(), -1);
    EXPECT_EQ(controller_->presentation()->request.fullWidth, 1920);
    EXPECT_EQ(controller_->presentation()->request.fullHeight, 1080);
}

// The retired continuous wheel zoom stored a scale relative to the fitted
// image under a mode the retired selector could not state. That scale is
// representable now, so the project reopens at the scale the artist was looking
// at WITH the control stating it; a record that is genuinely unreadable still
// recovers to a fitted view rather than a blank or shrunken image the control
// calls "Fit".
TEST_F(ReadViewerSurface, RetiredContinuousZoomReopensAtItsScaleAndUnreadableStateFits) {
    workspace_->setPanelState(
        panel_, QVariantMap{{QStringLiteral("zoomMode"), QStringLiteral("Custom")}, {QStringLiteral("zoom"), 0.8}});
    QTest::qWait(50);

    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; })) << controller_->error().toStdString();

    auto* item = viewerItem();
    ASSERT_NE(item, nullptr);
    const double imageWidth = controller_->sourceSize().width();
    const double imageHeight = controller_->sourceSize().height();
    const double fitted =
        std::min(std::max(1.0, item->width() - 12.0) / imageWidth, std::max(1.0, item->height() - 12.0) / imageHeight);
    // The retired scale was a multiple of the fitted display scale, so 0.8
    // reopens at four fifths of the fitted size — and the control says so.
    EXPECT_NEAR(sampleView().scale(), 0.8 * fitted, 0.01);
    EXPECT_EQ(statedZoom(), percentText(0.8 * fitted));
    capture(QStringLiteral("retired-custom-zoom-stated"));

    // A record nothing can read resolves to the fitted view, and the media
    // covers a real share of the surface rather than a thumbnail.
    workspace_->setPanelState(panel_, QVariantMap{{QStringLiteral("zoomMode"), QStringLiteral("AnotherMode")},
                                                  {QStringLiteral("zoom"), 0.05}});
    QTest::qWait(80);
    EXPECT_EQ(statedZoom(), QStringLiteral("Fit"));
    EXPECT_NEAR(sampleView().scale(), fitted, 0.01);
    const auto expected = expectedDisplayRgb(plate);
    const auto area = grabImageArea();
    ASSERT_FALSE(area.isNull());
    const double coverage = static_cast<double>(countPixelsNear(area, expected, 12)) / (area.width() * area.height());
    EXPECT_GT(coverage, 0.15) << "an unreadable view record must not present a shrunken image; status="
                              << controller_->status().toStdString() << " coverage=" << coverage;
    capture(QStringLiteral("retired-custom-zoom-recovered"));
    EXPECT_EQ(warnings_->count(), 0);
}

// Local confirmation against the owner's supplied project/media (issue #74
// reproduction pointers). Skipped unless NEMO74_USER_DOC names a saved
// document; the deterministic test above is the CI-safe equivalent. Point
// NEMO74_USER_WORKSPACE at the application workspace and NEMO74_VIEWER_PANEL
// at its viewer panel id to reproduce the owner's exact panel state.
TEST_F(ReadViewerSurface, UserDocumentDisplaysItsAttachedRead) {
    const auto path = qEnvironmentVariable("NEMO74_USER_DOC");
    if (path.isEmpty())
        GTEST_SKIP() << "set NEMO74_USER_DOC to a saved document to confirm the supplied media";
    std::ifstream in(path.toStdString());
    ASSERT_TRUE(in) << path.toStdString();
    nlohmann::json json;
    in >> json;
    auto loaded = nemo::loadDocument(json);
    ASSERT_TRUE(session_->replaceDocument(std::move(loaded.document)).replaced);

    const auto panelId = qEnvironmentVariable("NEMO74_VIEWER_PANEL", QStringLiteral("panel-a"));
    auto* controller = controller_ ? controller_ : controllerFor(panelId);
    ASSERT_NE(controller, nullptr) << "no viewer controller for " << panelId.toStdString();

    ASSERT_TRUE(
        waitFor([&] { return controller->presentation() != nullptr || !controller->error().isEmpty(); }, 60000));
    const auto presentation = controller->presentation();
    std::cout << "user-document: has_presentation=" << (presentation != nullptr)
              << " destination=" << controller->hasDestination() << " status=" << controller->status().toStdString()
              << " error=" << controller->error().toStdString();
    if (presentation)
        std::cout << " raster=" << presentation->frame.width << 'x' << presentation->frame.height
                  << " full=" << presentation->request.imageWidth() << 'x' << presentation->request.imageHeight()
                  << " output=" << presentation->request.output;
    std::cout << " source=" << controller->sourceSize().width() << 'x' << controller->sourceSize().height()
              << " frames=" << controller->frameCount() << " rate=" << controller->frameRate() << '\n';
    ASSERT_TRUE(presentation);
    ASSERT_TRUE(controller->error().isEmpty()) << controller->error().toStdString();

    // The attached Read's own media drives the surface and the transport: the
    // owner's H.264 clip is 650 frames at 25 fps, never the 240-frame fallback.
    const auto expectedFrames = qEnvironmentVariableIntValue("NEMO74_EXPECT_FRAMES", nullptr);
    if (expectedFrames != 0) {
        EXPECT_EQ(controller->frameCount(), expectedFrames);
        EXPECT_EQ(controller->frameRate(), qEnvironmentVariable("NEMO74_EXPECT_RATE", QStringLiteral("25")).toDouble());
    }
    const auto imageArea = grabImageArea(panelId);
    ASSERT_FALSE(imageArea.isNull());
    EXPECT_GT(luminanceSpread(imageArea), 8.0)
        << "viewer media surface is flat; status=" << controller->status().toStdString();
    const auto name = qEnvironmentVariable("NEMO74_EVIDENCE_NAME", QStringLiteral("user-media"));
    capture(name, panelId);
}

// Issue #75 config freshness. The document and its temp config are prepared
// BEFORE any observing ViewerController exists, so the first session
// notification the tested controllers receive is the replacement itself — the
// exact sequence that a stale "first notification seeds the baseline" sentinel
// would skip.
// Issue #84 slice 2. A viewer wheel gesture is a continuous, cursor-anchored
// scale: it is monotonic, reversible, never resets the pan, states the scale it
// produced, and it writes the project once when it settles.
TEST_F(ReadViewerSurface, ViewerWheelZoomIsContinuousAnchoredAndReversible) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; })) << controller_->error().toStdString();

    auto* item = viewerItem();
    ASSERT_NE(item, nullptr);
    ASSERT_NE(zoomControl(), nullptr);
    // An untouched viewer opens fitted, and the control says so.
    EXPECT_EQ(statedZoom(), QStringLiteral("Fit"));

    const auto fitted = sampleView();
    ASSERT_GT(fitted.scale(), 0.0);
    // Anchor near (not at) the centre so the view clamp cannot move the anchor.
    const QPointF anchorLocal(item->width() / 2 + 10, item->height() / 2 + 6);
    const QPoint anchorScene = item->mapToScene(anchorLocal).toPoint();
    QTest::mouseMove(window_, anchorScene);
    QTest::qWait(20);
    // The image point under the pointer, before the gesture.
    const double imageX = fitted.region.x + (anchorLocal.x() - fitted.rect.x()) / fitted.scale();
    const double imageY = fitted.region.y + (anchorLocal.y() - fitted.rect.y()) / fitted.scale();

    const double factor = std::exp(53.0 * 0.002);
    wheel(anchorScene, 120);
    QTest::qWait(60);
    const auto zoomedIn = sampleView();
    // One notch is one proportional step in the indicated direction, not a jump
    // between two modes.
    EXPECT_NEAR(zoomedIn.scale(), fitted.scale() * factor, fitted.scale() * 0.01);
    // The pixel under the pointer stays under the pointer.
    const auto anchored = zoomedIn.panelPoint(imageX, imageY);
    EXPECT_NEAR(anchored.x(), anchorLocal.x(), 2.0);
    EXPECT_NEAR(anchored.y(), anchorLocal.y(), 2.0);
    // The control states the scale the transform is drawing.
    EXPECT_EQ(statedZoom(), percentText(zoomedIn.scale()));
    const auto image = grabImageArea();
    ASSERT_FALSE(image.isNull());
    const auto expected = expectedDisplayRgb(plate);
    EXPECT_GT(countPixelsNear(image, expected, 12), 300)
        << "the zoomed view must still show the retained media; status=" << controller_->status().toStdString();
    capture(QStringLiteral("viewer-zoom-one-notch"));

    // Opposite deltas return to the same view, so the gesture is reversible and
    // zoom is not a one-way mode change.
    const auto fittedCentre =
        fitted.panelPoint(fitted.region.x + fitted.region.width / 2.0, fitted.region.y + fitted.region.height / 2.0);
    wheel(anchorScene, -120);
    QTest::qWait(60);
    const auto restored = sampleView();
    EXPECT_NEAR(restored.scale(), fitted.scale(), fitted.scale() * 0.01);
    const auto restoredCentre =
        restored.panelPoint(fitted.region.x + fitted.region.width / 2.0, fitted.region.y + fitted.region.height / 2.0);
    EXPECT_NEAR(restoredCentre.x(), fittedCentre.x(), 2.0);
    EXPECT_NEAR(restoredCentre.y(), fittedCentre.y(), 2.0);

    // A burst settles on the scale the artist indicated, however many notches it
    // carried, and writes the project once.
    int writes = 0;
    // A local context object: the connection must not outlive this body, or a
    // state write from panel teardown would call into dead locals.
    QObject writesScope;
    QObject::connect(workspace_.get(), &nemo::workspace::WorkspaceController::panelStateChanged, &writesScope,
                     [&](const QString& changed) {
                         if (changed == panel_)
                             ++writes;
                     });
    for (int notch = 0; notch < 6; ++notch)
        wheel(anchorScene, 120);
    QTest::qWait(400);
    EXPECT_EQ(writes, 1) << "a wheel burst must write the project once, not per input event";
    const auto settled = sampleView();
    EXPECT_NEAR(settled.scale(), fitted.scale() * std::exp(53.0 * 0.002 * 6.0), fitted.scale() * 0.02);
    EXPECT_EQ(statedZoom(), percentText(settled.scale()));
    const auto state = workspace_->panelState(panel_);
    EXPECT_EQ(state.value(QStringLiteral("zoomMode")).toString(), QStringLiteral("Scale"));
    EXPECT_NEAR(state.value(QStringLiteral("zoom")).toDouble(), settled.scale(), 0.05)
        << "the settled view is what the project records";
    capture(QStringLiteral("viewer-zoom-burst-settled"));
    EXPECT_EQ(warnings_->count(), 0);
}

// The viewer's view drives the evaluation request: fitting keeps the whole image
// at full sampling, zooming in requests the visible region at full sampling, and
// zooming far out lets the existing policy reduce sampling for a small display
// area. An explicit resolution mode keeps overriding all of it.
TEST_F(ReadViewerSurface, ViewerViewDrivesTheRequestRegionAndSampling) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.region.width == 96 && request.region.height == 64 && request.samplingScale == 1;
    })) << controller_->error().toStdString();

    auto* item = viewerItem();
    ASSERT_NE(item, nullptr);
    const QPoint anchorScene = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseMove(window_, anchorScene);
    QTest::qWait(20);

    // Zoomed in: the request carries the visible sub-region at full sampling,
    // inside the unchanged full image domain.
    for (int notch = 0; notch < 8; ++notch)
        wheel(anchorScene, 120);
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.region.width < 96 && request.samplingScale == 1;
    })) << controller_->error().toStdString()
        << " status=" << controller_->status().toStdString();
    const auto zoomedIn = presentedRequest();
    EXPECT_EQ(zoomedIn.fullWidth, 96);
    EXPECT_EQ(zoomedIn.fullHeight, 64);
    EXPECT_LT(zoomedIn.region.width, 96);
    EXPECT_GE(zoomedIn.region.x, 0);
    EXPECT_LE(zoomedIn.region.x + zoomedIn.region.width, 96);
    EXPECT_GT(zoomedIn.region.height, 0);
    capture(QStringLiteral("viewer-zoom-region"));

    // A pan moves the requested region and the displayed image without changing
    // the scale.
    const auto beforePan = sampleView();
    const auto anchorImage = beforePan.panelPoint(beforePan.region.x + beforePan.region.width / 2.0,
                                                  beforePan.region.y + beforePan.region.height / 2.0);
    const auto beforeScale = beforePan.scale();
    const int regionBefore = presentedRequest().region.x;
    drag(item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(),
         item->mapToScene(QPointF(item->width() / 2 + 24, item->height() / 2)).toPoint());
    ASSERT_TRUE(waitForRequest([&](const nemo::EvaluationRequest& request) {
        return request.region.x != regionBefore && request.samplingScale == 1;
    })) << controller_->error().toStdString();
    const auto afterPan = sampleView();
    EXPECT_NEAR(afterPan.scale(), beforeScale, beforeScale * 0.01) << "a pan must not change the zoom";
    const auto movedPoint = afterPan.panelPoint(beforePan.region.x + beforePan.region.width / 2.0,
                                                beforePan.region.y + beforePan.region.height / 2.0);
    EXPECT_NEAR(movedPoint.x() - anchorImage.x(), 24.0, 2.0) << "the image follows the drag one to one";
    capture(QStringLiteral("viewer-pan-region"));

    // Zoomed far enough out, the existing policy reduces sampling for the small
    // display area while the region is the whole image again.
    for (int notch = 0; notch < 60 && sampleView().scale() * 96 > 24; ++notch) {
        wheel(anchorScene, -120);
        QTest::qWait(5);
    }
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.samplingScale > 1 && request.region.width == 96;
    })) << "status="
        << controller_->status().toStdString();
    capture(QStringLiteral("viewer-zoom-out-sampling"));

    // An explicit resolution mode still overrides the policy at the same view.
    controller_->setResolutionMode(QStringLiteral("full"));
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) { return request.samplingScale == 1; }))
        << controller_->error().toStdString();
    EXPECT_EQ(presentedRequest().region.width, 96);
    EXPECT_EQ(warnings_->count(), 0);
}

// Panning the image is an explicit drag at any scale, on the left button as
// before and on the middle button as the graph already allows; it keeps the
// zoom, leaves the document alone, and writes the view once per gesture.
TEST_F(ReadViewerSurface, ViewerDragPanKeepsTheZoomAndTheDocument) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; })) << controller_->error().toStdString();

    auto* item = viewerItem();
    ASSERT_NE(item, nullptr);
    const QPoint center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseMove(window_, center);
    for (int notch = 0; notch < 8; ++notch)
        wheel(center, 120);
    // Let the burst settle before counting, so the gesture's own write is not
    // charged to the pan below.
    QTest::qWait(300);
    const auto zoomed = sampleView();
    ASSERT_GT(zoomed.scale(), 0.0);

    int writes = 0;
    // A local context object: the connection must not outlive this body, or a
    // state write from panel teardown would call into dead locals.
    QObject writesScope;
    QObject::connect(workspace_.get(), &nemo::workspace::WorkspaceController::panelStateChanged, &writesScope,
                     [&](const QString& changed) {
                         if (changed == panel_)
                             ++writes;
                     });
    const auto revision = session_->revision();
    const QPointF localCenter(item->width() / 2, item->height() / 2);

    // A fixed image point: its panel position is what a drag translates, and it
    // does not depend on which region the presented frame happens to carry.
    const double imageX = zoomed.region.x + zoomed.region.width / 2.0;
    const double imageY = zoomed.region.y + zoomed.region.height / 2.0;
    drag(item->mapToScene(localCenter).toPoint(), item->mapToScene(localCenter + QPointF(30, 0)).toPoint(),
         Qt::LeftButton);
    const auto afterLeft = sampleView();
    EXPECT_NEAR(afterLeft.scale(), zoomed.scale(), zoomed.scale() * 0.01) << "panning must not discard the zoom";
    EXPECT_NEAR(afterLeft.panelPoint(imageX, imageY).x() - zoomed.panelPoint(imageX, imageY).x(), 30.0, 2.0);
    EXPECT_EQ(writes, 1) << "one released pan is one persisted view";

    drag(item->mapToScene(localCenter).toPoint(), item->mapToScene(localCenter + QPointF(0, 20)).toPoint(),
         Qt::MiddleButton);
    const auto afterMiddle = sampleView();
    EXPECT_NEAR(afterMiddle.scale(), zoomed.scale(), zoomed.scale() * 0.01);
    EXPECT_NEAR(afterMiddle.panelPoint(imageX, imageY).y() - afterLeft.panelPoint(imageX, imageY).y(), 20.0, 2.0);
    EXPECT_EQ(writes, 2);
    EXPECT_EQ(session_->revision(), revision) << "view gestures are not document edits";
    const auto state = workspace_->panelState(panel_);
    EXPECT_NEAR(state.value(QStringLiteral("panX")).toDouble(), controller_->pan().x(), 0.6);
    capture(QStringLiteral("viewer-panned"));
    EXPECT_EQ(warnings_->count(), 0);
}

// The zoom control states the scale the panel is drawing, accepts a stated
// percentage and keeps the presets; a project's saved scale reopens with the
// control stating it, and a record that cannot be represented resolves to a
// fitted view rather than an image the artist cannot see.
TEST_F(ReadViewerSurface, ViewerZoomControlStatesAndAcceptsAnyScale) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    // A record that states an absolute scale, as this control's own writes do.
    workspace_->setPanelState(
        panel_, QVariantMap{{QStringLiteral("zoomMode"), QStringLiteral("Scale")}, {QStringLiteral("zoom"), 2.5}});
    QTest::qWait(40);
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; })) << controller_->error().toStdString();

    auto* item = viewerItem();
    ASSERT_NE(item, nullptr);
    auto* control = zoomControl();
    ASSERT_NE(control, nullptr);
    const double imageWidth = controller_->sourceSize().width();
    const double imageHeight = controller_->sourceSize().height();
    const double fittedScale =
        std::min(std::max(1.0, item->width() - 12.0) / imageWidth, std::max(1.0, item->height() - 12.0) / imageHeight);
    // Reopened at the saved scale, and the control states it.
    EXPECT_NEAR(sampleView().scale(), 2.5, 0.01);
    EXPECT_EQ(statedZoom(), QStringLiteral("250%"));
    capture(QStringLiteral("viewer-saved-scale"));

    // A directly stated percentage is applied as stated.
    auto* field = visualByName(window_->contentItem(), control->objectName() + QStringLiteral("Field"));
    ASSERT_NE(field, nullptr);
    field->forceActiveFocus();
    QTest::qWait(20);
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    for (const auto digit : {Qt::Key_1, Qt::Key_5, Qt::Key_0})
        QTest::keyClick(window_, digit);
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(80);
    EXPECT_NEAR(sampleView().scale(), 1.5, 0.01) << "a stated percentage is applied as stated";
    EXPECT_EQ(statedZoom(), QStringLiteral("150%"));

    // A stated scale outside what the request can carry is clamped to the
    // nearest representable scale, and the control states the clamped value:
    // the readout never disagrees with the image.
    field->forceActiveFocus();
    QTest::qWait(20);
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    for (const auto digit : {Qt::Key_1, Qt::Key_0})
        QTest::keyClick(window_, digit);
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::qWait(80);
    // The representable range is the controller's own, expressed through the
    // documented conversion: 0.05x..32x of the image fitted into the panel's
    // whole area (the panel's own fitted display keeps its 6 px margin).
    const double controllerFit =
        std::min(item->width() / (imageWidth * controller_->pixelAspect()), item->height() / imageHeight);
    EXPECT_NEAR(sampleView().scale(), 0.05 * controllerFit, 0.01);
    EXPECT_EQ(statedZoom(), percentText(0.05 * controllerFit));

    // A preset from the control's own menu re-fits the image, and the fitted
    // view is the accepted one: the media keeps its 6 px margin, the whole image
    // is requested, and the sampling stays full.
    pickZoomPreset(control, 0);
    QTest::qWait(80);
    EXPECT_EQ(statedZoom(), QStringLiteral("Fit"));
    EXPECT_NEAR(sampleView().scale(), fittedScale, 0.01);
    EXPECT_EQ(presentedRequest().samplingScale, 1);
    EXPECT_EQ(presentedRequest().region.width, 96);
    capture(QStringLiteral("viewer-preset-fit"));

    // The stated scale survives further gestures: zooming with the wheel from a
    // stated percentage keeps the control honest.
    const QPoint center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseMove(window_, center);
    wheel(center, 120);
    QTest::qWait(80);
    EXPECT_EQ(statedZoom(), percentText(sampleView().scale()));
    EXPECT_EQ(warnings_->count(), 0);
}

// Issue #85's coverage switch, driven through the real control. Zoomed in, the
// request follows the visible region; the switch makes it cover the whole image
// domain at the same sampling scale and the same view, and switching back
// restores exactly the regional coverage the view asks for. The choice is a
// panel-state record like the view itself: never a document edit and never a
// reason to recenter or refit.
TEST_F(ReadViewerSurface, ForceFullFrameCoversTheWholeDomainAndKeepsTheView) {
    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto blur =
        controller_->createGraphNode(rootNetwork(), QStringLiteral("blur"), QStringLiteral("Blur1"), 40.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, blur, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), blur, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.region.width == 96 && request.region.height == 64 && request.samplingScale == 1;
    })) << controller_->error().toStdString();

    auto* item = viewerItem();
    ASSERT_NE(item, nullptr);
    auto* toggle = visualByName(window_->contentItem(), QStringLiteral("viewerFullFrame_") + panel_);
    ASSERT_NE(toggle, nullptr) << "the coverage switch is a control of the real panel";
    ASSERT_TRUE(toggle->isVisible());
    // Off by default, and the request follows the visible region.
    EXPECT_FALSE(controller_->forceFullFrame());
    EXPECT_FALSE(toggle->property("checked").toBool());

    const QPoint anchor = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseMove(window_, anchor);
    QTest::qWait(20);
    for (int notch = 0; notch < 8; ++notch)
        wheel(anchor, 120);
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.region.width > 0 && request.region.width < 96 && request.samplingScale == 1;
    })) << controller_->error().toStdString()
        << " status=" << controller_->status().toStdString();
    const auto regional = presentedRequest();
    const auto regionalView = sampleView();
    const double regionalZoom = controller_->zoom();
    const QPointF regionalPan = controller_->pan();
    capture(QStringLiteral("viewer-full-frame-regional"), {}, false, true);

    // The real control: hovered, then clicked.
    const QPoint toggleCenter = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint();
    QTest::mouseMove(window_, toggleCenter);
    QTest::qWait(80);
    EXPECT_TRUE(toggle->property("hovered").toBool());
    capture(QStringLiteral("viewer-full-frame-hover"), {}, true, true);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, toggleCenter);
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.region.x == 0 && request.region.y == 0 && request.region.width == 96 &&
               request.region.height == 64;
    })) << controller_->error().toStdString()
        << " status=" << controller_->status().toStdString();

    // Whole-domain coverage at the SAME sampling density and the SAME view: the
    // switch states coverage, not quality and not a display transform.
    const auto whole = presentedRequest();
    EXPECT_EQ(whole.samplingScale, regional.samplingScale);
    EXPECT_EQ(whole.fullWidth, 96);
    EXPECT_EQ(whole.fullHeight, 64);
    EXPECT_TRUE(controller_->forceFullFrame());
    EXPECT_TRUE(toggle->property("checked").toBool());
    EXPECT_EQ(controller_->zoom(), regionalZoom);
    EXPECT_EQ(controller_->pan(), regionalPan);
    EXPECT_NEAR(sampleView().scale(), regionalView.scale(), regionalView.scale() * 0.01);
    EXPECT_EQ(statedZoom(), percentText(regionalView.scale()));
    capture(QStringLiteral("viewer-full-frame-checked"), {}, false, true);

    // The chosen sampling mode is untouched by the switch: the same whole-domain
    // coverage is requested through an explicit proxy mode.
    controller_->setResolutionMode(QStringLiteral("half"));
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) { return request.samplingScale == 2; }))
        << controller_->error().toStdString();
    EXPECT_EQ(presentedRequest().region.x, 0);
    EXPECT_EQ(presentedRequest().region.y, 0);
    EXPECT_EQ(presentedRequest().region.width, 96);
    EXPECT_EQ(presentedRequest().region.height, 64);
    controller_->setResolutionMode(QStringLiteral("auto"));
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) { return request.samplingScale == 1; }))
        << controller_->error().toStdString();

    // Switched off through the same control, the view asks for exactly the
    // regional coverage it asked for before. The wait is on the new frame, so a
    // click that changed nothing cannot pass on the frame already displayed.
    const auto beforeOffClick = controller_->presentation();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, toggleCenter);
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != beforeOffClick; }))
        << controller_->error().toStdString();
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) { return request.region.width < 96; }))
        << controller_->error().toStdString();
    EXPECT_FALSE(controller_->forceFullFrame());
    EXPECT_FALSE(toggle->property("checked").toBool());
    EXPECT_TRUE(presentedRequest() == regional) << "the view's own coverage is restored unchanged";
    EXPECT_NEAR(sampleView().scale(), regionalView.scale(), regionalView.scale() * 0.01);
    EXPECT_EQ(workspace_->panelState(panel_).value(QStringLiteral("forceFullFrame")).toBool(), false);

    // The panel-state record is what a reopened panel restores from, and
    // adopting it re-derives the request without touching the view.
    QVariantMap recorded = workspace_->panelState(panel_);
    ASSERT_TRUE(recorded.contains(QStringLiteral("zoomMode"))) << "the record carries the view too";
    recorded.insert(QStringLiteral("forceFullFrame"), true);
    const auto viewBeforeRecord = sampleView();
    // A fixed image point: where the panel draws it is what "not recentered"
    // means, and it does not depend on which region the frame carries.
    const double imageX = viewBeforeRecord.region.x + viewBeforeRecord.region.width / 2.0;
    const double imageY = viewBeforeRecord.region.y + viewBeforeRecord.region.height / 2.0;
    const QPointF pointBeforeRecord = viewBeforeRecord.panelPoint(imageX, imageY);
    workspace_->setPanelState(panel_, recorded);
    ASSERT_TRUE(waitFor([&] { return controller_->forceFullFrame(); }));
    ASSERT_TRUE(waitForRequest([](const nemo::EvaluationRequest& request) {
        return request.region.x == 0 && request.region.y == 0 && request.region.width == 96 &&
               request.region.height == 64;
    })) << controller_->error().toStdString();
    EXPECT_TRUE(toggle->property("checked").toBool()) << "the control follows the restored record";
    const auto viewAfterRecord = sampleView();
    EXPECT_NEAR(viewAfterRecord.scale(), viewBeforeRecord.scale(), viewBeforeRecord.scale() * 0.01);
    const QPointF pointAfterRecord = viewAfterRecord.panelPoint(imageX, imageY);
    EXPECT_NEAR(pointAfterRecord.x(), pointBeforeRecord.x(), 1.0) << "the restore never recenters the view";
    EXPECT_NEAR(pointAfterRecord.y(), pointBeforeRecord.y(), 1.0);
    EXPECT_EQ(controller_->zoom(), regionalZoom);
    EXPECT_NEAR(controller_->pan().x(), regionalPan.x(), 0.5);
    EXPECT_NEAR(controller_->pan().y(), regionalPan.y(), 0.5);
    EXPECT_EQ(warnings_->count(), 0);
}

// Issue #85: the coverage switch belongs to one panel. Two viewers of the same
// graph keep independent coverage, and turning the switch on in one of them
// leaves its sibling's request, view and control exactly as they were.
class ForceFullFrameSplitSurface : public ReadViewerSurface {
protected:
    ForceFullFrameSplitSurface() : ReadViewerSurface() {
        prepareWorkspaceDocument = [] {
            const auto viewerPanel = [](const QString& id, const QString& group, int viewerIndex) {
                return QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("type"), QStringLiteral("viewer")},
                    {QStringLiteral("group"), group},
                    {QStringLiteral("state"), QJsonObject{{QStringLiteral("viewerIndex"), viewerIndex}}}};
            };
            const auto leaf = [](const QString& id, const QString& active, const QJsonObject& onlyPanel) {
                return QJsonObject{{QStringLiteral("id"), id},
                                   {QStringLiteral("kind"), QStringLiteral("tabs")},
                                   {QStringLiteral("active"), active},
                                   {QStringLiteral("panels"), QJsonArray{onlyPanel}}};
            };
            const QJsonObject layout{
                {QStringLiteral("version"), 1},
                {QStringLiteral("root"),
                 QJsonObject{{QStringLiteral("id"), QStringLiteral("split-root")},
                             {QStringLiteral("kind"), QStringLiteral("split")},
                             {QStringLiteral("orientation"), QStringLiteral("horizontal")},
                             {QStringLiteral("ratio"), 0.3},
                             {QStringLiteral("children"),
                              QJsonArray{leaf(QStringLiteral("leaf-a"), QStringLiteral("panel-a"),
                                              viewerPanel(QStringLiteral("panel-a"), QStringLiteral("A"), 0)),
                                         leaf(QStringLiteral("leaf-b"), QStringLiteral("panel-b"),
                                              viewerPanel(QStringLiteral("panel-b"), QStringLiteral("B"), 1))}}}}};
            return QJsonObject{{QStringLiteral("version"), 2},
                               {QStringLiteral("activeWorkspaceId"), QStringLiteral("workspace-1")},
                               {QStringLiteral("workspaces"),
                                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-1")},
                                                       {QStringLiteral("name"), QStringLiteral("Coverage split")},
                                                       {QStringLiteral("layout"), layout}}}}};
        };
    }
};

TEST_F(ForceFullFrameSplitSurface, ForceFullFrameStaysInsideItsPanel) {
    auto* sibling = controllerFor(QStringLiteral("panel-b"));
    ASSERT_NE(sibling, nullptr) << "the second panel owns its own controller";
    ASSERT_NE(sibling, controller_);

    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto scope = rootNetwork();
    const auto read =
        controller_->createGraphNode(scope, QStringLiteral("source"), QStringLiteral("Read1"), 0.0, 0.0, {}, {});
    const auto blur =
        controller_->createGraphNode(scope, QStringLiteral("blur"), QStringLiteral("Blur1"), 40.0, 0.0, {}, {});
    const auto viewerA =
        controller_->createGraphNode(scope, QStringLiteral("viewer"), QStringLiteral("ViewerA"), 40.0, 120.0, {}, {});
    const auto viewerB =
        controller_->createGraphNode(scope, QStringLiteral("viewer"), QStringLiteral("ViewerB"), 120.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(scope, read, 0, blur, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(scope, blur, 0, viewerA, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(scope, blur, 0, viewerB, 0));
    ASSERT_TRUE(readSource_->setSourcePath(scope, read, QString::fromStdString(plate.string())));
    // Both viewers address the same blur output through their own Viewer node:
    // panel A selects viewer index 0, panel B index 1.
    ASSERT_TRUE(waitFor([&] {
        const auto presented = sibling->presentation();
        return presented && presented->request.region.width == 96 && presented->request.region.height == 64;
    })) << sibling->error().toStdString();

    // Zoom both panels in, so each asks for a region of the image rather than
    // the whole of it.
    for (const auto& panelId : {QStringLiteral("panel-a"), QStringLiteral("panel-b")}) {
        auto* item = viewerItem(panelId);
        ASSERT_NE(item, nullptr);
        const QPoint anchor = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QTest::mouseMove(window_, anchor);
        QTest::qWait(20);
        for (int notch = 0; notch < 8; ++notch)
            wheel(anchor, 120);
        ASSERT_TRUE(waitForRequest(
            [](const nemo::EvaluationRequest& request) {
                return request.region.width > 0 && request.region.width < 96 && request.samplingScale == 1;
            },
            panelId))
            << panelId.toStdString()
            << " status=" << (panelId == panel_ ? controller_->status() : sibling->status()).toStdString();
    }
    const auto regionalSibling = presentedRequest(QStringLiteral("panel-b"));
    const auto siblingView = sampleView(QStringLiteral("panel-b"));

    auto* toggle = visualByName(window_->contentItem(), QStringLiteral("viewerFullFrame_panel-a"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_TRUE(toggle->isVisible());
    auto* siblingToggle = visualByName(window_->contentItem(), QStringLiteral("viewerFullFrame_panel-b"));
    ASSERT_NE(siblingToggle, nullptr);
    // The split makes panel A narrower than the compact threshold and panel B
    // wider, so the switch is exercised in both layouts: it moves below the
    // image in the narrow panel and stays in the wide panel's header, as one
    // and the same display-control row.
    auto* narrowPanel = panelRootFor(window_->contentItem(), QStringLiteral("panel-a"));
    auto* widePanel = panelRootFor(window_->contentItem(), QStringLiteral("panel-b"));
    ASSERT_NE(narrowPanel, nullptr);
    ASSERT_NE(widePanel, nullptr);
    EXPECT_LT(narrowPanel->width(), 560.0) << "the narrow panel carries its display controls below the image";
    EXPECT_GE(widePanel->width(), 560.0) << "the wide panel keeps them in its header";
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                      toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint());
    ASSERT_TRUE(waitForRequest(
        [](const nemo::EvaluationRequest& request) {
            return request.region.x == 0 && request.region.y == 0 && request.region.width == 96 &&
                   request.region.height == 64;
        },
        panel_))
        << controller_->error().toStdString();

    EXPECT_TRUE(controller_->forceFullFrame());
    EXPECT_FALSE(sibling->forceFullFrame()) << "the switch is panel-local";
    EXPECT_FALSE(siblingToggle->property("checked").toBool());
    capture(QStringLiteral("viewer-full-frame-narrow-checked"), QStringLiteral("panel-a"));
    EXPECT_TRUE(presentedRequest(QStringLiteral("panel-b")) == regionalSibling) << "the sibling's request is untouched";
    EXPECT_NEAR(sampleView(QStringLiteral("panel-b")).scale(), siblingView.scale(), siblingView.scale() * 0.01);
    EXPECT_EQ(workspace_->panelState(panel_).value(QStringLiteral("forceFullFrame")).toBool(), true);
    EXPECT_FALSE(workspace_->panelState(QStringLiteral("panel-b")).value(QStringLiteral("forceFullFrame")).toBool())
        << "the sibling records nothing";
    EXPECT_EQ(warnings_->count(), 0);
}

class PreparedConfigSurface : public ReadViewerSurface {
protected:
    std::filesystem::path config_;
    QString network_;
    QString read_;

    void SetUp() override {
        // The configuration is installed before any config-dependent owner is
        // constructed; only the document is authored afterwards, still before
        // any observing controller exists.
        prepareConfig = [this] {
            config_ = std::filesystem::path(directory_.path().toStdString()) / "ocio" / "color.ocio";
            std::filesystem::create_directories(config_.parent_path());
            writeGammaConfig(config_, "2.2");
            EXPECT_TRUE(std::filesystem::exists(config_));
            qputenv("OCIO", config_.string().c_str());
            session_->setColorConfigPath(config_.string());
        };
        prepareSession = [this] { prepareLoadedProject(); };
        ReadViewerSurface::SetUp();
    }

    void prepareLoadedProject() {
        const auto png = writePng(directory_.path().toStdString(), "gamma-plate", 96, 64,
                                  {kConfigSampleR, kConfigSampleG, kConfigSampleB, 1.0F});
        network_ = QString::number(session_->document().rootNetworkId());
        const auto readId = std::make_shared<nemo::NodeId>();
        const auto viewerId = std::make_shared<nemo::NodeId>();
        EXPECT_TRUE(session_
                        ->submit(nemo::addNodeCommand(network_.toULongLong(), "source", "Read1", readId),
                                 {.expectedRevision = session_->revision()})
                        .committed);
        EXPECT_TRUE(session_
                        ->submit(nemo::addNodeCommand(network_.toULongLong(), "viewer", "Viewer1", viewerId),
                                 {.expectedRevision = session_->revision()})
                        .committed);
        read_ = QString::number(*readId);
        EXPECT_TRUE(readSource_->setSourcePath(network_, read_, QString::fromStdString(png.string())))
            << readSource_->error().toStdString();
        EXPECT_TRUE(waitFor([&] {
            return readSource_->info(network_, read_).value(QStringLiteral("state")).toString() ==
                   QStringLiteral("ready");
        })) << readSource_->info(network_, read_).value(QStringLiteral("error")).toString().toStdString();
        // An explicit named input override, so neither PNG metadata nor a file
        // rule decides the interpretation: the named space's gamma rewrite is
        // what must move the pixels.
        const nemo::ParameterAddress transformAddress{network_.toULongLong(), *readId, "inputTransform",
                                                      nemo::kInvalidNetworkInstance};
        const nemo::ParameterAddress spaceAddress{network_.toULongLong(), *readId, "inputColorSpace",
                                                  nemo::kInvalidNetworkInstance};
        EXPECT_TRUE(session_
                        ->submit(nemo::setParametersCommand(
                                     {{transformAddress, nemo::ParameterValue{nemo::ChoiceValue{"explicit"}}},
                                      {spaceAddress, nemo::ParameterValue{std::string{"rec709_texture"}}}}),
                                 {.expectedRevision = session_->revision()})
                        .committed);
        EXPECT_TRUE(session_
                        ->submit(nemo::connectCommand(network_.toULongLong(), {*readId, 0}, {*viewerId, 0}),
                                 {.expectedRevision = session_->revision()})
                        .committed);
        // The authored policy names the same working space the temp config
        // exposes, so the display expectation below is the document's own.
        nemo::ColorPolicy policy;
        policy.workingSpace = "working_rec709";
        policy.viewerTransform = "sRGB/rec709";
        policy.deliveryTransform = "sRGB/rec709";
        EXPECT_TRUE(session_->submit(nemo::setColorPolicyCommand(policy), {.expectedRevision = session_->revision()})
                        .committed);
    }
};

TEST_F(PreparedConfigSurface, SamePathConfigRewriteRefreshesOnFirstReplacement) {
    // The revision was captured inside SetUp at the real construction boundary
    // (after the preparation hook, before any observing ViewerController). The
    // test asserts it never moves before the replacement, so the replacement
    // really is the first session notification: a delayed media probe
    // completion, a stray authoring edit, or anything during window creation
    // and event pumping would move it and mask the regression this test exists
    // to catch.
    const auto boundaryRevision = constructionBoundaryRevision;

    // The prepared, untouched project renders at its initial gamma. Render
    // requests are not document mutations.
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; }))
        << controller_->error().toStdString() << " status=" << controller_->status().toStdString();
    const auto beforeExpected = expectedGammaDisplay(2.2);
    auto image = grabPanel();
    ASSERT_FALSE(image.isNull());
    EXPECT_GT(countPixelsNear(image, beforeExpected, 12), 300)
        << "the plate must render its computed display color; expected " << beforeExpected[0] << ','
        << beforeExpected[1] << ',' << beforeExpected[2] << " status=" << controller_->status().toStdString();
    capture(QStringLiteral("config-before-rewrite"));
    EXPECT_EQ(session_->revision(), boundaryRevision)
        << "the baseline render must not mutate the document, or the replacement below is not the first "
           "session notification";

    // The external edit: the SAME path, different content. Rewriting the
    // config file is not a document mutation.
    writeGammaConfig(config_, "2.8");
    EXPECT_EQ(session_->revision(), boundaryRevision) << "only the config file changed before the replacement boundary";
    const auto afterExpected = expectedGammaDisplay(2.8);
    ASSERT_NE(beforeExpected, afterExpected) << "the gamma rewrite must move the expected display color";
    // The changed red channel alone exceeds the matching tolerance, so a stale
    // render can never be counted as the refreshed one.
    ASSERT_GT(std::abs(beforeExpected[0] - afterExpected[0]), 12);

    // The FIRST session notification these controllers receive: the replacement
    // advances the project generation and must refresh the runtime's retained
    // OCIO processors on its worker.
    const auto revision = session_->revision();
    ASSERT_TRUE(session_->replaceDocument(session_->snapshot(), {}, {}, config_.string()).replaced);
    EXPECT_GT(session_->revision(), revision);

    ASSERT_TRUE(waitFor([&] {
        const auto current = grabPanel();
        return !current.isNull() && countPixelsNear(current, afterExpected, 12) > 300 &&
               countPixelsNear(current, beforeExpected, 12) == 0;
    })) << "the first replacement after a same-path config edit did not refresh the retained OCIO processors; "
           "expected "
        << afterExpected[0] << ',' << afterExpected[1] << ',' << afterExpected[2]
        << " status=" << controller_->status().toStdString() << " error=" << controller_->error().toStdString();
    image = grabPanel();
    EXPECT_GT(countPixelsNear(image, afterExpected, 12), 300);
    EXPECT_EQ(countPixelsNear(image, beforeExpected, 12), 0);
    capture(QStringLiteral("config-after-rewrite"));

    // Unrelated authored state survives the replacement: the Read still names
    // its own media and the project still carries the same config reference.
    const auto values = session_->queryValues(network_.toULongLong(), read_.toULongLong(), "source");
    ASSERT_FALSE(values.empty());
    EXPECT_FALSE(std::get<std::string>(values.front().value).empty());
    const auto transform = session_->queryValues(network_.toULongLong(), read_.toULongLong(), "inputColorSpace");
    ASSERT_FALSE(transform.empty());
    EXPECT_EQ(std::get<std::string>(transform.front().value), "rec709_texture");
    EXPECT_EQ(session_->colorConfigPath(), config_.string());
    EXPECT_EQ(warnings_->count(), 0);
}

}  // namespace
