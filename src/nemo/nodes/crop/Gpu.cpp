// Crop: the native box crop's GPU module (issue #92, stories 43, 45-49).
//
// Crop owns its payload (the resolved box, its behaviour flags, the retained
// domain in both coordinate systems, the incoming data window and the
// reformat-shifted read base) at set 0 binding 1, its independent GLSL reference
// source and its Slang kernel crop.slang. It is a WINDOW operation: the output
// raster is the requested region, and every sample keeps the incoming value at
// its own coordinates, is faded by the softness ramp, or is the clamped retained
// edge sample — nothing is resampled, so there is no filter, no weight buffer
// and no scratch raster.
//
// The node OWNS its channel layout (NodeContribution::ownsChannelLayout): every
// stored plane moves and crops with the box, so the executor's common auxiliary
// preservation — which copies a plane at UNCHANGED coordinates — must not run.
// The kernel therefore writes every plane itself: the primary roles from the
// incoming image's RGBA projection, every other plane from the same-named
// incoming plane at the sample it read, both faded by the same weight.
//
// Preparation is host-side value computation only: it resolves the typed
// parameters through the same per-node geometry the CPU adapter uses
// (Parameters.hpp), converts it to raster-index facts once, and never touches a
// device. The pixel math — membership, clamping, the ramp and the channel
// policy — is written independently here, in the GLSL reference and in the CPU
// adapter, so their agreement is evidence rather than a shared mistake.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/crop/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload (issue #92). Everything is stated in the frame the OUTPUT
// raster's samples live in, except the two facts that describe the READ:
//
//   box       the fractional box edges in full-resolution output coordinates,
//             in stored-raster convention: left, right, top (y down), bottom.
//             The ramp is measured from these, so a fractional edge fades
//             exactly where it was authored.
//   flags     (softness in full-resolution pixels, premultiplied association,
//             black outside, an alpha channel added by black outside).
//   retained  the retained domain's half-open RASTER-INDEX rectangle inside this
//             pass's own raster — the samples whose full-resolution anchor lies
//             inside the domain, the same rectangle the executor's own support
//             word is derived from, so kernel and guard agree by construction.
//             An empty width or height means the domain is thinner than one
//             sample at this density (the nearest retained anchor is then read)
//             or the whole raster lies outside it.
//   domain    the retained domain in full-resolution output coordinates; an
//             empty rectangle is a fully transparent image.
//   data      the incoming image's data window in full-resolution OUTPUT
//             coordinates — what the added alpha covers.
//   base      the incoming raster column/row that output raster column/row 0
//             reads. It carries the reformat translation: with reformat the
//             output frame is the retained enclosure moved to the origin, so
//             the read is shifted back into the incoming image's frame.
struct CropPayload {
    float box[4]{};
    float flags[4]{};
    std::int32_t retained[4]{};
    std::int32_t domain[4]{};
    std::int32_t data[4]{};
    std::int32_t base[4]{};
};
static_assert(sizeof(CropPayload) % 16 == 0);

constexpr const char* kCropGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform CropPayload {
    // (left, right, top [y down], bottom) fractional box edges in
    // full-resolution output coordinates.
    vec4 cropBox;
    // (softness in full-resolution pixels, premultiplied, black outside, added
    // alpha).
    vec4 cropFlags;
    // The retained domain's half-open raster-index rectangle inside this pass's
    // raster (x, y, width, height).
    ivec4 cropRetained;
    // The retained domain in full-resolution output coordinates.
    ivec4 cropDomain;
    // The incoming image's data window in full-resolution output coordinates.
    ivec4 cropData;
    // (incoming raster column for output column 0, row for output row 0, 0, 0).
    ivec4 cropBase;
};
)GLSL";

constexpr const char* kCropGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

// Floor division for signed values: C-style integer division truncates toward
// zero, which is wrong on the left/bottom half of the lattice.
int cropFloorDiv(int value, int divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

int cropCeilDiv(int value, int divisor) {
    return cropFloorDiv(value + divisor - 1, divisor);
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    // An empty retained domain is a valid fully transparent image (story 49),
    // and every stored channel is written, so nothing is left undefined.
    if (cropDomain.z <= 0 || cropDomain.w <= 0) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight, channels.y, channels.x);
        return;
    }
    const int scale = int(meta2.z);
    // Each axis is decided independently: a retained domain can be
    // representable at this density on one axis and not on the other (a box two
    // pixels tall at quarter resolution), and the clamped edge is then the
    // nearest anchor this raster can actually hold.
    const bool haveX = cropRetained.z > 0;
    const bool haveY = cropRetained.w > 0;
    const bool insideX =
        haveX && int(p.x) >= cropRetained.x && int(p.x) < cropRetained.x + cropRetained.z;
    const bool insideY =
        haveY && int(p.y) >= cropRetained.y && int(p.y) < cropRetained.y + cropRetained.w;
    // Black outside the retained box: the raster's own zero, in every stored
    // plane. The clamped extension reads the nearest retained sample instead.
    if (!(insideX && insideY) && cropFlags.z > 0.5) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight, channels.y, channels.x);
        return;
    }
    int column = int(p.x);
    int row = int(p.y);
    if (!insideX) {
        column = haveX ? clamp(int(p.x), cropRetained.x, cropRetained.x + cropRetained.z - 1)
                       : clamp(cropCeilDiv(cropDomain.x - meta.z, scale), 0, max(0, int(meta2.x) - 1));
    }
    if (!insideY) {
        row = haveY ? clamp(int(p.y), cropRetained.y, cropRetained.y + cropRetained.w - 1)
                    : clamp(cropCeilDiv(cropDomain.y - meta.w, scale), 0, max(0, int(meta2.y) - 1));
    }
    const int anchorX = meta.z + column * scale;
    const int anchorY = meta.w + row * scale;

    // The reference vignette, in full-resolution pixels: an inward linear ramp
    // per edge, the product at corners, zero hard. Measured from the sample's
    // own anchor so Full, Half and Quarter requests agree wherever their anchors
    // coincide.
    float weight = 1.0;
    if (cropFlags.x > 0.0) {
        const float ax = float(anchorX);
        const float ay = float(anchorY);
        weight = clamp((ax - cropBox.x) / cropFlags.x, 0.0, 1.0) *
                 clamp((cropBox.y - ax) / cropFlags.x, 0.0, 1.0) *
                 clamp((ay - cropBox.z) / cropFlags.x, 0.0, 1.0) *
                 clamp((cropBox.w - ay) / cropFlags.x, 0.0, 1.0);
    }

    // The incoming sample at the retained position this output sample reads.
    // Where the incoming image holds no sample the read is transparent black
    // (issue #88); the crop never manufactures data the input does not have.
    ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    ivec2 inPixel = ivec2(column + cropBase.x, row + cropBase.y);
    bool inInside = inPixel.x >= 0 && inPixel.y >= 0 && inPixel.x < extent.x && inPixel.y < extent.y;
    vec4 value = inInside ? gpuLoadRgba(in_main, inPixel, inputGeometry[0].rgba, extent.y,
                                        inputGeometry[0].channels.y)
                          : vec4(0.0);
    // Black outside adds the reference's solid alpha covering the incoming image
    // area when the image stores none (story 48).
    if (cropFlags.w > 0.5) {
        const bool covered = anchorX >= cropData.x && anchorY >= cropData.y &&
                             anchorX < cropData.x + cropData.z && anchorY < cropData.y + cropData.w;
        value.w = covered ? 1.0 : 0.0;
    }
    // Declared-association fade: straight primary RGB fades through alpha,
    // premultiplied RGB and alpha scale together. An output with no alpha has
    // nothing to fade through, so its stored values scale numerically.
    const bool numericFade = rgba.w < 0;
    if (weight != 1.0) {
        if (numericFade || cropFlags.y > 0.5) {
            value *= weight;
        } else {
            value.w *= weight;
        }
    }

    // Every stored plane of the produced raster (this node owns its channel
    // layout): the primary roles from the projection above, every other plane
    // from the same-named incoming plane at the sample just read, scaled
    // numerically by the same weight.
    vec4 pixel = vec4(0.0);
    if (rgba.x >= 0) { gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, rgba.x, value.x); }
    if (rgba.y >= 0) { gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, rgba.y, value.y); }
    if (rgba.z >= 0) { gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, rgba.z, value.z); }
    if (rgba.w >= 0) { gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, rgba.w, value.w); }
    const int incomingChannels = int(inputGeometry[0].channels.x);
    for (uint plane = 0u; plane < channels.x; ++plane) {
        const int index = int(plane);
        if (index == rgba.x || index == rgba.y || index == rgba.z || index == rgba.w) { continue; }
        float auxiliary = 0.0;
        if (index < incomingChannels && inInside) {
            auxiliary = gpuLoadChannel(in_main, inPixel, index, extent.y, inputGeometry[0].channels.y) * weight;
        }
        gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, index, auxiliary);
    }
    gpuPixelStore(out_color, ivec2(p), channels.y, pixel);
}
)GLSL";

[[nodiscard]] EffectPassDefinition cropPass() {
    return EffectPassDefinition{
        .id = "crop",
        .shader = "crop/crop",
        .glsl = nemo::nodes::gpuGlsl(kCropGlslPayload, kCropGlslBody),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: the same typed parameters and box geometry the
// CPU adapter resolves, converted once into raster-index facts. A parameter
// problem, missing incoming geometry or an unrepresentable box names the node
// and the parameter here, before any device work.
[[nodiscard]] GpuPreparation prepareCrop(const GpuNodeContext& context) {
    const CropParameters params = effectiveCrop(context.catalog, context.node, context.effectiveParams);
    const ImageDescription* const input = context.inputDescriptions.empty() ? nullptr : context.inputDescriptions[0];
    // The box is measured in the INCOMING image's own format, exactly as the CPU
    // adapter and the description rule resolve it; the owning network's saved
    // canvas is only where the box's initial values came from at creation.
    const CropBox box = cropBox(context.node, params, cropFrameHeight(context.node, input));
    const Region retained = cropRetained(box, params.intersect, input != nullptr ? &input->dataBounds : nullptr);
    // Reformat publishes the BOX's own floor/ceil enclosure as the output format
    // and translates everything by its top-left corner (story 46). `intersect`
    // only clips the DATA bounds inside that format, so a box that misses the
    // incoming image entirely still has a valid positive format with an empty
    // data window. A degenerate box (zero enclosure) is refused by the
    // description rule, so execution never sees one.
    const bool reformat = params.reformat && box.enclosure.width > 0 && box.enclosure.height > 0;
    const Region origin = reformat ? Region{box.enclosure.x, box.enclosure.y, 0, 0} : Region{};
    const Region domain{retained.x - origin.x, retained.y - origin.y, retained.width, retained.height};
    const int scale = isSamplingScale(context.request.samplingScale) ? context.request.samplingScale : 1;
    const EvaluationRequest& incoming = context.inputRequests.empty() ? context.request : context.inputRequests[0];
    const Region dataWindow = input != nullptr ? input->dataBounds : Region{};
    const bool inputHasAlpha = input != nullptr && rgbaChannelIndices(input->channels)[3] >= 0;

    CropPayload payload;
    payload.box[0] = box.left - static_cast<float>(origin.x);
    payload.box[1] = box.right - static_cast<float>(origin.x);
    payload.box[2] = box.top - static_cast<float>(origin.y);
    payload.box[3] = box.bottom - static_cast<float>(origin.y);
    payload.flags[0] = params.softness;
    payload.flags[1] = context.description.association == ImageAssociation::Premultiplied ? 1.0F : 0.0F;
    payload.flags[2] = params.blackOutside ? 1.0F : 0.0F;
    payload.flags[3] = params.blackOutside && !inputHasAlpha ? 1.0F : 0.0F;
    const Region raster = cropRasterRect(context.request.region, scale, domain);
    payload.retained[0] = raster.x;
    payload.retained[1] = raster.y;
    payload.retained[2] = raster.width;
    payload.retained[3] = raster.height;
    payload.domain[0] = domain.x;
    payload.domain[1] = domain.y;
    payload.domain[2] = domain.width;
    payload.domain[3] = domain.height;
    payload.data[0] = dataWindow.x - origin.x;
    payload.data[1] = dataWindow.y - origin.y;
    payload.data[2] = dataWindow.width;
    payload.data[3] = dataWindow.height;
    payload.base[0] = cropRasterBase(context.request.region.x, origin.x, incoming.region.x, scale);
    payload.base[1] = cropRasterBase(context.request.region.y, origin.y, incoming.region.y, scale);

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution cropGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::cropContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.crop.payload.v1";
    implementation.payloadSize = sizeof(CropPayload);
    implementation.passes = {cropPass()};
    implementation.prepare = &prepareCrop;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
