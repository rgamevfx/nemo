import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A single workspace panel. The panel body dispatches to the native viewer,
// authored graph, or source-timing timeline surface. The compact header keeps
// workspace layout operations separate from document commands.
Rectangle {
    id: panelRoot

    property var panel           // { id, type, group }
    property string leafId
    property var workspace
    property var drag            // the DockDrag coordinator

    readonly property string panelId: panel ? panel.id : ""
    readonly property string panelType: panel ? panel.type : "viewer"
    readonly property string panelGroup: panel ? panel.group : "A"

    color: "#2b2b2b"

    function displayType(t) {
        if (t === "viewer") return "Viewer"
        if (t === "nodegraph") return "Nodegraph"
        if (t === "timeline") return "Timeline"
        return t
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: "#333333"
            objectName: "panelHeader_" + panelRoot.panelId
            RowLayout {
                anchors.fill: parent
                anchors.margins: 2
                spacing: 2

                Button {
                    id: typeButton
                    flat: true
                    padding: 6
                    implicitWidth: contentItem.implicitWidth + 16
                    implicitHeight: 24
                    font.pixelSize: 12
                    text: panelRoot.displayType(panelRoot.panelType)
                    onClicked: typeMenu.open()
                    Accessible.name: "Panel type. Opens the panel menu."
                    objectName: "panelType_" + panelRoot.panelId

                    PanelDragHandler {
                        panelId: panelRoot.panelId
                        leafId: panelRoot.leafId
                        panelType: panelRoot.panelType
                        drag: panelRoot.drag
                    }

                    Menu {
                        id: typeMenu
                        objectName: "panelTypeMenu_" + panelRoot.panelId
                        x: 0
                        y: parent.height
                        MenuItem {
                            text: "Viewer"
                            checkable: true
                            checked: panelRoot.panelType === "viewer"
                            onTriggered: panelRoot.workspace.setPanelType(panelRoot.panelId, "viewer")
                        }
                        MenuItem {
                            text: "Nodegraph"
                            checkable: true
                            checked: panelRoot.panelType === "nodegraph"
                            onTriggered: panelRoot.workspace.setPanelType(panelRoot.panelId, "nodegraph")
                        }
                        MenuItem {
                            text: "Timeline"
                            checkable: true
                            checked: panelRoot.panelType === "timeline"
                            onTriggered: panelRoot.workspace.setPanelType(panelRoot.panelId, "timeline")
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: "Split Horizontal"
                            onTriggered: panelRoot.workspace.split(panelRoot.leafId, "horizontal")
                        }
                        MenuItem {
                            text: "Split Vertical"
                            onTriggered: panelRoot.workspace.split(panelRoot.leafId, "vertical")
                        }
                        MenuItem {
                            text: "Add Tab"
                            onTriggered: panelRoot.workspace.addTab(panelRoot.leafId, panelRoot.panelType)
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: "Close Panel"
                            onTriggered: panelRoot.workspace.closePanel(panelRoot.panelId)
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: "Reset Layout"
                            onTriggered: panelRoot.workspace.reset()
                        }
                    }
                }

                Button {
                    id: groupButton
                    flat: true
                    padding: 6
                    implicitWidth: 26
                    implicitHeight: 24
                    font.pixelSize: 12
                    text: panelRoot.panelGroup
                    onClicked: groupMenu.open()
                    Accessible.name: "Panel display group. Opens the group menu."
                    objectName: "panelGroup_" + panelRoot.panelId
                    ToolTip.visible: hovered
                    ToolTip.text: "A-E display group. Saved metadata only; no live context routing."

                    Menu {
                        id: groupMenu
                        x: 0
                        y: parent.height
                        MenuItem {
                            text: "A"
                            checkable: true
                            checked: panelRoot.panelGroup === "A"
                            onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, "A")
                        }
                        MenuItem {
                            text: "B"
                            checkable: true
                            checked: panelRoot.panelGroup === "B"
                            onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, "B")
                        }
                        MenuItem {
                            text: "C"
                            checkable: true
                            checked: panelRoot.panelGroup === "C"
                            onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, "C")
                        }
                        MenuItem {
                            text: "D"
                            checkable: true
                            checked: panelRoot.panelGroup === "D"
                            onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, "D")
                        }
                        MenuItem {
                            text: "E"
                            checkable: true
                            checked: panelRoot.panelGroup === "E"
                            onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, "E")
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    PanelDragHandler {
                        panelId: panelRoot.panelId
                        leafId: panelRoot.leafId
                        panelType: panelRoot.panelType
                        drag: panelRoot.drag
                    }
                }
            }
        }

        // Panel body. Each processing surface reads the same controller and
        // routes edits through its command invokables; only the viewer hosts
        // native presentation.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                anchors.fill: parent
                active: true
                source: panelRoot.panelType === "viewer" ? "ViewerPanel.qml"
                       : panelRoot.panelType === "nodegraph" ? "GraphPanel.qml"
                       : panelRoot.panelType === "timeline" ? "TimelinePanel.qml" : ""
            }
        }
    }
}
