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
   local time, full image domain, sampling scale, channels, quality,
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

7. **Content identity and spatial residency are separate (#85).**
   `nodeContentKey` propagates input content hashes independently of their
   backing rectangles. `regionResultKey` identifies a concrete covered
   representation. `ResultCache::findRegion` selects the smallest resident
   rectangle covering the demand on the same image-anchored sampling lattice.
   A downstream consumer reads that backing in place; changing coverage alone
   does not invalidate scene-linear content.

   The shared dependency planner rounds coverage outward to 64-raster-sample
   blocks, clipped to the domain. This amortizes small overlapping pans without
   fragmenting every node into many GPU dispatches. It is rectangular coverage
   reuse, not a tiled cache: a request extending beyond every resident rectangle
   is recomputed, even if some pixels overlap. No per-tile scheduler or stitching
   is introduced. Final delivery is exactly the normalized requested rectangle;
   GPU cropping stays in the evaluation's single submission. Freshness and the
   existing entry/allocation budgets remain unchanged.

   Plan steps report actual coverage; the GPU output step reports the cropped
   delivered raster when a final device copy is required. CPU plan result and
   normalized request identify delivery separately from backing steps.

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
shared playhead. That strip mapping is the *shared* `SourceReference`'s; a Read's
own mapping and policies are node parameters resolved by `resolveSourceRequest`
and are not edited from the timeline (see "Read source ownership…(#79)").
They do not claim clip-occurrence move/trim support: that
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
The storage choice above (independently copied document states) was later
superseded by structurally shared versions; see "Structurally shared document
versions (#72)".
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
is maintained during edits. Schema 4 records the scoped model, typed
values, and animation channels, with schema-1/2/3 migration; this is not
approval of a project-file container, open/save workflow, or recovery policy (#32/#35).

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

Schema 3 and later, and CLI transport, encode each value as `{"type": "...", "value": ...}`.
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

Value edits are routed by state captured at begin (issue #76).
`beginValueParameterGesture` records, per edited address, whether that address
authors the current-frame key on an existing animation channel — through the
existing keyframe factory, so interpolation, tangents and key identities are
preserved — or takes a static value and creates no channel. The routing cannot
change while the gesture lives, and commit is ONE command and one history entry
applying both parts atomically, so one inspector gesture may mix an animated
choice with its static companion. The UI controller's single-key calls
(`beginNodeParameterEdit`) and the Read editor's `commitValues` are the
one-element and batch shapes of this same owner; the CLI still exposes only the
static batch.

## Parameter animation integration (#48)

`Document` owns animation channels addressed by `{network, node, key, instance}`.
Each channel and key has a stable document-wide identity; allocator watermarks
survive deletion, undo, and serialization. Channels store typed key values,
finite subframe times, outgoing Hold/Linear/Bezier interpolation, Smooth/Broken
tangent mode, and unweighted incoming/outgoing slopes in actual value units per
frame. Scalar/vector/color channels interpolate continuously; boolean, integer,
choice, and string channels require Hold. Endpoint extrapolation is constant.
Smooth tangents require equal incoming/outgoing slopes; unused components are zero.

`setKeyframesCommand` edits all supplied key fields atomically. Zero IDs upsert
by the original channel/time; nonzero IDs must identify existing keys.
Conflicting targets, final-time collisions, invalid types, and tangent violations
reject the whole batch without history. Group moves and exact time swaps validate
their final state, not intermediate positions. `insertKeyframeCommand` samples the
curve and its derivative to preserve unweighted Bezier segments; insertion outside
the key range preserves constant extrapolation. Removing the last key removes its
channel. Static parameter reset does not implicitly remove animation.

Keyed parameter gestures share the existing session token, immutable preview,
revision checks, commit/cancel lifecycle, and document history. Their frame is
captured at begin; updating an existing key preserves its interpolation/tangents.
Adding a key allocates the same identities in the preview and eventual commit.
No preview changes the published document or triggers a current render. Current
time, selection, curve visibility, and panel framing remain presentation state.
Channel/key queries are bounded and paginated by stable identity rather than
mutable time order. Change records identify affected channels/keys and scoped
nodes/networks/instances so clients can discard deleted references; undo restores
the original identities.

Effective values resolve in order: definition static/default, definition animation,
instance static override, instance animation. CPU execution, GPU execution, and
viewer-key queries share only this metadata resolution, never pixel kernels.
Only nodes with effective overrides/animation need a local node copy. Content
keys use frame-resolved values, so key edits invalidate affected dependencies,
not every cached branch. Immutable snapshot and asynchronous publication rules
remain unchanged.

Schema 4 adds `animationChannels`, `nextAnimationChannelId`, and `nextKeyframeId`.
Keys retain tagged parameter values and explicit interpolation/tangent metadata;
schemas 1–3 migrate without invented animation. Restoration validates the entire
set before installation. Animation targeting an unavailable catalog node/parameter
currently fails load explicitly rather than being silently dropped or rendered
with invented semantics; historical unknown **nonanimated** fields remain retained.
This is an explicit limitation pending unavailable-extension recovery, not a
selection of the final project container. Public API review remains required.

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

## Structurally shared document versions (#72)

Date: 2026-09-13. Status: Accepted. Revises the storage choice of the
"Revisioned editing integration (#29)" section above; the decisions it
recorded about atomic publication, stable identities, monotonic watermarks,
owner-thread access, request identity and content-derived reuse keys all
stand unchanged.

## Context

#29 kept history as deep document copies: every submit copied the document,
undo/redo copied it again, gesture previews and prepared saves copied it
again, and publication discovered what changed by comparing the whole
before/after document. #69 measured the result: a single-parameter edit
costs p50 25 µs on an 82-node project and 1 479 µs on a 1 026-node
parameter-heavy one, a retained history entry costs ≈ 49.2 kB, and each
commit at capacity moves 255 retained documents to evict one. The cost
scales with the project, not with the edit.

## Decision

A document version is a set of handles to structurally shared storage
(`CowVector`/`CowMap`, `src/nemo/core/SharedContainers.hpp`). Copying a
document copies handles; a controlled mutation copies the index and only the
bounded records/chunks it changed. `CowVector` is a chunked sequence whose
chunks and chunk index are shared independently, so a parameter or layout
edit in one large network does not duplicate the network, and the adjacency
index is a NodeId-sorted sequence rather than a copied map.

Retained state is now a lightweight version handle plus the information
needed for correct transition notifications:

1. **Change recording, not a whole-document diff.** Controlled mutations
   record the identities they touched (`ChangeRecorder`). Publication
   compares the stored values of the touched identities between the two
   versions, so a recorded-but-unchanged write reports nothing and untouched
   records are never examined. Changed/created results stay directional.
2. **History holds version handles.** `CommandStack` stores the retained
   version and its transition's touched set in a preallocated bounded ring;
   capacity is unchanged (default 256) and eviction overwrites the oldest
   slot instead of moving the remaining entries. Undo/redo replay the same
   touched set in reverse and compare values, so there is still no second
   collection of mutable inverse callbacks.
3. **Gestures and saves retain versions.** A parameter gesture's preview is a
   shared candidate version, not a copy; commit stays one atomic history
   entry and cancel releases transient ownership. A prepared save retains a
   stable document version and the envelope fields; it no longer
   synchronously serializes the project merely to prepare a write.
4. **Saved-state comparison is structural.** `isDirty()` compares the current
   version against the version captured at open/save, treating storage that
   still shares chunks as identical. Content equality is still exact
   (including preserved opaque data, presentation and the color-config
   path); a revision/history-position check or an unverified hash alone is
   not used.
5. **Reference reconciliation is scoped.** `synchronizeReferences` runs over
   the networks, instances and channels the transaction touched; the
   complete pass remains for schema restoration and document replacement.
6. **No mutable escape hatch.** Catalog access installs the transaction
   recorder, the model exposes const-only lookups to consumers, and there is
   no legacy deep-copy edit path.

Worker isolation is unchanged and, if anything, cheaper: workers receive
immutable shared document versions, never the live session or a mutable
builder, and content-derived cache identities (ADR-0004) are untouched. A
document revision is still not an evaluation reuse key.

## Consequences

Ordinary edits, snapshots, gesture previews and history growth scale with the
changed records and bounded chunks rather than the project. Retained data is
released when the last version referencing a chunk is dropped, so eviction
does not need to destroy anything eagerly; reclaiming a genuinely deleted
payload remains real work. Non-trivial per-edit costs that remain are the
bounded chunk-index copy, dependency validation over the touched
relationships, and genuinely broad edits (topology or shared-definition
changes), which are reported rather than hidden.

## Read source ownership, effective requests, and schema 5 (#79)

Date: 2026-09-14. Status: Accepted. Scope: issue #75 (Read slice), delivered in
#79. Narrows the source-scoped reuse identity introduced by "Content-derived
keys" above; the freshness, publication and structurally shared version
decisions all stand unchanged.

## Context

Before schema 5 a Read's usable range, offset/step mapping and media
interpretation lived on the *shared* `SourceReference`, so two Reads of one file
could not disagree and an edit on one node silently retimed another consumer of
that source. Reuse identity was likewise derived from that shared record. Issue
#75 separates the shared media identity (path, content revision, committed
facts) from the per-Read choices (range mode and endpoints, mapping, before/
after/missing policy, explicit input transform, alpha association, migrated
interpretation hints), and requires migration of saved documents without
changing their pixels or times.

## Decision

1. **One resolver, no second owner.** `resolveSourceRequest`
   (`src/nemo/core/evaluation/SourceRequest.hpp/.cpp`) is the single place that
   combines a Read's effective node parameters with the shared reference and the
   committed facts. The CPU provider (`ImageSourceProvider::frame`), the GPU
   source session (`eval::SourceSession::acquire`) and the Read inspector
   (`ReadSourceController`) consume the resolved request, and source-node result
   keys are derived from it (`Reuse.cpp`); none of them re-derives mapping,
   coverage, policy precedence or interpretation. The media import worker is
   narrower rather than an exception: it receives the resolver's merged
   `InputColorChoice` plus the frozen source-local frame and maps that through
   the shared reference, so it re-derives no precedence either. Two resolver
   entry points exist: the node-scoped one used by Reads and by result keys, and
   a source-scoped one used by `eval::SourceSession::probe` (decode-path
   evidence) and available to any consumer that holds only a reference, which
   keeps the shared reference's authored mapping exactly. The request is plain,
   media-free data: it carries authored color *choices*, never an OCIO object or
   a config handle. The fill-only hint merge is itself one core owner,
   `applyReadInterpretationHints` (node scope first, else the shared
   reference, returning the node-origin bit mask); it accepts an empty map, so
   the same owner produces a not-yet-bound Read's probe hints and a bound
   request's merged interpretation, and admissibility is core's
   `readOverridesProblem` — the authoring commands and the resolver share it.

2. **Resolution is exclusive, never compositional.** A Read's node mapping
   *replaces* the shared mapping, so a legacy offset/step is applied exactly
   once after migration, and a migrated document resolves the same source frame
   as before. An overflow in the mapping is always an error before any policy is
   considered; a policy decision is reported (`status`, `policyError`,
   `transparentBlack`, `readFrame`) rather than thrown by the resolver, so a
   facts query and an evaluation see the same outcome. The Start At editor is
   the checked, step-magnitude-aware alias of the same mapping: `startAtOffset`
   anchors the selected first frame for a forward step and the selected last for
   a reverse one and throws when no offset is representable, while a fractional
   or otherwise unrepresentable alignment is reported as no value
   (`EffectiveSourceMapping::startAt()` returns `std::nullopt`), never as zero.
   The pre-binding lifecycle commands (`register`/`relink`/`reload`) carry the
   probe's classified `MediaKind` explicitly, so a movie keeps its container
   kind and interval rather than having one inferred from the path.

3. **Coverage facts are shared, policies are per Read.** Discovered facts extend
   the existing committed probe (original inclusive range, coverage quality,
   available/missing counts, compact hole runs, pixel aspect, rational rate,
   declared precision/channels/input color space) and stay in the media catalog,
   which already owns `stateRevision()` freshness. An Auto range follows those
   facts; a Custom range keeps its authored endpoints across reload and
   replacement. Boundary policies are enforced only against an authoritative
   interval (authored, or discovered and Validated): an Estimated or Unknown
   interval is reported truthfully and never fabricates a boundary failure, and
   a Hold whose boundary frame is itself a hole stays a missing-frame condition.

4. **Schema 5 migration.** Loading a schema-4 document materializes the shared
   timing and recognized interpretation keys onto each Read (every network,
   including nested definitions, and each instance occurrence that repoints a
   Read at another source key), writing through the deserialization path so no
   controlled-edit transition is recorded. The shared reference is left intact:
   source-scoped interpretation remains intentional policy for other consumers,
   and a migrated node hint is fill-only, so reliably tagged media still wins.
   Only keys that carry information are written, migration is applied once, and
   the loader reports it as a warning.

5. **Effective identity, config content included.** A source node's result key
   is the resolved request's canonical form (path, content revision, effective
   mapping, enforced interval, coverage, policies, mapped/read frames, transparent
   -black decision, interpretation hints, authored color choices, alpha) plus the
   opaque color-config content identity supplied by the executor
   (`KeyContext::colorConfigIdentity`, from the provider/session that owns the
   config). Node identity and the shared source key are **excluded**, so two
   Reads with equivalent effective requests share results through different node
   identities, while a differing mapping, policy, interpretation or config
   content can never alias. The config identity enters at the source seam, so
   dependents inherit it through their inputs' keys; provenance bits (which scope
   authored a hint) are deliberately excluded because they do not change pixels.

6. **Registered configuration references.** A color-configuration reference is
   either a filesystem path or one URI registered in the build
   (`kBuiltinColorConfigUri`, the pinned owner-approved ACES Studio config).
   Registered references are persisted verbatim, never rebased, and reported
   present by construction; any other URI-looking string stays an ordinary path
   and is reported honestly rather than assumed present. New-project defaults
   come from the existing project/session owner and never override an explicit
   `$OCIO`; legacy `ColorPolicy` defaults are unchanged, so existing documents
   keep their authored working space and viewing transform.

## Consequences

- Two Reads of one file share one media reference, one Media Bin entry and one
  set of discovered facts, yet keep independent timing, policies and
  interpretation; retiming one cannot silently retime another.
- Reload is the only shared refresh: it advances the content revision once and
  replaces the committed facts, so affected results invalidate through the
  existing content-keyed path and node-authored choices survive.
- A migrated document is indistinguishable from one authored under the new
  model: there is no legacy/new Read mode, no second per-node interpretation
  record, and no hidden per-Read copy of shared facts.
- Changing the OCIO configuration (or resolving a different input transform)
  changes the source identity, so a stale transform can never be served from a
  path-only identity; non-source branches keep their reuse.
- A stream rejected for missing interpretation is recoverable without changing
  the shared reference: the missing decode fields are authored as the Read's own
  hints (merged by `applyReadInterpretationHints`, which also serves an unbound
  Read's probe) and the artist explicitly retries the same selection — there is
  no automatic retry — while the shared reference keeps the path only and every
  other consumer's policy is untouched.

## Verification

`tests/ReadSourceTests.cpp` (command authoring, node independence, coverage and
hole policy, Start At/reverse/overflow arithmetic, identity equivalence,
schema-4 migration, hint precedence, save/load), `tests/PersistenceTests.cpp`
(discovered facts round trip and malformed-fact rejection),
`tests/SessionPersistenceTests.cpp` (ProjectSession independence, one undo entry,
save/reopen/Save As, registered reference persistence),
`tests/ViewerModelTests.cpp` (provider seam and node-over-shared replacement),
plus the media and viewer cases owned by #80/#81 for decoded pixels and the
native path. `tests/ReadSourceUiTests.cpp` covers the presentation adapter:
movie binding keeps its validated interval, an untagged movie is recovered only
by authoring the missing interpretation and explicitly retrying the same
selection, per-occurrence error/pending state, and Start At deriving the offset
through core rather than UI arithmetic.
