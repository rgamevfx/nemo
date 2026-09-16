// Shared native-effect binding contract, part v5 (issue #88).
// ---------------------------------------------------------
// The common request (set 0, binding 0) carries the node output's described data
// support as a half-open RASTER-INDEX rectangle; the GLSL front end declares the
// same word (nemo::nodes::kGpuRequestGlsl). Every kernel mirrors it: a sample
// outside the support is transparent black, a non-positive support size means
// the whole raster is transparent black, and (-1,-1,-1,-1) means the raster
// declares no support at all (a node-local scratch) so the kernel writes it in
// full.
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
// raster size in pixels, which bounds a neighborhood read's taps.
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

struct EffectInputGeometry {
    int4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uint4 extent;          // raster width/height, samplingScale, 1
};

[[vk::binding(2, 0)]]
StructuredBuffer<EffectInputGeometry> inputGeometry;
