#include "nemo/eval/GpuExecutor.hpp"

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include <algorithm>
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

// RGBA32F storage image covering one raster, kept in GENERAL for its whole
// life (spec section 10.4). Transfer usages exist because the returned output
// is cropped out of a padded backing on the device, in the same submission as
// the passes that produced it.
[[nodiscard]] gpu::Image createEffectImage(gpu::Allocator& allocator, const RasterGeometry& raster) {
    return allocator.create_image(
        static_cast<std::uint32_t>(raster.width), static_cast<std::uint32_t>(raster.height), 1,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 2);
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
// is the pass recorded just before it.
void recordRegionCopy(VkCommandBuffer command, const gpu::Image& source, const gpu::Image& destination,
                      std::uint32_t sourceX, std::uint32_t sourceY) {
    gpu::recordImageBarrier(command, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    gpu::recordImageBarrier(command, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffset = VkOffset3D{static_cast<std::int32_t>(sourceX), static_cast<std::int32_t>(sourceY), 0};
    copy.dstOffset = VkOffset3D{0, 0, 0};
    copy.extent = destination.extent();
    vkCmdCopyImage(command, source.handle(), VK_IMAGE_LAYOUT_GENERAL, destination.handle(), VK_IMAGE_LAYOUT_GENERAL, 1,
                   &copy);
    // The copy destination stays GENERAL: later consumers bind it as a storage
    // image, and its writer was a transfer, not a shader.
    gpu::recordImageBarrier(command, destination, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}

// The common request describes the raster the pass about to be recorded
// produces: a node-local scratch pass covers its own declared region, so its
// dispatch extent, pixel-origin mapping and raster size are its own.
EffectRequestUniforms requestUniforms(const EvaluationRequest& request) {
    EffectRequestUniforms result;
    const int scale = request.samplingScale;
    result.meta[0] = static_cast<std::uint32_t>(request.imageWidth());
    result.meta[1] = static_cast<std::uint32_t>(request.imageHeight());
    result.meta[2] = static_cast<std::uint32_t>(request.region.x);
    result.meta[3] = static_cast<std::uint32_t>(request.region.y);
    result.meta2[0] = static_cast<std::uint32_t>(scaledDimension(request.region.width, scale));
    result.meta2[1] = static_cast<std::uint32_t>(scaledDimension(request.region.height, scale));
    result.meta2[2] = static_cast<std::uint32_t>(scale);
    result.misc[0] = static_cast<float>(request.localTime);
    return result;
}

// A pass input as it is actually bound: the image, and the raster it covers.
struct BoundInput {
    const gpu::Image* image{};
    RasterGeometry raster;
};

// A pass declares the geometry block exactly when it samples an Input or
// Scratch image: the External source frame is the full-resolution decoded
// frame and is addressed by full-resolution coordinates, so it carries no
// raster origin.
[[nodiscard]] bool needsInputGeometry(const EffectPassDefinition& pass) {
    return std::any_of(pass.inputs.begin(), pass.inputs.end(), [](const EffectImageRef& input) {
        return input.kind == EffectImageKind::Input || input.kind == EffectImageKind::Scratch;
    });
}

// Set 0 binding 2 content for one pass: one entry per set-1 binding, in
// binding order.
[[nodiscard]] std::vector<EffectInputGeometry> inputGeometryBlock(const NodeInstance& node,
                                                                  const EffectProgram& program,
                                                                  const RasterGeometry& pass,
                                                                  const std::vector<BoundInput>& inputs) {
    std::vector<EffectInputGeometry> block;
    block.reserve(inputs.size());
    for (const BoundInput& input : inputs) {
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
        EffectInputGeometry geometry;
        geometry.regionAndOffset[0] = input.raster.originX;
        geometry.regionAndOffset[1] = input.raster.originY;
        geometry.regionAndOffset[2] = dx / pass.scale;
        geometry.regionAndOffset[3] = dy / pass.scale;
        geometry.extent[0] = static_cast<std::uint32_t>(input.raster.width);
        geometry.extent[1] = static_cast<std::uint32_t>(input.raster.height);
        geometry.extent[2] = static_cast<std::uint32_t>(input.raster.scale);
        geometry.extent[3] = 1u;
        block.push_back(geometry);
    }
    return block;
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

}  // namespace

ResultKey queryViewerResultKey(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                               std::string_view colorConfigIdentity) {
    validateRequest(document, request);
    // Content identity is coverage-independent, and the viewer's key is the
    // normalized request's — independently of which backing rectangle the
    // cache happens to hold (issue #85).
    const EvaluationRequest normalized = canonicalizeRequest(request);
    const auto registrations = effects.contributions();
    const RegionPlan plan = planDependencyRegions(document, normalized, *registrations);
    KeyContext context{effects.fingerprint(), std::string(colorConfigIdentity)};
    std::map<EvaluationNodeId, ResultKey> contentKeys;

    for (const ExpandedNode& expandedNode : plan.order) {
        if (!expandedNode.alias)
            static_cast<void>(
                effects.require(document.network(expandedNode.id.network).graph().catalog(), *expandedNode.node));
        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expandedNode, resolvedNode, static_cast<double>(request.localTime));
        EvaluationRequest scopedRequest = plan.requests.at(expandedNode.id);
        scopedRequest.network = expandedNode.id.network;
        std::vector<std::uint64_t> inputHashes;
        for (const auto& producer : expandedNode.inputs) {
            // Absent optional slots keep their declared-port position in the
            // key with the shared absent-input constant, so a missing mask
            // and a connected mask never collide and both executors agree.
            inputHashes.push_back(producer.node == kInvalidNode ? kAbsentInputKeyHash : contentKeys.at(producer).hash);
        }
        const auto key = nodeContentKey(document, *effectiveNode, inputHashes, scopedRequest, context);
        contentKeys.emplace(expandedNode.id, key);
    }
    const EvaluationNodeId outputKey{normalized.network, kInvalidNetworkInstance, normalized.output,
                                     kEvaluationWholeNode};
    return viewerResultKey(regionResultKey(contentKeys.at(outputKey), normalized), document.color);
}

CpuImage GpuEvaluation::readBack(NodeId node, gpu::Device& device, gpu::Allocator& allocator,
                                 std::uint64_t timeout_ns) {
    const auto it = images.find(node);
    if (it == images.end()) {
        throw EvaluationException("no device-resident image for node " + std::to_string(node));
    }
    const GpuNodeImage& resident = *it->second;
    CpuImage image(resident.layout);
    const std::size_t bytes = static_cast<std::size_t>(resident.layout.width) *
                              static_cast<std::size_t>(resident.layout.height) * kImageChannels * sizeof(float);
    {
        auto& queue = device.submissions(device.graphics_family());
        gpu::downloadImage(queue, allocator, resident.image, image.data(), bytes, timeout_ns);
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
                                               std::string_view colorConfigIdentity) {
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
    KeyContext keyContext{effects.fingerprint(), std::string(colorConfigIdentity)};

    // External media is acquired once per execution, full-frame, before any
    // region planning: the decode's actual pixel aspect is what the planner
    // needs for a tight Transform bound, and the frame itself is what the
    // source pass binds. Acquiring here also means one decode/probe per source
    // regardless of how many representations read it.
    struct AcquiredSource {
        std::shared_ptr<const gpu::Image> frame;
        RasterGeometry raster;
        float pixelAspect{1.0F};
        ColorInterpretation color{ColorInterpretation::SceneLinear};
        std::int64_t frameIndex{0};
    };
    const std::vector<ExpandedNode> order = expandDependencies(document, normalized.network, normalized.output);
    std::map<EvaluationNodeId, AcquiredSource> acquired;
    std::map<EvaluationNodeId, float> pixelAspects;
    for (const ExpandedNode& expandedNode : order) {
        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expandedNode, resolvedNode, static_cast<double>(normalized.localTime));
        if (effectiveNode->definition != kInvalidNetwork)
            continue;
        const auto& catalog = document.network(expandedNode.id.network).graph().catalog();
        const NodeContribution* registration = registrations->find(effectiveNode->type);
        if (registration == nullptr)
            continue;  // the execution loop reports an unregistered node honestly
        const auto& declaredInputs = catalog.inputPorts(effectiveNode->type);
        if (registration->role != NodeRole::Source) {
            // A generator (no declared image input) has square pixels; every
            // other node takes its main input's aspect, which the planner
            // propagates through the plan.
            if (declaredInputs.empty())
                pixelAspects.emplace(expandedNode.id, 1.0F);
            continue;
        }
        if (sources == nullptr || !effectiveNode->params.contains("source")) {
            pixelAspects.emplace(expandedNode.id, 0.0F);  // unknown, never assumed square
            continue;
        }
        EvaluationRequest sourceRequest = normalized;
        sourceRequest.network = expandedNode.id.network;
        sourceRequest.output = effectiveNode->id;
        const EffectiveSourceRequest source = resolveSourceRequest(document, *effectiveNode, sourceRequest.localTime);
        SourceSession::DecodedFrame decoded =
            sources->acquire(document, source, sourceRequest, timeout_ns.value_or(10'000'000'000ULL));
        const auto width = static_cast<int>(decoded.width);
        const auto height = static_cast<int>(decoded.height);
        pixelAspects.emplace(expandedNode.id, decoded.pixelAspect);
        acquired.emplace(expandedNode.id,
                         AcquiredSource{std::move(decoded.image), RasterGeometry{0, 0, width, height, 1},
                                        decoded.pixelAspect, decoded.color, decoded.frame});
    }

    // Dependency-first order plus the actual request of every node: the
    // planner owns the dependency rules (region expansion, aliasing, whole-
    // domain escalation), the executor only renders what it reports.
    const RegionPlan plan = planDependencyRegions(document, normalized, *registrations, pixelAspects);

    GpuEvaluation evaluation;
    evaluation.plan.request = normalized;
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
    const EvaluationNodeId outputKey{normalized.network, kInvalidNetworkInstance, normalized.output,
                                     kEvaluationWholeNode};

    for (const ExpandedNode& expandedNode : plan.order) {
        const NodeInstance& node = *expandedNode.node;
        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expandedNode, resolvedNode, static_cast<double>(normalized.localTime));
        EvaluationRequest nodeRequest = plan.requests.at(expandedNode.id);
        nodeRequest.network = expandedNode.id.network;
        // The domain is explicit on every per-node request: a regional request
        // without it would make imageWidth() mean the ROI width.
        if (nodeRequest.fullWidth == 0)
            nodeRequest.fullWidth = normalized.imageWidth();
        if (nodeRequest.fullHeight == 0)
            nodeRequest.fullHeight = normalized.imageHeight();
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
        for (const EvaluationNodeId& producer : expandedNode.inputs) {
            if (producer.node == kInvalidNode) {
                // Absent optional slot: keep declared-port alignment with an
                // invalid/default entry (issue #34) and the shared absent
                // constant in the content key. No producer key, image
                // identity, or descriptor exists.
                inputKeyHashes.push_back(kAbsentInputKeyHash);
                step.inputs.push_back(kInvalidNode);
                step.scopedInputs.push_back(ScopedPlanInput{});
                step.inputImages.push_back(ImageIdentity{});
                inputs.push_back(nullptr);
                inputRequests.push_back(EvaluationRequest{});
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
        }
        const ResultKey contentKey = nodeContentKey(document, *effectiveNode, inputKeyHashes, nodeRequest, keyContext);
        contentKeys.emplace(expandedNode.id, contentKey);

        if (reuse != nullptr) {
            // A covering resident rectangle of the same content serves the
            // demand in place: no crop of the input, no copy, and the hit's
            // own request is the geometry every consumer must sample.
            if (const auto hit = reuse->findRegion(contentKey, nodeRequest)) {
                EvaluationRequest backing = hit->request;
                backing.network = expandedNode.id.network;
                if (backing.fullWidth == 0)
                    backing.fullWidth = normalized.imageWidth();
                if (backing.fullHeight == 0)
                    backing.fullHeight = normalized.imageHeight();
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
        const auto role = registrations->find(effectiveNode->type)->role;
        const auto& declaredInputs = catalog.inputPorts(effectiveNode->type);
        bool maskPresent = false;
        for (std::size_t i = 0; i < declaredInputs.size(); ++i)
            if (declaredInputs[i].optional && declaredInputs[i].kind == PortKind::Mask)
                maskPresent = inputs[i] != nullptr;

        std::shared_ptr<const gpu::Image> sourceFrame;
        RasterGeometry sourceRaster{};
        float pixelAspect = 1.0F;
        // Interpretation of the source node's produced image: SceneLinear for a
        // managed source, Data when the request bypassed color conversion. A
        // non-display-referred image is never assumed to be managed
        // scene-linear (issue #81).
        ColorInterpretation sourceColor = ColorInterpretation::SceneLinear;
        if (role == NodeRole::Source) {
            if (sources == nullptr)
                failEffect(*effectiveNode, program, "real-media source node evaluated without a SourceSession");
            if (!effectiveNode->params.contains("source"))
                failEffect(*effectiveNode, program, "parameter 'source' (the document source key) is required");
            const auto frame = acquired.find(expandedNode.id);
            if (frame == acquired.end())
                failEffect(*effectiveNode, program,
                           "the source session did not supply a decoded full-resolution frame");
            // The decoded frame was acquired once, before region planning; the
            // same device image is bound here rather than probed again.
            sourceFrame = frame->second.frame;
            sourceRaster = frame->second.raster;
            sourceColor = frame->second.color;
            step.effectiveParams.emplace("frame", frame->second.frameIndex);
            pixelAspect = frame->second.pixelAspect;
        }
        // Pixel aspect travels with the main image layout (issue #34): the
        // transform rotates physical coordinates, and consumers see the same
        // propagated aspect in the produced identity.
        if (!expandedNode.inputs.empty() && expandedNode.inputs[0].node != kInvalidNode) {
            pixelAspect = resolved.at(expandedNode.inputs[0]).image->layout.pixelAspect;
        }

        GpuPreparation preparation;
        try {
            preparation =
                implementation.prepare({catalog, *effectiveNode, nodeRequest, step.effectiveParams, maskPresent,
                                        pixelAspect, sourceFrame ? sourceFrame->extent().width : 0,
                                        sourceFrame ? sourceFrame->extent().height : 0, inputRequests});
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
        if (role == NodeRole::Source) {
            // The source node's produced metadata reports the decoded
            // interpretation truthfully: Data when the request bypassed color
            // conversion, scene-linear otherwise.
            nodeLayout.color = sourceColor;
        }
        auto resident = std::make_shared<GpuNodeImage>();
        resident->layout = nodeLayout;
        try {
            resident->image = createEffectImage(allocator, nodeRaster);
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
                    auto image = std::make_shared<gpu::Image>(createEffectImage(allocator, passRaster));
                    result = image.get();
                    dispatch.scratch.emplace(definition.output.index, std::move(image));
                }
                const auto uniforms = requestUniforms(passRequest);
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
                    switch (reference.kind) {
                    case EffectImageKind::Input: {
                        image = inputs[reference.index];
                        raster = rasterGeometry(*effectiveNode, passProgram, inputRequests[reference.index]);
                        if (image == nullptr && declaredInputs[reference.index].optional && !inputs.empty()) {
                            // An absent optional mask keeps a valid descriptor
                            // (maskPresent = 0): the main image stands in, and
                            // reports its own geometry.
                            image = inputs[0];
                            raster = rasterGeometry(*effectiveNode, passProgram, inputRequests[0]);
                        }
                        break;
                    }
                    case EffectImageKind::Scratch: {
                        const Region& region = scratchRegions.at(reference.index);
                        image = dispatch.scratch.at(reference.index).get();
                        raster = rasterGeometry(*effectiveNode, passProgram, withRegion(nodeRequest, region));
                        break;
                    }
                    case EffectImageKind::External:
                        image = dispatch.externalInput.get();
                        raster = sourceRaster;
                        break;
                    case EffectImageKind::Output:
                        break;  // Invalid declarations are rejected before publication.
                    }
                    if (image == nullptr)
                        failEffect(*effectiveNode, passProgram,
                                   "local pass '" + definition.id + "' requires unavailable image binding " +
                                       std::to_string(binding));
                    bound.push_back(BoundInput{image, raster});
                    if (reference.kind != EffectImageKind::External &&
                        std::find(reads.begin(), reads.end(), image) == reads.end())
                        reads.push_back(image);
                }
                gpu::Buffer geometryBuffer;
                if (needsInputGeometry(definition)) {
                    const auto block = inputGeometryBlock(*effectiveNode, passProgram, passRaster, bound);
                    const auto bytes = block.size() * sizeof(EffectInputGeometry);
                    if (bytes > device.properties().limits.maxStorageBufferRange)
                        failEffect(*effectiveNode, passProgram,
                                   "per-input geometry exceeds the device storage-buffer limit");
                    geometryBuffer = allocator.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                             gpu::MemoryPreference::HostMapped);
                    std::memcpy(geometryBuffer.mapped(), block.data(), bytes);
                    bindings.push_back({0, 2, DescriptorKind::StorageBuffer, &geometryBuffer, nullptr, false});
                }
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
        try {
            consumer->image = createEffectImage(allocator, cropped);
        } catch (const gpu::GpuException& error) {
            throw EvaluationException("final output crop allocation failed for node " +
                                          std::to_string(normalized.output) + ": " + error.what(),
                                      outputNode ? outputNode->id : kInvalidNode,
                                      outputNode ? outputNode->name : std::string{});
        }
        crops.push_back(Crop{&backing.image->image, &consumer->image, static_cast<std::uint32_t>(dx / scale),
                             static_cast<std::uint32_t>(dy / scale)});
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
                    recordRegionCopy(command, *crop.source, *crop.destination, crop.sourceX, crop.sourceY);
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
                                       SourceSession* sources, std::string_view colorConfigIdentity) {
    return executeGpu(document, request, effects, device, allocator, std::nullopt, nullptr, sources,
                      colorConfigIdentity);
}

GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                          gpu::Device& device, gpu::Allocator& allocator, std::uint64_t timeout_ns,
                          ResultCache<GpuNodeImage>* reuse, SourceSession* sources,
                          std::string_view colorConfigIdentity) {
    auto evaluation =
        executeGpu(document, request, effects, device, allocator, timeout_ns, reuse, sources, colorConfigIdentity);
    if (!evaluation)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "GPU submission capacity exhausted");
    return std::move(*evaluation);
}

}  // namespace nemo::eval
