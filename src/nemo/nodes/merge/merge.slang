// Nemo native effect contract (spec section 10.4, issue #8; operations and
// masking per issue #75; contribution-local module per issue #83)
// ---------------------------------------------------------
// name: merge
// version: 2
// ports:
//   - in_a : image2D input, port A (background / base)
//   - in_b : image2D input, port B (foreground / source)
//   - in_mask : image2D input, optional port 2 (mask; absent binds a valid
//     dummy descriptor and reports maskPresent = 0, never a manufactured
//     source)
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - operation = "over" | "plus" | "multiply" | "screen" | "difference"
//     (over is the default; an unknown value is rejected by the schema and
//     by both executors, never a silent fallback). The code travels in
//     op.x: 0 Over, 1 Plus, 2 Multiply, 3 Screen, 4 Difference.
//   - mask = (maskChannel, invertMask, mix, maskPresent), the shared effect
//     mask word.
// bindings (native binding contract v2, see testpattern.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.merge.payload.v1")
//   set 1, binding 0 = A, binding 1 = B, binding 2 = mask — swapping A/B is a
//     different result and fails declared image comparison
//   set 2, binding 0 : output image2D
// dependencies:
//   region: output pixel (x, y) depends only on inputs (x, y); no
//     neighborhood reads, so no support-region expansion.
//   channels: RGBA uniform.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight (non-premultiplied), identical to the CPU reference.
//   Over keeps its existing expression exactly: out.rgb = fg.a*fg.rgb +
//   (1 - fg.a)*bg.rgb. The extended modes interpolate background -> blend
//   target by foreground alpha: Plus target bg+fg, Multiply bg*fg, Screen
//   1-(1-bg)*(1-fg), Difference |bg-fg|. Alpha is op-independent:
//   out.a = fg.a + (1 - fg.a)*bg.a. Scene-linear RGB is never clamped; the
//   final result is bg + coverage*mix*(composite - bg) with coverage 1 when
//   the mask is absent or None. Premultiplied interpretation is a declared
//   comparison failure, not an alternate path.

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0) — issue #11
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
};

[[vk::binding(1, 0)]]
cbuffer MergePayload {
    // (maskChannel, invertMask, mix, maskPresent)
    float4 mask;
    // x = operation code
    float4 op;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_a;
[[vk::binding(1, 1)]]
RWTexture2D<float4> in_b;
// Optional mask slot (port 2). An absent mask binds a valid dummy
// descriptor with maskPresent = 0, so the Load is always in bounds.
[[vk::binding(2, 1)]]
RWTexture2D<float4> in_mask;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    float4 bg = in_a.Load(p);   // port A: background (base)
    float4 fg = in_b.Load(p);   // port B: foreground (source)

    int operation = int(op.x);
    float4 composite;
    if (operation == 0) {
        // Over keeps the CPU reference's existing expression exactly.
        composite.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    } else {
        float3 target;
        if (operation == 1) { target = bg.xyz + fg.xyz; }
        else if (operation == 2) { target = bg.xyz * fg.xyz; }
        else if (operation == 3) { target = float3(1.0, 1.0, 1.0) - (float3(1.0, 1.0, 1.0) - bg.xyz) * (float3(1.0, 1.0, 1.0) - fg.xyz); }
        else { target = abs(bg.xyz - fg.xyz); }   // Difference
        composite.xyz = bg.xyz + fg.a * (target - bg.xyz);
    }
    // Alpha is operation-independent, as in current Over.
    composite.w = fg.a + (1.0 - fg.a) * bg.a;

    // Shared mask/mix coverage: absent mask or None has full coverage
    // regardless of inversion; otherwise the clamped selected stored channel
    // is optionally inverted. Scene-linear RGB is never clamped.
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        float selected = clamp(in_mask.Load(int2(p))[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the background, weight 1 the
    // unmasked composite, so Mix 0 or zero coverage returns the background.
    float weight = coverage * mask.z;
    out_color[p] = weight <= 0.0 ? bg : (weight >= 1.0 ? composite : lerp(bg, composite, weight));
}
