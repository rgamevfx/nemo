// Native installed-extension workflow regression (issue #37, acceptance
// examples 1, 3, 5 and 7).
//
// The reference workflow is driven through the real presentation the
// application composes: the installed package's node is created from the shared
// graph catalog, its mesh editor is hosted by an ordinary Parameters card, and
// the demonstration panel is an ordinary workspace panel with the shared shell,
// docking and persistence. Every edit is real pointer/key input through the
// shared session/gesture/history seam, and every assertion is about authored
// document state, the package's own published mapping, the persisted workspace
// record, or the pixels the viewer really presents — never about an internal
// helper or a registration call.
//
// It runs only on a real windowing platform with the Vulkan scene graph and a
// separately built + installed package:
//
//   cmake --build build/debug --target nemo_workspace_ui_tests
//   cmake -S examples/colorwarp -B build/colorwarp \
//       -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
//   cmake --build build/colorwarp --target colorwarp_package
//   NEMO_TEST_EXTENSION_ROOT="$PWD/build/colorwarp/package" \
//   NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 \
//   NEMO37_EVIDENCE_DIR=docs/evidence/issue37-native \
//   QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 \
//   build/debug/tests/nemo_workspace_ui_tests --gtest_filter='*Issue37*'
//
// The package is built and installed OUTSIDE this suite: it is located through
// NEMO_TEST_EXTENSION_ROOT (unset means skip; set but unusable means failure,
// reported with the loader's own diagnostics). The installed package's QML
// imports the same linked `Nemo` QML module the application ships.
//
// The absent/restored halves of acceptance example 7 are the two halves of one
// restart: AbsentPackageSurface opens the saved layout while the package is not
// installed, and RestoredPackageSurface opens the SAME layout with the package
// present. Neither half fabricates production content.

#include "HistoryController.hpp"
#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ParameterEditorRegistry.hpp"
#include "ParameterInteraction.hpp"
#include "ProjectFileController.hpp"
#include "ScopedEnvironment.hpp"
#include "ViewerController.hpp"
#include "ViewerControllerRegistry.hpp"
#include "ViewerRuntime.hpp"
#include "ViewportPicker.hpp"
#include "WorkspaceController.hpp"

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/extensions/GpuPackages.hpp"
#include "nemo/extensions/InstalledPackages.hpp"
#include "nemo/gpu/Error.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef NEMO_SLANG_SPV_DIR
#define NEMO_SLANG_SPV_DIR ""
#endif

Q_IMPORT_QML_PLUGIN(NemoPlugin)

namespace {

// The installed demo package's PUBLISHED QML surface. This scenario is the
// package's own workflow, so its object names are the handles an artist's
// pointer reaches; every host seam exercised below stays generic.
QString editorName(const QString& node) {
    return QStringLiteral("colorWarpEditor_") + node;
}
QString wheelName(const QString& node) {
    return QStringLiteral("colorWarpWheel_") + node;
}
QString pinName(const QString& node) {
    return QStringLiteral("colorWarpPin_") + node;
}
QString resetSelectedName(const QString& node) {
    return QStringLiteral("colorWarpResetSelected_") + node;
}
QString resetAllName(const QString& node) {
    return QStringLiteral("colorWarpResetAll_") + node;
}
QString noticeName(const QString& node) {
    return QStringLiteral("colorWarpNotice_") + node;
}
QString panelBodyName() {
    return QStringLiteral("colorWarpExamplePanel");
}

QQuickItem* visualByName(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualByName(child, name))
            return found;
    }
    return nullptr;
}

int countByName(QQuickItem* root, const QString& name) {
    if (!root)
        return 0;
    int found = root->objectName() == name ? 1 : 0;
    for (auto* child : root->childItems())
        found += countByName(child, name);
    return found;
}

// The first item carrying `property` == `value`: an open menu entry that only
// states wording is reached by that wording, not by an object name.
QQuickItem* visualWithProperty(QQuickItem* root, const char* property, const QVariant& value) {
    if (!root)
        return nullptr;
    if (root->property(property) == value)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualWithProperty(child, property, value))
            return found;
    }
    return nullptr;
}

// One key event per character, so a typed value really travels the same key
// path an artist's keyboard does.
void typeText(QWindow* window, const QString& text) {
    for (const QChar character : text)
        QTest::keyClick(window, character.toLatin1());
}

nemo::NetworkId networkIdentity(const QString& value) {
    return static_cast<nemo::NetworkId>(value.toULongLong());
}

nemo::NodeId nodeIdentity(const QString& value) {
    return static_cast<nemo::NodeId>(value.toULongLong());
}

// One QML function read back as a value. The package's own published mapping is
// what the artist's pointer is interpreted through, so the scenario addresses
// handles and states its expectations through it instead of re-deriving its math.
QVariant callFunction(QQuickItem* item, const char* name) {
    QVariant result;
    const QMetaObject* meta = item ? item->metaObject() : nullptr;
    if (!meta)
        return result;
    for (int index = 0; index < meta->methodCount(); ++index) {
        const auto candidate = meta->method(index);
        if (candidate.name() != name || candidate.parameterCount() != 0)
            continue;
        candidate.invoke(item, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result));
        break;
    }
    return result;
}

QVariant callFunction(QQuickItem* item, const char* name, const QVariant& first) {
    QVariant result;
    if (!item)
        return result;
    QMetaObject::invokeMethod(item, name, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, first));
    return result;
}

QVariant callFunction(QQuickItem* item, const char* name, const QVariant& first, const QVariant& second) {
    QVariant result;
    if (!item)
        return result;
    QMetaObject::invokeMethod(item, name, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, first),
                              Q_ARG(QVariant, second));
    return result;
}

QVariant callFunction(QQuickItem* item, const char* name, const QVariant& first, const QVariant& second,
                      const QVariant& third) {
    QVariant result;
    if (!item)
        return result;
    QMetaObject::invokeMethod(item, name, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, first),
                              Q_ARG(QVariant, second), Q_ARG(QVariant, third));
    return result;
}

QJsonObject panelRecord(const QString& id, const QString& type, const QString& group, const QJsonObject& state = {}) {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("type"), type},
                       {QStringLiteral("group"), group},
                       {QStringLiteral("state"), state}};
}

QJsonObject tabsNode(const QString& id, const QJsonArray& panels, const QString& active) {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("kind"), QStringLiteral("tabs")},
                       {QStringLiteral("active"), active},
                       {QStringLiteral("panels"), panels}};
}

QJsonObject splitNode(const QString& id, const QString& orientation, double ratio, const QJsonArray& children) {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("kind"), QStringLiteral("split")},
                       {QStringLiteral("orientation"), orientation},
                       {QStringLiteral("ratio"), ratio},
                       {QStringLiteral("children"), children}};
}

// The workspace document the fixture starts from, in the format the workspace
// controller itself persists.
QJsonObject workspaceDocument(const QString& name, const QJsonObject& root) {
    const QJsonObject layout{{QStringLiteral("version"), 1}, {QStringLiteral("root"), root}};
    return QJsonObject{
        {QStringLiteral("version"), 2},
        {QStringLiteral("activeWorkspaceId"), QStringLiteral("workspace-1")},
        {QStringLiteral("workspaces"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-1")},
                                                              {QStringLiteral("name"), name},
                                                              {QStringLiteral("layout"), layout}}}}};
}

class ExtensionSurface : public testing::Test {
protected:
    QTemporaryDir directory_;
    // Set before any other owner so the environment is restored last.
    std::unique_ptr<nemo::test::ScopedEnvironment> colorEnvironment_;
    // The composed installed-package inventory. Declared first so it is
    // destroyed after every contribution user (session, runtime, editors).
    std::unique_ptr<nemo::extensions::InstalledPackages> packages_;
    std::unique_ptr<nemo::ui::ViewerRuntime> runtime_;
    std::unique_ptr<nemo::ProjectSession> session_;
    nemo::ui::ParameterInteraction interaction_;
    std::unique_ptr<nemo::ui::HistoryController> history_;
    std::unique_ptr<nemo::ui::PanelContextRouter> router_;
    std::unique_ptr<nemo::ui::ViewerController> facade_;
    std::unique_ptr<nemo::ui::ViewerControllerRegistry> registry_;
    std::unique_ptr<nemo::workspace::WorkspaceController> workspace_;
    std::unique_ptr<nemo::ui::NativeFileChooser> chooser_;
    std::unique_ptr<nemo::ui::ProjectFileController> projectFile_;
    std::unique_ptr<nemo::ui::ParameterEditorRegistry> editors_;
    std::unique_ptr<nemo::ui::ViewportPicker> viewportPicker_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    std::unique_ptr<QSignalSpy> warnings_;
    QQuickWindow* window_{};
    QString viewerPanelId_{QStringLiteral("panel-viewer")};
    QString parametersPanelId_{QStringLiteral("panel-params")};
    QString graphPanelId_{QStringLiteral("panel-graph")};
    QString installedPanelId_{QStringLiteral("panel-colorwarp")};
    nemo::ui::ViewerController* controller_{};
    QString network_;
    QString plateId_;
    QString viewerId_;
    QString installedNodeId_;
    QString evidenceDirectory_;
    // The discovered installed package surface.
    QString installedNodeType_;
    QString installedNodeGroup_;
    QString installedPanelType_;
    QString installedPanelTitle_;
    QString installedEditorId_;
    // True when the machine has no usable GPU device, so setup stops after the
    // runtime reports it instead of running the scenario against nothing.
    bool gpuUnavailable_{false};

    // --- composition hooks -------------------------------------------------
    // Node graph over a viewer on the left, the shared Parameters panel on the
    // right: the catalog the artist creates from and the card the package's
    // editor is hosted by, side by side.
    virtual QJsonObject initialLayout() const {
        return splitNode(
            QStringLiteral("split-root"), QStringLiteral("horizontal"), 0.66,
            QJsonArray{
                splitNode(QStringLiteral("split-left"), QStringLiteral("vertical"), 0.45,
                          QJsonArray{tabsNode(QStringLiteral("leaf-graph"),
                                              QJsonArray{panelRecord(graphPanelId_, QStringLiteral("nodegraph"),
                                                                     QStringLiteral("A"))},
                                              graphPanelId_),
                                     tabsNode(QStringLiteral("leaf-viewer"),
                                              QJsonArray{panelRecord(viewerPanelId_, QStringLiteral("viewer"),
                                                                     QStringLiteral("A"),
                                                                     QJsonObject{{QStringLiteral("viewerIndex"), 0}})},
                                              viewerPanelId_)}),
                tabsNode(QStringLiteral("leaf-params"),
                         QJsonArray{panelRecord(parametersPanelId_, QStringLiteral("parameters"), QStringLiteral("A"),
                                                QJsonObject{{QStringLiteral("inspectors"), QJsonArray{}},
                                                            {QStringLiteral("columns"), false}})},
                         parametersPanelId_)});
    }
    // Whether the installed package's panel contribution is registered with the
    // workspace, i.e. whether the package is present at startup.
    virtual bool packagePresent() const { return true; }

    void SetUp() override {
        const bool nativeRequested = qEnvironmentVariableIntValue("NEMO_TEST_NATIVE_UI") == 1 &&
                                     qEnvironmentVariableIntValue("NEMO_TEST_VIEWER_WINDOW") == 1;
        if (!nativeRequested) {
            GTEST_SKIP() << "the installed-extension workflow requires NEMO_TEST_NATIVE_UI=1 "
                            "NEMO_TEST_VIEWER_WINDOW=1";
        }
        // Native evidence was asked for: a platform that presents no scene-graph
        // content is a rejection, never a silent pass.
        const QString platform = QGuiApplication::platformName();
        ASSERT_FALSE(platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal"))
            << "native evidence was requested but the platform is '" << platform.toStdString()
            << "', which presents no scene-graph content";
        ASSERT_FALSE(std::string(NEMO_SLANG_SPV_DIR).empty())
            << "the viewer cannot render the installed node's output without compiled Slang shaders";
        if (qEnvironmentVariable("NEMO_TEST_EXTENSION_ROOT").isEmpty())
            GTEST_SKIP() << "set NEMO_TEST_EXTENSION_ROOT to the root the demo package is installed under";
        evidenceDirectory_ = qEnvironmentVariable("NEMO37_EVIDENCE_DIR");

        const auto config = std::filesystem::path(NEMO_UI_QML_DIR).parent_path().parent_path().parent_path() /
                            "docs/evidence/issue12-view.ocio";
        colorEnvironment_ = std::make_unique<nemo::test::ScopedEnvironment>("OCIO", config.string());

        discoverPackage();
        if (testing::Test::HasFatalFailure())
            return;
        composeOwners();
        if (testing::Test::HasFatalFailure() || gpuUnavailable_)
            return;
        composeGraph();
        if (testing::Test::HasFatalFailure())
            return;
        composeEditors();
        if (testing::Test::HasFatalFailure())
            return;
        loadWindow();
    }

    void TearDown() override {
        if (runtime_)
            runtime_->stopWorker();
        warnings_.reset();
        engine_.reset();
        viewportPicker_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (runtime_)
            runtime_->quiesceForTeardown();
        editors_.reset();
        projectFile_.reset();
        chooser_.reset();
        registry_.reset();
        facade_.reset();
        workspace_.reset();
        router_.reset();
        history_.reset();
        session_.reset();
        runtime_.reset();
        packages_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        colorEnvironment_.reset();
    }

    // --- composition -------------------------------------------------------
    [[nodiscard]] std::string packageDiagnostics() const {
        std::string joined;
        for (const std::string& diagnostic : packages_->diagnostics()) {
            if (!joined.empty())
                joined += "; ";
            joined += diagnostic;
        }
        return joined.empty() ? std::string{"no diagnostic"} : joined;
    }

    // The installed package, discovered through the same loader the application
    // composes. A configured root that yields nothing usable is a failure, never
    // a skip, and the loader's own diagnostics are the failure message.
    void discoverPackage() {
        std::filesystem::path root{qEnvironmentVariable("NEMO_TEST_EXTENSION_ROOT").toStdString()};
        if (std::filesystem::exists(root / "manifest.json"))
            root = root.parent_path();
        ASSERT_TRUE(std::filesystem::is_directory(root))
            << "NEMO_TEST_EXTENSION_ROOT is set but is not a directory: " << root.string();
        if (!packagePresent()) {
            // This saved workspace names the published example type. The host
            // inventory itself is empty, not merely an omitted UI registration.
            installedPanelType_ = QStringLiteral("org.nemo.colorwarp.example");
            installedPanelTitle_ = QStringLiteral("ColorWarp Extension Example");
            packages_ = std::make_unique<nemo::extensions::InstalledPackages>(
                std::vector<std::filesystem::path>{directory_.filePath(QStringLiteral("absent")).toStdString()});
            return;
        }
        packages_ = std::make_unique<nemo::extensions::InstalledPackages>(std::vector<std::filesystem::path>{root});
        ASSERT_FALSE(packages_->panels().empty())
            << "no installed package with a panel contribution was discovered under " << root.string()
            << "; loader diagnostics: " << packageDiagnostics();

        const auto& panel = packages_->panels().front();
        installedPanelType_ = QString::fromStdString(panel.id);
        installedPanelTitle_ = QString::fromStdString(panel.title);
        ASSERT_FALSE(installedPanelType_.isEmpty());
        const QUrl panelSource(QString::fromStdString(panel.source));
        ASSERT_TRUE(panelSource.isLocalFile())
            << "a panel contribution's source must be an absolute file URL, got: " << panel.source;
        ASSERT_TRUE(QFileInfo::exists(panelSource.toLocalFile()))
            << "the installed panel's QML is missing: " << panelSource.toLocalFile().toStdString();

        // The installed node contribution: every entry the built-in inventory
        // does not already own came from an installed package.
        const auto builtins = nemo::builtinNodeContributions();
        const nemo::NodeContribution* installed = nullptr;
        for (const auto& entry : packages_->contributions()->entries()) {
            if (builtins->find(entry.descriptor.type) != nullptr)
                continue;
            installed = &entry;
            break;
        }
        ASSERT_NE(installed, nullptr) << "the installed package under " << root.string()
                                      << " supplies a panel but no node contribution; loader diagnostics: "
                                      << packageDiagnostics();
        installedNodeType_ = QString::fromStdString(installed->descriptor.type);
        installedNodeGroup_ = QString::fromStdString(installed->descriptor.group);
        for (const auto& editor : installed->editors) {
            if (editor.presentation != "section")
                continue;
            installedEditorId_ = QString::fromStdString(editor.id);
            const QUrl editorSource(QString::fromStdString(editor.source));
            ASSERT_TRUE(editorSource.isLocalFile())
                << "a section editor's source must be an absolute file URL, got: " << editor.source;
            ASSERT_TRUE(QFileInfo::exists(editorSource.toLocalFile()))
                << "the installed editor's QML is missing: " << editorSource.toLocalFile().toStdString();
        }
        ASSERT_FALSE(installedEditorId_.isEmpty())
            << "the installed node type " << installedNodeType_.toStdString()
            << " declares no section editor; loader diagnostics: " << packageDiagnostics();
    }

    void composeOwners() {
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
            // The application's own assembly: the built-in projection plus every
            // installed package's GPU callbacks and node metadata.
            runtime_->bootstrap(extensions, NEMO_SLANG_SPV_DIR, cacheOptions,
                                nemo::extensions::gpuContributions(*packages_));
        } catch (const nemo::gpu::GpuException& error) {
            if (error.errorCode() == nemo::gpu::GpuError::NoDevice) {
                gpuUnavailable_ = true;
                GTEST_SKIP() << error.what();
                return;
            }
            throw;
        }

        // The session is created from the COMPOSED catalog, so the installed
        // node type is a first-class part of the project model.
        const auto contributions = packages_->contributions();
        session_ = std::make_unique<nemo::ProjectSession>(nemo::Document(contributions->catalog()), 256, contributions);
        history_ = std::make_unique<nemo::ui::HistoryController>(*session_);
        router_ = std::make_unique<nemo::ui::PanelContextRouter>(*session_);
        facade_ = std::make_unique<nemo::ui::ViewerController>(runtime_.get(), *session_, interaction_);
        registry_ = std::make_unique<nemo::ui::ViewerControllerRegistry>(runtime_.get(), *session_, interaction_);

        const auto workspacePath = directory_.filePath(QStringLiteral("workspace.json"));
        const QJsonObject document = workspaceDocument(QStringLiteral("Installed extension"), initialLayout());
        QFile workspaceFile(workspacePath);
        ASSERT_TRUE(workspaceFile.open(QIODevice::WriteOnly));
        workspaceFile.write(QJsonDocument(document).toJson());
        workspaceFile.close();

        workspace_ = std::make_unique<nemo::workspace::WorkspaceController>(workspacePath);
        ASSERT_TRUE(workspace_->error().isEmpty()) << workspace_->error().toStdString();
        // The production descriptors: built-ins first, then the installed
        // package's panels through the SAME panel-type registry.
        workspace_->registerPanelType(QStringLiteral("viewer"), QStringLiteral("Viewer"),
                                      QStringLiteral("ViewerPanel.qml"), QString());
        workspace_->registerPanelType(QStringLiteral("nodegraph"), QStringLiteral("Node graph"),
                                      QStringLiteral("GraphPanel.qml"), QString());
        workspace_->registerPanelType(QStringLiteral("parameters"), QStringLiteral("Parameters"),
                                      QStringLiteral("ParametersPanel.qml"), QString());
        if (packagePresent()) {
            workspace_->registerPanelType(installedPanelType_, installedPanelTitle_,
                                          QString::fromStdString(packages_->panels().front().source), QString());
            ASSERT_TRUE(workspace_->error().isEmpty())
                << "the installed panel type must register through the shared registry: "
                << workspace_->error().toStdString();
        }
        router_->setWorkspaceController(workspace_.get());

        controller_ = qobject_cast<nemo::ui::ViewerController*>(registry_->controller(viewerPanelId_));
        ASSERT_NE(controller_, nullptr);
        ASSERT_TRUE(controller_->destination().has_value());
        network_ = controller_->rootNetworkId();
    }

    void composeGraph() {
        plateId_ = controller_->createGraphNode(network_, QStringLiteral("testpattern"), QStringLiteral("plate"), 0.0,
                                                0.0, {}, {});
        ASSERT_FALSE(plateId_.isEmpty());
        viewerId_ = controller_->createGraphNode(network_, QStringLiteral("viewer"), QStringLiteral("view1"), 0.0,
                                                 180.0, {}, {});
        ASSERT_FALSE(viewerId_.isEmpty());
    }

    void composeEditors() {
        chooser_ = std::make_unique<nemo::ui::NativeFileChooser>();
        projectFile_ = std::make_unique<nemo::ui::ProjectFileController>(*session_, *workspace_, *router_, *chooser_);
        editors_ = std::make_unique<nemo::ui::ParameterEditorRegistry>();
        // Every composed contribution registers, exactly as main.cpp does: the
        // installed package's editor sits beside the built-in ones.
        for (const auto& contribution : packages_->contributions()->entries()) {
            for (const auto& editor : contribution.editors) {
                QStringList consumes;
                for (const auto& key : editor.consumes)
                    consumes.push_back(QString::fromStdString(key));
                ASSERT_TRUE(editors_->registerEditor(QString::fromStdString(editor.id),
                                                     QUrl(QString::fromStdString(editor.source)), consumes,
                                                     QString::fromStdString(editor.presentation)))
                    << "editor " << editor.id
                    << " was refused: " << editors_->reason(QString::fromStdString(editor.id)).toStdString();
            }
        }
    }

    void addQmlImportPaths() {
        engine_->addImportPath(QStringLiteral("qrc:/qt/qml"));
        ASSERT_TRUE(QFile::exists(QStringLiteral(":/qt/qml/Nemo/qmldir")))
            << "the application's linked Nemo QML module is unavailable";
    }

    void loadWindow() {
        engine_ = std::make_unique<QQmlApplicationEngine>();
        warnings_ = std::make_unique<QSignalSpy>(engine_.get(), &QQmlEngine::warnings);
        addQmlImportPaths();
        viewportPicker_ = std::make_unique<nemo::ui::ViewportPicker>(*runtime_, *facade_, *session_);
        engine_->rootContext()->setContextProperty(QStringLiteral("workspace"), workspace_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("historyController"), history_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("panelContextRouter"), router_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("projectFile"), projectFile_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewerController"), facade_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewerControllers"), registry_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("parameterEditors"), editors_.get());
        engine_->rootContext()->setContextProperty(QStringLiteral("viewportPicker"), viewportPicker_.get());
        engine_->load(QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/Main.qml")));
        ASSERT_FALSE(engine_->rootObjects().isEmpty());
        window_ = qobject_cast<QQuickWindow*>(engine_->rootObjects().constFirst());
        ASSERT_NE(window_, nullptr);
        window_->resize(1600, 960);

        const QString attachError = runtime_->attachToWindow(window_);
        ASSERT_TRUE(attachError.isEmpty()) << attachError.toStdString();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window_));
    }

    // --- scene helpers -----------------------------------------------------
    QQuickItem* item(const QString& name) const { return visualByName(window_->contentItem(), name); }

    QPoint center(QQuickItem* target) const {
        return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
    }

    QPoint center(const QString& name) const {
        auto* target = item(name);
        EXPECT_NE(target, nullptr) << name.toStdString();
        return target ? center(target) : QPoint();
    }

    QQuickItem* parametersPanel() const { return item(QStringLiteral("parametersPanel")); }

    bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 120000) const {
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < timeoutMs) {
            if (predicate())
                return true;
            QTest::qWait(10);
        }
        return predicate();
    }

    void settle(int milliseconds = 60) const { QTest::qWait(milliseconds); }

    // --- document helpers --------------------------------------------------
    double authoredNumber(const QString& node, const QString& key) const {
        for (const auto& value :
             session_->queryValues(networkIdentity(network_), nodeIdentity(node), key.toStdString())) {
            if (value.key != key.toStdString())
                continue;
            if (const auto* number = std::get_if<double>(&value.value))
                return *number;
            if (const auto* integer = std::get_if<std::int64_t>(&value.value))
                return static_cast<double>(*integer);
        }
        return std::nan("");
    }

    bool authoredFlag(const QString& node, const QString& key) const {
        for (const auto& value :
             session_->queryValues(networkIdentity(network_), nodeIdentity(node), key.toStdString())) {
            if (value.key != key.toStdString())
                continue;
            if (const auto* flag = std::get_if<bool>(&value.value))
                return *flag;
        }
        return false;
    }

    // The value the shared inspector query states for one key at the current
    // frame — the same query the card renders its rows from.
    double inspectorValue(const QString& node, const QString& key) const {
        const auto inspector = controller_->parameterInspector(network_, node);
        for (const auto& section : inspector.value(QStringLiteral("sections")).toList()) {
            for (const auto& parameter : section.toMap().value(QStringLiteral("parameters")).toList()) {
                const auto row = parameter.toMap();
                if (row.value(QStringLiteral("key")).toString() == key)
                    return row.value(QStringLiteral("value")).toDouble();
            }
        }
        return std::nan("");
    }

    // The node id of the installed type in the shared graph, or empty.
    QString installedNodeFromGraph() const {
        for (const auto& value : controller_->graphNodes()) {
            const auto node = value.toMap();
            if (node.value(QStringLiteral("type")).toString() == installedNodeType_)
                return node.value(QStringLiteral("id")).toString();
        }
        return {};
    }

    // --- layout helpers ----------------------------------------------------
    QVariantMap leafContaining(const QVariantMap& node, const QString& panelId) const {
        if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("tabs")) {
            for (const auto& value : node.value(QStringLiteral("panels")).toList()) {
                if (value.toMap().value(QStringLiteral("id")).toString() == panelId)
                    return node;
            }
            return {};
        }
        for (const auto& child : node.value(QStringLiteral("children")).toList()) {
            const auto found = leafContaining(child.toMap(), panelId);
            if (!found.isEmpty())
                return found;
        }
        return {};
    }

    QVariantMap leafOf(const QString& panelId) const { return leafContaining(workspace_->root(), panelId); }

    QVariantMap panelRecordOf(const QString& panelId) const {
        for (const auto& value : leafOf(panelId).value(QStringLiteral("panels")).toList()) {
            const auto record = value.toMap();
            if (record.value(QStringLiteral("id")).toString() == panelId)
                return record;
        }
        return {};
    }

    // The split that directly owns `childId`, i.e. the node whose ratio the
    // divider between two panes edits.
    bool parentSplitOf(const QVariantMap& node, const QString& childId, QVariantMap* found) const {
        if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("split"))
            return false;
        for (const auto& value : node.value(QStringLiteral("children")).toList()) {
            const auto child = value.toMap();
            if (child.value(QStringLiteral("id")).toString() == childId) {
                *found = node;
                return true;
            }
            if (parentSplitOf(child, childId, found))
                return true;
        }
        return false;
    }

    int leafCount(const QVariantMap& node) const {
        if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("tabs"))
            return 1;
        int total = 0;
        for (const auto& child : node.value(QStringLiteral("children")).toList())
            total += leafCount(child.toMap());
        return total;
    }

    // --- input helpers -----------------------------------------------------
    void moveTo(const QPoint& position, Qt::MouseButton held = Qt::LeftButton) {
        QMouseEvent move(QEvent::MouseMove, QPointF(position), QPointF(window_->mapToGlobal(position)), Qt::NoButton,
                         held, Qt::NoModifier);
        QGuiApplication::sendEvent(window_, &move);
        QTest::qWait(5);
    }

    void drag(const QPoint& from, const QPoint& to, Qt::MouseButton button = Qt::LeftButton, int steps = 8) {
        QTest::mousePress(window_, button, Qt::NoModifier, from);
        for (int step = 1; step <= steps; ++step) {
            const QPoint position(from.x() + (to.x() - from.x()) * step / steps,
                                  from.y() + (to.y() - from.y()) * step / steps);
            moveTo(position, button);
        }
        QTest::mouseRelease(window_, button, Qt::NoModifier, to);
        settle();
    }

    void clickNamed(const QString& name) {
        auto* target = item(name);
        ASSERT_NE(target, nullptr) << name.toStdString();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(target));
        settle();
    }

    void typeInto(const QString& fieldName, const QString& text) {
        clickNamed(fieldName);
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        typeText(window_, text);
        QTest::keyClick(window_, Qt::Key_Return);
        settle(60);
    }

    // --- panel shell helpers -----------------------------------------------
    void openPanelTypeMenu(const QString& panelId) {
        clickNamed(QStringLiteral("panelType_") + panelId);
        ASSERT_TRUE(waitFor(
            [&] {
                auto* menu = window_->findChild<QObject*>(QStringLiteral("panelTypeMenu_") + panelId);
                return menu && menu->property("visible").toBool();
            },
            2000))
            << "the shared panel type menu must open";
    }

    void choosePanelType(const QString& panelId, const QString& typeId) {
        openPanelTypeMenu(panelId);
        clickNamed(QStringLiteral("panelTypeChoice_") + typeId + QStringLiteral("_") + panelId);
    }

    // One entry of the shared panel menu, addressed by its own stated wording.
    void clickPanelMenuEntry(const QString& panelId, const QString& text) {
        openPanelTypeMenu(panelId);
        auto* entry = visualWithProperty(window_->contentItem(), "text", text);
        ASSERT_NE(entry, nullptr) << "the shared panel menu must offer '" << text.toStdString() << "'";
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(entry));
        settle(80);
    }

    // --- inspector helpers -------------------------------------------------
    void inspect(const QString& node) {
        auto* panel = parametersPanel();
        ASSERT_NE(panel, nullptr);
        ASSERT_TRUE(waitFor([&] { return panel->property("stateReady").toBool(); }, 2000));
        QVariant result;
        ASSERT_TRUE(QMetaObject::invokeMethod(panel, "openInspector", Qt::DirectConnection,
                                              Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, network_),
                                              Q_ARG(QVariant, node)));
        settle();
    }

    // The package's mesh editor, mounted by the ordinary Parameters card.
    QQuickItem* meshEditor() const { return item(editorName(installedNodeId_)); }

    // The handle's own drawn position, stated by the package's published
    // mapping, converted to window coordinates.
    QPoint handlePoint(int index) const {
        auto* editor = meshEditor();
        auto* wheelItem = item(wheelName(installedNodeId_));
        if (!editor || !wheelItem)
            return {};
        const auto point = callFunction(editor, "knotPoint", QVariant(index)).toMap();
        return wheelItem
            ->mapToScene(
                QPointF(point.value(QStringLiteral("x")).toDouble(), point.value(QStringLiteral("y")).toDouble()))
            .toPoint();
    }

    // --- evidence ----------------------------------------------------------
    void capture(const QString& name, bool parkPointer = true) const {
        if (evidenceDirectory_.isEmpty())
            return;
        QDir().mkpath(evidenceDirectory_);
        if (parkPointer)
            QTest::mouseMove(window_, QPoint(4, 4));
        settle(90);
        EXPECT_TRUE(window_->grabWindow().save(evidenceDirectory_ + '/' + name + ".png"));
    }

    // The image area one viewer panel really presents, in window coordinates.
    QRect viewerImageRect(const QString& panelId) const {
        auto* image = item(QStringLiteral("viewerItem_") + panelId);
        if (!image)
            return {};
        return image->mapRectToScene(image->property("displayRect").toRectF())
            .toRect()
            .intersected(QRect(0, 0, window_->width(), window_->height()));
    }

    // A presented frame with the pointer parked, so a hover affordance can never
    // be mistaken for the deformation under test.
    QImage presentedFrame(const QString& panelId) const {
        QTest::mouseMove(window_, QPoint(4, 4));
        settle(150);
        const QRect rect = viewerImageRect(panelId);
        if (rect.isEmpty())
            return {};
        return window_->grabWindow().copy(rect);
    }

    // Mean absolute per-channel difference between two presented frames.
    static double frameDifference(const QImage& before, const QImage& after) {
        if (before.isNull() || after.isNull() || before.size() != after.size())
            return -1.0;
        double total = 0.0;
        for (int y = 0; y < before.height(); ++y) {
            for (int x = 0; x < before.width(); ++x) {
                const QRgb left = before.pixel(x, y);
                const QRgb right = after.pixel(x, y);
                total += std::abs(qRed(left) - qRed(right)) + std::abs(qGreen(left) - qGreen(right)) +
                         std::abs(qBlue(left) - qBlue(right));
            }
        }
        return total / (static_cast<double>(before.width()) * before.height() * 3.0);
    }

    // A presented frame that really carries the graph's image rather than an
    // empty surface: the pattern's own pixels vary across the frame.
    static bool carriesContent(const QImage& image) {
        if (image.isNull() || image.width() < 8 || image.height() < 8)
            return false;
        const QRgb first = image.pixel(0, 0);
        for (int y = 0; y < image.height(); y += 4) {
            for (int x = 0; x < image.width(); x += 4) {
                if (image.pixel(x, y) != first)
                    return true;
            }
        }
        return false;
    }

    void awaitInstalledFrame() {
        ASSERT_TRUE(waitFor(
            [&] {
                const auto shown = controller_->presentation();
                return shown && shown->revision == session_->document().stateRevision() &&
                       controller_->viewerTargetId() == installedNodeId_ && !controller_->outdated() &&
                       QString::number(static_cast<qulonglong>(shown->request.output)) == installedNodeId_;
            },
            10000))
            << "the viewer must present the installed node's current document output: "
            << controller_->error().toStdString();
        settle(200);
    }

    void expectNoQmlWarnings() const {
        for (const auto& warning : *warnings_) {
            for (const auto& error : warning.front().value<QList<QQmlError>>())
                ADD_FAILURE() << error.toString().toStdString();
        }
    }
};

// Acceptance examples 1 and 5: the installed node is created from the shared
// catalog, its mesh editor is the ordinary inspector card's control, a real
// pointer drag authors the mesh through the shared gesture (one undo entry), and
// the deformation the panel draws is the deformation the viewer really presents.
TEST_F(ExtensionSurface, Issue37InstalledMeshEditorAuthorsThroughTheSharedInspector) {
    // The catalog the artist creates from is the composed one: the installed
    // node type is listed by the graph panel's own Tools menu.
    bool catalogHasInstalledNode = false;
    for (const auto& value : controller_->nodeCatalog()) {
        const auto descriptor = value.toMap();
        if (descriptor.value(QStringLiteral("type")).toString() != installedNodeType_)
            continue;
        catalogHasInstalledNode = true;
        EXPECT_EQ(descriptor.value(QStringLiteral("group")).toString(), installedNodeGroup_);
    }
    ASSERT_TRUE(catalogHasInstalledNode) << "the composed catalog must list the installed node type "
                                         << installedNodeType_.toStdString();

    auto* tools = item(QStringLiteral("graphToolsButton"));
    ASSERT_NE(tools, nullptr) << "the graph panel must offer its shared catalog";
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(tools));
    settle(80);
    clickNamed(QStringLiteral("toolCategory_") + installedNodeGroup_);
    clickNamed(QStringLiteral("toolNode_") + installedNodeType_);
    installedNodeId_ = installedNodeFromGraph();
    ASSERT_FALSE(installedNodeId_.isEmpty())
        << "creating " << installedNodeType_.toStdString() << " from the catalog must add it to the graph";

    // plate -> installed node -> viewer, through the shared command path.
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, plateId_, 0, installedNodeId_, 0));
    ASSERT_TRUE(controller_->connectOrReplaceGraph(network_, installedNodeId_, 0, viewerId_, 0));
    controller_->setActiveViewer(network_, 0);
    awaitInstalledFrame();
    const QImage identityFrame = presentedFrame(viewerPanelId_);
    ASSERT_FALSE(identityFrame.isNull());
    ASSERT_TRUE(carriesContent(identityFrame)) << "the viewer must present the graph's image, not an empty surface";

    // The installed editor is hosted by the ordinary card, and the strength
    // setting it presents is the host's own shared numeric control.
    inspect(installedNodeId_);
    ASSERT_TRUE(waitFor([&] { return meshEditor() != nullptr; }, 3000))
        << "the installed section editor must mount on the shared inspector card";
    auto* editor = meshEditor();
    ASSERT_TRUE(editor->isVisible());
    auto* wheel = item(wheelName(installedNodeId_));
    ASSERT_NE(wheel, nullptr);
    ASSERT_TRUE(wheel->isVisible());
    EXPECT_EQ(
        countByName(window_->contentItem(), QStringLiteral("param_") + installedNodeId_ + QStringLiteral("_strength")),
        1)
        << "exactly one strength control may render";
    const QRectF wheelScene = wheel->mapRectToScene(QRectF(0, 0, wheel->width(), wheel->height()));
    ASSERT_TRUE(QRectF(0, 0, window_->width(), window_->height()).contains(wheelScene.center()))
        << "the mounted wheel must be on screen for a real gesture";

    // --- selection and a real drag -----------------------------------------
    const int index = 12;  // middle ring: the plate's chroma overlaps this knot
    const QPoint handle = handlePoint(index);
    ASSERT_FALSE(handle.isNull());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, handle);
    settle(40);
    EXPECT_EQ(editor->property("selectedIndex").toInt(), index) << "a real press selects the handle under the pointer";
    auto* selection = item(QStringLiteral("colorWarpSelection_") + installedNodeId_);
    ASSERT_NE(selection, nullptr);
    EXPECT_FALSE(selection->property("text").toString().isEmpty());
    // The wheel's own pixels, taken with the selection already drawn, so the
    // comparison below can only see the mesh the drag authors.
    const QRect wheelRect = wheelScene.toRect().intersected(QRect(0, 0, window_->width(), window_->height()));
    const QImage wheelBefore = window_->grabWindow().copy(wheelRect);
    capture(QStringLiteral("issue37-mesh-identity-full"));

    // The move distance follows the platform's own drag threshold while keeping
    // the authored displacement inside the validator's fold bound, so this is a
    // genuinely accepted edit rather than a refused one.
    const int threshold = controller_->dragDistance();
    const int distance = std::max(threshold + 1, 11);
    const QPoint end(handle.x(), handle.y() - distance);

    const auto revisionBefore = session_->revision();
    const QString hueKey = QStringLiteral("hue%1").arg(index);
    const QString saturationKey = QStringLiteral("saturation%1").arg(index);
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, handle);
    moveTo(QPoint(handle.x(), handle.y() - distance / 2));
    moveTo(end);
    ASSERT_TRUE(editor->property("dragging").toBool()) << "a live drag previews without publishing";
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, end);
    settle(80);

    const QPointF expected(authoredNumber(installedNodeId_, hueKey), authoredNumber(installedNodeId_, saturationKey));
    EXPECT_GT(expected.x(), 0.0) << "dragging upward rotates the selected knot";
    EXPECT_GT(expected.y(), 0.0) << "the off-axis endpoint increases its radius";
    EXPECT_EQ(session_->revision(), revisionBefore + 1) << "one drag is one history entry";
    // The drawn handle follows the same mapping the node evaluates.
    const QPoint drawn = handlePoint(index);
    EXPECT_NEAR(drawn.x(), end.x(), 2.0);
    EXPECT_NEAR(drawn.y(), end.y(), 2.0);
    EXPECT_GT(frameDifference(wheelBefore, window_->grabWindow().copy(wheelRect)), 0.2)
        << "the wheel must really redraw the authored deformation";

    // The presented image really changed: a deformation the panel draws but the
    // viewer did not apply would fail here.
    awaitInstalledFrame();
    const QImage warpedFrame = presentedFrame(viewerPanelId_);
    ASSERT_FALSE(warpedFrame.isNull());
    // Spoke zero affects the plate's red corner. Compare that region: unrelated
    // pixels can differ more through lossy viewer-cache replay than this local
    // deformation changes them.
    const QRect affected(identityFrame.width() * 4 / 5, 0, identityFrame.width() / 5, identityFrame.height() / 5);
    const QImage identityCorner = identityFrame.copy(affected);
    const double warpedDifference = frameDifference(identityCorner, warpedFrame.copy(affected));
    EXPECT_GT(warpedDifference, 0.0) << "the installed node must change the presented red region";
    capture(QStringLiteral("issue37-mesh-authored-full"));

    // --- shared history ----------------------------------------------------
    ASSERT_TRUE(history_->undo());
    settle(40);
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);
    EXPECT_EQ(handlePoint(index), handle);
    awaitInstalledFrame();
    EXPECT_LT(frameDifference(identityCorner, presentedFrame(viewerPanelId_).copy(affected)), warpedDifference)
        << "undoing the mesh must return the presented image to the identity bypass";
    ASSERT_TRUE(history_->redo());
    settle(40);
    EXPECT_NEAR(authoredNumber(installedNodeId_, hueKey), expected.x(), 1e-3);

    // --- Escape cancels a live gesture -------------------------------------
    const QPoint movedHandle = handlePoint(index);
    const auto revisionAtEscape = session_->revision();
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, movedHandle);
    moveTo(QPoint(movedHandle.x(), movedHandle.y() - distance));
    ASSERT_TRUE(editor->property("dragging").toBool());
    QTest::keyClick(window_, Qt::Key_Escape);
    settle(40);
    EXPECT_FALSE(editor->property("dragging").toBool());
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, QPoint(movedHandle.x(), movedHandle.y() - distance));
    settle(40);
    EXPECT_NEAR(authoredNumber(installedNodeId_, hueKey), expected.x(), 1e-3)
        << "a cancelled gesture restores the shape the drag began from";
    EXPECT_EQ(session_->revision(), revisionAtEscape) << "a cancelled gesture publishes no history entry";

    // --- the shared Undo owner cancels a live gesture too -------------------
    const auto revisionAtSharedUndo = session_->revision();
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, movedHandle);
    moveTo(QPoint(movedHandle.x(), movedHandle.y() - distance));
    ASSERT_TRUE(editor->property("dragging").toBool());
    ASSERT_TRUE(history_->undo()) << "the live mesh gesture must be the shared history target";
    settle(40);
    EXPECT_FALSE(editor->property("dragging").toBool());
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, QPoint(movedHandle.x(), movedHandle.y() - distance));
    settle(40);
    EXPECT_NEAR(authoredNumber(installedNodeId_, hueKey), expected.x(), 1e-3)
        << "cancelling a live gesture keeps the shape it began from";
    EXPECT_EQ(session_->revision(), revisionAtSharedUndo) << "a cancelled gesture publishes no history entry";

    // With no gesture live, the same owner reaches the document history: the
    // committed drag is exactly one undo step and redo re-applies it.
    ASSERT_TRUE(history_->undo());
    settle(40);
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);
    ASSERT_TRUE(history_->redo());
    settle(40);
    EXPECT_NEAR(authoredNumber(installedNodeId_, hueKey), expected.x(), 1e-3);

    // --- pin protects the authored point -----------------------------------
    const auto revisionAtPin = session_->revision();
    clickNamed(pinName(installedNodeId_));
    EXPECT_TRUE(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)));
    EXPECT_EQ(session_->revision(), revisionAtPin + 1) << "Pin is an authored, undoable edit";
    const double pinnedHue = authoredNumber(installedNodeId_, hueKey);
    const QPoint pinnedHandle = handlePoint(index);
    const auto revisionWhilePinned = session_->revision();
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, pinnedHandle);
    moveTo(QPoint(pinnedHandle.x(), pinnedHandle.y() - distance));
    moveTo(QPoint(pinnedHandle.x(), pinnedHandle.y() - distance));
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, QPoint(pinnedHandle.x(), pinnedHandle.y() - distance));
    settle(60);
    EXPECT_FALSE(editor->property("dragging").toBool()) << "a pinned point is never dragged";
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), pinnedHue) << "a pinned point keeps its authored value";
    EXPECT_EQ(session_->revision(), revisionWhilePinned);
    auto* notice = item(noticeName(installedNodeId_));
    ASSERT_NE(notice, nullptr);
    EXPECT_TRUE(notice->isVisible()) << "the wheel must state why the drag did not move the point";
    capture(QStringLiteral("issue37-mesh-pinned"));

    clickNamed(pinName(installedNodeId_));
    EXPECT_FALSE(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)));
    const QPoint unpinnedHandle = handlePoint(index);
    const auto revisionAfterUnpin = session_->revision();
    drag(unpinnedHandle, QPoint(unpinnedHandle.x() + distance, unpinnedHandle.y()));
    EXPECT_EQ(session_->revision(), revisionAfterUnpin + 1) << "an unpinned point drags again";

    // --- Reset Selected keeps the pin, Reset All clears it ------------------
    clickNamed(pinName(installedNodeId_));
    ASSERT_TRUE(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)));
    const auto revisionAtResetSelected = session_->revision();
    clickNamed(resetSelectedName(installedNodeId_));
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, saturationKey), 0.0);
    EXPECT_TRUE(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)))
        << "Reset Selected leaves the protection flag authored";
    EXPECT_EQ(session_->revision(), revisionAtResetSelected + 1);
    ASSERT_TRUE(history_->undo());
    settle(40);
    EXPECT_NE(authoredNumber(installedNodeId_, hueKey), 0.0);

    const auto revisionAtResetAll = session_->revision();
    clickNamed(resetAllName(installedNodeId_));
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);
    EXPECT_FALSE(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)))
        << "Reset All clears every coordinate and every pin";
    EXPECT_EQ(session_->revision(), revisionAtResetAll + 1);
    ASSERT_TRUE(history_->undo());
    settle(40);
    EXPECT_TRUE(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)));
    clickNamed(pinName(installedNodeId_));  // leave the point free for the drags below

    // --- strength is the host's shared numeric control ----------------------
    // Strength moves the drawn knot toward its observed identity position.
    const QPointF base = handle;
    const auto offsetOf = [&](const QPoint& point) { return std::hypot(point.x() - base.x(), point.y() - base.y()); };
    const double offsetBefore = offsetOf(handlePoint(index));
    ASSERT_GT(offsetBefore, 2.0) << "the scenario needs an authored displacement to scale";
    auto* strengthSlider = item(QStringLiteral("slider_") + installedNodeId_ + QStringLiteral("_strength"));
    ASSERT_NE(strengthSlider, nullptr);
    const QPoint sliderMiddle = center(strengthSlider);
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, sliderMiddle);
    settle(80);
    EXPECT_LT(strengthSlider->property("displayValue").toDouble(), 1.0);
    EXPECT_LT(offsetOf(handlePoint(index)), offsetBefore)
        << "the mesh must follow the shared numeric preview before pointer release";
    const double heldOffset = offsetOf(handlePoint(index));
    auto* editorController =
        qobject_cast<nemo::ui::ViewerController*>(meshEditor()->property("controller").value<QObject*>());
    ASSERT_NE(editorController, nullptr);
    const int frameBeforePreview = editorController->frame();
    editorController->setFrame(frameBeforePreview + 1);
    ASSERT_TRUE(waitFor([&] { return meshEditor()->property("frame").toInt() == frameBeforePreview + 1; }));
    settle(80);
    EXPECT_DOUBLE_EQ(offsetOf(handlePoint(index)), heldOffset)
        << "a transport refresh must preserve the still-held numeric preview";
    capture(QStringLiteral("issue37-mesh-held-strength"), false);
    editorController->setFrame(frameBeforePreview);
    settle(80);
    QTest::keyClick(window_, Qt::Key_Escape);
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, sliderMiddle);
    settle(80);
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, QStringLiteral("strength")), 1.0);
    EXPECT_DOUBLE_EQ(offsetOf(handlePoint(index)), offsetBefore);
    typeInto(QStringLiteral("param_") + installedNodeId_ + QStringLiteral("_strength"), QStringLiteral("0.5"));
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, QStringLiteral("strength")), 0.5);
    EXPECT_LT(offsetOf(handlePoint(index)), offsetBefore);
    EXPECT_GT(offsetOf(handlePoint(index)), 0.0);

    // Strength 0 is the identity mapping, so the pointer states no finite
    // displacement and the drag is refused instead of authoring something.
    typeInto(QStringLiteral("param_") + installedNodeId_ + QStringLiteral("_strength"), QStringLiteral("0"));
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, QStringLiteral("strength")), 0.0);
    EXPECT_EQ(handlePoint(index), handle) << "zero strength restores the drawn identity knot";
    const auto revisionAtIdentity = session_->revision();
    const QPoint identityHandle = handlePoint(index);
    drag(identityHandle, QPoint(identityHandle.x(), identityHandle.y() - distance));
    EXPECT_EQ(session_->revision(), revisionAtIdentity) << "the identity mapping authors nothing";
    typeInto(QStringLiteral("param_") + installedNodeId_ + QStringLiteral("_strength"), QStringLiteral("1"));
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, QStringLiteral("strength")), 1.0);
    EXPECT_TRUE(meshEditor()->property("wheelNotice").toString().isEmpty())
        << "restoring Strength must retire the obsolete zero-strength guidance";

    // --- save and reopen through the application's project file path --------
    const QString projectPath = evidenceDirectory_.isEmpty()
                                    ? directory_.filePath(QStringLiteral("issue37-colorwarp.nemo"))
                                    : evidenceDirectory_ + QStringLiteral("/issue37-colorwarp.nemo");
    QDir().mkpath(QFileInfo(projectPath).absolutePath());
    const double savedHue = authoredNumber(installedNodeId_, hueKey);
    const double savedSaturation = authoredNumber(installedNodeId_, saturationKey);
    const double savedStrength = authoredNumber(installedNodeId_, QStringLiteral("strength"));
    const bool savedPin = authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index));
    QSignalSpy saved(projectFile_.get(), &nemo::ui::ProjectFileController::saveFinished);
    projectFile_->saveAs(QUrl::fromLocalFile(projectPath));
    ASSERT_TRUE(waitFor([&] { return !saved.empty(); })) << projectFile_->error().toStdString();
    ASSERT_TRUE(saved.front().at(0).toBool()) << projectFile_->error().toStdString();

    clickNamed(resetAllName(installedNodeId_));
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), 0.0);

    QSignalSpy opened(projectFile_.get(), &nemo::ui::ProjectFileController::projectOpened);
    projectFile_->openProject(QUrl::fromLocalFile(projectPath));
    ASSERT_TRUE(waitFor([&] { return !opened.empty(); })) << projectFile_->error().toStdString();
    if (opened.front().at(0).toBool()) {
        auto* keep = item(QStringLiteral("keepCurrentWorkspaceButton"));
        if (keep && waitFor([&] { return keep->isVisible(); }, 2000)) {
            QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(keep));
            settle(80);
        }
    }
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, hueKey), savedHue) << "the authored mesh must survive reopen";
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, saturationKey), savedSaturation);
    EXPECT_DOUBLE_EQ(authoredNumber(installedNodeId_, QStringLiteral("strength")), savedStrength);
    EXPECT_EQ(authoredFlag(installedNodeId_, QStringLiteral("pin%1").arg(index)), savedPin);
    ASSERT_TRUE(waitFor([&] { return meshEditor() != nullptr; }, 3000))
        << "the installed editor must still be hosted after reopen";

    // --- the selected coordinate uses the shared keying semantics -----------
    const QPoint reopenedHandle = handlePoint(index);
    ASSERT_FALSE(reopenedHandle.isNull());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, reopenedHandle);
    settle(40);
    ASSERT_EQ(editor->property("selectedIndex").toInt(), index);
    const double keyedHue = inspectorValue(installedNodeId_, hueKey);
    ASSERT_FALSE(std::isnan(keyedHue)) << "the shared inspector query must state the coordinate";
    QTest::mouseClick(window_, Qt::LeftButton, Qt::AltModifier,
                      center(QStringLiteral("param_") + installedNodeId_ + QStringLiteral("_") + hueKey));
    settle(80);
    EXPECT_EQ(controller_->nodeParameterKeyStatus(network_, installedNodeId_, hueKey), QStringLiteral("key"))
        << "the selected coordinate must key through the shared keying gesture";
    EXPECT_DOUBLE_EQ(inspectorValue(installedNodeId_, hueKey), keyedHue)
        << "keying at the current frame leaves the value it keys";
    const auto revisionAtTypedKey = session_->revision();
    typeInto(QStringLiteral("param_") + installedNodeId_ + QStringLiteral("_") + hueKey, QStringLiteral("0.05"));
    EXPECT_DOUBLE_EQ(inspectorValue(installedNodeId_, hueKey), 0.05)
        << "a typed coordinate edits the shared channel at the current frame";
    EXPECT_EQ(session_->revision(), revisionAtTypedKey + 1) << "one typed coordinate is one history entry";
    capture(QStringLiteral("issue37-mesh-keyed"));

    awaitInstalledFrame();
    capture(QStringLiteral("issue37-mesh-reopened-full"));
    window_->resize(1100, 760);
    settle(150);
    capture(QStringLiteral("issue37-mesh-reopened-narrow"));
    expectNoQmlWarnings();
}

// A saved layout from a session where the package was installed: a viewer, the
// Parameters panel and the installed package's demonstration panel. A split
// holds exactly two children, so the third panel is nested.
class SeededInstalledPanelSurface : public ExtensionSurface {
protected:
    QJsonObject initialLayout() const override {
        return splitNode(
            QStringLiteral("split-root"), QStringLiteral("horizontal"), 0.5,
            QJsonArray{
                tabsNode(QStringLiteral("leaf-viewer"),
                         QJsonArray{panelRecord(viewerPanelId_, QStringLiteral("viewer"), QStringLiteral("A"),
                                                QJsonObject{{QStringLiteral("viewerIndex"), 0}})},
                         viewerPanelId_),
                splitNode(QStringLiteral("split-right"), QStringLiteral("horizontal"), 0.5,
                          QJsonArray{tabsNode(QStringLiteral("leaf-params"),
                                              QJsonArray{panelRecord(parametersPanelId_, QStringLiteral("parameters"),
                                                                     QStringLiteral("A"))},
                                              parametersPanelId_),
                                     tabsNode(QStringLiteral("leaf-installed"),
                                              QJsonArray{panelRecord(installedPanelId_, installedPanelType_,
                                                                     QStringLiteral("A"))},
                                              installedPanelId_)})});
    }
};

// Acceptance example 7, first half: with the package absent, the saved layout
// keeps the panel and its arrangement, and the shared shell states that the
// panel is unavailable instead of presenting invented content.
class AbsentPackageSurface : public SeededInstalledPanelSurface {
protected:
    bool packagePresent() const override { return false; }
};

TEST_F(AbsentPackageSurface, Issue37SavedPanelLayoutSurvivesPackageAbsence) {
    const auto savedLeaf = leafOf(installedPanelId_);
    ASSERT_FALSE(savedLeaf.isEmpty()) << "a saved layout must keep the installed panel's record";
    EXPECT_EQ(savedLeaf.value(QStringLiteral("id")).toString(), QStringLiteral("leaf-installed"));
    EXPECT_EQ(panelRecordOf(installedPanelId_).value(QStringLiteral("type")).toString(), installedPanelType_);
    EXPECT_EQ(panelRecordOf(installedPanelId_).value(QStringLiteral("group")).toString(), QStringLiteral("A"));

    ASSERT_TRUE(
        waitFor([&] { return item(QStringLiteral("unavailablePanel_") + installedPanelType_) != nullptr; }, 3000))
        << "the shared shell must present the unavailable panel";
    EXPECT_TRUE(item(QStringLiteral("unavailablePanel_") + installedPanelType_)->isVisible());
    EXPECT_EQ(item(panelBodyName()), nullptr) << "no package content may be presented while the package is absent";
    EXPECT_TRUE(workspace_->panelDescriptor(installedPanelType_).value(QStringLiteral("source")).toString().isEmpty())
        << "an absent package contributes no panel descriptor";
    capture(QStringLiteral("issue37-panel-unavailable-full"));
    window_->resize(1100, 760);
    settle(150);
    capture(QStringLiteral("issue37-panel-unavailable-narrow"));
    window_->resize(1600, 960);
    settle(150);

    // The workspace file the session persists still holds the record, so a later
    // session with the package installed recovers the same arrangement.
    ASSERT_TRUE(workspace_->save()) << workspace_->error().toStdString();
    nemo::workspace::WorkspaceController restored(directory_.filePath(QStringLiteral("workspace.json")));
    EXPECT_EQ(restored.root(), workspace_->root());
    EXPECT_FALSE(leafContaining(restored.root(), installedPanelId_).isEmpty());
    expectNoQmlWarnings();
}

// Acceptance example 7, second half plus example 3: the same saved layout opened
// with the package present recovers the panel in place, and that panel then uses
// the ordinary shell — type menu, split, resize, full-header docking, close and
// reopen — exactly like a built-in.
class RestoredPackageSurface : public SeededInstalledPanelSurface {};

TEST_F(RestoredPackageSurface, Issue37ExtensionPanelUsesTheSharedShell) {
    // The saved layout is the same one the absent session kept, and the panel
    // now loads its package content in the same leaf.
    ASSERT_TRUE(waitFor([&] { return item(panelBodyName()) != nullptr; }, 5000))
        << "the installed package's panel body must load into the shared shell";
    auto* body = item(panelBodyName());
    ASSERT_TRUE(body->isVisible());
    EXPECT_EQ(leafOf(installedPanelId_).value(QStringLiteral("id")).toString(), QStringLiteral("leaf-installed"));
    EXPECT_EQ(panelRecordOf(installedPanelId_).value(QStringLiteral("type")).toString(), installedPanelType_);

    // The panel's header states the descriptor's own title, and the shared type
    // menu offers the installed type beside the built-ins.
    auto* typeButton = item(QStringLiteral("panelType_") + installedPanelId_);
    ASSERT_NE(typeButton, nullptr);
    EXPECT_EQ(typeButton->property("text").toString(), installedPanelTitle_);
    openPanelTypeMenu(installedPanelId_);
    auto* choice =
        item(QStringLiteral("panelTypeChoice_") + installedPanelType_ + QStringLiteral("_") + installedPanelId_);
    ASSERT_NE(choice, nullptr) << "the shared panel menu must list the installed panel type";
    EXPECT_EQ(choice->property("text").toString(), installedPanelTitle_);
    QTest::keyClick(window_, Qt::Key_Escape);
    settle(60);
    capture(QStringLiteral("issue37-panel-restored-full"));

    ASSERT_TRUE(router_->setActivePanel(viewerPanelId_));
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(body));
    EXPECT_EQ(router_->activePanel(), installedPanelId_) << "body presses activate the shared panel context";

    const QRect bodyRect = body->mapRectToScene(QRectF(0, 0, body->width(), body->height())).toRect();
    const QImage graphiteBody = window_->grabWindow().copy(bodyRect);
    clickNamed(QStringLiteral("themeSettingsButton"));
    clickNamed(QStringLiteral("themePresetMenu"));
    QTest::keyClick(window_, Qt::Key_End);
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::keyClick(window_, Qt::Key_Escape);
    ASSERT_TRUE(waitFor([&] { return window_->grabWindow().copy(bodyRect) != graphiteBody; }, 2000))
        << "the installed body must visibly follow the shared appearance control";
    capture(QStringLiteral("issue37-panel-paper"));
    clickNamed(QStringLiteral("themeSettingsButton"));
    clickNamed(QStringLiteral("themePresetMenu"));
    QTest::keyClick(window_, Qt::Key_Home);
    QTest::keyClick(window_, Qt::Key_Return);
    QTest::keyClick(window_, Qt::Key_Escape);
    settle(100);

    // Split from the installed panel's own menu, then resize the shared divider.
    const int leavesBefore = leafCount(workspace_->root());
    clickPanelMenuEntry(installedPanelId_, QStringLiteral("Split Vertical"));
    EXPECT_GT(leafCount(workspace_->root()), leavesBefore) << "the shared shell must split for an installed panel";
    const QString installedLeafId = leafOf(installedPanelId_).value(QStringLiteral("id")).toString();
    QVariantMap parent;
    ASSERT_TRUE(parentSplitOf(workspace_->root(), installedLeafId, &parent));
    const double ratioBefore = parent.value(QStringLiteral("ratio")).toDouble();
    auto* leafItem = item(QStringLiteral("leaf_") + installedLeafId);
    ASSERT_NE(leafItem, nullptr);
    const QPoint divider = leafItem->mapToScene(QPointF(leafItem->width() / 2, leafItem->height() + 2)).toPoint();
    drag(divider, divider + QPoint(0, 60));
    QVariantMap resized;
    ASSERT_TRUE(parentSplitOf(workspace_->root(), installedLeafId, &resized));
    EXPECT_GT(resized.value(QStringLiteral("ratio")).toDouble(), ratioBefore)
        << "the shared divider must resize an installed panel's pane";

    // Dock the full header into the built-in leaf: the panel becomes a tab of
    // that leaf, exactly like a built-in panel.
    auto* header = item(QStringLiteral("panelHeader_") + installedPanelId_);
    ASSERT_NE(header, nullptr);
    auto* targetLeaf = item(QStringLiteral("leaf_") + leafOf(viewerPanelId_).value(QStringLiteral("id")).toString());
    ASSERT_NE(targetLeaf, nullptr);
    drag(center(header), center(targetLeaf));
    const auto dockedLeaf = leafOf(installedPanelId_);
    EXPECT_EQ(dockedLeaf.value(QStringLiteral("id")).toString(),
              leafOf(viewerPanelId_).value(QStringLiteral("id")).toString())
        << "a full-header drag must dock the installed panel beside the built-ins";
    EXPECT_EQ(dockedLeaf.value(QStringLiteral("panels")).toList().size(), 2);
    ASSERT_NE(item(QStringLiteral("panelTab_") + installedPanelId_), nullptr)
        << "a docked panel is an ordinary tab of the target leaf";
    auto* dockedBody = item(panelBodyName());
    ASSERT_NE(dockedBody, nullptr) << "the docked panel must still present its package content";
    EXPECT_TRUE(dockedBody->isVisible());
    capture(QStringLiteral("issue37-panel-docked-full"));

    // Close through the shared menu: the arrangement drops the panel and no
    // package content remains on screen.
    clickPanelMenuEntry(installedPanelId_, QStringLiteral("Close Panel"));
    EXPECT_TRUE(leafOf(installedPanelId_).isEmpty()) << "closing removes the panel from the arrangement";
    EXPECT_TRUE(waitFor([&] { return item(panelBodyName()) == nullptr; }, 3000))
        << "closing must unload the package's panel content";

    // Reopen through the ordinary controls: a new tab in the same leaf, whose
    // type is chosen from the shared type menu.
    clickPanelMenuEntry(viewerPanelId_, QStringLiteral("Add Tab"));
    const QString reopened = leafOf(viewerPanelId_).value(QStringLiteral("active")).toString();
    ASSERT_FALSE(reopened.isEmpty());
    choosePanelType(reopened, installedPanelType_);
    EXPECT_EQ(leafOf(reopened).value(QStringLiteral("id")).toString(),
              leafOf(viewerPanelId_).value(QStringLiteral("id")).toString())
        << "reopening in the same leaf keeps the arrangement";
    EXPECT_EQ(panelRecordOf(reopened).value(QStringLiteral("type")).toString(), installedPanelType_);
    ASSERT_TRUE(waitFor([&] { return item(panelBodyName()) != nullptr; }, 5000))
        << "the reopened installed panel must load its package content";

    // The arrangement is workspace state the workspace file keeps: a fresh
    // controller reads back exactly the layout this session reached, with or
    // without the package registered.
    ASSERT_TRUE(workspace_->save()) << workspace_->error().toStdString();
    nemo::workspace::WorkspaceController restored(directory_.filePath(QStringLiteral("workspace.json")));
    EXPECT_EQ(restored.root(), workspace_->root());
    EXPECT_TRUE(restored.panelDescriptor(installedPanelType_).value(QStringLiteral("source")).toString().isEmpty())
        << "the persisted layout is independent of whether the package is present";
    EXPECT_FALSE(leafContaining(restored.root(), reopened).isEmpty());
    if (!evidenceDirectory_.isEmpty()) {
        QFile retained(evidenceDirectory_ + QStringLiteral("/issue37-workspace.json"));
        QFile savedLayout(directory_.filePath(QStringLiteral("workspace.json")));
        ASSERT_TRUE(savedLayout.open(QIODevice::ReadOnly));
        ASSERT_TRUE(retained.open(QIODevice::WriteOnly));
        const QByteArray bytes = savedLayout.readAll();
        ASSERT_EQ(retained.write(bytes), bytes.size());
    }

    capture(QStringLiteral("issue37-panel-shell-full"));
    window_->resize(1100, 760);
    settle(150);
    capture(QStringLiteral("issue37-panel-shell-narrow"));
    expectNoQmlWarnings();
}

}  // namespace
