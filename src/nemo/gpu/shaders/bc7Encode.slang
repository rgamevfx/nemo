// Production BC7 mode-6 block encoder (issue #106).
// -------------------------------------------------
// name: bc7Encode
// version: 1
//
// Encodes one completed display-referred RGBA32F frame into BC7 blocks. This
// is the viewer cache's only block encoder: no CPU encoder, no alternate codec,
// no CUDA/NVENC, and no BC7 storage-image requirement — the kernel writes
// compressed blocks into a storage buffer and the caller copies them into a
// sampleable BC7 texture and, when persistence needs them, into a host-mapped
// readback. Full float or half-float precision never passes through this
// kernel in either direction.
//
// Mode 6 is the highest-index-precision BC7 mode that carries alpha in a
// single endpoint pair: one subset, RGBAP 7.7.7.7.1 endpoints (a shared P-bit
// per endpoint, i.e. 8 effective bits per component) and sixteen 4-bit
// indices. Combined colour+alpha endpoints mean the retained alpha is
// interpolated alongside colour instead of being split into a second index
// set or spent on extra subsets, which is what makes 4-bit indices — the
// finest BC7 index precision — available for the gradients, saturated edges
// and grain this cache has to carry.
//
// Bindings (set 0):
//   binding 0 : source   RWTexture2D<float4>     — the display-referred
//              RGBA32F frame in GENERAL, the same image the live presentation
//              copy consumes. Read without a declared format, like the live
//              presentation kernel.
//   binding 1 : blocks   RWStructuredBuffer<uint4> — one 16-byte BC7 block per
//              4x4 tile in raster block order (blocksX per row), written
//              little-endian (word 0 = block bits 0..31).
//   binding 2 : Bc7Uniforms — meta = (width, height, blocksX, 0).
//
// Block layout produced here (BC7 mode 6, 128 bits, LSB-first per field):
//   bits   0..6   mode = 0000001
//   bits   7..13  R0 | 14..20 R1 | 21..27 G0 | 28..34 G1
//   bits  35..41  B0 | 42..48 B1 | 49..55 A0 | 56..62 A1
//   bit   63      P0 | 64 P1
//   bits  65..67  index 0 (its most significant bit is the subset's omitted
//                 fix-up bit, so the endpoint order is chosen to make it zero)
//   bits  68..127 indices 1..15, four bits each
// Decoding uses the D3D11 BC7 interpolation ((64 - w) * e0 + w * e1 + 32) >> 6
// with the published 4-bit weights, reproduced exactly below.
//
// dependencies:
//   region: each 4x4 block depends only on its own texels.
//   channels: R,G,B,A all written; alpha is carried, never dropped.
//   temporal: none.
// quality_modes: 1
// precision: BC7 UNORM blocks (16 bytes per 4x4 texel tile)

[[vk::binding(0, 0)]]
RWTexture2D<float4> source;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint4> blocks;

[[vk::binding(2, 0)]]
cbuffer Bc7Uniforms {
    uint4 meta;  // (raster width, raster height, blocks per row, 0)
}

// D3D11 BC7 4-bit index weights (mode 6 uses the 4-bit table).
static const uint kWeights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

// The decoder's integer interpolation, used identically so the endpoints are
// fitted against the values the hardware will actually reconstruct.
uint interpolateChannel(uint e0, uint e1, uint index)
{
    uint w = kWeights4[index];
    return ((64u - w) * e0 + w * e1 + 32u) >> 6;
}

// Quantizes one endpoint to what mode 6 stores: a single P-bit shared by all
// four components plus seven stored bits each, reconstructed as
// (stored << 1) | p. The P-bit takes the parity most components want, so each
// component lands within one 8-bit unit of its target.
uint4 quantizeEndpoint(float4 target)
{
    int4 rounded = int4(clamp(floor(clamp(target, 0.0, 255.0) + 0.5), 0.0, 255.0));
    uint odd = (uint(rounded.x) & 1u) + (uint(rounded.y) & 1u) + (uint(rounded.z) & 1u) + (uint(rounded.w) & 1u);
    uint p = (odd > 2u) ? 1u : 0u;
    int ip = int(p);
    uint4 stored = uint4(uint(clamp((rounded.x - ip + 1) >> 1, 0, 127)),
                         uint(clamp((rounded.y - ip + 1) >> 1, 0, 127)),
                         uint(clamp((rounded.z - ip + 1) >> 1, 0, 127)),
                         uint(clamp((rounded.w - ip + 1) >> 1, 0, 127)));
    return (stored << 1) | p;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width = meta.x;
    uint height = meta.y;
    uint blocksX = meta.z;
    uint2 block = tid.xy;
    if (block.x >= blocksX || block.y >= (height + 3u) / 4u)
        return;

    // Gather the 4x4 tile in the decoder's texel order (x = i & 3, y = i >> 2).
    // A block that extends past the logical raster replicates the nearest
    // inside texel: deterministic for every partial edge block, and the padded
    // texels are never presented because the presentation pass samples only
    // the logical extent.
    uint4 texel[16];
    for (uint i = 0; i < 16u; ++i) {
        uint2 p = block * 4u + uint2(i & 3u, i >> 2);
        p = min(p, uint2(width - 1u, height - 1u));
        float4 value = saturate(source[p]);
        texel[i] = uint4(clamp(floor(value * 255.0 + 0.5), 0.0, 255.0));
    }

    float4 mean = 0.0;
    float4 lo = 1e9;
    float4 hi = -1e9;
    for (uint i = 0; i < 16u; ++i) {
        mean += float4(texel[i]);
        lo = min(lo, float4(texel[i]));
        hi = max(hi, float4(texel[i]));
    }
    mean *= 1.0 / 16.0;

    // Covariance of the tile in RGBA space; the principal axis of that
    // distribution is the line the endpoint pair must span, which fits a
    // diagonal colour gradient far better than the bounding box alone.
    float c00 = 0.0, c01 = 0.0, c02 = 0.0, c03 = 0.0;
    float c11 = 0.0, c12 = 0.0, c13 = 0.0;
    float c22 = 0.0, c23 = 0.0, c33 = 0.0;
    for (uint i = 0; i < 16u; ++i) {
        float4 d = float4(texel[i]) - mean;
        c00 += d.x * d.x; c01 += d.x * d.y; c02 += d.x * d.z; c03 += d.x * d.w;
        c11 += d.y * d.y; c12 += d.y * d.z; c13 += d.y * d.w;
        c22 += d.z * d.z; c23 += d.z * d.w;
        c33 += d.w * d.w;
    }

    // Power iteration, seeded with the bounding-box diagonal and run for a
    // fixed iteration count: deterministic, with no data-dependent exit.
    float4 axis = hi - lo;
    if (dot(axis, axis) < 1e-6)
        axis = float4(1.0, 1.0, 1.0, 1.0);
    for (uint it = 0; it < 8u; ++it) {
        float4 next = float4(c00 * axis.x + c01 * axis.y + c02 * axis.z + c03 * axis.w,
                             c01 * axis.x + c11 * axis.y + c12 * axis.z + c13 * axis.w,
                             c02 * axis.x + c12 * axis.y + c22 * axis.z + c23 * axis.w,
                             c03 * axis.x + c13 * axis.y + c23 * axis.z + c33 * axis.w);
        float len = length(next);
        if (len < 1e-6)
            break;
        axis = next / len;
    }

    float4 end0 = mean;
    float4 end1 = mean;
    if (dot(hi - lo, hi - lo) > 1e-6 && length(axis) > 1e-6) {
        axis = normalize(axis);
        float tmin = 1e9;
        float tmax = -1e9;
        for (uint i = 0; i < 16u; ++i) {
            float t = dot(float4(texel[i]) - mean, axis);
            tmin = min(tmin, t);
            tmax = max(tmax, t);
        }
        end0 = clamp(mean + tmin * axis, 0.0, 255.0);
        end1 = clamp(mean + tmax * axis, 0.0, 255.0);
    }

    // Endpoint refinement: quantize, pick the best index per texel, then refit
    // the endpoints by least squares against those indices (x = w/64 is exactly
    // the parameter the decoder interpolates with) and repeat. The first round
    // searches all sixteen levels; later rounds search a +-3 window around the
    // previous choice, because the refit moves the endpoints only slightly and
    // the optimum moves with them — the same result for a third of the
    // candidate comparisons, which is GPU work this cache does not need to
    // spend while a live frame is waiting to present. The best round by
    // measured error wins, so the result is never worse than the exhaustive
    // first round.
    uint4 bestE0 = quantizeEndpoint(end0);
    uint4 bestE1 = quantizeEndpoint(end1);
    uint bestIdx[16];
    uint prevIdx[16];
    for (uint i = 0; i < 16u; ++i)
        prevIdx[i] = 0u;
    uint bestError = 0xFFFFFFFFu;
    float4 a = end0;
    float4 b = end1;
    for (uint round = 0; round < 3u; ++round) {
        uint4 qa = quantizeEndpoint(a);
        uint4 qb = quantizeEndpoint(b);
        uint4 palette[16];
        for (uint k = 0; k < 16u; ++k) {
            palette[k] = uint4(interpolateChannel(qa.x, qb.x, k), interpolateChannel(qa.y, qb.y, k),
                               interpolateChannel(qa.z, qb.z, k), interpolateChannel(qa.w, qb.w, k));
        }
        uint idx[16];
        uint error = 0u;
        for (uint i = 0; i < 16u; ++i) {
            uint first = 0u;
            uint last = 15u;
            if (round > 0u) {
                first = prevIdx[i] > 3u ? prevIdx[i] - 3u : 0u;
                last = min(prevIdx[i] + 3u, 15u);
            }
            uint bestK = first;
            uint bestE = 0xFFFFFFFFu;
            for (uint k = first; k <= last; ++k) {
                int4 d = int4(palette[k]) - int4(texel[i]);
                uint e = uint(d.x * d.x + d.y * d.y + d.z * d.z + d.w * d.w);
                if (e < bestE) {
                    bestE = e;
                    bestK = k;
                }
            }
            idx[i] = bestK;
            prevIdx[i] = bestK;
            error += bestE;
        }
        if (error < bestError) {
            bestError = error;
            bestE0 = qa;
            bestE1 = qb;
            for (uint i = 0; i < 16u; ++i)
                bestIdx[i] = idx[i];
        }

        float sx = 0.0, sxx = 0.0;
        float4 sy = 0.0, sxy = 0.0;
        for (uint i = 0; i < 16u; ++i) {
            float x = float(kWeights4[idx[i]]) * (1.0 / 64.0);
            float4 y = float4(texel[i]);
            sx += x;
            sxx += x * x;
            sy += y;
            sxy += x * y;
        }
        float denom = 16.0 * sxx - sx * sx;
        float4 slope = 0.0;
        float4 intercept = sy * (1.0 / 16.0);
        if (abs(denom) > 1e-6) {
            slope = (16.0 * sxy - sx * sy) / denom;
            intercept = (sy - slope * sx) * (1.0 / 16.0);
        }
        a = clamp(intercept, 0.0, 255.0);
        b = clamp(intercept + slope, 0.0, 255.0);
    }

    uint4 e0 = bestE0;
    uint4 e1 = bestE1;
    uint idx[16];
    for (uint i = 0; i < 16u; ++i)
        idx[i] = bestIdx[i];

    // The first texel is the single subset's fix-up index: its most significant
    // bit is not stored, so the endpoint order must make it zero. Swapping the
    // endpoints and inverting every index is exact, because the 4-bit weights
    // satisfy w[15 - i] == 64 - w[i].
    if (idx[0] > 7u) {
        uint4 swap = e0;
        e0 = e1;
        e1 = swap;
        for (uint i = 0; i < 16u; ++i)
            idx[i] = 15u - idx[i];
    }

    uint sR0 = e0.x >> 1u, sR1 = e1.x >> 1u;
    uint sG0 = e0.y >> 1u, sG1 = e1.y >> 1u;
    uint sB0 = e0.z >> 1u, sB1 = e1.z >> 1u;
    uint sA0 = e0.w >> 1u, sA1 = e1.w >> 1u;
    uint p0 = e0.x & 1u;
    uint p1 = e1.x & 1u;

    // word 0 = block bits 0..31, word 1 = 32..63, word 2 = 64..95, word 3 = 96..127.
    uint word0 = 0x40u | (sR0 << 7u) | (sR1 << 14u) | (sG0 << 21u) | ((sG1 & 0xFu) << 28u);
    uint word1 = (sG1 >> 4u) | (sB0 << 3u) | (sB1 << 10u) | (sA0 << 17u) | (sA1 << 24u) | (p0 << 31u);
    uint word2 = p1 | (idx[0] << 1u);
    for (uint i = 1u; i < 8u; ++i)
        word2 |= idx[i] << (4u * i);
    uint word3 = 0u;
    for (uint i = 8u; i < 16u; ++i)
        word3 |= idx[i] << (4u * (i - 8u));

    blocks[block.y * blocksX + block.x] = uint4(word0, word1, word2, word3);
}
