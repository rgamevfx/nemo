#pragma once

// Native execution consumes immutable contribution snapshots and the shared
// dependency plan. Node modules prepare values and local pass descriptions;
// this owner performs allocation, barriers, recording and submission.
//
// Bindings: set 0/0 common request; set 0/1 optional node-local payload;
// set 0/2 per-input geometry; set 0/3 channel plan;
// set 1/n pass inputs; set 2/0 result; set 3/0 optional float weights.
// Native images are R32_SFLOAT 2D channel planes (issue #90): logical W×H with
// plane c at (x, y + c*H), so one GENERAL-layout device image carries every
// named channel. Submission retention covers registrations and all resources
// until actual completion, including cancellation/timeouts. Device
// initialization and execution run on workers, never the UI event thread.
// readBack is diagnostic-only.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/eval/GpuContribution.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"

namespace nemo::eval {

class SourceSession;

struct EffectProgram {
    std::shared_ptr<const std::vector<std::uint32_t>> spirv;
    std::string sourcePath;
};

enum class EffectBackend { Slang, Glsl };

// Prepared once before publication. Consumers only receive const entries;
// callbacks and programs stay alive through the immutable library snapshot.
struct RegisteredGpuEffect {
    std::optional<GpuImplementation> implementation;
    std::vector<EffectProgram> programs;
    std::string unavailableReason;
};

class EffectLibrary {
public:
    EffectLibrary();
    EffectLibrary(std::vector<GpuNodeContribution> contributions, EffectBackend backend,
                  const std::filesystem::path& spvDir = {}, const std::filesystem::path& sourceDir = {});
    [[nodiscard]] std::shared_ptr<const NodeContributions> contributions() const;
    [[nodiscard]] const RegisteredGpuEffect* find(std::string_view type) const;
    [[nodiscard]] std::uint64_t fingerprint() const;
    [[nodiscard]] std::shared_ptr<const void> retain() const;
    // Check schema and backend availability before reuse as well as execution.
    [[nodiscard]] const RegisteredGpuEffect& require(const NodeCatalog& catalog, const NodeInstance& node) const;

private:
    struct Data;
    std::shared_ptr<const Data> data_;
};

// Both backend projections use the single explicit built-in contribution list.
// Missing kernels are node-local unavailability, not failure of unrelated
// registrations. Evaluation reports the node and available shader path.
[[nodiscard]] EffectLibrary loadSlangEffectLibrary(const std::filesystem::path& spvDir,
                                                   const std::filesystem::path& sourceDir = {});

// GLSL reference implementations of the same contract, compiled at runtime
// with glslang (the seam the OCIO adapter already uses).
[[nodiscard]] EffectLibrary glslEffectLibrary();

// Computes the content-derived viewer identity for a request without
// executing GPU work and without acquiring a pixel (issue #88). The same
// described plan the native executor consumes resolves every node's effective
// state, described output and source request, so a cache lookup can happen
// before any allocation, dispatch or decode. The returned key includes the
// effect-library implementation tag and the document's viewer policy; callers
// append their concrete OCIO program/config identity and encoding
// representation settings.
//
// `colorConfigIdentity` (issue #81) is the media module's opaque OCIO
// config/context content identity; it participates in every source node's
// key, so a changed config can never serve a stale transform. Empty means
// "no config" and is a defined value, not a fallback.
//
// `sources` describes real media through the source description provider seam
// (a SourceSession, or any provider implementing
// SourceDescriptionProvider): the media's own metadata decides the described
// format and the pre-resolved request decides the source identity, so no
// decoder is opened and no frame is uploaded. A graph that needs a source
// description without a provider fails honestly here, exactly as the executor
// would.
//
// `plan` is the prebuilt resolved plan seam (issue #88). A caller that already
// built the described plan for THIS document snapshot, request and effect
// library — the viewer render path does, so key and execution never resolve the
// same authored state twice — passes it here and to evaluateGpu/submitGpu. The
// plan must come from
// `planDependencyRegions(document, canonicalizeRequest(request),
// *effects.contributions(), sources)`, and it is verified strictly against its
// recorded origin identity (document object, state revision, registration and
// the exact canonical demand): a plan that does not provably resolve this call
// is REFUSED with EvaluationException rather than replanned, so a supplied plan
// and its request can never disagree and the caller's key always describes the
// pixels that were executed. The plan BORROWS the document and the registration
// for the duration of the call — it must not be retained past the evaluation
// that owns them — and the request it is compared against is the caller's own
// canonical demand, domain included. nullptr (the default) plans here.
[[nodiscard]] ResultKey queryViewerResultKey(const Document& document, EvaluationRequest request,
                                             const EffectLibrary& effects, std::string_view colorConfigIdentity = {},
                                             SourceDescriptionProvider* sources = nullptr,
                                             const RegionPlan* plan = nullptr);

// One executed step's device-resident result. Shared ownership: a cache
// entry (issue #9) and a returned evaluation can hold the same image.
struct GpuNodeImage {
    // R32_SFLOAT channel planes (issue #90): extent (logical width,
    // logical height * channelCount), GENERAL layout invariant. `layout` keeps
    // the LOGICAL width/height and the image's named channels.
    gpu::Image image;
    ImageLayout layout;
};

class GpuEvaluation {
public:
    EvaluationPlan plan;
    // Completion belongs to device.submissions(device.graphics_family()).
    // Poll/wait before consuming on another queue. Dropping this result
    // cancels publication, not execution: the queue retains its resources.
    std::optional<std::uint64_t> completion;
    // Device-resident result of every scheduled node, keyed by node id.
    // Shared ownership keeps reused results alive in the evaluator cache
    // (issue #9) while the caller holds the returned evaluation.
    std::map<NodeId, std::shared_ptr<const GpuNodeImage>> images;
    // Content key of every scheduled node's result (issue #9 identity):
    // the seam downstream caches address — the viewer frames (#11) key
    // their representation reuse by viewerResultKey(keys.at(output)).
    std::map<NodeId, ResultKey> keys;

    // Test/diagnostic readback ONLY (spec section 10.4: no routine host
    // readback between native GPU effects): downloads the node's image,
    // records its content hash into the plan's identities, and returns the
    // host copy. Production and viewer paths never call this.
    [[nodiscard]] CpuImage readBack(NodeId node, gpu::Device& device, gpu::Allocator& allocator,
                                    std::uint64_t timeout_ns = 10'000'000'000ULL);
};

// Worker-side preparation and nonblocking GPU submission. nullopt reports
// bounded in-flight capacity (or the queue being briefly owned by another
// submitter, e.g. the presentation device, issue #11); no host GPU wait.
// Compilation/allocation are CPU preparation, not suitable for the UI event
// thread. No cache publication is performed; consumers own
// freshness/publication after completion.
//
// Planning is metadata-only (issue #88): the shared described plan resolves
// every node's effective state, described output and — for a Source node —
// its media request WITHOUT touching a pixel, and the sources are acquired
// lazily, per node, only after the plan and the backend are validated and the
// reuse cache has declined to serve that node. External media is described
// through the provider (`SourceSession` implements it); pass nullptr for
// graphs without real-media sources (the default), where a node that needs a
// source description fails honestly before anything is scheduled.
//
// `colorConfigIdentity` (issue #81) is the media module's opaque OCIO
// config/context content identity; it participates in every source node's
// key, so a changed config can never serve a stale transform. Empty means
// "no config" and is a defined value, not a fallback. The caller that owns
// the color state supplies it (the same value it passes to
// queryViewerResultKey), so this executor stays OCIO-agnostic.
//
// `plan` is the same prebuilt resolved plan seam queryViewerResultKey accepts
// (issue #88): passing the plan the key query already built makes one viewer
// render resolve its authored state exactly once. It is verified against its
// recorded origin identity and REFUSED (EvaluationException) when it does not
// provably resolve this exact document snapshot, registration and canonical
// demand; only an absent plan is planned here. Like the key query, this call
// borrows the plan's document/registration and never retains it.
[[nodiscard]] std::optional<GpuEvaluation> submitGpu(const Document& document, EvaluationRequest request,
                                                     const EffectLibrary& effects, gpu::Device& device,
                                                     gpu::Allocator& allocator, SourceSession* sources = nullptr,
                                                     std::string_view colorConfigIdentity = {},
                                                     const RegionPlan* plan = nullptr);

// Executes `request` on `device` through the effect library's native
// kernels, keeping every intermediate GPU-resident. With `reuse` (issue
// #9), matching content-keyed device-resident results skip their dispatch
// and are reused in place; computed results publish under the evaluation
// ticket's freshness guard. Throws EvaluationException (node-identifying)
// for plan/effect failures and GpuException for Vulkan failures.
// `colorConfigIdentity` and `plan` follow submitGpu (issues #81, #88).
[[nodiscard]] GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request,
                                        const EffectLibrary& effects, gpu::Device& device, gpu::Allocator& allocator,
                                        std::uint64_t timeout_ns = 10'000'000'000ULL,
                                        ResultCache<GpuNodeImage>* reuse = nullptr, SourceSession* sources = nullptr,
                                        std::string_view colorConfigIdentity = {}, const RegionPlan* plan = nullptr);
}  // namespace nemo::eval
