import QtQuick
import QtQuick.Controls

// Capture before opening: focus loss must not commit a text buffer merely to
// invoke its local history. The shell's ordinary Menu supplies the styling.
Menu {
    id: menu
    objectName: "editMenu"
    focus: false

    function openAt(trigger) {
        historyController.beginMenu(trigger.Window.window)
        var point = trigger.mapToItem(trigger.Window.window.contentItem, 0, trigger.height)
        x = point.x
        y = point.y + 2
        open()
    }
    onClosed: historyController.endMenu()

    HistoryMenuItem {
        objectName: "editUndoAction"
        historyMenu: menu
    }
    HistoryMenuItem {
        objectName: "editRedoAction"
        historyMenu: menu
        redo: true
    }
}
