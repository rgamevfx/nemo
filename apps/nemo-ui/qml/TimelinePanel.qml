import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Timeline view of actual Document source references. The current model has no
// persistent clip-occurrence placement/range, so this surface deliberately
// edits only source timing (slip and integer retime) through commands. The
// playhead is shared with ViewerPanel and never launches render-ahead.
Rectangle {
    id: timelinePanel
    objectName: "timelinePanel"
    color: "#202020"
    readonly property var controller: viewerController
    readonly property int timelineLength: controller.frameCount > 0 ? controller.frameCount : 240

    function scrubAt(x, width) {
        if (width <= 0)
            return
        var value = Math.round(Math.max(0, Math.min(1, x / width)) * (timelineLength - 1))
        controller.setFrame(value)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 78
            color: "#303030"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 5
                spacing: 3
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        objectName: "timelineState"
                        text: "Frame " + controller.frame + " · " + controller.renderState
                        color: controller.outdated ? "#e7ba76" : "#c8d3df"
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        objectName: "timelineUndo"
                        text: "Undo"
                        enabled: controller.canUndo
                        onClicked: controller.undo()
                    }
                    Button {
                        objectName: "timelineRedo"
                        text: "Redo"
                        enabled: controller.canRedo
                        onClicked: controller.redo()
                    }
                    Button {
                        objectName: "timelineCancel"
                        text: "Cancel"
                        enabled: controller.pending || controller.queued > 0
                        onClicked: controller.cancelRender()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text { text: "Cache range"; color: "#aeaeae" }
                    SpinBox {
                        id: rangeFirst
                        objectName: "timelineRangeFirst"
                        from: 0
                        to: Math.max(0, timelinePanel.timelineLength - 1)
                        value: 0
                        editable: true
                        implicitWidth: 78
                    }
                    Text { text: "–"; color: "#858585" }
                    SpinBox {
                        id: rangeLast
                        objectName: "timelineRangeLast"
                        from: 0
                        to: Math.max(0, timelinePanel.timelineLength - 1)
                        value: Math.min(47, Math.max(0, timelinePanel.timelineLength - 1))
                        editable: true
                        implicitWidth: 78
                    }
                    Button {
                        objectName: "timelineCacheRange"
                        text: "Cache requested range"
                        onClicked: controller.requestRange(rangeFirst.value, rangeLast.value)
                    }
                    Text {
                        objectName: "timelineSchedulerCounts"
                        text: "queued " + controller.queued + " · dropped " + controller.dropped + " · stale " + controller.staleRejected + " · done " + controller.completed
                        color: "#8e9cab"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#272727"
            Text {
                anchors.fill: parent
                anchors.margins: 5
                verticalAlignment: Text.AlignVCenter
                text: "Source strips · slip/retime edit source timing; parent clip move/trim is not represented by this document model"
                color: "#9da7b0"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }

        Flickable {
            id: timelineScroll
            objectName: "timelineSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: Math.max(width, timelineColumn.implicitWidth)
            contentHeight: Math.max(height, timelineColumn.implicitHeight)
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            Column {
                id: timelineColumn
                width: Math.max(timelineScroll.width - 8, 600)
                spacing: 7
                padding: 7

                Rectangle {
                    id: ruler
                    objectName: "timelineRuler"
                    width: timelineColumn.width - 14
                    height: 22
                    color: "#303030"
                    MouseArea {
                        anchors.fill: parent
                        onClicked: function(mouse) { timelinePanel.scrubAt(mouse.x, width) }
                        onPositionChanged: function(mouse) { if (pressed) timelinePanel.scrubAt(mouse.x, width) }
                    }
                    Repeater {
                        model: 9
                        delegate: Text {
                            x: (index / 8) * (ruler.width - width)
                            y: 3
                            text: Math.round(index * (timelinePanel.timelineLength - 1) / 8)
                            color: "#8d969f"
                            font.pixelSize: 10
                        }
                    }
                    Rectangle {
                        objectName: "timelinePlayhead"
                        x: timelinePanel.timelineLength > 1 ? (controller.frame / (timelinePanel.timelineLength - 1)) * parent.width - width / 2 : 0
                        y: 0
                        width: 2
                        height: parent.height
                        color: "#e6b35e"
                    }
                }

                Repeater {
                    id: clips
                    model: controller.timelineClips
                    delegate: Rectangle {
                        id: clip
                        objectName: "timelineClip_" + modelData.id
                        width: timelineColumn.width - 14
                        height: 84
                        color: "#353d47"
                        border.color: "#596a7c"
                        radius: 3
                        property var clipData: modelData

                        MouseArea {
                            anchors.fill: parent
                            z: 0
                            onClicked: function(mouse) { timelinePanel.scrubAt(mouse.x, width) }
                            onPositionChanged: function(mouse) { if (pressed) timelinePanel.scrubAt(mouse.x, width) }
                        }
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 3
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    objectName: "timelineClipLabel_" + clipData.id
                                    text: clipData.source + (clipData.end > 0 ? "   source coverage [0–" + clipData.end + ")" : "   source coverage unknown")
                                    color: "#e8edf2"
                                    font.bold: true
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: "source frame " + clipData.sourceFrame
                                    color: "#a9b6c3"
                                    font.pixelSize: 11
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 18
                                color: "#4c6b88"
                                border.color: "#7698b8"
                                Text {
                                    anchors.centerIn: parent
                                    text: "source " + clipData.source + "  offset " + clipData.offset + "  step " + clipData.step
                                    color: "#e9f0f7"
                                    font.pixelSize: 10
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Button {
                                    objectName: "timelineSlipMinus_" + clipData.id
                                    text: "Slip −1"
                                    implicitHeight: 23
                                    onClicked: controller.slipTimelineClip(clipData.source, -1)
                                }
                                Button {
                                    objectName: "timelineSlipPlus_" + clipData.id
                                    text: "Slip +1"
                                    implicitHeight: 23
                                    onClicked: controller.slipTimelineClip(clipData.source, 1)
                                }
                                Button {
                                    objectName: "timelineRetime1_" + clipData.id
                                    text: "Rate 1"
                                    implicitHeight: 23
                                    onClicked: controller.retimeTimelineClip(clipData.source, 1)
                                }
                                Button {
                                    objectName: "timelineRetime2_" + clipData.id
                                    text: "Rate 2"
                                    implicitHeight: 23
                                    onClicked: controller.retimeTimelineClip(clipData.source, 2)
                                }
                                Button {
                                    objectName: "timelineRetimeReverse_" + clipData.id
                                    text: "Reverse"
                                    implicitHeight: 23
                                    onClicked: controller.retimeTimelineClip(clipData.source, -1)
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }

                Text {
                    objectName: "timelineEmptyState"
                    visible: controller.timelineClips.length === 0
                    text: "Open a source in the Viewer to create an authored source strip."
                    color: "#8b8b8b"
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: "#252525"
            Text {
                anchors.fill: parent
                anchors.margins: 5
                text: controller.error.length > 0 ? controller.error : controller.status
                color: controller.error.length > 0 ? "#efb0b0" : "#8f9aa4"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }
}
