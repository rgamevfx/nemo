import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Production media bin panel (issue #43). This is a faithful port of the
// archived prototype MediaBinPanel.qml (docs/evidence/assets/
// issue25-prototype-reference-v1.tar.gz) onto the persistent catalog adapter
// exposed as the `mediaLibrary` context property (apps/nemo-ui/MediaLibraryModel).
//
// Ownership:
//   * Catalog identity, hierarchy, metadata, marks, probe commits and history
//     belong to the adapter/Document/ProjectSession. This file only reads
//     adapter records and submits `apply` operations; it never mutates catalog
//     primitives and never touches Document state for selection or view prefs.
//   * View and selection preferences persist through workspace panelState only.
//   * Explicit open routes mediaLibrary.openMediaSource(group, sourceId); a
//     single selection never loads a viewer.
//   * Insertion is an intent only: mediaLibrary.requestTimelineInsert(group,
//     sourceIds, mode, marks) is emitted on the menu insertion gesture and the
//     drag payload carries the same ordered payload for the timeline drop. The
//     playhead is never touched here.
//
// Port notes / integration contract
//   Prototype substitutions:
//     studio.media                -> mediaLibrary (MediaLibraryModel context property)
//     studio.openMedia            -> mediaLibrary.openMediaSource(group, sourceId)
//     studio.mediaInsertRequested -> mediaLibrary.requestTimelineInsert(group, sourceIds, mode, marks)
//     studio.timeline.fps         -> record.runtime.frameRate, 24 fps for an unknown rate
//                                    (ViewerController's documented default)
//     panel-local saved searches  -> persisted catalog bins via apply(createSmartBin /
//                                    setSmartBinQuery / renameSmartBin / deleteSmartBin) and
//                                    library.smartBins; the stored query's recursive flag (defaulting
//                                    to projectScope for older records) is passed to query();
//                                    Offline/Unused stay panel built-ins
//     synthetic preview slot      -> image://nemo-media/<requestId> from library.thumbnailUrl(id)
//                                    in the archived glyph slot; an empty URL keeps the archived
//                                    kind glyph and never draws stand-in footage
//   Approved presentation additions:
//     * Kinds: unknown/other records stay honestly unlabelled (neutral glyph, no substituted V)
//       and "media only" queries pass the adapter's mediaOnly filter flag, so not-yet-probed
//       media is never excluded from the Offline/Unused smart views or from insertion.
//     * Pending/Offline/Unsupported token beside each media name from library.probeState(id);
//       probeState is also the adapter's visible-request hook, so visible rows repopulate
//       thumbnails after a project open.
//     * Full decode/fallback diagnostics plus "Apply probe" in the archived metadata dialog. The
//       diagnostic block has a fixed height of 64px and scrolls internally, so it never changes
//       the dialog size; "Apply probe" sits in the archived Import/Relink/Proxy row. The same
//       block distinguishes the persisted user marking ("Marked offline by the user") from the
//       runtime fact ("Source file is missing"); the panel never edits the authored flag.
//     * Import/Relink enabled with the adapter's native chooser in archived-geometry dialogs
//       (width 350 / content 320). An explicit sequence path (/shots/plate.####.exr) and a start
//       frame are typed directly. Import writes reference.frameOffset; relink shows the preserved
//       start read-only and never changes it (relinkMediaSourceCommand copies the reference).
//     * Proxy stays disabled exactly as archived.
//   Co-owned families (not implemented here):
//     * #41/#47 group routing, per-group clocks, two-viewer independence and the viewer role
//       menu: PanelContextRouter + ViewerPanel. This panel only calls openMediaSource.
//     * #54 timeline clip creation: this panel emits ordered sourceIds with one mark per
//       occurrence, plus mode and group. Drag payload keys itemId/itemIds/sourceId/sourceIds/
//       marks/mode/group ride the archived application/x-nemo-source-id key; the timeline drop
//       decides insertion and owns the playhead. No authored graph or playhead mutation here.
//     * #40 shell geometry, group badge, registry and reveal (checks 56/57): Panel.qml /
//       WorkspaceController; TimelinePanel calls mediaLibrary.revealMediaPanel(group).
//   Persistence: view, selection and navigation preferences only, through
//   workspace.setPanelState(panelId, ...). Selection never edits the project. Catalog gestures and
//   undo/redo use the shared ProjectSession history.
FocusScope {
    id: root

    objectName: "mediaBinPanel"
    focus: true
    activeFocusOnTab: true
    clip: true

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var workspace: null
    property var theme: null
    // Adapter context property installed by main.cpp before QML loads.
    property var library: typeof mediaLibrary !== "undefined" ? mediaLibrary : null
    property string currentParentId: "root"
    property string currentSmartBinId: ""
    property real treeWidth: 184
    property bool treeCollapsed: false
    property bool treeDragging: false
    property bool projectScope: false
    property string searchText: ""
    property string kindFilter: "all"
    property bool unusedOnly: false
    property bool offlineOnly: false
    property string viewMode: "list"
    property string sortField: "name"
    property bool sortDescending: false
    property var expandedBins: ({
            "root": true
        })
    property var selectedIds: []
    property string anchorId: ""
    property string renameId: ""
    property var clipboardIds: []
    property bool clipboardCut: false
    property string statusMessage: ""
    property bool statusError: false
    property string hoverDropBinId: ""
    property string pendingHoverBinId: ""
    property var labelOptions: ["", "#d9855d", "#d6b65d", "#86aa72", "#6b9fc1", "#9b83b4", "#b87991"]
    property int libraryRevision: root.library ? Number(root.library.revision || 0) : 0
    property int smartBinRevision: 0
    property bool stateReady: false
    // Pending merged panelState: several presentation writes can happen before
    // the workspace echoes the new state back through the panelState binding.
    property var stateCache: ({})
    // The prototype kept Offline/Unused as panel-local built-ins; saved
    // searches are the adapter's persisted smart bins (bin records with a
    // stored query), never panel-local records.
    readonly property var builtInSmartBins: [{
            "id": "smart-offline",
            "name": "Offline",
            "builtIn": true,
            "query": {
                "parentId": "project",
                "projectScope": true,
                "searchText": "",
                "kindFilter": "all",
                "mediaOnly": true,
                "unusedOnly": false,
                "offlineOnly": true
            }
        }, {
            "id": "smart-unused",
            "name": "Unused",
            "builtIn": true,
            "query": {
                "parentId": "project",
                "projectScope": true,
                "searchText": "",
                "kindFilter": "all",
                "mediaOnly": true,
                "unusedOnly": true,
                "offlineOnly": false
            }
        }]
    property var savedSmartBins: root.library && root.library.smartBins ? root.library.smartBins : []
    readonly property var smartBins: builtInSmartBins.concat(savedSmartBins)
    readonly property Item dragOverlay: Overlay.overlay
    property var selectedRecord: selectedIds.length > 0 ? itemById(selectedIds[0]) : null
    // The ordered record snapshot for the current bin/scope/filters/sort. It is
    // evaluated once per catalog revision and shared by the list and grid
    // models, the selection projection and the gesture helpers; the imperative
    // visibleItems() stays the compute function.
    readonly property var visibleRecords: root.visibleItems()
    // One catalog walk per revision, shared by every delegate and menu instead
    // of one walk per row.
    readonly property var selectedPayload: root.selectionPayload(root.selectedIds)
    readonly property var selectedSourceIds: selectedPayload.sourceIds
    readonly property var selectedMarks: selectedPayload.marks
    property Component headerTools: mediaHeaderTools

    // --- Prototype catalog helpers -----------------------------------------

    function arrayValue(value) {
        return value && value.length !== undefined ? value : [];
    }

    function itemById(id) {
        if (!library || !library.item || id === undefined || id === null)
            return null;
        return library.item(String(id));
    }

    function isBin(item) {
        return !!item && String(item.kind || "") === "bin";
    }

    function currentPath() {
        if (!library || !library.path)
            return [{
                    "id": "root",
                    "name": "Project",
                    "kind": "bin"
                }];
        return library.path(currentParentId) || [{
                "id": "root",
                "name": "Project",
                "kind": "bin"
            }];
    }

    function currentName() {
        var item = itemById(currentParentId);
        return item && item.name ? String(item.name) : "Project";
    }

    function smartBinById(id) {
        var key = String(id || "");
        for (var i = 0; i < smartBins.length; ++i)
            if (smartBins[i] && String(smartBins[i].id) === key)
                return smartBins[i];
        return null;
    }

    function activeQuery() {
        var smart = smartBinById(currentSmartBinId);
        if (smart && smart.query) {
            // Normalize one key: older records may predate the stored
            // `recursive` flag, where project scope implied recursion.
            var stored = smart.query;
            return {
                "parentId": String(stored.parentId || "root"),
                "projectScope": !!stored.projectScope,
                "recursive": stored.recursive !== undefined ? !!stored.recursive : !!stored.projectScope,
                "searchText": String(stored.searchText || ""),
                "kindFilter": stored.kindFilter || "all",
                "mediaOnly": !!stored.mediaOnly,
                "unusedOnly": !!stored.unusedOnly,
                "offlineOnly": !!stored.offlineOnly
            };
        }
        return {
            "parentId": projectScope ? "project" : currentParentId,
            "projectScope": projectScope,
            "recursive": projectScope,
            "searchText": searchText,
            "kindFilter": kindFilter,
            "mediaOnly": false,
            "unusedOnly": unusedOnly,
            "offlineOnly": offlineOnly
        };
    }

    function leaveSmartBin() {
        if (currentSmartBinId)
            currentSmartBinId = "";
    }

    function queryFilter(query) {
        var filter = {};
        if (query.kindFilter && query.kindFilter !== "all")
            filter.kind = query.kindFilter;
        // "media only" excludes organizational bins and keeps every media
        // entry, including not-yet-probed (unknown) and other.
        if (query.mediaOnly)
            filter.mediaOnly = true;
        if (query.unusedOnly)
            filter.unused = true;
        if (query.offlineOnly)
            filter.offline = true;
        return filter;
    }

    function visibleItems() {
        var revision = libraryRevision + smartBinRevision;
        var query = activeQuery();
        if (!library || !library.query)
            return [];
        var result = library.query(query.parentId, query.searchText, query.recursive !== undefined ? !!query.recursive : !!query.projectScope, queryFilter(query)) || [];
        result = arrayValue(result).slice(0);
        result.sort(function (a, b) {
                if (a.kind === "bin" && b.kind !== "bin")
                    return -1;
                if (a.kind !== "bin" && b.kind === "bin")
                    return 1;
                var av;
                var bv;
                if (sortField === "kind") {
                    av = String(a.kind || "");
                    bv = String(b.kind || "");
                } else if (sortField === "duration") {
                    av = Number(a.duration || 0);
                    bv = Number(b.duration || 0);
                } else {
                    av = String(a.name || "").toLowerCase();
                    bv = String(b.name || "").toLowerCase();
                }
                var order = av < bv ? -1 : av > bv ? 1 : String(a.id).localeCompare(String(b.id));
                return sortDescending ? -order : order;
            });
        return result;
    }

    function smartRows() {
        var revision = smartBinRevision;
        var rows = [];
        for (var i = 0; i < smartBins.length; ++i) {
            var smart = smartBins[i];
            if (smart)
                rows.push({
                        "smart": true,
                        "smartId": String(smart.id),
                        "name": String(smart.name || smart.id),
                        "depth": 1
                    });
        }
        return rows;
    }

    function treeRows() {
        var revision = libraryRevision + smartBinRevision;
        var rows = [];
        var seen = {};
        function add(parentId, depth) {
            var children = library && library.children ? arrayValue(library.children(parentId)) : [];
            for (var i = 0; i < children.length; ++i) {
                var child = children[i];
                if (!child || !isBin(child) || seen[child.id])
                    continue;
                seen[child.id] = true;
                var open = !!expandedBins[String(child.id)];
                rows.push({
                        "record": child,
                        "depth": depth,
                        "open": open
                    });
                if (open)
                    add(String(child.id), depth + 1);
            }
        }
        var rootRecord = itemById("root") || {
            "id": "root",
            "parentId": "",
            "kind": "bin",
            "name": "Project"
        };
        rows.push({
                "record": rootRecord,
                "depth": 0,
                "open": !!expandedBins.root
            });
        if (expandedBins.root)
            add("root", 1);
        rows.push({
                "section": true,
                "name": "Smart Bins",
                "depth": 0,
                "open": true
            });
        return rows.concat(smartRows());
    }

    function openSmartBin(id) {
        var smart = smartBinById(id);
        if (!smart || !smart.query)
            return;
        contentPane.forceActiveFocus();
        var query = smart.query;
        currentSmartBinId = String(id);
        currentParentId = String(query.parentId || "root");
        projectScope = !!query.projectScope;
        // The stored recursive flag deliberately stays on the record: activeQuery()
        // reads it (defaulting to projectScope for older records) and visibleItems()
        // passes it to the catalog query, so a scoped recursive search stays recursive.
        searchText = String(query.searchText || "");
        kindFilter = query.kindFilter || "all";
        unusedOnly = !!query.unusedOnly;
        offlineOnly = !!query.offlineOnly;
        clearSelection();
        searchField.text = root.searchText;
        scopeCombo.currentIndex = root.projectScope ? 1 : 0;
        saveState({
                "currentSmartBinId": currentSmartBinId,
                "currentParentId": currentParentId,
                "projectScope": projectScope,
                "searchText": searchText,
                "kindFilter": kindFilter,
                "unusedOnly": unusedOnly,
                "offlineOnly": offlineOnly
            });
    }

    function beginSaveSmartBin() {
        var query = activeQuery();
        smartDialog.mode = "save";
        smartDialog.smartId = "";
        smartDialog.name = "Saved search";
        smartDialog.query = {
            "parentId": String(query.parentId || currentParentId),
            "projectScope": !!query.projectScope,
            "searchText": String(query.searchText || ""),
            "kindFilter": query.kindFilter || "all",
            "mediaOnly": !!query.mediaOnly,
            "unusedOnly": !!query.unusedOnly,
            "offlineOnly": !!query.offlineOnly
        };
        smartNameField.text = smartDialog.name;
        smartDialog.open();
        smartNameField.forceActiveFocus();
        smartNameField.selectAll();
    }

    function beginRenameSmartBin(id) {
        var smart = smartBinById(id);
        if (!smart || smart.builtIn)
            return;
        smartDialog.mode = "rename";
        smartDialog.smartId = String(id);
        smartDialog.name = String(smart.name || "");
        smartDialog.query = smart.query;
        smartNameField.text = smartDialog.name;
        smartDialog.open();
        smartNameField.forceActiveFocus();
        smartNameField.selectAll();
    }

    // Saved searches are persisted catalog bins with a stored query; the panel
    // never keeps a private record for them.
    function finishSmartDialog() {
        var name = String(smartNameField.text || "").trim();
        if (!name)
            return;
        if (smartDialog.mode === "rename") {
            applyCatalog({
                    "type": "renameSmartBin",
                    "id": smartDialog.smartId,
                    "name": name
                }, "Renamed saved search");
            return;
        }
        var query = smartDialog.query;
        if (!applyCatalog({
                "type": "createSmartBin",
                "name": name,
                "parentId": "root",
                "query": query
            }, "Saved search"))
            return;
        // The persisted bin identity is host-assigned; select the record the
        // adapter just published. The query equals the still-active filters.
        var saved = root.library && root.library.smartBins ? root.library.smartBins : [];
        for (var i = 0; i < saved.length; ++i) {
            if (String(saved[i].name) === name) {
                currentSmartBinId = String(saved[i].id);
                saveState({
                        "currentSmartBinId": currentSmartBinId
                    });
                break;
            }
        }
    }

    function deleteSmartBin(id) {
        var smart = smartBinById(id);
        if (!smart || smart.builtIn)
            return;
        if (!applyCatalog({
                "type": "deleteSmartBin",
                "id": String(id)
            }, "Deleted saved search"))
            return;
        if (currentSmartBinId === String(id)) {
            currentSmartBinId = "";
            saveState({
                    "currentSmartBinId": ""
                });
        }
    }

    function setStatus(message, error) {
        if (!error) {
            statusMessage = "";
            statusError = false;
            statusTimer.stop();
            return;
        }
        statusMessage = message ? String(message) : "";
        statusError = true;
        statusTimer.restart();
    }

    function catalogError(fallback) {
        return library && library.error ? String(library.error) : String(fallback || "Media catalog operation failed");
    }

    function applyCatalog(operation, successText) {
        if (!library || !library.apply) {
            setStatus("Media library is unavailable", true);
            return false;
        }
        var ok = !!library.apply(operation);
        if (ok)
            setStatus(successText || "", false);
        else
            setStatus(catalogError(), true);
        return ok;
    }

    function normalizeIds(ids) {
        var result = [];
        var values = arrayValue(ids);
        for (var i = 0; i < values.length; ++i) {
            var id = String(values[i]);
            if (id !== "root" && result.indexOf(id) < 0 && itemById(id))
                result.push(id);
        }
        return result;
    }

    function isSelected(id) {
        return selectedIds.indexOf(String(id)) >= 0;
    }

    function selectOne(id, modifiers) {
        id = String(id);
        var additive = !!(modifiers & Qt.ControlModifier);
        var range = !!(modifiers & Qt.ShiftModifier);
        var next = additive ? selectedIds.slice(0) : [];
        if (range && anchorId) {
            var rows = visibleRecords;
            var a = -1;
            var b = -1;
            for (var i = 0; i < rows.length; ++i) {
                if (String(rows[i].id) === anchorId)
                    a = i;
                if (String(rows[i].id) === id)
                    b = i;
            }
            if (a >= 0 && b >= 0) {
                var from = Math.min(a, b);
                var to = Math.max(a, b);
                next = additive ? next : [];
                for (var n = from; n <= to; ++n)
                    if (next.indexOf(String(rows[n].id)) < 0)
                        next.push(String(rows[n].id));
                selectedIds = normalizeIds(next);
                anchorId = id;
                persistSelection();
                return;
            }
        }
        var at = next.indexOf(id);
        if (additive && at >= 0)
            next.splice(at, 1);
        else if (at < 0)
            next.push(id);
        selectedIds = normalizeIds(next);
        anchorId = id;
        persistSelection();
    }

    function selectAll() {
        selectedIds = normalizeIds(visibleRecords.map(function (item) {
                    return item.id;
                }));
        if (selectedIds.length)
            anchorId = selectedIds[0];
        persistSelection();
    }

    function clearSelection() {
        selectedIds = [];
        anchorId = "";
        renameId = "";
        persistSelection();
    }

    function openBin(id) {
        // Focus the catalog leaf; focusing its scope restores the search editor.
        contentPane.forceActiveFocus();
        var record = itemById(id);
        if (!isBin(record))
            return;
        currentSmartBinId = "";
        currentParentId = String(id);
        projectScope = false;
        clearSelection();
        var next = Object.assign({}, expandedBins);
        next[String(id)] = true;
        expandedBins = next;
        scopeCombo.currentIndex = root.projectScope ? 1 : 0;
        saveState({
                "currentSmartBinId": "",
                "currentParentId": currentParentId,
                "projectScope": false,
                "expandedBins": expandedBins
            });
    }

    function toggleBin(id) {
        id = String(id);
        var next = Object.assign({}, expandedBins);
        next[id] = !expandedBins[id];
        expandedBins = next;
        saveState({
                "expandedBins": expandedBins
            });
    }

    function navigateBreadcrumb(id) {
        openBin(id);
    }

    function beginCreate(parentId, fromSelection) {
        createBinDialog.parentId = parentId || currentParentId;
        createBinDialog.fromSelection = !!fromSelection;
        createBinField.text = "New bin";
        createBinDialog.open();
        createBinField.forceActiveFocus();
        createBinField.selectAll();
    }

    function finishCreate() {
        var name = createBinField.text.trim();
        if (!name)
            return;
        var parent = createBinDialog.parentId || currentParentId;
        var chosen = normalizeIds(selectedIds);
        if (createBinDialog.fromSelection && chosen.length && library && library.newBinFromSelection) {
            if (library.newBinFromSelection(chosen, name, parent)) {
                setStatus("Created bin from selection", false);
                return;
            }
            setStatus(catalogError(), true);
            return;
        }
        if (applyCatalog({
                "type": "createBin",
                "parentId": parent,
                "name": name
            }, "Created bin")) {
            var nextExpanded = Object.assign({}, expandedBins);
            nextExpanded[parent] = true;
            expandedBins = nextExpanded;
            var children = library.children(parent) || [];
            for (var i = children.length - 1; i >= 0; --i) {
                if (children[i].name === name && children[i].kind === "bin") {
                    currentParentId = String(children[i].id);
                    break;
                }
            }
            if (createBinDialog.fromSelection && chosen.length) {
                applyCatalog({
                        "type": "move",
                        "ids": chosen,
                        "parentId": currentParentId
                    }, "Created bin from selection");
            }
            saveState({
                    "currentParentId": currentParentId,
                    "expandedBins": expandedBins
                });
        }
    }

    function startRename(id) {
        var item = itemById(id);
        if (!item || String(id) === "root")
            return;
        renameId = String(id);
        renameDialog.recordId = renameId;
        renameDialogField.text = String(item.name || "");
        renameDialog.open();
        renameDialogField.forceActiveFocus();
        renameDialogField.selectAll();
    }

    function commitRename(id, value) {
        var name = String(value || "").trim();
        renameId = "";
        if (name)
            applyCatalog({
                    "type": "rename",
                    "id": String(id),
                    "name": name
                }, "Renamed item");
    }

    function requestDelete(ids) {
        var chosen = normalizeIds(ids);
        if (!chosen.length)
            return;
        var nonempty = false;
        for (var i = 0; i < chosen.length; ++i) {
            var item = itemById(chosen[i]);
            if (isBin(item) && library.children && library.children(chosen[i]).length > 0)
                nonempty = true;
        }
        if (nonempty) {
            deleteDialog.ids = chosen;
            deleteDialog.open();
            return;
        }
        applyCatalog({
                "type": "remove",
                "ids": chosen,
                "keepContents": false
            }, "Deleted selection");
        clearSelection();
    }

    function deleteSelection() {
        requestDelete(selectedIds);
    }

    function copySelection(cut) {
        var chosen = normalizeIds(selectedIds);
        if (!chosen.length)
            return;
        clipboardIds = chosen;
        clipboardCut = !!cut;
        setStatus(cut ? "Cut " + chosen.length + " item(s)" : "Copied " + chosen.length + " item(s)", false);
    }

    function pasteSelection() {
        if (!clipboardIds.length)
            return;
        if (clipboardCut) {
            if (applyCatalog({
                    "type": "move",
                    "ids": clipboardIds,
                    "parentId": currentParentId
                }, "Moved selection")) {
                selectedIds = normalizeIds(clipboardIds);
                clipboardIds = [];
                clipboardCut = false;
                persistSelection();
            }
        } else {
            if (applyCatalog({
                    "type": "duplicate",
                    "ids": clipboardIds,
                    "parentId": currentParentId
                }, "Duplicated selection"))
                clearSelection();
        }
    }

    function applyMetadata(id, changes) {
        id = String(id || "");
        if (!id || !itemById(id))
            return;
        applyCatalog({
                "type": "metadata",
                "ids": [id],
                "changes": changes
            }, "Updated metadata");
    }

    function recordTags(item) {
        if (!item || !item.tags)
            return "";
        return item.tags.length !== undefined && typeof item.tags !== "string" ? item.tags.join(", ") : String(item.tags);
    }

    function openMetadata(id) {
        var item = itemById(id);
        if (!item)
            return;
        selectedIds = [String(id)];
        persistSelection();
        metadataDialog.recordId = String(id);
        metadataDialogField.text = recordTags(item);
        metadataDescriptionField.text = String(item.description || "");
        metadataLabelCombo.currentIndex = Math.max(0, labelOptions.indexOf(item.color ? String(item.color) : ""));
        metadataDialog.open();
    }

    function selectedSourceId() {
        var item = selectedRecord;
        return item && !isBin(item) && item.sourceId ? String(item.sourceId) : "";
    }

    function openSelected() {
        if (isBin(selectedRecord)) {
            openBin(selectedRecord.id);
            return;
        }
        var sourceId = selectedSourceId();
        if (!sourceId)
            return;
        if (!library || !library.openMediaSource) {
            setStatus("Media viewing is unavailable", true);
            return;
        }
        if (!library.openMediaSource(panelGroup, sourceId))
            setStatus(catalogError("Media source could not be opened"), true);
    }

    // One walk of the visible rows returns both ordered lists the timeline
    // payload needs: stable source keys and their parallel per-occurrence
    // marked ranges. The adapter exposes each entry's authored ranges, so
    // repeated catalog occurrences of one source keep their own range instead
    // of collapsing onto the first entry with that source key. When the
    // adapter does not publish per-entry ranges, an empty list lets
    // requestTimelineInsert resolve its defaults.
    //
    // The panel calls this once per catalog revision (root.selectedPayload) and
    // at gesture time; delegates and menus never re-walk the catalog.
    function selectionPayload(ids) {
        var values = arrayValue(ids);
        var rows = visibleRecords;
        var sourceIds = [];
        var marks = [];
        var explicit = true;
        for (var i = 0; i < rows.length; ++i) {
            var item = rows[i];
            if (isBin(item) || !item.sourceId || values.indexOf(item.id) < 0)
                continue;
            sourceIds.push(String(item.sourceId));
            if (item.marks === undefined) {
                explicit = false;
                continue;
            }
            if (!explicit)
                continue;
            var ranges = arrayValue(item.marks);
            var mark = {
                "sourceId": String(item.sourceId),
                "inFrame": null,
                "outFrame": null
            };
            if (ranges.length > 0 && ranges[0]) {
                if (ranges[0].inFrame !== undefined && ranges[0].inFrame !== null)
                    mark.inFrame = Number(ranges[0].inFrame);
                if (ranges[0].outFrame !== undefined && ranges[0].outFrame !== null)
                    mark.outFrame = Number(ranges[0].outFrame);
            }
            marks.push(mark);
        }
        return {
            "sourceIds": sourceIds,
            "marks": explicit ? marks : []
        };
    }

    // Archived handler surface: kept as thin wrappers over the single walk.
    function sourceIdsForItemIds(ids) {
        return selectionPayload(ids).sourceIds;
    }

    function marksForItemIds(ids) {
        return selectionPayload(ids).marks;
    }

    // One insertion intent per actual insertion gesture. The mode is
    // "insert"/"overwrite"; the playhead is owned by the timeline.
    function requestInsert(mode) {
        var payload = selectionPayload(selectedIds);
        if (!payload.sourceIds.length)
            return;
        if (!library || !library.requestTimelineInsert) {
            setStatus("Timeline insertion is unavailable", true);
            return;
        }
        if (library.requestTimelineInsert(panelGroup, payload.sourceIds, mode, payload.marks))
            setStatus("", false);
        else
            setStatus(catalogError("Timeline insertion failed"), true);
    }

    // Single owner of the record-drag mode vocabulary: overwrite stays
    // menu-only, so every record drag publishes "insert" from here.
    function dragMode() {
        return "insert";
    }

    function dragPayload(itemIds) {
        var ids = normalizeIds(itemIds);
        var payload = selectionPayload(ids);
        return {
            "itemIds": ids,
            "sourceIds": payload.sourceIds,
            "marks": payload.marks,
            "mode": dragMode(),
            "group": panelGroup
        };
    }

    function dragIds(drag) {
        var result = [];
        if (drag && drag.source && drag.source.itemIds)
            result = arrayValue(drag.source.itemIds);
        if (!result.length && drag && drag.mimeData && drag.mimeData.itemIds)
            result = arrayValue(drag.mimeData.itemIds);
        if (!result.length && drag && drag.source && drag.source.itemId)
            result = [drag.source.itemId];
        if (!result.length && drag && drag.mimeData && drag.mimeData.itemId)
            result = [drag.mimeData.itemId];
        return normalizeIds(result);
    }

    function revealOriginalBin(drag) {
        var ids = dragIds(drag);
        if (!ids.length)
            return;
        var item = itemById(ids[0]);
        if (item && item.parentId)
            openBin(item.parentId);
    }

    function handleSmartDrop(smartId, drag) {
        if (drag)
            drag.accepted = false;
        revealOriginalBin(drag);
        setStatus("Smart bins are read-only", true);
    }

    function handleDrop(parentId, drag) {
        var destination = itemById(parentId);
        if (!isBin(destination)) {
            if (drag)
                drag.accepted = false;
            return false;
        }
        var ids = dragIds(drag);
        if (!ids.length) {
            if (drag)
                drag.accepted = false;
            return false;
        }
        var ok = applyCatalog({
                "type": "move",
                "ids": ids,
                "parentId": String(parentId)
            }, "Moved selection");
        if (drag && ok)
            drag.accept(Qt.MoveAction);
        else if (drag)
            drag.accepted = false;
        hoverDropBinId = "";
        pendingHoverBinId = "";
        if (ok) {
            selectedIds = ids;
            persistSelection();
        }
        return ok;
    }

    function beginDropHover(id) {
        id = String(id);
        hoverDropBinId = id;
        pendingHoverBinId = id;
        hoverExpandTimer.restart();
    }

    function endDropHover(id) {
        if (hoverDropBinId === String(id))
            hoverDropBinId = "";
        if (pendingHoverBinId === String(id))
            pendingHoverBinId = "";
        hoverExpandTimer.stop();
    }

    function durationText(item) {
        var duration = Number(item && item.duration || 0);
        if (!isFinite(duration) || duration <= 0)
            return "";
        var fps = itemFrameRate(item);
        var frames = Math.round(duration);
        function pad(value) {
            return String(value).padStart(2, "0");
        }
        return pad(Math.floor(frames / (fps * 3600))) + ":" + pad(Math.floor(frames / (fps * 60)) % 60) + ":" + pad(Math.floor(frames / fps) % 60) + ":" + pad(frames % fps);
    }

    // Frame rate for the duration text. Production reads the probed source
    // rate and keeps the same 24 fps default ViewerController uses for an
    // unknown rate rather than inventing a timebase.
    function itemFrameRate(item) {
        var rate = Number(item && item.runtime ? item.runtime.frameRate : 0);
        return isFinite(rate) && rate > 0 ? rate : 24;
    }

    function kindGlyph(item) {
        if (!item)
            return "?";
        if (item.kind === "bin")
            return "▱";
        if (item.kind === "video")
            return "V";
        if (item.kind === "audio")
            return "A";
        if (item.kind === "still")
            return "S";
        // Unknown and other media keep the neutral glyph; never claim a kind
        // the catalog has not established.
        if (item.kind === "unknown" || item.kind === "other")
            return "•";
        return "•";
    }

    function kindColor(item) {
        if (item && item.color)
            return item.color;
        if (item && item.kind === "bin")
            return theme.muted;
        return theme.accent;
    }

    // --- Probe/runtime presentation (approved production additions) -------

    // Refresh key for every runtime/probe binding below: the adapter publishes
    // probe results and thumbnails through its revision.
    function probeStateFor(id) {
        var revision = libraryRevision;
        if (!library || !library.probeState)
            return null;
        var state = library.probeState(String(id));
        return state && state.id ? state : null;
    }

    // Exactly one token beside a media name: a running probe, a missing
    // source, or a source the decoders cannot interpret. The full diagnostic
    // lives in the metadata dialog and the status line. The record already
    // carries the displayed availability and probe status; probeState is still
    // consulted because it is the adapter's lazy visible-request hook and the
    // only source of the in-flight (Pending) flag.
    function probeBadge(record) {
        if (!record || isBin(record))
            return "";
        var state = probeStateFor(record.id);
        if (state && state.pending)
            return "Pending";
        if (state ? !!state.offline : !!record.offline)
            return "Offline";
        if (state && state.hasResult && String(state.error || "").length > 0)
            return "Unsupported";
        if (String(record.probeStatus || "") === "failed")
            return "Unsupported";
        return "";
    }

    function badgeColorFor(badge) {
        if (badge === "Unsupported")
            return theme ? theme.errorText : "#f0d0d0";
        return theme ? theme.muted : "#979ea8";
    }

    // Real provider URL in the archived preview slot; empty means no preview is
    // available and the slot keeps the archived kind glyph (no stand-in
    // footage is drawn). The record materializes this from the same adapter
    // call per revision, so the delegate needs no per-row adapter query.
    function thumbnailFor(record) {
        if (!record || isBin(record) || record.offline)
            return "";
        return record.thumbnailUrl ? String(record.thumbnailUrl) : "";
    }

    function probeDiagnostic(id) {
        var revision = libraryRevision;
        var item = itemById(id);
        if (!item || isBin(item))
            return "";
        var state = library && library.probeState ? library.probeState(String(id)) : null;
        var lines = [];
        var status = String(item.probeStatus || "unknown");
        if (state && state.pending)
            status = "pending";
        lines.push("Probe: " + status);
        var probe = item.probe;
        if (probe) {
            if (probe.width && probe.height)
                lines.push("Size: " + probe.width + " × " + probe.height);
            if (Number(probe.duration) > 0)
                lines.push("Duration: " + Number(probe.duration) + " frames");
            if (probe.codec)
                lines.push("Codec: " + probe.codec);
            var color = [];
            if (probe.colorPrimaries)
                color.push(String(probe.colorPrimaries));
            if (probe.colorTransfer)
                color.push(String(probe.colorTransfer));
            if (probe.colorMatrix)
                color.push(String(probe.colorMatrix));
            if (color.length)
                lines.push("Color: " + color.join(" / "));
            if (probe.provenance)
                lines.push("Provenance: " + probe.provenance);
        }
        var runtime = state && state.runtime ? state.runtime : item.runtime;
        if (runtime) {
            var decode = [];
            if (runtime.pixelFormat)
                decode.push(String(runtime.pixelFormat));
            if (Number(runtime.bitDepth) > 0)
                decode.push(Number(runtime.bitDepth) + "-bit");
            if (Number(runtime.frameRate) > 0)
                decode.push(Number(runtime.frameRate) + " fps");
            if (runtime.colorRange)
                decode.push(String(runtime.colorRange));
            if (decode.length)
                lines.push("Decode: " + decode.join(", "));
            if (runtime.fallbackReason)
                lines.push("Fallback: " + runtime.fallbackReason);
        }
        if (state && state.offline) {
            // Authored flag vs runtime fact: the runtime never rewrites the
            // user's persisted offline marking.
            lines.push(!!item.authoredOffline ? "Marked offline by the user" : "Source file is missing");
        }
        if (state && state.error)
            lines.push("Diagnostic: " + state.error);
        return lines.join("\n");
    }

    function probeCanApply(id) {
        var state = probeStateFor(id);
        return !!state && !!state.hasResult && !state.pending;
    }

    function applyProbeMetadata(id) {
        if (!library || !library.applyProbe) {
            setStatus("Probe metadata commit is unavailable", true);
            return;
        }
        if (library.applyProbe(String(id)))
            setStatus("Applied probe metadata", false);
        else
            setStatus(catalogError("Probe metadata could not be applied"), true);
    }

    // --- Import / relink (archived placements, native chooser) --------------

    function importDestinationFor(itemId) {
        var item = itemById(itemId);
        if (isBin(item))
            return String(item.id);
        return root.currentParentId;
    }

    function beginImport(parentId) {
        importDialog.parentId = String(parentId || root.currentParentId);
        importPathField.text = "";
        importStartFrameField.text = "";
        importDialog.open();
        importPathField.forceActiveFocus();
    }

    // The start frame maps onto the imported source reference's frameOffset.
    // An empty field means 0; anything but a whole number is rejected rather
    // than guessed.
    function importStartFrame() {
        var text = String(importStartFrameField.text || "").trim();
        if (!text.length)
            return 0;
        var value = Number(text);
        return isFinite(value) && Math.floor(value) === value ? value : null;
    }

    function importPaths(paths, parentId, frameOffset) {
        if (!library || !library.importPaths) {
            setStatus("Media import is unavailable", true);
            return false;
        }
        var offset = frameOffset === undefined || frameOffset === null ? 0 : Number(frameOffset);
        if (!isFinite(offset))
            offset = 0;
        if (library.importPaths(paths, String(parentId), offset)) {
            setStatus("Imported media", false);
            return true;
        }
        setStatus(catalogError("Media import failed"), true);
        return false;
    }

    function commitImport() {
        var path = String(importPathField.text || "").trim();
        if (!path) {
            setStatus("Enter a media path or use the file chooser", true);
            return;
        }
        var offset = importStartFrame();
        if (offset === null) {
            setStatus("Sequence start frame must be a whole number", true);
            return;
        }
        if (importPaths([path], importDialog.parentId || root.currentParentId, offset))
            importDialog.close();
    }

    function chooseImportFiles() {
        if (!library || !library.chooseImportPaths) {
            setStatus("A native file chooser is unavailable; enter the path directly", true);
            return;
        }
        library.chooseImportPaths(String(importDialog.parentId || root.currentParentId));
    }

    function beginRelink(itemId) {
        var item = itemById(itemId);
        if (!item || isBin(item))
            return;
        relinkDialog.recordId = String(itemId);
        relinkPathField.text = "";
        relinkDialog.open();
        relinkPathField.forceActiveFocus();
    }

    // Read-only display of the mapping relink preserves; relink replaces the
    // path only and never resets the start frame.
    function relinkStartFrameText(id) {
        var revision = libraryRevision;
        var item = itemById(id);
        if (!item || isBin(item))
            return "";
        if (item.frameOffset === undefined || item.frameOffset === null)
            return "";
        return "Current start frame (preserved): " + Number(item.frameOffset);
    }

    function relink(id, path) {
        if (!library || !library.relink) {
            setStatus("Media relink is unavailable", true);
            return false;
        }
        if (library.relink(String(id), path)) {
            setStatus("Relinked media source", false);
            return true;
        }
        setStatus(catalogError("Media relink failed"), true);
        return false;
    }

    function commitRelink() {
        var path = String(relinkPathField.text || "").trim();
        if (!path) {
            setStatus("Enter a media path or use the file chooser", true);
            return;
        }
        if (relink(relinkDialog.recordId, path))
            relinkDialog.close();
    }

    function chooseRelinkFile() {
        if (!library || !library.chooseRelinkPath) {
            setStatus("A native file chooser is unavailable; enter the path directly", true);
            return;
        }
        library.chooseRelinkPath(String(relinkDialog.recordId));
    }

    function reprobe(id) {
        if (!library || !library.reprobe) {
            setStatus("Media probe is unavailable", true);
            return;
        }
        if (!library.reprobe(String(id)))
            setStatus(catalogError("Media probe failed"), true);
    }

    // --- Panel state (workspace panelState only) ---------------------------

    // The complete set of state keys this panel owns, read from the live
    // properties. A write publishes this snapshot, so a field changed just
    // before the write can never be republished at its previous value and the
    // workspace echo can only confirm the state the panel already applied.
    function currentPanelState() {
        return {
            "viewMode": viewMode,
            "sortField": sortField,
            "sortDescending": sortDescending,
            "treeWidth": treeWidth,
            "treeCollapsed": treeCollapsed,
            "currentParentId": currentParentId,
            "currentSmartBinId": currentSmartBinId,
            "projectScope": projectScope,
            "searchText": searchText,
            "kindFilter": kindFilter,
            "unusedOnly": unusedOnly,
            "offlineOnly": offlineOnly,
            "expandedBins": expandedBins,
            "selectedIds": selectedIds,
            "anchorId": anchorId
        };
    }

    function saveState(changes) {
        if (!stateReady || !workspace || !panelId || !workspace.setPanelState)
            return;
        // One atomic publication of the whole owned state. Keys this panel does
        // not own (for example a viewerRole saved before the panel was
        // retyped) survive; every owned key is taken from the live snapshot,
        // never from a lagging cached or observed value.
        var merged = {};
        var cached = stateCache || {};
        for (var key in cached)
            merged[key] = cached[key];
        var observed = panelState || {};
        for (var key in observed)
            merged[key] = observed[key];
        var live = currentPanelState();
        for (var key in live)
            merged[key] = live[key];
        for (var key in changes)
            merged[key] = changes[key];
        stateCache = merged;
        workspace.setPanelState(panelId, merged);
    }

    function persistSelection() {
        var saved = arrayValue(stateCache.selectedIds).map(String).join(",");
        var current = selectedIds.map(String).join(",");
        if (saved === current && String(stateCache.anchorId || "") === anchorId)
            return;
        saveState({
                "selectedIds": selectedIds,
                "anchorId": anchorId
            });
    }

    function persistFilters() {
        if (String(stateCache.searchText || "") === searchText && String(stateCache.kindFilter || "") === kindFilter && !!stateCache.unusedOnly === unusedOnly && !!stateCache.offlineOnly === offlineOnly && String(stateCache.sortField || "") === sortField && !!stateCache.sortDescending === sortDescending)
            return;
        saveState({
                "searchText": searchText,
                "kindFilter": kindFilter,
                "unusedOnly": unusedOnly,
                "offlineOnly": offlineOnly,
                "sortField": sortField,
                "sortDescending": sortDescending
            });
    }

    function restoreState() {
        var state = panelState || {};
        stateCache = state;
        if (state.viewMode === "list" || state.viewMode === "grid")
            viewMode = state.viewMode;
        if (state.sortField === "name" || state.sortField === "kind" || state.sortField === "duration")
            sortField = state.sortField;
        if (state.sortDescending !== undefined)
            sortDescending = !!state.sortDescending;
        if (state.treeWidth !== undefined)
            treeWidth = Math.max(80, Math.min(320, Number(state.treeWidth) || 184));
        if (state.treeCollapsed !== undefined)
            treeCollapsed = !!state.treeCollapsed;
        if (state.kindFilter !== undefined)
            kindFilter = String(state.kindFilter);
        if (state.unusedOnly !== undefined)
            unusedOnly = !!state.unusedOnly;
        if (state.offlineOnly !== undefined)
            offlineOnly = !!state.offlineOnly;
        if (state.searchText !== undefined)
            searchText = String(state.searchText);
        if (state.currentParentId !== undefined)
            currentParentId = String(state.currentParentId);
        if (state.currentSmartBinId !== undefined)
            currentSmartBinId = String(state.currentSmartBinId);
        if (state.projectScope !== undefined)
            projectScope = !!state.projectScope;
        if (state.expandedBins && typeof state.expandedBins === "object")
            expandedBins = state.expandedBins;
        if (state.selectedIds)
            selectedIds = normalizeIds(state.selectedIds);
        if (state.anchorId !== undefined)
            anchorId = String(state.anchorId);
        // A restored navigation target may no longer exist (bin removed, saved
        // search deleted): fall back to the project root instead of showing an
        // empty unreachable bin.
        if (!projectScope && !itemById(currentParentId))
            currentParentId = "root";
        if (currentSmartBinId && !smartBinById(currentSmartBinId))
            currentSmartBinId = "";
        searchField.text = searchText;
        scopeCombo.currentIndex = projectScope ? 1 : 0;
    }

    // --- Header tools (archived placement) ---------------------------------

    Component {
        id: mediaHeaderTools
        Row {
            spacing: 2
            CompactButton {
                id: viewButton
                objectName: "mediaViewToggleButton"
                theme: root.theme
                glyph: root.viewMode === "list" ? "▦" : "☷"
                activeMode: true
                ToolTip.text: root.viewMode === "list" ? "Switch to grid view" : "Switch to list view"
                Accessible.name: ToolTip.text
                onClicked: {
                    root.viewMode = root.viewMode === "list" ? "grid" : "list";
                    root.saveState({
                            "viewMode": root.viewMode
                        });
                }
            }
            CompactButton {
                objectName: "mediaTreeToggleButton"
                theme: root.theme
                glyph: "▤"
                activeMode: !root.treeCollapsed
                ToolTip.text: root.treeCollapsed ? "Show bins" : "Hide bins"
                Accessible.name: ToolTip.text
                onClicked: {
                    root.treeCollapsed = !root.treeCollapsed;
                    root.saveState({
                            "treeCollapsed": root.treeCollapsed
                        });
                }
            }
            CompactButton {
                objectName: "mediaAddBinButton"
                theme: root.theme
                glyph: "+"
                ToolTip.text: "Create bin"
                Accessible.name: "Create bin"
                onClicked: root.beginCreate(root.currentParentId, false)
            }
            CompactButton {
                id: filterButton
                objectName: "mediaFilterButton"
                theme: root.theme
                glyph: "≡"
                ToolTip.text: "Filter or save search"
                Accessible.name: "Filter or save search"
                onClicked: filterMenu.open()
            }
        }
    }

    // Shared ChromeButton with the archived compact glyph geometry.
    component CompactButton: ChromeButton {
        id: compactButton
        property string glyph: ""
        property bool activeMode: false
        implicitWidth: 24
        implicitHeight: 24
        width: 24
        height: 24
        padding: 0
        contentItem: Text {
            text: compactButton.glyph
            color: compactButton.activeMode ? (root.theme ? root.theme.accent : "#3485f6") : compactButton.enabled ? (root.theme ? root.theme.muted : "#979ea8") : (root.theme ? root.theme.disabled : "#5f6670")
            font.pixelSize: 15
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: root.theme ? root.theme.smallRadius : 4
            color: compactButton.activeMode ? Qt.rgba((root.theme ? root.theme.accent : "#3485f6").r, (root.theme ? root.theme.accent : "#3485f6").g, (root.theme ? root.theme.accent : "#3485f6").b, 0.16) : compactButton.hovered ? (root.theme ? root.theme.hover : "#343940") : "transparent"
            border.width: compactButton.activeFocus ? 1 : 0
            border.color: root.theme ? root.theme.accent : "#3485f6"
        }
        ToolTip.visible: hovered
        ToolTip.delay: 450
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 5

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            TextField {
                id: searchField
                objectName: "mediaSearchField"
                Layout.fillWidth: true
                implicitHeight: 26
                color: root.theme ? root.theme.text : "#dce0e6"
                placeholderTextColor: root.theme ? root.theme.muted : "#979ea8"
                placeholderText: "Search media"
                text: root.searchText
                readOnly: !!root.currentSmartBinId
                selectByMouse: true
                background: Rectangle {
                    color: root.theme ? root.theme.field : "#24272c"
                    border.color: searchField.activeFocus ? (root.theme ? root.theme.accent : "#3485f6") : (root.theme ? root.theme.border : "#30343a")
                    radius: root.theme ? root.theme.smallRadius : 4
                }
                onActiveFocusChanged: if (activeFocus && root.currentSmartBinId)
                    root.leaveSmartBin()
                onTextChanged: if (!root.currentSmartBinId)
                    root.searchText = text
                onEditingFinished: root.persistFilters()
            }
            StudioComboBox {
                id: scopeCombo
                objectName: "mediaScopeCombo"
                theme: root.theme
                implicitWidth: 110
                implicitHeight: 26
                model: ["This Bin", "Project"]
                currentIndex: root.projectScope ? 1 : 0
                enabled: !root.currentSmartBinId
                onActivated: {
                    root.leaveSmartBin();
                    root.projectScope = currentIndex === 1;
                    root.saveState({
                            "projectScope": root.projectScope,
                            "currentSmartBinId": ""
                        });
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            // Empty smart-view breadcrumb layouts must not absorb spare height.
            // Non-empty paths retain their natural height.
            Layout.fillHeight: false
            spacing: 4
            Text {
                objectName: "mediaBreadcrumbs"
                text: ""
                visible: false
            }
            Repeater {
                model: root.currentPath()
                delegate: Item {
                    required property var modelData
                    implicitWidth: breadcrumbText.implicitWidth + 14
                    implicitHeight: 24
                    Button {
                        id: breadcrumbButton
                        anchors.fill: parent
                        objectName: "mediaBreadcrumb_" + String(modelData.id)
                        text: String(modelData.name || modelData.id)
                        flat: true
                        padding: 4
                        onClicked: root.navigateBreadcrumb(modelData.id)
                        contentItem: Text {
                            text: breadcrumbButton.text
                            color: root.theme ? root.theme.text : "#dce0e6"
                            font.pixelSize: root.theme ? root.theme.fontSize : 11
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Text {
                        id: breadcrumbText
                        visible: false
                        text: String(modelData.name || modelData.id)
                    }
                    DropArea {
                        anchors.fill: parent
                        objectName: "mediaBreadcrumbDrop_" + String(modelData.id)
                        keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                        onEntered: root.beginDropHover(modelData.id)
                        onExited: root.endDropHover(modelData.id)
                        onDropped: function (drop) {
                            root.handleDrop(modelData.id, drop);
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 5

            Item {
                id: treePane
                Layout.preferredWidth: root.treeCollapsed ? 0 : Math.min(root.treeWidth, Math.max(120, root.width * 0.4))
                Layout.minimumWidth: root.treeCollapsed ? 0 : Math.min(120, Math.max(80, root.width * 0.4))
                Layout.fillHeight: true
                clip: true
                visible: !root.treeCollapsed
                Rectangle {
                    anchors.fill: parent
                    color: root.theme ? root.theme.field : "#24272c"
                    radius: root.theme ? root.theme.smallRadius : 4
                    border.color: root.theme ? root.theme.border : "#30343a"
                    border.width: 1
                }
                ListView {
                    id: treeView
                    objectName: "mediaTreeView"
                    anchors.fill: parent
                    anchors.margins: 2
                    clip: true
                    model: root.treeRows()
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Item {
                        id: treeRow
                        required property var modelData
                        property bool sectionRow: !!modelData.section
                        property bool smartRow: !!modelData.smart
                        property var record: modelData.record || ({
                                "id": "",
                                "kind": "",
                                "name": modelData.name || ""
                            })
                        property int depth: Number(modelData.depth || 0)
                        property bool expanded: !!modelData.open
                        width: treeView.width
                        height: sectionRow ? 23 : 27
                        objectName: sectionRow ? "mediaSmartBinsHeader" : smartRow ? "mediaSmartBin_" + String(modelData.smartId) : "mediaTreeItem_" + String(record.id)
                        Rectangle {
                            anchors.fill: parent
                            radius: root.theme ? root.theme.smallRadius : 4
                            color: sectionRow ? (root.theme ? root.theme.header : "#212428") : smartRow && root.hoverDropBinId === String(modelData.smartId) ? Qt.rgba((root.theme ? root.theme.accent : "#3485f6").r, (root.theme ? root.theme.accent : "#3485f6").g, (root.theme ? root.theme.accent : "#3485f6").b, 0.24) : smartRow && root.currentSmartBinId === String(modelData.smartId) ? (root.theme ? root.theme.nodeSelected : "#293e57") : !smartRow && root.hoverDropBinId === String(record.id) ? Qt.rgba((root.theme ? root.theme.accent : "#3485f6").r, (root.theme ? root.theme.accent : "#3485f6").g, (root.theme ? root.theme.accent : "#3485f6").b, 0.24) : !smartRow && !root.currentSmartBinId && root.currentParentId === String(record.id) ? (root.theme ? root.theme.nodeSelected : "#293e57") : !smartRow && root.isSelected(record.id) ? (root.theme ? root.theme.hover : "#343940") : "transparent"
                            border.color: !sectionRow && ((smartRow && root.hoverDropBinId === String(modelData.smartId)) || (!smartRow && root.hoverDropBinId === String(record.id))) ? (root.theme ? root.theme.accent : "#3485f6") : "transparent"
                            border.width: 1
                        }
                        Text {
                            visible: !sectionRow && !smartRow
                            x: 5 + treeRow.depth * 13
                            width: 12
                            height: parent.height
                            text: root.library && root.library.children && root.library.children(record.id).length > 0 ? (treeRow.expanded ? "⌄" : "›") : ""
                            color: root.theme ? root.theme.muted : "#979ea8"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 13
                        }
                        Text {
                            x: sectionRow ? 8 : smartRow ? 20 : 20 + treeRow.depth * 13
                            width: Math.max(30, parent.width - x - 5)
                            height: parent.height
                            text: sectionRow ? String(modelData.name) : smartRow ? "⌾ " + String(modelData.name) : String(record.name || record.id)
                            color: sectionRow ? (root.theme ? root.theme.muted : "#979ea8") : (root.theme ? root.theme.text : "#dce0e6")
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: sectionRow ? (root.theme ? root.theme.fontSize - 1 : 10) : (root.theme ? root.theme.fontSize : 11)
                            font.bold: sectionRow
                        }
                        MouseArea {
                            anchors.fill: parent
                            z: 1
                            objectName: sectionRow ? "mediaSmartBinsHeaderInput" : smartRow ? "mediaSmartBinInput_" + String(modelData.smartId) : "mediaTreeInput_" + String(record.id)
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function (mouse) {
                                if (sectionRow)
                                    return;
                                if (smartRow) {
                                    if (mouse.button === Qt.RightButton) {
                                        smartMenu.smartId = String(modelData.smartId);
                                        smartMenu.x = mouse.x;
                                        smartMenu.y = mouse.y;
                                        smartMenu.open();
                                    } else {
                                        root.openSmartBin(modelData.smartId);
                                    }
                                    return;
                                }
                                root.selectOne(record.id, mouse.modifiers);
                                if (mouse.button === Qt.RightButton) {
                                    treeMenu.x = mouse.x;
                                    treeMenu.y = mouse.y;
                                    treeMenu.recordId = String(record.id);
                                    treeMenu.open();
                                } else if (mouse.x < 25 + treeRow.depth * 13 && root.library.children(record.id).length > 0) {
                                    root.toggleBin(record.id);
                                } else {
                                    root.openBin(record.id);
                                }
                            }
                        }
                        DropArea {
                            visible: !sectionRow
                            anchors.fill: parent
                            z: 2
                            objectName: smartRow ? "mediaSmartBinDrop_" + String(modelData.smartId) : "mediaTreeDrop_" + String(record.id)
                            keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                            onEntered: if (smartRow)
                                root.hoverDropBinId = String(modelData.smartId)
                            else
                                root.beginDropHover(record.id)
                            onExited: if (smartRow)
                                root.hoverDropBinId = ""
                            else
                                root.endDropHover(record.id)
                            onDropped: function (drop) {
                                if (smartRow)
                                    root.handleSmartDrop(modelData.smartId, drop);
                                else
                                    root.handleDrop(record.id, drop);
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: treeSplitter
                visible: !root.treeCollapsed
                Layout.preferredWidth: 4
                Layout.fillHeight: true
                color: splitterMouse.containsMouse || root.treeDragging ? (root.theme ? root.theme.accent : "#3485f6") : (root.theme ? root.theme.border : "#30343a")
                MouseArea {
                    id: splitterMouse
                    property real pressX: 0
                    property real pressWidth: 0
                    anchors.fill: parent
                    cursorShape: Qt.SizeHorCursor
                    hoverEnabled: true
                    onPressed: {
                        pressX = mouse.x;
                        pressWidth = root.treeWidth;
                        root.treeDragging = true;
                    }
                    onPositionChanged: function (mouse) {
                        if (pressed)
                            root.treeWidth = Math.max(80, Math.min(320, root.width * 0.4, pressWidth + mouse.x - pressX));
                    }
                    onReleased: {
                        root.treeDragging = false;
                        root.saveState({
                                "treeWidth": root.treeWidth
                            });
                    }
                    onCanceled: root.treeDragging = false
                }
            }

            Item {
                id: contentPane
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                Rectangle {
                    anchors.fill: parent
                    color: root.theme ? root.theme.panel : "#1e2023"
                    radius: root.theme ? root.theme.smallRadius : 4
                    border.color: root.theme ? root.theme.border : "#30343a"
                    border.width: 1
                }
                Item {
                    id: listHeader
                    objectName: "mediaListHeader"
                    visible: root.viewMode === "list" && contentPane.width >= 420
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: visible ? 20 : 0
                    Text {
                        x: 37
                        width: Math.max(40, parent.width - 190)
                        height: parent.height
                        text: "Name"
                        color: root.theme ? root.theme.muted : "#979ea8"
                        font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 2
                        verticalAlignment: Text.AlignVCenter
                    }
                    Text {
                        x: parent.width - 180
                        width: 94
                        height: parent.height
                        text: "Duration"
                        color: root.theme ? root.theme.muted : "#979ea8"
                        font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 2
                        verticalAlignment: Text.AlignVCenter
                    }
                    Text {
                        x: parent.width - 84
                        width: 72
                        height: parent.height
                        text: "Type"
                        color: root.theme ? root.theme.muted : "#979ea8"
                        font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 2
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                ListView {
                    id: listView
                    z: 1
                    objectName: "mediaListView"
                    visible: root.viewMode === "list"
                    anchors.top: listHeader.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 2
                    clip: true
                    focus: true
                    model: root.visibleRecords
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Item {
                        id: listRow
                        required property var modelData
                        property var record: modelData
                        property string itemId: String(record.id)
                        property string sourceId: record.sourceId ? String(record.sourceId) : ""
                        property string sourceGroup: root.panelGroup
                        property var itemIds: root.selectedIds
                        property var sourceIds: root.selectedSourceIds
                        // Parallel to sourceIds (hoisted per revision); the
                        // timeline drop reads these through drag.source so
                        // repeated occurrences keep their own marked range.
                        property var marks: root.selectedMarks
                        // Same mode the drag proxy publishes; overwrite stays
                        // menu-only, so no consumer hardcodes the vocabulary.
                        property string mode: root.dragMode()
                        property bool selected: root.isSelected(itemId)
                        property string badge: root.probeBadge(record)
                        property color badgeColor: root.badgeColorFor(badge)
                        property string thumbnailUrl: root.thumbnailFor(record)
                        // Measured status box: the name column reserves the
                        // badge's natural width plus the shared 6px gap. The
                        // measurement is independent of the badge's rendered
                        // width and of effective visibility (reading
                        // badge.visible would read the parent's effective
                        // visibility and latch at 0 while a view is hidden).
                        property real nameBand: Math.max(50, listRow.width - (listRow.width >= 420 ? 225 : 42))
                        property real statusWidth: listRow.badge.length > 0 ? Math.ceil(listBadgeMetrics.advanceWidth) : 0
                        property real nameWidth: Math.max(24, nameBand - (statusWidth > 0 ? statusWidth + 6 : 0))
                        // The badge keeps the band's right edge; in a band too
                        // small even for the measured text it shrinks with
                        // elision rather than touching the name.
                        property real statusBox: listRow.badge.length > 0 ? Math.min(statusWidth, Math.max(24, nameBand - nameWidth - 6)) : 0
                        width: listView.width
                        height: 34
                        objectName: "mediaItem_" + itemId
                        // Independent measurement of the status token.
                        TextMetrics {
                            id: listBadgeMetrics
                            font: listBadge.font
                            text: listRow.badge
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: root.theme ? root.theme.smallRadius : 4
                            color: listRow.selected ? (root.theme ? root.theme.nodeSelected : "#293e57") : rowMouse.containsMouse ? (root.theme ? root.theme.hover : "#343940") : "transparent"
                            border.color: listRow.selected ? (root.theme ? root.theme.accent : "#3485f6") : "transparent"
                            border.width: 1
                        }
                        // Archived preview slot: the real provider thumbnail when
                        // one exists, otherwise the archived kind glyph.
                        Item {
                            id: listThumbSlot
                            x: 7
                            y: 5
                            width: 20
                            height: 22
                            Image {
                                anchors.fill: parent
                                visible: listRow.thumbnailUrl.length > 0
                                source: listRow.thumbnailUrl
                                fillMode: Image.PreserveAspectFit
                                sourceSize.width: 60
                                sourceSize.height: 66
                                asynchronous: true
                            }
                            Text {
                                anchors.fill: parent
                                visible: listRow.thumbnailUrl.length === 0
                                text: root.kindGlyph(record)
                                color: root.kindColor(record)
                                font.bold: true
                                font.pixelSize: 14
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Text {
                            x: 33
                            y: 0
                            width: listRow.nameWidth
                            height: root.projectScope ? 19 : parent.height
                            text: String(record.name || itemId)
                            color: record.offline ? (root.theme ? root.theme.muted : "#979ea8") : (root.theme ? root.theme.text : "#dce0e6")
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: root.theme ? root.theme.fontSize : 11
                        }
                        Text {
                            visible: root.projectScope
                            x: 33
                            y: 19
                            width: listRow.nameWidth
                            height: 13
                            text: root.library.path(record.parentId).map(function (bin) {
                                    return bin.name;
                                }).join(" / ")
                            color: root.theme ? root.theme.muted : "#979ea8"
                            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 2
                            elide: Text.ElideRight
                        }
                        Text {
                            id: listBadge
                            visible: listRow.badge.length > 0
                            x: 33 + listRow.nameBand - listRow.statusBox
                            y: 0
                            width: listRow.statusBox
                            height: root.projectScope ? 19 : parent.height
                            text: listRow.badge
                            color: listRow.badgeColor
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideLeft
                            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
                        }
                        Text {
                            visible: listRow.width >= 420
                            x: parent.width - 180
                            y: 0
                            width: 94
                            height: parent.height
                            text: root.durationText(record) || "—"
                            color: root.theme ? root.theme.muted : "#979ea8"
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
                        }
                        Text {
                            visible: listRow.width >= 420
                            x: parent.width - 84
                            y: 0
                            width: 72
                            height: parent.height
                            text: String(record.kind || "—")
                            color: root.theme ? root.theme.muted : "#979ea8"
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            objectName: "mediaInput_" + itemId
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            hoverEnabled: true
                            preventStealing: true
                            drag.target: dragProxy
                            onPressed: function (mouse) {
                                contentPane.forceActiveFocus();
                                var preserveSelection = root.isSelected(itemId) && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier));
                                if (!preserveSelection)
                                    root.selectOne(itemId, mouse.modifiers);
                                if (!root.isSelected(itemId))
                                    return;
                                var p = rowMouse.mapToItem(root.dragOverlay, mouse.x, mouse.y);
                                dragProxy.x = p.x;
                                dragProxy.y = p.y;
                                dragProxy.itemId = itemId;
                                dragProxy.itemIds = root.selectedIds.slice(0);
                                dragProxy.sourceId = record.sourceId ? String(record.sourceId) : "";
                                var payload = root.dragPayload(dragProxy.itemIds);
                                dragProxy.sourceIds = payload.sourceIds;
                                dragProxy.marks = payload.marks;
                                dragProxy.mode = payload.mode;
                                dragProxy.group = payload.group;
                            }
                            onPositionChanged: function (mouse) {
                                if (pressed) {
                                    var p = rowMouse.mapToItem(root.dragOverlay, mouse.x, mouse.y);
                                    dragProxy.x = p.x;
                                    dragProxy.y = p.y;
                                }
                            }
                            onReleased: function () {
                                if (drag.active)
                                    dragProxy.Drag.drop();
                            }
                            onClicked: function (mouse) {
                                if (mouse.button === Qt.RightButton) {
                                    itemMenu.recordId = itemId;
                                    itemMenu.x = mouse.x;
                                    itemMenu.y = mouse.y;
                                    itemMenu.open();
                                }
                            }
                            onDoubleClicked: root.openSelected()
                        }
                        DropArea {
                            visible: root.isBin(record)
                            anchors.fill: parent
                            z: 3
                            objectName: "mediaListBinDrop_" + itemId
                            keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                            onEntered: root.beginDropHover(itemId)
                            onExited: root.endDropHover(itemId)
                            onDropped: function (drop) {
                                root.handleDrop(itemId, drop);
                            }
                        }
                        Menu {
                            id: itemMenu
                            property string recordId: ""
                            objectName: "mediaItemMenu_" + itemId
                            MenuItem {
                                text: "Open in viewer"
                                enabled: !!record && !root.isBin(record)
                                onTriggered: {
                                    root.selectedIds = [itemMenu.recordId];
                                    root.persistSelection();
                                    root.openSelected();
                                }
                            }
                            MenuItem {
                                text: "Insert at playhead"
                                enabled: root.selectedSourceIds.length > 0
                                onTriggered: root.requestInsert("insert")
                            }
                            MenuItem {
                                text: "Overwrite at playhead"
                                enabled: root.selectedSourceIds.length > 0
                                onTriggered: root.requestInsert("overwrite")
                            }
                            MenuItem {
                                text: "Metadata…"
                                enabled: !!record
                                onTriggered: root.openMetadata(itemMenu.recordId)
                            }
                            MenuItem {
                                text: "Rename"
                                enabled: itemMenu.recordId !== "root"
                                onTriggered: root.startRename(itemMenu.recordId)
                            }
                            MenuItem {
                                text: "New bin from selection"
                                onTriggered: root.beginCreate(root.currentParentId, true)
                            }
                            MenuSeparator {
                            }
                            MenuItem {
                                text: "Copy"
                                onTriggered: root.copySelection(false)
                            }
                            MenuItem {
                                text: "Cut"
                                onTriggered: root.copySelection(true)
                            }
                            MenuItem {
                                text: "Delete"
                                onTriggered: root.requestDelete([itemMenu.recordId])
                            }
                            MenuSeparator {
                            }
                            MenuItem {
                                objectName: "mediaImportItem_" + itemId
                                text: "Import media…"
                                onTriggered: root.beginImport(root.importDestinationFor(itemMenu.recordId))
                            }
                            MenuItem {
                                objectName: "mediaRelinkItem_" + itemId
                                text: "Relink media…"
                                enabled: !!record && !root.isBin(record)
                                onTriggered: root.beginRelink(itemMenu.recordId)
                            }
                            MenuItem {
                                objectName: "mediaReprobeItem_" + itemId
                                text: "Reprobe media"
                                enabled: !!record && !root.isBin(record)
                                onTriggered: root.reprobe(itemMenu.recordId)
                            }
                            MenuItem {
                                text: "Create proxy…"
                                enabled: false
                            }
                        }
                        Item {
                            id: dragProxy
                            property string itemId: ""
                            property var itemIds: []
                            property string sourceId: ""
                            property var sourceIds: []
                            property var marks: []
                            property string mode: "insert"
                            property string group: ""
                            parent: root.dragOverlay
                            width: 2
                            height: 2
                            Drag.active: rowMouse.drag.active
                            Drag.source: listRow
                            Drag.keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                            Drag.supportedActions: Qt.CopyAction | Qt.MoveAction
                            Drag.proposedAction: Qt.CopyAction
                            Drag.mimeData: ({
                                    "itemId": itemId,
                                    "itemIds": itemIds,
                                    "sourceId": sourceId,
                                    "sourceIds": sourceIds,
                                    "marks": marks,
                                    "mode": mode,
                                    "group": group,
                                    "application/x-nemo-source-id": sourceId
                                })
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: listView.count === 0
                        text: "No media in " + root.currentName()
                        color: root.theme ? root.theme.muted : "#979ea8"
                        font.pixelSize: root.theme ? root.theme.fontSize : 11
                    }
                }
                GridView {
                    id: gridView
                    z: 1
                    objectName: "mediaGridView"
                    visible: root.viewMode === "grid"
                    anchors.fill: parent
                    anchors.margins: 8
                    clip: true
                    cellWidth: 118
                    cellHeight: 82
                    model: root.visibleRecords
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Item {
                        id: gridCell
                        required property var modelData
                        property var record: modelData
                        property string itemId: String(record.id)
                        property string sourceId: record.sourceId ? String(record.sourceId) : ""
                        property string sourceGroup: root.panelGroup
                        property var itemIds: root.selectedIds
                        property var sourceIds: root.selectedSourceIds
                        // Parallel to sourceIds (hoisted per revision); the
                        // timeline drop reads these through drag.source so
                        // repeated occurrences keep their own marked range.
                        property var marks: root.selectedMarks
                        // Same mode the drag proxy publishes; overwrite stays
                        // menu-only, so no consumer hardcodes the vocabulary.
                        property string mode: root.dragMode()
                        property bool selected: root.isSelected(itemId)
                        property string badge: root.probeBadge(record)
                        property color badgeColor: root.badgeColorFor(badge)
                        property string thumbnailUrl: root.thumbnailFor(record)
                        // Measured status box (independent of the badge's
                        // rendered width and of effective visibility, which
                        // would latch at 0 while the grid view is hidden).
                        property real statusWidth: gridCell.badge.length > 0 ? Math.ceil(gridBadgeMetrics.advanceWidth) : 0
                        width: gridView.cellWidth - 8
                        height: gridView.cellHeight - 8
                        objectName: "mediaItem_" + itemId
                        // Independent measurement of the status token.
                        TextMetrics {
                            id: gridBadgeMetrics
                            font: gridBadge.font
                            text: gridCell.badge
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: root.theme ? root.theme.smallRadius : 4
                            color: gridCell.selected ? (root.theme ? root.theme.nodeSelected : "#293e57") : gridMouse.containsMouse ? (root.theme ? root.theme.hover : "#343940") : (root.theme ? root.theme.field : "#24272c")
                            border.color: gridCell.selected ? (root.theme ? root.theme.accent : "#3485f6") : (root.theme ? root.theme.border : "#30343a")
                            border.width: 1
                        }
                        // Archived preview slot: real thumbnail when available,
                        // archived kind glyph otherwise.
                        Item {
                            id: gridThumbSlot
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 10
                            width: 100
                            height: 30
                            Image {
                                anchors.fill: parent
                                visible: gridCell.thumbnailUrl.length > 0
                                source: gridCell.thumbnailUrl
                                fillMode: Image.PreserveAspectFit
                                sourceSize.width: 100
                                sourceSize.height: 30
                                asynchronous: true
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: gridCell.thumbnailUrl.length === 0
                                text: root.kindGlyph(record)
                                color: root.kindColor(record)
                                font.bold: true
                                font.pixelSize: 24
                            }
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6 + (gridCell.statusWidth > 0 ? gridCell.statusWidth + 6 : 0)
                            y: 43
                            height: 17
                            text: String(record.name || itemId)
                            color: record.offline ? (root.theme ? root.theme.muted : "#979ea8") : (root.theme ? root.theme.text : "#dce0e6")
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: root.theme ? root.theme.fontSize : 11
                        }
                        Text {
                            id: gridBadge
                            visible: gridCell.badge.length > 0
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            y: 43
                            height: 17
                            width: gridCell.statusWidth
                            text: gridCell.badge
                            color: gridCell.badgeColor
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideLeft
                            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: 61
                            height: 13
                            text: (record.kind || "") + (root.durationText(record) ? " · " + root.durationText(record) : "")
                            color: root.theme ? root.theme.muted : "#979ea8"
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 2
                        }
                        MouseArea {
                            id: gridMouse
                            anchors.fill: parent
                            objectName: "mediaGridInput_" + itemId
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            hoverEnabled: true
                            preventStealing: true
                            drag.target: gridDragProxy
                            onPressed: function (mouse) {
                                contentPane.forceActiveFocus();
                                var preserveSelection = root.isSelected(itemId) && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier));
                                if (!preserveSelection)
                                    root.selectOne(itemId, mouse.modifiers);
                                if (!root.isSelected(itemId))
                                    return;
                                var p = gridMouse.mapToItem(root.dragOverlay, mouse.x, mouse.y);
                                gridDragProxy.x = p.x;
                                gridDragProxy.y = p.y;
                                gridDragProxy.itemId = itemId;
                                gridDragProxy.itemIds = root.selectedIds.slice(0);
                                gridDragProxy.sourceId = record.sourceId ? String(record.sourceId) : "";
                                var payload = root.dragPayload(gridDragProxy.itemIds);
                                gridDragProxy.sourceIds = payload.sourceIds;
                                gridDragProxy.marks = payload.marks;
                                gridDragProxy.mode = payload.mode;
                                gridDragProxy.group = payload.group;
                            }
                            onPositionChanged: function (mouse) {
                                if (pressed) {
                                    var p = gridMouse.mapToItem(root.dragOverlay, mouse.x, mouse.y);
                                    gridDragProxy.x = p.x;
                                    gridDragProxy.y = p.y;
                                }
                            }
                            onReleased: if (drag.active)
                                gridDragProxy.Drag.drop()
                            onClicked: function (mouse) {
                                if (mouse.button === Qt.RightButton) {
                                    gridMenu.recordId = itemId;
                                    gridMenu.x = mouse.x;
                                    gridMenu.y = mouse.y;
                                    gridMenu.open();
                                }
                            }
                            onDoubleClicked: root.openSelected()
                        }
                        DropArea {
                            visible: root.isBin(record)
                            anchors.fill: parent
                            z: 3
                            objectName: "mediaGridBinDrop_" + itemId
                            keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                            onEntered: root.beginDropHover(itemId)
                            onExited: root.endDropHover(itemId)
                            onDropped: function (drop) {
                                root.handleDrop(itemId, drop);
                            }
                        }
                        Menu {
                            id: gridMenu
                            property string recordId: ""
                            objectName: "mediaGridMenu_" + itemId
                            MenuItem {
                                text: "Open in viewer"
                                enabled: !!record && !root.isBin(record)
                                onTriggered: {
                                    root.selectedIds = [gridMenu.recordId];
                                    root.persistSelection();
                                    root.openSelected();
                                }
                            }
                            MenuItem {
                                text: "Insert at playhead"
                                enabled: root.selectedSourceIds.length > 0
                                onTriggered: root.requestInsert("insert")
                            }
                            MenuItem {
                                text: "Overwrite at playhead"
                                enabled: root.selectedSourceIds.length > 0
                                onTriggered: root.requestInsert("overwrite")
                            }
                            MenuItem {
                                text: "Metadata…"
                                enabled: !!record
                                onTriggered: root.openMetadata(gridMenu.recordId)
                            }
                            MenuItem {
                                text: "Rename"
                                onTriggered: root.startRename(gridMenu.recordId)
                            }
                            MenuItem {
                                text: "Delete"
                                onTriggered: root.requestDelete([gridMenu.recordId])
                            }
                            MenuSeparator {
                            }
                            MenuItem {
                                objectName: "mediaImportGrid_" + itemId
                                text: "Import media…"
                                onTriggered: root.beginImport(root.importDestinationFor(gridMenu.recordId))
                            }
                            MenuItem {
                                objectName: "mediaRelinkGrid_" + itemId
                                text: "Relink media…"
                                enabled: !!record && !root.isBin(record)
                                onTriggered: root.beginRelink(gridMenu.recordId)
                            }
                            MenuItem {
                                objectName: "mediaReprobeGrid_" + itemId
                                text: "Reprobe media"
                                enabled: !!record && !root.isBin(record)
                                onTriggered: root.reprobe(gridMenu.recordId)
                            }
                            MenuItem {
                                text: "Create proxy…"
                                enabled: false
                            }
                        }
                        Item {
                            id: gridDragProxy
                            property string itemId: ""
                            property var itemIds: []
                            property string sourceId: ""
                            property var sourceIds: []
                            property var marks: []
                            property string mode: "insert"
                            property string group: ""
                            parent: root.dragOverlay
                            width: 2
                            height: 2
                            Drag.active: gridMouse.drag.active
                            Drag.source: gridCell
                            Drag.keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                            Drag.supportedActions: Qt.CopyAction | Qt.MoveAction
                            Drag.proposedAction: Qt.CopyAction
                            Drag.mimeData: ({
                                    "itemId": itemId,
                                    "itemIds": itemIds,
                                    "sourceId": sourceId,
                                    "sourceIds": sourceIds,
                                    "marks": marks,
                                    "mode": mode,
                                    "group": group,
                                    "application/x-nemo-source-id": sourceId
                                })
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: gridView.count === 0
                        text: "No media in " + root.currentName()
                        color: root.theme ? root.theme.muted : "#979ea8"
                        font.pixelSize: root.theme ? root.theme.fontSize : 11
                    }
                }
                DropArea {
                    anchors.fill: parent
                    z: 0
                    objectName: "mediaContentDropArea"
                    keys: ["application/x-nemo-source", "application/x-nemo-source-id", "text/plain"]
                    onEntered: root.beginDropHover(root.currentParentId)
                    onExited: root.endDropHover(root.currentParentId)
                    onDropped: function (drop) {
                        root.handleDrop(root.currentParentId, drop);
                    }
                }
            }
        }

        Label {
            id: statusLabel
            objectName: "mediaStatus"
            Layout.fillWidth: true
            visible: root.statusError && root.statusMessage.length > 0
            text: root.statusMessage
            color: root.theme ? root.theme.text : "#dce0e6"
            elide: Text.ElideRight
            font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
        }
    }

    Menu {
        id: filterMenu
        objectName: "mediaFilterMenu"
        title: "Media filters"
        Menu {
            title: "Kind"
            MenuItem {
                text: "All"
                checkable: true
                checked: root.kindFilter === "all"
                onTriggered: {
                    root.leaveSmartBin();
                    root.kindFilter = "all";
                    root.persistFilters();
                }
            }
            MenuItem {
                text: "Video"
                checkable: true
                checked: root.kindFilter === "video"
                onTriggered: {
                    root.leaveSmartBin();
                    root.kindFilter = "video";
                    root.persistFilters();
                }
            }
            MenuItem {
                text: "Audio"
                checkable: true
                checked: root.kindFilter === "audio"
                onTriggered: {
                    root.leaveSmartBin();
                    root.kindFilter = "audio";
                    root.persistFilters();
                }
            }
            MenuItem {
                text: "Still"
                checkable: true
                checked: root.kindFilter === "still"
                onTriggered: {
                    root.leaveSmartBin();
                    root.kindFilter = "still";
                    root.persistFilters();
                }
            }
            MenuItem {
                text: "Bins"
                checkable: true
                checked: root.kindFilter === "bin"
                onTriggered: {
                    root.leaveSmartBin();
                    root.kindFilter = "bin";
                    root.persistFilters();
                }
            }
        }
        MenuItem {
            text: "Unused only"
            checkable: true
            checked: root.unusedOnly
            onTriggered: {
                root.leaveSmartBin();
                root.unusedOnly = !root.unusedOnly;
                root.persistFilters();
            }
        }
        MenuItem {
            text: "Offline only"
            checkable: true
            checked: root.offlineOnly
            onTriggered: {
                root.leaveSmartBin();
                root.offlineOnly = !root.offlineOnly;
                root.persistFilters();
            }
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Sort by name"
            checkable: true
            checked: root.sortField === "name"
            onTriggered: {
                root.sortField = "name";
                root.persistFilters();
            }
        }
        MenuItem {
            text: "Sort by type"
            checkable: true
            checked: root.sortField === "kind"
            onTriggered: {
                root.sortField = "kind";
                root.persistFilters();
            }
        }
        MenuItem {
            text: "Sort by duration"
            checkable: true
            checked: root.sortField === "duration"
            onTriggered: {
                root.sortField = "duration";
                root.persistFilters();
            }
        }
        MenuItem {
            text: "Reverse sort"
            checkable: true
            checked: root.sortDescending
            onTriggered: {
                root.sortDescending = !root.sortDescending;
                root.persistFilters();
            }
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Save current search as Smart Bin…"
            enabled: !root.currentSmartBinId
            onTriggered: root.beginSaveSmartBin()
        }
    }

    Menu {
        id: treeMenu
        property string recordId: "root"
        objectName: "mediaTreeMenu"
        MenuItem {
            text: "Open bin"
            onTriggered: root.openBin(treeMenu.recordId)
        }
        MenuItem {
            text: "New bin"
            onTriggered: root.beginCreate(treeMenu.recordId, false)
        }
        MenuItem {
            text: "New bin from selection"
            onTriggered: root.beginCreate(treeMenu.recordId, true)
        }
        MenuItem {
            objectName: "mediaTreeImport"
            text: "Import media…"
            onTriggered: root.beginImport(root.importDestinationFor(treeMenu.recordId))
        }
        MenuItem {
            text: "Rename"
            enabled: treeMenu.recordId !== "root"
            onTriggered: root.startRename(treeMenu.recordId)
        }
        MenuItem {
            text: "Delete"
            enabled: treeMenu.recordId !== "root"
            onTriggered: root.requestDelete([treeMenu.recordId])
        }
    }

    Dialog {
        id: renameDialog
        property string recordId: ""
        objectName: "mediaRenameDialog"
        title: "Rename item"
        modal: true
        width: 320
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: TextField {
            id: renameDialogField
            objectName: "mediaRenameField"
            implicitWidth: 280
            selectByMouse: true
            onAccepted: renameDialog.accept()
        }
        onAccepted: root.commitRename(recordId, renameDialogField.text)
    }

    Dialog {
        id: metadataDialog
        property string recordId: ""
        objectName: "mediaMetadataDialog"
        title: "Media metadata"
        modal: true
        width: 350
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Column {
            spacing: 7
            width: 320
            Text {
                objectName: "mediaMetadataTitle"
                width: parent.width
                text: root.itemById(metadataDialog.recordId) ? String(root.itemById(metadataDialog.recordId).name || "") : ""
                color: root.theme ? root.theme.text : "#dce0e6"
                font.bold: true
                elide: Text.ElideRight
            }
            // Full decode/fallback diagnostic. The area keeps a fixed height and
            // scrolls internally, so a long diagnostic never changes the
            // dialog's size.
            Flickable {
                id: metadataDiagnostic
                objectName: "mediaMetadataDiagnostic"
                width: parent.width
                height: 64
                clip: true
                contentWidth: width
                contentHeight: metadataDiagnosticText.implicitHeight
                ScrollBar.vertical: ScrollBar {
                }
                Text {
                    id: metadataDiagnosticText
                    width: metadataDiagnostic.width
                    text: root.probeDiagnostic(metadataDialog.recordId)
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: root.theme ? root.theme.muted : "#979ea8"
                    font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
                }
            }
            TextField {
                id: metadataDialogField
                objectName: "mediaTagsField"
                width: parent.width
                placeholderText: "Tags, comma separated"
                selectByMouse: true
            }
            TextArea {
                id: metadataDescriptionField
                objectName: "mediaDescriptionField"
                width: parent.width
                height: 70
                placeholderText: "Description"
                wrapMode: TextEdit.Wrap
            }
            StudioComboBox {
                id: metadataLabelCombo
                objectName: "mediaLabelCombo"
                theme: root.theme
                width: parent.width
                model: root.labelOptions
            }
            Row {
                spacing: 6
                ChromeButton {
                    objectName: "mediaMetadataImportButton"
                    theme: root.theme
                    text: "Import media…"
                    implicitHeight: 24
                    implicitWidth: 92
                    padding: 5
                    onClicked: root.beginImport(root.importDestinationFor(metadataDialog.recordId))
                }
                ChromeButton {
                    objectName: "mediaMetadataRelinkButton"
                    theme: root.theme
                    text: "Relink…"
                    implicitHeight: 24
                    implicitWidth: 62
                    padding: 5
                    enabled: !!root.itemById(metadataDialog.recordId) && !root.isBin(root.itemById(metadataDialog.recordId))
                    onClicked: root.beginRelink(metadataDialog.recordId)
                }
                ChromeButton {
                    objectName: "mediaMetadataProxyButton"
                    theme: root.theme
                    text: "Proxy…"
                    implicitHeight: 24
                    implicitWidth: 60
                    padding: 5
                    enabled: false
                    ToolTip.visible: hovered
                    ToolTip.text: "Proxy generation is unavailable"
                }
                ChromeButton {
                    objectName: "mediaApplyProbeButton"
                    theme: root.theme
                    text: "Apply probe"
                    implicitHeight: 24
                    implicitWidth: 78
                    padding: 5
                    enabled: root.probeCanApply(metadataDialog.recordId)
                    ToolTip.visible: hovered
                    ToolTip.text: "Publish the pending probe result into the catalog metadata"
                    onClicked: root.applyProbeMetadata(metadataDialog.recordId)
                }
            }
        }
        onAccepted: {
            root.applyMetadata(recordId, {
                    "tags": metadataDialogField.text.split(",").map(function (v) {
                            return v.trim();
                        }).filter(function (v) {
                            return v.length > 0;
                        }),
                    "description": metadataDescriptionField.text,
                    "color": metadataLabelCombo.currentText
                });
        }
    }

    Dialog {
        id: smartDialog
        property string mode: "save"
        property string smartId: ""
        property string name: ""
        property var query: ({})
        objectName: "mediaSmartBinDialog"
        title: mode === "rename" ? "Rename Smart Bin" : "Save Smart Bin"
        modal: true
        width: 320
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: TextField {
            id: smartNameField
            objectName: "mediaSmartBinNameField"
            implicitWidth: 280
            placeholderText: "Search name"
            selectByMouse: true
            onAccepted: smartDialog.accept()
        }
        onAccepted: root.finishSmartDialog()
    }

    Menu {
        id: smartMenu
        property string smartId: ""
        objectName: "mediaSmartBinMenu"
        MenuItem {
            text: "Open Smart Bin"
            onTriggered: root.openSmartBin(smartMenu.smartId)
        }
        MenuItem {
            text: "Rename search…"
            enabled: !!root.smartBinById(smartMenu.smartId) && !root.smartBinById(smartMenu.smartId).builtIn
            onTriggered: root.beginRenameSmartBin(smartMenu.smartId)
        }
        MenuItem {
            text: "Delete search"
            enabled: !!root.smartBinById(smartMenu.smartId) && !root.smartBinById(smartMenu.smartId).builtIn
            onTriggered: root.deleteSmartBin(smartMenu.smartId)
        }
    }

    Dialog {
        id: createBinDialog
        property string parentId: "root"
        property bool fromSelection: false
        objectName: "mediaCreateBinDialog"
        title: fromSelection ? "New bin from selection" : "New bin"
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: TextField {
            id: createBinField
            implicitWidth: 260
            placeholderText: "Bin name"
            selectByMouse: true
            onAccepted: createBinDialog.accept()
        }
        onAccepted: root.finishCreate()
    }

    Dialog {
        id: deleteDialog
        property var ids: []
        objectName: "mediaDeleteDialog"
        title: "Delete non-empty bin"
        modal: true
        width: 380
        standardButtons: Dialog.Cancel
        contentItem: Column {
            spacing: 8
            width: 350
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Deleting this bin can reparent its contents to the parent bin, or remove the contents too."
                color: root.theme ? root.theme.text : "#dce0e6"
            }
            Row {
                spacing: 5
                ChromeButton {
                    objectName: "mediaDeleteKeepContents"
                    theme: root.theme
                    text: "Delete bin, keep contents"
                    implicitHeight: 24
                    implicitWidth: 148
                    padding: 5
                    onClicked: {
                        var idsToDelete = deleteDialog.ids;
                        deleteDialog.close();
                        if (root.applyCatalog({
                                "type": "remove",
                                "ids": idsToDelete,
                                "keepContents": true
                            }, "Deleted bin; kept contents"))
                            root.clearSelection();
                    }
                }
                ChromeButton {
                    objectName: "mediaDeleteContents"
                    theme: root.theme
                    text: "Delete contents"
                    implicitHeight: 24
                    implicitWidth: 102
                    padding: 5
                    onClicked: {
                        var idsToDelete = deleteDialog.ids;
                        deleteDialog.close();
                        if (root.applyCatalog({
                                "type": "remove",
                                "ids": idsToDelete,
                                "keepContents": false
                            }, "Deleted selection"))
                            root.clearSelection();
                    }
                }
            }
        }
    }

    // Archived metadata dialog geometry (350 wide, 320 content) reused for the
    // import/relink placements the prototype left disabled. The path field is
    // the explicit sequence-pattern entry for choosers that cannot pick a
    // pattern such as /shots/plate.####.exr. The start frame maps onto the
    // source reference's frameOffset on import; relink never changes it.
    Dialog {
        id: importDialog
        property string parentId: "root"
        objectName: "mediaImportDialog"
        title: "Import media"
        modal: true
        width: 350
        standardButtons: Dialog.Cancel
        contentItem: Column {
            spacing: 8
            width: 320
            TextField {
                id: importPathField
                objectName: "mediaImportPathField"
                width: parent.width
                placeholderText: "Sequence path, e.g. /shots/plate.####.exr"
                selectByMouse: true
                onAccepted: root.commitImport()
            }
            TextField {
                id: importStartFrameField
                objectName: "mediaImportStartFrameField"
                width: parent.width
                placeholderText: "Sequence start frame (default 0)"
                selectByMouse: true
                validator: IntValidator {
                }
                onAccepted: root.commitImport()
            }
            Row {
                spacing: 6
                ChromeButton {
                    objectName: "mediaImportChooseButton"
                    theme: root.theme
                    text: "Choose files…"
                    implicitHeight: 24
                    implicitWidth: 92
                    padding: 5
                    onClicked: root.chooseImportFiles()
                }
                ChromeButton {
                    objectName: "mediaImportConfirmButton"
                    theme: root.theme
                    text: "Import"
                    implicitHeight: 24
                    implicitWidth: 56
                    padding: 5
                    onClicked: root.commitImport()
                }
            }
        }
    }

    Dialog {
        id: relinkDialog
        property string recordId: ""
        objectName: "mediaRelinkDialog"
        title: "Relink media"
        modal: true
        width: 350
        standardButtons: Dialog.Cancel
        contentItem: Column {
            spacing: 8
            width: 320
            TextField {
                id: relinkPathField
                objectName: "mediaRelinkPathField"
                width: parent.width
                placeholderText: "Sequence path, e.g. /shots/plate.####.exr"
                selectByMouse: true
                onAccepted: root.commitRelink()
            }
            Text {
                objectName: "mediaRelinkStartFrame"
                width: parent.width
                text: root.relinkStartFrameText(relinkDialog.recordId)
                color: root.theme ? root.theme.muted : "#979ea8"
                font.pixelSize: (root.theme ? root.theme.fontSize : 11) - 1
                elide: Text.ElideRight
            }
            Row {
                spacing: 6
                ChromeButton {
                    objectName: "mediaRelinkChooseButton"
                    theme: root.theme
                    text: "Choose file…"
                    implicitHeight: 24
                    implicitWidth: 86
                    padding: 5
                    onClicked: root.chooseRelinkFile()
                }
                ChromeButton {
                    objectName: "mediaRelinkConfirmButton"
                    theme: root.theme
                    text: "Relink"
                    implicitHeight: 24
                    implicitWidth: 54
                    padding: 5
                    onClicked: root.commitRelink()
                }
            }
        }
    }

    Timer {
        id: statusTimer
        interval: 2800
        onTriggered: root.statusMessage = ""
    }
    Timer {
        id: hoverExpandTimer
        interval: 650
        onTriggered: {
            if (root.pendingHoverBinId && root.library && root.library.item(root.pendingHoverBinId)) {
                var nextExpanded = Object.assign({}, root.expandedBins);
                nextExpanded[root.pendingHoverBinId] = true;
                root.expandedBins = nextExpanded;
                root.pendingHoverBinId = "";
                root.saveState({
                        "expandedBins": root.expandedBins
                    });
            }
        }
    }

    Connections {
        target: root.library
        ignoreUnknownSignals: true
        function onRevisionChanged() {
            root.selectedIds = root.normalizeIds(root.selectedIds);
            // Recover from a removed bin, but never clobber project-scope or
            // smart-bin navigation (their scope id is not a catalog record).
            if (!root.projectScope && !root.itemById(root.currentParentId))
                root.currentParentId = "root";
        }
        function onErrorChanged() {
            if (root.library && root.library.error)
                root.setStatus(root.library.error, true);
        }
        // Full decode/fallback diagnostic in the archived status surface.
        function onProbeRejected(id, reason) {
            if (reason)
                root.setStatus(String(reason), true);
        }
        function onImportPathsChosen(paths, parentId) {
            var offset = root.importStartFrame();
            if (offset === null) {
                root.setStatus("Sequence start frame must be a whole number", true);
                return;
            }
            if (root.importPaths(paths, parentId, offset))
                importDialog.close();
        }
        function onRelinkPathChosen(id, path) {
            if (root.relink(id, path))
                relinkDialog.close();
        }
        function onMediaChooserCancelled() {
            root.setStatus("", false);
        }
        function onMediaChooserFailed(message) {
            root.setStatus(String(message), true);
        }
    }

    onPanelStateChanged: restoreState()
    Component.onCompleted: {
        restoreState();
        stateReady = true;
        if (!root.library || !root.library.item || !root.library.item("root"))
            root.setStatus("Media library is unavailable", true);
    }

    Keys.onPressed: function (event) {
        var key = event.key;
        var modifiers = event.modifiers;
        if ((modifiers & Qt.ControlModifier) && key === Qt.Key_A) {
            root.selectAll();
            event.accepted = true;
        } else if ((modifiers & Qt.ControlModifier) && key === Qt.Key_C) {
            root.copySelection(false);
            event.accepted = true;
        } else if ((modifiers & Qt.ControlModifier) && key === Qt.Key_X) {
            root.copySelection(true);
            event.accepted = true;
        } else if ((modifiers & Qt.ControlModifier) && key === Qt.Key_V) {
            root.pasteSelection();
            event.accepted = true;
        } else if ((modifiers & Qt.ControlModifier) && key === Qt.Key_Z && !(modifiers & Qt.ShiftModifier)) {
            if (root.library && root.library.undo)
                root.library.undo();
            event.accepted = true;
        } else if (((modifiers & Qt.ControlModifier) && key === Qt.Key_Z && (modifiers & Qt.ShiftModifier)) || ((modifiers & Qt.ShiftModifier) && key === Qt.Key_Z && !(modifiers & Qt.ControlModifier))) {
            if (root.library && root.library.redo)
                root.library.redo();
            event.accepted = true;
        } else if (key === Qt.Key_Delete) {
            root.deleteSelection();
            event.accepted = true;
        } else if (key === Qt.Key_F2 && root.selectedIds.length) {
            root.startRename(root.selectedIds[0]);
            event.accepted = true;
        } else if (key === Qt.Key_Escape) {
            root.clearSelection();
            event.accepted = true;
        }
    }
}
