#pragma once

// Native GPU effect execution (issue #8, spec sections 10.3-10.4, ADR-0004).
//
// The executor consumes the same scheduled plan as the CPU reference
// (nemo::scheduleDependencies) and executes each step as a Vulkan compute
// pass over the declared effect contract:
//
//   set 0, binding 0 : EffectUniforms (std140 uint4/float4 words only)
//   set 1, binding n : input image2D  (rgba32f storage image, straight alpha)
//   set 2, binding 0 : output image2D (rgba32f storage image)
//
// Effects are packages keyed by node type; both front ends meet this
// contract — build-time Slang SPIR-V (the native path) and runtime GLSL
// (glslang, the reference-equivalence path, see EffectShaders.hpp).
//
// Residency and lifetime (spec section 10.4, ADR-0004): every step writes a
// device-resident image in GENERAL layout; dependent passes read the same
// image through an explicit imageBarrier (write→read dependency). Nothing
// on this path reads pixels back to the host — GpuEvaluation::readBack is
// the declared test/diagnostic-only seam.
//
// Failures identify the offending node and the available shader source
// location (spec section 10.4); a failed effect never silently substitutes
// another implementation.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"

namespace nemo::eval {

// std140 image of the Slang/GLSL cbuffer: uint4/float4 words only, so the
// C++ mirror matches both front ends regardless of scalar packing rules.
// meta = (region width, region height, region.x, region.y);
// misc = (localTime, 0, 0, 0); param0/param1 are effect-specific declared
// parameters.
struct EffectUniforms {
    std::uint32_t meta[4]{};
    float misc[4]{};
    float param0[4]{};
    float param1[4]{};
};

struct EffectProgram {
    std::vector<std::uint32_t> spirv;  // prebuilt SPIR-V (Slang path) when non-empty
    std::string glsl;                  // runtime GLSL (glslang path) when non-empty
    std::string sourcePath;            // available shader source location for diagnostics
};

// Effect packages keyed by node type (the initial inventory from #1:
// testpattern, constcolor, merge, output).
using EffectLibrary = std::map<std::string, EffectProgram>;

// Loads the build-time Slang effect kernels (<type>.spv) from `spvDir`,
// recording `sourceDir`/<type>.slang as the source location when present.
// Throws GpuException naming the effect and path when a kernel is missing
// or not SPIR-V — no silent substitution of another effect.
[[nodiscard]] EffectLibrary loadSlangEffectLibrary(const std::filesystem::path& spvDir,
                                                   const std::filesystem::path& sourceDir = {});

// GLSL reference implementations of the same contract, compiled at runtime
// with glslang (the seam the OCIO adapter already uses).
[[nodiscard]] EffectLibrary glslEffectLibrary();

// One executed step's device-resident result. Shared ownership: a cache
// entry (issue #9) and a returned evaluation can hold the same image.
struct GpuNodeImage {
    gpu::Image image;  // RGBA32F, region-sized, GENERAL layout invariant
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

    // Test/diagnostic readback ONLY (spec section 10.4: no routine host
    // readback between native GPU effects): downloads the node's image,
    // records its content hash into the plan's identities, and returns the
    // host copy. Production and viewer paths never call this.
    [[nodiscard]] CpuImage readBack(NodeId node, gpu::Device& device, gpu::Allocator& allocator,
                                    std::uint64_t timeout_ns = 10'000'000'000ULL);
};

// Worker-side preparation and nonblocking GPU submission. nullopt reports
// bounded in-flight capacity; no host GPU wait. Compilation/allocation are
// CPU preparation, not suitable for the UI event thread. No cache publication
// is performed; consumers own freshness/publication after completion.
[[nodiscard]] std::optional<GpuEvaluation> submitGpu(const Document& document, EvaluationRequest request,
                                                     const EffectLibrary& effects, gpu::Device& device,
                                                     gpu::Allocator& allocator);

// Executes `request` on `device` through the effect library's native
// kernels, keeping every intermediate GPU-resident. With `reuse` (issue
// #9), matching content-keyed device-resident results skip their dispatch
// and are reused in place; computed results publish under the evaluation
// ticket's freshness guard. Throws EvaluationException (node-identifying)
// for plan/effect failures and GpuException for Vulkan failures.
[[nodiscard]] GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request,
                                        const EffectLibrary& effects, gpu::Device& device, gpu::Allocator& allocator,
                                        std::uint64_t timeout_ns = 10'000'000'000ULL,
                                        ResultCache<GpuNodeImage>* reuse = nullptr);
}  // namespace nemo::eval
