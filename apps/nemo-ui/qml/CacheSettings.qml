import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: cacheSettings
    required property var settings
    required property var controller
    required property var theme
    spacing: 12
    Label { text: "Cache & Storage"; color: cacheSettings.theme.text; font.pixelSize: 14; font.bold: true }
    Label {
        Layout.fillWidth: true
        text: "Viewer playback cache directory"
        color: cacheSettings.theme.muted
        wrapMode: Text.Wrap
    }
    TextField {
        id: directory
        objectName: "cacheDirectoryField"
        Layout.fillWidth: true
        text: cacheSettings.settings ? cacheSettings.settings.requestedCacheDirectory : ""
        selectByMouse: true
        Accessible.name: "Cache directory"
        onEditingFinished: if (cacheSettings.settings) {
            cacheSettings.settings.setCacheStorage(text, budget.text)
            text = cacheSettings.settings.requestedCacheDirectory
        }
        Connections {
            target: cacheSettings.settings
            function onChanged() { if (!directory.activeFocus) directory.text = cacheSettings.settings.requestedCacheDirectory }
        }
    }
    RowLayout {
        ChromeButton {
            objectName: "browseCacheFolder"
            theme: cacheSettings.theme
            text: "Browse…"
            onClicked: cacheSettings.settings.browseCacheFolder()
        }
        ChromeButton {
            theme: cacheSettings.theme
            text: "Open Folder"
            onClicked: cacheSettings.settings.openCacheFolder()
        }
    }
    RowLayout {
        Label { text: "Disk budget (MiB)"; color: cacheSettings.theme.text; Layout.fillWidth: true }
        TextField {
            id: budget
            objectName: "cacheDiskBudget"
            text: cacheSettings.controller.cacheDiskMiB.toString()
            implicitWidth: 100
            selectByMouse: true
            Accessible.name: "Disk cache budget in MiB"
            onEditingFinished: if (cacheSettings.settings) {
                cacheSettings.settings.setCacheStorage(directory.text, text)
                text = cacheSettings.controller.cacheDiskMiB.toString()
            }
        }
    }
    Label {
        Layout.fillWidth: true
        text: cacheSettings.settings ? cacheSettings.settings.cacheStatus : ""
        textFormat: Text.PlainText
        color: cacheSettings.theme.text
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        text: "Directory and disk-budget changes take effect after restart. Existing data is not moved or deleted. 1 MiB = 1,048,576 bytes."
        color: cacheSettings.theme.muted
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        text: "RAM/device-budget configuration and safe cache clearing are unavailable in this build: the runtime interfaces tracked by #14 are not implemented. No process-memory estimate is used as device usage."
        color: cacheSettings.theme.muted
        wrapMode: Text.Wrap
    }
}
