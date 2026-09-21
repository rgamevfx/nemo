#pragma once

// Roto's typed parameter interpretation and its motion-blur sampling (issue
// #93, stories 62, 65-68).
//
// Roto's controls are the reference's node-wide ones, reduced to the
// owner-approved practical set: the shapes themselves are typed document data
// (`core/document/Roto.hpp`), so this header owns only what the NODE controls:
// its global opacity, the named output channel and replacement policy, the
// optional input mask, the output clipping rule, and the node-wide motion blur
// (shutter, samples). Both executors and the authoring-time admissibility check
// read them exactly here, so no executor re-derives a default.
//
// Frozen motion-blur policy (the reference documents the controls, not the
// sampling):
//
//   * `shutter` is the exposure length in FRAMES, centered on the current
//     frame; `samples` is the number of subframe evaluations.
//   * `samples <= 1` or `shutter == 0` evaluates the current frame exactly once
//     — motion blur is disabled, not approximated.
//   * otherwise the exposure is covered by `samples` UNIFORM MIDPOINT samples
//     `time - shutter/2 + shutter*(i + 0.5)/samples`, and the COMPLETED
//     hierarchy matte of each sample is averaged (the shapes of one sample are
//     composited first, then the samples are averaged — never the other way
//     round, and never per-shape blurring).
//
// Frozen output policy (the reference documents `output`, `clip to` and
// `replace`; the details it leaves open are Nemo's, recorded in
// Contribution.cpp and roto.slang):
//
//   * `outputChannel` is the EXACT stored channel name the matte is written
//     into; every other stored channel keeps its incoming value. A disconnected
//     node produces exactly that one channel.
//   * `replace` clears the target channel's incoming value before drawing;
//     without it the matte is drawn OVER that value (the reference's "existing
//     channels are cleared to black before drawing into them").
//   * `maskChannel` names the EXACT channel of the connected mask input that
//     limits the shapes ("the roto shapes are limited to the non-black areas of
//     the mask"); `none` (the default) applies no mask at all, and `invertMask`
//     inverts the limiting coverage. An ABSENT mask input never limits anything,
//     which is the shared optional-mask contract every built-in effect keeps.
//   * `clip` restricts the produced image's data window to the incoming bbox
//     and/or the incoming format exactly as the reference documents those five
//     choices.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

enum class RotoClip { Format, Bbox, Union, Intersect, None };

struct RotoParameters {
    float shutter{0.5F};
    int samples{1};
    float opacity{1.0F};
    std::string outputChannel{"A"};
    bool replace{false};
    // Empty means "no mask channel named" (`none`).
    std::string maskChannel{};
    bool invertMask{false};
    RotoClip clip{RotoClip::Format};
};

[[nodiscard]] inline int rotoInteger(const NodeCatalog& catalog, const NodeInstance& node,
                                     const ParameterValues& effectiveParams, const char* key) {
    const auto& value = effectiveParameter(catalog, node, effectiveParams, key);
    const auto* number = std::get_if<std::int64_t>(&value);
    if (number == nullptr) {
        failNode(node,
                 std::string("parameter '") + key + "' must be an integer, got '" + parameterValueText(value) + "'");
    }
    if (*number < -2147483648LL || *number > 2147483647LL) {
        failNode(node, std::string("parameter '") + key + "' is outside the representable integer range");
    }
    return static_cast<int>(*number);
}

// The typed interpretation of Roto's authored node controls. Every refusal names
// the node and the parameter; a value that cannot be represented is never
// silently clamped into a different behaviour.
[[nodiscard]] inline RotoParameters effectiveRoto(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams) {
    RotoParameters params;
    // A shutter is an exposure LENGTH: zero closes it, and any longer exposure
    // is a legal authored value (issue #103). A negative length is refused
    // because the symmetric uniform-midpoint rule selects the identical sample
    // set for `-s`, so the sign is not a distinct effect.
    params.shutter = effectiveNumber(catalog, node, effectiveParams, "shutter");
    if (!(params.shutter >= 0.0F)) {
        failNode(node, "parameter 'shutter' must not be negative, got " + std::to_string(params.shutter));
    }
    params.samples = rotoInteger(catalog, node, effectiveParams, "samples");
    if (params.samples < 1 || params.samples > 64) {
        failNode(node, "parameter 'samples' must be within [1, 64], got " + std::to_string(params.samples));
    }
    // Opacity is a plain matte multiplier on both executors, applied AFTER the
    // hierarchy fold, so it has no coverage-algebra domain to stay inside and
    // no bound of its own (issue #103): only the finite, float-representable
    // requirement of `effectiveNumber` remains.
    params.opacity = effectiveNumber(catalog, node, effectiveParams, "opacity");
    params.outputChannel = effectiveText(catalog, node, effectiveParams, "outputChannel");
    if (params.outputChannel.empty()) {
        failNode(node, "parameter 'outputChannel' must name a channel (an empty name has no target to write)");
    }
    params.replace = effectiveFlag(catalog, node, effectiveParams, "replace");
    // `maskChannel` is the EXACT stored channel name of the mask input, with the
    // reserved word `none` meaning "no mask channel named" (the reference's
    // default). It is a name, not a four-way choice: a mask layer channel such as
    // `mask.a` is addressable by name like any other.
    const std::string mask = effectiveText(catalog, node, effectiveParams, "maskChannel");
    params.maskChannel = mask == "none" ? std::string{} : mask;
    params.invertMask = effectiveFlag(catalog, node, effectiveParams, "invertMask");
    const std::string& clip = effectiveChoice(catalog, node, effectiveParams, "clip");
    if (clip == "format") {
        params.clip = RotoClip::Format;
    } else if (clip == "bbox") {
        params.clip = RotoClip::Bbox;
    } else if (clip == "union") {
        params.clip = RotoClip::Union;
    } else if (clip == "intersect") {
        params.clip = RotoClip::Intersect;
    } else if (clip == "none") {
        params.clip = RotoClip::None;
    } else {
        failNode(node, "parameter 'clip' must be one of format, bbox, union, intersect, none, got '" + clip + "'");
    }
    return params;
}

// The subframe times one evaluation covers, oldest first (see the policy above).
// A single sample, or a closed shutter, is the current frame itself.
struct RotoSampleTimes {
    std::array<double, 64> times{};
    std::size_t count{1};
};

[[nodiscard]] inline RotoSampleTimes rotoSampleTimes(const RotoParameters& params, std::int64_t localTime) {
    RotoSampleTimes samples;
    const double current = static_cast<double>(localTime);
    if (params.samples <= 1 || params.shutter <= 0.0F) {
        samples.times[0] = current;
        samples.count = 1;
        return samples;
    }
    const double shutter = static_cast<double>(params.shutter);
    const double step = shutter / static_cast<double>(params.samples);
    const double first = current - shutter * 0.5 + step * 0.5;
    samples.count = static_cast<std::size_t>(params.samples);
    for (std::size_t index = 0; index < samples.count; ++index) {
        samples.times[index] = first + step * static_cast<double>(index);
    }
    return samples;
}

}  // namespace nemo
