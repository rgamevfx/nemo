import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Main owns application chrome; WorkspaceController owns presentation state,
// presets, appearance settings, and persistence.
ApplicationWindow {
    id: window
    objectName: "mainWindow"

    width: 1280
    height: 800
    minimumWidth: Math.max(900, rootNode.minimumPaneWidth)
    minimumHeight: Math.max(600, rootNode.minimumPaneHeight)
    visible: false

    readonly property var controller: workspace
    property bool closeOverride: false

    Theme {
        id: theme
        workspace: window.controller
    }

    palette.window: theme.window
    palette.windowText: theme.text
    palette.base: theme.surface
    palette.text: theme.text
    palette.button: theme.header
    palette.buttonText: theme.text
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentText

    onActiveChanged: {
        if (!active)
            dockDrag.cancelDrag()
    }

    Shortcut {
        sequence: "Ctrl+Shift+R"
        onActivated: window.controller.reset()
    }

    Rectangle {
        id: topBar
        objectName: "workspaceChrome"
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: 42
        color: theme.header
        border.color: theme.border

        RowLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 4

            Label {
                text: "Workspace"
                color: theme.text
                Layout.preferredWidth: 80
            }
            ComboBox {
                id: workspaceSelector
                objectName: "workspaceSelector"
                Layout.preferredWidth: 160
                model: window.controller ? window.controller.workspaces : []
                textRole: "name"
                valueRole: "id"
                currentIndex: {
                    if (!window.controller)
                        return -1
                    for (var i = 0; i < model.length; ++i)
                        if (model[i].id === window.controller.activeWorkspaceId)
                            return i
                    return -1
                }
                onActivated: window.controller.switchWorkspace(currentValue)
                Accessible.name: "Active workspace preset"
            }
            TextField {
                id: workspaceName
                objectName: "workspaceNameInput"
                Layout.preferredWidth: 140
                placeholderText: "Preset name"
                selectByMouse: true
            }
            Button {
                objectName: "workspaceCreate"
                text: "New"
                Layout.preferredWidth: 60
                onClicked: {
                    window.controller.createWorkspace(workspaceName.text.trim())
                    workspaceName.clear()
                }
            }
            Button {
                objectName: "workspaceRename"
                text: "Rename"
                Layout.preferredWidth: 90
                onClicked: window.controller.renameWorkspace(window.controller.activeWorkspaceId,
                                                               workspaceName.text.trim())
            }
            Button {
                objectName: "workspaceDuplicate"
                text: "Duplicate"
                Layout.preferredWidth: 105
                onClicked: {
                    window.controller.duplicateWorkspace(window.controller.activeWorkspaceId,
                                                          workspaceName.text.trim())
                    workspaceName.clear()
                }
            }
            Button {
                objectName: "workspaceClose"
                text: "Close"
                Layout.preferredWidth: 60
                onClicked: window.controller.closeWorkspace(window.controller.activeWorkspaceId)
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: theme.border
            }
            ComboBox {
                id: appearancePreset
                objectName: "appearancePreset"
                Layout.preferredWidth: 120
                model: ["Graphite", "Slate", "Paper"]
                currentIndex: Math.max(0, model.indexOf(window.controller.appearancePreset))
                onActivated: window.controller.setAppearancePreset(currentText)
                Accessible.name: "Appearance preset"
            }
            TextField {
                id: accentInput
                objectName: "appearanceAccent"
                Layout.preferredWidth: 110
                placeholderText: "#RRGGBB accent"
                text: window.controller.accentOverride
                selectByMouse: true
            }
            Button {
                objectName: "appearanceApplyAccent"
                text: "Accent"
                Layout.preferredWidth: 70
                onClicked: window.controller.setAccentOverride(accentInput.text.trim())
            }
            Button {
                objectName: "appearanceCategoriesToggle"
                text: "Categories"
                Layout.preferredWidth: 105
                onClicked: categoryControls.visible = !categoryControls.visible
                Accessible.name: "Category color settings"
            }
            Button {
                objectName: "appearanceReset"
                Layout.preferredWidth: 70
                text: "Reset"
                onClicked: window.controller.resetAppearance()
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: theme.border
            }
            Item { Layout.fillWidth: true }
        }
    }

    // Category overrides are compact, native controls kept in the application
    // chrome. They remain presentation settings and are not project data.
    Row {
        id: categoryControls
        objectName: "appearanceCategories"
        anchors {
            top: topBar.bottom
            left: parent.left
            right: parent.right
        }
        height: 32
        spacing: 4
        visible: false
        property var categories: ["Merge", "Filter", "IO", "Color", "Distort", "Utility"]
        Repeater {
            model: categoryControls.categories
            delegate: TextField {
                objectName: "appearanceCategory_" + modelData
                placeholderText: modelData + " #RRGGBB"
                text: window.controller.categoryColors[modelData] || ""
                onEditingFinished: window.controller.setCategoryColor(modelData, text.trim())
            }
        }
    }

    Rectangle {
        id: errorBanner
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: workspace.error.length > 0 ? 24 : 0
        visible: workspace.error.length > 0
        color: theme.errorSurface
        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: workspace.error
            color: theme.errorText
            font.pixelSize: 12
        }
    }

    DockDrag {
        id: dockDrag
        objectName: "dockDrag"
        anchors.fill: parent
        z: 10
        workspace: window.controller
        theme: theme
    }

    WorkspaceNode {
        id: rootNode
        anchors {
            top: categoryControls.visible ? categoryControls.bottom : topBar.bottom
            left: parent.left
            right: parent.right
            bottom: errorBanner.top
        }
        node: workspace.root
        workspace: window.controller
        drag: dockDrag
        theme: theme
    }

    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: dockDrag.active
        onActivated: dockDrag.cancelDrag()
    }

    onClosing: function(close) {
        if (window.closeOverride)
            return
        if (!workspace.save()) {
            close.accepted = false
            saveErrorDialog.open()
        }
    }

    Dialog {
        id: saveErrorDialog
        title: "Could not save workspace"
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: 480

        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: "The workspace layout could not be saved."
                color: theme.text
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: workspace.error
                color: theme.errorText
                wrapMode: Text.Wrap
            }
        }

        footer: RowLayout {
            spacing: 8
            Button { text: "Retry"; onClicked: retryOrClose() }
            Button { text: "Exit Without Saving"; onClicked: forceClose() }
            Button { text: "Cancel"; onClicked: saveErrorDialog.close() }
        }
    }

    function retryOrClose() {
        if (workspace.save()) {
            window.closeOverride = true
            window.close()
        }
    }

    function forceClose() {
        window.closeOverride = true
        window.close()
    }
}
