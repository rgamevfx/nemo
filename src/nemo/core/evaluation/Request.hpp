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

// Region of interest in image pixels. The CPU reference renders exactly this
// raster; full-frame requests are the supported path in this ticket and
// downstream resolution changes are separate concerns (spec section 8).
struct Region {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] bool operator==(const Region&) const = default;
};

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
};

}  // namespace nemo
