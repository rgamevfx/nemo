import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Registered linked-RGB editor for Grade's multichannel parameters
// (issue #77, stories 24-29, 32). Selected by the catalog `editor` metadata
// ("nemo.channels.rgb") and driven entirely by the shared parameter gesture
// API: one typed edit is one validated undo entry. Expanding, collapsing or
// linking is presentation state and never rewrites the stored channels.
Item {
    id: channelEditor

    property var theme: null
    property var panel: null
    property var controller: null
    property string networkId: ""
    property string nodeId: ""
    property string parameterKey: ""
    property var parameter: null
    // Presentation-only: never persisted into the Document.
    property bool expanded: false
    property string errorText: ""

    readonly property var components: {
        var source = parameter && parameter.value && parameter.value.length !== undefined ? parameter.value : null;
        var result = [0, 0, 0, 1];
        for (var i = 0; i < 4; ++i) {
            if (source && i < source.length)
                result[i] = Number(source[i]);
            else
                result[i] = i === 3 ? 1 : 0;
        }
        return result;
    }
    readonly property bool equalChannels: components[0] === components[1] && components[1] === components[2]
    readonly property string linkOperation: parameter && parameter.channels && parameter.channels.linked !== undefined ? String(parameter.channels.linked) : "none"
    readonly property bool multiplicative: linkOperation === "multiplicative"
    readonly property bool mixedLinked: !equalChannels && (linkOperation === "additive" || linkOperation === "multiplicative")
    // A mixed linked edit is expressed relative to the captured tuple: the
    // compact field starts from the neutral element, never from an arbitrary
    // representative channel.
    readonly property real neutralLinked: multiplicative ? 1 : 0
    readonly property string mixedPrefix: multiplicative ? "\u00d7" : "\u0394"
    readonly property bool hasMinimum: parameter && parameter.minimum !== undefined
    readonly property bool hasMaximum: parameter && parameter.maximum !== undefined
    readonly property real minimum: hasMinimum ? Number(parameter.minimum) : 0
    readonly property real maximum: hasMaximum ? Number(parameter.maximum) : 0
    readonly property bool hasSoftMinimum: parameter && parameter.softMinimum !== undefined
    readonly property bool hasSoftMaximum: parameter && parameter.softMaximum !== undefined
    readonly property real softMinimum: hasSoftMinimum ? Number(parameter.softMinimum) : 0
    readonly property real softMaximum: hasSoftMaximum ? Number(parameter.softMaximum) : 0
    readonly property real step: parameter && parameter.step !== undefined ? Number(parameter.step) : 0
    readonly property int decimals: parameter && parameter.displayDecimals !== undefined ? Number(parameter.displayDecimals) : -1
    readonly property int dragThreshold: controller ? Number(controller.dragDistance) : 4
    readonly property string fieldLabel: parameter && parameter.label !== undefined ? String(parameter.label) : parameterKey

    implicitHeight: editorColumn.implicitHeight
    Layout.fillWidth: true

    function rowObject() {
        return {
            "networkId": networkId,
            "nodeId": nodeId,
            "parameterKey": parameterKey,
            "parameter": parameter,
            "label": fieldLabel
        };
    }

    function commitValue(value) {
        if (!panel)
            return false;
        var committed = panel.gestureSingle(rowObject(), value);
        errorText = committed ? "" : (controller ? String(controller.error) : "");
        return committed;
    }

    function linkedTyped(value) {
        if (equalChannels)
            return [value, value, value, components[3]];
        if (multiplicative)
            return [components[0] * value, components[1] * value, components[2] * value, components[3]];
        return [components[0] + value, components[1] + value, components[2] + value, components[3]];
    }

    function componentTyped(index, value) {
        var next = components.slice();
        next[index] = value;
        return next;
    }

    function refreshError() {
        errorText = controller && String(controller.error).length > 0 ? String(controller.error) : "";
    }

    function requestKey() {
        if (panel)
            panel.keyParameterAtFrame(networkId, nodeId, parameterKey);
    }

    ColumnLayout {
        id: editorColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 2

        // Compact: linked RGB (common value, or explicit delta/factor when the
        // stored channels differ) + expander + separately labelled Alpha.
        RowLayout {
            Layout.fillWidth: true
            spacing: 3

            Text {
                visible: !channelEditor.expanded && channelEditor.mixedLinked
                text: channelEditor.mixedPrefix
                color: theme.muted
                font.pixelSize: theme.fontSize
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: channelEditor.multiplicative ? "Linked RGB factor" : "Linked RGB offset"
            }

            NumericField {
                id: linkedField
                visible: !channelEditor.expanded
                objectName: "channels_linked_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                theme: channelEditor.theme
                value: channelEditor.mixedLinked ? channelEditor.neutralLinked : channelEditor.components[0]
                hasMinimum: false
                hasMaximum: false
                hasSoftMinimum: channelEditor.mixedLinked ? false : channelEditor.hasSoftMinimum
                hasSoftMaximum: channelEditor.mixedLinked ? false : channelEditor.hasSoftMaximum
                softMinimum: channelEditor.softMinimum
                softMaximum: channelEditor.softMaximum
                step: channelEditor.step
                decimals: channelEditor.decimals
                label: channelEditor.mixedLinked ? (channelEditor.multiplicative ? channelEditor.fieldLabel + " linked factor" : channelEditor.fieldLabel + " linked offset") : channelEditor.fieldLabel + " RGB"
                errorText: channelEditor.errorText
                dragThreshold: channelEditor.dragThreshold
                fieldWidth: 56
                Layout.fillWidth: true
                Layout.minimumWidth: 56
                Layout.alignment: Qt.AlignVCenter
                onTextCommitted: function (text) {
                    var value = Number(text);
                    if (!Number.isFinite(value)) {
                        channelEditor.errorText = "Parameter '" + channelEditor.fieldLabel + "' rejects '" + text + "'";
                        return;
                    }
                    channelEditor.commitValue(channelEditor.linkedTyped(value));
                }
                onStepped: function (value) {
                    channelEditor.commitValue(channelEditor.linkedTyped(value));
                }
                onScrubStarted: {
                    channelEditor.errorText = "";
                    if (channelEditor.panel)
                        channelEditor.panel.beginEditFor(channelEditor.networkId, channelEditor.nodeId, channelEditor.parameterKey);
                }
                onScrubbed: function (value) {
                    if (channelEditor.panel)
                        channelEditor.panel.updateEdit(channelEditor.linkedTyped(value));
                }
                onScrubFinished: {
                    if (channelEditor.panel)
                        channelEditor.panel.commitEdit();
                    channelEditor.refreshError();
                }
                onScrubCancelled: {
                    if (channelEditor.panel)
                        channelEditor.panel.cancelEdit();
                }
                onKeyRequested: channelEditor.requestKey()
            }

            // Expanded: labelled R, G, B with the same Alpha field, so a channel
            // correction is understandable and individually scrub-able.
            RowLayout {
                visible: channelEditor.expanded
                Layout.fillWidth: true
                spacing: 4

                Repeater {
                    model: ["R", "G", "B", "A"]
                    delegate: RowLayout {
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: modelData
                            color: theme.muted
                            font.pixelSize: theme.fontSize
                            Layout.alignment: Qt.AlignVCenter
                        }
                        NumericField {
                            objectName: "channels_" + modelData + "_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                            theme: channelEditor.theme
                            value: channelEditor.components[index]
                            hasMinimum: channelEditor.hasMinimum
                            hasMaximum: channelEditor.hasMaximum
                            minimum: channelEditor.minimum
                            maximum: channelEditor.maximum
                            hasSoftMinimum: channelEditor.hasSoftMinimum
                            hasSoftMaximum: channelEditor.hasSoftMaximum
                            softMinimum: channelEditor.softMinimum
                            softMaximum: channelEditor.softMaximum
                            step: channelEditor.step
                            decimals: channelEditor.decimals
                            label: channelEditor.fieldLabel + " " + modelData
                            errorText: channelEditor.errorText
                            dragThreshold: channelEditor.dragThreshold
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            onTextCommitted: function (text) {
                                var value = Number(text);
                                if (!Number.isFinite(value)) {
                                    channelEditor.errorText = "Parameter '" + channelEditor.fieldLabel + "' rejects '" + text + "'";
                                    return;
                                }
                                channelEditor.commitValue(channelEditor.componentTyped(index, value));
                            }
                            onStepped: function (value) {
                                channelEditor.commitValue(channelEditor.componentTyped(index, value));
                            }
                            onScrubStarted: {
                                channelEditor.errorText = "";
                                if (channelEditor.panel)
                                    channelEditor.panel.beginEditFor(channelEditor.networkId, channelEditor.nodeId, channelEditor.parameterKey);
                            }
                            onScrubbed: function (value) {
                                if (channelEditor.panel)
                                    channelEditor.panel.updateEdit(channelEditor.componentTyped(index, value));
                            }
                            onScrubFinished: {
                                if (channelEditor.panel)
                                    channelEditor.panel.commitEdit();
                                channelEditor.refreshError();
                            }
                            onScrubCancelled: {
                                if (channelEditor.panel)
                                    channelEditor.panel.cancelEdit();
                            }
                            onKeyRequested: channelEditor.requestKey()
                        }
                    }
                }
            }
            Button {
                id: expander
                objectName: "channels_expand_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                flat: true
                implicitWidth: 18
                implicitHeight: 23
                padding: 0
                text: channelEditor.expanded ? "\u25be" : "\u25b8"
                Accessible.name: channelEditor.expanded ? "Collapse RGB channels" : "Expand RGB channels"
                onClicked: channelEditor.expanded = !channelEditor.expanded
                contentItem: Text {
                    text: expander.text
                    color: theme.muted
                    font.pixelSize: theme.fontSize
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: expander.hovered ? theme.hover : "transparent"
                    radius: theme.smallRadius
                }
            }

            Rectangle {
                visible: !channelEditor.expanded
                Layout.preferredWidth: 1
                Layout.preferredHeight: 16
                Layout.alignment: Qt.AlignVCenter
                color: theme.border
            }

            Text {
                visible: !channelEditor.expanded
                text: "A"
                color: theme.muted
                font.pixelSize: theme.fontSize
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: channelEditor.fieldLabel + " alpha"
            }

            NumericField {
                id: alphaField
                visible: !channelEditor.expanded
                objectName: "channels_alpha_" + channelEditor.nodeId + "_" + channelEditor.parameterKey
                theme: channelEditor.theme
                value: channelEditor.components[3]
                hasMinimum: channelEditor.hasMinimum
                hasMaximum: channelEditor.hasMaximum
                minimum: channelEditor.minimum
                maximum: channelEditor.maximum
                hasSoftMinimum: channelEditor.hasSoftMinimum
                hasSoftMaximum: channelEditor.hasSoftMaximum
                softMinimum: channelEditor.softMinimum
                softMaximum: channelEditor.softMaximum
                step: channelEditor.step
                decimals: channelEditor.decimals
                label: channelEditor.fieldLabel + " alpha"
                errorText: channelEditor.errorText
                dragThreshold: channelEditor.dragThreshold
                fieldWidth: 56
                Layout.preferredWidth: 56
                Layout.maximumWidth: 56
                Layout.alignment: Qt.AlignVCenter
                onTextCommitted: function (text) {
                    var value = Number(text);
                    if (!Number.isFinite(value)) {
                        channelEditor.errorText = "Parameter '" + channelEditor.fieldLabel + "' rejects '" + text + "'";
                        return;
                    }
                    channelEditor.commitValue(channelEditor.componentTyped(3, value));
                }
                onStepped: function (value) {
                    channelEditor.commitValue(channelEditor.componentTyped(3, value));
                }
                onScrubStarted: {
                    channelEditor.errorText = "";
                    if (channelEditor.panel)
                        channelEditor.panel.beginEditFor(channelEditor.networkId, channelEditor.nodeId, channelEditor.parameterKey);
                }
                onScrubbed: function (value) {
                    if (channelEditor.panel)
                        channelEditor.panel.updateEdit(channelEditor.componentTyped(3, value));
                }
                onScrubFinished: {
                    if (channelEditor.panel)
                        channelEditor.panel.commitEdit();
                    channelEditor.refreshError();
                }
                onScrubCancelled: {
                    if (channelEditor.panel)
                        channelEditor.panel.cancelEdit();
                }
                onKeyRequested: channelEditor.requestKey()
            }
        }

        Text {
            visible: channelEditor.errorText.length > 0 && !channelEditor.expanded
            Layout.fillWidth: true
            text: channelEditor.errorText
            color: theme.errorText
            font.pixelSize: Math.max(9, theme.fontSize - 1)
            elide: Text.ElideRight
            wrapMode: Text.WordWrap
        }
    }
}
