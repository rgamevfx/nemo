import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// The prototype defines the presentation. Real catalog entries and commands
// come from the production controller; graph gestures remain owned by #44.
FocusScope {
    id: graphPanel
    objectName: "graphPanel"
    focus: true
    clip: true

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var theme
    property var workspace
    property bool toolsOpen: false
    readonly property var controller: viewerController
    property var categories: []
    property var displayNodes: []

    function displayCategory(group) {
        if (group === "I/O") return "IO"
        if (group === "Compositing") return "Merge"
        return theme.nodeCategoryColors[group] !== undefined ? group : "Utility"
    }

    function refreshCatalog() {
        var catalog = controller.nodeCatalog
        var groups = ["Color", "Distort", "Filter", "Utility", "Merge", "IO"].map(
            function(category) { return {label: category, nodes: []} })
        var byType = {}
        for (var i = 0; i < catalog.length; ++i) {
            var descriptor = catalog[i]
            var label = displayCategory(descriptor.group)
            byType[descriptor.type] = descriptor
            var group = null
            for (var j = 0; j < groups.length; ++j)
                if (groups[j].label === label) group = groups[j]
            group.nodes.push(descriptor)
        }
        categories = groups
        var nodes = controller.graphNodes
        var presented = []
        for (var n = 0; n < nodes.length; ++n) {
            var node = nodes[n]
            var schema = byType[node.type]
            presented.push({id: node.id, name: node.name, type: node.type,
                            category: schema ? displayCategory(schema.group) : "Utility",
                            inputs: schema ? schema.inputs.length : 0, outputs: schema ? schema.outputs.length : 0})
        }
        displayNodes = presented
    }

    function createNode(descriptor) {
        var names = controller.graphNodes.map(function(node) { return node.name })
        var base = descriptor.displayName.replace(/\s+/g, "")
        var suffix = 1
        while (names.indexOf(base + suffix) !== -1) ++suffix
        controller.addGraphNode(descriptor.type, base + suffix)
    }

    Connections {
        target: controller
        function onGraphChanged() { graphPanel.refreshCatalog() }
        function onCatalogChanged() { graphPanel.refreshCatalog() }
    }
    Component.onCompleted: refreshCatalog()

    property Component headerTools: Component {
        RowLayout {
            spacing: 4
            ChromeButton {
                objectName: "graphFrameAll"
                theme: graphPanel.theme
                text: "Frame all"
                implicitHeight: 24
                implicitWidth: 67
                padding: 5
                enabled: false
                ToolTip.visible: hovered
                ToolTip.text: "Graph framing and zoom are not available yet (#44)."
            }
            ChromeButton {
                id: toolsButton
                objectName: "graphToolsButton"
                theme: graphPanel.theme
                text: "Tools"
                implicitHeight: 24
                implicitWidth: 49
                padding: 5
                onClicked: graphPanel.toolsOpen = !graphPanel.toolsOpen
                background: Rectangle {
                    radius: 4
                    color: toolsButton.down ? graphPanel.theme.hover
                         : toolsButton.hovered ? graphPanel.theme.raised : graphPanel.theme.panel
                    border.color: graphPanel.toolsOpen ? graphPanel.theme.accent : graphPanel.theme.border
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                objectName: "graphZoomLabel"
                text: "100%"
                color: graphPanel.theme.muted
                font.pixelSize: 11
                verticalAlignment: Text.AlignVCenter
                Layout.preferredWidth: 38
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: graphPanel.theme.background
        objectName: "graphCanvas"

        Rectangle {
            id: toolsBar
            objectName: "graphToolsBar"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: categoryFlow.implicitHeight + 8
            visible: graphPanel.toolsOpen
            z: 40
            color: Qt.rgba(graphPanel.theme.panel.r, graphPanel.theme.panel.g, graphPanel.theme.panel.b, 0.97)
            border.color: graphPanel.theme.border
            Flow {
                id: categoryFlow
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 3
                Repeater {
                    model: graphPanel.categories
                    delegate: ChromeButton {
                        id: categoryButton
                        required property var modelData
                        theme: graphPanel.theme
                        objectName: "toolCategory_" + modelData.label
                        text: modelData.label === "IO" ? "I/O" : modelData.label
                        enabled: modelData.nodes.length > 0
                        implicitHeight: 23
                        implicitWidth: Math.max(48, text.length * 7 + 18)
                        padding: 4
                        onClicked: categoryMenu.open()
                        Menu {
                            id: categoryMenu
                            y: categoryButton.height
                            Repeater {
                                model: categoryButton.modelData.nodes
                                delegate: MenuItem {
                                    required property var modelData
                                    objectName: "toolNode_" + modelData.type
                                    text: modelData.displayName
                                    onTriggered: graphPanel.createNode(modelData)
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            id: breadcrumb
            objectName: "graphBreadcrumbBar"
            anchors.top: parent.top
            anchors.topMargin: toolsBar.visible ? toolsBar.height : 0
            anchors.left: parent.left
            anchors.right: parent.right
            height: 27
            color: Qt.rgba(graphPanel.theme.panel.r, graphPanel.theme.panel.g, graphPanel.theme.panel.b, 0.94)
            border.color: graphPanel.theme.border
            Text {
                anchors.fill: parent
                anchors.leftMargin: 10
                text: "Root"
                color: graphPanel.theme.text
                font.pixelSize: 11
                verticalAlignment: Text.AlignVCenter
            }
        }

        Canvas {
            id: grid
            anchors.fill: graphScroll
            renderTarget: Canvas.FramebufferObject
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var border = graphPanel.theme.border
                ctx.strokeStyle = Qt.rgba(border.r, border.g, border.b, 0.24)
                ctx.lineWidth = 1
                var startX = ((-graphScroll.contentX % 24) + 24) % 24
                var startY = ((-graphScroll.contentY % 24) + 24) % 24
                ctx.beginPath()
                for (var x = startX; x < width; x += 24) {
                    ctx.moveTo(Math.round(x) + 0.5, 0)
                    ctx.lineTo(Math.round(x) + 0.5, height)
                }
                for (var y = startY; y < height; y += 24) {
                    ctx.moveTo(0, Math.round(y) + 0.5)
                    ctx.lineTo(width, Math.round(y) + 0.5)
                }
                ctx.stroke()
            }
            Connections {
                target: graphPanel.theme
                function onBorderChanged() { grid.requestPaint() }
            }
            Connections {
                target: graphScroll
                function onContentXChanged() { grid.requestPaint() }
                function onContentYChanged() { grid.requestPaint() }
            }
        }

        Flickable {
            id: graphScroll
            objectName: "graphSurface"
            anchors.top: breadcrumb.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: graphItem.width
            contentHeight: graphItem.height
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            GraphItem {
                id: graphItem
                objectName: "graphItem"
                width: Math.max(graphScroll.width, implicitWidth)
                height: Math.max(graphScroll.height, implicitHeight)
                nodes: graphPanel.displayNodes
                edges: controller.graphEdges
                categoryColors: graphPanel.theme.nodeCategoryColors
                visibleRect: Qt.rect(graphScroll.contentX, graphScroll.contentY, graphScroll.width, graphScroll.height)
            }
        }

        Text {
            objectName: "graphErrorLabel"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            visible: controller.error.length > 0
            text: controller.error
            color: graphPanel.theme.accent
            font.pixelSize: 11
            elide: Text.ElideRight
            z: 50
        }
    }
}
