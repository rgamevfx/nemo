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
    // Keep the shell usable in workspace-only harnesses that do not install the
    // optional router context property. Production Main always provides it.
    property var contextRouter: typeof panelContextRouter !== "undefined" ? panelContextRouter : null
    property int contextRevision: 0

    readonly property string panelId: panel && panel.id ? panel.id : ""
    readonly property string panelType: panel && panel.type ? panel.type : ""
    // The Workspace group remains the persisted A-E owner. Routing mode lives
    // in panelState and is deliberately not folded into this property.
    readonly property string panelGroup: panel && panel.group ? panel.group : "A"
    readonly property var panelState: panel && panel.state ? panel.state : ({})
    readonly property var descriptor: workspace && panelType.length > 0
                                      ? workspace.panelDescriptor(panelType) : ({})
    readonly property bool available: Boolean(descriptor && descriptor.source
                                              && descriptor.source.length > 0)
    readonly property string title: descriptor && descriptor.title
                                    && descriptor.title.length > 0
                                    ? descriptor.title : panelType
    readonly property var panelContext: {
        contextRevision
        return contextRouter && panelId.length > 0
                ? contextRouter.contextFor(panelId) : ({
                    panelId: panelId,
                    mode: bindingModeFromState(),
                    group: panelGroup,
                    resolvedGroup: panelGroup,
                    viewerRole: roleFromState(),
                    available: false,
                    unavailableReason: "Context router unavailable"
                })
    }
    readonly property string bindingMode: panelContext && panelContext.mode
                                         ? panelContext.mode : bindingModeFromState()
    readonly property string bindingBadge: bindingMode === "pinned" ? "P"
                                          : bindingMode === "group" ? panelGroup : "F"
    readonly property string bindingSummary: bindingMode === "follow"
                                             ? "Follow Active"
                                             : bindingMode === "pinned" ? "Pinned"
                                             : "Group " + panelGroup

    color: theme ? theme.panel : "#2b2b2b"

    function bindingModeFromState() {
        var value = panelState && panelState.linkMode ? panelState.linkMode : "group"
        return value === "follow" || value === "pinned" ? value : "group"
    }

    function roleFromState() {
        var value = panelState && panelState.viewerRole ? panelState.viewerRole : "graph"
        return value === "timeline" || value === "media" ? value : "graph"
    }


    function syncRouter() {
        if (!contextRouter || !panelId.length)
            return
        var expectedMode = bindingModeFromState()
        var current = contextRouter.contextFor(panelId)
        if (!current || !current.mode)
            contextRouter.registerPanel(panelId, panelGroup, expectedMode)
        else {
            if (current.mode !== expectedMode)
                contextRouter.setLinkMode(panelId, expectedMode)
            if (current.group !== panelGroup)
                contextRouter.setGroup(panelId, panelGroup)
        }
    }

    function setBindingMode(mode, group) {
        if (!contextRouter || !panelId.length)
            return
        if (mode === "group" && (group === undefined || group === ""))
            group = panelGroup
        if (mode === "group" && group !== panelGroup)
            contextRouter.setGroup(panelId, group)
        contextRouter.setLinkMode(panelId, mode)
        contextRevision++
    }

    function configureLoaded(item) {
        if (!item)
            return
        if ("panelId" in item)
            item.panelId = panelRoot.panelId
        if ("panelGroup" in item)
            item.panelGroup = panelRoot.panelGroup
        if ("panelState" in item)
            item.panelState = Qt.binding(function() { return panelRoot.panelState })
        if ("panelContext" in item)
            item.panelContext = Qt.binding(function() { return panelRoot.panelContext })
        if ("contextRouter" in item)
            item.contextRouter = panelRoot.contextRouter
        if ("workspace" in item)
            item.workspace = panelRoot.workspace
    }

    onPanelIdChanged: {
        if (contextRouter && panelId.length > 0)
            contextRouter.registerPanel(panelId, panelGroup, bindingModeFromState())
    }
    onPanelGroupChanged: syncRouter()
    onPanelStateChanged: syncRouter()
    onVisibleChanged: {
        if (visible && contextRouter && panelId.length > 0)
            contextRouter.setActivePanel(panelId)
    }
    Component.onCompleted: {
        syncRouter()
        if (contextRouter && panelId.length > 0 && visible)
            contextRouter.setActivePanel(panelId)
    }

    Connections {
        target: panelRoot.contextRouter
        function onPanelContextChanged(changedPanelId) {
            if (changedPanelId === panelRoot.panelId)
                panelRoot.contextRevision++
        }
        function onGroupContextChanged(changedGroup) {
            var resolved = panelRoot.panelContext && panelRoot.panelContext.resolvedGroup
            if (changedGroup === resolved)
                panelRoot.contextRevision++
        }
        function onActivePanelChanged() {
            panelRoot.contextRevision++
        }
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
                    id: bindingButton
                    flat: true
                    padding: 6
                    implicitWidth: 30
                    implicitHeight: 24
                    font.pixelSize: 12
                    text: panelRoot.bindingBadge
                    onClicked: bindingMenu.open()
                    Accessible.name: "Panel context binding. Opens Follow Active, group, and Pinned choices."
                    objectName: "panelBinding_" + panelRoot.panelId
                    ToolTip.visible: hovered
                    ToolTip.text: panelRoot.bindingSummary + " · "
                                   + (panelRoot.panelContext && panelRoot.panelContext.available
                                      ? "target available" : (panelRoot.panelContext && panelRoot.panelContext.unavailableReason
                                         ? panelRoot.panelContext.unavailableReason : "target unavailable"))

                    Menu {
                        id: bindingMenu
                        objectName: "panelBindingMenu_" + panelRoot.panelId
                        x: 0
                        y: parent.height
                        MenuItem {
                            text: "Follow Active"
                            checkable: true
                            checked: panelRoot.bindingMode === "follow"
                            objectName: "panelBindingFollow_" + panelRoot.panelId
                            onTriggered: panelRoot.setBindingMode("follow")
                        }
                        MenuSeparator {}
                        Repeater {
                            model: ["A", "B", "C", "D", "E"]
                            delegate: MenuItem {
                                required property string modelData
                                text: modelData
                                checkable: true
                                checked: panelRoot.bindingMode === "group" && panelRoot.panelGroup === modelData
                                objectName: "panelBindingGroup_" + modelData + "_" + panelRoot.panelId
                                onTriggered: panelRoot.setBindingMode("group", modelData)
                            }
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: "Pinned"
                            checkable: true
                            checked: panelRoot.bindingMode === "pinned"
                            objectName: "panelBindingPinned_" + panelRoot.panelId
                            onTriggered: panelRoot.setBindingMode("pinned")
                        }
                    }
                }

                Text {
                    objectName: "panelBindingState_" + panelRoot.panelId
                    text: panelRoot.bindingSummary
                    color: panelRoot.theme ? panelRoot.theme.mutedText : "#a0a0a0"
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.maximumWidth: 110
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
