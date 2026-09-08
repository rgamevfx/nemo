import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Minimal tiled workspace shell. `workspace` is the WorkspaceController
// context object: it provides `root` (the recursive root node map),
// `error`, and the mutating invokables split/setRatio/setPanelType/
// setGroup/activate/addTab/closePanel/save/reset.
ApplicationWindow {
    id: window

    width: 1280
    height: 800
    minimumWidth: Math.max(900, rootNode.minimumPaneWidth)
    minimumHeight: Math.max(600, rootNode.minimumPaneHeight)
    visible: true
    title: "Nemo"
    color: "#1e1e1e"
    palette.window: "#1e1e1e"
    palette.windowText: "#d0d0d0"
    palette.base: "#242424"
    palette.text: "#d0d0d0"
    palette.button: "#333333"
    palette.buttonText: "#d0d0d0"
    palette.highlight: "#4a6fa5"
    palette.highlightedText: "#ffffff"
    readonly property var controller: workspace

    // When true, a rejected close (after failed save) is allowed through.
    property bool closeOverride: false

    onActiveChanged: {
        if (!active)
            dockDrag.cancelDrag()
    }


    Shortcut {
        sequence: "Ctrl+Shift+R"
        onActivated: window.controller.reset()
    }

    // Actual error only: hidden when no error is present (no persistent noise).
    Rectangle {
        id: errorBanner
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: workspace.error.length > 0 ? 24 : 0
        visible: workspace.error.length > 0
        color: "#5a2b2b"
        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: workspace.error
            color: "#f0d0d0"
            font.pixelSize: 12
        }
    }

    // Workspace drag/drop coordinator. Declared before the tree so the leaves
    // can register as soon as they are created. It is a transparent overlay
    // (only its preview/label children are visible while a drag is active) and
    // never handles pointer events itself.
    DockDrag {
        id: dockDrag
        objectName: "dockDrag"
        anchors.fill: parent
        z: 10
        workspace: window.controller
    }

    // The recursive workspace tree. It is a persistent node whose `node`
    // binding follows `workspace.root`; the model emits rootChanged on every
    // mutating operation except setRatio, so a handle drag never rebuilds the
    // tree (only local geometry changes while dragging) while other operations
    // refresh the layout from the current model.
    WorkspaceNode {
        id: rootNode
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: errorBanner.top
        }
        node: workspace.root
        workspace: window.controller
        drag: dockDrag
    }

    // Cancel an in-progress drag on Escape; a no-op otherwise.
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
            // Block the close and surface the real error with an escape hatch.
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
                color: "#d0d0d0"
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: workspace.error
                color: "#f0b0b0"
                wrapMode: Text.Wrap
            }
        }

        footer: RowLayout {
            spacing: 8
            layoutDirection: Qt.LeftToRight
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
