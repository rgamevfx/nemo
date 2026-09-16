#pragma once

#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace nemo::eval {

// Identity of the native binding contract below. Any change to the meaning,
// layout or coordinate convention of these bindings requires a new value: the
// effect library mixes it into its fingerprint, so no cached result can
// survive a semantic change to the interface the kernels were compiled
// against. Node-local payload layouts are versioned separately by their own
// declaring node.
inline constexpr std::string_view kEffectBindingContractVersion = "nemo.native.bindings.v5";

// Internal binding contract v5 (issue #88). Only coordinate/time facts are
// common to effects. Node-local payloads have their own layout at set 0,
// binding 1.
//
// The common request describes the raster THIS PASS produces, so a pass whose
// output is a node-local scratch image (a separable filter's intermediate)
// dispatches over the scratch's own coverage instead of the node's. Origins
// are SIGNED (issue #88: a described output's region and an effect's own
// coverage may lie outside `[0, format)`), so every origin/mapping word below
// is signed and the raster extent stays unsigned.
//
// Inputs are located independently through the set 0 binding 2 geometry block:
// an input may cover a different rectangle of the same global sampling lattice
// (region evaluation, issue #85), so its raster origin and extent are bound
// with it.
struct EffectRequestUniforms {
    // meta: (full-resolution image width, full-resolution image height,
    // region origin x, region origin y). Origins are signed.
    std::int32_t meta[4]{};
    // meta2: (raster width, raster height, sampling scale, 0).
    std::uint32_t meta2[4]{};
    // misc: (localTime, 0, 0, 0).
    float misc[4]{};
    // support (issue #88): the half-open raster-index rectangle of the samples
    // that ARE the image's data, i.e. whose full-resolution anchor
    // `(meta.z + x*meta2.z, meta.w + y*meta2.z)` lies inside the produced
    // image's declared data bounds. Every sample outside it is transparent
    // black `{0,0,0,0}`; a zero-width or zero-height rectangle is therefore a
    // fully transparent raster (an empty data window is a valid, connected
    // image, not an absent input), and data outside the display format is kept
    // because the test is the data bounds alone.
    //
    // `(-1,-1,-1,-1)` means "this raster declares no support": a node-local
    // scratch image, whose only consumer is the node's own next pass within the
    // same node, is written in full and is never masked.
    //
    // The raster's extent, origin and sampling scale are NEVER changed by the
    // support: coverage stays what the request asked for and the guard only
    // decides which of its samples carry data.
    std::int32_t support[4]{};
};
static_assert(sizeof(EffectRequestUniforms) == 64);

// Set 0, binding 2: one entry per bound pass input, indexed exactly like the
// set-1 image bindings. `regionAndOffset` is (the input's full-resolution
// region origin x/y, its raster origin relative to the pass output raster x/y)
// — a pass input pixel is `p + offset` for pass output pixel `p`, because both
// rasters sit on the same lattice. `extent` is (raster width, raster height,
// sampling scale, 1). Every pass that declares image inputs declares this
// block; the executor always supplies one valid entry per binding.
//
// The External binding (a Source role's decoded frame) has no meaningful
// relative offset: it is addressed by absolute full-resolution coordinates.
// Its entry carries `regionAndOffset.xy` = the decoded raster's full-resolution
// origin (usually nonzero, possibly negative — a data window outside the
// format), `regionAndOffset.zw` = 0, and `extent.xy` = the decoded raster
// extent at `extent.z` = 1 (external media is always full resolution). A
// Source kernel maps absolute coordinate `c` to source pixel `c - origin` and
// returns transparent black when that index lies outside the extent; it never
// rescales by a fill ratio.
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
    const ParameterValues& effectiveParams;
    bool maskPresent;
    float pixelAspect;
    // Actual request of every declared input port, in declared-port order.
    // An absent optional slot holds a default-constructed request (the
    // declared-port alignment the CPU adapter also keeps); a connected input
    // holds the rectangle the executor will actually bind, which may be wider
    // than this node's demand whenever a cache hit backs it.
    std::span<const EvaluationRequest> inputRequests;
    // The node's described output (issue #88): the format, data bounds, pixel
    // aspect, channels and interpretation the shared planner resolved for
    // `node` once, before any execution. Preparation reads geometry and
    // interpretation from here instead of re-deriving them from the request or
    // from a decoded frame, so CPU and native execution describe the same
    // authored state. The display window's origin is 0; dataBounds is signed
    // and may lie outside the format, and an empty dataBounds is a valid
    // (fully transparent) image.
    const ImageDescription& description;
    // Pre-resolved external media request of a Source node (issue #88):
    // nullopt for every other role. The executor resolves it exactly once in
    // the shared plan, so preparation never re-resolves authored state and
    // never depends on a decoded frame being present.
    const EffectiveSourceRequest* source;
    // Description of every declared input port, in declared-port order; an
    // absent optional slot holds nullptr (the same alignment as
    // `inputRequests`). A node's describe callback reads its inputs' formats
    // and data bounds from here.
    std::span<const ImageDescription* const> inputDescriptions;
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
