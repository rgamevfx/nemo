import QtQuick
import QtQuick.Controls

Rectangle {
    id: unavailable
    property string panelType: ""
    property var theme
    color: "transparent"
    objectName: "unavailablePanel_" + panelType

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: panelType
            color: unavailable.theme ? unavailable.theme.text : "#d0d0d0"
            font.bold: true
            font.pixelSize: 15
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Panel unavailable; the saved layout was preserved."
            color: unavailable.theme ? unavailable.theme.mutedText : "#999999"
            font.pixelSize: 12
        }
    }
}
