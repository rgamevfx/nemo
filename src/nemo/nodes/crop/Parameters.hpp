#pragma once

// Crop's typed parameter interpretation and its box geometry (issue #92,
// stories 43, 45-49).
//
// Crop's authored numbers are the reference's own: a bottom-left box, in the
// INCOMING IMAGE's coordinate frame, whose four numbers are fractional. This
// header is where that authored state becomes the two things every consumer
// needs and both executors must agree on exactly:
//
//   * the fractional box edges in the stored raster's own convention (x right,
//     y DOWN), converted through the incoming image's current format height, and
//   * the integer floor/ceil ENCLOSURE those fractional edges enclose, which is
//     the retained domain the description publishes and the support guards and
//     planner reason about.
//
// The owning network's saved canvas is not part of either: it supplies this
// box's initial VALUES at creation (the schema's creation-time rule, seeded by
// the creation owner) and nothing else, so an upstream format change moves the
// frame the authored numbers are measured in rather than silently
// reinterpreting every box against the composition.
//
// The pixel math stays with each executor: the CPU adapter and the Slang/GLSL
// kernels each decide membership, clamping and the softness ramp themselves, so
// their agreement remains evidence rather than a shared implementation. What is
// shared here is parameter interpretation and the full-resolution/raster-index
// geometry every consumer of a Region must compute the same way (the same
// lattice rule `rasterSupport` and `enforceDataWindow` apply), never a pixel
// operation.

#include <algorithm>
#include <cmath>
#include <string>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

// Crop's authored controls (issue #92). `x`/`y` are the box's bottom-left
// corner measured from the incoming image's left/bottom edges, `right`/`top` its
// top-right corner, all in the incoming image's own format space; `softness` is
// an inward ramp width in full-resolution pixels; `blackOutside` selects black
// or clamped edge extension outside the box, and is what adds an alpha channel
// to an image that stores none; `intersect` limits the retained domain to the
// incoming data window; `reformat` publishes the retained domain as the output
// format, translated to the origin.
//
// The owning network's saved canvas supplies this box's INITIAL VALUES at
// creation (issue #96, the schema's creation-time rule) and nothing else: the
// authored numbers are the reference's image-relative ones, so an upstream
// format change moves the frame they are measured in rather than silently
// reinterpreting every authored box against the composition.
struct CropParameters {
    float x{0.0F};
    float y{0.0F};
    float right{0.0F};
    float top{0.0F};
    float softness{0.0F};
    bool reformat{false};
    bool intersect{false};
    bool blackOutside{true};
};

// One authored number, or a node- and parameter-identifying failure. Crop's
// geometry is derived from all four corners and a ramp width, so a non-finite
// value cannot be floored, enclosed or compared into anything meaningful: it is
// reported where it was authored instead of silently becoming a box nobody
// asked for.
[[nodiscard]] inline float cropNumber(const NodeCatalog& catalog, const NodeInstance& node,
                                      const ParameterValues& effectiveParams, const char* key, bool nonNegative) {
    const float value = effectiveNumber(catalog, node, effectiveParams, key);
    if (!std::isfinite(value)) {
        failNode(node, std::string("parameter '") + key + "' must be finite, got " + std::to_string(value));
    }
    if (nonNegative && value < 0.0F) {
        failNode(node, std::string("parameter '") + key +
                           "' must not be negative (it is a ramp width in pixels), got " + std::to_string(value));
    }
    return value;
}

// The typed interpretation of Crop's authored state, shared by both executors
// and by authoring-time admissibility. It never reads a graph, an image or a
// network: the frame the box is converted through is supplied by the caller.
[[nodiscard]] inline CropParameters effectiveCrop(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams) {
    CropParameters crop;
    crop.x = cropNumber(catalog, node, effectiveParams, "x", false);
    crop.y = cropNumber(catalog, node, effectiveParams, "y", false);
    crop.right = cropNumber(catalog, node, effectiveParams, "right", false);
    crop.top = cropNumber(catalog, node, effectiveParams, "top", false);
    crop.softness = cropNumber(catalog, node, effectiveParams, "softness", true);
    crop.reformat = effectiveFlag(catalog, node, effectiveParams, "reformat");
    crop.intersect = effectiveFlag(catalog, node, effectiveParams, "intersect");
    crop.blackOutside = effectiveFlag(catalog, node, effectiveParams, "blackOutside");
    return crop;
}

// The height the authored box is converted through: the INCOMING image's own
// described format (the reference's box is bottom-left in the image being
// cropped), which every input description already carries normalized to origin
// (0, 0). The owning network's saved canvas supplies the box's initial VALUES at
// creation only, and is never the conversion frame: an upstream format change
// moves the frame the authored numbers are measured in instead of silently
// reinterpreting every box against the composition.
//
// A crop without an incoming image has no frame at all, so this is where the
// required-port contract is enforced for everything that needs geometry: there
// is no viewer, selection or composition to fall back to, and guessing one would
// silently place the box somewhere the author never saw.
[[nodiscard]] inline double cropFrameHeight(const NodeInstance& node, const ImageDescription* incoming) {
    if (incoming == nullptr) {
        failNode(node, "crop requires a connected image input: its bottom-left box is measured in the incoming "
                       "image's own format");
    }
    if (incoming->format.height <= 0) {
        failNode(node, "crop cannot place its bottom-left box: the incoming image reports no format height");
    }
    return static_cast<double>(incoming->format.height);
}

// The box in stored-raster conventions: fractional edges in full-resolution
// image coordinates with x growing right and y growing DOWN, plus the integer
// floor/ceil enclosure of those edges.
//
// The authored frame is the INCOMING IMAGE's own format (the reference's box is
// bottom-left in the image being cropped): `y` and `top` are distances from that
// image's BOTTOM edge, so the convert is `yDown = frameHeight - yUp`, and the
// incoming format is normalized to origin (0, 0), so `x`/`right` are already
// stored columns. The enclosure is the retained domain: a box authored on
// fractional pixels keeps every pixel it touches (its floor/ceil enclosure),
// which is the approved coordinates policy — never a silently rounded-away row
// of data. An inverted box (right < x or top < y) is normalized rather than
// rejected: it encloses exactly the same pixels read the other way.
struct CropBox {
    float left{0.0F};
    float right{0.0F};
    float top{0.0F};
    float bottom{0.0F};
    Region enclosure{};
};

[[nodiscard]] inline CropBox cropBox(const NodeInstance& node, const CropParameters& params, double frameHeight) {
    if (!std::isfinite(frameHeight) || frameHeight <= 0.0) {
        failNode(node, "crop's box is measured in the incoming image's own format, but no usable incoming format "
                       "height was available");
    }
    const double left = std::min(static_cast<double>(params.x), static_cast<double>(params.right));
    const double right = std::max(static_cast<double>(params.x), static_cast<double>(params.right));
    const double top = frameHeight - std::max(static_cast<double>(params.y), static_cast<double>(params.top));
    const double bottom = frameHeight - std::min(static_cast<double>(params.y), static_cast<double>(params.top));
    const auto representable = [](double value) {
        return std::isfinite(value) && std::abs(value) <= static_cast<double>(kMaxDescribedCoordinate);
    };
    if (!representable(left) || !representable(right) || !representable(top) || !representable(bottom)) {
        failNode(node, "parameters 'x'/'y'/'right'/'top' place the crop box outside the representable image "
                       "coordinates (|coordinate| <= " +
                           std::to_string(kMaxDescribedCoordinate) + ")");
    }
    CropBox box;
    box.left = static_cast<float>(left);
    box.right = static_cast<float>(right);
    box.top = static_cast<float>(top);
    box.bottom = static_cast<float>(bottom);
    const double firstX = std::floor(left);
    const double firstY = std::floor(top);
    const double lastX = std::ceil(right);
    const double lastY = std::ceil(bottom);
    box.enclosure = Region{static_cast<int>(firstX), static_cast<int>(firstY), static_cast<int>(lastX - firstX),
                           static_cast<int>(lastY - firstY)};
    return box;
}

// The retained domain (issue #92, story 47): the crop box's enclosure, limited
// to the incoming data window when `intersect` is enabled. Intersecting may
// leave it empty, which is a valid fully transparent image (story 49), never an
// absent one.
[[nodiscard]] inline Region cropRetained(const CropBox& box, bool intersect, const Region* incomingDataBounds) {
    if (!intersect || incomingDataBounds == nullptr) {
        return box.enclosure;
    }
    return regionIntersection(box.enclosure, *incomingDataBounds);
}

// Floor division and its ceiling counterpart for SIGNED values (C++ integer
// division truncates toward zero, which is wrong on the left/bottom half of the
// lattice). These are the same lattice rules the executors' support guards use.
[[nodiscard]] inline int cropFloorDiv(int value, int divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

[[nodiscard]] inline int cropCeilDiv(int value, int divisor) {
    return cropFloorDiv(value + divisor - 1, divisor);
}

// The half-open RASTER-INDEX rectangle of a full-resolution domain inside one
// raster: the samples whose full-resolution ANCHOR (`region.x + x*scale`) lies
// inside `domain`. This is exactly the rule the CPU guard's clear bands and the
// GPU `support` word use, so a crop's own retained domain, the samples the
// executor keeps and the samples a kernel treats as retained are one rectangle.
// A domain that no sample anchor reaches is an empty rectangle.
[[nodiscard]] inline Region cropRasterRect(const Region& region, int scale, const Region& domain) {
    if (domain.width <= 0 || domain.height <= 0 || scale <= 0) {
        return Region{};
    }
    const int width = scaledDimension(region.width, scale);
    const int height = scaledDimension(region.height, scale);
    const int left = std::clamp(cropCeilDiv(domain.x - region.x, scale), 0, width);
    const int right = std::clamp(cropCeilDiv(domain.x + domain.width - region.x, scale), 0, width);
    const int top = std::clamp(cropCeilDiv(domain.y - region.y, scale), 0, height);
    const int bottom = std::clamp(cropCeilDiv(domain.y + domain.height - region.y, scale), 0, height);
    return Region{left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

// The input raster column whose sample covers the full-resolution pixel
// `fullX`, relative to `inputRegion` at `scale`: the raster indices of two
// rasters on the same lattice differ by a constant, computed once so a kernel
// never divides per sample. With a reformat translation the output frame is
// offset from the input's, which this constant carries — the constant is only
// valid for an output column whose own anchor is the unshifted one, so the
// executors use it with the CLAMPED column and evaluate the sample's coverage
// from that clamped anchor.
[[nodiscard]] inline int cropRasterBase(int outputOrigin, int frameOffset, int inputOrigin, int scale) {
    return cropFloorDiv(outputOrigin + frameOffset - inputOrigin, scale);
}

}  // namespace nemo
