// Shared native-effect binding contract, part v6 (issues #88, #90).
// ---------------------------------------------------------
// The common request (set 0, binding 0) carries the node output's described data
// support as a half-open RASTER-INDEX rectangle; the GLSL front end declares the
// same word (nemo::nodes::kGpuRequestGlsl). Every kernel mirrors it: a sample
// outside the support is transparent black, a non-positive support size means
// the whole raster is transparent black, and (-1,-1,-1,-1) means the raster
// declares no support at all (a node-local scratch) so the kernel writes it in
// full.
//
// The request block itself is declared by each shader (the same fields the GLSL
// front end declares): meta (full width/height, signed region origin), meta2
// (LOGICAL raster width/height, samplingScale, 0), misc (localTime), support,
// channels (this raster's channel plane count, 0, 0, 0) and rgba (the plane
// index of each R, G, B, A role, -1 = role absent).
//
// Per-input spatial geometry (set 0, binding 2) of every local pass that binds
// images at set 1. The GLSL reference front end declares the identical block
// (nemo::nodes::kGpuInputGeometryGlsl); both mirror nemo::eval::
// EffectInputGeometry, which is the layout the executor uploads.
//
// One entry per bound pass input, indexed by the set-1 binding it belongs to:
// a pass input pixel is `p + regionAndOffset.zw` for pass output pixel `p`,
// because every raster in one evaluation sits on the same sampling lattice.
// `regionAndOffset.xy` is the input raster's full-resolution region origin (the
// coordinate frame a geometric resample maps back into) and `extent.xy` its
// LOGICAL raster size in pixels, which bounds a neighborhood read's taps.
//
// Region origins are SIGNED: a request may address negative or beyond-format
// coordinates (issue #88), so `meta.z`/`meta.w` in the common request are ints
// and the offsets here may be negative. A tap outside `[0, extent.xy)` is
// outside the input's data — an input may hold a different, smaller or empty
// rectangle of the same lattice — and reads as transparent black, never as an
// out-of-bounds image load.
//
// An external source frame (EffectImageKind::External, a Read's decoded media)
// carries its coverage the same way: `regionAndOffset.xy` is the coverage's
// full-resolution origin (the media's own display/data window, normalized), and
// `extent.xy` its raster extent at full resolution. `regionAndOffset.zw` is
// unused there.
//
// Native channel planes (issue #90): a native image is one R32_SFLOAT 2D image
// holding every named channel as a vertical plane — logical pixel `(x, y)` and
// channel `c` live at `(x, y + c*planeHeight)`. `rgba` (from the request for
// this pass's output, from the geometry entry for a sampled input) is the
// pre-resolved plane index of each R/G/B/A role; the projection is exactly
// nemo::CpuImage::pixel's: a missing RGB role reads 0.0, a missing A role reads
// 1.0 when the image stores at least one RGB role and 0.0 when it stores none,
// and a store writes only roles the image stores — no named RGB or alpha is
// manufactured.
//
// The channel plan (set 0, binding 3, one entry per produced plane) is resolved
// by the executor from the described channel names: >= 0 names the source plane
// in this pass's set-1 binding 0 image, -1 means this pass's own math produces
// the plane, and -2 means the plane has no source and is numeric zero. Filling
// the planned planes preserves named auxiliary channels at UNCHANGED
// coordinates. Slang has no preprocessor, so unlike the GLSL front end these
// helpers do not apply the data-support guard themselves: the caller's existing
// support early-return is the write guard, exactly as every other store in these
// kernels.

struct EffectInputGeometry {
    int4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uint4 extent;          // LOGICAL raster width/height, samplingScale, 1
    int4 rgba;             // plane index per R,G,B,A role; -1 = role absent
    uint4 channels;        // (DEVICE channel plane count, 0, 0, 0)
};

[[vk::binding(2, 0)]]
StructuredBuffer<EffectInputGeometry> inputGeometry;

[[vk::binding(3, 0)]]
StructuredBuffer<int2> channelPlan;

int2 gpuPlanePixel(int2 p, int plane, int planeHeight)
{
    return int2(p.x, p.y + plane * planeHeight);
}

float gpuLoadPlane(RWTexture2D<float> src, int2 p, int plane, int planeHeight)
{
    return src[uint2(gpuPlanePixel(p, plane, planeHeight))];
}

void gpuStorePlane(RWTexture2D<float> dst, int2 p, int plane, int planeHeight, float value)
{
    dst[uint2(gpuPlanePixel(p, plane, planeHeight))] = value;
}

// The R/G/B/A projection of one sampled input image.
float4 gpuLoadRgba(RWTexture2D<float> src, int2 p, int4 rolePlanes, int planeHeight)
{
    float4 value = float4(0.0, 0.0, 0.0, (rolePlanes.x >= 0 || rolePlanes.y >= 0 || rolePlanes.z >= 0) ? 1.0 : 0.0);
    if (rolePlanes.x >= 0) { value.x = gpuLoadPlane(src, p, rolePlanes.x, planeHeight); }
    if (rolePlanes.y >= 0) { value.y = gpuLoadPlane(src, p, rolePlanes.y, planeHeight); }
    if (rolePlanes.z >= 0) { value.z = gpuLoadPlane(src, p, rolePlanes.z, planeHeight); }
    if (rolePlanes.w >= 0) { value.w = gpuLoadPlane(src, p, rolePlanes.w, planeHeight); }
    return value;
}

// Write the roles this pass's raster actually carries; a role it does not store
// is dropped, never manufactured.
void gpuStoreRgba(RWTexture2D<float> dst, int2 p, int4 rolePlanes, int planeHeight, float4 value)
{
    if (rolePlanes.x >= 0) { gpuStorePlane(dst, p, rolePlanes.x, planeHeight, value.x); }
    if (rolePlanes.y >= 0) { gpuStorePlane(dst, p, rolePlanes.y, planeHeight, value.y); }
    if (rolePlanes.z >= 0) { gpuStorePlane(dst, p, rolePlanes.z, planeHeight, value.z); }
    if (rolePlanes.w >= 0) { gpuStorePlane(dst, p, rolePlanes.w, planeHeight, value.w); }
}

// Fill every planned plane of the produced raster from `src` (this pass's set-1
// binding 0 image) at the SAME full-resolution coordinate: `q` is that image's
// pixel for output pixel `p` and `inside` says whether it holds it. A geometric
// (resampling) kernel MUST call this with the unchanged-coordinate pixel, never
// its resampled one.
void gpuPreserveAux(RWTexture2D<float> dst, int2 p, int dstPlaneHeight, RWTexture2D<float> src, int2 q, bool inside,
                    int srcPlaneHeight, uint planeCount)
{
    for (uint plane = 0u; plane < planeCount; ++plane) {
        const int source = channelPlan[plane].x;
        if (source == -1) { continue; }  // this pass's own math produces the plane
        const float value = (source >= 0 && inside) ? gpuLoadPlane(src, q, source, srcPlaneHeight) : 0.0;
        gpuStorePlane(dst, p, int(plane), dstPlaneHeight, value);
    }
}

// The aux helper for a pass whose binding 0 image sits on the SAME sampling
// lattice as this pass's output: the source pixel is the same full-resolution
// coordinate, `p + inputGeometry[0].regionAndOffset.zw`.
void gpuPreserveAuxLattice(RWTexture2D<float> dst, int2 p, int dstPlaneHeight, RWTexture2D<float> src,
                           int srcPlaneHeight, uint planeCount)
{
    const int2 q = p + inputGeometry[0].regionAndOffset.zw;
    const int2 extent = int2(inputGeometry[0].extent.xy);
    const bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
    gpuPreserveAux(dst, p, dstPlaneHeight, src, q, inside, srcPlaneHeight, planeCount);
}

// Every plane of the produced raster at transparent black. The outside-support
// branch of a kernel writes this instead of running its pixel math, so a sample
// outside the described data window — and every auxiliary plane — is
// initialized rather than left undefined device memory. It is explicit here
// because this front end has no view of the caller's support word.
void gpuZeroPlanes(RWTexture2D<float> dst, int2 p, int planeHeight, uint planeCount)
{
    for (uint plane = 0u; plane < planeCount; ++plane) {
        gpuStorePlane(dst, p, int(plane), planeHeight, 0.0);
    }
}
