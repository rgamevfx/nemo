#pragma once

// Reformat's typed parameter interpretation and its shared geometry/mapping math
// (issue #92, stories 50 and 52-57).
//
// Only two things live here: the typed interpretation and admissibility of the
// authored controls, and the affine map between an output full-resolution
// coordinate and the main input's own pixel coordinates. The pixel
// implementations stay independent (ADR-0004): the CPU reference
// (Contribution.cpp) and the native kernels (Gpu.cpp, reformat.slang) each
// evaluate the filter kernels themselves, so agreement between the backends
// remains evidence instead of a shared mistake.
//
// Frozen policy (Nuke 17.0 documentation where it is explicit; an explicit Nemo
// definition, recorded and reported, where the documentation is silent):
//
//   * output canvas. `format` takes the selected target: the owning network's
//     authored canvas for `composition` (never a selected source or viewer) or
//     the node's own width/height/pixel aspect for `custom`. An unforced `box`
//     derives both dimensions using the selected resize policy; `none` retains
//     the oriented input pixel dimensions (owner-approved). `forceShape` fixes
//     the requested canvas for clipping/padding. `scale` multiplies the input's
//     canvas by the two factors.
//   * rounding. Every derived or scaled dimension rounds to nearest with ties
//     up, and never below one pixel.
//   * resize. `none` places the image one source pixel per output pixel;
//     `width`/`height` match the output's physical width/height; `fit` contains
//     and `fill` covers the output in PHYSICAL aspect (an anamorphic image keeps
//     its shape); `distort` fills both axes without preserving the aspect.
//   * orientation. flop, then flip, then a 90-degree counter-clockwise turn,
//     then resize and alignment. A turn swaps the source's pixel dimensions and
//     inverts its pixel aspect, which is what keeps an anamorphic image rigid.
//   * alignment. `center` centers the placed image in the output; otherwise its
//     lower-left corner lines up with the output's lower-left corner.
//   * black outside. Off clamps every read to the input's data window (edge
//     extension: the image answers coordinates outside its data), on leaves them
//     transparent black. On also adds a solid alpha over the input image area
//     when the input carries no alpha, exactly as the reference documents.
//   * filters. Impulse is nearest; Cubic/Keys/Simon/Rifman/Mitchell/Parzen are
//     the Mitchell-Netravali BC family at (0,0)/(0,.5)/(0,.75)/(0,1)/(1/3,1/3)/
//     (1,0); Notch is a unit-width box; Lanczos4/Lanczos6 are windowed sinc with
//     radii 2/3; Sinc4 is a truncated sinc of radius 2. Every separable kernel
//     widens by the minification factor, is normalized, and enumerates its
//     support window CLOSED. Host double-precision geometry decides the window
//     once; each backend independently evaluates and normalizes its weights.
//     `clamp` bounds the result by the contributing samples' extrema, never 0..1.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo::nodes {

enum class ReformatMode { Format, Box, Scale };
enum class ReformatFormatSource { Composition, Custom };
enum class ReformatResize { None, Width, Height, Fit, Fill, Distort };
enum class ReformatFilter { Impulse, Cubic, Keys, Simon, Rifman, Mitchell, Parzen, Notch, Lanczos4, Lanczos6, Sinc4 };

// The filter choices in schema order, which is the order the descriptor
// declares and the order both front ends resolve the choice by.
inline constexpr std::string_view kReformatFilterNames[]{
    "Impulse", "Cubic", "Keys", "Simon", "Rifman", "Mitchell", "Parzen", "Notch", "Lanczos4", "Lanczos6", "Sinc4"};

// One image's logical canvas (a whole-pixel size at an authored pixel aspect).
struct ReformatCanvas {
    int width{1920};
    int height{1080};
    float pixelAspect{1.0F};

    [[nodiscard]] bool operator==(const ReformatCanvas&) const = default;
};

// Every authored control Reformat interprets, resolved once per description,
// planning and execution pass.
struct ReformatParameters {
    ReformatMode mode{ReformatMode::Format};
    ReformatFormatSource source{ReformatFormatSource::Composition};
    ReformatCanvas custom;
    ReformatCanvas box{200, 200, 1.0F};
    bool forceShape{false};
    float scaleX{1.0F};
    float scaleY{1.0F};
    ReformatResize resize{ReformatResize::Width};
    bool center{true};
    bool flip{false};
    bool flop{false};
    bool turn{false};
    ReformatFilter filter{ReformatFilter::Cubic};
    bool clamp{false};
    bool blackOutside{true};
    bool preserveBoundingBox{false};
};

// Filter support in source samples, one side of the kernel. Cubic is the
// BC(0,0) member of the family, whose support is one sample, not two.
[[nodiscard]] constexpr double reformatFilterRadius(ReformatFilter filter) {
    switch (filter) {
    case ReformatFilter::Impulse:
    case ReformatFilter::Notch:
        return 0.5;
    case ReformatFilter::Cubic:
        return 1.0;
    case ReformatFilter::Lanczos6:
        return 3.0;
    case ReformatFilter::Keys:
    case ReformatFilter::Simon:
    case ReformatFilter::Rifman:
    case ReformatFilter::Mitchell:
    case ReformatFilter::Parzen:
    case ReformatFilter::Lanczos4:
    case ReformatFilter::Sinc4:
        return 2.0;
    }
    return 2.0;
}

// Sampling windows are geometry, not filter coefficients. Resolve their integer
// boundaries in host double precision for every backend: a GPU float epsilon
// would turn a nearby non-boundary into a different Notch/Impulse sample.
// Keep the fractional position relative to an integer base so native kernels
// never subtract two large, rounded float coordinates to recover a tap offset.
struct ReformatSampleWindow {
    int first{};
    int last{};
    int base{};
    double fraction{};
    bool nearest{};
};

[[nodiscard]] inline ReformatSampleWindow reformatSampleWindow(ReformatFilter filter, double radius, double widen,
                                                               double center, const NodeInstance& node) {
    const bool nearest = filter == ReformatFilter::Impulse;
    const double support = nearest ? 0.0 : radius * widen;
    const double guard =
        8.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(center) + std::abs(support));
    const double base = std::floor(center + guard);
    const double first = nearest ? base : std::ceil(center - 0.5 - support - guard);
    const double last = nearest ? base : std::floor(center - 0.5 + support + guard);
    const double low = std::numeric_limits<int>::min();
    const double high = std::numeric_limits<int>::max() - 1.0;
    if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(base) || first < low || last > high ||
        base < low || base > high || first > last) {
        failNode(node, "reformat filter sampling window exceeds the supported integer range");
    }
    return {static_cast<int>(first), static_cast<int>(last), static_cast<int>(base), center - base, nearest};
}

// A whole-pixel dimension from a computed value: nearest, ties up, never below
// one pixel. An unrepresentable value is a node-identifying failure rather than
// a silently truncated canvas.
[[nodiscard]] inline int reformatRoundedDimension(double value, const NodeInstance& node, const std::string& what) {
    if (!std::isfinite(value) || std::abs(value) > static_cast<double>(kMaxDescribedCoordinate)) {
        failNode(node, what + " is not a representable pixel count (" + std::to_string(value) + ")");
    }
    const double rounded = std::floor(value + 0.5);
    return rounded < 1.0 ? 1 : static_cast<int>(rounded);
}

// One whole-pixel authored dimension, resolved through the shared typed seam.
[[nodiscard]] inline int reformatIntegerParameter(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams, const char* key) {
    const ParameterValue& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* number = std::get_if<std::int64_t>(&value);
    if (number == nullptr) {
        failNode(node,
                 std::string("parameter '") + key + "' must be an integer, got '" + parameterValueText(value) + "'");
    }
    return static_cast<int>(*number);
}

inline void requirePositiveDimension(const NodeInstance& node, const char* key, int value) {
    if (value < 1 || value > kMaxDescribedCoordinate) {
        failNode(node, std::string("parameter '") + key + "' must be a positive whole-pixel dimension, got " +
                           std::to_string(value));
    }
}

inline void requirePositiveAspect(const NodeInstance& node, const char* key, float value) {
    if (!std::isfinite(value) || !(value > 0.0F)) {
        failNode(node, std::string("parameter '") + key + "' must be a finite positive pixel aspect, got " +
                           std::to_string(value));
    }
}

// The typed interpretation and admissibility of Reformat's resolved parameters.
// Only the controls the selected mode consumes are constrained, so an unused
// authored value (a box size while `type` is `scale`) never rejects a valid
// setting.
[[nodiscard]] inline ReformatParameters effectiveReformat(const NodeCatalog& catalog, const NodeInstance& node,
                                                          const ParameterValues& effectiveParams) {
    ReformatParameters params;
    const std::string& mode = effectiveChoice(catalog, node, effectiveParams, "type");
    if (mode == "format") {
        params.mode = ReformatMode::Format;
    } else if (mode == "box") {
        params.mode = ReformatMode::Box;
    } else if (mode == "scale") {
        params.mode = ReformatMode::Scale;
    } else {
        failNode(node, "parameter 'type' must be one of format, box, scale, got '" + mode + "'");
    }

    const std::string& source = effectiveChoice(catalog, node, effectiveParams, "formatSource");
    if (source == "composition") {
        params.source = ReformatFormatSource::Composition;
    } else if (source == "custom") {
        params.source = ReformatFormatSource::Custom;
    } else {
        failNode(node, "parameter 'formatSource' must be one of composition, custom, got '" + source + "'");
    }

    params.custom.width = reformatIntegerParameter(catalog, node, effectiveParams, "width");
    params.custom.height = reformatIntegerParameter(catalog, node, effectiveParams, "height");
    params.custom.pixelAspect = effectiveNumber(catalog, node, effectiveParams, "pixelAspect");
    params.box.width = reformatIntegerParameter(catalog, node, effectiveParams, "boxWidth");
    params.box.height = reformatIntegerParameter(catalog, node, effectiveParams, "boxHeight");
    params.box.pixelAspect = effectiveNumber(catalog, node, effectiveParams, "boxPixelAspect");
    params.forceShape = effectiveFlag(catalog, node, effectiveParams, "forceShape");
    params.scaleX = effectiveNumber(catalog, node, effectiveParams, "scaleX");
    params.scaleY = effectiveNumber(catalog, node, effectiveParams, "scaleY");

    const std::string& resize = effectiveChoice(catalog, node, effectiveParams, "resize");
    if (resize == "none") {
        params.resize = ReformatResize::None;
    } else if (resize == "width") {
        params.resize = ReformatResize::Width;
    } else if (resize == "height") {
        params.resize = ReformatResize::Height;
    } else if (resize == "fit") {
        params.resize = ReformatResize::Fit;
    } else if (resize == "fill") {
        params.resize = ReformatResize::Fill;
    } else if (resize == "distort") {
        params.resize = ReformatResize::Distort;
    } else {
        failNode(node,
                 "parameter 'resize' must be one of none, width, height, fit, fill, distort, got '" + resize + "'");
    }

    params.center = effectiveFlag(catalog, node, effectiveParams, "center");
    params.flip = effectiveFlag(catalog, node, effectiveParams, "flip");
    params.flop = effectiveFlag(catalog, node, effectiveParams, "flop");
    params.turn = effectiveFlag(catalog, node, effectiveParams, "turn");

    const std::string& filter = effectiveChoice(catalog, node, effectiveParams, "filter");
    bool known = false;
    for (std::size_t index = 0; index < std::size(kReformatFilterNames); ++index) {
        if (filter == kReformatFilterNames[index]) {
            params.filter = static_cast<ReformatFilter>(index);
            known = true;
            break;
        }
    }
    if (!known) {
        failNode(node, "parameter 'filter' must name a declared filter, got '" + filter + "'");
    }

    params.clamp = effectiveFlag(catalog, node, effectiveParams, "clamp");
    params.blackOutside = effectiveFlag(catalog, node, effectiveParams, "blackOutside");
    params.preserveBoundingBox = effectiveFlag(catalog, node, effectiveParams, "preserveBoundingBox");

    switch (params.mode) {
    case ReformatMode::Format:
        if (params.source == ReformatFormatSource::Custom) {
            requirePositiveDimension(node, "width", params.custom.width);
            requirePositiveDimension(node, "height", params.custom.height);
            requirePositiveAspect(node, "pixelAspect", params.custom.pixelAspect);
        }
        break;
    case ReformatMode::Box:
        requirePositiveDimension(node, "boxWidth", params.box.width);
        requirePositiveDimension(node, "boxHeight", params.box.height);
        requirePositiveAspect(node, "boxPixelAspect", params.box.pixelAspect);
        break;
    case ReformatMode::Scale:
        // A scale factors a whole-pixel canvas, so zero, negatives, and a
        // reciprocal that overflows are inadmissible rather than silently
        // collapsing the output.
        if (!std::isfinite(params.scaleX) || !(params.scaleX > 0.0F) || !std::isfinite(1.0F / params.scaleX) ||
            !std::isfinite(params.scaleY) || !(params.scaleY > 0.0F) || !std::isfinite(1.0F / params.scaleY)) {
            failNode(node, "parameters 'scaleX'/'scaleY' must be finite and positive with finite reciprocals");
        }
        break;
    }
    return params;
}

// The resize policy in one place: the physical scale the control selects, turned
// into OUTPUT-pixels-per-source-pixel per axis. `pixelWidth`/`pixelHeight` and
// `pixelAspect` describe the ORIENTED source (already turned when `turn` is on).
inline void reformatPlacementScale(ReformatResize resize, const ReformatCanvas& output, double pixelWidth,
                                   double pixelHeight, double pixelAspect, double& gx, double& gy) {
    const double outWidth = static_cast<double>(output.width) * static_cast<double>(output.pixelAspect);
    const double outHeight = static_cast<double>(output.height);
    const double physicalWidth = pixelWidth * pixelAspect;
    switch (resize) {
    case ReformatResize::None:
        gx = 1.0;
        gy = 1.0;
        return;
    case ReformatResize::Width:
        gx = static_cast<double>(output.width) / pixelWidth;
        gy = outWidth / physicalWidth;
        return;
    case ReformatResize::Height:
        gy = outHeight / pixelHeight;
        gx = gy * pixelAspect / static_cast<double>(output.pixelAspect);
        return;
    case ReformatResize::Fit:
    case ReformatResize::Fill: {
        const double contain = outWidth / physicalWidth;
        const double cover = outHeight / pixelHeight;
        const double scale = resize == ReformatResize::Fit ? std::min(contain, cover) : std::max(contain, cover);
        gy = scale;
        gx = scale * pixelAspect / static_cast<double>(output.pixelAspect);
        return;
    }
    case ReformatResize::Distort:
        gx = static_cast<double>(output.width) / pixelWidth;
        gy = outHeight / pixelHeight;
        return;
    }
}
// The canvas this node produces, resolved from the mode and its target.
// `owning` is the owning network's authored canvas, required — with a
// node-identifying failure otherwise — for a `composition` target; `source` is
// the main input's logical canvas, from which the `box` and `scale` modes derive
// their dimensions.
[[nodiscard]] inline ReformatCanvas reformatOutputCanvas(const ReformatParameters& params, const ImageFormat* owning,
                                                         const ReformatCanvas& source, const NodeInstance& node) {
    ReformatCanvas output;
    // A turn swaps the source's pixel dimensions and inverts its pixel aspect, so
    // the derived dimensions describe the image the resize actually places.
    const double pixelWidth = params.turn ? static_cast<double>(source.height) : static_cast<double>(source.width);
    const double pixelHeight = params.turn ? static_cast<double>(source.width) : static_cast<double>(source.height);
    const double pixelAspect =
        params.turn ? 1.0 / static_cast<double>(source.pixelAspect) : static_cast<double>(source.pixelAspect);
    switch (params.mode) {
    case ReformatMode::Format:
        if (params.source == ReformatFormatSource::Composition) {
            if (owning == nullptr) {
                failNode(node, "reformat cannot resolve the 'composition' format without the owning network's "
                               "authored canvas");
            }
            output = ReformatCanvas{owning->width, owning->height, owning->pixelAspect};
        } else {
            output = params.custom;
        }
        break;
    case ReformatMode::Box: {
        output = params.box;
        if (!params.forceShape) {
            // The selected resize policy determines the unforced canvas, not
            // always its width. With None the oriented input dimensions survive;
            // Force shape explicitly opts into an unscaled crop/pad.
            double gx = 1.0;
            double gy = 1.0;
            reformatPlacementScale(params.resize, output, pixelWidth, pixelHeight, pixelAspect, gx, gy);
            output.width = reformatRoundedDimension(pixelWidth * gx, node, "the box width derived from resize");
            output.height = reformatRoundedDimension(pixelHeight * gy, node, "the box height derived from resize");
        }
        break;
    }
    case ReformatMode::Scale: {
        output.pixelAspect = static_cast<float>(pixelAspect);
        const double scaledWidth = pixelWidth * static_cast<double>(params.scaleX);
        const double scaledHeight = pixelHeight * static_cast<double>(params.scaleY);
        // The direction the resize control selects is the one that keeps an exact
        // whole-pixel count ("the scale factor is rounded slightly, so that the
        // output image is an integer number of pixels in the direction chosen
        // under resize type"); the other axis follows the input's aspect.
        switch (params.resize) {
        case ReformatResize::Width:
            output.width = reformatRoundedDimension(scaledWidth, node, "the scaled width");
            output.height = reformatRoundedDimension(static_cast<double>(output.width) * pixelHeight / pixelWidth, node,
                                                     "the scaled height");
            break;
        case ReformatResize::Height:
            output.height = reformatRoundedDimension(scaledHeight, node, "the scaled height");
            output.width = reformatRoundedDimension(static_cast<double>(output.height) * pixelWidth / pixelHeight, node,
                                                    "the scaled width");
            break;
        case ReformatResize::None:
        case ReformatResize::Fit:
        case ReformatResize::Fill:
        case ReformatResize::Distort:
            output.width = reformatRoundedDimension(scaledWidth, node, "the scaled width");
            output.height = reformatRoundedDimension(scaledHeight, node, "the scaled height");
            break;
        }
        break;
    }
    }
    if (output.width < 1 || output.height < 1 || output.width > kMaxDescribedCoordinate ||
        output.height > kMaxDescribedCoordinate) {
        failNode(node, "reformat resolved a non-positive output format (" + std::to_string(output.width) + "x" +
                           std::to_string(output.height) + ")");
    }
    requirePositiveAspect(node, "pixelAspect", output.pixelAspect);
    return output;
}

// The resolved geometry of one Reformat evaluation. Every field is host math
// shared by the three front ends; the filters themselves are not.
struct ReformatGeometry {
    ReformatCanvas output;
    ReformatCanvas source;
    // The main input's data window, in the source's own pixel coordinates.
    Region sourceBounds;
    // OUTPUT pixels per source pixel, per axis, under the physical-space policy
    // the resize control selected: exactly 1 is a one-to-one placement, above 1
    // magnifies and below 1 minifies (which is when the separable kernel widens).
    double gx{1.0};
    double gy{1.0};
    // The placed image's origin in output pixel coordinates (x right, y down).
    double ax{0.0};
    double ay{0.0};
    bool turn{false};
    bool flip{false};
    bool flop{false};
    ReformatFilter filter{ReformatFilter::Cubic};
    double radius{2.0};
    // Minification widening of the separable kernel, per axis (never below 1).
    double widenX{1.0};
    double widenY{1.0};
    bool clamp{false};
    bool blackOutside{true};
    // Black outside on and the input carrying no alpha: the output gains a solid
    // alpha over the input image area.
    bool solidAlpha{false};
    // The input's data window carries no samples: the result is transparent
    // everywhere, edge extension included.
    bool empty{false};
};

[[nodiscard]] inline ReformatGeometry resolveReformatGeometry(const ReformatParameters& params,
                                                              const ReformatCanvas& output,
                                                              const ReformatCanvas& source, const Region& sourceBounds,
                                                              bool sourceHasAlpha, const NodeInstance& node) {
    requirePositiveAspect(node, "pixelAspect", source.pixelAspect);
    requirePositiveAspect(node, "pixelAspect", output.pixelAspect);
    if (source.width < 1 || source.height < 1) {
        failNode(node, "the main input describes no pixel grid (" + std::to_string(source.width) + "x" +
                           std::to_string(source.height) + ")");
    }
    ReformatGeometry geometry;
    geometry.output = output;
    geometry.source = source;
    geometry.sourceBounds = sourceBounds;
    geometry.empty = sourceBounds.width <= 0 || sourceBounds.height <= 0;
    geometry.turn = params.turn;
    geometry.flip = params.flip;
    geometry.flop = params.flop;
    geometry.filter = params.filter;
    geometry.radius = reformatFilterRadius(params.filter);
    geometry.clamp = params.clamp;
    geometry.blackOutside = params.blackOutside;
    geometry.solidAlpha = params.blackOutside && !sourceHasAlpha;

    const double pixelWidth = params.turn ? static_cast<double>(source.height) : static_cast<double>(source.width);
    const double pixelHeight = params.turn ? static_cast<double>(source.width) : static_cast<double>(source.height);
    const double pixelAspect =
        params.turn ? 1.0 / static_cast<double>(source.pixelAspect) : static_cast<double>(source.pixelAspect);
    reformatPlacementScale(params.resize, output, pixelWidth, pixelHeight, pixelAspect, geometry.gx, geometry.gy);
    if (!std::isfinite(geometry.gx) || !std::isfinite(geometry.gy) || geometry.gx <= 0.0 || geometry.gy <= 0.0) {
        failNode(node, "reformat resolved a non-positive placement scale (" + std::to_string(geometry.gx) + ", " +
                           std::to_string(geometry.gy) + ")");
    }
    // A minifying axis reads more than one source sample per output sample, so
    // its separable kernel widens by exactly that factor.
    geometry.widenX = std::max(1.0, 1.0 / geometry.gx);
    geometry.widenY = std::max(1.0, 1.0 / geometry.gy);

    const double placedWidth = pixelWidth * geometry.gx;
    const double placedHeight = pixelHeight * geometry.gy;
    if (params.center) {
        geometry.ax = (static_cast<double>(output.width) - placedWidth) / 2.0;
        geometry.ay = (static_cast<double>(output.height) - placedHeight) / 2.0;
    } else {
        // "lower left corners line up": the placed image's bottom edge sits on the
        // output's bottom edge, and its left edge on the output's left edge.
        geometry.ax = 0.0;
        geometry.ay = static_cast<double>(output.height) - placedHeight;
    }
    return geometry;
}

// The source image coordinate an output full-resolution coordinate reads (the
// inverse map: output -> source).
inline void reformatMapToSource(const ReformatGeometry& geometry, double outX, double outY, double& sourceX,
                                double& sourceY) {
    const double orientedX = (outX - geometry.ax) / geometry.gx;
    const double orientedY = (outY - geometry.ay) / geometry.gy;
    double x = geometry.turn ? static_cast<double>(geometry.source.width) - orientedY : orientedX;
    double y = geometry.turn ? orientedX : orientedY;
    if (geometry.flop) {
        x = static_cast<double>(geometry.source.width) - x;
    }
    if (geometry.flip) {
        y = static_cast<double>(geometry.source.height) - y;
    }
    sourceX = x;
    sourceY = y;
}

// The output coordinate a source image coordinate lands on (the forward map:
// source -> output), used to describe the produced data window.
inline void reformatMapToOutput(const ReformatGeometry& geometry, double sourceX, double sourceY, double& outX,
                                double& outY) {
    double x = sourceX;
    double y = sourceY;
    if (geometry.flop) {
        x = static_cast<double>(geometry.source.width) - x;
    }
    if (geometry.flip) {
        y = static_cast<double>(geometry.source.height) - y;
    }
    const double orientedX = geometry.turn ? y : x;
    const double orientedY = geometry.turn ? static_cast<double>(geometry.source.width) - x : y;
    outX = geometry.ax + orientedX * geometry.gx;
    outY = geometry.ay + orientedY * geometry.gy;
}

}  // namespace nemo::nodes
