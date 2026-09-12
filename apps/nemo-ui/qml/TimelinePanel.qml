import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// Production timeline presentation. The native item draws bounded source
// references; it is deliberately not an editorial occurrence model. Seeking,
// source timing edits, cache admission and render history remain owned by the
// existing routed controller.
Pane {
    id: timelinePanel
    objectName: "timelinePanel"

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var theme: null
    property var workspace: null

    readonly property var controller: viewerController
    readonly property int timelineLength: controller.frameCount > 0 ? controller.frameCount : 240
    readonly property string selectedSource: timelineItem.selectedSource
    readonly property var sourceNames: {
        var result = []
        for (var i = 0; i < controller.timelineClips.length; ++i) {
            var clip = controller.timelineClips[i]
            if (clip)
                result.push(String(clip.source || clip.id || "Source"))
        }
        return result
    }
    readonly property real routedClock: contextRouter && panelContext && panelContext.timelineClock !== undefined
                                        ? Number(panelContext.timelineClock) : controller.frame
    readonly property int sourceRulerHeight: 28
    readonly property int sourceRowPitch: 40

    padding: 0
    font.pixelSize: theme ? theme.fontSize : 11
    background: Rectangle { color: timelinePanel.theme ? timelinePanel.theme.background : "#181a1d" }

    // Panel.qml loads this component into the shared compact panel header.
    // The compact header mirrors the prototype affordance; unsupported native
    // editor mutations remain visibly disabled until their owner is available.
    property Component headerTools: Component {
        Item {
            id: timelineHeaderTools
            implicitHeight: 24
            implicitWidth: toolsRow.implicitWidth

            property var theme: timelinePanel.theme

            component GlyphButton: Button {
                id: control
                property string glyph: ""
                property bool activeMode: false
                implicitWidth: 24
                implicitHeight: 24
                width: 24
                height: 24
                padding: 0
                flat: true
                background: Rectangle {
                    radius: timelinePanel.theme ? timelinePanel.theme.smallRadius : 4
                    color: control.activeMode
                           ? Qt.rgba((timelinePanel.theme ? timelinePanel.theme.accent : "#3485f6").r,
                                     (timelinePanel.theme ? timelinePanel.theme.accent : "#3485f6").g,
                                     (timelinePanel.theme ? timelinePanel.theme.accent : "#3485f6").b, 0.15)
                           : control.hovered
                             ? (timelinePanel.theme ? timelinePanel.theme.hover : "#343940")
                             : "transparent"
                    border.width: control.activeFocus ? 1 : 0
                    border.color: timelinePanel.theme ? timelinePanel.theme.accent : "#3485f6"
                    opacity: control.enabled ? 1 : 0.55
                }
                contentItem: Canvas {
                    id: glyphCanvas
                    anchors.fill: parent
                    anchors.margins: 5
                    antialiasing: true
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        var muted = timelinePanel.theme ? timelinePanel.theme.muted : "#979ea8"
                        var disabled = timelinePanel.theme ? timelinePanel.theme.disabled : "#626a75"
                        var accent = timelinePanel.theme ? timelinePanel.theme.accent : "#3485f6"
                        var text = timelinePanel.theme ? timelinePanel.theme.text : "#dce0e6"
                        var foreground = !control.enabled ? disabled : control.activeMode ? accent
                                                                                           : control.hovered ? text : muted
                        ctx.strokeStyle = foreground
                        ctx.fillStyle = foreground
                        ctx.lineWidth = 1.25
                        ctx.lineCap = "round"
                        ctx.lineJoin = "round"
                        var w = width
                        var h = height
                        var cx = w * 0.5
                        var cy = h * 0.5
                        if (control.glyph === "select") {
                            ctx.beginPath()
                            ctx.moveTo(w * .25, h * .14)
                            ctx.lineTo(w * .72, h * .55)
                            ctx.lineTo(w * .51, h * .58)
                            ctx.lineTo(w * .62, h * .87)
                            ctx.lineTo(w * .51, h * .91)
                            ctx.lineTo(w * .39, h * .61)
                            ctx.lineTo(w * .25, h * .75)
                            ctx.closePath()
                            ctx.fill()
                        } else if (control.glyph === "ripple") {
                            ctx.beginPath()
                            ctx.moveTo(w * .18, h * .32)
                            ctx.lineTo(w * .82, h * .32)
                            ctx.moveTo(w * .18, h * .68)
                            ctx.lineTo(w * .82, h * .68)
                            ctx.moveTo(w * .18, h * .32)
                            ctx.lineTo(w * .28, h * .22)
                            ctx.moveTo(w * .18, h * .32)
                            ctx.lineTo(w * .28, h * .42)
                            ctx.moveTo(w * .82, h * .68)
                            ctx.lineTo(w * .72, h * .58)
                            ctx.moveTo(w * .82, h * .68)
                            ctx.lineTo(w * .72, h * .78)
                            ctx.stroke()
                        } else if (control.glyph === "roll") {
                            ctx.strokeRect(w * .12, h * .30, w * .30, h * .40)
                            ctx.strokeRect(w * .58, h * .30, w * .30, h * .40)
                            ctx.beginPath()
                            ctx.moveTo(w * .42, cy)
                            ctx.lineTo(w * .58, cy)
                            ctx.stroke()
                            ctx.beginPath()
                            ctx.moveTo(w * .48, h * .42)
                            ctx.lineTo(w * .42, cy)
                            ctx.lineTo(w * .48, h * .58)
                            ctx.moveTo(w * .52, h * .42)
                            ctx.lineTo(w * .58, cy)
                            ctx.lineTo(w * .52, h * .58)
                            ctx.stroke()
                        } else if (control.glyph === "slip") {
                            ctx.strokeRect(w * .25, h * .27, w * .50, h * .46)
                            ctx.beginPath()
                            ctx.moveTo(w * .10, cy)
                            ctx.lineTo(w * .36, cy)
                            ctx.moveTo(w * .10, cy)
                            ctx.lineTo(w * .20, h * .40)
                            ctx.moveTo(w * .10, cy)
                            ctx.lineTo(w * .20, h * .60)
                            ctx.moveTo(w * .90, cy)
                            ctx.lineTo(w * .64, cy)
                            ctx.moveTo(w * .90, cy)
                            ctx.lineTo(w * .80, h * .40)
                            ctx.moveTo(w * .90, cy)
                            ctx.lineTo(w * .80, h * .60)
                            ctx.stroke()
                        } else if (control.glyph === "slide") {
                            ctx.beginPath()
                            ctx.moveTo(w * .18, cy)
                            ctx.lineTo(w * .82, cy)
                            ctx.moveTo(w * .18, cy)
                            ctx.lineTo(w * .30, h * .38)
                            ctx.moveTo(w * .18, cy)
                            ctx.lineTo(w * .30, h * .62)
                            ctx.moveTo(w * .82, cy)
                            ctx.lineTo(w * .70, h * .38)
                            ctx.moveTo(w * .82, cy)
                            ctx.lineTo(w * .70, h * .62)
                            ctx.stroke()
                            ctx.strokeRect(w * .40, h * .32, w * .20, h * .36)
                        } else if (control.glyph === "blade") {
                            ctx.beginPath()
                            ctx.moveTo(w * .24, h * .78)
                            ctx.lineTo(w * .72, h * .22)
                            ctx.stroke()
                            ctx.beginPath()
                            ctx.arc(w * .28, h * .75, w * .14, 0, Math.PI * 2)
                            ctx.stroke()
                            ctx.beginPath()
                            ctx.moveTo(w * .57, h * .37)
                            ctx.lineTo(w * .77, h * .57)
                            ctx.stroke()
                        } else if (control.glyph === "snap") {
                            ctx.beginPath()
                            ctx.moveTo(w * .18, cy)
                            ctx.lineTo(w * .82, cy)
                            ctx.moveTo(cx, h * .18)
                            ctx.lineTo(cx, h * .82)
                            ctx.stroke()
                            ctx.beginPath()
                            ctx.arc(cx, cy, w * .18, 0, Math.PI * 2)
                            ctx.stroke()
                        } else if (control.glyph === "link") {
                            ctx.strokeRect(w * .12, h * .42, w * .42, h * .27)
                            ctx.strokeRect(w * .46, h * .31, w * .42, h * .27)
                            ctx.beginPath()
                            ctx.moveTo(w * .38, h * .55)
                            ctx.lineTo(w * .62, h * .45)
                            ctx.stroke()
                        } else if (control.glyph === "frame") {
                            ctx.beginPath()
                            ctx.moveTo(w * .18, h * .35)
                            ctx.lineTo(w * .18, h * .18)
                            ctx.lineTo(w * .35, h * .18)
                            ctx.moveTo(w * .65, h * .18)
                            ctx.lineTo(w * .82, h * .18)
                            ctx.lineTo(w * .82, h * .35)
                            ctx.moveTo(w * .18, h * .65)
                            ctx.lineTo(w * .18, h * .82)
                            ctx.lineTo(w * .35, h * .82)
                            ctx.moveTo(w * .65, h * .82)
                            ctx.lineTo(w * .82, h * .82)
                            ctx.lineTo(w * .82, h * .65)
                            ctx.stroke()
                            ctx.beginPath()
                            ctx.arc(cx, cy, 1.35, 0, Math.PI * 2)
                            ctx.fill()
                        } else if (control.glyph === "media") {
                            ctx.strokeRect(w * .14, h * .21, w * .72, h * .58)
                            ctx.beginPath()
                            ctx.moveTo(w * .25, h * .35)
                            ctx.lineTo(w * .75, h * .35)
                            ctx.moveTo(w * .25, h * .52)
                            ctx.lineTo(w * .62, h * .52)
                            ctx.moveTo(w * .25, h * .67)
                            ctx.lineTo(w * .50, h * .67)
                            ctx.stroke()
                        } else if (control.glyph === "menu") {
                            ctx.beginPath()
                            ctx.arc(cx - 4.1, cy, 1.2, 0, Math.PI * 2)
                            ctx.arc(cx, cy, 1.2, 0, Math.PI * 2)
                            ctx.arc(cx + 4.1, cy, 1.2, 0, Math.PI * 2)
                            ctx.fill()
                        }
                    }
                    Component.onCompleted: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                }
                Connections {
                    target: timelinePanel.theme
                    function onTextChanged() { glyphCanvas.requestPaint() }
                    function onMutedChanged() { glyphCanvas.requestPaint() }
                    function onDisabledChanged() { glyphCanvas.requestPaint() }
                    function onAccentChanged() { glyphCanvas.requestPaint() }
                    function onHoverChanged() { glyphCanvas.requestPaint() }
                    function onSmallRadiusChanged() { glyphCanvas.requestPaint() }
                }
                onGlyphChanged: glyphCanvas.requestPaint()
                onActiveModeChanged: glyphCanvas.requestPaint()
                onHoveredChanged: glyphCanvas.requestPaint()
                onEnabledChanged: glyphCanvas.requestPaint()
                ToolTip.visible: hovered
                ToolTip.delay: 450
                ToolTip.text: ""
            }

            Row {
                id: toolsRow
                anchors.fill: parent
                spacing: 2
                GlyphButton {
                    id: toolButton
                    objectName: "timelineToolButton"
                    glyph: "select"
                    enabled: false
                    ToolTip.text: "Editing tools are not available in the native timeline yet"
                    Accessible.name: "Editing tool menu"
                    onClicked: toolMenu.open()
                }
                GlyphButton {
                    id: snapButton
                    objectName: "timelineSnapButton"
                    glyph: "snap"
                    enabled: false
                    ToolTip.text: "Timeline snapping is not available in the native timeline yet"
                    Accessible.name: "Toggle timeline snapping"
                }
                GlyphButton {
                    id: linkButton
                    objectName: "timelineLinkButton"
                    glyph: "link"
                    enabled: false
                    ToolTip.text: "Linked selection is not available in the native timeline yet"
                    Accessible.name: "Toggle linked selection"
                }
                GlyphButton {
                    id: frameButton
                    objectName: "timelineFrameButton"
                    glyph: "frame"
                    enabled: false
                    ToolTip.text: "Frame controls are available from the timeline ruler"
                    Accessible.name: "Frame timeline"
                }
                GlyphButton {
                    id: mediaButton
                    objectName: "timelineMediaButton"
                    glyph: "media"
                    // Revealing the matching-group Media Bin is presentation
                    // only and independent of editorial insertion: no clip is
                    // created here and editing stays unavailable until #54.
                    enabled: typeof mediaLibrary !== "undefined" && mediaLibrary !== null
                    ToolTip.text: "Open the Media Bin for this panel's group"
                    Accessible.name: "Open media bin"
                    onClicked: {
                        if (typeof mediaLibrary !== "undefined" && mediaLibrary !== null)
                            mediaLibrary.revealMediaPanel(timelinePanel.panelGroup)
                    }
                }
                GlyphButton {
                    id: menuButton
                    objectName: "timelineMenuButton"
                    glyph: "menu"
                    ToolTip.text: "Timeline editor menu"
                    Accessible.name: "Timeline editor menu"
                    onClicked: toolMenu.open()
                }
            }

            Menu {
                id: toolMenu
                objectName: "timelineToolMenu"
                title: "Editing tool"
                MenuItem {
                    text: "Select                         V"
                    enabled: false
                }
                MenuItem {
                    text: "Ripple                         R"
                    enabled: false
                }
                MenuItem {
                    text: "Roll                           T"
                    enabled: false
                }
                MenuItem {
                    text: "Slip                           Y"
                    enabled: false
                }
                MenuItem {
                    text: "Slide                          U"
                    enabled: false
                }
                MenuItem {
                    text: "Blade                          B"
                    enabled: false
                }
            }
        }
    }

    function updateClock(value) {
        var frame = Math.round(value)
        if (contextRouter && panelGroup.length > 0)
            contextRouter.setGroupContext(panelGroup, {timelineClock: frame})
        controller.setFrame(frame)
    }

    // Issue #43 acceptance 5: the media drag payload is the ordered occurrence
    // list (sourceIds with their parallel marks and requested mode) plus the
    // media panel group that produced it. A drop is only an insertion intent
    // when that payload is complete, so a blank, foreign or catalog-internal
    // drag (a bin row or anything without media sources) emits nothing. The
    // origin group is read for validation only: the insertion targets this
    // timeline's group, and the origin panel never hands over its own mark
    // state. Neither the playhead nor the Document is touched here.
    function mediaInsertPayload(drag) {
        var source = drag && drag.source ? drag.source : null
        if (!source || !source.sourceIds || source.sourceIds.length === undefined)
            return null
        var sourceIds = []
        for (var i = 0; i < source.sourceIds.length; ++i) {
            var sourceId = source.sourceIds[i]
            if (sourceId === undefined || sourceId === null || String(sourceId).length === 0)
                return null
            sourceIds.push(String(sourceId))
        }
        if (sourceIds.length === 0)
            return null
        // The payload's origin group is the media panel's group; the drag source
        // object names it `group` (payload vocabulary) or `sourceGroup` (the
        // delegate exposure). Either way it is only a completeness check.
        var originGroup = source.group !== undefined ? source.group : source.sourceGroup
        if (!originGroup || String(originGroup).length === 0)
            return null
        var marks = source.marks && source.marks.length !== undefined ? source.marks : []
        var mode = source.mode !== undefined && String(source.mode).length > 0 ? String(source.mode) : "insert"
        return {
            "sourceIds": sourceIds,
            "marks": marks,
            "mode": mode
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 2


        Flickable {
            id: timelineScroll
            objectName: "timelineSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 24
            clip: true
            contentWidth: timelineContent.width
            contentHeight: timelineContent.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

            Item {
                id: timelineContent
                width: Math.max(timelineScroll.width, 440)
                height: timelineItem.implicitHeight

                TimelineItem {
                    id: timelineItem
                    objectName: "timelineRuler"
                    anchors.fill: parent
                    clips: controller.timelineClips
                    frame: timelinePanel.routedClock
                    frameCount: controller.frameCount
                    viewportY: timelineScroll.contentY
                    viewportHeight: timelineScroll.height
                    backgroundColor: timelinePanel.theme ? timelinePanel.theme.background : "#181a1d"
                    panelColor: timelinePanel.theme ? timelinePanel.theme.panel : "#1e2023"
                    headerColor: timelinePanel.theme ? timelinePanel.theme.header : "#212428"
                    borderColor: timelinePanel.theme ? timelinePanel.theme.border : "#30343a"
                    accentColor: timelinePanel.theme ? timelinePanel.theme.accent : "#3485f6"
                    raisedColor: timelinePanel.theme ? timelinePanel.theme.raised : "#282c31"
                    onFrameSelected: function(frameValue) { timelinePanel.updateClock(frameValue) }
                }

                Repeater {
                    model: timelinePanel.sourceNames
                    Text {
                        objectName: "timelineSourceLabel_" + index
                        x: 7
                        y: timelinePanel.sourceRulerHeight + index * timelinePanel.sourceRowPitch + 5
                        width: 118
                        height: 18
                        text: modelData
                        color: timelinePanel.theme ? timelinePanel.theme.text : "#dce0e6"
                        font.pixelSize: timelinePanel.theme ? timelinePanel.theme.fontSize : 11
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Repeater {
                    model: 9
                    Text {
                        x: 132 + (timelineContent.width - 132) * index / 8 - 24
                        y: 4
                        width: 48
                        height: 16
                        text: Math.round((timelinePanel.timelineLength - 1) * index / 8)
                        color: timelinePanel.theme ? timelinePanel.theme.muted : "#979ea8"
                        font.pixelSize: 9
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }

                // Drop target for the media insertion intent. It spans the
                // visible timeline surface (the content rect, or the viewport
                // when the content is shorter, so it never scrolls out of
                // reach), draws nothing and takes no mouse gesture: seeking,
                // row selection, labels and flicking are unchanged. Only the
                // media drag keys enter it; the payload is validated on drop
                // and the drop is accepted only after requestTimelineInsert
                // reports success, so one release emits at most one intent and
                // a blank/foreign/catalog-internal drag emits none.
                DropArea {
                    id: timelineInsertDrop
                    objectName: "timelineInsertDrop"
                    x: 0
                    y: 0
                    width: timelineContent.width
                    height: Math.max(timelineContent.height, timelineScroll.height)
                    keys: ["application/x-nemo-source", "application/x-nemo-source-id"]
                    onDropped: function (drop) {
                        drop.accepted = false
                        var payload = timelinePanel.mediaInsertPayload(drop)
                        if (!payload)
                            return
                        if (typeof mediaLibrary === "undefined" || mediaLibrary === null
                                || !mediaLibrary.requestTimelineInsert)
                            return
                        if (mediaLibrary.requestTimelineInsert(timelinePanel.panelGroup, payload.sourceIds,
                                                               payload.mode, payload.marks))
                            drop.accept(Qt.CopyAction)
                    }
                }
            }
        }


    }
}
