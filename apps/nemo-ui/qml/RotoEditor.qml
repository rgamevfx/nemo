import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Roto authoring surface (issue #93), hosted by the generic inspector through
// ParameterEditorRegistry id "nemo.roto.shapes" with presentation "section".
// The host mounts it on the `opacity` row and this editor CONSUMES the node's
// own Roto controls (opacity, output channel, clip, mask, motion blur), so one
// presentation owns the whole compact Roto panel and exactly one control
// renders each setting.
//
// The layout is the reference's compact property editor, not a geometry
// spreadsheet: the node's output controls, then the selected shape's properties
// and its spline keys, then the shape hierarchy, which is the dominant surface
// here. Transform, lifetime and motion blur are folded secondary sections. No
// all-points table exists: the viewport overlay owns point, tangent and feather
// manipulation, and the folded Points section only states the selection and,
// for exactly one selected point, its exact numbers.
//
// Everything authored goes through the shared owners. The node controls are
// value gestures of the host panel (one undo each, existing keying, rejection
// and key menus); element and point properties are value gestures of the shared
// RotoController, which submits the same session commands as any other
// parameter. Nothing here re-implements validation, keying, undo or geometry,
// and the tree's structural edits are the controller's existing commands.
ColumnLayout {
    id: editor

    // Host-injected contract (ParametersPanel). A bare host may load this
    // editor without a controller; every control then states it cannot act.
    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "opacity"
    property var parameter
    property var controller
    property var panel

    // The inspector and viewport share selection within one node/context group.
    property var roto: null
    // The live numeric scrub of this editor (property gestures are one at a
    // time; the panel keeps its own token for its node rows).
    property var scrub: null
    onScrubChanged: {
        if (typeof historyController !== "undefined" && historyController)
            historyController.setGesture(editor, editor.scrub !== null);
    }

    readonly property color textColor: editor.theme ? editor.theme.text : "#dce0e6"
    readonly property color mutedColor: editor.theme ? editor.theme.muted : "#979ea8"
    readonly property color disabledColor: editor.theme ? editor.theme.disabled : "#5f6670"
    readonly property color borderColor: editor.theme ? editor.theme.border : "#30343a"
    readonly property color fieldColor: editor.theme ? editor.theme.field : "#24272c"
    readonly property color panelColor: editor.theme ? editor.theme.panel : "#1e2023"
    readonly property color raisedColor: editor.theme ? editor.theme.raised : "#282c31"
    readonly property color hoverColor: editor.theme ? editor.theme.hover : "#343940"
    readonly property color accentColor: editor.theme ? editor.theme.accent : "#3485f6"
    readonly property color errorColor: editor.theme ? editor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: editor.theme ? editor.theme.smallRadius : 4
    readonly property int fontSizeValue: editor.theme ? editor.theme.fontSize : 11
    readonly property int smallFontSize: Math.max(9, editor.fontSizeValue - 1)
    // The compact row grid of the shared chrome: captions are one column, so a
    // property row never grows with the number of shapes or points.
    readonly property int rowHeight: 21
    readonly property int captionWidth: 48

    // -- presentation state -------------------------------------------------
    // Fold state of the shape hierarchy and of the secondary sections. Folding
    // is presentation only: nothing here reaches the document.
    property bool treeExpanded: true
    property bool pointsExpanded: false
    property bool transformExpanded: false
    property bool lifetimeExpanded: false
    property bool motionBlurExpanded: false
    // Folded groups of the hierarchy, keyed by element id.
    property var collapsedGroups: ({})
    // Live hierarchy drag: the dragged element, the resolved drop and the
    // pointer's last view position (the auto-scroll tick re-resolves the drop
    // from it).
    property string dragId: ""
    property int dropRow: -1
    property bool dropInto: false
    property string dropParent: ""
    property int dropIndex: -1
    property real dragViewY: 0
    property int autoScrollStep: 0

    // The inspector query addresses the real node that owns the parameters (a
    // definition network for an occurrence exposure); the same scope owns the
    // Roto data, so both the tree and the gestures stay in one scope.
    readonly property string queryNetwork: editor.parameter && editor.parameter.targetNetwork !== undefined ? String(editor.parameter.targetNetwork) : editor.networkId
    readonly property string queryNode: editor.parameter && editor.parameter.targetNode !== undefined ? String(editor.parameter.targetNode) : editor.nodeId
    readonly property string group: editor.panel && editor.panel.panelGroup !== undefined ? String(editor.panel.panelGroup) : "A"
    readonly property int revision: editor.roto ? Number(editor.roto.revision) : 0
    // The host panel's own revision: it advances on document, frame and catalog
    // changes, so the node's consumed parameters are re-read from it.
    readonly property int panelRevision: editor.panel ? Number(editor.panel.revision) : 0
    readonly property int frame: editor.roto && editor.roto.viewerAttached ? Number(editor.roto.frame)
            : editor.panel && editor.panel.panelContext && editor.panel.panelContext.timelineClock !== undefined
              ? Number(editor.panel.panelContext.timelineClock) : (editor.controller ? Number(editor.controller.frame) : 0)
    readonly property int dragThreshold: editor.controller ? Number(editor.controller.dragDistance) : 4
    readonly property var elements: editor.roto ? (editor.roto.elements || []) : []
    readonly property var points: editor.roto ? (editor.roto.points || []) : []
    readonly property string selectedElement: editor.roto ? String(editor.roto.selectedElement || "") : ""
    // The whole shape selection, read as a property so a selection change
    // re-states every row that depends on it.
    readonly property var selectionIds: {
        if (!editor.roto)
            return [];
        var ids = editor.roto.selectedElements;
        if (ids === undefined || ids === null)
            return editor.selectedElement.length > 0 ? [editor.selectedElement] : [];
        return ids;
    }
    readonly property var selectedPointIds: editor.roto ? (editor.roto.selectedPoints || []) : []
    readonly property int selectionCount: editor.selectionIds.length
    readonly property int pointSelectionCount: editor.selectedPointIds.length
    readonly property bool hasSelection: editor.selectedElement.length > 0
    readonly property var selectedRecord: editor.elementRecord(editor.selectedElement)
    readonly property bool selectedIsGroup: editor.selectedRecord ? editor.selectedRecord.group === true : false
    readonly property bool selectedIsLocked: editor.selectedRecord ? editor.selectedRecord.locked === true : false
    // Shape properties are the ACTIVE shape's: with several shapes selected the
    // name field and the badge state that, so no row pretends to be a batch
    // value for the whole selection.
    readonly property string selectionBadge: editor.selectionCount > 1 ? String(editor.selectionCount) + " selected" : ""
    // One selected point is the precision case: its owning element is the
    // primary one, so its exact numbers are addressable through the controller.
    readonly property var primaryPoint: {
        var ids = editor.selectedPointIds;
        if (ids.length !== 1)
            return null;
        var list = editor.points;
        for (var index = 0; index < list.length; ++index)
            if (String(list[index].id) === String(ids[0]))
                return list[index];
        return null;
    }
    readonly property string precisionElement: editor.primaryPoint ? editor.selectedElement : ""
    readonly property string precisionPoint: editor.primaryPoint ? String(editor.primaryPoint.id) : ""
    // Sorted unique times of the selected shapes'/points' geometry channels.
    readonly property var keyTimes: editor.roto && editor.roto.keyTimes !== undefined && editor.roto.keyTimes !== null ? editor.roto.keyTimes : []
    readonly property bool keyedAtFrame: {
        for (var index = 0; index < editor.keyTimes.length; ++index)
            if (Math.abs(Number(editor.keyTimes[index]) - editor.frame) < 1e-6)
                return true;
        return false;
    }
    readonly property string keyTimesText: {
        if (!editor.hasSelection)
            return "no shape";
        if (editor.keyTimes.length === 0)
            return "unkeyed";
        var parts = [];
        for (var index = 0; index < editor.keyTimes.length; ++index)
            parts.push(String(Math.round(Number(editor.keyTimes[index]) * 1000) / 1000));
        return parts.join(", ");
    }
    // How many children each group owns, so a fold arrow states whether a group
    // has anything to fold without a per-row scan of the hierarchy.
    readonly property var childCounts: {
        var counts = ({});
        for (var index = 0; index < editor.elements.length; ++index) {
            var parent = String(editor.elements[index].parent);
            if (parent.length === 0 || parent === "0")
                continue;
            counts[parent] = (counts[parent] === undefined ? 0 : counts[parent]) + 1;
        }
        return counts;
    }
    // The hierarchy as shown: a folded group hides its whole subtree.
    readonly property var visibleRows: {
        var rows = [];
        var hiddenDepth = -1;
        for (var index = 0; index < editor.elements.length; ++index) {
            var entry = editor.elements[index];
            var depth = Number(entry.depth);
            if (hiddenDepth >= 0 && depth > hiddenDepth)
                continue;
            hiddenDepth = -1;
            rows.push(entry);
            if (entry.group === true && editor.groupCollapsed(String(entry.id)))
                hiddenDepth = depth;
        }
        return rows;
    }

    objectName: "rotoShapeEditor_" + editor.nodeId
    Layout.fillWidth: true
    spacing: 3

    onFrameChanged: editor.syncFrame()
    onGroupChanged: editor.bind()
    onQueryNetworkChanged: {
        editor.bind();
        editor.refreshNodeRows();
    }
    onQueryNodeChanged: {
        editor.bind();
        editor.refreshNodeRows();
    }
    onControllerChanged: {
        editor.bind();
        editor.refreshNodeRows();
    }
    onParameterChanged: editor.bind()
    onPanelChanged: {
        editor.bind();
        editor.refreshNodeRows();
    }
    onNetworkIdChanged: editor.refreshNodeRows()
    onNodeIdChanged: editor.refreshNodeRows()
    // The roto revision covers the authored hierarchy; the panel revision covers
    // the node's own parameters, which the roto adapter does not republish.
    onRevisionChanged: editor.refreshNodeRows()
    onPanelRevisionChanged: editor.refreshNodeRows()
    Component.onCompleted: {
        editor.bind();
        editor.refreshNodeRows();
    }
    Component.onDestruction: {
        editor.cancelScrub();
        if (typeof historyController !== "undefined" && historyController)
            historyController.setGesture(editor, false);
    }
    Connections {
        target: editor.roto
        function onViewerAttachedChanged() { editor.syncFrame() }
        function onGestureChanged() {
            if (!editor.roto.gestureActive)
                editor.scrub = null;
        }
    }
    Connections {
        target: editor.Window.window
        function onActiveChanged() {
            if (!editor.Window.window.active) {
                editor.cancelScrub();
                editor.cancelDrag();
            }
        }
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: editor.scrub !== null
        onActivated: editor.cancelScrub()
    }
    // The hierarchy keeps scrolling while a dragged element is held at an edge.
    Timer {
        interval: 40
        repeat: true
        running: editor.dragId.length > 0 && editor.autoScrollStep !== 0
        onTriggered: {
            tree.contentY = Math.max(0, Math.min(Math.max(0, tree.contentHeight - tree.height), tree.contentY + editor.autoScrollStep));
            editor.trackDrag(editor.dragViewY);
        }
    }

    // --- binding the shared adapter ---------------------------------------
    function bind() {
        var previous = editor.roto;
        var groupId = String(editor.group);
        var created = null;
        if (editor.controller && editor.queryNetwork.length > 0 && editor.queryNode.length > 0)
            created = editor.controller.createRotoControllerFor(editor.queryNetwork, editor.queryNode, groupId, editor);
        if (previous && previous !== created)
            previous.detachView(editor);
        editor.roto = created;
        if (!created)
            return;
        if (!created.viewerAttached)
            created.frame = editor.frame;
        if (created.available === true && String(created.selectedElement || "").length === 0 && (created.elements || []).length > 0)
            created.selectElement(String(created.elements[0].id), false);
    }

    function syncFrame() {
        if (editor.roto && !editor.roto.viewerAttached)
            editor.roto.frame = editor.frame;
    }

    function elementRecord(id) {
        if (String(id).length === 0)
            return null;
        for (var index = 0; index < editor.elements.length; ++index)
            if (String(editor.elements[index].id) === String(id))
                return editor.elements[index];
        return null;
    }

    function elementSelected(id) {
        return editor.selectionIds.indexOf(String(id)) >= 0;
    }

    function isRootParent(id) {
        var text = String(id);
        return text.length === 0 || text === "0";
    }

    // The reparent command takes an EMPTY parent id for the root ("0" is not a
    // valid identity), so every parent this editor resolves for it is
    // normalized once here.
    function parentKey(id) {
        return editor.isRootParent(id) ? "" : String(id);
    }

    function groupCollapsed(id) {
        return editor.collapsedGroups[String(id)] === true;
    }

    function toggleGroup(id) {
        var key = String(id);
        var next = ({});
        for (var existing in editor.collapsedGroups)
            next[existing] = editor.collapsedGroups[existing];
        if (next[key] === true)
            delete next[key];
        else
            next[key] = true;
        editor.collapsedGroups = next;
    }

    // Index of an element among its own siblings, which is what the reparent
    // command's insertion index counts.
    function siblingIndex(record) {
        var position = 0;
        for (var index = 0; index < editor.elements.length; ++index) {
            var entry = editor.elements[index];
            if (String(entry.parent) !== String(record.parent))
                continue;
            if (String(entry.id) === String(record.id))
                return position;
            ++position;
        }
        return position;
    }

    // --- the node's own controls (consumed keys) --------------------------
    // Read through the shared inspector query exactly as the generic rows do,
    // so an exposure or a schema change needs no second source of truth.
    property var nodeRows: ({})
    function refreshNodeRows() {
        var rows = ({});
        if (editor.controller && editor.queryNetwork.length > 0 && editor.queryNode.length > 0) {
            var inspector = editor.controller.parameterInspector(editor.queryNetwork, editor.queryNode);
            var sections = inspector && inspector.sections ? inspector.sections : [];
            for (var section = 0; section < sections.length; ++section) {
                var parameters = sections[section].parameters || [];
                for (var index = 0; index < parameters.length; ++index)
                    if (parameters[index] && parameters[index].key !== undefined)
                        rows[String(parameters[index].key)] = parameters[index];
            }
        }
        editor.nodeRows = rows;
    }

    function nodeRow(key) {
        var row = editor.nodeRows[key];
        return row === undefined ? null : row;
    }

    function hasNodeKey(key) {
        return editor.nodeRow(key) !== null;
    }

    function nodeLabel(key, fallback) {
        var row = editor.nodeRow(key);
        return row && row.label !== undefined && String(row.label).length > 0 ? String(row.label) : fallback;
    }

    function nodeValue(key, fallback) {
        var row = editor.nodeRow(key);
        return row && row.value !== undefined && row.value !== null ? row.value : fallback;
    }

    function nodeChoices(key) {
        var row = editor.nodeRow(key);
        return row && row.choices ? row.choices : [];
    }

    function nodeChoiceIndex(key) {
        var index = editor.nodeChoices(key).indexOf(String(editor.nodeValue(key, "")));
        return index < 0 ? 0 : index;
    }

    function nodeRowRef(key) {
        var row = editor.nodeRow(key);
        return {
            "networkId": editor.networkId,
            "nodeId": editor.nodeId,
            "parameterKey": key,
            "parameter": row,
            "label": editor.nodeLabel(key, key)
        };
    }

    function nodeKeyStatus(key) {
        editor.revision;
        editor.frame;
        if (!editor.panel || key.length === 0)
            return "none";
        return String(editor.panel.parameterKeyStatusFor(editor.networkId, editor.nodeId, key));
    }

    function commitNodeValue(key, value) {
        return editor.panel ? editor.panel.gestureSingle(editor.nodeRowRef(key), value) : false;
    }

    function commitNodeText(key, text) {
        if (!editor.panel)
            return false;
        return editor.panel.gestureText(editor.nodeRowRef(key), text) === true;
    }

    function keyNodeAtFrame(key) {
        return editor.panel ? editor.panel.keyParameterAtFrame(editor.networkId, editor.nodeId, key) : false;
    }

    function removeNodeKeyAtFrame(key) {
        return editor.panel ? editor.panel.removeParameterKeyAtFrame(editor.networkId, editor.nodeId, key) : false;
    }

    // The most recent rejected edit attributed to one of the consumed node
    // keys. The message comes from the controller/catalog; this editor never
    // re-validates.
    function nodeProblem(key) {
        if (!editor.panel || String(editor.panel.gestureErrorKey) !== String(key))
            return "";
        return String(editor.panel.gestureError);
    }

    // --- reads through the controller --------------------------------------
    function paramState(elementId, pointId, key) {
        editor.revision;
        editor.frame;
        if (editor.roto)
            editor.roto.frame;
        if (!editor.roto || !elementId || String(elementId).length === 0)
            return ({
                    "available": false
                });
        return editor.roto.parameterState(String(elementId), pointId ? String(pointId) : "", key);
    }

    function numberValue(elementId, pointId, key) {
        var state = editor.paramState(elementId, pointId, key);
        if (state.available !== true || state.value === undefined || state.value === null
                || state.value.length !== undefined)
            return 0;
        var value = Number(state.value);
        return isFinite(value) ? value : 0;
    }

    function vectorValue(elementId, pointId, key, component) {
        var state = editor.paramState(elementId, pointId, key);
        if (state.available !== true || !state.value || state.value.length === undefined)
            return 0;
        var value = Number(state.value[component]);
        return isFinite(value) ? value : 0;
    }

    function flagValue(elementId, pointId, key) {
        var state = editor.paramState(elementId, pointId, key);
        return state.available === true && state.value === true;
    }

    function choiceValue(elementId, pointId, key) {
        var state = editor.paramState(elementId, pointId, key);
        return state.available === true && state.value !== undefined && state.value !== null
                ? String(state.value) : "";
    }

    function keyStatus(elementId, pointId, key) {
        var state = editor.paramState(elementId, pointId, key);
        return state.available === true ? String(state.keyStatus || "none") : "none";
    }

    function hasMinimum(elementId, pointId, key) {
        return editor.paramState(elementId, pointId, key).hasMinimum === true;
    }

    function elementEditable() {
        return editor.roto !== null && editor.roto.available === true && !editor.selectedIsLocked;
    }

    // The authored lifetime bound of the selected element, falling back to the
    // current frame so the field always states a usable number.
    function lifetimeBound(first) {
        var found = editor.selectedRecord;
        if (!found)
            return editor.frame;
        var bound = first ? found.firstFrame : found.lastFrame;
        return bound === undefined || bound === null ? editor.frame : Number(bound);
    }

    function boundValue(elementId, pointId, key, which) {
        var state = editor.paramState(elementId, pointId, key);
        return Number(state[which] !== undefined ? state[which] : 0);
    }

    function stepValue(elementId, pointId, key) {
        var state = editor.paramState(elementId, pointId, key);
        return state.step !== undefined && Number(state.step) > 0 ? Number(state.step) : 0.01;
    }

    // --- writes through the controller -------------------------------------
    function beginField(elementId, pointId, key) {
        if (!editor.roto)
            return "";
        return String(editor.roto.beginGesture([{
            "element": String(elementId),
            "point": pointId ? String(pointId) : "",
            "key": String(key)
        }]));
    }

    function commitScalar(elementId, pointId, key, value) {
        var token = editor.beginField(elementId, pointId, key);
        if (token.length === 0)
            return false;
        if (editor.roto.updateGesture(token, [value]) !== true) {
            editor.roto.cancelGesture(token);
            return false;
        }
        return editor.roto.commitGesture(token);
    }

    function commitVector(elementId, pointId, key, component, value) {
        var state = editor.paramState(elementId, pointId, key);
        if (state.available !== true || !state.value || state.value.length === undefined)
            return false;
        var next = [Number(state.value[0]), Number(state.value[1])];
        next[component] = value;
        return editor.commitScalar(elementId, pointId, key, next);
    }

    function commitFlag(elementId, pointId, key, value) {
        return editor.commitScalar(elementId, pointId, key, value);
    }

    // Keying and unkeying are discrete authored commands rather than a preview
    // gesture, so they retire whatever the session owns first through the same
    // interaction owner their gestures use.
    function keyAtFrame(elementId, pointId, key) {
        if (!editor.roto)
            return false;
        return editor.roto.keyAtFrame(String(elementId), pointId ? String(pointId) : "", String(key));
    }

    function removeKeyAtFrame(elementId, pointId, key) {
        if (!editor.roto)
            return false;
        return editor.roto.removeKeyAtFrame(String(elementId), pointId ? String(pointId) : "", String(key));
    }

    // One continuous scrub per field: begin, preview, then exactly one commit
    // or cancel, all through the same gesture API.
    function beginScrub(elementId, pointId, key, component) {
        var token = editor.beginField(elementId, pointId, key);
        if (token.length === 0)
            return false;
        editor.scrub = {
            "token": token,
            "element": String(elementId),
            "point": pointId ? String(pointId) : "",
            "key": String(key),
            "component": component
        };
        return true;
    }

    function updateScrub(value) {
        if (!editor.scrub)
            return false;
        var state = editor.paramState(editor.scrub.element, editor.scrub.point, editor.scrub.key);
        var component = Number(editor.scrub.component);
        var next = value;
        if (component >= 0 && state.value && state.value.length !== undefined) {
            next = [Number(state.value[0]), Number(state.value[1])];
            next[component] = value;
        }
        return editor.roto.updateGesture(editor.scrub.token, [next]);
    }

    function finishScrub() {
        if (!editor.scrub)
            return false;
        var token = editor.scrub.token;
        editor.scrub = null;
        return editor.roto.commitGesture(token);
    }

    function cancelScrub() {
        if (!editor.scrub)
            return false;
        var token = editor.scrub.token;
        editor.scrub = null;
        return editor.roto ? editor.roto.cancelGesture(token) : false;
    }

    // The shared history owner reaches the live scrub through this: Escape and a
    // preview-only Undo discard the preview, and the release publishes nothing.
    function cancelHistoryGesture() {
        editor.cancelScrub();
    }

    // The live scrub belongs to exactly ONE field. A control compares the
    // address it is bound to with the staged scrub, so a gesture the session
    // handed to a successor (the controller clears the scrub on gestureChanged)
    // never keeps this field's preview alive, and stale events reach nothing.
    function scrubLive(elementId, pointId, key, component) {
        var scrub = editor.scrub;
        if (!scrub || !editor.roto || editor.roto.gestureActive !== true)
            return false;
        return String(scrub.element) === String(elementId)
                && String(scrub.point) === String(pointId === undefined || pointId === null ? "" : pointId)
                && String(scrub.key) === String(key)
                && Number(scrub.component) === Number(component);
    }

    // --- structural edits ---------------------------------------------------
    function addGroup() {
        var parent = editor.selectedIsGroup ? editor.selectedElement : "";
        if (editor.roto)
            editor.roto.addGroup(parent);
    }

    // Removes what is selected: the selected points when a point selection
    // exists, otherwise the selected shapes and their descendants. The
    // controller owns that rule, so the inspector and the viewport Delete key
    // publish the same command.
    function removeSelection() {
        if (editor.roto)
            editor.roto.deleteSelection();
    }

    function removeShapes() {
        if (!editor.roto || editor.selectionCount === 0)
            return;
        editor.roto.removeElements(editor.selectionIds);
    }

    function selectAllPoints() {
        if (editor.roto)
            editor.roto.selectAllPoints();
    }

    function smoothSelection(smooth) {
        if (editor.roto)
            editor.roto.smoothSelection(smooth);
    }

    function cycleBlend(record) {
        if (!editor.roto || !record)
            return;
        var order = ["combine", "intersect", "subtract"];
        var next = order[(order.indexOf(String(record.blend)) + 1) % order.length];
        editor.roto.setElementProperty(String(record.id), "blend", next);
    }

    function toggleVisible(record) {
        if (!editor.roto || !record)
            return;
        editor.commitFlag(String(record.id), "", "visible", record.visible !== true);
    }

    function toggleLocked(record) {
        if (!editor.roto || !record)
            return;
        editor.roto.setElementProperty(String(record.id), "locked", record.locked !== true);
    }

    function toggleInverted(record) {
        if (!editor.roto || !record)
            return;
        editor.commitFlag(String(record.id), "", "inverted", record.inverted !== true);
    }

    function selectRow(record, modifiers) {
        if (!editor.roto || !record)
            return;
        var additive = (modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0;
        editor.roto.selectElement(String(record.id), additive);
    }

    // --- spline keys --------------------------------------------------------
    // Geometry keys of the current selection: the controller owns which point
    // channels they are, this editor only states and toggles them at the frame.
    function toggleGeometryKey(remove) {
        return editor.roto ? editor.roto.keySelection(remove === true) === true : false;
    }

    function jumpToKey(forward) {
        var times = editor.keyTimes;
        if (times.length === 0)
            return false;
        var target = null;
        for (var index = 0; index < times.length; ++index) {
            var time = Number(times[index]);
            var beyond = forward ? time > editor.frame + 1e-6 : time < editor.frame - 1e-6;
            if (!beyond)
                continue;
            target = target === null ? time : (forward ? Math.min(target, time) : Math.max(target, time));
        }
        if (target === null)
            target = Number(forward ? times[0] : times[times.length - 1]);
        return editor.setGroupClock(target);
    }

    // Without a viewer, use the inspector group's clock. With a viewer, request
    // an explicit seek through its transport; graph viewers own their clock.
    function setGroupClock(value) {
        if (!editor.panel || !editor.panel.contextRouter || editor.panel.panelGroup === undefined)
            return false;
        var change = {};
        change["timelineClock"] = value;
        var accepted = editor.panel.contextRouter.setGroupContext(String(editor.panel.panelGroup), change) === true;
        if (accepted && editor.roto)
            editor.roto.seekRequested(Math.round(value));
        return accepted;
    }

    // --- hierarchy drag -----------------------------------------------------
    // The drop resolves from the pointer's own row position, so a drop above a
    // row inserts before it, a drop below a group's middle makes it that
    // group's first child, and a drop below a shape inserts after it. A group
    // may never receive itself or one of its own descendants.
    function dragTargetFor(contentY) {
        var rows = editor.visibleRows;
        if (rows.length === 0 || editor.dragId.length === 0)
            return null;
        var slot = Math.floor(Math.max(0, contentY) / editor.rowHeight);
        if (slot >= rows.length)
            slot = rows.length - 1;
        var row = rows[slot];
        if (String(row.id) === editor.dragId)
            return null;
        var fraction = (Math.max(0, contentY) - slot * editor.rowHeight) / editor.rowHeight;
        var before = fraction < 0.5;
        var parent = "";
        var index = -1;
        var into = false;
        if (before) {
            parent = editor.parentKey(row.parent);
            index = editor.siblingIndex(row);
        } else if (row.group === true) {
            parent = String(row.id);
            index = 0;
            into = true;
        } else {
            parent = editor.parentKey(row.parent);
            index = editor.siblingIndex(row) + 1;
        }
        if (!editor.canDropUnder(parent))
            return null;
        return {
            "row": slot,
            "parent": parent,
            "index": index,
            "into": into
        };
    }

    function canDropUnder(parent) {
        if (editor.isRootParent(parent))
            return true;
        if (parent === editor.dragId)
            return false;
        var walk = editor.elementRecord(parent);
        while (walk) {
            if (String(walk.id) === editor.dragId)
                return false;
            walk = editor.isRootParent(walk.parent) ? null : editor.elementRecord(walk.parent);
        }
        return true;
    }

    function trackDrag(viewY) {
        if (editor.dragId.length === 0)
            return;
        editor.dragViewY = viewY;
        editor.autoScrollStep = viewY < 8 ? -14 : (tree.height > 0 && viewY > tree.height - 8 ? 14 : 0);
        var contentY = tree.contentY + viewY;
        var target = editor.dragTargetFor(contentY);
        editor.dropRow = target ? Number(target.row) : -1;
        editor.dropInto = target ? target.into === true : false;
        editor.dropParent = target ? String(target.parent) : "";
        editor.dropIndex = target ? Number(target.index) : -1;
    }

    function cancelDrag() {
        editor.dragId = "";
        editor.dropRow = -1;
        editor.dropInto = false;
        editor.autoScrollStep = 0;
    }

    function commitDrag() {
        var id = editor.dragId;
        var parent = editor.dropParent;
        var index = editor.dropIndex;
        var valid = editor.dragId.length > 0 && index >= 0 && editor.dropRow >= 0;
        editor.cancelDrag();
        if (!valid || !editor.roto)
            return;
        // The command removes the element before inserting it, so an insertion
        // index counted on the current list shifts by one when the element
        // already sits earlier among the destination's children; a move that
        // resolves to its own position is not published at all.
        var record = editor.elementRecord(id);
        var target = index;
        if (record && editor.parentKey(record.parent) === editor.parentKey(parent)) {
            var from = editor.siblingIndex(record);
            if (from < index)
                target = index - 1;
            if (target === from)
                return;
        }
        editor.roto.reparentElement(id, parent, target);
    }

    function errorText() {
        if (editor.roto && String(editor.roto.error || "").length > 0)
            return String(editor.roto.error);
        if (editor.panel && String(editor.panel.gestureError || "").length > 0)
            return String(editor.panel.gestureError);
        return "";
    }

    // --- shared cell components -------------------------------------------
    component Cell: ExposureLabel {
        id: cell
        required property string editKey
        required property string elementId
        required property string pointId
        theme: editor.theme
        networkId: editor.queryNetwork
        instanceId: editor.instanceId
        nodeId: editor.queryNode
        parameterKey: cell.editKey
        frame: editor.frame
        keyStatus: editor.keyStatus(cell.elementId, cell.pointId, cell.editKey)
        implicitHeight: 23
        onKeyRequested: editor.keyAtFrame(cell.elementId, cell.pointId, cell.editKey)
        KeyIndicator {
            id: cellKeys
            visible: false
            theme: editor.cellTheme
            networkId: editor.queryNetwork
            nodeId: editor.queryNode
            parameterKey: cell.editKey
            parameterLabel: cell.labelText
            keyStatus: cell.keyStatus
            frame: editor.frame
            revealAvailable: false
            onKeyRequested: editor.keyAtFrame(cell.elementId, cell.pointId, cell.editKey)
            onRemoveKeyRequested: editor.removeKeyAtFrame(cell.elementId, cell.pointId, cell.editKey)
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: cellKeys.openMenu(cell)
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            visible: cell.keyStatus !== "none"
            color: cell.keyStatus === "key" ? editor.accentColor : editor.mutedColor
        }
    }

    component Action: Button {
        id: action
        implicitHeight: editor.rowHeight
        padding: 4
        font.pixelSize: editor.fontSizeValue
        contentItem: Text {
            text: action.text
            color: action.enabled ? editor.textColor : editor.disabledColor
            font.pixelSize: editor.fontSizeValue
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: action.down ? editor.raisedColor : action.hovered ? editor.hoverColor : editor.fieldColor
            border.color: editor.borderColor
            radius: editor.smallRadiusValue
        }
    }

    // A disclosure header: the host's own section chrome (glyph plus name), so
    // this editor adds no new navigation.
    component SectionHeader: Button {
        id: header
        required property string sectionKey
        required property string title
        property bool expanded: true
        // `toggled` is already AbstractButton's own signal, so the disclosure
        // request carries its own name.
        signal sectionToggled()
        objectName: "rotoSection_" + editor.nodeId + "_" + header.sectionKey
        implicitHeight: editor.rowHeight
        padding: 2
        contentItem: Text {
            text: (header.expanded ? "\u25be " : "\u25b8 ") + header.title
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignLeft
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: header.hovered ? editor.hoverColor : "transparent"
            radius: editor.smallRadiusValue
        }
        Accessible.name: header.title
        Accessible.description: header.expanded ? "Collapse this section" : "Expand this section"
        onClicked: header.sectionToggled()
        ToolTip.visible: hovered
        ToolTip.text: (header.expanded ? "Collapse " : "Expand ") + header.title
    }

    // One scalar element/point property through the shared numeric control.
    component ScalarRow: RowLayout {
        id: scalarRow
        required property string elementId
        required property string pointId
        required property string fieldKey
        required property string fieldLabel
        Layout.fillWidth: true
        spacing: 4
        Cell {
            editKey: scalarRow.fieldKey
            elementId: scalarRow.elementId
            pointId: scalarRow.pointId
            labelText: scalarRow.fieldLabel
            Layout.preferredWidth: editor.captionWidth
        }
        NumericField {
            objectName: "rotoField_" + editor.nodeId + "_" + scalarRow.fieldKey + "_" + scalarRow.elementId + "_" + scalarRow.pointId
            theme: editor.theme
            value: editor.numberValue(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey)
            hasMinimum: editor.hasMinimum(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey)
            minimum: editor.boundValue(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey, "minimum")
            hasMaximum: editor.paramState(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey).hasMaximum === true
            maximum: editor.boundValue(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey, "maximum")
            hasSoftMinimum: false
            hasSoftMaximum: false
            step: editor.stepValue(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey)
            label: scalarRow.fieldLabel
            dragThreshold: editor.dragThreshold
            fieldWidth: 62
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            enabled: editor.elementEditable()
            interactionOwner: editor.roto
            gestureLive: editor.scrubLive(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey, -1)
            onTextCommitted: function(text) {
                editor.commitScalar(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey, Number(text));
            }
            onStepped: function(value) {
                editor.commitScalar(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey, value);
            }
            onScrubStarted: editor.beginScrub(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey, -1)
            onScrubbed: function(value) {
                editor.updateScrub(value);
            }
            onScrubFinished: editor.finishScrub()
            onScrubCancelled: editor.cancelScrub()
            onKeyRequested: editor.keyAtFrame(scalarRow.elementId, scalarRow.pointId, scalarRow.fieldKey)
        }
    }

    // One Vector2 element/point property: two component fields over the one
    // addressed value, so a component edit authors the whole vector.
    component VectorRow: RowLayout {
        id: vectorRow
        required property string elementId
        required property string pointId
        required property string fieldKey
        required property string fieldLabel
        Layout.fillWidth: true
        spacing: 4
        Cell {
            editKey: vectorRow.fieldKey
            elementId: vectorRow.elementId
            pointId: vectorRow.pointId
            labelText: vectorRow.fieldLabel
            Layout.preferredWidth: editor.captionWidth
        }
        Repeater {
            model: ["X", "Y"]
            delegate: NumericField {
                id: componentField
                required property string modelData
                required property int index
                objectName: "rotoField_" + editor.nodeId + "_" + vectorRow.fieldKey + "_" + vectorRow.elementId + "_" + vectorRow.pointId + "_" + componentField.modelData
                theme: editor.theme
                value: editor.vectorValue(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey, componentField.index)
                hasMinimum: false
                hasMaximum: false
                hasSoftMinimum: false
                hasSoftMaximum: false
                step: editor.stepValue(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey)
                label: vectorRow.fieldLabel + " " + componentField.modelData
                dragThreshold: editor.dragThreshold
                fieldWidth: 56
                Layout.fillWidth: true
                Layout.minimumWidth: 34
                enabled: editor.elementEditable()
                interactionOwner: editor.roto
                gestureLive: editor.scrubLive(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey,
                                              componentField.index)
                onTextCommitted: function(text) {
                    editor.commitVector(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey,
                                        componentField.index, Number(text));
                }
                onStepped: function(value) {
                    editor.commitVector(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey,
                                        componentField.index, value);
                }
                onScrubStarted: editor.beginScrub(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey,
                                                  componentField.index)
                onScrubbed: function(value) {
                    editor.updateScrub(value);
                }
                onScrubFinished: editor.finishScrub()
                onScrubCancelled: editor.cancelScrub()
                onKeyRequested: editor.keyAtFrame(vectorRow.elementId, vectorRow.pointId, vectorRow.fieldKey)
            }
        }
    }

    // One flag over a keyable ELEMENT property (inverted / featherEnabled): a
    // discrete value gesture, so a keyed flag is keyed at the current frame
    // like every other parameter, and the shared key menu stays on it.
    component ElementFlag: CheckBox {
        id: flagBox
        required property string fieldKey
        required property string fieldLabel
        objectName: "rotoFlag_" + editor.nodeId + "_" + flagBox.fieldKey
        checked: editor.flagValue(editor.selectedElement, "", flagBox.fieldKey)
        text: flagBox.fieldLabel
        font.pixelSize: editor.fontSizeValue
        padding: 0
        leftPadding: 14
        implicitHeight: editor.rowHeight
        enabled: editor.elementEditable()
        Accessible.name: flagBox.fieldLabel
        contentItem: Text {
            text: flagBox.fieldLabel
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            verticalAlignment: Text.AlignVCenter
        }
        indicator: Rectangle {
            width: 12
            height: 12
            y: (flagBox.height - height) / 2
            radius: 2
            color: flagBox.checked ? editor.accentColor : editor.fieldColor
            border.color: flagBox.activeFocus ? editor.accentColor : editor.borderColor
            Text {
                anchors.centerIn: parent
                text: flagBox.checked ? "\u00d7" : ""
                color: editor.textColor
                font.pixelSize: 13
            }
        }
        background: Rectangle {
            color: flagBox.hovered ? editor.hoverColor : "transparent"
            radius: 2
        }
        onToggled: {
            // The checked binding is re-evaluated from the published value;
            // only a real change is authored, so a refresh never re-authors.
            if (checked === editor.flagValue(editor.selectedElement, "", flagBox.fieldKey))
                return;
            editor.commitFlag(editor.selectedElement, "", flagBox.fieldKey, checked);
        }
        KeyIndicator {
            id: flagKeys
            visible: false
            theme: editor.cellTheme
            networkId: editor.queryNetwork
            nodeId: editor.queryNode
            parameterKey: flagBox.fieldKey
            parameterLabel: flagBox.fieldLabel
            keyStatus: editor.keyStatus(editor.selectedElement, "", flagBox.fieldKey)
            frame: editor.frame
            revealAvailable: false
            onKeyRequested: editor.keyAtFrame(editor.selectedElement, "", flagBox.fieldKey)
            onRemoveKeyRequested: editor.removeKeyAtFrame(editor.selectedElement, "", flagBox.fieldKey)
        }
        MouseArea {
            anchors.fill: parent
            onPressed: function(mouse) {
                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier) || mouse.button === Qt.RightButton;
            }
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton)
                    flagKeys.openMenu(flagBox);
                else
                    editor.keyAtFrame(editor.selectedElement, "", flagBox.fieldKey);
            }
        }
        ToolTip.visible: hovered
        ToolTip.text: flagBox.fieldLabel + " (Alt-click to key, right-click for key actions)"
    }

    // One NODE parameter through the host's own numeric bundle: slider, typed
    // field, arrow steps, key-at-frame and the one live gesture per edit, with
    // this editor's caption column.
    component NodeNumberRow: RowLayout {
        id: nodeNumber
        required property string fieldKey
        required property string caption
        readonly property var parameterRow: editor.nodeRow(nodeNumber.fieldKey)
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: nodeNumber.caption
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: editor.captionWidth
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        Loader {
            Layout.fillWidth: true
            sourceComponent: nodeNumber.parameterRow !== null && editor.panel ? editor.panel.numericEditorComponent : null
            onLoaded: {
                item.theme = editor.theme;
                item.panel = editor.panel;
                item.row = adapter;
                item.compact = true;
                item.fieldFirst = true;
            }
        }
        QtObject {
            id: adapter
            readonly property string nodeId: editor.nodeId
            readonly property string parameterKey: nodeNumber.fieldKey
            readonly property string rowLabel: editor.nodeLabel(nodeNumber.fieldKey, nodeNumber.caption)
            readonly property bool integerParameter: nodeNumber.parameterRow ? String(nodeNumber.parameterRow.type) === "integer" : false
            readonly property real numberValue: Number(editor.nodeValue(nodeNumber.fieldKey, 0))
            readonly property string exactText: nodeNumber.parameterRow && nodeNumber.parameterRow.valueText !== undefined ? String(nodeNumber.parameterRow.valueText) : ""
            readonly property bool hasMinimum: nodeNumber.parameterRow ? nodeNumber.parameterRow.minimum !== undefined : false
            readonly property bool hasMaximum: nodeNumber.parameterRow ? nodeNumber.parameterRow.maximum !== undefined : false
            readonly property real minimum: nodeNumber.parameterRow && nodeNumber.parameterRow.minimum !== undefined ? Number(nodeNumber.parameterRow.minimum) : 0
            readonly property real maximum: nodeNumber.parameterRow && nodeNumber.parameterRow.maximum !== undefined ? Number(nodeNumber.parameterRow.maximum) : 0
            readonly property bool hasSoftMinimum: nodeNumber.parameterRow ? nodeNumber.parameterRow.softMinimum !== undefined : false
            readonly property bool hasSoftMaximum: nodeNumber.parameterRow ? nodeNumber.parameterRow.softMaximum !== undefined : false
            readonly property real softMinimum: nodeNumber.parameterRow && nodeNumber.parameterRow.softMinimum !== undefined ? Number(nodeNumber.parameterRow.softMinimum) : 0
            readonly property real softMaximum: nodeNumber.parameterRow && nodeNumber.parameterRow.softMaximum !== undefined ? Number(nodeNumber.parameterRow.softMaximum) : 0
            readonly property real numberStep: adapter.integerParameter ? 1 : (nodeNumber.parameterRow && nodeNumber.parameterRow.step !== undefined ? Number(nodeNumber.parameterRow.step) : 0.01)
            readonly property int decimals: nodeNumber.parameterRow && nodeNumber.parameterRow.displayDecimals !== undefined ? Number(nodeNumber.parameterRow.displayDecimals) : -1
            readonly property int dragThreshold: editor.dragThreshold
            readonly property string rowError: editor.nodeProblem(nodeNumber.fieldKey)
            function rowRef() {
                return editor.nodeRowRef(nodeNumber.fieldKey);
            }
            function commitText(text) {
                return editor.commitNodeText(nodeNumber.fieldKey, text);
            }
            function commitDiscrete(value) {
                return editor.commitNodeValue(nodeNumber.fieldKey, value);
            }
            function keyAtFrame() {
                return editor.keyNodeAtFrame(nodeNumber.fieldKey);
            }
        }
    }

    // One NODE flag through the host's discrete value gesture (replace /
    // invert mask), with the shared key menu.
    component NodeFlag: CheckBox {
        id: nodeFlag
        required property string fieldKey
        required property string fieldLabel
        objectName: "rotoFlag_" + editor.nodeId + "_" + nodeFlag.fieldKey
        checked: editor.nodeValue(nodeFlag.fieldKey, false) === true
        implicitHeight: editor.rowHeight
        padding: 0
        leftPadding: 14
        enabled: editor.hasNodeKey(nodeFlag.fieldKey)
        Accessible.name: nodeFlag.fieldLabel
        contentItem: Text {
            text: nodeFlag.fieldLabel
            color: nodeFlag.enabled ? editor.textColor : editor.disabledColor
            font.pixelSize: editor.smallFontSize
            verticalAlignment: Text.AlignVCenter
        }
        indicator: Rectangle {
            width: 12
            height: 12
            y: (nodeFlag.height - height) / 2
            radius: 2
            color: nodeFlag.checked ? editor.accentColor : editor.fieldColor
            border.color: nodeFlag.activeFocus ? editor.accentColor : editor.borderColor
            Text {
                anchors.centerIn: parent
                text: nodeFlag.checked ? "\u00d7" : ""
                color: editor.textColor
                font.pixelSize: 13
            }
        }
        background: Rectangle {
            color: nodeFlag.hovered ? editor.hoverColor : "transparent"
            radius: 2
        }
        onToggled: {
            if (checked === (editor.nodeValue(nodeFlag.fieldKey, false) === true))
                return;
            editor.commitNodeValue(nodeFlag.fieldKey, checked);
        }
        KeyIndicator {
            id: nodeFlagKeys
            visible: false
            theme: editor.cellTheme
            networkId: editor.queryNetwork
            nodeId: editor.queryNode
            parameterKey: nodeFlag.fieldKey
            parameterLabel: nodeFlag.fieldLabel
            keyStatus: editor.nodeKeyStatus(nodeFlag.fieldKey)
            frame: editor.frame
            revealAvailable: false
            onKeyRequested: editor.keyNodeAtFrame(nodeFlag.fieldKey)
            onRemoveKeyRequested: editor.removeNodeKeyAtFrame(nodeFlag.fieldKey)
        }
        MouseArea {
            anchors.fill: parent
            onPressed: function(mouse) {
                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier) || mouse.button === Qt.RightButton;
            }
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton)
                    nodeFlagKeys.openMenu(nodeFlag);
                else
                    editor.keyNodeAtFrame(nodeFlag.fieldKey);
            }
        }
        ToolTip.visible: hovered
        ToolTip.text: nodeFlag.fieldLabel + " (Alt-click to key, right-click for key actions)"
    }

    // One NODE string parameter (output / mask channel): the exact authored text
    // is committed through the host's own typed gesture, so an invalid name is
    // rejected by the catalog and stated by the shared error line.
    component NodeTextField: TextField {
        id: nodeText
        required property string fieldKey
        property int revision: editor.panelRevision
        objectName: "rotoString_" + editor.nodeId + "_" + nodeText.fieldKey
        text: String(editor.nodeValue(nodeText.fieldKey, ""))
        placeholderText: editor.nodeLabel(nodeText.fieldKey, nodeText.fieldKey)
        Layout.fillWidth: true
        Layout.minimumWidth: 46
        implicitHeight: 23
        font.pixelSize: editor.fontSizeValue
        color: editor.textColor
        selectByMouse: true
        enabled: editor.hasNodeKey(nodeText.fieldKey)
        Accessible.name: editor.nodeLabel(nodeText.fieldKey, nodeText.fieldKey)
        onEditingFinished: editor.commitNodeValue(nodeText.fieldKey, String(text))
        Keys.onEscapePressed: function(event) {
            event.accepted = true;
            nodeText.text = String(editor.nodeValue(nodeText.fieldKey, ""));
        }
        onRevisionChanged: {
            if (!activeFocus)
                nodeText.text = String(editor.nodeValue(nodeText.fieldKey, ""));
        }
        background: Rectangle {
            color: editor.fieldColor
            border.color: nodeText.activeFocus ? editor.accentColor : editor.borderColor
            radius: editor.smallRadiusValue
        }
        MouseArea {
            anchors.fill: parent
            onPressed: function(mouse) {
                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
            }
            onClicked: editor.keyNodeAtFrame(nodeText.fieldKey)
        }
        ToolTip.visible: hovered
        ToolTip.text: editor.nodeLabel(nodeText.fieldKey, nodeText.fieldKey) + " (Alt-click to key at this frame)"
    }

    // One NODE choice parameter (clip) through the shared combo and gesture.
    component NodeChoice: StudioComboBox {
        id: nodeChoice
        required property string fieldKey
        property int revision: editor.panelRevision
        objectName: "rotoChoice_" + editor.nodeId + "_" + nodeChoice.fieldKey
        theme: editor.theme
        implicitHeight: 23
        enabled: editor.hasNodeKey(nodeChoice.fieldKey)
        model: editor.nodeChoices(nodeChoice.fieldKey)
        currentIndex: editor.nodeChoiceIndex(nodeChoice.fieldKey)
        function restoreSelection() {
            currentIndex = Qt.binding(function() {
                return editor.nodeChoiceIndex(nodeChoice.fieldKey);
            });
        }
        onRevisionChanged: restoreSelection()
        onModelChanged: Qt.callLater(restoreSelection)
        onActivated: {
            editor.commitNodeValue(nodeChoice.fieldKey, String(model[currentIndex]));
            restoreSelection();
        }
        Accessible.name: editor.nodeLabel(nodeChoice.fieldKey, nodeChoice.fieldKey)
        // The shared keying shortcut: only an Alt-press reaches the handler, so
        // an ordinary click still opens the choice list.
        MouseArea {
            anchors.fill: parent
            onPressed: function(mouse) {
                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
            }
            onClicked: editor.keyNodeAtFrame(nodeChoice.fieldKey)
        }
        ToolTip.visible: hovered
        ToolTip.text: editor.nodeLabel(nodeChoice.fieldKey, nodeChoice.fieldKey) + " (Alt-click to key at this frame)"
    }

    // --- node output controls (flat, top) ----------------------------------
    // The node's own controls, in the reference's compact order: master opacity,
    // then the channel it writes, the clip rule and the input mask. Every row is
    // the host's gesture and key machinery, so keying and undo are identical to
    // an ordinary parameter row.
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: "master"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: editor.captionWidth
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            ToolTip.visible: masterHover.hovered
            ToolTip.text: "Master opacity: the node's global opacity over the composited matte. The selected shape's own opacity is below."
            HoverHandler {
                id: masterHover
            }
        }
        Loader {
            Layout.fillWidth: true
            sourceComponent: editor.panel ? editor.panel.numericEditorComponent : null
            onLoaded: {
                item.theme = editor.theme;
                item.panel = editor.panel;
                item.row = opacityRow;
                item.compact = true;
                item.fieldFirst = true;
            }
        }
        QtObject {
            id: opacityRow
            readonly property string nodeId: editor.nodeId
            readonly property string parameterKey: "opacity"
            readonly property string rowLabel: "Master Opacity"
            readonly property real numberValue: editor.parameter && editor.parameter.value !== undefined && editor.parameter.value !== null && editor.parameter.value.length === undefined ? Number(editor.parameter.value) : 1
            readonly property bool hasMinimum: true
            readonly property bool hasMaximum: true
            readonly property real minimum: 0
            readonly property real maximum: 1
            readonly property bool hasSoftMinimum: true
            readonly property bool hasSoftMaximum: true
            readonly property real softMinimum: 0
            readonly property real softMaximum: 1
            readonly property real numberStep: editor.parameter && editor.parameter.step !== undefined ? Number(editor.parameter.step) : 0.01
            readonly property int decimals: -1
            readonly property bool integerParameter: false
            readonly property string exactText: editor.parameter && editor.parameter.valueText !== undefined ? String(editor.parameter.valueText) : ""
            readonly property int dragThreshold: editor.dragThreshold
            readonly property string rowError: editor.panel && editor.panel.gestureErrorKey === "opacity" ? String(editor.panel.gestureError) : ""
            function rowRef() {
                return {
                    "networkId": editor.networkId,
                    "nodeId": editor.nodeId,
                    "parameterKey": "opacity",
                    "parameter": editor.parameter,
                    "label": "Master Opacity"
                };
            }
            function commitText(text) {
                return editor.panel.gestureText(rowRef(), text);
            }
            function commitDiscrete(value) {
                return editor.panel.gestureSingle(rowRef(), value);
            }
            function keyAtFrame() {
                return editor.panel.keyParameterAtFrame(editor.networkId, editor.nodeId, "opacity");
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: editor.hasNodeKey("outputChannel") || editor.hasNodeKey("replace")
        Text {
            text: "channel"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: editor.captionWidth
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        NodeTextField {
            visible: editor.hasNodeKey("outputChannel")
            fieldKey: "outputChannel"
        }
        NodeFlag {
            visible: editor.hasNodeKey("replace")
            fieldKey: "replace"
            fieldLabel: "replace"
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: editor.hasNodeKey("clip")
        Text {
            text: "clip"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: editor.captionWidth
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        NodeChoice {
            fieldKey: "clip"
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: editor.hasNodeKey("maskChannel") || editor.hasNodeKey("invertMask")
        Text {
            text: "mask"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: editor.captionWidth
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        NodeTextField {
            visible: editor.hasNodeKey("maskChannel")
            fieldKey: "maskChannel"
        }
        NodeFlag {
            visible: editor.hasNodeKey("invertMask")
            fieldKey: "invertMask"
            fieldLabel: "invert"
        }
    }

    // --- the selected shape's properties (flat) ----------------------------
    // These rows belong to the ACTIVE shape: its name is the row that owns them,
    // and a multi-shape selection states the count beside it instead of letting
    // one shape's numbers look like a batch value.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: editor.hasSelection

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            // TextInput itself declares no vertical alignment, so the compact
            // row height is owned by a wrapper and the field is centred inside
            // it, keeping the property-row grid without new chrome.
            Item {
                Layout.fillWidth: true
                implicitHeight: editor.rowHeight
                TextInput {
                    id: nameField
                    objectName: "rotoElementName_" + editor.nodeId
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        var found = editor.selectedRecord;
                        return found ? String(found.name) : "";
                    }
                    color: editor.textColor
                    font.pixelSize: editor.fontSizeValue
                    selectByMouse: true
                    enabled: !editor.selectedIsLocked
                    Accessible.name: editor.selectedIsGroup ? "Group name" : "Shape name"
                    ToolTip.visible: nameHover.hovered && editor.selectionCount > 1
                    ToolTip.text: "Every property below belongs to this shape, the active one of " + editor.selectionCount + " selected."
                    HoverHandler {
                        id: nameHover
                    }
                    onEditingFinished: {
                        if (editor.roto && text.length > 0)
                            editor.roto.renameElement(editor.selectedElement, text);
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: editor.borderColor
                    }
                }
            }
            Text {
                objectName: "rotoSelectionBadge_" + editor.nodeId
                visible: editor.selectionBadge.length > 0
                text: editor.selectionBadge
                color: editor.accentColor
                font.pixelSize: editor.smallFontSize
                verticalAlignment: Text.AlignVCenter
                ToolTip.visible: selectionHover.hovered
                ToolTip.text: "Several shapes are selected. These properties are the active shape; the hierarchy marks the whole selection."
                HoverHandler {
                    id: selectionHover
                }
            }
        }

        ScalarRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "opacity"
            fieldLabel: "opacity"
        }
        ScalarRow {
            visible: !editor.selectedIsGroup
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "feather"
            fieldLabel: "feather"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: !editor.selectedIsGroup
            ScalarRow {
                elementId: editor.selectedElement
                pointId: ""
                fieldKey: "featherFalloff"
                fieldLabel: "falloff"
                Layout.fillWidth: true
            }
            Loader {
                Layout.preferredWidth: 76
                sourceComponent: editor.roto !== null ? profileComboComponent : null
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            ElementFlag {
                visible: !editor.selectedIsGroup
                fieldKey: "featherEnabled"
                fieldLabel: "feather on"
            }
            ElementFlag {
                fieldKey: "inverted"
                fieldLabel: "invert"
            }
        }

        // The explicit spline keys of the current selection: Key/Remove at the
        // current frame plus the keyed times, which the arrows walk through the
        // group's own clock. No silent autokey exists on this surface.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                text: "spline"
                color: editor.textColor
                font.pixelSize: editor.fontSizeValue
                Layout.preferredWidth: editor.captionWidth
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                ToolTip.visible: splineHover.hovered
                ToolTip.text: "Geometry keys of the selection: point position, tangents, tension and feather at the current frame."
                HoverHandler {
                    id: splineHover
                }
            }
            Rectangle {
                objectName: "rotoSplineMarker_" + editor.nodeId
                width: 6
                height: 6
                radius: 3
                Layout.alignment: Qt.AlignVCenter
                color: editor.keyedAtFrame ? editor.accentColor : editor.borderColor
                border.color: editor.borderColor
                Accessible.name: editor.keyedAtFrame ? "Geometry keyed at this frame" : "No geometry key at this frame"
            }
            Text {
                objectName: "rotoSplineTimes_" + editor.nodeId
                Layout.fillWidth: true
                Layout.minimumWidth: 30
                text: editor.keyTimesText
                color: editor.keyTimes.length > 0 ? editor.textColor : editor.mutedColor
                font.pixelSize: editor.smallFontSize
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                ToolTip.visible: splineTimesHover.hovered
                ToolTip.text: editor.keyTimesText
                HoverHandler {
                    id: splineTimesHover
                }
            }
            Action {
                objectName: "rotoKeyPrev_" + editor.nodeId
                text: "\u2039"
                implicitWidth: 20
                enabled: editor.keyTimes.length > 0
                Accessible.name: "Go to the previous keyed frame"
                onClicked: editor.jumpToKey(false)
            }
            Action {
                objectName: "rotoKeyNext_" + editor.nodeId
                text: "\u203a"
                implicitWidth: 20
                enabled: editor.keyTimes.length > 0
                Accessible.name: "Go to the next keyed frame"
                onClicked: editor.jumpToKey(true)
            }
            Action {
                objectName: "rotoKeySet_" + editor.nodeId
                text: "key"
                enabled: editor.elementEditable()
                Accessible.name: "Key the selected geometry at this frame"
                onClicked: editor.toggleGeometryKey(false)
                ToolTip.visible: hovered
                ToolTip.text: "Key the selected points' position, tangents, tension and feather at this frame"
            }
            Action {
                objectName: "rotoKeyRemove_" + editor.nodeId
                text: "del"
                enabled: editor.elementEditable()
                Accessible.name: "Remove the geometry keys at this frame"
                onClicked: editor.toggleGeometryKey(true)
                ToolTip.visible: hovered
                ToolTip.text: "Remove the selected geometry's keys at this frame"
            }
        }
    }

    // --- hierarchy (dominant surface) --------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 3
        SectionHeader {
            Layout.fillWidth: true
            sectionKey: "shapes"
            title: "Shapes (" + editor.elements.length + ")"
            expanded: editor.treeExpanded
            onSectionToggled: editor.treeExpanded = !editor.treeExpanded
        }
        Action {
            objectName: "rotoAddGroup_" + editor.nodeId
            text: "+"
            implicitWidth: 21
            enabled: editor.roto !== null && editor.roto.available === true
            Accessible.name: "Add a group"
            onClicked: editor.addGroup()
            ToolTip.visible: hovered
            ToolTip.text: "Add a group; a selected group owns the new group"
        }
        Action {
            objectName: "rotoRemove_" + editor.nodeId
            text: "\u2212"
            implicitWidth: 21
            enabled: editor.selectionCount > 0 && !editor.selectedIsLocked
            Accessible.name: "Remove the selected shapes"
            onClicked: editor.removeShapes()
            ToolTip.visible: hovered
            ToolTip.text: "Remove the selected shapes and everything they own"
        }
    }

    Rectangle {
        Layout.fillWidth: true
        visible: editor.treeExpanded
        Layout.preferredHeight: Math.min(230, Math.max(136, editor.visibleRows.length * editor.rowHeight + 2))
        color: editor.fieldColor
        radius: editor.smallRadiusValue
        border.color: editor.borderColor
        clip: true

        ListView {
            id: tree
            objectName: "rotoTree_" + editor.nodeId
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: editor.visibleRows
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                id: treeRow
                required property var modelData
                readonly property bool group: treeRow.modelData.group === true
                readonly property string elementName: String(treeRow.modelData.name)
                readonly property bool isSelected: editor.elementSelected(String(treeRow.modelData.id))
                readonly property bool hasChildren: editor.childCounts[String(treeRow.modelData.id)] !== undefined
                width: tree.width
                height: editor.rowHeight
                color: treeRow.isSelected ? editor.raisedColor : (rowHover.hovered ? editor.hoverColor : "transparent")
                HoverHandler {
                    id: rowHover
                }
                MouseArea {
                    id: rowSelect
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onClicked: function(mouse) {
                        editor.selectRow(treeRow.modelData, mouse.modifiers);
                    }
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    color: treeRow.isSelected ? editor.accentColor : "transparent"
                }

                // Drag grip: reordering and reparenting the hierarchy, which is
                // the one structural edit this list owns. It stays a narrow
                // grip so a drag elsewhere still scrolls the list, and a press
                // that never moves is a plain row selection again. The fold
                // button is declared above it, so folding stays clickable.
                MouseArea {
                    id: grip
                    property bool moved: false
                    width: 24
                    height: parent.height
                    anchors.left: parent.left
                    anchors.leftMargin: 2
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true
                    cursorShape: Qt.SizeAllCursor
                    onPressed: {
                        grip.moved = false;
                        editor.dragId = String(treeRow.modelData.id);
                        editor.dropRow = -1;
                        editor.dropInto = false;
                        editor.autoScrollStep = 0;
                    }
                    onPositionChanged: function(mouse) {
                        if (editor.dragId.length === 0)
                            return;
                        grip.moved = true;
                        editor.trackDrag(treeRow.y + mouse.y - tree.contentY);
                    }
                    onReleased: function(mouse) {
                        if (!grip.moved) {
                            editor.cancelDrag();
                            editor.selectRow(treeRow.modelData, mouse.modifiers);
                            return;
                        }
                        editor.commitDrag();
                    }
                    onCanceled: editor.cancelDrag()
                    ToolTip.visible: containsMouse && editor.dragId.length === 0
                    ToolTip.text: "Drag to reorder or move this element into a group"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    spacing: 1
                    Item {
                        Layout.preferredWidth: 10 * Number(treeRow.modelData.depth)
                        Layout.fillHeight: true
                    }
                    Action {
                        objectName: "rotoFold_" + editor.nodeId + "_" + treeRow.modelData.id
                        visible: treeRow.group && treeRow.hasChildren
                        implicitWidth: 13
                        text: editor.groupCollapsed(String(treeRow.modelData.id)) ? "\u25b8" : "\u25be"
                        Accessible.name: (editor.groupCollapsed(String(treeRow.modelData.id)) ? "Expand " : "Collapse ") + treeRow.elementName
                        onClicked: editor.toggleGroup(String(treeRow.modelData.id))
                    }
                    Text {
                        text: treeRow.group ? "\u25a3" : "\u25cb"
                        color: editor.mutedColor
                        font.pixelSize: editor.smallFontSize
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        objectName: "rotoName_" + editor.nodeId + "_" + treeRow.modelData.id
                        text: treeRow.elementName
                        color: treeRow.elementName.length > 0 ? editor.textColor : editor.mutedColor
                        font.pixelSize: editor.fontSizeValue
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.minimumWidth: 24
                        verticalAlignment: Text.AlignVCenter
                    }
                    Action {
                        objectName: "rotoVisible_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 17
                        text: treeRow.modelData.visible === true ? "\u25c9" : "\u25cb"
                        Accessible.name: "Visibility of " + treeRow.elementName
                        onClicked: editor.toggleVisible(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Toggle this element's visibility at the current frame"
                    }
                    Action {
                        objectName: "rotoLock_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 17
                        text: treeRow.modelData.locked === true ? "\u2298" : "\u2013"
                        Accessible.name: "Lock of " + treeRow.elementName
                        onClicked: editor.toggleLocked(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Lock or unlock this element; a locked element accepts no edit"
                    }
                    Action {
                        objectName: "rotoInvert_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 17
                        text: treeRow.modelData.inverted === true ? "\u25a0" : "\u25a1"
                        Accessible.name: "Invert of " + treeRow.elementName
                        onClicked: editor.toggleInverted(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Invert this element's contribution to the accumulated matte"
                    }
                    Action {
                        objectName: "rotoBlend_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 17
                        text: String(treeRow.modelData.blend) === "intersect" ? "\u2229"
                              : String(treeRow.modelData.blend) === "subtract" ? "\u2212" : "\u222a"
                        Accessible.name: "Blend operation of " + treeRow.elementName
                        onClicked: editor.cycleBlend(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Combine, intersect or subtract against the accumulated matte"
                    }
                }
            }
        }
        // The insertion point of a live hierarchy drag: a line before a row, or
        // an outlined row when the element is about to become its group's first
        // child.
        Rectangle {
            objectName: "rotoDrop_" + editor.nodeId
            visible: editor.dragId.length > 0 && editor.dropRow >= 0 && !editor.dropInto
            x: 1
            y: editor.dropRow * editor.rowHeight - tree.contentY
            width: parent.width - 2
            height: 2
            color: editor.accentColor
        }
        Rectangle {
            objectName: "rotoDropInto_" + editor.nodeId
            visible: editor.dragId.length > 0 && editor.dropRow >= 0 && editor.dropInto
            x: 1
            y: editor.dropRow * editor.rowHeight - tree.contentY
            width: parent.width - 2
            height: editor.rowHeight
            color: "transparent"
            border.color: editor.accentColor
            radius: editor.smallRadiusValue
        }
        Text {
            anchors.centerIn: parent
            visible: editor.elements.length === 0
            text: editor.roto && editor.roto.available === true ? "no shapes" : "no Roto here"
            color: editor.mutedColor
            font.pixelSize: editor.smallFontSize
        }
    }

    // --- points of the selection (folded) ----------------------------------
    // Folded by default: the viewport overlay is where points are manipulated,
    // so this section only states the selection, offers the shared refinement
    // actions and, for exactly ONE selected point, its exact numbers.
    SectionHeader {
        Layout.fillWidth: true
        visible: editor.hasSelection && !editor.selectedIsGroup
        sectionKey: "points"
        title: "Points" + (editor.pointSelectionCount > 0 ? " (" + editor.pointSelectionCount + ")" : "")
        expanded: editor.pointsExpanded
        onSectionToggled: editor.pointsExpanded = !editor.pointsExpanded
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: editor.hasSelection && !editor.selectedIsGroup && editor.pointsExpanded

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                objectName: "rotoPointSelection_" + editor.nodeId
                Layout.fillWidth: true
                text: {
                    var total = editor.points.length;
                    if (editor.pointSelectionCount === 0)
                        return total + " points, none selected";
                    return editor.pointSelectionCount + " of " + total + " points selected";
                }
                color: editor.pointSelectionCount > 0 ? editor.textColor : editor.mutedColor
                font.pixelSize: editor.smallFontSize
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            Action {
                objectName: "rotoSelectAllPoints_" + editor.nodeId
                text: "all"
                enabled: editor.elementEditable() && editor.points.length > 0
                Accessible.name: "Select every point of the selected contour"
                onClicked: editor.selectAllPoints()
            }
            Action {
                objectName: "rotoRemovePoint_" + editor.nodeId
                text: "\u2212"
                implicitWidth: 21
                enabled: editor.elementEditable() && editor.pointSelectionCount > 0
                Accessible.name: "Remove the selected points"
                onClicked: editor.removeSelection()
                ToolTip.visible: hovered
                ToolTip.text: "Remove the selected points from this contour"
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Action {
                objectName: "rotoSmooth_" + editor.nodeId
                text: "smooth"
                Layout.fillWidth: true
                enabled: editor.elementEditable()
                Accessible.name: "Smooth the selected points"
                onClicked: editor.smoothSelection(true)
                ToolTip.visible: hovered
                ToolTip.text: "Mirror the handles of the selected points; with no point selected, of the whole contour (B-spline: tension 0)"
            }
            Action {
                objectName: "rotoCusp_" + editor.nodeId
                text: "cusp"
                Layout.fillWidth: true
                enabled: editor.elementEditable()
                Accessible.name: "Make the selected points cusps"
                onClicked: editor.smoothSelection(false)
                ToolTip.visible: hovered
                ToolTip.text: "Break the handles of the selected points; with no point selected, of the whole contour (B-spline: tension 1)"
            }
        }
        // Precision: the exact numbers of the ONE selected point, so a typed
        // coordinate stays available without an all-points table.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            visible: editor.primaryPoint !== null
            Text {
                objectName: "rotoPointPrecision_" + editor.nodeId
                Layout.fillWidth: true
                text: "point " + (editor.primaryPoint ? String(Number(editor.primaryPoint.index) + 1) : "")
                color: editor.mutedColor
                font.pixelSize: editor.smallFontSize
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            VectorRow {
                elementId: editor.precisionElement
                pointId: editor.precisionPoint
                fieldKey: "position"
                fieldLabel: "move"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                ScalarRow {
                    elementId: editor.precisionElement
                    pointId: editor.precisionPoint
                    fieldKey: "feather"
                    fieldLabel: "feather"
                    Layout.fillWidth: true
                }
                ScalarRow {
                    elementId: editor.precisionElement
                    pointId: editor.precisionPoint
                    fieldKey: "tension"
                    fieldLabel: "tension"
                    Layout.fillWidth: true
                }
            }
        }
    }

    // --- secondary: transform, lifetime, motion blur -----------------------
    SectionHeader {
        Layout.fillWidth: true
        visible: editor.hasSelection
        sectionKey: "transform"
        title: "Transform"
        expanded: editor.transformExpanded
        onSectionToggled: editor.transformExpanded = !editor.transformExpanded
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: editor.hasSelection && editor.transformExpanded
        VectorRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "translation"
            fieldLabel: "move"
        }
        VectorRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "scale"
            fieldLabel: "scale"
        }
        VectorRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "pivot"
            fieldLabel: "pivot"
        }
        ScalarRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "rotation"
            fieldLabel: "rotate"
        }
    }

    SectionHeader {
        Layout.fillWidth: true
        visible: editor.hasSelection
        sectionKey: "lifetime"
        title: "Lifetime"
        expanded: editor.lifetimeExpanded
        onSectionToggled: editor.lifetimeExpanded = !editor.lifetimeExpanded
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: editor.hasSelection && editor.lifetimeExpanded
        Text {
            text: "frames"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: editor.captionWidth
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            ToolTip.visible: lifetimeHover.hovered
            ToolTip.text: "Inclusive document-local frames this element contributes in; both bounds empty means unbounded."
            HoverHandler {
                id: lifetimeHover
            }
        }
        NumericField {
            id: firstFrameField
            objectName: "rotoFirstFrame_" + editor.nodeId
            theme: editor.theme
            value: editor.lifetimeBound(true)
            valueAvailable: {
                var record = editor.selectedRecord;
                return record && record.firstFrame !== undefined && record.firstFrame !== null;
            }
            hasMinimum: false
            hasMaximum: false
            hasSoftMinimum: false
            hasSoftMaximum: false
            step: 1
            label: "First frame"
            dragThreshold: editor.dragThreshold
            fieldWidth: 54
            Layout.fillWidth: true
            Layout.minimumWidth: 34
            enabled: !editor.selectedIsLocked
            interactionOwner: editor.roto
            onTextCommitted: function(text) {
                editor.roto.setElementProperty(editor.selectedElement, "firstFrame", Number(text));
            }
            onStepped: function(value) {
                editor.roto.setElementProperty(editor.selectedElement, "firstFrame", value);
            }
        }
        NumericField {
            id: lastFrameField
            objectName: "rotoLastFrame_" + editor.nodeId
            theme: editor.theme
            value: editor.lifetimeBound(false)
            valueAvailable: {
                var record = editor.selectedRecord;
                return record && record.lastFrame !== undefined && record.lastFrame !== null;
            }
            hasMinimum: false
            hasMaximum: false
            hasSoftMinimum: false
            hasSoftMaximum: false
            step: 1
            label: "Last frame"
            dragThreshold: editor.dragThreshold
            fieldWidth: 54
            Layout.fillWidth: true
            Layout.minimumWidth: 34
            enabled: !editor.selectedIsLocked
            interactionOwner: editor.roto
            onTextCommitted: function(text) {
                editor.roto.setElementProperty(editor.selectedElement, "lastFrame", Number(text));
            }
            onStepped: function(value) {
                editor.roto.setElementProperty(editor.selectedElement, "lastFrame", value);
            }
        }
        Action {
            objectName: "rotoClearLifetime_" + editor.nodeId
            text: "\u00d7"
            implicitWidth: 21
            Accessible.name: "Clear the lifetime bounds"
            enabled: !editor.selectedIsLocked
            onClicked: editor.roto.clearLifetime(editor.selectedElement)
            ToolTip.visible: hovered
            ToolTip.text: "Clear both bounds; the element is unbounded again"
        }
    }

    SectionHeader {
        Layout.fillWidth: true
        visible: editor.hasNodeKey("shutter") || editor.hasNodeKey("samples")
        sectionKey: "motionBlur"
        title: "Motion Blur"
        expanded: editor.motionBlurExpanded
        onSectionToggled: editor.motionBlurExpanded = !editor.motionBlurExpanded
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: (editor.hasNodeKey("shutter") || editor.hasNodeKey("samples")) && editor.motionBlurExpanded
        NodeNumberRow {
            visible: editor.hasNodeKey("shutter")
            fieldKey: "shutter"
            caption: "shutter"
        }
        NodeNumberRow {
            visible: editor.hasNodeKey("samples")
            fieldKey: "samples"
            caption: "samples"
        }
    }

    Text {
        objectName: "rotoEditorError_" + editor.nodeId
        visible: editor.errorText().length > 0
        Layout.fillWidth: true
        text: editor.errorText()
        color: editor.errorColor
        font.pixelSize: editor.smallFontSize
        elide: Text.ElideRight
        wrapMode: Text.WordWrap
        Accessible.name: editor.errorText()
    }

    Component {
        id: profileComboComponent
        StudioComboBox {
            id: profileCombo
            theme: editor.theme
            implicitHeight: 23
            model: ["linear", "smooth"]
            currentIndex: editor.choiceValue(editor.selectedElement, "", "featherProfile") === "smooth" ? 1 : 0
            function restoreSelection() {
                currentIndex = Qt.binding(function() {
                    return editor.choiceValue(editor.selectedElement, "", "featherProfile") === "smooth" ? 1 : 0;
                });
            }
            onModelChanged: Qt.callLater(restoreSelection)
            onActivated: {
                if (editor.roto)
                    editor.roto.setElementProperty(editor.selectedElement, "featherProfile", String(model[currentIndex]));
                restoreSelection();
            }
            Accessible.name: "Feather profile"
            ToolTip.visible: hovered
            ToolTip.text: "Linear or smooth feather ramp"
        }
    }

    readonly property var cellTheme: ({
            "text": editor.textColor,
            "muted": editor.mutedColor,
            "accent": editor.accentColor,
            "border": editor.borderColor,
            "hover": editor.hoverColor,
            "field": editor.fieldColor,
            "panel": editor.panelColor,
            "raised": editor.raisedColor,
            "disabled": editor.disabledColor,
            "errorText": editor.errorColor,
            "smallRadius": editor.smallRadiusValue,
            "fontSize": editor.fontSizeValue
        })
}
