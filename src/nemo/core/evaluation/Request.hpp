#pragma once

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

// An evaluation request identifies an output, local time, required
// region/channels, and quality (spec section 10.3). Local time is
// composition-local: parent-scope time mapping happens through Timing before
// a request is formed, so evaluation never depends on panel selection.
struct EvaluationRequest {
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
}  // namespace nemo
