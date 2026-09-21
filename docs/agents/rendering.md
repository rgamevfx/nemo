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

### Input color (an input transform is not a viewing transform)

- One owner resolves an encoded source's RGB interpretation
  (`media::resolveInputColor`, retained per decode session by
  `media::InputColorCache`): a Read's explicit OCIO input color space wins,
  Raw/Data bypasses transfer+gamut conversion, and otherwise the file/stream
  declaration wins over the fill-only interpretation hints (node scope, else
  the shared reference), then the project config's own file rule, then an error
  naming the offending relationship. Consumers never re-derive that precedence.
  Core's `applyReadInterpretationHints` is the one owner of the fill-only hint
  merge (node scope first, else the shared reference); it also accepts an empty
  map, so an unbound Read's probe carries authored hints before any reference
  exists. Raw/Data bypasses the RGB transfer/gamut half only: the mandatory
  Y′CbCr matrix, range and chroma-location decode still applies, so a stream
  that declares no matrix is refused naming that relationship until the fields
  are authored rather than guessed.
- `OcioInputTransform` (CPU) and `buildInputTransformGpu` (device pass, no
  readback) convert encoded RGB into the working space; the working target is
  validated by MEANING — a linear response measured against the pinned Rec.709
  gamut matrix — never by its name or a role (in the pinned ACES config the
  `scene_linear` role is ACEScg).
- Alpha is normalized in the ENCODED domain before any nonlinear conversion;
  a premultiplied zero-alpha pixel becomes a deterministic zero RGB and a
  straight source keeps its valid hidden RGB. Raw/Data changes neither samples
  nor association and is reported as `ColorInterpretation::Data`, never as
  managed scene-linear.
- Core resolves the request, media resolves the color.
  `resolveSourceRequest` (`src/nemo/core/evaluation/SourceRequest.hpp/.cpp`)
  combines a Read's node-scoped mapping, coverage choice, boundary/missing
  policy, alpha and authored color choices with the shared reference and its
  committed facts into one plain `EffectiveSourceRequest`; the
  `SourceProvider::frame` seam receives that resolved request, so no provider
  re-derives mapping, precedence or coverage (see "Ownership boundaries").
- A source node's reuse key mixes the OCIO config CONTENT identity
  (`OcioConfigSnapshot::identity()`: the resolved reference plus
  `Config::getCacheID`) through the provider's `colorConfigIdentity()`, so a
  same-path config edit or a source-content reload can never serve a stale
  decode; dependents inherit the key through their inputs. Nothing polls for a
  content change: the identity and the retained processors are replaced at the
  explicit refresh boundary (`refreshColorConfig` on project replacement or a
  deliberate config reload), and a decode already running keeps its own
  generation alive.
- Decoder reuse identity is frame-INDEPENDENT (`appendSourceDecodeIdentity`):
  media identity, effective interpretation and color context, never the frame,
  so successive frames of one clip reuse one decoder while a changed
  interpretation or configuration can never reuse another's.
  `appendEffectiveSourceIdentity` adds the frame-specific mapping, policies and
  outcome on top of that decode identity for result reuse.

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
- A request carries its own color choices and lifecycle identity, not just a
  path: the merged `InputColorChoice`, the frozen admitted frame and
  `projectGeneration` travel with `MediaImportRequest`, and `ProbeAlignment`
  chooses between the established mapped frame and one discovered available
  member for a fresh sequence selection. The worker therefore never reads
  `Document` state and never re-derives node/shared precedence, and retained
  worker color state is keyed on the project generation, so reopening or
  replacing a project cannot reuse the previous project's color context even
  when the config reference and working space are unchanged.

- A result carries its classified `MediaKind` (`Image` for a still, `Sequence`
  for a discovered numbered run, `Video` for a clip) alongside the validated
  probe. A Read lifecycle command commits that kind explicitly, so a movie keeps
  its container kind and interval; the only still conversion is the Read
  controller's explicit Sequence-vs-Single-Image choice for a numbered run.

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
| Effective source request: node-vs-shared mapping, coverage and policy resolution | Core `SourceRequest` (#79), consumed by every provider/session/inspector |
| Encoded RGB interpretation and the retained OCIO conversion | Media input-color owner (#81): `resolveInputColor`/`InputColorCache`, `OcioConfigSnapshot` |

GPU owns the mechanism; #13 owns policy on top of it. #14 adds accounting
and admission to the retained-resource mechanism. A bug at one boundary is
filed against its owner, not patched in a neighbor. `SourceRequest` decides
*what frame and policy*; the media input-color owner decides *what the samples
mean*; neither re-implements the other.

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

## Native effects — node-local execution

The independent CPU, Slang and retained GLSL implementations live in
`src/nemo/nodes/{grade,blur,transform,merge,shuffle,premult,unpremult,crop,reformat,roto}/`.
Each module contributes schema and CPU execution in `Contribution.cpp`, native
payload/pass preparation and GLSL in `Gpu.cpp`, and its Slang kernel(s).
Module-local `Parameters.hpp` owns typed effect interpretation; shared
[`Params.hpp`](../../src/nemo/core/evaluation/Params.hpp)
owns generic reads, effective animation, channels and `effectiveEffectMask`.
The shared evaluators consume immutable contributions and retain traversal,
request validation, source mapping, reuse and GPU resource ownership.
Independent analytic fixtures, not agreement between implementations, remain
the correctness oracle (ADR-0004, Fidelity below). Change the independent
implementations together when a numerical contract changes.

The internal native binding contract is version 8
([ADR-0008](../decisions/0008-built-in-node-contributions.md#boundaries)).
Use `gpu/ChannelImage.hpp` for physical storage: four stored channels are packed
RGBA32F; other counts are scalar R32_SFLOAT planes. Names/order remain intact.
Input geometry carries the actual format's components and resolved role indices;
canonical RGBA uses direct vector loads/stores, while reordered and auxiliary
channels use the general path. Scalar copies use logical plane height.
Only complete primary/root RGB receives color conversion; alpha-only and
auxiliary data bypass it. Identity packed viewer/replay images are retained
directly; other views use device-side projection. Shuffle uses these same
owners and the exact mapping/failure policies in ADR-0008.
`GpuPreparation` supplies owned payload/weight/geometry values, local passes and
per-scratch coverage. Image-sampling kernels address signed producer coverage,
not the output raster's dimensions; final writes respect described data support.
Header description is independent of decode and GPU setup. Allocation, barriers,
submission and retirement stay in `GpuExecutor` and the existing GPU owners. See
[ADR-0008](../decisions/0008-built-in-node-contributions.md) and
[`ownership.md`](ownership.md#add-a-node-or-effect).

Roto contributes typed shape geometry through set 4, with submission-retained
storage owned by the executor. Its hierarchy, signed feather, temporal sampling,
channel/mask and bounded-refusal policies are recorded in
[ADR-0008](../decisions/0008-built-in-node-contributions.md#roto-geometry-and-matte-policy-93).
Use those contracts rather than introducing a node-local allocator, alpha
conversion or export path; #89 and the delivery owner retain those gaps.

### Optional mask and mix (Grade, Blur, Transform, Merge)

These effects each have a required `Image` input at port 0 and an optional
`Mask` input (`PortSpec::optional`): port 1 for Grade/Blur/Transform, port 2 for
Merge (its port 1 is the required foreground). The executor resolves the
optional slot from the declared schema, not a hardcoded index. An absent mask
keeps its declared plan slot as the invalid sentinel `EvaluationNodeId{}`
(node == `kInvalidNode`); the GPU binds the main image as a dummy descriptor
with `maskPresent=0`, never an allocated fallback.

`maskChannel` ∈ {none, R, G, B, A}, `invertMask`, and finite `mix` (soft
navigation 0–1, not a hard range). An absent mask or `none` gives coverage 1
independent of inversion. A connected mask
contributes `coverage = clamp(selected stored channel, 0, 1)`, replaced by
`1 - coverage` when inverted. A single blend runs once, after the effect, at
output coordinates: `out = original + (processed - original) * (coverage * mix)`.
The mask is never transformed and stays in output space.
Mix outside 0–1 extrapolates on CPU, Slang and GLSL; only exactly zero/one
take endpoint shortcuts. Transform retains original data support whenever
Mix differs from one, including extrapolation.

### Grade

Per enabled channel (`channels` ∈ {RGB, RGBA, R, G, B, Alpha, None}), when
Blackpoint and Whitepoint differ:

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
coefficients and `gamma > 0`; nonsingular Reverse additionally requires nonzero
`a`. These domains are explicitly retained by owner approval in #103.
Invalid or unrepresentable settings remain node+parameter diagnostics.
Reverse inverts only the per-channel operation. Forward then reverse recovers
the source within floating-point tolerance for nonsingular, unclamped settings
at full effect coverage. Clipping, soft mask coverage, or `mix != 1` does not
generally round-trip.

At exact equality (`blackpoint == whitepoint`, no epsilon band), #103 defines
the shared endpoint as the range-mapped value. Forward computes
`signedPow((gain-lift)*multiply*blackpoint + lift + offset, 1/gamma)`.
Reverse outputs the shared endpoint directly, not a fabricated inverse.
Both then apply the same clamps and Mask/Mix; unselected channels pass through.
Neutral equal RGB endpoints 0.5 therefore yield scene-linear RGB 0.5, not a
promise about display gray. Forward multiplication by zero remains valid.

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
The radius is finite and nonnegative with no authored upper bound; 0–100 is
navigation only. Execution checks the actual support's payload/index
representation and widens halo arithmetic before checked coordinate conversion.
`GpuNodeContext::maxStorageBufferBytes` carries the existing device capacity
into preparation, so an oversized weights table is refused before host
allocation; the GPU executor retains final admission and completion ownership.
The CPU reference has an explicitly owner-approved **64 MiB weights-table
budget** (#103), checked before allocation and tap loops. This is not an image
memory budget or an authored maximum; radius 250 needs only 501 floats
(2004 bytes). Very expensive requests within either resource budget can still
be slow. CPU and native weight calculations remain independent.

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
archived prototype Transform surface as SOFT adjustment travel
(`softMinimum`/`softMaximum`): `translateX`/`translateY` ±200 step 1 sharing one
`Translate` row, `scale` soft [0.1, 3] step 0.001, `rotate` soft ±180 step 0.1,
and `filter` in the Sampling section. Soft travel is a navigation hint:
typing, scrubbing and stepping can all cross it without clamping or quantizing
the authored value. Scale must be finite and nonzero with a finite reciprocal;
a negative scale reverses both axes about the pivot. Production defaults remain
the identity transform. Reformat's positive scale factors are different:
they multiply output canvas dimensions rather than apply this signed map.

### Merge

Merge has two required `Image` inputs and one optional `Mask` input: port A
(index 0) is the foreground/source, port B (index 1) the background/main pipe,
and port 2 the optional mask. Merge declares `NodeDescriptor::mainInput = 1`:
format, pixel aspect, interpretation and auxiliary channels follow B; bounds
remain the union of A and B. Automatic graph insertion connects through B.
The GPU pass binds B, A, mask locally so its main image stays at binding 0.
This owner-approved correction (#78, 2026-09-19) intentionally changes old
graphs' interpretation without rewiring saved ports or adding compatibility
behavior. CPU pixels live in
`nodes/merge/Contribution.cpp`; the GPU path uses `nodes/merge/merge.slang`
and independent GLSL in `nodes/merge/Gpu.cpp`. Its local `MergePayload` carries
the operation in `op.x`, separate from the common request uniforms. Both resolve
`operation` through the module's `effectiveMergeOperation` and mask controls
through the shared `effectiveEffectMask`.

`operation` ∈ {over, plus, multiply, screen, difference}, default `over`. An
unknown value is rejected by the descriptor on authoring and deserialize and by
both executors with the supported set named — never a fallback.

For each RGB channel the unmasked composite forms a blend target and
interpolates from the background by foreground alpha:

```text
over        target = fg
plus        target = bg + fg
multiply    target = bg * fg
screen      target = 1 - (1 - bg) * (1 - fg)
difference  target = |bg - fg|
composite.rgb = bg + fg.a * (target - bg)     # Over keeps fg.a*fg + (1-fg.a)*bg
composite.a   = fg.a + (1 - fg.a) * bg.a      # operation-independent
```

The shared mask/mix blend then runs once: `out = bg + coverage * mix *
(composite - bg)`, so `mix` 0 or zero coverage returns the background exactly and
full coverage/mix returns the selected composite. Scene-linear RGB is never
clamped; HDR and negative values pass through, and partially transparent inputs
use the same expression.

Independent oracle: `bg = (0.2, 0.4, 0.6, 0.4)`, `fg = (0.8, 0.5, 0.25, 0.5)`
gives alpha `0.5 + 0.5*0.4 = 0.7` for every operation and unmasked RGB over
`(0.5, 0.45, 0.425)`, plus `(0.6, 0.65, 0.725)`, multiply `(0.18, 0.3, 0.375)`,
screen `(0.52, 0.55, 0.65)`, difference `(0.4, 0.25, 0.475)`. With mask A `0.3`
and `mix = 0.5` the weight is `0.15`, so masked Over is
`(0.245, 0.4075, 0.57375, 0.445)`.

Swap A/B (`swapInputsCommand`, exposed as the Swap A/B action and the
`swap-inputs` session command) exchanges the two sources as one atomic undo
step, retaining the mask connection, the parameters and the node layout; a
rejected swap changes neither connection.

### Spatial limits and capabilities

Grade, Merge, Blur and Transform support regional requests at sampling scales
1/2/4. Their spatial parameters remain full-resolution. The shared dependency
planner anchors coverage to the image-wide sampling lattice and combines
per-port requirements across consumers. Blur supplies its halo; Transform
supplies inverse/filter bounds plus original mix coverage. Unknown source
dimensions fail description explicitly rather than borrowing a network canvas.
Clip headers without PAR retain the established square-pixel default (owner
approval in #88); explicit header PAR is preserved.
Masks are read in output coordinates. A contribution declaring
`supportsRegion=false` processes the whole domain internally and still serves
regional consumers. See ADR-0007 for coverage reuse and its rectangular limit.

### Described images and the retained-edge-domain claim (issue #92)

A described image states what the image IS independently of the raster that
backs it: `format` (the logical rectangle), `dataBounds` (the signed rectangle
that holds data) and `edgeExtension`. The default `edgeExtension = false` is the
finite support every existing producer has — a sample outside `dataBounds` is
transparent black — and each executor enforces that centrally (CPU
`enforceDataWindow`, native output support) rather than in node pixel math.

`edgeExtension = true` is the producer's explicit claim that its FINITE
`dataBounds` are the RETAINED edge domain instead of the support of the image:
the effect answers requested coordinates outside them with real data of its own
(Crop or Reformat with black-outside disabled), so no consumer may
treat them as transparent. `hasEdgeExtension()` is the one predicate every guard
reads, and it also requires a NON-EMPTY retained domain: an empty `dataBounds`
stays fully transparent whatever the flag says, because an image that holds
nothing has no edge to extend.
When a contribution grows coverage by unioning inputs or adding constants, it
must copy the input's **effective** `hasEdgeExtension()` claim before changing
the bounds. Copying a raw true flag from an empty input and then making the
bounds non-empty would revive a nonexistent edge; a downstream offset could
then create out-of-domain pixels. Merge and Shuffle normalize at this boundary.

The claim is shared behavior, not a node-local switch:

- The CPU support guard stops clearing for such a description, and the native
  node-output pass declares NO support (`{-1,-1,-1,-1}`), so the whole produced
  raster is the effect's own output. Scratch passes are unchanged.
- The planner keeps the explicitly demanded coverage: a whole-frame-only
  extended producer's coverage is its retained domain UNIONED with the demand
  rather than the domain alone, while padding stays limited to the retained
  domain, so an ordinary region request never escalates to the whole frame.
- `requirementDomain` — the shared helper a node's own read rule clips its
  demand with — returns the caller's bounded fallback for an extended producer
  instead of clipping to geometry that producer answers outside of, so Blur's
  halo and Transform's inverse/filter read survive.
- Merge and Shuffle propagate the claim from any operand their math really
  reads; a pointwise effect (Grade) and a geometry-changing effect
  (Transform/Affine) inherit it from the main input, because their own
  resampling of an extended input is real data rather than a promise they cannot
  keep. A node that blacks out a region states `false` itself; the claim is
  never inferred from an input's flag alone.
- Reuse identity carries the claim: content keys include it and mix
  `image-space-v3`, so identical nodes, pixels and requests are NOT the same
  image once the retained window or the claim changes.

Regression: `tests/ContributionTests.cpp`
`ExtendedDescriptionAnswersOutsideItsRetainedWindowThroughAnOrdinaryEffect`
(finite vs extended vs empty-claim pixels through an ordinary downstream effect,
plus the retained demand in the plan and reuse invalidation on the change).

The owner accepted the issue34 native-effects controls, API/node behavior and
images in chat; the #16 reference benchmark remains an open gate. Passing
checks are not a performance result or a bitwise-Nuke agreement. Session
evidence:
[`session.json`](../evidence/assets/issue34-native-effects/session.json).
The issue34 session is historical evidence of the original whole-image-only
implementation; issue #85 supersedes that spatial limitation without changing
its accepted full-frame image contracts.

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
