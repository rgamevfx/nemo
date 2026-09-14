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

        session_ = std::make_unique<ProjectSession>();
        importer_ = std::make_unique<nemo::media::MediaImportService>();
        media_ = std::make_unique<nemo::ui::MediaLibraryModel>(*session_, *importer_);
        chooser_ = std::make_unique<nemo::ui::NativeFileChooser>();
        readSource_ = std::make_unique<nemo::ui::ReadSourceController>(*session_, *media_, *chooser_);
        router_ = std::make_unique<nemo::ui::PanelContextRouter>(*session_);
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
            QFile workspaceFile(workspacePath);
            ASSERT_TRUE(workspaceFile.open(QIODevice::WriteOnly));
            workspaceFile.write(QJsonDocument(document).toJson());
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
    // panel background; a displayed image introduces non-background pixels.
    [[nodiscard]] QImage grabPanel(const QString& panelId = {}) {
        auto* panel = panelRootFor(window_->contentItem(), panelId.isEmpty() ? panel_ : panelId);
        if (!panel)
            return {};
        QTest::mouseMove(window_, QPoint(4, 4));
        QTest::qWait(50);
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

    // Native capture for the recorded evidence directory, when one is set.
    void capture(const QString& name, const QString& panelId = {}) {
        if (evidenceDirectory_.isEmpty())
            return;
        QDir().mkpath(evidenceDirectory_);
        EXPECT_TRUE(grabPanel(panelId).save(evidenceDirectory_ + '/' + name + ".png"));
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

    ASSERT_TRUE(readSource_->clearSource(rootNetwork(), read));
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

// A project that persisted the retired continuous wheel zoom (a mode the
// selector cannot represent) must not reopen as a shrunken, effectively blank
// image labelled "Fit": zoom is mode-driven, so an unknown mode recovers at
// Fit and the control tells the truth.
TEST_F(ReadViewerSurface, RetiredContinuousZoomStateStillFitsTheImage) {
    workspace_->setPanelState(
        panel_, QVariantMap{{QStringLiteral("zoomMode"), QStringLiteral("Custom")}, {QStringLiteral("zoom"), 0.05}});
    QTest::qWait(50);

    const auto plate = writePng(directory_.path().toStdString(), "plate", 96, 64, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto read = controller_->createGraphNode(rootNetwork(), QStringLiteral("source"), QStringLiteral("Read1"),
                                                   0.0, 0.0, {}, {});
    const auto viewer = controller_->createGraphNode(rootNetwork(), QStringLiteral("viewer"), QStringLiteral("Viewer1"),
                                                     40.0, 120.0, {}, {});
    ASSERT_TRUE(controller_->connectOrReplaceGraph(rootNetwork(), read, 0, viewer, 0));
    ASSERT_TRUE(readSource_->setSourcePath(rootNetwork(), read, QString::fromStdString(plate.string())));
    ASSERT_TRUE(waitFor([&] { return controller_->presentation() != nullptr; })) << controller_->error().toStdString();

    const auto expected = expectedDisplayRgb(plate);
    // A retired continuous zoom drew the image at 18% of fit — a ~40 px thumb
    // in a ~500 px area — so this asserts the image covers a real share of the
    // media surface, not merely that its pixels exist somewhere.
    const auto area = grabImageArea(panel_);
    ASSERT_FALSE(area.isNull());
    const double coverage = static_cast<double>(countPixelsNear(area, expected, 12)) / (area.width() * area.height());
    EXPECT_GT(coverage, 0.15) << "a retired custom zoom reopened as a shrunken image; status="
                              << controller_->status().toStdString() << " coverage=" << coverage;
    capture(QStringLiteral("retired-custom-zoom-recovered"));
    auto* selector = visualByName(window_->contentItem(), "viewerZoomMenu_" + panel_);
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(selector->property("currentText").toString(), QStringLiteral("Fit"));
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

}  // namespace
