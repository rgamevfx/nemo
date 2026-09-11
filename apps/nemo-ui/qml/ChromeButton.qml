import QtQuick
import QtQuick.Controls

Button {
    id: control

    required property var theme

    implicitHeight: 28
    padding: 8
    font.pixelSize: theme.fontSize
    background: Rectangle {
        radius: 5
        color: control.down ? control.theme.raised
                            : control.hovered ? control.theme.hover : "transparent"
    }
}
