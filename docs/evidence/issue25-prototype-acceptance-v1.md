# Prototype acceptance inventory and reference baseline (issue #25)

**Version:** `v1`  
**Roadmap:** [#24](https://github.com/rgamevfx/nemo/issues/24)  
**Inventory owner:** presentation acceptance/reference only; this document does not approve prototype architecture or production parity.

## Baseline identity and durable reference

The read-only prototype checkout was captured at:

- **Root:** `/home/rgame/dev/Nemo_UI_Prototype`
- **Branch:** `prototype/viewer-workspace`
- **HEAD:** `76071624aebd6577ca613a9d3322f535fd80f399`
- **Captured status:** 0 staged files, 9 modified tracked files, 14 untracked files.
- **Archive:** [`issue25-prototype-reference-v1.tar.gz`](assets/issue25-prototype-reference-v1.tar.gz)
- **Archive SHA-256:** `0027b39f6ba20fc2168f83215f8ba036fcc408284e6b3a2702a21c2c37b8e57e`
- **Per-file manifest:** [`issue25-prototype-reference-manifest.json`](assets/issue25-prototype-reference-manifest.json)

The archive contains 107 current files: all 28 files under `apps/nemo-ui/qml/` (including backup files), all 76 files under `apps/nemo-ui/prototype/evidence/` (session JSON and image evidence), `apps/nemo-ui/prototype/main.cpp`, `apps/nemo-ui/prototype/CMakeLists.txt`, and `run-prototype.sh`. The manifest records a SHA-256, byte count, and relative path for every archived file. It also records the complete porcelain status at capture time. The archive is the durable reference; a future checkout need not retain this branch or its uncommitted work.

The uncommitted relevant files were preserved, not committed or reset:

- Modified: `apps/nemo-ui/prototype/main.cpp`, `qml/FrameRuler.qml`, `qml/GraphPanel.qml`, `qml/Main.qml`, `qml/Panel.qml`, `qml/ParametersPanel.qml`, `qml/StudioModel.qml`, `qml/Theme.qml`, `qml/ViewerPanel.qml`.
- Untracked current evidence: `compact-category-nodes.png`, `compact-docking-rearranged.png`, `compact-docking-session.json`, `compact-viewer-docking.png`, `node-category-settings.png`, `node-style-session.json`.
- Untracked backups: `AnimationHeaderTools.qml~`, `AnimationPanel.qml1~`, `AnimationPanel.qml~`, `GraphPanel.qml~`, `ParametersPanel.qml1~`, `ParametersPanel.qml~`, `StudioModel.qml1~`, `StudioModel.qml~`.

The source locations used for the inventory are the prototype's `qml/GraphPanel.qml`, `ParametersPanel.qml`, `AnimationPanel.qml`, `MediaBinPanel.qml`, `TimelinePanel.qml`, `ViewerPanel.qml`, `Main.qml`, `Panel.qml`, `StudioModel.qml`, `Theme.qml`, `FrameRuler.qml`, `AnimationHeaderTools.qml`, `TimelineHeaderTools.qml`, `DockDrag.qml`, `PanelDragHandler.qml`, and `WorkspaceNode.qml`, plus `apps/nemo-ui/prototype/main.cpp`, `apps/nemo-ui/prototype/CMakeLists.txt`, and `run-prototype.sh`. The archive/manifest also retain every sibling QML file and backup so the reference is not narrowed to a hand-picked screenshot path.

## Coverage contract

[`issue25-coverage-v1.json`](issue25-coverage-v1.json) is the machine-readable index. It contains all 15 evidence-session JSON records, explicit current/historical status at both record and individual-entry level, exact supersession pointers for replaced entries, every current check and source-behavior item, the exact source path and JSON field, and one or more roadmap ticket links per entry.

- Current records: 13; historical records: 2.
- Current mapped entries: 508; historical/superseded retained entries: 167.
- The 508 current entries include structured check objects as well as string checks; structured checks are serialized with stable JSON key ordering so no check disappears during indexing.
- A record-level mapping is never the acceptance proof by itself: each `current` entry carries its own `tickets` and `ticket_links` fields. Historical entries carry `superseded_by` pointers to exact successor entries; mixed records retain current entries beside explicitly superseded ones.
- Prototype checks demonstrate UI behavior only. They do **not** prove production `Document`, command history, persistence, evaluator, decoder, GPU, failure, or performance contracts.

## Current behavior inventory

The entries below summarize the current source/evidence families. The machine file is authoritative for the complete check-by-check list; the counts include both verification checks and source-behavior declarations where present.

| Family | Current source/evidence records | Current behavior captured | Production mapping |
| --- | --- | --- | --- |
| Graph | `graph-behaviors-session.json`, `graph-routing-session.json`, `subnet-interaction-session.json`, `node-creation-deletion-session.json`, `pipe-gesture-session.json` | Node movement with topology retention; output/input connect, replace, reconnect and disconnect; cancellation and invalid-drop preservation; Tab/catalog placement and wire insertion; inspector double-click; straight pipes, reroute dots and zoom-independent alignment; pan/zoom, box/group selection, optional mask ports, nested subnet collapse/navigation and interface routing; selected/last-click creation, deletion and dependent-state cleanup; source/destination pipe-body gestures. | [#33](https://github.com/rgamevfx/nemo/issues/33), [#44](https://github.com/rgamevfx/nemo/issues/44), [#49](https://github.com/rgamevfx/nemo/issues/49), [#46](https://github.com/rgamevfx/nemo/issues/46) |
| Parameters | `workflow-session.json`, `node-creation-deletion-session.json`, `node-style-session.json` | Single/double-click distinction; unique newest-first inspectors, pinning/eviction, one/two-column packing and scroll retention; catalog search excludes existing instances; compact appearance settings update existing/new nodes; 28-high compact nodes and category colors; deletion clears affected inspector/animation/viewer state. | [#33](https://github.com/rgamevfx/nemo/issues/33), [#46](https://github.com/rgamevfx/nemo/issues/46), [#51](https://github.com/rgamevfx/nemo/issues/51), [#44](https://github.com/rgamevfx/nemo/issues/44) |
| Animation | `animation-overhaul-session.json` | Track/Curves split, all numeric curves visible independently of selection, explicit hide/isolate/show, key selection and movement, Alt insertion, exact time/value/slope editing, interpolation/tangent modes, collision rejection, grouped undo/redo, frame/range tools, dense and narrow layouts. | [#46](https://github.com/rgamevfx/nemo/issues/46), [#48](https://github.com/rgamevfx/nemo/issues/48), [#50](https://github.com/rgamevfx/nemo/issues/50) |
| Media | `media-bin-session.json` | Nested bins and protected used media; list/grid/narrow/search/metadata/smart-search views; source-role and viewer-target selection; multi-source insert/overwrite and linked AV timeline candidates; dockable panel placement; dense catalog and scrolling. | [#39](https://github.com/rgamevfx/nemo/issues/39), [#43](https://github.com/rgamevfx/nemo/issues/43), [#41](https://github.com/rgamevfx/nemo/issues/41), [#40](https://github.com/rgamevfx/nemo/issues/40), [#47](https://github.com/rgamevfx/nemo/issues/47), [#52](https://github.com/rgamevfx/nemo/issues/52), [#54](https://github.com/rgamevfx/nemo/issues/54) |
| Timeline | `timeline-session.json` | Half-open frame clips and atomic preview/commit; move/trim/ripple/roll/slip/slide/split/delete/insert/overwrite and clipboard operations; linked AV policy; snapping, selection, targeting, locking, marks, transport, pan/zoom, edge scrolling and dense tracks; media popup insertion and candidate previews. | [#53](https://github.com/rgamevfx/nemo/issues/53), [#52](https://github.com/rgamevfx/nemo/issues/52), [#54](https://github.com/rgamevfx/nemo/issues/54), [#56](https://github.com/rgamevfx/nemo/issues/56), [#57](https://github.com/rgamevfx/nemo/issues/57) |
| Viewer | `viewer-session.json` | Play/stop/step, in/out marks and loops, editable timecode/frame, ruler seek and range dragging; layer/channel selection; Fit/100/50%; Full/Half/Quarter fixture proxy selection; narrow transport layout and compact viewer chrome. | [#40](https://github.com/rgamevfx/nemo/issues/40), [#41](https://github.com/rgamevfx/nemo/issues/41), [#47](https://github.com/rgamevfx/nemo/issues/47) |
| Workspace | `workspace-session.json`, `workflow-session.json` | Four-panel composition shell, splitter resizing, workspace/tab state retention, inspector accumulation, themed controls, catalog/tools entry points, and fixture panel state. | [#40](https://github.com/rgamevfx/nemo/issues/40), [#41](https://github.com/rgamevfx/nemo/issues/41) |
| Appearance | `node-style-session.json`, `workspace-session.json` | Current category fill defaults (`Merge #60656b`, `Filter #a96832`, `IO #386b91`), hex entry/reset/rejection, selected-node contrast, inherited colors for new nodes and IO synchronization, Paper/Graphite theme repaint. | [#40](https://github.com/rgamevfx/nemo/issues/40), [#44](https://github.com/rgamevfx/nemo/issues/44) |
| Docking | `compact-docking-session.json` plus `compact-docking-rearranged.png` and `compact-viewer-docking.png` | Complete compact-header behavior: blank compact viewer header starts drag; every panel title drags; center drop creates a tab while retaining both panels; title drag to the left edge creates a separate tile; Escape cancels without mutation; channel dropdown and next-frame transport remain clickable; the study switcher is absent. | [#40](https://github.com/rgamevfx/nemo/issues/40) |

### Compact node and docking requirements called out explicitly

- Compact nodes are **112 logical pixels wide and 28 high** after packing (42 high before compact mode). Their category fill remains visible when selected and text switches for contrast on white/black fills.
- Category settings are not a second production palette: the source evidence demonstrates the current session setting and reset behavior only. Production defaults and public appearance API still need the owner/image review required by the roadmap.
- Compact viewer docking uses the shared header, with no variant/study switcher. The full eight-entry docking record is current and mapped in the coverage JSON; both rearranged and compact-viewer images are archived.

## Historical supersession and exclusions

Historical evidence remains in the archive and machine index so that a reader can reproduce why it is not current:

- `animation-interaction-session.json` (54 checks) was superseded by `animation-repair-session.json`, then by `animation-overhaul-session.json`. Selected-channel-only curve visibility and the missing inspector dispatch are not current behavior.
- `animation-repair-session.json` (33 repair checks plus 54 prior checks) was superseded by the overhaul, which removed the rejected toolbar/instruction/status chrome while retaining repaired interaction semantics.
- The cubic-wire geometry study in `graph-behaviors-session.json` is superseded by straight displayed pipes/reroute dots in `graph-routing-session.json`.
- Navigation, multi-selection, mask and subnet behavior in `graph-behaviors-session.json` is superseded by `subnet-interaction-session.json`.
- The individual obsolete entries are retained and pointer-linked in the machine index: graph checks `[13]`, `[14]`, `[15]`, `[17]`, implementation `[4]`, and `controls/place` point to the current node-creation or graph-routing successors; workflow `verified[4]` and `behavior[4]` point to node-creation immediate creation. The mixed `controls/cancel` entry marks only its pending-placement clause superseded; wire cancellation and invalid-drop preservation remain current. `controls/insert` remains current and links the newer disconnected-node insertion check as corroborating evidence.
- The latest node-creation record **restores immediate creation** (`Tab`/search/Enter/click); it supersedes the older pending-preview/no-creation wording in workflow and graph behavior. The old ghost-placement/extra-click variant is not a current requirement.
- The compact docking record explicitly supersedes the viewer study-switcher variant: the switcher is absent from the current compact header. A screenshot of a study variant is not acceptance evidence.

No superseded record or entry is silently deleted; each historical entry is labelled `historical`, retains its original text, and points to an exact successor in the machine file. No prototype record is classified as production proof.

## Production gaps (not prototype failures)

These are required production work, not claims that the prototype should implement them:

- **Media reality:** filesystem import/probing, decode/format validation, thumbnails/waveforms, relinking, proxies, source failures, real AV playback and source-time mapping: [#43](https://github.com/rgamevfx/nemo/issues/43), [#21](https://github.com/rgamevfx/nemo/issues/21), [#52](https://github.com/rgamevfx/nemo/issues/52), [#54](https://github.com/rgamevfx/nemo/issues/54).
- **Authoritative state/history:** one persistent Document, typed values, stable identities, shared command history, atomic failure semantics, and revision checks: [#29](https://github.com/rgamevfx/nemo/issues/29), [#30](https://github.com/rgamevfx/nemo/issues/30), [#44](https://github.com/rgamevfx/nemo/issues/44), [#48](https://github.com/rgamevfx/nemo/issues/48), [#51](https://github.com/rgamevfx/nemo/issues/51), [#53](https://github.com/rgamevfx/nemo/issues/53).
- **Persistence/recovery:** project save/open, portability, versioning and crash recovery are not demonstrated by session-only QML: [#35](https://github.com/rgamevfx/nemo/issues/35).
- **Production evaluation/output:** real graph evaluation, independent CPU/GPU/reference oracles, renderer ownership, retained resources, cancellation/stale-result handling and actual viewer-cache behavior remain with the production rendering/evaluation tickets and contracts: [#13](https://github.com/rgamevfx/nemo/issues/13), [#22](https://github.com/rgamevfx/nemo/issues/22), [#47](https://github.com/rgamevfx/nemo/issues/47), [#42](https://github.com/rgamevfx/nemo/issues/42). Prototype images must not be used as pixel or latency gates.
- **Production scale/performance:** dense fixture counts are interaction evidence only; measured workload and integrated parity remain [#16](https://github.com/rgamevfx/nemo/issues/16) and [#42](https://github.com/rgamevfx/nemo/issues/42).

## Owner questions and required review

No new conflict was found that blocks the Phase 1 session/catalog foundation. The following are narrow, existing decisions and reviews; they block only their affected production work:

1. **Project-file format and recovery contract** remain an owner decision in [#32](https://github.com/rgamevfx/nemo/issues/32), affecting [#35](https://github.com/rgamevfx/nemo/issues/35). The inventory records session-only behavior and does not choose a format.
2. **First extension and lifecycle** remain an owner decision in [#28](https://github.com/rgamevfx/nemo/issues/28), affecting the extension jobs only. The catalog inventory does not imply an extension API.
3. **Human image/interaction review:** the source records explicitly await feel/visual review for graph gestures, node appearance, animation, timeline, viewer, workspace and media. Passing a throwaway native driver does not grant this approval.
4. **Public API/new-node/dependency/image baseline reviews** remain required by the roadmap/AGENTS.md. This document recommends mappings; it does not grant approval.

## Exact mechanical coverage recipe

Run from the production checkout after the archive and JSON are present. This checks the archive file hashes, the complete evidence-record set, every indexed entry's original text/field path, ticket mappings, mixed-record supersession pointers, and the published totals without building or launching either application:

```bash
python3 - <<'PY'
import hashlib, json, pathlib, re, tarfile

coverage = json.loads(pathlib.Path('docs/evidence/issue25-coverage-v1.json').read_text())
archive = 'docs/evidence/assets/issue25-prototype-reference-v1.tar.gz'
root = 'issue25-prototype-reference-v1'

def at(document, path):
    value = document
    for segment in filter(None, path.split('/')):
        match = re.fullmatch(r'([^\[]+)(?:\[(\d+)\])?', segment)
        assert match, path
        value = value[match.group(1)]
        if match.group(2):
            value = value[int(match.group(2))]
    return value

def entry_text(value):
    return value if isinstance(value, str) else json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))

with tarfile.open(archive) as tf:
    members = {member.name: member for member in tf.getmembers()}
    manifest = json.loads(tf.extractfile(root + '/REFERENCE-MANIFEST.json').read())
    for item in manifest['files']:
        member = root + '/' + item['path']
        data = tf.extractfile(member).read()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256'], item['path']
    evidence = sorted(name.removeprefix(root + '/')
                      for name in members
                      if name.startswith(root + '/apps/nemo-ui/prototype/evidence/')
                      and name.endswith('.json'))
    records = sorted(record['file'] for record in coverage['records'])
    assert evidence == records, (len(evidence), len(records), sorted(set(evidence) ^ set(records)))

    all_entries = {entry['id']: entry
                   for record in coverage['records'] for entry in record['entries']}
    assert len(all_entries) == sum(record['entry_count'] for record in coverage['records'])
    for record in coverage['records']:
        assert record['entry_count'] == len(record['entries'])
        document = json.loads(tf.extractfile(root + '/' + record['file']).read())
        for entry in record['entries']:
            filename, field = entry['id'].split(':', 1)
            assert filename == pathlib.PurePosixPath(record['file']).name
            assert entry['source'] == record['file'] + field
            assert entry['text'] == entry_text(at(document, field)), entry['id']
            if entry['status'] == 'current':
                assert entry['tickets'] and len(entry['tickets']) == len(entry['ticket_links'])
                assert all(link == f"https://github.com/rgamevfx/nemo/issues/{number}"
                           for number, link in zip(entry['tickets'], entry['ticket_links']))
            else:
                assert entry['superseded_by']
                assert all(target in all_entries for target in entry['superseded_by'])

    assert coverage['current_entry_count'] == sum(
        entry['status'] == 'current' for entry in all_entries.values())
    assert coverage['historical_entry_count'] == sum(
        entry['status'] != 'current' for entry in all_entries.values())
print('issue25 coverage OK:', coverage['current_entry_count'], 'current entries;',
      coverage['historical_entry_count'], 'historical/superseded entries;', len(evidence), 'evidence JSON records')
PY
```

The archive and manifest can then be checked independently with:

```bash
sha256sum docs/evidence/assets/issue25-prototype-reference-v1.tar.gz
# expected: 0027b39f6ba20fc2168f83215f8ba036fcc408284e6b3a2702a21c2c37b8e57e
```

Native reproduction, if a human chooses to inspect a disagreement, is `cd /home/rgame/dev/Nemo_UI_Prototype && ./run-prototype.sh`; use the session JSON's `surface`, controls and capture names for the recorded scenario. Do not modify that checkout while inspecting it. Production debug/release/sanitizer runs are outside this documentation capture and are not claimed here.
