# 0007 — Evaluator result reuse and invalidation

Date: 2026-09-07
Status: Accepted
References: issue #9, spec revision 2.3 §§8, 10.3, ADR-0004

## Context

The evaluator (issue #1 CPU reference, issue #8 native GPU execution) is a
pure function: every request re-executes the full dependency chain and
allocates fresh results. Spec sections 8/10.3 require reuse of matching
branch results, dependency-scoped invalidation, representation coexistence,
and publication freshness (stale work never overwrites newer results). No
document revision, result cache, or publication guard existed.

## Decision

1. **Content-derived keys, not history-derived.** A node result's identity
   (`ResultKey`, `src/nemo/core/evaluation/Reuse.hpp/.cpp`) covers the
   implementation version, node type, the node's authored parameter state,
   the keys of its effective inputs in declared port order, the mapped
   local time, region, full image domain, sampling scale, channels, quality,
   working space, source reference content (including media revision), and
   (for the GPU path) a fingerprint of the supplied effect library. Keys are computed
   before execution, so authored state is what is hashed:
   executor-injected defaults are a deterministic function of the
   implementation version. A node with an explicit default value and one
   relying on the default get different keys — conservative: this can
   only miss reuse, never serve a wrong result. The canonical form is
   length-prefixed per field (injective over arbitrary strings), so hash
   collisions and content aliasing cannot cause wrong reuse; equality
   compares the canonical form.

   Consequences: dependency-scoped invalidation falls out of key
   computation — an edit changes the keys of exactly the affected results;
   shared VFX with different grades reuse the shared upstream results;
   structurally identical occurrences share results regardless of node
   identity; unrelated document edits (including revision bumps) never
   invalidate branch reuse.

2. **Conservative key fields.** Mapped local time enters every node's key,
   even for operations that ignore it (e.g. `constcolor`). This is a
   superset of effective dependencies: correctness over hit rate. Per-type
   temporal-dependence refinement (and region/channel support-region
   narrowing) can tighten keys without changing the contract.

3. **Viewer state is excluded from scene-linear keys** (ADR-0004). A
   viewer/delivery-transform edit invalidates `viewerResultKey()`
   representations — the identity seam for the viewer-cache tickets #11/#12
   — while upstream scene-linear reuse stays valid.

4. **Publication freshness is a ticket, not invalidation.**
   `ResultCache::beginTicket(document)` captures the document revision
   (`Document::stateRevision()`: command generation + graph edit counter + color policy + source content)
   and a request generation. A computed result publishes only while the
   ticket still matches; superseded publications are discarded and counted
   (`staleRejected`), never stored. Reuse identity deliberately ignores the
   revision, so the guard never becomes an invalidation broadcast.

5. **Bounded residency, one policy for both residencies.** `ResultCache<T>`
   is a core header template instantiated for `CpuImage` (core) and
   `eval::GpuNodeImage` (eval module); GPU-resident results are never
   converted to CPU buffers for caching. The entry cap is a drop-oldest
   bound; the LRU/disk eviction policy of issue #14 builds elsewhere on
   this bound. Eviction is distinct from invalidation: an evicted valid
   identity may be computed again on demand.

6. **Plan evidence.** `PlanStep::cacheReused` and the reuse/miss/stale
   counters make avoided work observable without exposing storage layout.

## Alternatives considered

- **Document-revision-keyed cache entries** (compare revision to reuse):
  rejected — every unrelated edit would invalidate all branch reuse, which
  spec section 10.3 forbids.
- **Per-node-id cache maps**: rejected — content identity (shared VFX,
  equivalent occurrences, moved compositions) would not reuse across node
  identities.
- **Observer/event invalidation wired into `Graph`**: deferred — key
  computation already scopes invalidation; events matter only for #13's
  asynchronous scheduler and explicit UI-facing invalidation commands.

## Non-goals / deferred

Automatic LRU/budget policy and disk eviction (#14) remain separate.
Compressed viewer-cache representations (#12) and viewer presentation
(#11) implement their respective representation and ownership contracts.

## Application scheduling integration (#13)

`eval::ViewerScheduler` applies bounded admission and current-frame priority
without Qt or GPU ownership. Requests retain immutable Document snapshots.
Explicit ranges use one descriptor per destination and expand only when
selected for execution; ordinary scrubbing never creates a range.

Each destination has its own publication token. Superseding one viewer
does not make another viewer's result stale. Cancellation changes eligibility,
not GPU completion: #22 retains submitted resources through completion.
`ViewerRuntime` performs decode, compilation, rendering, and presentation
preparation on its worker, with destination-local result mailboxes.
Range requests have a separate identity from interactive scrubbing. Worker
failures remain visible for that range until cancellation or supersession.
Nonblocking cache-counter snapshots distinguish evaluation/admission from
persisted frames, queued encoding, rejected admission, and asynchronous errors.

Publication and reusable history remain distinct. Moving the playhead rejects
an obsolete viewer result but may preserve its already-requested cache history.
The asynchronous cache writer checks scheduler eligibility before publishing;
cancellation cannot be undone by later resubmitting the same revision.
Neither scheduling nor publication freshness removes unrelated committed
content-keyed representations.
Pending publications coalesce by shared content identity, not destination:
destination-local freshness must not place a duplicate identity twice in the
same encoded chunk and retire that chunk while publishing its second entry.
Coalesced work retains independent eligibility for each contributing
destination; any still-current producer can publish the shared image.
The destination registry and contributor set are bounded to 64 destinations.
Late cancellation during metadata I/O prunes only obsolete identities and
preserves original codec offsets; the index is rewritten without re-encoding
or reevaluating surviving frames.

The graph and timeline panels use the same ViewerController facade and
explicitly composed ProjectSession. The session owns one Document and
CommandStack per open project, so both panels share history. The graph exposes
node creation, connections, parameter editing, and output selection. Timeline
source strips expose the existing persistent source offset/step mapping and
shared playhead. They do not claim clip-occurrence move/trim support: that
model is not yet present. Unknown source coverage is
shown explicitly rather than inferred from the ruler's visible extent.
Dense graph and timeline content uses culled, batched C++ scene-graph items.
QML owns chrome and a single selected-source inspector; offscreen nodes,
connections, and source strips do not create per-element control trees.

## Revisioned editing integration (#29)

`ProjectSession::revision()` is a monotonically increasing, session-local
edit revision, not an image reuse key or the renderer's publication token.
Every public submit/undo/redo supplies an expected revision; conflicts return
the current revision and typed errors without publishing a partial edit.
Rendering compares `Document::stateRevision()` with the immutable request
snapshot. This query is pure, including on worker threads. Each successful
command, undo, and redo advances its command generation, so restoring source
or color values cannot make an obsolete render current.

`CommandStack` validates on a private candidate, prepares session event/retry
records, then publishes the document and history without allocation. Bounded
history retains document states, not a second collection of mutable inverse
callbacks. This trades whole-document history storage for atomic rollback
and stable redo identities; there are no render-path copies added by history.
Creation watermarks survive undo and serialization, including deletion of
the highest live ID. Existing JSON schema-1 IDs are restored exactly; legacy
missing IDs produce migration warnings. This does not select the final
project container or recovery format (#32/#35).

Queries return bounded, filtered value copies, ordered by stable node/edge
ID or source/parameter key. Limits are clamped to 256; exclusive cursors
allow pagination. Clients must not combine pages across revisions.
`changesSince(revision)` reports network-scoped node/edge IDs, network and
instance changes, source IDs, and color policy changes; an expired,
unavailable, or future event cursor requires resync.
Notifications are synchronous, owner-thread-only, nonblocking callbacks;
subscriptions must not outlive their session. Edits during a command or
notification are rejected.

Nonempty request IDs identify operations across submit/undo/redo in one live
session. The last 256 successful identified operations replay their original
result, even after subsequent edits or undo. IDs are limited to 256 bytes.
Failures are not retained; a restarted session has no retry history.
Reusing an ID means retrying the original operation, not a different payload.
File writes and render jobs remain outside document history.

Evaluation requests address `(NetworkId, NodeId)`; names are labels. QML
transports node IDs as decimal strings to preserve the entire 64-bit range
instead of rounding through JavaScript numbers. The current viewer retains
its node selection within the explicit root network; independent panel
network routing remains separate work.

The desktop-free consumer `nemo-cli project-session <project.json>` accepts
JSON-lines on stdin and emits one JSON result per line. It keeps one session
alive across requests and does not write the input project. For example:

```json
{"op":"query","network_id":1,"limit":16}
{"op":"transaction","expected_revision":1,"request_id":"gesture-a","commands":[{"op":"rename-node","network_id":1,"node_id":1,"name":"plate"},{"op":"set-param","network_id":1,"node_id":1,"key":"note","value":{"type":"string","value":"authored"}}]}
{"op":"changes","since":1}
{"op":"undo","expected_revision":2,"request_id":"undo-a"}
```

Edits require `expected_revision`; `request_id` is optional. Commands include
`add-node`, `set-param`, `reset-param`, `set-parameters`, `rename-node`,
`connect`, `transaction`, `undo`, and `redo`. Graph queries and child graph edits require `network_id`; mutation
addresses additionally use `node_id`, `from_node_id`, and `to_node_id`.
Ports use `from_port` and `to_port`. Query filters include `filter`, `type`,
`name`, `node_id`, `key_filter`, and `source_filter`; paging uses `node_after`,
`edge_after`, `key_after`, and `source_after`. Query results identify the
current revision; rejected edits distinguish revision conflicts, missing
objects, invalid arguments, unavailable operations, and reentrant mutation.

## Typed composition-network integration (#45)

The Document owns named definitions by `NetworkId`, an explicit root, and
document-scoped network instances. Each definition owns its graph once.
Node/edge/interface IDs are local to that definition; names and layout do
not identify objects. New definitions create an Output automatically.
Consumers select its default Output or explicitly request another Output;
deleting the selected node leaves an incomplete, saveable network rather
than silently selecting another result.

Formal terminals have stable IDs, names, and Image/Mask/Media types.
`bindInstanceInput` authors parent input bindings by formal port ID;
ordinary graph connections into an instance are rejected so there is no
second authoritative binding. Instance parameter overrides address a node
in the shared definition and a parameter key. Values use the typed
representation described below (#51). These records own no evaluator,
decoder, Qt, GPU, or plugin-runtime object.

`expandDependencies` resolves only dependencies of the requested output,
including lazy formal inputs. Nested outputs become routing aliases, not
effect kernels. Expanded identities include the occurrence path so two
outer uses of the same nested definition cannot collide. CPU and GPU share
dependency/parameter metadata, not pixel implementations. Content keys still
permit reuse across equivalent occurrences; publication freshness remains
separate. `evaluate` and `evaluate-gpu` accept `--network-id`; omitting it
selects the document's explicit root.

All reconciliation occurs on the mutable command candidate before
publication, or during schema restoration. Const snapshot queries never
repair persistent state or allocate incoming-edge caches. Graph adjacency
is maintained during edits. Schema 3 records the scoped model and typed
values, with schema-1/2 migration; this is not approval of a project-file
container, open/save workflow, or recovery policy (#32/#35).

## Typed parameter integration (#51)

`ParameterValue` stores boolean, signed 64-bit integer, floating-point scalar,
string, choice, 2D/3D vector, or RGBA color. Vector/color components retain
float storage; numeric representations must be finite and float-range
scalars must remain float-representable. The catalog owns parameter keys,
types, defaults, ranges, and choices. Known fields reject wrong types rather
than coercing them. Unknown fields remain recoverable typed data, subject to
the same representation validity rules.

Node and instance parameter maps, effective queries, CPU/GPU plan values,
and execution all use this representation. Evaluators do not parse authored
text. Type-tagged canonical values enter dependency identity; pixel math
and CPU/GPU reference independence are unchanged. Defaults are resolved
from the active catalog, including when queries return unauthored values.

Schema 3 and CLI transport encode each value as `{"type": "...", "value": ...}`.
Tags are `boolean`, `integer`, `float`, `string`, `choice`, `vector2`,
`vector3`, and `color`. For example, a color is
`{"type":"color","value":[0.25,0.5,0.75,1]}`. Schema-1/2 strings pass through
the catalog's explicit text parser; unknown fields remain strings.
Malformed or out-of-range values report their network/node/key context.
Existing text-entry controls and text-only command flags use this same
parser. QML does not convert integer text through JavaScript Number.

`setParametersCommand` accepts `ParameterEdit` records addressed by
`{network, node, key, instance}`; an absent value resets the authored field.
For an instance override, `network` is the referenced definition's scope.
Validation and publication run through the existing document command stack:
failed batches publish nothing, and successful batches/reset undo atomically.
CLI `set-parameters` takes an `edits` array of
`{network_id, node_id, key, value, instance_id?}`; `null` resets.

The owner-thread-only `ProjectSession` holds at most one parameter gesture.
Begin/update return a token and a shared immutable preview snapshot without
changing published values, revisions, events, or history. Updates merge by
parameter address; commit publishes once through the existing command stack.
Intervening edits make the gesture stale; clients cancel and begin again.
Cancel drops preview state without committing. No panel owns a history.
The CLI exposes `begin-parameter-gesture`, `update-parameter-gesture`,
`commit-parameter-gesture`, and `cancel-parameter-gesture`; begin/update use
`edits`, update/commit/cancel use `token`, and begin/commit require
`expected_revision`. Successful previews/cancellation report `ok: true`
and `committed: false`, not a document change.

## Verification

The workspace test executable stays offscreen by default, independent of a
desktop's exported `QT_QPA_PLATFORM` fallback list. Native input verification
uses `NEMO_TEST_NATIVE_UI=1 QT_QPA_PLATFORM=wayland` with
`nemo_workspace_ui_tests`; the issue #29 identity/gesture scenario exercises
IDs above JavaScript's exact integer range. Packaged-app frame-swapped and
screenshot evidence additionally verifies real Vulkan viewer presentation.

`tests/ReuseTests.cpp` (CPU, 12 scenarios: acceptance examples 1–6 of issue
#9) and `Effect.GpuReuseAvoidsRecomputationAndPreservesResults` (native GPU
path with Slang kernels) plus the full `ctest` matrix.

`ctest --preset debug -R Interactive` covers bounded priority, immutable
snapshots, destination isolation, cancellation with a real semaphore-gated
GPU submission, command undo, and graph/timeline QML input. Native Wayland
acceptance additionally exercises cached returns, edit-during-render, explicit
range backlog, resizing, zoom, and workspace switching; machine-specific
observations are recorded on issue #13.
