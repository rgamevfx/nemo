import QtQuick
import QtQuick.Controls

// Shared parameter animation/reset actions. A zero-sized instance owns only
// the value context menu; compact editors can retain a visible status cell.
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
    // Opt-in value reset. Only a consumer that offers the action sets
    // `resettable`; the animation-only menus of the other editors are
    // unchanged. `modified` is the action's enabled state.
    property bool resettable: false
    property bool modified: false

    signal keyRequested()
    signal removeKeyRequested()
    signal revealRequested()
    signal resetRequested()

    implicitWidth: 24
    implicitHeight: 22
    radius: theme.smallRadius
    color: indicatorMouse.containsMouse ? theme.hover : "transparent"
    border.width: width > 0 && height > 0 ? 1 : 0
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

    // Compact editors retain the shared key menu without a permanent key cell.
    // A caller with no cell of its own passes no target and the menu opens at
    // the pointer, exactly like an ordinary context menu.
    function openMenu(target) {
        if (target)
            keyContextMenu.popup(target, 0, target.height);
        else
            keyContextMenu.popup();
    }

    Text {
        visible: keyIndicator.width > 0 && keyIndicator.height > 0
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
            objectName: "parameterResetValue_" + keyIndicator.nodeId + "_" + keyIndicator.parameterKey
            text: "Reset Value"
            visible: keyIndicator.resettable
            enabled: keyIndicator.resettable && keyIndicator.modified
            Accessible.name: "Reset value to the schema default"
            onTriggered: keyIndicator.resetRequested()
        }
        MenuSeparator {
            visible: keyIndicator.resettable
        }
        MenuItem {
            objectName: "parameterSetKey_" + keyIndicator.nodeId + "_" + keyIndicator.parameterKey
            text: keyIndicator.setKeyText()
            onTriggered: keyIndicator.keyRequested()
        }
        MenuItem {
            objectName: "parameterRemoveKey_" + keyIndicator.nodeId + "_" + keyIndicator.parameterKey
            text: keyIndicator.removeKeyText()
            enabled: keyIndicator.keyStatus === "key"
            onTriggered: keyIndicator.removeKeyRequested()
        }
        MenuSeparator {
        }
        MenuItem {
            objectName: "parameterReveal_" + keyIndicator.nodeId + "_" + keyIndicator.parameterKey
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
