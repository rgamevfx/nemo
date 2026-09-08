#include "nemo/eval/GpuExecutor.hpp"

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/eval/EffectShaders.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Submit.hpp"
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace nemo::eval {

namespace {

using gpu::ComputeBinding;
using gpu::ComputePass;
using gpu::DescriptorKind;
using gpu::SubmissionQueue;

[[noreturn]] void failEffect(const Node& node, const EffectProgram& program, const std::string& what) {
    std::ostringstream text;
    text << describeNode(node) << ": effect '" << node.type << "' failed: " << what;
    if (!program.sourcePath.empty()) {
        text << " (shader source: " << program.sourcePath << ")";
    }
    throw EvaluationException(std::move(text).str(), node.id, node.name);
}

// Region-sized RGBA32F storage image, kept in GENERAL for its whole life.
[[nodiscard]] gpu::Image createEffectImage(gpu::Allocator& allocator, const EvaluationRequest& request) {
    return allocator.create_image(static_cast<uint32_t>(request.region.width),
                                  static_cast<uint32_t>(request.region.height), 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

void prepareFreshImage(SubmissionQueue& queue, const gpu::Image& image, std::uint64_t timeout_ns) {
    // UNDEFINED → GENERAL: first use is a storage write by the next dispatch.
    gpu::imageBarrier(queue, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT, timeout_ns);
}

void afterWriteBeforeRead(SubmissionQueue& queue, const gpu::Image& image, std::uint64_t timeout_ns) {
    // The declared write→read dependency between dependent passes (spec
    // section 10.4): the previous dispatch's shader writes are visible to
    // the next dispatch's image reads.
    gpu::imageBarrier(queue, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, timeout_ns);
}

// Prepares one step's effect execution: fills the uniform block the
// effect's declared contract consumes (recording consumed effective
// parameters into `effectiveParams` — plan state, not authored guesses),
// uploads it, binds set 0, and returns the port-ordered input count the
// effect declares. The type dispatch mirrors the CPU reference inventory
// (CpuReference.cpp); the per-type param parsing is shared with it via
// parseColor4 so both executors resolve identical effective state.
std::uint32_t prepareEffectStep(const Node& node, const EvaluationRequest& request, const EffectProgram& program,
                                std::map<std::string, std::string>& effectiveParams, EffectUniforms& uniforms,
                                std::vector<ComputeBinding>& bindings, gpu::Buffer& uniformBuffer,
                                gpu::Allocator& allocator) {
    uniforms.misc[0] = static_cast<float>(request.localTime);
    uniforms.meta[0] = static_cast<std::uint32_t>(request.region.width);
    uniforms.meta[1] = static_cast<std::uint32_t>(request.region.height);
    uniforms.meta[2] = static_cast<std::uint32_t>(request.region.x);
    uniforms.meta[3] = static_cast<std::uint32_t>(request.region.y);

    std::uint32_t inputs = 0;
    if (node.type == "testpattern" || node.type == "output") {
        inputs = node.type == "output" ? 1u : 0u;
    } else if (node.type == "constcolor") {
        const std::array<float, 4> color = parseColor4(node, effectiveParams, "color", {1.0F, 1.0F, 1.0F, 1.0F});
        for (int c = 0; c < 4; ++c) {
            uniforms.param0[c] = color[c];
        }
        inputs = 0;
    } else if (node.type == "merge") {
        // Identical operation bookkeeping as the CPU reference: 'over' is
        // the only implemented operation; anything else is a declared
        // limitation, never a silent substitution.
        const std::string operation = effectiveParams.count("operation") > 0 ? effectiveParams.at("operation") : [&] {
            effectiveParams.emplace("operation", "over");
            return std::string{"over"};
        }();
        if (operation != "over") {
            failEffect(node, program,
                       "unsupported merge operation '" + operation + "' (this inventory implements 'over' only)");
        }
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

}  // namespace

EffectLibrary loadSlangEffectLibrary(const std::filesystem::path& spvDir, const std::filesystem::path& sourceDir) {
    static const char* kEffects[] = {"testpattern", "constcolor", "merge", "output"};
    EffectLibrary library;
    for (const char* type : kEffects) {
        const std::filesystem::path spvPath = spvDir / (std::string(type) + ".spv");
        std::ifstream in(spvPath, std::ios::binary);
        if (!in) {
            throw gpu::GpuException(gpu::GpuError::InvalidRequest,
                                    std::string("effect '") + type + "': no compiled Slang kernel at " +
                                        spvPath.string() +
                                        " (build the nemo_shaders target: configure with -D NEMO_DOWNLOAD_SLANGC=ON, "
                                        "install slangc, or set NEMO_SLANGC)");
        }
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() < 4 || bytes.size() % sizeof(std::uint32_t) != 0 ||
            std::memcmp(bytes.data(), "\x03\x02#\x07", 4) != 0) {
            throw gpu::GpuException(gpu::GpuError::InvalidRequest, std::string("effect '") + type + "': " +
                                                                       spvPath.string() + " is not a SPIR-V module");
        }
        EffectProgram program;
        program.spirv.assign(reinterpret_cast<const std::uint32_t*>(bytes.data()),
                             reinterpret_cast<const std::uint32_t*>(bytes.data()) + bytes.size() / 4);
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
    static const char* kEffects[] = {"testpattern", "constcolor", "merge", "output"};
    const char* sources[] = {kGlslTestpattern, kGlslConstcolor, kGlslMerge, kGlslOutput};
    EffectLibrary library;
    for (std::size_t i = 0; i < 4; ++i) {
        EffectProgram program;
        program.glsl = std::string(kGlslPreamble) + sources[i];
        program.sourcePath = "runtime GLSL (glslang), EffectShaders.hpp:" + std::string(kEffects[i]);
        library.emplace(kEffects[i], std::move(program));
    }
    return library;
}

CpuImage GpuEvaluation::readBack(NodeId node, gpu::Device& device, gpu::Allocator& allocator,
                                 std::uint64_t timeout_ns) {
    const auto it = images.find(node);
    if (it == images.end()) {
        throw EvaluationException("no device-resident image for node " + std::to_string(node));
    }
    const GpuNodeImage& resident = it->second;
    CpuImage image(resident.layout);
    const std::size_t bytes = static_cast<std::size_t>(resident.layout.width) *
                              static_cast<std::size_t>(resident.layout.height) * kImageChannels * sizeof(float);
    {
        SubmissionQueue queue(device, device.graphics_family());
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

GpuEvaluation evaluateGpu(const Document& document, EvaluationRequest request, const EffectLibrary& effects,
                          gpu::Device& device, gpu::Allocator& allocator, std::uint64_t timeout_ns) {
    validateRequest(document, request);
    if (device.features().shaderStorageImageReadWithoutFormat == VK_FALSE ||
        device.features().shaderStorageImageWriteWithoutFormat == VK_FALSE) {
        throw gpu::GpuException(gpu::GpuError::NoDevice,
                                "device does not support storage images with unknown format "
                                "(shaderStorageImageRead/WriteWithoutFormat); native effect execution requires it");
    }

    const std::vector<const Node*> order = scheduleDependencies(document, request.output);
    SubmissionQueue queue(device, device.graphics_family());
    // Runtime-compile cache within this evaluation (the same effect source
    // can run on several steps). Cross-evaluation shader compilation and
    // pipeline creation — asynchronous and cached so they never block the
    // UI path (spec section 10.4) — is the evaluator reuse work of issues
    // #9/#13; this executor creates each step's pipeline synchronously.
    std::map<std::string, std::vector<std::uint32_t>> compiled;

    GpuEvaluation evaluation;
    evaluation.plan.request = request;
    std::map<NodeId, ImageIdentity> identities;
    const ImageLayout layout = [&] {
        ImageLayout l;
        l.width = request.region.width;
        l.height = request.region.height;
        return l;
    }();

    for (const Node* node : order) {
        const auto programIt = effects.find(node->type);
        if (programIt == effects.end()) {
            failEffect(*node, EffectProgram{},
                       "no effect package in the supplied effect library (no silent substitution: evaluation stops)");
        }
        const EffectProgram& program = programIt->second;

        PlanStep step;
        step.node = node->id;
        step.type = node->type;
        step.name = node->name;
        step.effectiveParams = node->params;
        const std::vector<NodeId> producers = resolveStepInputs(document, *node, identities, step);

        // Inputs in port order: GPU-resident results of earlier steps.
        std::vector<const gpu::Image*> inputs;
        for (const NodeId producer : producers) {
            const auto found = evaluation.images.find(producer);
            if (found == evaluation.images.end()) {
                failEffect(*node, program, "input image for port was not produced by an earlier step");
            }
            inputs.push_back(&found->second.image);
        }

        // Compile/binding failures identify the node and the available
        // shader source location (spec section 10.4).
        std::vector<std::uint32_t> spirv;
        const auto cached = compiled.find(node->type);
        if (cached != compiled.end()) {
            spirv = cached->second;
        } else {
            try {
                spirv = program.glsl.empty() ? program.spirv : gpu::compileGlslToSpirv(program.glsl);
            } catch (const gpu::CompileException& error) {
                failEffect(*node, program, std::string("shader compile failed: ") + error.what());
            }
            compiled.emplace(node->type, spirv);
        }

        EffectUniforms uniforms{};
        std::vector<ComputeBinding> bindings;
        gpu::Buffer uniformBuffer;
        const std::uint32_t inputCount = prepareEffectStep(*node, request, program, step.effectiveParams, uniforms,
                                                           bindings, uniformBuffer, allocator);
        if (inputCount != inputs.size()) {
            failEffect(*node, program,
                       "effect declares " + std::to_string(inputCount) + " inputs but the plan wires " +
                           std::to_string(inputs.size()));
        }
        for (std::uint32_t i = 0; i < inputCount; ++i) {
            bindings.push_back({1, i, DescriptorKind::StorageImage, nullptr, inputs[i], false});
        }

        GpuNodeImage resident;
        resident.layout = layout;
        try {
            resident.image = createEffectImage(allocator, request);
        } catch (const gpu::GpuException& error) {
            failEffect(*node, program, std::string("output image allocation failed: ") + error.what());
        }

        // Fresh output image: UNDEFINED → GENERAL before its first write.
        prepareFreshImage(queue, resident.image, timeout_ns);
        // Written inputs become readable by this dispatch (write→read).
        for (const gpu::Image* input : inputs) {
            afterWriteBeforeRead(queue, *input, timeout_ns);
        }

        bindings.push_back({2, 0, DescriptorKind::StorageImage, nullptr, &resident.image, false});

        std::unique_ptr<ComputePass> pass;
        try {
            pass = ComputePass::create(device, spirv, bindings);
        } catch (const gpu::GpuException& error) {
            failEffect(*node, program, std::string("pipeline creation failed: ") + error.what());
        }
        const uint32_t groupsX = static_cast<uint32_t>((request.region.width + 7) / 8);
        const uint32_t groupsY = static_cast<uint32_t>((request.region.height + 7) / 8);
        try {
            pass->dispatch(groupsX, groupsY, 1, timeout_ns);
        } catch (const gpu::GpuException& error) {
            failEffect(*node, program, std::string("dispatch failed: ") + error.what());
        }

        step.produced.contentHash = 0;  // established by declared readback only
        step.produced.layout = layout;
        step.produced.residency = Residency::GpuDevice;
        identities.emplace(node->id, step.produced);
        evaluation.images.emplace(node->id, std::move(resident));
        evaluation.plan.steps.push_back(std::move(step));
    }

    evaluation.plan.result = identities.at(request.output);
    return evaluation;
}

}  // namespace nemo::eval
