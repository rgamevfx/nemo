#pragma once

// Internal retained record of one accepted installed package (issue #37).
//
// InstalledPackages.hpp is the host contract: the immutable node
// registration, the installed panels and the refusal diagnostics. This header
// carries what the loader and the GPU projection must agree on and what must
// not become a public host API: the loaded library owner, the validated
// manifest facts and the one factory that projects a record onto a
// NodeContribution. Nothing here crosses the ABI — EffectAbi.h stays the only
// shared binary surface, and no Qt, STL or document object is handed to a
// package.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/extensions/EffectAbi.h"

namespace nemo::extensions::detail {

struct LibraryCloser {
    void operator()(void* handle) const noexcept;
};
using NativeLibraryHandle = std::unique_ptr<void, LibraryCloser>;

// One installed package library, owned by shared reference. The library is
// opened only after the package's complete manifest has been validated, and it
// is closed when the last CPU adapter and GPU preparation callback that
// captured it are gone: native code is never unloaded while an instance,
// callback or submitted GPU work can still reference it, and there is no hot
// unload. The host never calls dlclose itself.
class SharedLibrary {
public:
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    SharedLibrary(SharedLibrary&&) = delete;
    SharedLibrary& operator=(SharedLibrary&&) = delete;
    SharedLibrary(NativeLibraryHandle handle, const NemoEffectV1* entry) : handle_(std::move(handle)), entry_(entry) {}

    // dlopen(RTLD_NOW | RTLD_LOCAL) plus the fixed `nemo_effect_v1` entrypoint,
    // validating the ABI version, the struct size and every callback the
    // declared capabilities need (`needsPrepare` for a package that declares a
    // GPU implementation). Returns null and writes a diagnostic into `error`
    // when the library cannot be used; the handle is closed again in that case.
    [[nodiscard]] static std::shared_ptr<const SharedLibrary> open(const std::filesystem::path& library,
                                                                   bool needsPrepare, std::string& error);

    [[nodiscard]] const NemoEffectV1* entry() const { return entry_; }

private:
    NativeLibraryHandle handle_;
    const NemoEffectV1* entry_{nullptr};
};

// The native GPU half of one accepted package's manifest, already resolved
// against the package directory (both files exist and were read at activation).
struct PackageGpu {
    std::uint32_t payloadBytes{0};
    std::string payloadLayout;
    std::filesystem::path spirv;  // absolute path of the complete SPIR-V module
    std::string glsl;             // complete reference shader source
};

// One accepted package. `descriptor` is the schema projection the document
// catalog and the registration share; `implementationVersion` is the
// content-derived runtime identity of the node (package identity, version,
// state version and processing version), so a changed package can never serve
// a cached result; `stateIdentity` is the authored-state compatibility fact the
// document layer compares.
struct PackageRecord {
    std::string id;
    std::string stateIdentity;
    std::uint64_t version{1};
    std::uint64_t stateVersion{1};
    std::uint64_t processingVersion{1};
    std::uint64_t implementationVersion{1};
    NodeDescriptor descriptor;
    std::vector<NodeEditorContribution> editors;
    std::optional<PackageGpu> gpu;
    std::shared_ptr<const SharedLibrary> library;
};

// The CPU projection of one accepted package: the node's descriptor, the
// adapter that invokes the package's bulk interleaved-RGBA process callback
// over the real input geometry and channel roles, the generic authoring
// validation callback that calls the package's own `validate`, and the
// namespaced editor declarations. Both the CPU registration and the GPU
// projection build the node contribution through this one factory, so the two
// can never disagree about the schema, the editor declarations or the adapter.
[[nodiscard]] NodeContribution packageContribution(const PackageRecord& record);

// Effective parameters as the plain JSON object the ABI carries: numbers,
// booleans, strings and arrays, with no host tag or type envelope. Resolved
// defaults, instance overrides and request-local animation are already folded
// into `params` by the caller, so a package never fills a default itself.
[[nodiscard]] std::string parametersJson(const ParameterValues& params);

// The ABI flag word of one invocation. Bit 0 says the handed buffers carry
// premultiplied samples (the host never silently unassociates one); bit 1 says
// the image is data or identifies no complete primary RGB set, so the package
// must preserve its samples rather than applying color math.
[[nodiscard]] inline std::uint32_t effectFlags(ImageAssociation association, ColorInterpretation color,
                                               bool completeRgb) {
    std::uint32_t flags = 0;
    if (association == ImageAssociation::Premultiplied) {
        flags |= NEMO_EFFECT_PREMULTIPLIED;
    }
    if (color == ColorInterpretation::Data || !completeRgb) {
        flags |= NEMO_EFFECT_BYPASS_COLOR;
    }
    return flags;
}

// Capacity of the nonempty NUL-terminated diagnostic buffer the ABI writes on
// failure.
inline constexpr std::size_t kDiagnosticCapacity = 1024;

// The diagnostic a failed callback left in its buffer, or an explicit
// statement when it wrote nothing, so a refusal never reads as an empty error.
[[nodiscard]] inline std::string diagnosticText(const char* buffer) {
    const std::string text{buffer};
    return text.empty() ? std::string{"no diagnostic was supplied"} : text;
}

}  // namespace nemo::extensions::detail
