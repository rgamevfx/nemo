import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Main owns application chrome; WorkspaceController owns presentation state,
// presets, appearance settings, and persistence.
ApplicationWindow {
    id: window
    objectName: "mainWindow"

    width: 1568
    height: 926
    minimumWidth: Math.max(960, rootNode.minimumPaneWidth + 16)
    minimumHeight: Math.max(640, rootNode.minimumPaneHeight + 56)
    // main.cpp validates the Vulkan surface before showing this window.
    visible: false
    title: "Nemo"
    color: appTheme.background
    font.family: "Inter"
    font.pixelSize: appTheme.fontSize

    readonly property var controller: workspace
    property bool closeOverride: false

    Theme {
        id: appTheme
        workspace: window.controller
    }

    palette.window: appTheme.panel
    palette.windowText: appTheme.text
    palette.base: appTheme.field
    palette.text: appTheme.text
    palette.button: appTheme.raised
    palette.buttonText: appTheme.text
    palette.highlight: appTheme.accent
    palette.highlightedText: "#ffffff"
    palette.mid: appTheme.border
    palette.dark: appTheme.border
    palette.light: appTheme.raised

    onActiveChanged: {
        if (!active)
            dockDrag.cancelDrag()
    }

    Shortcut {
        sequence: "Ctrl+Shift+R"
        onActivated: window.controller.reset()
    }

    function workspaceIndex(id) {
        var entries = window.controller ? window.controller.workspaces : []
        for (var i = 0; i < entries.length; ++i) {
            if (entries[i].id === id)
                return i
        }
        return -1
    }

    function workspaceNameFor(id) {
        var index = workspaceIndex(id)
        return index >= 0 ? window.controller.workspaces[index].name : "Workspace"
    }

    Rectangle {
        id: topBar
        objectName: "workspaceChrome"
        height: 42
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        color: appTheme.background

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            Text {
                text: "Nemo"
                color: appTheme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
                height: 28
                verticalAlignment: Text.AlignVCenter
            }

            ChromeButton {
                id: themeSettingsButton
                objectName: "themeSettingsButton"
                theme: appTheme
                width: 30
                Accessible.name: "Appearance settings"
                onClicked: appearance.open()
                contentItem: Canvas {
                    id: settingsGlyph
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        ctx.strokeStyle = appTheme.muted
                        ctx.lineWidth = 1.2
                        var cx = width / 2
                        var cy = height / 2
                        ctx.beginPath()
                        ctx.arc(cx, cy, 4, 0, Math.PI * 2)
                        ctx.stroke()
                        for (var i = 0; i < 8; ++i) {
                            var angle = i * Math.PI / 4
                            ctx.beginPath()
                            ctx.moveTo(cx + 5 * Math.cos(angle), cy + 5 * Math.sin(angle))
                            ctx.lineTo(cx + 7 * Math.cos(angle), cy + 7 * Math.sin(angle))
                            ctx.stroke()
                        }
                    }
                    Connections {
                        target: appTheme
                        function onPresetChanged() {
                            settingsGlyph.requestPaint()
                        }
                    }
                }
            }
        }

        Row {
            id: tabsRow
            anchors.centerIn: parent
            height: parent.height
            spacing: 3

            Flickable {
                id: workspaceFlickable
                width: Math.min(workspaceTabs.width, Math.max(280, window.width - 420))
                height: parent.height
                contentWidth: workspaceTabs.width
                contentHeight: height
                clip: true
                flickableDirection: Flickable.HorizontalFlick

                Row {
                    id: workspaceTabs
                    height: parent.height
                    spacing: 3

                    Repeater {
                        model: window.controller ? window.controller.workspaces : []
                        delegate: Button {
                            id: workspaceTab
                            required property var modelData
                            required property int index
                            objectName: "workspaceTab_" + modelData.id
                            width: Math.max(64, label.implicitWidth + 30)
                            height: 36
                            y: 6
                            padding: 0
                            onClicked: {
                                dockDrag.cancelDrag()
                                window.controller.switchWorkspace(workspaceTab.modelData.id)
                            }
                            contentItem: Text {
                                id: label
                                text: workspaceTab.modelData.name
                                color: window.controller.activeWorkspaceId === workspaceTab.modelData.id
                                       ? appTheme.text : appTheme.muted
                                font.pixelSize: 12
                                font.weight: window.controller.activeWorkspaceId === workspaceTab.modelData.id
                                              ? Font.Medium : Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 6
                                color: window.controller.activeWorkspaceId === workspaceTab.modelData.id
                                       ? appTheme.header : workspaceTab.hovered ? appTheme.panel : "transparent"
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: parent.width - 18
                                    height: 2
                                    radius: 1
                                    color: appTheme.accent
                                    visible: window.controller.activeWorkspaceId === workspaceTab.modelData.id
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: {
                                    workspaceMenu.targetWorkspaceId = workspaceTab.modelData.id
                                    workspaceMenu.popup()
                                }
                            }
                        }
                    }
                }
            }

            ChromeButton {
                id: addWorkspaceButton
                objectName: "addWorkspaceButton"
                theme: appTheme
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                text: "+"
                font.pixelSize: 17
                Accessible.name: "Create workspace from current layout"
                onClicked: {
                    nameDialog.renaming = false
                    nameDialog.targetWorkspaceId = window.controller.activeWorkspaceId
                    workspaceName.text = "Workspace " + (window.controller.workspaces.length + 1)
                    nameDialog.open()
                }
            }
        }

        ChromeButton {
            id: workspaceOptionsButton
            objectName: "workspaceOptionsButton"
            theme: appTheme
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "…"
            font.pixelSize: 17
            Accessible.name: "Workspace options"
            onClicked: {
                workspaceMenu.targetWorkspaceId = window.controller.activeWorkspaceId
                workspaceMenu.popup()
            }
        }
    }

    Menu {
        id: workspaceMenu
        objectName: "workspaceMenu"
        property string targetWorkspaceId: ""

        MenuItem {
            text: "Rename workspace"
            onTriggered: {
                nameDialog.renaming = true
                nameDialog.targetWorkspaceId = workspaceMenu.targetWorkspaceId
                workspaceName.text = window.workspaceNameFor(workspaceMenu.targetWorkspaceId)
                nameDialog.open()
            }
        }
        MenuItem {
            text: "Duplicate workspace"
            onTriggered: {
                nameDialog.renaming = false
                nameDialog.targetWorkspaceId = workspaceMenu.targetWorkspaceId
                workspaceName.text = window.workspaceNameFor(workspaceMenu.targetWorkspaceId) + " copy"
                nameDialog.open()
            }
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Move left"
            enabled: window.workspaceIndex(workspaceMenu.targetWorkspaceId) > 0
            onTriggered: window.controller.moveWorkspace(workspaceMenu.targetWorkspaceId, -1)
        }
        MenuItem {
            text: "Move right"
            enabled: {
                var index = window.workspaceIndex(workspaceMenu.targetWorkspaceId)
                return index >= 0 && index < window.controller.workspaces.length - 1
            }
            onTriggered: window.controller.moveWorkspace(workspaceMenu.targetWorkspaceId, 1)
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Close workspace"
            enabled: window.controller.workspaces.length > 1
            onTriggered: window.controller.closeWorkspace(workspaceMenu.targetWorkspaceId)
        }
    }

    Dialog {
        id: nameDialog
        objectName: "workspaceNameDialog"
        property bool renaming: false
        property string targetWorkspaceId: ""
        anchors.centerIn: parent
        width: 310
        title: renaming ? "Rename workspace" : "New workspace"
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            workspaceName.forceActiveFocus()
            workspaceName.selectAll()
        }
        onAccepted: {
            if (renaming) {
                window.controller.renameWorkspace(targetWorkspaceId, workspaceName.text)
            } else {
                var created = window.controller.duplicateWorkspace(targetWorkspaceId,
                                                                   workspaceName.text.trim() || "Workspace")
                if (created.length)
                    window.controller.switchWorkspace(created)
            }
        }

        TextField {
            id: workspaceName
            objectName: "workspaceNameField"
            width: parent.width
            selectByMouse: true
            Accessible.name: "Workspace name"
            onAccepted: nameDialog.accept()
        }
    }

    Popup {
        id: appearance
        objectName: "appearancePopup"
        x: 80
        y: 40
        width: 344
        padding: 16
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: appTheme.header
            radius: appTheme.radius
            border.color: appTheme.border
        }
        contentItem: ColumnLayout {
            spacing: 12

            Text {
                text: "Appearance"
                color: appTheme.text
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            RowLayout {
                Text {
                    text: "Theme"
                    color: appTheme.muted
                    font.pixelSize: appTheme.fontSize
                    Layout.fillWidth: true
                }
                StudioComboBox {
                    id: themePresetMenu
                    objectName: "themePresetMenu"
                    theme: appTheme
                    model: ["Graphite", "Slate", "Paper"]
                    currentIndex: Math.max(0, model.indexOf(window.controller.appearancePreset))
                    implicitWidth: 142
                    implicitHeight: 28
                    onActivated: window.controller.setAppearancePreset(currentText)
                }
            }

            RowLayout {
                Text {
                    text: "Accent"
                    color: appTheme.muted
                    font.pixelSize: appTheme.fontSize
                    Layout.fillWidth: true
                }
                Rectangle {
                    width: 18
                    height: 18
                    radius: 9
                    color: appTheme.accent
                }
                TextField {
                    id: accentField
                    objectName: "accentColorField"
                    text: appTheme.accent.toString()
                    implicitWidth: 112
                    implicitHeight: 28
                    selectByMouse: true
                    Accessible.name: "Accent color"
                    validator: RegularExpressionValidator {
                        regularExpression: /#[0-9a-fA-F]{6}/
                    }
                    onEditingFinished: {
                        if (acceptableInput)
                            window.controller.setAccentOverride(text)
                        text = appTheme.accent.toString()
                    }
                    Connections {
                        target: window.controller
                        function onAppearanceChanged() {
                            accentField.text = appTheme.accent.toString()
                        }
                    }
                }
            }

            ChromeButton {
                objectName: "resetAccentButton"
                theme: appTheme
                text: "Reset accent"
                onClicked: {
                    window.controller.setAccentOverride("")
                    accentField.text = appTheme.accent.toString()
                }
            }

            Text {
                text: "Node category colors"
                color: appTheme.muted
                font.pixelSize: appTheme.fontSize
            }

            ScrollView {
                id: categoryScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(300, categoryColumn.implicitHeight)
                clip: true

                ColumnLayout {
                    id: categoryColumn
                    width: categoryScroll.availableWidth
                    spacing: 6

                    Repeater {
                        model: ["Color", "Distort", "Filter", "Utility", "Merge", "IO"]
                        delegate: RowLayout {
                            id: categoryRow
                            required property string modelData
                            property string categoryId: modelData
                            Layout.fillWidth: true
                            spacing: 7

                            Text {
                                text: categoryRow.categoryId === "IO" ? "I/O" : categoryRow.categoryId
                                color: appTheme.text
                                font.pixelSize: appTheme.fontSize
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                width: 18
                                height: 18
                                radius: appTheme.smallRadius
                                color: appTheme.nodeCategoryColor(categoryRow.categoryId)
                                border.color: appTheme.border
                            }
                            TextField {
                                id: categoryField
                                objectName: "categoryColorField_" + categoryRow.categoryId
                                text: appTheme.nodeCategoryColor(categoryRow.categoryId)
                                implicitWidth: 112
                                implicitHeight: 28
                                selectByMouse: true
                                Accessible.name: categoryRow.categoryId + " node category color"
                                validator: RegularExpressionValidator {
                                    regularExpression: /#[0-9a-fA-F]{6}/
                                }
                                onEditingFinished: {
                                    if (acceptableInput)
                                        window.controller.setCategoryColor(categoryRow.categoryId, text)
                                    text = appTheme.nodeCategoryColor(categoryRow.categoryId)
                                }
                                Connections {
                                    target: window.controller
                                    function onAppearanceChanged() {
                                        categoryField.text = appTheme.nodeCategoryColor(categoryRow.categoryId)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            ChromeButton {
                objectName: "resetNodeCategoryColorsButton"
                theme: appTheme
                text: "Reset node category colors"
                Layout.fillWidth: true
                onClicked: window.controller.resetCategoryColors()
            }
        }
    }

    DockDrag {
        id: dockDrag
        objectName: "dockDrag"
        anchors.fill: parent
        z: 10
        workspace: window.controller
        theme: appTheme
    }

    WorkspaceNode {
        id: rootNode
        anchors {
            top: topBar.bottom
            left: parent.left
            right: parent.right
            bottom: errorBanner.top
            margins: 8
            topMargin: 2
        }
        node: workspace.root
        workspace: window.controller
        drag: dockDrag
        theme: appTheme
    }

    Rectangle {
        id: errorBanner
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: workspace.error.length > 0 ? 24 : 0
        visible: height > 0
        color: appTheme.errorSurface
        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: workspace.error
            color: appTheme.errorText
            font.pixelSize: appTheme.fontSize
        }
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
                color: appTheme.text
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: workspace.error
                color: appTheme.errorText
                wrapMode: Text.Wrap
            }
        }

        footer: RowLayout {
            spacing: 8
            ChromeButton { theme: appTheme; text: "Retry"; onClicked: retryOrClose() }
            ChromeButton { theme: appTheme; text: "Exit Without Saving"; onClicked: forceClose() }
            ChromeButton { theme: appTheme; text: "Cancel"; onClicked: saveErrorDialog.close() }
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
