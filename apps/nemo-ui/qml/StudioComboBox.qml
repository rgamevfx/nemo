import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    required property var theme

    implicitWidth: 78
    implicitHeight: 25
    font.pixelSize: theme.fontSize
    leftPadding: 8
    rightPadding: 22
    contentItem: Text {
        text: control.displayText
        font: control.font
        color: control.enabled ? control.theme.text : control.theme.disabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: control.down ? control.theme.hover
                            : control.hovered ? control.theme.raised : control.theme.field
        border.color: control.activeFocus ? control.theme.accent : control.theme.border
        radius: control.theme.smallRadius
    }
    indicator: Canvas {
        id: chevron
        x: control.width - width - 9
        y: (control.height - height) / 2
        width: 7
        height: 4
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = control.theme.muted;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(.5, .5);
            ctx.lineTo(3.5, 3.5);
            ctx.lineTo(6.5, .5);
            ctx.stroke();
        }
        Connections {
            target: control.theme
            function onMutedChanged() { chevron.requestPaint() }
        }
    }
    delegate: Component {
        ItemDelegate {
            id: entry
            required property int index
            required property var modelData
            width: control.popup.width - 8
            height: 27
            highlighted: control.highlightedIndex === index
            contentItem: Text {
                text: entry.modelData
                font: control.font
                color: control.theme.text
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: entry.highlighted ? control.theme.hover : "transparent"
                radius: control.theme.smallRadius
            }
        }
    }
    popup: Popup {
        y: control.height + 3
        width: Math.max(110, control.width)
        implicitHeight: contentItem.implicitHeight + 8
        padding: 4
        background: Rectangle {
            color: control.theme.header
            radius: control.theme.radius
            border.color: control.theme.border
        }
        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(250, contentHeight)
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
}
