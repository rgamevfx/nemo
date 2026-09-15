# Issue #84 — graph and viewer pan/zoom fluidity, verification evidence

Recorded on Linux/wayland, Qt 6.4.2, DPR 1, window 1274x687, theme Graphite,
unless a row says otherwise. The graph records state on its Null-RHI harness and
pixels (OpenGL) on the opt-in native one; the viewer records pixels on Vulkan.

Scope: the two slices of [#84](https://github.com/rgamevfx/nemo/issues/84) — the
graph pan/zoom port (slice 1) and the viewer pan/zoom interaction (slice 2).
Assets: `docs/evidence/assets/issue84-graph-viewer-view/`.

## Commands

```bash
# Debug build and the UI suites (the non-native suites run in one process; the
# native viewers need a real window and the Vulkan scene graph)
cmake --build --preset debug
build/debug/tests/nemo_workspace_ui_tests                       # 158 ran / 142 passed / 16 native-gated
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 \
  build/debug/tests/nemo_workspace_ui_tests --gtest_filter='ReadViewerSurface.*:PreparedConfigSurface.*'   # 13/13
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 \
  build/debug/tests/nemo_workspace_ui_tests --gtest_filter='ViewerDestinationSurface.*'                   # 2/2
ctest --preset debug                                           # 688 / 689 (see Limitations)
cmake --preset release && cmake --build --preset release       # clean
# Evidence regeneration
NEMO_TEST_NATIVE_UI=1 NEMO84_GRAPH_CAPTURE=1 \
NEMO84_EVIDENCE_DIR=$PWD/docs/evidence/assets/issue84-graph-viewer-view \
  build/debug/tests/nemo_workspace_ui_tests \
  --gtest_filter='WorkspaceDragTest.GraphWheelBurst*:WorkspaceDragTest.GraphViewGesture*:WorkspaceDragTest.GraphScreenSpace*:WorkspaceDragTest.GraphViewInMotion*'
# Focused sanitizer subset (ownership/lifetime of the changed paths)
NEMO_TEST_NATIVE_UI=1 ASAN_OPTIONS=detect_leaks=0 \
  build/asan/tests/nemo_workspace_ui_tests --gtest_filter='WorkspaceDragTest.Graph*'   # 8/8 clean
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 ASAN_OPTIONS=detect_leaks=0 \
  build/asan/tests/nemo_workspace_ui_tests --gtest_filter='ReadViewerSurface.Viewer*:ReadViewerSurface.Retired*:ViewerDestinationSurface.ViewerViewsStayIndependentPerPanel'   # 6/6 clean
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 \
NEMO74_EVIDENCE_DIR=$PWD/docs/evidence/assets/issue84-graph-viewer-view \
  build/debug/tests/nemo_workspace_ui_tests \
  --gtest_filter='ReadViewerSurface.Viewer*:ReadViewerSurface.Retired*'
```

Sanitizer presets: a focused ASan/UBSan subset of the changed paths is clean
(above). That subset is what found and fixed a dangling test connection: a
state-write spy whose context object outlived the test body was being called
from panel teardown once the close hook below existed.

A view in motion is persisted when the application closes:
`WorkspaceDragTest.GraphViewInMotionIsPersistedWhenTheWindowCloses` applies a
burst, closes the window inside the settle window, and asserts the project
records the applied view.

## Write frequency per gesture (the bug class)

| Gesture | Wheel events | Panel-state writes | Source |
| --- | --- | --- | --- |
| Graph wheel burst, three notches one turn apart then five in one turn | 8 | 0 mid-burst, 1 when it settles | `graph-zoom-burst.json` (`applied_zoom_per_turn`: three states; `persisted_writes` is 4 for the record's bursts plus its frame-all) |
| Graph middle-drag pan, 8 moves | — | 0 mid-drag, 1 on release | `WorkspaceDragTest.GraphViewGestureWritesOnceAndOtherPanelsDoNotRewriteIt` |
| Viewer wheel burst, 6 notches | 6 | 1 when it settles | `WorkspaceDragTest`-style spy in `ReadViewerSurface.ViewerWheelZoomIsContinuousAnchoredAndReversible` |
| Viewer left/middle pan drag | — | 1 per released drag | `ReadViewerSurface.ViewerDragPanKeepsTheZoomAndTheDocument` |
| Viewer pan, other panel watching | — | graph panel: 0 writes, view unchanged | `graph-independent-from-viewer-write.json` |

The gesture reaches the view once per event-loop turn:
`applied_zoom_per_turn` in `graph-zoom-burst.json` holds three strictly
increasing states for three notches delivered one turn apart, while five notches
delivered inside one turn land on exactly one state — `fitted × exp(53 × 0.002 ×
8)` for the sampled three plus the five. The anchor holds exactly:
`anchor_scene` and `anchored_scene` are both `(380, 288)`.

## Viewer view → request contract

Recorded in the `environment-viewer-*.json` files beside each capture
(platform, Qt, DPR, window size, appearance preset, media-surface size, stated
zoom, displayed scale, controller zoom, pan and the request the frame carried).

| Case | Stated zoom | Drawn scale | Request region | Sampling | Capture |
| --- | --- | --- | --- | --- | --- |
| Fitted (Fit preset) | `Fit` | 807.8% (96×64 at 1256×529) | whole image 96×64 | 1 (Full) | `viewer-preset-fit.png` |
| One wheel notch in | 898.1% | 8.98 px/image px | 96×60 (visible) | 1 (Full) | `viewer-zoom-one-notch.png` |
| Burst settled | 1525.9% | 15.26 | 83×35 | 1 (Full) | `viewer-zoom-burst-settled.png` |
| Panned while zoomed | 1886.2% | 18.86 | 68×29, region.x 14→13 after the drag | 1 (Full) | `viewer-pan-region.png`, `viewer-panned.png` |
| Zoomed far out | 41.3% | 0.41 | whole image 96×64 | 2 (Half) | `viewer-zoom-out-sampling.png` |
| Retired continuous record | 646.3% | 6.46 (0.8 × fitted) | whole image | 1 | `retired-custom-zoom-stated.png` |
| Unreadable record | `Fit` | 807.8% | whole image | 1 | `retired-custom-zoom-recovered.png` |

The zoomed-out sampling reduction is the existing Auto policy: at `zoom = 0.05`
the image pixels per panel pixel are `1/0.05² ≈ 400`... measured through the
policy it resolves `reduction = 5.85` for this media, so **Half** is the correct
level rather than Quarter. An explicit `Full` selection overrides it at the same
view (asserted in the test).

## Graph conformance re-exercise (slice 1)

`graph-zoomed-in-hit-testing.json` / `.png` and
`graph-zoomed-out-hit-testing.json` / `.png` record the same gestures on the real
surface at 210% and 59%: the port glyph is acquired at both zoom levels (port 0
of the node under test), 20 px away nothing is acquired, a reroute dot inserted
on the pipe body stays hittable (`reroute_dot_hit_index` 0 at both levels), and a
60×40 screen drag maps to `delta / zoom` scene units (28.57 at 210%, 101.94 at
59%). Left/middle-drag panning, screen-space port acquisition and protection,
drag mapping, the wire drop, last-click placement in scene coordinates and
frame-all after the whole sequence are exercised at both zoom levels in
`WorkspaceDragTest.GraphScreenSpaceHitTestingSurvivesEveryZoomLevel` and
`GraphWheelBurstAppliesPerTurnAndPersistsOnceWhenItSettles`.
Prototype conformance and production correctness are separate findings: the
zoom maths, clamp, anchoring, pan gestures and screen-space hit testing are the
accepted prototype coverage (unchanged); the coalescing, the write frequency,
the cheaper grid and atlas, and the panel-state boundary are this change's
production corrections. The viewer's wheel behaviour is an owner-approved
deviation from the archived prototype (which had no wheel zoom or drag pan) and
is not presented as conformance.

## Two independent surfaces

`ViewerDestinationSurface.ViewerViewsStayIndependentPerPanel` (issue #47's
native two-panel harness, the successor of the archived
`media-bin-session.json:/verification/checks[53]` check) wheels over panel A and
asserts panel B's request region, displayed rect and panel-state record are
untouched — and that only A wrote a view record.

## Limitations and unverified behaviour

- **A view in motion when a panel is destroyed by a layout change is not
  written.** Panels persist on gesture settle, on a scope/viewer switch, on a
  drag release and when the application closes. A panel destroyed *by the
  layout* inside the settle window (for example a panel type change) keeps its
  last settled view: persisting during teardown would write into a record that
  no longer belongs to that panel (a project open reuses panel ids). The
  application-close case is covered, which is the case an artist actually hits.
- **Pan has no travel while the whole image is visible.** The drag is accepted
  at any scale and never discards the zoom, but the view centre is clamped so
  the image covers the panel — the same clamp the controller applies to the
  requested region, so the drawn image and the request cannot disagree. At Fit
  the whole image is visible, so there is nothing to pan onto.
- **The graph's wheel stays ignored during a wire drag only**, as the archived
  prototype guard does; every other graph gesture is untouched.
- **Left-drag pan on the viewer image is permanent in this change.** The
  specification lists this as an unresolved decision for narrow owner approval;
  the chosen behaviour is the specified one (left button as today, middle button
  as the graph allows).
- **The continuous range and step are the controller's own.** The range is
  0.05×…32× of the fitted image (the controller's own representable bound
  expressed through the documented conversion) and the step is
  `exp(pixelDelta.y × 0.002)` with the graph's 53 px-per-notch angle
  normalisation. Both remain open decisions in the specification.
- **Inter-suite interference in one process (pre-existing).** With
  `NEMO_TEST_NATIVE_UI=1` several window-based suites in a single process disturb
  each other's input (docking drags stop receiving the pointer) and the binary
  segfaults in `PanelContextUiTest` after the native suites; the pristine tree
  (changes stashed) behaves identically, so the suites are run per suite. The
  default `ctest --preset debug` run, where the native suites skip, is green
  apart from the pre-existing `Viewer.InterpretationMapIsParsedStrictly` failure
  (a generated clip's `colorimetry` side data; no file or target this change
  touches).
- **Not measured:** a benchmark of applied zoom steps per second on a dense
  network. The specification's budget is the frequency contract plus native
  interaction evidence, which is what is recorded here; the #16 gate remains the
  owner of measured performance.
