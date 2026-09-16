#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/shuffle/Parameters.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

// Shuffle's authored mapping schema (issue #90). The eight rows are ordinary
// typed parameters — Choice, String — owned by the shared parameter
// commands/history/persistence/animation seams like every other node setting;
// the mapping editor reads and writes exactly these keys and adds no second
// parameter or state framework. input1/input2 resolve physical B/A inputs;
// in1/in2 and out1/out2 describe the layers displayed by the editor.
NodeDescriptor shuffleDescriptor() {
    const auto choice = [](std::string name, std::string label, std::string value, std::vector<std::string> choices) {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::Choice,
                             .defaultValue = ParameterValue{ChoiceValue{std::move(value)}},
                             .choices = std::move(choices),
                             .label = std::move(label),
                             .section = "Mapping",
                             .editor = {}};
    };
    const auto text = [](std::string name, std::string label, std::string value) {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::String,
                             .defaultValue = ParameterValue{std::move(value)},
                             .label = std::move(label),
                             .section = "Mapping",
                             .editor = {}};
    };
    std::vector<ParameterSpec> parameters{
        // The first parameter carries the editor id: it is the host's mount row,
        // and it is deliberately NOT in the editor's `consumes` list (the editor
        // presents it itself, like every other key it owns).
        choice("input1", "Input 1", "B", {"B", "A"}),
        choice("input2", "Input 2", "B", {"B", "A"}),
        text("in1", "In 1", "rgba"),
        text("in2", "In 2", ""),
        text("out1", "Out 1", "rgba"),
        text("out2", "Out 2", ""),
    };
    parameters.front().editor = "nemo.shuffle.mapping";
    parameters.reserve(parameters.size() + kShuffleRows * 3);
    // Defaults are the reference's: rows 0..3 map B's R/G/B/A onto the output's
    // R/G/B/A in order, rows 4..7 are present but disabled (no output channel),
    // which is how the second group starts empty.
    constexpr std::array<const char*, kShuffleGroupRows> kDefaultChannels{"R", "G", "B", "A"};
    for (std::size_t row = 0; row < kShuffleRows; ++row) {
        const std::string index = std::to_string(row);
        const bool mapped = row < kShuffleGroupRows;
        const std::string defaultChannel = mapped ? kDefaultChannels[row] : std::string{};
        parameters.push_back(choice("sourceKind" + index, "Source " + index, mapped ? "input1" : "input2",
                                    {"input1", "input2", "zero", "one"}));
        parameters.push_back(text("sourceChannel" + index, "Source Channel " + index, defaultChannel));
        parameters.push_back(text("outputChannel" + index, "Output Channel " + index, defaultChannel));
    }
    return NodeDescriptor{.type = "shuffle",
                          .displayName = "Shuffle",
                          .group = "Channels",
                          .implementationVersion = 1,
                          // B is the primary (required) input; A is the optional
                          // second image a mapping may draw from (issue #90).
                          .inputs = {{PortKind::Image, "B", false}, {PortKind::Image, "A", true}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = std::move(parameters),
                          .capabilities = builtinCapabilities()};
}

// The image Shuffle produces (issue #90). B is the base: its format, pixel
// aspect, precision, alpha association and interpretation are the result's,
// because B's channels are carried through untouched unless a row replaces
// them. The output carries B's channels in B's own order first, then every
// authored output channel the base does not already have, in row order; a row
// that names an existing channel REPLACES that channel's value at its existing
// position rather than creating a second one.
//
// Support follows what the mapping really reads: B's own data window (its
// untouched channels), A's data window for any row sourced from A (a sampled A
// keeps its coordinates), and B's format for the zero/one constants, which are
// defined everywhere the main image is.
[[nodiscard]] ImageDescription describeShuffle(const NodeDescriptionContext& context) {
    const NodeInstance& node = context.node;
    const ImageDescription* const base = !context.inputs.empty() ? context.inputs[0] : nullptr;
    if (base == nullptr) {
        failNode(node, "shuffle requires a connected B input");
    }
    const ImageDescription* const second = context.inputs.size() > 1 ? context.inputs[1] : nullptr;
    const ShuffleParameters params = effectiveShuffle(context.catalog, node, node.params);
    ImageDescription described = *base;
    Region bounds = described.dataBounds;
    for (const ShuffleRow& row : params.rows) {
        if (!row.enabled()) {
            continue;
        }
        if (!hasChannel(described.channels, row.outputChannel)) {
            described.channels.push_back(row.outputChannel);
        }
        if (row.source == ShuffleSource::InputA && second != nullptr) {
            bounds = regionUnion(bounds, second->dataBounds);
        } else if (row.source == ShuffleSource::Zero || row.source == ShuffleSource::One) {
            bounds = regionUnion(bounds, described.format);
        }
    }
    described.dataBounds = bounds;
    return described;
}

std::vector<InputRequirement> shuffleInputRequirements(const NodeRegionContext& context) {
    const auto params = effectiveShuffle(context.catalog, context.node, context.effectiveParams);
    std::vector<InputRequirement> requirements(2);
    const auto& outputs = context.request.channels.empty() ? context.description.channels : context.request.channels;
    for (const auto& output : outputs) {
        std::size_t port = 0;
        std::string_view source = output;
        bool constant = false;
        for (const auto& row : params.rows) {
            if (!row.enabled() || row.outputChannel != output)
                continue;
            constant = row.source == ShuffleSource::Zero || row.source == ShuffleSource::One;
            port = row.source == ShuffleSource::InputA ? 1 : 0;
            source = row.sourceChannel;
            break;
        }
        if (constant || port >= context.inputs.size() || context.inputs[port] == nullptr ||
            !hasChannel(context.inputs[port]->channels, source))
            continue;
        auto& channels = requirements[port].channels;
        if (!hasChannel(channels, source))
            channels.emplace_back(source);
    }
    return requirements;
}

CpuImage executeShuffle(const CpuNodeContext& context) {
    const CpuImage& base = requiredImageInput(context, 0, "shuffle requires a connected B input");
    const CpuImage* second = optionalImageInput(context, 1);
    const ShuffleParameters params = effectiveShuffle(context.catalog, context.node, context.effectiveParams);
    CpuImage output(effectRasterLayout(context));

    // Resolve the whole mapping once, outside the pixel loop (issue #90): for
    // every output channel, which raster it reads, which stored channel of that
    // raster, and where that raster's origin sits relative to this node's. A
    // constant channel reads no raster at all. A source name the chosen input
    // does not carry, and a disconnected A, resolve to zero — the frozen policy
    // — never to an invented channel or an error.
    const std::vector<std::string>& names = output.layout().channels;
    const std::size_t channels = names.size();
    const InputAnchor baseAnchor = anchorInput(context, 0, base);
    const InputAnchor secondAnchor = second != nullptr ? anchorInput(context, 1, *second) : InputAnchor{};
    std::vector<const CpuImage*> raster(channels, &base);
    std::vector<int> sourceChannel(channels, -1);
    std::vector<int> offsetX(channels, baseAnchor.offsetX);
    std::vector<int> offsetY(channels, baseAnchor.offsetY);
    std::vector<float> constant(channels, 0.0F);
    bool hasWhite = false;
    // Untouched B: every produced channel the base carries keeps the base's own
    // value at unchanged coordinates.
    for (std::size_t index = 0; index < channels; ++index) {
        sourceChannel[index] = channelIndex(base.layout().channels, names[index]);
    }
    for (const ShuffleRow& row : params.rows) {
        if (!row.enabled()) {
            continue;
        }
        const int index = channelIndex(names, row.outputChannel);
        if (index < 0) {
            continue;  // the description always declares it; nothing to write otherwise
        }
        const auto target = static_cast<std::size_t>(index);
        switch (row.source) {
        case ShuffleSource::InputB:
            raster[target] = &base;
            offsetX[target] = baseAnchor.offsetX;
            offsetY[target] = baseAnchor.offsetY;
            sourceChannel[target] = channelIndex(base.layout().channels, row.sourceChannel);
            break;
        case ShuffleSource::InputA:
            // A sampled channel keeps its coordinates; a missing name or an
            // absent A input is zero, and the mapping editor reports the
            // unavailable row.
            raster[target] = second;
            offsetX[target] = secondAnchor.offsetX;
            offsetY[target] = secondAnchor.offsetY;
            sourceChannel[target] = second != nullptr ? channelIndex(second->layout().channels, row.sourceChannel) : -1;
            break;
        case ShuffleSource::Zero:
            raster[target] = nullptr;
            sourceChannel[target] = -1;
            constant[target] = 0.0F;
            break;
        case ShuffleSource::One:
            raster[target] = nullptr;
            sourceChannel[target] = -1;
            constant[target] = 1.0F;
            hasWhite = true;
            break;
        }
    }

    const int width = output.width();
    const Region& format = context.description.format;  // Shuffle retains B's format.
    float* destination = output.data();
    for (int y = 0; y < output.height(); ++y) {
        const auto fy = static_cast<std::int64_t>(context.request.region.y) +
                        static_cast<std::int64_t>(y) * context.request.samplingScale;
        for (int x = 0; x < width; ++x) {
            const auto fx = static_cast<std::int64_t>(context.request.region.x) +
                            static_cast<std::int64_t>(x) * context.request.samplingScale;
            const bool insideFormat = hasWhite && fx >= format.x && fy >= format.y &&
                                      fx < static_cast<std::int64_t>(format.x) + format.width &&
                                      fy < static_cast<std::int64_t>(format.y) + format.height;
            for (std::size_t index = 0; index < channels; ++index) {
                const CpuImage* source = raster[index];
                const int from = sourceChannel[index];
                destination[index] = source != nullptr && from >= 0
                                         ? source->channel(offsetX[index] + x, offsetY[index] + y, from)
                                         : (source == nullptr && insideFormat ? constant[index] : 0.0F);
            }
            destination += channels;
        }
    }
    return output;
}

std::optional<std::string> validateShuffleParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                     const ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] { static_cast<void>(effectiveShuffle(catalog, node, effectiveParams)); });
}

}  // namespace

NodeContribution shuffleContribution() {
    NodeContribution contribution;
    contribution.descriptor = shuffleDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeShuffle};
    contribution.validateParameters = &validateShuffleParameters;
    contribution.describe = &describeShuffle;
    // Shuffle's mapping IS its output layout: the executors must never copy the
    // main input's auxiliary channels over the channels this node authored.
    contribution.ownsChannelLayout = true;
    // Translate destination demand to available source names. A constant or a
    // missing name adds no requirement; an empty port stays conservative under
    // the shared inherited-demand contract.
    contribution.inputRequirements = &shuffleInputRequirements;
    contribution.editors = {NodeEditorContribution{.id = "nemo.shuffle.mapping",
                                                   .source = "qrc:/qt/qml/Nemo/qml/ShuffleEditor.qml",
                                                   .consumes = {"input2",
                                                                "in1",
                                                                "in2",
                                                                "out1",
                                                                "out2",
                                                                "sourceKind0",
                                                                "sourceChannel0",
                                                                "outputChannel0",
                                                                "sourceKind1",
                                                                "sourceChannel1",
                                                                "outputChannel1",
                                                                "sourceKind2",
                                                                "sourceChannel2",
                                                                "outputChannel2",
                                                                "sourceKind3",
                                                                "sourceChannel3",
                                                                "outputChannel3",
                                                                "sourceKind4",
                                                                "sourceChannel4",
                                                                "outputChannel4",
                                                                "sourceKind5",
                                                                "sourceChannel5",
                                                                "outputChannel5",
                                                                "sourceKind6",
                                                                "sourceChannel6",
                                                                "outputChannel6",
                                                                "sourceKind7",
                                                                "sourceChannel7",
                                                                "outputChannel7"},
                                                   .presentation = "section"}};
    return contribution;
}

}  // namespace nemo::nodes
