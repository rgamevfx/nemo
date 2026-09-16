# 0008 — Explicit built-in node contributions

Date: 2026-09-14
Status: Proposed — implemented on issue/83-node-contributions, extended by
issue #88 (described images, integer `shiftX`/`shiftY` on the test extension)
and issue #90 (named-channel Shuffle, per-input requirements and viewer
projection), then #98 (native storage and viewer scheduling repairs) and #92
(retained-edge-domain claim, owning-format context, creation initial values);
owner review pending and all prior landing holds still pending.
References: issues #83, #85, #88, #90, #92, #98; spec §§2, 10.2–10.4, 10.7, 12; ADR-0003, ADR-0004, ADR-0007

## Decision

Use one explicit built-in list (`src/nemo/nodes/BuiltinNodes.inc`) to derive
immutable schema/CPU/editor declarations and native execution registrations.
Node-local modules own descriptors, independent CPU/Slang/GLSL pixel code, typed
effect parameters, payload preparation and local pass descriptions. The schema
projection remains a runtime-object-free `NodeCatalog`; `NodeContributions` and
`EffectLibrary` are separately owned execution snapshots, not persistent model
state. Desktop, CLI and evaluation use the same builders.

This replaces the central per-effect switches, independently maintained runtime
shader inventories and growing shared effect uniform. It does not introduce a
plugin loader, stable binary ABI, service locator or universal extension SDK.
An ordinary effect requires its module, one list entry and normal build wiring;
new shared capabilities still require a change in their existing owning module.

## Boundaries

- Assemble and validate before publication. Duplicate identities, inconsistent
  schema/adapter versions, conflicting editor declarations, invalid payloads or
  pass references, and promised-but-missing implementations reject the whole
  candidate. Explicit backend unavailability or an unavailable kernel remains
  local to that node; unrelated supported nodes still execute.
- CPU execution retains dependency traversal, scoped expansion, effective
  animation/overrides, request validation, result reuse and error context in the
  shared evaluator. Read delegates to the existing source request/media owners;
  media error types remain intact. Output and Viewer retain their distinct roles.
  Formal inputs and network instances remain structural Evaluation behavior.
- Image descriptions and dependency demands use this same contribution, not a
  second registry. Optional `describe` changes the inherited description;
  `inputRequirements` declares each input's region and channels. Pointwise
  nodes inherit their main input's format/PAR/data bounds/interpretation and
  request their output coverage. Generators inherit their owning saved network
  format. Read describes its selected source through the media header seam.
  Evaluation resolves animation, instance overrides and source mapping once
  into a shared plan consumed by descriptions, demand rules, keys and execution.
- Data bounds, logical format, demand, coverage and density are distinct.
  Blur requests producer-local halos; Transform requests inverse/filter bounds
  plus original Mix coverage, with masks in output space. Merge inherits A's
  format and unions A/B data bounds; it never stretches B. Transform maps data
  support, preserving original support when Mix/masking can retain it; empty
  input remains empty. `supportsRegion=false` expands to that producer's useful
  domain. Executors receive each input's actual coverage and description.
- Native binding contract v7 separates a 96-byte **pass-raster** request at
  set 0/binding 0 from the optional node-local aligned payload at binding 1.
  The request includes signed origin, data support, stored channel count,
  components per texel, a planned-fill flag and resolved primary RGBA indices.
  Final output stores outside support become zero; scratch passes retain their
  declared working coverage.
  `gpu/ChannelImage.hpp` defines native storage: exactly four stored channels
  use RGBA32F at `W × H`; other counts use R32_SFLOAT at `W × (H × C)`.
  Count chooses storage, not channel names or interpretation. Each input has a
  64-byte geometry record at binding 2 carrying its own origin, raster offset,
  extent, scale, RGBA indices, actual channel count and components per texel.
  Binding 3 carries the resolved auxiliary-channel plan. The planned-fill flag
  is boolean, not a channel-count-limited bitmask. Canonical packed RGBA loads
  and stores avoid dynamic component indexing; reordered/data channels retain
  the general path.
  Names are resolved before dispatch, never per pixel. Pass inputs use set 1;
  output uses set 2/binding 0 and optional float weights set 3/binding 0.
  The executor owns allocations, barriers, submission and retirement, including
  a device-side final crop. No per-node waits or routine intermediate readback.
- CPU and GPU preparation callbacks may execute concurrently. Captures must be
  immutable or internally synchronized; context references/spans are valid only
  during invocation. A CPU request retains its registration until return. A GPU
  submission retains registrations, programs, payloads, weights and images until
  actual completion; dropping a result does not release in-flight resources.
  Backend initialization/compilation stays off the UI thread and out of the
  per-frame registration path.
- Descriptor and adapter versions must agree. Changed pixel/payload semantics
  require a version change; native identity also includes binding version and
  shader content. Registration order, labels, paths and callback addresses are
  not semantic identity. CPU keys retain the existing per-node version boundary,
  preserving reuse of unaffected branches; native library identity retains the
  conservative ADR-0007 boundary. Availability/schema compatibility is checked
  before reuse as well as execution.
- Optional editor declarations are projected into `ParameterEditorRegistry` at
  desktop assembly. The existing generic inspector, graph gestures, Commands,
  history and serialization remain their owners. Runtime registrations are not
  serialized. #88 changes the runtime image-space contract, not built-in node
  identities, inspector ownership or persistent graph state.

## Named-channel contribution (#90)

`ownsChannelLayout` distinguishes a contribution that creates/reorders named
channels from ordinary RGBA processing. Ordinary effects preserve other primary
input channels at their original lattice coordinates; their established RGBA
math is unchanged. This ownership participates in registration identity.

Shuffle is a version-1 built-in using the same contribution and editor registry.
B (port 0) is required; A (port 1) is optional. Its 30 ordinary parameters are
`input1/input2` (physical B/A selection), `in1/in2`, `out1/out2`, and eight rows
of `sourceKindN`, `sourceChannelN`, `outputChannelN`. Source kinds name
`input1/input2/zero/one`, not physical ports, so group selection and reordering
retain their routing meaning. The two four-socket output groups do not limit
the untouched B inventory.
Reordering uses the shared evaluated-value gesture; it is not a structural
permutation of animation tracks or exposed-parameter identities. Native proof
covers static reorder and keyed routing/history/reopen separately.

Explicit output names, including `mask.a`, are preserved. Only authored rows
create channels; unwired/missing names and disconnected A produce zero.
Untouched B channels survive; constants cover B's logical format; A is sampled
at its own absolute coordinates. The editor rejects duplicate output names
before its atomic command transaction; execution rejects invalid mappings
before producing pixels.
The owner approved these policies and the Nuke 17 documentation-only editor
adaptation; runtime parity with Nuke is not claimed.

The existing allocator charges the complete native image and retained
submissions. Physical dimensions must fit the device's 2D image limit:
unsupported dimensions or byte budgets fail explicitly, without channel loss or
reduced quality. This does not introduce tiling or replace #14 accounting.

Viewer projection yields the existing RGBA presentation. A packed image with
identity RGBA roles and matching raster extent is retained directly; other
selections use the device-side projection. The retained image and completion
identity survive the evaluation/session that produced them.
Only a complete identified primary/root RGB set is color-managed, by named
roles rather than storage order. Other selections bypass OCIO; a single named
plane, including alpha-only data, is opaque gray, while an ordered selection of
multiple data planes maps its first up-to-four selected planes positionally
onto the presentation RGBA and bypasses OCIO the same way; that display mapping
neither creates nor renames composition channels. Projection policy is part of
viewer-cache identity. Decoded replay is already packed RGBA and is retained
directly, with a device-side crop only for codec padding. `cropNativeImage`
copies a packed region once or each scalar plane using its logical height.

Still/software RGBA upload copies interleaved values directly into staging;
other channel counts transpose without padding or loss. Hardware decode converts
directly to packed RGBA32F. Source OCIO transforms that image in place, without
introducing image/buffer copies; source-linear and display-referred replay
remain separate contracts.

`nemo.shuffle.mapping` is a section editor hosted by the accepted inspector.
Mapping edits use shared commands/history. Output channel creation belongs to
`Out → new`; repeating it for the same layer fills the next free output socket.
`Out → none` disables that output group. There are no per-row add/clear buttons.
Clicking an existing output name opens routing/key controls in the inline panel;
shared Alt-click keying and exposure dragging remain on the labels.
Unavailable custom editors expose all ordinary parameters through the generic
fallback. Native evidence and the
remaining review/landing holds are recorded in
`docs/evidence/assets/issue90-channels/verification.json`.

Issue #98's repair checks, native captures, matched playback/upload/kernel
measurements and remaining limitations are recorded in
`docs/evidence/assets/issue98-viewer-performance/verification.json`.

## Retained-edge-domain claim and creation initial values (#92)

`ImageDescription` gains one semantic field, `edgeExtension` (default `false`).
`false` is the finite support every existing producer has: a sample outside
`dataBounds` is transparent black, enforced centrally by the executors (CPU
`enforceDataWindow`, native output support), never by node math. `true` is the
producing effect's explicit claim that its FINITE `dataBounds` are the RETAINED
edge domain instead of the image's support: the effect answers requested
coordinates outside them with data of its own, so no consumer may treat them as
transparent. Every guard reads the claim through `hasEdgeExtension()`, which also
requires a NON-EMPTY retained domain — an empty image stays transparent whatever
the flag says. The field participates in description equality, plan JSON and
content-key identity (`image-space-v3`), so identical nodes, pixels and requests
are different images once the retained window or the claim changes.

The claim is shared execution behavior, not a per-node branch:

- The CPU support guard clears nothing for an extended description; the native
  node-output pass declares NO support (`{-1,-1,-1,-1}`) so the whole produced
  raster is the effect's. Scratch passes keep their own rule.
- A whole-frame-only extended producer's coverage is its retained domain
  UNIONED with the demand instead of the domain alone; padding stays limited to
  the retained domain, so no ordinary region escalates to the whole frame.
- `requirementDomain` returns the caller's own bounded fallback for an extended
  producer rather than clipping a read to geometry the producer answers outside
  of (Blur's halo, Transform's inverse/filter read).
- Merge and Shuffle propagate the claim from each operand their math really
  reads; pointwise (Grade) and geometry-changing (Transform/Affine) effects
  inherit it from the main input, because their own resampling of an extended
  input is real data. A node that blacks out a region states `false` in its own
  rule; the claim is never inferred from an input's flag alone.

The same issue adds the one generic creation-time rule, so a node whose
parameters are stated in the owning network's own frame is created from that
network rather than from a transient selection: `ParameterInitialValue`
(`Default`, `OwningNetworkWidth`, `OwningNetworkHeight`) on `ParameterSpec`
seeds a scalar Integer/Float parameter in the creation owner
(`initialNodeParameters`, applied by `addNodeCommand` and
`insertNodeOnEdgeCommand`) from the owning network's saved `ImageFormat`. The
captured value is authored state from then on, the rule is schema identity (a
differing rule is an implementation mismatch), and a schema whose declared range
cannot hold the seeded dimension fails creation with that relationship named.
Rules are resolved from the graph's own catalog, so no node type is named in the
creation owner. Crop declares `right`/`top` this way; Reformat resolves its
composition target from `owningFormat` at description time.

The `owningFormat` pointer on `NodeDescriptionContext`, `NodeRegionContext`,
`CpuNodeContext` and `eval::GpuNodeContext` is that authored COMPOSITION canvas,
resolved once by the shared plan for the network that owns the node; it is null
only for a direct rule invocation without a network scope. It is deliberately
not a universal runtime coordinate frame: the owner's reference defines Crop's
box distances against the ORIGINAL INPUT image, so Crop converts its y-down
value with the CURRENT INPUT's described format height (its normalized origin)
and re-derives it whenever that input changes, while only the box's creation
uses the canvas through the rule above. Reformat's composition target is the
canvas. The shared regression is `tests/ContributionTests.cpp`
`ExtendedDescriptionAnswersOutsideItsRetainedWindowThroughAnOrdinaryEffect`;
Crop/Reformat numerical fixtures remain their own modules' evidence.

Reformat's affine geometry and closed tap membership are resolved in host double
precision. CPU, GLSL and Slang still independently evaluate and normalize their
filter coefficients. Native preparation stores one geometry entry per driving
output row/column in the existing retained `GpuPreparation::weights` buffer:
integer endpoints/base use pairs of exact numeric 16-bit limbs, alongside flags
and a fractional position relative to that base. All table floats stay finite
without restricting the signed index range. This prevents a float-sized epsilon
from adding a Notch tap or choosing the wrong Impulse neighbor; relative offsets
also avoid subtracting large rounded float coordinates.
The executor still owns upload and retirement; no new allocator, submission,
readback or render-path coefficient sharing is introduced.

## Verification seam

`tests/contributions/Affine.{hpp,cpp,slang}` is a real, namespaced, test-only
extension, not another shipped node. `ContributionTests.cpp` assembles it through
the production interfaces and exercises discovery, creation/connections, edits,
undo/redo, persistence, independent CPU/native expected pixels, atomic rejection,
unavailability, stale-reuse prevention and retained ownership. Existing effect
regressions remain the numerical oracle; a production Wayland inspector/Blur
scenario separately checks the accepted surface. Exact results and limitations
are recorded in the issue #83 evidence, not implied by this decision's status.

Issue #88 extends Affine with integer `shiftX`/`shiftY`: its description translates
data bounds and its per-input requirement translates demand inversely, while
scale/offset pixel math remains independently implemented on CPU and Slang.
The public session/history/save-reopen scenario reads independently authored
windowed EXR samples through that extension. The retained #88 JSON evidence and
task record distinguish CPU/native numerical checks from native viewer surface
checks; no Nuke runtime parity is claimed.
