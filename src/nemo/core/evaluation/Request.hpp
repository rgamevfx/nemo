#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

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

// Smallest rectangle containing both regions.
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

// `region` rounded outward to whole samples of `scale` and clipped to the
// image domain. Rounded outward first so the rounded result stays inside the
// domain, then clipped last so a domain edge that is not a lattice multiple
// (e.g. a 30 pixel image at sampling scale 4) remains reachable.
[[nodiscard]] inline Region regionOnLattice(const Region& region, int scale, int domainWidth, int domainHeight) {
    Region normalized;
    normalized.x = std::clamp(latticeFloor(region.x, scale), 0, std::max(0, domainWidth));
    normalized.y = std::clamp(latticeFloor(region.y, scale), 0, std::max(0, domainHeight));
    const int right = std::clamp(latticeCeil(region.x + region.width, scale), normalized.x, std::max(0, domainWidth));
    const int bottom =
        std::clamp(latticeCeil(region.y + region.height, scale), normalized.y, std::max(0, domainHeight));
    normalized.width = right - normalized.x;
    normalized.height = bottom - normalized.y;
    return normalized;
}

// The image domain a request addresses, as a region at the request's own
// sampling lattice.
[[nodiscard]] inline Region domainRegion(int domainWidth, int domainHeight) {
    return Region{0, 0, std::max(0, domainWidth), std::max(0, domainHeight)};
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
    std::string channels{"RGBA"};
    Quality quality{Quality::Full};
    int samplingScale{1};
    // Full-resolution image domain, independent of ROI and sampling scale.
    // Zero/zero means region is the entire image at origin (0,0).
    // Cropped requests supply the domain explicitly.
    int fullWidth{0};
    int fullHeight{0};
    [[nodiscard]] int imageWidth() const { return fullWidth ? fullWidth : region.width; }
    [[nodiscard]] int imageHeight() const { return fullHeight ? fullHeight : region.height; }
    [[nodiscard]] bool operator==(const EvaluationRequest&) const = default;
};

// The normalized form of a request: full-domain identity is preserved and made
// explicit, and the region is moved outward to the enclosing image-space
// sampling lattice (clipped to the domain). Sampling scale, quality, channels
// and time are never changed, and coverage is never reduced. Executors report
// this coverage as the plan's request and deliver exactly this raster.
[[nodiscard]] inline EvaluationRequest canonicalizeRequest(const EvaluationRequest& request) {
    EvaluationRequest canonical = request;
    const int width = request.imageWidth();
    const int height = request.imageHeight();
    canonical.fullWidth = width;
    canonical.fullHeight = height;
    if (isSamplingScale(request.samplingScale)) {
        canonical.region = regionOnLattice(request.region, request.samplingScale, width, height);
    }
    return canonical;
}
}  // namespace nemo
