import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Crop box editor (issue #92, stories 43-44), hosted by the generic inspector
// through ParameterEditorRegistry id "nemo.crop.box" with presentation
// "section". The host mounts it on the `x` row and consumes the Crop parameters.
//
// The authored state stays the four typed parameters: every field is read
// through the shared inspector query (frame-evaluated) and written through the
// shared panel gesture, so a typed value, an arrow-key step and a scrub are all
// exactly one validated undo entry with the existing key-at-frame semantics.
// The Width/Height display is a PRESENTATION conversion only: Width reads
// right - x and a width edit authors `right`, Height reads top - y and a height
// edit authors `top`, so no parallel box model and no extra parameter exists.
//
// The box is the reference's bottom-left box: x/y are the box's bottom-left and
// right/top its opposite corner, in the input format's coordinate space. The
// native viewer handles draw the same box; this editor never repeats that
// geometry, it only states the authored numbers.
ColumnLayout {
    id: cropEditor

    // Host-injected contract (ParametersPanel). A bare host may load this
    // editor without a controller; every control then states it cannot act.
    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "x"
    property var parameter
    property var controller
    property var panel

    // The panel's live interaction token, or empty when this editor has no
    // host. A control compares it with the token IT captured, so live means the
    // gesture it began is still the session's.
    readonly property string panelActiveToken: cropEditor.panel ? String(cropEditor.panel.activeToken) : ""

    readonly property color textColor: cropEditor.theme ? cropEditor.theme.text : "#dce0e6"
    readonly property color mutedColor: cropEditor.theme ? cropEditor.theme.muted : "#979ea8"
    readonly property color borderColor: cropEditor.theme ? cropEditor.theme.border : "#30343a"
    readonly property color fieldColor: cropEditor.theme ? cropEditor.theme.field : "#24272c"
    readonly property color panelColor: cropEditor.theme ? cropEditor.theme.panel : "#1e2023"
    readonly property color raisedColor: cropEditor.theme ? cropEditor.theme.raised : "#282c31"
    readonly property color hoverColor: cropEditor.theme ? cropEditor.theme.hover : "#343940"
    readonly property color accentColor: cropEditor.theme ? cropEditor.theme.accent : "#3485f6"
    readonly property color disabledColor: cropEditor.theme ? cropEditor.theme.disabled : "#5f6670"
    readonly property color errorColor: cropEditor.theme ? cropEditor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: cropEditor.theme ? cropEditor.theme.smallRadius : 4
    readonly property int fontSizeValue: cropEditor.theme ? cropEditor.theme.fontSize : 11
    readonly property int smallFontSize: Math.max(9, cropEditor.fontSizeValue - 1)

    // The four consumed keys. The extent pair is stated either as the box's
    // far corner (right/top) or as its size; both name the SAME parameters.
    readonly property var positionEntries: [{
        "key": "x",
        "label": "x"
    }, {
        "key": "y",
        "label": "y"
    }]
    readonly property var extentEntries: cropEditor.sizeMode ? [{
        "key": "right",
        "label": "w",
        "name": "Width"
    }, {
        "key": "top",
        "label": "h",
        "name": "Height"
    }] : [{
        "key": "right",
        "label": "r",
        "name": "Right"
    }, {
        "key": "top",
        "label": "t",
        "name": "Top"
    }]
    // Size display is presentation state on this one control: it never reaches
    // the document, the history or a second model.
    property bool sizeMode: false

    objectName: "cropBoxEditor_" + cropEditor.nodeId
    Layout.fillWidth: true
    spacing: 3

    // --- query coordinates -------------------------------------------------
    // The inspector query addresses the real node that owns the parameters (a
    // definition network for an occurrence exposure); gestures stay on the host
    // row, so an edit can never leave its own scope.
    readonly property string queryNetwork: cropEditor.parameter && cropEditor.parameter.targetNetwork !== undefined ? String(cropEditor.parameter.targetNetwork) : cropEditor.networkId
    readonly property string queryNode: cropEditor.parameter && cropEditor.parameter.targetNode !== undefined ? String(cropEditor.parameter.targetNode) : cropEditor.nodeId
    readonly property int revision: cropEditor.panel ? Number(cropEditor.panel.revision) : 0
    readonly property int frame: cropEditor.controller ? Number(cropEditor.controller.frame) : 0
    readonly property int dragThreshold: cropEditor.controller ? Number(cropEditor.controller.dragDistance) : 4

    // --- model -------------------------------------------------------------
    // Key -> the shared inspector row for that parameter. Re-queried whenever
    // the panel revision or the frame advances, so an animated box states the
    // value at the current frame and a keyed edit is visible immediately.
    property var paramRows: ({})
    onRevisionChanged: cropEditor.refresh()
    onFrameChanged: cropEditor.refresh()
    onNodeIdChanged: cropEditor.refresh()
    onQueryNetworkChanged: cropEditor.refresh()
    onQueryNodeChanged: cropEditor.refresh()
    onControllerChanged: cropEditor.refresh()
    onPanelChanged: cropEditor.refresh()
    Component.onCompleted: cropEditor.refresh()


    function refresh() {
        var next = ({});
        if (cropEditor.controller && cropEditor.queryNetwork.length > 0 && cropEditor.queryNode.length > 0) {
            var inspector = cropEditor.controller.parameterInspector(cropEditor.queryNetwork, cropEditor.queryNode);
            if (inspector && inspector.available === true) {
                var sections = inspector.sections || [];
                for (var i = 0; i < sections.length; ++i) {
                    var parameters = sections[i].parameters || [];
                    for (var j = 0; j < parameters.length; ++j) {
                        var entry = parameters[j];
                        if (entry && entry.key !== undefined)
                            next[String(entry.key)] = entry;
                    }
                }
            }
        }
        cropEditor.paramRows = next;
    }

    function paramRow(key) {
        var entry = cropEditor.paramRows[key];
        return entry !== undefined ? entry : null;
    }

    function numberValue(key) {
        var entry = cropEditor.paramRow(key);
        if (!entry || entry.value === undefined || entry.value === null || entry.value.length !== undefined)
            return 0;
        return Number(entry.value);
    }

    // The value the field STATES: the authored coordinate, or the derived size
    // while the size display is active.
    function displayValue(key) {
        if (!cropEditor.sizeMode)
            return cropEditor.numberValue(key);
        if (key === "right")
            return cropEditor.numberValue("right") - cropEditor.numberValue("x");
        if (key === "top")
            return cropEditor.numberValue("top") - cropEditor.numberValue("y");
        return cropEditor.numberValue(key);
    }

    // The value the DOCUMENT authors for a stated number. In size mode the
    // opposite corner moves, so the authored box keeps its bottom-left.
    function authoredValue(key, stated) {
        if (!cropEditor.sizeMode)
            return stated;
        if (key === "right")
            return cropEditor.numberValue("x") + stated;
        if (key === "top")
            return cropEditor.numberValue("y") + stated;
        return stated;
    }

    function rowFor(key) {
        var entry = cropEditor.paramRow(key);
        return {
            "networkId": cropEditor.networkId,
            "nodeId": cropEditor.nodeId,
            "parameterKey": key,
            "parameter": entry,
            "label": cropEditor.labelFor(key)
        };
    }

    function labelFor(key) {
        if (key === "x")
            return "X";
        if (key === "y")
            return "Y";
        if (key === "right")
            return cropEditor.sizeMode ? "Width" : "Right";
        if (key === "top")
            return cropEditor.sizeMode ? "Height" : "Top";
        return key;
    }

    function ownsKey(key) {
        return paramRows[key] !== undefined;
    }

    // --- shared key and exposure affordances --------------------------------
    // A consumed parameter has no generic row left, so the editor states those
    // same cells itself through the SAME shared owners: the label cell carries
    // the exposure drag and the Alt-click keying gesture, and the key cell owns
    // Set/Update/Remove Key and Show in Animation. No key state, command or
    // exposure rule lives here.
    readonly property var cellTheme: ({
            "text": cropEditor.textColor,
            "muted": cropEditor.mutedColor,
            "accent": cropEditor.accentColor,
            "border": cropEditor.borderColor,
            "hover": cropEditor.hoverColor,
            "field": cropEditor.fieldColor,
            "panel": cropEditor.panelColor,
            "raised": cropEditor.raisedColor,
            "disabled": cropEditor.disabledColor,
            "errorText": cropEditor.errorColor,
            "smallRadius": cropEditor.smallRadiusValue,
            "fontSize": cropEditor.fontSizeValue
        })

    function keyStatusOf(key) {
        cropEditor.revision;
        cropEditor.frame;
        if (!cropEditor.panel || key.length === 0)
            return "none";
        return String(cropEditor.panel.parameterKeyStatusFor(cropEditor.networkId, cropEditor.nodeId, key));
    }

    function keyAtFrame(key) {
        if (!cropEditor.panel || key.length === 0)
            return false;
        return cropEditor.panel.keyParameterAtFrame(cropEditor.networkId, cropEditor.nodeId, key);
    }

    function removeKeyAtFrame(key) {
        if (!cropEditor.panel || key.length === 0)
            return false;
        return cropEditor.panel.removeParameterKeyAtFrame(cropEditor.networkId, cropEditor.nodeId, key);
    }

    function revealInAnimation(key) {
        if (!cropEditor.panel || !cropEditor.panel.revealInAnimation)
            return;
        cropEditor.panel.revealInAnimation(cropEditor.networkId, cropEditor.nodeId, key);
    }

    function revealAvailable() {
        return cropEditor.panel && cropEditor.panel.groupHasAnimationPanel ? cropEditor.panel.groupHasAnimationPanel() : false;
    }

    // --- the shared gestures -----------------------------------------------
    function commitValue(key, stated) {
        if (!cropEditor.panel)
            return false;
        return cropEditor.panel.gestureSingle(cropEditor.rowFor(key), cropEditor.authoredValue(key, stated));
    }

    function commitStated(key, value) {
        return cropEditor.commitValue(key, value);
    }

    function commitText(key, text) {
        if (!cropEditor.panel)
            return false;
        // In the size display the typed number is a width/height, so it is
        // resolved to the authored corner here; the exact authored text of the
        // corner itself is only meaningful in the corner display.
        if (cropEditor.sizeMode) {
            var typed = Number(String(text).trim());
            if (!isFinite(typed)) {
                cropEditor.rejectText(key, text);
                return false;
            }
            return cropEditor.commitValue(key, typed);
        }
        return cropEditor.panel.gestureText(cropEditor.rowFor(key), text);
    }

    function rejectText(key, text) {
        if (!cropEditor.panel)
            return;
        cropEditor.panel.rejectText(cropEditor.rowFor(key), text);
    }

    // The field keeps its own token from the begin to the commit or cancel, so
    // a scrub whose gesture was retired cannot preview into or publish over the
    // interaction that replaced it.
    function beginScrub(key) {
        if (!cropEditor.panel)
            return "";
        return cropEditor.panel.beginScrub(cropEditor.rowFor(key));
    }

    function updateScrub(key, token, stated) {
        if (!cropEditor.panel)
            return false;
        return cropEditor.panel.updateScrub(token, cropEditor.authoredValue(key, stated));
    }

    function finishScrub(token) {
        return cropEditor.panel ? cropEditor.panel.finishScrub(token) : false;
    }

    function cancelScrub(token) {
        return cropEditor.panel ? cropEditor.panel.cancelScrub(token) : false;
    }

    // The most recent rejected edit attributed to one of the consumed keys. The
    // message comes from the controller/catalog; this editor never re-validates.
    readonly property string gestureProblem: cropEditor.panel && cropEditor.ownsKey(String(cropEditor.panel.gestureErrorKey)) ? String(cropEditor.panel.gestureError) : ""

    property string selectedPreset: ""
    readonly property var formats: {
        revision;
        if (!controller) return [];
        return [controller.networkFormat(queryNetwork)].concat(controller.namedFormats());
    }
    function resetBox() {
        var format = selectedPreset.length ? formats.find(function(entry) { return entry.name === cropEditor.selectedPreset; }) : formats[0];
        if (!format || !panel) return;
        var values = {x: 0, y: 0, right: Number(format.width), top: Number(format.height)};
        var token = panel.beginEditForMany(networkId, nodeId, Object.keys(values));
        if (token.length === 0)
            return;
        if (!panel.updateEditMany(token, values)) {
            panel.cancelEdit(token);
            return;
        }
        panel.commitEdit(token);
    }

    component Caption: ExposureLabel {
        id: caption
        required property string editKey
        theme: cropEditor.theme
        networkId: cropEditor.networkId
        instanceId: cropEditor.instanceId
        nodeId: cropEditor.nodeId
        parameterKey: editKey
        frame: cropEditor.frame
        keyStatus: cropEditor.keyStatusOf(editKey)
        implicitWidth: metrics.advanceWidth
        implicitHeight: 23
        onKeyRequested: cropEditor.keyAtFrame(editKey)
        TextMetrics { id: metrics; text: caption.labelText; font.pixelSize: cropEditor.fontSizeValue }
        KeyIndicator {
            id: keyActions
            visible: false
            theme: cropEditor.cellTheme
            networkId: cropEditor.networkId
            nodeId: cropEditor.nodeId
            parameterKey: caption.editKey
            parameterLabel: cropEditor.labelFor(caption.editKey)
            keyStatus: caption.keyStatus
            frame: cropEditor.frame
            revealAvailable: cropEditor.revealAvailable()
            onKeyRequested: cropEditor.keyAtFrame(caption.editKey)
            onRemoveKeyRequested: cropEditor.removeKeyAtFrame(caption.editKey)
            onRevealRequested: cropEditor.revealInAnimation(caption.editKey)
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: keyActions.openMenu(caption)
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            visible: caption.keyStatus !== "none"
            color: caption.keyStatus === "key" ? cropEditor.accentColor : cropEditor.mutedColor
        }
    }
    component Action: Button {
        id: action
        implicitHeight: 23
        padding: 4
        contentItem: Text {
            text: action.text
            color: cropEditor.textColor
            font.pixelSize: cropEditor.fontSizeValue
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: action.down ? cropEditor.raisedColor : action.hovered ? cropEditor.hoverColor : cropEditor.fieldColor
            border.color: cropEditor.borderColor
            radius: cropEditor.smallRadiusValue
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 5
        Text { text: "preset"; color: cropEditor.textColor; font.pixelSize: cropEditor.fontSizeValue; Layout.preferredWidth: 52 }
        StudioComboBox {
            id: preset
            objectName: "cropPreset_" + cropEditor.nodeId
            theme: cropEditor.theme
            implicitHeight: 23
            Layout.preferredWidth: 130
            model: cropEditor.formats.map(function(format, index) { return index === 0 ? "format" : String(format.name); })
            currentIndex: Math.max(0, cropEditor.formats.findIndex(function(format, index) { return index === 0 ? !cropEditor.selectedPreset.length : format.name === cropEditor.selectedPreset; }))
            function restoreSelection() {
                currentIndex = Qt.binding(function() { return Math.max(0, cropEditor.formats.findIndex(function(format, index) { return index === 0 ? !cropEditor.selectedPreset.length : format.name === cropEditor.selectedPreset; })); });
            }
            onModelChanged: Qt.callLater(restoreSelection)
            onActivated: {
                cropEditor.selectedPreset = currentIndex > 0 ? String(cropEditor.formats[currentIndex].name) : "";
                cropEditor.resetBox();
                restoreSelection();
            }
            ToolTip.visible: hovered
            ToolTip.text: "Set the box to the composition format or a saved format"
        }
        Action {
            objectName: "cropReset_" + cropEditor.nodeId
            text: "Reset"
            onClicked: cropEditor.resetBox()
            ToolTip.visible: hovered
            ToolTip.text: "Restore the box to the selected format; leave softness and flags unchanged"
        }
        Item { Layout.fillWidth: true }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text { text: "box"; color: cropEditor.textColor; font.pixelSize: cropEditor.fontSizeValue; Layout.preferredWidth: 52 }
        Repeater {
            model: cropEditor.positionEntries.concat(cropEditor.extentEntries)
            delegate: RowLayout {
                id: fieldRow
                required property var modelData
                readonly property string fieldKey: String(modelData.key)
                readonly property var fieldParameter: cropEditor.paramRow(fieldKey)
                // This field's own gesture token: the preview, the commit and
                // the cancel only ever name the gesture it began.
                property string gestureToken: ""
                Layout.fillWidth: true
                spacing: 3
                Caption { editKey: fieldRow.fieldKey; labelText: fieldRow.modelData.label }
                NumericField {
                    objectName: "cropBox_" + cropEditor.nodeId + "_" + fieldRow.fieldKey
                    interactionOwner: cropEditor.panel
                    theme: cropEditor.theme
                    value: cropEditor.displayValue(fieldRow.fieldKey)
                    hasMinimum: false
                    hasMaximum: false
                    hasSoftMinimum: false
                    hasSoftMaximum: false
                    step: fieldRow.fieldParameter ? Number(fieldRow.fieldParameter.step || 1) : 1
                    label: cropEditor.labelFor(fieldRow.fieldKey)
                    dragThreshold: cropEditor.dragThreshold
                    fieldWidth: 62
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    enabled: !!cropEditor.controller && !!fieldRow.fieldParameter
                    gestureLive: fieldRow.gestureToken.length > 0
                                 && cropEditor.panelActiveToken === fieldRow.gestureToken
                    onTextCommitted: function(text) { cropEditor.commitText(fieldRow.fieldKey, text); }
                    onTextRejected: function(text) { cropEditor.rejectText(fieldRow.fieldKey, text); }
                    onStepped: function(value) { cropEditor.commitValue(fieldRow.fieldKey, value); }
                    onScrubStarted: fieldRow.gestureToken = cropEditor.beginScrub(fieldRow.fieldKey)
                    onScrubbed: function(value) {
                        cropEditor.updateScrub(fieldRow.fieldKey, fieldRow.gestureToken, value);
                    }
                    onScrubFinished: {
                        var token = fieldRow.gestureToken;
                        fieldRow.gestureToken = "";
                        cropEditor.finishScrub(token);
                    }
                    onScrubCancelled: {
                        var token = fieldRow.gestureToken;
                        fieldRow.gestureToken = "";
                        cropEditor.cancelScrub(token);
                    }
                    onKeyRequested: cropEditor.keyAtFrame(fieldRow.fieldKey)
                }
            }
        }
        Action {
            objectName: "cropBoxSize_" + cropEditor.nodeId
            text: cropEditor.sizeMode ? "rt" : "wh"
            Accessible.name: cropEditor.sizeMode ? "Show right and top" : "Show width and height"
            onClicked: cropEditor.sizeMode = !cropEditor.sizeMode
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 5
        Caption { editKey: "softness"; labelText: "softness"; Layout.preferredWidth: 52 }
        Loader {
            Layout.fillWidth: true
            sourceComponent: cropEditor.panel ? cropEditor.panel.numericEditorComponent : null
            onLoaded: {
                item.theme = cropEditor.theme;
                item.panel = cropEditor.panel;
                item.row = softnessRow;
                item.compact = true;
                item.fieldFirst = true;
            }
        }
        QtObject {
            id: softnessRow
            readonly property string nodeId: cropEditor.nodeId
            readonly property string parameterKey: "softness"
            readonly property string rowLabel: "Softness"
            readonly property real numberValue: cropEditor.numberValue("softness")
            readonly property bool hasMinimum: true
            readonly property bool hasMaximum: false
            readonly property real minimum: 0
            readonly property real maximum: 0
            readonly property bool hasSoftMinimum: true
            readonly property bool hasSoftMaximum: true
            readonly property real softMinimum: 0
            readonly property real softMaximum: 100
            readonly property real numberStep: 1
            readonly property int decimals: -1
            readonly property bool integerParameter: false
            readonly property int dragThreshold: cropEditor.dragThreshold
            readonly property string rowError: cropEditor.panel && cropEditor.panel.gestureErrorKey === "softness" ? cropEditor.gestureProblem : ""
            function rowRef() { return cropEditor.rowFor("softness"); }
            function commitText(text) { return cropEditor.commitText("softness", text); }
            function commitDiscrete(value) { return cropEditor.commitValue("softness", value); }
            function keyAtFrame() { return cropEditor.keyAtFrame("softness"); }
        }
    }
    Flow {
        Layout.fillWidth: true
        Layout.leftMargin: 57
        spacing: 7
        Repeater {
            model: [{key: "reformat", label: "reformat"}, {key: "intersect", label: "intersect"}, {key: "blackOutside", label: "black outside"}]
            delegate: CheckBox {
                id: flag
                required property var modelData
                objectName: "cropFlag_" + cropEditor.nodeId + "_" + modelData.key
                checked: cropEditor.paramRow(modelData.key) ? cropEditor.paramRow(modelData.key).value === true : false
                implicitWidth: flagLabel.implicitWidth + 16
                implicitHeight: 23
                padding: 0
                leftPadding: 16
                Accessible.name: modelData.label
                onToggled: {
                    cropEditor.commitValue(modelData.key, checked);
                    checked = Qt.binding(function() { return cropEditor.paramRow(flag.modelData.key).value === true; });
                }
                contentItem: Caption {
                    id: flagLabel
                    editKey: flag.modelData.key
                    labelText: flag.modelData.label
                    MouseArea {
                        anchors.fill: parent
                        onPressed: function(mouse) { mouse.accepted = mouse.modifiers === Qt.NoModifier; }
                        onClicked: cropEditor.commitValue(flag.modelData.key, !flag.checked)
                    }
                }
                indicator: Rectangle {
                    width: 12; height: 12; y: (flag.height - height) / 2
                    radius: 2
                    color: flag.checked ? cropEditor.accentColor : cropEditor.fieldColor
                    border.color: flag.activeFocus ? cropEditor.accentColor : cropEditor.borderColor
                    Text { anchors.centerIn: parent; text: flag.checked ? "\u00d7" : ""; color: cropEditor.textColor; font.pixelSize: 13 }
                }
                background: Rectangle { color: flag.hovered ? cropEditor.hoverColor : "transparent"; radius: 2 }
            }
        }
    }

    Text {
        objectName: "cropBoxError_" + cropEditor.nodeId
        visible: cropEditor.gestureProblem.length > 0
        Layout.fillWidth: true
        text: cropEditor.gestureProblem
        color: cropEditor.errorColor
        font.pixelSize: cropEditor.smallFontSize
        elide: Text.ElideRight
        wrapMode: Text.WordWrap
        Accessible.name: cropEditor.gestureProblem
    }
}
