#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "nemo/core/document/Ids.hpp"

namespace nemo {

// Quality per spec section 8: reduced-quality results must not satisfy
// higher-quality requests, so quality is part of request identity.
enum class Quality { Draft, Full };

[[nodiscard]] inline const char* qualityName(Quality quality) {
    return quality == Quality::Draft ? "draft" : "full";
}

// Region of interest in FULL-RESOLUTION image pixels. The raster an
// executor produces is the region at the request's sampling scale: the CPU
// reference renders exactly ceil(region.width/samplingScale) x
// ceil(region.height/samplingScale) pixels, anchored to the full-resolution
// coordinates (spec sections 8/10.4). ROI and sampling resolution are
// distinct properties: a region-limited request preserves full-resolution
// coordinate semantics, and a sampling reduction changes pixel density, not
// the coordinate space.
struct Region {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool operator==(const Region&) const = default;
};

// Leaves room for union extents, signed differences and outward lattice
// rounding in 32-bit geometry. This is not a raster allocation limit.
inline constexpr int kMaxDescribedCoordinate = 1 << 28;

// Sampling reduction of a request (issue #11, spec section 8): 1 = Full,
// 2 = Half, 4 = Quarter image resolution. Full-resolution coordinate
// semantics are preserved at every supported scale; unsupported scales are
// a validation error, never a silent approximation.
inline constexpr int kSamplingScales[] = {1, 2, 4};

[[nodiscard]] inline bool isSamplingScale(int scale) {
    for (const int supported : kSamplingScales) {
        if (supported == scale) {
            return true;
        }
    }
    return false;
}

// Raster an executor produces for `region` at `scale`: ceil of the scaled
// dimensions, keeping every region edge covered even at odd sizes.
[[nodiscard]] inline int scaledDimension(int dimension, int scale) {
    return (dimension + scale - 1) / scale;
}

// ---------------------------------------------------------------------------
// Image-space region arithmetic (issue #85).
//
// Every executor request is anchored to the GLOBAL sampling lattice: its
// region origin is a whole number of the request's samples in full-resolution
// pixels. Two rasters of the same sampling scale are therefore offset by an
// integer number of samples, and the raster index of an absolute
// full-resolution coordinate is exactly (absolute - origin) / scale. Region
// edges are not required to be lattice multiples: a clipped edge may land
// mid-sample, and the raster covers the region with ceil edges
// (scaledDimension). Only the origin is an invariant, because it is what
// anchors a raster to absolute image space.
// ---------------------------------------------------------------------------

// Largest lattice value not above `value` (floor, valid for negative values).
[[nodiscard]] inline int latticeFloor(int value, int scale) {
    if (scale <= 0) {
        return value;
    }
    const int quotient = value >= 0 ? value / scale : -((-value + scale - 1) / scale);
    return quotient * scale;
}

// Smallest lattice value not below `value`.
[[nodiscard]] inline int latticeCeil(int value, int scale) {
    return latticeFloor(value + scale - 1, scale);
}

// True when `inner` lies entirely inside `outer` (both in full-resolution
// image coordinates).
[[nodiscard]] inline bool regionContains(const Region& outer, const Region& inner) {
    return inner.width > 0 && inner.height > 0 && inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width && inner.y + inner.height <= outer.y + outer.height;
}

// Smallest rectangle containing both regions. An empty region (non-positive
// width/height) contributes nothing.
[[nodiscard]] inline Region regionUnion(const Region& left, const Region& right) {
    if (left.width <= 0 || left.height <= 0) {
        return right;
    }
    if (right.width <= 0 || right.height <= 0) {
        return left;
    }
    const int x = std::min(left.x, right.x);
    const int y = std::min(left.y, right.y);
    return Region{x, y, std::max(left.x + left.width, right.x + right.width) - x,
                  std::max(left.y + left.height, right.y + right.height) - y};
}

// Largest rectangle contained in both regions, or canonical empty bounds.
[[nodiscard]] inline Region regionIntersection(const Region& left, const Region& right) {
    const int x = std::max(left.x, right.x);
    const int y = std::max(left.y, right.y);
    const int rightEdge = std::min(left.x + left.width, right.x + right.width);
    const int bottomEdge = std::min(left.y + left.height, right.y + right.height);
    if (rightEdge <= x || bottomEdge <= y)
        return {};
    return Region{x, y, rightEdge - x, bottomEdge - y};
}

// `region` rounded outward to whole samples of `scale` (issue #88). Signed
// coordinates are preserved exactly and the region is never clipped: an image's
// format is a description, not a storage bound, so a demand outside it — a
// negative data window, overscan, a region the caller asks for beyond the
// format — is a real, representable request for transparent black rather than
// something to discard.
[[nodiscard]] inline Region regionOnLattice(const Region& region, int scale) {
    if (scale <= 1 || region.width <= 0 || region.height <= 0) {
        return region;
    }
    Region normalized;
    normalized.x = latticeFloor(region.x, scale);
    normalized.y = latticeFloor(region.y, scale);
    normalized.width = latticeCeil(region.x + region.width, scale) - normalized.x;
    normalized.height = latticeCeil(region.y + region.height, scale) - normalized.y;
    return normalized;
}

// An evaluation request identifies the network scope as well as its output,
// local time, required region/channels, and quality (spec section 10.3).
// NetworkId defaults invalid so callers must choose the intended network
// before evaluation; output ids are local to that network.
struct EvaluationRequest {
    NetworkId network{kInvalidNetwork};
    NodeId output{kInvalidNode};
    std::int64_t localTime{0};
    Region region;
    // The named channels this request wants (issue #90). EMPTY means every
    // channel the requested image names — the default, and never "no channels";
    // an explicit list names channels exactly (no patterns, no renaming), and
    // planning re-bases an empty demand onto the described target's channels so
    // every key and per-node request carries real names. A named channel the
    // image does not carry is not an invented channel: it is transparent black
    // (zero) data, matching the frozen zero-fill policy.
    std::vector<std::string> channels{};
    Quality quality{Quality::Full};
    int samplingScale{1};
    // Full-resolution image domain, independent of ROI and sampling scale.
    // Zero/zero means region is the entire image at origin (0,0).
    // Cropped requests supply the domain explicitly. The region is signed and
    // may extend outside this domain: the domain is the image's format, while
    // the region is what the caller wants to see (issue #88). Planning re-bases
    // both on the described target format, so a caller need not know it.
    int fullWidth{0};
    int fullHeight{0};
    [[nodiscard]] int imageWidth() const { return fullWidth ? fullWidth : region.width; }
    [[nodiscard]] int imageHeight() const { return fullHeight ? fullHeight : region.height; }
    [[nodiscard]] bool operator==(const EvaluationRequest&) const = default;
};

// The normalized form of a request: full-domain identity is preserved and made
// explicit, and the region is moved outward to the enclosing image-space
// sampling lattice. Sampling scale, quality, channels and time are never
// changed, coverage is never reduced, and the region is never clipped: a signed
// demand that lies partly or wholly outside the format is exactly what the
// caller asked for (transparent black there), never silently trimmed. Executors
// report this coverage as the plan's request and deliver exactly this raster.
[[nodiscard]] inline EvaluationRequest canonicalizeRequest(const EvaluationRequest& request) {
    EvaluationRequest canonical = request;
    canonical.fullWidth = request.imageWidth();
    canonical.fullHeight = request.imageHeight();
    if (isSamplingScale(request.samplingScale)) {
        canonical.region = regionOnLattice(request.region, request.samplingScale);
    }
    return canonical;
}
}  // namespace nemo
