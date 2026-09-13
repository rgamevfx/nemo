import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Owner-approved nonmodal authoring surface for a subnet's exposed parameters
// (issue #49). It floats above the workspace, stays open while the artist
// navigates into the child graph and drags parameters from the ordinary
// Parameters inspector, and is presentation state only: every edit it makes is
// a shared document command with its own undo step.
Popup {
    id: subnetParameters

    objectName: "subnetParametersPopup"
    modal: false
    focus: false
    // The graph keeps its own Escape handling; the popout closes only from its
    // own control so an in-flight gesture is never stolen.
    closePolicy: Popup.NoAutoClose
    padding: 0
    width: 292
    height: Math.min(360, header.height + body.implicitHeight + 10)

    property var controller: null
    property var theme: null
    // Occurrence the popout was opened for. Kept across scope navigation so
    // the surface does not follow the active graph.
    property string panelNetworkId: ""
    property string panelNodeId: ""
    property var exposure: ({})
    property string message: ""

    readonly property string definition: exposure && exposure.definition !== undefined
                                         ? String(exposure.definition) : ""
    readonly property var rows: exposure && exposure.rows !== undefined ? exposure.rows : []
    readonly property string linkLabel: exposure && String(exposure.linkState) === "local" ? "Local subnet"
                                       : exposure && String(exposure.linkState) === "shared" ? "Shared definition"
                                                                                             : "Linked instance"

    function openFor(networkId, nodeId) {
        panelNetworkId = String(networkId || "");
        panelNodeId = String(nodeId || "");
        message = "";
        refresh();
        if (!opened)
            open();
    }

    function refresh() {
        if (!controller || panelNetworkId.length === 0 || panelNodeId.length === 0) {
            exposure = ({});
            return;
        }
        exposure = controller.subnetExposure(panelNetworkId, panelNodeId);
    }

    function reject(reason) {
        message = reason;
    }

    // Row actions for the delegate. The delegate calls these through a bound
    // reference rather than reaching for the popup id from inside a handler.
    function renameRow(parameterId, newName) {
        if (!controller || newName.length === 0 || parameterId.length === 0)
            return;
        if (controller.renameExposedParameter(definition, parameterId, newName))
            message = "";
    }

    function removeRow(parameterId) {
        if (!controller || parameterId.length === 0)
            return;
        if (controller.removeExposedParameter(definition, parameterId))
            message = "";
    }

    function moveRow(parameterId, index) {
        if (!controller || parameterId.length === 0)
            return;
        controller.moveExposedParameter(definition, parameterId, index);
    }

    // A drop only promotes a parameter dragged from this subnet's own
    // definition; anything else is reported rather than exposing an unrelated
    // parameter. The payload carries source identities, never a copy.
    function acceptsPayload(payload) {
        return !!payload && String(payload.networkId) === definition
               && String(payload.parameterKey || "").length > 0;
    }

    function dropParameter(payload) {
        if (!acceptsPayload(payload)) {
            reject("Drag a parameter from this subnet's own graph.");
            return;
        }
        if (controller.promoteParameter(String(payload.networkId), String(payload.nodeId),
                                        String(payload.parameterKey), ""))
            message = "";
    }

    onClosed: {
        panelNetworkId = "";
        panelNodeId = "";
        exposure = ({});
        message = "";
    }

    Connections {
        target: subnetParameters.controller
        function onGraphChanged() {
            subnetParameters.refresh();
        }
    }

    background: Rectangle {
        color: subnetParameters.theme ? subnetParameters.theme.panel : "#1e2023"
        border.color: subnetParameters.theme ? subnetParameters.theme.border : "#30343a"
        border.width: 1
        radius: subnetParameters.theme ? subnetParameters.theme.radius : 7
    }

    // Children of a Popup are parented into its content item by the popup
    // itself. Declaring them here (instead of assigning `contentItem`) keeps
    // every id in this file's creation context, so row and drop handlers
    // resolve the popup normally.
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- header: drag handle, link state, close --------------------------
        Rectangle {
            id: header
            objectName: "subnetParametersHeader"
            Layout.fillWidth: true
            implicitHeight: 28
            color: subnetParameters.theme ? subnetParameters.theme.panel : "#1e2023"

            DragHandler {
                id: headerDrag
                target: null
                acceptedButtons: Qt.LeftButton
                property real grabX: 0
                property real grabY: 0
                property point grabScene
                onActiveChanged: {
                    if (!active)
                        return;
                    grabX = subnetParameters.x;
                    grabY = subnetParameters.y;
                    grabScene = headerDrag.centroid.scenePosition;
                }
                onCentroidChanged: {
                    if (!active)
                        return;
                    var point = headerDrag.centroid.scenePosition;
                    subnetParameters.x = grabX + (point.x - grabScene.x);
                    subnetParameters.y = grabY + (point.y - grabScene.y);
                }
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: "Subnet Parameters"
                color: subnetParameters.theme ? subnetParameters.theme.text : "#dce0e6"
                font.pixelSize: subnetParameters.theme ? subnetParameters.theme.fontSize : 11
            }

            Text {
                id: linkLabelText
                objectName: "subnetParametersLinkState"
                anchors.right: closeButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: subnetParameters.linkLabel
                color: subnetParameters.theme ? subnetParameters.theme.muted : "#979ea8"
                font.pixelSize: subnetParameters.theme ? subnetParameters.theme.fontSize : 11
            }

            Button {
                id: closeButton
                objectName: "subnetParametersClose"
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 20
                implicitHeight: 20
                padding: 0
                Accessible.name: "Close subnet parameters"
                onClicked: subnetParameters.close()
                contentItem: Text {
                    text: "\u00d7"
                    color: closeButton.down ? (subnetParameters.theme ? subnetParameters.theme.accent : "#3485f6")
                                            : (subnetParameters.theme ? subnetParameters.theme.text : "#dce0e6")
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: subnetParameters.theme ? subnetParameters.theme.smallRadius : 4
                    color: closeButton.hovered ? (subnetParameters.theme ? subnetParameters.theme.hover : "#343940")
                                               : "transparent"
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: subnetParameters.theme ? subnetParameters.theme.border : "#30343a"
            }
        }

        // --- exposed rows and drop target ------------------------------------
        Item {
            id: body
            Layout.fillWidth: true
            implicitHeight: bodyColumn.implicitHeight + 8

            DropArea {
                id: parameterDrop
                objectName: "subnetParametersDropArea"
                anchors.fill: parent
                // Only parameters dragged from this subnet's own definition are
                // meaningful; a payload from any other graph is rejected with a
                // reason instead of exposing an unrelated parameter.
                onEntered: function (drag) {
                    if (subnetParameters.acceptsPayload(drag.source))
                        drag.acceptProposedAction();
                }
                onDropped: function (drop) {
                    subnetParameters.dropParameter(drop.source);
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: subnetParameters.theme ? subnetParameters.theme.smallRadius : 4
                    color: "transparent"
                    border.width: parameterDrop.containsDrag ? 1 : 0
                    border.color: subnetParameters.theme ? subnetParameters.theme.accent : "#3485f6"
                }
            }

            Column {
                id: bodyColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 4
                spacing: 2

                Text {
                    objectName: "subnetParametersHint"
                    visible: subnetParameters.rows.length === 0 && subnetParameters.message.length === 0
                    width: parent.width
                    leftPadding: 8
                    rightPadding: 8
                    wrapMode: Text.WordWrap
                    text: "Drag a parameter label from the Parameters inspector to expose it here."
                    color: subnetParameters.theme ? subnetParameters.theme.muted : "#979ea8"
                    font.pixelSize: subnetParameters.theme ? subnetParameters.theme.fontSize : 11
                }

                Text {
                    objectName: "subnetParametersMessage"
                    visible: subnetParameters.message.length > 0
                    width: parent.width
                    leftPadding: 8
                    rightPadding: 8
                    wrapMode: Text.WordWrap
                    text: subnetParameters.message
                    color: subnetParameters.theme ? subnetParameters.theme.accent : "#3485f6"
                    font.pixelSize: subnetParameters.theme ? subnetParameters.theme.fontSize : 11
                }

                Repeater {
                    model: subnetParameters.rows
                    delegate: Rectangle {
                        id: exposedRow
                        required property var modelData
                        required property int index
                        readonly property string rowId: modelData && modelData.id !== undefined
                                                        ? String(modelData.id) : ""
                        readonly property real rowPitch: height + bodyColumn.spacing
                        readonly property var host: subnetParameters

                        objectName: "subnetExposedRow_" + rowId
                        width: bodyColumn.width
                        height: 26
                        color: "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            spacing: 4

                            Text {
                                objectName: "subnetExposedHandle_" + exposedRow.rowId
                                Layout.preferredWidth: 12
                                text: "\u2261"
                                color: exposedRow.modelData && exposedRow.modelData.name !== undefined
                                       ? (subnetParameters.theme ? subnetParameters.theme.muted : "#979ea8") : "transparent"
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                Accessible.name: "Reorder handle"
                                DragHandler {
                                    id: reorderHandle
                                    target: null
                                    acceptedButtons: Qt.LeftButton
                                    property real releasedOffset: 0
                                    onActiveChanged: {
                                        if (active)
                                            return;
                                        var destination = Math.round((exposedRow.y + reorderHandle.releasedOffset)
                                                                     / exposedRow.rowPitch);
                                        destination = Math.max(0, Math.min(exposedRow.host.rows.length - 1, destination));
                                        exposedRow.host.moveRow(exposedRow.rowId, destination);
                                    }
                                    onCentroidChanged: {
                                        if (active)
                                            reorderHandle.releasedOffset = reorderHandle.centroid.scenePosition.y
                                                                           - reorderHandle.centroid.pressPosition.y;
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 22
                                radius: subnetParameters.theme ? subnetParameters.theme.smallRadius : 4
                                color: labelField.activeFocus
                                       ? (subnetParameters.theme ? subnetParameters.theme.field : "#24272c")
                                       : "transparent"
                                border.width: labelField.activeFocus ? 1 : 0
                                border.color: subnetParameters.theme ? subnetParameters.theme.accent : "#3485f6"

                                TextInput {
                                    id: labelField
                                    objectName: "subnetExposedLabel_" + exposedRow.rowId
                                    anchors.fill: parent
                                    anchors.leftMargin: 3
                                    anchors.rightMargin: 3
                                    text: exposedRow.modelData ? String(exposedRow.modelData.name) : ""
                                    color: subnetParameters.theme ? subnetParameters.theme.text : "#dce0e6"
                                    font.pixelSize: subnetParameters.theme ? subnetParameters.theme.fontSize : 11
                                    selectByMouse: true
                                    verticalAlignment: TextInput.AlignVCenter
                                    Accessible.name: "Exposed parameter label"
                                    Component.onCompleted: text = exposedRow.modelData ? String(exposedRow.modelData.name) : ""
                                    onEditingFinished: {
                                        if (text.trim().length === 0 || text === String(exposedRow.modelData.name))
                                            return;
                                        exposedRow.host.renameRow(exposedRow.rowId, text.trim());
                                    }
                                }
                            }

                            Text {
                                objectName: "subnetExposedSource_" + exposedRow.rowId
                                Layout.preferredWidth: 104
                                text: exposedRow.modelData ? String(exposedRow.modelData.source) : ""
                                color: subnetParameters.theme ? subnetParameters.theme.muted : "#979ea8"
                                font.pixelSize: subnetParameters.theme ? subnetParameters.theme.fontSize : 11
                                elide: Text.ElideMiddle
                                horizontalAlignment: Text.AlignRight
                                verticalAlignment: Text.AlignVCenter
                            }

                            Button {
                                id: removeButton
                                objectName: "subnetExposedRemove_" + exposedRow.rowId
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                padding: 0
                                Accessible.name: "Remove exposure"
                                onClicked: exposedRow.host.removeRow(exposedRow.rowId)
                                contentItem: Text {
                                    text: "\u2212"
                                    color: removeButton.down ? (subnetParameters.theme ? subnetParameters.theme.accent : "#3485f6")
                                                             : (subnetParameters.theme ? subnetParameters.theme.muted : "#979ea8")
                                    font.pixelSize: 13
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: subnetParameters.theme ? subnetParameters.theme.smallRadius : 4
                                    color: removeButton.hovered
                                           ? (subnetParameters.theme ? subnetParameters.theme.hover : "#343940") : "transparent"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
