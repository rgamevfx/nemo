import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Registered Merge editor (issue #75 stories 33-35, 38; #78). It presents the
// explicit A/B input roles and the Operation menu, and hosts the one atomic
// Swap A/B command the graph currently owns. Mask Channel/Invert Mask stay
// ordinary rows in the Mask section and Mix stays an independent row in
// Composite, so neither depends on the other.
Item {
    id: mergeEditor

    property var theme: null
    property var panel: null
    property var controller: null
    property string networkId: ""
    property string nodeId: ""
    property string parameterKey: ""
    property var parameter: null

    readonly property int revision: panel ? Number(panel.revision) : 0
    readonly property var occupancy: {
        revision;
        if (!controller || networkId.length === 0 || nodeId.length === 0)
            return null;
        return controller.nodeInputOccupancy(networkId, nodeId);
    }
    readonly property var ports: occupancy && occupancy.available === true ? occupancy.ports : []
    readonly property var firstPort: ports.length > 0 ? ports[0] : null
    readonly property var secondPort: ports.length > 1 ? ports[1] : null
    // A swap is meaningful only when it moves something: at least one of the
    // two declared image inputs is connected and the two sources differ. Any
    // other request is refused by the command owner without a history entry.
    readonly property bool swapMeaningful: {
        if (!firstPort || !secondPort)
            return false;
        if (String(firstPort.kind) !== "image" || String(secondPort.kind) !== "image")
            return false;
        if (!firstPort.connected && !secondPort.connected)
            return false;
        if (firstPort.connected && secondPort.connected && String(firstPort.source) === String(secondPort.source))
            return false;
        return true;
    }
    readonly property string swapReason: !firstPort || !secondPort
                                          ? "This node has fewer than two image inputs"
                                          : (!firstPort.connected && !secondPort.connected)
                                            ? "Connect A or B to swap the inputs"
                                            : (firstPort.connected && secondPort.connected && String(firstPort.source) === String(secondPort.source))
                                              ? "A and B already read the same source"
                                              : ""
    readonly property string operation: parameter && parameter.value !== undefined ? String(parameter.value) : "over"
    readonly property var choices: parameter && parameter.choices ? parameter.choices : []
    readonly property int choiceIndex: {
        var index = choices.indexOf(operation);
        return index < 0 ? 0 : index;
    }

    implicitHeight: editorColumn.implicitHeight
    Layout.fillWidth: true

    function rowObject() {
        return {
            "networkId": networkId,
            "nodeId": nodeId,
            "parameterKey": parameterKey,
            "parameter": parameter,
            "label": parameter && parameter.label !== undefined ? String(parameter.label) : parameterKey
        };
    }

    function setOperation(text) {
        if (!panel)
            return false;
        return panel.gestureSingle(rowObject(), String(text));
    }

    function swapInputs() {
        if (!controller || !swapMeaningful)
            return false;
        return controller.swapNodeInputs(networkId, nodeId, 0, 1);
    }

    ColumnLayout {
        id: editorColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Text {
                text: "A \u00b7 Foreground"
                color: theme.muted
                font.pixelSize: theme.fontSize
                Accessible.name: "Input A foreground"
            }
            Text {
                text: "B \u00b7 Background"
                color: theme.muted
                font.pixelSize: theme.fontSize
                Accessible.name: "Input B background"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            StudioComboBox {
                id: operationBox
                objectName: "merge_operation_" + mergeEditor.nodeId
                theme: mergeEditor.theme
                model: mergeEditor.choices
                currentIndex: mergeEditor.choiceIndex
                Layout.fillWidth: true
                implicitHeight: 23
                Accessible.name: "Merge operation"
                onActivated: mergeEditor.setOperation(currentText)
            }

            Button {
                id: swapButton
                objectName: "merge_swap_" + mergeEditor.nodeId
                enabled: mergeEditor.swapMeaningful
                implicitWidth: 62
                implicitHeight: 23
                padding: 0
                text: "Swap A/B"
                Accessible.name: "Swap merge inputs A and B"
                onClicked: mergeEditor.swapInputs()
                contentItem: Text {
                    text: swapButton.text
                    color: swapButton.enabled ? (swapButton.down ? theme.accent : theme.text) : theme.disabled
                    font.pixelSize: theme.fontSize
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: swapButton.enabled && swapButton.hovered ? theme.hover : "transparent"
                    border.color: mergeEditor.swapMeaningful ? theme.border : "transparent"
                    radius: theme.smallRadius
                }
                ToolTip.visible: swapHover.hovered && mergeEditor.swapReason.length > 0
                ToolTip.text: mergeEditor.swapReason
                HoverHandler {
                    id: swapHover
                }
            }
        }
    }
}
