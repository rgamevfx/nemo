import QtQuick
import QtQuick.Layouts

// The shared inspector checkbox (issue #102), used by an ordinary Boolean row
// and by an inline Boolean group (Grade's flags) alike.
//
// User input is reported through toggled() and this control NEVER assigns
// `checked`: the host keeps its authored binding, so a refused, deferred or
// re-published value is shown without a second synchronization pass, and the
// value is committed exactly once through the host's one-undo gesture.
//
// With a panel and an identity bound the control owns the shared value menu
// (key/reset actions) and reports the animation status; without them it is an
// ordinary toggle that leaves the host's own row menu in charge.
Item {
    id: checkBox

    property var theme: null
    property string text: ""
    property bool checked: false
    // Optional host wiring, exactly like NumericField's.
    property var panel: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: ""
    property string keyStatus: "none"
    property string scope: ""
    property int frame: 0
    property bool revealAvailable: false
    property bool modified: false

    signal toggled(bool checked)
    signal keyRequested()

    readonly property bool bound: panel !== null && networkId.length > 0 && nodeId.length > 0 && parameterKey.length > 0
    readonly property int textSize: theme && theme.inspectorFontSize !== undefined ? theme.inspectorFontSize : (theme ? theme.fontSize : 11)
    readonly property int gap: theme && theme.inspectorSpacing !== undefined ? theme.inspectorSpacing : 8
    readonly property color accentColor: theme ? theme.accent : "#3485f6"
    readonly property color borderColor: theme ? theme.border : "#30343a"
    readonly property color fieldColor: theme ? theme.field : "#24272c"
    readonly property color hoverColor: theme ? theme.hover : "#343940"

    implicitWidth: content.implicitWidth
    implicitHeight: content.implicitHeight
    activeFocusOnTab: enabled
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.focusable: true

    Keys.onSpacePressed: checkBox.toggled(!checkBox.checked)
    Keys.onReturnPressed: checkBox.toggled(!checkBox.checked)

    RowLayout {
        id: content
        anchors.fill: parent
        spacing: checkBox.gap

        Rectangle {
            id: box
            Layout.preferredWidth: 18
            Layout.preferredHeight: 18
            Layout.alignment: Qt.AlignVCenter
            radius: 3
            color: checkBox.checked ? checkBox.accentColor : input.containsMouse ? checkBox.hoverColor : checkBox.fieldColor
            border.width: 1
            border.color: checkBox.checked ? checkBox.accentColor : checkBox.activeFocus ? checkBox.accentColor : checkBox.borderColor

            Text {
                anchors.centerIn: parent
                visible: checkBox.checked
                text: "\u2713"
                color: "#ffffff"
                font.pixelSize: 12
                font.weight: Font.Bold
            }

            // The shared value menu, owned by the one control that already
            // states the key status and the action wording. It is layout-neutral
            // (a zero-size child of the box, never a cell), so the mockup-led
            // row has no key-button column.
            KeyIndicator {
                id: valueMenu
                width: 0
                height: 0
                theme: checkBox.theme
                networkId: checkBox.networkId
                nodeId: checkBox.nodeId
                parameterKey: checkBox.parameterKey
                parameterLabel: checkBox.text
                keyStatus: checkBox.keyStatus
                scope: checkBox.scope
                frame: checkBox.frame
                revealAvailable: checkBox.revealAvailable
                resettable: checkBox.bound
                modified: checkBox.modified
                onKeyRequested: if (checkBox.bound)
                    checkBox.panel.keyParameterAtFrame(checkBox.networkId, checkBox.nodeId, checkBox.parameterKey)
                onRemoveKeyRequested: if (checkBox.bound)
                    checkBox.panel.removeParameterKeyAtFrame(checkBox.networkId, checkBox.nodeId, checkBox.parameterKey)
                onRevealRequested: if (checkBox.bound)
                    checkBox.panel.revealInAnimation(checkBox.networkId, checkBox.nodeId, checkBox.parameterKey)
                onResetRequested: if (checkBox.bound)
                    checkBox.panel.resetValue({
                        "networkId": checkBox.networkId,
                        "nodeId": checkBox.nodeId,
                        "parameterKey": checkBox.parameterKey,
                        "label": checkBox.text
                    })
            }
        }

        ExposureLabel {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            visible: checkBox.text.length > 0
            clickable: true
            theme: checkBox.theme
            networkId: checkBox.networkId
            instanceId: checkBox.instanceId
            nodeId: checkBox.nodeId
            parameterKey: checkBox.parameterKey
            labelText: checkBox.text
            keyStatus: checkBox.keyStatus
            frame: checkBox.frame
            textSize: checkBox.textSize
            onKeyRequested: checkBox.keyRequested()
            onClicked: checkBox.toggled(!checkBox.checked)
        }
    }

    MouseArea {
        id: input
        // The label owns click/key/exposure gestures; this background area
        // handles the checkbox square and contextual clicks.
        z: -1
        anchors.fill: parent
        acceptedButtons: checkBox.bound ? Qt.LeftButton | Qt.RightButton : Qt.LeftButton
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: function (mouse) {
            if (mouse.button === Qt.RightButton) {
                valueMenu.openMenu(checkBox);
                return;
            }
            if (mouse.modifiers & Qt.AltModifier) {
                checkBox.keyRequested();
                return;
            }
            checkBox.toggled(!checkBox.checked);
        }
    }
}
