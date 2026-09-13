# Rendering & Media Context

Read this before rendering, native effect execution, media decode/encode, GPU
execution, viewer cache, or performance work. Ticket bodies own current status
and scope — this file carries only the approved contracts that stay true across
them.

## Two color contracts — never conflate

- **Source-linear working images**: source decode interprets the source and
  converts to the scene-linear working space. Y′CbCr matrix/range conversion
  alone yields nonlinear R′G′B′ — matrix conversion is not linearization.
  Honor transfer, primaries, matrix, range, chroma location, bit depth, and
  project color policy; make ambiguous/unsupported interpretation explicit
  rather than guessing silently. The project's authored color-config path is
  passed per viewer session to `ViewerSession` (through
  `ViewerRuntime`/`ViewerScheduler`, including the cache-viewer path); an empty
  path keeps the existing `$OCIO` environment fallback resolved on the first
  viewing request, and no process-global environment state is mutated.
- **Display-referred replay**: viewer-cache chunks are the baked
  display-referred representation. Decode them without re-applying the
  source linearization or view transform; interpretation metadata
  (transfer and primaries included) travels inside the chunk and round-trips
  through pixel verification.

## Media import previews

A Media Bin thumbnail is display-referred, but does not use the compressed
viewer-cache representation. `MediaImportService` decodes through the existing
`ImageSource`/`VideoDecode` adapters and applies the existing OCIO viewing
transform; previews are never fed back through source linearization or
synthesized. The source-linear decode contract above still owns interpretation.
`MediaLibraryModel` requests a reduction within 160×90 bounds.

- The service owns the bounded queue: outstanding sources (queued, decoding or
  awaiting collection) are capped, a repeat submission for an outstanding source
  coalesces to the newest reference, and the caller retries at the bound. Work
  happens off the GUI thread; there are no worker callbacks.
- A result is consumed only while its identity is current: `requestId`,
  `sourceKey` and `reference.revision`, together with the viewing transforms and
  color config. A result whose source or transform moved on is reported stale
  and never committed; the commit is an explicit catalog command.
- Import polling preserves synchronous publication. The adapter finishes its
  internal cache/index updates before emitting notifications, then re-checks
  result identity after a slot may have edited or reset the project.

Issue #43 evidence: [`session.json`](../evidence/assets/issue43-media-import/session.json).

## Validate capabilities and formats — never assume

- Registration is not usability: init-verify codecs/decoders against the
  actual device and report unsupported profiles and combinations with
  clip/format/reason.
- Validate the actual decoded frame (pixel format, dimensions, plane count,
  bit depth) before touching planes. Convert supported formats deliberately;
  reject the rest with a diagnostic. No assumption of planar 8-bit 4:2:0.
- Per-frame hardware/software selection is verified against the produced
  path, and fallback reasons are reported accurately.

## Per-submission retained ownership

- Every resource referenced by a submission — images, buffers, views,
  descriptors, foreign decoder frames — stays alive until GPU completion,
  on success, timeout, cancellation, and failure alike. RAII and
  exception-safe FFmpeg ownership end to end, including AVBufferRef and
  extension-name storage tied to decoder/device lifetime.
- A retry never resets or reuses an in-flight command buffer or fence.
- Teardown drains safely; a wait timeout is never a license to free.

## Ownership boundaries (do not blur)

| Concern | Owner |
| --- | --- |
| Queue serialization (including FFmpeg sharing the application device), submission, completion identity, resource retirement | GPU execution mechanism (#22) |
| Request prioritization, publication policy, UI responsiveness | Scheduler (#13) |
| Byte budgets, admission, eviction of cached results | Cache accounting (#14) |
| Viewer-cache storage and replay orchestration | #12 |
| Media contract correctness: interpretation, format validation, FFmpeg ownership | #21 |

GPU owns the mechanism; #13 owns policy on top of it. #14 adds accounting
and admission to the retained-resource mechanism. A bug at one boundary is
filed against its owner, not patched in a neighbor.

## Execution shape

- Device-resident encode: encoders consume the device-resident viewer
  representation (interop), not CPU staging by default; a CPU staging path
  is a measured, disclosed cost, never the assumed interface.
- No per-node idle: batch compatible barriers and dispatches into
  submissions; no routine `vkDeviceWaitIdle` per node or dispatch, and no
  host wait per barrier.
- Pool and reuse (command buffers, fences, pipelines, images) only where a
  measurement shows the benefit; a reduced API-call count alone is not a
  performance win.

## Native effects — Grade, Blur, Transform

The contract below is implemented by the CPU reference `evaluateNativeEffect`
([`NativeEffects.hpp`](../../src/nemo/core/evaluation/NativeEffects.hpp) /
`NativeEffects.cpp`) and independently by the GPU path
(`src/nemo/eval/GpuExecutor.cpp` plus `src/nemo/gpu/shaders/`). The shared
metadata seam [`Params.hpp`](../../src/nemo/core/evaluation/Params.hpp)
(`effectiveEffectMask`/`effectiveGrade`/`effectiveBlur`/`effectiveTransform`)
owns typed interpretation and admissibility; independent analytic fixtures and
interpretations, not agreement between the two implementations, remain the
correctness oracle (ADR-0004, Fidelity below). Change both implementations when
a contract changes.

### Optional mask and mix (Grade, Blur, Transform)

These three effects each have a required `Image` input at port 0 and an optional
`Mask` input at port 1 (`PortSpec::optional`). An absent mask keeps its declared
plan slot as the invalid sentinel `EvaluationNodeId{}` (node == `kInvalidNode`);
the GPU binds the main image as a dummy descriptor with `maskPresent=0`, never an
allocated fallback.

`maskChannel` ∈ {none, R, G, B, A}, `invertMask`, `mix` ∈ [0, 1]. An absent mask
or `none` gives coverage 1 independent of inversion. A connected mask
contributes `coverage = clamp(selected stored channel, 0, 1)`, replaced by
`1 - coverage` when inverted. A single blend runs once, after the effect, at
output coordinates: `out = original + (processed - original) * (coverage * mix)`.
The mask is never transformed and stays in output space.

### Grade

Per enabled channel (`channels` ∈ {RGB, RGBA, R, G, B, Alpha, None}):

```text
a = (gain - lift) * multiply / (whitepoint - blackpoint)
b = lift + offset - blackpoint * a
forward:  y = signedPow(a*x + b, 1/gamma)
reverse:  x = (signedPow(y, gamma) - b) / a
```

`signedPow(0, p) = 0`, otherwise `copysign(pow(abs(x), p), x)` — the explicit
negative/HDR extension, not a claim of bitwise agreement with the
[Foundry Grade reference](https://learn.foundry.com/nuke/content/reference_guide/color_nodes/grade.html).
Disabled channels are exact pass-through. `clampBlack`/`clampWhite` clamp to 0/1
after the operation and before mask/mix. Enabled channels require finite
coefficients, `gamma > 0`, and `whitepoint != blackpoint`; reverse additionally
requires nonzero `a`. Invalid or unrepresentable settings are rejected with a
node+parameter error, never substituted. Reverse inverts only the per-channel
operation. Forward then reverse recovers the source within floating-point
tolerance for nonsingular, unclamped settings at full effect coverage. Clipping,
soft mask coverage, or `mix < 1` does not generally round-trip.

### Blur

`size` is the full-resolution support radius; `sigma = size/3`. The separable
kernel uses a tap radius `rasterSupport = ceil(size/samplingScale)`, so
`2*rasterSupport + 1` taps per axis, with weights
`exp(-0.5*(i*samplingScale/sigma)^2)` normalized per axis; borders are
clamp-to-edge. `size == 0` is exact identity. `channels` ∈ {RGBA, RGB, Alpha}:
RGBA premultiplies straight RGB for filtering and unpremultiplies once at the
end (zero alpha → zero RGB); RGB filters RGB and preserves alpha; Alpha filters
alpha and preserves RGB. Two separable passes. The CPU skips the intermediate
buffer when one raster axis is a single pixel; the GPU retains the intermediate
scratch image for every nonzero size and, at `size == 0`, records one identity
pass with no scratch. The GPU fills the normalized weights once per node
preparation into a read-only storage buffer and records the passes in one
submission with no per-node wait.

### Transform

Image-center pivot `(width/2, height/2)` in full resolution, pixel centers
`x+0.5`/`y+0.5`, x right / y down, positive `rotate` clockwise in the stored
raster. Order: uniform `scale`, then rotation about the center in physical
coordinates `(x*pixelAspect, y)`, then `translateX`/`translateY`. Each output
pixel inverse-maps and samples the input. `filter`: Nearest =
`floor(mapped/samplingScale)`; Linear bilinear; Cubic separable Catmull-Rom
`a = -0.5`. Samples outside the image are transparent black. Interpolation is
alpha-aware (premultiplied, then straight output with zero-alpha RGB = 0 and no
implicit RGB clamp). `rotate` converts through double-precision radians; cos/sin
are precomputed on the host and rounded once to float so CPU and GPU rotation
residuals agree. The mask is not transformed. Declared metadata maps the
archived prototype Transform surface: `translateX`/`translateY` ∈ [-200, 200]
step 1, `scale` ∈ [0.1, 3] step 0.001, `rotate` ∈ [-180, 180] step 0.1, and
`filter` in the Sampling section; production defaults remain the identity
transform.

### Spatial limits and capabilities

Grade is neighborhood-free and keeps `supportsRegion=true`. Blur and Transform
resample or read neighborhoods, so both declare `supportsRegion=false` and are
whole-image only: region requests are rejected by the existing capability
validation. All three are RGBA at sampling scales 1/2/4, and their spatial
parameters stay full-resolution, so coordinates are preserved at reduced scales.

The owner accepted the issue34 native-effects controls, API/node behavior and
images in chat; the #16 reference benchmark remains an open gate. Passing
checks are not a performance result or a bitwise-Nuke agreement. Session
evidence:
[`session.json`](../evidence/assets/issue34-native-effects/session.json).

## Fidelity

- Independent oracles: fixtures and reference interpretations are derived
  independently of the code under test — independently tagged gray/color
  sources, corrupt-input cases. Agreement between paths that share math
  verifies parity, not independent correctness. Nonlinear midgray is not linear.
- Prototype and test-pass are not gate evidence: #10's green prototype did
  not establish the corrected contracts (#21), and a passing test suite is
  not a substitute for the demonstrated acceptance examples in the ticket.

## Profiling discipline

Every metric states its scope and units; transfers also state bytes and direction:

- **Scope**: exactly which stages are inside the timer — CPU
  conversion/allocation/packing vs the host→device transfer vs encoder
  submission/drain. Report initialization and mux/finalization separately
  and include them in total independently reusable-chunk cost. Per-frame
  averages must name their denominator and included stages.
- **Units**: consistent ns/ms fields and rendered units that agree; bytes
  moved and host/device direction for every transfer.
- **Cold/warm**: distinguish first-use from reused resources.
- **Memory**: attribute process peak RSS separately from decoder surfaces,
  VRAM, and retained source/reference buffers — a process-wide VmHWM is not
  isolated decoder/VRAM accounting.
- Small diagnostic workloads (e.g. the 640×360 sweep) are evidence about
  themselves, not the reference gate; the integrated visible-latency
  benchmark belongs to #16. Corrected codec measurements are #23's.

## Native Linux sanitizer environment

The [issue #70 diagnosis and repair](../evidence/issue70-vulkan-lifetime.md)
records a Mesa 26.1.6 EGL query-only cleanup defect reached indirectly during
Vulkan startup, plus its source patch and reproducible process-local setup.
The repaired environment passes the full native sanitizer suite without changing
Nemo lifetimes, GPU coverage or suppressions. Installed system packages were not
replaced; an unqualified run still uses the installed graphics stack. Consult the
recorded library identities and reproducer before attributing unloaded LSan frames
to a driver or changing application ownership.
