import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Schema-driven parameter inspector host. Every catalog parameter is rendered
// from its schema `kind`; there is no effect-name switch and no Document or GPU
// access. Value edits and keying are routed through ViewerController commands;
// card arrangement (open/pin/collapse/columns/limit) is panel state persisted
// per panel id through WorkspaceController.
FocusScope {
    id: parametersPanel

    objectName: "parametersPanel"
    clip: true

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var theme: null
    property var workspace: null

    // Context properties installed by main.cpp. `parameterEditors` hosts
    // namespaced custom parameter editors; the generic schema controls remain
    // usable when no editor is registered.
    readonly property var controller: viewerController
    // Registered section editors reuse the same numeric/slider gesture owner.
    readonly property Component numericEditorComponent: numericControlComponent

    // Accumulated inspectors, newest first. Identity is (network, node), never
    // the node id alone, so the same node id in two networks stays distinct.
    property var inspectors: []
    property int inspectorLimit: 3
    property bool twoColumns: true
    // Two columns are shown only when both minimum-width cards fit inside the
    // scroll viewport. The saved `twoColumns` preference is preserved; a narrow
    // panel overrides it for display only and never rewrites it. A live
    // parameter gesture holds the reflow until it settles, so the card under
    // the pointer is never repacked mid-drag; the packing itself preserves card
    // identity, so an in-progress text edit and an editor's expansion state
    // survive the change either way.
    readonly property bool columnsFit: twoColumns
                                      && inspectorScroll.availableWidth >= 2 * inspectorContent.minCardWidth
                                                                           + inspectorContent.columnGap
    property bool effectiveTwoColumns: false
    onColumnsFitChanged: if (activeToken.length === 0) effectiveTwoColumns = columnsFit
    onActiveTokenChanged: if (activeToken.length === 0) effectiveTwoColumns = columnsFit
    property string restoredForPanelId: ""
    property bool stateReady: false
    property bool savingState: false
    readonly property string persistedStateJson: JSON.stringify([panelState.inspectors || [], panelState.limit, panelState.columns])

    // One global revision drives inspector re-queries. It advances on document,
    // frame, and catalog changes. A refresh requested mid-gesture is deferred so
    // the delegate under an active drag or edit is never rebuilt underneath it.
    property int revision: 0
    property bool refreshPending: false

    // One parameter interaction is live in the session at a time, owned by the
    // control that began it. The panel tracks the token it last accepted and
    // the row it belongs to, so it can name a rejected edit and keep its own
    // presentation; a later begin retires the previous owner through the
    // controller's cancellation path instead of being refused.
    property string activeToken: ""
    property var activeRow: null
    // Accepted UI-keyed values let section editors follow numeric previews
    // without rebuilding the inspector or becoming a second gesture owner.
    signal editPreviewed(string token, var values)
    // The most recent rejected edit, attributed to one row. It is presentation
    // state only; the controller/catalog remain the validation authority, and
    // the row hands it to the control's own error affordance rather than
    // printing it into the inspector.
    property string gestureError: ""
    property string gestureErrorKey: ""

    // The single live parameter gesture is this panel's cancellable authored
    // edit: a preview-only Undo cancels it instead of touching the document.
    readonly property bool historyGestureActive: activeToken.length > 0
    onHistoryGestureActiveChanged: syncHistoryGesture()

    property alias headerTools: headerToolsComponent

    // --- identity helpers -------------------------------------------------
    function validIdentity(value) {
        return value !== undefined && value !== null && String(value).length > 0;
    }

    function normalizedEntry(entry) {
        return {
            "network": String(entry.network),
            "node": String(entry.node),
            "pinned": entry.pinned === true,
            "collapsed": entry.collapsed === true
        };
    }

    function entryIndex(network, node) {
        for (var i = 0; i < inspectors.length; ++i) {
            if (String(inspectors[i].network) === String(network) && String(inspectors[i].node) === String(node))
                return i;
        }
        return -1;
    }

    // Walk the item tree to the owning inspector card. Repeater delegates are
    // file-level components, so the card is resolved through the parent chain.
    function owningCard(item) {
        var current = item;
        while (current) {
            if (current.inspectorId !== undefined)
                return current;
            current = current.parent;
        }
        return null;
    }

    function parameterByKey(inspector, key) {
        var sections = inspector && inspector.sections ? inspector.sections : [];
        for (var i = 0; i < sections.length; ++i) {
            var params = sections[i].parameters || [];
            for (var j = 0; j < params.length; ++j) {
                if (String(params[j].key) === String(key))
                    return params[j];
            }
        }
        return null;
    }

    // Aggregate editors address sibling parameters on one node. An exposed
    // control addresses only its public interface key, so it keeps the ordinary
    // typed control instead of exposing unrelated definition parameters.
    function editorApplies(parameter, info) {
        return !info || String(info.presentation) !== "section"
                || !parameter || parameter.targetKey === undefined
                || String(parameter.key) === String(parameter.targetKey);
    }

    // Visibility is effective through the parent chain: a panel body inside a
    // hidden dock or inactive tab must not consume inspector requests.
    function effectivelyVisible() {
        var item = parametersPanel;
        while (item) {
            if (!item.visible)
                return false;
            item = item.parent;
        }
        return true;
    }

    // --- persistence ------------------------------------------------------
    function stateEntries() {
        var result = [];
        for (var i = 0; i < inspectors.length; ++i) {
            var entry = inspectors[i];
            result.push({
                    "network": String(entry.network),
                    "node": String(entry.node),
                    "pinned": entry.pinned === true,
                    "collapsed": entry.collapsed === true
                });
        }
        return result;
    }

    function panelStateCopy() {
        var source = panelState || ({});
        if (workspace && panelId && workspace.panelState)
            source = workspace.panelState(panelId) || source;
        var copy = {};
        for (var key in source)
            copy[key] = source[key];
        return copy;
    }

    // Merge inspector arrangement into the existing panel state so routing
    // fields owned by PanelContextRouter (viewerRole) are preserved.
    function saveState() {
        if (!workspace || !workspace.setPanelState || !panelId || !stateReady)
            return;
        var merged = panelStateCopy();
        merged.inspectors = stateEntries();
        merged.limit = inspectorLimit;
        merged.columns = twoColumns;
        if (JSON.stringify(merged) === JSON.stringify(panelStateCopy()))
            return;
        savingState = true;
        try {
            workspace.setPanelState(panelId, merged);
        } finally {
            savingState = false;
        }
    }

    function restoreState() {
        if (!panelId || restoredForPanelId === panelId)
            return;
        restoredForPanelId = panelId;
        stateReady = false;
        var saved = panelStateCopy();
        var loaded = [];
        var list = saved.inspectors;
        if (list && list.length !== undefined) {
            for (var i = 0; i < list.length; ++i) {
                var entry = list[i];
                if (!entry || !validIdentity(entry.network) || !validIdentity(entry.node))
                    continue;
                var normalized = normalizedEntry(entry);
                var duplicate = false;
                for (var j = 0; j < loaded.length; ++j) {
                    if (String(loaded[j].network) === normalized.network && String(loaded[j].node) === normalized.node) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    loaded.push(normalized);
            }
        }
        inspectors = loaded;
        if (saved.limit !== undefined && Number.isFinite(Number(saved.limit)))
            inspectorLimit = clampLimit(saved.limit);
        else
            inspectorLimit = 3;
        twoColumns = typeof saved.columns === "boolean" ? saved.columns : true;
        enforceLimit();
        stateReady = true;
    }

    // --- accumulation -----------------------------------------------------
    function clampLimit(value) {
        return Math.max(1, Math.min(20, Math.round(Number(value))));
    }

    function removeAt(index) {
        if (index < 0 || index >= inspectors.length)
            return;
        var next = inspectors.slice();
        next.splice(index, 1);
        inspectors = next;
    }

    // Oldest unpinned entry, scanned from the end of newest-first order.
    function oldestUnpinned() {
        for (var i = inspectors.length - 1; i >= 0; --i) {
            if (inspectors[i].pinned !== true)
                return i;
        }
        return -1;
    }

    function enforceLimit() {
        var unpinned = 0;
        for (var i = 0; i < inspectors.length; ++i) {
            if (inspectors[i].pinned !== true)
                ++unpinned;
        }
        while (unpinned > inspectorLimit) {
            var index = oldestUnpinned();
            if (index < 0)
                break;
            removeAt(index);
            --unpinned;
        }
    }

    function openInspector(network, node) {
        if (!validIdentity(network) || !validIdentity(node))
            return;
        var next = inspectors.slice();
        if (entryIndex(network, node) < 0) {
            next.unshift({
                    "network": String(network),
                    "node": String(node),
                    "pinned": false,
                    "collapsed": false
                });
            inspectors = next;
            enforceLimit();
            saveState();
        }
        revealTop();
    }

    function closeInspector(network, node) {
        var index = entryIndex(network, node);
        if (index < 0)
            return;
        removeAt(index);
        saveState();
    }

    function closeAllInspectors() {
        if (inspectors.length === 0)
            return;
        inspectors = [];
        saveState();
    }

    function setPinned(network, node, pinned) {
        var next = [];
        for (var i = 0; i < inspectors.length; ++i) {
            var current = inspectors[i];
            var copy = normalizedEntry(current);
            if (String(current.network) === String(network) && String(current.node) === String(node))
                copy.pinned = pinned === true;
            next.push(copy);
        }
        inspectors = next;
        if (!pinned)
            enforceLimit();
        saveState();
    }

    function setCollapsed(network, node, collapsed) {
        var next = [];
        for (var i = 0; i < inspectors.length; ++i) {
            var current = inspectors[i];
            var copy = normalizedEntry(current);
            if (String(current.network) === String(network) && String(current.node) === String(node))
                copy.collapsed = collapsed === true;
            next.push(copy);
        }
        inspectors = next;
        saveState();
    }

    function setLimit(value) {
        inspectorLimit = clampLimit(value);
        enforceLimit();
        saveState();
    }

    function setTwoColumns(value) {
        twoColumns = value === true;
        saveState();
    }

    function revealTop() {
        Qt.callLater(function () {
                if (inspectorScroll.contentItem)
                    inspectorScroll.contentItem.contentY = 0;
            });
    }

    // --- refresh ----------------------------------------------------------
    function requestRefresh() {
        if (activeToken.length > 0) {
            refreshPending = true;
            return;
        }
        revision++;
    }

    function flushRefresh() {
        refreshPending = false;
        revision++;
    }

    // --- gestures ---------------------------------------------------------

    // Identity-scoped begin, also usable by registered custom editors. The
    // controller retires whatever interaction was live first, so beginning a
    // second one never leaves the session locked; the returned token belongs to
    // THIS caller and is the only authority its later updates may name.
    function beginEditFor(networkId, nodeId, parameterKey) {
        if (!controller)
            return "";
        if (!validIdentity(networkId) || !validIdentity(nodeId) || !validIdentity(parameterKey))
            return "";
        var token = controller.beginNodeParameterEdit(String(networkId), String(nodeId), String(parameterKey));
        token = token === undefined || token === null ? "" : String(token);
        if (token.length === 0)
            return "";
        activeToken = token;
        activeRow = {
            "networkId": String(networkId),
            "nodeId": String(nodeId),
            "parameterKey": String(parameterKey)
        };
        clearGestureError();
        return token;
    }

    // Batch shape of the same panel gesture: several keys of ONE target edit
    // atomically (an animated key and a static companion together). The single
    // shapes below are the one-element case of these, so both go through one
    // owner in the controller.
    function beginEditForMany(networkId, nodeId, parameterKeys) {
        if (!controller)
            return "";
        if (!validIdentity(networkId) || !validIdentity(nodeId) || !parameterKeys || parameterKeys.length === 0)
            return "";
        var keys = [];
        for (var i = 0; i < parameterKeys.length; ++i) {
            var key = String(parameterKeys[i]);
            if (key.length === 0)
                return "";
            keys.push(key);
        }
        var token = controller.beginNodeParameterEdits(String(networkId), String(nodeId), keys);
        token = token === undefined || token === null ? "" : String(token);
        if (token.length === 0)
            return "";
        activeToken = token;
        activeRow = {
            "networkId": String(networkId),
            "nodeId": String(nodeId),
            "parameterKey": keys[0]
        };
        clearGestureError();
        return token;
    }

    // Every preview, commit and cancel names the interaction it belongs to. A
    // token that is not the live one was retired by a successor: it is ignored,
    // so no stale control can preview into, commit or cancel the replacement.
    function updateEditMany(token, values) {
        if (String(token).length === 0 || String(token) !== activeToken)
            return false;
        var result = controller.updateNodeParameterEdits(String(token), values);
        if (!result)
            recordError();
        else
            editPreviewed(String(token), values);
        return result;
    }

    function beginEdit(row) {
        return row ? beginEditFor(row.networkId, row.nodeId, row.parameterKey) : "";
    }

    function updateEdit(token, value) {
        if (String(token).length === 0 || String(token) !== activeToken)
            return false;
        if (!activeRow)
            return false;
        // The single-control API retains the resolved occurrence/exposed address.
        // A batch map instead names concrete keys on its captured target.
        var key = String(activeRow.parameterKey);
        var result = controller.updateNodeParameterEdit(String(token), value);
        if (!result) {
            recordError();
        } else {
            var values = ({});
            values[key] = value;
            editPreviewed(String(token), values);
        }
        return result;
    }

    // Publishing names the token too. The controller retires it through the same
    // notice as any cancellation, so the local state is cleared by that notice;
    // the row captured here still names the parameter in a failure message.
    function commitEdit(token) {
        if (String(token).length === 0 || String(token) !== activeToken)
            return false;
        var row = activeRow;
        var result = controller.commitNodeParameterEdit(String(token));
        retireToken(String(token));
        if (!result) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = row ? String(row.parameterKey) : "";
        }
        return result;
    }

    function cancelEdit(token) {
        if (String(token).length === 0 || String(token) !== activeToken)
            return false;
        var result = controller.cancelNodeParameterEdit(String(token));
        retireToken(String(token));
        return result;
    }

    // The controller retires a token through its cancellation path (a successor
    // interaction started, or this panel's own commit/cancel). Only matching
    // local presentation is dropped here: cancelling again would reach the
    // owner that replaced it. The re-query is queued because the notice can
    // arrive inside the owner's own call (a commit, a successor's begin) or
    // inside a document notification, and a gesture that begins before it runs
    // simply defers the refresh again instead of letting it re-enter a live
    // interaction.
    function retireToken(token) {
        var value = token === undefined || token === null ? "" : String(token);
        if (value.length === 0 || value !== activeToken)
            return false;
        activeToken = "";
        activeRow = null;
        Qt.callLater(function () { parametersPanel.requestRefresh(); });
        return true;
    }

    // Retire whatever interaction the session owns before a control starts a
    // text edit whose value is not yet known. Arming a viewport pick reserves
    // no core gesture, so this is the one path that releases it before the
    // first keystroke; discrete, key and reset commands hand off through their
    // own begin or through this call.
    function prepareParameterInteraction() {
        if (controller && controller.prepareParameterInteraction)
            controller.prepareParameterInteraction();
    }

    // Shared history routing: a preview-only Undo reaches the one row gesture
    // through its own cancellation path, so committing it stays impossible
    // (commitEdit returns false once the token is cleared).
    function cancelHistoryGesture() {
        cancelEdit(activeToken);
    }
    function syncHistoryGesture() {
        historyController.setGesture(parametersPanel, historyGestureActive);
    }

    // --- row errors -------------------------------------------------------
    function recordError() {
        if (!controller)
            return;
        gestureError = String(controller.error);
        gestureErrorKey = activeRow ? String(activeRow.parameterKey) : "";
    }

    function clearGestureError() {
        if (gestureError.length === 0 && gestureErrorKey.length === 0)
            return;
        gestureError = "";
        gestureErrorKey = "";
    }

    // Invalid typed text keeps the previous value and states which control
    // rejected it, without a second validation contract. It is never printed as
    // its own inspector row: the typed field states it.
    function rejectText(row, text) {
        gestureError = "Parameter '" + (row && row.label ? String(row.label) : (row ? row.parameterKey : "")) + "' rejects '" + text + "'";
        gestureErrorKey = row ? String(row.parameterKey) : "";
    }

    // --- gestures ---------------------------------------------------------
    function sameNumber(a, b) {
        return Math.abs(Number(a) - Number(b)) < 1e-12;
    }

    function sameValue(current, next) {
        if (current === undefined || current === null)
            return false;
        if (current.length !== undefined) {
            if (!next || next.length !== current.length)
                return false;
            for (var i = 0; i < current.length; ++i)
                if (!sameNumber(current[i], next[i]))
                    return false;
            return true;
        }
        return sameNumber(current, next);
    }

    // One discrete control action (toggle, choice, typed field, vector/color
    // component, arrow-key step) is a single begin/update/commit gesture, hence
    // one undo step. A no-op and a rejected value create no history entry. The
    // gesture is this call's own token from begin to commit, so a successor
    // interaction can never receive its update or its commit.
    function gestureSingle(row, value) {
        // Identity and parameter key are the required contract: a row may omit
        // its inspector metadata (registered editors build these rows locally),
        // in which case only the no-op comparison is skipped. Nothing here is
        // effect-specific.
        if (!row || String(row.parameterKey).length === 0)
            return false;
        clearGestureError();
        if (row.parameter && sameValue(row.parameter.value, value))
            return true;
        var token = beginEdit(row);
        if (token.length === 0) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        if (updateEdit(token, value) === false) {
            var message = controller ? String(controller.error) : "";
            cancelEdit(token);
            gestureError = message;
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        return commitEdit(token);
    }

    // Typed numeric entry keeps the exact text; the catalog parses it, so an
    // exact 64-bit integer is never rounded through a JavaScript number.
    function gestureText(row, entered) {
        if (!row || String(row.parameterKey).length === 0)
            return false;
        var text = String(entered).trim();
        if (text.length === 0)
            return false;
        clearGestureError();
        if (row.parameter && String(row.parameter.type) === "integer") {
            if (row.parameter.valueText !== undefined && String(row.parameter.valueText) === text)
                return true;
        } else if (row.parameter) {
            var numeric = Number(text);
            if (Number.isFinite(numeric) && sameValue(row.parameter.value, numeric))
                return true;
        }
        var token = beginEdit(row);
        if (token.length === 0) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        if (updateEdit(token, text) === false) {
            var message = controller ? String(controller.error) : "";
            cancelEdit(token);
            gestureError = message;
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        return commitEdit(token);
    }

    // Continuous scrub: the caller states its identity and keeps the returned
    // token, so previews, the one commit and the cancel all name the gesture
    // they belong to. A scrub that never changed the value cancels instead of
    // publishing a no-op history entry.
    function beginScrub(row) {
        clearGestureError();
        var token = beginEdit(row);
        if (token.length === 0) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = row ? String(row.parameterKey) : "";
        }
        return token;
    }

    function updateScrub(token, value) {
        return updateEdit(token, value);
    }

    function finishScrub(token) {
        return commitEdit(token);
    }

    function cancelScrub(token) {
        return cancelEdit(token);
    }

    // Reset delegates to the identity-scoped controller command: one history
    // entry, schema default, never Remove Animation. It publishes no preview of
    // its own, so it retires the live interaction first rather than editing
    // under one.
    function resetValue(row) {
        if (!controller || !row)
            return false;
        clearGestureError();
        var result = controller.resetNodeParameterEdit(String(row.networkId), String(row.nodeId), String(row.parameterKey));
        if (!result) {
            gestureError = String(controller.error);
            gestureErrorKey = String(row.parameterKey);
        }
        flushRefresh();
        return result;
    }

    // "Show in Animation" is a group-scoped presentation relay. When the group
    // has no Animation panel the action explains itself instead of repointing
    // or creating another panel. `workspace.root` is the WorkspaceController's
    // published tree property (not a call), and it is the only available
    // group->panel query: neither the workspace controller nor the context
    // router exposes one, so this panel owns the walk.
    function groupHasAnimationPanel() {
        if (!workspace || workspace.root === undefined || workspace.root === null)
            return false;
        var stack = [workspace.root];
        while (stack.length > 0) {
            var node = stack.pop();
            if (!node)
                continue;
            var panels = node.panels || [];
            for (var i = 0; i < panels.length; ++i) {
                if (String(panels[i].type) === "animation" && String(panels[i].group) === panelGroup)
                    return true;
            }
            var children = node.children || [];
            for (var c = 0; c < children.length; ++c)
                stack.push(children[c]);
        }
        return false;
    }

    function revealInAnimation(networkId, nodeId, parameterKey) {
        if (!contextRouter)
            return false;
        return contextRouter.requestAnimationReveal(panelGroup, String(networkId), String(nodeId), String(parameterKey));
    }

    // Stable identity-scoped seams for registered custom editors: the same
    // keying operations the generic rows use, with no Document access.
    function parameterKeyStatusFor(networkId, nodeId, key) {
        if (!controller || !validIdentity(networkId) || !validIdentity(nodeId) || !validIdentity(key))
            return "none";
        return String(controller.nodeParameterKeyStatus(String(networkId), String(nodeId), String(key)));
    }

    function keyParameterAtFrame(networkId, nodeId, key) {
        if (!controller || !validIdentity(networkId) || !validIdentity(nodeId) || !validIdentity(key))
            return false;
        return controller.keyNodeParameter(String(networkId), String(nodeId), String(key));
    }

    function removeParameterKeyAtFrame(networkId, nodeId, key) {
        if (!controller || !validIdentity(networkId) || !validIdentity(nodeId) || !validIdentity(key))
            return false;
        return controller.removeNodeParameterKey(String(networkId), String(nodeId), String(key));
    }

    // --- lifecycle --------------------------------------------------------
    Component.onCompleted: {
        Qt.callLater(restoreState);
        revision++;
        effectiveTwoColumns = columnsFit;
        syncHistoryGesture();
    }
    // Teardown drops the registration so reopening a panel never accumulates
    // competing owners.
    Component.onDestruction: historyController.setGesture(parametersPanel, false)

    // Panel injects ID, state and workspace together; wait for those bindings.
    // Arrangement edits save eagerly, never after an ID change or teardown.
    onPanelIdChanged: Qt.callLater(restoreState)
    // A restored presentation can reuse this panel ID and its loaded body.
    onPersistedStateJsonChanged: {
        if (!stateReady || savingState)
            return;
        stateReady = false;
        restoredForPanelId = "";
        cancelEdit(activeToken);
        Qt.callLater(restoreState);
    }

    // The inspector list is reassigned on every arrangement change (open, close,
    // pin, collapse); the packing follows it, and the Repeater's own item
    // signals cover a delegate's creation and destruction. A live gesture is
    // dropped first: its row may be rebuilt by this change, and an abandoned
    // token would otherwise keep every later edit out.
    onInspectorsChanged: {
        if (activeToken.length > 0)
            cancelEdit(activeToken);
        Qt.callLater(inspectorContent.packCards);
    }

    Connections {
        target: parametersPanel.contextRouter
        function onInspectorRequested(group, network, nodeId) {
            if (group !== parametersPanel.panelGroup)
                return;
            if (!parametersPanel.effectivelyVisible())
                return;
            parametersPanel.openInspector(String(network), String(nodeId));
        }
    }

    Connections {
        target: parametersPanel.controller
        function onGraphChanged() {
            parametersPanel.requestRefresh();
        }
        function onFrameChanged() {
            parametersPanel.requestRefresh();
        }
        function onCatalogChanged() {
            parametersPanel.requestRefresh();
        }
        // The session retired an interaction through its cancellation path,
        // which may have been this panel's own. Only the matching token clears
        // local state, so a successor gesture keeps working.
        function onParameterEditEnded(token) {
            parametersPanel.retireToken(String(token));
        }
    }

    // Escape cancels the one active gesture without leaving a partial edit.
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: parametersPanel.activeToken.length > 0
        onActivated: parametersPanel.cancelEdit(parametersPanel.activeToken)
    }

    // --- header tools -----------------------------------------------------
    Component {
        id: headerToolsComponent
        Item {
            implicitWidth: toolsRow.implicitWidth
            implicitHeight: toolsRow.implicitHeight

            RowLayout {
                id: toolsRow
                anchors.fill: parent
                spacing: 4

                SpinBox {
                    id: limitBox
                    objectName: "accumulationLimit"
                    from: 1
                    to: 20
                    value: parametersPanel.inspectorLimit
                    editable: true
                    implicitWidth: 48
                    implicitHeight: 23
                    font.pixelSize: theme.fontSize
                    leftPadding: 4
                    rightPadding: 4
                    up.indicator: Item {
                        width: 0
                        height: 0
                    }
                    down.indicator: Item {
                        width: 0
                        height: 0
                    }
                    Accessible.name: "Unpinned inspector limit"
                    onValueChanged: {
                        if (value !== parametersPanel.inspectorLimit)
                            parametersPanel.setLimit(value);
                    }
                    contentItem: TextInput {
                        text: limitBox.textFromValue(limitBox.value, limitBox.locale)
                        color: theme.text
                        font: limitBox.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        selectByMouse: true
                        validator: IntValidator {
                            bottom: 1
                            top: 20
                        }
                        onEditingFinished: limitBox.value = limitBox.valueFromText(text, limitBox.locale)
                    }
                    background: Rectangle {
                        color: theme.field
                        border.color: limitBox.activeFocus ? theme.accent : theme.border
                        radius: theme.smallRadius
                    }
                }

                Button {
                    id: columnToggle
                    objectName: "columnToggle"
                    checkable: true
                    checked: parametersPanel.twoColumns
                    property bool ready: false
                    implicitWidth: 28
                    implicitHeight: 24
                    padding: 0
                    Component.onCompleted: ready = true
                    Accessible.name: "Two inspector columns"
                    onToggled: if (ready)
                        parametersPanel.setTwoColumns(checked)
                    background: Rectangle {
                        radius: theme.smallRadius
                        color: columnToggle.hovered ? theme.hover : "transparent"
                    }
                    contentItem: Item {
                        Row {
                            anchors.centerIn: parent
                            spacing: 3
                            Repeater {
                                model: 2
                                Rectangle {
                                    width: 5
                                    height: 12
                                    radius: 1
                                    color: "transparent"
                                    border.color: columnToggle.checked ? theme.accent : theme.muted
                                }
                            }
                        }
                    }
                }

                Button {
                    id: closeAllButton
                    objectName: "closeAllInspectors"
                    flat: true
                    text: "Close all"
                    implicitWidth: 58
                    implicitHeight: 23
                    padding: 0
                    font.pixelSize: theme.fontSize
                    onClicked: parametersPanel.closeAllInspectors()
                    contentItem: Text {
                        text: closeAllButton.text
                        color: closeAllButton.down ? theme.accent : theme.text
                        font: closeAllButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: closeAllButton.hovered ? theme.hover : theme.panel
                        radius: theme.smallRadius
                    }
                }

                Text {
                    visible: width > 0
                    text: String(parametersPanel.inspectors.length) + " open"
                    color: theme.muted
                    font.pixelSize: theme.fontSize
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }

    // --- shared scroll and columns ---------------------------------------
    ScrollView {
        id: inspectorScroll
        anchors.fill: parent
        objectName: "inspectorScroll"
        anchors.margins: 7
        clip: true
        contentWidth: inspectorContent.width
        contentHeight: inspectorContent.height
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        Item {
            id: inspectorContent
            property real columnGap: 6
            // The minimum card keeps an inspector row readable: a label cell and
            // a control column wide enough for a numeric field beside a useful
            // slider. The two-column threshold derives from it, so the panel
            // never packs two cards that each lose that width.
            property real minCardWidth: 300
            // Two columns are shown only when they fit without horizontal
            // scrolling; the saved preference is preserved either way and is
            // never rewritten by a narrow layout.
            readonly property bool twoColumnLayout: parametersPanel.effectiveTwoColumns
            property real columnWidth: twoColumnLayout ? Math.max(minCardWidth, (inspectorScroll.availableWidth - columnGap) / 2) : Math.max(minCardWidth, inspectorScroll.availableWidth)
            // The last card packed into each column. The content and the column
            // anchors take their height from it, so a card that grows (expanded
            // RGB, a revealed editor) resizes the scrollable content without a
            // second layout pass.
            property var columnTails: [null, null]
            width: twoColumnLayout ? columnWidth * 2 + columnGap : columnWidth
            height: Math.max(columnHeight(0), columnHeight(1))

            function columnHeight(index) {
                var tail = columnTails[index];
                return tail ? tail.y + tail.height : 0;
            }

            // The column definitions. Cards are positioned against them rather
            // than parented into them: crossing the one/two-column threshold is
            // then a geometry change, so a card keeps its identity, an
            // in-progress text edit, its collapsed sections and its editor's
            // expansion state instead of being rebuilt by a new column model.
            Item {
                id: leftColumn
                objectName: "inspectorColumn_0"
                width: inspectorContent.columnWidth
                height: inspectorContent.columnHeight(0)
            }

            Item {
                id: rightColumn
                objectName: "inspectorColumn_1"
                visible: inspectorContent.twoColumnLayout
                x: leftColumn.width + inspectorContent.columnGap
                width: inspectorContent.columnWidth
                height: inspectorContent.columnHeight(1)
            }

            // Alternate newest-first entries, stacking each column independently.
            function packCards() {
                var count = cardRepeater.count;
                var previous = [null, null];
                var tails = [null, null];
                for (var i = 0; i < count; ++i) {
                    var card = cardRepeater.itemAt(i);
                    if (!card)
                        continue;
                    var column = inspectorContent.twoColumnLayout ? i % 2 : 0;
                    card.columnIndex = column;
                    card.previousCard = previous[column];
                    previous[column] = card;
                    tails[column] = card;
                }
                columnTails = tails;
            }

            Repeater {
                id: cardRepeater
                model: parametersPanel.inspectors
                delegate: inspectorCardComponent
                onItemAdded: Qt.callLater(inspectorContent.packCards)
                onItemRemoved: Qt.callLater(inspectorContent.packCards)
            }

            onTwoColumnLayoutChanged: packCards()
        }
    }

    // --- inspector card ---------------------------------------------------
    Component {
        id: inspectorCardComponent
        Rectangle {
            id: card
            property var delegateData: modelData
            property string networkId: delegateData && delegateData.network !== undefined ? String(delegateData.network) : ""
            property string nodeId: delegateData && delegateData.node !== undefined ? String(delegateData.node) : ""
            property string inspectorId: card.nodeId
            property var inspectorState: delegateData

            // Re-queried whenever the panel revision advances. The presenter
            // owns schema->kind mapping; this card never switches on effect name.
            readonly property var inspector: {
                parametersPanel.revision;
                if (card.networkId.length === 0 || card.nodeId.length === 0 || !parametersPanel.controller)
                    return ({
                            "available": false
                        });
                return parametersPanel.controller.parameterInspector(card.networkId, card.nodeId);
            }
            readonly property bool available: inspector && inspector.available === true
            readonly property string displayName: card.available ? (inspector.name !== undefined && String(inspector.name).length > 0 ? String(inspector.name) : card.nodeId) : card.nodeId + " (unavailable)"
            readonly property string displayType: card.available && inspector.type !== undefined ? String(inspector.type) : ""
            readonly property string category: card.available && inspector.category !== undefined ? String(inspector.category) : "Utility"
            readonly property string instanceId: card.available && inspector.instanceId !== undefined ? String(inspector.instanceId) : ""
            readonly property string unavailableReason: card.available ? "" : (inspector && inspector.reason !== undefined ? String(inspector.reason) : "")
            readonly property var sections: card.available && inspector.sections ? inspector.sections : []

            // The section shell only rebuilds when the schema shape changes, so
            // collapsed sections and scroll survive value/frame refreshes. Rows
            // re-read values by key through parameterByKey().
            property var sectionModel: []
            property string sectionSignature: ""

            // A registered editor may declare the parameter keys it owns (for
            // example the Read control's timing/color settings). An available
            // editor consumes those keys so exactly one control renders each
            // setting; an unavailable editor consumes nothing and the generic
            // rows stay usable.
            function consumedKeys() {
                var result = {};
                if (typeof parameterEditors === "undefined" || !parameterEditors)
                    return result;
                for (var i = 0; i < sections.length; ++i) {
                    var params = sections[i].parameters || [];
                    for (var j = 0; j < params.length; ++j) {
                        var editorId = params[j].editor !== undefined ? String(params[j].editor) : "";
                        if (editorId.length === 0)
                            continue;
                        var info = parameterEditors.editor(editorId);
                        if (!info || info.available !== true || !parametersPanel.editorApplies(params[j], info))
                            continue;
                        var declared = info.consumes || [];
                        for (var k = 0; k < declared.length; ++k) {
                            var key = String(declared[k]);
                            if (key.length > 0 && key !== String(params[j].key))
                                result[key] = true;
                        }
                    }
                }
                return result;
            }

            function refreshSectionModel() {
                var signature = "";
                var model = [];
                var consumed = consumedKeys();
                var consumedSignature = Object.keys(consumed).sort().join(",");
                for (var i = 0; i < sections.length; ++i) {
                    var section = sections[i];
                    var params = section.parameters || [];
                    // A section whose every parameter is consumed by a
                    // registered editor has nothing left to render: drop the
                    // whole section instead of leaving an empty heading.
                    var allConsumed = params.length > 0;
                    for (var c = 0; c < params.length && allConsumed; ++c)
                        allConsumed = consumed[String(params[c].key)] === true;
                    if (allConsumed)
                        continue;
                    var entries = [];
                    var keys = [];
                    for (var j = 0; j < params.length; ++j) {
                        var parameter = params[j];
                        if (consumed[String(parameter.key)] === true)
                            continue;
                        var rowName = parameter.row !== undefined ? String(parameter.row) : "";
                        keys.push(String(parameter.key) + "=" + rowName);
                        if (rowName.length > 0) {
                            var last = entries.length > 0 ? entries[entries.length - 1] : null;
                            if (last && String(last.row) === rowName) {
                                last.keys.push(String(parameter.key));
                                continue;
                            }
                            entries.push({
                                    "row": rowName,
                                    "keys": [String(parameter.key)]
                                });
                            continue;
                        }
                        entries.push({
                                "row": "",
                                "keys": [String(parameter.key)]
                            });
                    }
                    signature += String(section.name) + ":" + keys.join(",") + "+" + consumedSignature + "|";
                    model.push({
                            "name": String(section.name),
                            "entries": entries
                        });
                }
                if (signature === sectionSignature)
                    return;
                sectionSignature = signature;
                sectionModel = model;
            }

            // ONE value menu per card. A row (or a grouped cell) states which
            // parameter it belongs to and where the menu should open, so the
            // inspector never declares a key-button column and never keeps a
            // second copy of the key/reset actions.
            property string menuNetworkId: ""
            property string menuNodeId: ""
            property string menuParameterKey: ""
            property string menuLabel: ""

            function openValueMenu(anchor, rowRef, keyStatus, scope, label, modified) {
                if (!rowRef)
                    return;
                card.menuNetworkId = String(rowRef.networkId);
                card.menuNodeId = String(rowRef.nodeId);
                card.menuParameterKey = String(rowRef.parameterKey);
                card.menuLabel = label !== undefined ? String(label) : "";
                valueMenu.keyStatus = keyStatus !== undefined ? String(keyStatus) : "none";
                valueMenu.scope = scope !== undefined ? String(scope) : "";
                valueMenu.modified = modified === true;
                valueMenu.revealAvailable = parametersPanel.groupHasAnimationPanel();
                valueMenu.openMenu(anchor);
            }

            KeyIndicator {
                id: valueMenu
                width: 0
                height: 0
                theme: parametersPanel.theme
                networkId: card.menuNetworkId
                nodeId: card.menuNodeId
                parameterKey: card.menuParameterKey
                parameterLabel: card.menuLabel
                frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                resettable: true
                onKeyRequested: parametersPanel.keyParameterAtFrame(card.menuNetworkId, card.menuNodeId, card.menuParameterKey)
                onRemoveKeyRequested: parametersPanel.removeParameterKeyAtFrame(card.menuNetworkId, card.menuNodeId, card.menuParameterKey)
                onRevealRequested: parametersPanel.revealInAnimation(card.menuNetworkId, card.menuNodeId, card.menuParameterKey)
                onResetRequested: parametersPanel.resetValue({
                    "networkId": card.menuNetworkId,
                    "nodeId": card.menuNodeId,
                    "parameterKey": card.menuParameterKey,
                    "label": card.menuLabel
                })
            }

            onSectionsChanged: refreshSectionModel()
            Component.onCompleted: refreshSectionModel()

            Connections {
                target: typeof parameterEditors === "undefined" ? null : parameterEditors
                function onEditorsChanged() {
                    card.sectionSignature = "";
                    card.refreshSectionModel();
                }
            }

            // Packed by inspectorContent.packCards() against the column
            // anchors. The card is never reparented or rebuilt when the panel
            // crosses the one/two-column threshold, so its live edit, collapsed
            // sections and registered editor state stay exactly as they were.
            property int columnIndex: 0
            property Item previousCard: null

            width: leftColumn.width
            x: card.columnIndex === 0 ? leftColumn.x : rightColumn.x
            y: card.previousCard ? card.previousCard.y + card.previousCard.height + inspectorContent.columnGap : 0
            height: bodyColumn.implicitHeight + 2
            implicitHeight: bodyColumn.implicitHeight + 2
            color: theme.panel
            border.color: theme.border
            border.width: 1
            radius: theme.radius
            objectName: "inspector_" + card.nodeId

            ColumnLayout {
                id: bodyColumn
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    leftMargin: 1
                    rightMargin: 1
                    topMargin: 1
                }
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.inspectorControlHeight
                    Layout.leftMargin: 9
                    Layout.rightMargin: 7
                    spacing: 6

                    Rectangle {
                        Layout.preferredWidth: 9
                        Layout.preferredHeight: 9
                        radius: 5
                        color: theme.nodeCategoryColor(card.category)
                    }

                    Text {
                        Layout.fillWidth: true
                        text: card.displayName
                        color: theme.text
                        font.pixelSize: theme.inspectorFontSize + 2
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        visible: card.width >= 330
                        text: card.displayType
                        color: theme.muted
                        font.pixelSize: Math.max(9, theme.inspectorFontSize - 2)
                        elide: Text.ElideRight
                        Layout.maximumWidth: 84
                    }

                    PinButton {
                        id: pinButton
                        objectName: "pin_" + card.nodeId
                        theme: parametersPanel.theme
                        pinned: card.inspectorState.pinned === true
                        Accessible.name: pinned ? "Unpin inspector" : "Pin inspector"
                        onClicked: parametersPanel.setPinned(card.networkId, card.nodeId, !pinned)
                    }

                    Button {
                        id: collapseButton
                        flat: true
                        text: card.inspectorState.collapsed === true ? "\u25b8" : "\u25be"
                        implicitWidth: 19
                        implicitHeight: theme.inspectorControlHeight - 7
                        objectName: "collapse_" + card.nodeId
                        padding: 0
                        onClicked: parametersPanel.setCollapsed(card.networkId, card.nodeId, card.inspectorState.collapsed !== true)
                        contentItem: Text {
                            text: collapseButton.text
                            color: theme.muted
                            font.pixelSize: theme.inspectorFontSize
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Button {
                        id: closeButton
                        objectName: "close_" + card.nodeId
                        flat: true
                        text: "\u00d7"
                        implicitWidth: 22
                        implicitHeight: theme.inspectorControlHeight - 7
                        padding: 0
                        onClicked: parametersPanel.closeInspector(card.networkId, card.nodeId)
                        contentItem: Text {
                            text: closeButton.text
                            color: closeButton.down ? theme.accent : theme.muted
                            font.pixelSize: theme.inspectorFontSize + 2
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                ColumnLayout {
                    visible: card.inspectorState.collapsed !== true
                    Layout.fillWidth: true
                    spacing: 0

                    Repeater {
                        model: card.sectionModel
                        delegate: sectionComponent
                    }

                    Text {
                        visible: !card.available && card.unavailableReason.length > 0
                        Layout.fillWidth: true
                        Layout.margins: 9
                        text: card.unavailableReason
                        color: theme.muted
                        font.pixelSize: theme.inspectorFontSize
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    // --- section ----------------------------------------------------------
    Component {
        id: sectionComponent
        // A schema section is a group boundary, not a titled box: the shared
        // separator states it and its rows follow, so the card keeps one
        // explicit collapse/close/pin header instead of one per section. The
        // section name still orders and separates the groups.
        ColumnLayout {
            id: section
            property var sectionData: modelData
            property int sectionIndex: index
            Layout.fillWidth: true
            spacing: 0

            InspectorSeparator {
                visible: section.sectionIndex > 0
                Layout.fillWidth: true
                Layout.leftMargin: 9
                Layout.rightMargin: 9
                theme: parametersPanel.theme
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 9
                Layout.rightMargin: 9
                Layout.bottomMargin: 3
                spacing: 3

                Repeater {
                    model: section.sectionData.entries
                    delegate: Loader {
                        id: rowLoader
                        property var entry: modelData
                        Layout.fillWidth: true
                        sourceComponent: entry.row.length > 0 && entry.keys.length > 1 ? rowGroupComponent : parameterComponent
                        onEntryChanged: if (item)
                            item.entryData = entry
                        onLoaded: if (item)
                            item.entryData = entry
                    }
                }
            }
        }
    }

    // --- parameter row ----------------------------------------------------
    Component {
        id: parameterComponent
        Item {
            id: parameterRow
            property var entryData: null

            readonly property var card: parametersPanel.owningCard(parameterRow)
            readonly property int revision: parametersPanel.revision
            readonly property string parameterKey: entryData && entryData.keys && entryData.keys.length > 0 ? String(entryData.keys[0]) : ""
            readonly property string networkId: card ? card.networkId : ""
            readonly property string instanceId: card ? card.instanceId : ""
            readonly property string nodeId: card ? card.nodeId : ""
            readonly property var parameter: parametersPanel.parameterByKey(card ? card.inspector : null, parameterRow.parameterKey)
            readonly property string kind: parameter && parameter.kind !== undefined ? String(parameter.kind) : ""
            readonly property string rowLabel: parameter && parameter.label !== undefined && String(parameter.label).length > 0 ? String(parameter.label) : parameterRow.parameterKey
            readonly property string keyStatus: {
                parametersPanel.revision;
                if (!parametersPanel.controller || parameterRow.parameterKey.length === 0)
                    return "none";
                return String(parametersPanel.controller.nodeParameterKeyStatus(parameterRow.networkId, parameterRow.nodeId, parameterRow.parameterKey));
            }
            readonly property bool modified: parameter ? parameter.modified === true : false
            readonly property string scope: parameter && parameter.scope !== undefined ? String(parameter.scope) : ""
            readonly property bool integerParameter: parameter && String(parameter.type) === "integer"
            readonly property var rawValue: parameter && parameter.value !== undefined && parameter.value !== null && parameter.value.length !== undefined ? parameter.value : []
            readonly property real numberValue: parameter && parameter.value !== undefined && parameter.value !== null && parameter.value.length === undefined ? Number(parameter.value) : 0
            readonly property real numberStep: parameterRow.integerParameter ? 1 : (parameter && parameter.step !== undefined ? Number(parameter.step) : 0.01)
            readonly property bool hasMinimum: parameter && parameter.minimum !== undefined
            readonly property bool hasMaximum: parameter && parameter.maximum !== undefined
            readonly property real minimum: parameterRow.hasMinimum ? Number(parameter.minimum) : 0
            readonly property real maximum: parameterRow.hasMaximum ? Number(parameter.maximum) : 0
            readonly property bool hasSoftMinimum: parameter && parameter.softMinimum !== undefined
            readonly property bool hasSoftMaximum: parameter && parameter.softMaximum !== undefined
            readonly property real softMinimum: parameterRow.hasSoftMinimum ? Number(parameter.softMinimum) : 0
            readonly property real softMaximum: parameterRow.hasSoftMaximum ? Number(parameter.softMaximum) : 0
            readonly property int decimals: parameter && parameter.displayDecimals !== undefined ? Number(parameter.displayDecimals) : -1
            readonly property bool boolValue: parameter ? parameter.value === true : false
            readonly property string stringValue: parameter && parameter.value !== undefined && parameter.value !== null ? String(parameter.value) : ""
            onStringValueChanged: {
                if (stringField && !stringField.activeFocus)
                    stringField.text = stringValue;
            }
            readonly property int choiceIndex: {
                if (!parameter || !parameter.choices)
                    return 0;
                var index = parameter.choices.indexOf(parameter.value);
                return index < 0 ? 0 : index;
            }
            readonly property color colorValue: {
                var value = parameterRow.rawValue;
                if (value.length < 3)
                    return "transparent";
                return Qt.rgba(Number(value[0]), Number(value[1]), Number(value[2]), value.length > 3 ? Number(value[3]) : 1);
            }
            readonly property bool hasCustomEditor: parameter && parameter.editor !== undefined && String(parameter.editor).length > 0
            readonly property string editorId: hasCustomEditor ? String(parameter.editor) : ""
            readonly property var editorInfo: {
                if (!parameterRow.hasCustomEditor || typeof parameterEditors === "undefined" || !parameterEditors)
                    return ({
                            "available": false,
                            "source": "",
                            "reason": ""
                        });
                return parameterEditors.editor(parameterRow.editorId);
            }
            readonly property bool editorAvailable: parameterRow.editorInfo && parameterRow.editorInfo.available === true
            readonly property bool customEditorActive: parameterRow.hasCustomEditor && parameterRow.editorAvailable
                && parametersPanel.editorApplies(parameterRow.parameter, parameterRow.editorInfo)
            // The registered editor's declared host layout. A "section" editor
            // is an aggregate control: it spans the full row and provides its own
            // per-parameter affordances, so no outer label/key/marker cell wraps
            // it. No parameter type is special-cased here.
            readonly property string editorPresentation:
                parameterRow.customEditorActive && parameterRow.editorInfo && parameterRow.editorInfo.presentation !== undefined
                    ? String(parameterRow.editorInfo.presentation) : "row"
            readonly property bool sectionEditor: parameterRow.editorPresentation === "section"
            // Numeric controls consume this row-local rejection through their
            // existing border/tooltip/description. Other controls leave the
            // diagnosis on the controller; no row prints an error paragraph.
            readonly property string rowError: parametersPanel.gestureErrorKey === parameterRow.parameterKey ? parametersPanel.gestureError : ""
            // Exact authored text for numeric rows; Integer values must never
            // be displayed or committed through a lossy JavaScript number.
            readonly property string exactText: parameter && parameter.valueText !== undefined ? String(parameter.valueText) : ""
            readonly property int dragThreshold: parametersPanel.controller ? Number(parametersPanel.controller.dragDistance) : 4
            readonly property var componentLabels: parameterRow.kind === "vector3" ? ["X", "Y", "Z"]
                                                     : parameterRow.kind === "vector2" ? ["X", "Y"]
                                                                                       : ["R", "G", "B", "A"]

            implicitHeight: rowLayout.implicitHeight
            width: parent ? parent.width : implicitWidth
            Layout.fillWidth: true

            // Capture identities at drag start. Native MIME transport crosses
            // QQuickWindow boundaries without moving the inspector's layout.
            function componentValue(index) {
                var value = parameterRow.rawValue;
                return index < value.length ? Number(value[index]) : 0;
            }

            function componentEdited(index, value) {
                var next = [];
                for (var i = 0; i < parameterRow.rawValue.length; ++i)
                    next.push(Number(parameterRow.rawValue[i]));
                if (index >= 0 && index < next.length)
                    next[index] = value;
                return next;
            }

            function rowRef() {
                return {
                    "networkId": parameterRow.networkId,
                    "nodeId": parameterRow.nodeId,
                    "parameterKey": parameterRow.parameterKey,
                    "parameter": parameterRow.parameter,
                    "label": parameterRow.rowLabel
                };
            }

            function commitDiscrete(value) {
                return parametersPanel.gestureSingle(parameterRow.rowRef(), value);
            }

            function commitText(entered) {
                return parametersPanel.gestureText(parameterRow.rowRef(), entered);
            }

            function commitComponent(index, value) {
                return parameterRow.commitDiscrete(parameterRow.componentEdited(index, value));
            }

            function keyAtFrame() {
                if (!parametersPanel.controller || parameterRow.parameterKey.length === 0)
                    return false;
                return parametersPanel.controller.keyNodeParameter(parameterRow.networkId, parameterRow.nodeId, parameterRow.parameterKey);
            }

            // Adapter for the shared numeric bundle: the row supplies its own
            // identity, metadata and gesture entry points.
            function bindNumericBundle(bundle) {
                if (!bundle)
                    return;
                bundle.theme = parametersPanel.theme;
                bundle.panel = parametersPanel;
                bundle.row = parameterRow;
                bundle.showSlider = true;
                bundle.compact = false;
                bundle.fieldFirst = true;
                bundle.stepper = true;
                bundle.graduated = false;
                bundle.controlHeight = theme.inspectorControlHeight;
                bundle.textSize = theme.inspectorFontSize;
            }

            // Right-clicking the value offers the shared value actions. The row
            // keeps no key-button column: the one card menu states the key
            // status, the applicable key action and the reset, and ordinary
            // text editing on the value cell is untouched.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: parameterRow.openValueMenu(rowLayout)
            }

            function openValueMenu(anchor) {
                if (parameterRow.card)
                    parameterRow.card.openValueMenu(anchor, parameterRow.rowRef(), parameterRow.keyStatus, parameterRow.scope, parameterRow.rowLabel, parameterRow.modified);
            }

            RowLayout {
                id: rowLayout
                anchors.fill: parent
                spacing: theme.inspectorSpacing

                // Modified-from-default marker: distinct position plus an
                // accessible name, so it does not rely on color alone.
                Rectangle {
                    objectName: "modified_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                    visible: !parameterRow.sectionEditor
                    Layout.preferredWidth: 6
                    Layout.maximumWidth: 6
                    Layout.preferredHeight: 6
                    Layout.alignment: Qt.AlignVCenter
                    radius: 3
                    color: parameterRow.modified ? theme.accent : "transparent"
                    Accessible.name: parameterRow.modified ? "Modified from default" : "At default value"
                    ToolTip.visible: markerHover.hovered
                    ToolTip.text: parameterRow.modified ? "Modified from default" : "At default value"
                    HoverHandler {
                        id: markerHover
                    }
                }

                // The shared label cell: exposure dragging, the supported
                // Alt-click keying shortcut and the edit-scope tooltip stay
                // exactly where they were, at the inspector's own readable
                // metrics.
                ExposureLabel {
                    visible: !parameterRow.sectionEditor
                    Layout.preferredWidth: theme.inspectorLabelWidth
                    Layout.minimumWidth: 44
                    Layout.maximumWidth: theme.inspectorLabelWidth
                    Layout.alignment: Qt.AlignVCenter
                    theme: parametersPanel.theme
                    networkId: parameterRow.networkId
                    instanceId: parameterRow.instanceId
                    nodeId: parameterRow.nodeId
                    parameterKey: parameterRow.parameterKey
                    labelText: parameterRow.rowLabel
                    keyStatus: parameterRow.keyStatus
                    frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                    textSize: theme.inspectorFontSize
                    controlHeight: theme.inspectorControlHeight
                    onKeyRequested: parameterRow.keyAtFrame()
                }

                ColumnLayout {
                    id: controlColumn
                    Layout.fillWidth: true
                    Layout.maximumWidth: Infinity
                    spacing: 2

                    // number: the shared numeric control bundle (slider, typed
                    // field, error) — one owner for ordinary and grouped rows.
                    Loader {
                        id: numberBundle
                        visible: parameterRow.kind === "number" && !parameterRow.customEditorActive
                        active: parameterRow.kind === "number" && !parameterRow.customEditorActive
                        Layout.fillWidth: true
                        sourceComponent: numericControlComponent
                        onLoaded: parameterRow.bindNumericBundle(item)
                    }

                    StudioComboBox {
                        id: choiceBox
                        visible: parameterRow.kind === "choice" && !parameterRow.customEditorActive
                        property int revision: parameterRow.revision
                        objectName: "choice_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                        theme: parametersPanel.theme
                        controlHeight: theme.inspectorControlHeight
                        textSize: theme.inspectorFontSize
                        model: parameterRow.parameter && parameterRow.parameter.choices ? parameterRow.parameter.choices : []
                        currentIndex: parameterRow.choiceIndex
                        Layout.fillWidth: true
                        Accessible.name: parameterRow.rowLabel
                        function syncSelection() {
                            currentIndex = Qt.binding(function () {
                                return parameterRow.choiceIndex;
                            });
                        }
                        onRevisionChanged: syncSelection()
                        // Match StudioComboBox's typed-readout lifecycle: Qt
                        // resets selection after assigning a changed model.
                        onModelChanged: Qt.callLater(syncSelection)
                        onActivated: parameterRow.commitDiscrete(String(currentText))
                        MouseArea {
                            anchors.fill: parent
                            onPressed: function (mouse) {
                                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                            }
                            onClicked: parameterRow.keyAtFrame()
                        }
                    }

                    // Boolean: the shared checkbox. It reports the user's input
                    // and never assigns `checked`, so the authored binding above
                    // it survives a refused or deferred commit.
                    InspectorCheckBox {
                        id: toggleBox
                        visible: parameterRow.kind === "toggle" && !parameterRow.customEditorActive
                        objectName: "toggle_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                        theme: parametersPanel.theme
                        checked: parameterRow.boolValue
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        Accessible.name: parameterRow.rowLabel
                        onToggled: function (checked) {
                            parameterRow.commitDiscrete(checked);
                        }
                        onKeyRequested: parameterRow.keyAtFrame()
                    }

                    // vector2/vector3: one NumericField per component, labelled
                    // and individually scrub-able. Only the intended component
                    // of the evaluated typed value changes.
                    RowLayout {
                        visible: (parameterRow.kind === "vector2" || parameterRow.kind === "vector3") && !parameterRow.customEditorActive
                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: parameterRow.kind === "vector3" ? 3 : 2
                            delegate: RowLayout {
                                required property int index
                                // This component's own gesture: a preview or a
                                // release only ever names the token it began.
                                property string gestureToken: ""
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: parameterRow.componentLabels[index]
                                    color: theme.muted
                                    font.pixelSize: theme.inspectorFontSize
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                NumericField {
                                    objectName: "vector_" + index + "_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                                    interactionOwner: parametersPanel
                                    theme: parametersPanel.theme
                                    value: parameterRow.componentValue(index)
                                    hasMinimum: parameterRow.hasMinimum
                                    hasMaximum: parameterRow.hasMaximum
                                    minimum: parameterRow.minimum
                                    maximum: parameterRow.maximum
                                    step: parameterRow.numberStep
                                    decimals: parameterRow.decimals
                                    integer: parameterRow.integerParameter
                                    label: parameterRow.rowLabel + " " + parameterRow.componentLabels[index]
                                    errorText: parameterRow.rowError
                                    dragThreshold: parameterRow.dragThreshold
                                    keyStatus: parameterRow.keyStatus
                                    controlHeight: theme.inspectorControlHeight
                                    textSize: theme.inspectorFontSize
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    onTextCommitted: function (text) {
                                        var numeric = Number(text);
                                        if (Number.isFinite(numeric))
                                            parameterRow.commitComponent(index, numeric);
                                        else
                                            parametersPanel.rejectText(parameterRow.rowRef(), text);
                                    }
                                    onTextRejected: parametersPanel.rejectText(parameterRow.rowRef(), text)
                                    onStepped: function (value) {
                                        parameterRow.commitComponent(index, value);
                                    }
                                    onScrubStarted: gestureToken = parametersPanel.beginScrub(parameterRow.rowRef())
                                    onScrubbed: function (value) {
                                        parametersPanel.updateScrub(gestureToken, parameterRow.componentEdited(index, value));
                                    }
                                    onScrubFinished: {
                                        var token = gestureToken;
                                        gestureToken = "";
                                        parametersPanel.finishScrub(token);
                                    }
                                    onScrubCancelled: {
                                        var token = gestureToken;
                                        gestureToken = "";
                                        parametersPanel.cancelScrub(token);
                                    }
                                    onKeyRequested: parameterRow.keyAtFrame()
                                    // A retired gesture (Escape, a preview-only
                                    // Undo or a successor interaction) returns
                                    // the component to the authored value at
                                    // once.
                                    gestureLive: parametersPanel.activeToken.length > 0
                                                 && String(parametersPanel.activeToken) === gestureToken
                                }
                            }
                        }
                    }

                    // color: labelled R/G/B/A component fields. Grade selects
                    // the registered linked-RGB editor instead; this generic
                    // fallback stays usable when no editor is registered.
                    RowLayout {
                        visible: parameterRow.kind === "color" && !parameterRow.customEditorActive
                        Layout.fillWidth: true
                        spacing: 4

                        Rectangle {
                            id: colorSwatch
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 20
                            radius: theme.smallRadius
                            color: parameterRow.colorValue
                            border.color: theme.border
                            Accessible.name: parameterRow.rowLabel + " color"
                        }

                        Repeater {
                            model: 4
                            delegate: RowLayout {
                                required property int index
                                // This component's own gesture: a preview or a
                                // release only ever names the token it began.
                                property string gestureToken: ""
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: parameterRow.componentLabels[index]
                                    color: theme.muted
                                    font.pixelSize: theme.inspectorFontSize
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                NumericField {
                                    objectName: "color_" + index + "_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                                    interactionOwner: parametersPanel
                                    theme: parametersPanel.theme
                                    value: parameterRow.componentValue(index)
                                    hasMinimum: parameterRow.hasMinimum
                                    hasMaximum: parameterRow.hasMaximum
                                    minimum: parameterRow.minimum
                                    maximum: parameterRow.maximum
                                    step: parameterRow.numberStep
                                    decimals: parameterRow.decimals
                                    label: parameterRow.rowLabel + " " + parameterRow.componentLabels[index]
                                    errorText: parameterRow.rowError
                                    dragThreshold: parameterRow.dragThreshold
                                    keyStatus: parameterRow.keyStatus
                                    controlHeight: theme.inspectorControlHeight
                                    textSize: theme.inspectorFontSize
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    onTextCommitted: function (text) {
                                        var numeric = Number(text);
                                        if (Number.isFinite(numeric))
                                            parameterRow.commitComponent(index, numeric);
                                        else
                                            parametersPanel.rejectText(parameterRow.rowRef(), text);
                                    }
                                    onTextRejected: parametersPanel.rejectText(parameterRow.rowRef(), text)
                                    onStepped: function (value) {
                                        parameterRow.commitComponent(index, value);
                                    }
                                    onScrubStarted: gestureToken = parametersPanel.beginScrub(parameterRow.rowRef())
                                    onScrubbed: function (value) {
                                        parametersPanel.updateScrub(gestureToken, parameterRow.componentEdited(index, value));
                                    }
                                    onScrubFinished: {
                                        var token = gestureToken;
                                        gestureToken = "";
                                        parametersPanel.finishScrub(token);
                                    }
                                    onScrubCancelled: {
                                        var token = gestureToken;
                                        gestureToken = "";
                                        parametersPanel.cancelScrub(token);
                                    }
                                    onKeyRequested: parameterRow.keyAtFrame()
                                    // A retired gesture (Escape, a preview-only
                                    // Undo or a successor interaction) returns
                                    // the component to the authored value at
                                    // once.
                                    gestureLive: parametersPanel.activeToken.length > 0
                                                 && String(parametersPanel.activeToken) === gestureToken
                                }
                            }
                        }
                    }

                    // string: single text field.
                    TextField {
                        id: stringField
                        visible: parameterRow.kind === "string" && !parameterRow.customEditorActive
                        property int revision: parameterRow.revision
                        objectName: "string_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                        text: parameterRow.stringValue
                        Layout.fillWidth: true
                        implicitHeight: theme.inspectorControlHeight
                        font.pixelSize: theme.inspectorFontSize
                        color: theme.text
                        selectByMouse: true
                        Accessible.name: parameterRow.rowLabel
                        onEditingFinished: parameterRow.commitDiscrete(String(text))
                        // Typing is a parameter interaction from the first
                        // keystroke, so it retires whatever the session owns.
                        onActiveFocusChanged: if (activeFocus)
                            parametersPanel.prepareParameterInteraction()
                        Keys.onEscapePressed: function (event) {
                            event.accepted = true;
                            stringField.text = parameterRow.stringValue;
                        }
                        onRevisionChanged: {
                            if (!activeFocus)
                                stringField.text = parameterRow.stringValue;
                        }
                        background: Rectangle {
                            color: theme.field
                            border.color: stringField.activeFocus ? theme.accent : theme.border
                            radius: theme.smallRadius
                        }
                        MouseArea {
                            anchors.fill: parent
                            onPressed: function (mouse) {
                                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                            }
                            onClicked: parameterRow.keyAtFrame()
                        }
                    }

                    // Registered namespaced editor host. When the editor is
                    // unavailable the generic control stays usable and the
                    // reason is surfaced without discarding parameter state.
                    Loader {
                        id: customEditorLoader
                        visible: parameterRow.customEditorActive
                        active: parameterRow.customEditorActive
                        property int revision: parameterRow.revision
                        Layout.fillWidth: true
                        source: parameterRow.editorAvailable ? parameterRow.editorInfo.source : ""
                        // The host owns the current row; a refresh must not leave
                        // a custom editor showing a stale value.
                        onRevisionChanged: parameterRow.syncCustomEditor()
                        onLoaded: parameterRow.syncCustomEditor()
                    }

                    Text {
                        visible: parameterRow.hasCustomEditor && !parameterRow.editorAvailable
                        Layout.fillWidth: true
                        text: parameterRow.editorInfo && parameterRow.editorInfo.reason !== undefined && String(parameterRow.editorInfo.reason).length > 0 ? String(parameterRow.editorInfo.reason) : "Parameter editor '" + parameterRow.editorId + "' is unavailable"
                        color: theme.muted
                        font.pixelSize: Math.max(9, theme.inspectorFontSize - 2)
                        elide: Text.ElideRight
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        visible: parameterRow.kind.length > 0 && parameterRow.kind !== "number" && parameterRow.kind !== "choice" && parameterRow.kind !== "toggle" && parameterRow.kind !== "vector2" && parameterRow.kind !== "vector3" && parameterRow.kind !== "color" && parameterRow.kind !== "string"
                        text: "Unsupported parameter kind '" + parameterRow.kind + "'"
                        color: theme.muted
                        font.pixelSize: theme.inspectorFontSize
                    }
                }
            }

            function syncCustomEditor() {
                var item = customEditorLoader.item;
                if (!item)
                    return;
                if ("theme" in item)
                    item.theme = parametersPanel.theme;
                if ("networkId" in item)
                    item.networkId = parameterRow.networkId;
                if ("instanceId" in item)
                    item.instanceId = parameterRow.instanceId;
                if ("nodeId" in item)
                    item.nodeId = parameterRow.nodeId;
                if ("parameterKey" in item)
                    item.parameterKey = parameterRow.parameterKey;
                if ("parameter" in item)
                    item.parameter = parameterRow.parameter;
                if ("controller" in item)
                    item.controller = parametersPanel.controller;
                if ("rowAdapter" in item)
                    item.rowAdapter = parameterRow;
                if ("panel" in item)
                    item.panel = parametersPanel;
            }
        }
    }

    // The numeric control bundle: ONE owner for the typed field and the
    // useful-width slider used by an ordinary number row, by a grouped pair row
    // and by a registered row editor alike. A rejected edit keeps its state and
    // reaches the field's own error affordance; the bundle prints no text.
    // Callers supply a `row` adapter object exposing the same members every row
    // kind already has, plus the presentation flags below. Grouping and
    // registration therefore add no second control semantics.
    Component {
        id: numericControlComponent
        ColumnLayout {
            id: numericControl

            property var theme: null
            property var panel: null
            property var row: null
            // Live fit: a pair cell hides only the slider when there is no room
            // for it; the field and marker always remain.
            property bool showSlider: true
            property bool compact: false
            // Field first, then the useful-width slider, as the design states.
            property bool fieldFirst: true
            // Grade-style ruler; the host's ordinary rows leave it plain.
            property bool graduated: false
            // The up/down affordance of the mockup's numeric cells.
            property bool stepper: false
            property int controlHeight: 23
            property int textSize: 0

            readonly property string keyStatus: numericControl.row && numericControl.row.keyStatus !== undefined ? String(numericControl.row.keyStatus) : "none"
            // The gesture THIS bundle began. Both controls of the bundle edit
            // the same row, so they share one token; live means that token is
            // still the panel's, so a retired gesture (Escape, a preview-only
            // Undo, or a successor interaction) stops the preview at once and
            // one that was never granted can never preview into another edit.
            property string gestureToken: ""
            readonly property bool gestureLive: numericControl.gestureToken.length > 0
                                                 && numericControl.panel !== null
                                                 && String(numericControl.panel.activeToken) === numericControl.gestureToken

            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                layoutDirection: numericControl.fieldFirst ? Qt.RightToLeft : Qt.LeftToRight

                // The shared slider: it owns the pointer gesture and the host's
                // one parameter gesture is begun, previewed, committed or
                // cancelled through these four signals. Every call names the
                // token this bundle captured, so a begin that was not granted
                // (gestureLive stays false) publishes nothing.
                ParameterSlider {
                    id: bundleSlider
                    visible: numericControl.showSlider
                    objectName: numericControl.row ? "slider_" + numericControl.row.nodeId + "_" + numericControl.row.parameterKey : ""
                    theme: numericControl.theme
                    from: numericControl.row && numericControl.row.hasSoftMinimum ? numericControl.row.softMinimum : (numericControl.row && numericControl.row.hasMinimum ? numericControl.row.minimum : 0)
                    to: {
                        var low = bundleSlider.from;
                        var high = numericControl.row && numericControl.row.hasSoftMaximum ? numericControl.row.softMaximum
                                   : (numericControl.row && numericControl.row.hasMaximum ? numericControl.row.maximum : 1);
                        return Math.max(low + 1e-9, high);
                    }
                    stepSize: numericControl.row ? numericControl.row.numberStep : 0.01
                    value: numericControl.row ? numericControl.row.numberValue : 0
                    graduated: numericControl.graduated
                    label: numericControl.row ? numericControl.row.rowLabel : ""
                    gestureLive: numericControl.gestureLive
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    Layout.preferredWidth: 120
                    Layout.alignment: Qt.AlignVCenter
                    onEditStarted: if (numericControl.panel && numericControl.row)
                        numericControl.gestureToken = numericControl.panel.beginScrub(numericControl.row.rowRef())
                    onValueEdited: function (value) {
                        if (!numericControl.panel)
                            return;
                        var integer = numericControl.row && numericControl.row.integerParameter;
                        numericControl.panel.updateScrub(numericControl.gestureToken, integer ? Math.round(value) : value);
                    }
                    onEditFinished: {
                        var token = numericControl.gestureToken;
                        numericControl.gestureToken = "";
                        if (numericControl.panel)
                            numericControl.panel.finishScrub(token);
                    }
                    onEditCancelled: {
                        var token = numericControl.gestureToken;
                        numericControl.gestureToken = "";
                        if (numericControl.panel)
                            numericControl.panel.cancelScrub(token);
                    }
                    onKeyRequested: if (numericControl.row)
                        numericControl.row.keyAtFrame()
                }

                NumericField {
                    id: bundleField
                    objectName: numericControl.row ? "param_" + numericControl.row.nodeId + "_" + numericControl.row.parameterKey : ""
                    interactionOwner: numericControl.panel
                    theme: numericControl.theme
                    value: numericControl.row ? numericControl.row.numberValue : 0
                    text: numericControl.row && numericControl.row.exactText !== undefined ? numericControl.row.exactText : ""
                    hasMinimum: numericControl.row ? numericControl.row.hasMinimum : false
                    hasMaximum: numericControl.row ? numericControl.row.hasMaximum : false
                    minimum: numericControl.row ? numericControl.row.minimum : 0
                    maximum: numericControl.row ? numericControl.row.maximum : 0
                    step: numericControl.row ? numericControl.row.numberStep : 0.01
                    decimals: numericControl.row ? numericControl.row.decimals : -1
                    integer: numericControl.row ? numericControl.row.integerParameter : false
                    label: numericControl.row ? numericControl.row.rowLabel : ""
                    errorText: numericControl.row ? numericControl.row.rowError : ""
                    dragThreshold: numericControl.row ? numericControl.row.dragThreshold : 4
                    keyStatus: numericControl.keyStatus
                    stepper: numericControl.stepper
                    controlHeight: numericControl.controlHeight
                    textSize: numericControl.textSize
                    fieldWidth: numericControl.compact ? 56 : numericControl.stepper ? 68 : 62
                    // The field keeps its width; the slider takes the rest of
                    // the row, so the value stays readable and the travel stays
                    // useful as the dock resizes.
                    Layout.fillWidth: numericControl.compact
                    Layout.minimumWidth: numericControl.compact ? 48 : numericControl.stepper ? 68 : 62
                    Layout.preferredWidth: numericControl.compact ? 56 : numericControl.stepper ? 68 : 62
                    Layout.maximumWidth: numericControl.compact ? 56 : numericControl.stepper ? 68 : 62
                    Layout.alignment: Qt.AlignVCenter
                    onTextCommitted: function (text) {
                        if (numericControl.row)
                            numericControl.row.commitText(text);
                    }
                    onTextRejected: function (text) {
                        if (numericControl.panel && numericControl.row)
                            numericControl.panel.rejectText(numericControl.row.rowRef(), text);
                    }
                    onStepped: function (value) {
                        if (numericControl.row)
                            numericControl.row.commitDiscrete(value);
                    }
                    onScrubStarted: if (numericControl.panel && numericControl.row)
                        numericControl.gestureToken = numericControl.panel.beginScrub(numericControl.row.rowRef())
                    onScrubbed: function (value) {
                        if (!numericControl.panel)
                            return;
                        numericControl.panel.updateScrub(numericControl.gestureToken, value);
                    }
                    onScrubFinished: {
                        var token = numericControl.gestureToken;
                        numericControl.gestureToken = "";
                        if (numericControl.panel)
                            numericControl.panel.finishScrub(token);
                    }
                    onScrubCancelled: {
                        var token = numericControl.gestureToken;
                        numericControl.gestureToken = "";
                        if (numericControl.panel)
                            numericControl.panel.cancelScrub(token);
                    }
                    onKeyRequested: if (numericControl.row)
                        numericControl.row.keyAtFrame()
                    // A retired gesture (Escape, a preview-only Undo or a
                    // successor interaction) returns the field to the authored
                    // value at once.
                    gestureLive: numericControl.gestureLive
                }
            }
        }
    }

    // --- grouped row ------------------------------------------------------
    // Consecutive parameters sharing a schema `row` render on one line. A group
    // of Booleans states its checkboxes inline (Grade's flags); a group led by a
    // Choice states the group label and then choice plus checkboxes (the shared
    // mask footer); a group of numbers keeps the labelled paired cells
    // (Transform's Translate X/Y). The schema `row` and each parameter's kind
    // decide, never an effect name. Identities, animation channels, commands and
    // exposure stay independent per key.
    Component {
        id: rowGroupComponent
        Item {
            id: rowGroup
            property var entryData: null

            readonly property var card: parametersPanel.owningCard(rowGroup)
            readonly property int revision: parametersPanel.revision
            readonly property string rowName: entryData ? String(entryData.row) : ""
            readonly property var keys: entryData && entryData.keys ? entryData.keys : []
            readonly property string networkId: card ? card.networkId : ""
            readonly property string instanceId: card ? card.instanceId : ""
            readonly property string nodeId: card ? card.nodeId : ""
            readonly property int dragThreshold: parametersPanel.controller ? Number(parametersPanel.controller.dragDistance) : 4
            // An inline group (Booleans and Choices only) states its controls on
            // one line; its label cell appears only when a labelled control leads
            // it, so a flags row starts exactly where an ordinary row's label
            // starts. Anything else keeps the labelled paired cells.
            readonly property bool inlineGroup: rowGroup.keys.length > 0 && rowGroup.kindList().every(function (kind) {
                return kind === "toggle" || kind === "choice";
            })
            readonly property bool labelledGroup: rowGroup.inlineGroup && rowGroup.kindOf(rowGroup.keys[0]) !== "toggle"

            implicitHeight: groupLayout.implicitHeight
            width: parent ? parent.width : implicitWidth
            Layout.fillWidth: true

            function parameterOf(key) {
                return parametersPanel.parameterByKey(rowGroup.card ? rowGroup.card.inspector : null, key);
            }

            function kindOf(key) {
                var parameter = rowGroup.parameterOf(key);
                return parameter && parameter.kind !== undefined ? String(parameter.kind) : "";
            }

            function kindList() {
                var result = [];
                for (var i = 0; i < rowGroup.keys.length; ++i)
                    result.push(rowGroup.kindOf(rowGroup.keys[i]));
                return result;
            }

            function labelOf(key) {
                var parameter = rowGroup.parameterOf(key);
                return parameter && parameter.label !== undefined && String(parameter.label).length > 0 ? String(parameter.label) : String(key);
            }

            function choiceIndexOf(key) {
                var parameter = rowGroup.parameterOf(key);
                if (!parameter || !parameter.choices)
                    return 0;
                var index = parameter.choices.indexOf(parameter.value);
                return index < 0 ? 0 : index;
            }

            function keyStatusOf(key) {
                parametersPanel.revision;
                if (!parametersPanel.controller || String(key).length === 0)
                    return "none";
                return String(parametersPanel.controller.nodeParameterKeyStatus(rowGroup.networkId, rowGroup.nodeId, String(key)));
            }

            // Every cell states its own key when it opens the card's shared
            // value menu, so each key of the group keeps its own key, remove and
            // reset actions without a second menu.
            function openKeyMenu(anchor, key, label) {
                if (!rowGroup.card)
                    return;
                var parameter = rowGroup.parameterOf(key);
                rowGroup.card.openValueMenu(anchor, {
                    "networkId": rowGroup.networkId,
                    "nodeId": rowGroup.nodeId,
                    "parameterKey": String(key)
                }, rowGroup.keyStatusOf(key), parameter && parameter.scope !== undefined ? String(parameter.scope) : "", label, parameter && parameter.modified === true);
            }

            RowLayout {
                id: groupLayout
                anchors.fill: parent
                spacing: theme.inspectorSpacing

                Rectangle {
                    // Alignment spacer: the group's first control starts at the
                    // same place an ordinary row's control column starts.
                    Layout.preferredWidth: 6
                    Layout.maximumWidth: 6
                    Layout.preferredHeight: 6
                    Layout.alignment: Qt.AlignVCenter
                    radius: 3
                    color: "transparent"
                }

                Item {
                    visible: !rowGroup.inlineGroup || rowGroup.labelledGroup
                    Layout.preferredWidth: theme.inspectorLabelWidth
                    Layout.minimumWidth: 44
                    Layout.maximumWidth: theme.inspectorLabelWidth
                    Layout.alignment: Qt.AlignVCenter
                    implicitHeight: theme.inspectorControlHeight
                    Text {
                        visible: !rowGroup.labelledGroup
                        anchors.fill: parent
                        text: rowGroup.rowName
                        color: theme.text
                        font.pixelSize: theme.inspectorFontSize
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        Accessible.name: rowGroup.rowName
                    }
                    ExposureLabel {
                        anchors.fill: parent
                        visible: rowGroup.labelledGroup
                        theme: parametersPanel.theme
                        networkId: rowGroup.networkId
                        instanceId: rowGroup.instanceId
                        nodeId: rowGroup.nodeId
                        parameterKey: rowGroup.labelledGroup ? String(rowGroup.keys[0]) : ""
                        labelText: rowGroup.rowName
                        keyStatus: rowGroup.keyStatusOf(parameterKey)
                        frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                        textSize: theme.inspectorFontSize
                        controlHeight: theme.inspectorControlHeight
                        onKeyRequested: parametersPanel.keyParameterAtFrame(networkId, nodeId, parameterKey)
                    }
                }

                Row {
                    id: groupControls
                    Layout.fillWidth: true
                    spacing: 3

                    // Inline group: checkboxes and/or a choice on one line. It
                    // flows, so a narrow card wraps the flags instead of
                    // clipping them, and a leading choice states the group's
                    // value at a readable width.
                    Flow {
                        id: inlineGroupFlow
                        visible: rowGroup.inlineGroup
                        width: groupControls.width
                        spacing: 10

                        Repeater {
                            model: rowGroup.inlineGroup ? rowGroup.keys : []
                            delegate: Item {
                                id: inlineCell
                                required property string modelData

                                readonly property string kind: rowGroup.kindOf(inlineCell.modelData)
                                readonly property string cellLabel: rowGroup.labelOf(inlineCell.modelData)
                                readonly property var parameter: rowGroup.parameterOf(inlineCell.modelData)
                                readonly property int revision: rowGroup.revision
                                readonly property real cellWidth: inlineCell.kind === "choice" ? Math.max(78, Math.min(240, inlineGroupFlow.width * 0.45)) : inlineLoader.implicitWidth
                                readonly property real cellHeight: inlineLoader.implicitHeight

                                width: inlineCell.cellWidth
                                height: inlineCell.cellHeight

                                function rowRef() {
                                    return {
                                        "networkId": rowGroup.networkId,
                                        "nodeId": rowGroup.nodeId,
                                        "parameterKey": inlineCell.modelData,
                                        "parameter": inlineCell.parameter,
                                        "label": inlineCell.cellLabel
                                    };
                                }

                                function commitDiscrete(value) {
                                    return parametersPanel.gestureSingle(inlineCell.rowRef(), value);
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.RightButton
                                    onClicked: rowGroup.openKeyMenu(inlineCell, inlineCell.modelData, inlineCell.cellLabel)
                                }

                                Loader {
                                    id: inlineLoader
                                    anchors.fill: parent
                                    sourceComponent: inlineCell.kind === "choice" ? inlineChoiceComponent : inlineToggleComponent
                                }

                                Component {
                                    id: inlineToggleComponent
                                    InspectorCheckBox {
                                        objectName: "toggle_" + rowGroup.nodeId + "_" + inlineCell.modelData
                                        theme: parametersPanel.theme
                                        text: inlineCell.cellLabel
                                        networkId: rowGroup.networkId
                                        instanceId: rowGroup.instanceId
                                        nodeId: rowGroup.nodeId
                                        parameterKey: inlineCell.modelData
                                        keyStatus: rowGroup.keyStatusOf(inlineCell.modelData)
                                        frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                                        checked: inlineCell.parameter ? inlineCell.parameter.value === true : false
                                        onToggled: function (checked) {
                                            inlineCell.commitDiscrete(checked);
                                        }
                                        onKeyRequested: parametersPanel.keyParameterAtFrame(rowGroup.networkId, rowGroup.nodeId, inlineCell.modelData)
                                    }
                                }

                                Component {
                                    id: inlineChoiceComponent
                                    StudioComboBox {
                                        objectName: "choice_" + rowGroup.nodeId + "_" + inlineCell.modelData
                                        theme: parametersPanel.theme
                                        controlHeight: theme.inspectorControlHeight
                                        textSize: theme.inspectorFontSize
                                        model: inlineCell.parameter && inlineCell.parameter.choices ? inlineCell.parameter.choices : []
                                        currentIndex: rowGroup.choiceIndexOf(inlineCell.modelData)
                                        Accessible.name: inlineCell.cellLabel
                                        // Match the ordinary choice cell: Qt
                                        // resets the selection after a changed
                                        // model, so the authored value is
                                        // re-stated on every refresh.
                                        function syncSelection() {
                                            currentIndex = Qt.binding(function () {
                                                return rowGroup.choiceIndexOf(inlineCell.modelData);
                                            });
                                        }
                                        onModelChanged: Qt.callLater(syncSelection)
                                        onActivated: inlineCell.commitDiscrete(String(currentText))
                                        MouseArea {
                                            anchors.fill: parent
                                            onPressed: function (mouse) {
                                                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                                            }
                                            onClicked: parametersPanel.keyParameterAtFrame(rowGroup.networkId, rowGroup.nodeId, inlineCell.modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Paired numeric cells: one compact shared bundle per key,
                    // each keeping its own identity, animation and gesture.
                    Repeater {
                        model: rowGroup.inlineGroup ? [] : rowGroup.keys
                        delegate: Item {
                            id: groupedRow
                            required property string modelData
                            width: Math.max(implicitWidth, (groupControls.width
                                   - groupControls.spacing * (rowGroup.keys.length - 1)) / rowGroup.keys.length)
                            implicitWidth: cellLayout.implicitWidth
                            implicitHeight: cellLayout.implicitHeight

                            readonly property var parameter: rowGroup.parameterOf(groupedRow.modelData)
                            readonly property string componentLabel: rowGroup.labelOf(groupedRow.modelData)
                            readonly property string keyStatus: rowGroup.keyStatusOf(groupedRow.modelData)
                            readonly property string scope: parameter && parameter.scope !== undefined ? String(parameter.scope) : ""
                            readonly property bool integerParameter: parameter && String(parameter.type) === "integer"
                            readonly property bool hasMinimum: parameter && parameter.minimum !== undefined
                            readonly property bool hasMaximum: parameter && parameter.maximum !== undefined
                            readonly property bool hasSoftMinimum: parameter && parameter.softMinimum !== undefined
                            readonly property bool hasSoftMaximum: parameter && parameter.softMaximum !== undefined
                            readonly property int decimals: parameter && parameter.displayDecimals !== undefined ? Number(parameter.displayDecimals) : -1
                            readonly property real numberStep: groupedRow.integerParameter ? 1 : (parameter && parameter.step !== undefined ? Number(parameter.step) : 0.01)
                            readonly property string rowError: parametersPanel.gestureErrorKey === groupedRow.modelData ? parametersPanel.gestureError : ""
                            // Every grouped control reads its numbers through these
                            // guarded accessors, so a row whose inspector data is
                            // momentarily absent cannot dereference null.
                            readonly property real minimumValue: groupedRow.hasMinimum && groupedRow.parameter ? Number(groupedRow.parameter.minimum) : 0
                            readonly property real maximumValue: groupedRow.hasMaximum && groupedRow.parameter ? Number(groupedRow.parameter.maximum) : 0
                            readonly property real softMinimumValue: groupedRow.hasSoftMinimum && groupedRow.parameter ? Number(groupedRow.parameter.softMinimum) : 0
                            readonly property real softMaximumValue: groupedRow.hasSoftMaximum && groupedRow.parameter ? Number(groupedRow.parameter.softMaximum) : 0
                            readonly property real currentValue: groupedRow.parameter && groupedRow.parameter.value !== undefined ? Number(groupedRow.parameter.value) : 0
                            // Bundle adapter surface, identical to a plain row's.
                            readonly property real numberValue: groupedRow.currentValue
                            readonly property string nodeId: rowGroup.nodeId
                            readonly property string parameterKey: groupedRow.modelData
                            readonly property string exactText: groupedRow.parameter && groupedRow.parameter.valueText !== undefined ? String(groupedRow.parameter.valueText) : ""
                            readonly property string rowLabel: rowGroup.rowName + " " + groupedRow.componentLabel
                            readonly property real minimum: groupedRow.minimumValue
                            readonly property real maximum: groupedRow.maximumValue
                            readonly property real softMinimum: groupedRow.softMinimumValue
                            readonly property real softMaximum: groupedRow.softMaximumValue
                            readonly property int dragThreshold: rowGroup.dragThreshold

                            function rowRef() {
                                return {
                                    "networkId": rowGroup.networkId,
                                    "nodeId": rowGroup.nodeId,
                                    "parameterKey": groupedRow.modelData,
                                    "parameter": groupedRow.parameter,
                                    "label": rowGroup.rowName + " " + groupedRow.componentLabel
                                };
                            }

                            function keyAtFrame() {
                                if (!parametersPanel.controller)
                                    return false;
                                return parametersPanel.controller.keyNodeParameter(rowGroup.networkId, rowGroup.nodeId, groupedRow.modelData);
                            }

                            function commitText(entered) {
                                return parametersPanel.gestureText(groupedRow.rowRef(), entered);
                            }

                            function commitDiscrete(value) {
                                return parametersPanel.gestureSingle(groupedRow.rowRef(), value);
                            }

                            function bindNumericBundle(bundle) {
                                if (!bundle)
                                    return;
                                bundle.theme = parametersPanel.theme;
                                bundle.panel = parametersPanel;
                                bundle.row = groupedRow;
                                // A paired cell is narrow: keep the slider only
                                // when the whole grouped row is wide enough for it.
                                bundle.showSlider = Qt.binding(function () { return rowGroup.width >= 520; });
                                bundle.compact = true;
                                bundle.fieldFirst = true;
                                bundle.stepper = false;
                                bundle.graduated = false;
                                bundle.controlHeight = theme.inspectorControlHeight;
                                bundle.textSize = theme.inspectorFontSize;
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: rowGroup.openKeyMenu(groupedRow, groupedRow.modelData, groupedRow.rowLabel)
                            }

                            RowLayout {
                                id: cellLayout
                                anchors.fill: parent
                                spacing: 4
                                // Keep both component hit targets disjoint in a
                                // narrow card; the label needs its glyph width,
                                // not a second fixed-width row-label column.

                                ExposureLabel {
                                    Layout.minimumWidth: implicitWidth
                                    Layout.preferredWidth: implicitWidth
                                    Layout.maximumWidth: implicitWidth
                                    Layout.alignment: Qt.AlignVCenter
                                    theme: parametersPanel.theme
                                    networkId: rowGroup.networkId
                                    instanceId: rowGroup.instanceId
                                    nodeId: rowGroup.nodeId
                                    parameterKey: groupedRow.modelData
                                    labelText: groupedRow.componentLabel
                                    keyStatus: groupedRow.keyStatus
                                    frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                                    textSize: theme.inspectorFontSize
                                    controlHeight: theme.inspectorControlHeight
                                    onKeyRequested: groupedRow.keyAtFrame()
                                }

                                // Modified-from-default marker: same identity as a
                                // plain numeric row.
                                Rectangle {
                                    objectName: "modified_" + rowGroup.nodeId + "_" + groupedRow.modelData
                                    Layout.preferredWidth: 6
                                    Layout.maximumWidth: 6
                                    Layout.preferredHeight: 6
                                    Layout.alignment: Qt.AlignVCenter
                                    radius: 3
                                    color: groupedRow.parameter && groupedRow.parameter.modified === true ? theme.accent : "transparent"
                                    Accessible.name: groupedRow.parameter && groupedRow.parameter.modified === true ? "Modified from default" : "At default value"
                                }

                                // The same shared numeric control bundle as a plain
                                // number row, in its compact presentation: the soft
                                // slider is shown only when this row genuinely has
                                // room for it (the row width, not the field width, so
                                // the decision cannot feed back into the layout). The
                                // field, prefix, marker and error always remain, and a
                                // keyed slider stays draggable when it fits.
                                Loader {
                                    id: groupedBundle
                                    Layout.fillWidth: true
                                    sourceComponent: numericControlComponent
                                    onLoaded: groupedRow.bindNumericBundle(item)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
