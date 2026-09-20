import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    required property var theme
    // Optional presentation metrics (issue #102). The defaults are this
    // control's accepted appearance, so no unrelated consumer changes; an
    // inspector row passes its own readable metrics explicitly.
    property int controlHeight: 25
    property int textSize: theme.fontSize
    // A typeable combo states a live value beside its presets: `readout` is
    // shown while the artist is not editing, and the text they type or pick is
    // reported through textAccepted. A plain combo is selection-only.
    property bool typeable: false
    property string readout: ""
    property bool typing: false
    signal textAccepted(string text)

    editable: typeable
    implicitWidth: 78
    implicitHeight: controlHeight
    font.pixelSize: textSize
    leftPadding: 8
    rightPadding: 22

    // The live value is the field's text whenever the artist is not editing it,
    // so a gesture-driven change (a wheel, a preset) is stated immediately. The
    // field is the control's stated value: a stated scale that matches no preset
    // has no preset index to report.
    function syncReadout() {
        if (!typeable || typing)
            return;
        var preset = model && model.indexOf ? model.indexOf(readout) : -1;
        if (currentIndex !== preset)
            currentIndex = preset;
        editText = readout;
    }
    onReadoutChanged: syncReadout()
    // Qt resets the current item when an asynchronous preset model changes.
    // Restore the authored readout after that reset, without interrupting typing.
    onModelChanged: if (typeable) Qt.callLater(syncReadout)
    onTypingChanged: if (!typing) syncReadout()

    // Commits the text the artist stated, before the control returns to its
    // live value (or the commit would carry the value it replaces).
    function commitTyped() {
        if (!typing)
            return;
        var stated = editText;
        typing = false;
        textAccepted(stated);
    }

    contentItem: Loader {
        sourceComponent: control.typeable ? typedField : readoutText
    }
    Component {
        id: readoutText
        Text {
            text: control.displayText
            font: control.font
            color: control.enabled ? control.theme.text : control.theme.disabled
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    Component {
        id: typedField
        TextField {
            objectName: control.objectName + "Field"
            text: control.editText
            font: control.font
            color: control.enabled ? control.theme.text : control.theme.disabled
            verticalAlignment: Text.AlignVCenter
            selectByMouse: true
            background: null
            leftPadding: 0
            rightPadding: 0
            onTextEdited: {
                control.typing = true;
                control.editText = text;
            }
            // Return fires both accepted and editingFinished; the first one
            // commits and the control leaves the editing state, so a typed
            // value is committed exactly once.
            onAccepted: control.commitTyped()
            onEditingFinished: control.commitTyped()
            Component.onCompleted: control.editText = control.readout
        }
    }
    background: Rectangle {
        color: control.down ? control.theme.hover
                            : control.hovered ? control.theme.raised : control.theme.field
        border.color: control.activeFocus ? control.theme.accent : control.theme.border
        radius: control.theme.smallRadius
    }
    // The arrow is the preset menu's target on a typeable control (Qt's
    // editable-combo convention: the body belongs to the field), so its click
    // area is a comfortable button rather than the glyph alone. A plain combo
    // keeps the accepted arrow geometry: the glyph sits 9 px from the edge.
    indicator: Item {
        id: arrow
        x: control.width - width - 2
        y: (control.height - height) / 2
        implicitWidth: 7
        implicitHeight: 4
        width: control.typeable ? 20 : 7
        height: control.typeable ? control.height : 4
        Canvas {
            id: chevron
            anchors.centerIn: parent
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
