import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Node-local media control for a Read node (issue #61), hosted by the generic
// inspector through ParameterEditorRegistry id "nemo.read.source". It presents
// the shared media reference the node names: the resolved file/pattern, the
// authored sequence range and time mapping, and explicit
// pending/offline/unsupported/error states. Every edit goes through
// ReadSourceController, which probes and then submits one undoable command.
ColumnLayout {
    id: readEditor

    property var theme
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "source"
    property var parameter
    property var controller
    property var panel

    // The controller is an application context property; guard for hosts that
    // load this editor without it (the generic control stays usable).
    readonly property var readSource: typeof readSourceController !== "undefined" && readSourceController ? readSourceController : null

    // Theme with fallbacks so the editor also renders in a bare test host.
    readonly property color borderColor: readEditor.theme ? readEditor.theme.border : "#30343a"
    readonly property color fieldColor: readEditor.theme ? readEditor.theme.field : "#24272c"
    readonly property color accentColor: readEditor.theme ? readEditor.theme.accent : "#3485f6"
    readonly property color textColor: readEditor.theme ? readEditor.theme.text : "#dce0e6"
    readonly property color mutedColor: readEditor.theme ? readEditor.theme.muted : "#979ea8"
    readonly property color errorColor: readEditor.theme ? readEditor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: readEditor.theme ? readEditor.theme.smallRadius : 4
    readonly property int fontSizeValue: readEditor.theme ? readEditor.theme.fontSize : 11
    readonly property int smallFontSize: Math.max(9, readEditor.fontSizeValue - 1)

    property var info: ({})
    readonly property string state: readEditor.info && readEditor.info.state !== undefined ? String(readEditor.info.state) : "unresolved"
    readonly property bool pending: readEditor.info && readEditor.info.pending === true
    readonly property string problem: readEditor.info && readEditor.info.error !== undefined ? String(readEditor.info.error) : ""
    readonly property string resolvedPath: readEditor.info && readEditor.info.resolvedPath !== undefined ? String(readEditor.info.resolvedPath) : ""

    objectName: "readSource_" + readEditor.nodeId
    spacing: 3

    function refresh() {
        if (!readEditor.readSource || readEditor.nodeId.length === 0) {
            readEditor.info = ({});
            return;
        }
        readEditor.info = readEditor.readSource.info(readEditor.networkId, readEditor.nodeId);
    }

    function fieldText(name) {
        return readEditor.info && readEditor.info[name] !== undefined ? String(readEditor.info[name]) : "";
    }

    function commitPath(text) {
        if (!readEditor.readSource)
            return;
        readEditor.readSource.setSourcePath(readEditor.networkId, readEditor.nodeId, text);
        readEditor.refresh();
    }

    function commitTiming() {
        if (!readEditor.readSource)
            return;
        readEditor.readSource.setSourceTiming(readEditor.networkId, readEditor.nodeId, offsetField.text, stepField.text,
                                             firstField.text, lastField.text);
        readEditor.refresh();
    }

    function statusText() {
        if (readEditor.pending)
            return "Probing media...";
        if (readEditor.problem.length > 0)
            return readEditor.problem;
        if (readEditor.state === "empty")
            return "No media selected. Choose a file or type a sequence pattern.";
        if (readEditor.state === "offline")
            return readEditor.resolvedPath.length > 0 ? "Offline: " + readEditor.resolvedPath : "Offline media.";
        if (readEditor.state === "ready")
            return "Online";
        return "Unresolved source reference.";
    }

    // One authored integer field; empty means "unauthored" (offset 0, step 1,
    // unbounded range) and is committed through the shared timing command.
    component FrameField: TextField {
        id: frameField
        required property string fieldName
        property int revision: readEditor.panel ? readEditor.panel.revision : 0
        Layout.fillWidth: true
        implicitHeight: 21
        font.pixelSize: readEditor.fontSizeValue
        color: readEditor.textColor
        placeholderText: "any"
        horizontalAlignment: Text.AlignRight
        selectByMouse: true
        text: readEditor.fieldText(fieldName)
        onEditingFinished: readEditor.commitTiming()
        Keys.onEscapePressed: function (event) {
            event.accepted = true;
            frameField.text = readEditor.fieldText(fieldName);
        }
        onRevisionChanged: {
            if (!activeFocus)
                frameField.text = readEditor.fieldText(fieldName);
        }
        background: Rectangle {
            color: readEditor.fieldColor
            border.color: frameField.activeFocus ? readEditor.accentColor : readEditor.borderColor
            radius: readEditor.smallRadiusValue
        }
    }

    component FrameLabel: Text {
        color: readEditor.mutedColor
        font.pixelSize: readEditor.smallFontSize
    }

    Component.onCompleted: refresh()
    onNodeIdChanged: refresh()
    onParameterChanged: refresh()
    onNetworkIdChanged: refresh()

    Connections {
        target: readEditor.readSource
        function onChanged() {
            readEditor.refresh();
        }
    }

    // A document edit from any panel (undo, Media Bin relink) republishes the
    // inspector revision; re-read authored state then. The guard keeps a bare
    // host (no panel) from warning about an absent target.
    Connections {
        target: readEditor.panel !== undefined && readEditor.panel ? readEditor.panel : null
        enabled: readEditor.panel !== undefined && readEditor.panel !== null
        function onRevisionChanged() {
            readEditor.refresh();
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        TextField {
            id: pathField
            property int revision: readEditor.panel ? readEditor.panel.revision : 0
            objectName: "readSourcePath_" + readEditor.nodeId
            Layout.fillWidth: true
            implicitHeight: 23
            font.pixelSize: readEditor.fontSizeValue
            color: readEditor.textColor
            placeholderText: "File or sequence pattern (#, @)"
            selectByMouse: true
            text: readEditor.fieldText("path")
            onEditingFinished: readEditor.commitPath(text)
            Keys.onEscapePressed: function (event) {
                event.accepted = true;
                pathField.text = readEditor.fieldText("path");
            }
            onRevisionChanged: {
                if (!activeFocus)
                    pathField.text = readEditor.fieldText("path");
            }
            background: Rectangle {
                color: readEditor.fieldColor
                border.color: pathField.activeFocus ? readEditor.accentColor : readEditor.borderColor
                radius: readEditor.smallRadiusValue
            }
        }

        ChromeButton {
            objectName: "readSourceBrowse_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Browse\u2026"
            implicitHeight: 23
            enabled: readEditor.readSource !== null && !readEditor.pending
            onClicked: {
                readEditor.readSource.chooseSource(readEditor.networkId, readEditor.nodeId);
                readEditor.refresh();
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.pending || readEditor.problem.length > 0 || readEditor.state !== "ready"

        Text {
            objectName: "readSourceStatus_" + readEditor.nodeId
            Layout.fillWidth: true
            text: readEditor.statusText()
            color: readEditor.problem.length > 0 ? readEditor.errorColor : readEditor.mutedColor
            font.pixelSize: readEditor.smallFontSize
            wrapMode: Text.WordWrap
        }

        ChromeButton {
            objectName: "readSourceRelink_" + readEditor.nodeId
            visible: readEditor.state === "offline"
            theme: readEditor.theme
            text: "Relink\u2026"
            implicitHeight: 23
            enabled: !readEditor.pending
            onClicked: {
                readEditor.readSource.chooseRelinkSource(readEditor.networkId, readEditor.nodeId);
                readEditor.refresh();
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        FrameLabel {
            text: "First"
        }
        FrameField {
            id: firstField
            objectName: "readSourceFirst_" + readEditor.nodeId
            fieldName: "firstFrame"
        }

        FrameLabel {
            text: "Last"
        }
        FrameField {
            id: lastField
            objectName: "readSourceLast_" + readEditor.nodeId
            fieldName: "lastFrame"
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        FrameLabel {
            text: "Offset"
        }
        FrameField {
            id: offsetField
            objectName: "readSourceOffset_" + readEditor.nodeId
            fieldName: "frameOffset"
        }

        FrameLabel {
            text: "Step"
        }
        FrameField {
            id: stepField
            objectName: "readSourceStep_" + readEditor.nodeId
            fieldName: "frameStep"
        }

        ChromeButton {
            objectName: "readSourceClear_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Clear"
            implicitHeight: 21
            enabled: readEditor.readSource !== null && readEditor.state !== "empty"
            onClicked: {
                readEditor.readSource.clearSource(readEditor.networkId, readEditor.nodeId);
                readEditor.refresh();
            }
        }
    }
}
