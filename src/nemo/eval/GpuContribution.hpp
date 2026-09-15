#pragma once

#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Request.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace nemo::eval {

// Internal binding contract v3. Only coordinate/time facts are common to
// effects. Node-local payloads have their own layout at set 0, binding 1.
//
// The common request describes the raster THIS PASS produces, so a pass whose
// output is a node-local scratch image (a separable filter's intermediate)
// dispatches over the scratch's own coverage instead of the node's. Inputs are
// located independently through the set 0 binding 2 geometry block: an input
// may cover a different rectangle of the same global sampling lattice (region
// evaluation, issue #85), so its raster origin and extent are bound with it.
struct EffectRequestUniforms {
    std::uint32_t meta[4]{};
    std::uint32_t meta2[4]{};
    float misc[4]{};
};
static_assert(sizeof(EffectRequestUniforms) == 48);

// Set 0, binding 2: one entry per bound pass input, indexed exactly like the
// set-1 image bindings. `regionAndOffset` is (the input's full-resolution
// region origin x/y, its raster origin relative to the pass output raster x/y)
// — a pass input pixel is `p + offset` for pass output pixel `p`, because both
// rasters sit on the same lattice. `extent` is (raster width, raster height,
// sampling scale, 1). Every pass that declares image inputs declares this
// block; the executor always supplies one valid entry per binding.
struct EffectInputGeometry {
    std::int32_t regionAndOffset[4]{};
    std::uint32_t extent[4]{};
};
static_assert(sizeof(EffectInputGeometry) == 32);

enum class EffectImageKind { Input, Scratch, External, Output };
struct EffectImageRef {
    EffectImageKind kind{EffectImageKind::Input};
    std::uint32_t index{};
    friend bool operator==(const EffectImageRef&, const EffectImageRef&) = default;
};

// Local pass names never enter the global node inventory. Inputs bind in
// vector order at set 1; the result binds at set 2, binding 0. Only a Source
// role can read External 0.
struct EffectPassDefinition {
    std::string id;
    std::string shader;
    std::string glsl;
    std::vector<EffectImageRef> inputs;
    EffectImageRef output{EffectImageKind::Output, 0};
    bool weights{false};
};

// Coverage of one scratch image the selected local passes produce. A scratch
// raster is node-local, so the node declares it: the executor allocates and
// dispatches that pass over exactly this full-resolution region at the node
// request's sampling scale. A separable filter's intermediate is the reason
// this exists: its horizontal pass needs an X range the vertical pass will read
// and a Y range that includes the taps of every output row, which is neither
// the node's own coverage nor its input's.
struct EffectScratchRegion {
    std::uint32_t index{};
    Region region;
    friend bool operator==(const EffectScratchRegion&, const EffectScratchRegion&) = default;
};

struct GpuPreparation {
    std::vector<std::byte> payload;
    std::vector<float> weights;
    std::vector<std::uint32_t> passes;
    // One entry per scratch image the selected passes write; the executor
    // rejects a selected pass whose scratch output is undeclared, and a
    // declaration whose pass is not selected.
    std::vector<EffectScratchRegion> scratch;
};

struct GpuNodeContext {
    const NodeCatalog& catalog;
    const NodeInstance& node;
    const EvaluationRequest& request;
    ParameterValues& effectiveParams;
    bool maskPresent;
    float pixelAspect;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    // Actual request of every declared input port, in declared-port order.
    // An absent optional slot holds a default-constructed request (the
    // declared-port alignment the CPU adapter also keeps); a connected input
    // holds the rectangle the executor will actually bind, which may be wider
    // than this node's demand whenever a cache hit backs it.
    std::span<const EvaluationRequest> inputRequests;
};

// Preparation is worker-side, reentrant value computation: no device,
// allocator, queue, callbacks into the UI, or borrowed resources. The shared
// executor owns upload, allocation, recording, submission and retirement.
// Version must match the schema; changing code/layout requires a new version.
// Captures must be immutable or synchronized; context references are valid
// only during preparation, and returned payload/weight values are owned.
struct GpuImplementation {
    std::uint64_t version{1};
    std::string payloadLayout;
    std::size_t payloadSize{};
    std::vector<EffectPassDefinition> passes;
    std::function<GpuPreparation(const GpuNodeContext&)> prepare;
};

struct GpuNodeContribution {
    NodeContribution node;
    std::optional<GpuImplementation> gpu;
    std::string gpuUnavailableReason;
};

template <typename T>
[[nodiscard]] std::vector<std::byte> effectPayload(const T& value) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 16 == 0);
    std::vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

[[nodiscard]] std::vector<GpuNodeContribution> builtinGpuContributions();

}  // namespace nemo::eval
