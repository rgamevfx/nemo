#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/premult/Parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor premultDescriptor() {
    return NodeDescriptor{.type = "premult",
                          .displayName = "Premult",
                          .group = "Channels",
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "in", false}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = {{.name = "multiply",
                                          .type = ParameterType::Choice,
                                          .defaultValue = ParameterValue{ChoiceValue{"RGB"}},
                                          .choices = {"RGB", "RGBA", "R", "G", "B", "Alpha", "None"},
                                          .label = "Multiply",
                                          .section = "Premult",
                                          .editor = {}},
                                         {.name = "by",
                                          .type = ParameterType::Choice,
                                          .defaultValue = ParameterValue{ChoiceValue{"Alpha"}},
                                          .choices = {"R", "G", "B", "Alpha"},
                                          .label = "By",
                                          .section = "Premult",
                                          .editor = {}}},
                          .capabilities = builtinCapabilities()};
}

constexpr std::array<std::uint32_t, kImageChannels> kChannelBits{kEffectChannelR, kEffectChannelG, kEffectChannelB,
                                                                 kEffectChannelA};

[[nodiscard]] bool selectedRoleExists(const std::array<int, 4>& roles, std::uint32_t selected) {
    for (std::size_t role = 0; role < roles.size(); ++role) {
        if ((selected & kChannelBits[role]) != 0U && roles[role] >= 0)
            return true;
    }
    return false;
}

void requireMultiplier(const NodeInstance& node, const std::vector<std::string>& channels,
                       const PremultParameters& params) {
    const auto roles = rgbaChannelIndices(channels);
    if (selectedRoleExists(roles, params.channels) && roles[static_cast<std::size_t>(params.byRole)] < 0) {
        failNode(node, "parameter 'by' selects a channel the input does not carry");
    }
}

std::vector<InputRequirement> premultInputRequirements(const NodeRegionContext& context) {
    std::vector<InputRequirement> requirements(1);
    if (context.inputs.empty() || context.inputs[0] == nullptr)
        failNode(context.node, "premult requires a connected image input");

    const PremultParameters params = effectivePremult(context.catalog, context.node, context.effectiveParams);
    const auto roles = rgbaChannelIndices(context.inputs[0]->channels);
    const auto& demand = context.request.channels.empty() ? context.description.channels : context.request.channels;
    bool changesDemand = false;
    for (const std::string& channel : demand) {
        for (std::size_t role = 0; role < roles.size(); ++role) {
            if ((params.channels & kChannelBits[role]) != 0U && roles[role] >= 0 &&
                channel == context.inputs[0]->channels[static_cast<std::size_t>(roles[role])]) {
                changesDemand = true;
                break;
            }
        }
    }
    if (!changesDemand)
        return requirements;

    requireMultiplier(context.node, context.inputs[0]->channels, params);
    requirements[0].channels = demand;
    const std::string& multiplier = context.inputs[0]->channels[static_cast<std::size_t>(roles[params.byRole])];
    if (!hasChannel(requirements[0].channels, multiplier))
        requirements[0].channels.push_back(multiplier);
    return requirements;
}

CpuImage executePremult(const CpuNodeContext& context) {
    const CpuImage& input = requiredImageInput(context, 0, "premult requires a connected image input");
    const PremultParameters params = effectivePremult(context.catalog, context.node, context.effectiveParams);
    const auto roles = rgbaChannelIndices(input.layout().channels);
    requireMultiplier(context.node, input.layout().channels, params);

    CpuImage output(effectRasterLayout(context));
    if (input.width() <= 0 || input.height() <= 0)
        return output;

    const InputAnchor anchor = anchorInput(context, 0, input);
    const std::size_t channelCount = output.channelCount();
    std::vector<int> sourceIndices(channelCount, -1);
    std::vector<int> roleAt(channelCount, -1);
    for (std::size_t index = 0; index < channelCount; ++index) {
        sourceIndices[index] = channelIndex(input.layout().channels, output.layout().channels[index]);
        for (std::size_t role = 0; role < roles.size(); ++role) {
            if (roles[role] == sourceIndices[index]) {
                roleAt[index] = static_cast<int>(role);
                break;
            }
        }
    }

    const std::size_t inputChannels = input.channelCount();
    for (int y = 0; y < output.height(); ++y) {
        const std::size_t sourceRow =
            (static_cast<std::size_t>(anchor.offsetY + y) * static_cast<std::size_t>(input.width()) +
             static_cast<std::size_t>(anchor.offsetX)) *
            inputChannels;
        const float* source = input.data() + sourceRow;
        float* destination =
            output.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(output.width()) * channelCount;
        for (int x = 0; x < output.width(); ++x) {
            const float multiplier = source[static_cast<std::size_t>(roles[params.byRole])];
            for (std::size_t index = 0; index < channelCount; ++index) {
                const int from = sourceIndices[index];
                const float value = from >= 0 ? source[static_cast<std::size_t>(from)] : 0.0F;
                const int role = roleAt[index];
                destination[index] = role >= 0 && (params.channels & kChannelBits[static_cast<std::size_t>(role)]) != 0U
                                         ? value * multiplier
                                         : value;
            }
            source += inputChannels;
            destination += channelCount;
        }
    }
    return output;
}

std::optional<std::string> validatePremultParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                     const ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] { static_cast<void>(effectivePremult(catalog, node, effectiveParams)); });
}

}  // namespace

NodeContribution premultContribution() {
    NodeContribution contribution;
    contribution.descriptor = premultDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executePremult};
    contribution.validateParameters = &validatePremultParameters;
    contribution.inputRequirements = &premultInputRequirements;
    return contribution;
}

}  // namespace nemo::nodes
