#pragma once

// Native execution consumes immutable contribution snapshots and the shared
// dependency plan. Node modules prepare values and local pass descriptions;
// this owner performs allocation, barriers, recording and submission.
//
// Bindings: set 0/0 common request; set 0/1 optional node-local payload;
// set 1/n pass inputs; set 2/0 result; set 3/0 optional float weights.
// Intermediates remain RGBA32F device images in GENERAL layout. Submission
// retention covers registrations and all resources until actual completion,
// including cancellation/timeouts. Device initialization and execution run
// on workers, never the UI event thread. readBack is diagnostic-only.

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
// executing GPU work. Dependencies are walked in the same order as the
// native evaluator, so a cache lookup can happen before any allocation or
// dispatch. The returned key includes the effect-library implementation tag
// and the document's viewer policy; callers append their concrete OCIO
// program/config identity and encoding representation settings.
//
// `colorConfigIdentity` (issue #81) is the media module's opaque OCIO
// config/context content identity; it participates in every source node's
// key, so a changed config can never serve a stale transform. Empty means
// "no config" and is a defined value, not a fallback.
[[nodiscard]] ResultKey queryViewerResultKey(const Document& document, EvaluationRequest request,
                                             const EffectLibrary& effects, std::string_view colorConfigIdentity = {});

// One executed step's device-resident result. Shared ownership: a cache
// entry (issue #9) and a returned evaluation can hold the same image.
struct GpuNodeImage {
    gpu::Image image;  // RGBA32F, representation-sized, GENERAL layout invariant
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
// freshness/publication after completion. With `sources` (issue #11),
// `source` nodes resolve their decoded frames through that session —
// pass nullptr for graphs without real-media sources (the default).
//
// `colorConfigIdentity` (issue #81) is the media module's opaque OCIO
// config/context content identity; it participates in every source node's
// key, so a changed config can never serve a stale transform. Empty means
// "no config" and is a defined value, not a fallback. The caller that owns
// the color state supplies it (the same value it passes to
// queryViewerResultKey), so this executor stays OCIO-agnostic.
[[nodiscard]] std::optional<GpuEvaluation> submitGpu(const Document& document, EvaluationRequest request,
                                                     const EffectLibrary& effects, gpu::Device& device,
                                                     gpu::Allocator& allocator, SourceSession* sources = nullptr,
                                                     std::string_view colorConfigIdentity = {});

// Executes `request` on `device` through the effect library's native
// kernels, keeping every intermediate GPU-resident. With `reuse` (issue
// #9), matching content-keyed device-resident results skip their dispatch
// and are reused in place; computed results publish under the evaluation
// ticket's freshness guard. Throws EvaluationException (node-identifying)
// for plan/effect failures and GpuException for Vulkan failures.
// `colorConfigIdentity` follows submitGpu (issue #81).
[[nodiscard]] GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request,
                                        const EffectLibrary& effects, gpu::Device& device, gpu::Allocator& allocator,
                                        std::uint64_t timeout_ns = 10'000'000'000ULL,
                                        ResultCache<GpuNodeImage>* reuse = nullptr, SourceSession* sources = nullptr,
                                        std::string_view colorConfigIdentity = {});
}  // namespace nemo::eval
