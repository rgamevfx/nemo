import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Crop box editor (issue #92, stories 43-44), hosted by the generic inspector
// through ParameterEditorRegistry id "nemo.crop.box" with presentation
// "section". The host mounts it on the `x` row and consumes x/y/right/top, so
// this editor is the ONE control for the node's box coordinates.
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
    property var theme
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "x"
    property var parameter
    property var controller
    property var panel

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
        "label": "X"
    }, {
        "key": "y",
        "label": "Y"
    }]
    readonly property var extentEntries: cropEditor.sizeMode ? [{
        "key": "right",
        "label": "W",
        "name": "Width"
    }, {
        "key": "top",
        "label": "H",
        "name": "Height"
    }] : [{
        "key": "right",
        "label": "Right",
        "name": "Right"
    }, {
        "key": "top",
        "label": "Top",
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
        return key === "x" || key === "y" || key === "right" || key === "top";
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

    function beginScrub(key) {
        if (!cropEditor.panel)
            return;
        cropEditor.panel.beginScrub(cropEditor.rowFor(key));
    }

    function updateScrub(key, stated) {
        if (!cropEditor.panel)
            return false;
        return cropEditor.panel.updateScrub(cropEditor.authoredValue(key, stated));
    }

    function finishScrub() {
        return cropEditor.panel ? cropEditor.panel.finishScrub() : false;
    }

    function cancelScrub() {
        return cropEditor.panel ? cropEditor.panel.cancelScrub() : false;
    }

    // The most recent rejected edit attributed to one of the consumed keys. The
    // message comes from the controller/catalog; this editor never re-validates.
    readonly property string gestureProblem: cropEditor.panel && cropEditor.ownsKey(String(cropEditor.panel.gestureErrorKey)) ? String(cropEditor.panel.gestureError) : ""

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        Text {
            text: "Crop Box"
            color: cropEditor.mutedColor
            font.pixelSize: cropEditor.smallFontSize
            Accessible.name: "Crop box"
        }

        Item {
            Layout.fillWidth: true
        }

        Button {
            id: sizeToggle
            objectName: "cropBoxSize_" + cropEditor.nodeId
            implicitWidth: 96
            implicitHeight: 19
            padding: 0
            text: cropEditor.sizeMode ? "Right / Top" : "Width / Height"
            Accessible.name: cropEditor.sizeMode ? "Show the box's right and top edges" : "Show the box's width and height"
            ToolTip.visible: sizeToggle.hovered
            ToolTip.text: cropEditor.sizeMode ? "Showing width and height. Click for right/top." : "Showing right and top. Click for width/height."
            onClicked: cropEditor.sizeMode = !cropEditor.sizeMode
            contentItem: Text {
                text: sizeToggle.text
                color: sizeToggle.enabled ? cropEditor.textColor : cropEditor.disabledColor
                font.pixelSize: cropEditor.smallFontSize
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: sizeToggle.down ? cropEditor.raisedColor : sizeToggle.hovered ? cropEditor.hoverColor : "transparent"
                border.color: cropEditor.borderColor
                radius: cropEditor.smallRadiusValue
            }
        }
    }

    Repeater {
        model: cropEditor.positionEntries.concat(cropEditor.extentEntries)
        delegate: RowLayout {
            id: fieldRow
            required property var modelData

            readonly property string fieldKey: String(modelData.key)
            readonly property string fieldLabel: String(modelData.name !== undefined ? modelData.name : modelData.label)
            readonly property var fieldParameter: cropEditor.paramRow(fieldRow.fieldKey)
            readonly property string fieldError: cropEditor.ownsKey(String(cropEditor.panel ? cropEditor.panel.gestureErrorKey : "")) && String(cropEditor.panel ? cropEditor.panel.gestureErrorKey : "") === fieldRow.fieldKey ? cropEditor.gestureProblem : ""

            Layout.fillWidth: true
            spacing: 4

            ExposureLabel {
                objectName: "label_" + cropEditor.nodeId + "_" + fieldRow.fieldKey
                Layout.preferredWidth: 72
                Layout.minimumWidth: 44
                Layout.maximumWidth: 72
                Layout.alignment: Qt.AlignVCenter
                theme: cropEditor.theme
                networkId: cropEditor.networkId
                instanceId: cropEditor.instanceId
                nodeId: cropEditor.nodeId
                parameterKey: fieldRow.fieldKey
                labelText: fieldRow.fieldLabel
                keyStatus: cropEditor.keyStatusOf(fieldRow.fieldKey)
                frame: cropEditor.frame
                onKeyRequested: cropEditor.keyAtFrame(fieldRow.fieldKey)
            }

            KeyIndicator {
                objectName: "key_" + cropEditor.nodeId + "_" + fieldRow.fieldKey
                Layout.preferredWidth: 24
                Layout.maximumWidth: 24
                Layout.alignment: Qt.AlignVCenter
                theme: cropEditor.cellTheme
                networkId: cropEditor.networkId
                nodeId: cropEditor.nodeId
                parameterKey: fieldRow.fieldKey
                parameterLabel: fieldRow.fieldLabel
                keyStatus: cropEditor.keyStatusOf(fieldRow.fieldKey)
                scope: fieldRow.fieldParameter && fieldRow.fieldParameter.scope !== undefined ? String(fieldRow.fieldParameter.scope) : ""
                frame: cropEditor.frame
                revealAvailable: cropEditor.revealAvailable()
                onKeyRequested: cropEditor.keyAtFrame(fieldRow.fieldKey)
                onRemoveKeyRequested: cropEditor.removeKeyAtFrame(fieldRow.fieldKey)
                onRevealRequested: cropEditor.revealInAnimation(fieldRow.fieldKey)
            }

            NumericField {
                id: field
                objectName: "cropBox_" + cropEditor.nodeId + "_" + fieldRow.fieldKey
                theme: cropEditor.theme
                value: cropEditor.displayValue(fieldRow.fieldKey)
                text: ""
                hasMinimum: false
                hasMaximum: false
                hasSoftMinimum: false
                hasSoftMaximum: false
                step: fieldRow.fieldParameter && fieldRow.fieldParameter.step !== undefined ? Number(fieldRow.fieldParameter.step) : 0.01
                decimals: fieldRow.fieldParameter && fieldRow.fieldParameter.displayDecimals !== undefined ? Number(fieldRow.fieldParameter.displayDecimals) : -1
                integer: false
                label: fieldRow.fieldLabel
                errorText: fieldRow.fieldError
                dragThreshold: cropEditor.dragThreshold
                fieldWidth: 62
                Layout.fillWidth: true
                Layout.minimumWidth: 56
                Layout.alignment: Qt.AlignVCenter
                enabled: cropEditor.controller !== null && cropEditor.controller !== undefined && fieldRow.fieldParameter !== null
                gestureLive: cropEditor.panel ? cropEditor.panel.activeToken.length > 0 : false
                onTextCommitted: function (text) {
                    cropEditor.commitText(fieldRow.fieldKey, text);
                }
                onTextRejected: cropEditor.rejectText(fieldRow.fieldKey, text)
                onStepped: function (value) {
                    cropEditor.commitValue(fieldRow.fieldKey, value);
                }
                onScrubStarted: cropEditor.beginScrub(fieldRow.fieldKey)
                onScrubbed: function (value) {
                    cropEditor.updateScrub(fieldRow.fieldKey, value);
                }
                onScrubFinished: cropEditor.finishScrub()
                onScrubCancelled: cropEditor.cancelScrub()
                onKeyRequested: cropEditor.keyAtFrame(fieldRow.fieldKey)
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
