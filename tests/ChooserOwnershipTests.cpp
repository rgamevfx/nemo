// Native chooser request/response ownership (issue #43).
//
// One NativeFileChooser serves every workflow, so the hazard is not that a
// dialog exists but that a choice meant for one workflow reaches another: a
// media import pick read as a project Save As overwrites the picked media with
// project JSON. These tests compose the two real clients (ProjectFileController
// for Open/Recover/Save As, MediaLibraryModel for Import/Relink) over one real
// chooser and assert which workflow acts on each picked file, including the
// cases the ownership contract has to survive: a second client refused while a
// request is outstanding, cancellation and failure, and an initiator destroyed
// before its outcome.
//
// The chooser's DBus path is real: the process is pointed at a private session
// bus that hosts no portal, so no native dialog can appear and no user is ever
// asked to pick a file. The portal's Response is then handed to the chooser
// through its own portal entry point — the slot QtDBus connects to the request
// handle — which is exactly the message the session portal would send. Nothing
// here writes a project or media file: what the user would see is the workflow
// that receives the choice.

#include "MediaLibraryModel.hpp"
#include "NativeFileChooser.hpp"
#include "PanelContextRouter.hpp"
#include "ProjectFileController.hpp"
#include "WorkspaceController.hpp"

#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/MediaImportService.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <memory>

namespace {

// A session bus with no portal: the chooser runs its real DBus path while the
// platform can never show a dialog. It is started from a minimal temporary
// configuration that has no service directories, so the portal is neither
// running nor activatable on it.
//
// Qt caches its default session connection and does not reliably re-point it
// once opened (disconnecting and reconnecting the default name has been
// observed to leave a dead connection behind), so this bus is established once
// for the whole test binary and shared by every chooser test; no test swaps it
// under another. No other test in this binary uses DBus.
//
// A machine without dbus-daemon skips: there is nothing to isolate and nothing
// to drive. An available dbus-daemon that cannot be isolated — no usable
// address, no connection, or a bus that still offers the real desktop portal —
// is reported as a setup failure instead of a silent pass, so a real chooser is
// never opened and a misconfigured fixture is never mistaken for coverage.
class PrivateSessionBus {
public:
    enum class Status { Ready, Unavailable, Misconfigured };

    // Established on first use and kept for the lifetime of the process.
    static PrivateSessionBus& instance() {
        static PrivateSessionBus bus;
        return bus;
    }

    [[nodiscard]] Status status() const { return status_; }
    [[nodiscard]] QString diagnostic() const { return diagnostic_; }

private:
    PrivateSessionBus() { start(); }
    ~PrivateSessionBus() {
        daemon_.terminate();
        daemon_.waitForFinished(2000);
    }

    void start() {
        // No <servicedir> and no <standard_session_servicedirs/>: the daemon has
        // nothing to activate, so org.freedesktop.portal.Desktop cannot appear.
        static const char kConfiguration[] =
            R"BUSCONF(<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
)BUSCONF";
        if (!configurationDirectory_.isValid()) {
            status_ = Status::Misconfigured;
            diagnostic_ = QStringLiteral("no temporary directory for the bus configuration");
            return;
        }
        const QString configuration = configurationDirectory_.filePath(QStringLiteral("bus.conf"));
        QFile file(configuration);
        if (!file.open(QIODevice::WriteOnly) || file.write(kConfiguration) < 0) {
            status_ = Status::Misconfigured;
            diagnostic_ = QStringLiteral("cannot write %1").arg(configuration);
            return;
        }
        file.close();

        daemon_.setProcessChannelMode(QProcess::MergedChannels);
        daemon_.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--config-file=") + configuration,
                                                      QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
        if (!daemon_.waitForStarted(kStartupTimeoutMs)) {
            status_ = Status::Unavailable;
            diagnostic_ = QStringLiteral("dbus-daemon could not be started: %1").arg(daemon_.errorString());
            return;
        }

        QByteArray output;
        QByteArray address;
        QElapsedTimer elapsed;
        elapsed.start();
        while (address.isEmpty() && elapsed.elapsed() < kStartupTimeoutMs) {
            if (!daemon_.waitForReadyRead(100)) {
                if (daemon_.state() == QProcess::NotRunning) {
                    break;
                }
                continue;
            }
            output += daemon_.readAll();
            for (const QByteArray& line : output.split('\n')) {
                const QByteArray candidate = line.trimmed();
                if (candidate.startsWith("unix:") || candidate.startsWith("tcp:")) {
                    address = candidate;
                    break;
                }
            }
        }
        if (address.isEmpty()) {
            status_ = Status::Misconfigured;
            diagnostic_ =
                QStringLiteral("the private bus printed no address (%1)").arg(QString::fromLocal8Bit(output).trimmed());
            return;
        }
        qputenv("DBUS_SESSION_BUS_ADDRESS", address);
        if (!QDBusConnection::sessionBus().isConnected()) {
            status_ = Status::Misconfigured;
            diagnostic_ =
                QStringLiteral("the private bus at %1 could not be used").arg(QString::fromLocal8Bit(address));
            return;
        }
        QDBusConnectionInterface* bus = QDBusConnection::sessionBus().interface();
        if (bus == nullptr) {
            status_ = Status::Misconfigured;
            diagnostic_ = QStringLiteral("the private bus exposes no connection interface");
            return;
        }
        // Fail closed: the real desktop portal must be unreachable from here.
        const QString portal = QStringLiteral("org.freedesktop.portal.Desktop");
        if (bus->isServiceRegistered(portal).value() || bus->activatableServiceNames().value().contains(portal)) {
            status_ = Status::Misconfigured;
            diagnostic_ = QStringLiteral("the session bus in use still offers %1").arg(portal);
            return;
        }
        status_ = Status::Ready;
    }

    static constexpr int kStartupTimeoutMs = 10000;

    QTemporaryDir configurationDirectory_;
    QProcess daemon_;
    Status status_{Status::Unavailable};
    QString diagnostic_;
};

// Handles the outstanding chooser request the way the session portal would:
// response 0 accepted with `paths`, 1 cancelled by the user, 2+ failed.
void deliverPortalResponse(nemo::ui::NativeFileChooser& chooser, uint response, const QStringList& paths = {}) {
    ASSERT_TRUE(chooser.inFlight()) << "no chooser request is outstanding";
    QVariantMap results;
    QStringList uris;
    uris.reserve(paths.size());
    for (const QString& path : paths) {
        uris.push_back(QUrl::fromLocalFile(path).toString());
    }
    results.insert(QStringLiteral("uris"), uris);
    ASSERT_TRUE(QMetaObject::invokeMethod(&chooser, "onPortalResponse", Qt::DirectConnection, Q_ARG(uint, response),
                                          Q_ARG(QVariantMap, results)));
}

void chooseFiles(nemo::ui::NativeFileChooser& chooser, const QStringList& paths) {
    deliverPortalResponse(chooser, 0U, paths);
}

void cancelDialog(nemo::ui::NativeFileChooser& chooser) {
    deliverPortalResponse(chooser, 1U);
}

void failDialog(nemo::ui::NativeFileChooser& chooser) {
    deliverPortalResponse(chooser, 2U);
}

class ChooserOwnershipTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory_.isValid());
        const PrivateSessionBus& bus = PrivateSessionBus::instance();
        switch (bus.status()) {
        case PrivateSessionBus::Status::Ready:
            return;
        case PrivateSessionBus::Status::Unavailable:
            GTEST_SKIP() << "no dbus-daemon to host a dialog-free session bus: " << bus.diagnostic().toStdString();
            return;
        case PrivateSessionBus::Status::Misconfigured:
            FAIL() << "the dialog-free session bus could not be set up: " << bus.diagnostic().toStdString();
            return;
        }
        FAIL() << "unknown private session bus status";
    }

    QTemporaryDir directory_;
    nemo::ProjectSession session_;
    nemo::workspace::WorkspaceController workspace_{directory_.filePath(QStringLiteral("workspace.json"))};
    nemo::ui::PanelContextRouter router_{session_};
    nemo::ui::NativeFileChooser chooser_;
    nemo::media::MediaImportService importer_;
};

// The reported data-loss path: the media workflow's choice arrived at the
// project workflow, which still held its previous Save As purpose and saved the
// project over the picked media file.
TEST_F(ChooserOwnershipTest, MediaChoiceAfterAProjectSaveAsIsHandledOnlyByTheMediaWorkflow) {
    nemo::ui::ProjectFileController projectFile{session_, workspace_, router_, chooser_};
    nemo::ui::MediaLibraryModel library{session_, importer_, &router_, &workspace_};
    library.setNativeFileChooser(&chooser_);

    QSignalSpy saveAsChosen(&projectFile, &nemo::ui::ProjectFileController::saveAsChosen);
    QSignalSpy openChosen(&projectFile, &nemo::ui::ProjectFileController::openFileChosen);
    QSignalSpy recoveryChosen(&projectFile, &nemo::ui::ProjectFileController::recoveryFileChosen);
    QSignalSpy projectFailed(&projectFile, &nemo::ui::ProjectFileController::fileDialogFailed);
    QSignalSpy projectCancelled(&projectFile, &nemo::ui::ProjectFileController::fileDialogCancelled);
    QSignalSpy importChosen(&library, &nemo::ui::MediaLibraryModel::importPathsChosen);
    QSignalSpy relinkChosen(&library, &nemo::ui::MediaLibraryModel::relinkPathChosen);
    QSignalSpy mediaFailed(&library, &nemo::ui::MediaLibraryModel::mediaChooserFailed);
    QSignalSpy mediaCancelled(&library, &nemo::ui::MediaLibraryModel::mediaChooserCancelled);

    // A project Save As completes first, leaving the project workflow with a
    // Save As of its own behind it.
    const QString projectPath = directory_.filePath(QStringLiteral("project.nemo"));
    projectFile.chooseSaveAs();
    chooseFiles(chooser_, {projectPath});
    ASSERT_EQ(saveAsChosen.count(), 1);
    EXPECT_EQ(saveAsChosen.at(0).at(0).value<QUrl>(), QUrl::fromLocalFile(projectPath));
    saveAsChosen.clear();

    // The user now picks media to import.
    const QString mediaPath = directory_.filePath(QStringLiteral("shot.exr"));
    library.chooseImportPaths(QStringLiteral("root"));
    chooseFiles(chooser_, {mediaPath});

    ASSERT_EQ(importChosen.count(), 1);
    EXPECT_EQ(importChosen.at(0).at(0).toStringList(), QStringList{mediaPath});
    EXPECT_EQ(importChosen.at(0).at(1).toString(), QStringLiteral("root"));
    // The project workflow must not act on the media choice.
    EXPECT_EQ(saveAsChosen.count(), 0);
    EXPECT_EQ(openChosen.count(), 0);
    EXPECT_EQ(recoveryChosen.count(), 0);
    EXPECT_EQ(projectFailed.count(), 0);
    EXPECT_EQ(projectCancelled.count(), 0);
    EXPECT_EQ(relinkChosen.count(), 0);
    EXPECT_EQ(mediaFailed.count(), 0);
    EXPECT_EQ(mediaCancelled.count(), 0);
}

// The other direction: a picked project file is this workflow's to open, and no
// media workflow may treat it as an import or relink.
TEST_F(ChooserOwnershipTest, ProjectChoiceIsNeverAnImport) {
    nemo::ui::ProjectFileController projectFile{session_, workspace_, router_, chooser_};
    nemo::ui::MediaLibraryModel library{session_, importer_, &router_, &workspace_};
    library.setNativeFileChooser(&chooser_);

    QSignalSpy openChosen(&projectFile, &nemo::ui::ProjectFileController::openFileChosen);
    QSignalSpy saveAsChosen(&projectFile, &nemo::ui::ProjectFileController::saveAsChosen);
    QSignalSpy importChosen(&library, &nemo::ui::MediaLibraryModel::importPathsChosen);
    QSignalSpy relinkChosen(&library, &nemo::ui::MediaLibraryModel::relinkPathChosen);
    QSignalSpy mediaFailed(&library, &nemo::ui::MediaLibraryModel::mediaChooserFailed);
    QSignalSpy mediaCancelled(&library, &nemo::ui::MediaLibraryModel::mediaChooserCancelled);

    const QString projectPath = directory_.filePath(QStringLiteral("opened.nemo"));
    projectFile.chooseOpenProject();
    chooseFiles(chooser_, {projectPath});

    ASSERT_EQ(openChosen.count(), 1);
    EXPECT_EQ(openChosen.at(0).at(0).value<QUrl>(), QUrl::fromLocalFile(projectPath));
    EXPECT_EQ(saveAsChosen.count(), 0);
    EXPECT_EQ(importChosen.count(), 0);
    EXPECT_EQ(relinkChosen.count(), 0);
    EXPECT_EQ(mediaFailed.count(), 0);
    EXPECT_EQ(mediaCancelled.count(), 0);
}

// Cancellation and failure belong to the initiator like a choice does.
TEST_F(ChooserOwnershipTest, CancellationAndFailureReachOnlyTheInitiatingWorkflow) {
    nemo::ui::ProjectFileController projectFile{session_, workspace_, router_, chooser_};
    nemo::ui::MediaLibraryModel library{session_, importer_, &router_, &workspace_};
    library.setNativeFileChooser(&chooser_);

    QSignalSpy mediaCancelled(&library, &nemo::ui::MediaLibraryModel::mediaChooserCancelled);
    QSignalSpy mediaFailed(&library, &nemo::ui::MediaLibraryModel::mediaChooserFailed);
    QSignalSpy projectCancelled(&projectFile, &nemo::ui::ProjectFileController::fileDialogCancelled);
    QSignalSpy projectFailed(&projectFile, &nemo::ui::ProjectFileController::fileDialogFailed);

    library.chooseImportPaths(QStringLiteral("root"));
    cancelDialog(chooser_);
    EXPECT_EQ(mediaCancelled.count(), 1);
    EXPECT_EQ(mediaFailed.count(), 0);
    EXPECT_EQ(projectCancelled.count(), 0);
    EXPECT_EQ(projectFailed.count(), 0);

    projectFile.chooseSaveAs();
    failDialog(chooser_);
    EXPECT_EQ(projectFailed.count(), 1);
    EXPECT_FALSE(projectFile.error().isEmpty());
    EXPECT_EQ(mediaFailed.count(), 0);
}

// A refused request (one dialog is already outstanding) must not arm a second
// client, and the outcome must still reach the client that started the dialog.
TEST_F(ChooserOwnershipTest, RefusedSecondClientCannotCaptureTheOutstandingOutcome) {
    nemo::ui::ProjectFileController projectFile{session_, workspace_, router_, chooser_};
    nemo::ui::MediaLibraryModel library{session_, importer_, &router_, &workspace_};
    library.setNativeFileChooser(&chooser_);

    QSignalSpy importChosen(&library, &nemo::ui::MediaLibraryModel::importPathsChosen);
    QSignalSpy mediaFailed(&library, &nemo::ui::MediaLibraryModel::mediaChooserFailed);
    QSignalSpy saveAsChosen(&projectFile, &nemo::ui::ProjectFileController::saveAsChosen);
    QSignalSpy projectFailed(&projectFile, &nemo::ui::ProjectFileController::fileDialogFailed);
    QSignalSpy projectCancelled(&projectFile, &nemo::ui::ProjectFileController::fileDialogCancelled);

    // The media import owns the dialog; the project Save As is refused.
    const QStringList mediaPaths{directory_.filePath(QStringLiteral("a.exr")),
                                 directory_.filePath(QStringLiteral("b.exr"))};
    library.chooseImportPaths(QStringLiteral("root"));
    ASSERT_TRUE(chooser_.inFlight());
    projectFile.chooseSaveAs();
    ASSERT_TRUE(chooser_.inFlight());
    chooseFiles(chooser_, mediaPaths);
    ASSERT_EQ(importChosen.count(), 1);
    EXPECT_EQ(importChosen.at(0).at(0).toStringList(), mediaPaths);
    EXPECT_EQ(importChosen.at(0).at(1).toString(), QStringLiteral("root"));
    EXPECT_EQ(saveAsChosen.count(), 0);
    EXPECT_EQ(projectFailed.count(), 0);
    EXPECT_EQ(projectCancelled.count(), 0);

    // The project Save As owns the dialog; the media import is refused.
    const QString projectPath = directory_.filePath(QStringLiteral("project.nemo"));
    projectFile.chooseSaveAs();
    ASSERT_TRUE(chooser_.inFlight());
    library.chooseImportPaths(QStringLiteral("root"));
    ASSERT_TRUE(chooser_.inFlight());
    chooseFiles(chooser_, {projectPath});
    ASSERT_EQ(saveAsChosen.count(), 1);
    EXPECT_EQ(saveAsChosen.at(0).at(0).value<QUrl>(), QUrl::fromLocalFile(projectPath));
    EXPECT_EQ(importChosen.count(), 1);
    EXPECT_EQ(mediaFailed.count(), 0);
}

// An initiator destroyed while its dialog is open must receive nothing, and the
// shared chooser must stay usable for the clients that are still alive.
TEST_F(ChooserOwnershipTest, DestroyedInitiatorReceivesNoOutcomeAndTheChooserRecovers) {
    {
        auto library = std::make_unique<nemo::ui::MediaLibraryModel>(session_, importer_, &router_, &workspace_);
        library->setNativeFileChooser(&chooser_);
        library->chooseImportPaths(QStringLiteral("root"));
        ASSERT_TRUE(chooser_.inFlight());
    }  // The import workflow is gone before its outcome arrives.
    ASSERT_TRUE(chooser_.inFlight());
    chooseFiles(chooser_, {directory_.filePath(QStringLiteral("shot.exr"))});
    EXPECT_FALSE(chooser_.inFlight());

    nemo::ui::ProjectFileController projectFile{session_, workspace_, router_, chooser_};
    QSignalSpy openChosen(&projectFile, &nemo::ui::ProjectFileController::openFileChosen);
    const QString projectPath = directory_.filePath(QStringLiteral("again.nemo"));
    projectFile.chooseOpenProject();
    ASSERT_TRUE(chooser_.inFlight());
    chooseFiles(chooser_, {projectPath});
    ASSERT_EQ(openChosen.count(), 1);
    EXPECT_EQ(openChosen.at(0).at(0).value<QUrl>(), QUrl::fromLocalFile(projectPath));
}

}  // namespace
