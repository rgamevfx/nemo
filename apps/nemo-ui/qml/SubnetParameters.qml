import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// A transient tool window, not a scene popup. The target remains independent
// of graph selection; persistent edits still belong to the shared commands.
ApplicationWindow {
    id: subnetParameters
    objectName: "subnetParametersWindow"
    required property var controller
    required property var theme

    // Match the main window's chrome and native system move/resize gestures.
    // Qt.Tool keeps this above Nemo without a desktop-wide always-on-top flag.
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal
    visible: false
    width: 460
    height: 480
    minimumWidth: 380
    minimumHeight: 340
    title: "Edit exposed parameters" + (available ? " — " + exposure.name : "")
    color: theme.panel
    font.family: "Inter"
    font.pixelSize: theme.fontSize
    palette: transientParent.palette

    Component.onCompleted: historyController.registerWindow(subnetParameters)
    Component.onDestruction: historyController.unregisterWindow(subnetParameters)

    HistoryMenu {
        id: editMenu
    }

    header: Rectangle {
        height: 34
        color: subnetParameters.theme.header
        MouseArea {
            objectName: "subnetParametersMoveArea"
            anchors.fill: parent
            onPressed: subnetParameters.startSystemMove()
        }
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "Edit exposed parameters"
            color: subnetParameters.theme.text
        }
        ChromeButton {
            id: editMenuTrigger
            objectName: "editMenuButton"
            anchors.right: parent.right
            anchors.rightMargin: 40
            anchors.verticalCenter: parent.verticalCenter
            theme: subnetParameters.theme
            text: "Edit"
            focusPolicy: Qt.NoFocus
            Accessible.name: "Edit menu"
            onClicked: editMenu.openAt(editMenuTrigger)
        }
        ChromeButton {
            objectName: "subnetParametersClose"
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            theme: subnetParameters.theme
            text: "×"
            implicitWidth: 28
            Accessible.name: "Close exposed parameter editor"
            onClicked: subnetParameters.close()
        }
    }
    MouseArea {
        objectName: "subnetParametersResize"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 12
        height: 12
        z: 10
        cursorShape: Qt.SizeFDiagCursor
        onPressed: subnetParameters.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        Label {
            anchors.centerIn: parent
            text: "⌟"
            color: subnetParameters.theme.muted
        }
    }

    property string panelNetworkId: ""
    property string panelNodeId: ""
    property string parentPath: ""
    property var exposure: ({})
    property bool pickerOpen: false
    property var candidates: []
    property int insertionIndex: -1
    readonly property bool available: exposure.available === true
    readonly property string definition: available ? String(exposure.definition) : ""
    readonly property var rows: available ? exposure.rows : []
    readonly property int rowHeight: 46
    readonly property string linkLabel: !available ? "Unavailable" : exposure.linkState === "local" ? "Local subnet" : "Shared definition"
    readonly property var filteredCandidates: candidates.filter(function (candidate) {
            var query = search.text.trim().toLowerCase();
            return (candidate.source + " " + candidate.label).toLowerCase().indexOf(query) >= 0;
        })

    function openFor(networkId, nodeId, path) {
        panelNetworkId = String(networkId);
        panelNodeId = String(nodeId);
        parentPath = String(path || "");
        pickerOpen = false;
        search.text = "";
        refresh();
        show();
        raise();
        requestActivate();
    }

    function refresh() {
        exposure = panelNetworkId.length && controller ? controller.subnetExposure(panelNetworkId, panelNodeId) : ({});
        if (pickerOpen)
            refreshCandidates();
    }

    function refreshCandidates() {
        var result = [];
        if (available) {
            var nodes = controller.graphSnapshot(definition).nodes || [];
            for (var n = 0; n < nodes.length; ++n) {
                var node = nodes[n];
                // Formal terminals and subnet occurrences have no directly
                // promotable catalog parameters. Do not fabricate a schema.
                if (!/^\d+$/.test(String(node.id)) || node.instance)
                    continue;
                var inspector = controller.parameterInspector(definition, String(node.id));
                var sections = inspector.sections || [];
                for (var s = 0; s < sections.length; ++s) {
                    var parameters = sections[s].parameters || [];
                    for (var p = 0; p < parameters.length; ++p) {
                        var parameter = parameters[p];
                        result.push({
                                "networkId": definition,
                                "nodeId": String(node.id),
                                "parameterKey": String(parameter.key),
                                "label": String(parameter.label),
                                "source": String(node.name) + "." + parameter.key,
                                "type": String(parameter.type)
                            });
                    }
                }
            }
        }
        result.sort(function (a, b) {
                return a.source.localeCompare(b.source);
            });
        candidates = result;
    }

    function exposedIndex(payload) {
        for (var i = 0; i < rows.length; ++i) {
            if (String(rows[i].node) === String(payload.nodeId) && String(rows[i].key) === String(payload.parameterKey))
                return i;
        }
        return -1;
    }

    function rejection(payload) {
        if (!available)
            return "This subnet is no longer available.";
        if (!payload || String(payload.networkId) !== definition)
            return "Choose a parameter from this subnet's own graph.";
        if (payload.exposureId) {
            for (var i = 0; i < rows.length; ++i)
                if (String(rows[i].id) === String(payload.exposureId))
                    return "";
            return "This exposure is no longer available.";
        }
        if (!/^\d+$/.test(String(payload.nodeId)) || !String(payload.parameterKey || "").length)
            return "This drag does not identify a parameter.";
        if (exposedIndex(payload) >= 0)
            return "This parameter is already exposed.";
        return "";
    }

    function dragPayload(drag) {
        var formats = drag.formats;
        var type = formats.indexOf("application/x-nemo-parameter") >= 0 ? "application/x-nemo-parameter" : formats.indexOf("application/x-nemo-exposure") >= 0 ? "application/x-nemo-exposure" : "";
        if (!type.length)
            return null;
        try {
            return JSON.parse(drag.getDataAsString(type));
        } catch (error) {
            return null;
        }
    }

    // A refusal is a return value, never a printed line: the rejection reason is
    // the drag's own acceptance test, and every command failure stays on the
    // controller that owns it.
    function dropParameter(payload, index) {
        if (rejection(payload).length)
            return false;
        return controller.promoteParameter(definition, String(payload.nodeId), String(payload.parameterKey), "", index);
    }

    function renameRow(id, name) {
        if (!available)
            return;
        // An empty label is refused by the command owner (it requires a name);
        // the field returns to the authored label with no line printed here.
        controller.renameExposedParameter(definition, id, name.trim());
    }

    function removeRow(id) {
        if (available)
            controller.removeExposedParameter(definition, id);
    }

    function moveRow(id, index) {
        if (!available)
            return false;
        return controller.moveExposedParameter(definition, id, index);
    }

    function acceptDrop(payload, index) {
        if (!payload.exposureId)
            return dropParameter(payload, index);
        var from = -1;
        for (var i = 0; i < rows.length; ++i)
            if (String(rows[i].id) === String(payload.exposureId))
                from = i;
        var destination = Math.max(0, Math.min(rows.length - 1, index - (from < index ? 1 : 0)));
        if (from < 0)
            return false;
        if (destination === from)
            return true;
        return moveRow(String(payload.exposureId), destination);
    }

    onClosing: {
        panelNetworkId = "";
        panelNodeId = "";
        exposure = ({});
        candidates = [];
        pickerOpen = false;
    }
    Connections {
        target: subnetParameters.controller
        function onGraphChanged() {
            if (subnetParameters.visible)
                Qt.callLater(subnetParameters.refresh);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: subnetParameters.parentPath + (subnetParameters.available ? " / " + subnetParameters.exposure.name : "")
            elide: Text.ElideMiddle
            color: subnetParameters.theme.text
            font.bold: true
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                objectName: "subnetParametersLinkState"
                text: subnetParameters.linkLabel
                color: subnetParameters.theme.muted
                Layout.fillWidth: true
            }
            ChromeButton {
                objectName: "subnetAddParameter"
                theme: subnetParameters.theme
                text: subnetParameters.pickerOpen ? "Hide browser" : "Add parameter…"
                enabled: subnetParameters.available
                onClicked: {
                    subnetParameters.pickerOpen = !subnetParameters.pickerOpen;
                    if (subnetParameters.pickerOpen) {
                        subnetParameters.refreshCandidates();
                        search.forceActiveFocus();
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 90
            color: subnetParameters.theme.field
            border.color: subnetParameters.theme.border
            radius: subnetParameters.theme.smallRadius

            ListView {
                id: exposureList
                objectName: "subnetExposedList"
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: subnetParameters.rows
                ScrollBar.vertical: ScrollBar {
                }
                delegate: Rectangle {
                    id: exposedRow
                    required property var modelData
                    required property int index
                    readonly property string rowId: String(modelData.id)
                    objectName: "subnetExposedRow_" + rowId
                    width: exposureList.width
                    height: subnetParameters.rowHeight
                    color: index % 2 ? subnetParameters.theme.field : subnetParameters.theme.panel
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.MoveAction
                    Drag.proposedAction: Qt.MoveAction
                    Drag.mimeData: ({
                            "application/x-nemo-exposure": JSON.stringify({
                                    "networkId": subnetParameters.definition,
                                    "exposureId": exposedRow.rowId
                                })
                        })
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 6
                        Label {
                            objectName: "subnetExposedHandle_" + exposedRow.rowId
                            text: "≡"
                            color: subnetParameters.theme.muted
                            Layout.preferredWidth: 18
                            Accessible.name: "Reorder " + exposedRow.modelData.name
                            HoverHandler {
                                cursorShape: Qt.OpenHandCursor
                            }
                            DragHandler {
                                id: reorderDrag
                                target: null
                                acceptedButtons: Qt.LeftButton
                                onActiveChanged: {
                                    if (!active) {
                                        exposedRow.Drag.active = false;
                                        return;
                                    }
                                    exposedRow.grabToImage(function (image) {
                                            if (!reorderDrag.active)
                                                return;
                                            exposedRow.Drag.imageSource = image.url;
                                            exposedRow.Drag.active = true;
                                        });
                                }
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            TextField {
                                id: labelField
                                objectName: "subnetExposedLabel_" + exposedRow.rowId
                                Layout.fillWidth: true
                                implicitHeight: 23
                                padding: 3
                                text: exposedRow.modelData.name
                                selectByMouse: true
                                Accessible.name: "Exposed parameter label"
                                background: Rectangle {
                                    color: labelField.activeFocus ? subnetParameters.theme.raised : "transparent"
                                    border.width: labelField.activeFocus ? 1 : 0
                                    border.color: subnetParameters.theme.accent
                                    radius: subnetParameters.theme.smallRadius
                                }
                                onEditingFinished: {
                                    if (text.trim() !== String(exposedRow.modelData.name))
                                        subnetParameters.renameRow(exposedRow.rowId, text);
                                }
                                Keys.onEscapePressed: function (event) {
                                    text = exposedRow.modelData.name;
                                    focus = false;
                                    event.accepted = true;
                                }
                            }
                            Label {
                                objectName: "subnetExposedSource_" + exposedRow.rowId
                                Layout.fillWidth: true
                                text: exposedRow.modelData.source + "  ·  " + exposedRow.modelData.type
                                color: subnetParameters.theme.muted
                                elide: Text.ElideMiddle
                            }
                        }
                        ChromeButton {
                            objectName: "subnetExposedRemove_" + exposedRow.rowId
                            theme: subnetParameters.theme
                            text: "−"
                            implicitWidth: 26
                            Accessible.name: "Remove exposure " + exposedRow.modelData.name
                            ToolTip.visible: hovered
                            ToolTip.text: "Remove exposure; keep the source parameter and animation"
                            onClicked: subnetParameters.removeRow(exposedRow.rowId)
                        }
                    }
                }
            }
            Label {
                objectName: "subnetParametersHint"
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: subnetParameters.rows.length === 0
                text: subnetParameters.available ? "Drag a parameter label here\nor choose Add parameter…" : String(subnetParameters.exposure.reason || "This subnet is unavailable.")
                color: subnetParameters.theme.muted
            }
            DropArea {
                id: parameterDrop
                objectName: "subnetParametersDropArea"
                anchors.fill: exposureList
                keys: ["application/x-nemo-parameter", "application/x-nemo-exposure"]
                property var payload: null
                function updatePosition(y) {
                    subnetParameters.insertionIndex = Math.max(0, Math.min(subnetParameters.rows.length, Math.floor((y + exposureList.contentY + subnetParameters.rowHeight / 2) / subnetParameters.rowHeight)));
                }
                onEntered: function (drag) {
                    payload = subnetParameters.dragPayload(drag);
                    drag.accepted = subnetParameters.rejection(payload).length === 0;
                    if (drag.accepted)
                        updatePosition(drag.y);
                }
                onPositionChanged: function (drag) {
                    updatePosition(drag.y);
                }
                onExited: subnetParameters.insertionIndex = -1
                onDropped: function (drop) {
                    var incoming = subnetParameters.dragPayload(drop);
                    if (subnetParameters.rejection(incoming).length === 0 && subnetParameters.acceptDrop(incoming, subnetParameters.insertionIndex))
                        drop.accept(incoming.exposureId ? Qt.MoveAction : Qt.CopyAction);
                    subnetParameters.insertionIndex = -1;
                }
            }
            Rectangle {
                x: 4
                y: Math.max(4, Math.min(parent.height - 5, 4 + subnetParameters.insertionIndex * subnetParameters.rowHeight - exposureList.contentY))
                width: parent.width - 8
                height: 2
                visible: parameterDrop.containsDrag && subnetParameters.insertionIndex >= 0
                color: subnetParameters.theme.accent
            }
            Timer {
                interval: 35
                repeat: true
                running: parameterDrop.containsDrag
                onTriggered: {
                    var y = parameterDrop.drag.y;
                    var delta = y < 24 ? -8 : y > exposureList.height - 24 ? 8 : 0;
                    exposureList.contentY = Math.max(0, Math.min(Math.max(0, exposureList.contentHeight - exposureList.height), exposureList.contentY + delta));
                    parameterDrop.updatePosition(y);
                }
            }
        }
        ColumnLayout {
            visible: subnetParameters.pickerOpen
            Layout.fillWidth: true
            spacing: 4
            TextField {
                id: search
                objectName: "subnetParameterSearch"
                Layout.fillWidth: true
                placeholderText: "Find node or parameter…"
                color: subnetParameters.theme.text
                placeholderTextColor: subnetParameters.theme.muted
                selectByMouse: true
                Keys.onEscapePressed: subnetParameters.pickerOpen = false
            }
            ListView {
                id: candidateList
                objectName: "subnetParameterCandidates"
                Layout.fillWidth: true
                Layout.preferredHeight: 130
                clip: true
                model: subnetParameters.filteredCandidates
                ScrollBar.vertical: ScrollBar {
                }
                delegate: ItemDelegate {
                    id: candidate
                    required property var modelData
                    width: candidateList.width
                    height: 28
                    readonly property bool alreadyExposed: subnetParameters.exposedIndex(modelData) >= 0
                    enabled: !alreadyExposed
                    text: modelData.source + "  ·  " + modelData.type + (alreadyExposed ? "  — exposed" : "")
                    onClicked: subnetParameters.dropParameter(modelData, subnetParameters.rows.length)
                }
                Label {
                    anchors.centerIn: parent
                    visible: candidateList.count === 0
                    text: "No matching parameters"
                    color: subnetParameters.theme.muted
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: subnetParameters.available && subnetParameters.exposure.linkState !== "local" ? "Interface changes affect linked instances. Values stay instance-local." : "Edit values in the Parameters inspector. Changes are undoable."
            color: subnetParameters.theme.muted
            wrapMode: Text.WordWrap
        }
    }
}
