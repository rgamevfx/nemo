#include "nemo/core/session/ProjectFile.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nemo {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kAutosaveInfix = ".autosave";
constexpr std::string_view kTemporarySuffix = ".nemo-tmp";
constexpr std::string_view kBackupSuffix = ".bak";
constexpr std::string_view kFormatKey = "format";
constexpr std::string_view kFormatValue = "nemo";

// Access-mode bits preserved across atomic replacement.
constexpr fs::perms kModeMask = fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all;

ProjectFileError makeError(ProjectFileError::Code code, std::string message, fs::path path = {}) {
    ProjectFileError error;
    error.code = code;
    error.message = std::move(message);
    error.path = std::move(path);
    return error;
}

std::string systemMessage(int code) {
    return std::error_code(code, std::generic_category()).message();
}

fs::path absoluteNormalized(const fs::path& path) {
    if (path.empty())
        return {};
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec)
        absolute = path;
    return absolute.lexically_normal();
}

bool isPatterned(std::string_view path) {
    return path.find_first_of("#%*?[") != std::string_view::npos;
}

bool referenceExists(const std::string& resolved) {
    if (resolved.empty())
        return false;
    const fs::path path(resolved);
    std::error_code ec;
    if (fs::exists(path, ec) && !ec)
        return true;
    // A sequence source may be addressed by its directory rather than one file.
    return !path.has_extension() && fs::is_directory(path, ec);
}

bool relativeCapable(const fs::path& base, const fs::path& resolved) {
    if (base.empty() || !resolved.is_absolute())
        return false;
    const fs::path relative = resolved.lexically_relative(base);
    return !relative.empty() && relative != fs::path(".");
}

bool samePath(const fs::path& left, const fs::path& right) {
    if (left.empty() || right.empty())
        return false;
    std::error_code ec;
    if (fs::exists(left, ec) && !ec) {
        // equivalent() also detects two names for the same file (hard links).
        std::error_code equivalentError;
        if (fs::equivalent(left, right, equivalentError) && !equivalentError)
            return true;
    }
    const fs::path canonicalLeft = fs::weakly_canonical(left, ec);
    if (ec)
        return absoluteNormalized(left) == absoluteNormalized(right);
    const fs::path canonicalRight = fs::weakly_canonical(right, ec);
    if (ec)
        return absoluteNormalized(left) == absoluteNormalized(right);
    return canonicalLeft == canonicalRight;
}

bool readFileText(const fs::path& file, std::string& text, ProjectFileError& error) {
    std::error_code ec;
    if (!fs::exists(file, ec) || ec) {
        error = makeError(ProjectFileError::Code::NotFound, "project file does not exist", file);
        return false;
    }
    if (!fs::is_regular_file(file, ec) || ec) {
        error = makeError(ProjectFileError::Code::NotAFile, "project path is not a regular file", file);
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = makeError(ProjectFileError::Code::ReadFailed, "cannot open project file for reading", file);
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        error = makeError(ProjectFileError::Code::ReadFailed, "cannot read project file", file);
        return false;
    }
    text = std::move(buffer).str();
    return true;
}

// Exclusive creation distinguishes a name collision (retry with another unique
// name) from a real write failure. `created` reports whether this call
// exclusively created the file, so callers never infer ownership from a path.
enum class FileWriteStatus { Ok, Exists, Failed };

struct FileWriteResult {
    FileWriteStatus status{FileWriteStatus::Failed};
    bool created{false};
};

// Removes the file this call exclusively created when it cannot finish writing
// it. Armed only after exclusive creation, so a path created by someone else
// (or left by a failed open) is never removed.
class TemporaryFileGuard final {
public:
    TemporaryFileGuard() = default;
    TemporaryFileGuard(const TemporaryFileGuard&) = delete;
    TemporaryFileGuard& operator=(const TemporaryFileGuard&) = delete;
    ~TemporaryFileGuard() {
        if (!armed_)
            return;
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    void arm(fs::path path) {
        path_ = std::move(path);
        armed_ = true;
    }
    void disarm() noexcept { armed_ = false; }

private:
    fs::path path_;
    bool armed_{false};
};

#ifdef _WIN32
using FileHandle = int;
constexpr FileHandle kInvalidHandle = -1;
#else
using FileHandle = int;
constexpr FileHandle kInvalidHandle = -1;
#endif

FileWriteResult writeFileSynced(const fs::path& file, const std::string& data, ProjectFileError& error) {
#ifdef _WIN32
    const FileHandle handle = ::_wopen(file.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    // Owner-only from the outset: a private project's bytes are never readable
    // by others while the temporary is being written.
    const FileHandle handle = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    if (handle == kInvalidHandle) {
        if (errno == EEXIST)
            return FileWriteResult{FileWriteStatus::Exists, false};
        error = makeError(ProjectFileError::Code::WriteFailed,
                          "cannot create temporary project file: " + systemMessage(errno), file);
        return FileWriteResult{FileWriteStatus::Failed, false};
    }
    // From here the file is exclusively ours: a failure removes only it, after
    // its descriptor is closed.
    TemporaryFileGuard created;
    created.arm(file);
    std::size_t written = 0;
    while (written < data.size()) {
#ifdef _WIN32
        const int chunk = ::_write(handle, data.data() + written,
                                   static_cast<unsigned>(std::min<std::size_t>(data.size() - written, 1u << 30)));
#else
        const ssize_t chunk = ::write(handle, data.data() + written, data.size() - written);
#endif
        if (chunk <= 0) {
            if (chunk < 0 && errno == EINTR)
                continue;
            const std::string message = "cannot write temporary project file: " + systemMessage(errno);
#ifdef _WIN32
            ::_close(handle);
#else
            ::close(handle);
#endif
            error = makeError(ProjectFileError::Code::WriteFailed, message, file);
            return FileWriteResult{FileWriteStatus::Failed, true};
        }
        written += static_cast<std::size_t>(chunk);
    }
#ifdef _WIN32
    if (::_commit(handle) != 0) {
        const std::string message = "cannot flush temporary project file: " + systemMessage(errno);
        ::_close(handle);
        error = makeError(ProjectFileError::Code::WriteFailed, message, file);
        return FileWriteResult{FileWriteStatus::Failed, true};
    }
    if (::_close(handle) != 0) {
        error = makeError(ProjectFileError::Code::WriteFailed,
                          "cannot close temporary project file: " + systemMessage(errno), file);
        return FileWriteResult{FileWriteStatus::Failed, true};
    }
#else
    if (::fsync(handle) != 0) {
        const std::string message = "cannot flush temporary project file: " + systemMessage(errno);
        ::close(handle);
        error = makeError(ProjectFileError::Code::WriteFailed, message, file);
        return FileWriteResult{FileWriteStatus::Failed, true};
    }
    if (::close(handle) != 0) {
        error = makeError(ProjectFileError::Code::WriteFailed,
                          "cannot close temporary project file: " + systemMessage(errno), file);
        return FileWriteResult{FileWriteStatus::Failed, true};
    }
#endif
    created.disarm();
    return FileWriteResult{FileWriteStatus::Ok, true};
}

bool replaceFile(const fs::path& from, const fs::path& to, ProjectFileError& error) {
#ifdef _WIN32
    if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = makeError(ProjectFileError::Code::ReplaceFailed,
                          "cannot replace project file: windows error " + std::to_string(::GetLastError()), to);
        return false;
    }
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        error = makeError(ProjectFileError::Code::ReplaceFailed, "cannot replace project file: " + systemMessage(errno),
                          to);
        return false;
    }
#endif
    return true;
}

bool syncDirectory(const fs::path& directory, ProjectFileError& error) {
#ifdef _WIN32
    // MoveFileExW already requests write-through for the replacement itself.
    static_cast<void>(directory);
    return true;
#else
    if (directory.empty())
        return true;
    const int handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (handle < 0) {
        error = makeError(ProjectFileError::Code::WriteFailed,
                          "project file replaced but its directory cannot be opened for durability sync: " +
                              systemMessage(errno),
                          directory);
        return false;
    }
    if (::fsync(handle) != 0) {
        const int code = errno;
        static_cast<void>(::close(handle));
        // A filesystem without directory fsync support cannot report
        // durability; every other failure is real.
        if (code == EINVAL || code == ENOTSUP)
            return true;
        error = makeError(ProjectFileError::Code::WriteFailed,
                          "project file replaced but its directory durability sync failed: " + systemMessage(code),
                          directory);
        return false;
    }
    static_cast<void>(::close(handle));
    return true;
#endif
}

fs::path sibling(const fs::path& target, std::string_view suffix) {
    return target.parent_path() / (target.filename().string() + std::string(suffix));
}

std::uint64_t processId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// Exclusive, unpredictable sibling name: two writers never collide and a
// predictable name can never truncate a user file or follow a symlink.
fs::path uniqueTemporaryPath(const fs::path& target) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t sequence = ++counter;
    const auto stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string suffix = std::string(kTemporarySuffix) + "." + std::to_string(processId()) + "." +
                               std::to_string(stamp) + "." + std::to_string(sequence);
    return sibling(target, suffix);
}

fs::path projectBaseFor(const ProjectWriteRequest& request) {
    if (!request.projectBase.empty())
        return absoluteNormalized(request.projectBase);
    return absoluteNormalized(request.target).parent_path();
}

nlohmann::json buildProjectJson(const ProjectWriteRequest& request) {
    const fs::path base = projectBaseFor(request);
    nlohmann::json document = saveDocument(*request.snapshot);
    if (request.pathPolicy != PathPolicy::KeepStored) {
        // Rebase only known reference fields in the serialized payload; avoid
        // cloning the entire immutable document or touching opaque extensions.
        for (const auto& [id, source] : request.snapshot->sources)
            document.at("sources").at(id).at("path") =
                ProjectFile::rebaseReferencePath(base, source.path, request.pathPolicy);
    }
    if (!request.colorConfigPath.empty())
        document[std::string(kColorConfigKey)] =
            ProjectFile::rebaseReferencePath(base, request.colorConfigPath, request.pathPolicy);
    if (!request.presentation.is_null())
        document[std::string(kPresentationKey)] = request.presentation;
    return document;
}

ProjectFileError::Code classifyCodecError(std::string_view message) {
    std::string lower(message);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    for (const std::string_view needle : {"unsupported", "newer", "requires"})
        if (lower.find(needle) != std::string::npos)
            return ProjectFileError::Code::Unsupported;
    return ProjectFileError::Code::ParseFailed;
}

bool parseSlotIndex(const std::string& digits, std::size_t& index) {
    if (digits.empty())
        return false;
    std::size_t value = 0;
    for (const char digit : digits) {
        if (std::isdigit(static_cast<unsigned char>(digit)) == 0)
            return false;
        if (value > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(digit - '0')) / 10)
            return false;
        value = value * 10 + static_cast<std::size_t>(digit - '0');
    }
    index = value;
    return true;
}

struct AutosaveSlot {
    std::size_t index{};
    fs::path path;
    fs::file_time_type time{};
};

std::vector<AutosaveSlot> collectAutosaveSlots(const fs::path& target) {
    std::vector<AutosaveSlot> slots;
    const fs::path directory = target.parent_path();
    const std::string prefix = target.filename().string() + std::string(kAutosaveInfix);
    std::error_code ec;
    if (!fs::is_directory(directory, ec) || ec)
        return slots;
    fs::directory_iterator iterator(directory, ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        std::error_code statusError;
        if (iterator->is_regular_file(statusError) && !statusError) {
            const std::string name = iterator->path().filename().string();
            std::size_t index = 0;
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
                parseSlotIndex(name.substr(prefix.size()), index)) {
                const auto time = fs::last_write_time(iterator->path(), statusError);
                slots.push_back(AutosaveSlot{index, iterator->path(), statusError ? fs::file_time_type{} : time});
            }
        }
        iterator.increment(ec);
    }
    return slots;
}

void sortNewestFirst(std::vector<AutosaveSlot>& slots) {
    std::sort(slots.begin(), slots.end(), [](const AutosaveSlot& left, const AutosaveSlot& right) {
        if (left.time != right.time)
            return left.time > right.time;
        return left.index > right.index;
    });
}

// Owned autosave slot classification. Only genuinely corrupt or empty slots
// (ParseFailed) may be retired; anything this build cannot read for another
// reason (newer schema, unknown required feature, unreadable) is protected
// recovery data that must never be destroyed.
enum class SlotState { Readable, Corrupt, Protected };

SlotState slotState(const fs::path& path) {
    const ProjectReadResult result = ProjectFile::read(path);
    if (result.ok)
        return SlotState::Readable;
    if (result.error.code == ProjectFileError::Code::ParseFailed)
        return SlotState::Corrupt;
    return SlotState::Protected;
}

}  // namespace

nlohmann::json makePresentationEnvelope(nlohmann::json data) {
    nlohmann::json envelope = nlohmann::json::object();
    envelope["version"] = kPresentationEnvelopeVersion;
    envelope["data"] = std::move(data);
    return envelope;
}

nlohmann::json presentationData(const nlohmann::json& envelope) {
    if (!envelope.is_object())
        return nlohmann::json();
    const auto found = envelope.find("data");
    if (found == envelope.end())
        return nlohmann::json();
    return *found;
}

ProjectReadResult ProjectFile::read(const fs::path& file, std::shared_ptr<const NodeCatalog> catalog) {
    ProjectReadResult result;
    result.sourcePath = absoluteNormalized(file);
    std::string text;
    if (!readFileText(file, text, result.error))
        return result;

    nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        result.error = makeError(ProjectFileError::Code::ParseFailed, "project file is not valid JSON", file);
        return result;
    }
    if (!json.is_object()) {
        result.error = makeError(ProjectFileError::Code::ParseFailed, "project root must be a JSON object", file);
        return result;
    }
    if (const auto format = json.find(std::string(kFormatKey)); format != json.end()) {
        const bool nemo = format->is_string() && format->get<std::string>() == kFormatValue;
        if (!nemo) {
            const std::string value = format->is_string() ? format->get<std::string>() : format->dump();
            result.error =
                makeError(ProjectFileError::Code::Unsupported, "not a Nemo project file: format '" + value + "'", file);
            return result;
        }
    }

    if (const auto envelope = json.find(std::string(kPresentationKey)); envelope != json.end()) {
        result.presentation = *envelope;
        if (result.presentation.is_object()) {
            if (const auto version = result.presentation.find("version");
                version != result.presentation.end() && version->is_number_integer() &&
                version->get<int>() > kPresentationEnvelopeVersion) {
                result.warnings.push_back("presentation envelope version " + std::to_string(version->get<int>()) +
                                          " is newer than this build supports (" +
                                          std::to_string(kPresentationEnvelopeVersion) + "); preserved verbatim");
            }
        }
        json.erase(envelope);
    }
    std::string colorConfig;
    if (const auto colorConfigEntry = json.find(std::string(kColorConfigKey)); colorConfigEntry != json.end()) {
        if (!colorConfigEntry->is_string()) {
            // A known malformed field must fail the read instead of opening with
            // an environment fallback and silently discarding authored data on
            // the next save.
            result.error = makeError(ProjectFileError::Code::ParseFailed,
                                     "project field '" + std::string(kColorConfigKey) +
                                         "' must be a string path; refusing to load and lose it",
                                     file);
            return result;
        }
        colorConfig = colorConfigEntry->get<std::string>();
        json.erase(colorConfigEntry);
    }

    if (!catalog)
        catalog = builtinNodeCatalogPtr();
    LoadResult loaded;
    try {
        loaded = loadDocument(json, std::move(catalog));
    } catch (const DeserializeError& error) {
        result.error = makeError(classifyCodecError(error.what()), error.what(), file);
        return result;
    } catch (const std::exception& error) {
        result.error = makeError(ProjectFileError::Code::ParseFailed, error.what(), file);
        return result;
    }

    result.document = std::move(loaded.document);
    result.warnings.insert(result.warnings.end(), loaded.warnings.begin(), loaded.warnings.end());

    const fs::path base = result.sourcePath.parent_path();
    result.references = referenceState(result.document, colorConfig, base);
    for (const auto& reference : result.references) {
        if (reference.identity.rfind("source:", 0) == 0) {
            const std::string id = reference.identity.substr(std::string_view("source:").size());
            if (const auto source = result.document.sources.find(id); source != result.document.sources.end())
                source->second.path = reference.resolvedPath;
        } else if (reference.identity == "colorConfig") {
            result.colorConfigPath = reference.resolvedPath;
        }
    }
    const auto dependencyWarnings = missingDependencyWarnings(result.references);
    result.warnings.insert(result.warnings.end(), dependencyWarnings.begin(), dependencyWarnings.end());
    const auto unresolvedWarnings = unresolvedDependencyWarnings(result.references);
    result.warnings.insert(result.warnings.end(), unresolvedWarnings.begin(), unresolvedWarnings.end());
    result.ok = true;
    return result;
}

ProjectReadResult ProjectFile::readRecovery(const fs::path& slot, std::shared_ptr<const NodeCatalog> catalog) {
    ProjectReadResult result = read(slot, std::move(catalog));
    if (!result.ok)
        return result;
    result.recovered = true;
    result.recoveryOriginal = originalTargetForSlot(slot);
    result.sourcePath.clear();
    std::string warning = "recovered unsaved copy from " + slot.string();
    if (!result.recoveryOriginal.empty())
        warning += "; save as a new file to keep " + result.recoveryOriginal.string() + " untouched";
    result.warnings.insert(result.warnings.begin(), std::move(warning));
    return result;
}

ProjectWriteResult ProjectFile::writeAtomic(const ProjectWriteRequest& request) {
    ProjectWriteResult result;
    result.target = request.target;
    if (!request.snapshot) {
        result.error =
            makeError(ProjectFileError::Code::InvalidArgument, "project snapshot must not be null", request.target);
        return result;
    }
    if (request.target.empty()) {
        result.error = makeError(ProjectFileError::Code::InvalidArgument, "project target must not be empty");
        return result;
    }
    if (!request.protectedTarget.empty() && samePath(request.target, request.protectedTarget)) {
        result.error = makeError(ProjectFileError::Code::ProtectedTarget,
                                 "refusing to replace " + request.protectedTarget.string() +
                                     " from a recovered copy; save the recovered work to a different file",
                                 request.target);
        return result;
    }

    std::error_code ec;
    if (fs::is_directory(request.target, ec)) {
        result.error =
            makeError(ProjectFileError::Code::TargetIsDirectory, "project target is a directory", request.target);
        return result;
    }
    const fs::path parent = request.target.parent_path();
    if (!parent.empty()) {
        if (!fs::exists(parent, ec)) {
            fs::create_directories(parent, ec);
            if (ec) {
                result.error = makeError(ProjectFileError::Code::WriteFailed,
                                         "cannot create project directory: " + ec.message(), parent);
                return result;
            }
        } else if (!fs::is_directory(parent, ec)) {
            result.error =
                makeError(ProjectFileError::Code::WriteFailed, "project parent path is not a directory", parent);
            return result;
        }
    }

    // A destructive save is rejected outright: an existing target this build
    // cannot read (empty/truncated, corrupt, newer schema, unknown required
    // feature) is never replaced and its previous-good backup is never touched.
    // An externally truncated project must not cost the last good recovery.
    bool previousGood = false;
    if (fs::is_regular_file(request.target, ec) && !ec) {
        const ProjectReadResult existing = read(request.target);
        if (!existing.ok) {
            result.error = makeError(ProjectFileError::Code::UnsupportedTarget,
                                     "refusing to replace existing project " + request.target.string() + ": " +
                                         existing.error.message,
                                     request.target);
            return result;
        }
        previousGood = true;
    }

    std::string text;
    try {
        text = buildProjectJson(request).dump(2);
    } catch (const std::exception& error) {
        result.error = makeError(ProjectFileError::Code::WriteFailed,
                                 std::string("cannot serialize project: ") + error.what(), request.target);
        return result;
    } catch (...) {
        result.error = makeError(ProjectFileError::Code::WriteFailed, "cannot serialize project", request.target);
        return result;
    }

    // Exclusive, unique sibling temporary. A predictable name is never reused,
    // so a user file or a concurrent writer is never truncated or replaced.
    ProjectFileError error;
    fs::path temporary;
    bool temporaryReady = false;
    for (int attempt = 0; attempt < 16 && !temporaryReady; ++attempt) {
        temporary = uniqueTemporaryPath(request.target);
        const FileWriteResult write = writeFileSynced(temporary, text, error);
        if (write.status == FileWriteStatus::Failed) {
            // writeFileSynced already removed the partial file it owned; a path
            // this call did not create is never inferred from existence.
            result.error = std::move(error);
            return result;
        }
        temporaryReady = write.status == FileWriteStatus::Ok;
    }
    if (!temporaryReady) {
        result.error = makeError(ProjectFileError::Code::WriteFailed, "cannot create a unique temporary project file",
                                 request.target);
        return result;
    }
    result.temporary = temporary;

    // Every path below removes the created temporary on failure and never a
    // pre-existing path.
    TemporaryFileGuard temporaryCleanup;
    temporaryCleanup.arm(temporary);

    // Preserve the existing project's permissions on the replacement inode so a
    // private project is not widened to the default creation mode. The
    // temporary starts owner-only; the existing mode is applied once the
    // content is complete. A brand-new project keeps the private default.
    fs::perms targetPermissions = fs::perms::unknown;
    if (previousGood) {
        std::error_code statusError;
        const fs::perms raw = fs::status(request.target, statusError).permissions();
        if (statusError || raw == fs::perms::unknown) {
            result.error = makeError(ProjectFileError::Code::WriteFailed,
                                     "cannot read the existing project permissions to preserve them", request.target);
            return result;
        }
        const fs::perms permissions = raw & kModeMask;
        std::error_code permissionError;
        fs::permissions(temporary, permissions, fs::perm_options::replace, permissionError);
        if (permissionError) {
            result.error =
                makeError(ProjectFileError::Code::WriteFailed,
                          "cannot preserve the existing project permissions: " + permissionError.message(), temporary);
            return result;
        }
        targetPermissions = permissions;
    }

    if (request.backup && previousGood) {
        const fs::path backup = sibling(request.target, kBackupSuffix);
        // Publish the previous good bytes through an exclusive unique sibling
        // and an atomic rename: a `.bak` symlink is replaced, never followed,
        // and an existing good backup is never truncated on failure.
        std::string previousBytes;
        ProjectFileError readError;
        if (!readFileText(request.target, previousBytes, readError)) {
            result.error =
                makeError(ProjectFileError::Code::BackupFailed,
                          "cannot read the previous project for backup: " + readError.message, request.target);
            return result;
        }
        fs::path backupTemporary;
        bool backupReady = false;
        for (int attempt = 0; attempt < 16 && !backupReady; ++attempt) {
            backupTemporary = uniqueTemporaryPath(backup);
            const FileWriteResult write = writeFileSynced(backupTemporary, previousBytes, error);
            if (write.status == FileWriteStatus::Failed) {
                result.error = makeError(ProjectFileError::Code::BackupFailed,
                                         "cannot write previous-good backup: " + error.message, backupTemporary);
                return result;
            }
            backupReady = write.status == FileWriteStatus::Ok;
        }
        if (!backupReady) {
            result.error = makeError(ProjectFileError::Code::BackupFailed,
                                     "cannot create a unique previous-good backup file", backup);
            return result;
        }
        TemporaryFileGuard backupCleanup;
        backupCleanup.arm(backupTemporary);
        if (targetPermissions != fs::perms::unknown) {
            // The backup must not be more permissive than the project it guards.
            std::error_code permissionError;
            fs::permissions(backupTemporary, targetPermissions, fs::perm_options::replace, permissionError);
            if (permissionError) {
                result.error =
                    makeError(ProjectFileError::Code::BackupFailed,
                              "cannot preserve the previous-good backup permissions: " + permissionError.message(),
                              backupTemporary);
                return result;
            }
        }
        if (!replaceFile(backupTemporary, backup, error)) {
            result.error = makeError(ProjectFileError::Code::BackupFailed,
                                     "cannot publish previous-good backup: " + error.message, backup);
            return result;
        }
        backupCleanup.disarm();
        // The backup must be durable before the project itself is replaced.
        if (!syncDirectory(backup.parent_path(), error)) {
            result.error = std::move(error);
            return result;
        }
        result.backup = backup;
    }

    if (!replaceFile(temporary, request.target, error)) {
        result.error = std::move(error);
        return result;
    }
    temporaryCleanup.disarm();
    if (!syncDirectory(request.target.parent_path(), error)) {
        // The replacement is already visible and the previous-good backup is
        // retained; report the durability failure rather than guessing a
        // rollback.
        result.error = std::move(error);
        return result;
    }
    result.ok = true;
    return result;
}

std::vector<ExternalReference> ProjectFile::referenceState(const Document& document, std::string_view colorConfigPath,
                                                           const fs::path& projectBase) {
    const fs::path base = absoluteNormalized(projectBase);
    std::vector<ExternalReference> references;
    references.reserve(document.sources.size() + 1);
    for (const auto& [id, source] : document.sources) {
        if (source.path.empty())
            continue;
        ExternalReference reference;
        reference.identity = "source:" + id;
        reference.storedPath = source.path;
        reference.resolvedPath = resolveReferencePath(base, source.path);
        reference.state =
            isPatterned(reference.resolvedPath)
                ? ReferenceState::Unresolved
                : (referenceExists(reference.resolvedPath) ? ReferenceState::Present : ReferenceState::Missing);
        reference.relativeCapable = relativeCapable(base, reference.resolvedPath);
        references.push_back(std::move(reference));
    }
    if (!colorConfigPath.empty()) {
        ExternalReference reference;
        reference.identity = "colorConfig";
        reference.storedPath = std::string(colorConfigPath);
        reference.resolvedPath = resolveReferencePath(base, colorConfigPath);
        reference.state =
            isPatterned(reference.resolvedPath)
                ? ReferenceState::Unresolved
                : (referenceExists(reference.resolvedPath) ? ReferenceState::Present : ReferenceState::Missing);
        reference.relativeCapable = relativeCapable(base, reference.resolvedPath);
        references.push_back(std::move(reference));
    }
    return references;
}

std::vector<std::string> ProjectFile::missingDependencyWarnings(const std::vector<ExternalReference>& references) {
    std::vector<std::string> warnings;
    for (const auto& reference : references) {
        if (reference.state != ReferenceState::Missing)
            continue;
        warnings.push_back("missing dependency " + reference.identity + ": " + reference.resolvedPath +
                           " (authored as '" + reference.storedPath + "')");
    }
    return warnings;
}

std::vector<std::string> ProjectFile::unresolvedDependencyWarnings(const std::vector<ExternalReference>& references) {
    std::vector<std::string> warnings;
    for (const auto& reference : references) {
        if (reference.state != ReferenceState::Unresolved)
            continue;
        warnings.push_back("unresolved sequence dependency " + reference.identity + ": " + reference.resolvedPath +
                           " (authored as '" + reference.storedPath +
                           "'); frame range needs media-owned sequence resolution");
    }
    return warnings;
}

std::string ProjectFile::resolveReferencePath(const fs::path& projectBase, std::string_view stored) {
    if (stored.empty())
        return {};
    const fs::path path{std::string(stored)};
    if (path.is_absolute())
        return path.lexically_normal().string();
    if (projectBase.empty())
        return path.lexically_normal().string();
    return (absoluteNormalized(projectBase) / path).lexically_normal().string();
}

std::string ProjectFile::rebaseReferencePath(const fs::path& projectBase, std::string_view inMemoryPath,
                                             PathPolicy policy) {
    if (inMemoryPath.empty())
        return {};
    const std::string stored(inMemoryPath);
    if (policy == PathPolicy::KeepStored)
        return stored;
    const fs::path base = absoluteNormalized(projectBase);
    const fs::path absolute = resolveReferencePath(base, stored);
    if (policy == PathPolicy::RebaseAbsolute || base.empty() || !absolute.is_absolute())
        return absolute.lexically_normal().string();
    const fs::path relative = absolute.lexically_relative(base);
    if (relative.empty() || relative == fs::path("."))
        return absolute.lexically_normal().string();
    return relative.string();
}

std::string ProjectFile::serializeContent(const Document& document, const nlohmann::json& presentation,
                                          std::string_view colorConfigPath) {
    std::string text = saveDocument(document).dump();
    text.push_back('\n');
    if (!presentation.is_null())
        text += presentation.dump();
    text.push_back('\n');
    text.append(colorConfigPath);
    return text;
}

fs::path ProjectFile::originalTargetForSlot(const fs::path& slot) {
    const std::string name = slot.filename().string();
    const std::size_t position = name.rfind(std::string(kAutosaveInfix));
    if (position != std::string::npos && position != 0) {
        std::size_t index = 0;
        if (parseSlotIndex(name.substr(position + kAutosaveInfix.size()), index))
            return slot.parent_path() / name.substr(0, position);
    }
    // A previous-good backup is an equally valid native Recover source and
    // protects the same original target.
    if (name.size() > kBackupSuffix.size() && name.ends_with(kBackupSuffix))
        return slot.parent_path() / name.substr(0, name.size() - kBackupSuffix.size());
    return {};
}

AutosaveStore::AutosaveStore(fs::path projectTarget, std::size_t slots)
    : target_(std::move(projectTarget)), slots_(std::max<std::size_t>(1, slots)) {}

fs::path AutosaveStore::slotPath(std::size_t index) const {
    return sibling(target_, std::string(kAutosaveInfix) + std::to_string(index));
}

std::vector<fs::path> AutosaveStore::existingSlots() const {
    std::vector<AutosaveSlot> slots = collectAutosaveSlots(target_);
    sortNewestFirst(slots);
    std::vector<fs::path> paths;
    paths.reserve(slots.size());
    for (const auto& slot : slots)
        paths.push_back(slot.path);
    return paths;
}

std::optional<fs::path> AutosaveStore::latest() const {
    const std::vector<fs::path> slots = existingSlots();
    if (slots.empty())
        return std::nullopt;
    return slots.front();
}

std::optional<std::size_t> AutosaveStore::nextSlotIndex() const {
    const std::vector<AutosaveSlot> slots = collectAutosaveSlots(target_);
    std::vector<SlotState> states;
    states.reserve(slots.size());
    std::size_t corrupt = std::numeric_limits<std::size_t>::max();
    for (const auto& slot : slots) {
        const SlotState state = slotState(slot.path);
        states.push_back(state);
        if (state == SlotState::Corrupt)
            corrupt = std::min(corrupt, slot.index);
    }
    // A genuinely corrupt or empty owned slot is the preferred replacement: it
    // must be rotated out rather than a good slot, and must not linger.
    if (corrupt != std::numeric_limits<std::size_t>::max())
        return corrupt;

    // A free index inside the bound is always safe.
    for (std::size_t index = 0; index < slots_; ++index) {
        const bool used =
            std::any_of(slots.begin(), slots.end(), [index](const AutosaveSlot& slot) { return slot.index == index; });
        if (!used)
            return index;
    }
    // Otherwise recycle the oldest recyclable slot inside the bound. Protected
    // slots are never retired; if nothing is recyclable, autosave must fail
    // visibly instead of destroying newer recovery data.
    const AutosaveSlot* oldest = nullptr;
    for (std::size_t position = 0; position < slots.size(); ++position) {
        if (states[position] == SlotState::Protected || slots[position].index >= slots_)
            continue;
        if (oldest == nullptr || slots[position].time < oldest->time ||
            (slots[position].time == oldest->time && slots[position].index < oldest->index))
            oldest = &slots[position];
    }
    if (oldest == nullptr)
        return std::nullopt;
    return oldest->index;
}

ProjectWriteResult AutosaveStore::write(const ProjectWriteRequest& request) {
    ProjectWriteRequest slotRequest = request;
    const std::optional<std::size_t> index = nextSlotIndex();
    if (!index) {
        static_cast<void>(prune());
        ProjectWriteResult blocked;
        blocked.target = target_;
        blocked.error = makeError(ProjectFileError::Code::Unsupported,
                                  "autosave blocked: every slot holds project data this build cannot read "
                                  "(newer schema or unknown required feature); refusing to destroy recovery data",
                                  target_);
        return blocked;
    }
    const fs::path slot = slotPath(*index);
    slotRequest.target = slot;
    slotRequest.backup = false;
    slotRequest.protectedTarget.clear();
    ProjectWriteResult result = ProjectFile::writeAtomic(slotRequest);
    if (!result.ok && result.error.code == ProjectFileError::Code::UnsupportedTarget &&
        slotState(slot) == SlotState::Corrupt) {
        // Retire only a genuinely corrupt or empty owned slot, retrying that
        // exact slot so no other recovery data is rotated out.
        std::error_code removeError;
        fs::remove(slot, removeError);
        result = ProjectFile::writeAtomic(slotRequest);
    }
    static_cast<void>(prune());
    return result;
}

std::size_t AutosaveStore::prune() const {
    std::vector<AutosaveSlot> slots = collectAutosaveSlots(target_);
    sortNewestFirst(slots);
    std::size_t removed = 0;
    std::size_t kept = 0;
    for (const auto& slot : slots) {
        // Newer-schema / unknown-required-feature slots are protected recovery
        // data: they are never deleted, even beyond the bound.
        if (slotState(slot.path) == SlotState::Protected)
            continue;
        if (kept < slots_) {
            ++kept;
            continue;
        }
        std::error_code ec;
        if (fs::remove(slot.path, ec) && !ec)
            ++removed;
    }
    return removed;
}

}  // namespace nemo
