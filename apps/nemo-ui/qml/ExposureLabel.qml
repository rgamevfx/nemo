import QtQuick
import QtQuick.Controls

// The parameter label cell, shared by generic and grouped rows so exposure
// dragging, the supported Alt-click keying shortcut and the edit-scope tooltip
// behave identically everywhere. The numeric editor never takes this gesture.
Item {
    id: exposureLabel

    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: ""
    property string labelText: ""
    property string keyStatus: "none"
    property int frame: 0

    signal keyRequested()

    implicitHeight: labelItem.implicitHeight
    property var dragPayload: ({})

    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction
    Drag.proposedAction: Qt.CopyAction
    Drag.mimeData: ({
            "application/x-nemo-parameter": JSON.stringify(exposureLabel.dragPayload)
        })

    function tooltipText() {
        var base = exposureLabel.parameterKey.startsWith("exposed:") ? "Exposed control. " : "Drag to edit exposed parameters. ";
        if (exposureLabel.keyStatus === "key")
            return base + "Keyed at frame " + exposureLabel.frame + ". Editing this value updates that key.";
        if (exposureLabel.keyStatus === "animated")
            return base + "Animated; no key at frame " + exposureLabel.frame + ". Alt-click to key there.";
        return base + "Alt-click to add a key at frame " + exposureLabel.frame;
    }

    Text {
        id: labelItem
        anchors.fill: parent
        objectName: exposureLabel.parameterKey.length > 0 ? "label_" + exposureLabel.nodeId + "_" + exposureLabel.parameterKey : ""
        text: exposureLabel.labelText.length > 0 ? exposureLabel.labelText : exposureLabel.parameterKey
        color: theme.text
        font.pixelSize: theme.fontSize
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        Accessible.name: text
    }

    // Alt-click keys at the current frame; a plain press is released so the
    // label stays selectable and the exposure drag can start.
    MouseArea {
        anchors.fill: parent
        onPressed: function (mouse) {
            mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
        }
        onClicked: exposureLabel.keyRequested()
    }

    DragHandler {
        id: labelDrag
        target: null
        acceptedButtons: Qt.LeftButton
        acceptedModifiers: Qt.NoModifier
        enabled: !exposureLabel.parameterKey.startsWith("exposed:")
        onActiveChanged: {
            if (!active) {
                exposureLabel.Drag.active = false;
                return;
            }
            exposureLabel.dragPayload = {
                "networkId": exposureLabel.networkId,
                "instanceId": exposureLabel.instanceId,
                "nodeId": exposureLabel.nodeId,
                "parameterKey": exposureLabel.parameterKey
            };
            exposureLabel.grabToImage(function (image) {
                    if (!labelDrag.active)
                        return;
                    exposureLabel.Drag.imageSource = image.url;
                    exposureLabel.Drag.active = true;
                });
        }
    }

    ToolTip.visible: labelHover.hovered
    ToolTip.text: tooltipText()
    HoverHandler {
        id: labelHover
    }
}
