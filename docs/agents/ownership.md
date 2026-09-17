# Ownership and Contribution Guide

This is the implementation map for ownership and contribution entry points. The
product contract remains [`composition_network_vfx_nle_spec_v2.md`](../composition_network_vfx_nle_spec_v2.md),
durable trade-offs remain in [`docs/decisions/`](../decisions/), and the
workflow for issues and pull requests remains [`issue-tracker.md`](issue-tracker.md).
Do not create a parallel registry or instruction hierarchy when a responsibility
already has an owner below.

## Choose the contribution path before editing

The root `AGENTS.md` Development contract is mandatory for every task.
Record its pre-edit contract using [`issue-tracker.md`](issue-tracker.md).

| Task class | Start from | Change boundary |
| --- | --- | --- |
| Prototype port | Accepted production component plus current archived prototype source/behavior entries | Port the missing observable behavior through production queries/commands; retain the accepted shell and adjacent interactions |
| System extension | Existing catalog, command, evaluator, persistence or panel contribution entry point below | Supply data/behavior through that contract; shared UI remains unchanged unless an explicitly scoped prerequisite extends it |
| New design | Narrow owner decision linked from the task | Implement only the approved observable decisions; reuse existing host controls and ownership |

The immutable [prototype inventory](../evidence/issue25-prototype-acceptance-v1.md)
defines visual/interaction authority and supersession. The accepted production
baseline is the integration starting point, not permission to ignore prototype
details still assigned to later tasks. Spec/ADRs define production ownership;
prototype fixture models are reference behavior, not production architecture.

Baseline #40/#59 supplies the shared shell and existing panel presentation.
Context routing is group-only: a panel's context is always its own A–E group,
and every panel header keeps the visible A–E selector required by spec §6 and
the approved prototype. The `follow`/`pinned` panel-routing modes retired by #60
do not return; the visible selector was restored by #63; inspector-card pinning
is separate and remains valid. Functional ports
#44/#46/#50/#47/#43 extend those owners. #43's implemented media-import/Media
Bin owners are recorded below; #47's two-viewer/media-role parity and the human
API/schema/image review of #43 remain open gates, so read the live owning issue
to distinguish implemented capabilities from planned ones before using an
extension path. #75's coordinated slices extend those same owners instead of
adding parallel ones — shared numeric/key/exposure editing (#76:
`NumericField.qml`/`KeyIndicator.qml`/`ExposureLabel.qml`), Grade's linked-RGB
editor (#77: `nemo.channels.rgb`), Merge's operation/mask/atomic Swap A/B (#78),
and Read ownership/discovery/color/control (#79–#82). Their revised presentation,
the schema-5 migration, and any public catalog/editor/schema/image change keep
the normal owner review gates; landing a slice is not approval of them.

## Target graph and dependency direction

These are the public CMake target names and their direct interfaces as currently
built:

```text
nemo::core       -> nlohmann_json
nemo::gpu        -> Vulkan, glslang (PUBLIC); VMA (PRIVATE)  (NEMO_BUILD_GPU)
nemo::media      -> nemo::core                           (plus GPU/FFmpeg in GPU builds)
nemo::eval       -> nemo::core, nemo::gpu, nemo::media   (NEMO_BUILD_GPU)
nemo::workspace  -> nlohmann_json                       (Qt-free)
nemo-cli        -> nemo::core, nemo::media              (plus nemo::eval in GPU builds)
nemo-ui         -> nemo::workspace, nemo::eval, Qt6     (NEMO_BUILD_UI)
```

The target definitions are in [`src/nemo/CMakeLists.txt`](../../src/nemo/CMakeLists.txt),
[`src/nemo/eval/CMakeLists.txt`](../../src/nemo/eval/CMakeLists.txt),
[`src/nemo/media/CMakeLists.txt`](../../src/nemo/media/CMakeLists.txt),
[`apps/nemo-ui/CMakeLists.txt`](../../apps/nemo-ui/CMakeLists.txt), and
[`apps/nemo-cli/CMakeLists.txt`](../../apps/nemo-cli/CMakeLists.txt).
`nemo::core` owns the persistent model, catalog, commands/session, and CPU
reference evaluation; it remains free of Qt, Vulkan, plugin-runtime, and UI
links. `nemo::workspace` is the Qt-free arrangement model, not a dependency of
core or evaluation. UI/workspace code may consume evaluation and core, never the
reverse. Shared operations retain the `NEMO_BUILD_UI=OFF` and
`NEMO_BUILD_GPU=OFF` path.

The interface check is target-based, not a source-text scan. It is implemented
by [`cmake/NemoDependencyChecks.cmake`](../../cmake/NemoDependencyChecks.cmake),
which defines `nemo_check_dependency_direction()` and is invoked at the end of
the top-level `CMakeLists.txt`, after all subdirectories are declared. It walks
the `LINK_LIBRARIES` and `INTERFACE_LINK_LIBRARIES` properties of `nemo_core`
and `nemo_eval`. The normal check is part of configuration. To prove the
forbidden direction fails, use:

```bash
cmake --preset headless -DNEMO_TEST_FORBIDDEN_DEPENDENCY=ON
```

This injects `nemo_core -> nemo::workspace` and must fail configuration (with
`NEMO_BUILD_TESTS=ON` or `NEMO_BUILD_UI=ON` so the workspace target exists).

## Authoritative ownership map

| Responsibility | Owner and interface | Concrete use site / boundary |
| --- | --- | --- |
| Persistent document, graph, IDs, serialization | `src/nemo/core/document/Document.hpp`, `Graph.hpp`, `Serialization.hpp`; `Document`, `Graph`, and `NodeCatalog` own persistent state | `apps/nemo-ui/ViewerController.cpp` submits commands; UI and automation do not mutate graph primitives directly |
| Saved composition formats | `Network::format()` in `Graph.hpp`, document-owned named presets, and `core/commands/NetworkCommands.hpp` | `ProjectSession`/CLI own authoring and history. ADR-0007 documents migration/value-copy semantics; per-node descriptions consume this format for generators, not as a request-global canvas |
| Project file persistence, autosave, recovery | `src/nemo/core/session/ProjectFile.hpp`; `ProjectFile` owns read/write, path policy, file envelope and reference state; `AutosaveStore` owns bounded slots; `ProjectSession::prepareSave`/`commitSave` own path, dirty baseline and recovery guard | `apps/nemo-ui/ProjectFileController.*` and the CLI `file-state`/`open`/`save`/`save-as`/`autosave`/`recover` ops are the only callers; both go through this owner, never a second codec |
| Node schemas and discovery | `src/nemo/core/nodes/NodeCatalog.hpp`; immutable `NodeDescriptor` records and exact-inventory `NodeCatalog(std::vector<NodeDescriptor>)` | `src/nemo/nodes/BuiltinNodes.inc` supplies the built-in inventory; `NodeContributions::catalog()` projects schema for `Graph::catalog()`, desktop and CLI. No mutation or unregister operation |
| Validated edits and history | `src/nemo/core/document/Document.hpp` command factories plus `src/nemo/core/commands/`; `Command`, `CommandStack`, and `ProjectSession` | `apps/nemo-cli/ProjectSessionCommand.cpp::makeCommand()` maps JSON operations; `ProjectSession::submit()` is the commit seam |
| Desktop history routing | `apps/nemo-ui/HistoryController.hpp`, explicitly injected with the application `ProjectSession`; `qml/HistoryMenu.qml` / `HistoryMenuItem.qml` | Register existing document windows; use this adapter for shared shortcuts/menu actions, never a viewer forwarding method or per-panel stack. Context precedence and the gesture contribution contract are below |
| Structural document storage | `src/nemo/core/SharedContainers.hpp` (`CowVector`, `CowMap`) and the document's controlled mutations; `ChangeRecorder` (`src/nemo/core/document/ChangeRecorder.hpp`) records the identities a transaction touched | Commands and sessions retain version handles, never deep copies; publication/undo/redo derive their notifications from the touched identities. See ADR-0007 "Structurally shared document versions (#72)"; do not reintroduce a whole-document copy, diff or serialization on the ordinary edit path |
| CPU evaluation and shared plan | `src/nemo/core/evaluation/CpuReference.hpp`; `evaluateCpu`, `expandDependencies`, `scheduleDependencies`, and the external media seam `SourceProvider::frame`/`colorConfigIdentity` | `apps/nemo-cli/main.cpp` uses `evaluateCpu`; input is a `const Document` snapshot and the CPU image is reference-owned. The provider receives the resolved `EffectiveSourceRequest` and supplies the opaque color-config identity the source-node keys mix in; without a provider a source node is an explicit error, never a synthetic pattern |
| Per-node image descriptions and demand | `ImageDescription` in `Image.hpp`; `ImageDescriptionPlan`/`planResolvedRegions` in `CpuReference`; contribution `describe`/`inputRequirements` | `ViewerSession` resolves a Qt-free `ViewIntent` against the current frame, then consumes that same description plan for demand, keys and execution. `SourceDescriptionProvider` uses still/sequence headers through `ImageSource` and clip headers through `media::inspectClipHeader`; independent header-only consumers retain Describe. See ADR-0004/0007/0008 for worker scheduling, signed bounds and reuse identity |
| GPU primitives and resource lifetime | `src/nemo/gpu/` (`Device`, `Allocator`, `Submit`, `ComputePass`) | `src/nemo/eval/GpuExecutor.cpp` records/submits work; follow [`rendering.md`](rendering.md) for retained ownership and synchronization rather than copying those rules here |
| Native effect execution | `core/evaluation/NodeContributions.hpp` and `eval/GpuContribution.hpp` declare immutable CPU/native adapters; `eval/GpuExecutor.hpp` exposes `EffectLibrary`, `submitGpu`, and `evaluateGpu` | `src/nemo/nodes/<slug>/` owns schema, independent CPU/Slang/GLSL pixels, typed effect parameters and local pass preparation. Shared evaluators own traversal, requests, reuse and resource lifetime; see the contribution recipe below and [`rendering.md`](rendering.md) |
| Optional input ports and absent slots | `src/nemo/core/nodes/NodeCatalog.hpp` (`PortSpec::optional`); `Plan.hpp` slot model with `CpuReference.cpp` expansion | An absent optional slot keeps its declared port position as `EvaluationNodeId{}` (node == `kInvalidNode`) in `ExpandedNode.inputs`/`PlanStep.inputs`; `Reuse.hpp` marks it in result identity with `kAbsentInputKeyHash`; `GpuExecutor` binds the main image as a valid dummy descriptor with `maskPresent=0`, never an allocated fallback. Grade/Blur/Transform's optional input is port 1 `mask`; Merge's is port 2 (its port 1 is the required foreground). Merge's `A`/`B` roles are production order — A background/base, B foreground/source — and are never silently reversed; `swapInputsCommand` (`Document.hpp`, exposed as `ViewerController::swapNodeInputs` and the `swap-inputs` CLI op) exchanges exactly the two *image* sources as one atomic undo step, retaining the mask, parameters, node identity and layout, and refuses an empty pair or two edges from one source before touching history |
| Media and color adapters | `src/nemo/media/` public image/viewing contracts; external OIIO/OCIO/FFmpeg types stay behind `.cpp` adapters | CLI probe/render and `eval::SourceSession` consume the application media contract. `src/nemo/media/InputColor.{hpp,cpp}` is the ONE owner of an encoded source's RGB interpretation (`resolveInputColor`, retained per generation by `InputColorCache`); `ViewingTransform.hpp`'s `OcioConfigSnapshot` is the ONE retained config load (content identity, canonical space enumeration, file rules, processors). `src/nemo/media/ImageSource.{hpp,cpp}` owns still/sequence decode (`probeImageFrame`/`readImageFrame`, headless `ImageSourceProvider`); `VideoDecode.hpp` owns clip decode and takes the same input-color context. Reuse these rather than adding a second still reader or a second color resolver |
| Effective source request (Read choices vs the shared reference) | `src/nemo/core/evaluation/SourceRequest.{hpp,cpp}`; `resolveSourceRequest`, `EffectiveSourceRequest`, `ReadNodeOverrides`, `readAuthoredOverrides`/`readOverrideParameters`/`readInitializationParameters`, `mapSourceFrame`/`startAtOffset` | One resolver combines a Read's node-scoped choices with the shared `SourceReference` and committed facts. The node-scoped entry point is used by Reads and by result keys; the source-scoped entry point (shared reference's own mapping, no node overrides) is used by `eval::SourceSession::probe`. The media import worker consumes a `SourceReference` snapshot and maps it with `SourceReference::frameAt` — a Read requester hands it the snapshot that already carries the Read's effective mapping. `CpuReference.cpp`, `Reuse.cpp`, `eval::SourceSession::acquire` and `ReadSourceController` consume the resolved request and never re-derive mapping or precedence; see ADR-0007 "Read source ownership, effective requests, and schema 5 (#79)" |
| Media import, probing and preview | `src/nemo/media/MediaImportService.hpp`; `MediaImportService`, `MediaImportRequest`/`MediaImportResult`, `inspectMediaSource`, plus `SequenceDiscovery.hpp` (`discoverSequenceRange`) | One service worker decodes, probes, discovers numbered-sequence coverage and reduces a display-referred preview off the GUI thread; the queue is bounded, a cancelled scan stops early with no facts claimed, and results carry request identity. A request also carries the merged `InputColorChoice`, the frozen source-local frame, `ProbeAlignment` and `projectGeneration`, so the worker never reads `Document` state and a result from a superseded project, frame or node never publishes into the new one. `apps/nemo-ui/MediaLibraryModel.*` is the production consumer, requests previews within 160×90 bounds, and owns no decoder |
| Media Bin catalog adapter | `apps/nemo-ui/MediaLibraryModel.*`; Qt/QML query/command surface over `Document`/`MediaCatalog` | Submits validated catalog commands through `ProjectSession`; owns transient probe results and the bounded display-referred `QImage` thumbnail cache/provider, not persistent catalog state. A runtime probe is a proposal until `applyProbe` commits it; `relink` copies the preserved `SourceReference` |
| Media Bin panel presentation | `apps/nemo-ui/qml/MediaBinPanel.qml` | Reactive adapter records plus panel-state view/selection preferences; it submits catalog operations, emits ordered `requestTimelineInsert` intent for #54, reveals through `revealMediaPanel`, and opens explicitly only through `MediaLibraryModel::openMediaSource` |
| Native file chooser | `apps/nemo-ui/NativeFileChooser.hpp`; `openFiles`/`saveFile` with a requester-owned `OutcomeHandler` | One platform implementation per build; each request belongs to its requester and no outcome is broadcast. `ProjectFileController`, `MediaLibraryModel` and `ReadSourceController` are separate requesters of the same chooser, so a Read's browse result reaches only the Read that asked |
| Workspace arrangement and panel state | `apps/nemo-ui/Workspace.hpp`/`Workspace.cpp`; Qt-free `Workspace`, `Panel`, and `Workspace::createPanel` | `WorkspaceController::registerPanelType`, `createPanel`, and `setPanelState` are the Qt/QML boundary; `main.cpp` registers production panels before QML loads |
| Presentation and input | `apps/nemo-ui/WorkspaceController.*`, `ViewerController.*`, and `apps/nemo-ui/qml/` | Presentation reads state and submits commands; it does not own `Document`, evaluator, or GPU resource state |
| Headless automation | `apps/nemo-cli/`; JSON-lines project-session protocol and `nemo-cli` commands | `ProjectSessionCommand.cpp::makeCommand()` is the current command dispatch seam; do not invent a second command registry |

### State and threading boundaries

- `ProjectSession` methods and subscriptions are owner-thread-only; the session
  must outlive its subscriptions. Development builds diagnose off-owner access,
  subscription moves/removal and session/subscription destruction before touching
  owned state. Ownership stays with the constructing thread across document
  replacement/opening. `NDEBUG` removes the assertions, not the contract; no
  synchronization or cross-thread dispatch is added. Callbacks run synchronously
  after publication and must not block or mutate during notification. Workers
  receive `ProjectSession::snapshot()` copies — structurally shared immutable
  versions, so a snapshot is a handle set, never a live session or a mutable
  builder — and never the session object. A caller that needs a mutable worker
  view copies the snapshot first; mutation of a copy is copy-on-write and never
  reaches the published version.
- `NodeCatalog` is an immutable snapshot after construction. Descriptors contain
  schema facts only: no Qt, Vulkan, plugin, executor, or image objects.
- `NodeContributions` and `EffectLibrary` are immutable, retained snapshots.
  CPU callbacks and GPU preparation may execute concurrently; captures must
  remain immutable or synchronize their own state. Context references, input
  spans and effective-parameter maps are borrowed for that invocation only.
  A CPU request retains its registration until return; GPU submissions retain
  the library, programs and prepared resources until actual completion, even
  when their result handle is dropped. Compilation/assembly is preparation,
  never per-frame inventory rebuilding or UI-thread device initialization.
- Evaluation consumes an immutable document view. `ViewerScheduler` is the
  thread-safe queue/policy boundary and retains immutable snapshots; it rejects
  stale publication without waiting for in-flight GPU work.
- Color-configuration freshness is an explicit boundary, never a watcher.
  `OcioConfigSnapshot` (media) is one immutable load per generation, and the
  retained processors plus the opaque content identity are replaced at project
  replacement/reopen — `ViewerController::documentChanged` observes
  `ProjectSession::projectGeneration()` and calls the worker-side
  `ViewerRuntime::refreshColorConfig`; `SourceSession::refreshColorConfig`,
  `ImageSourceProvider::refreshColorConfig` and the Read adapter's
  project-generation observer are the same boundary for their owners. A decode
  or read already in flight holds its own shared generation, so the refresh
  never invalidates work underneath it. A source Reload is source-only and
  deliberately does not reach this path.
- GPU submission, compilation, allocation, and completion belong off the UI
  event thread. GPU work retains every referenced resource until completion;
  consult [`rendering.md`](rendering.md) for the complete execution contract.
- `Workspace` owns serializable arrangement/panel state and has no Qt or
  rendering dependency. `WorkspaceController` and QML are presentation-thread
  state; hand work to that boundary through the Qt application machinery rather
  than sharing mutable workspace state with workers.

## Contribution entry points

### Add a node or effect

Built-ins use one explicit inventory, `src/nemo/nodes/BuiltinNodes.inc`.
`builtinNodeContributions()` derives immutable schema/CPU/editor declarations;
`builtinGpuContributions()` derives the native projection from the same list.
Desktop and CLI use these builders, not private inventories. See
[ADR-0008](../decisions/0008-built-in-node-contributions.md).

1. Add `src/nemo/nodes/<slug>/Contribution.cpp`: a namespaced persistent type,
   immutable descriptor, role, versioned CPU adapter, and optional namespaced
   editor declarations. Preserve legacy built-in identifiers. Keep Qt/GPU
   runtime objects out of the descriptor. Add effect-local typed interpretation
   in `Parameters.hpp` when both CPU and GPU need it; reuse generic typed reads,
   mask/channel rules and effective animation from `core/evaluation/Params.hpp`.
   Override `describe` only for changed output properties; otherwise inherit the
   main input. Neighborhood/geometric effects declare per-port region/channel
   `inputRequirements` and read through supplied coverage geometry. Pointwise
   inputs default to output demand. Effective parameters are immutable and
   already resolved. Whole-frame-only declarations escalate inside Evaluation.
   A node whose authored values live in the COMPOSITION's own frame reads the
   network's saved canvas from `owningFormat` on
   `NodeDescriptionContext`/`NodeRegionContext`/`CpuNodeContext`/
   `eval::GpuNodeContext` (Reformat's to-format target; the canvas that also
   seeds created Crop values). A node whose authored values are stated against
   the IMAGE it processes reads that input's described format instead
   (`inputs`/`inputDescriptions`/`description`) — Crop's bottom-left box is
   converted against the current input height, never the composition canvas.
   An effect
   that answers requested coordinates OUTSIDE its finite data bounds states
   `ImageDescription::edgeExtension` (read through `hasEdgeExtension`, which
   requires a non-empty retained domain); ordinary pointwise/geometric effects
   inherit the main input's claim, and a node whose own math blacks out a region
   states `false` itself — see
   [`rendering.md`](rendering.md#described-images-and-the-retained-edge-domain-claim-issue-92).
   A channel-creating contribution declares `ownsChannelLayout`; Shuffle is the
   example. Ordinary RGBA effects preserve the inherited auxiliary inventory.
   Names are resolved outside pixel loops; see ADR-0008 for the plane contract.
2. Add `Gpu.cpp` with versioned `GpuImplementation`, independent GLSL pixels,
   node-local payload preparation and local pass definitions; keep independent
   Slang kernels beside it. `eval/GpuContribution.hpp` defines the supported
   binding/pass contract. Blur demonstrates scratch images and retained weights;
   Constcolor demonstrates a generator. Do not allocate, submit, wait or access
   the UI from a contribution callback.
3. Add one `NEMO_NODE(slug)` entry to `BuiltinNodes.inc` and normal source/shader
   build wiring. No effect-specific edit belongs in central CPU/GPU dispatch,
   shared request uniforms, the inspector, Commands or serialization.
   `NodeCatalog(vector)` is an exact schema inventory; metadata-only fixtures
   can use `extendedBuiltinSchema(...)`. Executable extensions must instead
   assemble `NodeContributions`/`EffectLibrary` with their real adapters.
4. Assemble before publishing. Duplicate identities, incompatible versions,
   invalid metadata/payload/pass references and promised-but-missing adapters
   fail atomically with the affected relationship. Explicitly unavailable
   backends and missing shader files stay node-local; requesting them fails
   honestly while unrelated supported nodes remain usable. Bump descriptor and
   adapter versions together when pixel code or payload semantics change.
5. Keep ports, parameters, numerical contracts and independent image
   expectations aligned. Read delegates source mapping/media to their existing
   owners; Output and Viewer keep distinct roles. Network instances and formal
   inputs remain structural Evaluation behavior. A genuinely new shared
   execution capability needs a scoped change in its existing owner, not an
   effect-local bypass.

The executable addition example is `tests/contributions/Affine.{hpp,cpp,slang}`
plus `ContributionTests.cpp`: append its declaration to a supplied contribution
vector, assemble through the production builders, and use the resulting catalog
with the ordinary ProjectSession, persistence and CPU/GPU APIs. It is test-only,
not shipped in the artist catalog. Its scale/offset RGB operation preserves
negative/HDR values and alpha; integer shifts translate the output data bounds
and inverse-translate input demand. Independent source labels verify the result
through session history, save/reopen and CPU/native evaluation.

These internal C++ interfaces are not a plugin loader or stable binary SDK.
New types, public interfaces, shaders, dependencies and image baselines still
require owner review; performance claims additionally need the #16 gate.

#### Parameter and inspector boundary

`ParameterSpec` in `NodeCatalog.hpp` owns name, type, typed default, optional
numeric min/max, choice values, and optional presentation metadata (`label`,
`section`, `step`, namespaced `editor`). #75 adds `softMinimum`/`softMaximum`
(soft scrubbing/slider travel that is never a legal bound — a typed value is not
clamped or quantized to it), `displayDecimals` (display rounding only),
`row` (consecutive same-`row` parameters render side-by-side, Transform's
`Translate` X/Y), `channels` (`ChannelHint`: `ChannelLink` Additive/
Multiplicative plus `alphaSeparate`, the linked-RGB editing semantics) and
`nonzero` (excludes zero in addition to any declared numeric bounds).
`initialValue` (`ParameterInitialValue`, issue #92) is the only creation-time
rule: `Default` keeps the schema default, and `OwningNetworkWidth`/
`OwningNetworkHeight` make the creation owner
(`initialNodeParameters` + `addNodeCommand`/`insertNodeOnEdgeCommand`) capture
the owning network's saved canvas dimension as authored state when the node is
created, so a later canvas edit never rewrites it. It is identity, not
presentation, and is legal only on scalar Integer/Float parameters — declare it
in a schema only when the declared range admits every valid canvas dimension.
`ParameterValue.hpp` defines Boolean,
Integer, Float, Choice, Vector2, Vector3, Color and String values; serialization
and shared parameter commands own conversion/validation, not QML. Use those
definitions rather than maintaining a second parameter-type table.

**Typed values and the generic inspector host are delivered.**
[#46](https://github.com/rgamevfx/nemo/issues/46) is closed owner-accepted
(including the `ParameterSpec` metadata addition, the `ParameterEditorRegistry`
interface and the 2×2 default workspace baseline). The production host is
`apps/nemo-ui/qml/ParametersPanel.qml` over the `ViewerController` parameter and
animation surface (`parameterInspector`, key status, key/remove, network-scoped
one-undo parameter gestures); a custom editor registers through the namespaced
`ParameterEditorRegistry` and is selected from catalog `editor` metadata.
#46's double-click activation was replaced by the group-based request in #60;
the panel, accumulation, pinning, columns, keying and unavailable-state
acceptance stand. [#37](https://github.com/rgamevfx/nemo/issues/37) supplies
ColorWarp's custom editor through that same host. Generic controls stay usable
when a registered editor is unavailable, and a schema field or namespaced
editor selector not present in the current catalog remains an owner-reviewed
public catalog change, not an assumed capability.

#75 keeps that host and adds one seam rather than a second inspector. Every
ordinary parameter — numeric, vector, color, choice, Boolean — is a generic row
rendered by `ParametersPanel.qml` from `ViewerController::parameterInspector`:
one aligned value/key/exposure column per row (`NumericField.qml` owns
click-to-type/drag-to-scrub with Shift fine / Control coarse and
`dragDistance`; `ExposureLabel.qml` keeps the parameter-exposure drag;
`KeyIndicator.qml` owns the static/animated/keyed-at-frame state and the
Set/Update Key, Remove Key and Show in Animation menu). `NumericField.qml`
also presents an UNAVAILABLE value: an address whose presentation has no
representable value (a Read offset with no integral alignment) shows a
placeholder and disables scrub/step, while typed entry stays the recovery and
the host commits it through the same shared gesture. A registered editor
(`apps/nemo-ui/ParameterEditorRegistry.hpp`) declares the keys it owns
(`consumes`) and the layout it needs: `presentation: "row"` renders it beside
the ordinary label/key cells, `presentation: "section"` renders it full width
with no outer wrapper for an aggregate control. Consumed keys are omitted from
the generic rows and a fully consumed section is dropped, so exactly one control
renders each setting; an unavailable editor consumes nothing and the generic
rows stay usable, with the refusal reason reported by `editor(id)`.
Section editors address sibling keys of their own node. A single exposed
parameter keeps the generic typed/key/exposure control instead; the host neither
mounts the aggregate editor nor consumes its sibling keys for that interface
address. Row editors remain applicable. This shared rule prevents editing a
definition's unrelated parameters through one instance override (#92).
Single-control updates use `updateNodeParameterEdit`, which retains the resolved
occurrence/exposed address captured at begin. Only aggregate controls send a
concrete-key map to `updateNodeParameterEdits`; an exposed control ID is not a
parameter key on the underlying node.
Boolean refreshes retain their value binding, and choice refreshes restore the
authored selection after Qt resets a changed model (the same deferred readout
pattern as `StudioComboBox`). Opening an inspector must not show a transient
control default in place of the authored value.
`main.cpp` projects the node contributions into `ParameterEditorRegistry`:
Read declares `nemo.read.source`
(section — the Read control presents file/summary/timing/color itself),
Grade declares `nemo.channels.rgb` (`ChannelEditor.qml`, linked RGB with an
expandable labelled R/G/B view and a separate Alpha for Primary/Range
coefficients), Merge declares `nemo.merge.operation`
(`MergeOperationEditor.qml`, the operation menu plus the Swap A/B action), and
Shuffle declares `nemo.shuffle.mapping` (`ShuffleEditor.qml`, the full-width
two-input socket mapper). Shuffle's routing key/exposure cells reuse the shared
controls inside its channel dialog; generic fallback retains all 30 parameters.
Crop declares `nemo.crop.box` (`CropBoxEditor.qml`, x/y/right/top with an
extent-display toggle); Reformat declares `nemo.reformat.format`
(`ReformatFormatEditor.qml`, a compact mode-dependent section with one
composition/named/custom output selector and inline flags). Its adjacent format
popup keeps dimensions as an unapplied draft: Apply authors the node in one
gesture; preset save/update/delete changes only the document-owned registry.
Presets are copied by value. Numeric fields and label exposure/Alt-click keying
reuse the shared controls; label right-click opens the shared key menu without
permanent key-button cells. No private history or renderer state is owned here.
Viewer Crop handles use `ViewerPanel`'s existing image mapping and the shared
batch-parameter gesture/history owner, not an independent undo stack.
Their held-pointer mapping captures scalar camera coordinates; a newly delivered
regional/full-domain raster must not change the coordinate of that pointer.
The three box calculations have different contracts: `CropBoxEditor` displays
signed authored extents (`right - x`, `top - y`) without sorting or rounding;
`ViewerPanel` projects continuous edges while retaining which authored endpoint
each handle edits; node-local `crop/Parameters.hpp` owns discrete floor/ceil
raster enclosure, validation and sampling. Presentation must reproduce the
enclosure origin only when projecting a reformatted image, not use intersected
data bounds as that origin. Keep the frozen #92 coordinate contract and native
reversed-endpoint/upstream-format cases aligned when changing either projection;
do not couple presentation to node-internal sampling helpers.
Linking or collapsing is presentation state: it never
equalizes stored values, Alpha is never edited by a linked RGB change, and
changing editor presentation never changes the effect's execution parameters.

Inspector arrangement lives in workspace `panel.state.inspectors`, not graph
selection. `ParametersPanel.qml` saves arrangement edits there and rehydrates
external state even when a restored layout reuses the same panel ID. The shared
`PinButton.qml` preserves the accepted inspector glyph; inspector accumulation
pins and Animation visibility pins remain independent actions.

For a new Glow-like effect, add schema plus real CPU/GPU execution through the
steps above. The graph catalog discovers it, and the existing generic inspector
renders its schema through `parameterInspector` with no effect-name switches; a
custom control goes through `ParameterEditorRegistry`, never a private QML
editor. The effect task does not change graph selection/wiring, inspector card
layout, theme or docking. Verify values and execution through shared commands,
headless evaluation, and the native inspector host.

### Extend animation editing

`AnimationViewModel` is the panel-local query/edit adapter created by
`ViewerController::createAnimationModel(owner)`. It projects immutable channels
from the application `ProjectSession` using explicit scoped `targets`:
`{network, node}` selects a node, optionally narrowed by `parameter` and
`component`. Definition-local and exposed occurrence channels retain their
display network/node identity. Vector/color components have opaque presentation
IDs but share the typed key's time; editing one component preserves the others.
Unavailable targets never fall back to root or discard other valid targets.

`AnimationPanel.qml` and `AnimationHeaderTools.qml` port #50's archived Track/Curves
presentation. Selection, visibility, framing, scroll, pins and cancellable
previews belong to the panel and its per-group workspace state. Following the
owner-approved #50 refinement, Tracks and Curves show the union of open inspector
cards in the same A–E group and that Animation panel's pins. The router derives
`inspectorNodes` from all Parameters panels in the active workspace, including
inactive tabs; collapsed cards and scrolling do not change membership. There is
no second inspector-membership registry.

Pins identify a scoped node or parameter/component, not a channel ID, so removing
and recreating keys does not lose them. Unpinning returns to inspector-following;
explicit Hide/Isolate masks remain independent. External workspace state
rehydrates reused panels without feeding their own writes back into active
gestures or writing stale state during teardown. Scope/target requests use the
existing `PanelContextRouter` inspector route; playhead changes use the shared
group clock. New effects contribute catalog schema and ordinary animation
channels, not their own editor or graph gestures.

Every committed key edit uses the existing animation commands and shared session
undo/redo. A gesture captures revision and project generation, then commits once;
collisions and stale revisions preserve document/history. The adapter does not
own another animation model, evaluator, save format or undo stack.

Value editing and keying are the same core gesture, not two modes the UI picks.
`ProjectSession::beginValueParameterGesture` (issue #76) captures, at begin,
whether each edited address already has an animation channel — that address then
authors the current-frame key through the existing keyframe factory, preserving
its interpolation, tangents and key identities — or has none, in which case it
takes a static value and never creates a channel. Preview, update, commit and
cancel share the existing lifecycle, the routing cannot change while the gesture
lives, and commit is ONE command and one history entry applying both parts
atomically; a batch may therefore mix an animated choice with its static
companion. `ViewerController::beginNodeParameterEdits`/`updateNodeParameterEdits`
and the Read editor's `commitValues` are the presentation shape of that one
gesture (the single-key calls are its one-element case), so an inspector row
never decides key-vs-static itself. Reset (`resetNodeParameterEdit`) authors the
schema default at the same scope/frame and never removes a channel: Remove Key
stays a separate action. The CLI exposes only the static batch
(`begin`/`update`/`commit`/`cancel-parameter-gesture`); the keyed and mixed
gestures are reached through the UI controller.
[#50](https://github.com/rgamevfx/nemo/issues/50) retains the native comparison
evidence and the outstanding owner image/internal-API review gate.

### Extend desktop history access

`HistoryController` is the session-injected presentation entry point for Undo/Redo
(#86). `Main.qml` and `SubnetParameters.qml` register their existing window
lifetimes. Standard Qt key sequences and shared `HistoryMenu` actions resolve
text buffer → active authored gesture → chronological session history. Exhausted
text history still owns the action. Menu invocation captures its target before
focus can change; `HistoryMenuItem` preserves text focus even on Qt 6.4 menu hover.
Do not add panel-local history shortcuts or restore the retired ViewerController,
AnimationViewModel or MediaLibraryModel history forwarding.

An authored-preview owner registers itself with `setGesture(owner, active)` and
exposes `cancelHistoryGesture()`, which invokes its existing cancellation path.
Graph, Parameters and Animation retain their own transient state machines. Redo
is consumed while a gesture is active; Undo cancels only that preview. A cancelled
control must discard its displayed preview immediately and ignore later motion
or release from the same press. Pan, box selection and other navigation gestures
are not authored-edit registrations.

`CommandStack` retains each command label beside its version/touched identities.
`ProjectSession::undoLabel()` / `redoLabel()` are owner-thread-only, read-only
queries for the next transition. Labels move with history entries; they are not
serialized document content. Availability and labels neither scan the graph nor
retain snapshots. New panels use the shared adapter, not a new session or stack.

For history-routing changes, reuse the native scenarios and distinguish the
production and owner-appearance gates recorded in
[`issue86-history-routing.json`](../evidence/issue86-history-routing.json).

### Extend graph editing

`ViewerController::graphSnapshot(networkId)` is the panel query seam. It returns
availability plus graph nodes/edges with decimal-string identities, typed indexed
ports, authored positions and routes. Formal terminals are projected from Network
metadata as `input:<id>` / `output:<id>` records; their binding wires also have
opaque presentation identities. They are not additional persistent graph nodes.
GraphItem accepts those opaque strings; controller commands resolve them to the
owning model. Graph mutation methods take the network identity
explicitly; an unavailable scope never falls back to editing the root. Root
`graphNodes`/`graphEdges` properties remain read-only root queries, not panel scope.

`GraphPanel.qml` owns the single gesture state machine and transient previews.
It computes prototype placement/snapping; `createGraphNode` submits the anchor,
new position and downstream layout batch together. `addNodeCommand` owns atomic
creation/fan-out insertion; `rewireGraphEdgeCommand` owns endpoint replacement,
including an occupied destination. Route/layout/deletion use the same
`ProjectSession` history. A completed gesture is one commit; canceled previews
do not submit document edits.

`GraphItem` parses plain GUI-thread records and presentation colors. Render-thread
code consumes those records only; hit and paint geometry share the decorated
local port positions. UI selection/view/click anchors live in panel-state
`graphSelections`/`graphViews` keyed by network, while authored node positions and
routes live in Document. Saving that presentation metadata must not reactivate
an unrelated panel; workspace activation follows panel identity, not map changes.

`ViewerController::graphScope(rootNetworkId, instancePath)` resolves occurrence
ancestry and breadcrumbs without querying every node's parameters. GraphPanel
stores `scopePath` in panel state; definition identity alone cannot identify the
parent of a shared network. Navigation changes no document revision. Removing an
entered occurrence unwinds to its surviving ancestor even if another occurrence
keeps the definition alive.

`core/commands/NetworkCommands.hpp` owns collapse/unpack, interface and parameter
promotion, linked creation, independent copies and selection copying. Submit
these through ProjectSession/CommandStack; that owner supplies the private
candidate and atomic publication. Formal-input wires, including wires into
nested instance nodes, live in `Network::inputConnections()`. Instance
`inputBindings` bind parent graph outputs to definition inputs. Input-to-output
pass-through lives in `Network::outputInputBindings()`. Dependency expansion
retains the source scope when forwarding inputs through nested definitions.
Owned local definitions are copied independently and cleaned up with their
owning occurrence; explicitly linked definitions retain their identity.

Network exposed-parameter metadata references a single node/key and derives its
type from the catalog. Promotion/removal does not rewrite the target value or
animation. `ViewerController::subnetExposure(networkId, nodeId)` resolves an
occurrence's definition and link state, `promoteParameter`/`renameExposedParameter`/
`removeExposedParameter`/`moveExposedParameter` submit the shared commands, and
`parameterInspector` presents a subnet occurrence as its definition's exposed
controls, addressed by `exposed:<id>` so a renamed or duplicate label can never
retarget an edit. `SubnetParameters.qml` is the modeless, transient tool window
for the [owner-approved #49 authoring correction](https://github.com/rgamevfx/nemo/issues/49#issuecomment-5655521991).
It remains attached to the selected subnet while graph/inspector context changes.
The shared inspector sends stable identities as `application/x-nemo-parameter`
native drag MIME data across windows; the authoring window also discovers sources
through `graphSnapshot`/`parameterInspector` for its searchable Add parameter list.
Label edits, native drag reorder and removal use the same commands. Promotion
accepts an optional insertion index as one atomic history entry (also available
as `index` on the CLI `promote-parameter` operation). Ordinary values remain in the
shared inspector. Removing exposure leaves source values and animation untouched.
The owner accepted the revised native window on 2026-09-13. The UI regression
uses the source picker and Qt drag/drop events at the separate-window MIME
boundary, including rejection/cancellation and reorder, rather than bypassing
the UI with promotion calls. OS/compositor pointer gestures remain a native
review check, not a claim made by the offscreen event test.

Effect additions use the catalog entry point above, including the category
metadata that drives shared colors and search. They do not extend this gesture
machine. The delivered generic inspector consumes group-scoped graph
double-click inspector requests that carry (group, network, node); #49 owns
hierarchy navigation/collapse and owned-subnet lifecycle.
Neither responsibility is simulated inside the graph editor.

### Add a panel

The current panel registry is `WorkspaceController::registerPanelType` in
`apps/nemo-ui/WorkspaceController.cpp`/`.hpp`. Add the QML body under
`apps/nemo-ui/qml/`, register its descriptor in `apps/nemo-ui/main.cpp` before
`QQmlApplicationEngine::load`, and create it through
`WorkspaceController::createPanel` (which validates the registered type before
calling the Qt-free `Workspace::createPanel`). Panel-local persisted state goes
through `panelState`/`setPanelState`; it is not `Document` state. A panel reads
its own record with `workspace.panelState(panelId)` and re-reads it on
`WorkspaceController::panelStateChanged(panelId)`, the owner-only notification a
state write emits: a state write no longer re-delivers the workspace root, so
one panel's view or preference write does not rebuild another panel's display
model. `rootChanged` remains the arrangement notification (layout, scope, a
project open) that every panel does react to. Panels and the context router,
which derives inspector membership from Parameters panel state, observe that
boundary; do not reintroduce a whole-root read on the per-event path. Do not add
panel switches to the shared shell or make workspace a core/evaluation
dependency.

### Extend media import and the Media Bin

Media Bin work keeps the three layers separate; add to the layer that owns the
change rather than importing decode logic into the panel or catalog state into
the adapter.

1. **Decode, probe and preview** belong to `src/nemo/media/MediaImportService.hpp`.
   One worker thread consumes only the application media contract: stills and
   image sequences through `ImageSource.hpp`, clips through the source decoder's
   bounded single-frame read, and the preview is a display-referred image within
   the Media Bin's requested 160×90 bounds, through the existing OCIO viewing transform — never
   viewer-cache replay and never a synthetic image. The outstanding set is
   bounded; a repeat submission for an outstanding source coalesces to the newest
   reference, and failure is data, not an exception. Results carry the submitted
   request, so re-import or relink re-queues work rather than guessing.
2. **Catalog access and previews** belong to `apps/nemo-ui/MediaLibraryModel.*`,
   the Qt/QML query/command surface over `Document`/`MediaCatalog`. It submits
   validated catalog commands through `ProjectSession`, owns no persistent
   catalog state, and polls the import service on the GUI thread. A runtime
   result is a proposal until `applyProbe` commits it; `relink` copies the
   preserved `SourceReference`. Extend the adapter rather than adding a second
   QML catalog.
3. **The panel** `apps/nemo-ui/qml/MediaBinPanel.qml` is a reactive projection of
   adapter query/selection records plus panel-state view preferences; it submits
   catalog operations and never mutates catalog primitives. `requestTimelineInsert`
   is ordered intent only — insertion and the playhead belong to #54's timeline
   owner — and `revealMediaPanel` activates or creates the group's media panel.
   Explicit open routes `openMediaSource` through `PanelContextRouter` to
   `ViewerController`, which renders the routed catalog reference from a
   request-owned evaluation snapshot; no authored graph node, used-media mark or
   history entry is created for a catalog open. Two-viewer/media-role parity
   remains with #47.
4. **The native chooser** `apps/nemo-ui/NativeFileChooser.hpp` is the shared
   platform seam. It is shared with `ProjectFileController`; each is a separate
   requester and receives only its own requests' outcomes. The panel owns the
   follow-up `importPaths`/`relink` call and its failure presentation. The
   shared filter list and local-path translation live in
   [`MediaChooserSupport.hpp`](../../apps/nemo-ui/MediaChooserSupport.hpp) so the
   Media Bin and the Read node's control cannot drift apart.

### Read node media control (issues #61/#75/#82)

The Read node is the persistent `source` catalog type with display name "Read";
its `source` parameter names a `Document` source key that the CPU reference's
source path, `eval::SourceSession` and the inspector all resolve through the core
source-request resolver. Issue #75 divides what #61 kept together: the shared
`SourceReference` owns media identity (path, content revision, committed facts
and its own authored mapping/interpretation for non-Read consumers), while
**the Read's own choices** are ordinary node parameters authored and animated
like any other setting — `rangeMode`/`rangeFirst`/`rangeLast`,
`frameOffset`/`frameStep`, `beforePolicy`/`afterPolicy`/`missingPolicy`,
`inputTransform`/`inputColorSpace`/`alphaMode` and the fill-only `source*`
encoding hints. Do not move those choices back onto the shared reference, and do
not create a second per-Read record of shared facts.

[`ReadSourceCommands.hpp`](../../src/nemo/core/commands/ReadSourceCommands.hpp)
owns binding and shared-scope repair:

- `registerReadSourceCommand(target, time, path, initializeOverrides, kind, probe)`
  resolves or creates the reference for a chosen path
  (reusing an existing reference and Media Bin entry with the same normalized
  path), commits the validated probe as the entry's facts, and binds it to the
  Read's own `source` parameter as ONE command and one undo entry. It is the only
  authoring path that initializes a Read's choices (fill-only through
  `readInitializationParameters`: a key the scope already authors or already
  animates is never overwritten, so a cleared or keyed Read keeps its trim), and
  it authors the current-frame key when the addressed `source` parameter is
  animated. Schema migration materializes the same translation without an
  authoring command (see "Change project file persistence" below).
- `relinkReadSourceCommand` and `reloadReadSourceCommand` are the explicit
  SHARED repairs: they advance the reference's content revision once and replace
  the committed facts for every catalog entry sharing the key, gate on an
  expected reference (a stale/repointed repair is refused with
  `GraphError::StaleMediaSource`), and never touch node choices, so a custom trim
  survives both. Reload is explicit, not a directory watcher or global cache
  clear.
- Value edits — range, mapping, policies, Input Transform, alpha, hints,
  including File Clear — are **not** owned by a Read-specific command: they go
  through the shared panel value gesture (`ProjectSession::beginValueParameterGesture`
  via `ViewerController`), whose per-address routing decides key-vs-static. One
  gesture is one command and one undo entry.
- `SourceReference::frameOffset`/`frameStep`/`firstFrame`/`lastFrame`/
  `interpretation` remain the authored values of the *shared* reference, enforced
  in the source-scoped image read path (`ImageSource.cpp` reports an
  out-of-range `#`/`@` frame instead of clamping) for consumers that resolve a
  reference directly. A Read does not consult them for its own evaluation:
  resolution is exclusive, so a Read's node mapping replaces them and a migrated
  legacy offset is applied exactly once.

All three lifecycle commands take the probe's classified `MediaKind` explicitly,
never inferring it from the path — `registerReadSourceCommand(target, time, path,
initializeOverrides, kind, probe)`, `relinkReadSourceCommand(sourceKey, expected,
path, kind, probe)` and `reloadReadSourceCommand(sourceKey, expected, kind,
probe)` — and commit it with the probe onto the shared catalog entry. Reload
preserves the existing kind when the supplied kind is `Unknown`; relink first
invalidates the old path's kind and probe. A movie's classified kind (`video`)
and validated interval survive normal register/relink/reload. The controller's
explicit Sequence-vs-Single-Image choice maps a committed Single selection to
`image`.

`apps/nemo-ui/ReadSourceController` is the presentation adapter behind the
registered editor id `nemo.read.source`. It owns no decoder, no value edit and
no second media model: it probes through the Media Bin adapter's one import
worker with `MediaLibraryModel::requestReferenceProbe` (a requester-scoped probe
of a reference that may not be in the catalog yet) and only then submits a
ReadSource command. Every request freezes the ADMITTED occurrence and frame — a
probe/repair completion applies only while the node still resolves that frozen
key and scope, an outstanding request is superseded by generation and dropped
across a project-generation change. One error slot retains its exact address
(network+node+key+occurrence), so it is not displayed on another occurrence;
only success at that same address clears it. A newer rejection replaces the
slot rather than accumulating an error per occurrence. A numbered
selection that matches several files is held as a pending
Sequence-vs-Single-Image choice and document changes only when one is committed;
a cancelled browse publishes nothing. `ReadSourceEditor.qml` is the aggregate
control (presentation `section`) that consumes the Read keys and presents file,
summary, timing and color groups.

The probe's merged `InputColorChoice` comes from core's
`applyReadInterpretationHints` over an empty hint map for a Read that is not yet
bound, and from `resolveSourceRequest` for a bound one — the same merge owner in
both cases, so QML never re-derives hint precedence. Node-authored hints are the
recovery path for rejected media without changing the shared reference's policy:
an untagged stream is refused naming the missing relationship and authors
nothing, the artist authors the missing decode fields through the shared value
gesture, and the artist must EXPLICITLY retry the same selection (there is no
automatic retry); the shared reference keeps the media path only. Start At is a
checked, step-magnitude-aware alias of Offset: `startAtOffset` anchors the
selected first frame for a forward step and the selected last for a reverse one
and throws on overflow; the effective mapping reports a fractional or otherwise
unrepresentable alignment as no value (`EffectiveSourceMapping::startAt()`,
never zero), which the shared `NumericField` presents as unavailable.

Issue #43 evidence: [`session.json`](../evidence/assets/issue43-media-import/session.json).
Issue #75 inspector evidence: [`issue75-inspector.json`](../evidence/issue75-inspector.json).
The evidence records the full-debug run and focused corrections separately
from owner appearance/API approval; passing a smoke check is not that approval.

### Extend shared UI presentation

The existing `Nemo` QML module owns the shared UI library alongside its panels:
`Theme.qml` derives read-only tokens from `WorkspaceController`; `ChromeButton.qml`
and `StudioComboBox.qml` require an explicit `theme`. Appearance defaults,
validation, independent accent/category resets and persistence stay in the
controller. `Main.qml` composes workspace navigation and settings; `Panel.qml`
owns headers, including the visible A–E group selector. Selecting a group
submits `WorkspaceController::setGroup(panelId, group)`; the `panelGroup`
binding and context-router synchronization follow that write. Panel bodies
declare a `theme` property, supplied
by `Loader.setSource` before construction so nested shared controls never read
an uninitialized theme. Other panel context bindings are supplied by `configureLoaded`.
Panel-specific header controls live in the body's optional `headerTools` Component;
`headerToolsFillWidth` requests the viewer's expanding header arrangement and
`headerPreferredHeight` selects compact chrome. Registry `headerSource` is the
alternative for externally supplied header controls. Keep these components with
their panel; the shell has no panel-type switches.

`Theme.qml` also owns pane minimums and splitter size, consumed by both
`WorkspaceNode.qml` and `DockDrag.qml`: drop previews must predict the resulting
geometry and reject splits that cannot fit usable panes. Saved ratios are
clamped to available geometry without changing the saved preference on resize.
Main remains hidden until the native host attaches its Vulkan presentation device.

For native Wayland/Vulkan windows, `main.cpp` supplies the frameless policy as an
initial QML property. Qt 6.4's Vulkan path does not composite its client decoration,
but decorated input still subtracts those margins. `Main.qml` owns system move
and edge-resize requests through `startSystemMove`/`startSystemResize`; establish
the policy before native creation. `Window.flags` has no QML change notification
in this Qt version, so it is not a reactive policy source.

Native pointer acceptance must use compositor/OS-level input on the normal app
window. Qt-injected `QTest` clicks and temporary `Qt.Tool` capture windows bypass
the failing coordinate boundary. The [physical-pointer evidence](../evidence/assets/issue59-native-pointer/session.json)
records the 3/30-pixel failure and zero-offset correction.

For presentation changes, follow the root Development contract and the
pre-edit/reference mapping and intermediate evidence gates in
[`issue-tracker.md`](issue-tracker.md). Shell evidence does not establish
panel-content or complete-workflow parity.

The #59 cutover removes temporary viewer-path, graph-edit-form and timeline-cache
toolbars rather than keeping them behind compatibility menus. The prototype
defines visible controls; unsupported actions remain disabled until their owning
feature ticket implements them. Graph creation reads the production catalog and
submits existing commands through its category menu. Native media verification
uses the Media Bin import/relink path in the media entry point below, or the
existing `--source` option with an explicit OCIO configuration; a temporary
test entry field is not the media-import workflow. GraphItem/TimelineItem
consume plain presentation colors and records, never theme QObjects on the
render thread.

### Add a command

Put a validated command factory beside its owner: graph/document factories are
currently declared in `src/nemo/core/document/Document.hpp` and implemented in
`Document.cpp`; domain-specific factories live in
`src/nemo/core/commands/`. The factory returns `nemo::Command`, validates the
candidate mutation, and is submitted through `ProjectSession::submit()` so
revision, undo/redo, notifications, and automation remain identical. Wire a
headless operation only through `apps/nemo-cli/ProjectSessionCommand.cpp::makeCommand()`;
use the existing `transactionCommand` for an atomic multi-edit. The concrete
existing example is `addNodeCommand(...)` used by both `makeCommand()` and
the catalog-backed graph creation controller.

### Change project file persistence (`.nemo`)

`.nemo` is plain versioned JSON: the core codec's authored document plus a
file-owned `presentation`/`colorConfig` envelope the codec never sees. Keep
ownership layered rather than adding a UI or CLI codec — `ProjectFile` owns
read/write, path policy, version/envelope handling and reference state;
`ProjectSession` owns the path, dirty baseline, save-completion generation and
recovery guard; the `Document` codec owns the authored schema and
required-feature metadata. `ProjectFileController` and the CLI file ops call
these owners; the File menu and pending-quit prompt bind
`ProjectFileController`'s chooser/open/save/saveAs/recover invokables, and
headless automation uses the CLI `file-state`/`open`/`save`/`save-as`/
`autosave`/`recover` ops, so neither re-implements the protocol.

- **Unknown data.** Unknown authored fields and records in supported schemas
  are retained as JSON values, so a load/save round-trip preserves them. An
  unknown or newer required feature, or a newer `schema`, is rejected with an
  actionable diagnostic rather than guessed. The presentation envelope is
  versioned independently and headless callers never interpret it; a newer
  envelope is retained verbatim with a warning, not rewritten from guesswork.
- **External references.** Authored sources and the color configuration resolve
  to absolute in-memory targets at read time; the write path policy is
  `KeepStored`, portable `RebaseRelative` (Save As) or `RebaseAbsolute`. A
  normal Save never packages media, and unknown opaque presentation paths are
  never resolved. Missing or unresolved references are reported with
  identity+path for relink or media-owned sequence resolution. A registered
  color-configuration reference (`nemo::kBuiltinColorConfigUri` in
  `Document.hpp`, the pinned ACES Studio URI) is not a file: it ignores the
  path policy, is stored and reopened verbatim, and is reported present by
  construction; any other URI-looking string stays an ordinary path reported
  honestly. A new project adopts media's `newProjectColorDefault()` through
  `ProjectSession` (never by changing `ColorPolicy`'s literals), and an
  explicit `$OCIO` override wins.
- **Read ownership migration (schema 5).** Loading a schema-4 document
  materializes the formerly shared Read timing/range and recognized
  interpretation keys onto each Read scope (every network including nested
  definitions, and each occurrence that repoints a Read at another source key)
  through the deserialization path, so no controlled-edit transition is recorded
  and the loader reports it as a warning. The migration and Read-node
  construction share the one translation in `SourceRequest.hpp`; the shared
  reference is left intact for other consumers and a migrated node hint is
  fill-only, so reliably tagged media still wins. There is no legacy/new Read
  mode and no second per-node interpretation record.
- **Atomic write.** `ProjectFile::writeAtomic` writes a temp sibling, flushes
  and fsyncs, replaces, and keeps a previous-good backup; a failure never
  destroys the last valid target.
- **Save lifecycle.** `prepareSave` captures an owned immutable snapshot plus
  session file state on the owner thread with no I/O; a worker performs the
  write; `commitSave` publishes the outcome and makes the written snapshot the
  baseline. A save that finishes for an older snapshot stays dirty, and saving
  never creates an undo entry. The native chooser only suggests the `.nemo`
  name: the application honors the exact destination it returns and never
  appends or alters a suffix after overwrite confirmation, so an extensionless
  Save As cannot silently overwrite a different `<stem>.nemo`.
- **Autosave and recovery.** The UI-owned timer runs every 120 seconds while
  the document or presentation is dirty and retains 3 slots; `AutosaveStore`
  only enforces storage and never writes the project target. A recovery copy
  is an unsaved project with no source path and a protected original, so it is
  kept only by saving elsewhere (Save As) and can never silently replace its
  original. Slots holding newer-schema or unknown-required-feature data are
  preserved rather than recycled, and the store fails visibly when no safe slot
  remains.
- **Project color configuration.** A non-empty authored `colorConfig` reaches
  `ViewerSession` (through `ViewerRuntime`/`ViewerScheduler` and the
  cache-viewer path) as that session's OCIO config; an empty path keeps the
  existing `$OCIO` environment fallback. No process-global state is mutated.
- **#58 scope.** `.nemonet`/`.nemopreset`/`.nemoworkspace` custom asset formats
  are not implemented here; they must reuse this `.nemo` owner rather than add
  parallel format codecs.

Issue #35 evidence: [`issue35-persistence.json`](../evidence/issue35-persistence.json).

## Verification and review gates

Use the repository's debug/release/sanitizer commands in `AGENTS.md` and the
rendering-specific evidence rules in [`rendering.md`](rendering.md). For the
headless ownership path, use these exact commands:

```bash
cmake --preset headless
cmake --build --preset headless --target nemo_core_tests
build/headless/tests/nemo_core_tests --gtest_filter='CatalogTest.*:ProjectSessionTest.*:CommandStackTest.*'
```

The architecture check is exercised by configuration through
`nemo_check_dependency_direction()` (there is no source-text assertion target).
CI job `headless-core` runs the same headless ownership path and its negative
configure hook, checking the dependency-direction diagnostic. A negative proof
must use:

```bash
cmake --preset headless -DNEMO_TEST_FORBIDDEN_DEPENDENCY=ON
```

and observe configuration failure. Core, session, and catalog work must remain
buildable with both `NEMO_BUILD_UI=OFF` and `NEMO_BUILD_GPU=OFF`.

UI changes additionally require launching the native UI and human inspection of
visible behavior; headless or model-only checks are not UI evidence. Image
baselines, public APIs, new node types, and dependencies require explicit owner
review.

### Focused static analysis (clang-tidy 18)

The required analysis gate covers the `nemo_core` target and the core headers
selected by the root [`.clang-tidy`](../../.clang-tidy) `HeaderFilterRegex`
scope: the module sources under `src/nemo/core/` and the headers they include.
It does not analyze UI, GPU, media, runtime evaluation (`src/nemo/eval/`), CLI
or test code.

The pinned analyzer is **clang-tidy 18** (Ubuntu 24.04 package `clang-tidy-18`).
The opt-in CMake switch `NEMO_ENABLE_CLANG_TIDY=ON` installs the analyzer as the
`nemo_core`-only compile-time launcher with `--config-file=<root>/.clang-tidy`.
Configuration fails with a message naming the tool when it is missing or its
version does not match 18; explicit analysis never silently degrades to a clean
result. Warning failure is owned by the root `.clang-tidy` `WarningsAsErrors`
policy, which is the single authority, so a selected diagnostic fails the run.

The gated check set is selected in the root `.clang-tidy` and is bounded to
correctness-oriented families: the `clang-analyzer-core`, `-cplusplus`,
`-deadcode` and `-unix` path checks, the correctness-oriented `bugprone-*`
checks, plus `performance-for-range-copy`, `performance-move-const-arg` and
`performance-unnecessary-copy-initialization`, whose findings are actionable in
this scope. Style (`readability-*`) and modernization (`modernize-*`) checks are
explicitly outside the required gate.

Requirements: CMake 3.28+ with Ninja, a C++20 compiler (CI uses `g++-13`),
`VCPKG_ROOT` exported to the pinned vcpkg toolchain, and clang-tidy 18 on the
path. Qt, Vulkan and generated shaders are not needed because the preset keeps
UI, GPU, CLI and tests off. Run the gate with the single command shared by local
development and the `analysis` CI job:

```bash
cmake --workflow --preset analysis
```

The `analysis` workflow preset configures with the `analysis` configure preset
(headless, plus `NEMO_ENABLE_CLANG_TIDY=ON`) and then builds only `nemo_core`
with two build jobs. The build step is clean-first on purpose: clang-tidy runs
as part of compilation, so an incremental build skips up-to-date translation
units and can report success without analyzing anything. Cleaning first makes
every invocation actually re-analyze the gated scope instead of no-op passing.

The build is verbose, so the workflow log records the launcher, the
`--config-file` it used and one clang-tidy command per analyzed core translation
unit. That makes execution visible: an empty or missing invocation list is a
failed run, not a clean result.

For the initial gate's clean/negative execution and compatibility evidence,
see [`issue67-static-analysis.json`](../evidence/issue67-static-analysis.json).

External contributions remain behind the provisional gate in
[`0004-provisional-licensing.md`](../decisions/0004-provisional-licensing.md):
accept external PRs only after relicensing or recording a CLA/DCO that grants
the required relicensing rights. This guide does not change that decision or
approve a release license. Use [`issue-tracker.md`](issue-tracker.md) for issue,
claim, PR, and evidence workflow; agents execute checks and prepare evidence,
but the owner remains the final reviewer for public API, node, dependency, and
image-baseline changes.
