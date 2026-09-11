import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Native viewer presentation. Routing chooses the panel's presentation target
// and source clock; ViewerController remains the existing load/render facade.
Rectangle {
    id: viewerPanel

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var workspace: null
    readonly property var controller: viewerController
    readonly property bool hasImage: controller.hasSource && controller.sourceSize.width > 0
    readonly property real sourceWidth: hasImage ? controller.sourceSize.width : 0
    readonly property real sourceHeight: hasImage ? controller.sourceSize.height : 0
    // These are panel-local display settings. They intentionally do not use
    // ViewerController's shared render state, so two panels can be viewed at
    // different zoom/pan/channel settings.
    property real displayZoom: panelState && panelState.zoom !== undefined ? Number(panelState.zoom) : 1
    property point displayPan: Qt.point(panelState && panelState.panX !== undefined ? Number(panelState.panX) : 0,
                                       panelState && panelState.panY !== undefined ? Number(panelState.panY) : 0)
    property string channel: panelState && panelState.channel ? panelState.channel : "rgba"
    readonly property string viewerRole: panelContext && panelContext.viewerRole
                                        ? panelContext.viewerRole : "graph"
    readonly property string resolvedGroup: panelContext && panelContext.resolvedGroup
                                           ? panelContext.resolvedGroup : panelGroup
    readonly property real routedClock: viewerRole === "graph"
                                        ? Number(panelContext.graphClock || 0)
                                        : viewerRole === "timeline"
                                          ? Number(panelContext.timelineClock || 0)
                                          : Number(panelContext.sourceClock || 0)
    readonly property string targetId: viewerRole === "graph" ? String(panelContext.graphTarget || "")
                                       : viewerRole === "timeline" ? String(panelContext.timelineTarget || "")
                                                                   : String(panelContext.sourceTarget || "")
    readonly property bool targetAvailable: panelContext && panelContext.available === true
    readonly property string targetStatus: targetAvailable
                                          ? viewerRole[0].toUpperCase() + viewerRole.slice(1)
                                            + " target: " + targetId
                                          : (panelContext && panelContext.unavailableReason
                                             ? panelContext.unavailableReason : viewerRole + " target unavailable")

    color: "#1a1a1a"
    objectName: "viewerPanel"

    function saveState(changes) {
        if (!workspace || !panelId.length || !workspace.setPanelState)
            return
        var next = {}
        if (panelState) {
            for (var key in panelState)
                next[key] = panelState[key]
        }
        for (var changed in changes)
            next[changed] = changes[changed]
        workspace.setPanelState(panelId, next)
    }

    function setRole(role) {
        if (!contextRouter || !panelId.length)
            return
        contextRouter.setViewerRole(panelId, role)
    }

    function updateClock(value) {
        if (contextRouter && resolvedGroup.length > 0) {
            var change = {}
            change[viewerRole === "media" ? "sourceClock" : viewerRole + "Clock"] = Math.round(value)
            contextRouter.setGroupContext(resolvedGroup, change)
        }
        // The existing controller remains the single request consumer until
        // issue #47 gives each viewer an independent scheduler destination.
        controller.setFrame(Math.round(value))
    }

    function loadSource(path) {
        var trimmed = path.trim()
        if (!trimmed.length)
            return
        if (contextRouter && resolvedGroup.length > 0) {
            contextRouter.setGroupContext(resolvedGroup, {
                sourceTarget: "src",
                sourceClock: Number(panelContext.sourceClock || 0)
            })
        }
        controller.openSource(trimmed)
    }

    function displayScale() {
        if (sourceWidth <= 0 || sourceHeight <= 0 || viewer.width <= 0 || viewer.height <= 0)
            return 1
        return Math.min(viewer.width / (sourceWidth * controller.pixelAspect), viewer.height / sourceHeight)
    }

    function computeDisplayRect() {
        if (!hasImage)
            return Qt.rect(0, 0, 0, 0)
        var s = displayScale() * displayZoom
        var sx = s * controller.pixelAspect
        var visibleW = Math.min(sourceWidth, viewer.width / sx)
        var visibleH = Math.min(sourceHeight, viewer.height / s)
        var cx = Math.max(visibleW / 2, Math.min(sourceWidth - visibleW / 2,
                                                 sourceWidth / 2 + displayPan.x))
        var cy = Math.max(visibleH / 2, Math.min(sourceHeight - visibleH / 2,
                                                 sourceHeight / 2 + displayPan.y))
        var region = controller.presentedRegion
        return Qt.rect(viewer.width / 2 + (region.x - cx) * sx,
                       viewer.height / 2 + (region.y - cy) * s,
                       region.width * sx, region.height * s)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 86
            color: "#333333"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 3
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: sourceInput
                        objectName: "viewerSourceInput_" + viewerPanel.panelId
                        Layout.fillWidth: true
                        implicitHeight: 26
                        placeholderText: "media path (e.g. /path/to/clip.mkv)"
                        font.pixelSize: 11
                        selectByMouse: true
                    }
                    Button {
                        id: sourceLoad
                        objectName: "viewerSourceLoad_" + viewerPanel.panelId
                        text: "Load"
                        implicitHeight: 24
                        implicitWidth: 48
                        font.pixelSize: 11
                        onClicked: viewerPanel.loadSource(sourceInput.text)
                    }
                    ComboBox {
                        id: roleSelector
                        objectName: "viewerRole_" + viewerPanel.panelId
                        implicitHeight: 24
                        implicitWidth: 102
                        font.pixelSize: 11
                        model: ["Graph", "Timeline", "Media"]
                        currentIndex: viewerPanel.viewerRole === "graph" ? 0
                                      : viewerPanel.viewerRole === "timeline" ? 1 : 2
                        onActivated: viewerPanel.setRole(currentText.toLowerCase())
                        ToolTip.visible: hovered
                        ToolTip.text: "Viewer role / target: Graph, Timeline, or Media. Changing role never loads a source."
                    }
                    ComboBox {
                        id: channelSelector
                        objectName: "viewerChannel_" + viewerPanel.panelId
                        implicitHeight: 24
                        implicitWidth: 78
                        font.pixelSize: 11
                        model: ["RGBA", "Red", "Green", "Blue", "Alpha"]
                        currentIndex: Math.max(0, ["rgba", "red", "green", "blue", "alpha"].indexOf(viewerPanel.channel))
                        onActivated: {
                            viewerPanel.channel = currentText.toLowerCase()
                            viewerPanel.saveState({channel: viewerPanel.channel})
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Independent display channel for this viewer panel."
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        objectName: "viewerTargetStatus_" + viewerPanel.panelId
                        text: viewerPanel.targetStatus
                        color: viewerPanel.targetAvailable ? "#a8d5a8" : "#efb0b0"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        objectName: "viewerRoleState_" + viewerPanel.panelId
                        text: "Role: " + viewerPanel.viewerRole + " · group " + viewerPanel.resolvedGroup
                        color: "#aeb7c2"
                        font.pixelSize: 11
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    SpinBox {
                        id: frameSelector
                        objectName: "viewerFrame_" + viewerPanel.panelId
                        implicitHeight: 24
                        implicitWidth: 96
                        font.pixelSize: 11
                        from: 0
                        to: viewerPanel.controller.frameCount > 0 ? viewerPanel.controller.frameCount - 1 : 9999
                        value: viewerPanel.routedClock
                        onValueModified: viewerPanel.updateClock(value)
                        editable: true
                    }
                    Slider {
                        id: frameScrubber
                        objectName: "viewerPlayhead_" + viewerPanel.panelId
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, viewerPanel.controller.frameCount > 0 ? viewerPanel.controller.frameCount - 1 : 239)
                        value: viewerPanel.routedClock
                        onMoved: viewerPanel.updateClock(value)
                    }
                    ComboBox {
                        id: modeSelector
                        objectName: "viewerMode_" + viewerPanel.panelId
                        implicitHeight: 24
                        implicitWidth: 96
                        font.pixelSize: 11
                        model: ["Auto", "Full", "Half", "Quarter"]
                        currentIndex: viewerPanel.controller.resolutionMode === "full" ? 1
                                      : viewerPanel.controller.resolutionMode === "half" ? 2
                                      : viewerPanel.controller.resolutionMode === "quarter" ? 3 : 0
                        onActivated: viewerPanel.controller.setResolutionMode(currentText.toLowerCase())
                        ToolTip.visible: hovered
                        ToolTip.text: "Sampling resolution: Auto derives a stable level from image area, aspect and zoom; Full/Half/Quarter are explicit reductions."
                    }
                    Text {
                        objectName: "viewerZoomLabel_" + viewerPanel.panelId
                        text: Math.round(viewerPanel.displayZoom * 100) + "%"
                        color: "#9a9a9a"
                        font.pixelSize: 11
                    }
                    Button {
                        id: resetView
                        objectName: "viewerResetView_" + viewerPanel.panelId
                        text: "Fit"
                        implicitHeight: 24
                        implicitWidth: 40
                        font.pixelSize: 11
                        onClicked: {
                            viewerPanel.displayZoom = 1
                            viewerPanel.displayPan = Qt.point(0, 0)
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Reset this panel's display zoom and pan."
                    }
                    Button {
                        objectName: "viewerCancel_" + viewerPanel.panelId
                        text: "Cancel"
                        enabled: viewerPanel.controller.pending || viewerPanel.controller.queued > 0
                        onClicked: viewerPanel.controller.cancelRender()
                    }
                }
            }
        }

        Item {
            id: surface
            Layout.fillWidth: true
            Layout.fillHeight: true

            ViewerItem {
                id: viewer
                objectName: "viewerItem_" + viewerPanel.panelId
                anchors.fill: parent
                clip: true
                controller: viewerPanel.controller
                displayRect: viewerPanel.computeDisplayRect()
            }

            MouseArea {
                id: panArea
                anchors.fill: parent
                cursorShape: viewerPanel.displayZoom > 1 ? Qt.OpenHandCursor : Qt.ArrowCursor
                property real lastX: 0
                property real lastY: 0
                onWheel: function(wheel) {
                    viewer.makePrimary()
                    viewerPanel.displayZoom = Math.max(1, Math.min(32,
                        viewerPanel.displayZoom * Math.pow(1.15, wheel.angleDelta.y / 120)))
                    wheel.accepted = true
                }
                onPressed: function(mouse) {
                    lastX = mouse.x
                    lastY = mouse.y
                    forceActiveFocus()
                }
                onPositionChanged: function(mouse) {
                    if (!pressed || viewerPanel.displayZoom <= 1)
                        return
                    var s = viewerPanel.displayScale() * viewerPanel.displayZoom
                    var dx = (mouse.x - lastX) / s
                    var dy = (mouse.y - lastY) / s
                    lastX = mouse.x
                    lastY = mouse.y
                    viewerPanel.displayPan = Qt.point(viewerPanel.displayPan.x - dx,
                                                       viewerPanel.displayPan.y - dy)
                }
                onClicked: viewer.makePrimary()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: "#242424"
            Text {
                id: statusText
                objectName: "viewerStatus_" + viewerPanel.panelId
                anchors.fill: parent
                anchors.margins: 4
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                text: "[" + viewerPanel.controller.renderState + "] "
                      + (viewerPanel.controller.error.length > 0 ? viewerPanel.controller.error
                         : (viewerPanel.controller.status.length > 0 ? viewerPanel.controller.status
                            : viewerPanel.targetStatus))
                color: viewerPanel.controller.error.length > 0 || !viewerPanel.targetAvailable
                       ? "#f0b0b0" : viewerPanel.controller.outdated ? "#e7ba76" : "#8a8a8a"
                font.pixelSize: 11
            }
        }
    }
}
