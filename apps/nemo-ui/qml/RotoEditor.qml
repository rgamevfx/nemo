import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Roto shape editor (issue #93), hosted by the generic inspector through
// ParameterEditorRegistry id "nemo.roto.shapes" with presentation "section".
// The host mounts it on the `opacity` row and consumes that one key, so this
// editor renders the global opacity through the host's own numeric control and
// adds the shape controls beside it.
//
// Everything authored goes through the shared owners: the global opacity is a
// node parameter edit of the host panel, and every element/point property is a
// value gesture of the shared RotoController, which submits to the same session
// commands as any other parameter. Nothing here re-implements validation,
// keying, undo or geometry: the tree and the numbers are reads of the session's
// published state through that controller.
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
    // time; the panel keeps its own token for the opacity row).
    property var scrub: null
    onScrubChanged: {
        if (typeof historyController !== "undefined" && historyController)
            historyController.setGesture(editor, editor.scrub !== null);
    }

    readonly property color textColor: editor.theme ? editor.theme.text : "#dce0e6"
    readonly property color mutedColor: editor.theme ? editor.theme.muted : "#979ea8"
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

    // The inspector query addresses the real node that owns the parameters (a
    // definition network for an occurrence exposure); the same scope owns the
    // Roto data, so both the tree and the gestures stay in one scope.
    readonly property string queryNetwork: editor.parameter && editor.parameter.targetNetwork !== undefined ? String(editor.parameter.targetNetwork) : editor.networkId
    readonly property string queryNode: editor.parameter && editor.parameter.targetNode !== undefined ? String(editor.parameter.targetNode) : editor.nodeId
    readonly property string group: editor.panel && editor.panel.panelGroup !== undefined ? String(editor.panel.panelGroup) : "A"
    readonly property int revision: editor.roto ? Number(editor.roto.revision) : 0
    readonly property int frame: editor.panel && editor.panel.panelContext && editor.panel.panelContext.timelineClock !== undefined
            ? Number(editor.panel.panelContext.timelineClock) : (editor.controller ? Number(editor.controller.frame) : 0)
    readonly property int dragThreshold: editor.controller ? Number(editor.controller.dragDistance) : 4
    readonly property var elements: editor.roto ? (editor.roto.elements || []) : []
    readonly property var points: editor.roto ? (editor.roto.points || []) : []
    readonly property string selectedElement: editor.roto ? String(editor.roto.selectedElement || "") : ""
    readonly property bool selectedIsGroup: {
        var found = editor.elementRecord(editor.selectedElement);
        return found ? found.group === true : false;
    }
    readonly property bool selectedIsLocked: {
        var found = editor.elementRecord(editor.selectedElement);
        return found ? found.locked === true : false;
    }
    // Groups this element can be reparented under, plus the root.
    readonly property var parentChoices: {
        var choices = [{
            "id": "",
            "name": "root"
        }];
        for (var index = 0; index < editor.elements.length; ++index) {
            var entry = editor.elements[index];
            if (entry.group === true && String(entry.id) !== editor.selectedElement)
                choices.push({
                    "id": String(entry.id),
                    "name": String(entry.name)
                });
        }
        return choices;
    }

    objectName: "rotoShapeEditor_" + editor.nodeId
    Layout.fillWidth: true
    spacing: 3

    onFrameChanged: editor.syncFrame()
    onGroupChanged: editor.bind()
    onQueryNetworkChanged: editor.bind()
    onQueryNodeChanged: editor.bind()
    onControllerChanged: editor.bind()
    onParameterChanged: editor.bind()
    onPanelChanged: editor.bind()
    Component.onCompleted: editor.bind()
    Component.onDestruction: {
        editor.cancelScrub();
        if (typeof historyController !== "undefined" && historyController)
            historyController.setGesture(editor, false);
    }
    Connections {
        target: editor.roto
        function onGestureChanged() {
            if (!editor.roto.gestureActive)
                editor.scrub = null;
        }
    }
    Connections {
        target: editor.Window.window
        function onActiveChanged() {
            if (!editor.Window.window.active)
                editor.cancelScrub();
        }
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: editor.scrub !== null
        onActivated: editor.cancelScrub()
    }

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
        created.frame = editor.frame;
        if (created.available === true && String(created.selectedElement || "").length === 0 && (created.elements || []).length > 0)
            created.selectElement(String(created.elements[0].id), false);
    }

    function syncFrame() {
        if (editor.roto)
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

    // The authored lifetime bound of the selected element, falling back to the
    // current frame so the field always states a usable number.
    function lifetimeBound(first) {
        var found = editor.elementRecord(editor.selectedElement);
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

    function keyAtFrame(elementId, pointId, key) {
        return editor.roto ? editor.roto.keyAtFrame(String(elementId), pointId ? String(pointId) : "", String(key)) : false;
    }

    function removeKeyAtFrame(elementId, pointId, key) {
        return editor.roto ? editor.roto.removeKeyAtFrame(String(elementId), pointId ? String(pointId) : "", String(key)) : false;
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

    function gestureLive() {
        return editor.roto ? editor.roto.gestureActive === true : false;
    }

    // --- structure ---------------------------------------------------------
    function addGroup() {
        var parent = editor.selectedIsGroup ? editor.selectedElement : "";
        if (editor.roto)
            editor.roto.addGroup(parent);
    }

    function removeSelected() {
        if (editor.roto && editor.selectedElement.length > 0)
            editor.roto.removeElements([editor.selectedElement]);
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

    function reparentSelected(parentId) {
        if (editor.roto && editor.selectedElement.length > 0)
            editor.roto.reparentElement(editor.selectedElement, String(parentId), -1);
    }

    function addPoint() {
        if (!editor.roto || editor.selectedElement.length === 0 || editor.selectedIsGroup)
            return;
        // Inserted after the last selected point, or appended: the new point
        // lands in the segment the author is working on.
        var selected = editor.roto.selectedPoints || [];
        var index = -1;
        for (var position = 0; position < editor.points.length; ++position) {
            if (selected.indexOf(String(editor.points[position].id)) >= 0) {
                index = position + 1;
                break;
            }
        }
        var x = 0;
        var y = 0;
        if (editor.points.length >= 2) {
            var at = index < 0 ? editor.points.length - 1 : (index - 1) % editor.points.length;
            var from = editor.points[at];
            var to = editor.points[(at + 1) % editor.points.length];
            x = (Number(from.x) + Number(to.x)) / 2;
            y = (Number(from.y) + Number(to.y)) / 2;
        }
        editor.roto.addPoint(editor.selectedElement, index, x, y);
    }

    function removePoints() {
        if (!editor.roto || !editor.roto.selectedPoints || editor.roto.selectedPoints.length === 0)
            return;
        editor.roto.removePoints(editor.selectedElement, editor.roto.selectedPoints);
    }

    function setSmooth(smooth) {
        if (!editor.roto || editor.selectedElement.length === 0)
            return;
        editor.roto.setSmooth(editor.selectedElement, editor.roto.selectedPoints || [], smooth);
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
        implicitHeight: 21
        padding: 4
        font.pixelSize: editor.fontSizeValue
        contentItem: Text {
            text: action.text
            color: editor.textColor
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
            Layout.preferredWidth: 84
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
            enabled: editor.roto !== null && editor.roto.available === true && !editor.selectedIsLocked
            gestureLive: editor.gestureLive()
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
            Layout.preferredWidth: 84
        }
        Repeater {
            model: ["X", "Y"]
            delegate: NumericField {
                id: componentField
                required property string modelData
                required property int index
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
                enabled: editor.roto !== null && editor.roto.available === true && !editor.selectedIsLocked
                gestureLive: editor.gestureLive()
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

    // One flag over a keyable element property (visible / inverted /
    // featherEnabled): a discrete value gesture, so a keyed flag is keyed at
    // the current frame like every other parameter.
    component FlagRow: RowLayout {
        id: flagRow
        required property string fieldKey
        required property string fieldLabel
        Layout.fillWidth: true
        spacing: 4
        Cell {
            editKey: flagRow.fieldKey
            elementId: editor.selectedElement
            pointId: ""
            labelText: flagRow.fieldLabel
            Layout.preferredWidth: 84
        }
        CheckBox {
            id: flagBox
            objectName: "rotoFlag_" + editor.nodeId + "_" + flagRow.fieldKey
            checked: editor.flagValue(editor.selectedElement, "", flagRow.fieldKey)
            text: flagRow.fieldLabel
            font.pixelSize: editor.fontSizeValue
            padding: 0
            leftPadding: 16
            implicitHeight: 23
            contentItem: Text {
                text: flagBox.checked ? "on" : "off"
                color: editor.textColor
                font.pixelSize: editor.fontSizeValue
                leftPadding: 16
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
            onToggled: {
                // The checked binding is re-evaluated from the published value;
                // only a real change is authored, so a refresh never re-authors.
                if (checked === editor.flagValue(editor.selectedElement, "", flagRow.fieldKey))
                    return;
                editor.commitFlag(editor.selectedElement, "", flagRow.fieldKey, checked);
            }
        }
        Item { Layout.fillWidth: true }
    }

    // --- global opacity (the consumed row this editor renders) -------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: "opacity"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.preferredWidth: Math.max(42, Math.min(84, editor.width * 0.3))
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
            readonly property string rowLabel: "Opacity"
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
            readonly property int dragThreshold: editor.dragThreshold
            readonly property string rowError: editor.panel && editor.panel.gestureErrorKey === "opacity" ? String(editor.panel.gestureError) : ""
            function rowRef() {
                return {
                    "networkId": editor.networkId,
                    "nodeId": editor.nodeId,
                    "parameterKey": "opacity",
                    "parameter": editor.parameter,
                    "label": "Opacity"
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

    // --- hierarchy ---------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            text: "shapes"
            color: editor.textColor
            font.pixelSize: editor.fontSizeValue
            Layout.fillWidth: true
        }
        Action {
            objectName: "rotoAddGroup_" + editor.nodeId
            text: "Group"
            enabled: editor.roto !== null && editor.roto.available === true
            onClicked: editor.addGroup()
            ToolTip.visible: hovered
            ToolTip.text: "Add a group; a selected group owns the new group"
        }
        Action {
            objectName: "rotoRemove_" + editor.nodeId
            text: "Remove"
            enabled: editor.selectedElement.length > 0 && !editor.selectedIsLocked
            onClicked: editor.removeSelected()
            ToolTip.visible: hovered
            ToolTip.text: "Remove the selected element and everything it owns"
        }
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(160, Math.max(42, editor.elements.length * 21 + 2))
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
            model: editor.elements
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                id: treeRow
                required property var modelData
                width: tree.width
                height: 21
                color: String(modelData.id) === editor.selectedElement ? editor.raisedColor : "transparent"
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (editor.roto)
                            editor.roto.selectElement(String(treeRow.modelData.id), false);
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 2 + 10 * Number(treeRow.modelData.depth)
                    spacing: 2
                    Text {
                        text: treeRow.modelData.group === true ? "\u25A3" : "\u25CB"
                        color: editor.mutedColor
                        font.pixelSize: editor.smallFontSize
                    }
                    Text {
                        objectName: "rotoName_" + editor.nodeId + "_" + treeRow.modelData.id
                        text: String(treeRow.modelData.name)
                        color: editor.textColor
                        font.pixelSize: editor.fontSizeValue
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Action {
                        objectName: "rotoVisible_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 19
                        text: treeRow.modelData.visible === true ? "\u25C9" : "\u25CB"
                        Accessible.name: "Visibility of " + String(treeRow.modelData.name)
                        onClicked: editor.toggleVisible(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Toggle this element's visibility at the current frame"
                    }
                    Action {
                        objectName: "rotoLock_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 19
                        text: treeRow.modelData.locked === true ? "\u2298" : "\u2013"
                        Accessible.name: "Lock of " + String(treeRow.modelData.name)
                        onClicked: editor.toggleLocked(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Lock or unlock this element; a locked element accepts no edit"
                    }
                    Action {
                        objectName: "rotoBlend_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 19
                        text: String(treeRow.modelData.blend) === "intersect" ? "\u2229"
                              : String(treeRow.modelData.blend) === "subtract" ? "\u2212" : "\u222A"
                        Accessible.name: "Blend operation of " + String(treeRow.modelData.name)
                        onClicked: editor.cycleBlend(treeRow.modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: "Combine, intersect or subtract against the accumulated matte"
                    }
                    Action {
                        objectName: "rotoUp_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 19
                        text: "\u25B4"
                        Accessible.name: "Move " + String(treeRow.modelData.name) + " earlier"
                        enabled: treeRow.modelData.locked !== true
                        onClicked: editor.roto.moveElement(String(treeRow.modelData.id), -1)
                    }
                    Action {
                        objectName: "rotoDown_" + editor.nodeId + "_" + treeRow.modelData.id
                        implicitWidth: 19
                        text: "\u25BE"
                        Accessible.name: "Move " + String(treeRow.modelData.name) + " later"
                        enabled: treeRow.modelData.locked !== true
                        onClicked: editor.roto.moveElement(String(treeRow.modelData.id), 1)
                    }
                }
            }
        }
        Text {
            anchors.centerIn: parent
            visible: editor.elements.length === 0
            text: editor.roto && editor.roto.available === true ? "no shapes" : "no Roto here"
            color: editor.mutedColor
            font.pixelSize: editor.smallFontSize
        }
    }

    // --- selected element --------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: editor.selectedElement.length > 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            TextInput {
                id: nameField
                objectName: "rotoElementName_" + editor.nodeId
                Layout.fillWidth: true
                text: {
                    var found = editor.elementRecord(editor.selectedElement);
                    return found ? String(found.name) : "";
                }
                color: editor.textColor
                font.pixelSize: editor.fontSizeValue
                selectByMouse: true
                enabled: !editor.selectedIsLocked
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
            Loader {
                id: parentLoader
                Layout.preferredWidth: 92
                sourceComponent: editor.selectedElement.length > 0 ? parentComboComponent : null
            }
        }

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
        ScalarRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "opacity"
            fieldLabel: "opacity"
        }
        ScalarRow {
            elementId: editor.selectedElement
            pointId: ""
            fieldKey: "feather"
            fieldLabel: "feather"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            ScalarRow {
                elementId: editor.selectedElement
                pointId: ""
                fieldKey: "featherFalloff"
                fieldLabel: "falloff"
                Layout.fillWidth: true
            }
            Loader {
                Layout.preferredWidth: 74
                sourceComponent: editor.roto !== null ? profileComboComponent : null
            }
        }
        FlagRow {
            fieldKey: "featherEnabled"
            fieldLabel: "feather on"
        }
        FlagRow {
            fieldKey: "inverted"
            fieldLabel: "invert"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                text: "lifetime"
                color: editor.textColor
                font.pixelSize: editor.fontSizeValue
                Layout.preferredWidth: 84
            }
            NumericField {
                id: firstFrameField
                objectName: "rotoFirstFrame_" + editor.nodeId
                theme: editor.theme
                value: editor.lifetimeBound(true)
                valueAvailable: {
                    var record = editor.elementRecord(editor.selectedElement);
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
                    var record = editor.elementRecord(editor.selectedElement);
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
            }
        }

        // --- points of the selected path -----------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: editor.selectedIsGroup !== true
            Text {
                text: "points"
                color: editor.textColor
                font.pixelSize: editor.fontSizeValue
                Layout.fillWidth: true
            }
            Action {
                objectName: "rotoAddPoint_" + editor.nodeId
                text: "+"
                implicitWidth: 21
                Accessible.name: "Add a point"
                enabled: !editor.selectedIsLocked
                onClicked: editor.addPoint()
                ToolTip.visible: hovered
                ToolTip.text: "Insert a point after the selected point"
            }
            Action {
                objectName: "rotoRemovePoint_" + editor.nodeId
                text: "\u2212"
                implicitWidth: 21
                Accessible.name: "Remove the selected points"
                enabled: !editor.selectedIsLocked && editor.roto !== null && editor.roto.selectedPoints.length > 0
                onClicked: editor.removePoints()
            }
            Action {
                objectName: "rotoSmooth_" + editor.nodeId
                text: "\u25DC"
                implicitWidth: 21
                Accessible.name: "Smooth the selected points"
                enabled: !editor.selectedIsLocked
                onClicked: editor.setSmooth(true)
                ToolTip.visible: hovered
                ToolTip.text: "Mirror the handles of the selected points (B-spline: tension 0)"
            }
            Action {
                objectName: "rotoCusp_" + editor.nodeId
                text: "\u2227"
                implicitWidth: 21
                Accessible.name: "Make the selected points cusps"
                enabled: !editor.selectedIsLocked
                onClicked: editor.setSmooth(false)
                ToolTip.visible: hovered
                ToolTip.text: "Break the handles of the selected points (B-spline: tension 1)"
            }
        }
        Repeater {
            model: editor.selectedIsGroup === true ? [] : editor.points
            delegate: ColumnLayout {
                id: pointRow
                required property var modelData
                readonly property bool selected: editor.roto !== null && editor.roto.pointSelected(String(pointRow.modelData.id))
                Layout.fillWidth: true
                spacing: 1
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 2
                    color: pointRow.selected ? editor.accentColor : "transparent"
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        objectName: "rotoPointIndex_" + editor.nodeId + "_" + pointRow.modelData.id
                        text: String(Number(pointRow.modelData.index) + 1)
                        color: pointRow.selected ? editor.accentColor : editor.mutedColor
                        font.pixelSize: editor.fontSizeValue
                        Layout.preferredWidth: Math.max(18, implicitWidth)
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (editor.roto)
                                    editor.roto.selectPoint(String(pointRow.modelData.id), false);
                            }
                        }
                    }
                    NumericField {
                        objectName: "rotoPointX_" + editor.nodeId + "_" + pointRow.modelData.id
                        theme: editor.theme
                        value: editor.vectorValue(editor.selectedElement, pointRow.modelData.id, "position", 0)
                        hasMinimum: false
                        hasMaximum: false
                        hasSoftMinimum: false
                        hasSoftMaximum: false
                        step: 1
                        label: "Point x"
                        dragThreshold: editor.dragThreshold
                        fieldWidth: 54
                        Layout.fillWidth: true
                        Layout.minimumWidth: 34
                        enabled: !editor.selectedIsLocked
                        gestureLive: editor.gestureLive()
                        onTextCommitted: function(text) {
                            editor.commitVector(editor.selectedElement, pointRow.modelData.id, "position", 0, Number(text));
                        }
                        onStepped: function(value) {
                            editor.commitVector(editor.selectedElement, pointRow.modelData.id, "position", 0, value);
                        }
                        onScrubStarted: editor.beginScrub(editor.selectedElement, pointRow.modelData.id, "position", 0)
                        onScrubbed: function(value) {
                            editor.updateScrub(value);
                        }
                        onScrubFinished: editor.finishScrub()
                        onScrubCancelled: editor.cancelScrub()
                        onKeyRequested: editor.keyAtFrame(editor.selectedElement, pointRow.modelData.id, "position")
                    }
                    NumericField {
                        objectName: "rotoPointY_" + editor.nodeId + "_" + pointRow.modelData.id
                        theme: editor.theme
                        value: editor.vectorValue(editor.selectedElement, pointRow.modelData.id, "position", 1)
                        hasMinimum: false
                        hasMaximum: false
                        hasSoftMinimum: false
                        hasSoftMaximum: false
                        step: 1
                        label: "Point y"
                        dragThreshold: editor.dragThreshold
                        fieldWidth: 54
                        Layout.fillWidth: true
                        Layout.minimumWidth: 34
                        enabled: !editor.selectedIsLocked
                        gestureLive: editor.gestureLive()
                        onTextCommitted: function(text) {
                            editor.commitVector(editor.selectedElement, pointRow.modelData.id, "position", 1, Number(text));
                        }
                        onStepped: function(value) {
                            editor.commitVector(editor.selectedElement, pointRow.modelData.id, "position", 1, value);
                        }
                        onScrubStarted: editor.beginScrub(editor.selectedElement, pointRow.modelData.id, "position", 1)
                        onScrubbed: function(value) {
                            editor.updateScrub(value);
                        }
                        onScrubFinished: editor.finishScrub()
                        onScrubCancelled: editor.cancelScrub()
                        onKeyRequested: editor.keyAtFrame(editor.selectedElement, pointRow.modelData.id, "position")
                    }
                    NumericField {
                        objectName: "rotoPointFeather_" + editor.nodeId + "_" + pointRow.modelData.id
                        theme: editor.theme
                        value: editor.numberValue(editor.selectedElement, pointRow.modelData.id, "feather")
                        hasMinimum: false
                        hasMaximum: false
                        hasSoftMinimum: false
                        hasSoftMaximum: false
                        step: 1
                        label: "Point feather"
                        dragThreshold: editor.dragThreshold
                        fieldWidth: 50
                        Layout.fillWidth: true
                        Layout.minimumWidth: 32
                        enabled: !editor.selectedIsLocked
                        gestureLive: editor.gestureLive()
                        onTextCommitted: function(text) {
                            editor.commitScalar(editor.selectedElement, pointRow.modelData.id, "feather", Number(text));
                        }
                        onStepped: function(value) {
                            editor.commitScalar(editor.selectedElement, pointRow.modelData.id, "feather", value);
                        }
                        onScrubStarted: editor.beginScrub(editor.selectedElement, pointRow.modelData.id, "feather", -1)
                        onScrubbed: function(value) {
                            editor.updateScrub(value);
                        }
                        onScrubFinished: editor.finishScrub()
                        onScrubCancelled: editor.cancelScrub()
                        onKeyRequested: editor.keyAtFrame(editor.selectedElement, pointRow.modelData.id, "feather")
                    }
                    NumericField {
                        objectName: "rotoPointTension_" + editor.nodeId + "_" + pointRow.modelData.id
                        theme: editor.theme
                        value: editor.numberValue(editor.selectedElement, pointRow.modelData.id, "tension")
                        hasMinimum: true
                        minimum: 0
                        hasMaximum: true
                        maximum: 1
                        hasSoftMinimum: true
                        softMinimum: 0
                        hasSoftMaximum: true
                        softMaximum: 1
                        step: 0.01
                        label: "Point tension"
                        dragThreshold: editor.dragThreshold
                        fieldWidth: 50
                        Layout.fillWidth: true
                        Layout.minimumWidth: 32
                        enabled: !editor.selectedIsLocked
                        gestureLive: editor.gestureLive()
                        onTextCommitted: function(text) {
                            editor.commitScalar(editor.selectedElement, pointRow.modelData.id, "tension", Number(text));
                        }
                        onStepped: function(value) {
                            editor.commitScalar(editor.selectedElement, pointRow.modelData.id, "tension", value);
                        }
                        onScrubStarted: editor.beginScrub(editor.selectedElement, pointRow.modelData.id, "tension", -1)
                        onScrubbed: function(value) {
                            editor.updateScrub(value);
                        }
                        onScrubFinished: editor.finishScrub()
                        onScrubCancelled: editor.cancelScrub()
                        onKeyRequested: editor.keyAtFrame(editor.selectedElement, pointRow.modelData.id, "tension")
                    }
                }
            }
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
        id: parentComboComponent
        StudioComboBox {
            id: parentCombo
            theme: editor.theme
            implicitHeight: 21
            model: editor.parentChoices.map(function(entry) { return String(entry.name); })
            currentIndex: {
                var current = editor.elementRecord(editor.selectedElement);
                var parent = current ? String(current.parent) : "";
                for (var index = 0; index < editor.parentChoices.length; ++index)
                    if (String(editor.parentChoices[index].id) === parent)
                        return index;
                return 0;
            }
            onActivated: {
                var chosen = editor.parentChoices[currentIndex];
                if (chosen)
                    editor.reparentSelected(String(chosen.id));
            }
            Accessible.name: "Parent group"
            ToolTip.visible: hovered
            ToolTip.text: "Move the selected element into a group"
        }
    }

    Component {
        id: profileComboComponent
        StudioComboBox {
            id: profileCombo
            theme: editor.theme
            implicitHeight: 21
            model: ["linear", "smooth"]
            currentIndex: editor.choiceValue(editor.selectedElement, "", "featherProfile") === "smooth" ? 1 : 0
            onActivated: {
                if (editor.roto)
                    editor.roto.setElementProperty(editor.selectedElement, "featherProfile", String(model[currentIndex]));
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
            "disabled": editor.theme ? editor.theme.disabled : "#5f6670",
            "errorText": editor.errorColor,
            "smallRadius": editor.smallRadiusValue,
            "fontSize": editor.fontSizeValue
        })
}
