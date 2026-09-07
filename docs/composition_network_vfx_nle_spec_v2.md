# Unified VFX Compositor and NLE

## Product and architecture specification · Revision 2.2

**Purpose:** A professional node-based compositor with integrated editing and color workflows. Nuke defines the primary compositing interaction model; Houdini informs network hierarchy, reusable tools, and programmatic control; Resolve informs continuity between editing, VFX, color, and delivery.

This specification consolidates the concept decisions and targeted technical architecture. “Must” denotes required behavior or an architectural constraint. The stack in Section 10 is the implementation target, subject to the prototype gates in Section 11; it is not a claim of implemented capability. Format coverage, exact platform baselines, and quantitative performance budgets remain separate technical decisions.

## 1. Product principles

- **Primary user:** a Nuke compositor. The strongest initial power user also understands Houdini. Editing and color must remain approachable to generalists and familiar in flow to Resolve users.
- **Two primary editing surfaces:** a conventional NLE timeline and a Composition Network editor. Viewers, parameters, scopes, and other panels support those surfaces.
- **One procedural network model:** VFX, color, effects, adjustment processing, generators, subnets, and reusable tools use the same underlying graph type.
- **Editorial structure belongs to the timeline.** Users do not need to learn or maintain a node representation of a timeline.
- **Workspaces arrange tools; they do not define separate processing systems.** Switching workspace must not alter the evaluated image.
- **Interactive performance is a core feature.** Preview quality may be reduced explicitly; final-quality evaluation must remain correct and reproducible.
- **Public APIs are a product foundation.** Built-in workflows, extensions, Python, and agent adapters use the same validated application operations; automation must not depend on UI simulation.

## 2. Core objects and ownership

| Object | Definition and responsibility |
| --- | --- |
| Media source | Reference to footage, an image sequence, audio, or other supported source, including interpretation metadata. |
| Timeline / sequence | Conventional tracks, clip occurrences, transitions, audio, and editorial timing. Can contain media, compositions, and nested sequences. |
| Clip occurrence | One placement of content in a timeline. Owns its placement, source mapping, and occurrence-specific processing. Multiple occurrences may reference the same content. |
| Composition Network | A procedural graph with a formal typed input/output interface. Can be standalone, nested, or used as timeline content or processing. |
| Network instance | A use of a network definition with input bindings and parameter values. Sharing is explicit. |
| Published asset | A reusable, versioned network definition with an exposed interface, available through the tool shelf and node creation UI. |
| Workspace | Saved panel arrangement, panel roles, and context-linking rules. |

Graph dependencies and ownership are separate. A Color network can read a VFX composition without owning or duplicating its definition. The UI must identify the current processing scope and indicate shared definitions.

### Processing scopes

| Scope | Changes apply to |
| --- | --- |
| Source | All consumers of that source interpretation or explicitly shared source processing. |
| Clip occurrence | One timeline placement, including its default Color treatment. |
| Composition | All uses of that composition's definition. |
| Explicit group | Members assigned to shared processing, such as a shot look. |
| Sequence | The assembled sequence at its designated processing stage. |

The default shot path is **media → VFX composition, when needed → occurrence Color network → timeline assembly**. Sequence finishing can follow assembly. Empty processing stages are pass-through and require no manual setup.

## 3. Composition Network contract

- Networks support named, typed inputs and outputs. The architecture accommodates image/media, masks, geometry, data, and audio; the initial supported node/type inventory is a separate implementation decision.
- A valid evaluable network has at least one **Output** node. New networks create a default Output automatically. Incomplete graphs remain saveable but show validation errors when evaluated.
- Multiple outputs are permitted. One compatible output is designated as the default for timeline playback; other consumers bind to named outputs explicitly.
- **Viewer** nodes or viewer assignments inspect any compatible intermediate result. Viewing a node does not change the formal output.
- **Write / Export** nodes may branch from any compatible point, in any quantity. They execute only when explicitly requested or queued, never as a side effect of ordinary playback.
- Networks can contain subnets, reference other networks, and evaluate independently of a timeline.
- Selecting nodes and collapsing them into a subnet preserves behavior and exposes boundary connections. Users can promote parameters and customize the subnet interface.
- Network creation, connections, parameters, hierarchy, and asset instantiation must be accessible programmatically as well as through the UI. Expressions and parameter references must have inspectable dependencies.
- Invalid type connections and circular evaluation dependencies are rejected with an explanation identifying the offending relationship. Nesting a timeline and network within one another is allowed only when dependencies remain acyclic.

**Output defines the result. Write defines an explicit deliverable. Cache and bake control how results are reused.**

## 4. Timeline and composition integration

### 4.1 Ordinary editing

The timeline provides conventional placement, stacking, trimming, slipping, retiming, transitions, audio synchronization, and nested sequences. Quick transforms, resizing, opacity, and effects expose direct controls backed by the common processing system. Users do not need to open a graph for routine edits.

An ordinary clip can expose its processing through **Open in Graph** without requiring a rendered conversion. This operation preserves its editorial identity and relationships. Building a composition from one or more clips is a separate action.

### 4.2 Create Composition from selected clips

This is a one-time conversion from an editorial assembly into a normal artist-editable VFX graph:

1. Use the earliest selected visible start as composition-local time zero. Set the initial duration to the selected assembly's end minus that start.
2. Reference original sources independently; do not render the selection into a single flattened image.
3. Capture relative placement, trims, source offsets, and retimes as explicit editable input settings or timing nodes.
4. Translate supported transforms, opacity, effects, transitions, and track stacking into processing and merge operations in their original evaluation order.
5. Replace the selected assembly with one composition clip at the same parent-timeline range. Preserve its visible result and relevant audio relationships.
6. Retain source availability beyond the initial visible range where available; trimming does not destructively discard source handles.
7. Retain originating source/clip identifiers and captured timing as provenance. They are not live editorial links.

The resulting composition has **no automatically generated internal timeline**. Its graph is not rebuilt when the parent edit changes. Creation must be undoable as one operation.

If unselected tracks are interleaved with selected material, or an effect/transition depends on content outside the selection, exact replacement may be impossible. The operation must identify the dependency and require selection expansion or an explicit rendered fallback; it must never silently change the image. Unsupported effects likewise require an explicit choice before conversion.

Audio must not disappear or double-play. Audio not explicitly moved into a supported composition audio path remains in the parent timeline with synchronization links preserved.

### 4.3 Timing semantics

Every composition has local time independent of parent placement. Source time and composition time are connected by explicit mappings. Frame-rate conversion is explicit and stable; moving an item does not reinterpret its frame rate.

| Operation on a composition clip | Required behavior |
| --- | --- |
| Move | Changes parent placement only; internal animation and timing stay fixed. |
| Trim | Changes the visible local-time interval. |
| Slip | Changes the local-time interval sampled without moving the parent item. |
| Retime | Changes sampling of the completed composition. |
| Retime an internal source | Changes that source before downstream VFX operations. |
| Extend beyond available coverage | Shows the affected range and applies the input's explicit boundary policy; no silent timing shift. |

Inputs added directly use source range and adjustable local offsets. Inputs created from timeline selections use captured timing values. **Follow Timeline Clip is excluded:** inputs do not track the old clip's position after conversion.

Tracking and other time-dependent analysis retain source and analyzed-range information. Changes that invalidate that analysis must be visible. Viewer time displays can show both local and parent-sequence time.

### 4.4 Explicit sequence inputs and adjustment networks

A sequence can be explicitly supplied as a network input—for example, an edited montage used in a screen replacement. This reads the sequence result, not a former clip's placement. Editing the referenced sequence updates its dependent result.

An adjustment network receives the assembled content beneath its timeline position over its active range, before that adjustment is applied. It uses the same network type with a context-bound input. Its binding must exclude its own result and downstream adjustments.

## 5. VFX and Color

- VFX and Color use the same node editor, node definitions, graph evaluation, and nesting system. Context-specific controls change presentation, not semantics.
- The default Color network belongs to a clip occurrence and reads that occurrence's media or VFX result. Reusing a VFX composition does not automatically share its grade.
- Opening Color for a selected clip resolves this stage automatically. Opening VFX resolves the upstream composition or clip-processing context.
- If a clip is graded before VFX is created, creating its VFX stage preserves the existing downstream grade. Multi-clip conversion preserves existing per-input processing order rather than relocating grades indiscriminately.
- Users may explicitly place color operations inside a composition, share looks, or add sequence finishing. The UI must show their scope and processing order.
- Color controls, scopes, shot navigation, and comparison views are workspace presentations of the same processing system. A focused Color view must still make the full graph path accessible.
- Project color policy explicitly defines source interpretation, working spaces, viewer transforms, and delivery transforms. Viewer transforms are not baked into composition outputs unless deliberately authored as processing or delivery operations.

## 6. Node editor and workspaces

### Nuke-oriented interaction

The default node editor must closely match Nuke's interaction patterns for node creation/search, wiring, selection, placement, keyboard-driven operations, navigation, viewer assignment, parameter editing, and entering/exiting groups. Visual modernization must preserve familiar spatial behavior. Exact shortcut and gesture mappings require a dedicated interaction specification; visual similarity alone is insufficient.

Houdini-inspired capabilities include consistent hierarchy, exposed interfaces, reusable definitions, parameter references, and programmatic graph construction. These capabilities extend the familiar compositing workflow without requiring them for a simple comp.

### Modular workspaces

Panels support splitting, tiling, resizing, tabbing, rearrangement, and saved user presets. Default workspaces are configurations of the same panels:

| Workspace | Primary emphasis |
| --- | --- |
| Conforming | Media references, relinking, metadata, sequence inspection. |
| Editing | Timeline, source/program viewers, bins, direct clip controls, audio. |
| VFX | Node graph, viewer, parameters, channels, animation and analysis controls. |
| Color | Viewer, scopes, grading controls, shot navigation, graph access. |
| Exporting | Outputs, ranges, delivery settings, render queue and status. |

Switching workspace preserves the active shot and corresponding playhead context. Saved layouts do not force users back to a previously active shot.

### Panel linkage

Each contextual panel has a compact badge offering **Follow Active**, **A–E link groups**, or **Pinned**. Groups may have names and optional colors; letters remain visible. Hovering a badge highlights related panels.

Groups share relevant context such as composition, node selection, and playhead. Viewer target is independently pinnable: a graph and parameter panel can follow selection while a viewer remains on final output. Pinned panels resist unrelated selection changes and display the pinned target. Missing targets show an explicit unavailable state.

## 7. Copying, linked instances, and reusable tools

| Command | Required behavior |
| --- | --- |
| Copy / Paste | Copies selected network structure independently. Existing media references remain references; explicitly linked assets retain their declared links. |
| Copy as Linked Instance / Paste | Places a linked instance at the paste destination. If needed, establishes a shared definition for the original and pasted uses. |
| Make Independent | Detaches the selected instance while preserving its effective graph and values. Nested linked assets remain linked unless explicitly detached. |
| Publish to Tool Shelf | Saves a reusable, versioned asset with named inputs, outputs, exposed controls, and discoverability through shelf/search. |

Local groups are not linked by default. Creating a linked copy does not require finding or organizing a definition folder first. Local shared definitions persist with the project.

Linked instances expose a visible link indicator and **Go to Definition**. Definition edits affect linked uses; parameter overrides affect only their owning instance. Arbitrary internal structural overrides are excluded from the initial model: edit the definition or make the instance independent.

Published assets use pinned versions and explicit updates. Saving a new asset version does not silently update existing projects. Missing definitions produce a recoverable missing-asset state rather than substituting unrelated content.

## 8. Interactive evaluation, cache, bake, and export

### Interactive evaluation

The engine must prioritize current-frame work, cancel obsolete requests, reuse unaffected branches, and evaluate only required regions/channels where supported. Preview controls include resolution and node-supported quality reductions. Refinement after interaction stops is configurable.

The viewer and timeline must distinguish full-quality, reduced-quality, pending, and outdated results. Quality substitutions must be explicit; unsupported approximations must not silently change node meaning.

Cache identity must include relevant sources, graph/parameter state, time, resolution, quality, channels, and color configuration. Affected cached results invalidate on dependency changes. Approximate results must not satisfy full-quality requests.

### Artist-facing commands

| Action | Contract |
| --- | --- |
| Cache for Playback | Builds managed, disposable results for a selected range at preview or full quality. Invalidated results recompute; storage may be evicted. The live graph remains authoritative. |
| Bake and Use Render | Creates a persistent render of specified outputs/range and selects it as a playback representation while retaining the editable network. |
| Update Bake | Regenerates the persistent representation from the current graph. |
| Return to Live | Stops substituting the bake and evaluates the retained network. |
| Write / Export | Produces an explicitly requested deliverable with selected output, range, format, and delivery settings. |

Cache and bake commands are available directly from a timeline composition's context menu and from the graph. Users do not need to insert Write nodes to accelerate playback.

Bakes record graph/source state, range, channels, quality, and color interpretation. Upstream edits mark them outdated without silently overwriting them. Requests outside their coverage, or requiring unavailable channels, use live evaluation or report the missing coverage; a limited bake cannot masquerade as a complete network result.

Final-quality exports use current full-quality evaluation or a verified matching full-quality representation. Using an outdated bake for delivery requires an explicit choice. Source graphs are retained regardless of cache eviction or bake removal.

## 9. Acceptance scenarios

1. A plate at sequence frame 200 and overlay at 212 become a composition with local starts 0 and 12. Moving that composition preserves the offset and its internal animation.
2. Editing a converted graph does not trigger timeline reconstruction. Each original source remains separately editable.
3. Two occurrences can read the same VFX composition and have independent grades. Editing the shared VFX definition updates both; editing one occurrence's grade updates only that occurrence.
4. A Color-to-VFX-to-Editing workspace switch retains the shot and corresponding time; a pinned comparison viewer remains pinned.
5. Copy as Linked Instance followed by Paste creates the instance at the chosen destination. Definition edits propagate, instance overrides remain local, and Make Independent stops propagation.
6. Caching from the timeline improves reuse without adding Write nodes. Editing an upstream node invalidates affected cache results and marks a bake outdated.
7. A sequence may feed a screen-replacement composition in another sequence. Attempting to make it depend on its own result is rejected with the dependency identified.
8. Composition creation either preserves the selected assembly's result or identifies the unsupported/external dependency before conversion; it never silently drops effects or audio.

## 10. Target stack and architectural rules

### 10.1 Stack selection

Target **C++20 + Vulkan + Slang + Qt 6**, with Windows and native Linux Wayland as first-class desktop targets. C++ is selected for direct integration with the proposed UI, media, and VFX ecosystem. Clean ownership, dependency boundaries, and evaluation design are mandatory.

| Layer | Target | Role in this application |
| --- | --- | --- |
| Core | C++20 | Document model, network relationships, timeline semantics, commands, evaluation, and caching. |
| GPU backend | Vulkan | Image-processing compute, GPU memory, synchronization, and viewer resource delivery. |
| Native shaders | Slang compiled to SPIR-V | Primary implementation path for GPU image effects. |
| GLSL ingestion | glslang compiled to SPIR-V | Additional shader authoring path using the application's effect contract. |
| Desktop UI | Qt 6 Quick/QML with custom C++ items | Modular workspaces, controls, graph, timeline, and viewer integration. |
| External effects | Dedicated OpenFX host adapter | Plugin discovery, lifecycle, parameters, image requests, rendering, and interaction. |
| Video and audio I/O | FFmpeg libraries | Decode/encode and container handling behind application-owned interfaces. |
| Image I/O | OpenImageIO and OpenEXR | Still-image and image-sequence access, including declared channel/metadata support. |
| Color management | OpenColorIO | Source, working, viewing, and delivery transformations under project policy. |
| Automation | Python bindings, headless CLI, optional MCP adapter | Clients of the common application API for inspection, editing, previews, and jobs. |
| Build | CMake + Ninja; pinned dependencies | Repeatable Windows/Linux builds and independently buildable modules. |

Qt Widgets remains a fallback UI choice if prototype results favor it; mixing UI frameworks throughout the application is not the default. Direct Vulkan is preferred over a broader GPU abstraction initially because resource interoperability and explicit scheduling are central requirements.

Slang supports SPIR-V output, and glslang provides a GLSL-to-SPIR-V path. FFmpeg and OpenImageIO supply media APIs rather than an application graph model. See [Slang](https://shader-slang.org/slang/user-guide/spirv-target-specific), [glslang](https://github.com/KhronosGroup/glslang), [FFmpeg](https://www.ffmpeg.org/about.html), and [OpenImageIO](https://openimageio.readthedocs.io/).

### 10.2 Module boundaries and ownership

| Module | Owns | Boundary rule |
| --- | --- | --- |
| Document | IDs, sources, sequences, network definitions/instances, parameters, versioned serialization | No Qt, Vulkan, or plugin-runtime objects in the persistent model. |
| Commands | Validated edits, transactions, undo/redo | UI, extensions, scripts, and agent adapters use the same mutation API. |
| Evaluation | Dependency plans, time mapping, scheduling, invalidation, cache policy | Operates headlessly; does not depend on panel selection or layout. |
| GPU | Allocations, kernels, command submission, synchronization | Exposes resource handles with explicit lifetime and completion rules. |
| Media | Decode/encode, source metadata, image I/O | Converts external representations to the application's media contract. |
| OpenFX | Plugin instances, suites, compatibility and capability negotiation | Adapts plugins to evaluation; does not define core node semantics. |
| Presentation | Panels, input, selection, view models, drawing | Reads model state and submits commands; never mutates render state directly. |
| Automation | Python, CLI, and MCP adapters to public queries/commands | No adapter dependency in core evaluation or per-pixel native execution. |

Render requests consume immutable or equivalently stable evaluation snapshots. Every request/result carries a revision and request identity so obsolete work cannot overwrite a newer viewer result. Editing must remain responsive during decode, compilation, rendering, caching, and export; these tasks must not block the UI event thread. GPU submissions may finish after cancellation, but stale results must be discarded and resources retained until completion.

Use RAII, explicit resource ownership, bounded queues, and documented threading contracts. Avoid mutable global application state, general-purpose service locators, and cross-module access to internal data structures. Third-party types remain behind adapters where practical. Native plugin calls must respect advertised thread-safety constraints.

### 10.3 How network relationships become evaluation

The authored graph and executable plan are different representations. The evaluator may expand subnets and compile timeline assembly into internal tasks, but must not expose or persist an editable generated timeline graph. Conversion from a timeline selection still produces ordinary authored nodes once, as defined in Section 4.

Each evaluation request identifies an output, local time, required region/channels, quality, and effective parameter/input state. The evaluator resolves network references, applies instance overrides, maps time, and schedules only required dependencies. VFX and Color are not separate engines.

For two timeline occurrences sharing a VFX composition, the evaluator may reuse the VFX result when requested time, inputs, parameters, and quality match. Their different Color networks produce separate downstream results. Moving an occurrence changes its parent time mapping without changing the composition definition; editing its grade invalidates that downstream branch. Editing a shared VFX definition invalidates dependent branches in every affected occurrence.

Cache identity includes effective inputs and overrides, node/asset implementation versions, source revisions, time, image requirements, and relevant color policy. Shared definition identity alone is insufficient for reuse. Viewer display transforms are downstream viewing operations and must not contaminate reusable composition results.

### 10.4 GPU and native effect contract

Native effects are packages containing shader code, typed ports, parameter/UI metadata, implementation version, supported quality modes, and declared region/channel/temporal dependencies. A shader compiler alone does not establish effect compatibility: bindings, coordinates, sampling, alpha conventions, color interpretation, and output formats must conform to the application contract. Arbitrary OpenGL programs are not drop-in GLSL effects.

The image contract must represent dimensions, bounds, pixel aspect, named channels, precision, alpha association, and color interpretation. Image processing uses floating-point representations as required; final precision is declared by operation and pipeline policy. Preview precision reductions must be marked and cannot satisfy higher-quality requests. CPU and GPU implementations are compared with operation-specific tolerances rather than an unsupported promise of universal bitwise equality.

GPU resources use explicit lifetime and synchronization rules with budgeted allocation/reuse. Avoid routine CPU readback between native GPU effects and the viewer. Capability-dependent external sharing may require transfers; measure and expose their cost in profiling. GPU acceleration is the native target, not a promise that every media decoder or third-party node executes on GPU.

Shader compilation and pipeline creation run asynchronously and are cached. Compilation errors identify the node and source location where available. No implicit effect fallback may silently change the image. Full-resolution requests that exceed memory budgets must use supported tiling/spill strategies or report a clear failure rather than silently reducing quality.

### 10.5 OpenFX compatibility boundary

OpenFX support is a tested capability matrix. A Vulkan renderer does not automatically support a plugin's GPU API. The documented OpenFX rendering suite uses OpenGL; viewer interacts can require legacy OpenGL or the newer drawing suite. See [OpenFX rendering](https://openfx.readthedocs.io/en/main/Reference/ofxRendering.html) and [interacts](https://openfx.readthedocs.io/en/main/Reference/ofxInteracts.html).

Implement and validate in this order:

1. CPU rendering, lifecycle, parameters/animation, time requests, bounds, formats, and declared threading behavior.
2. Viewer overlays, pointer/keyboard interactions, and coordinate mapping into the color-managed viewer.
3. OpenGL acceleration and tested Vulkan/OpenGL interoperability on supported drivers, with explicit transfer fallback where possible.
4. Additional GPU extensions driven by the target plugin compatibility list.

Advertise only implemented suites and capabilities. A plugin without a compatible render path is unavailable with a reason; CPU fallback applies only when that plugin supports it. Plugin state and required versions persist with the project. Scan plugins outside the main UI process to contain discovery failures. Render-process isolation is a separate prototype decision because it changes resource sharing and latency; in-process rendering does not guarantee plugin crash containment.

### 10.6 Modular UI implementation

Use Qt Quick/QML for panel chrome, tabs, controls, and menus; use custom C++ scene-graph items for dense graph/timeline drawing, curves, scopes, and GPU viewer integration. Cull invisible content, batch geometry where appropriate, and generate thumbnails/waveforms asynchronously. Do not create a heavyweight control tree for every wire, frame, or keyframe.

Qt supports native Wayland clients and Qt Quick rendering through Vulkan. Its scene graph is a UI rendering mechanism, distinct from the application's Composition Networks. Native resource sharing still requires explicit synchronization and prototype validation. See [Qt and Wayland](https://doc.qt.io/qt-6/wayland-and-qt.html), [Qt Quick scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html), and [Vulkan integration](https://doc.qt.io/qt-6/qtquick-scenegraph-vulkanunderqml-example.html).

The workspace model owns a serializable split tree, tab groups, panel type/instance IDs, sizes, and context-binding rules. A panel registry creates panels from these definitions. Adding a panel type must not require changes to graph evaluation. Layout state is separate from project processing state; closing a panel neither deletes a network nor changes its output.

A context router implements Follow Active, A–E groups, and pinning using stable document IDs. Composition context, selected node, playhead, and viewer output target are distinct fields; relevant updates propagate without feedback loops. A Color workspace binds controls to the same parameter model used by the graph editor. QML bindings present values; command transactions own their edits and undo behavior.

Windows and native Wayland are required validation targets, not merely build targets. Test text/IME input, clipboard, drag/drop, shortcuts, tablet input, mixed-DPI screens, multi-window behavior, and layout restoration. Unsupported window placement requests must degrade gracefully. HDR/reference-monitor output requires separate display-path validation; Vulkan and color-managed shaders alone do not guarantee it.

### 10.7 Maintainability and contribution rules

- Keep headless evaluation buildable and runnable without the desktop UI. Provide module-level build targets and small public interfaces.
- Pin dependency versions and document one reproducible setup path per OS. Keep formatting, static analysis, and relevant sanitizer jobs in CI.
- Version project schemas, asset interfaces, and native effect contracts. Provide migration tests; retain unknown/missing node data so projects can recover when dependencies return.
- Provide a minimal native effect example, an automation example, and contributor documentation explaining ownership, threading, cache invalidation, and module boundaries.
- Test timing/undo semantics, serialization, image correctness, OpenFX compatibility, and cache invalidation. Maintain representative performance workloads rather than relying on isolated kernel benchmarks.
- Decide project licensing before public contributions. Track dependency licenses and build options, including per-module Qt terms and media-library configuration. The stack does not itself select the application's license. See [Qt licensing](https://doc.qt.io/qt-6/licensing.html).
- Treat lightweight operation as measurable startup, idle CPU, RAM/VRAM, install footprint, and response latency. Load expensive subsystems/assets on demand and bound caches; do not replace mature libraries solely to reduce dependency count.

## 11. Technical prototype gates

Before freezing the stack, build a vertical slice: **decode → native Slang effect → OpenFX effect → OpenColorIO viewing transform → viewer**, alongside a functioning graph and timeline in a tiled workspace.

| Gate | Required evidence |
| --- | --- |
| GPU/UI integration | Correct resource synchronization and no routine viewer readback on the chosen native path. |
| Interactive behavior | Scrubbing, cancellation, edits during rendering, resizing, and workspace switching remain responsive. |
| Graph reuse | Shared VFX plus independent grades reuse and invalidate only the appropriate results. |
| Shader contract | Equivalent sample Slang/GLSL effects bind correctly and pass declared image tolerances. |
| Plugin support | Representative CPU/OpenGL plugins render and interact correctly; incompatible paths report clearly. |
| Platform behavior | Pass on Windows and native Wayland across a declared GPU/driver and desktop test matrix. |
| Resource pressure | Measured RAM/VRAM budgets, cache eviction, bake coverage, and failure recovery. |
| Reproducibility | Headless and interactive full-quality outputs agree within declared tolerances for the same project state. |

Record hardware, media, resolution, graph, quality, cache state, and driver for each benchmark. Set numerical latency, throughput, startup, and memory budgets from these workloads before making release performance commitments. UI framework choice, external GPU sharing, plugin isolation, and minimum Vulkan/device requirements remain provisional until these gates are evaluated.

## 12. Public API, extensions, and agent control

### 12.1 Extension model

The application API is the shared foundation. Plugins, Python, the headless CLI, and MCP are clients of that API, not separate implementations of project behavior. MCP is an optional external adapter; the application and renderer must work without it.

| Layer | Extension mechanisms | Intended use |
| --- | --- | --- |
| Creative tools | Published networks, native shader packages, OpenFX | Reusable compositions, looks, generators, and image effects. |
| Application extensions | Registered commands/panels, Python tools, versioned native interfaces | Workflow tools, custom controls, import/export adapters, and publishing integrations. |
| Automation | Python, headless CLI, MCP | Inspect projects, edit graphs/timelines, request previews, and manage jobs. |

Network assets require no native compilation. Native shader packages follow Section 10.4; OpenFX follows its own host contract in Section 10.5. These extension types must not be conflated into one binary SDK.

### 12.2 API coverage and state separation

Public interfaces expose typed queries and commands over stable IDs. Names, graph positions, and timeline positions are editable properties, never object identity. IDs are persisted with their owning document; API references include document identity where needed.

| Domain | Required public operations |
| --- | --- |
| Editorial | Inspect/create sources, sequences, occurrences, time mappings, transitions, and occurrence processing; create compositions from selections. |
| Networks | Discover node types; create/delete nodes; inspect/edit ports, connections, parameters, animation, subnets, and instance overrides. |
| Assets | Publish definitions, inspect dependencies/versions, instantiate, update explicitly, and make independent. |
| Evaluation | Request named outputs/previews; inspect diagnostics; start/query/cancel render, cache, and bake jobs. |
| Presentation | Query/update selection, viewer targets, panel instances, workspace layouts, and context groups when a UI session exists. |
| Persistence | Save/load projects, inspect missing dependencies, and report schema compatibility. |

Document state, UI-session state, and evaluation/job state remain separate. Inspecting or rendering a composition must not change selection or open a panel. Presentation-only calls report unavailability in headless mode; document and render operations remain usable. Queries support bounded results and filtering so tools need not retrieve an entire project to inspect one shot.

### 12.3 Commands, transactions, and concurrency

- All public document edits use validated commands. Direct mutation of internal objects is unsupported, including from built-in panels and Python.
- A transaction groups edits into one atomic document change and one undo step. Validation includes types, dependency cycles, scope, and referenced objects. Failure leaves no partial document edit.
- Mutations accept an expected document revision. Conflicts return the current revision and affected-object information where available; clients must re-query rather than silently overwrite intervening artist edits.
- Successful edits return created/affected IDs and the resulting revision. Retryable external mutations and job submissions support request identifiers to prevent duplicate execution within a documented retry window.
- Undo follows document history. An agent cannot undo an older edit through intervening user edits by assuming it still owns the top undo entry; revision checks apply.
- File writes, external publishing, and render jobs are explicit side effects outside atomic document transactions. Undoing a graph edit does not claim to reverse an external export.
- Structured errors distinguish invalid arguments, missing objects, revision conflicts, unavailable capabilities, and evaluation failures. Include node/source locations where applicable.
- Capability discovery reports API versions, available node schemas, registered commands, plugin capabilities, and current access rights. Clients must not infer support from display names.
- Events announce document revisions, changed IDs, selection/context changes, diagnostics, and job progress. Subscribers can detect missed events and resynchronize; the UI thread must not wait for external consumers.

Example: an agent creates a screen-replacement network by submitting one transaction containing node creation, connections, parameter values, and output assignment. The application validates and commits it once. Preview rendering is a separate asynchronous request against the committed revision.

### 12.4 Developer interfaces and plugin lifecycle

Provide Python bindings for queries and commands, plus a headless CLI suitable for scripted verification. Python orchestrates operations; it is not the native per-pixel execution path. APIs must expose the same instance-sharing, scope, and time-mapping semantics as the artist UI.

The native extension boundary targets a versioned C ABI with opaque handles and an optional C++ convenience SDK. Do not expose internal C++ classes, STL containers, or Qt objects across that stable binary boundary. Define ownership, allocation/free pairs, threading, callback lifetimes, errors, and version negotiation. Exceptions must not cross the boundary. Exact ABI declarations are a follow-on specification.

Extensions declare identity, version, required API range, dependencies, and registered capabilities. Commands, panels, and node types use namespaced identifiers. Incompatible extensions report a reason; missing nodes retain serialized state for recovery. Extension versions that affect evaluation participate in cache identity. Hot unload is not required initially; never unload native code while instances, callbacks, or GPU work still reference it.

Panels register through the panel registry and use public view models and commands. Built-in and extension panels obey the same layout and context-linking rules. The first panel SDK should use host-provided UI facilities; an unrestricted native Qt panel bridge, if added, must be explicitly tied to a compatible application/Qt build rather than advertised as a stable C ABI feature.

Native and embedded Python extensions execute trusted code in their host process unless explicitly isolated. Registration metadata and API permissions alone do not sandbox such code.

### 12.5 MCP and agent access

Expose an optional MCP adapter with task-oriented operations backed by the public API:

- Inspect a shot, composition, network scope, or missing dependency.
- Discover node types, port schemas, parameter schemas, and application capabilities.
- Validate and apply a graph/editorial transaction with an expected revision.
- Request a preview for an explicit output, time, range/region, and quality.
- Start, query, and cancel render/cache/bake jobs.
- Retrieve structured diagnostics and relevant document-change events through supported adapter mechanisms.

Previews return image data or retrievable artifacts plus the document revision, output, time, resolution, quality, and viewing-transform metadata. Agents must be able to distinguish a current full-quality result from an approximate or obsolete preview. Job responses include stable job IDs, status, progress when available, diagnostics, and cancellation state.

Agent access is opt-in. Separate read/inspect, document-edit, render, file-export, and arbitrary-code capabilities. Start with inspection/previews; add transactional editing when conflict handling and undo are verified. Arbitrary Python or shell execution is not implied by MCP access and is disabled through that adapter by default.

Bind local control to an explicitly configured session; remote/network access requires authentication and explicit enablement. Enforce granted capabilities in the application service layer, not merely in prompts or tool descriptions. Show active connections and provide revocation. Keep credentials out of project files and diagnostic logs.

Agent mutations are attributed in history with client identity, transaction description, and revision. User editing remains available; no long-running render or agent session holds an indefinite document edit lock. Protocol transport details and exact MCP schemas remain adapter specifications and must not leak into the document model.

### 12.6 Delivery order and acceptance gates

1. Implement stable IDs, headless queries, commands, transactions, revision checks, and undo.
2. Expose Python and a small example tool using only public operations.
3. Implement native effect packages and OpenFX hosting against the evaluation interfaces.
4. Build an example extension that registers a panel and commands without private access.
5. Add MCP inspection/previews, then transactional editing and job control.
6. Stabilize the public SDK after these clients exercise it; document compatibility and migration policy before third-party adoption.

Required tests: an external client can create a composition, edit it, render it, and save/load it without private APIs; a just-committed transaction undoes as one step; a revision conflict leaves the document unchanged; inspection does not move selection; cancelled/obsolete jobs cannot overwrite a newer preview; denied capabilities are rejected at the service boundary; missing extensions preserve recoverable state. Python, UI, and MCP must produce equivalent document behavior for equivalent commands.

## 13. Boundaries and remaining technical specifications

Excluded from the initial design: an editable timeline-as-nodegraph surface; generated internal timelines for converted compositions; live references to originating clip positions; implicit linking of ordinary groups; arbitrary structural overrides inside linked instances.

Separate specifications must define the node/tool inventory, exact Nuke-oriented input mappings, media/interchange and codec coverage, audio device/playback implementation and feature depth, concrete API/ABI schemas, extension packaging and compatibility policy, MCP transport/authentication details, OS/GPU/driver minimums, serialization schema, color/display validation, and measured performance budgets. Windows and native Linux Wayland are selected targets; their exact supported environments remain to be qualified. These are unresolved implementation requirements, not promises of existing compatibility or capability.
