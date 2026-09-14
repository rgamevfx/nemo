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

    // Accumulated inspectors, newest first. Identity is (network, node), never
    // the node id alone, so the same node id in two networks stays distinct.
    property var inspectors: []
    property int inspectorLimit: 3
    property bool twoColumns: true
    // Two columns are shown only when both minimum-width cards fit inside the
    // scroll viewport. The saved `twoColumns` preference is preserved; a narrow
    // panel overrides it for display only and never rewrites it.
    readonly property bool effectiveTwoColumns: twoColumns
                                                && inspectorScroll.availableWidth >= 2 * 260 + 6 + 14
    property string restoredForPanelId: ""
    property bool stateReady: false
    property bool savingState: false
    readonly property string persistedStateJson: JSON.stringify([panelState.inspectors || [], panelState.limit, panelState.columns])

    // One global revision drives inspector re-queries. It advances on document,
    // frame, and catalog changes. A refresh requested mid-gesture is deferred so
    // the delegate under an active drag or edit is never rebuilt underneath it.
    property int revision: 0
    property bool refreshPending: false

    // Only one parameter gesture is active at a time (beginNodeParameterEdit
    // rejects a second begin). The panel tracks the single live token and the
    // row it belongs to, so a rejected edit can name the parameter.
    property string activeToken: ""
    property var activeRow: null
    // The most recent rejected edit, attributed to one row. It is presentation
    // only; the controller/catalog remain the validation authority.
    property string gestureError: ""
    property string gestureErrorKey: ""

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

    function columnEntries(columnIndex) {
        if (!effectiveTwoColumns)
            return columnIndex === 0 ? inspectors : [];
        var result = [];
        for (var i = 0; i < inspectors.length; ++i) {
            if (i % 2 === columnIndex)
                result.push(inspectors[i]);
        }
        return result;
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

    // Identity-scoped begin, also usable by registered custom editors. Only one
    // gesture may be live; a second begin fails and returns the empty token.
    function beginEditFor(networkId, nodeId, parameterKey) {
        if (!controller)
            return "";
        if (activeToken.length > 0)
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
        return token;
    }

    // Batch shape of the same panel gesture: several keys of ONE target edit
    // atomically (an animated key and a static companion together). The single
    // shapes below are the one-element case of these, so both go through one
    // owner in the controller.
    function beginEditForMany(networkId, nodeId, parameterKeys) {
        if (!controller || activeToken.length > 0)
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
        return token;
    }

    function updateEditMany(values) {
        if (activeToken.length === 0)
            return false;
        return controller.updateNodeParameterEdits(activeToken, values);
    }

    function beginEdit(row) {
        return row ? beginEditFor(row.networkId, row.nodeId, row.parameterKey) : "";
    }

    function updateEdit(value) {
        if (activeToken.length === 0)
            return false;
        if (!activeRow)
            return false;
        var values = {};
        values[String(activeRow.parameterKey)] = value;
        return updateEditMany(values);
    }

    function commitEdit() {
        if (activeToken.length === 0)
            return false;
        var row = activeRow;
        var result = controller.commitNodeParameterEdit(activeToken);
        activeToken = "";
        activeRow = null;
        flushRefresh();
        if (!result) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = row ? String(row.parameterKey) : "";
        }
        return result;
    }

    function cancelEdit() {
        if (activeToken.length === 0)
            return false;
        var result = controller.cancelNodeParameterEdit(activeToken);
        activeToken = "";
        activeRow = null;
        flushRefresh();
        return result;
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

    // Invalid typed text keeps the previous value and explains the parameter
    // without a second validation contract: the message names the control.
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
    // one undo step. A no-op and a rejected value create no history entry.
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
        if (beginEdit(row).length === 0) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        if (updateEdit(value) === false) {
            var message = controller ? String(controller.error) : "";
            cancelEdit();
            gestureError = message;
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        return commitEdit();
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
        if (beginEdit(row).length === 0) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        if (updateEdit(text) === false) {
            var message = controller ? String(controller.error) : "";
            cancelEdit();
            gestureError = message;
            gestureErrorKey = String(row.parameterKey);
            return false;
        }
        return commitEdit();
    }

    // Continuous scrub shares the one live gesture; a scrub that never changed
    // the value cancels instead of publishing a no-op history entry.
    function beginScrub(row) {
        clearGestureError();
        if (beginEdit(row).length === 0) {
            gestureError = controller ? String(controller.error) : "";
            gestureErrorKey = row ? String(row.parameterKey) : "";
        }
    }

    function updateScrub(value) {
        return updateEdit(value);
    }

    function finishScrub() {
        if (activeToken.length === 0)
            return false;
        return commitEdit();
    }

    function cancelScrub() {
        if (activeToken.length === 0)
            return false;
        return cancelEdit();
    }

    // Reset delegates to the identity-scoped controller command: one history
    // entry, schema default, never Remove Animation.
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
    }

    // Panel injects ID, state and workspace together; wait for those bindings.
    // Arrangement edits save eagerly, never after an ID change or teardown.
    onPanelIdChanged: Qt.callLater(restoreState)
    // A restored presentation can reuse this panel ID and its loaded body.
    onPersistedStateJsonChanged: {
        if (!stateReady || savingState)
            return;
        stateReady = false;
        restoredForPanelId = "";
        cancelEdit();
        Qt.callLater(restoreState);
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
    }

    // Escape cancels the one active gesture without leaving a partial edit.
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: parametersPanel.activeToken.length > 0
        onActivated: parametersPanel.cancelEdit()
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
            property real minCardWidth: 260
            // Two columns are shown only when they fit without horizontal
            // scrolling; the saved preference is preserved either way and is
            // never rewritten by a narrow layout.
            readonly property bool twoColumnLayout: parametersPanel.effectiveTwoColumns
            property real columnWidth: twoColumnLayout ? Math.max(minCardWidth, (inspectorScroll.availableWidth - columnGap) / 2) : Math.max(minCardWidth, inspectorScroll.availableWidth)
            width: twoColumnLayout ? columnWidth * 2 + columnGap : columnWidth
            height: Math.max(leftInspectorColumn.implicitHeight, rightInspectorColumn.implicitHeight)

            Column {
                id: leftInspectorColumn
                objectName: "inspectorColumn_0"
                width: inspectorContent.columnWidth
                spacing: inspectorContent.columnGap

                Repeater {
                    model: parametersPanel.columnEntries(0)
                    delegate: inspectorCardComponent
                }
            }

            Column {
                id: rightInspectorColumn
                objectName: "inspectorColumn_1"
                visible: inspectorContent.twoColumnLayout
                x: leftInspectorColumn.width + inspectorContent.columnGap
                width: inspectorContent.columnWidth
                spacing: inspectorContent.columnGap

                Repeater {
                    model: parametersPanel.columnEntries(1)
                    delegate: inspectorCardComponent
                }
            }
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
                        if (!info || info.available !== true)
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

            function parameterByKey(key) {
                for (var i = 0; i < sections.length; ++i) {
                    var params = sections[i].parameters || [];
                    for (var j = 0; j < params.length; ++j) {
                        if (String(params[j].key) === String(key))
                            return params[j];
                    }
                }
                return null;
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

            width: parent ? parent.width : implicitWidth
            height: bodyColumn.implicitHeight + 2
            implicitWidth: 260
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
                    Layout.preferredHeight: 30
                    spacing: 3

                    Rectangle {
                        Layout.preferredWidth: 7
                        Layout.preferredHeight: 7
                        radius: 3
                        color: theme.nodeCategoryColor(card.category)
                    }

                    Button {
                        id: collapseButton
                        flat: true
                        text: card.inspectorState.collapsed === true ? "\u25b8" : "\u25be"
                        implicitWidth: 19
                        objectName: "collapse_" + card.nodeId
                        padding: 0
                        onClicked: parametersPanel.setCollapsed(card.networkId, card.nodeId, card.inspectorState.collapsed !== true)
                        contentItem: Text {
                            text: collapseButton.text
                            color: theme.muted
                            font.pixelSize: theme.fontSize
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: card.displayName
                            color: theme.text
                            font.pixelSize: theme.fontSize
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            visible: card.width >= 330
                            text: card.displayType
                            color: theme.muted
                            font.pixelSize: Math.max(9, theme.fontSize - 1)
                            elide: Text.ElideRight
                            Layout.maximumWidth: 84
                        }
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
                        id: closeButton
                        objectName: "close_" + card.nodeId
                        flat: true
                        text: "\u00d7"
                        implicitWidth: 22
                        implicitHeight: 23
                        padding: 0
                        onClicked: parametersPanel.closeInspector(card.networkId, card.nodeId)
                        contentItem: Text {
                            text: closeButton.text
                            color: closeButton.down ? theme.accent : theme.muted
                            font.pixelSize: theme.fontSize + 1
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                ColumnLayout {
                    visible: card.inspectorState.collapsed !== true
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: card.sectionModel
                        delegate: sectionComponent
                    }

                    Text {
                        visible: !card.available && card.unavailableReason.length > 0
                        Layout.fillWidth: true
                        Layout.margins: 7
                        text: card.unavailableReason
                        color: theme.muted
                        font.pixelSize: theme.fontSize
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    // --- section ----------------------------------------------------------
    Component {
        id: sectionComponent
        Rectangle {
            id: section
            property var sectionData: modelData
            property bool collapsed: false
            Layout.fillWidth: true
            implicitHeight: sectionColumn.implicitHeight
            color: theme.panel
            radius: theme.smallRadius

            ColumnLayout {
                id: sectionColumn
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                }
                spacing: 0

                Button {
                    id: sectionButton
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    flat: true
                    padding: 5
                    text: (section.collapsed ? "\u25b8 " : "\u25be ") + String(section.sectionData.name)
                    contentItem: Text {
                        text: sectionButton.text
                        color: theme.text
                        font.pixelSize: theme.fontSize
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    onClicked: section.collapsed = !section.collapsed
                    background: Rectangle {
                        color: sectionButton.hovered ? theme.hover : theme.panel
                        radius: theme.smallRadius
                    }
                }

                ColumnLayout {
                    visible: !section.collapsed
                    Layout.fillWidth: true
                    Layout.leftMargin: 7
                    Layout.rightMargin: 7
                    Layout.bottomMargin: 5
                    spacing: 4

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
            readonly property var parameter: card ? card.parameterByKey(parameterRow.parameterKey) : null
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
            // Slider travel is the soft adjustment range when declared; it never
            // bounds a typed value.
            readonly property real sliderFrom: parameterRow.hasSoftMinimum ? parameterRow.softMinimum : (parameterRow.hasMinimum ? parameterRow.minimum : 0)
            readonly property real sliderTo: parameterRow.hasSoftMaximum ? parameterRow.softMaximum : (parameterRow.hasMaximum ? parameterRow.maximum : 1)
            readonly property bool boolValue: parameter ? parameter.value === true : false
            readonly property string stringValue: parameter && parameter.value !== undefined && parameter.value !== null ? String(parameter.value) : ""
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
            // The registered editor's declared host layout. A "section" editor
            // is an aggregate control: it spans the full row and provides its own
            // per-parameter affordances, so no outer label/key/marker cell wraps
            // it. No parameter type is special-cased here.
            readonly property string editorPresentation:
                parameterRow.customEditorActive && parameterRow.editorInfo && parameterRow.editorInfo.presentation !== undefined
                    ? String(parameterRow.editorInfo.presentation) : "row"
            readonly property bool sectionEditor: parameterRow.editorPresentation === "section"
            // Row-local feedback for the most recent rejected edit. The message
            // comes from the controller/catalog; the panel never re-validates.
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

            function removeKey() {
                if (!parametersPanel.controller || parameterRow.parameterKey.length === 0)
                    return false;
                return parametersPanel.controller.removeNodeParameterKey(parameterRow.networkId, parameterRow.nodeId, parameterRow.parameterKey);
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
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: rowMenu.popup()
            }

            RowLayout {
                id: rowLayout
                anchors.fill: parent
                spacing: 6

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

                ExposureLabel {
                    visible: !parameterRow.sectionEditor
                    Layout.preferredWidth: 72
                    Layout.minimumWidth: 44
                    Layout.maximumWidth: 72
                    Layout.alignment: Qt.AlignVCenter
                    theme: parametersPanel.theme
                    networkId: parameterRow.networkId
                    instanceId: parameterRow.instanceId
                    nodeId: parameterRow.nodeId
                    parameterKey: parameterRow.parameterKey
                    labelText: parameterRow.rowLabel
                    keyStatus: parameterRow.keyStatus
                    frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                    onKeyRequested: parameterRow.keyAtFrame()
                }

                KeyIndicator {
                    objectName: "key_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                    visible: !parameterRow.sectionEditor
                    Layout.preferredWidth: 24
                    Layout.maximumWidth: 24
                    Layout.alignment: Qt.AlignVCenter
                    theme: parametersPanel.theme
                    networkId: parameterRow.networkId
                    nodeId: parameterRow.nodeId
                    parameterKey: parameterRow.parameterKey
                    parameterLabel: parameterRow.rowLabel
                    keyStatus: parameterRow.keyStatus
                    scope: parameterRow.scope
                    frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                    revealAvailable: {
                        parametersPanel.revision;
                        return parametersPanel.groupHasAnimationPanel();
                    }
                    onKeyRequested: parameterRow.keyAtFrame()
                    onRemoveKeyRequested: parameterRow.removeKey()
                    onRevealRequested: parametersPanel.revealInAnimation(parameterRow.networkId, parameterRow.nodeId, parameterRow.parameterKey)
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
                        model: parameterRow.parameter && parameterRow.parameter.choices ? parameterRow.parameter.choices : []
                        currentIndex: parameterRow.choiceIndex
                        Layout.fillWidth: true
                        implicitHeight: 23
                        Accessible.name: parameterRow.rowLabel
                        onRevisionChanged: currentIndex = parameterRow.choiceIndex
                        onActivated: parameterRow.commitDiscrete(String(currentText))
                        MouseArea {
                            anchors.fill: parent
                            onPressed: function (mouse) {
                                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                            }
                            onClicked: parameterRow.keyAtFrame()
                        }
                    }

                    Switch {
                        id: toggleBox
                        visible: parameterRow.kind === "toggle" && !parameterRow.customEditorActive
                        property bool syncing: false
                        property bool ready: false
                        property int revision: parameterRow.revision
                        objectName: "toggle_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                        checked: parameterRow.boolValue
                        implicitWidth: 30
                        implicitHeight: 20
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        Accessible.name: parameterRow.rowLabel
                        Component.onCompleted: ready = true
                        onToggled: {
                            if (!ready || syncing)
                                return;
                            parameterRow.commitDiscrete(checked);
                        }
                        onRevisionChanged: {
                            syncing = true;
                            checked = parameterRow.boolValue;
                            syncing = false;
                        }
                        indicator: Rectangle {
                            x: 1
                            y: (toggleBox.height - height) / 2
                            width: 28
                            height: 16
                            radius: 8
                            color: toggleBox.checked ? theme.accent : theme.raised
                            border.color: theme.border
                            Rectangle {
                                x: toggleBox.checked ? parent.width - width - 2 : 2
                                y: 2
                                width: 12
                                height: 12
                                radius: 6
                                color: theme.text
                            }
                        }
                        contentItem: Item {
                        }
                        MouseArea {
                            anchors.fill: parent
                            onPressed: function (mouse) {
                                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                            }
                            onClicked: parameterRow.keyAtFrame()
                        }
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
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: parameterRow.componentLabels[index]
                                    color: theme.muted
                                    font.pixelSize: theme.fontSize
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                NumericField {
                                    objectName: "vector_" + index + "_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                                    theme: parametersPanel.theme
                                    value: parameterRow.componentValue(index)
                                    hasMinimum: parameterRow.hasMinimum
                                    hasMaximum: parameterRow.hasMaximum
                                    minimum: parameterRow.minimum
                                    maximum: parameterRow.maximum
                                    hasSoftMinimum: parameterRow.hasSoftMinimum
                                    hasSoftMaximum: parameterRow.hasSoftMaximum
                                    softMinimum: parameterRow.softMinimum
                                    softMaximum: parameterRow.softMaximum
                                    step: parameterRow.numberStep
                                    decimals: parameterRow.decimals
                                    integer: parameterRow.integerParameter
                                    label: parameterRow.rowLabel + " " + parameterRow.componentLabels[index]
                                    errorText: parameterRow.rowError
                                    dragThreshold: parameterRow.dragThreshold
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
                                    onScrubStarted: parametersPanel.beginScrub(parameterRow.rowRef())
                                    onScrubbed: parametersPanel.updateScrub(parameterRow.componentEdited(index, value))
                                    onScrubFinished: parametersPanel.finishScrub()
                                    onScrubCancelled: parametersPanel.cancelScrub()
                                    onKeyRequested: parameterRow.keyAtFrame()
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
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: parameterRow.componentLabels[index]
                                    color: theme.muted
                                    font.pixelSize: theme.fontSize
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                NumericField {
                                    objectName: "color_" + index + "_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                                    theme: parametersPanel.theme
                                    value: parameterRow.componentValue(index)
                                    hasMinimum: parameterRow.hasMinimum
                                    hasMaximum: parameterRow.hasMaximum
                                    minimum: parameterRow.minimum
                                    maximum: parameterRow.maximum
                                    hasSoftMinimum: parameterRow.hasSoftMinimum
                                    hasSoftMaximum: parameterRow.hasSoftMaximum
                                    softMinimum: parameterRow.softMinimum
                                    softMaximum: parameterRow.softMaximum
                                    step: parameterRow.numberStep
                                    decimals: parameterRow.decimals
                                    label: parameterRow.rowLabel + " " + parameterRow.componentLabels[index]
                                    errorText: parameterRow.rowError
                                    dragThreshold: parameterRow.dragThreshold
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
                                    onScrubStarted: parametersPanel.beginScrub(parameterRow.rowRef())
                                    onScrubbed: parametersPanel.updateScrub(parameterRow.componentEdited(index, value))
                                    onScrubFinished: parametersPanel.finishScrub()
                                    onScrubCancelled: parametersPanel.cancelScrub()
                                    onKeyRequested: parameterRow.keyAtFrame()
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
                        implicitHeight: 23
                        font.pixelSize: theme.fontSize
                        color: theme.text
                        selectByMouse: true
                        Accessible.name: parameterRow.rowLabel
                        onEditingFinished: parameterRow.commitDiscrete(String(text))
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

                    // Row-local error feedback for the non-numeric kinds; the
                    // numeric bundle renders its own.
                    Text {
                        objectName: "error_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                        visible: parameterRow.rowError.length > 0 && parameterRow.kind !== "number"
                        Layout.fillWidth: true
                        text: parameterRow.rowError
                        color: theme.errorText
                        font.pixelSize: Math.max(9, theme.fontSize - 1)
                        elide: Text.ElideRight
                        wrapMode: Text.WordWrap
                        Accessible.name: parameterRow.rowError
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
                        font.pixelSize: Math.max(9, theme.fontSize - 1)
                        elide: Text.ElideRight
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        visible: parameterRow.kind.length > 0 && parameterRow.kind !== "number" && parameterRow.kind !== "choice" && parameterRow.kind !== "toggle" && parameterRow.kind !== "vector2" && parameterRow.kind !== "vector3" && parameterRow.kind !== "color" && parameterRow.kind !== "string"
                        text: "Unsupported parameter kind '" + parameterRow.kind + "'"
                        color: theme.muted
                        font.pixelSize: theme.fontSize
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
                if ("panel" in item)
                    item.panel = parametersPanel;
            }

            // Right-click anywhere on the row opens the row actions; the
            // animation column keeps its own menu, and left clicks pass through
            // to the controls.
            Menu {
                id: rowMenu
                MenuItem {
                    text: "Reset Value"
                    enabled: parameterRow.modified
                    Accessible.name: "Reset value to the schema default"
                    onTriggered: parametersPanel.resetValue(parameterRow.rowRef())
                }
            }

        }
    }

    // The numeric control bundle: ONE owner for the soft-travel slider, the typed
    // field and the rejected-edit message used by an ordinary number row and by
    // a grouped pair row alike. Callers supply a `row` adapter object exposing
    // the same members both row kinds already have, plus whether the slider is
    // shown at this width. Grouping therefore adds no second control semantics.
    Component {
        id: numericControlComponent
        ColumnLayout {
            id: numericControl

            property var theme: null
            property var panel: null
            property var row: null
            // Live fit: a pair cell hides only the slider when there is no room
            // for it; the field, key, marker and error always remain.
            property bool showSlider: true
            property bool compact: false

            width: parent ? parent.width : implicitWidth
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                Slider {
                    id: bundleSlider
                    visible: numericControl.showSlider
                    objectName: numericControl.row ? "slider_" + numericControl.row.nodeId + "_" + numericControl.row.parameterKey : ""
                    property bool movedDuringPress: false
                    property int revision: numericControl.panel ? numericControl.panel.revision : 0
                    from: numericControl.row && numericControl.row.hasSoftMinimum ? numericControl.row.softMinimum : (numericControl.row && numericControl.row.hasMinimum ? numericControl.row.minimum : 0)
                    to: {
                        var low = bundleSlider.from;
                        var high = numericControl.row && numericControl.row.hasSoftMaximum ? numericControl.row.softMaximum
                                   : (numericControl.row && numericControl.row.hasMaximum ? numericControl.row.maximum : 1);
                        return Math.max(low + 1e-9, high);
                    }
                    stepSize: numericControl.row ? numericControl.row.numberStep : 0.01
                    snapMode: Slider.SnapAlways
                    value: Math.min(bundleSlider.to, Math.max(bundleSlider.from, numericControl.row ? numericControl.row.numberValue : 0))
                    Layout.fillWidth: true
                    implicitHeight: 20
                    Accessible.name: (numericControl.row ? numericControl.row.rowLabel : "") + " slider"
                    onMoved: {
                        movedDuringPress = true;
                        if (numericControl.panel && numericControl.row)
                            numericControl.panel.updateEdit(numericControl.row.integerParameter ? Math.round(value) : value);
                    }
                    onPressedChanged: {
                        if (pressed) {
                            movedDuringPress = false;
                            if (numericControl.panel && numericControl.row)
                                numericControl.panel.beginScrub(numericControl.row.rowRef());
                        } else if (numericControl.panel && numericControl.panel.activeToken.length > 0) {
                            if (movedDuringPress)
                                numericControl.panel.commitEdit();
                            else
                                numericControl.panel.cancelEdit();
                        }
                    }
                    Component.onDestruction: if (pressed && numericControl.panel)
                        numericControl.panel.cancelEdit()
                    onRevisionChanged: if (!pressed)
                        value = Math.min(bundleSlider.to, Math.max(bundleSlider.from, numericControl.row ? numericControl.row.numberValue : 0))
                    background: Rectangle {
                        x: 0
                        y: bundleSlider.topPadding + bundleSlider.availableHeight / 2 - height / 2
                        width: bundleSlider.availableWidth
                        height: 3
                        radius: 2
                        color: numericControl.theme.border
                    }
                    handle: Rectangle {
                        x: bundleSlider.leftPadding + bundleSlider.visualPosition * (bundleSlider.availableWidth - width)
                        y: bundleSlider.topPadding + bundleSlider.availableHeight / 2 - height / 2
                        width: 10
                        height: 10
                        radius: 5
                        color: numericControl.theme.accent
                    }
                    MouseArea {
                        anchors.fill: parent
                        onPressed: function (mouse) {
                            mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                        }
                        onClicked: if (numericControl.row)
                            numericControl.row.keyAtFrame()
                    }
                }

                NumericField {
                    id: bundleField
                    objectName: numericControl.row ? "param_" + numericControl.row.nodeId + "_" + numericControl.row.parameterKey : ""
                    theme: numericControl.theme
                    value: numericControl.row ? numericControl.row.numberValue : 0
                    text: numericControl.row && numericControl.row.exactText !== undefined ? numericControl.row.exactText : ""
                    hasMinimum: numericControl.row ? numericControl.row.hasMinimum : false
                    hasMaximum: numericControl.row ? numericControl.row.hasMaximum : false
                    minimum: numericControl.row ? numericControl.row.minimum : 0
                    maximum: numericControl.row ? numericControl.row.maximum : 0
                    hasSoftMinimum: numericControl.row ? numericControl.row.hasSoftMinimum : false
                    hasSoftMaximum: numericControl.row ? numericControl.row.hasSoftMaximum : false
                    softMinimum: numericControl.row ? numericControl.row.softMinimum : 0
                    softMaximum: numericControl.row ? numericControl.row.softMaximum : 0
                    step: numericControl.row ? numericControl.row.numberStep : 0.01
                    decimals: numericControl.row ? numericControl.row.decimals : -1
                    integer: numericControl.row ? numericControl.row.integerParameter : false
                    label: numericControl.row ? numericControl.row.rowLabel : ""
                    errorText: numericControl.row ? numericControl.row.rowError : ""
                    dragThreshold: numericControl.row ? numericControl.row.dragThreshold : 4
                    fieldWidth: numericControl.compact ? 56 : 62
                    Layout.fillWidth: true
                    Layout.minimumWidth: 56
                    Layout.preferredWidth: numericControl.compact ? 56 : 62
                    Layout.maximumWidth: numericControl.compact ? 56 : 1000000
                    Layout.alignment: Qt.AlignVCenter
                    onTextCommitted: function (text) {
                        if (numericControl.row)
                            numericControl.row.commitText(text);
                    }
                    onTextRejected: if (numericControl.panel && numericControl.row)
                        numericControl.panel.rejectText(numericControl.row.rowRef(), text)
                    onStepped: function (value) {
                        if (numericControl.row)
                            numericControl.row.commitDiscrete(value);
                    }
                    onScrubStarted: if (numericControl.panel && numericControl.row)
                        numericControl.panel.beginScrub(numericControl.row.rowRef())
                    onScrubbed: function (value) {
                        if (numericControl.panel)
                            numericControl.panel.updateScrub(value);
                    }
                    onScrubFinished: if (numericControl.panel)
                        numericControl.panel.finishScrub()
                    onScrubCancelled: if (numericControl.panel)
                        numericControl.panel.cancelScrub()
                    onKeyRequested: if (numericControl.row)
                        numericControl.row.keyAtFrame()
                }
            }

            Text {
                objectName: numericControl.row ? "error_" + numericControl.row.nodeId + "_" + numericControl.row.parameterKey : ""
                visible: numericControl.row ? numericControl.row.rowError.length > 0 : false
                Layout.fillWidth: true
                text: numericControl.row ? numericControl.row.rowError : ""
                color: numericControl.theme.errorText
                font.pixelSize: Math.max(9, numericControl.theme.fontSize - 1)
                elide: Text.ElideRight
                wrapMode: Text.WordWrap
                Accessible.name: numericControl.row ? numericControl.row.rowError : ""
            }
        }
    }

    // --- grouped row ------------------------------------------------------
    // Consecutive parameters sharing a schema `row` render side by side with
    // their labels as component labels (Transform "Translate" X/Y). Their
    // identities, animation channels, commands and exposure stay independent.
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

            implicitHeight: groupLayout.implicitHeight
            width: parent ? parent.width : implicitWidth
            Layout.fillWidth: true

            RowLayout {
                id: groupLayout
                anchors.fill: parent
                spacing: 6

                Rectangle {
                    Layout.preferredWidth: 6
                    Layout.maximumWidth: 6
                    Layout.preferredHeight: 6
                    Layout.alignment: Qt.AlignVCenter
                    radius: 3
                    color: "transparent"
                }

                Item {
                    Layout.preferredWidth: 72
                    Layout.minimumWidth: 44
                    Layout.maximumWidth: 72
                    Layout.alignment: Qt.AlignVCenter
                    implicitHeight: groupLabelText.implicitHeight
                    Text {
                        id: groupLabelText
                        anchors.fill: parent
                        text: rowGroup.rowName
                        color: theme.text
                        font.pixelSize: theme.fontSize
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        Accessible.name: rowGroup.rowName
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Repeater {
                        model: rowGroup.keys
                        delegate: RowLayout {
                            id: groupedRow
                            required property string modelData
                            Layout.fillWidth: true
                            spacing: 4

                            readonly property var parameter: rowGroup.card ? rowGroup.card.parameterByKey(groupedRow.modelData) : null
                            readonly property string componentLabel: parameter && parameter.label !== undefined && String(parameter.label).length > 0 ? String(parameter.label) : groupedRow.modelData
                            readonly property string keyStatus: {
                                parametersPanel.revision;
                                if (!parametersPanel.controller || groupedRow.modelData.length === 0)
                                    return "none";
                                return String(parametersPanel.controller.nodeParameterKeyStatus(rowGroup.networkId, rowGroup.nodeId, groupedRow.modelData));
                            }
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
                            }

                            ExposureLabel {
                                Layout.preferredWidth: 34
                                Layout.maximumWidth: 34
                                Layout.alignment: Qt.AlignVCenter
                                theme: parametersPanel.theme
                                networkId: rowGroup.networkId
                                instanceId: rowGroup.instanceId
                                nodeId: rowGroup.nodeId
                                parameterKey: groupedRow.modelData
                                labelText: groupedRow.componentLabel
                                keyStatus: groupedRow.keyStatus
                                frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
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
                            // field, prefix, marker, error and key cell always
                            // remain, and a keyed slider stays draggable when it
                            // fits.
                            Loader {
                                id: groupedBundle
                                Layout.fillWidth: true
                                sourceComponent: numericControlComponent
                                onLoaded: groupedRow.bindNumericBundle(item)
                            }

                            KeyIndicator {
                                objectName: "key_" + rowGroup.nodeId + "_" + groupedRow.modelData
                                Layout.preferredWidth: 24
                                Layout.maximumWidth: 24
                                Layout.alignment: Qt.AlignVCenter
                                theme: parametersPanel.theme
                                networkId: rowGroup.networkId
                                nodeId: rowGroup.nodeId
                                parameterKey: groupedRow.modelData
                                parameterLabel: rowGroup.rowName + " " + groupedRow.componentLabel
                                keyStatus: groupedRow.keyStatus
                                scope: groupedRow.scope
                                frame: parametersPanel.controller ? parametersPanel.controller.frame : 0
                                revealAvailable: {
                                    parametersPanel.revision;
                                    return parametersPanel.groupHasAnimationPanel();
                                }
                                onKeyRequested: groupedRow.keyAtFrame()
                                onRemoveKeyRequested: {
                                    if (parametersPanel.controller)
                                        parametersPanel.controller.removeNodeParameterKey(rowGroup.networkId, rowGroup.nodeId, groupedRow.modelData);
                                }
                                onRevealRequested: parametersPanel.revealInAnimation(rowGroup.networkId, rowGroup.nodeId, groupedRow.modelData)
                            }
                        }
                    }
                }
            }
        }
    }
}
