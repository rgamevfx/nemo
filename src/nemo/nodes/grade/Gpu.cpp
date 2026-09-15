// Grade: per-channel color correction with the common mask/mix controls
// (issue #83 node-local GPU module).
//
// Grade owns its nine-word payload (mask + seven per-channel tuples + flags)
// at set 0 binding 1 and its own GLSL reference source. The signed-power
// forward/reverse math is unchanged from issue #34; only the binding layout
// moved out of the former shared uniform structure.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/grade/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: the shared mask word, the seven per-channel tuples, and
// the flags word (enabled channels, reverse, clamps).
struct GradePayload {
    float mask[4]{};  // (maskChannel, invertMask, mix, maskPresent)
    float blackpoint[4]{};
    float whitepoint[4]{};
    float lift[4]{};
    float gain[4]{};
    float multiply[4]{};
    float offset[4]{};
    float gamma[4]{};
    float flags[4]{};  // (channels bitmask, reverse, clampBlack, clampWhite)
};
static_assert(sizeof(GradePayload) % 16 == 0);

constexpr const char* kGradeGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform GradePayload {
    // (maskChannel, invertMask, mix, maskPresent)
    vec4 mask;
    vec4 gradeBlackpoint;
    vec4 gradeWhitepoint;
    vec4 gradeLift;
    vec4 gradeGain;
    vec4 gradeMultiply;
    vec4 gradeOffset;
    vec4 gradeGamma;
    // (channels bitmask, reverse, clampBlack, clampWhite)
    vec4 gradeFlags;
};
)GLSL";

constexpr const char* kGradeGlslBody = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_mask;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

// signedPow(0, p) = 0 and sign(x)*pow(abs(x), p): the explicit
// negative/HDR extension, not a claim of bitwise Nuke parity.
float signedPow(float x, float p) {
    return x == 0.0 ? 0.0 : sign(x) * pow(abs(x), p);
}

// Forward:  y = signedPow(a*x + b, 1/gamma)
// Reverse:  x = (signedPow(y, gamma) - b) / a
// a = (gain - lift) * multiply / (whitepoint - blackpoint)
// b = lift + offset - blackpoint * a
float gradeChannel(float x, int c) {
    const int channels = int(gradeFlags.x);
    if ((channels & (1 << c)) == 0) { return x; }  // disabled: exact pass-through
    const float blackpoint = gradeBlackpoint[c];
    const float whitepoint = gradeWhitepoint[c];
    const float a = ((gradeGain[c] - gradeLift[c]) * gradeMultiply[c]) / (whitepoint - blackpoint);
    const float b = gradeLift[c] + gradeOffset[c] - blackpoint * a;
    float y;
    if (gradeFlags.y > 0.5) {
        y = (signedPow(x, gradeGamma[c]) - b) / a;  // reverse requires nonzero a (validated)
    } else {
        y = signedPow(a * x + b, 1.0 / gradeGamma[c]);
    }
    // Clamps apply after the operation, before mask/mix (Foundry default black clamp).
    if (gradeFlags.z > 0.5 && y < 0.0) { y = 0.0; }
    if (gradeFlags.w > 0.5 && y > 1.0) { y = 1.0; }
    return y;
}

vec4 gradePixel(vec4 x) {
    vec4 y = x;
    for (int c = 0; c < 4; ++c) { y[c] = gradeChannel(x[c], c); }
    return y;
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // Same-lattice inputs: the pixel with the same full-resolution sample,
    // located through each input's own raster origin.
    ivec2 mainOffset = inputGeometry[0].regionAndOffset.zw;
    ivec2 maskOffset = inputGeometry[1].regionAndOffset.zw;
    vec4 orig = imageLoad(in_main, ivec2(p) + mainOffset);
    vec4 processed = gradePixel(orig);
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        float selected = clamp(imageLoad(in_mask, ivec2(p) + maskOffset)[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel, so no HDR 0*inf cancellation occurs in mix().
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : mix(orig, processed, weight));
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

[[nodiscard]] EffectPassDefinition gradePass() {
    return EffectPassDefinition{
        .id = "grade",
        .shader = "grade/grade",
        .glsl = nemo::nodes::gpuGlsl(kGradeGlslPayload, kGradeGlslBody, true),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Input, 1}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: the shared typed Grade metadata (validated by
// the same owner the CPU reference uses) becomes this node's payload.
[[nodiscard]] GpuPreparation prepareGrade(const GpuNodeContext& context) {
    const GradeParameters grade = effectiveGrade(context.catalog, context.node, context.effectiveParams);
    const std::array<float, 4> mask =
        nemo::nodes::gpuMaskWord(context.catalog, context.node, context.effectiveParams, context.maskPresent);

    GradePayload payload;
    std::copy(mask.begin(), mask.end(), payload.mask);
    std::copy(grade.blackpoint.begin(), grade.blackpoint.end(), payload.blackpoint);
    std::copy(grade.whitepoint.begin(), grade.whitepoint.end(), payload.whitepoint);
    std::copy(grade.lift.begin(), grade.lift.end(), payload.lift);
    std::copy(grade.gain.begin(), grade.gain.end(), payload.gain);
    std::copy(grade.multiply.begin(), grade.multiply.end(), payload.multiply);
    std::copy(grade.offset.begin(), grade.offset.end(), payload.offset);
    std::copy(grade.gamma.begin(), grade.gamma.end(), payload.gamma);
    payload.flags[0] = static_cast<float>(grade.channels);
    payload.flags[1] = grade.reverse ? 1.0F : 0.0F;
    payload.flags[2] = grade.clampBlack ? 1.0F : 0.0F;
    payload.flags[3] = grade.clampWhite ? 1.0F : 0.0F;

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution gradeGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::gradeContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.grade.payload.v1";
    implementation.payloadSize = sizeof(GradePayload);
    implementation.passes = {gradePass()};
    implementation.prepare = &prepareGrade;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
