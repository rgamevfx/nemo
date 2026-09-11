import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Timeline chrome remains QML, while TimelineItem owns the dense ruler and
// source-reference drawing/input surface. The playhead is a presentation
// clock routed through the panel's resolved group; source timing edits still
// use the existing controller command API.
Pane {
    id: timelinePanel
    objectName: "timelinePanel"
    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    readonly property var controller: viewerController
    readonly property int timelineLength: controller.frameCount > 0 ? controller.frameCount : 240
    readonly property string selectedSource: timelineItem.selectedSource
    readonly property var selectedClip: {
        for (var i = 0; i < controller.timelineClips.length; ++i) {
            var candidate = controller.timelineClips[i]
            if (candidate && candidate.source === selectedSource)
                return candidate
        }
        return null
    }
    readonly property string resolvedGroup: panelContext && panelContext.resolvedGroup
                                           ? panelContext.resolvedGroup : panelGroup
    readonly property real routedClock: contextRouter && panelContext && panelContext.timelineClock !== undefined
                                        ? Number(panelContext.timelineClock) : controller.frame
    readonly property bool targetAvailable: !contextRouter
                                            || Boolean(panelContext && panelContext.timelineTarget)
    readonly property string targetStatus: targetAvailable ? "Timeline target: " + (panelContext.timelineTarget || "available")
                                                           : "Timeline target is unavailable"
    padding: 0
    font.pixelSize: 12
    background: Rectangle { color: "#202020" }

    function updateClock(value) {
        if (contextRouter && resolvedGroup.length > 0)
            contextRouter.setGroupContext(resolvedGroup, {timelineClock: Math.round(value)})
        controller.setFrame(Math.round(value))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 70
            color: "#303030"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 5
                spacing: 3
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        objectName: "timelineState"
                        font.pixelSize: 12
                        text: "Frame " + timelinePanel.routedClock + " · " + controller.renderState
                              + " · " + timelinePanel.targetStatus
                        color: timelinePanel.targetAvailable
                               ? (controller.outdated ? "#e7ba76" : "#c8d3df") : "#efb0b0"
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Button {
                        implicitHeight: 26
                        objectName: "timelineUndo"
                        text: "Undo"
                        enabled: controller.canUndo
                        onClicked: controller.undo()
                    }
                    Button {
                        implicitHeight: 26
                        objectName: "timelineRedo"
                        text: "Redo"
                        enabled: controller.canRedo
                        onClicked: controller.redo()
                    }
                    Button {
                        implicitHeight: 26
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
                        implicitHeight: 26
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
                        implicitHeight: 26
                        id: rangeLast
                        objectName: "timelineRangeLast"
                        from: 0
                        to: Math.max(0, timelinePanel.timelineLength - 1)
                        value: Math.min(47, Math.max(0, timelinePanel.timelineLength - 1))
                        editable: true
                        implicitWidth: 78
                    }
                    Button {
                        implicitHeight: 26
                        objectName: "timelineCacheRange"
                        text: "Cache requested range"
                        onClicked: controller.requestRange(rangeFirst.value, rangeLast.value)
                    }
                    Text {
                        objectName: "timelineSchedulerCounts"
                        text: "queued " + controller.queued + " · dropped " + controller.dropped
                              + " · stale " + controller.staleRejected + " · request completions " + controller.completed
                              + " · cache queued " + controller.cacheQueued + " · cache dropped " + controller.cacheDropped
                              + " · stored " + controller.cachePublished + " · cache errors " + controller.cacheErrors
                        color: "#8e9cab"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Flickable {
            id: timelineScroll
            objectName: "timelineSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 24
            clip: true
            contentWidth: timelineItem.width
            contentHeight: timelineItem.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            TimelineItem {
                id: timelineItem
                objectName: "timelineRuler"
                width: Math.max(timelineScroll.width - 8, 600)
                height: implicitHeight
                clips: controller.timelineClips
                frame: timelinePanel.routedClock
                frameCount: controller.frameCount
                viewportY: timelineScroll.contentY
                viewportHeight: timelineScroll.height
                onFrameSelected: function(frameValue) { timelinePanel.updateClock(frameValue) }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: "#292929"
            border.color: "#414b55"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 5
                spacing: 3
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        objectName: "timelineSelectedSource"
                        font.pixelSize: 12
                        text: selectedSource.length > 0 ? "Selected source: " + selectedSource : "No source selected"
                        color: "#e5e9ed"
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        objectName: "timelineSelectedCoverage"
                        text: selectedClip ? (selectedClip.end > selectedClip.start ? "coverage [" + selectedClip.start + "–" + selectedClip.end + ")" : "coverage unknown") : ""
                        color: "#a9b6c3"
                        font.pixelSize: 11
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        text: selectedClip ? ("source frame " + (selectedClip.sourceFrame >= 0 ? selectedClip.sourceFrame : "unknown")
                                               + " · offset " + selectedClip.offset + " · step " + selectedClip.step) : "Open a source in the Viewer to create an authored source strip."
                        color: "#a9b6c3"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Button {
                        objectName: "timelineSlipMinus"
                        text: "Slip −1"
                        enabled: selectedSource.length > 0
                        implicitHeight: 26
                        onClicked: controller.slipTimelineClip(selectedSource, -1)
                    }
                    Button {
                        objectName: "timelineSlipPlus"
                        text: "Slip +1"
                        enabled: selectedSource.length > 0
                        implicitHeight: 26
                        onClicked: controller.slipTimelineClip(selectedSource, 1)
                    }
                    Button {
                        objectName: "timelineRetime1"
                        text: "Rate 1"
                        enabled: selectedSource.length > 0
                        implicitHeight: 26
                        onClicked: controller.retimeTimelineClip(selectedSource, 1)
                    }
                    Button {
                        objectName: "timelineRetime2"
                        text: "Rate 2"
                        enabled: selectedSource.length > 0
                        implicitHeight: 26
                        onClicked: controller.retimeTimelineClip(selectedSource, 2)
                    }
                    Button {
                        objectName: "timelineRetimeReverse"
                        text: "Reverse"
                        enabled: selectedSource.length > 0
                        implicitHeight: 26
                        onClicked: controller.retimeTimelineClip(selectedSource, -1)
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            color: "#252525"
            Text {
                objectName: "timelineStatus"
                anchors.fill: parent
                anchors.margins: 5
                text: controller.cacheError.length > 0 ? controller.cacheError
                      : (controller.error.length > 0 ? controller.error : timelinePanel.targetStatus)
                color: controller.cacheError.length > 0 || controller.error.length > 0 || !timelinePanel.targetAvailable
                       ? "#efb0b0" : "#8f9aa4"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }
}
