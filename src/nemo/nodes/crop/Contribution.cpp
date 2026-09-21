// Crop: the native box crop with soft edges, black-outside/edge-extension and
// an optional reformat (issue #92, stories 43, 45-49).
//
// Crop is a window operation, not a resample: a sample the retained box covers
// keeps the incoming value at its own coordinates, a sample the fade ramp
// reaches is faded, and a sample outside the box is black or the clamped
// retained edge sample. Nothing in this file resamples, so there is no filter
// and no half-float intermediate: the output raster is the requested region at
// the request's sampling scale, written sample by sample.
//
// The box is authored bottom-left in the INCOMING IMAGE's current format and is
// fractional (#92): Parameters.hpp owns the conversion into the stored raster's
// y-down convention and the floor/ceil enclosure, which is the domain this node
// DESCRIBES. The owning network's saved canvas only supplied the box's initial
// values at creation. The pixel math stays here and in the two kernels (issues
// #83, #92): each decides membership, clamping and the softness ramp itself, so
// their agreement is evidence rather than a shared mistake.

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/crop/Parameters.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor cropDescriptor() {
    // The box and its behaviour, in the reference's own control order. `x`
    // carries the namespaced editor id: it is the host's mount row for
    // CropBoxEditor.qml, which groups the box, softness and behavior flags and
    // writes them through the shared parameter commands.
    return NodeDescriptor{.type = "crop",
                          .displayName = "Crop",
                          .group = "Transform",
                          // 1: first version of the box/reformat/extension
                          // contract (issue #92).
                          .implementationVersion = 1,
                          // One required image. Crop has no mask controls, so
                          // it declares no optional mask port either.
                          .inputs = {{PortKind::Image, "image", false}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters =
                              {
                                  {.name = "x",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{0.0},
                                   .step = 1.0,
                                   .label = "X",
                                   .section = "Crop",
                                   .editor = "nemo.crop.box",
                                   .row = "Box"},
                                  {.name = "y",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{0.0},
                                   .step = 1.0,
                                   .label = "Y",
                                   .section = "Crop",
                                   .editor = {},
                                   .row = "Box"},
                                  // The box's far corner starts at the owning
                                  // network's saved canvas (issue #92): the
                                  // creation owner seeds these two from the
                                  // network, so a new Crop encloses the whole
                                  // composition instead of a degenerate box.
                                  {.name = "right",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{0.0},
                                   .step = 1.0,
                                   .label = "Right",
                                   .section = "Crop",
                                   .editor = {},
                                   .row = "Box",
                                   .initialValue = ParameterInitialValue::OwningNetworkWidth},
                                  {.name = "top",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{0.0},
                                   .step = 1.0,
                                   .label = "Top",
                                   .section = "Crop",
                                   .editor = {},
                                   .row = "Box",
                                   .initialValue = ParameterInitialValue::OwningNetworkHeight},
                                  {.name = "softness",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{0.0},
                                   .minimum = 0.0,
                                   .step = 1.0,
                                   .label = "Softness",
                                   .section = "Crop",
                                   .editor = {},
                                   .softMinimum = 0.0,
                                   .softMaximum = 100.0},
                                  {.name = "reformat",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{false},
                                   .label = "Reformat",
                                   .section = "Crop",
                                   .editor = {}},
                                  {.name = "intersect",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{false},
                                   .label = "Intersect",
                                   .section = "Crop",
                                   .editor = {}},
                                  {.name = "blackOutside",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{true},
                                   .label = "Black Outside",
                                   .section = "Crop",
                                   .editor = {}},
                              },
                          .capabilities = builtinCapabilities()};
}

// The input description the node reads its incoming coverage from, or null for
// an unconnected/unestablished port.
[[nodiscard]] const ImageDescription* mainInput(const CpuNodeContext& context) {
    return context.inputDescriptions.empty() ? nullptr : context.inputDescriptions[0];
}

[[nodiscard]] const ImageDescription* mainInput(const NodeRegionContext& context) {
    return context.inputs.empty() ? nullptr : context.inputs[0];
}

[[nodiscard]] const ImageDescription* mainInput(const NodeDescriptionContext& context) {
    return context.inputs.empty() ? nullptr : context.inputs[0];
}

[[nodiscard]] Region cropRetainedFor(const CropBox& box, const CropParameters& params, const ImageDescription* input) {
    return cropRetained(box, params.intersect, input != nullptr ? &input->dataBounds : nullptr);
}

// The image Crop produces (issue #88, #92). Everything but the geometry is
// inherited from the main input — its format when the crop only changes data
// bounds (story 45), its pixel aspect, precision, association and
// interpretation. What Crop states itself is:
//
//   * its data bounds: the box's enclosure, intersected with the incoming data
//     window when `intersect` is enabled (story 47). An empty intersection is a
//     valid fully transparent image (story 49), never a rejected one.
//   * `edgeExtension` when `blackOutside` is disabled: the retained bounds are
//     then an edge domain the node answers outside of, with the clamped
//     retained edge sample (the shared contract's meaning of the flag), so no
//     consumer — and no support guard — treats that real data as transparent.
//     With black outside the default, the retained bounds stay an ordinary
//     finite support and everything outside them is transparent black.
//   * its channel vocabulary: exactly the incoming channels, plus one alpha
//     channel when black outside is enabled and the image stores none. The
//     reference adds a solid alpha covering the image area there, and without
//     one the faded edge of a straight image would have nothing to fade
//     through. No other channel is created, renamed or dropped.
//   * the format, when `reformat` is enabled (story 46): the BOX's own floor/ceil
//     enclosure translated to the origin, so downstream nodes see the cropped
//     image's intended dimensions and coordinates. `intersect` only clips the
//     data window INSIDE that format (story 47), so a box that misses the
//     incoming image entirely still publishes a valid positive format whose data
//     bounds are empty — a fully transparent image, exactly as the
//     non-reformat case leaves it. Only a degenerate box (a zero-size enclosure,
//     e.g. right == x) has no format to publish, and that is refused here, where
//     the node is known, instead of publishing a format nothing can hold.
[[nodiscard]] ImageDescription describeCrop(const NodeDescriptionContext& context) {
    const NodeInstance& node = context.node;
    const CropParameters params = effectiveCrop(context.catalog, node, node.params);
    const ImageDescription* const input = mainInput(context);
    const CropBox box = cropBox(node, params, cropFrameHeight(node, input));
    const Region retained = cropRetainedFor(box, params, input);

    ImageDescription described = context.inherited;
    described.channels = input != nullptr ? input->channels : described.channels;
    if (params.blackOutside && rgbaChannelIndices(described.channels)[3] < 0) {
        described.channels.emplace_back("A");
    }
    described.dataBounds = retained;
    described.edgeExtension = !params.blackOutside;
    if (params.reformat) {
        const Region& enclosure = box.enclosure;
        if (enclosure.width <= 0 || enclosure.height <= 0) {
            failNode(node, "parameter 'reformat' would format the output to zero pixels: the crop box's floor/ceil "
                           "enclosure is empty (check parameters 'x'/'right' and 'y'/'top')");
        }
        described.format = Region{0, 0, enclosure.width, enclosure.height};
        described.dataBounds =
            Region{retained.x - enclosure.x, retained.y - enclosure.y, retained.width, retained.height};
    }
    return described;
}

// Crop reads the incoming image inside the retained box and nowhere else: its
// extension answers a coordinate outside the box by clamping BACK into it, and
// a black-outside crop does not read there at all. So the demand is the box's
// enclosure, bounded by what the producer can actually answer — the shared
// `requirementDomain`, which declines to clip at an extended producer's
// retained bounds because those bounds are not the limit of what it answers.
//
// The demand is stated in the INCOMING image's coordinates, never the
// reformatted output's: reformat translates the node's own output frame, while
// the pixels it reads are still where the box is in the incoming image.
std::vector<InputRequirement> cropInputRequirements(const NodeRegionContext& context) {
    const NodeInstance& node = context.node;
    const CropParameters params = effectiveCrop(context.catalog, node, context.effectiveParams);
    const ImageDescription* const input = mainInput(context);
    const CropBox box = cropBox(node, params, cropFrameHeight(node, input));
    const Region retained = cropRetainedFor(box, params, input);
    if (retained.width <= 0 || retained.height <= 0) {
        return {InputRequirement{Region{}, {}}};
    }
    const Region domain = requirementDomain(context, 0, retained);
    return {InputRequirement{regionIntersection(retained, domain), {}}};
}

// One output sample: which retained sample it reads, and that sample's
// full-resolution anchor. A sample whose own anchor is inside the retained
// domain reads itself; every other sample — black outside, or answered by the
// clamped edge — reads the nearest retained sample, which is the exact reading
// the description's `edgeExtension` claim promises consumers.
//
// The two axes are decided INDEPENDENTLY, because a retained domain can be
// representable at this density on one axis and not on the other (a box two
// pixels tall at quarter resolution). An axis no sample anchor reaches is the
// sample-level limit of a reduction: the clamped edge is then the nearest
// anchor the raster can actually hold. Full-resolution requests (scale 1) reach
// exactly the retained pixels, so the rule is exact there and degenerate only
// where the reduction genuinely cannot represent the box.
struct CropSample {
    bool inside{false};
    int column{0};
    int row{0};
    int anchorX{0};
    int anchorY{0};
};

[[nodiscard]] CropSample cropSample(const EvaluationRequest& request, int scale, const Region& domain,
                                    const Region& raster, int column, int row) {
    CropSample sample;
    const bool haveX = raster.width > 0;
    const bool haveY = raster.height > 0;
    const bool insideX = haveX && column >= raster.x && column < raster.x + raster.width;
    const bool insideY = haveY && row >= raster.y && row < raster.y + raster.height;
    sample.inside = insideX && insideY;
    if (insideX) {
        sample.column = column;
    } else if (haveX) {
        sample.column = std::clamp(column, raster.x, raster.x + raster.width - 1);
    } else {
        sample.column = std::clamp(cropCeilDiv(domain.x - request.region.x, scale), 0,
                                   std::max(0, scaledDimension(request.region.width, scale) - 1));
    }
    if (insideY) {
        sample.row = row;
    } else if (haveY) {
        sample.row = std::clamp(row, raster.y, raster.y + raster.height - 1);
    } else {
        sample.row = std::clamp(cropCeilDiv(domain.y - request.region.y, scale), 0,
                                std::max(0, scaledDimension(request.region.height, scale) - 1));
    }
    sample.anchorX = request.region.x + sample.column * scale;
    sample.anchorY = request.region.y + sample.row * scale;
    return sample;
}

// This node's own output raster of the incoming image, one sample at a time.
//
// Membership, clamping and fading are decided in the OUTPUT frame: with
// `reformat` the retained enclosure is translated to the origin, and the
// request region this adapter is handed is stated in that output frame, so the
// box edges, the retained domain and the request all share one origin and only
// the READ is offset back into the incoming image's frame (`cropRasterBase`).
//
// The fade is the reference's vignette, in full-resolution pixels: an inward
// linear ramp per edge, the product at corners, zero hard. It is a function of
// the sample's own full-resolution ANCHOR, which is what makes Full, Half and
// Quarter requests agree wherever their anchors coincide instead of sampling a
// differently-placed ramp. Retained samples the ramp reaches fade; samples
// outside the box are black or the clamped edge, so a soft crop fades inward
// from its own edges rather than out of them.
//
// Channel policy (issue #92, the frozen Nemo disposition):
//
//   * this node owns its channel layout, because every plane it produces moves
//     and crops — the shared auxiliary preservation copies a plane at UNCHANGED
//     coordinates and would be wrong here — so it writes every stored plane:
//     the primary roles from the incoming image's RGBA projection, every other
//     plane from the same-named incoming plane at the sample it read.
//   * an image that stores no alpha and black-outs gets the reference's solid
//     alpha: 1 where the sample's full-resolution pixel lies inside the incoming
//     data window, 0 elsewhere, faded with the rest of the pixel. No other
//     channel is invented.
//   * the fade follows the image's declared association: straight primary RGB
//     fades through alpha (alpha scales, RGB is kept, which is what
//     straight-alpha compositing shows), premultiplied primary RGB scales
//     directly with alpha. An image with no alpha in the OUTPUT (only reachable
//     with black outside disabled and no incoming alpha) has nothing to fade
//     through, so its stored channels scale numerically.
//   * auxiliary planes always scale numerically, and are zero where the
//     incoming image has no sample.
CpuImage executeCrop(const CpuNodeContext& context) {
    const NodeInstance& node = context.node;
    const EvaluationRequest& request = context.request;
    if (!isSamplingScale(request.samplingScale)) {
        failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const int scale = request.samplingScale;
    const CpuImage& input = requiredImageInput(context, 0, "crop requires a connected image input");
    // The port's raster must cover the demand placed on it; where it holds no
    // sample, the incoming image simply has no data and the read below is
    // transparent black (issue #88).
    static_cast<void>(anchorInput(context, 0, input));

    const CropParameters params = effectiveCrop(context.catalog, node, context.effectiveParams);
    const ImageDescription* const source = mainInput(context);
    const CropBox box = cropBox(node, params, cropFrameHeight(node, source));
    const Region retained = cropRetainedFor(box, params, source);
    // Reformat publishes the BOX's floor/ceil enclosure at the origin (story 46),
    // so every output coordinate is the enclosure's own: the box, the retained
    // domain and the reads all move with it, and `intersect` only clipped the
    // retained domain inside that format. A degenerate enclosure is refused by
    // the description rule, so it is treated as "no reformat" here rather than
    // shifting by a rectangle that does not exist.
    const bool reformat = params.reformat && box.enclosure.width > 0 && box.enclosure.height > 0;
    const Region origin = reformat ? Region{box.enclosure.x, box.enclosure.y, 0, 0} : Region{};
    const Region domain = Region{retained.x - origin.x, retained.y - origin.y, retained.width, retained.height};
    const Region raster = cropRasterRect(request.region, scale, domain);
    const float left = box.left - static_cast<float>(origin.x);
    const float right = box.right - static_cast<float>(origin.x);
    const float top = box.top - static_cast<float>(origin.y);
    const float bottom = box.bottom - static_cast<float>(origin.y);
    const float softness = params.softness;

    const EvaluationRequest& incoming = context.inputRequests.empty() ? request : context.inputRequests[0];
    const int baseX = cropRasterBase(request.region.x, origin.x, incoming.region.x, scale);
    const int baseY = cropRasterBase(request.region.y, origin.y, incoming.region.y, scale);
    const Region dataWindow = source != nullptr ? source->dataBounds : Region{};
    const bool hasDataWindow = source != nullptr;

    CpuImage output(effectRasterLayout(context));
    const std::vector<std::string>& names = output.layout().channels;
    const std::array<int, 4> roles = output.rgbaIndices();
    std::vector<int> roleOfPlane(names.size(), -1);
    for (std::size_t role = 0; role < roles.size(); ++role) {
        if (roles[role] >= 0) {
            roleOfPlane[static_cast<std::size_t>(roles[role])] = static_cast<int>(role);
        }
    }
    std::vector<int> auxiliary(names.size(), -1);
    for (std::size_t plane = 0; plane < names.size(); ++plane) {
        if (roleOfPlane[plane] < 0) {
            auxiliary[plane] = channelIndex(input.layout().channels, names[plane]);
        }
    }
    const bool addedAlpha = roles[3] >= 0 && input.rgbaIndices()[3] < 0;
    const bool premultiplied = context.description.association == ImageAssociation::Premultiplied;
    const bool numericFade = roles[3] < 0;

    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            if (domain.width <= 0 || domain.height <= 0) {
                continue;  // an empty retained domain is a fully transparent image
            }
            const CropSample sample = cropSample(request, scale, domain, raster, x, y);
            if (!sample.inside && params.blackOutside) {
                continue;  // black outside: the raster's own zero
            }
            float weight = 1.0F;
            if (softness > 0.0F) {
                const float anchorX = static_cast<float>(sample.anchorX);
                const float anchorY = static_cast<float>(sample.anchorY);
                const float horizontal = std::clamp((anchorX - left) / softness, 0.0F, 1.0F) *
                                         std::clamp((right - anchorX) / softness, 0.0F, 1.0F);
                const float vertical = std::clamp((anchorY - top) / softness, 0.0F, 1.0F) *
                                       std::clamp((bottom - anchorY) / softness, 0.0F, 1.0F);
                weight = horizontal * vertical;
            }
            const int inputColumn = sample.column + baseX;
            const int inputRow = sample.row + baseY;
            std::array<float, 4> value = sampledPixel(input, inputColumn, inputRow);
            if (addedAlpha) {
                const int fullX = sample.anchorX + origin.x;
                const int fullY = sample.anchorY + origin.y;
                const bool covered = !hasDataWindow || (fullX >= dataWindow.x && fullY >= dataWindow.y &&
                                                        fullX < dataWindow.x + dataWindow.width &&
                                                        fullY < dataWindow.y + dataWindow.height);
                value[3] = covered ? 1.0F : 0.0F;
            }
            if (weight != 1.0F) {
                if (numericFade || premultiplied) {
                    for (float& component : value) {
                        component *= weight;
                    }
                } else {
                    value[3] *= weight;
                }
            }
            for (std::size_t plane = 0; plane < names.size(); ++plane) {
                const int role = roleOfPlane[plane];
                if (role >= 0) {
                    output.setChannel(x, y, static_cast<int>(plane), value[static_cast<std::size_t>(role)]);
                    continue;
                }
                const int incomingPlane = auxiliary[plane];
                if (incomingPlane < 0) {
                    continue;  // a plane the incoming image does not store stays numeric zero
                }
                output.setChannel(x, y, static_cast<int>(plane),
                                  input.channel(inputColumn, inputRow, incomingPlane) * weight);
            }
        }
    }
    return output;
}

std::optional<std::string> validateCropParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] { static_cast<void>(effectiveCrop(catalog, node, effectiveParams)); });
}

}  // namespace

NodeContribution cropContribution() {
    NodeContribution contribution;
    contribution.descriptor = cropDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeCrop};
    contribution.validateParameters = &validateCropParameters;
    contribution.inputRequirements = &cropInputRequirements;
    contribution.describe = &describeCrop;
    // Every stored plane moves with the crop and is clamped or blacked the same
    // way, so this node produces its whole layout itself (issue #90): the shared
    // auxiliary preservation would copy a plane at unchanged coordinates.
    contribution.ownsChannelLayout = true;
    contribution.editors = {NodeEditorContribution{
        .id = "nemo.crop.box",
        .source = "qrc:/qt/qml/Nemo/qml/CropBoxEditor.qml",
        .consumes = {"x", "y", "right", "top", "softness", "reformat", "intersect", "blackOutside"},
        .presentation = "section"}};
    return contribution;
}

}  // namespace nemo::nodes
