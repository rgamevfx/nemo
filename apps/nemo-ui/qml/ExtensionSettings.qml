import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: extensions
    required property var settings
    required property var theme
    spacing: 12
    Label { text: "Local extensions"; color: extensions.theme.text; font.pixelSize: 14; font.bold: true }
    Label {
        Layout.fillWidth: true
        text: "Link a package folder containing manifest.json, or place one package per child folder in your Extensions directory. Discovery never activates native code."
        color: extensions.theme.muted
        wrapMode: Text.Wrap
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        ChromeButton {
            objectName: "addExtensionFolder"
            theme: extensions.theme
            text: "Add Extension Folder…"
            onClicked: extensions.settings.addExtensionFolder()
        }
        ChromeButton {
            objectName: "openExtensionsFolder"
            theme: extensions.theme
            text: "Open Extensions Folder"
            onClicked: extensions.settings.openExtensionsFolder()
        }
        ChromeButton {
            objectName: "refreshExtensions"
            theme: extensions.theme
            text: "Refresh"
            onClicked: extensions.settings.refresh()
        }
    }
    Label {
        Layout.fillWidth: true
        text: extensions.settings ? extensions.settings.extensionNotice : ""
        visible: text.length > 0
        color: extensions.theme.muted
        wrapMode: Text.Wrap
    }
    Repeater {
        model: extensions.settings ? extensions.settings.packages : []
        delegate: Rectangle {
            id: packageCard
            required property var modelData
            Layout.fillWidth: true
            implicitHeight: packageContent.implicitHeight + 24
            color: extensions.theme.header
            border.color: extensions.theme.border
            radius: extensions.theme.smallRadius
            ColumnLayout {
                id: packageContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: (packageCard.modelData.name || packageCard.modelData.id || "Unidentified package") + (packageCard.modelData.version ? " · " + packageCard.modelData.version : "")
                    color: extensions.theme.text
                    font.bold: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
                Label {
                    Layout.fillWidth: true
                    text: packageCard.modelData.details
                    color: extensions.theme.muted
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                }
                Label {
                    Layout.fillWidth: true
                    text: packageCard.modelData.status
                    color: extensions.theme.text
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
                RowLayout {
                    ChromeButton {
                        objectName: "enableExtension_" + packageCard.modelData.id
                        theme: extensions.theme
                        text: packageCard.modelData.requestedEnabled ? "Disable" : "Enable…"
                        enabled: packageCard.modelData.canChange
                        onClicked: {
                            confirmation.directory = packageCard.modelData.directory
                            confirmation.packageId = packageCard.modelData.id
                            confirmation.packageVersion = packageCard.modelData.version
                            confirmation.enablePackage = !packageCard.modelData.requestedEnabled
                            confirmation.removePackage = false
                            confirmation.open()
                        }
                    }
                    ChromeButton {
                        objectName: "removeExtension_" + packageCard.modelData.id
                        theme: extensions.theme
                        text: "Remove link…"
                        visible: packageCard.modelData.linked
                        onClicked: {
                            confirmation.directory = packageCard.modelData.directory
                            confirmation.enablePackage = false
                            confirmation.removePackage = true
                            confirmation.open()
                        }
                    }
                }
            }
        }
    }
    Dialog {
        id: confirmation
        objectName: "extensionConfirmation"
        property string directory: ""
        property string packageId: ""
        property string packageVersion: ""
        property bool enablePackage: false
        property bool removePackage: false
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(460, parent.width - 32)
        height: Math.min(implicitHeight, parent.height - 32)
        modal: true
        title: removePackage ? "Remove linked folder?" : enablePackage ? "Trust and enable extension?" : "Disable extension?"
        onOpened: {
            const accept = standardButton(Dialog.Ok)
            accept.objectName = "confirmExtensionChange"
            accept.text = removePackage ? "Remove link" : enablePackage ? "Trust and enable" : "Disable"
        }
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ScrollView {
            id: confirmationScroll
            implicitHeight: confirmationText.implicitHeight
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Label {
                id: confirmationText
                width: confirmationScroll.availableWidth
                text: (confirmation.removePackage
                       ? "Forget this registration; its files will not be changed or deleted."
                       : confirmation.enablePackage
                         ? "Native extensions execute with Nemo's full privileges, not in a sandbox. Enable only code you trust. Dependencies will not be enabled automatically."
                         : "The package remains active for this process, if already loaded.")
                      + "\n\n" + confirmation.directory + "\n\n"
                      + (extensions.settings ? extensions.settings.changeExplanation(confirmation.directory, confirmation.enablePackage) : "")
                      + "\n\nActivation changes take effect after restart. Unsaved project edits are not affected."
                color: extensions.theme.text
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
            }
        }
        onAccepted: {
            if (removePackage)
                extensions.settings.removeExtensionFolder(directory)
            else
                extensions.settings.setPackageEnabled(directory, packageId, packageVersion, enablePackage)
        }
    }
}
