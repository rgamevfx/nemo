# 0008 — Explicit built-in node contributions

Date: 2026-09-14
Status: Proposed — implemented on issue/83-node-contributions, extended by
issue #88 (described images, integer `shiftX`/`shiftY` on the test extension)
and issue #90 (named-channel Shuffle, per-input requirements and viewer
projection), then #98 (native storage and viewer scheduling repairs), #92
(retained-edge-domain claim, owning-format context, creation initial values),
and #89 (explicit Premult/Unpremult stored-channel arithmetic); owner review
pending and all prior landing holds still pending.
References: issues #83, #85, #88–#90, #92, #98; spec §§2, 10.2–10.4, 10.7, 12; ADR-0003, ADR-0004, ADR-0007

## Decision

Use one explicit built-in list (`src/nemo/nodes/BuiltinNodes.inc`) to derive
immutable schema/CPU/editor declarations and native execution registrations.
Node-local modules own descriptors, independent CPU/Slang/GLSL pixel code, typed
effect parameters, payload preparation and local pass descriptions. The schema
projection remains a runtime-object-free `NodeCatalog`; `NodeContributions` and
`EffectLibrary` are separately owned execution snapshots, not persistent model
state. Desktop, CLI and evaluation use the same builders.

This replaces the central per-effect switches, independently maintained runtime
shader inventories and growing shared effect uniform. The internal C++
interfaces are not a binary SDK; the separate installed-package boundary below
adds a constrained C ABI without exporting those interfaces. An ordinary
built-in effect requires its module, one list entry and normal build wiring;
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
- Native binding contract v8 separates a 96-byte **pass-raster** request at
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
  output uses set 2/binding 0, optional float weights set 3/binding 0, and
  optional node-owned packed geometry set 4/binding 0. Geometry is an owned,
  word-aligned byte vector; the executor validates storage-buffer limits and
  binds it only for passes declaring geometry.
  The executor owns allocations, barriers, submission and retirement, including
  a device-side final crop. No per-node waits or routine intermediate readback.
- CPU and GPU preparation callbacks may execute concurrently. Captures must be
  immutable or internally synchronized; context references/spans are valid only
  during invocation. A CPU request retains its registration until return. A GPU
  submission retains registrations, programs, payloads, weights, geometry and
  images until actual completion; dropping a result does not release in-flight resources.
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
viewer-cache identity. Issue #106 replaces float/video replay with a separately
typed display-referred BC7 texture. Replay samples that texture directly into
the shared RGBA8 presentation surface; it never enters contribution execution,
native working-image projection or source-color interpretation. Compressed edge
blocks preserve logical odd dimensions without a float crop/reconstruction.

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

## Explicit alpha arithmetic (#89)

Premult and Unpremult are ordinary pointwise image contributions. Each has one
required image input and exactly two generic `Choice` parameters: Premult
declares `multiply` (default RGB) and `by` (default Alpha); Unpremult declares
`divide` (default RGB) and `by` (default Alpha). They add no custom editor,
mask, mix, inversion, renderer path or automatic alpha conversion.

The selected primary stored roles are multiplied or divided by the selected
primary stored role. Unselected primary roles and every auxiliary channel are
exact pass-through. Dependency planning explicitly adds the multiplier/divisor
when a selected output channel is demanded, and rejects a missing required role
with the node/parameter relationship. Premult is exact float multiplication.
Unpremult divides every nonzero value exactly, including near-zero, negative and
HDR values; an exact zero divisor writes zero. No epsilon or clamp is applied.

These nodes inherit the input description, including association metadata. They
are explicit authored arithmetic and are never removed or cancelled as an
association canonicalization. Existing straight-input source, Grade, Blur,
Transform, Merge, viewer and delivery behavior is unchanged. CPU, Slang and
retained GLSL implementations are independent and use the ordinary named-channel
storage, role resolution, reuse and submission-lifetime contracts.

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

## Roto geometry and matte policy (#93)

Roto contributes through the same schema/editor/CPU/native entry points as other
effects. Its typed authored hierarchy and animation addresses are documented in
ADR-0007. A node contributes bounded geometry values, not a Vulkan allocation or
a second scheduler. Optional unconnected images bind an unread connected image
descriptor, or the output for a generator; no dummy image is allocated.

The approved numerical policy is:

- Closed Bézier and tension-controlled cubic B-spline contours use odd-even
  coverage. Full-resolution image coordinates are independent of proxy density.
  Nested transforms apply scale, physical-aspect-aware rotation about the pivot,
  then translation; rotation uses the saved owning network's pixel aspect.
  A connected background still owns the output format and display mapping.
- Siblings fold in authored order: Combine `a + b - a*b`, Intersect `a*b`,
  Subtract `a*(1-b)`. The first contributing sibling seeds the fold, except
  leading Subtract starts from zero. Groups composite their children before
  group inversion/opacity; lifetime and visibility remove the whole subtree.
  Locks protect authored edits to that element, not its descendants.
- Signed feather is outward when positive and inward when negative. Point and
  enabled element widths add; inherited group bias is applied after each scale.
  Width scales by the geometric mean of the affine axis scales. Disabling a
  shape's feather disables its point widths too, without erasing their values.
  Linear or smoothstep ramps are raised to `1 / falloff` (`falloff > 0`).
- Motion blur is node-wide only: nonnegative shutter in frames, default `.5`,
  centered midpoint samples, at most 64. #103 removes the shutter upper bound
  and explicitly retains the 1–64 temporal-work exception. Samples 1 or shutter
  0 uses the current frame.
  Each sample evaluates animated points, transforms, properties and lifetime,
  completes the hierarchy, then contributes to the average.
- A generator stores exactly the named output channel (default `A`). A
  connected node preserves other channels and appends a missing target.
  Replace writes the matte; otherwise target output is
  `matte + (1 - matte) * incoming`. Finite node opacity scales the completed
  matte without hard bounds (#103); element/group opacity remains in `[0,1]`
  because it participates in coverage combination. Point tension retains its
  `[0,1]` smooth-curve/control-polygon domain. These two domains are explicitly
  owner-approved in #103. An optional exact named mask limits the matte.
  An absent/disabled mask is unlimited; a missing channel on a
  connected mask is zero, before optional inversion.
- Clip uses format, incoming bbox, their union/intersection, or no restriction.
  For a generator, bbox is the sampled matte extent; inversion includes the
  canvas. Unrestricted support unions canvas, incoming bounds and matte extent.

Adaptive tessellation targets `.25` full-resolution pixels and is shared
geometry preparation, not a shared pixel oracle: CPU scanlines and native
ray crossings remain independently implemented. Explicit refusals bound each
contour to 4096 vertices, a sample to 65536 vertices/1024 items/32 hierarchy
levels, packed geometry to 4M words, and CPU row scratch to 4M values.
Oversized or non-finite geometry is diagnosed rather than truncated.

This is not an alpha-association migration. RGB premultiplication remains #89's
contract; Roto preserves incoming association. The absent delivery-job seam
remains owned by #87. Neither gap is an integrated-completion claim for #93.
Reference comparison is against Nuke 17 documentation, not a Nuke runtime.

## Trusted installed pointwise packages (#37, #105)

`extensions/InstalledPackages` separates metadata inventory from activation.
Discovery parses one strictly validated manifest per package folder and never
opens a library, calls an entrypoint or runs package code, so listing,
refreshing or inspecting packages cannot execute one. The manifest's `name`,
`author` and `description` are optional presentation keys; absence is reported
as absence. Inspection exposes the declared identity, name, author, version,
description, contributions, dependencies, admission (`PackageInfo::admitted`)
and a typed `PackageStatus` — active, disabled, missing package, malformed
manifest, incompatible, duplicate identity, missing/disabled/refused
dependency, dependency cycle, failed to load — so a presentation layer never
parses diagnostic text. A package refused after its identity and version were
read still reports them, and a dependency is never enabled implicitly.

Activation is one immutable startup snapshot: only enabled, admitted packages
are opened through the fixed ABI in `EffectAbi.h`, in dependency order. Strict
manifest/schema/API/capability/file validation, duplicate identity checks (every
offender refused, every offending location named) and dependency ordering
precede executable activation. Failed dependencies refuse their dependants
without hiding unrelated valid packages. Installed code is trusted: metadata
validation is not sandboxing, and there is no hot reload, package marketplace or
project-embedded executable discovery.

Enablement is per-user state (`extensions/InstalledPackages.hpp`): the registered
linked package folders and the enabled packages, persisted as (identity,
canonical location) pairs at `$XDG_CONFIG_HOME/nemo/extensions.json`
(`%LOCALAPPDATA%/Nemo/extensions.json` on Windows) and written atomically. An
identity alone never enables a package, a folder discovered by two routes is one
package, newly discovered and pre-existing installations are disabled until the
user enables that exact location, and removing a registration only forgets it —
package files are never copied, moved or deleted. A corrupt or unwritable
settings file enables nothing and reports why. Both the desktop shell and the
headless CLI construct `InstalledPackages` the same way, so a package runs
headlessly exactly when it runs interactively; the explicit `NEMO_EXTENSION_PATH`
developer/test override selects its roots and activates every admitted package
without reading or writing the user's settings.

`extensions/EffectAbi.h` defines version 1: effective-parameter JSON and
host-owned flat buffers cross a C callback table; C++ objects, Qt, Vulkan
handles, allocators and exceptions do not. The first capability is one-input
pointwise RGBA processing, not a universal effect runtime. CPU adapters and GPU
payload preparation retain the library; submission completion retains the
existing immutable `EffectLibrary` snapshot. Installed absolute SPIR-V paths
extend that library's existing loading path, not GPU execution or synchronization.

Desktop and CLI compose the installed catalog once and inject it into their
existing session/evaluation/file/runtime owners. The session validates effective
authored parameter edits and previews before publication, so a custom editor
cannot bypass the same constraint enforced for command and headless callers.
QML editors and demo panels enter the existing registries and shared chrome.

Descriptors carry a persistent `stateIdentity`; incompatible/missing installed
state stays unavailable and retains authored parameters, animation and links.
Package/processing versions participate in runtime result identity separately
from state compatibility. File-open/recovery uses the installed inventory, not
the previous document's compatibility-filtered catalog.

ColorWarp's bounded mesh, fold criterion, scene-linear opponent transform,
alpha/data policy and independent CPU/Slang/GLSL kernels live in the separate
[`examples/colorwarp`](../../examples/colorwarp/README.md) package. Its editor is
new owner-approved content, not a historical prototype image. Public API,
new-node and image approval remain owner gates; implementation evidence does
not remove them.

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
