#pragma once

// ColorWarp wheel math (issue #37), CPU reference in double precision.
//
// The color contract is scene-linear and exact:
//
//   luma Y = .2126 R + .7152 G + .0722 B
//   opponent u = (2R - G - B)/sqrt(6), v = (G - B)/sqrt(2)
//   chroma c = hypot(u, v), scale s = 1 + |Y|
//   radius r = c/(s + c), hue = atan2(v, u)/(2pi) mod 1
//
// The mesh lives in cell coordinates: U = 12*hue in [0, 12) (one cell = 1/12
// turn) and V = 4*radius in [0, 4] (one cell = 1/4 radius). hue{i}/saturation{i}
// are DISPLACEMENTS in those coordinates, so the mapping is
//
//   (U, V) -> (U, V) + strength * displacementAt(U, V)
//
// with cubic smoothstep (t*t*(3-2t)) tensor weights inside each cell, which is
// C1 across every cell edge and periodic in U. Decoding is the exact inverse of
// the encoding above: the mapped position is turned back into a radius and a
// hue and recombined with the SAME luma through the orthonormal opponent basis.
//
// Two numerical properties are deliberate, because the naive form of the
// inverse is not usable:
//
//   * radius and its complement are BOTH computed from the same denominator,
//     and the inverse uses the complement (s/(s+c)) directly. c/(s+c) rounds to
//     exactly 1 for a large finite chroma, so `1 - radius` cancels away the
//     whole chroma and `s*radius/(1-radius)` would return an infinity for a
//     perfectly finite input; s/(s+c) stays exact where radius saturates.
//   * a pixel whose interpolated displacement is exactly zero — the neutral
//     axis, and every point of the fixed boundary rings — is returned
//     UNCHANGED, so the identity is bit-exact instead of a round trip through
//     the opponent basis.
//
// Nothing here clamps: negatives, HDR values and an alpha of zero are all
// carried through the math as they are. Values that the wheel cannot represent
// (a mapped complement at or below zero) are reported, never substituted.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "ColorWarpSchema.hpp"

namespace colorwarp {

// Rec. 709 luma weights and the orthonormal opponent basis they define.
inline constexpr double kLumaR = 0.2126;
inline constexpr double kLumaG = 0.7152;
inline constexpr double kLumaB = 0.0722;
inline constexpr double kInvSqrt6 = 0.4082482904638631;       // 1/sqrt(6)
inline constexpr double kInvSqrt2 = 0.7071067811865476;       // 1/sqrt(2)
inline constexpr double kSqrtTwoThirds = 0.8164965809277260;  // sqrt(2/3)
inline constexpr double kTwoPi = 6.2831853071795865;
inline constexpr double kInvTwoPi = 0.15915494309189535;  // 1/(2pi)

// The fold criterion: 1.5*(max du + max dv) < 0.95 per cell and component.
inline constexpr double kFoldBound = 0.95;
// Smoothstep's maximum derivative, the factor the per-cell differences are
// scaled by to bound the Jacobian perturbation of the interpolated field.
inline constexpr double kSmoothstepMaxSlope = 1.5;

// The largest finite float. The payload carries displacements as float4 control
// words and the CPU result is stored as float, so a value beyond this is not
// representable by the contract: the package refuses it instead of letting
// either backend run an infinity.
inline constexpr double kMaxFiniteFloat = 3.4028234663852886e38;  // FLT_MAX

// Authored deformation of the mesh, in cell coordinates.
struct ColorWarpMesh {
    double hue[kPointCount]{};
    double saturation[kPointCount]{};
};

struct Displacement {
    double u{0.0};
    double v{0.0};
};

// One knot's authored displacement. The centre (ring 0) and the outer boundary
// (ring kBoundaryRing) are fixed at zero by the contract; spokes wrap.
[[nodiscard]] inline double knotComponent(const ColorWarpMesh& mesh, int spoke, int ring, int component) noexcept {
    if (ring <= 0 || ring >= kBoundaryRing) {
        return 0.0;
    }
    const int wrapped = ((spoke % kSpokeCount) + kSpokeCount) % kSpokeCount;
    const int index = pointIndex(wrapped, ring);
    return component == 0 ? mesh.hue[index] : mesh.saturation[index];
}

[[nodiscard]] inline Displacement knotDisplacement(const ColorWarpMesh& mesh, int spoke, int ring) noexcept {
    return Displacement{knotComponent(mesh, spoke, ring, 0), knotComponent(mesh, spoke, ring, 1)};
}

[[nodiscard]] inline double smoothstep(double t) noexcept {
    return t * t * (3.0 - 2.0 * t);
}

// The displacement field: tensor (smoothstep-weighted bilinear) interpolation of
// one cell's four knots, so the field is C1 across every edge. `u` is expected
// in [0, 12) and `v` in [0, 4]; v is used as given — the caller's own encoding
// produces that range, and the fixed boundary ring is the identity there, so no
// value clamp is needed or wanted.
[[nodiscard]] inline Displacement displacementAt(const ColorWarpMesh& mesh, double u, double v) noexcept {
    const int cellU = static_cast<int>(std::floor(u));
    const int cellV = std::min(static_cast<int>(std::floor(v)), kBoundaryRing - 1);
    const double su = smoothstep(u - std::floor(u));
    const double sv = smoothstep(v - cellV);
    const Displacement bottomLeft = knotDisplacement(mesh, cellU, cellV);
    const Displacement bottomRight = knotDisplacement(mesh, cellU + 1, cellV);
    const Displacement topLeft = knotDisplacement(mesh, cellU, cellV + 1);
    const Displacement topRight = knotDisplacement(mesh, cellU + 1, cellV + 1);
    const double w0 = (1.0 - su) * (1.0 - sv);
    const double w1 = su * (1.0 - sv);
    const double w2 = (1.0 - su) * sv;
    const double w3 = su * sv;
    return Displacement{w0 * bottomLeft.u + w1 * bottomRight.u + w2 * topLeft.u + w3 * topRight.u,
                        w0 * bottomLeft.v + w1 * bottomRight.v + w2 * topLeft.v + w3 * topRight.v};
}

// The fold bound of one cell (spoke, ring) for one displacement component:
//
//   1.5 * ( max(|right-bottom - left-bottom|, |right-top - left-top|)
//         + max(|left-top - left-bottom|, |right-top - right-bottom|) )
//
// A cell whose bound is not below kFoldBound may fold. Every cell and both
// components must pass; because the interpolation weights are a convex
// combination, this is also a global bound on the Jacobian perturbation of the
// whole field, so a passing mesh maps the wheel injectively onto itself (the
// fixed boundary rings keep the domain's edge in place, so the injective map is
// a bijection and a mapped radius can never reach 1).
[[nodiscard]] inline double foldBound(const ColorWarpMesh& mesh, int spoke, int ring, int component) noexcept {
    const double leftBottom = knotComponent(mesh, spoke, ring, component);
    const double rightBottom = knotComponent(mesh, spoke + 1, ring, component);
    const double leftTop = knotComponent(mesh, spoke, ring + 1, component);
    const double rightTop = knotComponent(mesh, spoke + 1, ring + 1, component);
    const double duBottom = std::fabs(rightBottom - leftBottom);
    const double duTop = std::fabs(rightTop - leftTop);
    const double dvLeft = std::fabs(leftTop - leftBottom);
    const double dvRight = std::fabs(rightTop - rightBottom);
    return kSmoothstepMaxSlope * (std::fmax(duBottom, duTop) + std::fmax(dvLeft, dvRight));
}

// The whole authored deformation's admissibility: the failure message, or
// nullopt when the resolved parameters are admissible. `strength` is the
// resolved strength, which the mapping applies to the perturbation, so the
// schema's own [0, 1] range is part of the guarantee and is re-checked here.
[[nodiscard]] inline std::optional<std::string> validateMesh(const ColorWarpMesh& mesh, double strength) {
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return "strength " + std::to_string(strength) + " is outside the declared range [0, 1]";
    }
    for (int index = 0; index < kPointCount; ++index) {
        const double hue = mesh.hue[index];
        const double saturation = mesh.saturation[index];
        if (!std::isfinite(hue) || !std::isfinite(saturation)) {
            return "point " + std::to_string(index) + " has a non-finite displacement";
        }
        if (std::fabs(hue) > kMaxFiniteFloat || std::fabs(saturation) > kMaxFiniteFloat) {
            return "point " + std::to_string(index) + " displacement is not representable in the payload";
        }
    }
    for (int ring = 0; ring < kBoundaryRing; ++ring) {
        for (int spoke = 0; spoke < kSpokeCount; ++spoke) {
            for (int component = 0; component < 2; ++component) {
                const double bound = foldBound(mesh, spoke, ring, component);
                if (!(bound < kFoldBound)) {
                    return "mesh folds: cell (spoke " + std::to_string(spoke) + ", ring " + std::to_string(ring) +
                           ") " + (component == 0 ? "hue" : "saturation") + " Jacobian bound " + std::to_string(bound) +
                           " is not below " + std::to_string(kFoldBound);
                }
            }
        }
    }
    return std::nullopt;
}

// True when the deformation is the identity everywhere: every displacement is
// zero, or strength is zero.
[[nodiscard]] inline bool isIdentity(const ColorWarpMesh& mesh, double strength) noexcept {
    if (strength == 0.0) {
        return true;
    }
    for (int index = 0; index < kPointCount; ++index) {
        if (mesh.hue[index] != 0.0 || mesh.saturation[index] != 0.0) {
            return false;
        }
    }
    return true;
}

// Warp one straight (non-premultiplied) scene-linear RGB triple in place.
//
// Returns true when r, g and b hold the warped result — and when the pixel's
// interpolated displacement is exactly zero (the neutral axis, and every point
// of the fixed boundary rings) they are left as the input, so the identity is
// bit-exact instead of a round trip through the opponent basis.
//
// Returns false when the mapped position cannot be represented: the mapped
// complement reached zero or below (which the fold criterion makes unreachable
// for an admissible mesh, because the mapping is then a bijection of the wheel
// onto itself), or the result does not fit a finite float. r, g and b are then
// UNTOUCHED and the caller refuses the deformation — a diagnostic on the CPU
// path, and the unrepresentable marker on the kernel, which has no error
// channel. Nothing is ever substituted, clamped, or silently kept.
//
// The stable complement below is what keeps the first case numerically
// avoidable rather than a rounding artifact: `c/(s+c)` saturates at 1 for a
// large finite chroma, so `s*radius/(1-radius)` would divide by zero for a
// perfectly representable input. The second case is a genuine representability
// limit: the fold criterion bounds the field's variation, not its magnitude, so
// it cannot bound the result's size.
[[nodiscard]] inline bool warpRgb(const ColorWarpMesh& mesh, double strength, double& r, double& g,
                                  double& b) noexcept {
    const double luma = kLumaR * r + kLumaG * g + kLumaB * b;
    const double opponentU = (2.0 * r - g - b) * kInvSqrt6;
    const double opponentV = (g - b) * kInvSqrt2;
    const double chroma = std::hypot(opponentU, opponentV);
    if (chroma == 0.0) {
        return true;  // the neutral axis is fixed: no invented hue
    }
    const double scale = 1.0 + std::fabs(luma);
    const double total = scale + chroma;
    double hue = std::atan2(opponentV, opponentU) * kInvTwoPi;  // (-0.5, 0.5]
    if (hue < 0.0) {
        hue += 1.0;  // [0, 1)
    }
    const double cellU = hue * kSpokeCount;                 // [0, 12)
    const double cellV = (chroma / total) * kBoundaryRing;  // [0, 4)
    const Displacement displacement = displacementAt(mesh, cellU, cellV);
    if (displacement.u == 0.0 && displacement.v == 0.0) {
        return true;  // exact identity, not a round trip
    }
    // 1 - mapped radius, from the stable complement rather than from 1 - radius.
    const double complement = scale / total - strength * displacement.v / kBoundaryRing;
    if (!(complement > 0.0)) {
        return false;
    }
    const double mappedChroma = scale * (1.0 - complement) / complement;
    double mappedTurns = (cellU + strength * displacement.u) / kSpokeCount;
    mappedTurns -= std::floor(mappedTurns);  // [0, 1): the hue axis is periodic
    const double mappedU = mappedChroma * std::cos(kTwoPi * mappedTurns);
    const double mappedV = mappedChroma * std::sin(kTwoPi * mappedTurns);
    const double q0 = kSqrtTwoThirds * mappedU;
    const double q1 = -mappedU * kInvSqrt6 + mappedV * kInvSqrt2;
    const double q2 = -mappedU * kInvSqrt6 - mappedV * kInvSqrt2;
    // The same luma is restored exactly: the correction is orthogonal to the
    // opponent plane, so the mapping never changes Y.
    const double correction = luma - (kLumaR * q0 + kLumaG * q1 + kLumaB * q2);
    const double warpedR = q0 + correction;
    const double warpedG = q1 + correction;
    const double warpedB = q2 + correction;
    const double magnitude = std::fmax(std::fabs(warpedR), std::fmax(std::fabs(warpedG), std::fabs(warpedB)));
    if (!(std::isfinite(magnitude) && magnitude <= kMaxFiniteFloat)) {
        return false;
    }
    r = warpedR;
    g = warpedG;
    b = warpedB;
    return true;
}

// The declared GPU payload layout: one float4 per editable control point in
// stable index order — (hue displacement, saturation displacement, 0, 0) — then
// one settings float4 (strength, flags, identity, 0).
inline constexpr std::uint32_t kPayloadBytes = 592;

struct ColorWarpPayload {
    float control[kPointCount][4]{};
    float settings[4]{};
};

static_assert(sizeof(ColorWarpPayload) == kPayloadBytes, "the payload layout is declared by the manifest");

}  // namespace colorwarp
