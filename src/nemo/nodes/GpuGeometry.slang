// Shared native-effect binding contract, part v3 (issue #85).
// ---------------------------------------------------------
// Per-input spatial geometry (set 0, binding 2) of every local pass that binds
// images at set 1. The GLSL reference front end declares the identical block
// (nemo::nodes::kGpuInputGeometryGlsl); both mirror nemo::eval::
// EffectInputGeometry, which is the layout the executor uploads.
//
// One entry per bound pass input, indexed by the set-1 binding it belongs to:
// a pass input pixel is `p + regionAndOffset.zw` for pass output pixel `p`,
// because every raster in one evaluation sits on the same sampling lattice.
// `regionAndOffset.xy` is the input raster's full-resolution region origin
// (the coordinate frame a geometric resample maps back into) and `extent.xy`
// its raster size in pixels, which bounds a neighborhood read's taps.

struct EffectInputGeometry {
    int4 regionAndOffset;  // input region origin x/y, raster offset x/y
    uint4 extent;          // raster width/height, samplingScale, 1
};

[[vk::binding(2, 0)]]
StructuredBuffer<EffectInputGeometry> inputGeometry;
