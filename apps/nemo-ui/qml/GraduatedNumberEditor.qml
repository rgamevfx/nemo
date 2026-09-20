import QtQuick
import QtQuick.Layouts

// A presentation-only composition of the host's numeric row and gesture owner.
Loader {
    id: editor
    property var theme: null
    property var panel: null
    property var rowAdapter: null

    Layout.fillWidth: true
    sourceComponent: panel ? panel.numericEditorComponent : null
    onLoaded: {
        item.theme = Qt.binding(function() { return editor.theme; });
        item.panel = Qt.binding(function() { return editor.panel; });
        item.row = Qt.binding(function() { return editor.rowAdapter; });
        item.fieldFirst = true;
        item.graduated = true;
        item.controlHeight = Qt.binding(function() { return editor.theme.inspectorControlHeight; });
        item.textSize = Qt.binding(function() { return editor.theme.inspectorFontSize; });
    }
}
