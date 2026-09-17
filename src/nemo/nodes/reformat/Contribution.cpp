#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/reformat/Parameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

// Reformat (issue #92, stories 50 and 52-57): one resampling effect with a
// resolved output canvas, an orientation stage and the full documented filter
// inventory. The parameter interpretation, the output-canvas policy and the
// output<->source map are the node's shared metadata (Parameters.hpp); the
// filter kernels and the pixel math below are this front end's own, and the
// native kernels evaluate the same declared filters independently (ADR-0004).
NodeDescriptor reformatDescriptor() {
    const auto choice = [](std::string name, std::string label, std::string value, std::vector<std::string> choices,
                           std::string section = "Reformat", std::string row = {}) {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::Choice,
                             .defaultValue = ParameterValue{ChoiceValue{std::move(value)}},
                             .choices = std::move(choices),
                             .label = std::move(label),
                             .section = std::move(section),
                             .editor = {},
                             .row = std::move(row)};
    };
    const auto integer = [](std::string name, std::string label, std::int64_t value, std::string row,
                            double softMaximum, std::string section = "Reformat") {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::Integer,
                             .defaultValue = ParameterValue{value},
                             .minimum = 1.0,
                             .step = 1.0,
                             .label = std::move(label),
                             .section = std::move(section),
                             .editor = {},
                             .softMinimum = 1.0,
                             .softMaximum = softMaximum,
                             .row = std::move(row)};
    };
    const auto aspect = [](std::string name, std::string label, std::string row) {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::Float,
                             .defaultValue = ParameterValue{1.0},
                             .minimum = 0.0,
                             .step = 0.001,
                             .label = std::move(label),
                             .section = "Reformat",
                             .editor = {},
                             .softMinimum = 0.1,
                             .softMaximum = 4.0,
                             .displayDecimals = 3,
                             .row = std::move(row),
                             .nonzero = true};
    };
    const auto scale = [](std::string name, std::string label) {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::Float,
                             .defaultValue = ParameterValue{1.0},
                             .minimum = 0.0,
                             .step = 0.001,
                             .label = std::move(label),
                             .section = "Reformat",
                             .editor = {},
                             .softMinimum = 0.01,
                             .softMaximum = 4.0,
                             .displayDecimals = 3,
                             .row = "Scale",
                             .nonzero = true};
    };
    const auto flag = [](std::string name, std::string label, bool value, std::string section) {
        return ParameterSpec{.name = std::move(name),
                             .type = ParameterType::Boolean,
                             .defaultValue = ParameterValue{value},
                             .label = std::move(label),
                             .section = std::move(section),
                             .editor = {}};
    };
    std::vector<ParameterSpec> parameters{
        choice("type", "Type", "format", {"format", "box", "scale"}),
        choice("formatSource", "Format", "composition", {"composition", "custom"}),
        integer("width", "Width", 1920, "OutputFormat", 8192.0),
        integer("height", "Height", 1080, "OutputFormat", 8192.0),
        aspect("pixelAspect", "Pixel Aspect", "OutputFormat"),
        integer("boxWidth", "W", 200, "Box", 4096.0),
        integer("boxHeight", "H", 200, "Box", 4096.0),
        aspect("boxPixelAspect", "PAR", "Box"),
        flag("forceShape", "Force This Shape", false, "Reformat"),
        scale("scaleX", "X"),
        scale("scaleY", "Y"),
        choice("resize", "Resize", "width", {"none", "width", "height", "fit", "fill", "distort"}, "Geometry"),
        flag("center", "Center", true, "Geometry"),
        flag("flip", "Flip", false, "Geometry"),
        flag("flop", "Flop", false, "Geometry"),
        flag("turn", "Turn", false, "Geometry"),
        choice("filter", "Filter", "Cubic",
               {"Impulse", "Cubic", "Keys", "Simon", "Rifman", "Mitchell", "Parzen", "Notch", "Lanczos4", "Lanczos6",
                "Sinc4"},
               "Sampling"),
        flag("clamp", "Clamp", false, "Sampling"),
        flag("blackOutside", "Black Outside", true, "Sampling"),
        flag("preserveBoundingBox", "Preserve Bounding Box", false, "Sampling"),
    };
    for (ParameterSpec& parameter : parameters) {
        if (parameter.name == "type") {
            // One section editor owns the mode-dependent Reformat surface.
            parameter.editor = "nemo.reformat.format";
        }
    }
    return NodeDescriptor{.type = "reformat",
                          .displayName = "Reformat",
                          .group = "Transform",
                          .implementationVersion = 1,
                          // One required image: the document's "black outside"
                          // control is the documented edge behaviour, so the node
                          // declares no optional mask of its own.
                          .inputs = {{PortKind::Image, "image", false}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = std::move(parameters),
                          .capabilities = builtinCapabilities()};
}

// ---------------------------------------------------------------------------
// Filter kernels (this front end's own; the native kernels declare theirs).
// ---------------------------------------------------------------------------

[[nodiscard]] double sinc(double value) {
    if (value == 0.0) {
        return 1.0;
    }
    const double scaled = std::numbers::pi * value;
    return std::sin(scaled) / scaled;
}

// The Mitchell-Netravali BC family. Cubic is (0,0), Keys (0,1/2), Simon
// (0,3/4), Rifman (0,1), Mitchell (1/3,1/3) and Parzen (1,0); the last four
// carry the documented sharpening or blurring.
[[nodiscard]] double bcCubic(double b, double c, double distance) {
    const double x = std::fabs(distance);
    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
    }
    if (x < 2.0) {
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x + (-12.0 * b - 48.0 * c) * x +
                (8.0 * b + 24.0 * c)) /
               6.0;
    }
    return 0.0;
}

[[nodiscard]] double reformatKernel(ReformatFilter filter, double distance) {
    const double x = std::fabs(distance);
    switch (filter) {
    case ReformatFilter::Impulse:
        return x < 0.5 ? 1.0 : 0.0;
    case ReformatFilter::Cubic:
        return bcCubic(0.0, 0.0, x);
    case ReformatFilter::Keys:
        return bcCubic(0.0, 0.5, x);
    case ReformatFilter::Simon:
        return bcCubic(0.0, 0.75, x);
    case ReformatFilter::Rifman:
        return bcCubic(0.0, 1.0, x);
    case ReformatFilter::Mitchell:
        return bcCubic(1.0 / 3.0, 1.0 / 3.0, x);
    case ReformatFilter::Parzen:
        return bcCubic(1.0, 0.0, x);
    case ReformatFilter::Notch:
        // The unit-width box has no interior shape: its weights ARE the closed
        // support window the enumerator produces, in the enumerator's own sample
        // units (reformatTapWeight below).
        break;
    case ReformatFilter::Lanczos4:
        return x < 2.0 ? sinc(x) * sinc(x * 0.5) : 0.0;
    case ReformatFilter::Lanczos6:
        return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    case ReformatFilter::Sinc4:
        return x < 2.0 ? sinc(x) : 0.0;
    }
    return 0.0;
}

// The window already decided the closed box boundary in double precision.
// Continuous coefficients remain independent of the native implementations.
[[nodiscard]] double reformatTapWeight(ReformatFilter filter, double offset, double widen) {
    return filter == ReformatFilter::Notch ? 1.0 : reformatKernel(filter, offset / widen);
}

// ---------------------------------------------------------------------------
// One axis's resolved taps.
// ---------------------------------------------------------------------------

// `indices` holds INPUT RASTER indices (raster sample i carries the source pixel
// whose center is i + 0.5) and `weights` the normalized separable kernel. A tap
// the raster does not hold contributes nothing, and with black outside off every
// tap is clamped into the input's data window first — the documented "outermost
// pixels fill the outside area" extension. Weights are normalized over the
// kernel's ideal support, so a tap outside it stays black instead of
// renormalizing the edge.
struct AxisTaps {
    std::vector<int> indices;
    std::vector<double> weights;
};

void fillAxisTaps(AxisTaps& taps, const ReformatGeometry& geometry, double widen, double center, int windowLow,
                  int windowHigh, int rasterExtent, const NodeInstance& node) {
    taps.indices.clear();
    taps.weights.clear();
    const auto window = reformatSampleWindow(geometry.filter, geometry.radius, widen, center, node);
    if (window.nearest) {
        int index = window.base;
        if (!geometry.blackOutside) {
            index = std::clamp(index, windowLow, windowHigh);
        }
        if (geometry.blackOutside && (index < windowLow || index > windowHigh))
            return;
        if (index >= 0 && index < rasterExtent) {
            taps.indices.push_back(index);
            taps.weights.push_back(1.0);
        }
        return;
    }
    double total = 0.0;
    for (int index = window.first; index <= window.last; ++index) {
        const double offset = window.fraction + static_cast<double>(window.base - index) - 0.5;
        const double weight = reformatTapWeight(geometry.filter, offset, widen);
        taps.indices.push_back(index);
        taps.weights.push_back(weight);
        total += weight;
    }
    if (total == 0.0) {
        // A degenerate position whose whole sampled window falls on kernel zeros:
        // fall back to the nearest sample, never to an empty sum.
        taps.indices.assign(1, window.base);
        taps.weights.assign(1, 1.0);
    } else {
        for (double& weight : taps.weights) {
            weight /= total;
        }
    }
    std::size_t kept = 0;
    for (std::size_t tap = 0; tap < taps.indices.size(); ++tap) {
        int index = taps.indices[tap];
        if (!geometry.blackOutside) {
            index = std::clamp(index, windowLow, windowHigh);
        }
        if (geometry.blackOutside && (index < windowLow || index > windowHigh))
            continue;
        if (index < 0 || index >= rasterExtent) {
            continue;
        }
        taps.indices[kept] = index;
        taps.weights[kept] = taps.weights[tap];
        ++kept;
    }
    taps.indices.resize(kept);
    taps.weights.resize(kept);
}

// The raster-sample range (inclusive) of one axis' data window, or an empty range
// that no tap can satisfy when the raster holds none of it.
void windowRange(double first, double last, int rasterExtent, int& low, int& high) {
    low = std::max(0, static_cast<int>(std::ceil(first)));
    high = std::min(rasterExtent - 1, static_cast<int>(std::ceil(last)) - 1);
    if (low > high) {
        low = -1;
        high = -1;
    }
}

// One mapped bound as a Region, or a node-identifying failure: truncating an
// unrepresentable bound would silently declare and demand a window other than
// the reformat's own.
[[nodiscard]] Region representableRegion(const NodeInstance& node, const char* what, double left, double top,
                                         double right, double bottom) {
    const auto representable = [](double value) {
        return std::isfinite(value) && std::abs(value) <= static_cast<double>(kMaxDescribedCoordinate);
    };
    if (!representable(left) || !representable(top) || !representable(right) || !representable(bottom)) {
        failNode(node, std::string(what) + " lies outside the representable image coordinates (|coordinate| <= " +
                           std::to_string(kMaxDescribedCoordinate) + ")");
    }
    const int x = static_cast<int>(std::floor(left));
    const int y = static_cast<int>(std::floor(top));
    return Region{x, y, static_cast<int>(std::ceil(right)) - x, static_cast<int>(std::ceil(bottom)) - y};
}

// The produced data window: the input's own data window mapped through the very
// geometry the pixels use, expanded by the filter's reach so every sample the
// kernel can still draw real data from stays inside the claim. `preserveBoundingBox`
// decides whether it may reach outside the output format; black outside decides
// whether the image really answers there.
[[nodiscard]] Region reformatDataBounds(const ReformatGeometry& geometry, const ReformatParameters& params,
                                        const NodeInstance& node) {
    const Region format{0, 0, geometry.output.width, geometry.output.height};
    if (geometry.empty) {
        return Region{};  // an empty data window stays transparent, extension included
    }
    // A sample center `c` draws real data while its window of contributing tap
    // centers — the outermost real sample plus the separable kernel's support —
    // overlaps the input's data window. The claim is stated in the source's own
    // coordinates and mapped forward, so the geometry, the orientation and the
    // expansion all travel through exactly one map.
    const Region& bounds = geometry.sourceBounds;
    const double reachX = geometry.radius * geometry.widenX;
    const double reachY = geometry.radius * geometry.widenY;
    const double lowX = static_cast<double>(bounds.x) + 0.5 - reachX;
    const double highX = static_cast<double>(bounds.x + bounds.width) - 0.5 + reachX;
    const double lowY = static_cast<double>(bounds.y) + 0.5 - reachY;
    const double highY = static_cast<double>(bounds.y + bounds.height) - 0.5 + reachY;
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (int corner = 0; corner < 4; ++corner) {
        const double x = (corner & 1) != 0 ? highX : lowX;
        const double y = (corner & 2) != 0 ? highY : lowY;
        double outX = 0.0;
        double outY = 0.0;
        reformatMapToOutput(geometry, x, y, outX, outY);
        minX = std::min(minX, outX);
        minY = std::min(minY, outY);
        maxX = std::max(maxX, outX);
        maxY = std::max(maxY, outY);
    }
    const Region mapped = representableRegion(node, "reformat's mapped data window", std::floor(minX), std::floor(minY),
                                              std::ceil(maxX), std::ceil(maxY));
    if (!params.blackOutside) {
        // Edge extension answers every requested coordinate, so the described
        // domain is the whole output format (plus the preserved bounds).
        return params.preserveBoundingBox ? regionUnion(mapped, format) : format;
    }
    return params.preserveBoundingBox ? mapped : regionIntersection(mapped, format);
}

// ---------------------------------------------------------------------------
// Description, planning and execution.
// ---------------------------------------------------------------------------

[[nodiscard]] ReformatGeometry resolveGeometry(const ImageDescription& input, const ImageDescription& output,
                                               const ReformatParameters& params, const NodeInstance& node) {
    const bool sourceHasAlpha = rgbaChannelIndices(input.channels)[3] >= 0;
    return resolveReformatGeometry(params,
                                   ReformatCanvas{output.format.width, output.format.height, output.pixelAspect},
                                   ReformatCanvas{input.format.width, input.format.height, input.pixelAspect},
                                   input.dataBounds, sourceHasAlpha, node);
}

// Reformat changes the image's size, so it owns its output description (issue
// #88): the format and pixel aspect come from the resolved canvas, the channels
// and interpretation from the main input — plus the documented solid alpha when
// black outside is on and the input carries none — and the data window follows.
[[nodiscard]] ImageDescription describeReformat(const NodeDescriptionContext& context) {
    const NodeInstance& node = context.node;
    const ImageDescription* const input = context.inputs.empty() ? nullptr : context.inputs.front();
    if (input == nullptr) {
        failNode(node, "reformat requires a connected main image input");
    }
    const ReformatParameters params = effectiveReformat(context.catalog, node, node.params);
    const ReformatCanvas source{input->format.width, input->format.height, input->pixelAspect};
    const ImageFormat* owning = nullptr;
    if (params.mode == ReformatMode::Format && params.source == ReformatFormatSource::Composition) {
        // The composition canvas is the OWNING network's authored format — never
        // the selected source or the viewer's resolution. Without the network
        // scope there is no composition to resolve, which is a hard failure
        // rather than an invented size.
        owning = context.owningFormat;
    }
    const ReformatCanvas output = reformatOutputCanvas(params, owning, source, node);
    const bool sourceHasAlpha = rgbaChannelIndices(input->channels)[3] >= 0;
    const ReformatGeometry resolved =
        resolveReformatGeometry(params, output, source, input->dataBounds, sourceHasAlpha, node);

    ImageDescription described = *input;
    described.format = Region{0, 0, output.width, output.height};
    described.pixelAspect = output.pixelAspect;
    described.dataBounds = reformatDataBounds(resolved, params, node);
    described.edgeExtension = !params.blackOutside && !resolved.empty;
    if (resolved.solidAlpha) {
        described.channels.push_back("A");
    }
    return described;
}

CpuImage executeReformat(const CpuNodeContext& context) {
    const NodeInstance& node = context.node;
    const CpuImage& input = requiredImageInput(context, 0, "reformat requires a connected main image input");
    if (context.inputDescriptions.empty() || context.inputDescriptions.front() == nullptr) {
        failNode(node, "reformat requires a described main image input");
    }
    const ImageDescription& inputDescription = *context.inputDescriptions.front();
    const ReformatParameters params = effectiveReformat(context.catalog, node, context.effectiveParams);
    const ReformatGeometry geometry = resolveGeometry(inputDescription, context.description, params, node);
    CpuImage result(effectRasterLayout(context));
    if (geometry.empty) {
        // An empty data window is a valid connected image: every sample stays
        // transparent, edge extension included (nothing to extend from).
        return result;
    }
    const int scale = context.request.samplingScale;
    if (!isSamplingScale(scale)) {
        failNode(node, "sampling scale " + std::to_string(scale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const EvaluationRequest& sourceRequest = inputRequest(context, 0);
    if (sourceRequest.samplingScale != scale) {
        failNode(node, "input raster at sampling scale " + std::to_string(sourceRequest.samplingScale) +
                           " cannot be read at this node's scale " + std::to_string(scale));
    }

    const double sampling = static_cast<double>(scale);
    const double regionX = static_cast<double>(context.request.region.x);
    const double regionY = static_cast<double>(context.request.region.y);
    // The input raster's own lattice: raster sample i carries the source pixel
    // whose center is i + 0.5 in the raster's index space.
    const double shiftX = static_cast<double>(sourceRequest.region.x) / sampling;
    const double shiftY = static_cast<double>(sourceRequest.region.y) / sampling;
    const Region& bounds = geometry.sourceBounds;
    int windowLowX = 0;
    int windowHighX = 0;
    int windowLowY = 0;
    int windowHighY = 0;
    windowRange((static_cast<double>(bounds.x) - sourceRequest.region.x) / sampling,
                (static_cast<double>(bounds.x + bounds.width) - sourceRequest.region.x) / sampling, input.width(),
                windowLowX, windowHighX);
    windowRange((static_cast<double>(bounds.y) - sourceRequest.region.y) / sampling,
                (static_cast<double>(bounds.y + bounds.height) - sourceRequest.region.y) / sampling, input.height(),
                windowLowY, windowHighY);

    const int width = result.width();
    const int height = result.height();
    // A turn CROSSES the axes: the source's X coordinate is then driven by the
    // output's Y, and the source's Y by the output's X. Each source tap window is
    // therefore resolved along the output axis that actually drives it — one
    // window per row when the geometry is turned, per column otherwise — which is
    // also the axis the solid-alpha test belongs to.
    const bool crosses = geometry.turn;
    const int driveX = crosses ? height : width;
    const int driveY = crosses ? width : height;
    std::vector<AxisTaps> sourceXWindows(static_cast<std::size_t>(driveX));
    std::vector<AxisTaps> sourceYWindows(static_cast<std::size_t>(driveY));
    std::vector<char> insideX(static_cast<std::size_t>(driveX), 0);
    std::vector<char> insideY(static_cast<std::size_t>(driveY), 0);
    for (int index = 0; index < driveX; ++index) {
        // The nominal coordinate on the other output axis is irrelevant: the map
        // resolves each source coordinate from exactly one output axis.
        const double outputX = regionX + (crosses ? 0.5 : (static_cast<double>(index) + 0.5) * sampling);
        const double outputY = regionY + (crosses ? (static_cast<double>(index) + 0.5) * sampling : 0.5);
        double sourceX = 0.0;
        double sourceY = 0.0;
        reformatMapToSource(geometry, outputX, outputY, sourceX, sourceY);
        fillAxisTaps(sourceXWindows[static_cast<std::size_t>(index)], geometry, geometry.widenX,
                     sourceX / sampling - shiftX, windowLowX, windowHighX, input.width(), context.node);
        insideX[static_cast<std::size_t>(index)] =
            sourceX >= static_cast<double>(bounds.x) && sourceX < static_cast<double>(bounds.x + bounds.width) ? 1 : 0;
    }
    for (int index = 0; index < driveY; ++index) {
        const double outputX = regionX + (crosses ? (static_cast<double>(index) + 0.5) * sampling : 0.5);
        const double outputY = regionY + (crosses ? 0.5 : (static_cast<double>(index) + 0.5) * sampling);
        double sourceX = 0.0;
        double sourceY = 0.0;
        reformatMapToSource(geometry, outputX, outputY, sourceX, sourceY);
        fillAxisTaps(sourceYWindows[static_cast<std::size_t>(index)], geometry, geometry.widenY,
                     sourceY / sampling - shiftY, windowLowY, windowHighY, input.height(), context.node);
        insideY[static_cast<std::size_t>(index)] =
            sourceY >= static_cast<double>(bounds.y) && sourceY < static_cast<double>(bounds.y + bounds.height) ? 1 : 0;
    }

    const std::vector<std::string>& outputChannels = result.layout().channels;
    const bool premultiply = rgbaChannelIndices(inputDescription.channels)[3] >= 0 &&
                             inputDescription.association == ImageAssociation::Straight;
    // Named auxiliary planes are resampled numerically and independently of the
    // association-aware primary RGBA (issue #90): every stored channel of the
    // output is produced by this node, which is why it owns its channel layout.
    std::vector<int> auxiliaryOutput;
    std::vector<int> auxiliarySource;
    for (std::size_t index = 0; index < outputChannels.size(); ++index) {
        bool primary = false;
        for (std::size_t role = 0; role < kImageChannels; ++role) {
            if (channelsDetail::isPrimaryRoleName(outputChannels[index], role)) {
                primary = true;
                break;
            }
        }
        if (primary) {
            continue;
        }
        auxiliaryOutput.push_back(static_cast<int>(index));
        auxiliarySource.push_back(channelIndex(input.layout().channels, outputChannels[index]));
    }
    std::vector<double> auxiliaryAccumulated(auxiliaryOutput.size(), 0.0);
    std::vector<double> auxiliaryLowest(auxiliaryOutput.size(), 0.0);
    std::vector<double> auxiliaryHighest(auxiliaryOutput.size(), 0.0);
    constexpr double kInfinity = std::numeric_limits<double>::infinity();

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t xIndex = static_cast<std::size_t>(crosses ? y : x);
            const std::size_t yIndex = static_cast<std::size_t>(crosses ? x : y);
            const AxisTaps& xTaps = sourceXWindows[xIndex];
            const AxisTaps& yTaps = sourceYWindows[yIndex];
            const bool insideSource = insideX[xIndex] != 0 && insideY[yIndex] != 0;
            std::array<double, kImageChannels> accumulated{0.0, 0.0, 0.0, 0.0};
            std::array<double, kImageChannels> lowest{kInfinity, kInfinity, kInfinity, kInfinity};
            std::array<double, kImageChannels> highest{-kInfinity, -kInfinity, -kInfinity, -kInfinity};
            std::fill(auxiliaryAccumulated.begin(), auxiliaryAccumulated.end(), 0.0);
            std::fill(auxiliaryLowest.begin(), auxiliaryLowest.end(), kInfinity);
            std::fill(auxiliaryHighest.begin(), auxiliaryHighest.end(), -kInfinity);
            bool contributed = false;
            for (std::size_t rowTap = 0; rowTap < yTaps.indices.size(); ++rowTap) {
                const double rowWeight = yTaps.weights[rowTap];
                if (rowWeight == 0.0) {
                    continue;
                }
                const int tapY = yTaps.indices[rowTap];
                for (std::size_t columnTap = 0; columnTap < xTaps.indices.size(); ++columnTap) {
                    const double weight = xTaps.weights[columnTap] * rowWeight;
                    if (weight == 0.0) {
                        continue;
                    }
                    const int tapX = xTaps.indices[columnTap];
                    std::array<float, kImageChannels> sample = sampledPixel(input, tapX, tapY);
                    if (premultiply) {
                        sample = {sample[0] * sample[3], sample[1] * sample[3], sample[2] * sample[3], sample[3]};
                    }
                    for (std::size_t role = 0; role < kImageChannels; ++role) {
                        const double value = static_cast<double>(sample[role]);
                        accumulated[role] += weight * value;
                        lowest[role] = std::min(lowest[role], value);
                        highest[role] = std::max(highest[role], value);
                    }
                    for (std::size_t plane = 0; plane < auxiliaryOutput.size(); ++plane) {
                        const int source = auxiliarySource[plane];
                        if (source < 0) {
                            continue;
                        }
                        const double value = static_cast<double>(input.channel(tapX, tapY, source));
                        auxiliaryAccumulated[plane] += weight * value;
                        auxiliaryLowest[plane] = std::min(auxiliaryLowest[plane], value);
                        auxiliaryHighest[plane] = std::max(auxiliaryHighest[plane], value);
                    }
                    contributed = true;
                }
            }
            std::array<float, kImageChannels> filtered{0.0F, 0.0F, 0.0F, 0.0F};
            for (std::size_t role = 0; role < kImageChannels; ++role) {
                // `clamp` bounds the result by the contributing SAMPLES' extrema —
                // never by 0..1 — which is what removes filter haloing without
                // clipping legitimate HDR or negative values.
                const double value = contributed && params.clamp
                                         ? std::clamp(accumulated[role], lowest[role], highest[role])
                                         : accumulated[role];
                filtered[role] = static_cast<float>(value);
            }
            std::array<float, kImageChannels> pixel = premultiply ? straightPixel(filtered) : filtered;
            if (geometry.solidAlpha) {
                // The documented "solid alpha covering the input image area" for
                // an input that carries none.
                pixel[3] = insideSource ? 1.0F : 0.0F;
            }
            result.setPixel(x, y, pixel);
            for (std::size_t plane = 0; plane < auxiliaryOutput.size(); ++plane) {
                double value = auxiliaryAccumulated[plane];
                if (params.clamp && auxiliaryLowest[plane] <= auxiliaryHighest[plane]) {
                    value = std::clamp(value, auxiliaryLowest[plane], auxiliaryHighest[plane]);
                }
                result.setChannel(x, y, auxiliaryOutput[plane], static_cast<float>(value));
            }
        }
    }
    return result;
}

// Reformat reads the inverse image of the region it must produce, expanded by the
// sampling filter's footprint (issue #85): a mapped coordinate the input does not
// hold needs no pixels at all — it is transparent black, or the outermost edge
// sample when black outside is off, exactly as the whole-image reference has it.
std::vector<InputRequirement> reformatInputRequirements(const NodeRegionContext& context) {
    const ImageDescription* const input = context.inputs.empty() ? nullptr : context.inputs.front();
    if (input == nullptr) {
        failNode(context.node, "reformat requires a connected main image input");
    }
    const ReformatParameters params = effectiveReformat(context.catalog, context.node, context.effectiveParams);
    const ReformatGeometry geometry = resolveGeometry(*input, context.description, params, context.node);
    if (geometry.empty) {
        // Nothing to read from an empty data window: the produced image is
        // transparent everywhere.
        return {InputRequirement{Region{}, {}}};
    }
    const EvaluationRequest& request = context.request;
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    const int samplesX = scaledDimension(request.region.width, scale);
    const int samplesY = scaledDimension(request.region.height, scale);
    if (samplesX <= 0 || samplesY <= 0) {
        return {InputRequirement{Region{}, {}}};
    }
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const int x : {0, samplesX - 1}) {
        for (const int y : {0, samplesY - 1}) {
            const double outX =
                static_cast<double>(request.region.x) + (static_cast<double>(x) + 0.5) * static_cast<double>(scale);
            const double outY =
                static_cast<double>(request.region.y) + (static_cast<double>(y) + 0.5) * static_cast<double>(scale);
            double sourceX = 0.0;
            double sourceY = 0.0;
            reformatMapToSource(geometry, outX, outY, sourceX, sourceY);
            if (!std::isfinite(sourceX) || !std::isfinite(sourceY)) {
                failNode(context.node, "the reformat's inverse map is not finite for region (" +
                                           std::to_string(request.region.x) + "," + std::to_string(request.region.y) +
                                           ") " + std::to_string(request.region.width) + "x" +
                                           std::to_string(request.region.height) + ")");
            }
            minX = std::min(minX, sourceX);
            minY = std::min(minY, sourceY);
            maxX = std::max(maxX, sourceX);
            maxY = std::max(maxY, sourceY);
        }
    }
    // Filter support is measured in source RASTER samples. Convert its halo
    // back to full-resolution coordinates before the shared planner sees it.
    const double padX = (geometry.radius * geometry.widenX + 1.0) * scale;
    const double padY = (geometry.radius * geometry.widenY + 1.0) * scale;
    double left = minX - padX;
    double top = minY - padY;
    double right = maxX + padX;
    double bottom = maxY + padY;
    const Region& domain = geometry.sourceBounds;
    if (geometry.blackOutside) {
        left = std::max(left, static_cast<double>(domain.x));
        top = std::max(top, static_cast<double>(domain.y));
        right = std::min(right, static_cast<double>(domain.x + domain.width));
        bottom = std::min(bottom, static_cast<double>(domain.y + domain.height));
    } else {
        // A wholly distant demand still needs the nearest RETAINED lattice
        // sample. Intersecting it with the domain would incorrectly fetch none.
        const int lowX = latticeCeil(domain.x, scale);
        const int lowY = latticeCeil(domain.y, scale);
        const int highX = latticeFloor(domain.x + domain.width - 1, scale);
        const int highY = latticeFloor(domain.y + domain.height - 1, scale);
        if (lowX > highX || lowY > highY)
            return {InputRequirement{Region{}, {}}};
        left = std::clamp(left, static_cast<double>(lowX), static_cast<double>(highX));
        top = std::clamp(top, static_cast<double>(lowY), static_cast<double>(highY));
        right = std::clamp(right, static_cast<double>(lowX + 1), static_cast<double>(highX + 1));
        bottom = std::clamp(bottom, static_cast<double>(lowY + 1), static_cast<double>(highY + 1));
    }
    if (right <= left || bottom <= top)
        return {InputRequirement{Region{}, {}}};
    return {
        InputRequirement{representableRegion(context.node, "reformat's retained input read", left, top, right, bottom),
                         {}}};
}

std::optional<std::string> validateReformatParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                      const ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] { static_cast<void>(effectiveReformat(catalog, node, effectiveParams)); });
}

}  // namespace

NodeContribution reformatContribution() {
    NodeContribution contribution;
    contribution.descriptor = reformatDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeReformat};
    contribution.validateParameters = &validateReformatParameters;
    contribution.inputRequirements = &reformatInputRequirements;
    contribution.describe = &describeReformat;
    // Reformat's own math produces every stored channel of its output — the
    // primary RGBA association-aware, each named auxiliary plane numerically, and
    // the documented solid alpha — so the executors must never copy the main
    // input's channels over it (issue #90).
    contribution.ownsChannelLayout = true;
    contribution.editors = {NodeEditorContribution{
        .id = "nemo.reformat.format",
        .source = "qrc:/qt/qml/Nemo/qml/ReformatFormatEditor.qml",
        .consumes = {"type",     "formatSource", "width",          "height",       "pixelAspect",
                     "boxWidth", "boxHeight",    "boxPixelAspect", "forceShape",   "scaleX",
                     "scaleY",   "resize",       "center",         "flip",         "flop",
                     "turn",     "filter",       "clamp",          "blackOutside", "preserveBoundingBox"},
        .presentation = "section"}};
    return contribution;
}

}  // namespace nemo::nodes
