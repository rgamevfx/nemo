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
    property string restoredForPanelId: ""
    property bool stateReady: false

    // One global revision drives inspector re-queries. It advances on document,
    // frame, and catalog changes. A refresh requested mid-gesture is deferred so
    // the delegate under an active drag or edit is never rebuilt underneath it.
    property int revision: 0
    property bool refreshPending: false

    // Only one parameter gesture is active at a time (beginNodeParameterEdit
    // rejects a second begin). The panel tracks the single live token.
    property string activeToken: ""

    property alias headerTools: headerToolsComponent

    // --- identity helpers -------------------------------------------------

    function validIdentity(value) {
        return value !== undefined && value !== null && String(value).length > 0
    }

    function normalizedEntry(entry) {
        return {
            "network": String(entry.network),
            "node": String(entry.node),
            "pinned": entry.pinned === true,
            "collapsed": entry.collapsed === true
        }
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
        workspace.setPanelState(panelId, merged);
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
        if (!twoColumns)
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
        return token;
    }

    function beginEdit(row) {
        return row ? beginEditFor(row.networkId, row.nodeId, row.parameterKey) : "";
    }

    function updateEdit(value) {
        if (activeToken.length === 0)
            return false;
        return controller.updateNodeParameterEdit(activeToken, value);
    }

    function commitEdit() {
        if (activeToken.length === 0)
            return false;
        var result = controller.commitNodeParameterEdit(activeToken);
        activeToken = "";
        flushRefresh();
        return result;
    }

    function cancelEdit() {
        if (activeToken.length === 0)
            return false;
        var result = controller.cancelNodeParameterEdit(activeToken);
        activeToken = "";
        flushRefresh();
        return result;
    }

    // One discrete control action (toggle, choice, typed field, vector/color
    // component) is a single begin/update/commit gesture, hence one undo step.
    // A rejected value cancels instead of committing an empty transaction.
    function gestureSingle(row, value) {
        if (beginEdit(row).length === 0)
            return false;
        if (updateEdit(value) === false) {
            cancelEdit();
            return false;
        }
        return commitEdit();
    }

    function formatParameter(value, step) {
        return Number(value).toFixed(step >= 1 ? 0 : step < 0.01 ? 3 : 2);
    }

    function commitNumeric(row, text) {
        var parameter = row.parameter;
        if (!parameter)
            return false;
        var number = Number(text);
        if (!Number.isFinite(number))
            return false;
        if (row.integerParameter)
            number = Math.round(number);
        var minimum = parameter.minimum !== undefined ? Number(parameter.minimum) : number;
        var maximum = parameter.maximum !== undefined ? Number(parameter.maximum) : number;
        number = Math.max(minimum, Math.min(maximum, number));
        var step = parameter.step !== undefined ? Number(parameter.step) : 0;
        if (step > 0)
            number = minimum + Math.round((number - minimum) / step) * step;
        return gestureSingle(row, number);
    }

    function commitVectorComponent(row, index, text) {
        var value = row.parameter ? row.parameter.value : null;
        if (!value || value.length === undefined)
            return false;
        var number = Number(text);
        if (!Number.isFinite(number))
            return false;
        var next = [];
        for (var i = 0; i < value.length; ++i)
            next.push(Number(value[i]));
        next[index] = number;
        return gestureSingle(row, next);
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
        restoreState();
        revision++;
    }

    onPanelIdChanged: {
        if (restoredForPanelId && restoredForPanelId !== panelId)
            saveState();
        restoreState();
    }

    Component.onDestruction: saveState()

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
            property real columnWidth: parametersPanel.twoColumns
                                           ? Math.max(minCardWidth, (inspectorScroll.availableWidth - columnGap) / 2)
                                           : Math.max(minCardWidth, inspectorScroll.availableWidth)
            width: parametersPanel.twoColumns ? columnWidth * 2 + columnGap : columnWidth
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
                visible: parametersPanel.twoColumns
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
            property string networkId: delegateData && delegateData.network !== undefined
                                       ? String(delegateData.network) : ""
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
            readonly property string displayName: card.available
                                                  ? (inspector.name !== undefined && String(inspector.name).length > 0
                                                     ? String(inspector.name) : card.nodeId)
                                                  : card.nodeId + " (unavailable)"
            readonly property string displayType: card.available && inspector.type !== undefined
                                                  ? String(inspector.type) : ""
            readonly property string category: card.available && inspector.category !== undefined
                                               ? String(inspector.category) : "Utility"
            readonly property string instanceId: card.available && inspector.instanceId !== undefined
                                                 ? String(inspector.instanceId) : ""
            readonly property string unavailableReason: card.available ? ""
                                                          : (inspector && inspector.reason !== undefined
                                                             ? String(inspector.reason) : "")
            readonly property var sections: card.available && inspector.sections ? inspector.sections : []

            // The section shell only rebuilds when the schema shape changes, so
            // collapsed sections and scroll survive value/frame refreshes. Rows
            // re-read values by key through parameterByKey().
            property var sectionModel: []
            property string sectionSignature: ""

            function refreshSectionModel() {
                var signature = "";
                var model = [];
                for (var i = 0; i < sections.length; ++i) {
                    var section = sections[i];
                    var keys = [];
                    var params = section.parameters || [];
                    for (var j = 0; j < params.length; ++j)
                        keys.push(String(params[j].key));
                    signature += String(section.name) + ":" + keys.join(",") + "|";
                    model.push({
                        "name": String(section.name),
                        "keys": keys
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
                        onClicked: parametersPanel.setCollapsed(card.networkId, card.nodeId,
                                                                card.inspectorState.collapsed !== true)
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

                    Button {
                        id: pinButton
                        objectName: "pin_" + card.nodeId
                        flat: true
                        implicitWidth: 22
                        implicitHeight: 23
                        padding: 0
                        Accessible.name: card.inspectorState.pinned ? "Unpin inspector" : "Pin inspector"
                        onClicked: parametersPanel.setPinned(card.networkId, card.nodeId,
                                                             card.inspectorState.pinned !== true)
                        contentItem: Canvas {
                            id: pinGlyph
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.reset();
                                ctx.strokeStyle = card.inspectorState.pinned ? theme.accent : theme.muted;
                                ctx.fillStyle = ctx.strokeStyle;
                                ctx.lineWidth = 1;
                                var cx = width / 2, cy = height / 2;
                                ctx.beginPath();
                                ctx.moveTo(cx - 3, cy - 5);
                                ctx.lineTo(cx + 3, cy - 5);
                                ctx.lineTo(cx + 2, cy - 1);
                                ctx.lineTo(cx + 4, cy + 2);
                                ctx.lineTo(cx - 4, cy + 2);
                                ctx.lineTo(cx - 2, cy - 1);
                                ctx.closePath();
                                if (card.inspectorState.pinned)
                                    ctx.fill();
                                else
                                    ctx.stroke();
                                ctx.beginPath();
                                ctx.moveTo(cx, cy + 2);
                                ctx.lineTo(cx, cy + 6);
                                ctx.stroke();
                            }
                            Connections {
                                target: theme
                                function onAccentChanged() {
                                    pinGlyph.requestPaint();
                                }
                                function onMutedChanged() {
                                    pinGlyph.requestPaint();
                                }
                            }
                        }
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
                        model: section.sectionData.keys
                        delegate: parameterComponent
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

            readonly property var card: parametersPanel.owningCard(parameterRow)
            readonly property int revision: parametersPanel.revision
            readonly property string parameterKey: String(modelData)
            readonly property string networkId: card ? card.networkId : ""
            readonly property string instanceId: card ? card.instanceId : ""
            readonly property string nodeId: card ? card.nodeId : ""
            readonly property var parameter: card ? card.parameterByKey(parameterRow.parameterKey) : null
            readonly property string kind: parameter && parameter.kind !== undefined ? String(parameter.kind) : ""
            readonly property string keyStatus: {
                parametersPanel.revision;
                if (!parametersPanel.controller || parameterRow.parameterKey.length === 0)
                    return "none";
                return String(parametersPanel.controller.nodeParameterKeyStatus(parameterRow.networkId, parameterRow.nodeId,
                                                                                parameterRow.parameterKey));
            }
            readonly property real numberValue: parameter ? Number(parameter.value) : 0
            readonly property bool integerParameter: parameter && String(parameter.type) === "integer"
            readonly property real numberStep: parameterRow.integerParameter
                                                ? 1 : (parameter && parameter.step !== undefined
                                                       ? Number(parameter.step) : 0.01)
            readonly property string formattedNumber: parameterRow.integerParameter
                                                      ? String(Math.round(parameterRow.numberValue))
                                                      : parametersPanel.formatParameter(parameterRow.numberValue,
                                                                                         parameter && parameter.step !== undefined
                                                                                         ? parameter.step : undefined)
            readonly property bool boolValue: parameter ? parameter.value === true : false
            readonly property string stringValue: parameter && parameter.value !== undefined
                                                  ? String(parameter.value) : ""
            readonly property int choiceIndex: {
                if (!parameter || !parameter.choices)
                    return 0;
                var index = parameter.choices.indexOf(parameter.value);
                return index < 0 ? 0 : index;
            }
            readonly property color colorValue: {
                var value = parameter ? parameter.value : null;
                if (!value || value.length === undefined || value.length < 3)
                    return "transparent";
                return Qt.rgba(Number(value[0]), Number(value[1]), Number(value[2]), value.length > 3 ? Number(value[3]) : 1);
            }
            readonly property bool hasCustomEditor: parameter && parameter.editor !== undefined
                                                    && String(parameter.editor).length > 0
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

            // Identity payload consumed by the #49 exposure popout. Numeric ids
            // stay decimal strings so JavaScript never rounds them.
            readonly property var dragPayload: QtObject {
                readonly property string networkId: parameterRow.networkId
                readonly property string instanceId: parameterRow.instanceId
                readonly property string nodeId: parameterRow.nodeId
                readonly property string parameterKey: parameterRow.parameterKey
            }

            implicitHeight: Math.max(labelText.implicitHeight, controlColumn.implicitHeight)
            Layout.fillWidth: true

            Drag.active: labelDrag.active
            Drag.source: parameterRow.dragPayload
            Drag.hotSpot: Qt.point(0, 0)

            function componentValue(index) {
                var value = parameter ? parameter.value : null;
                if (!value || value.length === undefined || index >= value.length)
                    return "0";
                return parametersPanel.formatParameter(Number(value[index]),
                                                       parameter.step !== undefined ? parameter.step : undefined);
            }

            function keyAtFrame() {
                if (!parametersPanel.controller || parameterRow.parameterKey.length === 0)
                    return false;
                return parametersPanel.controller.keyNodeParameter(parameterRow.networkId, parameterRow.nodeId,
                                                                   parameterRow.parameterKey);
            }

            function removeKey() {
                if (!parametersPanel.controller || parameterRow.parameterKey.length === 0)
                    return false;
                return parametersPanel.controller.removeNodeParameterKey(parameterRow.networkId, parameterRow.nodeId,
                                                                         parameterRow.parameterKey);
            }

            function altOnly(mouse) {
                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
            }

            RowLayout {
                anchors.fill: parent
                spacing: 6

                Item {
                    id: labelRegion
                    Layout.preferredWidth: 78
                    Layout.minimumWidth: 48
                    Layout.alignment: Qt.AlignVCenter
                    implicitHeight: labelText.implicitHeight

                    Text {
                        id: labelText
                        anchors.fill: parent
                        objectName: "label_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                        text: parameterRow.parameter && parameterRow.parameter.label !== undefined
                              && String(parameterRow.parameter.label).length > 0
                              ? String(parameterRow.parameter.label) : parameterRow.parameterKey
                        color: theme.text
                        font.pixelSize: theme.fontSize
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }

                    // Alt-click the label keys at the current frame; a plain
                    // press is released so the label stays selectable for #49.
                    MouseArea {
                        anchors.fill: parent
                        onPressed: parameterRow.altOnly(mouse)
                        onClicked: parameterRow.keyAtFrame()
                    }

                    DragHandler {
                        id: labelDrag
                        target: null
                        acceptedButtons: Qt.LeftButton
                    }

                    ToolTip.visible: labelHover.hovered
                    ToolTip.text: parameterRow.keyStatus === "key"
                                  ? "Keyed at frame " + parametersPanel.controller.frame
                                  : parameterRow.keyStatus === "animated"
                                    ? "Animated; Alt-click to key at frame " + parametersPanel.controller.frame
                                    : "Alt-click to add a key at frame " + parametersPanel.controller.frame
                    HoverHandler {
                        id: labelHover
                    }
                }

                Rectangle {
                    id: keyIndicator
                    objectName: "key_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 22
                    Layout.alignment: Qt.AlignVCenter
                    radius: theme.smallRadius
                    color: keyIndicatorMouse.containsMouse ? theme.hover : "transparent"
                    border.width: 1
                    border.color: parameterRow.keyStatus === "key" ? theme.accent
                                  : parameterRow.keyStatus === "animated" ? theme.muted : theme.border
                    Accessible.name: "Animation key status: " + parameterRow.keyStatus
                    Accessible.description: "Click to insert or update a key at the current frame. Right-click to remove a key at the current frame."

                    Text {
                        anchors.centerIn: parent
                        text: parameterRow.keyStatus === "key" ? "\u25c6"
                              : parameterRow.keyStatus === "animated" ? "\u25c7" : "\u25cb"
                        color: parameterRow.keyStatus === "none" ? theme.muted : theme.accent
                        font.pixelSize: 15
                    }

                    MouseArea {
                        id: keyIndicatorMouse
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        hoverEnabled: true
                        onClicked: function (mouse) {
                            if (mouse.button === Qt.RightButton) {
                                keyContextMenu.popup();
                                return;
                            }
                            parameterRow.keyAtFrame();
                        }
                    }

                    Menu {
                        id: keyContextMenu
                        MenuItem {
                            text: "Key at frame " + parametersPanel.controller.frame
                            onTriggered: parameterRow.keyAtFrame()
                        }
                        MenuItem {
                            text: "Remove key at frame " + parametersPanel.controller.frame
                            enabled: parameterRow.keyStatus === "key"
                            onTriggered: parameterRow.removeKey()
                        }
                    }

                    ToolTip.visible: indicatorHover.hovered
                    ToolTip.text: parameterRow.keyStatus === "key"
                                  ? "Key at frame " + parametersPanel.controller.frame + ". Click to update; right-click to remove."
                                  : parameterRow.keyStatus === "animated"
                                    ? "Animated parameter. Click to add a key at frame " + parametersPanel.controller.frame
                                    : "Not animated. Click to add a key at frame " + parametersPanel.controller.frame
                    HoverHandler {
                        id: indicatorHover
                    }
                }

                ColumnLayout {
                    id: controlColumn
                    Layout.fillWidth: true
                    spacing: 2

                    // number: slider + typed field. A drag is one gesture.
                    RowLayout {
                        visible: parameterRow.kind === "number" && !parameterRow.customEditorActive
                        Layout.fillWidth: true
                        spacing: 4

                        Slider {
                            id: numberSlider
                            property int revision: parameterRow.revision
                            objectName: "slider_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                            from: parameterRow.parameter && parameterRow.parameter.minimum !== undefined
                                  ? Number(parameterRow.parameter.minimum) : 0
                            to: parameterRow.parameter && parameterRow.parameter.maximum !== undefined
                                ? Number(parameterRow.parameter.maximum) : 1
                            stepSize: parameterRow.numberStep
                            snapMode: Slider.SnapAlways
                            value: parameterRow.numberValue
                            Layout.fillWidth: true
                            implicitHeight: 20
                            onMoved: parametersPanel.updateEdit(value)
                            onPressedChanged: {
                                if (pressed) {
                                    parametersPanel.beginEdit(parameterRow);
                                } else if (parametersPanel.activeToken.length > 0) {
                                    parametersPanel.commitEdit();
                                } else {
                                    numberSlider.value = parameterRow.numberValue;
                                }
                            }
                            Component.onDestruction: if (pressed)
                                parametersPanel.cancelEdit()
                            onRevisionChanged: if (!pressed)
                                value = parameterRow.numberValue
                            background: Rectangle {
                                x: 0
                                y: numberSlider.topPadding + numberSlider.availableHeight / 2 - height / 2
                                width: numberSlider.availableWidth
                                height: 3
                                radius: 2
                                color: theme.border
                            }
                            handle: Rectangle {
                                x: numberSlider.leftPadding + numberSlider.visualPosition * (numberSlider.availableWidth - width)
                                y: numberSlider.topPadding + numberSlider.availableHeight / 2 - height / 2
                                width: 10
                                height: 10
                                radius: 5
                                color: theme.accent
                            }
                            MouseArea {
                                anchors.fill: parent
                                onPressed: parameterRow.altOnly(mouse)
                                onClicked: parameterRow.keyAtFrame()
                            }
                        }

                        TextField {
                            id: numberField
                            property bool pendingNumericEdit: false
                            property int revision: parameterRow.revision
                            objectName: "param_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                            text: parameterRow.formattedNumber
                            implicitWidth: 52
                            implicitHeight: 23
                            font.pixelSize: theme.fontSize
                            color: theme.text
                            selectByMouse: true
                            horizontalAlignment: Text.AlignRight
                            validator: DoubleValidator {
                                bottom: parameterRow.parameter && parameterRow.parameter.minimum !== undefined
                                        ? Number(parameterRow.parameter.minimum) : -1e9
                                top: parameterRow.parameter && parameterRow.parameter.maximum !== undefined
                                     ? Number(parameterRow.parameter.maximum) : 1e9
                            }
                            onEditingFinished: {
                                if (pendingNumericEdit)
                                    parametersPanel.commitNumeric(parameterRow, numberField.text);
                                pendingNumericEdit = false;
                            }
                            onTextEdited: pendingNumericEdit = true
                            Keys.onEscapePressed: function (event) {
                                event.accepted = true;
                                pendingNumericEdit = false;
                                numberField.text = parameterRow.formattedNumber;
                            }
                            onRevisionChanged: {
                                if (!activeFocus || !pendingNumericEdit) {
                                    numberField.text = parameterRow.formattedNumber;
                                    pendingNumericEdit = false;
                                }
                            }
                            background: Rectangle {
                                color: theme.field
                                border.color: numberField.activeFocus ? theme.accent : theme.border
                                radius: theme.smallRadius
                            }
                            MouseArea {
                                anchors.fill: parent
                                onPressed: parameterRow.altOnly(mouse)
                                onClicked: parameterRow.keyAtFrame()
                            }
                        }
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
                        onRevisionChanged: currentIndex = parameterRow.choiceIndex
                        onActivated: parametersPanel.gestureSingle(parameterRow, String(currentText))
                        MouseArea {
                            anchors.fill: parent
                            onPressed: parameterRow.altOnly(mouse)
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
                        Component.onCompleted: ready = true
                        onToggled: {
                            if (!ready || syncing)
                                return;
                            parametersPanel.gestureSingle(parameterRow, checked);
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
                            onPressed: parameterRow.altOnly(mouse)
                            onClicked: parameterRow.keyAtFrame()
                        }
                    }

                    // vector2/vector3: N numeric fields (owner-approved new design).
                    RowLayout {
                        visible: (parameterRow.kind === "vector2" || parameterRow.kind === "vector3")
                                 && !parameterRow.customEditorActive
                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: parameterRow.kind === "vector3" ? 3 : 2
                            delegate: TextField {
                                required property int index
                                property int revision: parameterRow.revision
                                objectName: "vector_" + index + "_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                                text: parameterRow.componentValue(index)
                                Layout.fillWidth: true
                                implicitHeight: 23
                                font.pixelSize: theme.fontSize
                                color: theme.text
                                selectByMouse: true
                                horizontalAlignment: Text.AlignRight
                                validator: DoubleValidator {
                                }
                                onEditingFinished: parametersPanel.commitVectorComponent(parameterRow, index, text)
                                Keys.onEscapePressed: function (event) {
                                    event.accepted = true;
                                    text = parameterRow.componentValue(index);
                                }
                                onRevisionChanged: if (!activeFocus)
                                    text = parameterRow.componentValue(index)
                                background: Rectangle {
                                    color: theme.field
                                    border.color: activeFocus ? theme.accent : theme.border
                                    radius: theme.smallRadius
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onPressed: parameterRow.altOnly(mouse)
                                    onClicked: parameterRow.keyAtFrame()
                                }
                            }
                        }
                    }

                    // color: swatch + 4 numeric fields (owner-approved new design).
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
                        }

                        Repeater {
                            model: 4
                            delegate: TextField {
                                required property int index
                                property int revision: parameterRow.revision
                                objectName: "color_" + index + "_" + parameterRow.nodeId + "_" + parameterRow.parameterKey
                                text: parameterRow.componentValue(index)
                                Layout.fillWidth: true
                                implicitHeight: 23
                                font.pixelSize: theme.fontSize
                                color: theme.text
                                selectByMouse: true
                                horizontalAlignment: Text.AlignRight
                                validator: DoubleValidator {
                                }
                                onEditingFinished: parametersPanel.commitVectorComponent(parameterRow, index, text)
                                Keys.onEscapePressed: function (event) {
                                    event.accepted = true;
                                    text = parameterRow.componentValue(index);
                                }
                                onRevisionChanged: if (!activeFocus)
                                    text = parameterRow.componentValue(index)
                                background: Rectangle {
                                    color: theme.field
                                    border.color: activeFocus ? theme.accent : theme.border
                                    radius: theme.smallRadius
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onPressed: parameterRow.altOnly(mouse)
                                    onClicked: parameterRow.keyAtFrame()
                                }
                            }
                        }
                    }

                    // string: single text field (owner-approved new design).
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
                        onEditingFinished: parametersPanel.gestureSingle(parameterRow, String(text))
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
                            onPressed: parameterRow.altOnly(mouse)
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
                        Layout.fillWidth: true
                        source: parameterRow.editorAvailable ? parameterRow.editorInfo.source : ""
                        onLoaded: {
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
                    }

                    Text {
                        visible: parameterRow.hasCustomEditor && !parameterRow.editorAvailable
                        Layout.fillWidth: true
                        text: parameterRow.editorInfo && parameterRow.editorInfo.reason !== undefined
                              && String(parameterRow.editorInfo.reason).length > 0
                              ? String(parameterRow.editorInfo.reason)
                              : "Parameter editor '" + parameterRow.editorId + "' is unavailable"
                        color: theme.muted
                        font.pixelSize: Math.max(9, theme.fontSize - 1)
                        elide: Text.ElideRight
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        visible: parameterRow.kind !== "" && parameterRow.kind !== "number"
                                 && parameterRow.kind !== "choice" && parameterRow.kind !== "toggle"
                                 && parameterRow.kind !== "vector2" && parameterRow.kind !== "vector3"
                                 && parameterRow.kind !== "color" && parameterRow.kind !== "string"
                        text: "Unsupported parameter kind '" + parameterRow.kind + "'"
                        color: theme.muted
                        font.pixelSize: theme.fontSize
                    }
                }
            }
        }
    }
}
