import QtQuick
import QtQuick.Controls

MenuItem {
    id: item
    required property var historyMenu
    property bool redo: false
    text: redo ? historyController.redoText : historyController.undoText
    enabled: redo ? historyController.canRedo : historyController.canUndo
    focusPolicy: Qt.NoFocus
    hoverEnabled: false
    highlighted: pointer.containsMouse
    onTriggered: {
        if (redo)
            historyController.redo()
        else
            historyController.undo()
        historyMenu.close()
    }

    // Qt Menu forces focus from its MenuItem hoveredChanged handler even with
    // NoFocus. Own hover and pointer activation without emitting that signal,
    // preserving the text buffer and ordinary MenuItem styling/accessibility.
    MouseArea {
        id: pointer
        hoverEnabled: true
        anchors.fill: parent
        onPressed: item.down = true
        onReleased: item.down = false
        onCanceled: item.down = false
        onClicked: item.triggered()
    }
}
