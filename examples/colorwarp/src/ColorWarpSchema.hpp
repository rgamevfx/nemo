#pragma once

// ColorWarp mesh topology (issue #37).
//
// The authored parameter set is declared by the package's manifest.json
// (`node.parameters`); this header is the single source of the KEY NAMES and
// the stable point indexing the native implementation reads them by, so the
// manifest declaration and the execution code cannot disagree about a name.
//
// The wheel is fixed: u is periodic over [0, 12) with one cell = 1/12 turn, v
// spans [0, 4] with one cell = 1/4 radius. Knots are (h, r) for spokes
// h = 0..11 and rings r = 0..4; the three editable interior rings r = 1..3
// carry the stable index i = (r - 1) * 12 + h, and the centre r = 0 and the
// outer boundary r = 4 are fixed at zero displacement.

#include <string>

namespace colorwarp {

inline constexpr int kSpokeCount = 12;
inline constexpr int kInteriorRings = 3;
// The outer boundary ring; v = kBoundaryRing is the fixed edge of the wheel.
inline constexpr int kBoundaryRing = 4;
// Editable control points: rings 1..3 x spokes 0..11.
inline constexpr int kPointCount = kSpokeCount * kInteriorRings;

// Stable index of the control point at (spoke, ring), for an interior ring.
[[nodiscard]] constexpr int pointIndex(int spoke, int ring) noexcept {
    return (ring - 1) * kSpokeCount + spoke;
}

// Parameter keys of the two displacements. hue{i}/saturation{i} are
// DISPLACEMENTS in cell coordinates (u and v respectively), not absolute
// positions. pin{i} is UI protection on the authored point and is deliberately
// not read by this package: it is declared by manifest.json alone and never
// reaches execution.
[[nodiscard]] inline std::string hueKey(int index) {
    return "hue" + std::to_string(index);
}

[[nodiscard]] inline std::string saturationKey(int index) {
    return "saturation" + std::to_string(index);
}

}  // namespace colorwarp
