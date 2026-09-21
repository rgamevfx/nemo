#include "nemo/extensions/GpuPackages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/eval/BuiltinGpuContributions.hpp"
#include "nemo/extensions/EffectAbi.h"
#include "nemo/extensions/PackageRecord.hpp"

namespace nemo::extensions {
namespace {

using detail::PackageGpu;
using detail::PackageRecord;
using detail::SharedLibrary;

// The ABI flag word of one GPU preparation, derived from the described input the
// pass reads exactly like the CPU adapter derives it from the raster it reads:
// the input's association and its named channels decide whether the handed
// buffers are premultiplied and whether the package must bypass its color math.
[[nodiscard]] std::uint32_t preparationFlags(const eval::GpuNodeContext& context, std::uint32_t mainInput) {
    const ImageDescription* described =
        mainInput < context.inputDescriptions.size() ? context.inputDescriptions[mainInput] : nullptr;
    const ImageAssociation association =
        described != nullptr ? described->association : context.description.association;
    const std::span<const std::string> channels = described != nullptr
                                                      ? std::span<const std::string>{described->channels}
                                                      : std::span<const std::string>{context.description.channels};
    const ColorInterpretation color = described != nullptr ? described->color : context.description.color;
    return detail::effectFlags(association, color, hasPrimaryRgb(channels));
}

// The package's one local pass: its complete SPIR-V module and its complete
// GLSL reference source, reading the node's single image input and writing the
// node result. `spirvPath` is the absolute installed path the shared effect
// library loads, so an installed package never depends on the build tree's
// shader staging directory; `shader` stays the module's file name, which is
// what the library's shader-identity check compares.
[[nodiscard]] eval::EffectPassDefinition packagePass(const PackageGpu& gpu) {
    eval::EffectPassDefinition pass;
    pass.id = "main";
    pass.glsl = gpu.glsl;
    pass.inputs = {eval::EffectImageRef{eval::EffectImageKind::Input, 0}};
    pass.output = eval::EffectImageRef{eval::EffectImageKind::Output, 0};
    pass.weights = false;
    pass.geometry = false;
    pass.spirvPath = gpu.spirv;
    return pass;
}

// Worker-side preparation: the package fills exactly the payload its manifest
// declared, from the same resolved parameters the CPU adapter consumes and the
// same flag word, so both backends describe one authored state. Nothing is
// uploaded, submitted or waited on here.
[[nodiscard]] eval::GpuPreparation preparePackage(const std::shared_ptr<const SharedLibrary>& library,
                                                  const std::string& id, std::uint32_t payloadBytes,
                                                  std::uint32_t mainInput, const eval::GpuNodeContext& context) {
    const NemoEffectV1* entry = library->entry();
    eval::GpuPreparation preparation;
    preparation.payload.resize(payloadBytes);
    const std::string parameters = detail::parametersJson(context.effectiveParams);
    const std::uint32_t flags = preparationFlags(context, mainInput);
    std::array<char, detail::kDiagnosticCapacity> error{};
    const int status = entry->prepare(parameters.c_str(), flags, preparation.payload.data(), payloadBytes, error.data(),
                                      static_cast<std::uint32_t>(error.size()));
    error.back() = '\0';
    if (status != 0) {
        failNode(context.node,
                 "installed package '" + id + "' refused these parameters: " + detail::diagnosticText(error.data()));
    }
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

std::vector<eval::GpuNodeContribution> gpuContributions(const InstalledPackages& packages) {
    std::vector<eval::GpuNodeContribution> contributions = eval::builtinGpuContributions();
    contributions.reserve(contributions.size() + packages.records_.size());
    for (const PackageRecord& record : packages.records_) {
        eval::GpuNodeContribution contribution;
        contribution.node = detail::packageContribution(record);
        if (!record.gpu) {
            contribution.node.nativeGpu = false;
            contribution.gpuUnavailableReason = "the installed package declares no native GPU implementation";
            contributions.push_back(std::move(contribution));
            continue;
        }
        const std::shared_ptr<const SharedLibrary> library = record.library;
        eval::GpuImplementation implementation;
        implementation.version = record.implementationVersion;
        implementation.payloadLayout = record.gpu->payloadLayout;
        implementation.payloadSize = record.gpu->payloadBytes;
        implementation.passes = {packagePass(*record.gpu)};
        implementation.prepare = [library, id = record.id, payloadBytes = record.gpu->payloadBytes,
                                  mainInput = record.descriptor.mainInput](const eval::GpuNodeContext& context) {
            return preparePackage(library, id, payloadBytes, mainInput, context);
        };
        contribution.gpu = std::move(implementation);
        contributions.push_back(std::move(contribution));
    }
    return contributions;
}

}  // namespace nemo::extensions
