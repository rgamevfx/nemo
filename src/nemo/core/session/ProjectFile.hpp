#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"

namespace nemo {

// A project file is plain versioned JSON carrying the authored document emitted
// by the core codec plus a file-owned envelope the codec never sees. The
// envelope is stripped before loadDocument and re-attached on write, so unknown
// authored JSON and required-feature metadata stay owned by the codec. The file
// layer is Qt-free and safe to call from a worker: it never touches a
// ProjectSession and never mutates the live Document.
inline constexpr std::string_view kPresentationKey = "presentation";
inline constexpr std::string_view kColorConfigKey = "colorConfig";
inline constexpr int kPresentationEnvelopeVersion = 1;

// Owner-approved presentation defaults. The autosave timer itself belongs to
// the presentation layer; these are the shared policy values it drives.
inline constexpr std::size_t kDefaultAutosaveSlots = 3;
inline constexpr std::chrono::seconds kDefaultAutosaveInterval{120};

// Presentation/session state is versioned independently from the document
// schema. Headless callers never interpret it; they preserve it across
// open/save. A newer envelope version is retained verbatim and reported as a
// warning instead of being rewritten from guesswork.
[[nodiscard]] nlohmann::json makePresentationEnvelope(nlohmann::json data);
[[nodiscard]] nlohmann::json presentationData(const nlohmann::json& envelope);

// How stored external reference paths are written.
//  - KeepStored keeps the in-memory model's paths unchanged.
//  - RebaseRelative rewrites absolute in-memory targets relative to the project
//    base when the target is reachable from it (Save As portability).
//  - RebaseAbsolute rewrites stored targets to absolute in-memory form.
enum class PathPolicy { KeepStored, RebaseRelative, RebaseAbsolute };

struct ProjectFileError {
    enum class Code {
        None,
        InvalidArgument,
        NotFound,
        NotAFile,
        ReadFailed,
        ParseFailed,
        Unsupported,
        WriteFailed,
        ReplaceFailed,
        BackupFailed,
        TargetIsDirectory,
        ProtectedTarget,
        UnsupportedTarget,
    };

    Code code{Code::None};
    std::string message;
    std::filesystem::path path;

    [[nodiscard]] bool failed() const noexcept { return code != Code::None; }
};

// Honest known state of one external resource the project references. Sequence
// patterns cannot be validated here: frame-range resolution is owned by the
// media module, so the file layer never claims a pattern is present or missing.
enum class ReferenceState {
    Present,     // the resolved target exists
    Missing,     // the resolved target is proven absent
    Unresolved,  // a sequence/pattern target still needs media-owned resolution
};

// One external resource the project references. Sources and the color
// configuration are resolved to absolute in-memory targets at read time; no
// other path is guessed. Missing or unresolved references are reported with
// identity+path so a relink or sequence resolve can be actionable.
struct ExternalReference {
    std::string identity;      // "source:<id>" or "colorConfig"
    std::string storedPath;    // path as authored in the file
    std::string resolvedPath;  // absolute in-memory target
    ReferenceState state{ReferenceState::Unresolved};
    bool relativeCapable{false};
};

struct ProjectReadResult {
    Document document;
    std::vector<std::string> warnings;
    nlohmann::json presentation;  // full envelope object; null when absent/headless
    std::string colorConfigPath;  // absolute in-memory target; empty when unauthored
    std::vector<ExternalReference> references;
    std::filesystem::path sourcePath;        // absolute file read; empty for recovery/new
    bool recovered{false};                   // readRecovery() copy of an autosave slot
    std::filesystem::path recoveryOriginal;  // original project target the slot came from
    bool ok{false};
    ProjectFileError error;
};

struct ProjectWriteRequest {
    // Owned immutable snapshot. Workers serialize this, never the live session
    // document.
    std::shared_ptr<const Document> snapshot;
    std::filesystem::path target;
    nlohmann::json presentation;
    std::string colorConfigPath;
    PathPolicy pathPolicy{PathPolicy::RebaseRelative};
    std::filesystem::path projectBase;  // empty -> target.parent_path()
    bool backup{true};
    // A recovery copy can never replace the original project file: writeAtomic
    // refuses while this target matches, so recovered work is kept only by
    // saving to a different path.
    std::filesystem::path protectedTarget;
    // Session bookkeeping captured by ProjectSession::prepareSave; the file
    // writer ignores all three fields.
    std::uint64_t expectedRevision{0};
    std::uint64_t projectGeneration{0};
    std::string baseline;
};

struct ProjectWriteResult {
    bool ok{false};
    std::filesystem::path target;
    std::filesystem::path backup;
    std::filesystem::path temporary;
    ProjectFileError error;
};

class ProjectFile final {
public:
    // Synchronous, worker-safe read. Resolves known external references against
    // the file's directory and returns warnings for missing dependencies.
    [[nodiscard]] static ProjectReadResult read(const std::filesystem::path& file,
                                                std::shared_ptr<const NodeCatalog> catalog = builtinNodeCatalogPtr());
    // Reads a bounded autosave slot as an unsaved copy: sourcePath is empty, the
    // original target is recorded only as a recovery guard and is never opened
    // for writing by this path.
    [[nodiscard]] static ProjectReadResult
    readRecovery(const std::filesystem::path& slot,
                 std::shared_ptr<const NodeCatalog> catalog = builtinNodeCatalogPtr());

    // Synchronous, worker-safe atomic write: temp sibling + flush/fsync +
    // platform replacement, previous-good backup, never destroying the last
    // valid target on failure.
    [[nodiscard]] static ProjectWriteResult writeAtomic(const ProjectWriteRequest& request);

    // Known external references: authored media sources plus the color
    // configuration. Unknown opaque presentation paths are never resolved.
    [[nodiscard]] static std::vector<ExternalReference> referenceState(const Document& document,
                                                                       std::string_view colorConfigPath,
                                                                       const std::filesystem::path& projectBase);
    [[nodiscard]] static std::vector<std::string>
    missingDependencyWarnings(const std::vector<ExternalReference>& references);
    // Sequence/pattern references whose frame range still needs media-owned
    // resolution; the file layer cannot prove them present or missing.
    [[nodiscard]] static std::vector<std::string>
    unresolvedDependencyWarnings(const std::vector<ExternalReference>& references);

    [[nodiscard]] static std::string resolveReferencePath(const std::filesystem::path& projectBase,
                                                          std::string_view stored);
    [[nodiscard]] static std::string rebaseReferencePath(const std::filesystem::path& projectBase,
                                                         std::string_view inMemoryPath, PathPolicy policy);

    // Canonical in-memory content used as the dirty baseline. Two sessions with
    // equal authored content serialize to equal text.
    [[nodiscard]] static std::string serializeContent(const Document& document, const nlohmann::json& presentation,
                                                      std::string_view colorConfigPath);

    // Derives the original project target from an autosave slot or
    // previous-good backup name; empty when the name does not follow the owned
    // naming scheme.
    [[nodiscard]] static std::filesystem::path originalTargetForSlot(const std::filesystem::path& slot);
};

// Bounded autosave storage. The presentation layer drives the timer; this class
// only enforces the storage policy and never writes the project target.
class AutosaveStore final {
public:
    explicit AutosaveStore(std::filesystem::path projectTarget, std::size_t slots = kDefaultAutosaveSlots);

    [[nodiscard]] const std::filesystem::path& projectTarget() const noexcept { return target_; }
    [[nodiscard]] std::size_t slotLimit() const noexcept { return slots_; }
    [[nodiscard]] std::filesystem::path slotPath(std::size_t index) const;
    [[nodiscard]] std::vector<std::filesystem::path> existingSlots() const;  // newest first
    [[nodiscard]] std::optional<std::filesystem::path> latest() const;

    // Redirects the request to the next safe slot (no backup, no project
    // target write, no protection bypass). Genuinely corrupt/empty owned slots
    // are replaced; slots holding newer-schema or unknown-required-feature
    // projects are never destroyed, and write() fails visibly with
    // ProjectFileError::Code::Unsupported when no safe slot remains.
    [[nodiscard]] ProjectWriteResult write(const ProjectWriteRequest& request);
    // Removes recyclable slots beyond the bound; protected newer/unknown
    // recovery data is never pruned.
    [[nodiscard]] std::size_t prune() const;

private:
    // A safe replacement slot: a free index inside the bound, the oldest
    // recyclable slot, or nullopt when every bound slot holds protected
    // (newer schema / unknown required feature) recovery data.
    [[nodiscard]] std::optional<std::size_t> nextSlotIndex() const;

    std::filesystem::path target_;
    std::size_t slots_;
};

}  // namespace nemo
