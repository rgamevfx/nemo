import QtQuick
import QtQuick.Controls

Button {
    id: control
    required property var theme
    property bool pinned: false
    flat: true
    implicitWidth: 22
    implicitHeight: 23
    padding: 0
    contentItem: Canvas {
        id: glyph
        readonly property color tint: control.theme ? (control.pinned ? control.theme.accent : control.theme.muted) : "transparent"
        onTintChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = tint;
            ctx.fillStyle = tint;
            ctx.lineWidth = 1;
            var cx = width / 2, cy = height / 2;
            ctx.beginPath();
            ctx.moveTo(cx - 3, cy - 5);
            ctx.lineTo(cx + 3, cy - 5);
            ctx.lineTo(cx + 2, cy - 1);
            ctx.lineTo(cx + 4, cy + 2);
            ctx.lineTo(cx - 4, cy + 2);
            ctx.lineTo(cx - 2, cy - 1);
            ctx.closePath();
            if (control.pinned)
                ctx.fill();
            else
                ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(cx, cy + 2);
            ctx.lineTo(cx, cy + 6);
            ctx.stroke();
        }
    }
    onPinnedChanged: glyph.requestPaint()
}
