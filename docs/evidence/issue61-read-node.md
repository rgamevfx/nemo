# Read node with a node-local media path (issue #61)

**Branch:** `issue/61-read-node`
**Work class:** system extension of the catalog/parameter/inspector/command/media
seams, plus the owner-approved new-design slice (node-local media control).
**Headless capture:** [`issue61-read-node.json`](issue61-read-node.json)
**Assets:** [`assets/issue61-read-node/`](assets/issue61-read-node/)

## Recorded decisions

The issue's unresolved observable decisions were implemented with the choices
recorded on the task before editing (owner approval of appearance/API remains a
review gate, not a blocker for implementation):

1. Persistent type stays `source`; display name is **Read**.
2. The node's `source` parameter remains the document source key (matching
   `evalSource` / `eval::SourceSession` and cache identity); the hosted control
   edits the underlying `SourceReference` and Media Bin entry and surfaces the
   resolved path.
3. Reuse is by normalized path + interpretation; the existing Media Bin entry
   for that key is reused, and a different file re-points only that node.
4. `#`/`@` patterns resolve through the shared image adapter; `frameOffset` /
   `frameStep` plus optional authored `firstFrame` / `lastFrame` are explicit,
   and a frame outside the authored range is reported, never clamped. The
   prototype's `colorspace` choice is **not** adopted: interpretation stays
   explicit metadata resolved by the adapter (scene-linear Rec.709).
5. The file control is a registered parameter editor (`nemo.read.source`) with
   text entry + browse through the shared native chooser; the node is created
   first and browse edits it.
6. Offline is reported on the resolved path; relink updates the keyed reference
   in place so every node sharing it recovers with authored state intact.
7. No implicit viewer attachment; viewing stays #60's explicit Viewer-node
   action.

## Production correctness

- **One undoable command per edit.** `registerReadSourceCommand` resolves or
  creates the reference + entry and points the node at it; `relinkReadSourceCommand`
  and `setReadSourceTimingCommand` carry a stale-reference guard
  (`GraphError::StaleMediaSource`). A cancelled browse, a rejected path and an
  ambiguous/unsupported file submit nothing (proven by tests).
- **Persistence.** `firstFrame`/`lastFrame` are additive optional fields:
  written only when authored, restored verbatim, and an inverted authored range
  is a structural load error. Existing documents keep their byte shape.
- **Evaluation.** The range check lives in the shared image adapter
  (`readImageFrame` / `probeImageFrame`), so CPU and GPU agree; out-of-range is
  an error naming the path and range before any plane access. A sequence
  reference is classified as image data from its own path, so an out-of-range
  frame is no longer misclassified as a clip on the GPU path.
- **No duplicate owners.** One import worker (media::MediaImportService) is
  shared with the Media Bin adapter through a requester-scoped probe
  (`requestReferenceProbe`); the Read controller owns no decoder, catalog or
  undo stack.

## Evidence

Local debug/release presets (no hosted CI required for this change).

| Check | Result |
| --- | --- |
| `ctest`/binary debug core suite (`nemo_core_tests`) | 303/303 pass |
| debug `nemo_media_tests` | 20/20 pass |
| debug `nemo_workspace_ui_tests` | 104 pass, 1 skipped (native-only viewer-surface case) |
| debug `nemo_viewer_tests` (GPU) | 39/39 pass |
| headless `nemo_core_tests` (Qt-free) | 292/292 pass |
| release `nemo_core_tests` | 291/291 pass |
| release `nemo_media_tests` | 20/20 pass |
| release `nemo_workspace_ui_tests` | 104 pass, 1 skipped |
| debug `nemo-ui`, `nemo-cli` build | clean |
| release `nemo-ui`, `nemo-cli` build | clean |
| ASan+UBSan focused (`ReadSourceTest.*`, `ReadSourceUiTest.*`, `MediaTest.*Range*`) | pass, no leaks/reports |

New tests: `tests/ReadSourceTests.cpp` (command/reference/persistence
semantics), `tests/ReadSourceUiTests.cpp` (adapter + editor through the
parameter-editor seam), `MediaTest.AuthoredSequenceRangeRejectsOutOfRangeFrames`,
and the GPU case in `Viewer.StillSequenceOutOfRangeFrameFailsNamingPath`.

### Focused static analysis

`cmake --workflow --preset analysis` fails on this tree **before** and
independently of this change: `core/commands/NetworkCommands.cpp:82`
(`performance-unnecessary-copy-initialization`, treated as an error) is a
pre-existing finding introduced by #49 and is not touched here. The analyzer
reports it as a local-copy warning on the `Network sourceSnapshot = source;`
line whose copy exists so the source survives a mutating `addNetwork` call — a
known false positive of that check for aliased containers, so it needs the
owner's call rather than a mechanical removal. Every core translation unit
changed by this issue (`ReadSourceCommands.cpp`, `NodeCatalog.cpp`,
`Serialization.cpp`, `Document.cpp`) analyzes clean when built individually
with the same pinned clang-tidy 18 configuration.

Headless CPU-reference and native GPU evaluation of the same node reference
agree exactly (bit-identical PPM) for a still and for an in-range sequence
frame; an out-of-range sequence frame fails with the same range diagnostic on
both. Reproduced with:

```bash
build/debug/apps/nemo-cli/nemo-cli evaluate      docs/evidence/assets/issue61-read-node/read-still.nemo \
    --out /tmp/cpu-still.ppm --frame 0 --width 24 --height 16
build/debug/apps/nemo-cli/nemo-cli evaluate-gpu  docs/evidence/assets/issue61-read-node/read-still.nemo \
    --out /tmp/gpu-still.ppm --frame 0 --width 24 --height 16
# sequence, in range and out of range
build/debug/apps/nemo-cli/nemo-cli evaluate     docs/evidence/assets/issue61-read-node/read-seq-range.nemo \
    --out /tmp/cpu-seq.ppm --frame 0 --width 24 --height 16
build/debug/apps/nemo-cli/nemo-cli evaluate-gpu docs/evidence/assets/issue61-read-node/read-seq-range.nemo \
    --out /tmp/gpu-seq.ppm --frame 0 --width 24 --height 16
build/debug/apps/nemo-cli/nemo-cli evaluate-gpu docs/evidence/assets/issue61-read-node/read-seq-range.nemo \
    --out /tmp/out-range.ppm --frame 2 --width 16 --height 16
```

## Prototype conformance

- On-canvas presentation is unchanged and driven by the catalog: the node is
  created from the **I/O** category and displays as **Read** (the prototype's
  `StudioModel.qml` I/O group and `Read` type). `GraphItem` geometry, ports and
  chrome are untouched.
- The prototype `Read` node took its file from the Media Bin and exposed
  `colorspace` / `first` / `last`. The node-local file control is the
  **owner-approved production-only deviation** recorded in the issue. Its
  controls consume the shared `#40` components (`ChromeButton`, `Theme` tokens)
  and the existing inspector host; `first`/`last`/offset/step are authored on
  the shared media reference rather than duplicated as a second timing model;
  `colorspace` is deliberately not adopted (see decision 4).

## Remaining gaps (owner/human gate)

- **Native capture and interaction evidence** for browse / edit / clear /
  relink at matched reference sizes, plus the owner image/API review of the
  display-name, the reference schema addition and the new control, remain open.
  This environment exposes no AT-SPI accessibility bus, so the harness cannot
  drive or capture the native application; the offscreen editor-host test is
  the strongest automated check available here.
- The `nemo-cli render` subcommand writes a fixed synthetic gradient and is not
  an evaluation path; the headless proof above uses `evaluate` /
  `evaluate-gpu`.
