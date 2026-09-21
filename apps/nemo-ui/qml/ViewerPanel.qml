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

    // Application-injected viewport color sampling (issue #102). Absent in
    // workspace-only harnesses, so every use is guarded; the eyedropper's
    // gesture, sampling request and authored edit all live in that owner.
    readonly property var viewportSampler: typeof viewportPicker !== "undefined" ? viewportPicker : null
    readonly property bool pickArmed: viewerPanel.viewportSampler !== null
                                      && viewerPanel.viewportSampler.active === true
    // What the pick states: the instruction while it is armed, and the reason
    // nothing was authored when a click or the sample was refused. A completed
    // pick clears it — the authored value is its own feedback — so a stated text
    // while nothing is armed is a refusal, never a stale progress message.
    readonly property string pickStatus: viewerPanel.viewportSampler !== null
                                         ? String(viewerPanel.viewportSampler.status || "") : ""
    readonly property bool pickStatusIsError: viewerPanel.pickStatus.length > 0 && !viewerPanel.pickArmed

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
    // A panel-state write is the panel's own record of its view or preferences:
    // adopting it changes the display transform only, and never rebuilds a
    // model or reaches another panel.
    onPanelStateChanged: {
        if (viewInitialised && !panning && !zoomQueued)
            restoreView();
        restoreForceFullFrame();
    }
    onHasImageChanged: {
        refreshView();
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    // A view still in motion when the application closes is still the view the
    // project should record.
    Connections {
        target: viewerPanel.Window.window
        function onClosing() { viewerPanel.saveView(); }
    }
    // The image domain can change without hasImage changing (a media probe
    // resolving, a retargeted viewer), and a record the fitted view cannot
    // represent is converted against that domain.
    onSourceWidthChanged: {
        refreshView();
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onSourceHeightChanged: {
        refreshView();
        refreshCropOverlay();
        refreshRotoOverlay();
    }

    // The coalescing and settling discipline of the wheel burst: one zoom
    // application per event-loop turn, one persisted view once it settles.
    Timer {
        id: zoomFrame
        interval: 0
        repeat: false
        onTriggered: viewerPanel.applyZoomTarget()
    }
    Timer {
        id: zoomSettle
        interval: 200
        repeat: false
        onTriggered: viewerPanel.settleView()
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
    // Display settings are panel-local: they deliberately do not use the
    // controller's render state, so two panels may pan and zoom separately.
    //
    // The view is one continuous, cursor-anchored scale plus an image-space pan.
    // `viewZoom` is the artist-facing ABSOLUTE scale (1.0 is 1:1), the number
    // the control states; `viewFitted` means the panel recomputes the scale
    // from its own geometry, which is what an untouched viewer opens with.
    //
    // ONE conversion between that scale and the request: the controller's zoom
    // is relative to the fitted image in the panel's full area
    // (`controllerFitScale()`), so `controllerZoom()` is the scale the request
    // carries and the display transform draws. Because both ends use one
    // mapping, the readout can never state a scale the request disagrees with.
    // The representable range is the controller's own bound (0.05x..32x of the
    // fitted image) expressed through the same conversion.
    property real viewZoom: 1
    property bool viewFitted: true
    property real viewPanX: 0
    property real viewPanY: 0
    // Accumulated wheel target and anchor for the burst in flight. A gesture
    // applies once per event-loop turn and persists once it settles.
    property real zoomTarget: 1
    property real zoomAnchorX: 0
    property real zoomAnchorY: 0
    property bool zoomQueued: false
    property bool zoomApplied: false
    property bool panning: false
    property real panLastX: 0
    property real panLastY: 0
    property bool viewReady: false
    property bool viewInitialised: false
    property bool viewRestorePending: false
    // --- Crop box handles (issue #92, stories 43-44) ------------------------
    // The overlay descriptor for the Crop node this panel DIRECTLY views and
    // the same group's inspector has open, or null. It is refreshed from the
    // existing inspector query and the worker's described input geometry; no
    // pixel work, no source read and no second parameter model happens here.
    property var cropOverlay: null
    // Bumped when the described input geometry answer advances.
    property int cropEpoch: 0
    // The one live box gesture (the session's token) and the frozen mapping the
    // pointer is interpreted through, so a reformat output that changes size
    // during the drag cannot move the box under the pointer.
    property string cropGestureToken: ""
    property bool cropGestureActive: false
    property string cropDragHandle: ""
    property var cropDragFrozen: null
    // The previewed box in canvas y-down coordinates while a gesture is live.
    property var cropPreviewBox: null
    property bool cropDragMoved: false
    property string cropGestureError: ""
    // Roto authoring follows an inspected node in this context group, including
    // downstream images whose pixel coordinates still match that node.
    property var rotoOverlayState: null
    property string rotoOverlayReason: ""
    // The shared per-node authoring adapter, keyed by node and owned by the
    // authoring facade (ViewerController), so the overlay and the inspector's
    // shape list share one selection and one draft of that node and nothing else.
    property var rotoController: null
    readonly property var rotoOverlayItem: rotoOverlayLoader.item
    readonly property bool rotoActive: viewerPanel.rotoOverlayState !== null && viewerPanel.rotoController !== null && viewerPanel.rotoController.available === true
    readonly property bool rotoGestureActive: viewerPanel.rotoOverlayItem !== null && viewerPanel.rotoOverlayItem.gestureLive === true
    onRotoGestureActiveChanged: viewerPanel.syncRotoHistoryGesture()
    onCropGestureActiveChanged: syncCropHistoryGesture()
    onCropOverlayChanged: cropCanvas.requestPaint()
    onCropEpochChanged: cropCanvas.requestPaint()
    onCropPreviewBoxChanged: cropCanvas.requestPaint()
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
    // Actionable viewer state only: the controller's own error, or the guidance
    // for a state that has nothing to present (an unbound Read, a missing or
    // unavailable target). A normal render states NO text over the media — the
    // owner-approved removal of the transient progress/node-name flash (issue
    // #98) — and a failed or pending request that retains a frame reports its
    // message beside the media instead of covering it.
    readonly property string viewerDiagnostic: {
        var error = String(controller.error || "")
        if (error.length > 0)
            return error
        var state = String(controller.renderState || "")
        if (state === "empty" || state === "unavailable")
            return String(controller.status || "")
        if (!targetAvailable)
            return targetName
        return ""
    }
    readonly property bool viewerShowsFrame: controller.hasPresentation === true
    readonly property color viewerDiagnosticColor: String(controller.error || "").length > 0
                                                   ? themeColor("errorText", "#f0d0d0")
                                                   : themeColor("muted", "#979ea8")
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
                // The layers the target actually describes, with the root layer
                // the channel-name convention starts from. A target that carries
                // no other layer simply offers none: no layer is invented to
                // fill the menu.
                model: viewerPanel.controller.layers
                currentIndex: Math.max(0, model.indexOf(viewerPanel.controller.layer))
                onActivated: viewerPanel.controller.setLayer(currentText)
                ToolTip.visible: hovered
                ToolTip.text: viewerPanel.controller.layerReason
                Accessible.name: "Image layer"
            }

            StudioComboBox {
                id: channelSelector
                objectName: "viewerChannel_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                // RGBA is the selected layer's composite; the rest are that
                // layer's real channel names, in the order the target describes.
                model: viewerPanel.controller.displayChannels
                currentIndex: Math.max(0, model.indexOf(viewerPanel.controller.channel))
                onActivated: viewerPanel.controller.setChannel(currentText)
                ToolTip.visible: hovered
                ToolTip.text: "Display channel within the selected layer. RGBA is the layer's composite; a single data channel is isolated as itself."
                Accessible.name: "Display channels"
            }

            StudioComboBox {
                id: zoomSelector
                objectName: "viewerZoomMenu_" + viewerPanel.panelId
                theme: viewerPanel.theme
                width: 78
                height: 24
                model: ["Fit", "100%", "50%"]
                // The control states the scale the panel is drawing: "Fit" while
                // the view is fitted, the exact percentage otherwise. Presets
                // stay direct choices, and a percentage can be typed (with or
                // without the % sign) so a known inspection scale is reachable.
                typeable: true
                readout: viewerPanel.zoomReadout()
                onActivated: viewerPanel.setZoomPreset(currentText)
                onTextAccepted: function (text) {
                    viewerPanel.setZoomPreset(text);
                }
                ToolTip.visible: hovered
                ToolTip.text: "Zoom. Pick Fit, 100% or 50%, or type a percentage."
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

            // Whole-frame coverage switch (issue #85). It sits beside the
            // resolution control because it answers the same question — how much
            // of the image the request covers — and it is icon-only so the
            // compact row keeps the resolution choice as its last stated value;
            // the tooltip and the accessible name carry its meaning. The
            // controller is the live owner of the switch, so `checked` stays a
            // binding and a click only submits the next value: a checkable
            // Button would write `checked` itself, destroy the binding, and
            // leave the control disagreeing with the request after a panel-state
            // restore.
            Button {
                id: fullFrameButton
                objectName: "viewerFullFrame_" + viewerPanel.panelId
                width: 24
                height: 24
                padding: 0
                flat: true
                checked: viewerPanel.controller.forceFullFrame === true
                Accessible.name: "Force full-frame rendering"
                Accessible.checkable: true
                Accessible.checked: fullFrameButton.checked
                ToolTip.visible: hovered
                ToolTip.text: "Force full-frame rendering"
                onClicked: viewerPanel.setForceFullFrame(!viewerPanel.controller.forceFullFrame)
                background: Rectangle {
                    radius: viewerPanel.theme ? viewerPanel.theme.smallRadius : 4
                    color: fullFrameButton.down ? viewerPanel.themeColor("raised", "#282c31")
                          : fullFrameButton.hovered ? viewerPanel.themeColor("hover", "#343940") : "transparent"
                    border.width: fullFrameButton.checked || fullFrameButton.activeFocus ? 1 : 0
                    border.color: viewerPanel.themeColor("accent", "#3485f6")
                }
                contentItem: Canvas {
                    id: fullFrameGlyph
                    anchors.fill: parent
                    // The Canvas repaints from its own state property, so the
                    // repaint trigger can never resolve to a neighbouring
                    // object's signal.
                    readonly property bool active: fullFrameButton.checked
                    onActiveChanged: requestPaint()
                    onPaint: viewerPanel.drawFullFrameGlyph(getContext("2d"), width, height, active)
                    Component.onCompleted: requestPaint()
                    Connections {
                        target: viewerPanel
                        function onThemeChanged() { fullFrameGlyph.requestPaint() }
                    }
                    Connections {
                        target: viewerPanel.theme
                        function onPresetChanged() { fullFrameGlyph.requestPaint() }
                        function onAccentOverrideChanged() { fullFrameGlyph.requestPaint() }
                    }
                }
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

    // The coverage switch is panel-local and is recorded in this panel's own
    // state record; the workspace stays the one owner of persisted panel state
    // and nothing here touches the document, its history, or a view transform.
    function setForceFullFrame(value) {
        var next = value === true
        controller.setForceFullFrame(next)
        saveState({forceFullFrame: next})
    }

    // Adopt the recorded switch into this panel's controller. Coverage is a
    // request property, so restoring it re-derives the request without
    // recentering or refitting the view.
    function restoreForceFullFrame() {
        if (!controller)
            return
        controller.setForceFullFrame(panelState && panelState.forceFullFrame === true)
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

    // The controller fits the image into the panel's whole area; the panel's own
    // fitted display keeps the accepted 6 px margin. Both are the same scale
    // factor, so the conversion below is exact rather than approximate.
    function controllerFitScale() {
        if (sourceWidth <= 0 || sourceHeight <= 0 || viewer.width <= 0 || viewer.height <= 0)
            return 0
        return Math.min(viewer.width / (sourceWidth * controller.pixelAspect), viewer.height / sourceHeight)
    }

    function clampViewZoom(value) {
        var fit = controllerFitScale()
        if (!isFinite(value))
            return fit > 0 ? fit : 1
        if (fit <= 0)
            return Math.max(0.01, Math.min(64, value))
        return Math.max(0.05 * fit, Math.min(32 * fit, value))
    }

    // The scale the display transform draws at, and therefore the scale the
    // request must carry.
    function imageScale() {
        return viewFitted ? displayScale() : clampViewZoom(viewZoom)
    }

    function controllerZoom() {
        var fit = controllerFitScale()
        return fit > 0 ? imageScale() / fit : 1
    }

    function viewVisibleWidth(scale) {
        return Math.min(sourceWidth, Math.max(1, viewer.width) / (scale * controller.pixelAspect))
    }

    function viewVisibleHeight(scale) {
        return Math.min(sourceHeight, Math.max(1, viewer.height) / scale)
    }

    function viewCenterX(scale) {
        var visible = viewVisibleWidth(scale)
        return Math.max(visible / 2, Math.min(sourceWidth - visible / 2, sourceWidth / 2 + viewPanX))
    }

    function viewCenterY(scale) {
        var visible = viewVisibleHeight(scale)
        return Math.max(visible / 2, Math.min(sourceHeight - visible / 2, sourceHeight / 2 + viewPanY))
    }

    // Push the panel's view into the viewer controller so the request region and
    // sampling follow what the artist is looking at.
    function syncView() {
        var fit = controllerFitScale()
        if (fit <= 0 || !controller)
            return
        viewReady = true
        controller.setZoom(controllerZoom())
        controller.setPan(Qt.point(viewPanX, viewPanY))
    }

    // The zoom the control states: the fitted state, or the exact absolute
    // scale the gesture produced.
    function zoomPercentText(scale) {
        var percent = Math.round(scale * 1000) / 10
        return (percent === Math.round(percent) ? String(Math.round(percent)) : String(percent)) + "%"
    }

    function zoomReadout() {
        return viewFitted ? "Fit" : zoomPercentText(imageScale())
    }

    function computeDisplayRect() {
        if (!hasImage)
            return Qt.rect(0, 0, 0, 0)
        var scale = imageScale()
        var sx = scale * controller.pixelAspect
        var cx = viewCenterX(scale)
        var cy = viewCenterY(scale)
        var region = controller.presentedRegion
        return Qt.rect(viewer.width / 2 + (region.x - cx) * sx,
                       viewer.height / 2 + (region.y - cy) * scale,
                       region.width * sx, region.height * scale)
    }

    // Keep the image point under (px, py) where the artist put it while the
    // scale changes.
    function setViewScale(next, px, py) {
        var current = imageScale()
        if (next === current && !viewFitted)
            return
        var scale = clampViewZoom(next)
        if (px !== undefined && current > 0 && viewer.width > 0 && viewer.height > 0) {
            var offsetX = px - viewer.width / 2
            var offsetY = py - viewer.height / 2
            var imageX = viewCenterX(current) + offsetX / (current * controller.pixelAspect)
            var imageY = viewCenterY(current) + offsetY / current
            viewPanX = imageX - offsetX / (scale * controller.pixelAspect) - sourceWidth / 2
            viewPanY = imageY - offsetY / scale - sourceHeight / 2
        }
        viewFitted = false
        viewZoom = scale
        syncView()
    }

    // A wheel burst: accumulate the target and apply it once per event-loop
    // turn, then persist the settled view once.
    function queueZoom(factor, px, py) {
        if (!zoomQueued) {
            zoomTarget = imageScale()
            zoomQueued = true
            zoomFrame.start()
        }
        zoomTarget = clampViewZoom(zoomTarget * factor)
        zoomAnchorX = px
        zoomAnchorY = py
        zoomSettle.restart()
    }

    function applyZoomTarget() {
        zoomQueued = false
        var next = clampViewZoom(zoomTarget)
        if (next === imageScale() && !viewFitted)
            return
        setViewScale(next, zoomAnchorX, zoomAnchorY)
        zoomApplied = true
    }

    function settleView() {
        if (!zoomApplied)
            return
        zoomApplied = false
        saveView()
    }

    function saveView() {
        if (!viewReady)
            return
        saveState({
                      "zoomMode": viewFitted ? "Fit" : "Scale",
                      "zoom": Number(imageScale()),
                      "panX": Number(viewPanX),
                      "panY": Number(viewPanY)
                  })
    }

    function setZoomPreset(text) {
        var value = String(text).trim().toLowerCase()
        if (value === "fit") {
            viewFitted = true
            viewPanX = 0
            viewPanY = 0
            syncView()
            saveView()
            return
        }
        var percent = parseFloat(value)
        if (!isFinite(percent))
            return
        setViewScale(clampViewZoom(percent / 100), viewer.width / 2, viewer.height / 2)
        saveView()
    }

    // --- Crop box handles ---------------------------------------------------
    // The overlay is the reference Crop box drawn over THIS panel's own view.
    // It exists only when the node this panel directly renders is also the node
    // the same group's inspector has open, and only the graph role has such a
    // target: another panel's inspection, a selected-but-not-viewed node or a
    // media/timeline role never draws handles.
    //
    // The authored values are the node's typed parameters at the current frame,
    // read from the same inspector query the parameters panel uses. Values are
    // bottom-left coordinates in the incoming image's format; only creation
    // defaults come from the owning network. Reformat translates the authored
    // box enclosure to zero, independently of any data-bound intersection.
    function refreshCropOverlay() {
        var next = null;
        if (controller && graphRole && hasImage && targetAvailable) {
            var network = String(controller.rootNetworkId || "");
            var target = String(controller.viewerTargetId || "");
            var inspected = false;
            var nodes = panelContext && panelContext.inspectorNodes ? panelContext.inspectorNodes : [];
            for (var index = 0; index < nodes.length; ++index) {
                if (String(nodes[index].network) === network && String(nodes[index].node) === target) {
                    inspected = true;
                    break;
                }
            }
            if (inspected && network.length > 0 && target.length > 0)
                next = cropOverlayFor(network, target);
        }
        cropOverlay = next;
    }

    function cropParamNumber(rows, key, fallback) {
        var entry = rows[key];
        if (!entry || entry.value === undefined || entry.value === null || entry.value.length !== undefined)
            return fallback;
        var value = Number(entry.value);
        return isFinite(value) ? value : fallback;
    }

    function cropParamFlag(rows, key) {
        var entry = rows[key];
        return entry ? entry.value === true || String(entry.value) === "true" : false;
    }

    function cropOverlayFor(network, node) {
        var inspector = controller.parameterInspector(network, node);
        if (!inspector || inspector.available !== true || String(inspector.type) !== "crop")
            return null;
        var rows = ({});
        var sections = inspector.sections || [];
        for (var i = 0; i < sections.length; ++i) {
            var parameters = sections[i].parameters || [];
            for (var j = 0; j < parameters.length; ++j) {
                if (parameters[j] && parameters[j].key !== undefined)
                    rows[String(parameters[j].key)] = parameters[j];
            }
        }
        // The authored box is stated in the ORIGINAL INPUT IMAGE's own space:
        // x/right are distances from that image's left edge and y/top distances
        // from its bottom edge. The input's ACTUAL described format therefore
        // fixes the conversion, never the owning composition's saved canvas and
        // never the selected source or another panel's viewer. Without a
        // described input there is no honest mapping, so no box is drawn.
        var input = cropInputGeometry(network, node);
        if (!input)
            return null;
        var inputHeight = Number(input.height);
        if (!(inputHeight > 0))
            return null;
        var x = cropParamNumber(rows, "x", 0);
        var r = cropParamNumber(rows, "right", 0);
        var y = cropParamNumber(rows, "y", 0);
        var t = cropParamNumber(rows, "top", 0);
        var left = Math.min(x, r);
        var right = Math.max(x, r);
        var top = inputHeight - Math.max(y, t);
        var bottom = inputHeight - Math.min(y, t);
        var reformat = cropParamFlag(rows, "reformat");
        // A reformat crop's output format is the NONEMPTY floor/ceil enclosure
        // of the authored box, translated to zero; `intersect` clips the DATA
        // only, so it never moves or hides the box. An empty intersection is a
        // valid (fully transparent) result and the handles stay usable.
        var offsetX = 0;
        var offsetY = 0;
        if (reformat) {
            offsetX = Math.floor(left);
            offsetY = Math.floor(top);
        }
        return {
            "network": network,
            "node": node,
            "left": left,
            "right": right,
            "top": top,
            "bottom": bottom,
            "leftKey": x <= r ? "x" : "right",
            "rightKey": x <= r ? "right" : "x",
            "bottomKey": y <= t ? "y" : "top",
            "topKey": y <= t ? "top" : "y",
            "inputWidth": Number(input.width),
            "inputHeight": inputHeight,
            "offsetX": offsetX,
            "offsetY": offsetY,
            "reformat": reformat,
            "intersect": cropParamFlag(rows, "intersect")
        };
    }

    // The described image arriving at the crop's own image input: its format,
    // whose height fixes the authored box's y-up conversion. The authored box is
    // stated in that image's OWN normalized space (distances from its left and
    // bottom edges) and the presentation is resolved 0-based, so no origin term
    // participates. Metadata only, answered by the worker; nothing is probed and
    // no source is read on this thread.
    function cropInputGeometry(network, node) {
        var answer = controller.nodeInputChannels(network, node);
        if (!answer || answer.available !== true)
            return null;
        var ports = answer.ports || [];
        for (var index = 0; index < ports.length; ++index) {
            var port = ports[index];
            if (port.image !== true || !port.format)
                continue;
            if (Number(port.format.width) <= 0 || Number(port.format.height) <= 0)
                continue;
            return {
                "width": Number(port.format.width),
                "height": Number(port.format.height)
            };
        }
        return null;
    }

    // The view the box is drawn through and the pointer is interpreted through.
    // ONE mapping serves both, so a handle always sits under its own coordinate.
    function cropViewMapping() {
        var scale = imageScale()
        var sx = scale * controller.pixelAspect
        return {
            "scale": scale,
            "sx": sx,
            // Snapshot scalar camera coordinates, not a live QRectF property
            // reference whose raster origin can change when a frame arrives.
            "originX": viewer.width / 2 - viewCenterX(scale) * sx,
            "originY": viewer.height / 2 - viewCenterY(scale) * scale
        }
    }

    function cropScreenX(view, imageX) {
        return view.originX + imageX * view.sx
    }

    function cropScreenY(view, imageY) {
        return view.originY + imageY * view.scale
    }

    // The full-resolution image coordinate a panel point addresses, in the
    // PRESENTED frame's own domain: the exact inverse of the same mapping the
    // box handles are drawn and hit through, so a click and a handle can never
    // disagree about where a pixel is. Null when there is nothing to map.
    function imagePointAt(px, py) {
        if (!viewerPanel.hasImage)
            return null
        var view = cropViewMapping()
        if (!(view.sx > 0) || !(view.scale > 0))
            return null
        return {
            "x": (px - view.originX) / view.sx,
            "y": (py - view.originY) / view.scale
        }
    }

    // The box in the presented image's own y-down coordinates.
    function cropDisplayBox(left, right, top, bottom, offsetX, offsetY) {
        return {
            "left": left - offsetX,
            "right": right - offsetX,
            "top": top - offsetY,
            "bottom": bottom - offsetY
        }
    }

    // The box's screen rectangle: the frozen mapping while a gesture is live (so
    // a reformat output that changes size mid-drag cannot move it), the live
    // view otherwise.
    function cropScreenRect() {
        if (cropDragHandle.length > 0 && cropDragFrozen) {
            var frozenBox = cropPreviewBox
            if (!frozenBox)
                return null
            var frozenView = cropDragFrozen.view
            return Qt.rect(cropScreenX(frozenView, frozenBox.left - cropDragFrozen.offsetX),
                           cropScreenY(frozenView, frozenBox.top - cropDragFrozen.offsetY),
                           (frozenBox.right - frozenBox.left) * frozenView.sx,
                           (frozenBox.bottom - frozenBox.top) * frozenView.scale)
        }
        var overlay = cropOverlay
        if (!overlay)
            return null
        var view = cropViewMapping()
        var box = cropDisplayBox(overlay.left, overlay.right, overlay.top, overlay.bottom, overlay.offsetX,
                                 overlay.offsetY)
        return Qt.rect(cropScreenX(view, box.left), cropScreenY(view, box.top),
                       (box.right - box.left) * view.sx, (box.bottom - box.top) * view.scale)
    }

    function cropHandleRadius() {
        return 6
    }

    // Which part of the box a point addresses: a corner or edge resize, the
    // centre move handle, or nothing (blank image area, which pans).
    function cropHandleAt(px, py) {
        var rect = cropScreenRect()
        if (!rect)
            return ""
        var radius = cropHandleRadius()
        var nearLeft = Math.abs(px - rect.x) <= radius
        var nearRight = Math.abs(px - (rect.x + rect.width)) <= radius
        var nearTop = Math.abs(py - rect.y) <= radius
        var nearBottom = Math.abs(py - (rect.y + rect.height)) <= radius
        if (px < rect.x - radius || px > rect.x + rect.width + radius || py < rect.y - radius
                || py > rect.y + rect.height + radius)
            return ""
        var horizontal = nearLeft ? "left" : nearRight ? "right" : ""
        var vertical = nearTop ? "top" : nearBottom ? "bottom" : ""
        if (horizontal.length > 0 && vertical.length > 0)
            return horizontal + "-" + vertical
        if (horizontal.length > 0)
            return horizontal
        if (vertical.length > 0)
            return vertical
        if (Math.abs(px - (rect.x + rect.width / 2)) <= radius
                && Math.abs(py - (rect.y + rect.height / 2)) <= radius)
            return "center"
        return ""
    }

    // Pointer -> authored canvas coordinate, through ONE frozen mapping. The
    // offset is captured with the mapping, so a reformat output whose size
    // changes under the drag can never move the coordinate the pointer states.
    function cropCanvasX(frozen, px) {
        return (px - frozen.view.originX) / frozen.view.sx + frozen.offsetX
    }

    function cropCanvasYUp(frozen, py) {
        return frozen.inputHeight - ((py - frozen.view.originY) / frozen.view.scale + frozen.offsetY)
    }

    function beginCropGesture(handle, px, py) {
        if (!controller)
            return false
        var overlay = cropOverlay
        if (!overlay)
            return false
        cropGestureError = ""
        // One interaction is live in the session at a time: this begin retires
        // whatever was live first through the controller's cancellation path,
        // so a press always starts a drag instead of being refused and leaving
        // the parameter locked. Any notice for a previous token of this panel
        // has already cleared the local drag state below.
        var token = String(controller.beginNodeParameterEdits(overlay.network, overlay.node,
                                                              ["x", "y", "right", "top"]))
        if (token.length === 0) {
            cropGestureError = String(controller.error)
            return false
        }
        cropGestureToken = token
        cropDragHandle = handle
        cropDragMoved = false
        var frozen = {
            "view": cropViewMapping(),
            "offsetX": overlay.offsetX,
            "offsetY": overlay.offsetY,
            "inputHeight": overlay.inputHeight,
            "left": overlay.left,
            "right": overlay.right,
            "top": overlay.top,
            "bottom": overlay.bottom,
            "leftKey": overlay.leftKey,
            "rightKey": overlay.rightKey,
            "topKey": overlay.topKey,
            "bottomKey": overlay.bottomKey,
            "pointerX": 0,
            "pointerY": 0
        }
        frozen.pointerX = cropCanvasX(frozen, px)
        frozen.pointerY = cropCanvasYUp(frozen, py)
        cropDragFrozen = frozen
        cropPreviewBox = {
            "left": overlay.left,
            "right": overlay.right,
            "top": overlay.top,
            "bottom": overlay.bottom
        }
        cropGestureActive = true
        return true
    }

    function updateCropGesture(px, py) {
        if (cropGestureToken.length === 0 || !cropDragFrozen)
            return false
        var frozen = cropDragFrozen
        var canvasX = cropCanvasX(frozen, px)
        var canvasYUp = cropCanvasYUp(frozen, py)
        var values = ({})
        var box = {
            "left": frozen.left,
            "right": frozen.right,
            "top": frozen.inputHeight - frozen.top,
            "bottom": frozen.inputHeight - frozen.bottom
        }
        if (cropDragHandle === "center") {
            var deltaX = canvasX - frozen.pointerX
            var deltaYUp = canvasYUp - frozen.pointerY
            box.left += deltaX
            box.right += deltaX
            box.bottom += deltaYUp
            box.top += deltaYUp
            values[frozen.leftKey] = box.left
            values[frozen.rightKey] = box.right
            values[frozen.bottomKey] = box.bottom
            values[frozen.topKey] = box.top
        } else {
            if (cropDragHandle.indexOf("left") >= 0) {
                values[frozen.leftKey] = canvasX
                box.left = canvasX
            }
            if (cropDragHandle.indexOf("right") >= 0) {
                values[frozen.rightKey] = canvasX
                box.right = canvasX
            }
            if (cropDragHandle.indexOf("top") >= 0) {
                values[frozen.topKey] = canvasYUp
                box.top = canvasYUp
            }
            if (cropDragHandle.indexOf("bottom") >= 0) {
                values[frozen.bottomKey] = canvasYUp
                box.bottom = canvasYUp
            }
        }
        // The preview box is stated in the frozen mapping's y-down terms, so it
        // is the same rectangle the pointer is reading.
        cropPreviewBox = {
            "left": Math.min(box.left, box.right),
            "right": Math.max(box.left, box.right),
            "top": frozen.inputHeight - Math.max(box.top, box.bottom),
            "bottom": frozen.inputHeight - Math.min(box.top, box.bottom)
        }
        cropDragMoved = true
        if (controller.updateNodeParameterEdits(cropGestureToken, values) !== true) {
            cropGestureError = String(controller.error)
            cancelCropGesture()
            return false
        }
        return true
    }

    // A release publishes the one history entry; a press that never moved
    // cancels instead, so a click on a handle never creates a no-op entry.
    function finishCropGesture() {
        var token = cropGestureToken
        var moved = cropDragMoved
        cropGestureToken = ""
        cropGestureActive = false
        cropDragHandle = ""
        cropDragFrozen = null
        cropPreviewBox = null
        cropDragMoved = false
        if (token.length === 0)
            return false
        var committed = moved ? controller.commitNodeParameterEdit(token) : controller.cancelNodeParameterEdit(token)
        if (!committed && moved)
            cropGestureError = String(controller.error)
        refreshCropOverlay()
        return committed
    }

    function cancelCropGesture() {
        var token = cropGestureToken
        cropGestureToken = ""
        cropGestureActive = false
        cropDragHandle = ""
        cropDragFrozen = null
        cropPreviewBox = null
        cropDragMoved = false
        if (token.length === 0)
            return false
        return controller.cancelNodeParameterEdit(token)
    }

    // The session retired an interaction, which may have been this panel's crop
    // gesture because another control took over. Only the matching token drops
    // the local drag, so the replacement keeps working and the cancellation is
    // never repeated against it. The re-derive is queued because this notice
    // arrives inside the owner's own call.
    function retireCropGesture(token) {
        if (String(token).length === 0 || String(token) !== cropGestureToken)
            return false
        cropGestureToken = ""
        cropGestureActive = false
        cropDragHandle = ""
        cropDragFrozen = null
        cropPreviewBox = null
        cropDragMoved = false
        Qt.callLater(function () { viewerPanel.refreshCropOverlay() })
        return true
    }

    // Escape and a preview-only Undo reach the one live box or Roto gesture
    // through the shared history owner: the session's preview is discarded and
    // the release that follows publishes nothing.
    function cancelHistoryGesture() {
        cancelCropGesture()
        if (viewerPanel.rotoOverlayItem)
            viewerPanel.rotoOverlayItem.cancelAll()
    }

    // One panel registers ONE gesture owner, whether the live edit is a crop box
    // or a Roto handle.
    function syncCropHistoryGesture() {
        if (typeof historyController === "undefined" || !historyController)
            return
        historyController.setGesture(viewerPanel, cropGestureActive || viewerPanel.rotoGestureActive)
    }

    function syncRotoHistoryGesture() {
        syncCropHistoryGesture()
    }

    // --- Roto overlay (issue #93) ------------------------------------------
    function refreshRotoOverlay() {
        var next = null
        var refusal = ""
        if (controller && rotoFactory && graphRole && hasImage && targetAvailable) {
            var network = String(controller.rootNetworkId || "")
            var target = String(controller.viewerTargetId || "")
            var nodes = panelContext && panelContext.inspectorNodes ? panelContext.inspectorNodes : []
            // Prefer the directly viewed Roto; otherwise use the first inspected
            // upstream Roto with an unambiguous coordinate-preserving path.
            for (var pass = 0; pass < 2 && !next; ++pass) {
                for (var index = 0; index < nodes.length; ++index) {
                    var node = String(nodes[index].node)
                    if (String(nodes[index].network) !== network || (node === target) !== (pass === 0))
                        continue
                    var inspector = controller.parameterInspector(network, node)
                    if (!inspector || inspector.available !== true || String(inspector.type) !== "roto")
                        continue
                    var candidate = rotoFactory.createRotoControllerFor(network, node, panelGroup, viewerPanel)
                    if (!candidate)
                        continue
                    if (candidate.canOverlayViewer(target)) {
                        next = { "network": network, "node": node, "target": target }
                        break
                    }
                    var reason = String(candidate.overlayReason(target))
                    if (!refusal && reason.length > 0)
                        refusal = reason
                    if (candidate !== viewerPanel.rotoController)
                        candidate.detachView(viewerPanel)
                }
            }
        }
        viewerPanel.rotoOverlayReason = next ? "" : refusal
        var previous = viewerPanel.rotoOverlayState
        var changed = (previous === null) !== (next === null)
        if (!changed && previous !== null && next !== null)
            changed = String(previous.node) !== String(next.node) || String(previous.network) !== String(next.network)
                    || String(previous.target) !== String(next.target)
        if (!changed)
            return
        if (viewerPanel.rotoOverlayItem)
            viewerPanel.rotoOverlayItem.cancelAll()
        viewerPanel.rotoOverlayState = next
        viewerPanel.bindRotoController()
    }

    // The authoring facade shares an adapter within this context group.
    // Independent groups retain independent clocks and transient selections.
    readonly property var rotoFactory: (typeof viewerController !== "undefined" && viewerController) ? viewerController : controller

    function bindRotoController() {
        var state = viewerPanel.rotoOverlayState
        if (!state || !viewerPanel.rotoFactory) {
            if (viewerPanel.rotoController)
                viewerPanel.rotoController.detachView(viewerPanel)
            viewerPanel.rotoController = null
            return
        }
        var created = viewerPanel.rotoFactory.createRotoControllerFor(state.network, state.node, viewerPanel.panelGroup, viewerPanel)
        if (viewerPanel.rotoController && viewerPanel.rotoController !== created)
            viewerPanel.rotoController.detachView(viewerPanel)
        viewerPanel.rotoController = created && String(created.nodeId) === String(state.node) && String(created.networkId) === String(state.network) ? created : null
        // The adapter evaluates and keys at the panel's frame; assigning it here
        // and on every frame change is what makes the drawn geometry the frame
        // on screen.
        if (viewerPanel.rotoController)
            viewerPanel.rotoController.setViewerFrame(viewerPanel, viewerPanel.currentFrame)
    }

    Connections {
        target: viewerPanel.rotoController
        function onSeekRequested(frame) { viewerPanel.updateClock(frame) }
    }

    function rotoViewMapping() {
        return viewerPanel.cropViewMapping()
    }

    function rotoCursorShape(px, py) {
        if (!viewerPanel.rotoOverlayItem)
            return Qt.ArrowCursor
        return viewerPanel.rotoOverlayItem.cursorShape(px, py)
    }

    function cropCursorShape(px, py) {
        var handle = cropHandleAt(px, py)
        if (handle === "center")
            return Qt.SizeAllCursor
        if (handle.indexOf("left") >= 0 || handle.indexOf("right") >= 0) {
            if (handle.indexOf("top") >= 0 || handle.indexOf("bottom") >= 0)
                return handle === "left-top" || handle === "right-bottom" ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
            return Qt.SizeHorCursor
        }
        if (handle.indexOf("top") >= 0 || handle.indexOf("bottom") >= 0)
            return Qt.SizeVerCursor
        return Qt.ArrowCursor
    }

    // The box is one thin themed outline with a solid grab handle at every
    // corner and edge midpoint and a centre move affordance. It is drawn only
    // over this panel's own image area and never becomes a raster of its own.
    function paintCropHandles(ctx, width, height) {
        ctx.reset()
        var rect = cropScreenRect()
        if (!rect)
            return
        var accent = themeColor("accent", "#3485f6")
        var surround = themeColor("imageSurround", "#17191b")
        ctx.save()
        ctx.beginPath()
        ctx.rect(0, 0, width, height)
        ctx.clip()
        ctx.strokeStyle = accent
        ctx.lineWidth = 1
        ctx.strokeRect(Math.round(rect.x) + 0.5, Math.round(rect.y) + 0.5,
                       Math.round(rect.x + rect.width) - Math.round(rect.x),
                       Math.round(rect.y + rect.height) - Math.round(rect.y))
        var size = 5
        var points = [
            [rect.x, rect.y],
            [rect.x + rect.width / 2, rect.y],
            [rect.x + rect.width, rect.y],
            [rect.x, rect.y + rect.height / 2],
            [rect.x + rect.width, rect.y + rect.height / 2],
            [rect.x, rect.y + rect.height],
            [rect.x + rect.width / 2, rect.y + rect.height],
            [rect.x + rect.width, rect.y + rect.height]
        ]
        for (var index = 0; index < points.length; ++index) {
            var pointX = Math.round(points[index][0] - size / 2)
            var pointY = Math.round(points[index][1] - size / 2)
            ctx.fillStyle = surround
            ctx.fillRect(pointX, pointY, size, size)
            ctx.strokeStyle = accent
            ctx.strokeRect(pointX + 0.5, pointY + 0.5, size - 1, size - 1)
        }
        var centreX = Math.round(rect.x + rect.width / 2) + 0.5
        var centreY = Math.round(rect.y + rect.height / 2) + 0.5
        ctx.strokeStyle = accent
        ctx.beginPath()
        ctx.moveTo(centreX - 5, centreY)
        ctx.lineTo(centreX + 5, centreY)
        ctx.moveTo(centreX, centreY - 5)
        ctx.lineTo(centreX, centreY + 5)
        ctx.stroke()
        ctx.restore()
    }

    // The view becomes live once the image domain and the panel geometry are
    // both known; afterwards a resize re-fits or re-derives the request region
    // without touching the artist's scale.
    function refreshView() {
        if (!viewInitialised) {
            if (!hasImage || viewer.width <= 0 || viewer.height <= 0)
                return
            viewInitialised = true
            restoreView()
            return
        }
        if (viewFitted)
            viewZoom = displayScale()
        if (viewRestorePending && !panning && !zoomQueued)
            restoreView()
        else
            syncView()
    }

    // Restore the persisted view. The saved scale is absolute, so the control
    // states the scale the project was saved at; an unreadable record resolves
    // to a fitted view rather than an image the artist cannot see. The retired
    // continuous mode stored a fit-relative scale under an unknown mode, and
    // that scale is representable now, so it is converted rather than dropped.
    function restoreView() {
        // A stored scale relative to the fitted image can only be converted once
        // the image domain is known, so the restore waits for it and is retried
        // when it arrives.
        if (sourceWidth <= 0 || sourceHeight <= 0 || viewer.width <= 0 || viewer.height <= 0) {
            viewRestorePending = true
            return
        }
        viewRestorePending = false
        var mode = panelState && panelState.zoomMode ? String(panelState.zoomMode) : "Fit"
        var stored = panelState && panelState.zoom !== undefined ? Number(panelState.zoom) : NaN
        viewPanX = panelState && panelState.panX !== undefined ? Number(panelState.panX) || 0 : 0
        viewPanY = panelState && panelState.panY !== undefined ? Number(panelState.panY) || 0 : 0
        if (mode === "Scale" && isFinite(stored))
            viewFitted = false
        else if (mode === "100%")
            viewFitted = false
        else if (mode === "50%")
            viewFitted = false
        else if (mode === "Custom" && isFinite(stored)) {
            // The retired continuous mode stored a multiple of the panel's own
            // fitted display scale, so that is the scale it was drawing.
            viewFitted = false
            viewZoom = clampViewZoom(stored * displayScale())
            syncView()
            return
        } else {
            viewFitted = true
            viewZoom = displayScale()
            syncView()
            return
        }
        viewZoom = clampViewZoom(mode === "100%" ? 1 : mode === "50%" ? 0.5 : stored)
        syncView()
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

    // The whole-frame switch's icon: the frame is the image domain and the
    // inner box is the region a request covers. While the request follows the
    // visible region the inner box is an outline; once the whole frame is
    // requested it is filled, and the frame itself takes the accent. Drawn in
    // the same stroke vocabulary as the transport glyphs.
    function drawFullFrameGlyph(ctx, width, height, active) {
        ctx.reset()
        var colour = viewerPanel.themeColor(active ? "accent" : "text", "#dce0e6")
        ctx.strokeStyle = colour
        ctx.fillStyle = colour
        ctx.lineWidth = 1.5
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        var left = Math.round(width * 0.17) + 0.5
        var top = Math.round(height * 0.24) + 0.5
        var right = Math.round(width * 0.83) - 0.5
        var bottom = Math.round(height * 0.76) - 0.5
        ctx.strokeRect(left, top, right - left, bottom - top)
        var insetX = Math.max(3, (right - left) * 0.24)
        var insetY = Math.max(1, (bottom - top) * 0.24)
        if (active) {
            ctx.fillRect(left + insetX, top + insetY, right - left - 2 * insetX, bottom - top - 2 * insetY)
        } else {
            ctx.strokeRect(left + insetX, top + insetY, right - left - 2 * insetX, bottom - top - 2 * insetY)
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
                // A resize re-fits a fitted view and re-derives the request
                // region for a stated one; it never changes the artist's scale.
                onWidthChanged: viewerPanel.refreshView()
                onHeightChanged: viewerPanel.refreshView()
            }

            // Centered placeholder for "no frame": an unavailable target, an
            // unbound Read, or a failure before the first frame. The controller
            // owns the message, so a probe/render/color failure is never a
            // silent blank area.
            Text {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - 32)
                visible: viewerPanel.viewerDiagnostic.length > 0 && !viewerPanel.viewerShowsFrame
                objectName: "viewerUnavailable_" + viewerPanel.panelId
                text: viewerPanel.viewerDiagnostic
                color: viewerPanel.viewerDiagnosticColor
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            // A retained frame stays visible while a newer request is pending
            // or has failed; the message sits at the bottom instead of covering
            // the media the artist is inspecting.
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                width: Math.max(0, parent.width - 32)
                visible: viewerPanel.viewerDiagnostic.length > 0 && viewerPanel.viewerShowsFrame
                objectName: "viewerStatusLine_" + viewerPanel.panelId
                text: viewerPanel.viewerDiagnostic
                color: viewerPanel.viewerDiagnosticColor
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            // Viewport color picking (issue #102) states itself where the click
            // lands: the instruction while a pick is armed, and the reason
            // nothing was authored when the click or the sample is refused. The
            // picker owns the text, the panel only presents it, and it never
            // covers the media (it sits at the top edge, not over the centre).
            Rectangle {
                objectName: "viewerPickStatus_" + viewerPanel.panelId
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 8
                visible: viewerPanel.pickStatus.length > 0
                color: viewerPanel.pickStatusIsError
                       ? themeColor("errorSurface", "#5a2b2b")
                       : themeColor("header", "#212428")
                border.color: themeColor("border", "#30343a")
                radius: themeColor("smallRadius", 4)
                implicitWidth: pickStatusText.implicitWidth + 12
                implicitHeight: pickStatusText.implicitHeight + 6

                Text {
                    id: pickStatusText
                    anchors.centerIn: parent
                    text: viewerPanel.pickStatus
                    color: viewerPanel.pickStatusIsError
                           ? themeColor("errorText", "#f0d0d0")
                           : themeColor("text", "#dce0e6")
                    font.pixelSize: 11
                }
            }

            MouseArea {
                id: panArea
                anchors.fill: parent
                // The cursor states the box's handle under the pointer while an
                // overlay is present, and keeps the accepted pan cursor
                // otherwise. Hover is what makes that state follow the pointer.
                hoverEnabled: true
                cursorShape: {
                    // An armed viewport pick states the sampling cursor over the
                    // whole image area: that click samples, it never pans.
                    if (viewerPanel.pickArmed)
                        return viewerPanel.viewportSampler.picking ? Qt.BusyCursor : Qt.CrossCursor;
                    // A handle under the pointer states its own resize/move
                    // cursor; everywhere else the accepted pan cursor is kept.
                    if (viewerPanel.cropOverlay !== null) {
                        var handleCursor = viewerPanel.cropCursorShape(mouseX, mouseY);
                        if (handleCursor !== Qt.ArrowCursor)
                            return handleCursor;
                    }
                    if (viewerPanel.rotoActive) {
                        var rotoCursor = viewerPanel.rotoCursorShape(mouseX, mouseY);
                        if (rotoCursor !== Qt.ArrowCursor)
                            return rotoCursor;
                    }
                    return viewerPanel.imageScale() > viewerPanel.displayScale() ? Qt.OpenHandCursor : Qt.ArrowCursor;
                }
                enabled: viewerPanel.targetAvailable
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                onWheel: function(wheel) {
                    viewer.makePrimary();
                    // A wheel event only accumulates: one application per
                    // event-loop turn, anchored at the pointer, and no pan
                    // reset. Direction is monotonic and the deltas are
                    // symmetric, so the gesture is reversible.
                    var delta = wheel.pixelDelta && wheel.pixelDelta.y ? wheel.pixelDelta.y : (wheel.angleDelta.y / 120) * 53;
                    if (!delta)
                        return;
                    viewerPanel.queueZoom(Math.exp(delta * 0.002), wheel.x, wheel.y);
                    wheel.accepted = true;
                }
                onPressed: function(mouse) {
                    // An armed viewport pick owns the left press: the click
                    // samples the displayed target through the shared picker
                    // (which states its own refusals) and never starts a pan.
                    if (mouse.button === Qt.LeftButton && viewerPanel.pickArmed) {
                        forceActiveFocus();
                        mouse.accepted = true;
                        var picked = viewerPanel.imagePointAt(mouse.x, mouse.y);
                        viewerPanel.viewportSampler.sample(viewerPanel.controller,
                                                           picked !== null ? picked.x : NaN,
                                                           picked !== null ? picked.y : NaN);
                        return;
                    }
                    // A left press that lands on the box's own handle edits the
                    // authored crop box instead of panning; every other press
                    // (blank image area, middle button) keeps the pan gesture.
                    if (mouse.button === Qt.LeftButton && viewerPanel.cropOverlay !== null) {
                        var handle = viewerPanel.cropHandleAt(mouse.x, mouse.y);
                        if (handle.length > 0) {
                            forceActiveFocus();
                            mouse.accepted = true;
                            viewerPanel.panLastX = mouse.x;
                            viewerPanel.panLastY = mouse.y;
                            viewerPanel.beginCropGesture(handle, mouse.x, mouse.y);
                            return;
                        }
                    }
                    // Roto owns left-button selection and marquee. Middle-drag
                    // and the wheel keep the viewer's navigation behavior.
                    if (mouse.button === Qt.LeftButton && viewerPanel.rotoActive && viewerPanel.rotoOverlayItem
                            && viewerPanel.rotoOverlayItem.press(mouse.x, mouse.y, mouse.modifiers)) {
                        forceActiveFocus();
                        mouse.accepted = true;
                        viewerPanel.panLastX = mouse.x;
                        viewerPanel.panLastY = mouse.y;
                        return;
                    }
                    viewerPanel.panning = true;
                    viewerPanel.panLastX = mouse.x;
                    viewerPanel.panLastY = mouse.y;
                    forceActiveFocus();
                }
                onPositionChanged: function(mouse) {
                    if (viewerPanel.cropDragHandle.length > 0) {
                        viewerPanel.panLastX = mouse.x;
                        viewerPanel.panLastY = mouse.y;
                        viewerPanel.updateCropGesture(mouse.x, mouse.y);
                        return;
                    }
                    if (!viewerPanel.panning && viewerPanel.rotoActive && viewerPanel.rotoOverlayItem
                            && viewerPanel.rotoOverlayItem.move(mouse.x, mouse.y)) {
                        viewerPanel.panLastX = mouse.x;
                        viewerPanel.panLastY = mouse.y;
                        return;
                    }
                    if (!viewerPanel.panning)
                        return;
                    // Panning is an image drag at any scale; it moves the view,
                    // never the zoom, and it never becomes document state.
                    var scale = viewerPanel.imageScale();
                    if (scale <= 0)
                        return;
                    viewerPanel.viewPanX -= (mouse.x - viewerPanel.panLastX) / (scale * controller.pixelAspect);
                    viewerPanel.viewPanY -= (mouse.y - viewerPanel.panLastY) / scale;
                    viewerPanel.panLastX = mouse.x;
                    viewerPanel.panLastY = mouse.y;
                    viewerPanel.syncView();
                }
                onReleased: function(mouse) {
                    if (viewerPanel.cropDragHandle.length > 0) {
                        viewerPanel.finishCropGesture();
                        return;
                    }
                    if (!viewerPanel.panning && viewerPanel.rotoActive && viewerPanel.rotoOverlayItem
                            && viewerPanel.rotoOverlayItem.release(mouse.x, mouse.y))
                        return;
                    if (!viewerPanel.panning)
                        return;
                    viewerPanel.panning = false;
                    viewerPanel.saveView();
                }
                onCanceled: {
                    viewerPanel.panning = false;
                    if (viewerPanel.cropDragHandle.length > 0) {
                        viewerPanel.cancelCropGesture();
                        return;
                    }
                    if (viewerPanel.rotoActive && viewerPanel.rotoOverlayItem) {
                        viewerPanel.rotoOverlayItem.cancelAll();
                        return;
                    }
                    viewerPanel.panning = false;
                }
                onClicked: viewer.makePrimary()
            }

            // Crop box handles (issue #92, stories 43-44). This layer draws the
            // box over this panel's own image; the pan gesture above owns the
            // pointer, so a blank press still pans, middle/wheel are unchanged
            // and there is exactly ONE hit test for the handles.
            Item {
                id: cropHandleLayer
                objectName: "cropHandleLayer_" + viewerPanel.panelId
                anchors.fill: parent
                visible: viewerPanel.cropOverlay !== null

                Canvas {
                    id: cropCanvas
                    objectName: "cropBoxCanvas_" + viewerPanel.panelId
                    anchors.fill: parent
                    onPaint: viewerPanel.paintCropHandles(getContext("2d"), width, height)
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onVisibleChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                    Connections {
                        target: viewerPanel
                        function onThemeChanged() { cropCanvas.requestPaint() }
                        function onCropDragHandleChanged() { cropCanvas.requestPaint() }
                        function onViewZoomChanged() { cropCanvas.requestPaint() }
                        function onViewPanXChanged() { cropCanvas.requestPaint() }
                        function onViewPanYChanged() { cropCanvas.requestPaint() }
                        function onViewFittedChanged() { cropCanvas.requestPaint() }
                    }
                    Connections {
                        target: viewerPanel.controller
                        function onFrameArrived() { cropCanvas.requestPaint() }
                    }
                    Connections {
                        target: viewerPanel.theme
                        function onPresetChanged() { cropCanvas.requestPaint() }
                        function onAccentOverrideChanged() { cropCanvas.requestPaint() }
                    }
                }
            }

            // Roto authoring overlay (issue #93). It sits beside the crop
            // handles and never takes the pointer itself: the pan gesture above
            // delegates to it, so blank presses still pan, the middle drag and
            // the wheel are unchanged, and a Crop overlay is never affected.
            Loader {
                id: rotoOverlayLoader
                objectName: "rotoOverlayHost_" + viewerPanel.panelId
                anchors.fill: parent
                active: viewerPanel.rotoOverlayState !== null
                sourceComponent: rotoOverlayComponent
                onLoaded: viewerPanel.bindRotoController()

                Component {
                    id: rotoOverlayComponent
                    RotoOverlay {
                        panel: viewerPanel
                        theme: viewerPanel.theme
                        roto: viewerPanel.rotoController
                        panelId: viewerPanel.panelId
                    }
                }
            }

            Text {
                objectName: "rotoOverlayReason_" + viewerPanel.panelId
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 8
                visible: viewerPanel.rotoOverlayReason.length > 0
                text: viewerPanel.rotoOverlayReason
                color: viewerPanel.themeColor("muted", "#979ea8")
                font.pixelSize: 11
                wrapMode: Text.Wrap
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
        restoreForceFullFrame()
        refreshCropOverlay()
        refreshRotoOverlay()
    })
    onViewerIndexChanged: {
        activateViewer();
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onGraphRoleChanged: {
        if (graphRole)
            activateViewer();
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onVisibleChanged: if (visible && graphRole) activateViewer()

    // Resolved panel context is forwarded to this panel's own destination.
    onPanelContextChanged: {
        forwardContext();
        // A live box gesture belongs to the context it started in: another
        // group's/panel's inspection or target change withdraws the overlay and
        // discards the preview instead of committing it.
        if (cropGestureActive)
            cancelCropGesture();
        if (viewerPanel.rotoOverlayItem)
            viewerPanel.rotoOverlayItem.cancelAll();
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onViewerRoleChanged: {
        forwardContext();
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onRoutedClockChanged: forwardContext()

    Connections {
        target: viewerPanel.controller
        function onGraphChanged() {
            viewerPanel.graphRevision++
            // A crop box gesture is a live authored edit against the topology
            // it started on. Any other published change during the gesture
            // (a deletion, an upstream geometry change, a reopen, an Undo from
            // another owner) makes the frozen mapping stale, so the gesture is
            // cancelled instead of committing through it.
            if (viewerPanel.cropGestureActive)
                viewerPanel.cancelCropGesture();
            // A Roto draft or handle drag is the same kind of live edit against
            // the topology it started on: any published change (a deletion, an
            // Undo from another owner, a reopen) discards it uncommitted.
            if (viewerPanel.rotoOverlayItem)
                viewerPanel.rotoOverlayItem.cancelAll();
            viewerPanel.refreshCropOverlay();
            viewerPanel.refreshRotoOverlay();
        }
        // The described input geometry a reformat+intersect offset needs
        // arrives asynchronously from the worker, so the overlay is re-derived
        // when that answer advances.
        function onNodeChannelsChanged() {
            viewerPanel.cropEpoch++;
            viewerPanel.refreshCropOverlay();
            viewerPanel.refreshRotoOverlay();
        }
        // The session retired an interaction through its cancellation path.
        // When it was this panel's crop drag, the local drag ends with it.
        function onParameterEditEnded(token) {
            viewerPanel.retireCropGesture(String(token))
        }
    }
    onGraphRevisionChanged: {
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onCurrentFrameChanged: {
        if (cropGestureActive)
            cancelCropGesture();
        // The evaluated geometry of a Roto node is per frame, so a frame change
        // discards a draft or a live drag: the frame it was seeded at is gone.
        if (viewerPanel.rotoOverlayItem)
            viewerPanel.rotoOverlayItem.cancelAll();
        if (viewerPanel.rotoController)
            viewerPanel.rotoController.setViewerFrame(viewerPanel, viewerPanel.currentFrame);
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    onTargetAvailableChanged: {
        refreshCropOverlay();
        refreshRotoOverlay();
    }
    // Losing the panel's focus withdraws the transient authoring state: a draft
    // that can no longer receive Enter or Escape must not keep accepting clicks.
    onActiveFocusChanged: {
        if (!activeFocus && viewerPanel.rotoOverlayItem)
            viewerPanel.rotoOverlayItem.cancelAll();
    }

    Keys.onPressed: function(event) {
        if (frameField.activeFocus || timecodeField.activeFocus)
            return
        // Escape discards the one live box gesture: the authored values return
        // and the release that follows publishes nothing.
        if (event.key === Qt.Key_Escape && viewerPanel.cropGestureActive) {
            viewerPanel.cancelCropGesture();
            event.accepted = true;
            return
        }
        if (viewerPanel.rotoActive && viewerPanel.rotoOverlayItem
                && viewerPanel.rotoOverlayItem.handleKey(event.key, event.modifiers)) {
            event.accepted = true;
            return
        }
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
