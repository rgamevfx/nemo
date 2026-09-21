import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: settingsWindow
    objectName: "settingsWindow"
    required property var controller
    required property var settings
    required property var theme
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal
    visible: false
    title: "Settings — Nemo"
    width: controller.settingsWidth
    height: controller.settingsHeight
    minimumWidth: 560
    minimumHeight: 400
    color: theme.panel
    font.family: "Inter"
    font.pixelSize: theme.fontSize
    palette: transientParent.palette
    property int section: controller.settingsSection

    function open() {
        show()
        raise()
        requestActivate()
    }
    function remember() {
        rememberTimer.stop()
        return controller.setSettingsWindow(section, width, height)
    }
    onWidthChanged: if (visible) rememberTimer.restart()
    onHeightChanged: if (visible) rememberTimer.restart()
    onSectionChanged: if (visible) rememberTimer.restart()
    Timer {
        interval: 1000
        repeat: true
        running: settingsWindow.visible && settingsWindow.section === 0 && settingsWindow.settings !== null
        onTriggered: settingsWindow.settings.pollUsage()
    }
    onClosing: remember()
    Timer { id: rememberTimer; interval: 300; onTriggered: settingsWindow.remember() }
    Shortcut { sequence: "Escape"; onActivated: settingsWindow.close() }

    header: Rectangle {
        height: 34
        color: settingsWindow.theme.header
        MouseArea {
            anchors.fill: parent
            onPressed: settingsWindow.startSystemMove()
        }
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "Settings"
            color: settingsWindow.theme.text
        }
        ChromeButton {
            objectName: "settingsClose"
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            theme: settingsWindow.theme
            text: "×"
            Accessible.name: "Close Settings"
            onClicked: settingsWindow.close()
        }
    }
    MouseArea {
        objectName: "settingsResize"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 14
        height: 14
        z: 10
        cursorShape: Qt.SizeFDiagCursor
        onPressed: settingsWindow.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        Label { anchors.centerIn: parent; text: "⌟"; color: settingsWindow.theme.muted }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: restartLabel.implicitHeight + 16
            visible: settingsWindow.settings && settingsWindow.settings.restartRequired
            color: settingsWindow.theme.raised
            Label {
                id: restartLabel
                anchors.fill: parent
                anchors.margins: 8
                text: "Restart required. Save your work, quit Nemo normally, then reopen. Current extensions and cache storage stay active until then."
                color: settingsWindow.theme.text
                wrapMode: Text.Wrap
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 142
                color: settingsWindow.theme.header
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 8
                    spacing: 4
                    Repeater {
                        model: ["App Settings", "UI Settings", "Extensions"]
                        ChromeButton {
                            required property string modelData
                            required property int index
                            objectName: "settingsSection_" + index
                            Layout.fillWidth: true
                            theme: settingsWindow.theme
                            text: modelData
                            highlighted: settingsWindow.section === index
                            font.bold: highlighted
                            onClicked: settingsWindow.section = index
                        }
                    }
                }
            }
            ScrollView {
                id: settingsScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.margins: 16
                        spacing: 12
                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: settingsWindow.controller.error
                            textFormat: Text.PlainText
                            color: settingsWindow.theme.text
                            wrapMode: Text.Wrap
                        }
                        Label {
                            objectName: "settingsError"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: settingsWindow.settings ? settingsWindow.settings.error : ""
                            textFormat: Text.PlainText
                            color: settingsWindow.theme.text
                            wrapMode: Text.Wrap
                        }
                        CacheSettings {
                            visible: settingsWindow.section === 0
                            Layout.fillWidth: true
                            settings: settingsWindow.settings
                            controller: settingsWindow.controller
                            theme: settingsWindow.theme
                        }
                        AppearanceSettings {
                            visible: settingsWindow.section === 1
                            Layout.fillWidth: true
                            controller: settingsWindow.controller
                            theme: settingsWindow.theme
                        }
                        ExtensionSettings {
                            visible: settingsWindow.section === 2
                            Layout.fillWidth: true
                            settings: settingsWindow.settings
                            theme: settingsWindow.theme
                        }
                    }
                }
            }
        }
    }
}
