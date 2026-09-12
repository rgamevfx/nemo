import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Native viewer presentation. Each panel owns a ViewerController (and its
// scheduler destination) plus a ViewerItem that presents that destination's
// retained frames; this file arranges their existing operations in the compact
// viewer presentation.
FocusScope {
    id: viewerPanel

    objectName: "viewerPanel"
    focus: true
    activeFocusOnTab: true
    clip: true

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var workspace: null
    property var theme: null

    // Each panel instance owns its controller and therefore its scheduler
    // destination and retained presentation. Harnesses that install only the
    // shared controller keep using it, as does a panel without an id.
    readonly property var controller: {
        if (panelId.length > 0 && typeof viewerControllers !== "undefined" && viewerControllers) {
            var panelController = viewerControllers.controller(panelId)
            if (panelController)
                return panelController
        }
        return viewerController
    }
    // Closing the panel retires its destination together with its controller.
    Component.onDestruction: {
        if (panelId.length > 0 && typeof viewerControllers !== "undefined" && viewerControllers)
            viewerControllers.release(panelId)
    }
    // Panel-local viewer selector. Index addresses the network's Viewer
    // nodes in ascending NodeId order; this panel's controller renders one.
    readonly property int viewerIndex: panelState && panelState.viewerIndex !== undefined
                                       ? Math.max(0, Math.round(Number(panelState.viewerIndex))) : 0
    // Re-evaluated on every graph change: a Q_INVOKABLE used directly in a
    // binding would otherwise never refresh when a Viewer node is created or
    // deleted, because its only other dependency (the root id) is constant.
    property int graphRevision: 0
    readonly property int viewerTotal: {
        graphRevision
        return controller.viewerCount(controller.rootNetworkId)
    }
    // Display math reads the composition canvas, not the media-only source
    // size: a media-free graph still has a 1920x1080 domain to fit.
    readonly property real compositionWidth: controller.compositionSize.width
    readonly property real compositionHeight: controller.compositionSize.height
    readonly property bool hasImage: compositionWidth > 0 && compositionHeight > 0
    readonly property real sourceWidth: hasImage ? compositionWidth : 0
    readonly property real sourceHeight: hasImage ? compositionHeight : 0
    // Display settings are panel-local. They deliberately do not use the
    // controller's render state, so two panels may pan/zoom separately.
    readonly property real displayZoom: panelState && panelState.zoom !== undefined ? Number(panelState.zoom) : 1
    readonly property point displayPan: Qt.point(panelState && panelState.panX !== undefined ? Number(panelState.panX) : 0,
                                                panelState && panelState.panY !== undefined ? Number(panelState.panY) : 0)
    readonly property string zoomMode: panelState && panelState.zoomMode ? panelState.zoomMode : "Fit"
    readonly property string viewerRole: panelContext && panelContext.viewerRole
                                        ? panelContext.viewerRole : "graph"
    readonly property string resolvedGroup: panelGroup
    readonly property bool graphRole: viewerRole === "graph"
    // The graph role follows this panel's active Viewer node attachment; the
    // router carries no graph target, so the panel controller owns it.
    readonly property string graphTargetId: String(controller.viewerTargetId || "")
    readonly property string graphTargetName: controller.viewerTargetName || ""
    readonly property real routedClock: graphRole
                                        ? controller.frame
                                        : viewerRole === "timeline"
                                          ? Number(panelContext.timelineClock || 0)
                                          : Number(panelContext.sourceClock || 0)
    readonly property string targetId: graphRole ? graphTargetId
                                       : viewerRole === "timeline" ? String(panelContext.timelineTarget || "")
                                                                   : String(panelContext.sourceTarget || "")
    readonly property bool targetAvailable: graphRole ? graphTargetId.length > 0
                                                      : panelContext && panelContext.available === true
    readonly property string targetName: graphRole
                                        ? (graphTargetId.length > 0
                                           ? (graphTargetName.length > 0 ? graphTargetName : graphTargetId)
                                           : "No Viewer target")
                                        : targetAvailable && targetId.length > 0
                                          ? targetId
                                          : viewerRole === "timeline" ? "No timeline target" : "No media source"
    readonly property int firstFrame: 0
    readonly property int lastFrame: controller.frameCount > 0 ? controller.frameCount - 1 : 239
    readonly property int currentFrame: Math.max(firstFrame, Math.min(lastFrame, Math.round(routedClock)))
    // Marks are destination-scoped on the panel controller; the panel only
    // clamps them into the displayed frame domain for painting and hit tests.
    readonly property int markInFrame: boundedFrame(controller.inFrame, firstFrame)
    readonly property int markOutFrame: boundedFrame(controller.outFrame, lastFrame)
    readonly property int markedRangeFirst: Math.min(markInFrame, markOutFrame)
    readonly property int markedRangeLast: Math.max(markInFrame, markOutFrame)
    // Transport and display commands only target a panel that owns a
    // destination and has a usable target; an unavailable target must never
    // borrow another panel's render.
    readonly property bool transportReady: targetAvailable && controller.hasDestination === true
    readonly property string timecodeText: {
        var value = controller.timecode
        return value !== undefined && value !== null && String(value).length > 0
                ? String(value) : "--:--:--:--"
    }
    property bool headerToolsFillWidth: true
    readonly property int headerPreferredHeight: 32


    property Component displayControlsComponent: Component {
        Flow {
            id: displayControls
            objectName: "viewerDisplayControls"
            spacing: 4
            flow: Flow.LeftToRight

            StudioComboBox {
                id: layerSelector
                objectName: "viewerLayer_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                model: ["rgb"]
                // Only the real RGB layer is offered. The depth layer is a
                // reported capability boundary, not a fabricated option.
                currentIndex: Math.max(0, model.indexOf(viewerPanel.controller.layer))
                onActivated: viewerPanel.controller.setLayer(currentText)
                ToolTip.visible: hovered
                ToolTip.text: "Depth layer is unavailable in the native viewer."
                Accessible.name: "Image layer"
            }

            StudioComboBox {
                id: channelSelector
                objectName: "viewerChannel_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                model: ["RGBA", "R", "G", "B", "A"]
                currentIndex: Math.max(0, model.indexOf(viewerPanel.controller.channel))
                onActivated: viewerPanel.controller.setChannel(currentText)
                ToolTip.visible: hovered
                ToolTip.text: "Display channel."
                Accessible.name: "Display channels"
            }

            StudioComboBox {
                id: zoomSelector
                objectName: "viewerZoomMenu_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                model: ["Fit", "100%", "50%"]
                currentIndex: Math.max(0, model.indexOf(viewerPanel.zoomMode))
                onActivated: viewerPanel.setZoom(currentText)
                Accessible.name: "Viewer zoom"
            }

            StudioComboBox {
                id: resolutionSelector
                objectName: "viewerResolution_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                model: ["Auto", "Full", "Half", "Quarter"]
                currentIndex: viewerPanel.controller.resolutionMode === "full" ? 1
                              : viewerPanel.controller.resolutionMode === "half" ? 2
                              : viewerPanel.controller.resolutionMode === "quarter" ? 3 : 0
                onActivated: viewerPanel.controller.setResolutionMode(currentText.toLowerCase())
                ToolTip.visible: hovered
                ToolTip.text: "Sampling resolution."
                Accessible.name: "Proxy resolution"
            }
        }
    }

    property Component headerTools: Component {
        Item {
            id: viewerHeaderTools
            implicitHeight: Math.max(24, headerDisplayControls.implicitHeight)
            implicitWidth: headerDisplayControls.implicitWidth
                           + (viewerSelector.visible ? viewerSelector.width + 4 : 0)
                           + targetButton.implicitWidth + 4

            RowLayout {
                anchors.fill: parent
                spacing: 4

                Loader {
                    id: headerDisplayControls
                    Layout.fillWidth: true
                    sourceComponent: viewerPanel.width >= 560 ? viewerPanel.displayControlsComponent : null
                }

                StudioComboBox {
                    id: viewerSelector
                    objectName: "viewerSelector_" + viewerPanel.panelId
                    theme: viewerPanel.theme
                    Layout.alignment: Qt.AlignVCenter
                    width: 84
                    height: 24
                    visible: viewerPanel.viewerTotal > 1
                    model: {
                        var items = []
                        for (var index = 0; index < viewerPanel.viewerTotal; ++index)
                            items.push("Viewer " + (index + 1))
                        return items
                    }
                    currentIndex: Math.max(0, Math.min(viewerPanel.viewerTotal - 1, viewerPanel.viewerIndex))
                    onActivated: viewerPanel.selectViewer(currentIndex)
                    Accessible.name: "Viewer node"
                    ToolTip.visible: hovered
                    ToolTip.text: "Active Viewer node rendered by the shared viewer."
                }

                ChromeButton {
                    id: targetButton
                    objectName: "viewerTargetMenu_" + viewerPanel.panelId
                    Layout.preferredWidth: Math.min(190, Math.max(80, targetMetrics.width + 24))
                    Layout.minimumWidth: 80
                    Layout.maximumWidth: 190
                    Layout.alignment: Qt.AlignVCenter
                    theme: viewerPanel.theme
                    padding: 4
                    text: viewerPanel.targetName
                    Accessible.name: "Viewer target"
                    onClicked: targetMenu.open()
                    implicitHeight: 24
                    contentItem: Row {
                        spacing: 3
                        Text {
                            text: targetButton.text
                            color: viewerPanel.theme ? viewerPanel.theme.text : "#dce0e6"
                            font.pixelSize: viewerPanel.theme ? viewerPanel.theme.fontSize : 11
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            width: Math.max(0, targetButton.width - 19)
                        }
                        Text {
                            text: "⌄"
                            color: viewerPanel.theme ? viewerPanel.theme.muted : "#979ea8"
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Menu {
                        id: targetMenu
                        objectName: "viewerTargetChoices_" + viewerPanel.panelId
                        x: 0
                        y: targetButton.height
                        MenuItem {
                            objectName: "viewerRole_" + viewerPanel.panelId
                            text: "Node Graph — " + (viewerPanel.graphTargetName.length > 0
                                                       ? viewerPanel.graphTargetName : "No Viewer target")
                            checkable: true
                            checked: viewerPanel.viewerRole === "graph"
                            onTriggered: viewerPanel.setRole("graph")
                        }
                        MenuItem {
                            objectName: "viewerRoleTimeline_" + viewerPanel.panelId
                            text: "Timeline — " + (viewerPanel.panelContext && viewerPanel.panelContext.timelineTarget
                                                    ? viewerPanel.panelContext.timelineTarget : "Unavailable")
                            checkable: true
                            checked: viewerPanel.viewerRole === "timeline"
                            onTriggered: viewerPanel.setRole("timeline")
                        }
                        MenuItem {
                            objectName: "viewerRoleMedia_" + viewerPanel.panelId
                            text: "Media Bin — " + (viewerPanel.panelContext && viewerPanel.panelContext.sourceTarget
                                                   ? viewerPanel.panelContext.sourceTarget : "Unavailable")
                            checkable: true
                            checked: viewerPanel.viewerRole === "media"
                            onTriggered: viewerPanel.setRole("media")
                        }
                    }
                }
            }

            TextMetrics {
                id: targetMetrics
                text: targetButton.text
                font: targetButton.font
            }
        }
    }




    Rectangle {
        anchors.fill: parent
        color: viewerPanel.theme ? viewerPanel.theme.panel : "#1e2023"
    }

    function themeColor(role, fallback) {
        return viewerPanel.theme && viewerPanel.theme[role] !== undefined
                ? viewerPanel.theme[role] : fallback
    }

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
        if (role === "graph")
            activateViewer()
    }

    // Each panel owns its controller, so this panel's selected Viewer node is
    // the render target with no shared-controller contention.
    function activateViewer() {
        if (!controller)
            return
        controller.setActiveViewer(controller.rootNetworkId, viewerIndex)
        if (viewer)
            viewer.makePrimary()
    }

    function selectViewer(index) {
        var bounded = Math.max(0, Math.round(Number(index)))
        if (bounded === viewerIndex) {
            activateViewer()
            return
        }
        saveState({viewerIndex: bounded})
        activateViewer()
    }

    function boundedFrame(value, fallback) {
        var bounded = Number(value)
        if (!isFinite(bounded))
            return fallback
        return Math.max(firstFrame, Math.min(lastFrame, Math.round(bounded)))
    }

    // The graph role reads this panel's controller frame directly; media and
    // timeline clocks are group-routed presentation state, so transport keeps
    // them in step with the frame the destination actually renders.
    function publishClock(frame) {
        if (graphRole || !contextRouter || resolvedGroup.length === 0)
            return
        var change = {}
        change[viewerRole === "media" ? "sourceClock" : "timelineClock"] = frame
        contextRouter.setGroupContext(resolvedGroup, change)
    }

    function updateClock(value) {
        var bounded = Math.max(firstFrame, Math.min(lastFrame, Math.round(value)))
        publishClock(bounded)
        controller.setFrame(bounded)
    }

    // Forward this panel's resolved context to its own destination.
    property var forwardedContext: ({role: "", target: "", clock: NaN})
    function forwardContext() {
        if (!controller)
            return
        if (forwardedContext.role === viewerRole && forwardedContext.target === targetId
                && forwardedContext.clock === routedClock)
            return
        forwardedContext = {role: viewerRole, target: targetId, clock: routedClock}
        controller.setViewerContext(viewerRole, targetId, routedClock)
    }

    // Transport is panel-local: every call lands on this panel's destination.
    function stepFrames(delta) {
        controller.stepBy(delta)
        publishClock(controller.frame)
    }

    function seekToMark(mark) {
        if (mark === "in")
            controller.seekToIn()
        else
            controller.seekToOut()
        publishClock(controller.frame)
    }

    function runTransport(action) {
        if (!transportReady)
            return
        if (action === "markIn")
            controller.setMarkIn()
        else if (action === "markOut")
            controller.setMarkOut()
        else if (action === "start")
            seekToMark("in")
        else if (action === "end")
            seekToMark("out")
        else if (action === "previous")
            stepFrames(-1)
        else if (action === "next")
            stepFrames(1)
        else if (action === "play")
            controller.togglePlay()
        else if (action === "stop") {
            controller.stop()
            publishClock(controller.frame)
        }
    }


    function displayScale() {
        if (sourceWidth <= 0 || sourceHeight <= 0 || viewer.width <= 0 || viewer.height <= 0)
            return 1
        return Math.min(Math.max(1, viewer.width - 12) / (sourceWidth * controller.pixelAspect),
                        Math.max(1, viewer.height - 12) / sourceHeight)
    }

    function imageScale() {
        if (zoomMode === "100%") return 1
        if (zoomMode === "50%") return 0.5
        return displayScale() * displayZoom
    }

    function computeDisplayRect() {
        if (!hasImage)
            return Qt.rect(0, 0, 0, 0)
        var scale = imageScale()
        var sx = scale * controller.pixelAspect
        var visibleW = Math.min(sourceWidth, viewer.width / sx)
        var visibleH = Math.min(sourceHeight, viewer.height / scale)
        var cx = Math.max(visibleW / 2, Math.min(sourceWidth - visibleW / 2,
                                                 sourceWidth / 2 + displayPan.x))
        var cy = Math.max(visibleH / 2, Math.min(sourceHeight - visibleH / 2,
                                                 sourceHeight / 2 + displayPan.y))
        var region = controller.presentedRegion
        return Qt.rect(viewer.width / 2 + (region.x - cx) * sx,
                       viewer.height / 2 + (region.y - cy) * scale,
                       region.width * sx, region.height * scale)
    }

    function setZoom(mode) {
        var nextZoom = 1
        if (mode === "100%")
            nextZoom = 1 / displayScale()
        else if (mode === "50%")
            nextZoom = 0.5 / displayScale()
        saveState({zoom: Math.max(0.05, Math.min(32, nextZoom)), panX: 0, panY: 0, zoomMode: mode})
    }


    // The commit writes only the owners (routed group clock + panel
    // controller); the displayed text stays the focused field's own while the
    // edit is active and returns to the binding once focus leaves.
    function commitFrame() {
        var value = Number(frameField.text)
        if (isFinite(value))
            viewerPanel.updateClock(value)
    }

    function commitTimecode() {
        var value = viewerPanel.controller.frameForTimecode(timecodeField.text)
        if (isFinite(value))
            viewerPanel.updateClock(value)
    }


    function drawGlyph(ctx, glyph, width, height, enabled) {
        ctx.reset()
        var colour = enabled ? themeColor("text", "#dce0e6") : themeColor("disabled", "#5f6670")
        ctx.strokeStyle = colour
        ctx.fillStyle = colour
        ctx.lineWidth = 1.5
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        var w = width, h = height, mid = h / 2
        function triangle(direction) {
            ctx.beginPath()
            if (direction > 0) {
                ctx.moveTo(w * 0.34, h * 0.2); ctx.lineTo(w * 0.76, mid); ctx.lineTo(w * 0.34, h * 0.8)
            } else {
                ctx.moveTo(w * 0.66, h * 0.2); ctx.lineTo(w * 0.24, mid); ctx.lineTo(w * 0.66, h * 0.8)
            }
            ctx.closePath(); ctx.fill()
        }
        if (glyph === "play") {
            triangle(1)
        } else if (glyph === "pause") {
            ctx.fillRect(w * 0.26, h * 0.18, w * 0.18, h * 0.64)
            ctx.fillRect(w * 0.56, h * 0.18, w * 0.18, h * 0.64)
        } else if (glyph === "stop") {
            ctx.fillRect(w * 0.25, h * 0.24, w * 0.5, h * 0.52)
        } else if (glyph === "previous") {
            ctx.beginPath(); ctx.moveTo(w * 0.72, h * 0.2); ctx.lineTo(w * 0.34, mid); ctx.lineTo(w * 0.72, h * 0.8); ctx.stroke()
            ctx.beginPath(); ctx.moveTo(w * 0.2, h * 0.18); ctx.lineTo(w * 0.2, h * 0.82); ctx.stroke()
        } else if (glyph === "next") {
            ctx.beginPath(); ctx.moveTo(w * 0.28, h * 0.2); ctx.lineTo(w * 0.66, mid); ctx.lineTo(w * 0.28, h * 0.8); ctx.stroke()
            ctx.beginPath(); ctx.moveTo(w * 0.8, h * 0.18); ctx.lineTo(w * 0.8, h * 0.82); ctx.stroke()
        } else if (glyph === "start") {
            ctx.beginPath(); ctx.moveTo(w * 0.72, h * 0.2); ctx.lineTo(w * 0.34, mid); ctx.lineTo(w * 0.72, h * 0.8); ctx.closePath(); ctx.fill()
            ctx.beginPath(); ctx.moveTo(w * 0.19, h * 0.17); ctx.lineTo(w * 0.19, h * 0.83); ctx.stroke()
        } else if (glyph === "end") {
            ctx.beginPath(); ctx.moveTo(w * 0.28, h * 0.2); ctx.lineTo(w * 0.66, mid); ctx.lineTo(w * 0.28, h * 0.8); ctx.closePath(); ctx.fill()
            ctx.beginPath(); ctx.moveTo(w * 0.81, h * 0.17); ctx.lineTo(w * 0.81, h * 0.83); ctx.stroke()
        } else if (glyph === "markIn") {
            ctx.beginPath(); ctx.moveTo(w * 0.3, h * 0.2); ctx.lineTo(w * 0.3, h * 0.8); ctx.lineTo(w * 0.7, h * 0.8); ctx.stroke()
            ctx.beginPath(); ctx.moveTo(w * 0.44, h * 0.32); ctx.lineTo(w * 0.7, h * 0.32); ctx.stroke()
        } else if (glyph === "markOut") {
            ctx.beginPath(); ctx.moveTo(w * 0.7, h * 0.2); ctx.lineTo(w * 0.7, h * 0.8); ctx.lineTo(w * 0.3, h * 0.8); ctx.stroke()
            ctx.beginPath(); ctx.moveTo(w * 0.56, h * 0.32); ctx.lineTo(w * 0.3, h * 0.32); ctx.stroke()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0


        Item {
            id: imageArea
            objectName: "viewerImageArea"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 40
            clip: true

            Rectangle {
                anchors.fill: parent
                color: viewerPanel.theme ? viewerPanel.theme.imageSurround : "#17191b"
            }

            ViewerItem {
                id: viewer
                objectName: "viewerItem_" + viewerPanel.panelId
                anchors.fill: parent
                clip: true
                controller: viewerPanel.controller
                displayRect: viewerPanel.computeDisplayRect()
                visible: viewerPanel.targetAvailable
            }

            Text {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - 32)
                visible: !viewerPanel.targetAvailable
                objectName: "viewerUnavailable_" + viewerPanel.panelId
                text: viewerPanel.targetName
                color: viewerPanel.theme ? viewerPanel.theme.muted : "#979ea8"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            MouseArea {
                id: panArea
                anchors.fill: parent
                cursorShape: viewerPanel.imageScale() > viewerPanel.displayScale() ? Qt.OpenHandCursor : Qt.ArrowCursor
                enabled: viewerPanel.targetAvailable
                property real lastX: 0
                property real lastY: 0
                onWheel: function(wheel) {
                    viewer.makePrimary()
                    var relativeZoom = viewerPanel.imageScale() / viewerPanel.displayScale()
                    var nextZoom = Math.max(0.05, Math.min(32,
                        relativeZoom * Math.pow(1.15, wheel.angleDelta.y / 120)))
                    viewerPanel.saveState({zoom: nextZoom, zoomMode: "Custom"})
                    wheel.accepted = true
                }
                onPressed: function(mouse) {
                    lastX = mouse.x
                    lastY = mouse.y
                    forceActiveFocus()
                }
                onPositionChanged: function(mouse) {
                    if (!pressed || viewerPanel.imageScale() <= viewerPanel.displayScale())
                        return
                    var scale = viewerPanel.imageScale()
                    var dx = (mouse.x - lastX) / scale
                    var dy = (mouse.y - lastY) / scale
                    lastX = mouse.x
                    lastY = mouse.y
                    viewerPanel.saveState({panX: viewerPanel.displayPan.x - dx, panY: viewerPanel.displayPan.y - dy})
                }
                onClicked: viewer.makePrimary()
            }
        }
        Loader {
            id: belowDisplayControls
            objectName: "belowDisplayControls"
            visible: viewerPanel.width < 560
            Layout.fillWidth: true
            Layout.preferredHeight: visible && item ? item.implicitHeight + 8 : 0
            Layout.minimumHeight: 0
            sourceComponent: visible ? viewerPanel.displayControlsComponent : null
        }



        // Compact FrameRuler port. Clicking seeks the routed clock; dragging an
        // in/out bracket edits the panel controller's marks with the prototype's
        // inclusive range semantics.
        Item {
            id: frameRuler
            objectName: "viewerPlayhead_" + viewerPanel.panelId
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            clip: true
            property int frame: viewerPanel.currentFrame
            property int firstFrame: viewerPanel.firstFrame
            property int lastFrame: viewerPanel.lastFrame
            property int inFrame: viewerPanel.markedRangeFirst
            property int outFrame: viewerPanel.markedRangeLast
            property bool dragging: false
            property string dragMode: ""
            property int previewFrame: 0
            property int previewValue: 0
            readonly property int displayFrame: Math.max(firstFrame, Math.min(lastFrame,
                dragging && dragMode === "seek" ? previewFrame : frame))
            readonly property int visualInFrame: dragging && dragMode === "in"
                                                 ? Math.min(outFrame, previewValue) : inFrame
            readonly property int visualOutFrame: dragging && dragMode === "out"
                                                  ? Math.max(inFrame, previewValue) : outFrame

            function handleRadius() { return Math.max(7, Math.min(11, width * 0.12)) }

            function plotLeft() { return width >= 32 ? 8 : 2 }
            function plotRight() { return width >= 32 ? Math.max(plotLeft(), width - 8) : Math.max(plotLeft(), width - 2) }
            function xForFrame(value) {
                var span = lastFrame - firstFrame
                if (span <= 0)
                    return (plotLeft() + plotRight()) / 2
                var bounded = Math.max(firstFrame, Math.min(lastFrame, value))
                return plotLeft() + (bounded - firstFrame) * (plotRight() - plotLeft()) / span
            }
            function frameForX(value) {
                var span = lastFrame - firstFrame
                if (span <= 0 || plotRight() <= plotLeft())
                    return firstFrame
                var ratio = Math.max(0, Math.min(1, (value - plotLeft()) / (plotRight() - plotLeft())))
                return Math.max(firstFrame, Math.min(lastFrame, Math.round(firstFrame + ratio * span)))
            }
            function tickStep() {
                var span = Math.max(1, lastFrame - firstFrame)
                var usable = Math.max(1, plotRight() - plotLeft())
                var raw = span / Math.max(1, Math.floor(usable / 30))
                var power = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10))
                var multiples = [1, 2, 5, 10]
                for (var i = 0; i < multiples.length; ++i) {
                    var candidate = multiples[i] * power
                    if (candidate * usable / span >= 30)
                        return Math.max(1, candidate)
                }
                return Math.max(1, 10 * power)
            }

            Rectangle { anchors.fill: parent; color: viewerPanel.theme ? viewerPanel.theme.panel : "transparent" }
            Canvas {
                id: rulerCanvas
                anchors.fill: parent
                antialiasing: true
                renderTarget: Canvas.FramebufferObject
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    var left = frameRuler.plotLeft(), right = frameRuler.plotRight()
                    var baseY = Math.min(22, Math.max(15, height - 11))
                    var stripY = Math.max(6, baseY - 5)
                    var labelY = Math.min(height - 2, baseY + 10)
                    var span = frameRuler.lastFrame - frameRuler.firstFrame
                    var major = frameRuler.tickStep()
                    ctx.strokeStyle = viewerPanel.themeColor("border", "#30343a")
                    ctx.lineWidth = 1
                    ctx.beginPath(); ctx.moveTo(left, baseY + 0.5); ctx.lineTo(right, baseY + 0.5); ctx.stroke()
                    var inX = frameRuler.xForFrame(frameRuler.visualInFrame)
                    var outX = frameRuler.xForFrame(frameRuler.visualOutFrame)
                    ctx.fillStyle = viewerPanel.themeColor("accent", "#3485f6")
                    ctx.fillRect(inX, stripY, Math.max(2, outX - inX), 7)
                    var firstMajor = span > 0 ? Math.ceil(frameRuler.firstFrame / major) * major : frameRuler.firstFrame
                    var labels = []
                    if (span <= 0) labels.push(frameRuler.firstFrame)
                    else {
                        for (var majorFrame = firstMajor; majorFrame <= frameRuler.lastFrame; majorFrame += major)
                            labels.push(majorFrame)
                        if (labels.length === 0 || labels[0] !== frameRuler.firstFrame) labels.unshift(frameRuler.firstFrame)
                        if (labels[labels.length - 1] !== frameRuler.lastFrame) labels.push(frameRuler.lastFrame)
                    }
                    ctx.font = "9px sans-serif"
                    ctx.textBaseline = "alphabetic"
                    ctx.strokeStyle = viewerPanel.themeColor("muted", "#979ea8")
                    var previousLabelRight = -Infinity
                    for (var index = 0; index < labels.length; ++index) {
                        var label = String(labels[index]), labelWidth = ctx.measureText(label).width
                        var tickX = frameRuler.xForFrame(labels[index])
                        ctx.beginPath(); ctx.moveTo(tickX + 0.5, baseY - 10); ctx.lineTo(tickX + 0.5, baseY + 0.5); ctx.stroke()
                        var labelLeft = Math.max(1, Math.min(width - labelWidth - 1, tickX - labelWidth / 2))
                        if (width >= labelWidth + 3 && labelLeft >= previousLabelRight + 3) {
                            ctx.fillStyle = viewerPanel.themeColor("text", "#dce0e6")
                            ctx.fillText(label, labelLeft, labelY)
                            previousLabelRight = labelLeft + labelWidth
                        }
                    }
                    // In/out brackets stay visible even when labels are suppressed.
                    ctx.strokeStyle = viewerPanel.themeColor("muted", "#979ea8")
                    ctx.fillStyle = viewerPanel.themeColor("muted", "#979ea8")
                    ctx.lineWidth = 1
                    var endpoints = [inX, outX]
                    for (var endpoint = 0; endpoint < endpoints.length; ++endpoint) {
                        var endpointX = endpoints[endpoint]
                        ctx.beginPath(); ctx.moveTo(endpointX + 0.5, stripY - 2); ctx.lineTo(endpointX + 0.5, Math.min(height - 5, baseY + 9)); ctx.stroke()
                        ctx.beginPath()
                        if (endpoint === 0) {
                            ctx.moveTo(endpointX, Math.min(height - 4, baseY + 10)); ctx.lineTo(endpointX + 5, Math.min(height - 9, baseY + 5)); ctx.lineTo(endpointX, Math.min(height - 9, baseY + 5))
                        } else {
                            ctx.moveTo(endpointX, Math.min(height - 4, baseY + 10)); ctx.lineTo(endpointX - 5, Math.min(height - 9, baseY + 5)); ctx.lineTo(endpointX, Math.min(height - 9, baseY + 5))
                        }
                        ctx.closePath(); ctx.fill()
                    }
                    var playheadX = frameRuler.xForFrame(frameRuler.displayFrame)
                    ctx.strokeStyle = viewerPanel.themeColor("accent", "#3485f6")
                    ctx.lineWidth = 1.5
                    ctx.beginPath(); ctx.moveTo(playheadX + 0.5, 0); ctx.lineTo(playheadX + 0.5, Math.min(height - 4, baseY + 10)); ctx.stroke()
                    ctx.fillStyle = viewerPanel.themeColor("accent", "#3485f6")
                    ctx.beginPath(); ctx.moveTo(playheadX, Math.min(height - 4, baseY + 10)); ctx.lineTo(playheadX - 4, Math.min(height - 9, baseY + 5)); ctx.lineTo(playheadX + 4, Math.min(height - 9, baseY + 5)); ctx.closePath(); ctx.fill()
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onEnabledChanged: requestPaint()
                Connections {
                    target: viewerPanel
                    function onThemeChanged() { rulerCanvas.requestPaint() }
                }
                Connections {
                    target: viewerPanel.theme
                    function onPresetChanged() { rulerCanvas.requestPaint() }
                    function onAccentOverrideChanged() { rulerCanvas.requestPaint() }
                }
            }
            MouseArea {
                id: rulerInteraction
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                cursorShape: frameRuler.dragging && frameRuler.dragMode !== "seek"
                             ? Qt.SizeHorCursor : Qt.PointingHandCursor
                onPressed: function(mouse) {
                    viewerPanel.forceActiveFocus()
                    var inX = frameRuler.xForFrame(frameRuler.visualInFrame)
                    var outX = frameRuler.xForFrame(frameRuler.visualOutFrame)
                    var radius = frameRuler.handleRadius()
                    var inHit = viewerPanel.transportReady && Math.abs(mouse.x - inX) <= radius
                    var outHit = viewerPanel.transportReady && Math.abs(mouse.x - outX) <= radius
                    if (inHit || outHit) {
                        if (inHit && outHit)
                            frameRuler.dragMode = mouse.x <= (inX + outX) / 2 ? "in" : "out"
                        else
                            frameRuler.dragMode = inHit ? "in" : "out"
                        frameRuler.previewValue = frameRuler.dragMode === "in" ? frameRuler.visualInFrame
                                                                               : frameRuler.visualOutFrame
                    } else {
                        frameRuler.dragMode = "seek"
                        frameRuler.previewFrame = frameRuler.frameForX(mouse.x)
                        viewerPanel.updateClock(frameRuler.previewFrame)
                    }
                    frameRuler.dragging = true
                    rulerCanvas.requestPaint()
                }
                onPositionChanged: function(mouse) {
                    if (!pressed || !frameRuler.dragging)
                        return
                    var value = frameRuler.frameForX(mouse.x)
                    if (frameRuler.dragMode === "seek") {
                        if (value === frameRuler.previewFrame)
                            return
                        frameRuler.previewFrame = value
                        viewerPanel.updateClock(value)
                    } else if (frameRuler.dragMode === "in") {
                        value = Math.min(value, frameRuler.outFrame)
                        if (value === frameRuler.previewValue)
                            return
                        frameRuler.previewValue = value
                        viewerPanel.controller.setMarkInFrame(value)
                    } else if (frameRuler.dragMode === "out") {
                        value = Math.max(value, frameRuler.inFrame)
                        if (value === frameRuler.previewValue)
                            return
                        frameRuler.previewValue = value
                        viewerPanel.controller.setMarkOutFrame(value)
                    }
                    rulerCanvas.requestPaint()
                }
                onReleased: {
                    frameRuler.dragging = false
                    frameRuler.dragMode = ""
                    rulerCanvas.requestPaint()
                }
                onCanceled: {
                    frameRuler.dragging = false
                    frameRuler.dragMode = ""
                    rulerCanvas.requestPaint()
                }
            }
            onFrameChanged: rulerCanvas.requestPaint()
            onFirstFrameChanged: rulerCanvas.requestPaint()
            onLastFrameChanged: rulerCanvas.requestPaint()
            onInFrameChanged: rulerCanvas.requestPaint()
            onOutFrameChanged: rulerCanvas.requestPaint()
            onDisplayFrameChanged: rulerCanvas.requestPaint()
            onVisualInFrameChanged: rulerCanvas.requestPaint()
            onVisualOutFrameChanged: rulerCanvas.requestPaint()
            onPreviewFrameChanged: rulerCanvas.requestPaint()
            onPreviewValueChanged: rulerCanvas.requestPaint()
            onDraggingChanged: rulerCanvas.requestPaint()
        }

        Rectangle {
            id: transportBar
            objectName: "transportBar"
            Layout.fillWidth: true
            Layout.preferredHeight: compact ? 64 : 36
            readonly property bool compact: width < 530
            color: viewerPanel.theme ? viewerPanel.theme.panel : "#1e2023"
            clip: true

            Row {
                id: transportControls
                objectName: "transportControls"
                x: transportBar.compact ? (parent.width - width) / 2
                                        : (parent.width - (65 + 8 + width + 8 + 119)) / 2 + 65 + 8
                y: 4
                spacing: 3
                Repeater {
                    model: [
                        {name: "inButton", glyph: "markIn", action: "markIn", tooltip: "Mark in at current frame"},
                        {name: "startButton", glyph: "start", action: "start", tooltip: "Go to in point"},
                        {name: "previousButton", glyph: "previous", action: "previous", tooltip: "Previous frame"},
                        {name: "playButton", glyph: "play", action: "play", tooltip: ""},
                        {name: "stopButton", glyph: "stop", action: "stop", tooltip: "Stop and return to in point"},
                        {name: "nextButton", glyph: "next", action: "next", tooltip: "Next frame"},
                        {name: "endButton", glyph: "end", action: "end", tooltip: "Go to out point"},
                        {name: "outButton", glyph: "markOut", action: "markOut", tooltip: "Mark out at current frame"}
                    ]
                    delegate: Button {
                        id: transportButton
                        property var entry: modelData
                        readonly property bool playbackToggle: entry.action === "play"
                        readonly property string glyph: playbackToggle && viewerPanel.controller.playing
                                                         ? "pause" : entry.glyph
                        readonly property string tooltip: playbackToggle
                                                          ? (viewerPanel.controller.playing ? "Pause playback"
                                                                                            : "Play playback")
                                                          : entry.tooltip
                        objectName: entry.name
                        width: 29
                        height: 28
                        padding: 0
                        flat: true
                        enabled: viewerPanel.transportReady
                        Accessible.name: tooltip
                        ToolTip.visible: hovered
                        ToolTip.text: tooltip
                        onGlyphChanged: transportGlyph.requestPaint()
                        onClicked: viewerPanel.runTransport(entry.action)
                        background: Rectangle {
                            radius: viewerPanel.theme ? viewerPanel.theme.smallRadius : 4
                            color: transportButton.pressed ? viewerPanel.themeColor("raised", "#282c31")
                                  : transportButton.hovered ? viewerPanel.themeColor("hover", "#343940") : "transparent"
                            border.width: transportButton.activeFocus
                                          || (transportButton.playbackToggle && viewerPanel.controller.playing) ? 1 : 0
                            border.color: viewerPanel.themeColor("accent", "#3485f6")
                        }
                        contentItem: Canvas {
                            id: transportGlyph
                            anchors.fill: parent
                            anchors.margins: 7
                            onPaint: viewerPanel.drawGlyph(getContext("2d"), transportButton.glyph,
                                                            width, height, transportButton.enabled)
                            onEnabledChanged: requestPaint()
                            Component.onCompleted: requestPaint()
                            Connections {
                                target: viewerPanel
                                function onThemeChanged() { transportGlyph.requestPaint() }
                            }
                            Connections {
                                target: viewerPanel.theme
                                function onPresetChanged() { transportGlyph.requestPaint() }
                                function onAccentOverrideChanged() { transportGlyph.requestPaint() }
                            }
                        }
                    }
                }
            }

            TextField {
                id: frameField
                objectName: "viewerFrame_" + viewerPanel.panelId
                x: transportBar.compact ? transportBar.width / 2 - width - 4 : transportControls.x - width - 8
                y: transportBar.compact ? 34 : 4
                width: 65
                height: 28
                font.family: "Monospace"
                font.pixelSize: 11
                color: viewerPanel.theme.text
                horizontalAlignment: Text.AlignHCenter
                selectByMouse: true
                validator: IntValidator {
                    bottom: viewerPanel.firstFrame
                    top: viewerPanel.lastFrame
                }
                onAccepted: viewerPanel.commitFrame()
                onEditingFinished: viewerPanel.commitFrame()
                background: Rectangle {
                    color: viewerPanel.theme.field
                    radius: viewerPanel.theme.smallRadius
                    border.color: frameField.activeFocus ? viewerPanel.theme.accent : viewerPanel.theme.border
                }
                Accessible.name: "Frame"
            }

            // The frame display is a declarative binding, never an imperative
            // assignment: an assignment from JS would destroy the binding and
            // freeze the field at the value it happened to hold. RestoreNone
            // keeps the displayed value while the binding is suspended by the
            // edit gesture, so focusing the field never clears its text.
            Binding {
                target: frameField
                property: "text"
                value: String(viewerPanel.currentFrame)
                when: !frameField.activeFocus
                restoreMode: Binding.RestoreNone
            }

            TextField {
                id: timecodeField
                objectName: "viewerTimecode_" + viewerPanel.panelId
                x: transportBar.compact ? transportBar.width / 2 + 4 : transportControls.x + transportControls.width + 8
                y: transportBar.compact ? 34 : 4
                width: 119
                height: 28
                horizontalAlignment: Text.AlignHCenter
                font.family: "Monospace"
                font.pixelSize: viewerPanel.theme ? viewerPanel.theme.fontSize : 11
                color: viewerPanel.theme.text
                selectByMouse: true
                onAccepted: viewerPanel.commitTimecode()
                onEditingFinished: viewerPanel.commitTimecode()
                background: Rectangle {
                    color: viewerPanel.theme ? viewerPanel.theme.field : "#24272c"
                    radius: viewerPanel.theme ? viewerPanel.theme.smallRadius : 4
                    border.color: timecodeField.activeFocus ? viewerPanel.theme.accent
                                                            : viewerPanel.themeColor("border", "#30343a")
                }
                Accessible.name: "Timecode"
            }

            // Same rule as the frame display: the timecode is a binding on the
            // panel controller's own clock, so it follows a routed seek in the
            // same dependency order the controller publishes it. A frameChanged
            // handler assigning this text is what pinned a stale timecode while
            // the routed frame field already showed the new frame. RestoreNone
            // preserves the displayed value while the edit gesture suspends the
            // binding.
            Binding {
                target: timecodeField
                property: "text"
                value: viewerPanel.timecodeText
                when: !timecodeField.activeFocus
                restoreMode: Binding.RestoreNone
            }
        }
    }

    // This panel renders its own selected Viewer node; there is no shared
    // render target to contend for.
    Component.onCompleted: Qt.callLater(function() {
        activateViewer()
        forwardContext()
    })
    onViewerIndexChanged: activateViewer()
    onGraphRoleChanged: if (graphRole) activateViewer()
    onVisibleChanged: if (visible && graphRole) activateViewer()

    // Resolved panel context is forwarded to this panel's own destination.
    onPanelContextChanged: forwardContext()
    onViewerRoleChanged: forwardContext()
    onRoutedClockChanged: forwardContext()

    Connections {
        target: viewerPanel.controller
        function onGraphChanged() {
            viewerPanel.graphRevision++
        }
    }

    Keys.onPressed: function(event) {
        if (frameField.activeFocus || timecodeField.activeFocus)
            return
        if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            var index = event.key - Qt.Key_1
            if (index < viewerPanel.viewerTotal) {
                viewerPanel.selectViewer(index)
                event.accepted = true
            }
            return
        }
        if (event.key === Qt.Key_Space) {
            viewerPanel.runTransport("play"); event.accepted = true
        } else if (event.key === Qt.Key_Left) {
            viewerPanel.runTransport("previous"); event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            viewerPanel.runTransport("next"); event.accepted = true
        } else if (event.key === Qt.Key_I) {
            viewerPanel.runTransport("markIn"); event.accepted = true
        } else if (event.key === Qt.Key_O) {
            viewerPanel.runTransport("markOut"); event.accepted = true
        } else if (event.key === Qt.Key_Home) {
            viewerPanel.runTransport("start"); event.accepted = true
        } else if (event.key === Qt.Key_End) {
            viewerPanel.runTransport("end"); event.accepted = true
        }
    }
}
