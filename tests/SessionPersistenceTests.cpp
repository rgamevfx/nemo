#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"

namespace {

namespace fs = std::filesystem;
using namespace nemo;

void countObserver(void* context) noexcept {
    ++*static_cast<int*>(context);
}

Document makeDocument(std::string name) {
    Document document;
    document.name = std::move(name);
    auto& network = document.network(document.rootNetworkId());
    static_cast<void>(network.graph().addNode("testpattern", "plate"));
    return document;
}

Document makeParameterDocument() {
    Document document;
    document.name = "parameters";
    auto& network = document.network(document.rootNetworkId());
    static_cast<void>(network.graph().addNode("constcolor", "tint"));
    return document;
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.good()) << "cannot write " << path;
    stream << text;
}

std::string readText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.good()) << "cannot read " << path;
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

constexpr fs::perms kPermissionMask = fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all;

unsigned modeOf(const fs::path& path) {
    return static_cast<unsigned>(fs::status(path).permissions() & kPermissionMask);
}

ProjectWriteRequest makeAutosaveRequest(const fs::path& projectTarget, const std::string& name) {
    ProjectWriteRequest request;
    request.snapshot = std::make_shared<const Document>(makeDocument(name));
    request.target = projectTarget;
    return request;
}

ProjectWriteResult writeDocument(const fs::path& target, const Document& document,
                                 PathPolicy policy = PathPolicy::KeepStored, bool backup = true) {
    ProjectWriteRequest request;
    request.snapshot = std::make_shared<const Document>(document);
    request.target = target;
    request.pathPolicy = policy;
    request.backup = backup;
    return ProjectFile::writeAtomic(request);
}

class SessionPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code ec;
        dir_ = fs::temp_directory_path(ec) / ("nemo-session-file-" + std::to_string(static_cast<long>(::getpid())) +
                                              "-" + std::to_string(counter_++));
        fs::remove_all(dir_, ec);
        ASSERT_TRUE(fs::create_directories(dir_, ec) || !ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path dir_;

private:
    static int counter_;
};

int SessionPersistenceTest::counter_ = 0;

TEST_F(SessionPersistenceTest, ReadResolvesRelativeReferencesAndWarnsOnMissingDependencies) {
    const fs::path media = dir_ / "media";
    ASSERT_TRUE(fs::create_directories(media));
    writeText(media / "plate.exr", "x");

    Document document = makeDocument("relative");
    document.sources["plate"].path = "media/plate.exr";
    document.sources["gone"].path = "media/gone.exr";
    ASSERT_TRUE(writeDocument(dir_ / "shot.nemo", document, PathPolicy::KeepStored).ok);

    const ProjectReadResult read = ProjectFile::read(dir_ / "shot.nemo");
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.sourcePath, (dir_ / "shot.nemo").lexically_normal());
    EXPECT_EQ(read.document.sources.at("plate").path, (media / "plate.exr").lexically_normal().string());
    EXPECT_EQ(read.document.sources.at("gone").path, (media / "gone.exr").lexically_normal().string());

    ASSERT_EQ(read.references.size(), 2u);
    bool plateResolved = false;
    bool goneMissing = false;
    for (const auto& reference : read.references) {
        if (reference.identity == "source:plate")
            plateResolved = reference.state == ReferenceState::Present && reference.storedPath == "media/plate.exr";
        if (reference.identity == "source:gone")
            goneMissing = reference.state == ReferenceState::Missing;
    }
    EXPECT_TRUE(plateResolved);
    EXPECT_TRUE(goneMissing);

    bool warned = false;
    for (const auto& warning : read.warnings)
        warned = warned || warning.find("missing dependency source:gone") != std::string::npos;
    EXPECT_TRUE(warned);
}

TEST_F(SessionPersistenceTest, SaveAsRebasesResolvedTargetsWithoutCopyingResources) {
    const fs::path media = dir_ / "media";
    ASSERT_TRUE(fs::create_directories(media));
    writeText(media / "plate.exr", "x");

    Document document = makeDocument("rebase");
    document.sources["plate"].path = (media / "plate.exr").string();

    const fs::path moved = dir_ / "moved";
    ASSERT_TRUE(fs::create_directories(moved));
    const fs::path target = moved / "shot.nemo";
    const ProjectWriteResult written = writeDocument(target, document, PathPolicy::RebaseRelative);
    ASSERT_TRUE(written.ok) << written.error.message;

    const ProjectReadResult read = ProjectFile::read(target);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.document.sources.at("plate").path, (media / "plate.exr").lexically_normal().string());
    ASSERT_EQ(read.references.size(), 1u);
    EXPECT_TRUE(read.references.front().relativeCapable);
    EXPECT_EQ(read.references.front().resolvedPath, (media / "plate.exr").lexically_normal().string());
    // The reference round-trips and nothing was copied next to the new target.
    EXPECT_EQ(fs::file_size(media / "plate.exr"), 1u);
    EXPECT_FALSE(fs::exists(moved / "media"));
}

TEST_F(SessionPersistenceTest, SequencePatternReferencesReportUnresolvedState) {
    const fs::path media = dir_ / "media";
    // The parent directory exists; that must not be reported as a present
    // sequence, because frame-range resolution is media-owned.
    ASSERT_TRUE(fs::create_directories(media));
    Document document = makeDocument("sequence");
    document.sources["seq"].path = "media/plate.####.exr";
    ASSERT_TRUE(writeDocument(dir_ / "shot.nemo", document, PathPolicy::KeepStored).ok);

    const ProjectReadResult read = ProjectFile::read(dir_ / "shot.nemo");
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_EQ(read.references.size(), 1u);
    EXPECT_EQ(read.references.front().state, ReferenceState::Unresolved);
    bool unresolvedDiagnostic = false;
    bool missingClaim = false;
    for (const auto& warning : read.warnings) {
        unresolvedDiagnostic =
            unresolvedDiagnostic || warning.find("unresolved sequence dependency source:seq") != std::string::npos;
        missingClaim = missingClaim || warning.find("missing dependency source:seq") != std::string::npos;
    }
    EXPECT_TRUE(unresolvedDiagnostic);
    EXPECT_FALSE(missingClaim);
    // The in-memory target is still resolved absolutely for the media pass.
    EXPECT_EQ(read.document.sources.at("seq").path, (media / "plate.####.exr").lexically_normal().string());
}

TEST_F(SessionPersistenceTest, BackupRecoveryInfersAndProtectsTheOriginalTarget) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    ASSERT_TRUE(writeDocument(target, makeDocument("v2")).ok);
    const fs::path backup = dir_ / "shot.nemo.bak";
    ASSERT_EQ(ProjectFile::originalTargetForSlot(backup), target.lexically_normal());

    const ProjectReadResult recovered = ProjectFile::readRecovery(backup);
    ASSERT_TRUE(recovered.ok) << recovered.error.message;
    EXPECT_TRUE(recovered.recovered);
    EXPECT_TRUE(recovered.sourcePath.empty());
    EXPECT_EQ(recovered.recoveryOriginal, target.lexically_normal());
    EXPECT_EQ(recovered.document.name, "v1");

    ProjectSession session(makeDocument("unused"));
    ASSERT_TRUE(session.open(recovered).replaced);
    const ProjectWriteRequest attempt = session.prepareSave(target);
    EXPECT_EQ(attempt.protectedTarget, target.lexically_normal());
    EXPECT_EQ(ProjectFile::writeAtomic(attempt).error.code, ProjectFileError::Code::ProtectedTarget);
    EXPECT_EQ(ProjectFile::read(target).document.name, "v2");
}

TEST_F(SessionPersistenceTest, ProtectedTargetFollowsHardLinks) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    const fs::path link = dir_ / "alias.nemo";
    std::error_code ec;
    fs::create_hard_link(target, link, ec);
    if (ec)
        GTEST_SKIP() << "hard links are unavailable: " << ec.message();

    ProjectWriteRequest request;
    request.snapshot = std::make_shared<const Document>(makeDocument("v2"));
    request.target = link;
    request.protectedTarget = target;
    const ProjectWriteResult refused = ProjectFile::writeAtomic(request);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, ProjectFileError::Code::ProtectedTarget);
    EXPECT_EQ(ProjectFile::read(target).document.name, "v1");
}

TEST_F(SessionPersistenceTest, AtomicWritePreservesExistingTargetPermissions) {
    const fs::path target = dir_ / "private.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    // A private project must not be widened to the default creation mode.
    const fs::perms expected = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    std::error_code ec;
    fs::permissions(target, expected, fs::perm_options::replace, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(modeOf(target), static_cast<unsigned>(expected));

    const ProjectWriteResult second = writeDocument(target, makeDocument("v2"));
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(modeOf(target), static_cast<unsigned>(expected));
    ASSERT_FALSE(second.backup.empty());
    EXPECT_EQ(modeOf(second.backup), static_cast<unsigned>(expected));
}

TEST_F(SessionPersistenceTest, BackupFailureRemovesTheCreatedTemporaryAndKeepsTarget) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    const fs::path backup = dir_ / "shot.nemo.bak";
    ASSERT_TRUE(fs::create_directories(backup));
    writeText(backup / "occupied", "x");

    const ProjectWriteResult failed = writeDocument(target, makeDocument("v2"));
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.error.code, ProjectFileError::Code::BackupFailed);
    EXPECT_EQ(ProjectFile::read(target).document.name, "v1");

    std::size_t temporaries = 0;
    for (const auto& entry : fs::directory_iterator(dir_))
        if (entry.path().filename().string().find(".nemo-tmp") != std::string::npos)
            ++temporaries;
    EXPECT_EQ(temporaries, 0u);
}

TEST_F(SessionPersistenceTest, BackupReplacesSymlinkWithoutFollowingIt) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    const std::string previousBytes = readText(target);
    const fs::path victim = dir_ / "victim.txt";
    writeText(victim, "victim data");
    const fs::path backup = dir_ / "shot.nemo.bak";
    std::error_code ec;
    fs::create_symlink(victim, backup, ec);
    if (ec)
        GTEST_SKIP() << "symlinks are unavailable: " << ec.message();

    const ProjectWriteResult written = writeDocument(target, makeDocument("v2"));
    ASSERT_TRUE(written.ok) << written.error.message;
    EXPECT_EQ(ProjectFile::read(target).document.name, "v2");
    // The referent is untouched; the previous-good backup is published at .bak
    // itself as a regular file with the exact prior bytes.
    EXPECT_EQ(readText(victim), "victim data");
    EXPECT_FALSE(fs::is_symlink(backup));
    EXPECT_EQ(readText(backup), previousBytes);
    EXPECT_EQ(ProjectFile::read(backup).document.name, "v1");
}

TEST_F(SessionPersistenceTest, AtomicWriteKeepsPreviousGoodBackup) {
    const fs::path target = dir_ / "shot.nemo";
    const ProjectWriteResult first = writeDocument(target, makeDocument("v1"));
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_TRUE(first.backup.empty());

    const ProjectWriteResult second = writeDocument(target, makeDocument("v2"));
    ASSERT_TRUE(second.ok) << second.error.message;
    ASSERT_FALSE(second.backup.empty());
    ASSERT_TRUE(fs::exists(second.backup));
    EXPECT_EQ(ProjectFile::read(target).document.name, "v2");
    EXPECT_EQ(ProjectFile::read(second.backup).document.name, "v1");
}

TEST_F(SessionPersistenceTest, AtomicWriteFailurePreservesPreviousGoodTarget) {
    // A filename at the platform limit cannot host the longer exclusive
    // temporary sibling name: creation fails on a real filesystem obstacle
    // after the previous good target was already accepted.
    const fs::path target = dir_ / std::string(255, 'p');
    writeText(target, saveDocument(makeDocument("v1")).dump());
    ASSERT_TRUE(ProjectFile::read(target).ok);

    const ProjectWriteResult failed = writeDocument(target, makeDocument("v2"));
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.error.code, ProjectFileError::Code::WriteFailed);
    EXPECT_EQ(ProjectFile::read(target).document.name, "v1");
    // The sibling backup name exceeds NAME_MAX here, so existence must be
    // probed without throwing; a failed write creates no backup either way.
    std::error_code backupError;
    const bool backupExists = fs::exists(dir_ / (std::string(255, 'p') + ".bak"), backupError);
    EXPECT_FALSE(backupExists);
}

TEST_F(SessionPersistenceTest, AtomicWriteNeverTouchesAUserFileAtThePredictableTempName) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    const fs::path decoy = dir_ / "shot.nemo.nemo-tmp";
    writeText(decoy, "user data");

    const ProjectWriteResult written = writeDocument(target, makeDocument("v2"));
    ASSERT_TRUE(written.ok) << written.error.message;
    EXPECT_EQ(readText(decoy), "user data");
    EXPECT_EQ(ProjectFile::read(target).document.name, "v2");
}

TEST_F(SessionPersistenceTest, CorruptExistingTargetIsNotDestructivelyReplaced) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    ASSERT_TRUE(writeDocument(target, makeDocument("v2")).ok);
    const fs::path backup = dir_ / "shot.nemo.bak";
    ASSERT_EQ(ProjectFile::read(backup).document.name, "v1");

    writeText(target, "{truncated");
    const ProjectWriteResult refused = writeDocument(target, makeDocument("v3"));
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, ProjectFileError::Code::UnsupportedTarget);
    EXPECT_EQ(readText(target), "{truncated");
    EXPECT_EQ(ProjectFile::read(backup).document.name, "v1");
}

TEST_F(SessionPersistenceTest, TruncatedEmptyTargetIsNotDestructivelyReplaced) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    ASSERT_TRUE(writeDocument(target, makeDocument("v2")).ok);
    const fs::path backup = dir_ / "shot.nemo.bak";
    ASSERT_EQ(ProjectFile::read(backup).document.name, "v1");

    // An externally truncated project (zero bytes) must not cost the last good
    // recovery.
    writeText(target, "");
    ASSERT_EQ(fs::file_size(target), 0u);

    const ProjectWriteResult refused = writeDocument(target, makeDocument("v3"));
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, ProjectFileError::Code::UnsupportedTarget);
    EXPECT_EQ(fs::file_size(target), 0u);
    EXPECT_EQ(ProjectFile::read(backup).document.name, "v1");
}

TEST_F(SessionPersistenceTest, UnsupportedNewerTargetIsNotDestructivelyReplaced) {
    const fs::path target = dir_ / "future.nemo";
    const std::string unsupported =
        nlohmann::json{{"format", "nemo"},
                       {"schema", Document::kSchemaVersion},
                       {"requiredFeatures", {{{"id", "futureEditorialModel"}, {"version", 1}}}}}
            .dump();
    writeText(target, unsupported);

    const ProjectWriteResult refused = writeDocument(target, makeDocument("v1"));
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, ProjectFileError::Code::UnsupportedTarget);
    EXPECT_NE(refused.error.message.find("futureEditorialModel"), std::string::npos);
    EXPECT_EQ(readText(target), unsupported);
}

TEST_F(SessionPersistenceTest, AtomicWriteRejectsDirectoryTarget) {
    const fs::path target = dir_ / "project-dir";
    ASSERT_TRUE(fs::create_directories(target));
    const ProjectWriteResult result = writeDocument(target, makeDocument("v1"));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, ProjectFileError::Code::TargetIsDirectory);
}

TEST_F(SessionPersistenceTest, AutosaveStoreIsBoundedAndNeverWritesTheTarget) {
    const fs::path target = dir_ / "auto.nemo";
    AutosaveStore store(target, 3);
    EXPECT_EQ(store.slotLimit(), 3u);
    for (int index = 0; index < 5; ++index) {
        ProjectWriteRequest request;
        request.snapshot = std::make_shared<const Document>(makeDocument("v" + std::to_string(index)));
        request.target = target;
        const ProjectWriteResult result = store.write(request);
        ASSERT_TRUE(result.ok) << result.error.message;
        EXPECT_NE(result.target, target);
    }
    EXPECT_EQ(store.existingSlots().size(), 3u);
    EXPECT_FALSE(fs::exists(target));
    const auto latest = store.latest();
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(ProjectFile::read(*latest).document.name, "v4");
}

TEST_F(SessionPersistenceTest, CorruptOwnedAutosaveSlotIsReplacedWithoutLosingOtherSlots) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("saved")).ok);
    ASSERT_TRUE(writeDocument(target, makeDocument("saved2")).ok);
    const fs::path backup = dir_ / "shot.nemo.bak";
    const std::string targetBytes = readText(target);
    const std::string backupBytes = readText(backup);

    AutosaveStore store(target, 2);
    ASSERT_TRUE(store.write(makeAutosaveRequest(target, "v0")).ok);
    ASSERT_TRUE(store.write(makeAutosaveRequest(target, "v1")).ok);
    const fs::path first = store.slotPath(0);
    const fs::path second = store.slotPath(1);
    ASSERT_TRUE(fs::exists(first));
    ASSERT_TRUE(fs::exists(second));

    // The oldest owned slot becomes unreadable: autosave must replace only that
    // slot instead of wedging, and must keep the other valid slot.
    writeText(first, "{corrupt");
    const ProjectWriteResult third = store.write(makeAutosaveRequest(target, "v2"));
    ASSERT_TRUE(third.ok) << third.error.message;
    EXPECT_EQ(third.target, first);
    EXPECT_EQ(ProjectFile::read(first).document.name, "v2");
    EXPECT_EQ(ProjectFile::read(second).document.name, "v1");
    EXPECT_EQ(readText(target), targetBytes);
    EXPECT_EQ(readText(backup), backupBytes);
}

TEST_F(SessionPersistenceTest, AutosaveNeverDestroysNewerSchemaRecoveryData) {
    const fs::path target = dir_ / "shot.nemo";
    AutosaveStore store(target, 2);
    const fs::path future = store.slotPath(0);
    const std::string futureBytes =
        nlohmann::json{{"format", "nemo"},
                       {"schema", Document::kSchemaVersion},
                       {"requiredFeatures", {{{"id", "futureEditorialModel"}, {"version", 1}}}}}
            .dump();
    writeText(future, futureBytes);

    const ProjectWriteResult written = store.write(makeAutosaveRequest(target, "v1"));
    ASSERT_TRUE(written.ok) << written.error.message;
    // Newer recovery data survives an older store's rotation, and the bound
    // still holds by using the free slot.
    EXPECT_EQ(readText(future), futureBytes);
    EXPECT_NE(written.target, future);
    EXPECT_EQ(store.existingSlots().size(), 2u);
}

TEST_F(SessionPersistenceTest, AutosaveFailsVisiblyWhenEverySlotIsNewerData) {
    const fs::path target = dir_ / "shot.nemo";
    AutosaveStore store(target, 1);
    const fs::path future = store.slotPath(0);
    const std::string futureBytes =
        nlohmann::json{{"format", "nemo"},
                       {"schema", Document::kSchemaVersion},
                       {"requiredFeatures", {{{"id", "futureEditorialModel"}, {"version", 1}}}}}
            .dump();
    writeText(future, futureBytes);

    const ProjectWriteResult blocked = store.write(makeAutosaveRequest(target, "v1"));
    EXPECT_FALSE(blocked.ok);
    EXPECT_EQ(blocked.error.code, ProjectFileError::Code::Unsupported);
    EXPECT_EQ(readText(future), futureBytes);
}

TEST_F(SessionPersistenceTest, RecoveryIsAnUnsavedCopyThatProtectsTheOriginalTarget) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("saved")).ok);

    ProjectSession draft(makeDocument("draft"));
    AutosaveStore store(target, kDefaultAutosaveSlots);
    const ProjectWriteResult autosaved = store.write(draft.prepareSave(target));
    ASSERT_TRUE(autosaved.ok) << autosaved.error.message;
    const auto slot = store.latest();
    ASSERT_TRUE(slot.has_value());

    const ProjectReadResult recovered = ProjectFile::readRecovery(*slot);
    ASSERT_TRUE(recovered.ok) << recovered.error.message;
    EXPECT_TRUE(recovered.recovered);
    EXPECT_TRUE(recovered.sourcePath.empty());
    EXPECT_EQ(recovered.recoveryOriginal, target.lexically_normal());
    EXPECT_FALSE(recovered.warnings.empty());

    ProjectSession reopened(makeDocument("unused"));
    const ProjectReplaceResult opened = reopened.open(recovered);
    ASSERT_TRUE(opened.replaced) << (opened.error ? opened.error->message : std::string{});
    EXPECT_TRUE(reopened.isDirty());
    EXPECT_TRUE(reopened.projectPath().empty());
    EXPECT_TRUE(reopened.recovered());

    ProjectWriteRequest request = reopened.prepareSave(target);
    EXPECT_EQ(request.protectedTarget, target.lexically_normal());
    const ProjectWriteResult blocked = ProjectFile::writeAtomic(request);
    EXPECT_FALSE(blocked.ok);
    EXPECT_EQ(blocked.error.code, ProjectFileError::Code::ProtectedTarget);
    EXPECT_EQ(ProjectFile::read(target).document.name, "saved");

    // Save As to a copy keeps the recovered work; the guard survives for the
    // session so the original is still protected afterwards.
    const fs::path copy = dir_ / "recovered.nemo";
    const ProjectWriteRequest saveAs = reopened.prepareSave(copy);
    const ProjectWriteResult written = ProjectFile::writeAtomic(saveAs);
    ASSERT_TRUE(written.ok) << written.error.message;
    EXPECT_EQ(ProjectFile::read(copy).document.name, "draft");
    EXPECT_EQ(ProjectFile::read(target).document.name, "saved");

    ASSERT_TRUE(reopened.commitSave(saveAs, written).committed);
    EXPECT_FALSE(reopened.isDirty());
    EXPECT_TRUE(reopened.recovered());
    EXPECT_EQ(reopened.projectPath(), copy.lexically_normal());
    EXPECT_FALSE(reopened.canUndo());

    const ProjectWriteRequest originalAttempt = reopened.prepareSave(target);
    EXPECT_EQ(ProjectFile::writeAtomic(originalAttempt).error.code, ProjectFileError::Code::ProtectedTarget);
    EXPECT_EQ(ProjectFile::read(target).document.name, "saved");

    const ProjectWriteRequest copySave = reopened.prepareSave(copy);
    EXPECT_TRUE(ProjectFile::writeAtomic(copySave).ok);
}

TEST_F(SessionPersistenceTest, StaleSaveCompletionAfterReplacementDoesNotAdoptTheNewProject) {
    ProjectSession session(makeDocument("first"));
    const ProjectWriteRequest stale = session.prepareSave(dir_ / "stale.nemo");
    const std::uint64_t generation = session.projectGeneration();
    EXPECT_EQ(stale.projectGeneration, generation);

    const fs::path opened = dir_ / "opened.nemo";
    ASSERT_TRUE(writeDocument(opened, makeDocument("second")).ok);
    ProjectReadResult read = ProjectFile::read(opened);
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_TRUE(session.replaceDocument(std::move(read), false).replaced);
    EXPECT_NE(session.projectGeneration(), generation);
    EXPECT_EQ(session.projectPath(), opened.lexically_normal());
    const std::uint64_t revisionAfterOpen = session.revision();

    const ProjectWriteResult written = ProjectFile::writeAtomic(stale);
    ASSERT_TRUE(written.ok) << written.error.message;
    const EditResult committed = session.commitSave(stale, written);
    EXPECT_FALSE(committed.committed);
    ASSERT_TRUE(committed.error.has_value());
    EXPECT_EQ(committed.error->code, EditErrorCode::IoError);
    // The newly opened project keeps its path, baseline and clean state.
    EXPECT_EQ(session.projectPath(), opened.lexically_normal());
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(session.revision(), revisionAfterOpen);
    EXPECT_FALSE(session.lastFileError().empty());
}

TEST_F(SessionPersistenceTest, StaleSaveCompletionNeverClearsNewerEdits) {
    ProjectSession session(makeDocument("v1"));
    const fs::path target = dir_ / "shot.nemo";
    EXPECT_FALSE(session.isDirty());

    const ProjectWriteRequest job = session.prepareSave(target);
    const NetworkId network = session.document().rootNetworkId();
    const NodeId node = session.document().network(network).graph().nodeByName("plate")->id;
    const EditResult edited =
        session.submit(renameNodeCommand(network, node, "renamed"), EditOptions{session.revision(), {}});
    ASSERT_TRUE(edited.committed) << (edited.error ? edited.error->message : std::string{});
    EXPECT_TRUE(session.isDirty());

    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    const EditResult committed = session.commitSave(job, written);
    ASSERT_TRUE(committed.committed) << (committed.error ? committed.error->message : std::string{});

    // The session advanced past the written snapshot: it stays dirty and the
    // newer edit is still present.
    EXPECT_TRUE(session.isDirty());
    EXPECT_EQ(session.savedRevision(), job.expectedRevision);
    EXPECT_GT(session.revision(), job.expectedRevision);
    EXPECT_EQ(session.projectPath(), target.lexically_normal());
    EXPECT_NE(session.document().network(network).graph().nodeByName("renamed"), nullptr);
    EXPECT_EQ(ProjectFile::read(target).document.network(network).graph().nodeByName("renamed"), nullptr);
}

TEST_F(SessionPersistenceTest, UndoToSavedStateIsClean) {
    ProjectSession session(makeDocument("v1"));
    EXPECT_FALSE(session.isDirty());
    const NetworkId network = session.document().rootNetworkId();
    const NodeId node = session.document().network(network).graph().nodeByName("plate")->id;

    ASSERT_TRUE(
        session.submit(renameNodeCommand(network, node, "renamed"), EditOptions{session.revision(), {}}).committed);
    EXPECT_TRUE(session.isDirty());
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    EXPECT_FALSE(session.isDirty());
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    EXPECT_TRUE(session.isDirty());
}

TEST_F(SessionPersistenceTest, ReplacementKeepsObserversAndClearsSessionHistory) {
    ProjectSession session(makeDocument("first"));
    const NetworkId network = session.document().rootNetworkId();
    const NodeId node = session.document().network(network).graph().nodeByName("plate")->id;
    ASSERT_TRUE(
        session.submit(renameNodeCommand(network, node, "renamed"), EditOptions{session.revision(), {}}).committed);
    ASSERT_TRUE(session.canUndo());

    int notifications = 0;
    auto subscription = session.subscribe(&notifications, countObserver);
    const std::uint64_t before = session.revision();

    const fs::path target = dir_ / "replacement.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("second")).ok);
    ProjectReadResult read = ProjectFile::read(target);
    ASSERT_TRUE(read.ok) << read.error.message;

    const ProjectReplaceResult replaced = session.replaceDocument(std::move(read), false);
    ASSERT_TRUE(replaced.replaced);
    EXPECT_EQ(replaced.revision, before + 1);
    EXPECT_EQ(notifications, 1);
    EXPECT_FALSE(session.canUndo());
    EXPECT_FALSE(session.canRedo());
    EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(session.projectPath(), target.lexically_normal());
    EXPECT_TRUE(session.changesSince(before).resyncRequired);

    // The observer registration survived the in-place replacement.
    const NetworkId replacedNetwork = session.document().rootNetworkId();
    const NodeId replacedNode = session.document().network(replacedNetwork).graph().nodeByName("plate")->id;
    ASSERT_TRUE(
        session.submit(renameNodeCommand(replacedNetwork, replacedNode, "again"), EditOptions{session.revision(), {}})
            .committed);
    EXPECT_EQ(notifications, 2);
    EXPECT_TRUE(session.isDirty());
}

TEST_F(SessionPersistenceTest, ReplacementClearsGesturesAndRequestDeduplication) {
    ProjectSession session(makeParameterDocument());
    const NetworkId network = session.document().rootNetworkId();
    const NodeId node = session.document().network(network).graph().nodeByName("tint")->id;
    const ParameterAddress address{network, node, "color"};

    const EditResult identified =
        session.submit(renameNodeCommand(network, node, "identified"), EditOptions{session.revision(), "op-1"});
    ASSERT_TRUE(identified.committed) << (identified.error ? identified.error->message : std::string{});

    const ParameterGestureResult gesture =
        session.beginParameterGesture({ParameterEdit{address, ParameterValue{ColorValue{{1.F, 0.F, 0.F, 1.F}}}}},
                                      EditOptions{session.revision(), {}});
    ASSERT_NE(gesture.token, 0u) << (gesture.result.error ? gesture.result.error->message : std::string{});

    const fs::path target = dir_ / "replacement.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("second")).ok);
    ProjectReadResult read = ProjectFile::read(target);
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_TRUE(session.replaceDocument(std::move(read), false).replaced);

    const ParameterGestureResult abandoned = session.updateParameterGesture(gesture.token, {});
    ASSERT_TRUE(abandoned.result.error.has_value());
    EXPECT_EQ(abandoned.result.error->code, EditErrorCode::Unavailable);

    const NetworkId replacedNetwork = session.document().rootNetworkId();
    const NodeId replacedNode = session.document().network(replacedNetwork).graph().nodeByName("plate")->id;
    const EditResult reused = session.submit(renameNodeCommand(replacedNetwork, replacedNode, "fresh"),
                                             EditOptions{session.revision(), "op-1"});
    ASSERT_TRUE(reused.committed) << (reused.error ? reused.error->message : std::string{});
    EXPECT_NE(session.document().network(replacedNetwork).graph().nodeByName("fresh"), nullptr);
}

TEST_F(SessionPersistenceTest, SaveCompletionCreatesNoUndoEntry) {
    ProjectSession session(makeDocument("v1"));
    const fs::path target = dir_ / "shot.nemo";
    const ProjectWriteRequest job = session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(job, written).committed);
    EXPECT_FALSE(session.canUndo());
    EXPECT_FALSE(session.canRedo());
    EXPECT_FALSE(session.isDirty());
    EXPECT_TRUE(session.lastFileError().empty());
}

TEST_F(SessionPersistenceTest, PresentationEnvelopeRoundTripsAndStaysHeadless) {
    ProjectSession session(makeDocument("presented"));
    EXPECT_FALSE(session.isDirty());
    session.setPresentation(makePresentationEnvelope(nlohmann::json{{"workspace", "w1"}, {"restore", true}}));
    EXPECT_TRUE(session.isDirty());

    const fs::path target = dir_ / "presented.nemo";
    const ProjectWriteRequest job = session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(job, written).committed);
    EXPECT_FALSE(session.isDirty());

    const ProjectReadResult read = ProjectFile::read(target);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.presentation, session.presentation());
    EXPECT_EQ(presentationData(read.presentation).at("workspace").get<std::string>(), "w1");
    // A newer independent envelope version is preserved verbatim and surfaced.
    session.setPresentation(nlohmann::json{{"version", kPresentationEnvelopeVersion + 1}, {"data", {{"future", 1}}}});
    const ProjectWriteRequest future = session.prepareSave(target);
    ASSERT_TRUE(ProjectFile::writeAtomic(future).ok);
    const ProjectReadResult futureRead = ProjectFile::read(target);
    EXPECT_EQ(futureRead.presentation, session.presentation());
    bool versionWarning = false;
    for (const auto& warning : futureRead.warnings)
        versionWarning = versionWarning || warning.find("presentation envelope version") != std::string::npos;
    EXPECT_TRUE(versionWarning);
}

TEST_F(SessionPersistenceTest, WrittenProjectCarriesFormatAndRequiredFeatures) {
    const fs::path target = dir_ / "formatted.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    const nlohmann::json written = nlohmann::json::parse(readText(target));
    EXPECT_EQ(written.at("format").get<std::string>(), kProjectFormat);
    EXPECT_TRUE(written.contains("requiredFeatures"));
    EXPECT_FALSE(written.contains("presentation")) << "absent until the session supplies one";
}

TEST_F(SessionPersistenceTest, ReadValidatesFormatDiscriminatorSchemaAndCorruption) {
    // A legacy document without the format discriminator or feature metadata
    // still migrates through its schema version.
    nlohmann::json legacyJson = saveDocument(makeDocument("legacy"));
    legacyJson.erase("format");
    legacyJson.erase("requiredFeatures");
    writeText(dir_ / "legacy.nemo", legacyJson.dump());
    const ProjectReadResult legacy = ProjectFile::read(dir_ / "legacy.nemo");
    EXPECT_TRUE(legacy.ok) << legacy.error.message;
    EXPECT_EQ(legacy.document.name, "legacy");

    writeText(dir_ / "other.nemo", nlohmann::json{{"format", "other"}, {"schema", 4}}.dump());
    const ProjectReadResult foreign = ProjectFile::read(dir_ / "other.nemo");
    EXPECT_FALSE(foreign.ok);
    EXPECT_EQ(foreign.error.code, ProjectFileError::Code::Unsupported);

    writeText(dir_ / "new.nemo", nlohmann::json{{"format", "nemo"}, {"schema", Document::kSchemaVersion + 1}}.dump());
    const ProjectReadResult newer = ProjectFile::read(dir_ / "new.nemo");
    EXPECT_FALSE(newer.ok);
    EXPECT_EQ(newer.error.code, ProjectFileError::Code::Unsupported);
    EXPECT_NE(newer.error.message.find("schema"), std::string::npos);

    writeText(dir_ / "feature.nemo",
              nlohmann::json{{"format", "nemo"},
                             {"schema", Document::kSchemaVersion},
                             {"requiredFeatures", {{{"id", "futureEditorialModel"}, {"version", 1}}}}}
                  .dump());
    const ProjectReadResult unresolved = ProjectFile::read(dir_ / "feature.nemo");
    EXPECT_FALSE(unresolved.ok);
    EXPECT_EQ(unresolved.error.code, ProjectFileError::Code::Unsupported);
    EXPECT_NE(unresolved.error.message.find("futureEditorialModel"), std::string::npos);

    // Structurally malformed documents are parse failures, not unsupported
    // features, so callers can tell corruption from a newer format.
    writeText(dir_ / "structural.nemo", nlohmann::json{{"format", "nemo"}}.dump());
    EXPECT_EQ(ProjectFile::read(dir_ / "structural.nemo").error.code, ProjectFileError::Code::ParseFailed);

    writeText(dir_ / "bad.nemo", "{not json");
    EXPECT_EQ(ProjectFile::read(dir_ / "bad.nemo").error.code, ProjectFileError::Code::ParseFailed);
    EXPECT_EQ(ProjectFile::read(dir_).error.code, ProjectFileError::Code::NotAFile);
    EXPECT_EQ(ProjectFile::read(dir_ / "absent.nemo").error.code, ProjectFileError::Code::NotFound);
}

TEST_F(SessionPersistenceTest, MalformedColorConfigRejectsReadAndPreservesExistingTarget) {
    const fs::path target = dir_ / "shot.nemo";
    ASSERT_TRUE(writeDocument(target, makeDocument("v1")).ok);
    ASSERT_TRUE(writeDocument(target, makeDocument("v2")).ok);
    const fs::path backup = dir_ / "shot.nemo.bak";
    ASSERT_EQ(ProjectFile::read(backup).document.name, "v1");

    // A non-string authored field must not be silently discarded on load.
    nlohmann::json malformed = saveDocument(makeDocument("v3"));
    malformed["colorConfig"] = nlohmann::json{{"unexpected", true}};
    writeText(target, malformed.dump());

    const ProjectReadResult read = ProjectFile::read(target);
    EXPECT_FALSE(read.ok);
    EXPECT_EQ(read.error.code, ProjectFileError::Code::ParseFailed);
    EXPECT_NE(read.error.message.find("colorConfig"), std::string::npos);

    // Saving over it is refused: neither the authored bytes nor the
    // previous-good backup are lost.
    const ProjectWriteResult refused = writeDocument(target, makeDocument("v4"));
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, ProjectFileError::Code::UnsupportedTarget);
    EXPECT_EQ(readText(target), malformed.dump());
    EXPECT_EQ(ProjectFile::read(backup).document.name, "v1");
}

TEST_F(SessionPersistenceTest, EmptyColorConfigReferenceLoadsLikeAnAbsentOne) {
    const fs::path target = dir_ / "shot.nemo";
    nlohmann::json authored = saveDocument(makeDocument("empty-color"));
    authored["colorConfig"] = "";
    writeText(target, authored.dump());

    const ProjectReadResult read = ProjectFile::read(target);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_TRUE(read.colorConfigPath.empty());
    EXPECT_TRUE(read.references.empty());
}

TEST_F(SessionPersistenceTest, ColorConfigReferenceResolvesRebasesAndDirtiesTheSession) {
    const fs::path configs = dir_ / "configs";
    ASSERT_TRUE(fs::create_directories(configs));
    writeText(configs / "nemo.ocio", "ocio");
    const fs::path configPath = (configs / "nemo.ocio").lexically_normal();

    Document document = makeDocument("color");
    ProjectWriteRequest request;
    request.snapshot = std::make_shared<const Document>(document);
    request.target = dir_ / "shot.nemo";
    request.colorConfigPath = configPath.string();
    request.pathPolicy = PathPolicy::RebaseRelative;
    const ProjectWriteResult written = ProjectFile::writeAtomic(request);
    ASSERT_TRUE(written.ok) << written.error.message;

    const ProjectReadResult read = ProjectFile::read(request.target);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.colorConfigPath, configPath.string());
    bool found = false;
    for (const auto& reference : read.references) {
        if (reference.identity != "colorConfig")
            continue;
        found = true;
        EXPECT_EQ(reference.state, ReferenceState::Present);
        EXPECT_EQ(reference.resolvedPath, configPath.string());
    }
    EXPECT_TRUE(found);

    const fs::path absent = dir_ / "gone.ocio";
    Document missing = makeDocument("color-missing");
    ProjectWriteRequest missingRequest;
    missingRequest.snapshot = std::make_shared<const Document>(missing);
    missingRequest.target = dir_ / "missing.nemo";
    missingRequest.colorConfigPath = absent.string();
    missingRequest.pathPolicy = PathPolicy::RebaseRelative;
    ASSERT_TRUE(ProjectFile::writeAtomic(missingRequest).ok);
    const ProjectReadResult missingRead = ProjectFile::read(missingRequest.target);
    ASSERT_TRUE(missingRead.ok) << missingRead.error.message;
    bool warned = false;
    for (const auto& warning : missingRead.warnings)
        warned = warned || warning.find("missing dependency colorConfig") != std::string::npos;
    EXPECT_TRUE(warned);

    ProjectSession session(document);
    EXPECT_FALSE(session.isDirty());
    session.setColorConfigPath(configPath.string());
    EXPECT_TRUE(session.isDirty());
    const ProjectWriteRequest sessionRequest = session.prepareSave(dir_ / "session.nemo");
    const ProjectWriteResult sessionWritten = ProjectFile::writeAtomic(sessionRequest);
    ASSERT_TRUE(sessionWritten.ok) << sessionWritten.error.message;
    ASSERT_TRUE(session.commitSave(sessionRequest, sessionWritten).committed);
    EXPECT_FALSE(session.isDirty());
    const ProjectReadResult reopened = ProjectFile::read(dir_ / "session.nemo");
    ASSERT_TRUE(reopened.ok) << reopened.error.message;
    EXPECT_EQ(reopened.colorConfigPath, session.colorConfigPath());
    EXPECT_EQ(reopened.colorConfigPath, configPath.string());
}

TEST_F(SessionPersistenceTest, RelinkThroughSourceCommandSurvivesSaveAndReopen) {
    const fs::path media = dir_ / "media";
    ASSERT_TRUE(fs::create_directories(media));
    writeText(media / "new.exr", "x");
    const fs::path missingPath = (media / "old.exr").lexically_normal();
    const fs::path resolvedPath = (media / "new.exr").lexically_normal();

    Document document = makeDocument("relink");
    document.sources["plate"] = SourceReference{};
    document.sources["plate"].path = missingPath.string();
    const MediaSourceId entry = document.mediaCatalog.addEntry("plate", kInvalidMediaBin, MediaMetadata{});
    ASSERT_NE(entry, kInvalidMediaSource);

    ProjectSession session(document);
    const fs::path target = dir_ / "shot.nemo";
    const ProjectWriteRequest job = session.prepareSave(target);
    const ProjectWriteResult first = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(session.commitSave(job, first).committed);

    // A missing dependency keeps its source key and catalog identity across
    // save/reopen and is reported honestly.
    const ProjectReadResult reopened = ProjectFile::read(target);
    ASSERT_TRUE(reopened.ok) << reopened.error.message;
    ASSERT_EQ(reopened.document.sources.count("plate"), 1u);
    EXPECT_EQ(reopened.document.sources.at("plate").path, missingPath.string());
    ASSERT_EQ(reopened.references.size(), 1u);
    EXPECT_EQ(reopened.references.front().state, ReferenceState::Missing);
    const MediaCatalogEntry* kept = reopened.document.mediaCatalog.entry(entry);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->sourceKey, "plate");

    // Relink is a normal source edit on the same identity, not a new source.
    SourceReference relinked = session.document().sources.at("plate");
    relinked.path = resolvedPath.string();
    const EditResult edited = session.submit(setSourceCommand("plate", relinked), EditOptions{session.revision(), {}});
    ASSERT_TRUE(edited.committed) << (edited.error ? edited.error->message : std::string{});

    const ProjectWriteRequest relinkJob = session.prepareSave(target);
    const ProjectWriteResult relinkWrite = ProjectFile::writeAtomic(relinkJob);
    ASSERT_TRUE(relinkWrite.ok) << relinkWrite.error.message;
    ASSERT_TRUE(session.commitSave(relinkJob, relinkWrite).committed);
    EXPECT_FALSE(session.isDirty());

    const ProjectReadResult afterRelink = ProjectFile::read(target);
    ASSERT_TRUE(afterRelink.ok) << afterRelink.error.message;
    ASSERT_EQ(afterRelink.document.sources.count("plate"), 1u);
    EXPECT_EQ(afterRelink.document.sources.at("plate").path, resolvedPath.string());
    ASSERT_EQ(afterRelink.references.size(), 1u);
    EXPECT_EQ(afterRelink.references.front().state, ReferenceState::Present);
    EXPECT_NE(afterRelink.document.mediaCatalog.entry(entry), nullptr);

    // Undo/redo stay correct across the relink and the saved baseline.
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    EXPECT_EQ(session.document().sources.at("plate").path, missingPath.string());
    EXPECT_TRUE(session.isDirty());
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    EXPECT_EQ(session.document().sources.at("plate").path, resolvedPath.string());
    EXPECT_FALSE(session.isDirty());
}

}  // namespace
