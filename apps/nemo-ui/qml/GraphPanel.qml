import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Graph presentation over the authored Document. Nodes and edges come from
// ViewerController's query properties; every mutation goes through a command
// invokable so the same undo/redo and revision path serves UI and automation.
Pane {
    id: graphPanel
    objectName: "graphPanel"
    padding: 0
    font.pixelSize: 12
    background: Rectangle { color: "#202020" }
    readonly property var controller: viewerController

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flickable {
            id: toolbar
            Layout.fillWidth: true
            Layout.preferredHeight: 174
            contentWidth: Math.max(width, toolbarRows.implicitWidth)
            contentHeight: toolbarRows.implicitHeight
            clip: true
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout {
                id: toolbarRows
                width: toolbar.contentWidth
                height: toolbarRows.implicitHeight
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: nodeType
                        objectName: "graphAddType"
                        Layout.preferredWidth: 150
                        model: controller.nodeCatalog
                        textRole: "displayName"
                        valueRole: "type"
                    }
                    TextField {
                        id: nodeName
                        objectName: "graphAddName"
                        Layout.fillWidth: true
                        placeholderText: "New node name"
                        selectByMouse: true
                    }
                    Button {
                        objectName: "graphAddButton"
                        text: "Add node"
                        enabled: nodeType.currentIndex >= 0
                        onClicked: {
                            controller.addGraphNode(nodeType.currentValue, nodeName.text.trim())
                            nodeName.clear()
                        }
                    }
                    Button {
                        objectName: "graphUndo"
                        text: "Undo"
                        enabled: controller.canUndo
                        onClicked: controller.undo()
                    }
                    Button {
                        objectName: "graphRedo"
                        text: "Redo"
                        enabled: controller.canRedo
                        onClicked: controller.redo()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "View"; color: "#bdbdbd" }
                    ComboBox {
                        objectName: "graphOutput"
                        Layout.preferredWidth: 130
                        model: controller.outputNames
                        currentIndex: Math.max(0, controller.outputNames.indexOf(controller.outputName))
                        onActivated: controller.setOutputName(currentText)
                    }
                    Slider {
                        id: graphPlayhead
                        objectName: "graphPlayhead"
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, controller.frameCount > 0 ? controller.frameCount - 1 : 239)
                        stepSize: 1
                        snapMode: Slider.SnapAlways
                        value: controller.frame
                        onMoved: controller.setFrame(Math.round(value))
                    }
                    Text { objectName: "graphFrame"; text: controller.frame; color: "#d0d0d0" }
                    Text {
                        objectName: "graphState"
                        text: controller.renderState
                        color: controller.outdated ? "#e7ba76" : "#a8b5c5"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: fromNode
                        objectName: "graphFromNode"
                        Layout.fillWidth: true
                        model: controller.graphNodes
                        textRole: "name"
                    }
                    SpinBox { id: fromPort; objectName: "graphFromPort"; from: 0; to: 15; Layout.preferredWidth: 66 }
                    Text { text: "→"; color: "#8f8f8f" }
                    ComboBox {
                        id: toNode
                        objectName: "graphToNode"
                        Layout.fillWidth: true
                        model: controller.graphNodes
                        textRole: "name"
                    }
                    SpinBox { id: toPort; objectName: "graphToPort"; from: 0; to: 15; Layout.preferredWidth: 66 }
                    Button {
                        objectName: "graphConnectButton"
                        text: "Connect"
                        enabled: fromNode.currentIndex >= 0 && toNode.currentIndex >= 0
                        onClicked: {
                            var from = controller.graphNodes[fromNode.currentIndex]
                            var to = controller.graphNodes[toNode.currentIndex]
                            if (from && to)
                                controller.connectGraphNodes(from.id, fromPort.value, to.id, toPort.value)
                        }
                    }
                }
                RowLayout {
                    id: parameterEditor
                    Layout.fillWidth: true
                    property var selectedNode: parameterNode.currentIndex >= 0
                                                     ? controller.graphNodes[parameterNode.currentIndex]
                                                     : null
                    property var selectedParameter: {
                        if (!selectedNode)
                            return null
                        for (var descriptorIndex = 0; descriptorIndex < controller.nodeCatalog.length; ++descriptorIndex) {
                            var descriptor = controller.nodeCatalog[descriptorIndex]
                            if (descriptor.type !== selectedNode.type)
                                continue
                            for (var parameterIndex = 0; parameterIndex < descriptor.parameters.length; ++parameterIndex) {
                                var candidate = descriptor.parameters[parameterIndex]
                                if (candidate.name === parameterKey.text.trim())
                                    return candidate
                            }
                        }
                        return null
                    }
                    ComboBox {
                        id: parameterNode
                        objectName: "graphParameterNode"
                        Layout.preferredWidth: 110
                        model: controller.graphNodes
                        textRole: "name"
                    }
                    TextField {
                        id: parameterKey
                        objectName: "graphParameterKey"
                        Layout.preferredWidth: 100
                        placeholderText: "Parameter key"
                        selectByMouse: true
                    }
                    TextField {
                        id: parameterValue
                        objectName: "graphParameterValue"
                        Layout.fillWidth: true
                        placeholderText: parameterEditor.selectedParameter ? parameterEditor.selectedParameter.type + " value" : "Value"
                        selectByMouse: true
                    }
                    Button {
                        objectName: "graphParameterApply"
                        text: "Apply"
                        enabled: parameterNode.currentIndex >= 0 && parameterKey.text.trim().length > 0
                        onClicked: {
                            var node = controller.graphNodes[parameterNode.currentIndex]
                            if (node)
                                controller.setNodeParameterText(node.id, parameterKey.text.trim(), parameterValue.text)
                        }
                    }
                    Button {
                        objectName: "graphParameterReset"
                        text: "Reset"
                        enabled: parameterNode.currentIndex >= 0 && parameterKey.text.trim().length > 0
                        onClicked: {
                            var node = controller.graphNodes[parameterNode.currentIndex]
                            if (node)
                                controller.resetNodeParameter(node.id, parameterKey.text.trim())
                        }
                    }
                }
            }
        }

        Flickable {
            id: graphScroll
            objectName: "graphSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: graphItem.width
            contentHeight: graphItem.height
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            // The custom item is the only scene-graph content for the dense
            // graph. It receives model snapshots on the GUI thread, caches
            // layout records, and draws only the visible records from
            // updatePaintNode. Flickable supplies the viewport and scrolling;
            // visibleRect remains in the item's content coordinates.
            GraphItem {
                id: graphItem
                objectName: "graphItem"
                width: Math.max(graphScroll.width, implicitWidth)
                height: Math.max(graphScroll.height, implicitHeight)
                nodes: controller.graphNodes
                edges: controller.graphEdges
                visibleRect: Qt.rect(graphScroll.contentX, graphScroll.contentY,
                                     graphScroll.width, graphScroll.height)
            }
        }
    }
}
