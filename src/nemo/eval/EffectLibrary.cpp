#include "nemo/eval/GpuExecutor.hpp"

#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/eval/BuiltinGpuContributions.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/Error.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace nemo::eval {
namespace {

[[noreturn]] void invalid(const NodeContribution& node, const std::string& relationship) {
    throw std::invalid_argument("node contribution '" + node.descriptor.type + "': GPU " + relationship);
}

void validateGpu(const GpuNodeContribution& contribution) {
    const auto& node = contribution.node;
    if (!contribution.gpu) {
        if (node.nativeGpu && contribution.gpuUnavailableReason.empty())
            invalid(node, "backend is promised without an implementation or an unavailability reason");
        return;
    }
    if (!node.nativeGpu)
        invalid(node, "implementation conflicts with unsupported backend declaration");
    if (!contribution.gpuUnavailableReason.empty())
        invalid(node, "implementation conflicts with an unavailable backend declaration");
    const auto& implementation = *contribution.gpu;
    if (implementation.version != node.descriptor.implementationVersion)
        invalid(node, "implementation version conflicts with schema version");
    if (!implementation.prepare)
        invalid(node, "implementation has no preparation callback");
    if (implementation.payloadSize % 16 != 0 ||
        (implementation.payloadSize == 0) != implementation.payloadLayout.empty())
        invalid(node, "payload size and layout declaration are incompatible");
    if (implementation.payloadSize > 0 && implementation.payloadLayout.find('.') == std::string::npos)
        invalid(node, "payload layout identity must be namespaced");
    if (implementation.passes.empty())
        invalid(node, "implementation declares no local passes");
    std::set<std::string> ids;
    std::set<std::uint32_t> scratch;
    bool hasResult = false;
    for (const auto& pass : implementation.passes) {
        if (pass.id.empty() || !ids.insert(pass.id).second)
            invalid(node, "duplicate or empty local pass identity '" + pass.id + "'");
        if (pass.shader.empty() && pass.glsl.empty())
            invalid(node, "local pass '" + pass.id + "' has no shader implementation");
        if ((pass.output.kind != EffectImageKind::Output && pass.output.kind != EffectImageKind::Scratch) ||
            (pass.output.kind == EffectImageKind::Output && pass.output.index != 0))
            invalid(node, "local pass '" + pass.id + "' has an invalid output reference");
        for (const auto& input : pass.inputs) {
            switch (input.kind) {
            case EffectImageKind::Input:
                if (input.index >= node.descriptor.inputs.size())
                    invalid(node, "local pass '" + pass.id + "' references undeclared input port " +
                                      std::to_string(input.index));
                if (node.descriptor.inputs[input.index].kind == PortKind::Media)
                    invalid(node, "local pass '" + pass.id + "' binds a Media port as an image");
                break;
            case EffectImageKind::Scratch:
                if (!scratch.contains(input.index))
                    invalid(node, "local pass '" + pass.id + "' reads scratch without a preceding producer");
                break;
            case EffectImageKind::External:
                if (node.role != NodeRole::Source || input.index != 0)
                    invalid(node, "local pass '" + pass.id + "' reads external media outside the Source role");
                break;
            case EffectImageKind::Output:
            default:
                invalid(node, "local pass '" + pass.id + "' reads an invalid image reference");
            }
            if (input == pass.output)
                invalid(node, "local pass '" + pass.id + "' reads its own output");
        }
        if (pass.output.kind == EffectImageKind::Scratch) {
            if (!scratch.insert(pass.output.index).second)
                invalid(node, "local pass '" + pass.id + "' overwrites an existing scratch image");
        } else {
            hasResult = true;
        }
    }
    if (!hasResult)
        invalid(node, "local passes never produce the node output");
}

}  // namespace

struct EffectLibrary::Data {
    std::shared_ptr<const NodeContributions> contributions;
    std::map<std::string, RegisteredGpuEffect, std::less<>> effects;
    std::uint64_t fingerprint{kFnv1a64Basis};
};

std::vector<GpuNodeContribution> builtinGpuContributions() {
    std::vector<GpuNodeContribution> result;
#define NEMO_NODE(name) result.push_back(nodes::name##GpuContribution());
#include "nemo/nodes/BuiltinNodes.inc"
#undef NEMO_NODE
    return result;
}

EffectLibrary::EffectLibrary() : EffectLibrary({}, EffectBackend::Slang) {}

EffectLibrary::EffectLibrary(std::vector<GpuNodeContribution> contributions, EffectBackend backend,
                             const std::filesystem::path& spvDir, const std::filesystem::path& sourceDir) {
    // Validate the entire declaration before loading any code or exposing a
    // snapshot. A schema may legitimately have no implementation in this build.
    std::vector<NodeContribution> core;
    core.reserve(contributions.size());
    std::map<std::string, std::string> shaderNames;
    for (const auto& contribution : contributions) {
        validateGpu(contribution);
        if (contribution.gpu) {
            for (const auto& pass : contribution.gpu->passes) {
                if (pass.shader.empty())
                    continue;
                const auto basename = std::filesystem::path(pass.shader).filename().string();
                const auto [entry, inserted] = shaderNames.emplace(basename, pass.shader);
                if (!inserted && entry->second != pass.shader)
                    invalid(contribution.node, "shader '" + pass.shader +
                                                   "' conflicts with compiled shader identity '" + entry->second + "'");
            }
        }
        core.push_back(contribution.node);
    }
    auto data = std::make_shared<Data>();
    data->contributions = std::make_shared<const NodeContributions>(std::move(core));
    std::map<std::string, std::shared_ptr<const std::vector<std::uint32_t>>, std::less<>> programs;
    for (auto& contribution : contributions) {
        RegisteredGpuEffect effect;
        effect.implementation = std::move(contribution.gpu);
        effect.unavailableReason = std::move(contribution.gpuUnavailableReason);
        if (!effect.implementation && effect.unavailableReason.empty())
            effect.unavailableReason = "native GPU execution is not supported by this node role";
        if (effect.implementation) {
            const auto& implementation = *effect.implementation;
            effect.programs.reserve(implementation.passes.size());
            for (const auto& pass : implementation.passes) {
                EffectProgram program;
                const auto relative = std::filesystem::path(pass.shader);
                const auto spvPath = spvDir / (relative.filename().string() + ".spv");
                const auto sourcePath = sourceDir / (pass.shader + ".slang");
                program.sourcePath =
                    !sourceDir.empty() && std::filesystem::exists(sourcePath) ? sourcePath.string() : spvPath.string();
                try {
                    if (backend == EffectBackend::Slang && pass.shader.empty())
                        throw std::runtime_error("no native Slang implementation is supplied");
                    if (backend == EffectBackend::Glsl) {
                        program.sourcePath = pass.shader + " (GLSL reference, local pass '" + pass.id + "')";
                        if (pass.glsl.empty())
                            throw std::runtime_error("no GLSL reference implementation is supplied");
                    }
                    const auto nativePath = spvPath.string();
                    const auto& codeKey = backend == EffectBackend::Slang ? nativePath : pass.glsl;
                    auto compiled = programs.find(codeKey);
                    if (compiled == programs.end()) {
                        auto code = backend == EffectBackend::Slang ? gpu::loadSpirv(spvPath)
                                                                    : gpu::compileGlslToSpirv(pass.glsl);
                        compiled =
                            programs
                                .emplace(codeKey, std::make_shared<const std::vector<std::uint32_t>>(std::move(code)))
                                .first;
                    }
                    program.spirv = compiled->second;
                } catch (const std::exception& error) {
                    if (!effect.unavailableReason.empty())
                        effect.unavailableReason += "; ";
                    effect.unavailableReason +=
                        "local pass '" + pass.id + "' at " + program.sourcePath + ": " + error.what();
                }
                effect.programs.push_back(std::move(program));
            }
        }
        data->effects.emplace(contribution.node.descriptor.type, std::move(effect));
    }
    // Stable, order-independent registration identity. Paths, labels and
    // callback addresses are diagnostics/implementation details, not semantics.
    auto& hash = data->fingerprint;
    hashMixText(hash, std::string(kEffectBindingContractVersion));
    hashMixWord(hash, data->contributions->fingerprint());
    for (const auto& [type, effect] : data->effects) {
        hashMixText(hash, type);
        if (!effect.implementation)
            continue;
        const auto& implementation = *effect.implementation;
        hashMixWord(hash, implementation.version);
        hashMixText(hash, implementation.payloadLayout);
        hashMixWord(hash, implementation.payloadSize);
        for (std::size_t i = 0; i < implementation.passes.size(); ++i) {
            const auto& pass = implementation.passes[i];
            hashMixText(hash, pass.id);
            hashMixWord(hash, pass.inputs.size());
            for (const auto& input : pass.inputs) {
                hashMixWord(hash, static_cast<std::uint64_t>(input.kind));
                hashMixWord(hash, input.index);
            }
            hashMixWord(hash, static_cast<std::uint64_t>(pass.output.kind));
            hashMixWord(hash, pass.output.index);
            hashMixWord(hash, pass.weights);
            hashMixWord(hash, pass.geometry);
            const auto& spirv = effect.programs[i].spirv;
            hashMixWord(hash, spirv ? spirv->size() : 0);
            if (spirv)
                hashMix(hash, spirv->data(), spirv->size() * sizeof(std::uint32_t));
        }
    }
    data_ = std::move(data);
}

std::shared_ptr<const NodeContributions> EffectLibrary::contributions() const {
    return data_->contributions;
}
const RegisteredGpuEffect* EffectLibrary::find(std::string_view type) const {
    const auto found = data_->effects.find(type);
    return found == data_->effects.end() ? nullptr : &found->second;
}
std::uint64_t EffectLibrary::fingerprint() const {
    return data_->fingerprint;
}
std::shared_ptr<const void> EffectLibrary::retain() const {
    return data_;
}
const RegisteredGpuEffect& EffectLibrary::require(const NodeCatalog& catalog, const NodeInstance& node) const {
    data_->contributions->validate(catalog, node);
    const auto* effect = find(node.type);
    if (!effect || !effect->implementation || !effect->unavailableReason.empty()) {
        const std::string reason = effect ? effect->unavailableReason : "no GPU contribution is registered";
        throw EvaluationException(describeNode(node) + ": GPU executor unavailable: " + reason, node.id, node.name);
    }
    return *effect;
}

EffectLibrary loadSlangEffectLibrary(const std::filesystem::path& spvDir, const std::filesystem::path& sourceDir) {
    return EffectLibrary(builtinGpuContributions(), EffectBackend::Slang, spvDir, sourceDir);
}
EffectLibrary glslEffectLibrary() {
    return EffectLibrary(builtinGpuContributions(), EffectBackend::Glsl);
}

}  // namespace nemo::eval
