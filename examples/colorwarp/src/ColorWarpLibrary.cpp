// ColorWarp installed effect package (issue #37): the native ABI 1 entry point.
//
// This translation unit is the whole shared-library surface. It is ordinary
// trusted installed code — no Qt, no Vulkan, no host symbol, no live document —
// and it owns exactly three callbacks:
//
//   validate  the authoring/admission gate: the resolved parameters must name a
//             mesh the mapping can represent (the fold criterion, finite values,
//             payload-representable displacements) or the mesh is refused.
//   process   the CPU reference: bulk interleaved scene-linear RGBA in double
//             precision, one warp per pixel.
//   prepare   the GPU payload: the declared 592-byte layout, byte for byte.
//
// Every callback is stateless and reentrant, catches every exception, and
// reports failure as a nonzero return plus a bounded NUL-terminated diagnostic.
// The parameter object is UTF-8 JSON of resolved plain values; nlohmann is used
// privately here and never crosses the ABI.

#include "nemo/extensions/EffectAbi.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

#include "ColorWarpMath.hpp"

namespace {

using Json = nlohmann::json;

// Longest diagnostic the package writes, including the terminator. The host's
// buffer may be smaller; writeDiagnostic never exceeds it.
constexpr std::size_t kDiagnosticCapacity = 512;

// Bounded, allocation-free diagnostic write: the host's buffer is NUL
// terminated within its own capacity and never overrun.
void writeDiagnostic(char* error, std::uint32_t capacity, const char* text) noexcept {
    if (error == nullptr || capacity == 0) {
        return;
    }
    const std::size_t limit = static_cast<std::size_t>(capacity) - 1;
    std::size_t length = 0;
    if (text != nullptr) {
        while (length < limit && text[length] != '\0') {
            ++length;
        }
        std::memcpy(error, text, length);
    }
    error[length] = '\0';
}

void writeDiagnostic(char* error, std::uint32_t capacity, const std::string& text) noexcept {
    writeDiagnostic(error, capacity, text.c_str());
}

// The resolved parameters this package executes: the mesh displacements and the
// global strength. `pin{i}` is authored UI protection only and is deliberately
// not read — it never reaches execution.
struct ColorWarpSettings {
    colorwarp::ColorWarpMesh mesh;
    double strength{1.0};
};

[[nodiscard]] double readNumber(const Json& document, const std::string& key, double fallback) {
    const auto entry = document.find(key);
    if (entry == document.end()) {
        return fallback;
    }
    if (!entry->is_number()) {
        throw std::invalid_argument("parameter '" + key + "' is not a number");
    }
    return entry->get<double>();
}

// The hue axis is periodic with period kSpokeCount cells, so shifting EVERY
// control point's hue displacement by the same whole number of turns is the
// same mapping exactly — the decoded angle moves by that many turns, and turns
// are modulo one. Canonicalizing that common offset away is what keeps the
// float4 payload's phase resolvable for any authored magnitude (a float cannot
// resolve the fractional part of a huge displacement, and the CPU's double
// could, so without this the two backends would disagree), and it leaves the
// fold criterion untouched because that criterion reads only differences.
//
// The saturation displacement is NOT periodic: v spans [0, 4] and the boundary
// rings are fixed, so it is carried exactly as authored.
void canonicalizeTurns(colorwarp::ColorWarpMesh& mesh) noexcept {
    const double reference = mesh.hue[0];
    const double offset = std::floor(reference / colorwarp::kSpokeCount) * colorwarp::kSpokeCount;
    if (offset == 0.0) {
        return;
    }
    for (int index = 0; index < colorwarp::kPointCount; ++index) {
        mesh.hue[index] -= offset;
    }
}

[[nodiscard]] ColorWarpSettings parseSettings(const char* parameters) {
    if (parameters == nullptr) {
        throw std::invalid_argument("the parameter object is missing");
    }
    const Json document = Json::parse(parameters);
    if (!document.is_object()) {
        throw std::invalid_argument("the parameter object is not a JSON object");
    }
    ColorWarpSettings settings;
    settings.strength = readNumber(document, "strength", 1.0);
    for (int index = 0; index < colorwarp::kPointCount; ++index) {
        settings.mesh.hue[index] = readNumber(document, colorwarp::hueKey(index), 0.0);
        settings.mesh.saturation[index] = readNumber(document, colorwarp::saturationKey(index), 0.0);
    }
    canonicalizeTurns(settings.mesh);
    return settings;
}

// The admissibility gate, shared by all three callbacks so a mesh that was never
// validated can never be executed: the failure message, or nullopt.
[[nodiscard]] std::optional<std::string> admissible(const ColorWarpSettings& settings) {
    return colorwarp::validateMesh(settings.mesh, settings.strength);
}

int validateEffect(const char* parameters, char* error, std::uint32_t error_capacity) noexcept {
    try {
        const ColorWarpSettings settings = parseSettings(parameters);
        if (const std::optional<std::string> reason = admissible(settings)) {
            writeDiagnostic(error, error_capacity, "ColorWarp: " + *reason);
            return 1;
        }
        return 0;
    } catch (const std::exception& failure) {
        char buffer[kDiagnosticCapacity];
        std::snprintf(buffer, sizeof(buffer), "ColorWarp: %s", failure.what());
        writeDiagnostic(error, error_capacity, buffer);
        return 1;
    } catch (...) {
        writeDiagnostic(error, error_capacity, "ColorWarp: unknown failure");
        return 1;
    }
}

int processEffect(const char* parameters, const float* input, float* output, std::uint64_t pixel_count,
                  std::uint32_t flags, char* error, std::uint32_t error_capacity) noexcept {
    try {
        if (input == nullptr || output == nullptr) {
            throw std::invalid_argument("the pixel buffers are missing");
        }
        if (input == output) {
            throw std::invalid_argument("the input and output buffers must not alias");
        }
        if (pixel_count > std::numeric_limits<std::size_t>::max() / (4 * sizeof(float))) {
            throw std::invalid_argument("the pixel count does not fit an addressable buffer");
        }
        const ColorWarpSettings settings = parseSettings(parameters);
        if (const std::optional<std::string> reason = admissible(settings)) {
            writeDiagnostic(error, error_capacity, "ColorWarp: " + *reason);
            return 1;
        }
        const bool premultiplied = (flags & NEMO_EFFECT_PREMULTIPLIED) != 0;
        // The bypass bit is the host's own decision not to color-process this
        // raster — an explicit bypass, and a data-only or incomplete-RGB image,
        // whose meaning a color effect must not invent.
        const bool bypass = (flags & NEMO_EFFECT_BYPASS_COLOR) != 0;
        const std::size_t sample_count = static_cast<std::size_t>(pixel_count) * 4;
        if (bypass || colorwarp::isIdentity(settings.mesh, settings.strength)) {
            std::memcpy(output, input, sample_count * sizeof(float));
            return 0;
        }
        for (std::uint64_t index = 0; index < pixel_count; ++index) {
            const float* source = input + index * 4;
            float* destination = output + index * 4;
            const double alpha = source[3];
            // A transparent premultiplied sample keeps its own values: there is
            // no unassociated color to warp and none is invented.
            if (premultiplied && alpha == 0.0) {
                std::memcpy(destination, source, 4 * sizeof(float));
                continue;
            }
            // A sample that is not a number has no color to map: the mapping is
            // undefined there, so the result is the same not-a-number marker the
            // kernel writes — not an echoed input, not a clamped value and not a
            // substituted color. Alpha is carried, as it always is.
            if (!(std::isfinite(source[0]) && std::isfinite(source[1]) && std::isfinite(source[2]) &&
                  std::isfinite(alpha))) {
                const float marker = std::numeric_limits<float>::quiet_NaN();
                destination[0] = marker;
                destination[1] = marker;
                destination[2] = marker;
                destination[3] = source[3];
                continue;
            }
            double r = source[0];
            double g = source[1];
            double b = source[2];
            if (premultiplied) {
                r /= alpha;
                g /= alpha;
                b /= alpha;
            }
            if (!colorwarp::warpRgb(settings.mesh, settings.strength, r, g, b)) {
                // The wheel has no representable value at the mapped position,
                // or the result does not fit a finite float. The CPU path has a
                // diagnostic channel, so it refuses the deformation by name
                // instead of substituting a color, clamping, or quietly keeping
                // the sample.
                char buffer[kDiagnosticCapacity];
                std::snprintf(buffer, sizeof(buffer),
                              "ColorWarp: pixel %llu has no representable result (unrepresentable deformation)",
                              static_cast<unsigned long long>(index));
                writeDiagnostic(error, error_capacity, buffer);
                return 1;
            }
            if (premultiplied) {
                r *= alpha;
                g *= alpha;
                b *= alpha;
            }
            destination[0] = static_cast<float>(r);
            destination[1] = static_cast<float>(g);
            destination[2] = static_cast<float>(b);
            destination[3] = source[3];
        }
        return 0;
    } catch (const std::exception& failure) {
        char buffer[kDiagnosticCapacity];
        std::snprintf(buffer, sizeof(buffer), "ColorWarp: %s", failure.what());
        writeDiagnostic(error, error_capacity, buffer);
        return 1;
    } catch (...) {
        writeDiagnostic(error, error_capacity, "ColorWarp: unknown failure");
        return 1;
    }
}

int prepareEffect(const char* parameters, std::uint32_t flags, void* payload, std::uint32_t payload_bytes, char* error,
                  std::uint32_t error_capacity) noexcept {
    try {
        if (payload == nullptr) {
            throw std::invalid_argument("the payload buffer is missing");
        }
        if (payload_bytes != colorwarp::kPayloadBytes) {
            throw std::invalid_argument("the payload buffer is " + std::to_string(payload_bytes) + " bytes; the " +
                                        "manifest declares " + std::to_string(colorwarp::kPayloadBytes));
        }
        const ColorWarpSettings settings = parseSettings(parameters);
        if (const std::optional<std::string> reason = admissible(settings)) {
            writeDiagnostic(error, error_capacity, "ColorWarp: " + *reason);
            return 1;
        }
        colorwarp::ColorWarpPayload words;
        for (int index = 0; index < colorwarp::kPointCount; ++index) {
            words.control[index][0] = static_cast<float>(settings.mesh.hue[index]);
            words.control[index][1] = static_cast<float>(settings.mesh.saturation[index]);
        }
        // Only the ABI's defined flag bits are carried in the settings word, as
        // exact small integers in a float.
        words.settings[0] = static_cast<float>(settings.strength);
        words.settings[1] = static_cast<float>(flags & (NEMO_EFFECT_PREMULTIPLIED | NEMO_EFFECT_BYPASS_COLOR));
        words.settings[2] = colorwarp::isIdentity(settings.mesh, settings.strength) ? 1.0F : 0.0F;
        std::memcpy(payload, &words, sizeof(words));
        return 0;
    } catch (const std::exception& failure) {
        char buffer[kDiagnosticCapacity];
        std::snprintf(buffer, sizeof(buffer), "ColorWarp: %s", failure.what());
        writeDiagnostic(error, error_capacity, buffer);
        return 1;
    } catch (...) {
        writeDiagnostic(error, error_capacity, "ColorWarp: unknown failure");
        return 1;
    }
}

const NemoEffectV1 kColorWarpEffect{NEMO_EFFECT_ABI_VERSION, static_cast<std::uint32_t>(sizeof(NemoEffectV1)),
                                    &validateEffect, &processEffect, &prepareEffect};

}  // namespace

extern "C" NEMO_EFFECT_EXPORT const NemoEffectV1* nemo_effect_v1(void) {
    return &kColorWarpEffect;
}
