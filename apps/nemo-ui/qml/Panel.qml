import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared presentation shell for every registered panel type. The descriptor
// is the only source of a panel's title, body, and optional header tools.
Rectangle {
    id: panelRoot

    property var panel
    property string leafId
    property var workspace
    property var drag
    property var theme

    readonly property string panelId: panel && panel.id ? panel.id : ""
    readonly property string panelType: panel && panel.type ? panel.type : ""
    readonly property string panelGroup: panel && panel.group ? panel.group : "A"
    readonly property var panelState: panel && panel.state ? panel.state : ({})
    readonly property var descriptor: workspace && panelType.length > 0
                                      ? workspace.panelDescriptor(panelType) : ({})
    readonly property bool available: Boolean(descriptor && descriptor.source
                                              && descriptor.source.length > 0)
    readonly property string title: descriptor && descriptor.title
                                    && descriptor.title.length > 0
                                    ? descriptor.title : panelType

    color: theme ? theme.panel : "#2b2b2b"

    function configureLoaded(item) {
        if (!item)
            return
        if ("panelId" in item)
            item.panelId = panelRoot.panelId
        if ("panelGroup" in item)
            item.panelGroup = panelRoot.panelGroup
        if ("panelState" in item)
            item.panelState = panelRoot.panelState
        if ("workspace" in item)
            item.workspace = panelRoot.workspace
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: panelRoot.theme ? panelRoot.theme.header : "#333333"
            objectName: "panelHeader_" + panelRoot.panelId

            // This is deliberately the sole drag recognizer for a panel.
            // Child buttons retain click delivery until the drag threshold.
            PanelDragHandler {
                panelId: panelRoot.panelId
                leafId: panelRoot.leafId
                panelType: panelRoot.panelType
                drag: panelRoot.drag
            }

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
                    text: panelRoot.title
                    onClicked: typeMenu.open()
                    Accessible.name: "Panel type. Opens the panel menu."
                    objectName: "panelType_" + panelRoot.panelId

                    Menu {
                        id: typeMenu
                        objectName: "panelTypeMenu_" + panelRoot.panelId
                        x: 0
                        y: parent.height

                        Repeater {
                            model: panelRoot.workspace ? panelRoot.workspace.panelTypes : []
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData.title
                                checkable: true
                                checked: panelRoot.panelType === modelData.typeId
                                objectName: "panelTypeChoice_" + modelData.typeId + "_" + panelRoot.panelId
                                onTriggered: panelRoot.workspace.setPanelType(panelRoot.panelId, modelData.typeId)
                            }
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
                            objectName: "panelClose_" + panelRoot.panelId
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
                        objectName: "panelGroupMenu_" + panelRoot.panelId
                        x: 0
                        y: parent.height
                        Repeater {
                            model: ["A", "B", "C", "D", "E"]
                            delegate: MenuItem {
                                required property string modelData
                                text: modelData
                                checkable: true
                                checked: panelRoot.panelGroup === modelData
                                onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, modelData)
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                Loader {
                    id: headerTools
                    Layout.fillHeight: true
                    visible: panelRoot.available && panelRoot.descriptor.headerSource
                             && panelRoot.descriptor.headerSource.length > 0
                    source: visible ? panelRoot.descriptor.headerSource : ""
                    onLoaded: panelRoot.configureLoaded(item)
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                id: bodyLoader
                anchors.fill: parent
                active: panelRoot.available
                source: panelRoot.available ? panelRoot.descriptor.source : ""
                onLoaded: panelRoot.configureLoaded(item)
            }

            Loader {
                anchors.fill: parent
                active: !panelRoot.available
                sourceComponent: unavailableComponent
                onLoaded: {
                    if (item) {
                        item.panelType = panelRoot.panelType
                        item.theme = panelRoot.theme
                    }
                }
            }
        }
    }

    Component {
        id: unavailableComponent
        UnavailablePanel {}
    }
}
