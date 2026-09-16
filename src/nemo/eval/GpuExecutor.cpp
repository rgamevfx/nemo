#include "nemo/eval/GpuExecutor.hpp"

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nemo::eval {

namespace {

using gpu::ComputeBinding;
using gpu::ComputePass;
using gpu::DescriptorKind;
using gpu::SubmissionQueue;

[[noreturn]] void failEffect(const NodeInstance& node, const EffectProgram& program, const std::string& what) {
    std::ostringstream text;
    text << describeNode(node) << ": effect '" << node.type << "' failed: " << what;
    if (!program.sourcePath.empty()) {
        text << " (shader source: " << program.sourcePath << ")";
    }
    throw EvaluationException(std::move(text).str(), node.id, node.name);
}

// Where one actual request's raster sits: its full-resolution region origin and
// its pixel extent at the request's sampling scale. Every request the planner
// produces sits on the global sampling lattice, so two rasters of one
// evaluation differ by an exact whole number of pixels — which is what makes a
// pass able to locate an input by a raster offset instead of a resample.
struct RasterGeometry {
    int originX{};
    int originY{};
    int width{};
    int height{};
    int scale{1};
};

[[nodiscard]] RasterGeometry rasterGeometry(const NodeInstance& node, const EffectProgram& program,
                                            const EvaluationRequest& request) {
    const int scale = request.samplingScale;
    if (scale <= 0 || request.region.x % scale != 0 || request.region.y % scale != 0) {
        failEffect(node, program,
                   "request region (" + std::to_string(request.region.x) + "," + std::to_string(request.region.y) +
                       " " + std::to_string(request.region.width) + "x" + std::to_string(request.region.height) +
                       ") is not on the sampling lattice of scale " + std::to_string(scale));
    }
    return RasterGeometry{request.region.x, request.region.y, scaledDimension(request.region.width, scale),
                          scaledDimension(request.region.height, scale), scale};
}

// One raster's storage: an R32_SFLOAT 2D image holding every named channel as
// a vertical plane (issue #90). Logical pixel (x, y) and channel c live at
// (x, y + c*logicalHeight), so the device extent is (width, height * planeCount)
// while dispatch and every coordinate in the request stay logical. The image
// stays in GENERAL for its whole life (spec section 10.4). Transfer usages exist
// because the returned output is cropped out of a padded backing on the device,
// in the same submission as the passes that produced it.
[[nodiscard]] gpu::Image createEffectImage(gpu::Allocator& allocator, const RasterGeometry& raster,
                                           std::size_t planeCount) {
    return allocator.create_image(
        static_cast<std::uint32_t>(raster.width), static_cast<std::uint32_t>(raster.height * planeCount), 1,
        VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 2);
}

// Refuse a channel-plane raster the device cannot represent, naming the node and
// the actual numbers (issue #90): a described image is never silently degraded,
// truncated or resampled to fit. `planeCount` is at least one for every
// admissible description (a description always names its channels).
void validateRasterDimensions(const NodeInstance& node, const EffectProgram& program, const RasterGeometry& raster,
                              std::size_t planeCount, uint32_t maxDimension) {
    if (raster.width <= 0 || raster.height <= 0 || planeCount == 0)
        failEffect(node, program,
                   "described raster is empty (" + std::to_string(raster.width) + "x" + std::to_string(raster.height) +
                       " with " + std::to_string(planeCount) + " channel planes)");
    const auto rows = static_cast<std::uint64_t>(static_cast<std::uint32_t>(raster.height)) * planeCount;
    if (static_cast<std::uint64_t>(raster.width) > maxDimension || rows > maxDimension)
        failEffect(node, program,
                   "channel-plane raster " + std::to_string(raster.width) + "x" + std::to_string(raster.height) + " (" +
                       std::to_string(planeCount) + " planes -> " + std::to_string(raster.width) + "x" +
                       std::to_string(rows) + " device texels) exceeds the device's 2D image limit " +
                       std::to_string(maxDimension));
}

// Barrier for an image whose producing submission is already COMPLETE but
// was recorded on another wrapper of the graphics queue (decoded source
// frames, issue #11): the generic write→read dependency over ALL_COMMANDS.
void afterExternalWriteBeforeRead(VkCommandBuffer command, const gpu::Image& image) {
    gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void prepareFreshImage(VkCommandBuffer command, const gpu::Image& image) {
    gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT);
}

void afterWriteBeforeRead(VkCommandBuffer command, const gpu::Image& image) {
    gpu::recordImageBarrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

// One final-output crop, recorded into the evaluation's own submission. The
// backing may be a padded miss raster or a wider rectangle served by the
// cache, and may have been written by any earlier submission, so this uses the
// conservative ALL_COMMANDS write dependency rather than assuming the producer
// is the pass recorded just before it. A native raster is a channel-plane image
// (issue #90), so the crop is one copy region per plane: plane c of the
// destination starts at device row c*`destinationPlaneHeight` and is copied
// from plane c of the source at the same crop offset.
void recordRegionCopy(VkCommandBuffer command, const gpu::Image& source, const gpu::Image& destination,
                      std::uint32_t sourceX, std::uint32_t sourceY, std::uint32_t planes,
                      std::uint32_t sourcePlaneHeight, std::uint32_t destinationPlaneHeight, std::uint32_t width,
                      std::uint32_t height) {
    gpu::recordImageBarrier(command, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    gpu::recordImageBarrier(command, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    std::vector<VkImageCopy> copies(planes);
    for (std::uint32_t plane = 0; plane < planes; ++plane) {
        VkImageCopy& copy = copies[plane];
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = VkOffset3D{static_cast<std::int32_t>(sourceX),
                                    static_cast<std::int32_t>(sourceY + plane * sourcePlaneHeight), 0};
        copy.dstOffset = VkOffset3D{0, static_cast<std::int32_t>(plane * destinationPlaneHeight), 0};
        copy.extent = VkExtent3D{width, height, 1};
    }
    vkCmdCopyImage(command, source.handle(), VK_IMAGE_LAYOUT_GENERAL, destination.handle(), VK_IMAGE_LAYOUT_GENERAL,
                   static_cast<std::uint32_t>(copies.size()), copies.data());
    // The copy destination stays GENERAL: later consumers bind it as a storage
    // image, and its writer was a transfer, not a shader.
    gpu::recordImageBarrier(command, destination, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}

// The half-open raster-index rectangle of the samples that ARE the produced
// image's data (issue #88), for the pass that writes a node's own output. A
// sample at raster index (x, y) is data exactly when its full-resolution anchor
// `(region.x + x*scale, region.y + y*scale)` lies inside `dataBounds` — half-open
// integer intervals, the same rule the CPU reference guard applies, so the two
// executors agree on which samples are data. An empty data window yields an
// empty rectangle (a fully transparent raster), and a sample inside the data
// bounds but outside the display format is kept (overscan is data, not black).
// Coverage, origin and sampling scale are never changed by this.
[[nodiscard]] std::array<std::int32_t, 4> rasterSupport(const EvaluationRequest& request, const Region& dataBounds) {
    const int scale = request.samplingScale;
    if (dataBounds.width <= 0 || dataBounds.height <= 0 || scale <= 0)
        return {0, 0, 0, 0};
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    };
    const auto ceilDiv = [&floorDiv](int value, int divisor) { return floorDiv(value + divisor - 1, divisor); };
    const int width = scaledDimension(request.region.width, scale);
    const int height = scaledDimension(request.region.height, scale);
    const int left = std::clamp(ceilDiv(dataBounds.x - request.region.x, scale), 0, width);
    const int right = std::clamp(ceilDiv(dataBounds.x + dataBounds.width - request.region.x, scale), 0, width);
    const int top = std::clamp(ceilDiv(dataBounds.y - request.region.y, scale), 0, height);
    const int bottom = std::clamp(ceilDiv(dataBounds.y + dataBounds.height - request.region.y, scale), 0, height);
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

// The common request describes the raster the pass about to be recorded
// produces: a node-local scratch pass covers its own declared region, so its
// dispatch extent, pixel-origin mapping and raster size are its own. Region
// origins are signed (issue #88): a described output's coverage may extend
// outside `[0, format)`. `support` is the data rectangle of the produced raster:
// the node's own declared support for the pass that writes the node output, or
// "no support" for a node-local scratch raster. `planeCount`/`rgba` are the
// same raster's channel-plane facts (issue #90), resolved here from the
// described channel names so no kernel ever looks a name up per pixel.
EffectRequestUniforms requestUniforms(const EvaluationRequest& request, const std::array<std::int32_t, 4>& support,
                                      std::uint32_t planeCount, const std::array<int, 4>& rgbaRoles) {
    EffectRequestUniforms result;
    const int scale = request.samplingScale;
    result.meta[0] = static_cast<std::int32_t>(request.imageWidth());
    result.meta[1] = static_cast<std::int32_t>(request.imageHeight());
    result.meta[2] = static_cast<std::int32_t>(request.region.x);
    result.meta[3] = static_cast<std::int32_t>(request.region.y);
    result.meta2[0] = static_cast<std::uint32_t>(scaledDimension(request.region.width, scale));
    result.meta2[1] = static_cast<std::uint32_t>(scaledDimension(request.region.height, scale));
    result.meta2[2] = static_cast<std::uint32_t>(scale);
    result.misc[0] = static_cast<float>(request.localTime);
    std::copy(support.begin(), support.end(), result.support);
    result.channels[0] = planeCount;
    std::copy(rgbaRoles.begin(), rgbaRoles.end(), result.rgba);
    return result;
}

// A pass input as it is actually bound: the image, the raster it covers,
// whether it is addressed by absolute full-resolution coordinates instead of by
// a lattice-relative offset (the External decoded frame, issue #88), and the
// named channels those plane indices refer to (issue #90).
struct BoundInput {
    const gpu::Image* image{};
    RasterGeometry raster;
    bool fullResolution{false};
    std::span<const std::string> channels;
};

// Set 0 binding 2 content for one pass: one entry per set-1 binding, in
// binding order. The block is bound for every pass — a geometry word is the
// only way a kernel locates an input — so a pass with no sampled input still
// uploads its (unread) first entry rather than leaving the binding undefined.
[[nodiscard]] std::vector<EffectInputGeometry> inputGeometryBlock(const NodeInstance& node,
                                                                  const EffectProgram& program,
                                                                  const RasterGeometry& pass,
                                                                  const std::vector<BoundInput>& inputs) {
    std::vector<EffectInputGeometry> block;
    block.reserve(std::max<std::size_t>(inputs.size(), 1));
    for (const BoundInput& input : inputs) {
        EffectInputGeometry geometry;
        if (input.fullResolution) {
            // External media keeps full-resolution coordinate semantics: the
            // raster is located by its own absolute origin and extent, never by
            // a relative offset and never rescaled by a fill ratio. Its
            // sampling scale is 1 by construction.
            geometry.regionAndOffset[0] = input.raster.originX;
            geometry.regionAndOffset[1] = input.raster.originY;
            geometry.regionAndOffset[2] = 0;
            geometry.regionAndOffset[3] = 0;
            geometry.extent[0] = static_cast<std::uint32_t>(input.raster.width);
            geometry.extent[1] = static_cast<std::uint32_t>(input.raster.height);
            geometry.extent[2] = 1u;
            geometry.extent[3] = 1u;
        } else {
            if (input.raster.scale != pass.scale) {
                failEffect(node, program,
                           "pass input is sampled at scale " + std::to_string(input.raster.scale) +
                               " but the pass output is at scale " + std::to_string(pass.scale));
            }
            const int dx = pass.originX - input.raster.originX;
            const int dy = pass.originY - input.raster.originY;
            if (dx % pass.scale != 0 || dy % pass.scale != 0) {
                failEffect(node, program, "pass input raster origin is not on the pass output's sampling lattice");
            }
            geometry.regionAndOffset[0] = input.raster.originX;
            geometry.regionAndOffset[1] = input.raster.originY;
            geometry.regionAndOffset[2] = dx / pass.scale;
            geometry.regionAndOffset[3] = dy / pass.scale;
            geometry.extent[0] = static_cast<std::uint32_t>(input.raster.width);
            geometry.extent[1] = static_cast<std::uint32_t>(input.raster.height);
            geometry.extent[2] = static_cast<std::uint32_t>(input.raster.scale);
            geometry.extent[3] = 1u;
        }
        // The input's own channel facts, resolved once (issue #90): which plane
        // holds each R/G/B/A role, and how many planes it carries. A device
        // image holds `extent.x` x `extent.y * channels.x` texels.
        const std::array<int, 4> roles = rgbaChannelIndices(input.channels);
        std::copy(roles.begin(), roles.end(), geometry.rgba);
        geometry.channels[0] = static_cast<std::uint32_t>(input.channels.size());
        block.push_back(geometry);
    }
    if (block.empty())
        block.push_back(EffectInputGeometry{});
    return block;
}

// The produced raster's channel plan (issue #90): one entry per plane. The
// pass's own RGBA math writes the roles its `rgba` word names; every other plane
// keeps the same-named channel of the pass's set-1 binding 0 image at UNCHANGED
// coordinates, or is numeric zero when that image has no such channel. That is
// the shared auxiliary-channel preservation: an effect that selects only RGB (or
// only alpha) never drops the named channels it did not touch, and no per-effect
// rule is involved. A node that owns its channel layout (Shuffle) is the
// exception: its kernel produces every plane itself, so its plan is all
// "produced by this pass".
//
// A node whose local pass has no sampled image input has nothing to preserve
// from: a described output channel it neither writes nor can copy is refused
// here, naming the channel, instead of leaving undefined device memory in a
// published result.
[[nodiscard]] std::vector<EffectChannelPlanEntry> channelPlanFor(const NodeInstance& node, const EffectProgram& program,
                                                                 const std::vector<std::string>& outputChannels,
                                                                 const std::array<int, 4>& outputRoles,
                                                                 std::span<const std::string> sourceChannels,
                                                                 bool ownsChannelLayout) {
    std::vector<EffectChannelPlanEntry> plan(outputChannels.size());
    if (ownsChannelLayout)
        return plan;  // every entry stays -1: this node's kernel produces every plane
    for (std::size_t plane = 0; plane < outputChannels.size(); ++plane) {
        if (std::find(outputRoles.begin(), outputRoles.end(), static_cast<int>(plane)) != outputRoles.end())
            continue;  // the pass's own RGBA math writes this plane
        if (sourceChannels.empty())
            failEffect(node, program,
                       "the node's local pass binds no sampled image to preserve its output channel '" +
                           outputChannels[plane] + "' from");
        const int source = channelIndex(sourceChannels, outputChannels[plane]);
        plan[plane].sourcePlane = source >= 0 ? source : -2;  // -2: no source channel -> numeric zero
    }
    return plan;
}

// Validates one preparation and returns its node-local scratch coverage, keyed
// by scratch index. A selected pass that writes scratch must declare its
// coverage, a declaration without a producer is rejected, and every declared
// region must be non-empty and on the node request's sampling lattice — the
// executor never guesses a raster for a scratch image.
[[nodiscard]] std::map<std::uint32_t, Region> validatePreparation(const NodeInstance& node,
                                                                  const RegisteredGpuEffect& effect,
                                                                  const GpuPreparation& preparation,
                                                                  const EvaluationRequest& request) {
    const auto& implementation = *effect.implementation;
    const auto& program = effect.programs.front();
    if (preparation.payload.size() != implementation.payloadSize)
        failEffect(node, program,
                   "prepared payload does not match declared layout '" + implementation.payloadLayout + "'");
    if (preparation.passes.empty())
        failEffect(node, program, "preparation selected no local passes");
    std::set<std::uint32_t> scratch;
    std::set<std::uint32_t> selected;
    bool output = false;
    bool weights = false;
    for (const auto index : preparation.passes) {
        if (index >= implementation.passes.size() || !selected.insert(index).second)
            failEffect(node, program, "preparation selected an invalid or repeated local pass");
        const auto& pass = implementation.passes[index];
        if (output)
            failEffect(node, program, "local pass '" + pass.id + "' follows the final output pass");
        for (const auto& input : pass.inputs)
            if (input.kind == EffectImageKind::Scratch && !scratch.contains(input.index))
                failEffect(node, effect.programs[index], "local pass '" + pass.id + "' reads unproduced scratch");
        if (pass.output.kind == EffectImageKind::Output)
            output = true;
        else if (!scratch.insert(pass.output.index).second)
            failEffect(node, effect.programs[index], "local pass '" + pass.id + "' overwrites scratch");
        weights = weights || pass.weights;
    }
    if (!output)
        failEffect(node, program, "selected local passes do not produce the node output");
    if (weights != !preparation.weights.empty())
        failEffect(node, program, "prepared weight buffer conflicts with the selected local pass bindings");
    if (std::any_of(preparation.weights.begin(), preparation.weights.end(),
                    [](float value) { return !std::isfinite(value); }))
        failEffect(node, program, "prepared weight buffer contains nonfinite values");

    std::map<std::uint32_t, Region> coverage;
    for (const EffectScratchRegion& declared : preparation.scratch) {
        if (!coverage.emplace(declared.index, declared.region).second)
            failEffect(node, program,
                       "preparation declares scratch " + std::to_string(declared.index) + " more than once");
        if (!scratch.contains(declared.index))
            failEffect(node, program,
                       "preparation declares coverage for unproduced scratch " + std::to_string(declared.index));
        const Region& region = declared.region;
        if (region.width <= 0 || region.height <= 0)
            failEffect(node, program, "scratch " + std::to_string(declared.index) + " has an empty coverage");
        if (region.x % request.samplingScale != 0 || region.y % request.samplingScale != 0)
            failEffect(node, program,
                       "scratch " + std::to_string(declared.index) +
                           " coverage is not on the node request's sampling lattice");
    }
    for (const std::uint32_t index : scratch) {
        if (!coverage.contains(index))
            failEffect(node, program, "preparation does not declare the coverage of scratch " + std::to_string(index));
    }
    return coverage;
}

[[nodiscard]] EvaluationRequest withRegion(EvaluationRequest request, const Region& region) {
    request.region = region;
    return request;
}

// A caller-supplied prebuilt plan (issue #88) is trusted only when it provably
// resolves THIS call: the same document snapshot, the same registration, and
// exactly this canonical demand — region, domain, time, quality, channels and
// sampling scale included, because two requests can agree on network/time/scale
// and still address different pixels.
//
// A mismatching plan is REFUSED, never silently replanned: a caller that hands
// over a plan and a request that disagree has a bug, and quietly resolving a
// second state would hide it while making the caller's cache key and the
// executed pixels describe different things.
void verifySuppliedPlan(const RegionPlan& plan, const Document& document, const NodeContributions& contributions,
                        const EvaluationRequest& normalized) {
    const auto refuse = [](const std::string& detail) {
        throw EvaluationException("supplied region plan does not resolve this request: " + detail);
    };
    if (plan.document != &document)
        refuse("it was planned from a different document object");
    if (plan.documentRevision != document.stateRevision())
        refuse("the document changed since it was planned (revision " + std::to_string(plan.documentRevision) + " vs " +
               std::to_string(document.stateRevision()) + ")");
    if (plan.contributions != &contributions)
        refuse("it was planned with a different node registration");
    if (!(plan.demand == normalized))
        refuse("the planned demand differs from the request");
    if (plan.images.order.empty() || plan.requests.empty())
        refuse("it carries no scheduled nodes");
}

// The resolved plan this call consumes: a verified caller plan when one is
// supplied, otherwise one planned right here.
[[nodiscard]] const RegionPlan& resolvedPlan(const Document& document, const EvaluationRequest& normalized,
                                             const NodeContributions& contributions, SourceDescriptionProvider* sources,
                                             const RegionPlan* supplied, RegionPlan& owned) {
    if (supplied != nullptr) {
        verifySuppliedPlan(*supplied, document, contributions, normalized);
        return *supplied;
    }
    owned = planDependencyRegions(document, normalized, contributions, sources);
    return owned;
}

}  // namespace

ResultKey queryViewerResultKey(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                               std::string_view colorConfigIdentity, SourceDescriptionProvider* sources,
                               const RegionPlan* plan) {
    validateRequest(document, request);
    // Content identity is coverage-independent, and the viewer's key is the
    // normalized request's — independently of which backing rectangle the
    // cache happens to hold (issue #85).
    const EvaluationRequest normalized = canonicalizeRequest(request);
    const auto registrations = effects.contributions();
    // The SAME described plan the executor consumes (issue #88): the query
    // resolves every node's effective state, description and source request
    // through the shared planner, so a cache lookup never acquires a pixel and
    // both executors key one authored state identically. With `sources` a
    // real-media graph is described from the media's own metadata; without one
    // a graph that needs a source description fails honestly here, before any
    // work is scheduled. A caller that already built this plan for this exact
    // demand (the viewer render path) hands it over, so key and execution share
    // one resolution instead of resolving the same state twice.
    const EvaluationNodeId outputKey{normalized.network, kInvalidNetworkInstance, normalized.output,
                                     kEvaluationWholeNode};
    RegionPlan owned;
    const RegionPlan& resolved = resolvedPlan(document, normalized, *registrations, sources, plan, owned);
    const KeyContext base{effects.fingerprint(), std::string(colorConfigIdentity)};
    std::map<EvaluationNodeId, ResultKey> contentKeys;

    for (const ExpandedNode& expandedNode : resolved.images.order) {
        const ResolvedImageNode& resolvedNode = resolved.images.nodes.at(expandedNode.id);
        if (!expandedNode.alias)
            static_cast<void>(
                effects.require(document.network(expandedNode.id.network).graph().catalog(), resolvedNode.node));
        // The node's own described state participates in its key: a changed
        // logical format, data bounds, aspect, channels or interpretation is a
        // different result, and a source's pre-resolved media request is used
        // as-is instead of being re-derived.
        KeyContext context = base;
        context.description = &resolvedNode.description;
        context.source = resolvedNode.source ? &*resolvedNode.source : nullptr;
        EvaluationRequest scopedRequest = resolved.requests.at(expandedNode.id);
        scopedRequest.network = expandedNode.id.network;
        std::vector<std::uint64_t> inputHashes;
        for (const auto& producer : expandedNode.inputs) {
            // Absent optional slots keep their declared-port position in the
            // key with the shared absent-input constant, so a missing mask
            // and a connected mask never collide and both executors agree.
            inputHashes.push_back(producer.node == kInvalidNode ? kAbsentInputKeyHash : contentKeys.at(producer).hash);
        }
        const auto key = nodeContentKey(document, resolvedNode.node, inputHashes, scopedRequest, context);
        contentKeys.emplace(expandedNode.id, key);
    }
    return viewerResultKey(regionResultKey(contentKeys.at(outputKey), normalized), document.color);
}

CpuImage GpuEvaluation::readBack(NodeId node, gpu::Device& device, gpu::Allocator& allocator,
                                 std::uint64_t timeout_ns) {
    const auto it = images.find(node);
    if (it == images.end()) {
        throw EvaluationException("no device-resident image for node " + std::to_string(node));
    }
    const GpuNodeImage& resident = *it->second;
    const auto width = static_cast<std::size_t>(resident.layout.width);
    const auto height = static_cast<std::size_t>(resident.layout.height);
    const std::size_t planes = resident.layout.channels.size();
    CpuImage image(resident.layout);
    // The device raster is the channel-plane image (issue #90): plane c occupies
    // device rows [c*height, (c+1)*height), so the download is interleaved here,
    // by channel index, into the reference image's own channel order. No role is
    // projected or invented: this is the exact stored value of every named
    // channel.
    const std::size_t bytes = width * height * planes * sizeof(float);
    std::vector<float> planesData(width * height * planes);
    {
        auto& queue = device.submissions(device.graphics_family());
        gpu::downloadImage(queue, allocator, resident.image, planesData.data(), bytes, timeout_ns);
    }
    for (std::size_t plane = 0; plane < planes; ++plane) {
        const float* source = planesData.data() + plane * width * height;
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                image.setChannel(static_cast<int>(x), static_cast<int>(y), static_cast<int>(plane),
                                 source[y * width + x]);
            }
        }
    }
    // The diagnostic readback also establishes content identity for the
    // plan (residency stays GpuDevice; the hash is computed host-side from
    // the downloaded values). Device-resident results are never hashed on
    // the routine path.
    const std::uint64_t hash = cpuImageHash(image);
    for (PlanStep& step : plan.steps) {
        if (step.node == node) {
            step.produced.contentHash = hash;
        }
    }
    if (plan.request.output == node) {
        plan.result.contentHash = hash;
    }
    return image;
}

static std::optional<GpuEvaluation> executeGpu(const Document& document, EvaluationRequest request,
                                               const EffectLibrary& effects, gpu::Device& device,
                                               gpu::Allocator& allocator, std::optional<std::uint64_t> timeout_ns,
                                               ResultCache<GpuNodeImage>* reuse, SourceSession* sources,
                                               std::string_view colorConfigIdentity, const RegionPlan* suppliedPlan) {
    validateRequest(document, request);
    if (request.samplingScale != 1 && request.samplingScale != 2 && request.samplingScale != 4) {
        throw EvaluationException("samplingScale " + std::to_string(request.samplingScale) +
                                  " is not supported (declared scales: 1, 2, 4)");
    }
    if (device.features().shaderStorageImageReadWithoutFormat == VK_FALSE ||
        device.features().shaderStorageImageWriteWithoutFormat == VK_FALSE) {
        throw gpu::GpuException(gpu::GpuError::NoDevice,
                                "device does not support storage images with unknown format "
                                "(shaderStorageImageRead/WriteWithoutFormat); native effect execution requires it");
    }

    const EvaluationRequest normalized = canonicalizeRequest(request);
    auto& queue = device.submissions(device.graphics_family());
    const auto registrations = effects.contributions();
    // The immutable library already owns compiled programs. Only per-request
    // payloads, descriptors and image resources are prepared here.
    const EvaluationTicket ticket = reuse != nullptr ? reuse->beginTicket(document) : EvaluationTicket{};
    // The caller-supplied OCIO identity participates in every key (issue
    // #81): a changed config/context can never serve a stale source result.
    const KeyContext keyContext{effects.fingerprint(), std::string(colorConfigIdentity)};

    // Dependency-first order, every node's once-resolved effective state, its
    // described output and (for a Source node) its once-resolved media request,
    // plus the actual request of every node. NO PIXEL is touched here (issue
    // #88): the planner describes real media through the source description
    // provider instead of decoding a frame to learn its geometry, so a
    // metadata-only plan costs no decode, and a node whose result is reused
    // never acquires media at all. A caller that already built this plan for
    // this exact demand (the viewer render path, whose cache key came from it)
    // hands it over, so one render resolves its authored state ONCE: the same
    // nodes, descriptions and source requests key the lookup and are then
    // executed.
    const EvaluationNodeId outputKey{normalized.network, kInvalidNetworkInstance, normalized.output,
                                     kEvaluationWholeNode};
    RegionPlan plannedHere;
    const RegionPlan& plan = resolvedPlan(document, normalized, *registrations, sources, suppliedPlan, plannedHere);

    GpuEvaluation evaluation;
    evaluation.plan.request = normalized;
    // The target's described output is the plan's own result description
    // (issue #88): the caller reads the actual format/data bounds of what it
    // received instead of re-deriving them from the request.
    evaluation.plan.description = plan.images.nodes.at(outputKey).description;
    // All contributed local passes are recorded into one shared submission.
    struct SubDispatch {
        std::unique_ptr<ComputePass> pass;
        const gpu::Image* output;              // image this pass writes (fresh)
        RasterGeometry raster;                 // dispatch extent of that image
        std::vector<const gpu::Image*> reads;  // images to barrier before record
    };
    struct Dispatch {
        std::vector<SubDispatch> passes;
        std::shared_ptr<const GpuNodeImage> output;
        std::shared_ptr<const gpu::Image> externalInput;  // decoded source frame
        std::map<std::uint32_t, std::shared_ptr<gpu::Image>> scratch;
    };
    // The final output's crop: the consumer receives exactly the normalized
    // request rectangle, copied from its backing in this same submission.
    struct Crop {
        const gpu::Image* source{};
        const gpu::Image* destination{};
        std::uint32_t sourceX{};
        std::uint32_t sourceY{};
        std::uint32_t planes{};
        std::uint32_t sourcePlaneHeight{};
        std::uint32_t destinationPlaneHeight{};
        std::uint32_t width{};
        std::uint32_t height{};
    };
    struct ResolvedResult {
        std::shared_ptr<const GpuNodeImage> image;
        EvaluationRequest request;  // actual coverage backing `image`
        ImageIdentity identity;
        bool reused{false};
    };
    std::vector<Dispatch> dispatches;
    std::vector<Crop> crops;
    gpu::SubmissionQueue::RetainedResources retained;
    retained.push_back(effects.retain());
    std::map<EvaluationNodeId, ResultKey> contentKeys;
    std::map<EvaluationNodeId, ResolvedResult> resolved;
    std::optional<std::size_t> outputStep;
    const NodeInstance* outputNode = nullptr;

    for (const ExpandedNode& expandedNode : plan.images.order) {
        // Every node's effective state was resolved exactly once by the shared
        // planner (issue #88): the executor never re-resolves animation,
        // instance overrides or defaults during execution, so what was planned
        // is what is executed.
        const ResolvedImageNode& planNode = plan.images.nodes.at(expandedNode.id);
        const NodeInstance& node = planNode.node;
        const NodeInstance* effectiveNode = &planNode.node;
        EvaluationRequest nodeRequest = plan.requests.at(expandedNode.id);
        nodeRequest.network = expandedNode.id.network;
        // The domain and coverage are the planner's: every per-node request
        // already carries that node's own described format as its domain and a
        // lattice-aligned coverage, which may be signed and may exceed the
        // format. Nothing here re-derives or clips it.
        PlanStep step;
        step.network = expandedNode.id.network;
        step.instance = expandedNode.id.instance;
        step.node = node.id;
        step.outputPort = expandedNode.id.outputPort;
        step.path = expandedNode.id.path;
        step.type = node.type;
        step.name = node.name;
        step.effectiveParams = effectiveNode->params;
        step.region = nodeRequest.region;
        step.description = planNode.description;
        if (expandedNode.id == outputKey) {
            outputStep = evaluation.plan.steps.size();
            outputNode = &node;
        }
        const auto& catalog = document.network(nodeRequest.network).graph().catalog();
        const RegisteredGpuEffect* registered =
            expandedNode.alias ? nullptr : &effects.require(catalog, *effectiveNode);
        if (registered && registered->implementation->payloadSize > device.properties().limits.maxUniformBufferRange)
            failEffect(*effectiveNode, registered->programs.front(),
                       "declared payload exceeds the device uniform-buffer limit");

        std::vector<std::uint64_t> inputKeyHashes;
        inputKeyHashes.reserve(expandedNode.inputs.size());
        std::vector<const gpu::Image*> inputs;  // declared-port aligned; null for absent optional
        inputs.reserve(expandedNode.inputs.size());
        std::vector<EvaluationRequest> inputRequests;  // declared-port aligned; default for absent
        inputRequests.reserve(expandedNode.inputs.size());
        std::vector<const ImageDescription*> inputDescriptions;  // declared-port aligned; null for absent
        inputDescriptions.reserve(expandedNode.inputs.size());
        for (const EvaluationNodeId& producer : expandedNode.inputs) {
            if (producer.node == kInvalidNode) {
                // Absent optional slot: keep declared-port alignment with an
                // invalid/default entry (issue #34) and the shared absent
                // constant in the content key. No producer key, image
                // identity, description, or descriptor exists.
                inputKeyHashes.push_back(kAbsentInputKeyHash);
                step.inputs.push_back(kInvalidNode);
                step.scopedInputs.push_back(ScopedPlanInput{});
                step.inputImages.push_back(ImageIdentity{});
                inputs.push_back(nullptr);
                inputRequests.push_back(EvaluationRequest{});
                inputDescriptions.push_back(nullptr);
                continue;
            }
            // Content identity of the input, never its coverage: a wider
            // backing rectangle does not make the pixels a different result.
            inputKeyHashes.push_back(contentKeys.at(producer).hash);
            step.inputs.push_back(producer.node);
            step.scopedInputs.push_back(ScopedPlanInput{producer.network, producer.instance, producer.node,
                                                        producer.outputPort, producer.path});
            const auto& input = resolved.at(producer);
            step.inputImages.push_back(input.identity);
            inputs.push_back(&input.image->image);
            inputRequests.push_back(input.request);
            inputDescriptions.push_back(&plan.images.nodes.at(producer).description);
        }
        // The node's own described state participates in its key, so a changed
        // format, data bounds, aspect, channels or interpretation is a
        // different result and a source's pre-resolved media request is used
        // as-is rather than re-derived.
        KeyContext nodeKeyContext = keyContext;
        nodeKeyContext.description = &planNode.description;
        nodeKeyContext.source = planNode.source ? &*planNode.source : nullptr;
        const ResultKey contentKey =
            nodeContentKey(document, *effectiveNode, inputKeyHashes, nodeRequest, nodeKeyContext);
        contentKeys.emplace(expandedNode.id, contentKey);

        if (reuse != nullptr) {
            // A covering resident rectangle of the same content serves the
            // demand in place: no crop of the input, no copy, and the hit's
            // own request is the geometry every consumer must sample.
            if (const auto hit = reuse->findRegion(contentKey, nodeRequest)) {
                EvaluationRequest backing = hit->request;
                backing.network = expandedNode.id.network;
                resolved.emplace(expandedNode.id, ResolvedResult{hit->image, backing, hit->identity, true});
                step.produced = hit->identity;
                step.region = backing.region;
                step.cacheReused = true;
                if (expandedNode.id.network == normalized.network &&
                    expandedNode.id.instance == kInvalidNetworkInstance)
                    evaluation.images.emplace(node.id, hit->image);
                evaluation.plan.steps.push_back(std::move(step));
                continue;
            }
        }
        if (expandedNode.alias) {
            const auto aliased = resolved.at(*expandedNode.alias);
            resolved.emplace(expandedNode.id, aliased);
            step.produced = aliased.identity;
            step.region = aliased.request.region;
            if (expandedNode.id.network == normalized.network && expandedNode.id.instance == kInvalidNetworkInstance)
                evaluation.images.emplace(node.id, aliased.image);
            evaluation.plan.steps.push_back(std::move(step));
            continue;
        }

        const auto& implementation = *registered->implementation;
        const auto& program = registered->programs.front();
        const NodeContribution& contribution = *registrations->find(effectiveNode->type);
        const auto role = contribution.role;
        // A node that owns its channel layout (issue #90: Shuffle) produces
        // every plane of its output itself; the executor's common auxiliary
        // preservation must not run for it, or an authored mapping would be
        // overwritten by the source channel of the same name.
        const bool ownsChannelLayout = contribution.ownsChannelLayout;
        const auto& declaredInputs = catalog.inputPorts(effectiveNode->type);
        bool maskPresent = false;
        for (std::size_t i = 0; i < declaredInputs.size(); ++i)
            if (declaredInputs[i].optional && declaredInputs[i].kind == PortKind::Mask)
                maskPresent = inputs[i] != nullptr;

        // The node's described output state and every semantic fact derived
        // from it come from the plan (issue #88): geometry, pixel aspect,
        // channels and interpretation are described once, so native and CPU
        // execution agree on the authored state and neither re-derives it from
        // the request's domain (which may legitimately differ from the format,
        // e.g. for a description with an empty format).
        const ImageDescription& description = planNode.description;
        const float pixelAspect = description.pixelAspect;
        // The node's described channels, resolved once (issue #90): the output
        // raster's plane count and the plane index of each R/G/B/A role. The
        // roles are what the node's own arithmetic reads and writes; every other
        // plane is preserved by the executor's channel plan.
        const auto outputPlaneCount = static_cast<std::uint32_t>(description.channels.size());
        const std::array<int, 4> outputRoles = rgbaChannelIndices(description.channels);

        // External media is acquired HERE, after the shared plan described the
        // node and after the reuse lookup declined to serve it: a metadata plan
        // and a reused result both cost zero pixels (issue #88). The decoded
        // raster's ACTUAL coverage — not 0 and not the request region, and
        // never a fill ratio — is the geometry the source pass is bound with.
        std::shared_ptr<const gpu::Image> sourceFrame;
        RasterGeometry sourceRaster{};
        // The decoded frame's named channels, in plane order (issue #90): the
        // source kernel maps frame plane c to output plane c, and the channel
        // plan resolves an auxiliary output channel against these names. Owned
        // here because the decoded frame's own description ends with this block.
        std::vector<std::string> sourceFrameChannels;
        if (role == NodeRole::Source) {
            if (sources == nullptr)
                failEffect(*effectiveNode, program, "real-media source node evaluated without a SourceSession");
            if (!planNode.source)
                failEffect(*effectiveNode, program,
                           "the shared plan did not resolve this source node's media request (parameter 'source' "
                           "is required, or the source description provider is missing)");
            SourceSession::DecodedFrame decoded;
            try {
                decoded = sources->acquire(document, *planNode.source, timeout_ns.value_or(10'000'000'000ULL));
            } catch (const EvaluationException& error) {
                // A provider failure is reported with the offending node, unless
                // it already names one; a device-level failure keeps its own
                // type so callers can still tell Vulkan failures apart.
                if (error.hasNode())
                    throw;
                failEffect(*effectiveNode, program, "decoding source media failed: " + std::string(error.what()));
            } catch (const media::MediaDecodeError& error) {
                throw media::MediaDecodeError(error.clip, error.format,
                                              describeNode(*effectiveNode) + ": " + error.reason);
            } catch (const gpu::GpuException&) {
                throw;
            } catch (const std::exception& error) {
                failEffect(*effectiveNode, program, "decoding source media failed: " + std::string(error.what()));
            }
            if (decoded.coverage.width <= 0 || decoded.coverage.height <= 0)
                failEffect(*effectiveNode, program,
                           "the source session returned a decoded frame without an actual raster coverage");
            if (decoded.description.channels.empty())
                failEffect(*effectiveNode, program,
                           "the source session returned a decoded frame with no named channels");
            const std::uint64_t frameRows = decoded.image != nullptr ? decoded.image->extent().height : 0;
            if (decoded.image == nullptr || decoded.image->format() != VK_FORMAT_R32_SFLOAT ||
                decoded.image->dimensions() != 2 ||
                decoded.image->extent().width != static_cast<std::uint64_t>(decoded.coverage.width) ||
                frameRows % static_cast<std::uint64_t>(decoded.coverage.height) != 0 ||
                frameRows / static_cast<std::uint64_t>(decoded.coverage.height) == 0)
                failEffect(*effectiveNode, program,
                           "the decoded frame is not an R32_SFLOAT channel-plane image of " +
                               std::to_string(decoded.coverage.width) + "x" + std::to_string(decoded.coverage.height) +
                               " with whole channel planes");
            // The frame's plane count is its PHYSICAL height divided by its
            // logical height (issue #90), never the described channel count: a
            // policy-cleared frame is one retained transparent sample whose
            // description still names the whole source. Truncating the names to
            // the planes that actually exist keeps every resolved index a real
            // plane, so a name only the description carries reads as zero.
            const auto sourceFramePlanes =
                static_cast<std::size_t>(frameRows / static_cast<std::uint64_t>(decoded.coverage.height));
            sourceFrameChannels = decoded.description.channels;
            sourceFrameChannels.resize(std::min(sourceFrameChannels.size(), sourceFramePlanes));
            sourceFrame = std::move(decoded.image);
            sourceRaster = RasterGeometry{decoded.coverage.x, decoded.coverage.y, decoded.coverage.width,
                                          decoded.coverage.height, 1};
            step.effectiveParams.emplace("frame", decoded.frame);
        }

        GpuPreparation preparation;
        try {
            preparation = implementation.prepare({catalog, *effectiveNode, nodeRequest, node.params, maskPresent,
                                                  pixelAspect, inputRequests, description,
                                                  planNode.source ? &*planNode.source : nullptr, inputDescriptions});
        } catch (const std::exception& error) {
            failEffect(*effectiveNode, program, std::string("local preparation failed: ") + error.what());
        }
        const std::map<std::uint32_t, Region> scratchRegions =
            validatePreparation(*effectiveNode, *registered, preparation, nodeRequest);
        if (preparation.weights.size() > device.properties().limits.maxStorageBufferRange / sizeof(float))
            failEffect(*effectiveNode, program, "prepared weights exceed the device storage-buffer limit");

        const RasterGeometry nodeRaster = rasterGeometry(*effectiveNode, program, nodeRequest);
        ImageLayout nodeLayout;
        nodeLayout.width = nodeRaster.width;
        nodeLayout.height = nodeRaster.height;
        nodeLayout.pixelAspect = pixelAspect;
        nodeLayout.channels = description.channels;
        nodeLayout.precision = description.precision;
        // The produced raster reports the described interpretation, so a Data
        // source (or anything downstream of one) is never relabelled
        // scene-linear and a display-referred result is never transformed
        // twice.
        nodeLayout.color = description.color;
        auto resident = std::make_shared<GpuNodeImage>();
        resident->layout = nodeLayout;
        validateRasterDimensions(*effectiveNode, program, nodeRaster, outputPlaneCount,
                                 device.properties().limits.maxImageDimension2D);
        try {
            resident->image = createEffectImage(allocator, nodeRaster, outputPlaneCount);
        } catch (const gpu::GpuException& error) {
            failEffect(*effectiveNode, program, std::string("output image allocation failed: ") + error.what());
        }

        Dispatch dispatch;
        dispatch.output = resident;
        dispatch.externalInput = std::move(sourceFrame);

        try {
            gpu::Buffer payloadBuffer;
            if (!preparation.payload.empty()) {
                payloadBuffer = allocator.create_buffer(preparation.payload.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                        gpu::MemoryPreference::HostMapped);
                std::memcpy(payloadBuffer.mapped(), preparation.payload.data(), preparation.payload.size());
            }
            gpu::Buffer weightBuffer;
            if (!preparation.weights.empty()) {
                const auto bytes = preparation.weights.size() * sizeof(float);
                weightBuffer = allocator.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                       gpu::MemoryPreference::HostMapped);
                std::memcpy(weightBuffer.mapped(), preparation.weights.data(), bytes);
            }
            for (const auto passIndex : preparation.passes) {
                const auto& definition = implementation.passes[passIndex];
                const auto& passProgram = registered->programs[passIndex];
                // The pass output: the node's own request, or a scratch raster
                // whose coverage the node declared.
                EvaluationRequest passRequest = nodeRequest;
                RasterGeometry passRaster = nodeRaster;
                const gpu::Image* result = &resident->image;
                if (definition.output.kind == EffectImageKind::Scratch) {
                    passRequest = withRegion(nodeRequest, scratchRegions.at(definition.output.index));
                    passRaster = rasterGeometry(*effectiveNode, passProgram, passRequest);
                    // A scratch raster carries the node's own channel planes: the
                    // pass that reads it is the same node, sampling the same
                    // named channels.
                    validateRasterDimensions(*effectiveNode, passProgram, passRaster, outputPlaneCount,
                                             device.properties().limits.maxImageDimension2D);
                    auto image =
                        std::make_shared<gpu::Image>(createEffectImage(allocator, passRaster, outputPlaneCount));
                    result = image.get();
                    dispatch.scratch.emplace(definition.output.index, std::move(image));
                }
                // The produced raster carries its own support: the pass that
                // writes the node's output is masked to the node's described
                // data window, so a generator or an offsetting effect can never
                // claim data outside it. A scratch raster has no declared
                // support — only this node's next pass reads it.
                const std::array<std::int32_t, 4> support = definition.output.kind == EffectImageKind::Output
                                                                ? rasterSupport(passRequest, description.dataBounds)
                                                                : std::array<std::int32_t, 4>{-1, -1, -1, -1};
                const auto uniforms = requestUniforms(passRequest, support, outputPlaneCount, outputRoles);
                gpu::Buffer requestBuffer = allocator.create_buffer(
                    sizeof(uniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, gpu::MemoryPreference::HostMapped);
                std::memcpy(requestBuffer.mapped(), &uniforms, sizeof(uniforms));
                std::vector<ComputeBinding> bindings{
                    {0, 0, DescriptorKind::UniformBuffer, &requestBuffer, nullptr, false}};
                if (implementation.payloadSize != 0)
                    bindings.push_back({0, 1, DescriptorKind::UniformBuffer, &payloadBuffer, nullptr, false});
                std::vector<BoundInput> bound;
                std::vector<const gpu::Image*> reads;
                bound.reserve(definition.inputs.size());
                reads.reserve(definition.inputs.size());
                for (std::uint32_t binding = 0; binding < definition.inputs.size(); ++binding) {
                    const auto reference = definition.inputs[binding];
                    const gpu::Image* image = nullptr;
                    RasterGeometry raster{};
                    bool fullResolution = false;
                    std::span<const std::string> channels;
                    switch (reference.kind) {
                    case EffectImageKind::Input: {
                        image = inputs[reference.index];
                        raster = rasterGeometry(*effectiveNode, passProgram, inputRequests[reference.index]);
                        // An absent optional slot has no description: its
                        // channels come from the main image the executor binds
                        // as its dummy descriptor below.
                        if (inputDescriptions[reference.index] != nullptr)
                            channels = inputDescriptions[reference.index]->channels;
                        if (image == nullptr && declaredInputs[reference.index].optional && !inputs.empty()) {
                            // An absent optional mask keeps a valid descriptor
                            // (maskPresent = 0): the main image stands in, and
                            // reports its own geometry and channels.
                            image = inputs[0];
                            raster = rasterGeometry(*effectiveNode, passProgram, inputRequests[0]);
                            channels = inputDescriptions[0]->channels;
                        }
                        break;
                    }
                    case EffectImageKind::Scratch: {
                        const Region& region = scratchRegions.at(reference.index);
                        image = dispatch.scratch.at(reference.index).get();
                        raster = rasterGeometry(*effectiveNode, passProgram, withRegion(nodeRequest, region));
                        // A scratch raster is allocated with the node's own
                        // described channels, so its plane order is theirs.
                        channels = description.channels;
                        break;
                    }
                    case EffectImageKind::External: {
                        // The decoded frame is full resolution and is addressed
                        // by absolute image coordinates: its geometry entry is
                        // its own actual coverage (issue #88), which is neither
                        // the request's rectangle nor a fill-ratio resample.
                        image = dispatch.externalInput.get();
                        raster = sourceRaster;
                        fullResolution = true;
                        channels = sourceFrameChannels;
                        break;
                    }
                    case EffectImageKind::Output:
                        break;  // Invalid declarations are rejected before publication.
                    }
                    if (image == nullptr) {
                        if (reference.kind == EffectImageKind::External && planNode.source == std::nullopt)
                            failEffect(*effectiveNode, passProgram,
                                       "local pass '" + definition.id +
                                           "' binds decoded media but parameter 'source' (the document source key) "
                                           "is absent");
                        failEffect(*effectiveNode, passProgram,
                                   "local pass '" + definition.id + "' requires unavailable image binding " +
                                       std::to_string(binding));
                    }
                    bound.push_back(BoundInput{image, raster, fullResolution, channels});
                    if (reference.kind != EffectImageKind::External &&
                        std::find(reads.begin(), reads.end(), image) == reads.end())
                        reads.push_back(image);
                }
                // Per-input geometry: bound for every pass, because a sampled
                // input is located through it (and the channel plan's source is
                // binding 0's raster).
                const auto block = inputGeometryBlock(*effectiveNode, passProgram, passRaster, bound);
                const auto geometryBytes = block.size() * sizeof(EffectInputGeometry);
                if (geometryBytes > device.properties().limits.maxStorageBufferRange)
                    failEffect(*effectiveNode, passProgram,
                               "per-input geometry exceeds the device storage-buffer limit");
                gpu::Buffer geometryBuffer = allocator.create_buffer(geometryBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                     gpu::MemoryPreference::HostMapped);
                std::memcpy(geometryBuffer.mapped(), block.data(), geometryBytes);
                bindings.push_back({0, 2, DescriptorKind::StorageBuffer, &geometryBuffer, nullptr, false});
                // The produced raster's channel plan, resolved from the named
                // channels of this pass's binding-0 image (issues #90): every
                // plane the pass's RGBA math does not write is preserved from
                // the same-named channel at unchanged coordinates, or zeroed.
                const std::span<const std::string> planSource =
                    bound.empty() ? std::span<const std::string>{} : bound.front().channels;
                const auto plan = channelPlanFor(*effectiveNode, passProgram, description.channels, outputRoles,
                                                 planSource, ownsChannelLayout);
                const auto planBytes = plan.size() * sizeof(EffectChannelPlanEntry);
                if (planBytes > device.properties().limits.maxStorageBufferRange)
                    failEffect(*effectiveNode, passProgram, "channel plan exceeds the device storage-buffer limit");
                gpu::Buffer planBuffer =
                    allocator.create_buffer(std::max<std::size_t>(planBytes, sizeof(EffectChannelPlanEntry)),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gpu::MemoryPreference::HostMapped);
                auto* planEntries = static_cast<EffectChannelPlanEntry*>(planBuffer.mapped());
                const std::size_t planCapacity = planBuffer.size() / sizeof(EffectChannelPlanEntry);
                for (std::size_t i = 0; i < planCapacity; ++i)
                    planEntries[i] = EffectChannelPlanEntry{};  // -1: produced by this pass's own math
                if (!plan.empty())
                    std::memcpy(planBuffer.mapped(), plan.data(), planBytes);
                bindings.push_back({0, 3, DescriptorKind::StorageBuffer, &planBuffer, nullptr, false});
                if (definition.weights)
                    bindings.push_back({3, 0, DescriptorKind::StorageBuffer, &weightBuffer, nullptr, false});
                for (std::uint32_t binding = 0; binding < bound.size(); ++binding)
                    bindings.push_back(
                        {1, binding, DescriptorKind::StorageImage, nullptr, bound[binding].image, false});
                bindings.push_back({2, 0, DescriptorKind::StorageImage, nullptr, result, false});
                std::unique_ptr<ComputePass> pass;
                try {
                    pass = ComputePass::create(device, *passProgram.spirv, bindings);
                } catch (const gpu::GpuException& error) {
                    failEffect(*effectiveNode, passProgram,
                               "local pass '" + definition.id + "' pipeline creation failed: " + error.what());
                }
                retained.push_back(pass->retain());
                dispatch.passes.push_back({std::move(pass), result, passRaster, std::move(reads)});
            }
        } catch (const gpu::GpuException& error) {
            failEffect(*effectiveNode, program, std::string("pass resource preparation failed: ") + error.what());
        }
        if (dispatch.externalInput)
            retained.push_back(dispatch.externalInput);
        dispatches.push_back(std::move(dispatch));

        step.produced.contentHash = 0;
        step.produced.layout = nodeLayout;
        step.produced.residency = Residency::GpuDevice;
        resolved.emplace(expandedNode.id, ResolvedResult{resident, nodeRequest, step.produced, false});
        if (expandedNode.id.network == normalized.network && expandedNode.id.instance == kInvalidNetworkInstance)
            evaluation.images.emplace(node.id, resident);
        evaluation.plan.steps.push_back(std::move(step));
    }

    // The consumer's result is the normalized request, not whatever backing
    // rectangle served it: a padded miss raster or a wider cache entry is
    // cropped on the device inside this submission, while the backing stays
    // published (and retained) under its own coverage so the next overlapping
    // request is still served from it.
    const ResolvedResult backing = resolved.at(outputKey);
    ImageIdentity consumerIdentity = backing.identity;
    std::shared_ptr<const GpuNodeImage> consumerImage = backing.image;
    if (backing.request.region != normalized.region) {
        const int scale = normalized.samplingScale;
        const int dx = normalized.region.x - backing.request.region.x;
        const int dy = normalized.region.y - backing.request.region.y;
        if (dx < 0 || dy < 0 || dx % scale != 0 || dy % scale != 0) {
            throw EvaluationException("normalized request rectangle does not sit inside the backing rectangle of "
                                      "node " +
                                          std::to_string(normalized.output),
                                      outputNode ? outputNode->id : kInvalidNode,
                                      outputNode ? outputNode->name : std::string{});
        }
        const RasterGeometry cropped{normalized.region.x, normalized.region.y,
                                     scaledDimension(normalized.region.width, scale),
                                     scaledDimension(normalized.region.height, scale), scale};
        auto consumer = std::make_shared<GpuNodeImage>();
        consumer->layout = backing.image->layout;
        consumer->layout.width = cropped.width;
        consumer->layout.height = cropped.height;
        const auto planes = static_cast<std::uint32_t>(consumer->layout.channels.size());
        try {
            consumer->image = createEffectImage(allocator, cropped, planes);
        } catch (const gpu::GpuException& error) {
            throw EvaluationException("final output crop allocation failed for node " +
                                          std::to_string(normalized.output) + ": " + error.what(),
                                      outputNode ? outputNode->id : kInvalidNode,
                                      outputNode ? outputNode->name : std::string{});
        }
        crops.push_back(Crop{
            &backing.image->image, &consumer->image, static_cast<std::uint32_t>(dx / scale),
            static_cast<std::uint32_t>(dy / scale), planes, static_cast<std::uint32_t>(backing.image->layout.height),
            static_cast<std::uint32_t>(consumer->layout.height), static_cast<std::uint32_t>(consumer->layout.width),
            static_cast<std::uint32_t>(consumer->layout.height)});
        retained.push_back(backing.image->image.retain());
        retained.push_back(consumer->image.retain());
        consumerImage = std::move(consumer);
        consumerIdentity.layout = consumerImage->layout;
        evaluation.images[normalized.output] = consumerImage;
    }
    evaluation.plan.result = consumerIdentity;
    if (outputStep) {
        // The output step reports what the caller receives: the normalized
        // rectangle, even when a padded or wider backing served it.
        evaluation.plan.steps[*outputStep].region = normalized.region;
        evaluation.plan.steps[*outputStep].produced = consumerIdentity;
    }
    // The caller's key for the returned result is the normalized request's:
    // downstream viewer caches address the representation they asked for, not
    // the backing rectangle this evaluation happened to hold.
    for (const auto& [id, key] : contentKeys) {
        if (id.network == normalized.network && id.instance == kInvalidNetworkInstance) {
            const EvaluationRequest& keyed = id == outputKey ? normalized : resolved.at(id).request;
            evaluation.keys.emplace(id.node, regionResultKey(key, keyed));
        }
    }

    if (!dispatches.empty() || !crops.empty()) {
        const auto completion = queue.submit(
            [&](VkCommandBuffer command) {
                for (const auto& dispatch : dispatches) {
                    if (dispatch.externalInput)
                        afterExternalWriteBeforeRead(command, *dispatch.externalInput);
                    for (const auto& sub : dispatch.passes) {
                        prepareFreshImage(command, *sub.output);
                        for (const auto* input : sub.reads)
                            afterWriteBeforeRead(command, *input);
                        sub.pass->record(command, static_cast<uint32_t>((sub.raster.width + 7) / 8),
                                         static_cast<uint32_t>((sub.raster.height + 7) / 8), 1);
                    }
                }
                for (const auto& crop : crops)
                    recordRegionCopy(command, *crop.source, *crop.destination, crop.sourceX, crop.sourceY, crop.planes,
                                     crop.sourcePlaneHeight, crop.destinationPlaneHeight, crop.width, crop.height);
            },
            std::move(retained), {}, timeout_ns.value_or(0));
        if (!completion)
            return std::nullopt;
        evaluation.completion = completion;
        if (timeout_ns && !queue.wait(*completion, *timeout_ns)) {
            throw gpu::GpuException(gpu::GpuError::SubmissionTimeout,
                                    "GPU evaluation did not complete within " + std::to_string(*timeout_ns) + " ns");
        }
    }
    // Only completed work enters the existing cache. The async interface
    // leaves publication to its consumer after checking completion/freshness.
    // Every entry keeps its own actual coverage, so an overlapping request is
    // served from it without a copy.
    if (reuse != nullptr && timeout_ns) {
        for (const auto& [id, result] : resolved) {
            if (!result.reused) {
                reuse->publishRegion(document, ticket, contentKeys.at(id), result.request, result.image,
                                     result.identity);
            }
        }
    }
    return evaluation;
}

std::optional<GpuEvaluation> submitGpu(const Document& document, EvaluationRequest request,
                                       const EffectLibrary& effects, gpu::Device& device, gpu::Allocator& allocator,
                                       SourceSession* sources, std::string_view colorConfigIdentity,
                                       const RegionPlan* plan) {
    return executeGpu(document, request, effects, device, allocator, std::nullopt, nullptr, sources,
                      colorConfigIdentity, plan);
}

GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                          gpu::Device& device, gpu::Allocator& allocator, std::uint64_t timeout_ns,
                          ResultCache<GpuNodeImage>* reuse, SourceSession* sources,
                          std::string_view colorConfigIdentity, const RegionPlan* plan) {
    auto evaluation = executeGpu(document, request, effects, device, allocator, timeout_ns, reuse, sources,
                                 colorConfigIdentity, plan);
    if (!evaluation)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "GPU submission capacity exhausted");
    return std::move(*evaluation);
}

}  // namespace nemo::eval
