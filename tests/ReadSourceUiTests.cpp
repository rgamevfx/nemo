// Read node media control tests (issue #61).
//
// These cover the adapter the inspector's node-local file control consumes:
// choosing a real file registers a validated document source + Media Bin entry
// as one undoable command, a rejected path authors nothing, relink preserves
// the shared reference in place, timing edits validate the range, and clearing
// leaves the explicit empty state. Decode/format/color validation belongs to
// MediaTests/MediaImportTests; this file asserts only what the adapter adds.

#include "MediaLibraryModel.hpp"
#include "NativeFileChooser.hpp"
#include "ReadSourceController.hpp"

#include "nemo/core/commands/ReadSourceCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/MediaImportService.hpp"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {

using nemo::NodeId;
using nemo::ProjectSession;
using nemo::SourceReference;

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        QTest::qWait(20);
    }
    return predicate();
}

// A real, decodable EXR still: the shared import worker reads it through the
// image adapter, so the probe is not simulated.
std::filesystem::path writeExr(const std::filesystem::path& directory, const std::string& name) {
    nemo::CpuImage image(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            image.setPixel(x, y, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto path = directory / (name + ".exr");
    nemo::media::writeImage(path.string(), image, nemo::media::OutputPrecision::Half);
    return path;
}

class ReadSourceFixture {
public:
    ReadSourceFixture() : model_(session_, importer_), controller_(session_, model_, chooser_) {
        auto created = std::make_shared<NodeId>();
        const auto result =
            session_.submit(nemo::addNodeCommand(session_.document().rootNetworkId(), "source", "Read1", created),
                            {.expectedRevision = session_.revision()});
        EXPECT_TRUE(result.committed);
        node_ = *created;
    }

    [[nodiscard]] QString networkId() const { return QString::number(session_.document().rootNetworkId()); }
    [[nodiscard]] QString nodeId() const { return QString::number(node_); }
    [[nodiscard]] QVariantMap info() const { return controller_.info(networkId(), nodeId()); }
    [[nodiscard]] std::string nodeKey() const {
        const auto* instance = session_.document().network(session_.document().rootNetworkId()).graph().node(node_);
        if (instance == nullptr)
            return {};
        const auto found = instance->params.find("source");
        if (found == instance->params.end())
            return {};
        const auto* text = std::get_if<std::string>(&found->second);
        return text == nullptr ? std::string{} : *text;
    }

    QTemporaryDir temporary_;
    ProjectSession session_;
    nemo::media::MediaImportService importer_;
    nemo::ui::MediaLibraryModel model_;
    nemo::ui::NativeFileChooser chooser_;
    nemo::ui::ReadSourceController controller_;
    NodeId node_{nemo::kInvalidNode};
};

}  // namespace

TEST(ReadSourceUiTest, ChosenPathRegistersAValidatedReferenceAndEntryAsOneHistoryEntry) {
    ReadSourceFixture fixture;
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "plate");
    const auto baseline = fixture.session_.revision();

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    const std::string key = fixture.nodeKey();
    const auto reference = fixture.session_.document().sources.find(key);
    ASSERT_NE(reference, fixture.session_.document().sources.end());
    EXPECT_EQ(nemo::normalizedSourcePath(reference->second.path), nemo::normalizedSourcePath(still.string()));
    EXPECT_EQ(reference->second.revision, 1u);

    const nemo::MediaCatalogEntry* entry = nullptr;
    for (const auto& candidate : fixture.session_.document().mediaCatalog().entries())
        if (candidate.sourceKey == key)
            entry = &candidate;
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->provenance, "oiio");

    // One validated, undoable command; the controller's probe is a proposal,
    // not a document edit.
    EXPECT_EQ(fixture.session_.revision(), baseline + 1);

    const QVariantMap state = fixture.info();
    EXPECT_EQ(state.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    EXPECT_EQ(state.value(QStringLiteral("sourceKey")).toString(), QString::fromStdString(key));
    EXPECT_TRUE(state.value(QStringLiteral("error")).toString().isEmpty());

    ASSERT_TRUE(fixture.session_.undo({.expectedRevision = fixture.session_.revision()}).committed);
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("empty"));
}

TEST(ReadSourceUiTest, RejectedAndMissingPathsAuthorNothing) {
    ReadSourceFixture fixture;
    const auto baseline = fixture.session_.revision();

    // No file: the probe reports the missing path and no command is submitted.
    const auto missing = fixture.temporary_.path().toStdString() + "/absent.exr";
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(missing)));
    ASSERT_TRUE(waitFor([&] { return !fixture.info().value(QStringLiteral("error")).toString().isEmpty(); }));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), baseline);

    // Ambiguous color metadata: the adapter's own reason is surfaced and again
    // nothing is authored.
    const auto ambiguous = fixture.temporary_.path().toStdString() + "/ambiguous.tif";
    {
        nemo::CpuImage image(2, 2);
        image.setPixel(0, 0, {0.5F, 0.5F, 0.5F, 1.0F});
        image.setPixel(1, 0, {0.5F, 0.5F, 0.5F, 1.0F});
        image.setPixel(0, 1, {0.5F, 0.5F, 0.5F, 1.0F});
        image.setPixel(1, 1, {0.5F, 0.5F, 0.5F, 1.0F});
        nemo::media::writeImage(ambiguous, image, nemo::media::OutputPrecision::Float32);
    }
    fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(ambiguous));
    ASSERT_TRUE(waitFor([&] {
        const QString error = fixture.info().value(QStringLiteral("error")).toString();
        return !error.isEmpty() && !error.contains(QStringLiteral("absent"));
    }));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), baseline);
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("empty"));
}

TEST(ReadSourceUiTest, SharedReferenceIsReusedAndTimingAndClearAreSingleCommands) {
    ReadSourceFixture fixture;
    auto second = std::make_shared<NodeId>();
    ASSERT_TRUE(
        fixture.session_
            .submit(nemo::addNodeCommand(fixture.session_.document().rootNetworkId(), "source", "Read2", second),
                    {.expectedRevision = fixture.session_.revision()})
            .committed);
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "shared");

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();

    const auto revisionBeforeSecond = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), QString::number(*second),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] {
        const auto* instance =
            fixture.session_.document().network(fixture.session_.document().rootNetworkId()).graph().node(*second);
        return instance != nullptr && instance->params.contains("source");
    }));
    const auto* instance =
        fixture.session_.document().network(fixture.session_.document().rootNetworkId()).graph().node(*second);
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(std::get<std::string>(instance->params.at("source")), key);
    // Reuse is not a new media reference.
    EXPECT_EQ(fixture.session_.document().sources.size(), 1u);
    EXPECT_EQ(fixture.session_.revision(), revisionBeforeSecond + 1);

    // Timing/range edit on the shared reference, then clear on this node only.
    const auto revisionBeforeTiming = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.setSourceTiming(fixture.networkId(), fixture.nodeId(), QStringLiteral("1"),
                                                    QStringLiteral("2"), QStringLiteral("1"), QStringLiteral("40")));
    EXPECT_EQ(fixture.session_.revision(), revisionBeforeTiming + 1);
    const auto reference = fixture.session_.document().sources.find(key);
    ASSERT_NE(reference, fixture.session_.document().sources.end());
    EXPECT_EQ(reference->second.frameOffset, 1);
    EXPECT_EQ(reference->second.frameStep, 2);
    EXPECT_EQ(reference->second.firstFrame, std::optional<std::int64_t>{1});
    EXPECT_EQ(reference->second.lastFrame, std::optional<std::int64_t>{40});

    ASSERT_TRUE(fixture.controller_.clearSource(fixture.networkId(), fixture.nodeId()));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("empty"));
    // The shared reference and its Media Bin entry survive for the sibling.
    EXPECT_EQ(fixture.session_.document().sources.size(), 1u);
    EXPECT_FALSE(fixture.session_.document().mediaCatalog().entries().empty());
}

TEST(ReadSourceUiTest, RelinkPreservesIdentityAndReportsOfflineBeforehand) {
    ReadSourceFixture fixture;
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "relink");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();
    const auto reference = fixture.session_.document().sources.at(key);

    // Move the file: the control reports offline on the resolved path.
    const auto moved = fixture.temporary_.path().toStdString() + "/moved.exr";
    std::filesystem::rename(still, moved);
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("offline"));

    const auto revisionBefore = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.relinkSource(fixture.networkId(), fixture.nodeId(), QString::fromStdString(moved)));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.document().sources.at(key).path == moved; }));
    const auto relinked = fixture.session_.document().sources.at(key);
    EXPECT_EQ(relinked.revision, reference.revision + 1);
    EXPECT_EQ(fixture.nodeKey(), key);
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    EXPECT_EQ(fixture.session_.revision(), revisionBefore + 1);
}

// The registered namespaced editor (ParametersPanel's Loader -> this control)
// binds the node's real media state through ReadSourceController: the path
// field shows the resolved reference and clearing submits one command.
TEST(ReadSourceUiTest, EditorControlLoadsAndBindsThroughTheParameterEditorSeam) {
    ReadSourceFixture fixture;
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "editor");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("readSourceController"), &fixture.controller_);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(NEMO_UI_QML_DIR "/ReadSourceEditor.qml")));
    if (component.isError()) {
        for (const auto& error : component.errors())
            ADD_FAILURE() << error.toString().toStdString();
    }
    ASSERT_FALSE(component.isError());
    QVariantMap theme;
    theme.insert(QStringLiteral("fontSize"), 11);
    theme.insert(QStringLiteral("smallRadius"), 4);
    theme.insert(QStringLiteral("raised"), QStringLiteral("#282c31"));
    theme.insert(QStringLiteral("hover"), QStringLiteral("#343940"));
    theme.insert(QStringLiteral("border"), QStringLiteral("#30343a"));
    theme.insert(QStringLiteral("field"), QStringLiteral("#24272c"));
    theme.insert(QStringLiteral("text"), QStringLiteral("#dce0e6"));
    theme.insert(QStringLiteral("muted"), QStringLiteral("#979ea8"));
    theme.insert(QStringLiteral("accent"), QStringLiteral("#3485f6"));
    theme.insert(QStringLiteral("errorText"), QStringLiteral("#f0d0d0"));
    QScopedPointer<QObject> editor(
        component.createWithInitialProperties({{QStringLiteral("networkId"), fixture.networkId()},
                                               {QStringLiteral("nodeId"), fixture.nodeId()},
                                               {QStringLiteral("theme"), theme}}));
    ASSERT_FALSE(editor.isNull());

    const QString suffix = fixture.nodeId();
    auto* pathField = editor->findChild<QObject*>(QStringLiteral("readSourcePath_") + suffix);
    ASSERT_NE(pathField, nullptr);
    EXPECT_EQ(pathField->property("text").toString(), QString::fromStdString(still.string()));
    ASSERT_NE(editor->findChild<QObject*>(QStringLiteral("readSourceBrowse_") + suffix), nullptr);
    auto* clearButton = editor->findChild<QObject*>(QStringLiteral("readSourceClear_") + suffix);
    ASSERT_NE(clearButton, nullptr);

    const auto revisionBefore = fixture.session_.revision();
    ASSERT_TRUE(QMetaObject::invokeMethod(clearButton, "clicked"));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.session_.revision(), revisionBefore + 1);
}
