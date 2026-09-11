import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Native viewer presentation. The ViewerController and ViewerItem remain the
// production source/render path; this file only arranges their existing
// operations in the compact viewer presentation.
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

    readonly property var controller: viewerController
    readonly property bool hasImage: controller.hasSource && controller.sourceSize.width > 0
    readonly property real sourceWidth: hasImage ? controller.sourceSize.width : 0
    readonly property real sourceHeight: hasImage ? controller.sourceSize.height : 0
    // Display settings are panel-local. They deliberately do not use the
    // controller's shared render state, so two panels may pan/zoom separately.
    readonly property real displayZoom: panelState && panelState.zoom !== undefined ? Number(panelState.zoom) : 1
    readonly property point displayPan: Qt.point(panelState && panelState.panX !== undefined ? Number(panelState.panX) : 0,
                                                panelState && panelState.panY !== undefined ? Number(panelState.panY) : 0)
    readonly property string zoomMode: panelState && panelState.zoomMode ? panelState.zoomMode : "Fit"
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
    readonly property string targetName: targetAvailable && targetId.length > 0
                                        ? targetId
                                        : viewerRole === "graph" ? "No node graph target"
                                          : viewerRole === "media" ? "No media source" : "Timeline"
    readonly property int firstFrame: 0
    readonly property int lastFrame: controller.frameCount > 0 ? controller.frameCount - 1 : 239
    readonly property int currentFrame: Math.max(firstFrame, Math.min(lastFrame, Math.round(routedClock)))
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
                currentIndex: 0
                enabled: false
                ToolTip.visible: hovered
                ToolTip.text: "Image layer selection is unavailable in the native viewer."
                Accessible.name: "Image layer (unavailable)"
            }

            StudioComboBox {
                id: channelSelector
                objectName: "viewerChannel_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                model: ["RGBA", "R", "G", "B", "A"]
                currentIndex: 0
                ToolTip.visible: hovered
                ToolTip.text: "Display channel selection is unavailable until viewer runtime #47."
                Accessible.name: "Display channels (unavailable)"
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
            implicitWidth: headerDisplayControls.implicitWidth + targetButton.implicitWidth + 4

            RowLayout {
                anchors.fill: parent
                spacing: 4

                Loader {
                    id: headerDisplayControls
                    Layout.fillWidth: true
                    sourceComponent: viewerPanel.width >= 560 ? viewerPanel.displayControlsComponent : null
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
                            text: "Node Graph — " + (viewerPanel.panelContext && viewerPanel.panelContext.graphTarget
                                                       ? viewerPanel.panelContext.graphTarget : "Unavailable")
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
    }

    function updateClock(value) {
        var bounded = Math.max(firstFrame, Math.min(lastFrame, Math.round(value)))
        if (contextRouter && resolvedGroup.length > 0) {
            var change = {}
            change[viewerRole === "media" ? "sourceClock" : viewerRole + "Clock"] = bounded
            contextRouter.setGroupContext(resolvedGroup, change)
        }
        // The controller remains the single request consumer until issue #47
        // supplies independent viewer scheduler destinations.
        controller.setFrame(bounded)
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


    function commitFrame() {
        var value = Number(frameField.text)
        if (isFinite(value))
            viewerPanel.updateClock(value)
        frameField.text = String(viewerPanel.currentFrame)
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



        // Compact FrameRuler port. Production currently owns a single routed
        // clock; full-range endpoints are shown while mark editing remains an
        // honest #47 capability boundary rather than a local fake state model.
        Item {
            id: frameRuler
            objectName: "viewerPlayhead_" + viewerPanel.panelId
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            clip: true
            property int frame: viewerPanel.currentFrame
            property int firstFrame: viewerPanel.firstFrame
            property int lastFrame: viewerPanel.lastFrame

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
                    ctx.fillStyle = viewerPanel.themeColor("accent", "#3485f6")
                    ctx.fillRect(left, stripY, Math.max(2, right - left), 7)
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
                    var playheadX = frameRuler.xForFrame(frameRuler.frame)
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
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                cursorShape: Qt.PointingHandCursor
                onPressed: function(mouse) { viewerPanel.updateClock(frameRuler.frameForX(mouse.x)) }
                onPositionChanged: function(mouse) { if (pressed) viewerPanel.updateClock(frameRuler.frameForX(mouse.x)) }
            }
            onFrameChanged: rulerCanvas.requestPaint()
            onFirstFrameChanged: rulerCanvas.requestPaint()
            onLastFrameChanged: rulerCanvas.requestPaint()
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
                        {name: "inButton", glyph: "markIn", action: "markIn", supported: false},
                        {name: "startButton", glyph: "start", action: "start", supported: true},
                        {name: "previousButton", glyph: "previous", action: "previous", supported: true},
                        {name: "playButton", glyph: "play", action: "play", supported: false},
                        {name: "stopButton", glyph: "stop", action: "stop", supported: false},
                        {name: "nextButton", glyph: "next", action: "next", supported: true},
                        {name: "endButton", glyph: "end", action: "end", supported: true},
                        {name: "outButton", glyph: "markOut", action: "markOut", supported: false}
                    ]
                    delegate: Button {
                        id: transportButton
                        property var entry: modelData
                        objectName: entry.name
                        width: 29
                        height: 28
                        padding: 0
                        flat: true
                        enabled: entry.supported && viewerPanel.controller.hasSource
                                 && (entry.action !== "end" || viewerPanel.controller.frameCount > 0)
                        Accessible.name: entry.supported ? entry.name : entry.name + " unavailable until viewer runtime #47"
                        ToolTip.visible: hovered
                        ToolTip.text: entry.supported ? entry.name : "Viewer runtime control is owned by issue #47"
                        onClicked: {
                            if (entry.action === "start")
                                viewerPanel.updateClock(viewerPanel.firstFrame)
                            else if (entry.action === "previous")
                                viewerPanel.updateClock(viewerPanel.currentFrame - 1)
                            else if (entry.action === "next")
                                viewerPanel.updateClock(viewerPanel.currentFrame + 1)
                            else if (entry.action === "end")
                                viewerPanel.updateClock(viewerPanel.lastFrame)
                        }
                        background: Rectangle {
                            radius: viewerPanel.theme ? viewerPanel.theme.smallRadius : 4
                            color: transportButton.pressed ? viewerPanel.themeColor("raised", "#282c31")
                                  : transportButton.hovered ? viewerPanel.themeColor("hover", "#343940") : "transparent"
                            border.width: transportButton.activeFocus ? 1 : 0
                            border.color: viewerPanel.themeColor("accent", "#3485f6")
                        }
                        contentItem: Canvas {
                            id: transportGlyph
                            anchors.fill: parent
                            anchors.margins: 7
                            onPaint: viewerPanel.drawGlyph(getContext("2d"), transportButton.entry.glyph,
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
                text: String(viewerPanel.currentFrame)
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

            TextField {
                id: timecodeField
                objectName: "viewerTimecode_" + viewerPanel.panelId
                x: transportBar.compact ? transportBar.width / 2 + 4 : transportControls.x + transportControls.width + 8
                y: transportBar.compact ? 34 : 4
                width: 119
                height: 28
                enabled: false
                readOnly: true
                text: "--:--:--:--"
                horizontalAlignment: Text.AlignHCenter
                font.family: "Monospace"
                font.pixelSize: viewerPanel.theme ? viewerPanel.theme.fontSize : 11
                color: viewerPanel.themeColor("disabled", "#5f6670")
                Accessible.name: "Timecode unavailable"
                ToolTip.visible: hovered
                ToolTip.text: "Timecode is unavailable until the native viewer provides an authoritative frame rate."
                background: Rectangle {
                    color: viewerPanel.theme ? viewerPanel.theme.field : "#24272c"
                    radius: viewerPanel.theme ? viewerPanel.theme.smallRadius : 4
                    border.color: viewerPanel.themeColor("border", "#30343a")
                }
            }
        }
    }

    onCurrentFrameChanged: {
        if (!frameField.activeFocus)
            frameField.text = String(currentFrame)
    }

    Keys.onPressed: function(event) {
        if (frameField.activeFocus)
            return
        if (event.key === Qt.Key_Left) {
            viewerPanel.updateClock(viewerPanel.currentFrame - 1); event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            viewerPanel.updateClock(viewerPanel.currentFrame + 1); event.accepted = true
        } else if (event.key === Qt.Key_Home) {
            viewerPanel.updateClock(viewerPanel.firstFrame); event.accepted = true
        } else if (event.key === Qt.Key_End) {
            viewerPanel.updateClock(viewerPanel.lastFrame); event.accepted = true
        }
    }
}
