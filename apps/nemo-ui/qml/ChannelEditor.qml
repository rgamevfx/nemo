import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One schema-selected RGB editor. Expansion is presentation only; gestures
// always carry the complete tuple, with alpha independent of linked RGB.
Item {
    id: channelEditor

    property var theme: null
    property var panel: null
    property var controller: null
    property string networkId: ""
    property string nodeId: ""
    property string parameterKey: ""
    property var parameter: null
    property bool expanded: false
    property string errorText: ""
    property var gestureBase: []
    property var previewComponents: []

    readonly property var components: parameter && parameter.value ? parameter.value : [0, 0, 0, 1]
    readonly property bool equalChannels: components[0] === components[1] && components[1] === components[2]
    readonly property bool multiplicative: parameter && parameter.channels && parameter.channels.linked === "multiplicative"
    readonly property bool mixedLinked: !equalChannels
    readonly property real neutralLinked: multiplicative ? 1 : 0
    readonly property real linkedValue: mixedLinked ? neutralLinked : Number(components[0])
    readonly property real trackFrom: parameter && parameter.softMinimum !== undefined ? Number(parameter.softMinimum) : 0
    readonly property real trackTo: parameter && parameter.softMaximum !== undefined ? Number(parameter.softMaximum) : 1
    readonly property real step: parameter && parameter.step !== undefined ? Number(parameter.step) : 0.01
    readonly property int decimals: parameter && parameter.displayDecimals !== undefined ? Number(parameter.displayDecimals) : -1
    readonly property string fieldLabel: parameter && parameter.label ? String(parameter.label) : parameterKey
    // This editor's own gesture token. Live means the token it began is still
    // the panel's, so a retired gesture (Escape, a preview-only Undo or a
    // successor interaction) drops the local preview and can never publish.
    property string gestureToken: ""
    readonly property bool gestureLive: panel !== null && gestureToken.length > 0
                                        && String(panel.activeToken) === gestureToken
    readonly property var picker: typeof viewportPicker !== "undefined" ? viewportPicker : null
    readonly property var channelColors: ["#ed5559", "#56ce65", "#5e85ed", theme ? theme.muted : "#979ea8"]
    readonly property int controlHeight: theme && theme.inspectorControlHeight !== undefined ? theme.inspectorControlHeight : 30
    readonly property int textSize: theme && theme.inspectorFontSize !== undefined ? theme.inspectorFontSize : 13
    onGestureLiveChanged: if (!gestureLive) previewComponents = []

    implicitHeight: editorColumn.implicitHeight
    Layout.fillWidth: true

    function rowObject() {
        return { networkId: networkId, nodeId: nodeId, parameterKey: parameterKey,
                 parameter: parameter, label: fieldLabel };
    }

    function editedTuple(index, value, base) {
        var next = Array.prototype.slice.call(base);
        if (index >= 0) {
            next[index] = value;
        } else if (base[0] === base[1] && base[1] === base[2]) {
            next[0] = next[1] = next[2] = value;
        } else {
            for (var i = 0; i < 3; ++i)
                next[i] = multiplicative ? Number(base[i]) * value : Number(base[i]) + value;
        }
        return next;
    }

    function commitValue(index, value) {
        if (!panel)
            return;
        var committed = panel.gestureSingle(rowObject(), editedTuple(index, value, components));
        errorText = committed ? "" : (controller ? String(controller.error) : "");
    }

    function beginGesture() {
        errorText = "";
        gestureBase = Array.prototype.slice.call(components);
        if (panel)
            gestureToken = panel.beginEditFor(networkId, nodeId, parameterKey);
        return gestureToken;
    }

    function preview(index, value) {
        if (!gestureLive)
            return;
        var tuple = editedTuple(index, value, gestureBase);
        if (panel.updateEdit(gestureToken, tuple))
            previewComponents = tuple;
    }

    function finishGesture() {
        var token = gestureToken;
        gestureToken = "";
        if (panel && token.length > 0) {
            var committed = panel.commitEdit(token);
            errorText = committed ? "" : (controller ? String(controller.error) : "");
        }
        previewComponents = [];
    }

    function cancelGesture() {
        var token = gestureToken;
        gestureToken = "";
        if (panel && token.length > 0)
            panel.cancelEdit(token);
        previewComponents = [];
    }

    component ChannelField: NumericField {
        required property int componentIndex
        theme: channelEditor.theme
        panel: channelEditor.panel
        networkId: channelEditor.networkId
        nodeId: channelEditor.nodeId
        parameterKey: channelEditor.parameterKey
        keyStatus: {
            if (!channelEditor.panel) return "none";
            channelEditor.panel.revision;
            return channelEditor.panel.parameterKeyStatusFor(networkId, nodeId, parameterKey);
        }
        scope: channelEditor.parameter && channelEditor.parameter.scope ? String(channelEditor.parameter.scope) : "RGBA"
        frame: channelEditor.controller ? channelEditor.controller.frame : 0
        revealAvailable: channelEditor.panel ? channelEditor.panel.groupHasAnimationPanel() : false
        modified: channelEditor.parameter && channelEditor.parameter.modified === true
        value: componentIndex < 0 ? channelEditor.linkedValue : Number(channelEditor.components[componentIndex])
        hasMinimum: componentIndex >= 0 && channelEditor.parameter && channelEditor.parameter.minimum !== undefined
        hasMaximum: componentIndex >= 0 && channelEditor.parameter && channelEditor.parameter.maximum !== undefined
        minimum: hasMinimum ? Number(channelEditor.parameter.minimum) : 0
        maximum: hasMaximum ? Number(channelEditor.parameter.maximum) : 0
        step: channelEditor.step
        decimals: channelEditor.decimals
        label: channelEditor.fieldLabel + (componentIndex < 0 ? (channelEditor.mixedLinked ? (channelEditor.multiplicative ? " linked factor" : " linked offset") : " RGB") : " " + ["R", "G", "B", "Alpha"][componentIndex])
        errorText: channelEditor.errorText
        dragThreshold: channelEditor.controller ? Number(channelEditor.controller.dragDistance) : 4
        gestureLive: channelEditor.gestureLive
        controlHeight: channelEditor.controlHeight
        textSize: channelEditor.textSize
        onTextCommitted: function(text) { channelEditor.commitValue(componentIndex, Number(text)); }
        onTextRejected: function(text) {
            channelEditor.errorText = "Parameter '" + channelEditor.fieldLabel + "' rejects '" + text + "'";
        }
        onStepped: function(value) { channelEditor.commitValue(componentIndex, value); }
        onScrubStarted: channelEditor.beginGesture()
        onScrubbed: function(value) { channelEditor.preview(componentIndex, value); }
        onScrubFinished: channelEditor.finishGesture()
        onScrubCancelled: channelEditor.cancelGesture()
        onKeyRequested: if (channelEditor.panel) channelEditor.panel.keyParameterAtFrame(networkId, nodeId, parameterKey)
    }

    ColumnLayout {
        id: editorColumn
        width: parent.width
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                GridLayout {
                    visible: channelEditor.expanded
                    Layout.fillWidth: true
                    columns: width >= 340 ? 4 : width >= 160 ? 2 : 1
                    columnSpacing: 6
                    rowSpacing: 4
                    Repeater {
                        model: ["R", "G", "B", "A"]
                        delegate: RowLayout {
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            spacing: 4
                            Rectangle {
                                width: 7
                                height: 7
                                radius: 4
                                color: channelEditor.channelColors[index]
                            }
                            Text {
                                text: modelData
                                color: channelEditor.theme.text
                                font.pixelSize: channelEditor.textSize
                            }
                            ChannelField {
                                componentIndex: index
                                objectName: "channels_" + modelData + "_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                                Layout.fillWidth: true
                                Layout.minimumWidth: 46
                            }
                        }
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: !channelEditor.expanded && width >= 155 ? 2 : 1
                    columnSpacing: 7
                    rowSpacing: 2
                    RowLayout {
                        visible: !channelEditor.expanded
                        spacing: 3
                        Text {
                            visible: channelEditor.mixedLinked
                            text: channelEditor.multiplicative ? "\u00d7" : "\u0394"
                            color: channelEditor.theme.muted
                            font.pixelSize: channelEditor.textSize
                        }
                        ChannelField {
                            componentIndex: -1
                            objectName: "channels_linked_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                            Layout.preferredWidth: 58
                            Layout.minimumWidth: 46
                        }
                    }
                    ParameterSlider {
                        objectName: "channels_slider_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                        theme: channelEditor.theme
                        Layout.fillWidth: true
                        Layout.minimumWidth: 52
                        from: channelEditor.trackFrom
                        to: channelEditor.trackTo
                        value: channelEditor.linkedValue
                        stepSize: channelEditor.step
                        graduated: true
                        gestureLive: channelEditor.gestureLive
                        markers: {
                            var tuple = channelEditor.previewComponents.length ? channelEditor.previewComponents : channelEditor.components;
                            if (!channelEditor.expanded)
                                return [];
                            return [0, 1, 2].map(function(i) { return { value: Number(tuple[i]), color: channelEditor.channelColors[i] }; });
                        }
                        Accessible.name: channelEditor.fieldLabel + (channelEditor.mixedLinked ? (channelEditor.multiplicative ? " linked factor slider" : " linked offset slider") : " slider")
                        onEditStarted: channelEditor.beginGesture()
                        onValueEdited: function(value) { channelEditor.preview(-1, value); }
                        onEditFinished: channelEditor.finishGesture()
                        onEditCancelled: channelEditor.cancelGesture()
                        onKeyRequested: if (channelEditor.panel)
                            channelEditor.panel.keyParameterAtFrame(channelEditor.networkId, channelEditor.nodeId, channelEditor.parameterKey)
                    }
                }
            }

            Button {
                id: pickButton
                objectName: "channels_pick_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                implicitWidth: channelEditor.controlHeight
                implicitHeight: channelEditor.controlHeight
                padding: 5
                enabled: channelEditor.picker !== null && !channelEditor.gestureLive
                Accessible.name: "Pick " + channelEditor.fieldLabel + " RGB from a viewer"
                onClicked: channelEditor.picker.begin(channelEditor.networkId, channelEditor.nodeId, channelEditor.parameterKey)
                contentItem: Rectangle {
                    color: "white"
                    border.color: "#b7bcc4"
                    radius: 1
                }
                background: Rectangle {
                    color: pickButton.hovered ? channelEditor.theme.hover : channelEditor.theme.field
                    border.color: pickButton.activeFocus ? channelEditor.theme.accent : channelEditor.theme.border
                    radius: channelEditor.theme.smallRadius
                    opacity: pickButton.enabled ? 1 : 0.45
                }
                ToolTip.visible: hovered
                ToolTip.text: "Click an open viewer to pick working-space RGB. Alpha is unchanged. Escape cancels."
            }

            Button {
                id: expander
                objectName: "channels_expand_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                implicitWidth: channelEditor.controlHeight
                implicitHeight: channelEditor.controlHeight
                enabled: !channelEditor.gestureLive
                padding: 0
                text: "3"
                Accessible.name: channelEditor.expanded ? "Collapse RGB channels" : "Expand RGB channels and alpha"
                onClicked: channelEditor.expanded = !channelEditor.expanded
                contentItem: Text {
                    text: expander.text
                    color: channelEditor.expanded ? channelEditor.theme.accent : channelEditor.theme.muted
                    font.pixelSize: channelEditor.textSize
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: expander.hovered ? channelEditor.theme.hover : channelEditor.theme.field
                    border.color: channelEditor.expanded || expander.activeFocus ? channelEditor.theme.accent : channelEditor.theme.border
                    radius: channelEditor.theme.smallRadius
                }
            }
        }
    }
}
