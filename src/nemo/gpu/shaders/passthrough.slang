// Nemo effect contract (spec section 10.4)
// ----------------------------------------
// name: passthrough
// version: 1
// ports:
//   - in_color  : image2D  input   (float4, RGBA, premultiplied-alpha, linear color interpretation)
//   - out_color : image2D  output  (float4, RGBA, premultiplied-alpha, matches input format)
// parameters: none
// dependencies:
//   region: output pixel (x, y) depends only on input pixel (x, y); no neighborhood reads.
//   channels: all channels processed uniformly; no channel cross-reads.
//   temporal: none; result is a pure function of the input image.
// quality_modes: 1 (single full-quality path)
//
// Trivial identity pass-through: out[y][x] = in[y][x]. Compile target SPIR-V,
// compute stage. This shader exists to exercise build-time Slang -> SPIR-V
// compilation (issue #3); execution and image-tolerance validation are the
// native execution gate (issue #8).

Texture2D<float4> in_color;
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    out_color[tid.xy] = in_color.Load(int3(tid.xy, 0));
}
