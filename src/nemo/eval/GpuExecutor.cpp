#include "nemo/eval/GpuExecutor.hpp"

#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/eval/EffectShaders.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <numbers>
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

// Prepares one step's effect execution: fills the uniform block the
// effect's declared contract consumes (recording consumed effective
// parameters into `effectiveParams` — plan state, not authored guesses),
// uploads it, binds set 0, and returns the port-ordered input count the
// effect declares. The type dispatch mirrors the CPU reference inventory
// (CpuReference.cpp); values are already typed at the document boundary, so
// execution never reparses parameter text.
// `sourceFrame` supplies the decoded full-resolution frame for `source`
// nodes (issue #11): it binds as set 1 input 0 and its dimensions feed the
// fill kernel through param0. `maskPresent` reports whether the optional
// mask input is connected (issue #34: absent binds a valid dummy descriptor
// and reports coverage 1); `pixelAspect` is inherited from the main input
// layout for the transform's physical-coordinate rotation.
void fillMaskUniforms(const EffectMaskParameters& mask, bool maskPresent, EffectUniforms& uniforms) {
    uniforms.mask[0] = static_cast<float>(mask.channel);
    uniforms.mask[1] = mask.invert ? 1.0F : 0.0F;
    uniforms.mask[2] = mask.mix;
    uniforms.mask[3] = maskPresent ? 1.0F : 0.0F;
}

std::uint32_t prepareEffectStep(const NodeCatalog& catalog, const NodeInstance& node, const EvaluationRequest& request,
                                const EffectProgram& program, bool maskPresent, float pixelAspect,
                                ParameterValues& effectiveParams, EffectUniforms& uniforms,
                                std::vector<ComputeBinding>& bindings, gpu::Buffer& uniformBuffer,
                                gpu::Allocator& allocator, const gpu::Image* sourceFrame = nullptr) {
    uniforms.misc[0] = static_cast<float>(request.localTime);
    // The request region stays FULL-RESOLUTION; the executed raster samples
    // it at samplingScale (issue #11). Both sets of numbers travel in the
    // uniforms so kernels keep full coordinate semantics on any declared
    // representation.
    const int scale = request.samplingScale;
    uniforms.meta[0] = static_cast<std::uint32_t>(request.imageWidth());
    uniforms.meta[1] = static_cast<std::uint32_t>(request.imageHeight());
    uniforms.meta[2] = static_cast<std::uint32_t>(request.region.x);
    uniforms.meta[3] = static_cast<std::uint32_t>(request.region.y);
    uniforms.meta2[0] = static_cast<std::uint32_t>((request.region.width + scale - 1) / scale);
    uniforms.meta2[1] = static_cast<std::uint32_t>((request.region.height + scale - 1) / scale);
    uniforms.meta2[2] = static_cast<std::uint32_t>(scale);
    uniforms.meta2[3] = 0;

    std::uint32_t inputs = 0;
    if (node.type == "source") {
        if (sourceFrame == nullptr) {
            failEffect(node, program, "source fill has no decoded frame (SourceSession did not supply one)");
        }
        const VkExtent3D extent = sourceFrame->extent();
        uniforms.param0[0] = static_cast<float>(extent.width);
        uniforms.param0[1] = static_cast<float>(extent.height);
        effectiveParams.emplace("sourceDimensions",
                                std::string(std::to_string(extent.width) + "x" + std::to_string(extent.height)));
        inputs = 0;
        // The decoded frame is the declared set 1 input of the source fill.
        bindings.push_back({1, 0, DescriptorKind::StorageImage, nullptr, sourceFrame, false});
    } else if (node.type == "testpattern" || node.type == "output") {
        inputs = node.type == "output" ? 1u : 0u;
    } else if (node.type == "constcolor") {
        const std::array<float, 4> color = effectiveColor4(catalog, node, effectiveParams, "color");
        for (int c = 0; c < 4; ++c) {
            uniforms.param0[c] = color[c];
        }
        inputs = 0;
    } else if (node.type == "merge") {
        // Identical operation bookkeeping as the CPU reference: 'over' is
        // the only implemented operation; anything else is a declared
        // limitation, never a silent substitution.
        const auto& operationValue = effectiveParameter(catalog, node, effectiveParams, "operation");
        const auto* operationChoice = std::get_if<ChoiceValue>(&operationValue);
        if (operationChoice == nullptr) {
            failEffect(node, program,
                       "parameter 'operation' must be a choice, got '" + parameterValueText(operationValue) + "'");
        }
        const std::string& operation = operationChoice->value;
        if (operation != "over") {
            failEffect(node, program,
                       "unsupported merge operation '" + operation + "' (this inventory implements 'over' only)");
        }
        inputs = 2;
    } else if (node.type == "grade") {
        // issue #34: per-channel Color parameters plus the enabled-channel
        // bitmask, reverse, and the two clamps; mask controls are common.
        const GradeParameters grade = effectiveGrade(catalog, node, effectiveParams);
        for (int c = 0; c < 4; ++c) {
            uniforms.gradeBlackpoint[c] = grade.blackpoint[c];
            uniforms.gradeWhitepoint[c] = grade.whitepoint[c];
            uniforms.gradeLift[c] = grade.lift[c];
            uniforms.gradeGain[c] = grade.gain[c];
            uniforms.gradeMultiply[c] = grade.multiply[c];
            uniforms.gradeOffset[c] = grade.offset[c];
            uniforms.gradeGamma[c] = grade.gamma[c];
        }
        uniforms.gradeFlags[0] = static_cast<float>(grade.channels);
        uniforms.gradeFlags[1] = grade.reverse ? 1.0F : 0.0F;
        uniforms.gradeFlags[2] = grade.clampBlack ? 1.0F : 0.0F;
        uniforms.gradeFlags[3] = grade.clampWhite ? 1.0F : 0.0F;
        fillMaskUniforms(effectiveEffectMask(catalog, node, effectiveParams), maskPresent, uniforms);
        inputs = 2;
    } else if (node.type == "blur") {
        const BlurParameters blur = effectiveBlur(catalog, node, effectiveParams);
        uniforms.blur[0] = blur.size;
        uniforms.blur[1] = static_cast<float>(blur.channels);
        // Raster support: size is a full-resolution radius, so a reduced
        // raster needs ceil(size/samplingScale) taps per axis.
        uniforms.blur[2] = static_cast<float>(static_cast<int>(std::ceil(blur.size / static_cast<float>(scale))));
        fillMaskUniforms(effectiveEffectMask(catalog, node, effectiveParams), maskPresent, uniforms);
        inputs = 2;
    } else if (node.type == "transform") {
        const TransformParameters transform = effectiveTransform(catalog, node, effectiveParams);
        // The aspect travels from the main input layout; an unrepresentable
        // aspect is a node+parameter error, never a silent fallback.
        if (!std::isfinite(pixelAspect) || pixelAspect <= 0.0F) {
            failEffect(node, program,
                       "transform main input has an invalid pixel aspect (" + std::to_string(pixelAspect) + ")");
        }
        uniforms.transform[0] = transform.translateX;
        uniforms.transform[1] = transform.translateY;
        uniforms.transform[2] = transform.scale;
        uniforms.transform[3] = transform.rotate;
        // Rotation trig is computed once on the host in double precision so
        // exact 90-degree ties stay exact in float; the kernels never
        // evaluate radians/cos/sin per output pixel (issue #34 parity).
        const double angleRadians = static_cast<double>(transform.rotate) * (std::numbers::pi / 180.0);
        uniforms.transformFlags[0] = static_cast<float>(transform.filter);
        uniforms.transformFlags[1] = pixelAspect;
        uniforms.transformFlags[2] = static_cast<float>(std::cos(angleRadians));
        uniforms.transformFlags[3] = static_cast<float>(std::sin(angleRadians));
        fillMaskUniforms(effectiveEffectMask(catalog, node, effectiveParams), maskPresent, uniforms);
        inputs = 2;
    } else {
        failEffect(node, program,
                   "type '" + node.type +
                       "' has no native effect implementation in the supplied "
                       "effect library");
    }

    if (uniformBuffer.handle() == VK_NULL_HANDLE) {
        uniformBuffer = allocator.create_buffer(sizeof(EffectUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                gpu::MemoryPreference::HostMapped);
        std::memcpy(uniformBuffer.mapped(), &uniforms, sizeof(EffectUniforms));
    }
    bindings.push_back({0, 0, DescriptorKind::UniformBuffer, &uniformBuffer, nullptr, false});
    return inputs;
}

// Stable fingerprint of an effect library: the front end (Slang vs GLSL,
// and any source change) must never share reuse keys (issue #9: reuse
// includes implementation identity).
[[nodiscard]] std::uint64_t fingerprintEffectLibrary(const EffectLibrary& effects) {
    std::uint64_t hash = kFnv1a64Basis;
    for (const auto& [type, program] : effects) {
        hashMixText(hash, type);
        hashMix(hash, program.spirv.data(), program.spirv.size() * sizeof(std::uint32_t));
        hashMixText(hash, program.glsl);
        hashMixText(hash, program.sourcePath);
    }
    return hash;
}

}  // namespace

EffectLibrary loadSlangEffectLibrary(const std::filesystem::path& spvDir, const std::filesystem::path& sourceDir) {
    // Node-visible effects plus blur's internal first pass. Every program is
    // loaded through this one library so the native and reference front ends
    // share loading, fingerprinting, and diagnostics (issue #34).
    static const char* kEffects[] = {"testpattern", "constcolor", "merge",          "output",   "source",
                                     "grade",       "blur",       "blurHorizontal", "transform"};
    EffectLibrary library;
    for (const char* type : kEffects) {
        const std::filesystem::path spvPath = spvDir / (std::string(type) + ".spv");
        EffectProgram program;
        try {
            program.spirv = gpu::loadSpirv(spvPath);
        } catch (const gpu::GpuException& error) {
            throw gpu::GpuException(error.errorCode(), std::string("effect '") + type + "': " + error.what());
        }
        program.sourcePath = spvPath.string();
        const std::filesystem::path slangPath = sourceDir / (std::string(type) + ".slang");
        if (!sourceDir.empty() && std::filesystem::exists(slangPath)) {
            program.sourcePath = slangPath.string();
        }
        library.emplace(type, std::move(program));
    }
    return library;
}

EffectLibrary glslEffectLibrary() {
    static const char* kEffects[] = {"testpattern", "constcolor", "merge",          "output",   "source",
                                     "grade",       "blur",       "blurHorizontal", "transform"};
    const char* sources[] = {kGlslTestpattern, kGlslConstcolor, kGlslMerge,          kGlslOutput,   kGlslSource,
                             kGlslGrade,       kGlslBlur,       kGlslBlurHorizontal, kGlslTransform};
    EffectLibrary library;
    for (std::size_t i = 0; i < 9; ++i) {
        EffectProgram program;
        program.glsl = std::string(kGlslPreamble) + sources[i];
        program.sourcePath = "runtime GLSL (glslang), EffectShaders.hpp:" + std::string(kEffects[i]);
        library.emplace(kEffects[i], std::move(program));
    }
    return library;
}

ResultKey queryViewerResultKey(const Document& document, EvaluationRequest request, const EffectLibrary& effects) {
    validateRequest(document, request);
    const auto order = expandDependencies(document, request.network, request.output);
    const KeyContext context{fingerprintEffectLibrary(effects)};
    std::map<EvaluationNodeId, ResultKey> keys;
    ImageIdentity placeholder;
    placeholder.layout.width = scaledDimension(request.region.width, request.samplingScale);
    placeholder.layout.height = scaledDimension(request.region.height, request.samplingScale);
    placeholder.layout.color = ColorInterpretation::SceneLinear;
    placeholder.residency = Residency::GpuDevice;

    for (const ExpandedNode& expandedNode : order) {
        if (!expandedNode.alias && effects.find(expandedNode.node->type) == effects.end()) {
            throw EvaluationException(describeNode(*expandedNode.node) +
                                      ": no effect package in the supplied effect library "
                                      "(viewer key query cannot prove a cache hit)");
        }
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
                                               ResultCache<GpuNodeImage>* reuse, SourceSession* sources) {
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
    // Shader compilation is preparation work on the calling worker; native
    // Slang packages are precompiled. No GPU wait occurs until the complete
    // expanded graph has been recorded into one submission.
    std::map<std::string, std::vector<std::uint32_t>> compiled;
    const EvaluationTicket ticket = reuse != nullptr ? reuse->beginTicket(document) : EvaluationTicket{};
    const KeyContext keyContext{fingerprintEffectLibrary(effects)};
    std::map<EvaluationNodeId, ResultKey> keys;

    GpuEvaluation evaluation;
    // One recorded compute pass and the images it reads/writes. A node maps
    // to one pass, except blur, whose two separable passes both live in this
    // one submission (issue #34: no per-node wait, no readback between).
    struct SubDispatch {
        std::unique_ptr<ComputePass> pass;
        const gpu::Image* output;              // image this pass writes (fresh)
        std::vector<const gpu::Image*> reads;  // images to barrier before record
    };
    struct Dispatch {
        std::vector<SubDispatch> passes;
        std::shared_ptr<const GpuNodeImage> output;
        std::shared_ptr<const gpu::Image> externalInput;  // decoded source frame
        std::shared_ptr<gpu::Image> scratch;              // blur intermediate
    };
    std::vector<Dispatch> dispatches;
    gpu::SubmissionQueue::RetainedResources retained;
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

        std::vector<std::uint64_t> inputKeyHashes;
        inputKeyHashes.reserve(expandedNode.inputs.size());
        std::vector<const gpu::Image*> inputs;      // declared-port aligned; null for absent optional
        std::vector<const gpu::Image*> realInputs;  // connected images only (barrier set)
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
            realInputs.push_back(image);
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

        const auto programIt = effects.find(effectiveNode->type);
        if (programIt == effects.end()) {
            const std::string availability =
                document.network(scopedRequest.network).graph().descriptor(effectiveNode->type) != nullptr
                    ? "declared node type is unavailable to the GPU executor"
                    : "unknown node type";
            failEffect(*effectiveNode, EffectProgram{},
                       availability + " (no effect package in the supplied effect library; no silent substitution)");
        }
        const EffectProgram& program = programIt->second;
        // Connected/absent optional inputs (issue #34): port 0 is the
        // required main image, port 1 the optional mask. An absent mask
        // binds the main image as a valid dummy descriptor with
        // maskPresent=0 (no allocated white fallback).
        const gpu::Image* mainImage = inputs.empty() ? nullptr : inputs[0];
        const gpu::Image* maskImage = inputs.size() > 1 ? inputs[1] : nullptr;
        const bool maskPresent = maskImage != nullptr;
        const gpu::Image* dummyMask = maskPresent ? maskImage : mainImage;

        std::shared_ptr<const gpu::Image> sourceFrame;
        float pixelAspect = 1.0F;
        if (effectiveNode->type == "source") {
            if (sources == nullptr)
                failEffect(*effectiveNode, program, "real-media source node evaluated without a SourceSession");
            const auto sourceParam = effectiveNode->params.find("source");
            if (sourceParam == effectiveNode->params.end())
                failEffect(*effectiveNode, program, "parameter 'source' (the document source key) is required");
            const SourceSession::DecodedFrame decoded =
                sources->acquire(document, scopedRequest.network, *effectiveNode, scopedRequest.localTime,
                                 timeout_ns.value_or(10'000'000'000ULL));
            sourceFrame = std::move(decoded.image);
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

        // Resolves one program's SPIR-V: build-time Slang when precompiled,
        // otherwise runtime GLSL compiled and cached by library key (issue
        // #34: blur needs both of its pass programs through this seam).
        auto spirvFor = [&](const std::string& type, const EffectProgram& effectProgram,
                            const NodeInstance& failingNode) -> const std::vector<std::uint32_t>* {
            if (!effectProgram.glsl.empty()) {
                auto cached = compiled.find(type);
                if (cached == compiled.end()) {
                    try {
                        cached = compiled.emplace(type, gpu::compileGlslToSpirv(effectProgram.glsl)).first;
                    } catch (const gpu::CompileException& error) {
                        failEffect(failingNode, effectProgram, std::string("shader compile failed: ") + error.what());
                    }
                }
                return &cached->second;
            }
            return &effectProgram.spirv;
        };
        const std::vector<std::uint32_t>* spirv = spirvFor(effectiveNode->type, program, *effectiveNode);

        EffectUniforms uniforms{};
        std::vector<ComputeBinding> baseBindings;
        gpu::Buffer uniformBuffer;
        const std::uint32_t inputCount =
            prepareEffectStep(document.network(scopedRequest.network).graph().catalog(), *effectiveNode, scopedRequest,
                              program, maskPresent, pixelAspect, step.effectiveParams, uniforms, baseBindings,
                              uniformBuffer, allocator, sourceFrame ? &*sourceFrame : nullptr);
        if (inputCount != inputs.size())
            failEffect(*effectiveNode, program,
                       "effect declares " + std::to_string(inputCount) + " inputs but the plan wires " +
                           std::to_string(inputs.size()));

        ImageLayout nodeLayout = layout;
        nodeLayout.pixelAspect = pixelAspect;
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

        if (effectiveNode->type == "blur") {
            if (mainImage == nullptr)
                failEffect(*effectiveNode, program, "blur requires a connected main image input");
            const float blurSize = uniforms.blur[0];
            const int support = static_cast<int>(uniforms.blur[2]);
            if (blurSize <= 0.0F) {
                // size 0 is an exact identity: ONE pass of the vertical
                // program (its identity branch never reads the scratch slot)
                // with no scratch raster and no horizontal pass. The scratch
                // descriptor is the main image as a valid dummy and a
                // single-float weight buffer satisfies the pipeline layout.
                const float identityWeight = 1.0F;
                gpu::Buffer weightBuffer = allocator.create_buffer(sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                   gpu::MemoryPreference::HostMapped);
                std::memcpy(weightBuffer.mapped(), &identityWeight, sizeof(float));
                retained.push_back(weightBuffer.retain());

                std::vector<ComputeBinding> bindings = baseBindings;
                bindings.push_back({1, 0, DescriptorKind::StorageImage, nullptr, mainImage, false});
                bindings.push_back({1, 1, DescriptorKind::StorageImage, nullptr, mainImage, false});
                bindings.push_back({1, 2, DescriptorKind::StorageImage, nullptr, dummyMask, false});
                bindings.push_back({2, 0, DescriptorKind::StorageImage, nullptr, &resident->image, false});
                bindings.push_back({3, 0, DescriptorKind::StorageBuffer, &weightBuffer, nullptr, false});
                std::unique_ptr<ComputePass> pass;
                try {
                    pass = ComputePass::create(device, *spirv, bindings);
                } catch (const gpu::GpuException& error) {
                    failEffect(*effectiveNode, program,
                               std::string("blur identity pipeline creation failed: ") + error.what());
                }
                std::vector<const gpu::Image*> reads{mainImage};
                if (maskPresent)
                    reads.push_back(maskImage);
                dispatch.passes.push_back({std::move(pass), &resident->image, std::move(reads)});
            } else {
                // Two separable passes, one submission: pass 1 (horizontal,
                // internal program) main -> retained scratch; pass 2
                // (vertical, the node-visible program) scratch -> output,
                // applying the single mask/mix blend against the original
                // main pixel.
                const auto horizontalIt = effects.find("blurHorizontal");
                if (horizontalIt == effects.end())
                    failEffect(*effectiveNode, program,
                               "internal blur pass 'blurHorizontal' is missing from the supplied effect library");
                const std::vector<std::uint32_t>* horizontalSpirv =
                    spirvFor("blurHorizontal", horizontalIt->second, *effectiveNode);
                try {
                    dispatch.scratch = std::make_shared<gpu::Image>(createEffectImage(allocator, scopedRequest));
                } catch (const gpu::GpuException& error) {
                    failEffect(*effectiveNode, program, std::string("blur scratch allocation failed: ") + error.what());
                }

                // Normalized Gaussian weights, precomputed once per Blur
                // preparation (issue #34 perf: no exp()/normalization per
                // output pixel). size is the full-res support radius,
                // sigma = size/3, and raster sample i sits at full-res
                // offset i*scale.
                std::vector<float> weights(static_cast<std::size_t>(2 * support + 1), 1.0F);
                const double sigma = static_cast<double>(blurSize) / 3.0;
                double total = 0.0;
                for (int i = -support; i <= support; ++i) {
                    const double weight = std::exp(-0.5 * std::pow(static_cast<double>(i * scale) / sigma, 2.0));
                    weights[static_cast<std::size_t>(i + support)] = static_cast<float>(weight);
                    total += weight;
                }
                for (float& weight : weights)
                    weight = static_cast<float>(static_cast<double>(weight) / total);
                gpu::Buffer weightBuffer =
                    allocator.create_buffer(weights.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            gpu::MemoryPreference::HostMapped);
                std::memcpy(weightBuffer.mapped(), weights.data(), weights.size() * sizeof(float));
                retained.push_back(weightBuffer.retain());

                std::vector<ComputeBinding> horizontalBindings = baseBindings;
                horizontalBindings.push_back({1, 0, DescriptorKind::StorageImage, nullptr, mainImage, false});
                horizontalBindings.push_back(
                    {2, 0, DescriptorKind::StorageImage, nullptr, dispatch.scratch.get(), false});
                horizontalBindings.push_back({3, 0, DescriptorKind::StorageBuffer, &weightBuffer, nullptr, false});
                std::unique_ptr<ComputePass> horizontalPass;
                try {
                    horizontalPass = ComputePass::create(device, *horizontalSpirv, horizontalBindings);
                } catch (const gpu::GpuException& error) {
                    failEffect(*effectiveNode, program,
                               std::string("blur horizontal pipeline creation failed: ") + error.what());
                }
                dispatch.passes.push_back({std::move(horizontalPass), dispatch.scratch.get(), {mainImage}});

                std::vector<ComputeBinding> verticalBindings = baseBindings;
                verticalBindings.push_back(
                    {1, 0, DescriptorKind::StorageImage, nullptr, dispatch.scratch.get(), false});
                verticalBindings.push_back({1, 1, DescriptorKind::StorageImage, nullptr, mainImage, false});
                verticalBindings.push_back({1, 2, DescriptorKind::StorageImage, nullptr, dummyMask, false});
                verticalBindings.push_back({2, 0, DescriptorKind::StorageImage, nullptr, &resident->image, false});
                verticalBindings.push_back({3, 0, DescriptorKind::StorageBuffer, &weightBuffer, nullptr, false});
                std::unique_ptr<ComputePass> verticalPass;
                try {
                    verticalPass = ComputePass::create(device, *spirv, verticalBindings);
                } catch (const gpu::GpuException& error) {
                    failEffect(*effectiveNode, program,
                               std::string("blur vertical pipeline creation failed: ") + error.what());
                }
                std::vector<const gpu::Image*> verticalReads{dispatch.scratch.get(), mainImage};
                if (maskPresent)
                    verticalReads.push_back(maskImage);
                dispatch.passes.push_back({std::move(verticalPass), &resident->image, std::move(verticalReads)});
            }
        } else {
            std::vector<ComputeBinding> bindings = baseBindings;
            for (std::uint32_t i = 0; i < inputCount; ++i) {
                const gpu::Image* bound = inputs[i];
                if (bound == nullptr) {
                    if (i == 1 && mainImage != nullptr) {
                        bound = mainImage;  // absent optional mask: valid dummy descriptor
                    } else {
                        failEffect(*effectiveNode, program,
                                   "input port " + std::to_string(i) + " is required but not connected");
                    }
                }
                bindings.push_back({1, i, DescriptorKind::StorageImage, nullptr, bound, false});
            }
            bindings.push_back({2, 0, DescriptorKind::StorageImage, nullptr, &resident->image, false});
            std::unique_ptr<ComputePass> pass;
            try {
                pass = ComputePass::create(device, *spirv, bindings);
            } catch (const gpu::GpuException& error) {
                failEffect(*effectiveNode, program, std::string("pipeline creation failed: ") + error.what());
            }
            dispatch.passes.push_back({std::move(pass), &resident->image, realInputs});
        }

        for (const auto& sub : dispatch.passes)
            retained.push_back(sub.pass->retain());
        if (dispatch.scratch)
            retained.push_back(dispatch.scratch->retain());
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
                                       SourceSession* sources) {
    return executeGpu(document, request, effects, device, allocator, std::nullopt, nullptr, sources);
}

GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                          gpu::Device& device, gpu::Allocator& allocator, std::uint64_t timeout_ns,
                          ResultCache<GpuNodeImage>* reuse, SourceSession* sources) {
    auto evaluation = executeGpu(document, request, effects, device, allocator, timeout_ns, reuse, sources);
    if (!evaluation)
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "GPU submission capacity exhausted");
    return std::move(*evaluation);
}

}  // namespace nemo::eval
