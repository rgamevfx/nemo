import QtQuick
import QtQuick.Controls

// The one aligned animation column, shared by generic and grouped parameter
// rows. It reads only presentation state and submits the existing keying
// commands through the panel; it never touches the Document.
Rectangle {
    id: keyIndicator

    property var theme: null
    property string networkId: ""
    property string nodeId: ""
    property string parameterKey: ""
    property string parameterLabel: ""
    // static "none", "animated" (between keys) or "key" (keyed at this frame).
    property string keyStatus: "none"
    // The typed-parameter scope a key action applies to, e.g. RGBA / XYZ.
    property string scope: ""
    property int frame: 0
    // False when this group has no Animation panel to reveal into.
    property bool revealAvailable: false

    signal keyRequested()
    signal removeKeyRequested()
    signal revealRequested()

    implicitWidth: 24
    implicitHeight: 22
    radius: theme.smallRadius
    color: indicatorMouse.containsMouse ? theme.hover : "transparent"
    border.width: 1
    border.color: keyStatus === "key" ? theme.accent : keyStatus === "animated" ? theme.muted : theme.border
    Accessible.name: "Animation key status: " + keyStatus
    Accessible.description: statusText() + " " + scopeText()

    function statusText() {
        if (keyStatus === "key")
            return "Keyed at frame " + frame + ". Editing this value updates that key.";
        if (keyStatus === "animated")
            return "Animated; no key at frame " + frame + ". Editing this value authors a key there.";
        return "Not animated. Editing this value changes the static parameter.";
    }

    function scopeText() {
        return scope.length > 0 ? "Applies to the whole " + scope + " value." : "";
    }

    function setKeyText() {
        return (keyStatus === "key" ? "Update Key" : "Set Key") + " at Frame " + frame
             + (scope.length > 0 ? " (" + scope + ")" : "");
    }

    function removeKeyText() {
        return keyStatus === "key" ? "Remove Key at Frame " + frame
                                   : "Remove Key (no key at frame " + frame + ")";
    }

    function revealText() {
        return revealAvailable ? "Show in Animation"
                               : "Show in Animation (no Animation panel in this group)";
    }

    Text {
        anchors.centerIn: parent
        text: keyIndicator.keyStatus === "key" ? "\u25c6" : keyIndicator.keyStatus === "animated" ? "\u25c7" : "\u25cb"
        color: keyIndicator.keyStatus === "none" ? theme.muted : theme.accent
        font.pixelSize: 15
    }

    MouseArea {
        id: indicatorMouse
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        onClicked: function (mouse) {
            if (mouse.button === Qt.RightButton) {
                keyContextMenu.popup();
                return;
            }
            keyIndicator.keyRequested();
        }
    }

    Menu {
        id: keyContextMenu
        MenuItem {
            text: keyIndicator.setKeyText()
            onTriggered: keyIndicator.keyRequested()
        }
        MenuItem {
            text: keyIndicator.removeKeyText()
            enabled: keyIndicator.keyStatus === "key"
            onTriggered: keyIndicator.removeKeyRequested()
        }
        MenuSeparator {
        }
        MenuItem {
            text: keyIndicator.revealText()
            enabled: keyIndicator.revealAvailable
            onTriggered: keyIndicator.revealRequested()
        }
    }

    ToolTip.visible: indicatorHover.hovered
    ToolTip.text: statusText() + " " + scopeText()
    HoverHandler {
        id: indicatorHover
    }
}
