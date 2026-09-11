# Ownership and Contribution Guide

This is the implementation map for ownership and contribution entry points. The
product contract remains [`composition_network_vfx_nle_spec_v2.md`](../composition_network_vfx_nle_spec_v2.md),
durable trade-offs remain in [`docs/decisions/`](../decisions/), and the
workflow for issues and pull requests remains [`issue-tracker.md`](issue-tracker.md).
Do not create a parallel registry or instruction hierarchy when a responsibility
already has an owner below.

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
| Node schemas and discovery | `src/nemo/core/nodes/NodeCatalog.hpp`; immutable `NodeDescriptor` records and `NodeCatalog(std::vector<NodeDescriptor>)` | Built-ins are assembled in `NodeCatalog.cpp`; `Graph::catalog()` and evaluation query the catalog. There is no unregister operation |
| Validated edits and history | `src/nemo/core/document/Document.hpp` command factories plus `src/nemo/core/commands/`; `Command`, `CommandStack`, and `ProjectSession` | `apps/nemo-cli/ProjectSessionCommand.cpp::makeCommand()` maps JSON operations; `ProjectSession::submit()` is the commit seam |
| CPU evaluation and shared plan | `src/nemo/core/evaluation/CpuReference.hpp`; `evaluateCpu`, `expandDependencies`, `scheduleDependencies` | `apps/nemo-cli/main.cpp` uses `evaluateCpu`; input is a `const Document` snapshot and the CPU image is reference-owned |
| GPU primitives and resource lifetime | `src/nemo/gpu/` (`Device`, `Allocator`, `Submit`, `ComputePass`) | `src/nemo/eval/GpuExecutor.cpp` records/submits work; follow [`rendering.md`](rendering.md) for retained ownership and synchronization rather than copying those rules here |
| Native effect execution | `src/nemo/eval/GpuExecutor.hpp`; `EffectProgram`, `EffectLibrary`, `loadSlangEffectLibrary`, `glslEffectLibrary`, `submitGpu`, `evaluateGpu` | `src/nemo/eval/Viewer.cpp::ViewerSession` loads the Slang library. The evaluator owns execution; effects never add Qt or persistent-model dependencies |
| Media and color adapters | `src/nemo/media/` public image/viewing contracts; external OIIO/OCIO/FFmpeg types stay behind `.cpp` adapters | CLI probe/render and `eval::SourceSession` consume the application media contract |
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
   `evaluateCpu` (for example, the `constcolor` branch).
3. For a native GPU effect, add the Slang kernel under
   `src/nemo/gpu/shaders/`, include its type in
   `eval::loadSlangEffectLibrary`; keep the runtime GLSL reference in
   `src/nemo/eval/EffectShaders.hpp` and its type in `glslEffectLibrary`.
   `EffectLibrary` is the current execution input, not a new public registry.
4. Keep catalog ports/parameters, CPU behavior, GPU binding contract, and
   implementation version aligned. A new node type, public interface, shader,
   dependency, or image baseline requires owner review; passing an agent check
   is not approval.

Current examples/use sites: `constColorDescriptor()` plus the `constcolor`
CPU/GPU paths, and `ViewerController::addGraphNode()` calling
`addNodeCommand(...)`. Unknown declared types fail explicitly when an executor
has no implementation; do not silently substitute another effect.

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

### Extend shared UI presentation

The existing `Nemo` QML module owns the shared UI library alongside its panels:
`Theme.qml` derives read-only tokens from `WorkspaceController`; `ChromeButton.qml`
and `StudioComboBox.qml` require an explicit `theme`. Appearance defaults,
validation, independent accent/category resets and persistence stay in the
controller. `Main.qml` composes workspace navigation and settings; `Panel.qml`
owns headers, context badges and descriptor-provided header tools. A panel may
expose `headerPreferredHeight` for compact chrome. Keep panel-specific composition
with its panel rather than adding panel-type switches or a second UI system.

`Theme.qml` also owns pane minimums and splitter size, consumed by both
`WorkspaceNode.qml` and `DockDrag.qml`: drop previews must predict the resulting
geometry and reject splits that cannot fit usable panes. Saved ratios are
clamped to available geometry without changing the saved preference on resize.
Main remains hidden until the native host attaches its Vulkan presentation device.

For presentation changes, start from the immutable
[prototype source/evidence baseline](../evidence/issue25-prototype-acceptance-v1.md),
port the actual shared pattern, and retain matched native images and gesture
evidence. The prototype is the precise design contract; owner approval is
required for deviations. [#40](https://github.com/rgamevfx/nemo/issues/40) owns
the shared shell/library; [#59](https://github.com/rgamevfx/nemo/issues/59) owns
existing panel-content cutover. Shell evidence does not establish panel-content
or complete-workflow parity.

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
`ViewerController::addGraphNode()`.

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
