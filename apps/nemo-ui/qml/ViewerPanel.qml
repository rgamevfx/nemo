import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Native Vulkan viewer panel content (issue #11). `viewerController` is the
// GUI-thread viewer facade context property: its explicitly composed
// ProjectSession owns the Document and command history (mutated only through
// the command API), while the controller drives the render worker and exposes
// source/status/error/view state. `viewer` is the native QSG item that
// imports the GPU presentation image; the panel computes the aspect-fit
// display rectangle (zoom/pan in image pixels, clamped so the image center
// stays inside the pane) and the resolution/zoom controls re-issue requests.
Rectangle {
    id: viewerPanel

    // Set by the shared Panel shell for every restored instance.
    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    readonly property var controller: viewerController
    readonly property bool hasImage: controller.hasSource && controller.sourceSize.width > 0
    readonly property real sourceWidth: hasImage ? controller.sourceSize.width : 0
    readonly property real sourceHeight: hasImage ? controller.sourceSize.height : 0

    color: "#1a1a1a"
    objectName: "viewerPanel"

    function displayScale() {
        if (sourceWidth <= 0 || sourceHeight <= 0 || viewer.width <= 0 || viewer.height <= 0)
            return 1;
        return Math.min(viewer.width / (sourceWidth * controller.pixelAspect), viewer.height / sourceHeight);
    }

    function computeDisplayRect() {
        if (!hasImage)
            return Qt.rect(0, 0, 0, 0);
        var s = displayScale() * controller.zoom;
        var sx = s * controller.pixelAspect;
        var visibleW = Math.min(sourceWidth, viewer.width / sx);
        var visibleH = Math.min(sourceHeight, viewer.height / s);
        var cx = Math.max(visibleW / 2, Math.min(sourceWidth - visibleW / 2, sourceWidth / 2 + controller.pan.x));
        var cy = Math.max(visibleH / 2, Math.min(sourceHeight - visibleH / 2, sourceHeight / 2 + controller.pan.y));
        var region = controller.presentedRegion;
        return Qt.rect(viewer.width / 2 + (region.x - cx) * sx, viewer.height / 2 + (region.y - cy) * s, region.width * sx, region.height * s);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Toolbar: source loading, frame, resolution mode, zoom.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 62
            color: "#333333"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 3
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: sourceInput
                        objectName: "viewerSourceInput"
                        Layout.fillWidth: true
                        implicitHeight: 26
                        placeholderText: "media path (e.g. /path/to/clip.mkv)"
                        font.pixelSize: 11
                        selectByMouse: true
                    }
                    Button {
                        id: sourceLoad
                        objectName: "viewerSourceLoad"
                        text: "Load"
                        implicitHeight: 24
                        implicitWidth: 48
                        font.pixelSize: 11
                        onClicked: viewerPanel.controller.openSource(sourceInput.text.trim())
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    SpinBox {
                        id: frameSelector
                        objectName: "viewerFrame"
                        implicitHeight: 24
                        implicitWidth: 96
                        font.pixelSize: 11
                        from: 0
                        to: viewerPanel.controller.frameCount > 0 ? viewerPanel.controller.frameCount - 1 : 9999
                        value: viewerPanel.controller.frame
                        onValueModified: viewerPanel.controller.setFrame(value)
                        editable: true
                    }
                    Slider {
                        id: frameScrubber
                        objectName: "viewerPlayhead"
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, viewerPanel.controller.frameCount > 0 ? viewerPanel.controller.frameCount - 1 : 239)
                        value: viewerPanel.controller.frame
                        onMoved: viewerPanel.controller.setFrame(Math.round(value))
                    }
                    ComboBox {
                        id: modeSelector
                        objectName: "viewerMode"
                        implicitHeight: 24
                        implicitWidth: 96
                        font.pixelSize: 11
                        model: ["Auto", "Full", "Half", "Quarter"]
                        currentIndex: viewerPanel.controller.resolutionMode === "full" ? 1 : viewerPanel.controller.resolutionMode === "half" ? 2 : viewerPanel.controller.resolutionMode === "quarter" ? 3 : 0
                        onActivated: viewerPanel.controller.setResolutionMode(currentText.toLowerCase())
                        ToolTip.visible: hovered
                        ToolTip.text: "Sampling resolution: Auto derives a stable level from image area, aspect and zoom; Full/Half/Quarter are explicit reductions."
                    }
                    Text {
                        objectName: "viewerZoomLabel"
                        text: Math.round(viewerPanel.controller.zoom * 100) + "% · 1:" + viewerPanel.controller.effectiveScale
                        color: "#9a9a9a"
                        font.pixelSize: 11
                    }
                    Button {
                        id: resetView
                        objectName: "viewerResetView"
                        text: "Fit"
                        implicitHeight: 24
                        implicitWidth: 40
                        font.pixelSize: 11
                        onClicked: viewerPanel.controller.resetView()
                        ToolTip.visible: hovered
                        ToolTip.text: "Reset zoom to aspect fit and clear pan."
                    }
                    Button {
                        objectName: "viewerCancel"
                        text: "Cancel"
                        enabled: viewerPanel.controller.pending || viewerPanel.controller.queued > 0
                        onClicked: viewerPanel.controller.cancelRender()
                    }
                }
            }
        }

        // Native presentation surface. Wheel zooms around the pane center,
        // drag pans when zoomed in.
        Item {
            id: surface
            Layout.fillWidth: true
            Layout.fillHeight: true

            ViewerItem {
                id: viewer
                objectName: "viewerItem"
                anchors.fill: parent
                clip: true
                controller: viewerPanel.controller
                displayRect: viewerPanel.computeDisplayRect()
            }


            MouseArea {
                id: panArea
                anchors.fill: parent
                cursorShape: viewerPanel.controller.zoom > 1 ? Qt.OpenHandCursor : Qt.ArrowCursor
                property real lastX: 0
                property real lastY: 0
                onWheel: function (wheel) {
                    viewer.makePrimary();
                    viewerPanel.controller.zoomBy(Math.pow(1.15, wheel.angleDelta.y / 120));
                    wheel.accepted = true;
                }
                onPressed: function (mouse) {
                    lastX = mouse.x;
                    lastY = mouse.y;
                    forceActiveFocus();
                }
                onPositionChanged: function (mouse) {
                    if (!pressed || viewerPanel.controller.zoom <= 1)
                        return;
                    var s = viewerPanel.displayScale() * viewerPanel.controller.zoom;
                    var dx = (mouse.x - lastX) / s;
                    var dy = (mouse.y - lastY) / s;
                    lastX = mouse.x;
                    lastY = mouse.y;
                    viewerPanel.controller.setPan(Qt.point(viewerPanel.controller.pan.x - dx, viewerPanel.controller.pan.y - dy));
                }
                onClicked: viewer.makePrimary()
            }
        }

        // Status / error line. Errors are the worker's precise, verbatim
        // diagnostics (decode interpretation, GPU, evaluation) — never
        // paraphrased or retried silently.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: "#242424"
            Text {
                id: statusText
                objectName: "viewerStatus"
                anchors.fill: parent
                anchors.margins: 4
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                text: "[" + viewerPanel.controller.renderState + "] " + (viewerPanel.controller.error.length > 0 ? viewerPanel.controller.error : (viewerPanel.controller.status.length > 0 ? viewerPanel.controller.status : "no source loaded"))
                color: viewerPanel.controller.error.length > 0 ? "#f0b0b0" : viewerPanel.controller.outdated ? "#e7ba76" : "#8a8a8a"
                font.pixelSize: 11
            }
        }
    }
}
