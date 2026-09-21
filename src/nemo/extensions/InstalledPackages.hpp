#pragma once

// Installed trusted native effect packages (issue #37).
//
// The host's installed-package seam: explicit roots discovered without any
// working-directory or project loading, one strictly validated manifest per
// package folder, one immutable NodeContributions assembled from the built-ins
// plus every accepted package, the installed panel declarations and the refusal
// diagnostics. A package that is refused — malformed manifest, unsupported
// format/API/capability, missing or escaping declared file, duplicate identity,
// missing or cyclic dependency, invalid schema, unusable library — is reported
// as a diagnostic and contributes nothing; it never fails the application and
// never hides an unrelated supported package. Dependencies of a refused package
// are refused transitively, and every offender of a duplicate identity is
// refused, so which package wins is never decided by discovery order.
//
// Packages are trusted installed native code, not a sandbox: the library is
// opened through the fixed ABI in EffectAbi.h, no Qt object is created on this
// path, and nothing but plain JSON strings and host-owned buffers crosses the
// boundary. Activation validates the complete manifest before the entrypoint
// runs; the library stays loaded (owned by shared reference) until the last CPU
// or GPU user of its contributions is gone, so there is no hot unload.

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

// Explicit NEMO_EXTENSION_PATH roots (colon-separated on Unix, semicolon on
// Windows), otherwise the user's platform data directory. No implicit working
// directory or project package discovery.
[[nodiscard]] std::vector<std::filesystem::path> installedPackageRoots();

class InstalledPackages {
public:
    // Discovers and activates every package folder of `roots` (each root's
    // direct child directories). Never throws because of a package: a refused
    // package becomes a diagnostic. Every root that is not an existing
    // directory is skipped.
    explicit InstalledPackages(std::vector<std::filesystem::path> roots);

    // The immutable registration this build runs with: exactly the built-in
    // contributions plus one contribution per accepted package. The snapshot
    // also retains the accepted packages' native libraries.
    [[nodiscard]] std::shared_ptr<const NodeContributions> contributions() const { return contributions_; }

    // Installed panel declarations, in discovery order (root order, then
    // package folder name).
    [[nodiscard]] const std::vector<PanelContribution>& panels() const { return panels_; }

    // One entry per refused package, in discovery order: the package's identity
    // (its id once the manifest declared one, else its directory) and the
    // reason it was refused. Accepted packages add no entry.
    [[nodiscard]] const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    friend std::vector<eval::GpuNodeContribution> gpuContributions(const InstalledPackages&);
    std::vector<detail::PackageRecord> records_;
    std::vector<PanelContribution> panels_;
    std::vector<std::string> diagnostics_;
    std::shared_ptr<const NodeContributions> contributions_;
};

}  // namespace nemo::extensions
