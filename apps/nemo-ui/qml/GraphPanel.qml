import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Graph presentation over the authored Document. Nodes and edges come from
// ViewerController's query properties; every mutation goes through a command
// invokable so the same undo/redo and revision path serves UI and automation.
Rectangle {
    id: graphPanel
    objectName: "graphPanel"
    color: "#202020"
    readonly property var controller: viewerController

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flickable {
            id: toolbar
            Layout.fillWidth: true
            Layout.preferredHeight: 164
            contentWidth: Math.max(width, toolbarRows.implicitWidth)
            contentHeight: 154
            clip: true
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout {
                id: toolbarRows
                width: toolbar.contentWidth
                height: 154
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: nodeType
                        objectName: "graphAddType"
                        Layout.preferredWidth: 108
                        model: ["constcolor", "testpattern", "source", "merge", "output"]
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
                        onClicked: {
                            controller.addGraphNode(nodeType.currentText, nodeName.text.trim())
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
                    Layout.fillWidth: true
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
                        placeholderText: "Value"
                        selectByMouse: true
                    }
                    Button {
                        objectName: "graphParameterApply"
                        text: "Apply"
                        enabled: parameterNode.currentIndex >= 0 && parameterKey.text.trim().length > 0
                        onClicked: {
                            var node = controller.graphNodes[parameterNode.currentIndex]
                            if (node)
                                controller.setNodeParameter(node.name, parameterKey.text.trim(), parameterValue.text)
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
            contentWidth: Math.max(width, graphColumn.implicitWidth)
            contentHeight: Math.max(height, graphColumn.implicitHeight)
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            Column {
                id: graphColumn
                width: Math.max(graphScroll.width - 8, 520)
                spacing: 6
                padding: 6

                Repeater {
                    model: controller.graphNodes
                    delegate: Rectangle {
                        objectName: "graphNode_" + modelData.id
                        width: graphColumn.width - 12
                        height: Math.max(76, parameters.implicitHeight + 40)
                        color: "#343434"
                        border.color: "#555555"
                        radius: 3
                        property var nodeData: modelData

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 3
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: nodeData.name
                                    color: "#f0f0f0"
                                    font.bold: true
                                }
                                Text {
                                    text: nodeData.type + "  (#" + nodeData.id + ")"
                                    color: "#909090"
                                    font.pixelSize: 11
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Column {
                                id: parameters
                                Layout.fillWidth: true
                                spacing: 2
                                Repeater {
                                    model: Object.keys(nodeData.params)
                                    delegate: RowLayout {
                                        width: parameters.width
                                        spacing: 4
                                        Text {
                                            text: modelData
                                            color: "#adadad"
                                            Layout.preferredWidth: 88
                                            elide: Text.ElideRight
                                        }
                                        TextField {
                                            objectName: "graphParam_" + nodeData.name + "_" + modelData
                                            text: nodeData.params[modelData]
                                            Layout.fillWidth: true
                                            selectByMouse: true
                                            onEditingFinished: controller.setNodeParameter(nodeData.name, modelData, text)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "graphConnections"
                    width: graphColumn.width - 12
                    height: Math.max(34, edgesColumn.implicitHeight + 12)
                    color: "#292929"
                    border.color: "#484848"
                    Column {
                        id: edgesColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 6
                        spacing: 2
                        Text { text: "Connections"; color: "#d0d0d0"; font.bold: true }
                        Repeater {
                            model: controller.graphEdges
                            delegate: Text {
                                objectName: "graphEdge_" + modelData.id
                                text: "#" + modelData.id + "  " + graphPanel.nodeLabel(modelData.fromNode) + ":" + modelData.fromPort + "  →  " + graphPanel.nodeLabel(modelData.toNode) + ":" + modelData.toPort
                                color: "#a9c4d8"
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }
    }

    function nodeLabel(id) {
        for (var i = 0; i < controller.graphNodes.length; ++i) {
            if (controller.graphNodes[i].id === id)
                return controller.graphNodes[i].name
        }
        return "node " + id
    }
}
