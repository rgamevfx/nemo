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
    title: projectFile.windowTitle + " — Nemo"
    color: appTheme.background
    font.family: "Inter"
    font.pixelSize: appTheme.fontSize

    readonly property var controller: workspace
    property bool closeOverride: false
    property bool frameless: false
    // Pending destructive action awaiting the unsaved-changes answer.
    // Values: "" | "open" | "recover" | "quit".
    property string pendingAction: ""
    property url pendingUrl
    property bool savingForPending: false
    // "" | "menu"; distinguishes an explicit Save As from a save-before-action.
    property string saveAsPurpose: ""
    property bool awaitingPresentation: false
    property bool showRecoveredNotice: false
    property bool warningsAcknowledged: false
    property bool quitPending: false
    flags: Qt.Window | (frameless ? Qt.FramelessWindowHint : 0)

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

    // Approved standard File shortcuts. Recover stays menu-only.
    Shortcut {
        sequence: "Ctrl+O"
        onActivated: window.chooseOpenProject()
    }
    Shortcut {
        sequence: "Ctrl+S"
        onActivated: window.requestSave()
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: window.chooseSaveAs()
    }

    function openFileMenu() {
        // Open below the trigger using the same host popup anchors as the
        // workspace options menu, instead of the cursor/0,0 default.
        var point = fileMenuTrigger.mapToItem(window.contentItem, 0, fileMenuTrigger.height)
        fileMenu.x = point.x
        fileMenu.y = point.y + 2
        fileMenu.open()
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

    function chooseOpenProject() {
        if (projectFile.busy)
            return
        projectFile.chooseOpenProject()
    }

    function chooseSaveAs() {
        if (projectFile.busy)
            return
        window.saveAsPurpose = "menu"
        projectFile.chooseSaveAs()
    }

    function requestSave() {
        if (projectFile.busy)
            return
        if (!projectFile.hasPath) {
            window.saveAsPurpose = "menu"
            projectFile.chooseSaveAs()
            return
        }
        window.savingForPending = false
        projectFile.save()
    }

    // A file was chosen for a destructive action; ask about unsaved changes
    // first, then run the action against the file the user actually picked.
    function beginPending(action, url) {
        window.pendingAction = action
        window.pendingUrl = url
        if (projectFile.dirty) {
            unsavedChangesDialog.open()
            return
        }
        window.runPending()
    }

    function runPending() {
        var action = window.pendingAction
        var url = window.pendingUrl
        window.pendingAction = ""
        window.pendingUrl = ""
        if (action === "open")
            projectFile.openProject(url)
        else if (action === "recover")
            projectFile.recoverProject(url)
        else if (action === "quit")
            window.finishQuit()
    }

    function savePendingAction() {
        unsavedChangesDialog.handled = true
        unsavedChangesDialog.close()
        if (!projectFile.hasPath) {
            window.saveAsPurpose = "pending"
            projectFile.chooseSaveAs()
            return
        }
        window.savingForPending = true
        projectFile.save()
    }

    function discardPendingAction() {
        unsavedChangesDialog.handled = true
        unsavedChangesDialog.close()
        window.runPending()
    }

    function cancelPendingAction() {
        unsavedChangesDialog.handled = true
        unsavedChangesDialog.close()
        window.pendingAction = ""
        window.pendingUrl = ""
    }

    function finishQuit() {
        // Never tear down while a read/write is in flight; resume when the I/O
        // worker reports idle instead of blocking the UI thread.
        if (projectFile.busy) {
            window.quitPending = true
            return
        }
        if (!workspace.save()) {
            saveErrorDialog.open()
            return
        }
        window.closeOverride = true
        window.close()
    }

    function clearPendingChooserState() {
        if (window.saveAsPurpose === "pending") {
            // The chooser did not complete: the destructive action that asked
            // for a Save As stays cancelled and the project keeps its changes.
            window.savingForPending = false
            window.pendingAction = ""
            window.pendingUrl = ""
        }
        window.saveAsPurpose = ""
    }

    function resolveRestore(restore) {
        restoreSessionDialog.resolved = true
        restoreSessionDialog.close()
        projectFile.resolvePresentation(restore)
    }

    function openPostOpenDialogs() {
        if (window.showRecoveredNotice) {
            window.showRecoveredNotice = false
            recoveredCopyNotice.open()
            return
        }
        if (projectFile.warnings.length > 0 && !window.warningsAcknowledged) {
            window.warningsAcknowledged = true
            projectWarningsDialog.open()
            return
        }
        if (window.awaitingPresentation) {
            window.awaitingPresentation = false
            restoreSessionDialog.open()
        }
    }

    Rectangle {
        id: topBar
        objectName: "workspaceChrome"
        height: 42
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        color: appTheme.background

        MouseArea {
            objectName: "windowMoveArea"
            anchors.fill: parent
            enabled: window.frameless
            onPressed: window.startSystemMove()
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            Item {
                id: fileMenuTrigger
                objectName: "fileMenuButton"
                width: nemoTitle.implicitWidth
                height: 28

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -4
                    radius: appTheme.smallRadius
                    color: fileMenuArea.containsMouse || fileMenu.visible ? appTheme.header : "transparent"
                }

                Text {
                    id: nemoTitle
                    anchors.centerIn: parent
                    text: "Nemo"
                    color: appTheme.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                }

                MouseArea {
                    id: fileMenuArea
                    objectName: "fileMenuTriggerArea"
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    Accessible.name: "File menu"
                    onClicked: window.openFileMenu()
                }
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

    // Replace the native move/resize hit regions removed with invisible CSD.
    // The four-pixel grips fit inside the existing eight-pixel workspace inset.
    component WindowResizeArea: MouseArea {
        required property int edges
        enabled: window.frameless
        onPressed: window.startSystemResize(edges)
    }

    Item {
        anchors.fill: parent
        z: 100
        visible: window.frameless
        WindowResizeArea { x: 0; y: 8; width: 4; height: parent.height - 16; edges: Qt.LeftEdge; cursorShape: Qt.SizeHorCursor }
        WindowResizeArea { x: parent.width - width; y: 8; width: 4; height: parent.height - 16; edges: Qt.RightEdge; cursorShape: Qt.SizeHorCursor }
        WindowResizeArea { x: 8; y: 0; width: parent.width - 16; height: 4; edges: Qt.TopEdge; cursorShape: Qt.SizeVerCursor }
        WindowResizeArea { x: 8; y: parent.height - height; width: parent.width - 16; height: 4; edges: Qt.BottomEdge; cursorShape: Qt.SizeVerCursor }
        WindowResizeArea { x: 0; y: 0; width: 8; height: 8; edges: Qt.TopEdge | Qt.LeftEdge; cursorShape: Qt.SizeFDiagCursor }
        WindowResizeArea { x: parent.width - width; y: 0; width: 8; height: 8; edges: Qt.TopEdge | Qt.RightEdge; cursorShape: Qt.SizeBDiagCursor }
        WindowResizeArea { x: 0; y: parent.height - height; width: 8; height: 8; edges: Qt.BottomEdge | Qt.LeftEdge; cursorShape: Qt.SizeBDiagCursor }
        WindowResizeArea { x: parent.width - width; y: parent.height - height; width: 8; height: 8; edges: Qt.BottomEdge | Qt.RightEdge; cursorShape: Qt.SizeFDiagCursor }
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

    // Approved production-only File menu: the existing Nemo title is its
    // trigger. Open, Save, Save As and Recover only; no New action.
    Menu {
        id: fileMenu
        objectName: "fileMenu"

        MenuItem {
            objectName: "fileOpenAction"
            text: "Open…"
            onTriggered: window.chooseOpenProject()
        }
        MenuItem {
            objectName: "fileSaveAction"
            text: "Save"
            enabled: !projectFile.busy
            onTriggered: window.requestSave()
        }
        MenuItem {
            objectName: "fileSaveAsAction"
            text: "Save As…"
            enabled: !projectFile.busy
            onTriggered: window.chooseSaveAs()
        }
        MenuSeparator {
        }
        MenuItem {
            objectName: "fileRecoverAction"
            text: "Recover…"
            onTriggered: projectFile.chooseRecoveryProject()
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
        if (projectFile.busy) {
            // A read/write is in flight: defer instead of joining it here.
            close.accepted = false
            window.quitPending = true
            return
        }
        if (projectFile.dirty) {
            close.accepted = false
            window.pendingAction = "quit"
            window.pendingUrl = ""
            unsavedChangesDialog.open()
            return
        }
        if (!workspace.save()) {
            close.accepted = false
            saveErrorDialog.open()
        }
    }

    Connections {
        target: projectFile

        function onOpenFileChosen(url) {
            window.beginPending("open", url)
        }
        function onRecoveryFileChosen(url) {
            window.beginPending("recover", url)
        }
        function onSaveAsChosen(url) {
            var purpose = window.saveAsPurpose
            window.saveAsPurpose = ""
            if (purpose === "pending")
                window.savingForPending = true
            projectFile.saveAs(url)
        }
        function onSaveFinished(ok) {
            if (!ok) {
                // A cancelled or failed save cancels the pending destructive
                // action; the project error dialog explains why.
                window.savingForPending = false
                window.pendingAction = ""
                window.pendingUrl = ""
                projectErrorDialog.open()
                return
            }
            if (window.savingForPending) {
                window.savingForPending = false
                // A save that completed while newer document/presentation edits
                // arrived leaves the project dirty: re-ask rather than proceed.
                if (projectFile.dirty) {
                    unsavedChangesDialog.open()
                } else {
                    window.runPending()
                }
            }
        }
        function onProjectOpened(hasPresentation, recovered) {
            window.warningsAcknowledged = false
            window.showRecoveredNotice = recovered
            window.awaitingPresentation = hasPresentation
            window.openPostOpenDialogs()
        }
        function onProjectOpenFailed() {
            window.pendingAction = ""
            window.pendingUrl = ""
            projectErrorDialog.open()
        }
        function onFileDialogFailed() {
            // A failed chooser aborts the pending destructive action exactly
            // like cancel; the controller already set the diagnostic text.
            window.clearPendingChooserState()
            projectErrorDialog.open()
        }
        function onAutosaveFailed() {
            // Distinct autosave failures surface through the existing error
            // dialog; identical repeats are deduplicated in the controller.
            projectErrorDialog.open()
        }
        function onFileDialogCancelled() {
            window.clearPendingChooserState()
        }
        function onBusyChanged() {
            if (!window.quitPending || projectFile.busy)
                return
            window.quitPending = false
            if (projectFile.dirty) {
                window.pendingAction = "quit"
                window.pendingUrl = ""
                unsavedChangesDialog.open()
            } else {
                window.finishQuit()
            }
        }
    }

    Dialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        title: "Unsaved changes"
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: 460
        property bool handled: false
        onOpened: handled = false
        onRejected: {
            unsavedChangesDialog.handled = true
            window.cancelPendingAction()
        }
        onClosed: {
            if (!unsavedChangesDialog.handled)
                window.cancelPendingAction()
        }

        contentItem: Text {
            text: "This project has unsaved changes. Save them before continuing?"
            color: appTheme.text
            wrapMode: Text.Wrap
            width: unsavedChangesDialog.availableWidth
        }

        footer: RowLayout {
            spacing: 8
            ChromeButton {
                objectName: "unsavedSaveButton"
                theme: appTheme
                text: "Save"
                onClicked: window.savePendingAction()
            }
            ChromeButton {
                objectName: "unsavedDiscardButton"
                theme: appTheme
                text: "Discard"
                onClicked: window.discardPendingAction()
            }
            ChromeButton {
                objectName: "unsavedCancelButton"
                theme: appTheme
                text: "Cancel"
                onClicked: window.cancelPendingAction()
            }
        }
    }

    Dialog {
        id: restoreSessionDialog
        objectName: "restoreSessionDialog"
        title: "Project presentation"
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: 520
        property bool resolved: false
        onOpened: resolved = false
        onClosed: {
            if (!resolved)
                window.resolveRestore(false)
        }

        contentItem: Text {
            text: "This project stores a workspace layout and panel context. " +
                  "Restore that presentation, or keep the current workspace? " +
                  "Keeping the current workspace does not change processing output."
            color: appTheme.text
            wrapMode: Text.Wrap
            width: restoreSessionDialog.availableWidth
        }

        footer: RowLayout {
            spacing: 8
            ChromeButton {
                objectName: "restoreSessionButton"
                theme: appTheme
                text: "Restore Session"
                onClicked: window.resolveRestore(true)
            }
            ChromeButton {
                objectName: "keepCurrentWorkspaceButton"
                theme: appTheme
                text: "Keep Current"
                onClicked: window.resolveRestore(false)
            }
        }
    }

    Dialog {
        id: projectWarningsDialog
        objectName: "projectWarningsDialog"
        title: "Project warnings"
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: 560
        onClosed: window.openPostOpenDialogs()

        contentItem: ColumnLayout {
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: "The project opened with warnings."
                color: appTheme.text
                wrapMode: Text.Wrap
            }
            ScrollView {
                id: warningScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(320, warningColumn.implicitHeight)
                clip: true

                ColumnLayout {
                    id: warningColumn
                    // Constrain to the viewport, not the Flickable's content
                    // width: wrapping warning text must not widen the column and
                    // clip long paths at the right edge.
                    width: warningScroll.availableWidth
                    spacing: 4

                    Repeater {
                        model: projectFile.warnings
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            text: "• " + modelData
                            color: appTheme.muted
                            wrapMode: Text.Wrap
                        }
                    }
                    Repeater {
                        model: projectFile.references
                        delegate: Text {
                            required property var modelData
                            visible: !modelData.exists
                            Layout.fillWidth: true
                            text: "• " + modelData.state + " " + modelData.identity + ": " + modelData.resolvedPath
                            color: appTheme.errorText
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }

        footer: RowLayout {
            spacing: 8
            ChromeButton {
                objectName: "projectWarningsDismissButton"
                theme: appTheme
                text: "Continue"
                onClicked: projectWarningsDialog.close()
            }
        }
    }

    Dialog {
        id: recoveredCopyNotice
        objectName: "recoveredCopyNotice"
        title: "Recovered unsaved copy"
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: 520
        onClosed: window.openPostOpenDialogs()

        contentItem: Text {
            text: "Opened a protected unsaved copy. The original project file was not " +
                  "modified; use Save As to keep this copy as a new project."
            color: appTheme.text
            wrapMode: Text.Wrap
            width: recoveredCopyNotice.availableWidth
        }

        footer: RowLayout {
            spacing: 8
            ChromeButton {
                objectName: "recoveredCopyDismissButton"
                theme: appTheme
                text: "Continue"
                onClicked: recoveredCopyNotice.close()
            }
        }
    }

    Dialog {
        id: projectErrorDialog
        objectName: "projectErrorDialog"
        title: "Project file error"
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: 520
        onClosed: projectFile.clearError()

        contentItem: Text {
            text: projectFile.error.length > 0 ? projectFile.error : "The project file operation did not complete."
            color: appTheme.errorText
            wrapMode: Text.Wrap
            width: projectErrorDialog.availableWidth
        }

        footer: RowLayout {
            spacing: 8
            ChromeButton {
                objectName: "projectErrorDismissButton"
                theme: appTheme
                text: "Close"
                onClicked: projectErrorDialog.close()
            }
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
