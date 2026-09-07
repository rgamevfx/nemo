#pragma once

#include <cstdint>
#include <cmath>

namespace nemo {

// Composition-local time <-> parent time mapping (spec section 4.3).
// Moving a composition clip changes parentStart only; internal animation and
// timing stay fixed because this mapping is the only coupling.
struct Timing {
    std::int64_t parentStart{0}; // parent time at which composition-local time is zero
    double rate{1.0};            // parent frames per composition frame

    [[nodiscard]] std::int64_t toParent(std::int64_t local) const {
        return parentStart + std::llround(static_cast<double>(local) * rate);
    }

    [[nodiscard]] std::int64_t toLocal(std::int64_t parent) const {
        return std::llround(static_cast<double>(parent - parentStart) / rate);
    }
};

// A source sampled inside a composition: retiming an internal source changes
// sampling before downstream operations (spec section 4.3 table).
struct SourceTiming {
    std::int64_t sourceOffset{0}; // composition local time zero reads this source frame
    double rate{1.0};             // source frames per composition frame

    [[nodiscard]] std::int64_t toSource(std::int64_t local) const {
        return sourceOffset + std::llround(static_cast<double>(local) * rate);
    }
};

} // namespace nemo
