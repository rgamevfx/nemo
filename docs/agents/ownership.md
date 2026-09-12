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
extension path.

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
| Project file persistence, autosave, recovery | `src/nemo/core/session/ProjectFile.hpp`; `ProjectFile` owns read/write, path policy, file envelope and reference state; `AutosaveStore` owns bounded slots; `ProjectSession::prepareSave`/`commitSave` own path, dirty baseline and recovery guard | `apps/nemo-ui/ProjectFileController.*` and the CLI `file-state`/`open`/`save`/`save-as`/`autosave`/`recover` ops are the only callers; both go through this owner, never a second codec |
| Node schemas and discovery | `src/nemo/core/nodes/NodeCatalog.hpp`; immutable `NodeDescriptor` records and `NodeCatalog(std::vector<NodeDescriptor>)` | Built-ins are assembled in `NodeCatalog.cpp`; `Graph::catalog()` and evaluation query the catalog. There is no unregister operation |
| Validated edits and history | `src/nemo/core/document/Document.hpp` command factories plus `src/nemo/core/commands/`; `Command`, `CommandStack`, and `ProjectSession` | `apps/nemo-cli/ProjectSessionCommand.cpp::makeCommand()` maps JSON operations; `ProjectSession::submit()` is the commit seam |
| CPU evaluation and shared plan | `src/nemo/core/evaluation/CpuReference.hpp`; `evaluateCpu`, `expandDependencies`, `scheduleDependencies` | `apps/nemo-cli/main.cpp` uses `evaluateCpu`; input is a `const Document` snapshot and the CPU image is reference-owned |
| GPU primitives and resource lifetime | `src/nemo/gpu/` (`Device`, `Allocator`, `Submit`, `ComputePass`) | `src/nemo/eval/GpuExecutor.cpp` records/submits work; follow [`rendering.md`](rendering.md) for retained ownership and synchronization rather than copying those rules here |
| Native effect execution | `src/nemo/eval/GpuExecutor.hpp` (`EffectProgram`, `EffectLibrary`, `loadSlangEffectLibrary`, `glslEffectLibrary`, `submitGpu`, `evaluateGpu`) and the CPU reference [`NativeEffects.hpp`](../../src/nemo/core/evaluation/NativeEffects.hpp) (`evaluateNativeEffect`) | [`Params.hpp`](../../src/nemo/core/evaluation/Params.hpp) owns the typed effective parameters (`effectiveEffectMask`/`effectiveGrade`/`effectiveBlur`/`effectiveTransform`) both executors consume; `CpuReference.cpp` dispatches `grade`/`blur`/`transform` to `evaluateNativeEffect`. `ViewerSession` loads the Slang library. Numerical contract and spatial limits: [`rendering.md`](rendering.md) |
| Optional input ports and absent slots | `src/nemo/core/nodes/NodeCatalog.hpp` (`PortSpec::optional`); `Plan.hpp` slot model with `CpuReference.cpp` expansion | An absent optional slot keeps its declared port position as `EvaluationNodeId{}` (node == `kInvalidNode`) in `ExpandedNode.inputs`/`PlanStep.inputs`; `Reuse.hpp` marks it in result identity with `kAbsentInputKeyHash`; `GpuExecutor` binds the main image as a valid dummy descriptor with `maskPresent=0`, never an allocated fallback. Grade/Blur/Transform's only optional input is port 1 `mask` |
| Media and color adapters | `src/nemo/media/` public image/viewing contracts; external OIIO/OCIO/FFmpeg types stay behind `.cpp` adapters | CLI probe/render and `eval::SourceSession` consume the application media contract. `src/nemo/media/ImageSource.{hpp,cpp}` owns still/sequence `SourceReference` decode (pattern resolution, declared-color interpretation, scene-linear straight-alpha conversion, headless `ImageSourceProvider`) — reuse it rather than adding a second still reader |
| Media import, probing and preview | `src/nemo/media/MediaImportService.hpp`; `MediaImportService`, `MediaImportRequest`/`MediaImportResult`, `inspectMediaSource` | One service worker decodes, probes and reduces a display-referred preview off the GUI thread; the queue is bounded and results carry request identity. `apps/nemo-ui/MediaLibraryModel.*` is the production consumer, requests previews within 160×90 bounds, and owns no decoder |
| Media Bin catalog adapter | `apps/nemo-ui/MediaLibraryModel.*`; Qt/QML query/command surface over `Document`/`MediaCatalog` | Submits validated catalog commands through `ProjectSession`; owns transient probe results and the bounded display-referred `QImage` thumbnail cache/provider, not persistent catalog state. A runtime probe is a proposal until `applyProbe` commits it; `relink` copies the preserved `SourceReference` |
| Media Bin panel presentation | `apps/nemo-ui/qml/MediaBinPanel.qml` | Reactive adapter records plus panel-state view/selection preferences; it submits catalog operations, emits ordered `requestTimelineInsert` intent for #54, reveals through `revealMediaPanel`, and opens explicitly only through `MediaLibraryModel::openMediaSource` |
| Native file chooser | `apps/nemo-ui/NativeFileChooser.hpp`; `openFiles`/`saveFile` with a requester-owned `OutcomeHandler` | One platform implementation per build; each request belongs to its requester and no outcome is broadcast. `ProjectFileController` and `MediaLibraryModel` are separate requesters of the same chooser |
| Workspace arrangement and panel state | `apps/nemo-ui/Workspace.hpp`/`Workspace.cpp`; Qt-free `Workspace`, `Panel`, and `Workspace::createPanel` | `WorkspaceController::registerPanelType`, `createPanel`, and `setPanelState` are the Qt/QML boundary; `main.cpp` registers production panels before QML loads |
| Presentation and input | `apps/nemo-ui/WorkspaceController.*`, `ViewerController.*`, and `apps/nemo-ui/qml/` | Presentation reads state and submits commands; it does not own `Document`, evaluator, or GPU resource state |
| Headless automation | `apps/nemo-cli/`; JSON-lines project-session protocol and `nemo-cli` commands | `ProjectSessionCommand.cpp::makeCommand()` is the current command dispatch seam; do not invent a second command registry |

### State and threading boundaries

- `ProjectSession` methods and subscriptions are owner-thread-only. Its
  callbacks run synchronously after publication and must not block or mutate
  during notification. Workers receive `ProjectSession::snapshot()` copies,
  never the session object.
- `NodeCatalog` is an immutable snapshot after construction. Descriptors contain
  schema facts only: no Qt, Vulkan, plugin, executor, or image objects.
- Evaluation consumes an immutable document view. `ViewerScheduler` is the
  thread-safe queue/policy boundary and retains immutable snapshots; it rejects
  stale publication without waiting for in-flight GPU work.
- GPU submission, compilation, allocation, and completion belong off the UI
  event thread. GPU work retains every referenced resource until completion;
  consult [`rendering.md`](rendering.md) for the complete execution contract.
- `Workspace` owns serializable arrangement/panel state and has no Qt or
  rendering dependency. `WorkspaceController` and QML are presentation-thread
  state; hand work to that boundary through the Qt application machinery rather
  than sharing mutable workspace state with workers.

## Contribution entry points

### Add a node or effect

There is no generic runtime effect registry in the current tree. A built-in
node/effect is a coordinated change to the existing catalog and executor seams:

1. Add the immutable schema in `src/nemo/core/nodes/NodeCatalog.cpp` and append
   it in `NodeCatalog::NodeCatalog()`. For a supplied extension/fixture,
   construct `NodeCatalog(std::vector<NodeDescriptor>)`; keep identifiers
   namespaced and descriptors free of runtime objects.
2. Add the CPU reference implementation beside the existing implementations in
   `src/nemo/core/evaluation/CpuReference.cpp` and its explicit type dispatch in
   `evaluateCpu` (for example, the `constcolor` branch). An effect with its own
   numerical contract follows the Grade/Blur/Transform shape: shared typed
   metadata and admissibility in `src/nemo/core/evaluation/Params.hpp`, pixel
   math in `evaluateNativeEffect` (`NativeEffects.{hpp,cpp}`), dispatched from
   the `grade`/`blur`/`transform` branch.
3. For a native GPU effect, add the Slang kernel under
   `src/nemo/gpu/shaders/`, include its type in
   `eval::loadSlangEffectLibrary`; keep the runtime GLSL reference in
   `src/nemo/eval/EffectShaders.hpp` and its type in `glslEffectLibrary`. A
   multi-pass effect registers each pass as its own library key (`blurHorizontal`
   is the internal first pass beside the node-visible `blur`). `EffectLibrary`
   is the current execution input, not a new public registry.
4. Keep catalog ports/parameters (including the optional mask port declared by
   `effectInputs()`), CPU behavior, GPU binding contract, and implementation
   version aligned, and update the numerical contract in
   [`rendering.md`](rendering.md). A new node type, public interface, shader,
   dependency, or image baseline requires owner review; a performance claim
   additionally requires the #16 reference gate. Passing an agent check is not
   approval.

Current examples/use sites: `constColorDescriptor()` plus the `constcolor`
CPU/GPU paths; the #34 native effects — `gradeDescriptor()`/`blurDescriptor()`/
`transformDescriptor()` with `evaluateNativeEffect`, `Params.hpp` metadata and
the matching Slang kernels (contract in [`rendering.md`](rendering.md)); and
catalog-backed graph creation submitting `addNodeCommand(...)`. Unknown declared
types fail explicitly when an executor has no implementation; do not silently
substitute another effect.

#### Parameter and inspector boundary

`ParameterSpec` in `NodeCatalog.hpp` owns name, type, typed default, optional
numeric min/max, choice values, and optional presentation metadata (`label`,
`section`, `step`, namespaced `editor`). `ParameterValue.hpp` defines Boolean,
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

For a new Glow-like effect, add schema plus real CPU/GPU execution through the
steps above. The graph catalog discovers it, and the existing generic inspector
renders its schema through `parameterInspector` with no effect-name switches; a
custom control goes through `ParameterEditorRegistry`, never a private QML
editor. The effect task does not change graph selection/wiring, inspector card
layout, theme or docking. Verify values and execution through shared commands,
headless evaluation, and the native inspector host.

### Extend graph editing

`ViewerController::graphSnapshot(networkId)` is the panel query seam. It returns
availability plus nodes/edges with decimal-string identities, typed indexed ports,
authored positions and routes. Graph mutation methods take the network identity
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
through `panelState`/`setPanelState`; it is not `Document` state. Do not add
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
   follow-up `importPaths`/`relink` call and its failure presentation.

Issue #43 evidence: [`session.json`](../evidence/assets/issue43-media-import/session.json).
The human image/API/schema review of this surface remains open; passing an agent
or smoke check is not approval.

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
  identity+path for relink or media-owned sequence resolution.
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
buildable with both `NEMO_BUILD_UI=OFF` and `NEMO_BUILD_GPU=OFF`. UI changes
additionally require launching the native UI and human inspection of visible
behavior; headless or model-only checks are not UI evidence. Image baselines,
public APIs, new node types, and dependencies require explicit owner review.

External contributions remain behind the provisional gate in
[`0004-provisional-licensing.md`](../decisions/0004-provisional-licensing.md):
accept external PRs only after relicensing or recording a CLA/DCO that grants
the required relicensing rights. This guide does not change that decision or
approve a release license. Use [`issue-tracker.md`](issue-tracker.md) for issue,
claim, PR, and evidence workflow; agents execute checks and prepare evidence,
but the owner remains the final reviewer for public API, node, dependency, and
image-baseline changes.
