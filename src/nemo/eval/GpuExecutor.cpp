#include "nemo/eval/GpuExecutor.hpp"

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

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

// Representation-sized RGBA32F storage image (ceil(region/scale), issue
// #11), kept in GENERAL for its whole life.
[[nodiscard]] gpu::Image createEffectImage(gpu::Allocator& allocator, const EvaluationRequest& request) {
    const int scale = request.samplingScale;
    return allocator.create_image(static_cast<uint32_t>((request.region.width + scale - 1) / scale),
                                  static_cast<uint32_t>((request.region.height + scale - 1) / scale), 1,
                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 2);
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

void validatePreparation(const NodeInstance& node, const RegisteredGpuEffect& effect,
                         const GpuPreparation& preparation) {
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
}

}  // namespace

ResultKey queryViewerResultKey(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                               std::string_view colorConfigIdentity) {
    validateRequest(document, request);
    const auto order = expandDependencies(document, request.network, request.output);
    KeyContext context{effects.fingerprint(), std::string(colorConfigIdentity)};
    std::map<EvaluationNodeId, ResultKey> keys;

    for (const ExpandedNode& expandedNode : order) {
        if (!expandedNode.alias)
            static_cast<void>(
                effects.require(document.network(expandedNode.id.network).graph().catalog(), *expandedNode.node));
        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expandedNode, resolvedNode, static_cast<double>(request.localTime));
        EvaluationRequest scopedRequest = request;
        scopedRequest.network = expandedNode.id.network;
        std::vector<std::uint64_t> inputHashes;
        for (const auto& producer : expandedNode.inputs) {
            // Absent optional slots keep their declared-port position in the
            // key with the shared absent-input constant, so a missing mask
            // and a connected mask never collide and both executors agree.
            inputHashes.push_back(producer.node == kInvalidNode ? kAbsentInputKeyHash : keys.at(producer).hash);
        }
        const auto key = nodeResultKey(document, *effectiveNode, inputHashes, scopedRequest, context);
        keys.emplace(expandedNode.id, key);
    }
    const EvaluationNodeId outputKey{request.network, kInvalidNetworkInstance, request.output, kEvaluationWholeNode};
    return viewerResultKey(keys.at(outputKey), document.color);
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

    const std::vector<ExpandedNode> order = expandDependencies(document, request.network, request.output);
    auto& queue = device.submissions(device.graphics_family());
    // The immutable library already owns compiled programs. Only per-request
    // payloads, descriptors and image resources are prepared here.
    const EvaluationTicket ticket = reuse != nullptr ? reuse->beginTicket(document) : EvaluationTicket{};
    // The caller-supplied OCIO identity participates in every key (issue
    // #81): a changed config/context can never serve a stale source result.
    KeyContext keyContext{effects.fingerprint(), std::string(colorConfigIdentity)};
    std::map<EvaluationNodeId, ResultKey> keys;

    GpuEvaluation evaluation;
    // All contributed local passes are recorded into one shared submission.
    struct SubDispatch {
        std::unique_ptr<ComputePass> pass;
        const gpu::Image* output;              // image this pass writes (fresh)
        std::vector<const gpu::Image*> reads;  // images to barrier before record
    };
    struct Dispatch {
        std::vector<SubDispatch> passes;
        std::shared_ptr<const GpuNodeImage> output;
        std::shared_ptr<const gpu::Image> externalInput;  // decoded source frame
        std::map<std::uint32_t, std::shared_ptr<gpu::Image>> scratch;
    };
    std::vector<Dispatch> dispatches;
    gpu::SubmissionQueue::RetainedResources retained;
    retained.push_back(effects.retain());
    const auto registrations = effects.contributions();
    gpu::Buffer requestBuffer;
    evaluation.plan.request = request;
    std::map<EvaluationNodeId, ImageIdentity> identities;
    std::map<EvaluationNodeId, std::shared_ptr<const GpuNodeImage>> scopedImages;
    const int scale = request.samplingScale;
    const int imageWidth = (request.region.width + scale - 1) / scale;
    const int imageHeight = (request.region.height + scale - 1) / scale;
    const ImageLayout layout = [&] {
        ImageLayout l;
        l.width = imageWidth;
        l.height = imageHeight;
        return l;
    }();

    for (const ExpandedNode& expandedNode : order) {
        const NodeInstance& node = *expandedNode.node;
        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expandedNode, resolvedNode, static_cast<double>(request.localTime));
        EvaluationRequest scopedRequest = request;
        scopedRequest.network = expandedNode.id.network;
        PlanStep step;
        step.network = expandedNode.id.network;
        step.instance = expandedNode.id.instance;
        step.node = node.id;
        step.outputPort = expandedNode.id.outputPort;
        step.path = expandedNode.id.path;
        step.type = node.type;
        step.name = node.name;
        step.effectiveParams = effectiveNode->params;
        const auto& catalog = document.network(scopedRequest.network).graph().catalog();
        const RegisteredGpuEffect* registered =
            expandedNode.alias ? nullptr : &effects.require(catalog, *effectiveNode);
        if (registered && registered->implementation->payloadSize > device.properties().limits.maxUniformBufferRange)
            failEffect(*effectiveNode, registered->programs.front(),
                       "declared payload exceeds the device uniform-buffer limit");

        std::vector<std::uint64_t> inputKeyHashes;
        inputKeyHashes.reserve(expandedNode.inputs.size());
        std::vector<const gpu::Image*> inputs;  // declared-port aligned; null for absent optional
        inputs.reserve(expandedNode.inputs.size());
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
                continue;
            }
            inputKeyHashes.push_back(keys.at(producer).hash);
            step.inputs.push_back(producer.node);
            step.scopedInputs.push_back(ScopedPlanInput{producer.network, producer.instance, producer.node,
                                                        producer.outputPort, producer.path});
            step.inputImages.push_back(identities.at(producer));
            const gpu::Image* image = &scopedImages.at(producer)->image;
            inputs.push_back(image);
        }
        const ResultKey key = nodeResultKey(document, *effectiveNode, inputKeyHashes, scopedRequest, keyContext);
        keys.emplace(expandedNode.id, key);

        if (reuse != nullptr) {
            if (const auto hit = reuse->find(key);
                hit && hit->identity.layout.width == imageWidth && hit->identity.layout.height == imageHeight) {
                step.produced = hit->identity;
                step.cacheReused = true;
                scopedImages.emplace(expandedNode.id, hit->image);
                identities.emplace(expandedNode.id, step.produced);
                if (expandedNode.id.network == request.network && expandedNode.id.instance == kInvalidNetworkInstance)
                    evaluation.images.emplace(node.id, hit->image);
                evaluation.plan.steps.push_back(std::move(step));
                continue;
            }
        }
        if (expandedNode.alias) {
            const auto aliased = scopedImages.at(*expandedNode.alias);
            step.produced = identities.at(*expandedNode.alias);
            scopedImages.emplace(expandedNode.id, aliased);
            identities.emplace(expandedNode.id, step.produced);
            if (expandedNode.id.network == request.network && expandedNode.id.instance == kInvalidNetworkInstance)
                evaluation.images.emplace(node.id, aliased);
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
        float pixelAspect = 1.0F;
        // Interpretation of the source node's produced image: SceneLinear for a
        // managed source, Data when the request bypassed color conversion. A
        // non-display-referred image is never assumed to be managed
        // scene-linear (issue #81).
        ColorInterpretation sourceColor = ColorInterpretation::SceneLinear;
        if (role == NodeRole::Source) {
            if (sources == nullptr)
                failEffect(*effectiveNode, program, "real-media source node evaluated without a SourceSession");
            const auto sourceParam = effectiveNode->params.find("source");
            if (sourceParam == effectiveNode->params.end())
                failEffect(*effectiveNode, program, "parameter 'source' (the document source key) is required");
            // One resolution owns mapping, coverage, boundary policies and the
            // authored color choices; the session consumes it verbatim (issue
            // #75: no consumer re-derives the effective request).
            const EffectiveSourceRequest sourceRequest =
                resolveSourceRequest(document, *effectiveNode, scopedRequest.localTime);
            auto decoded =
                sources->acquire(document, sourceRequest, scopedRequest, timeout_ns.value_or(10'000'000'000ULL));
            sourceFrame = std::move(decoded.image);
            sourceColor = decoded.color;
            step.effectiveParams.emplace("frame", decoded.frame);
            // The decoded frame's actual pixel aspect drives this node's
            // output and propagates through every downstream node.
            pixelAspect = decoded.pixelAspect;
        }
        // Pixel aspect travels with the main image layout (issue #34): the
        // transform rotates physical coordinates, and consumers see the same
        // propagated aspect in the produced identity.
        if (!expandedNode.inputs.empty() && expandedNode.inputs[0].node != kInvalidNode) {
            pixelAspect = scopedImages.at(expandedNode.inputs[0])->layout.pixelAspect;
        }

        GpuPreparation preparation;
        try {
            preparation = implementation.prepare(
                {catalog, *effectiveNode, scopedRequest, step.effectiveParams, maskPresent, pixelAspect,
                 sourceFrame ? sourceFrame->extent().width : 0, sourceFrame ? sourceFrame->extent().height : 0});
        } catch (const std::exception& error) {
            failEffect(*effectiveNode, program, std::string("local preparation failed: ") + error.what());
        }
        validatePreparation(*effectiveNode, *registered, preparation);
        if (preparation.weights.size() > device.properties().limits.maxStorageBufferRange / sizeof(float))
            failEffect(*effectiveNode, program, "prepared weights exceed the device storage-buffer limit");

        ImageLayout nodeLayout = layout;
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
            resident->image = createEffectImage(allocator, scopedRequest);
        } catch (const gpu::GpuException& error) {
            failEffect(*effectiveNode, program, std::string("output image allocation failed: ") + error.what());
        }

        Dispatch dispatch;
        dispatch.output = resident;
        dispatch.externalInput = std::move(sourceFrame);

        try {
            if (requestBuffer.handle() == VK_NULL_HANDLE) {
                const auto uniforms = requestUniforms(request);
                requestBuffer = allocator.create_buffer(sizeof(uniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                        gpu::MemoryPreference::HostMapped);
                std::memcpy(requestBuffer.mapped(), &uniforms, sizeof(uniforms));
            }
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
                const gpu::Image* result = &resident->image;
                if (definition.output.kind == EffectImageKind::Scratch) {
                    auto image = std::make_shared<gpu::Image>(createEffectImage(allocator, scopedRequest));
                    result = image.get();
                    dispatch.scratch.emplace(definition.output.index, std::move(image));
                }
                std::vector<ComputeBinding> bindings{
                    {0, 0, DescriptorKind::UniformBuffer, &requestBuffer, nullptr, false}};
                if (implementation.payloadSize != 0)
                    bindings.push_back({0, 1, DescriptorKind::UniformBuffer, &payloadBuffer, nullptr, false});
                if (definition.weights)
                    bindings.push_back({3, 0, DescriptorKind::StorageBuffer, &weightBuffer, nullptr, false});
                std::vector<const gpu::Image*> reads;
                reads.reserve(definition.inputs.size());
                for (std::uint32_t binding = 0; binding < definition.inputs.size(); ++binding) {
                    const auto reference = definition.inputs[binding];
                    const gpu::Image* image = nullptr;
                    switch (reference.kind) {
                    case EffectImageKind::Input:
                        image = inputs[reference.index];
                        if (image == nullptr && declaredInputs[reference.index].optional && !inputs.empty())
                            image = inputs[0];
                        break;
                    case EffectImageKind::Scratch:
                        image = dispatch.scratch.at(reference.index).get();
                        break;
                    case EffectImageKind::External:
                        image = dispatch.externalInput.get();
                        break;
                    case EffectImageKind::Output:
                        break;  // Invalid declarations are rejected before publication.
                    }
                    if (image == nullptr)
                        failEffect(*effectiveNode, passProgram,
                                   "local pass '" + definition.id + "' requires unavailable image binding " +
                                       std::to_string(binding));
                    bindings.push_back({1, binding, DescriptorKind::StorageImage, nullptr, image, false});
                    if (reference.kind != EffectImageKind::External &&
                        std::find(reads.begin(), reads.end(), image) == reads.end())
                        reads.push_back(image);
                }
                bindings.push_back({2, 0, DescriptorKind::StorageImage, nullptr, result, false});
                std::unique_ptr<ComputePass> pass;
                try {
                    pass = ComputePass::create(device, *passProgram.spirv, bindings);
                } catch (const gpu::GpuException& error) {
                    failEffect(*effectiveNode, passProgram,
                               "local pass '" + definition.id + "' pipeline creation failed: " + error.what());
                }
                retained.push_back(pass->retain());
                dispatch.passes.push_back({std::move(pass), result, std::move(reads)});
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
        identities.emplace(expandedNode.id, step.produced);
        scopedImages.emplace(expandedNode.id, resident);
        if (expandedNode.id.network == request.network && expandedNode.id.instance == kInvalidNetworkInstance)
            evaluation.images.emplace(node.id, resident);
        evaluation.plan.steps.push_back(std::move(step));
    }

    const EvaluationNodeId outputKey{request.network, kInvalidNetworkInstance, request.output, kEvaluationWholeNode};
    evaluation.plan.result = identities.at(outputKey);
    for (const auto& [id, key] : keys)
        if (id.network == request.network && id.instance == kInvalidNetworkInstance)
            evaluation.keys.emplace(id.node, key);
    if (!dispatches.empty()) {
        const auto completion = queue.submit(
            [&](VkCommandBuffer command) {
                for (const auto& dispatch : dispatches) {
                    if (dispatch.externalInput)
                        afterExternalWriteBeforeRead(command, *dispatch.externalInput);
                    for (const auto& sub : dispatch.passes) {
                        prepareFreshImage(command, *sub.output);
                        for (const auto* input : sub.reads)
                            afterWriteBeforeRead(command, *input);
                        sub.pass->record(command, static_cast<uint32_t>((imageWidth + 7) / 8),
                                         static_cast<uint32_t>((imageHeight + 7) / 8), 1);
                    }
                }
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
    if (reuse != nullptr && timeout_ns) {
        for (const auto& step : evaluation.plan.steps) {
            if (!step.cacheReused) {
                const EvaluationNodeId id{step.network, step.instance, step.node, step.outputPort, step.path};
                reuse->publish(document, ticket, keys.at(id), scopedImages.at(id), step.produced);
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
