import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A node of the recursive binary split tree. A "split" node renders a
// SplitView holding two child WorkspaceNodes; a "tabs" (leaf) node renders a
// compact tab strip (only when it has more than one panel) plus the active
// panel. Preferred sizes are derived from the model ratio in a one-shot
// manner (no binding) so a handle drag never fights the model: while dragging,
// SplitView owns the local geometry; on drag completion the completed ratio is
// persisted through workspace.setRatio.
Item {
    id: rootNode

    property var node
    property var workspace
    property var drag            // the DockDrag coordinator
    property var theme

    readonly property bool isValid: node !== undefined && node !== null
                                    && node.kind !== undefined && node.kind !== ""
    readonly property bool isSplit: isValid && node.kind === "split"
    readonly property real minimumPaneWidth: isSplit && inner.item ? inner.item.minimumPaneWidth : 120
    readonly property real minimumPaneHeight: isSplit && inner.item ? inner.item.minimumPaneHeight : 80

    Loader {
        id: inner
        anchors.fill: parent
        sourceComponent: rootNode.isValid ? (rootNode.isSplit ? splitComponent
                                                              : leafComponent) : null
    }

    Component {
        id: splitComponent
        SplitView {
            id: splitView

            anchors.fill: parent
            orientation: rootNode.node.orientation === "vertical" ? Qt.Vertical : Qt.Horizontal
            property bool wasResizing: false
            readonly property real minimumPaneWidth: orientation === Qt.Horizontal
                ? firstNode.SplitView.minimumWidth + secondNode.SplitView.minimumWidth + 4
                : Math.max(firstNode.SplitView.minimumWidth, secondNode.SplitView.minimumWidth)
            readonly property real minimumPaneHeight: orientation === Qt.Vertical
                ? firstNode.SplitView.minimumHeight + secondNode.SplitView.minimumHeight + 4
                : Math.max(firstNode.SplitView.minimumHeight, secondNode.SplitView.minimumHeight)

            // Re-derive pane sizes whenever the underlying node map changes
            // (e.g. after a structural rebuild), so a saved ratio is restored
            // without recreating the whole tree.
            property var boundNode: rootNode.node
            onBoundNodeChanged: splitView.applyRatio()

            handle: Rectangle {
                implicitWidth: 4
                implicitHeight: 4
                color: SplitHandle.pressed
                       ? (rootNode.theme ? rootNode.theme.accent : "#4a6fa5")
                       : (SplitHandle.hovered ? (rootNode.theme ? rootNode.theme.border : "#464646")
                                               : (rootNode.theme ? rootNode.theme.border : "#383838"))
            }

            Loader {
                id: firstNode
                source: "WorkspaceNode.qml"
                onLoaded: {
                    item.node = Qt.binding(function() { return rootNode.node.children[0] })
                    item.workspace = Qt.binding(function() { return rootNode.workspace })
                    item.drag = Qt.binding(function() { return rootNode.drag })
                    item.theme = Qt.binding(function() { return rootNode.theme })
                }
                SplitView.minimumWidth: item ? item.minimumPaneWidth : 120
                SplitView.minimumHeight: item ? item.minimumPaneHeight : 80
            }
            Loader {
                id: secondNode
                source: "WorkspaceNode.qml"
                onLoaded: {
                    item.node = Qt.binding(function() { return rootNode.node.children[1] })
                    item.workspace = Qt.binding(function() { return rootNode.workspace })
                    item.drag = Qt.binding(function() { return rootNode.drag })
                    item.theme = Qt.binding(function() { return rootNode.theme })
                }
                SplitView.minimumWidth: item ? item.minimumPaneWidth : 120
                SplitView.minimumHeight: item ? item.minimumPaneHeight : 80
            }

            // Derive the pane sizes from the saved ratio. Only called when the
            // split view is not being dragged, so the drag keeps its local
            // geometry; a window resize still restores the ratio because the
            // model ratio is (re)applied here.
            function applyRatio() {
                if (splitView.resizing)
                    return
                var ratio = typeof rootNode.node.ratio === "number" ? rootNode.node.ratio : 0.5
                var handle = 4
                if (splitView.orientation === Qt.Horizontal) {
                    var w = splitView.width - handle
                    if (w <= 0)
                        return
                    firstNode.SplitView.preferredWidth = ratio * w
                    secondNode.SplitView.preferredWidth = (1 - ratio) * w
                } else {
                    var h = splitView.height - handle
                    if (h <= 0)
                        return
                    firstNode.SplitView.preferredHeight = ratio * h
                    secondNode.SplitView.preferredHeight = (1 - ratio) * h
                }
            }

            function currentRatio() {
                var def = typeof rootNode.node.ratio === "number" ? rootNode.node.ratio : 0.5
                if (splitView.orientation === Qt.Horizontal) {
                    var total = firstNode.width + secondNode.width
                    return total > 0 ? firstNode.width / total : def
                } else {
                    var totalH = firstNode.height + secondNode.height
                    return totalH > 0 ? firstNode.height / totalH : def
                }
            }

            onResizingChanged: {
                // Only persist once a real drag is complete, never during
                // initialization (which would clobber the saved ratio).
                if (splitView.wasResizing && !splitView.resizing) {
                    var r = splitView.currentRatio()
                    rootNode.workspace.setRatio(rootNode.node.id, r)
                    rootNode.node["ratio"] = r
                }
                splitView.wasResizing = splitView.resizing
            }

            onWidthChanged: {
                if (splitView.orientation === Qt.Horizontal)
                    splitView.applyRatio()
            }
            onHeightChanged: {
                if (splitView.orientation === Qt.Vertical)
                    splitView.applyRatio()
            }
            Component.onCompleted: splitView.applyRatio()
        }
    }

    Component {
        id: leafComponent
        Rectangle {
            id: leaf
            anchors.fill: parent
            color: rootNode.theme ? rootNode.theme.panel : "#2b2b2b"
            objectName: "leaf_" + leaf.nodeId
            readonly property real minimumPaneWidth: 120
            readonly property real minimumPaneHeight: 80

            readonly property var node: rootNode.node
            readonly property var workspace: rootNode.workspace
            readonly property var drag: rootNode.drag
            readonly property var theme: rootNode.theme
            readonly property string nodeId: node ? node.id : ""
            readonly property int panelCount: node.panels ? node.panels.length : 0
            readonly property Item tabStrip: leaf.panelCount > 1 ? tabRow : null
            readonly property var tabButtons: tabItems
            property string _registeredId: ""

            function registerCurrentLeaf() {
                if (leaf.drag) {
                    if (_registeredId)
                        leaf.drag.unregisterLeaf(_registeredId, leaf)
                    _registeredId = leaf.nodeId
                    leaf.drag.registerLeaf(_registeredId, leaf)
                }
            }
            onDragChanged: registerCurrentLeaf()
            onNodeIdChanged: registerCurrentLeaf()
            Component.onCompleted: registerCurrentLeaf()
            Component.onDestruction: {
                if (leaf.drag && _registeredId)
                    leaf.drag.unregisterLeaf(_registeredId, leaf)
            }

            readonly property var activePanel: {
                var panels = node.panels
                var result = null
                if (panels && panels.length > 0) {
                    for (var i = 0; i < panels.length; i++) {
                        if (panels[i].id === node.active) {
                            result = panels[i]
                            break
                        }
                    }
                    if (result === null)
                        result = panels[0]
                }
                result
            }

            function panelTitle(typeId) {
                var descriptor = leaf.workspace ? leaf.workspace.panelDescriptor(typeId) : ({})
                return descriptor && descriptor.title ? descriptor.title : typeId
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Tab controls only when there is more than one panel.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: leaf.panelCount > 1 ? 26 : 0
                    visible: leaf.panelCount > 1
                    color: leaf.theme ? leaf.theme.tabs : "#262626"
                    RowLayout {
                        id: tabRow
                        anchors.fill: parent
                        anchors.margins: 2
                        spacing: 2
                        Repeater {
                            id: tabItems
                            model: leaf.node.panels
                            delegate: Button {
                                id: tabButton
                                flat: true
                                objectName: "panelTab_" + modelData.id
                                contentItem: Text {
                                    text: leaf.panelTitle(modelData.type)
                                    color: modelData.id === leaf.node.active
                                           ? (leaf.theme ? leaf.theme.text : "#eaeaea")
                                           : (leaf.theme ? leaf.theme.mutedText : "#8a8a8a")
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    radius: 3
                                    color: modelData.id === leaf.node.active
                                           ? (leaf.theme ? leaf.theme.header : "#3f3f3f")
                                           : "transparent"
                                }
                                onClicked: leaf.workspace.activate(leaf.node.id, modelData.id)
                                Accessible.name: "Panel tab for " + leaf.panelTitle(modelData.type)
                                implicitHeight: 22
                                padding: 8

                                PanelDragHandler {
                                    panelId: modelData.id
                                    leafId: leaf.node.id
                                    panelType: modelData.type
                                    drag: leaf.drag
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }

                Panel {
                    visible: leaf.activePanel !== null
                    panel: leaf.activePanel
                    leafId: leaf.node.id
                    workspace: leaf.workspace
                    drag: leaf.drag
                    theme: leaf.theme
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }
        }
    }
}
