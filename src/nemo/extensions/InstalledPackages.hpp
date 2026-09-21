#pragma once

// Installed trusted native effect packages (#37) and their per-user settings
// (#105).
//
// One seam, three parts:
//
//   * metadata inventory — package-folder discovery, one strictly validated
//     manifest per package folder, and the declared metadata (identity, name,
//     author, version, description, contributions, dependencies, diagnostics).
//     Inspection never opens a library, never calls an entrypoint and touches
//     no file other than the manifest, so listing or refreshing the Settings
//     surface cannot execute a package.
//   * per-user settings — the registered linked package folders and the
//     packages the user enabled, persisted as (identity, canonical location)
//     pairs so trust follows the install location: a scan order, a renamed
//     folder or a second copy of an identity can never silently change which
//     installed code runs, and an identity alone never enables anything.
//   * activation — one immutable startup snapshot assembled from the enabled,
//     admitted packages through the fixed ABI in EffectAbi.h. A package that is
//     refused — malformed manifest, unsupported format/API/capability, missing
//     or escaping declared file, duplicate identity, missing/disabled/cyclic
//     dependency, invalid schema, unusable library — is reported as a
//     diagnostic and contributes nothing; it never fails the application and
//     never hides an unrelated supported package. Dependencies of a refused
//     package are refused transitively, every offender of a duplicate identity
//     is refused, and a disabled prerequisite is diagnosed rather than enabled
//     implicitly.
//
// Packages are trusted installed native code, not a sandbox: the library is
// opened through the fixed ABI in EffectAbi.h, no Qt object is created on this
// path, and nothing but plain JSON strings and host-owned buffers crosses the
// boundary. Activation validates the complete manifest before the entrypoint
// runs; the library stays loaded (owned by shared reference) until the last CPU
// or GPU user of its contributions is gone, so there is no hot unload.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/extensions/PackageRecord.hpp"

namespace nemo::eval {
struct GpuNodeContribution;
}

namespace nemo::extensions {

// One installed panel a package contributes. `source` is an absolute file URL
// of the package's own presentation resource; the host resolves it with the
// same QML editor host the built-in panels use.
struct PanelContribution {
    std::string id;
    std::string title;
    std::string source;
};

// Why a discovered package is, or is not, active. The Settings surface shows
// this instead of parsing diagnostic text; `diagnostic` carries the human
// detail and the offending location.
enum class PackageStatus {
    // The package contributed to the startup snapshot this process runs with.
    Active,
    // Admitted, but the persisted policy has not enabled it.
    Disabled,
    // A registered package folder or its manifest.json is missing or moved.
    MissingPackage,
    // manifest.json could not be read, is not JSON, or is not a valid
    // declaration (wrong shape, wrong value type, unknown key).
    MalformedManifest,
    // The declaration itself is not supported by this build: manifest format,
    // effect API range, capability, declared file, node schema or GPU binding
    // contract.
    Incompatible,
    // Another discovered package declares the same identity, node type, editor
    // or panel; every offender is refused, so discovery order never decides.
    DuplicateIdentity,
    // A declared dependency is not installed.
    MissingDependency,
    // A declared dependency is installed but the user has not enabled it. It is
    // diagnosed, never enabled implicitly.
    DisabledDependency,
    // A declared dependency was refused in this snapshot.
    RefusedDependency,
    // The declared dependencies contain a cycle.
    DependencyCycle,
    // The native library could not be opened, or its declared default
    // parameters are invalid.
    FailedToLoad,
};

// One package folder the host found, with the metadata its manifest declares.
// This is inspection data: filling it in parses nothing but manifest.json, and
// `active` is only ever true for an entry of the startup snapshot that really
// contributed.
struct PackageInfo {
    // The declared package identity, empty when the manifest could not be
    // parsed far enough to declare one.
    std::string id;
    // Declared display name, author and description. Empty means the manifest
    // declared none: the host never invents one.
    std::string name;
    std::string author;
    std::string description;
    // The declared package version, 0 when no manifest declared one. It is the
    // package's own version, not the manifest format, the authored-state
    // version, the processing version or the effect ABI version.
    std::uint64_t version{0};
    // Canonical absolute location of the package folder.
    std::string directory;
    // Declared node types, editor identities, panel identities and package
    // dependencies, in declaration order.
    std::vector<std::string> nodeTypes;
    std::vector<std::string> editors;
    std::vector<std::string> panels;
    std::vector<std::string> dependencies;
    // The state, with the reason for a non-active one in `diagnostic` (empty
    // only for an active package).
    PackageStatus status{PackageStatus::Disabled};
    // The package's DECLARATION is admitted by this build: the manifest was
    // read and its format, API range, capabilities, declared files, node schema
    // and identities are usable. A dependency problem or a native load failure
    // does not clear this fact; metadata admission is never trust.
    bool admitted{false};
    // The Settings surface may offer to ENABLE this package: its declaration is
    // admitted and no blocker this side cannot repair (a missing dependency, a
    // refused dependency, a cycle, or a failed native load) stands in the way. A
    // merely disabled prerequisite keeps this true — the user enables the
    // prerequisite explicitly and nothing enables it for them. Disabling never
    // needs this.
    bool canEnable{false};
    // Why this package contributes nothing, empty when it is admitted. In an
    // activation snapshot it also carries a native load or default-validation
    // failure; metadata inspection never has one.
    std::string diagnostic;
    // The persisted policy asks for this package (matched by canonical
    // location, and by identity as well when the manifest declared one).
    bool requestedEnabled{false};
    // The package contributed to this startup snapshot. A package that is
    // requested but refused is inactive; enabling or disabling one takes effect
    // at restart. False for every entry of a metadata inspection.
    bool active{false};
};

// One explicit enablement: a package identity at one canonical package folder.
struct EnabledPackage {
    std::string id;
    std::string directory;
};

// The user's persisted package settings. Both lists are canonical absolute
// paths, so two spellings of one folder never register or enable it twice.
struct PackagePreferences {
    // Registered external package folders, in registration order. A folder
    // stays listed after it moves or disappears; it is never copied, moved or
    // deleted, and registering it activates nothing on its own.
    std::vector<std::string> linkedFolders;
    // Packages the user enabled, by identity AND canonical location.
    std::vector<EnabledPackage> enabled;

    // Whether `id` at `canonicalDirectory` is enabled: the location must match,
    // so an identity alone never enables a different installed copy.
    [[nodiscard]] bool isEnabled(const std::string& id, const std::string& canonicalDirectory) const;
    // Enables or disables one package location. Enabling twice, or disabling
    // what is not enabled, changes nothing.
    void setEnabled(const std::string& id, const std::string& canonicalDirectory, bool enabled);
    // Forgets a registered folder and the enablement of the package installed
    // there. Removing a registration never deletes or alters package files.
    void removeLinkedFolder(const std::string& canonicalDirectory);
};

// The outcome of reading the settings file. A corrupt, unreadable or
// unsupported file yields EMPTY preferences — nothing linked, nothing enabled —
// and one diagnostic, so an unreadable trust decision always fails closed and
// is visible instead of being silently guessed.
struct PackagePreferencesLoad {
    PackagePreferences preferences;
    std::string diagnostic;
};

// The per-user package settings file: $XDG_CONFIG_HOME/nemo/extensions.json, or
// ~/.config/nemo/extensions.json; %LOCALAPPDATA%/Nemo/extensions.json on
// Windows. Empty when the platform gives no per-user location; both applications
// and the headless CLI read exactly this file, so they share one activation
// policy.
[[nodiscard]] std::filesystem::path packagePreferencesPath();

// Reads the settings file. A missing file is a normal first launch: empty
// preferences and no diagnostic. The file is never rewritten or repaired here.
[[nodiscard]] PackagePreferencesLoad loadPackagePreferences(const std::filesystem::path& path);

// Writes the settings file atomically (a temporary file in the same directory
// is renamed over the target). Returns false and sets `diagnostic` when the
// file could not be written, so a caller never reports a failed save as saved.
[[nodiscard]] bool savePackagePreferences(const std::filesystem::path& path, const PackagePreferences& preferences,
                                          std::string& diagnostic);

// The standard per-user extension directory: one package per child folder.
// Linux $XDG_DATA_HOME/nemo/extensions, otherwise ~/.local/share/nemo/extensions;
// Windows %LOCALAPPDATA%/Nemo/extensions. Empty when the platform gives no
// per-user location.
[[nodiscard]] std::vector<std::filesystem::path> standardPackageRoots();

// Canonical location of a user-selected package folder: the folder must exist,
// be a directory and carry a manifest.json, otherwise the selection is refused
// with a reason and nothing is registered.
[[nodiscard]] bool canonicalPackageFolder(const std::filesystem::path& folder, std::string& canonical,
                                          std::string& reason);

// Metadata inspection of the packages one normal startup discovers: the child
// package folders of the standard per-user roots plus every registered linked
// folder, which IS a package folder. A registered folder that moved or lost its
// manifest stays listed with PackageStatus::MissingPackage instead of
// disappearing. Inspection never executes a package: `active` is false and
// `status` reports metadata admission only. Package failures become diagnostics.
[[nodiscard]] std::vector<PackageInfo> inspectInstalledPackages(const PackagePreferences& preferences);

class InstalledPackages {
public:
    // One normal application startup. When NEMO_EXTENSION_PATH is set, that
    // explicit developer/test override selects the roots and every admitted
    // package is activated with no preference read or written; otherwise the
    // persisted preferences select the roots and the enabled packages, and
    // packages that were never enabled stay inactive.
    InstalledPackages();

    // The trusted explicit override: discovers `roots` (each root's direct child
    // package folders) and activates every admitted package, without reading or
    // writing any preference. This is the developer/test seam — it never
    // changes what a normal launch runs.
    explicit InstalledPackages(std::vector<std::filesystem::path> roots);

    // The inventory of every package folder this startup discovered, in
    // discovery order: metadata, requested enablement and whether the package
    // really contributed. Admitted packages have an empty diagnostic.
    [[nodiscard]] const std::vector<PackageInfo>& inventory() const { return inventory_; }

    // True when an explicit root override rather than the persisted preferences
    // selected what this startup runs.
    [[nodiscard]] bool trustedOverride() const { return trustedOverride_; }

    // The immutable registration this build runs with: exactly the built-in
    // contributions plus one contribution per active package. The snapshot
    // also retains the active packages' native libraries.
    [[nodiscard]] std::shared_ptr<const NodeContributions> contributions() const { return contributions_; }

    // Active panel declarations, in discovery order (root order, then package
    // folder name).
    [[nodiscard]] const std::vector<PanelContribution>& panels() const { return panels_; }

    // One entry per package that contributed nothing, in discovery order: the
    // package's identity (its id once the manifest declared one, else its
    // directory) and the reason. Active packages add no entry.
    [[nodiscard]] const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    friend std::vector<eval::GpuNodeContribution> gpuContributions(const InstalledPackages&);
    // One startup. `trustedRoots` is the explicit developer/test override: those
    // roots are scanned and every admitted package becomes active, with no
    // preference read or written. Otherwise the standard per-user roots plus the
    // persisted linked folders and enablements select what runs.
    void initialize(std::vector<std::filesystem::path> trustedRoots, bool trusted);

    std::vector<detail::PackageRecord> records_;
    std::vector<PanelContribution> panels_;
    std::vector<std::string> diagnostics_;
    std::vector<PackageInfo> inventory_;
    bool trustedOverride_{false};
    std::shared_ptr<const NodeContributions> contributions_;
};

}  // namespace nemo::extensions
