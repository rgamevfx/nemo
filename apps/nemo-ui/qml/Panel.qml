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
    // The Workspace group is the persisted A-E owner and the panel's sole
    // context key.
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
                    group: panelGroup,
                    viewerRole: roleFromState(),
                    available: false,
                    unavailableReason: "Context router unavailable"
                })
    }

    color: theme ? theme.panel : "#2b2b2b"
    radius: theme ? theme.radius : 7
    border.color: theme ? theme.border : "#30343a"
    clip: true

    function roleFromState() {
        var value = panelState && panelState.viewerRole ? panelState.viewerRole : "graph"
        return value === "timeline" || value === "media" ? value : "graph"
    }


    function syncRouter() {
        if (!contextRouter || !panelId.length)
            return
        // Hydrate from saved state without writing back while QML is reading
        // the workspace root.
        contextRouter.registerPanel(panelId, panelGroup)
        contextRevision++
    }

    function configureLoaded(item) {
        if (!item)
            return
        if ("panelId" in item)
            item.panelId = Qt.binding(function() { return panelRoot.panelId })
        if ("panelGroup" in item)
            item.panelGroup = Qt.binding(function() { return panelRoot.panelGroup })
        if ("panelState" in item)
            item.panelState = Qt.binding(function() { return panelRoot.panelState })
        if ("panelContext" in item)
            item.panelContext = Qt.binding(function() { return panelRoot.panelContext })
        if ("contextRouter" in item)
            item.contextRouter = panelRoot.contextRouter
        if ("workspace" in item)
            item.workspace = panelRoot.workspace
        if ("theme" in item)
            item.theme = Qt.binding(function() { return panelRoot.theme })
    }

    function configureHeader() {
        headerTools.source = ""
        headerTools.sourceComponent = null
        if (bodyLoader.item && bodyLoader.item.headerTools !== undefined)
            headerTools.sourceComponent = bodyLoader.item.headerTools
        else if (available && descriptor.headerSource)
            headerTools.setSource(descriptor.headerSource, {theme: panelRoot.theme})
    }

    function loadBody() {
        var nextSource = available ? Qt.resolvedUrl(descriptor.source) : ""
        if (String(bodyLoader.source) !== String(nextSource))
            bodyLoader.setSource(nextSource, nextSource ? {theme: panelRoot.theme} : {})
    }

    onDescriptorChanged: Qt.callLater(loadBody)

    // Coalesce dependent ID/group/state bindings before hydrating the router.
    onPanelIdChanged: Qt.callLater(syncRouter)
    onPanelGroupChanged: Qt.callLater(syncRouter)
    onPanelStateChanged: Qt.callLater(syncRouter)
    onVisibleChanged: {
        if (visible && contextRouter && panelId.length > 0)
            contextRouter.setActivePanel(panelId)
    }
    Component.onCompleted: {
        loadBody()
        Qt.callLater(syncRouter)
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
            if (changedGroup === panelRoot.panelGroup)
                panelRoot.contextRevision++
        }
        function onActivePanelChanged() {
            panelRoot.contextRevision++
        }
    }

    // Observe panel presses, then reject the event so the real control,
    // graph gesture, or dock drag underneath remains the input owner.
    MouseArea {
        anchors.fill: parent
        z: 1000
        acceptedButtons: Qt.LeftButton
        propagateComposedEvents: true
        onPressed: function(mouse) {
            if (panelRoot.contextRouter && panelRoot.panelId.length > 0)
                panelRoot.contextRouter.setActivePanel(panelRoot.panelId)
            mouse.accepted = false
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        Rectangle {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: bodyLoader.item
                                     && bodyLoader.item.headerPreferredHeight !== undefined
                                     ? Math.max(bodyLoader.item.headerPreferredHeight,
                                                headerTools.item ? headerTools.item.implicitHeight + 4 : 0)
                                     : 36
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
                    // Reserve the fixed group badge and the row gap so a long
                    // title can never push the A-E selector out of the header.
                    Layout.maximumWidth: Math.max(0, header.width
                                                  - groupButton.implicitWidth - 2
                                                  - (headerTools.visible && !headerTools.fillHeader ? headerTools.implicitWidth : 0) - 8)
                    text: panelRoot.title
                    background: Rectangle {
                        radius: panelRoot.theme ? panelRoot.theme.smallRadius : 4
                        color: typeButton.hovered
                               ? (panelRoot.theme ? panelRoot.theme.hover : "#343940")
                               : "transparent"
                    }
                    contentItem: Text {
                        text: typeButton.text
                        color: panelRoot.theme ? panelRoot.theme.text : "#dce0e6"
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
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

                // Restored prototype group badge: the persisted workspace group
                // is the panel's context key, so it stays visible and selectable
                // from every contextual header. The menu writes through the
                // existing workspace API; the router follows the binding.
                Button {
                    id: groupButton
                    flat: true
                    padding: 6
                    implicitWidth: 26
                    implicitHeight: 24
                    // Never let the layout squeeze the badge out on narrow and
                    // compact headers.
                    Layout.minimumWidth: implicitWidth
                    font.pixelSize: 12
                    text: panelRoot.panelGroup
                    background: Rectangle {
                        radius: panelRoot.theme ? panelRoot.theme.smallRadius : 4
                        color: groupButton.hovered
                               ? (panelRoot.theme ? panelRoot.theme.hover : "#343940")
                               : "transparent"
                    }
                    contentItem: Text {
                        text: groupButton.text
                        color: panelRoot.theme ? panelRoot.theme.muted : "#979ea8"
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: groupMenu.open()
                    Accessible.name: "Panel display group. Opens the group menu."
                    objectName: "panelGroup_" + panelRoot.panelId
                    ToolTip.visible: hovered
                    ToolTip.text: "Panel group"

                    Menu {
                        id: groupMenu
                        objectName: "panelGroupMenu_" + panelRoot.panelId
                        x: 0
                        y: parent.height

                        Repeater {
                            model: ["A", "B", "C", "D", "E"]
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData
                                checkable: true
                                checked: panelRoot.panelGroup === modelData
                                objectName: "panelGroupChoice_" + modelData + "_" + panelRoot.panelId
                                onTriggered: panelRoot.workspace.setGroup(panelRoot.panelId, modelData)
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: !headerTools.fillHeader
                    Layout.fillHeight: true
                }

                Loader {
                    id: headerTools
                    readonly property bool fillHeader: bodyLoader.item
                                                       && bodyLoader.item.headerToolsFillWidth === true
                    Layout.preferredWidth: fillHeader ? 0 : implicitWidth
                    Layout.minimumWidth: 0
                    Layout.fillWidth: fillHeader
                    Layout.fillHeight: true
                    visible: item !== null
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
                onLoaded: {
                    panelRoot.configureLoaded(item)
                    panelRoot.configureHeader()
                }
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
