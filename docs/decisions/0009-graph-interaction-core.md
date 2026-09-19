# 0009 — Graph interaction core

Date: 2026-09-18
Status: Proposed — implemented on issue/100-graph-interaction-core; owner review of
prototype feel and visual parity is the outstanding gate this work exists to close.
References: issue #100; issues #25, #44, #46, #49, #84; spec §6 (node editor and
workspaces), §10.2 (module boundaries and ownership); ADR-0003 (the test layers the
count assertions below are made through)

## Decision

The graph panel's interaction layer is seven units, each with one responsibility,
in `apps/nemo-ui` (`namespace nemo::ui`). A unit's header is its contract.

- **Scene** — `GraphScene.hpp/.cpp`. The read-only network snapshot
  (`ViewerController::graphSnapshot()`) projected by `buildGraphScene` into
  immutable records (`GraphNodeRecord`, `GraphPortRecord`, `GraphEdgeRecord`,
  `GraphEndpointRecord`) with an identity index (`GraphScene::node`/`edge`) and a
  revision stamp (`GraphScene::revision`). It holds no Qt Quick types and no
  transient state, and nothing downstream mutates it: the paint item, the hit
  test and the gesture sessions all read the one parsed record set.
- **Geometry** — `GraphGeometry.hpp/.cpp`. The single owner of card rectangle
  (`cardRect`), port position (`portPosition`), the affordance chip and its hit
  region (`affordanceRect`, `withinAffordance`), the port side guard
  (`portSide`, `portGuardSatisfied`), the route polyline (`routePolyline`),
  point-to-segment projection (`projectOnPolyline`, `PolylineProjection`),
  content bounds (`contentBounds`) and `GraphViewTransform` (`toScreen`,
  `toScene`), which converts between scene and screen space.
  `portPosition` is the one place a port centre is computed; the painter and the
  picker both call it, and a second implementation of any of these is a defect.
  The layer's preserved constants are declared here and are not retuned while
  rebuilding: port hit radius 14 (`kPortHitRadius`), pipe-body tolerance 12
  (`kPipeHitTolerance`), reroute tolerance 9 (`kRerouteHitTolerance`), zoom clamp
  0.2–2.5 (`kZoomMinimum`/`kZoomMaximum`), wheel factor `exp(delta × 0.002)`
  (`kWheelZoomExponent`) with 53 px per notch (`kWheelPixelsPerNotch`), 3 px move
  threshold (`kMoveThreshold`), 200 ms zoom settle (`kZoomSettleMs`), one zoom
  application per event-loop turn, wheel ignored during a wire drag, port side
  guard 0.45 (`kPortGuardFraction`), snap tolerance 6 (`kSnapTolerance`) and card
  112×28 (`kCardWidth`/`kCardHeight`), together with the painted and hit
  affordance boxes, which the prototype kept as two slightly different boxes.
- **Hit test** — `GraphHitTest.hpp/.cpp`. `hitTestGraph(scene, view, screen)` is
  ONE ordered pass resolving every class the pointer can acquire
  (`GraphHitKind`: `Affordance`, `Port`, `Endpoint`, `Reroute`, `Card`,
  `PipeBody`) plus `GraphHover`, the published pointer feedback derived from the
  same result. Within a class the topmost element wins — a later candidate wins a
  tie, which is the paint order — and a port wins over its own card.
  `GraphHitResult::primary()` is the topology priority: port, connection
  endpoint, reroute dot, card, pipe body, and a subnet card's enter affordance
  wins over everything. `GraphHitResult` also publishes each direction's port
  winner (`portOutput`, `portInput`) beside the combined `port`: a consumer that
  needs one direction — a wire pull resolving its compatible destination — reads
  its own field, so a port it cannot use never shadows the one it can.
- **Gestures** — `GraphSessions.hpp/.cpp`. `GraphSession` is the explicit
  contract (`historyGesture`, `acceptsWheel`, `moved`, `preview`,
  `preferOutputEndpoint`, `update`, `cancel`, `commit`) with seven concrete
  sessions: `MoveSession`,
  `ToggleSession`, `MarqueeSession`, `ConnectSession` (connect, reconnect,
  disconnect and pipe pull), `RerouteSession`, `ScopeEnterSession` and
  `ViewSession`. A session's transient state is its `GraphPreview`; cancellation
  is dropping the session.
- **Command facade** — `GraphCommandFacade.hpp/.cpp`. The only object in the
  interaction layer that calls the existing graph command API (`moveNodes`,
  `connectOrReplace`, `rewire`, `disconnect`, `commitRoute`,
  `insertExistingNodeOnEdge`, `createNode`, `deleteNodes`, `assignViewer`,
  `collapseSelection`, `copySelection`, `pasteSelection`). Every UI commit path
  goes through it, so UI and automation keep sharing one command path.
- **Painter** — `GraphItem.hpp/.cpp`. The dense scene-graph item
  (`GraphItem::updatePaintNode`), reduced to consuming the scene, the geometry
  and the active session's transient `GraphPreview`. It computes no layout, holds
  no interaction state and does not re-parse a snapshot.
- **Panel** — `qml/GraphPanel.qml`. Input plumbing, chrome, popups and shortcuts.
  It computes no graph geometry, iterates no graph elements and resolves no pick;
  every coordinate it positions chrome with comes from a record the interaction
  published.

The panel-facing object is `GraphInteraction` (`GraphInteraction.hpp/.cpp`,
registered as the QML element `GraphInteraction`, `objectName: graphInteraction`
in the panel). It owns the scene, the view transform, the selection, the hover
result and the active session, and it is the only object the panel and the
painter talk to. Hover feedback is published from the same hit-test result a
press resolves its target with, and `GraphInteraction::hitTestPasses` counts the
passes, which is how the cost contract below is asserted.

The persistent document, graph, identities, catalog, commands and session
history, evaluation, media and GPU owners are unchanged. The interaction layer
consumes `ViewerController::graphSnapshot()`, `ViewerController::graphScope()`
and the existing graph command API; it authors no state of its own and adds no
persistent field.

The rejected alternative was a general-purpose third-party node-editor framework.
Such a framework is Qt Widgets/QGraphicsView based, so it cannot be embedded in
the Vulkan-backed Qt Quick window without leaving the Quick scene, and it would
introduce a second owner for graph state, styling and history. No new dependency
was introduced: the units above are built from the Qt Core and Qt Quick types the
`nemo-ui` target already links.

## Session contract

`GraphSession` is the whole gesture contract: `update`, `cancel` and `commit`,
plus `historyGesture`, `acceptsWheel`, `moved` and `preview`. A session begins
when `GraphInteraction` constructs it for a press — or, for `ViewSession`, for a
wheel burst that may arrive with no button down. `update` receives a
`GraphSessionContext` (command facade, current view transform, this event's own
ordered pass, screen point, whether the pointer is inside the surface). The
pass is the same result the interaction publishes as hover, so a session reads
the element it needs instead of resolving a second pick: a pointer move costs
one C++ pass whatever gesture is running, and feedback cannot disagree with the
pick. `commit` returns the `GraphCommitResult` the interaction acts on:
`settleView`, `writesSelection`/`selection`, `scopeEntry`. A command's refusal
is visible through the document, so the facade's edit methods return nothing and
the result carries no verdict for a caller to discard.

| Session | Begins on | `update` produces | `commit` produces |
| --- | --- | --- | --- |
| `MoveSession` | A left press on a card, or on one node of the current selection | Transient card positions in `GraphPreview::positions`, one entry per moved node written in place, with `applySnap` resolving the 6 px snap | One `moveNodes` covering every moved node, or `insertExistingNodeOnEdge` for a lone disconnected processing node released over a pipe; a drag that never left the 3 px threshold submits nothing |
| `ToggleSession` | Shift press on a card, which toggles the selection at the press | The move threshold only; no candidate | No command and no history; the release is the panel-state write boundary |
| `MarqueeSession` | Shift press on empty canvas, seeded with the current selection | `GraphPreview::marqueeScreen`, the candidate rectangle in viewport space, published from the press | The union of the base selection and the cards the box touches; a panel-state selection write only |
| `ConnectSession` | A `ConnectBegin` from a port press, a connection-end press, or a pipe-body press held over the move threshold | `wireVisible`/`wireFixed`/`wireFree`, the preview route between the press-time fixed end and the free end, with `wireHiddenEdge` hiding the pipe being pulled; `candidate()` resolves the compatible destination | `connectOrReplace`, `rewire` of an existing end, or `disconnect` when an existing end is released on empty canvas; a drop on a card or outside the surface submits nothing. `acceptsWheel()` is `false`, so the view cannot shift under a wire |
| `RerouteSession` | A press on a reroute dot, or Alt-click on a pipe body, which inserts a dot at the projection point | `routeEdge`/`routePoints`, the candidate polyline for that edge, re-published on every pointer event | `commitRoute` for a moved dot, an inserted dot, or an Alt removal click; any other release leaves the authored route alone |
| `ScopeEnterSession` | A press on a subnet card's enter affordance | Nothing; the release re-resolves its own hit | `scopeEntry` for a release still on the affordance the press acquired — one panel-state scope change and no history entry; released anywhere else it does nothing |
| `ViewSession` | `beginPan` for a middle drag or an empty-canvas drag, and a wheel burst that may begin with no button down | Pan applies `panOrigin + delta` on every move; zoom accumulates a clamped target applied at most once per event-loop turn (`turnTimer_`), anchored so the pixel under the pointer stays under it | One settled panel-state write (`settled`) once the 200 ms `settleTimer_` cashes in the burst; `settleView` is `false` while a burst is in flight, so the release cannot write twice |

Cancellation is dropping the session. `cancel()` clears the candidate, the
interaction discards the session, and nothing reaches the command facade, the
panel or history: Escape, a right press and the shared history adapter taking
Undo all take this path. A queued zoom burst is not a gesture and keeps running.
`settleView` is false only for `ScopeEnterSession`, whose navigation writes
through its own owner.

## Cost contract

The responsiveness contract is a budget of counts, not timings, and each count is
observable and asserted in a test:

- A node move costs at most one transient position write per moved node, zero
  model republications and zero scene re-parses. `GraphPreview::positions` is
  replaced in place, and the scene is rebuilt only by
  `GraphInteraction::setSnapshot`.
- An active gesture costs zero label rasterisations and zero texture uploads.
  The label atlas identity is the label set and the device pixel ratio, never a
  view scale or a position, so a drag moves quads instead of re-uploading a
  texture; `GraphItem::labelAtlasesRasterized` is the counter.
- Hover is exactly one hit-test pass per pointer move, in C++.
  `GraphInteraction::hitTestPasses` is the counter, and a press resolves its
  target from the same pass that produced the hover.
- A gesture end is exactly one command and one panel-state write, or zero for
  cancel, pan, marquee, zoom and scope entry.
- The port the painter draws is the port the pick acquires at every zoom: both
  call `portPosition`, and every tolerance is screen-space and converted through
  the same `GraphViewTransform`.
- A zoom burst applies at most one transform per event-loop turn and writes once
  when it settles.

Wall-clock measurement of frame time, scrub latency and panel responsiveness
stays with its existing performance owner; this record fixes only the counts.

## Command, history and persistence

- One gesture produces one command and one undo entry, or nothing.
  `MoveSession`, `ConnectSession` and `RerouteSession` are the sessions whose
  release can author a change; each submits through `GraphCommandFacade` at most
  once, and a refused command returns `false` with the topology left untouched.
- Cancel produces nothing. A cancelled session never reaches the command facade
  and never produces a history entry.
- Marquee, pan, zoom and scope entry produce no history entry. They write panel
  state only (`GraphCommitResult::writesSelection`, `settleView`), and
  `ViewSession::settled` is the one write boundary of a settled burst.
- Sessions address nodes and ports by identity. Node and edge identities are the
  snapshot's string ids; a port is addressed by its declared index, which is the
  identity `portPosition` takes, the pick reports and the snapshot publishes as
  `index`.
- Sessions retain the scene they began from. A snapshot carrying a different
  revision aborts a live session and never rebases one
  (`GraphInteraction::setSnapshot`).
- There is no document-format change and no migration. Authored node positions
  and routes stay in the document; zoom, pan, scope and selection stay panel
  state keyed by network and panel, written once per settled gesture (#84).

## Maintenance rules for graph code

These ten rules are binding on all future graph work, in this panel and in any
graph surface built after it:

1. One owner per fact. Geometry exists once and is consumed by both drawing and
   picking.
2. QML never computes graph geometry, iterates graph elements, or resolves picks.
3. Transient state lives beside the painter, never in the model; one write on
   commit.
4. Never rebuild on pointer motion. Records are indexed, updated in place, and
   split into dirty domains (elements, routes, labels) so that motion cannot
   re-rasterise text.
5. Gestures are explicit sessions. No string-typed gesture state, no loose flag
   properties.
6. Hit testing is one ordered pass in C++ with screen-space tolerance.
7. A new behavior is a new session type plus a paint layer, or one new
   hit-priority entry — never another branch in a shared handler.
8. Every performance claim is a count, asserted in a test.
9. Feedback derives from the same queries gestures use; it does not recompute
   them.
10. Shared interaction primitives — view transform, drag session, marquee,
    snapping and guides, cursor arbitration — are introduced at their second real
    consumer, in the owning ticket for that consumer, and never speculatively.

## Boundaries

The seven units are presentation-only. They read the network snapshot and submit
through the existing command path; the persistent document, graph, identities,
catalog, commands and session history, evaluation, media and GPU owners are
unchanged, and no transient interaction state reaches the model. Owner review of
prototype feel and visual parity is the gate this work exists to close; it is the
only gate this record claims.

Two feedback directions remain, and they are separate:

- **Conformance** — prototype parity for the behavior the interaction core
  ported. The #44 machine reference index and the derived
  [`conformance-ledger.json`](../evidence/assets/issue100-graph-interaction-core/conformance-ledger.json)
  are the machine record of which rows belong to which unit, and the #25
  prototype inventory remains the visual and interaction authority.
- **New design** — pointer cursors, node hover states, alignment guides,
  insertion preview, keyboard navigation, in-place rename and dense-label
  visibility. Each requires narrow owner approval for the affected slice and
  lands in its own commits with its own evidence.

Conformance and new design never share a commit.
