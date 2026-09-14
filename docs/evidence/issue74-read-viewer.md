# Read-to-Viewer repair (issue #74)

**Branch:** `issue/74-read-viewer-repair`, landed on `main` by owner direction
(it carries the #61 implementation `b78f9cc`; #61's owner review stays open)
**Work class:** system-extension repair of the Read-to-Viewer integration, plus
the prototype-conformance correction that the reported blank surface required.
**Native capture:** [`assets/issue74-read-viewer/`](assets/issue74-read-viewer/)

## Diagnosis

The reporter's symptoms were reproduced against the owner's saved document,
workspace and media on the real Qt/Vulkan window
(`ReadViewerSurface.UserDocumentDisplaysItsAttachedRead`). Two independent
defects produced them.

### 1. The graph viewer resolved the wrong media reference (verified metadata gap)

`ViewerController::refreshRequest` probed the hard-coded CLI fixture key `src`
for the graph role. An artist-selected Read names an arbitrary document source
key (for example `ltxsamplevideo` or `figure_006`), so the probe never matched
the attached target:

- `sourceSize_`/`pixelAspect_` stayed empty, `frameCount_` stayed `-1`.
- The request ran against the 1920x1080 default composition canvas instead of
  the media's own raster.
- The transport kept the 240-frame fallback; the video's detected 650 frames at
  25 fps never reached the panel.
- The status text lost its media description, so the failure read as an
  application problem rather than a routing problem.

Pre-fix native observation (deterministic 96x64 still selected in Read):

```text
PngSelectedInReadAppearsInItsAttachedViewer:
  frame.width  = 960   (expected 96)
  frame.height = 540   (expected 64)
  sourceSize   = empty (expected 96x64)
  frameCount   = -1    (expected 1)
  status       = "Displayed 960x540, 1:2, frame 0; live render; "   (no media description)
```

Pre-fix native observation (owner's document, `read-still.nemo.autosave2`):
`raster=960x540 full=1920x1080 source=-1x-1 frames=-1` — the retained transport
fallback the issue records.

### 2. A retired continuous zoom state made a displayed image read as blank

The viewer panel's wheel handler wrote `{zoomMode: "Custom", zoom: <0.05..32>}`,
a mode the zoom control (`["Fit", "100%", "50%"]`) cannot represent:
`currentIndex: Math.max(0, model.indexOf(zoomMode))` clamps to `Fit`. The
owner's saved workspace and project both persisted that state
(`viewerRole: "graph"`, `zoomMode: "Custom"`, `zoom: 0.184` / `0.105`), so the
image was drawn at 18% of the fit size — a roughly 110 px thumb in a ~620 px
panel — while the control reported **Fit**. The accepted prototype has no such
state: its wheel handler selects a control mode
(`setZoom(angleDelta.y > 0 ? "100%" : "50%")`), so this is also a
prototype-conformance deviation introduced by the #59 cutover.

Combined with defect 1 the symptom is exactly the report: a connected Read, a
named Viewer, a 0..239 ruler, and an apparently empty image area with no
visible error (the panel rendered no controller status or error at all).

## Changes

### `apps/nemo-ui/ViewerController.cpp` / `.hpp`

- `resolveTargetMedia(document, network, target)` resolves the media reference a
  render target actually displays: a `source` node names its own reference; a
  downstream target consumes the single media source its dependency closure
  reaches through the existing `expandDependencies` owner. Several sources (or
  none) keep the established 1920x1080 default canvas — no arbitrary pick and no
  new multi-source composition-format policy. Legacy CLI-created projects
  (`source` -> `merge` -> viewer) keep their metadata because their closure
  reaches exactly one source.
- The graph role now probes that reference, so the probed dimensions, pixel
  aspect, duration and rate drive the request raster and the transport. The
  authored `firstFrame`/`lastFrame` sequence bounds stay authored; only the
  controller's detected transport metadata changes.
- The probed identity now includes the source key, so a retargeted Read
  re-probes even when two keys name the same path and revision.
- An attached `source` node that names nothing is an explicit empty viewer
  (`renderState() == "empty"`, status names the node); the previous frame is
  dropped instead of lingering as current output.
- A probe result whose reference disappeared re-resolves instead of leaving the
  panel pending forever.
- `hasPresentation` is a new read-only Q_PROPERTY so the panel can distinguish a
  stale frame from an empty area; nothing else about the public surface changes.

### `apps/nemo-ui/qml/ViewerPanel.qml`

- The panel now renders the controller's status/error: a centered placeholder
  when no frame is shown (unavailable target, unbound Read, failure) and a
  bottom status line while a retained frame is stale/pending/failed. Probe,
  offline, format and render/color failures are therefore visible on the actual
  Viewer instead of a silent blank area.
- Zoom is mode-driven as in the prototype: `Fit` recomputes against the panel,
  `100%`/`50%` are absolute, an unknown persisted mode resolves to `Fit`, and
  the wheel selects `100%`/`50%` instead of a private continuous scale. A
  project saved with the retired `Custom` state reopens at Fit; the selector and
  the image agree.

## Verification

Local debug and release presets; no hosted CI required (not the pipeline).

| Check | Result |
| --- | --- |
| debug `nemo_workspace_ui_tests` (full, offscreen) | 104 pass, 10 native-only skipped |
| debug native `ReadViewerSurface.*` | 8 pass |
| debug native `ViewerDestinationSurface.*` (#47 prior art) | 1 pass |
| release `nemo_workspace_ui_tests` (full, offscreen) | 104 pass, 10 skipped |
| release native `ReadViewerSurface.*` | 8 pass |
| debug `nemo_core_tests` / `nemo_media_tests` / `nemo_viewer_tests` | 303 / 20 / 39 pass |
| debug `nemo_workspace_tests` / `nemo_color_tests` / `nemo_effect_tests` | 26 / 11 / 12 pass |
| debug/release `nemo-ui`, `nemo-workspace_ui_tests` build | clean |

Both deterministic regressions were confirmed to fail on the pre-fix tree: the
PNG case reported `960x540`, empty `sourceSize` and `frameCount -1`, and the
retired-zoom case measured image coverage `0.0014` of the media surface against
the `0.15` threshold.

New tests (`tests/Issue74ReadViewerUiTests.cpp`, native Qt/Vulkan window):

- `PngSelectedInReadAppearsInItsAttachedViewer` — a deterministic 96x64 PNG
  selected through the real Read control presents at 96x64 with `frameCount 1`
  and displays the still's display color, asserted against an independent CPU
  oracle (`readImageFrame` + `applyViewingTransformCpu` under the same OCIO
  policy), not against the viewer's own output.
- `GraphViewerUsesTheAttachedTargetsReferenceNotTheLegacySrcKey` — a document
  holding both `src` (2x2) and the Read's `plate` (96x64) renders the attached
  target's media. This is the regression for the verified gap.
- `ReplacingTheReadPathUpdatesTheViewerAndUndoRestoresIt` — retargeting a Read
  updates the presented raster and metadata without recreating the panel; undo
  restores the previous visible result.
- `ClearingTheReadShowsTheExplicitEmptyState` — clearing drops the frame, empties
  the probed media and shows an actionable message naming the node.
- `OfflineReadReportsItsPathOnTheViewer` — a missing reference fails visibly with
  the path on the panel, and rebinding recovers it.
- `DownstreamEffectKeepsItsSingleMediaSourcesDomain` — an effect on a Read keeps
  the Read's raster and transport.
- `SeveralMediaSourcesKeepTheDefaultCompositionCanvas` — with several sources the
  viewer keeps the established default canvas rather than picking one.
- `RetiredContinuousZoomStateStillFitsTheImage` — a persisted
  `{zoomMode: "Custom", zoom: 0.05}` panel state still shows the image at Fit
  (covering the media surface) and the zoom selector reads `Fit`.
- `UserDocumentDisplaysItsAttachedRead` (local confirmation, skipped unless
  `NEMO74_USER_DOC` is set) — the owner's document and workspace: the PNG
  reports `source=1920x1440 frames=1`, the H.264 clip reports
  `source=1920x1080 frames=650 rate=25` with `decode selection: Vulkan`, and both
  captures are visibly rendered. Private media is never a CI fixture.

Commands (run from the repository root):

```bash
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 \
  build/debug/tests/nemo_workspace_ui_tests --gtest_filter='ReadViewerSurface.*-ReadViewerSurface.UserDocument*'

# Local confirmation on the supplied examples (owner media, not a fixture):
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 \
  NEMO74_USER_DOC=/tmp/nemo74-user.nemo \
  NEMO74_USER_WORKSPACE="$HOME/.config/Nemo/nemo-ui/workspace.json" \
  NEMO74_VIEWER_PANEL=panel-1 NEMO74_EXPECT_FRAMES=650 NEMO74_EXPECT_RATE=25 \
  build/debug/tests/nemo_workspace_ui_tests --gtest_filter='ReadViewerSurface.UserDocument*'
```

## Production correctness

- One media owner: the same `MediaImportService`, `SourceSession`, evaluation
  plan and command history are reused; no second decoder, registry or
  controller-specific model is introduced.
- Request/source identity, snapshot revision and per-destination freshness are
  unchanged; a retarget or relink re-probes under a new generation and
  superseded probe/render results are rejected.
- Evaluation stays independent of panel selection; presentation still follows
  the panel's own destination and the document is mutated only through commands.
- Missing/invalid OCIO remains an explicit render failure under the existing
  policy; nothing bypasses the viewing transform or ships a demo config.

## Prototype conformance

- Node/panel geometry, chrome, header and transport are unchanged; the new
  viewer status text uses the existing theme tokens and sits inside the accepted
  image area, replacing a placeholder that already existed.
- Zoom follows the prototype's mode-driven contract (`Fit`/`100%`/`50%`, wheel
  selects a mode); the retired continuous scale is removed.
- Reading viewer media from the attached target does not change graph gestures
  or inspector presentation.

## Remaining gates (owner)

- #61's owner image/API review of the Read display name, the reference schema
  addition and the node-local file control is still open. The owner directed
  this repair's landing on `main`; that direction does not close #61's review,
  which remains the open gate on the landed Read surface.
- The `nemo-cli render` subcommand remains a synthetic gradient stub; the
  headless proof path here is native presentation, not `render`.
- Windows-native confirmation was not run (Linux Wayland/X11 only).
