import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Reformat format editor (issue #92, stories 50-53), hosted by the generic
// inspector through ParameterEditorRegistry id "nemo.reformat.format" with
// presentation "section". The host mounts it on the `formatSource` row and
// consumes formatSource/width/height/pixelAspect, so this editor is the ONE
// control for the node's output format.
//
// It states the two reference sources — the owning document network's saved
// canvas, or an explicit custom size — and the document's own authored canvas
// presets. A preset is applied BY VALUE: its width, height and pixel aspect are
// copied into the node's own parameters and the source becomes custom, in one
// shared batch gesture (one undo entry). Editing or deleting a preset later
// therefore cannot mutate any node, and applying one never changes a network's
// canvas.
//
// Presets live in the Document, so this editor owns no registry and no model:
// it reads them through the session query and creates/updates/deletes them
// through the shared document commands. The composition choice is resolved from
// the owning network, never from the selected viewer or a Read.
ColumnLayout {
    id: formatEditor

    // Host-injected contract (ParametersPanel). A bare host may load this
    // editor without a controller; every control then states it cannot act.
    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "formatSource"
    property var parameter
    property var controller
    property var panel

    readonly property color textColor: formatEditor.theme ? formatEditor.theme.text : "#dce0e6"
    readonly property color mutedColor: formatEditor.theme ? formatEditor.theme.muted : "#979ea8"
    readonly property color borderColor: formatEditor.theme ? formatEditor.theme.border : "#30343a"
    readonly property color fieldColor: formatEditor.theme ? formatEditor.theme.field : "#24272c"
    readonly property color panelColor: formatEditor.theme ? formatEditor.theme.panel : "#1e2023"
    readonly property color raisedColor: formatEditor.theme ? formatEditor.theme.raised : "#282c31"
    readonly property color hoverColor: formatEditor.theme ? formatEditor.theme.hover : "#343940"
    readonly property color accentColor: formatEditor.theme ? formatEditor.theme.accent : "#3485f6"
    readonly property color disabledColor: formatEditor.theme ? formatEditor.theme.disabled : "#5f6670"
    readonly property color errorColor: formatEditor.theme ? formatEditor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: formatEditor.theme ? formatEditor.theme.smallRadius : 4
    readonly property int fontSizeValue: formatEditor.theme ? formatEditor.theme.fontSize : 11
    readonly property int smallFontSize: Math.max(9, formatEditor.fontSizeValue - 1)

    readonly property var sourceChoices: ["composition", "custom"]
    readonly property var sizeKeys: ["formatSource", "width", "height", "pixelAspect"]

    objectName: "reformatFormatEditor_" + formatEditor.nodeId
    Layout.fillWidth: true
    spacing: 3

    // --- query coordinates -------------------------------------------------
    readonly property string queryNetwork: formatEditor.parameter && formatEditor.parameter.targetNetwork !== undefined ? String(formatEditor.parameter.targetNetwork) : formatEditor.networkId
    readonly property string queryNode: formatEditor.parameter && formatEditor.parameter.targetNode !== undefined ? String(formatEditor.parameter.targetNode) : formatEditor.nodeId
    readonly property int revision: formatEditor.panel ? Number(formatEditor.panel.revision) : 0
    readonly property int frame: formatEditor.controller ? Number(formatEditor.controller.frame) : 0
    readonly property int dragThreshold: formatEditor.controller ? Number(formatEditor.controller.dragDistance) : 4

    // --- model -------------------------------------------------------------
    property var paramRows: ({})
    property var presets: []
    property string schemaProblem: ""
    onRevisionChanged: formatEditor.refresh()
    onFrameChanged: formatEditor.refresh()
    onNodeIdChanged: formatEditor.refresh()
    onQueryNetworkChanged: formatEditor.refresh()
    onQueryNodeChanged: formatEditor.refresh()
    onControllerChanged: formatEditor.refresh()
    onPanelChanged: formatEditor.refresh()
    Component.onCompleted: formatEditor.refresh()


    function refresh() {
        var rows = ({});
        if (formatEditor.controller && formatEditor.queryNetwork.length > 0 && formatEditor.queryNode.length > 0) {
            var inspector = formatEditor.controller.parameterInspector(formatEditor.queryNetwork, formatEditor.queryNode);
            if (inspector && inspector.available === true) {
                var sections = inspector.sections || [];
                for (var i = 0; i < sections.length; ++i) {
                    var parameters = sections[i].parameters || [];
                    for (var j = 0; j < parameters.length; ++j) {
                        if (parameters[j] && parameters[j].key !== undefined)
                            rows[String(parameters[j].key)] = parameters[j];
                    }
                }
            }
            formatEditor.schemaProblem = inspector && inspector.available === false && inspector.reason !== undefined ? String(inspector.reason) : "";
        } else {
            formatEditor.schemaProblem = "";
        }
        formatEditor.paramRows = rows;
        formatEditor.refreshPresets();
    }

    function refreshPresets() {
        if (!formatEditor.controller || !formatEditor.controller.namedFormats) {
            formatEditor.presets = [];
            return;
        }
        formatEditor.presets = formatEditor.controller.namedFormats();
    }

    function paramRow(key) {
        var entry = formatEditor.paramRows[key];
        return entry !== undefined ? entry : null;
    }

    function paramString(key, fallback) {
        var entry = formatEditor.paramRow(key);
        if (!entry || entry.value === undefined || entry.value === null)
            return fallback;
        return String(entry.value);
    }

    function paramNumber(key, fallback) {
        var entry = formatEditor.paramRow(key);
        if (!entry || entry.value === undefined || entry.value === null || entry.value.length !== undefined)
            return fallback;
        var value = Number(entry.value);
        return isFinite(value) ? value : fallback;
    }

    // The owning network's saved canvas, which the composition source resolves.
    // Never the selected viewer's or a Read's own size.
    readonly property var compositionFormat: {
        formatEditor.revision;
        formatEditor.queryNetwork;
        if (!formatEditor.controller || !formatEditor.controller.networkFormat || formatEditor.queryNetwork.length === 0)
            return ({
                    "available": false
                });
        return formatEditor.controller.networkFormat(formatEditor.queryNetwork);
    }

    readonly property string source: formatEditor.paramString("formatSource", "composition")
    readonly property bool customSource: formatEditor.source === "custom"
    // `type` selects which of the node's controls are live: only the format
    // mode uses the source and the explicit size.
    readonly property string formatType: formatEditor.paramString("type", "format")
    readonly property bool formatMode: formatEditor.formatType === "format"
    readonly property bool editable: formatEditor.controller !== null && formatEditor.controller !== undefined && formatEditor.formatMode

    // The size the fields STATE: the explicit custom values, or the composition
    // the node resolves when the source is the owning network.
    readonly property real displayedWidth: formatEditor.customSource ? formatEditor.paramNumber("width", 1920)
                                                                     : Number(formatEditor.compositionFormat.available === true ? formatEditor.compositionFormat.width : 0)
    readonly property real displayedHeight: formatEditor.customSource ? formatEditor.paramNumber("height", 1080)
                                                                      : Number(formatEditor.compositionFormat.available === true ? formatEditor.compositionFormat.height : 0)
    readonly property real displayedPixelAspect: formatEditor.customSource ? formatEditor.paramNumber("pixelAspect", 1.0)
                                                                           : Number(formatEditor.compositionFormat.available === true ? formatEditor.compositionFormat.pixelAspect : 1.0)

    // --- shared cells ------------------------------------------------------
    readonly property var cellTheme: ({
            "text": formatEditor.textColor,
            "muted": formatEditor.mutedColor,
            "accent": formatEditor.accentColor,
            "border": formatEditor.borderColor,
            "hover": formatEditor.hoverColor,
            "field": formatEditor.fieldColor,
            "panel": formatEditor.panelColor,
            "raised": formatEditor.raisedColor,
            "disabled": formatEditor.disabledColor,
            "errorText": formatEditor.errorColor,
            "smallRadius": formatEditor.smallRadiusValue,
            "fontSize": formatEditor.fontSizeValue
        })

    function ownsKey(key) {
        return key === "formatSource" || key === "width" || key === "height" || key === "pixelAspect";
    }

    function keyStatusOf(key) {
        formatEditor.revision;
        formatEditor.frame;
        if (!formatEditor.panel || key.length === 0)
            return "none";
        return String(formatEditor.panel.parameterKeyStatusFor(formatEditor.networkId, formatEditor.nodeId, key));
    }

    function keyAtFrame(key) {
        if (!formatEditor.panel || key.length === 0)
            return false;
        return formatEditor.panel.keyParameterAtFrame(formatEditor.networkId, formatEditor.nodeId, key);
    }

    function removeKeyAtFrame(key) {
        if (!formatEditor.panel || key.length === 0)
            return false;
        return formatEditor.panel.removeParameterKeyAtFrame(formatEditor.networkId, formatEditor.nodeId, key);
    }

    function revealInAnimation(key) {
        if (!formatEditor.panel || !formatEditor.panel.revealInAnimation)
            return;
        formatEditor.panel.revealInAnimation(formatEditor.networkId, formatEditor.nodeId, key);
    }

    function revealAvailable() {
        return formatEditor.panel && formatEditor.panel.groupHasAnimationPanel ? formatEditor.panel.groupHasAnimationPanel() : false;
    }

    // --- the shared gestures -----------------------------------------------
    function rowFor(key) {
        return {
            "networkId": formatEditor.networkId,
            "nodeId": formatEditor.nodeId,
            "parameterKey": key,
            "parameter": formatEditor.paramRow(key),
            "label": formatEditor.labelFor(key)
        };
    }

    function labelFor(key) {
        if (key === "formatSource")
            return "Format";
        if (key === "width")
            return "Width";
        if (key === "height")
            return "Height";
        if (key === "pixelAspect")
            return "Pixel Aspect";
        return key;
    }

    function commitValue(key, value) {
        if (!formatEditor.panel)
            return false;
        return formatEditor.panel.gestureSingle(formatEditor.rowFor(key), value);
    }

    function commitText(key, text) {
        if (!formatEditor.panel)
            return false;
        return formatEditor.panel.gestureText(formatEditor.rowFor(key), text);
    }

    function rejectText(key, text) {
        if (!formatEditor.panel)
            return;
        formatEditor.panel.rejectText(formatEditor.rowFor(key), text);
    }

    function beginScrub(key) {
        if (!formatEditor.panel)
            return;
        formatEditor.panel.beginScrub(formatEditor.rowFor(key));
    }

    function updateScrub(value) {
        return formatEditor.panel ? formatEditor.panel.updateScrub(value) : false;
    }

    function finishScrub() {
        return formatEditor.panel ? formatEditor.panel.finishScrub() : false;
    }

    function cancelScrub() {
        return formatEditor.panel ? formatEditor.panel.cancelScrub() : false;
    }

    // One atomic edit of the whole resolved size: the source and the three
    // explicit values are one gesture and one history entry.
    function gestureFormat(values) {
        if (!formatEditor.panel || !formatEditor.panel.beginEditForMany)
            return false;
        if (Object.keys(values).length === 0)
            return false;
        var token = String(formatEditor.panel.beginEditForMany(formatEditor.networkId, formatEditor.nodeId,
                                                               Object.keys(values)));
        if (token.length === 0) {
            formatEditor.formatProblem = formatEditor.controller ? String(formatEditor.controller.error) : "";
            return false;
        }
        if (formatEditor.panel.updateEditMany(values) !== true) {
            var message = formatEditor.controller ? String(formatEditor.controller.error) : "";
            formatEditor.panel.cancelEdit();
            formatEditor.formatProblem = message;
            formatEditor.refresh();
            return false;
        }
        if (formatEditor.panel.commitEdit() !== true) {
            formatEditor.formatProblem = formatEditor.controller ? String(formatEditor.controller.error) : "";
            formatEditor.refresh();
            return false;
        }
        formatEditor.formatProblem = "";
        formatEditor.refresh();
        return true;
    }

    function setSource(text) {
        if (text === formatEditor.source)
            return false;
        return formatEditor.gestureFormat({
            "formatSource": text
        });
    }

    // --- presets -----------------------------------------------------------
    // A preset is document state: selection copies its value into this node by
    // value (and states the custom source atomically), and never rewrites the
    // preset or a network canvas.
    function presetFor(name) {
        var list = formatEditor.presets || [];
        for (var index = 0; index < list.length; ++index) {
            if (String(list[index].name) === String(name))
                return list[index];
        }
        return null;
    }

    function selectPreset(name) {
        var preset = formatEditor.presetFor(name);
        if (!preset)
            return false;
        return formatEditor.gestureFormat({
            "formatSource": "custom",
            "width": Number(preset.width),
            "height": Number(preset.height),
            "pixelAspect": Number(preset.pixelAspect)
        });
    }

    property string presetProblem: ""
    property string presetDraftName: ""
    property real presetDraftWidth: 1920
    property real presetDraftHeight: 1080
    property real presetDraftPixelAspect: 1.0

    function openPresetEditor(name) {
        formatEditor.presetProblem = "";
        var preset = name !== undefined && String(name).length > 0 ? formatEditor.presetFor(name) : null;
        formatEditor.presetDraftName = preset ? String(preset.name) : "";
        formatEditor.presetDraftWidth = preset ? Number(preset.width) : formatEditor.displayedWidth;
        formatEditor.presetDraftHeight = preset ? Number(preset.height) : formatEditor.displayedHeight;
        formatEditor.presetDraftPixelAspect = preset ? Number(preset.pixelAspect) : formatEditor.displayedPixelAspect;
        presetPopup.open();
    }

    function savePreset() {
        if (!formatEditor.controller)
            return false;
        var name = String(formatEditor.presetDraftName).trim();
        if (name.length === 0) {
            formatEditor.presetProblem = "A canvas preset needs a name";
            return false;
        }
        if (!formatEditor.controller.setNamedFormat(name, Math.round(formatEditor.presetDraftWidth),
                                                    Math.round(formatEditor.presetDraftHeight),
                                                    Number(formatEditor.presetDraftPixelAspect))) {
            formatEditor.presetProblem = String(formatEditor.controller.error);
            return false;
        }
        formatEditor.presetProblem = "";
        formatEditor.refreshPresets();
        return true;
    }

    function deletePreset() {
        if (!formatEditor.controller)
            return false;
        var name = String(formatEditor.presetDraftName).trim();
        if (name.length === 0 || !formatEditor.presetFor(name)) {
            formatEditor.presetProblem = "No canvas preset named '" + name + "'";
            return false;
        }
        if (!formatEditor.controller.removeNamedFormat(name)) {
            formatEditor.presetProblem = String(formatEditor.controller.error);
            return false;
        }
        formatEditor.presetProblem = "";
        formatEditor.refreshPresets();
        return true;
    }

    property string formatProblem: ""
    readonly property string gestureProblem: formatEditor.panel && formatEditor.ownsKey(String(formatEditor.panel.gestureErrorKey)) ? String(formatEditor.panel.gestureError) : ""

    // --- presentation ------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        ExposureLabel {
            objectName: "label_" + formatEditor.nodeId + "_formatSource"
            Layout.preferredWidth: 72
            Layout.minimumWidth: 44
            Layout.maximumWidth: 72
            Layout.alignment: Qt.AlignVCenter
            theme: formatEditor.theme
            networkId: formatEditor.networkId
            instanceId: formatEditor.instanceId
            nodeId: formatEditor.nodeId
            parameterKey: "formatSource"
            labelText: "Format"
            keyStatus: formatEditor.keyStatusOf("formatSource")
            frame: formatEditor.frame
            onKeyRequested: formatEditor.keyAtFrame("formatSource")
        }

        KeyIndicator {
            objectName: "key_" + formatEditor.nodeId + "_formatSource"
            Layout.preferredWidth: 24
            Layout.maximumWidth: 24
            Layout.alignment: Qt.AlignVCenter
            theme: formatEditor.cellTheme
            networkId: formatEditor.networkId
            nodeId: formatEditor.nodeId
            parameterKey: "formatSource"
            parameterLabel: "Format"
            keyStatus: formatEditor.keyStatusOf("formatSource")
            frame: formatEditor.frame
            revealAvailable: formatEditor.revealAvailable()
            onKeyRequested: formatEditor.keyAtFrame("formatSource")
            onRemoveKeyRequested: formatEditor.removeKeyAtFrame("formatSource")
            onRevealRequested: formatEditor.revealInAnimation("formatSource")
        }

        StudioComboBox {
            id: sourceBox
            objectName: "reformatSource_" + formatEditor.nodeId
            theme: formatEditor.theme
            model: formatEditor.sourceChoices
            currentIndex: Math.max(0, formatEditor.sourceChoices.indexOf(formatEditor.source))
            Layout.fillWidth: true
            implicitHeight: 23
            enabled: formatEditor.editable
            Accessible.name: "Reformat format source"
            ToolTip.visible: hovered
            ToolTip.text: "Composition uses the owning network's saved canvas; custom uses this node's own width, height and pixel aspect."
            onActivated: formatEditor.setSource(currentText)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        Repeater {
            model: [{
                "key": "width",
                "label": "W"
            }, {
                "key": "height",
                "label": "H"
            }, {
                "key": "pixelAspect",
                "label": "PAR"
            }]
            delegate: ColumnLayout {
                id: sizeCell
                required property var modelData

                readonly property string sizeKey: String(modelData.key)
                readonly property real sizeValue: sizeCell.sizeKey === "width" ? formatEditor.displayedWidth
                                                      : sizeCell.sizeKey === "height" ? formatEditor.displayedHeight
                                                                                      : formatEditor.displayedPixelAspect
                readonly property string sizeError: String(formatEditor.panel ? formatEditor.panel.gestureErrorKey : "") === sizeCell.sizeKey ? formatEditor.gestureProblem : ""

                Layout.fillWidth: true
                spacing: 1

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    ExposureLabel {
                        objectName: "label_" + formatEditor.nodeId + "_" + sizeCell.sizeKey
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        theme: formatEditor.theme
                        networkId: formatEditor.networkId
                        instanceId: formatEditor.instanceId
                        nodeId: formatEditor.nodeId
                        parameterKey: sizeCell.sizeKey
                        labelText: sizeCell.modelData.label
                        keyStatus: formatEditor.keyStatusOf(sizeCell.sizeKey)
                        frame: formatEditor.frame
                        onKeyRequested: formatEditor.keyAtFrame(sizeCell.sizeKey)
                    }

                    KeyIndicator {
                        objectName: "key_" + formatEditor.nodeId + "_" + sizeCell.sizeKey
                        Layout.preferredWidth: 24
                        Layout.maximumWidth: 24
                        Layout.alignment: Qt.AlignVCenter
                        theme: formatEditor.cellTheme
                        networkId: formatEditor.networkId
                        nodeId: formatEditor.nodeId
                        parameterKey: sizeCell.sizeKey
                        parameterLabel: sizeCell.modelData.label
                        keyStatus: formatEditor.keyStatusOf(sizeCell.sizeKey)
                        frame: formatEditor.frame
                        revealAvailable: formatEditor.revealAvailable()
                        onKeyRequested: formatEditor.keyAtFrame(sizeCell.sizeKey)
                        onRemoveKeyRequested: formatEditor.removeKeyAtFrame(sizeCell.sizeKey)
                        onRevealRequested: formatEditor.revealInAnimation(sizeCell.sizeKey)
                    }
                }

                NumericField {
                    id: sizeField
                    objectName: "reformat_" + formatEditor.nodeId + "_" + sizeCell.sizeKey
                    theme: formatEditor.theme
                    value: sizeCell.sizeValue
                    text: ""
                    hasMinimum: false
                    hasMaximum: false
                    hasSoftMinimum: false
                    hasSoftMaximum: false
                    step: sizeCell.sizeKey === "pixelAspect" ? 0.01 : 1
                    integer: sizeCell.sizeKey !== "pixelAspect"
                    label: sizeCell.modelData.label
                    errorText: sizeCell.sizeError
                    dragThreshold: formatEditor.dragThreshold
                    fieldWidth: 62
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    // The composition numbers are stated, not authored: only the
                    // custom source owns this node's explicit size.
                    enabled: formatEditor.editable && formatEditor.customSource
                    gestureLive: formatEditor.panel ? formatEditor.panel.activeToken.length > 0 : false
                    onTextCommitted: formatEditor.commitText(sizeCell.sizeKey, text)
                    onTextRejected: formatEditor.rejectText(sizeCell.sizeKey, text)
                    onStepped: function (value) {
                        formatEditor.commitValue(sizeCell.sizeKey, sizeCell.sizeKey === "pixelAspect" ? value : Math.round(value));
                    }
                    onScrubStarted: formatEditor.beginScrub(sizeCell.sizeKey)
                    onScrubbed: function (value) {
                        formatEditor.updateScrub(sizeCell.sizeKey === "pixelAspect" ? value : Math.round(value));
                    }
                    onScrubFinished: formatEditor.finishScrub()
                    onScrubCancelled: formatEditor.cancelScrub()
                    onKeyRequested: formatEditor.keyAtFrame(sizeCell.sizeKey)
                }
            }
        }
    }

    Text {
        objectName: "reformatComposition_" + formatEditor.nodeId
        visible: !formatEditor.customSource
        Layout.fillWidth: true
        text: formatEditor.compositionFormat.available === true
              ? "Composition \u00b7 " + Math.round(formatEditor.compositionFormat.width) + " x " + Math.round(formatEditor.compositionFormat.height) + " @" + formatEditor.compositionFormat.pixelAspect
              : (formatEditor.compositionFormat.reason !== undefined && String(formatEditor.compositionFormat.reason).length > 0
                 ? String(formatEditor.compositionFormat.reason) : "The owning network states no canvas")
        color: formatEditor.mutedColor
        font.pixelSize: formatEditor.smallFontSize
        elide: Text.ElideRight
        Accessible.name: text
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        StudioComboBox {
            id: presetBox
            objectName: "reformatPreset_" + formatEditor.nodeId
            theme: formatEditor.theme
            model: {
                var names = ["Custom size"];
                var list = formatEditor.presets || [];
                for (var index = 0; index < list.length; ++index)
                    names.push(String(list[index].name));
                return names;
            }
            currentIndex: 0
            Layout.fillWidth: true
            implicitHeight: 23
            enabled: formatEditor.formatMode
            Accessible.name: "Canvas preset"
            ToolTip.visible: hovered
            ToolTip.text: "Apply a saved canvas preset by value. The preset is copied into this node; changing the preset later never changes the node."
            onActivated: {
                if (currentIndex <= 0)
                    return;
                formatEditor.selectPreset(currentText);
            }
        }

        Button {
            id: presetEditButton
            objectName: "reformatPresetEdit_" + formatEditor.nodeId
            implicitWidth: 56
            implicitHeight: 23
            padding: 0
            text: "Presets"
            Accessible.name: "Create, change or delete canvas presets"
            onClicked: formatEditor.openPresetEditor("")
            contentItem: Text {
                text: presetEditButton.text
                color: presetEditButton.enabled ? formatEditor.textColor : formatEditor.disabledColor
                font.pixelSize: formatEditor.smallFontSize
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: presetEditButton.down ? formatEditor.raisedColor : presetEditButton.hovered ? formatEditor.hoverColor : "transparent"
                border.color: formatEditor.borderColor
                radius: formatEditor.smallRadiusValue
            }
        }
    }

    Text {
        objectName: "reformatError_" + formatEditor.nodeId
        visible: formatEditor.formatProblem.length > 0 || formatEditor.gestureProblem.length > 0 || formatEditor.schemaProblem.length > 0
        Layout.fillWidth: true
        text: formatEditor.formatProblem.length > 0 ? formatEditor.formatProblem
            : formatEditor.gestureProblem.length > 0 ? formatEditor.gestureProblem : formatEditor.schemaProblem
        color: formatEditor.errorColor
        font.pixelSize: formatEditor.smallFontSize
        elide: Text.ElideRight
        wrapMode: Text.WordWrap
        Accessible.name: text
    }

    // Compact preset editor: create/update and delete one document-owned canvas
    // preset through the shared commands. It never edits a node and never
    // rewrites a network's canvas.
    Popup {
        id: presetPopup
        objectName: "reformatPresetPopup_" + formatEditor.nodeId
        width: Math.max(190, formatEditor.width)
        parent: formatEditor
        x: 0
        y: formatEditor.height
        modal: false
        focus: true
        padding: 6
        background: Rectangle {
            color: formatEditor.panelColor
            radius: formatEditor.smallRadiusValue
            border.color: formatEditor.borderColor
        }
        contentItem: ColumnLayout {
            spacing: 3

            Text {
                text: "Canvas presets"
                color: formatEditor.textColor
                font.pixelSize: formatEditor.fontSizeValue
            }

            TextField {
                id: presetNameField
                objectName: "reformatPresetName_" + formatEditor.nodeId
                Layout.fillWidth: true
                implicitHeight: 22
                font.pixelSize: formatEditor.smallFontSize
                color: formatEditor.textColor
                selectByMouse: true
                placeholderText: "Preset name"
                onTextEdited: formatEditor.presetDraftName = text
                background: Rectangle {
                    color: formatEditor.fieldColor
                    radius: formatEditor.smallRadiusValue
                    border.color: presetNameField.activeFocus ? formatEditor.accentColor : formatEditor.borderColor
                }
                Accessible.name: "Preset name"
            }

            // The draft name is a binding, never an imperative assignment: an
            // assignment from the edit gesture would destroy it and leave the
            // field and the draft disagreeing after the popup is reopened.
            Binding {
                target: presetNameField
                property: "text"
                value: formatEditor.presetDraftName
                when: !presetNameField.activeFocus
                restoreMode: Binding.RestoreNone
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 3

                NumericField {
                    objectName: "reformatPresetWidth_" + formatEditor.nodeId
                    theme: formatEditor.theme
                    value: formatEditor.presetDraftWidth
                    integer: true
                    hasMinimum: false
                    hasMaximum: false
                    hasSoftMinimum: false
                    hasSoftMaximum: false
                    label: "Preset width"
                    fieldWidth: 54
                    Layout.fillWidth: true
                    onTextCommitted: function(text) { formatEditor.presetDraftWidth = Number(text) }
                }

                NumericField {
                    objectName: "reformatPresetHeight_" + formatEditor.nodeId
                    theme: formatEditor.theme
                    value: formatEditor.presetDraftHeight
                    integer: true
                    hasMinimum: false
                    hasMaximum: false
                    hasSoftMinimum: false
                    hasSoftMaximum: false
                    label: "Preset height"
                    fieldWidth: 54
                    Layout.fillWidth: true
                    onTextCommitted: function(text) { formatEditor.presetDraftHeight = Number(text) }
                }

                NumericField {
                    objectName: "reformatPresetAspect_" + formatEditor.nodeId
                    theme: formatEditor.theme
                    value: formatEditor.presetDraftPixelAspect
                    hasMinimum: false
                    hasMaximum: false
                    hasSoftMinimum: false
                    hasSoftMaximum: false
                    step: 0.01
                    label: "Preset pixel aspect"
                    fieldWidth: 54
                    Layout.fillWidth: true
                    onTextCommitted: function(text) { formatEditor.presetDraftPixelAspect = Number(text) }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 3

                Button {
                    id: presetSaveButton
                    objectName: "reformatPresetSave_" + formatEditor.nodeId
                    Layout.fillWidth: true
                    implicitHeight: 22
                    padding: 0
                    text: formatEditor.presetFor(formatEditor.presetDraftName) ? "Update" : "Create"
                    Accessible.name: text + " canvas preset"
                    onClicked: formatEditor.savePreset()
                    contentItem: Text {
                        text: presetSaveButton.text
                        color: formatEditor.textColor
                        font.pixelSize: formatEditor.smallFontSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: presetSaveButton.down ? formatEditor.raisedColor : presetSaveButton.hovered ? formatEditor.hoverColor : "transparent"
                        border.color: formatEditor.borderColor
                        radius: formatEditor.smallRadiusValue
                    }
                }

                Button {
                    id: presetDeleteButton
                    objectName: "reformatPresetDelete_" + formatEditor.nodeId
                    Layout.fillWidth: true
                    implicitHeight: 22
                    padding: 0
                    enabled: formatEditor.presetFor(formatEditor.presetDraftName) !== null
                    text: "Delete"
                    Accessible.name: "Delete canvas preset"
                    onClicked: formatEditor.deletePreset()
                    contentItem: Text {
                        text: presetDeleteButton.text
                        color: presetDeleteButton.enabled ? formatEditor.textColor : formatEditor.disabledColor
                        font.pixelSize: formatEditor.smallFontSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: presetDeleteButton.down ? formatEditor.raisedColor : presetDeleteButton.hovered ? formatEditor.hoverColor : "transparent"
                        border.color: formatEditor.borderColor
                        radius: formatEditor.smallRadiusValue
                    }
                }

                Button {
                    id: presetCloseButton
                    objectName: "reformatPresetClose_" + formatEditor.nodeId
                    Layout.fillWidth: true
                    implicitHeight: 22
                    padding: 0
                    text: "Close"
                    Accessible.name: "Close preset editor"
                    onClicked: presetPopup.close()
                    contentItem: Text {
                        text: presetCloseButton.text
                        color: formatEditor.textColor
                        font.pixelSize: formatEditor.smallFontSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: presetCloseButton.down ? formatEditor.raisedColor : presetCloseButton.hovered ? formatEditor.hoverColor : "transparent"
                        border.color: formatEditor.borderColor
                        radius: formatEditor.smallRadiusValue
                    }
                }
            }

            Text {
                objectName: "reformatPresetError_" + formatEditor.nodeId
                visible: formatEditor.presetProblem.length > 0
                Layout.fillWidth: true
                text: formatEditor.presetProblem
                color: formatEditor.errorColor
                font.pixelSize: formatEditor.smallFontSize
                elide: Text.ElideRight
                wrapMode: Text.WordWrap
                Accessible.name: formatEditor.presetProblem
            }
        }
    }
}
