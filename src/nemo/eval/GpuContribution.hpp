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
inline constexpr std::string_view kEffectBindingContractVersion = "nemo.native.bindings.v8";

// Internal binding contract v8 (issues #88, #90, #98, #93). Only coordinate/time facts
// and the resolved named-channel projection are common to effects. Node-local
// payloads have their own layout at set 0, binding 1.
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
    // same node, is written in full and is never masked — and so is the output
    // pass of an explicitly EDGE-EXTENDED description (issue #92), whose finite
    // data bounds are a retained edge domain rather than the support of the
    // image.
    //
    // The raster's extent, origin and sampling scale are NEVER changed by the
    // support: coverage stays what the request asked for and the guard only
    // decides which of its samples carry data.
    std::int32_t support[4]{};
    // channels (issues #90, #98, native channel images): (the produced raster's
    // STORED channel count, its components per texel, whether the channel plan
    // fills any stored channel, 0). A native image carries every stored channel
    // in stored order and never drops or pads one; its representation follows
    // from the stored count alone:
    //
    //   components 4   one packed VK_FORMAT_R32G32B32A32_SFLOAT image at the
    //                  LOGICAL extent W×H, whose texel (x, y) holds stored
    //                  channels 0..3 of logical pixel (x, y) in its R,G,B,A
    //                  components, so a whole pixel is one vector load or store.
    //   components 1   VK_FORMAT_R32_SFLOAT scalar planes at W×(H*C), stored
    //                  channel c of logical pixel (x, y) at (x, y + c*H), so
    //                  one-, two- and three-channel data is never expanded.
    //
    // `channels.y` is the produced raster's ACTUAL storage, so a kernel picks its
    // access shape from the binding rather than from a compiled-in assumption.
    // `channels.z` is 0 exactly when the pass's own RGBA math produces every
    // stored channel, which is when the channel plan has nothing to fill and the
    // kernel never reads it. `channels.x` is the number of entries in the set 0
    // binding 3 channel plan.
    std::uint32_t channels[4]{};
    // rgba (issues #90, #98): the produced raster's STORED channel index for the
    // R, G, B and A projection roles, resolved once by the executor from the
    // described channel names (`nemo::rgbaChannelIndices`); -1 means the role is
    // absent from the image. The four stored channels of a packed image are NOT
    // assumed to be R,G,B,A — this word is the only authority on the roles, so
    // an arbitrary four-channel image (any names, any order) reads and writes
    // correctly. A kernel writes ONLY the roles that exist, so no named RGB or
    // alpha is ever manufactured, and reads a missing role as 0.0 (R/G/B) or,
    // for A, as 1.0 when the image carries at least one RGB role and 0.0 when it
    // carries none — the same projection the CPU reference's `CpuImage::pixel`
    // applies.
    std::int32_t rgba[4]{-1, -1, -1, -1};
};
static_assert(sizeof(EffectRequestUniforms) == 96);

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
//
// `extent.xy` is the input raster's LOGICAL size (issues #90, #98): an image
// stored as channel planes is `(extent.x, extent.y * channelCount)` texels on
// the device, and its stored channel c starts at device row `c * extent.y`; a
// packed four-channel image is exactly `(extent.x, extent.y)` texels, its four
// stored channels sharing each texel.
struct EffectInputGeometry {
    std::int32_t regionAndOffset[4]{};
    std::uint32_t extent[4]{};
    // The input image's STORED channel index for the R, G, B and A roles,
    // resolved from its described channel names; -1 = the role is absent (issues
    // #90, #98).
    std::int32_t rgba[4]{-1, -1, -1, -1};
    // The input image's ACTUAL storage: its stored channel count and its
    // components per texel (4 = packed four-channel, 1 = scalar channel planes),
    // both derived from the image's real format, never guessed from a
    // description. A decoded frame may carry fewer stored channels than its
    // description names (a retained policy-cleared sample), so a kernel that
    // addresses channels 1:1 bounds itself by this word — and reads its access
    // shape from it — rather than by the description.
    std::uint32_t channels[4]{};
};
static_assert(sizeof(EffectInputGeometry) == 64);

// Set 0, binding 3: the pass output's channel plan, one entry per STORED channel
// in stored order (issues #90, #98). A kernel's RGBA math writes the stored
// channels the request's `rgba` word names; every OTHER stored channel of the
// produced raster is filled from this plan by the shared `gpuStorePixel` helper,
// so named channels a pass does not select survive at unchanged coordinates
// instead of being dropped or left undefined. The executor resolves the plan
// once, from the described channel names, before any dispatch, and the request's
// `channels.z` says whether it fills anything at all — a pass whose raster holds
// only its own roles never reads this buffer.
struct EffectChannelPlanEntry {
    // >= 0: copy this stored channel of the pass's set-1 binding 0 image at the
    // same lattice coordinates.
    // -1: the pass's own kernel produces this channel (nothing to preserve).
    // -2: the channel has no source in the bound image and is numeric zero.
    std::int32_t sourcePlane{-1};
    std::int32_t reserved{};
};
static_assert(sizeof(EffectChannelPlanEntry) == 8);

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
    // Read-only node-local geometry words at set 4/binding 0. The executor
    // owns upload and retention; the contribution owns the versioned layout.
    bool geometry{false};
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
    // Node-local finite float table, uploaded verbatim at set 3/binding 0.
    // Usually filter weights. Geometry indices wider than float's exact integer
    // range use two numeric 16-bit limbs, preserving all signed 32-bit values.
    std::vector<float> weights;
    std::vector<std::uint32_t> passes;
    // One entry per scratch image the selected passes write; the executor
    // rejects a selected pass whose scratch output is undeclared, and a
    // declaration whose pass is not selected.
    std::vector<EffectScratchRegion> scratch;
    // Owned, 32-bit-word-aligned geometry payload. Bounded by the device's
    // storage-buffer limit before allocation; never borrowed from the document.
    std::vector<std::byte> geometry;
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
    // Device storage-buffer capacity, supplied by the executor. Preparation can
    // reject an oversized table before host allocation; resource ownership and
    // final admission remain in the executor.
    std::uint32_t maxStorageBufferBytes;
    // The authored format of the network that owns this node (issue #92): the
    // saved composition canvas, resolved once by the shared plan, never the
    // selected viewer/Read. It is NOT automatically the node's runtime
    // coordinate frame: a node whose values are stated against the image it
    // processes (Crop's bottom-left box) converts through its input's described
    // format, which travels in `inputDescriptions`/`description`. Null only for a
    // direct preparation call without a network scope.
    const ImageFormat* owningFormat{nullptr};
    // Immutable document snapshot for contribution-owned subframe geometry.
    const Document* document{nullptr};
    NetworkId network{kInvalidNetwork};
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
