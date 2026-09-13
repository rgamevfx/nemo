# Issue #72 — Structurally shared document versions

Evidence for [#72](https://github.com/rgamevfx/nemo/issues/72). The diagnosis
and pre-fix baseline remain [#69](https://github.com/rgamevfx/nemo/issues/69)
([report](issue69-editing-scale.md)); this ticket implements the fix and records
one bounded post-fix pass against that retained evidence. No new measurement
system, benchmark target or CI gate is introduced.

## What changed

The persistent document is now a set of handles to structurally shared storage
(`CowVector`/`CowMap`, `src/nemo/core/SharedContainers.hpp`). Copying a
document copies handles; a controlled mutation copies the chunk index and only
the bounded chunks/records it changed.

- `CommandStack` retains a version handle plus the identities the transition
  touched (`ChangeRecorder`), in a preallocated bounded ring; capacity is
  unchanged. Undo/redo replay the touched set in reverse and compare values.
- Publication derives changed/created identities from the touched set instead
  of diffing the document.
- Gesture previews and prepared saves retain shared versions; a prepared save
  no longer serializes the project to prepare a write.
- `isDirty()` compares the current version against the version captured at
  open/save, treating storage that still shares chunks as identical.
- `synchronizeReferences` is scoped to the touched networks, instances and
  channels; the complete pass remains for restoration/replacement.
- `MediaCatalog`/`Document` mutation is reachable only through the controlled
  path that records the transaction.

ADR-0007 records this as an accepted revision of its deep-copy history choice;
the ownership map gained the storage row.

## Method

Same machine, compiler, preset and driver as #69: release
(`-O3 -DNDEBUG -flto=auto`), glibc `mallinfo2()` in-use-heap attribution,
nearest-rank percentiles, 20 cold + 200 warm samples (60 warm for the save-side
timers) with the harness's untimed undo keeping document size and history depth
constant. Workloads, timers and their scopes are exactly #69's; the driver was
recompiled against this revision.

The retention driver's prepared-save step was adapted to the new contract: the
write request no longer carries a serialized baseline string, so the retained
snapshot is reported alone (`prepared_save_snapshot`).

## Results — operation cost

Every row is #69's warm p50 for that workload; "before" is #69's retained
record, "after" is this revision.

| workload | operation | before (µs) | after (µs) | change |
|---|---|---|---|---|
| graph.chain.small | `submit.set_param` | 25.28 | 5.06 | 5x |
| graph.chain.small | `snapshot` | 12.77 | 0.05 | 241x |
| graph.chain.small | `prepare_save` | 543.01 | 0.41 | 1331x |
| graph.chain.small | `gesture.begin` | 10.73 | 3.71 | 3x |
| graph.chain.small | `gesture.update` | 17.29 | 4.92 | 4x |
| graph.chain.small | `gesture.commit` | 29.74 | 8.68 | 3x |
| graph.chain.large | `submit.set_param` | 383.09 | 17.28 | 22x |
| graph.chain.large | `submit.set_layout` | 372.53 | 17.98 | 21x |
| graph.chain.large | `submit.add_node` | 510.11 | 23.54 | 22x |
| graph.chain.large | `submit.connect` | 452.60 | 159.78 | 3x |
| graph.chain.large | `submit.set_parameters.64` | 430.45 | 407.61 | 1x |
| graph.chain.large | `snapshot` | 102.90 | 0.05 | 2018x |
| graph.chain.large | `prepare_save` | 4245.87 | 0.40 | 10615x |
| graph.chain.large | `gesture.begin` | 72.73 | 3.51 | 21x |
| graph.chain.large | `gesture.update` | 118.55 | 4.84 | 24x |
| graph.chain.large | `gesture.commit` | 430.01 | 19.51 | 22x |
| params.small | `submit.set_parameters.64` | 129.93 | 149.37 | 1x |
| params.large | `submit.set_param` | 1479.24 | 45.13 | 33x |
| params.large | `submit.set_parameters.64` | 1566.61 | 780.47 | 2x |
| params.large | `snapshot` | 722.92 | 0.05 | 14175x |
| params.large | `prepare_save` | 30259.33 | 0.40 | 75838x |
| graph.multinetwork.large | `submit.set_param` | 877.47 | 8.56 | 103x |
| graph.multinetwork.large | `snapshot` | 327.98 | 0.05 | 6560x |
| graph.multinetwork.large | `prepare_save` | 13989.07 | 0.40 | 35148x |
| animation.small | `submit.insert_keyframe` | 38.08 | 25.90 | 1x |
| animation.large | `submit.insert_keyframe` | 1150.96 | 875.51 | 1x |
| media.large | `submit.set_media_metadata` | 654.84 | 12.51 | 52x |
| media.large | `snapshot` | 286.56 | 0.05 | 5307x |
| media.large | `prepare_save` | 7659.00 | 0.39 | 19689x |

| workload | sequence | before (µs) | after (µs) | change |
|---|---|---|---|---|
| graph.chain.small | `undo_sequence` | 21.79 | 2.36 | 9x |
| graph.chain.small | `redo_sequence` | 21.46 | 2.30 | 9x |
| graph.chain.large | `undo_sequence` | 373.59 | 12.79 | 29x |
| graph.chain.large | `redo_sequence` | 381.82 | 12.74 | 30x |
| params.large | `undo_sequence` | 1604.44 | 24.93 | 64x |
| params.large | `redo_sequence` | 1592.68 | 24.55 | 65x |
| graph.multinetwork.large | `undo_sequence` | 830.74 | 4.24 | 196x |
| graph.multinetwork.large | `redo_sequence` | 821.01 | 3.91 | 210x |
| animation.large | `undo_sequence` | 429.36 | 5.18 | 83x |
| animation.large | `redo_sequence` | 429.93 | 5.12 | 84x |
| media.large | `undo_sequence` | 542.23 | 3.09 | 176x |
| media.large | `redo_sequence` | 562.70 | 3.08 | 183x |

## Results — newly exposed dirty-query and save-preparation costs

A throwaway probe (`dirty_probe.cpp`, compiled against the release core, not a
build target) measures the saved-state query and save preparation on a 65-node
and a 1 025-node document; `is_dirty_*` are the session's own answers, so the
probe also re-checks the contract rather than only timing it.

| document | `isDirty()` clean | dirty | after undo | one edit (undone untimed) | `prepareSave` |
|---|---|---|---|---|---|
| 65 nodes | 0.049 µs | 0.166 µs | 0.110 µs | 5.27 µs | 0.431 µs |
| 1 025 nodes | 0.049 µs | 0.183 µs | 0.120 µs | 45.55 µs | 0.432 µs |

`is_dirty_before = false`, `is_dirty_after_edit = true`,
`is_dirty_after_undo = false`: opening is clean, an edit is dirty, and undoing
back to the saved version is clean again.

Before this change the dirty refresh serialized the project
(`ProjectFile::serializeContent`, p50 0.53 ms for the 82-node document and
29.3 ms for the 1 026-node one, per #69), and `prepareSave` serialized it too.

## Results — retained allocation

#69's retention phases, in-use heap deltas in one process:

| phase | before | after |
|---|---|---|
| populate base document | 355 152 B | 231 312 B |
| one retained document snapshot | 41 200 B | **0 B** |
| history, 64/128/192/256 commits (capacity 256) | 2.93 / 3.15 / 3.15 / 3.15 MB | 0.48 / 0.53 / 0.53 / 0.53 MB |
| history, 320 / 384 commits (eviction engaged) | 216 816 / −384 B | 73 472 / 496 B |
| zero-capacity session, 64 commits | 5 296 B | 33 504 B |
| zero-capacity session, populated | 116 640 B | 229 664 B |
| gesture preview begin / cancel | 11 200 / −11 776 B | 10 656 / −11 040 B |
| prepared save (snapshot + baseline) | 18 816 B | 0 B (snapshot only) |

Per retained history entry this is ≈ 8.2 kB against #69's ≈ 49.2 kB (6x less),
and it is attributable to the changed record's bounded chunk rather than one
full document per entry. A snapshot, a prepared save and a gesture preview now
retain 0 B of their own: they share the existing storage.

Reported costs and residual gaps (measured, not hidden):

- **Wide parameter batches.** `submit.set_parameters.64` improves 1.5–6.5x on
  the large/multi-network/media documents and 2.0x on `params.large`, but is
  0.5x (39 → 74 µs) on `graph.chain.small` and 0.9x (130 → 149 µs) on
  `params.small`: 64 edits touch up to six 16-record chunks *and* add touched
  bookkeeping, while the batch's own validation/apply already performs one
  linear node lookup per edit (`Graph::node()` is a linear scan — pre-existing,
  unchanged by this work). A single edit is 5x faster; the regression is
  confined to wide batches on small single-network documents and stays below
  0.15 ms.
- **Animated key edits.** `submit.insert_keyframe` is ~1.3x better
  (1 151 → 922 µs on `animation.large`): the animation commands still revalidate
  and re-install the whole channel set, so channel payload dominates. Shared,
  untouched channel storage is not the cost.
- **Saved-state comparison.** `isDirty()` compares the current version against
  the version captured at open/save, skipping storage that still shares chunks
  (0.05 µs clean, 0.18 µs dirty at 1 025 nodes). It is a structural walk of the
  version, not per-identity divergence bookkeeping; it is exact content
  equality and never serializes.
- **Publication scans.** Publication makes one pass over each touched network's
  records per version to locate the touched identities (publication cost is
  O(records in touched networks), not O(project)). A full id index inside
  `Graph` would remove that pass; the model's existing node lookup is linear,
  so the pass is not the dominant term for the edits measured here.
- **Mutation boundary.** `Document::network()`/`Network::graph()`/sources/color
  remain writable by the command implementations rather than being hidden
  behind a builder type; every such access installs the transaction recorder
  and nothing falls back to a whole-document copy. Type-level encapsulation of
  the mutable surface remains open.
- **Native smoke scenario.** Not run: the shipped interaction-level coverage
  (`InteractiveUiTests`, `PanelContextUiTests` through the production
  controllers) passes, but the human-driven native pass — adjust a parameter,
  commit/cancel a gesture, undo/redo, watch the unsaved indicator, save while
  the viewer is live — is left to the owner.

## Verification

| Check | Selection | Result |
|---|---|---|
| Debug suite | `ctest --preset debug` | 515/515 passed |
| ASan+UBSan suite | `ctest --preset asan -R "Session\|Command\|Persistence\|Animation\|MediaCatalog\|Network\|Graph"` | 197/197 passed, non-empty selection |
| Driver correctness (release) | `editing_scale_driver correctness` | 11/11 passed |
| Driver correctness (ASan+UBSan, `-Werror`) | same, asan build | 11/11 passed, no sanitizer output |
| Release build | `cmake --build --preset release` | clean |
| Headless build/tests | `ctest --preset headless -E nemo_media_tests` | green |

**Pre-existing headless limitation (not introduced here):**
`cmake --build --preset headless` cannot link `nemo_media_tests` because
`tests/MediaTests.cpp:222` calls
`nemo::media::probeMediaCapabilities(const nemo::gpu::Device*)`, which is not
compiled with `NEMO_BUILD_GPU=OFF`; `ctest --preset headless` therefore reports
one `nemo_media_tests_NOT_BUILT` placeholder. Unchanged from #69.

## Not covered here

- No hosted CI run and no native edit-to-visible timing; #69's timings are
  core/session call costs, not interactive latency. The native smoke scenario
  (parameter adjust, gesture commit/cancel, undo/redo, unsaved indicator, save)
  and the media-library/viewer integration remain separate observations.
- Windows execution is outside the current Linux-only window.
- A large genuinely deleted payload still costs real reclamation work; the
  measurement above shows release, not constant-time destruction.

## Commands (reproduce)

```bash
# driver (throwaway, one per preset; flags from that preset's compile_commands)
SRC=docs/evidence/assets/issue69-editing-scale/editing_scale_driver.cpp
/usr/bin/c++ -I src -isystem build/release/vcpkg_installed/x64-linux/include \
  -O3 -DNDEBUG -std=c++20 -flto=auto -fno-fat-lto-objects -Wall -Wextra -Wpedantic \
  $SRC build/release/src/nemo/libnemo_core.a -o /tmp/issue72-driver/editing_scale_driver
/tmp/issue72-driver/editing_scale_driver timings   > timings-release.jsonl
/tmp/issue72-driver/editing_scale_driver retention > retention-release.jsonl
```
