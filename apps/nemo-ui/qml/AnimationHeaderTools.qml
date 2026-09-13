import QtQuick
import QtQuick.Controls

Item {
    id: headerTools

    required property var theme
    required property var editor

    implicitWidth: 102
    implicitHeight: 24
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
            radius: theme.smallRadius
            color: control.activeMode ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.15) : control.hovered ? theme.hover : "transparent"
            border.width: control.activeFocus ? 1 : 0
            border.color: theme.accent
            opacity: control.enabled ? 1 : 0.55
        }

        contentItem: Canvas {
            id: glyphCanvas
            anchors.fill: parent
            anchors.margins: 5
            antialiasing: true

            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                var foreground = !control.enabled ? theme.disabled : control.activeMode ? theme.accent : control.hovered ? theme.text : theme.muted;
                ctx.strokeStyle = foreground;
                ctx.fillStyle = foreground;
                ctx.lineWidth = 1.25;
                ctx.lineCap = "round";
                ctx.lineJoin = "round";
                var w = width;
                var h = height;
                var cx = w * 0.5;
                var cy = h * 0.5;
                function diamond(x, y, radius) {
                    ctx.beginPath();
                    ctx.moveTo(x, y - radius);
                    ctx.lineTo(x + radius, y);
                    ctx.lineTo(x, y + radius);
                    ctx.lineTo(x - radius, y);
                    ctx.closePath();
                    ctx.fill();
                }
                if (control.glyph === "track") {
                    ctx.beginPath();
                    ctx.moveTo(1.5, 3.5);
                    ctx.lineTo(w - 1.5, 3.5);
                    ctx.moveTo(1.5, cy);
                    ctx.lineTo(w - 1.5, cy);
                    ctx.moveTo(1.5, h - 3.5);
                    ctx.lineTo(w - 1.5, h - 3.5);
                    ctx.stroke();
                    diamond(w * 0.28, 3.5, 1.7);
                    diamond(w * 0.56, cy, 1.7);
                    diamond(w * 0.78, h - 3.5, 1.7);
                } else if (control.glyph === "curves") {
                    ctx.beginPath();
                    ctx.moveTo(1.5, h - 3.5);
                    ctx.bezierCurveTo(w * 0.27, 1.5, w * 0.63, 1.5, w - 1.5, h - 3.5);
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.arc(1.8, h - 3.5, 1.55, 0, Math.PI * 2);
                    ctx.arc(w - 1.8, h - 3.5, 1.55, 0, Math.PI * 2);
                    ctx.fill();
                    ctx.beginPath();
                    ctx.arc(cx, h * 0.34, 1.35, 0, Math.PI * 2);
                    ctx.fill();
                } else if (control.glyph === "frame") {
                    var left = 2.25;
                    var right = w - 2.25;
                    var top = 2.25;
                    var bottom = h - 2.25;
                    var corner = 3;
                    ctx.beginPath();
                    ctx.moveTo(left + corner, top);
                    ctx.lineTo(left, top);
                    ctx.lineTo(left, top + corner);
                    ctx.moveTo(right - corner, top);
                    ctx.lineTo(right, top);
                    ctx.lineTo(right, top + corner);
                    ctx.moveTo(left, bottom - corner);
                    ctx.lineTo(left, bottom);
                    ctx.lineTo(left + corner, bottom);
                    ctx.moveTo(right - corner, bottom);
                    ctx.lineTo(right, bottom);
                    ctx.lineTo(right, bottom - corner);
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.arc(cx, cy, 1.35, 0, Math.PI * 2);
                    ctx.fill();
                } else if (control.glyph === "menu") {
                    ctx.beginPath();
                    ctx.arc(cx - 4.1, cy, 1.2, 0, Math.PI * 2);
                    ctx.arc(cx, cy, 1.2, 0, Math.PI * 2);
                    ctx.arc(cx + 4.1, cy, 1.2, 0, Math.PI * 2);
                    ctx.fill();
                }
            }

            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }

        Connections {
            target: theme
            function onTextChanged() {
                glyphCanvas.requestPaint();
            }
            function onMutedChanged() {
                glyphCanvas.requestPaint();
            }
            function onDisabledChanged() {
                glyphCanvas.requestPaint();
            }
            function onAccentChanged() {
                glyphCanvas.requestPaint();
            }
            function onHoverChanged() {
                glyphCanvas.requestPaint();
            }
            function onSmallRadiusChanged() {
                glyphCanvas.requestPaint();
            }
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
        anchors.fill: parent
        spacing: 2

        GlyphButton {
            id: trackButton
            objectName: "animationTrackView"
            glyph: "track"
            activeMode: headerTools.editor.viewMode === "track"
            ToolTip.text: "Track view"
            Accessible.name: "Track view"
            onClicked: {
                headerTools.editor.viewMode = "track";
                headerTools.editor.forceActiveFocus();
            }
        }

        GlyphButton {
            id: curvesButton
            objectName: "animationCurvesView"
            glyph: "curves"
            activeMode: headerTools.editor.viewMode === "curves"
            ToolTip.text: "Curves view"
            Accessible.name: "Curves view"
            onClicked: {
                headerTools.editor.viewMode = "curves";
                headerTools.editor.forceActiveFocus();
            }
        }

        GlyphButton {
            id: frameButton
            objectName: "animationFrameAll"
            glyph: "frame"
            ToolTip.text: "Frame all (Home); Shift-click: frame selected (F)"
            Accessible.name: "Frame all. Shift-click to frame selected. Home and F shortcuts."
            onClicked: {
                headerTools.editor.frameAll();
                headerTools.editor.forceActiveFocus();
            }

            MouseArea {
                id: frameModifierArea
                anchors.fill: parent
                z: 2
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                property bool shiftClick: false

                onPressed: function (mouse) {
                    shiftClick = !!(mouse.modifiers & Qt.ShiftModifier);
                    mouse.accepted = shiftClick;
                }
                onClicked: function (mouse) {
                    if (shiftClick) {
                        headerTools.editor.frameSelected();
                        headerTools.editor.forceActiveFocus();
                        mouse.accepted = true;
                    } else {
                        mouse.accepted = false;
                    }
                    shiftClick = false;
                }
                onCanceled: shiftClick = false
            }
        }

        GlyphButton {
            id: menuButton
            objectName: "animationEditorMenu"
            glyph: "menu"
            ToolTip.text: "Animation editor menu"
            Accessible.name: "Animation editor menu"
            onClicked: headerTools.editor.openEditorMenu(menuButton)
        }
    }
}
