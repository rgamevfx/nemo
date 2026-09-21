import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Screenshot-led Reformat surface. Persistent values, keying and history remain
// with the shared inspector; this editor owns only layout and an unapplied format
// draft. Named formats are document-owned and always copied into a node by value.
ColumnLayout {
    id: formatEditor
    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "type"
    property var parameter
    property var controller
    property var panel

    objectName: "reformatFormatEditor_" + nodeId
    Layout.fillWidth: true
    spacing: 4

    readonly property string queryNetwork: parameter && parameter.targetNetwork !== undefined ? String(parameter.targetNetwork) : networkId
    readonly property string queryNode: parameter && parameter.targetNode !== undefined ? String(parameter.targetNode) : nodeId
    readonly property int revision: panel ? Number(panel.revision) : 0
    // The panel's live interaction token, or empty when this editor has no
    // host. A control compares it with the token IT captured, so live means the
    // gesture it began is still the session's.
    readonly property string panelActiveToken: panel ? String(panel.activeToken) : ""
    readonly property int frame: controller ? Number(controller.frame) : 0
    readonly property int dragThreshold: controller ? Number(controller.dragDistance) : 4
    readonly property bool editable: !!controller && !!panel
    property var paramRows: ({})
    property var presets: []
    property string schemaProblem: ""
    property string formatProblem: ""
    readonly property string gestureProblem: panel && paramRows[String(panel.gestureErrorKey)] ? String(panel.gestureError) : ""
    readonly property string formatType: valueOf("type", "format")
    readonly property bool customSource: valueOf("formatSource", "composition") === "custom"
    readonly property var compositionFormat: {
        revision;
        return controller && queryNetwork.length ? controller.networkFormat(queryNetwork) : ({});
    }
    readonly property real displayedWidth: customSource ? Number(valueOf("width", 1920)) : Number(compositionFormat.width || 0)
    readonly property real displayedHeight: customSource ? Number(valueOf("height", 1080)) : Number(compositionFormat.height || 0)
    readonly property real displayedPixelAspect: customSource ? Number(valueOf("pixelAspect", 1)) : Number(compositionFormat.pixelAspect || 1)

    onRevisionChanged: refresh()
    onFrameChanged: refresh()
    onQueryNetworkChanged: refresh()
    onQueryNodeChanged: refresh()
    onControllerChanged: refresh()
    onPanelChanged: refresh()
    Component.onCompleted: refresh()

    function refresh() {
        var rows = ({});
        if (controller && queryNetwork.length && queryNode.length) {
            var inspector = controller.parameterInspector(queryNetwork, queryNode);
            var sections = inspector.sections || [];
            for (var i = 0; i < sections.length; ++i) {
                var parameters = sections[i].parameters || [];
                for (var j = 0; j < parameters.length; ++j)
                    rows[String(parameters[j].key)] = parameters[j];
            }
            schemaProblem = inspector.available === false ? String(inspector.reason || "") : "";
            presets = controller.namedFormats();
        }
        paramRows = rows;
    }
    function valueOf(key, fallback) {
        var row = paramRows[key];
        return row && row.value !== undefined && row.value !== null ? row.value : fallback;
    }
    function rowFor(key) {
        return { networkId: networkId, nodeId: nodeId, parameterKey: key,
                 parameter: paramRows[key], label: paramRows[key] ? paramRows[key].label : key };
    }
    function keyStatusOf(key) {
        revision; frame;
        return panel ? String(panel.parameterKeyStatusFor(networkId, nodeId, key)) : "none";
    }
    function keyAtFrame(key) {
        return panel ? panel.keyParameterAtFrame(networkId, nodeId, key) : false;
    }
    function commitValue(key, value) {
        return panel ? panel.gestureSingle(rowFor(key), value) : false;
    }
    function gestureFormat(values) {
        if (!panel)
            return false;
        var token = panel.beginEditForMany(networkId, nodeId, Object.keys(values));
        if (token.length === 0)
            return false;
        if (!panel.updateEditMany(token, values)) {
            formatProblem = controller ? String(controller.error) : "";
            panel.cancelEdit(token);
            return false;
        }
        if (!panel.commitEdit(token)) {
            formatProblem = controller ? String(controller.error) : "";
            return false;
        }
        formatProblem = "";
        refresh();
        return true;
    }
    function dimensions(width, height, aspect) {
        return Math.round(width) + "x" + Math.round(height) + (Number(aspect) === 1 ? "" : " @" + Number(Number(aspect).toPrecision(5)));
    }
    function matchingPreset() {
        if (!customSource)
            return -1;
        for (var i = 0; i < presets.length; ++i) {
            var preset = presets[i];
            if (Number(preset.width) === displayedWidth && Number(preset.height) === displayedHeight
                    && Number(preset.pixelAspect) === displayedPixelAspect)
                return i;
        }
        return -1;
    }
    readonly property var outputChoices: {
        var entries = ["Composition " + dimensions(compositionFormat.width || 0, compositionFormat.height || 0, compositionFormat.pixelAspect || 1)];
        for (var i = 0; i < presets.length; ++i)
            entries.push(String(presets[i].name) + " " + dimensions(presets[i].width, presets[i].height, presets[i].pixelAspect));
        entries.push("Custom...");
        return entries;
    }
    readonly property int outputIndex: !customSource ? 0 : matchingPreset() >= 0 ? matchingPreset() + 1 : outputChoices.length - 1
    readonly property string outputReadout: !customSource ? outputChoices[0]
        : matchingPreset() >= 0 ? outputChoices[matchingPreset() + 1]
        : "Custom " + dimensions(displayedWidth, displayedHeight, displayedPixelAspect)

    function selectOutput(index) {
        if (index === 0)
            gestureFormat({ formatSource: "composition" });
        else if (index <= presets.length) {
            var preset = presets[index - 1];
            gestureFormat({ formatSource: "custom", width: Number(preset.width),
                            height: Number(preset.height), pixelAspect: Number(preset.pixelAspect) });
        } else
            openFormatEditor();
    }

    // Reuse the shared key menu and exposure gesture without a column of idle
    // key buttons. Alt-click keys; right-click opens the existing key menu.
    component ParameterLabel: ExposureLabel {
        id: caption
        required property string editKey
        property bool keyingEnabled: true
        theme: formatEditor.theme
        networkId: formatEditor.networkId
        instanceId: formatEditor.instanceId
        nodeId: formatEditor.nodeId
        parameterKey: editKey
        frame: formatEditor.frame
        keyStatus: formatEditor.keyStatusOf(editKey)
        implicitWidth: metrics.advanceWidth
        implicitHeight: 23
        onKeyRequested: if (keyingEnabled) formatEditor.keyAtFrame(editKey)
        ToolTip.text: keyingEnabled ? tooltipText() + " Right-click for key actions." : "Apply the format before keying these dimensions."
        TextMetrics { id: metrics; text: caption.labelText; font.pixelSize: formatEditor.theme.fontSize }
        KeyIndicator {
            id: keyActions
            visible: false
            theme: formatEditor.theme
            networkId: formatEditor.networkId
            nodeId: formatEditor.nodeId
            parameterKey: caption.editKey
            parameterLabel: caption.labelText
            keyStatus: caption.keyStatus
            frame: formatEditor.frame
            revealAvailable: formatEditor.panel ? formatEditor.panel.groupHasAnimationPanel() : false
            onKeyRequested: formatEditor.keyAtFrame(caption.editKey)
            onRemoveKeyRequested: formatEditor.panel.removeParameterKeyAtFrame(formatEditor.networkId, formatEditor.nodeId, caption.editKey)
            onRevealRequested: formatEditor.panel.revealInAnimation(formatEditor.networkId, formatEditor.nodeId, caption.editKey)
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: if (caption.keyingEnabled) keyActions.openMenu(caption)
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            visible: caption.keyStatus !== "none"
            color: caption.keyStatus === "key" ? formatEditor.theme.accent : formatEditor.theme.muted
        }
    }
    component ParameterChoice: StudioComboBox {
        id: choice
        required property string editKey
        readonly property var values: formatEditor.paramRows[editKey] ? formatEditor.paramRows[editKey].choices : []
        property var labels: values
        objectName: "choice_" + formatEditor.nodeId + "_" + editKey
        theme: formatEditor.theme
        model: labels
        implicitHeight: 23
        leftPadding: 4
        rightPadding: 16
        enabled: formatEditor.editable
        currentIndex: Math.max(0, values.indexOf(String(formatEditor.valueOf(editKey, ""))))
        function restoreSelection() {
            currentIndex = Qt.binding(function() { return Math.max(0, choice.values.indexOf(String(formatEditor.valueOf(choice.editKey, "")))); });
        }
        onModelChanged: Qt.callLater(restoreSelection)
        onActivated: {
            formatEditor.commitValue(editKey, String(values[currentIndex]));
            restoreSelection();
        }
    }
    component Flag: CheckBox {
        id: flag
        required property string editKey
        objectName: "toggle_" + formatEditor.nodeId + "_" + editKey
        enabled: formatEditor.editable
        checked: formatEditor.valueOf(editKey, false) === true
        implicitHeight: 23
        implicitWidth: label.implicitWidth + 16
        padding: 0
        leftPadding: 16
        spacing: 4
        Accessible.name: text
        onToggled: {
            formatEditor.commitValue(editKey, checked);
            checked = Qt.binding(function() { return formatEditor.valueOf(flag.editKey, false) === true; });
        }
        contentItem: ParameterLabel {
            id: label
            editKey: flag.editKey
            labelText: flag.text
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: function(mouse) { mouse.accepted = mouse.modifiers === Qt.NoModifier; }
                onClicked: formatEditor.commitValue(flag.editKey, !flag.checked)
            }
        }
        indicator: Rectangle {
            x: 0
            y: (flag.height - height) / 2
            width: 12
            height: 12
            radius: 2
            color: flag.checked ? formatEditor.theme.accent : formatEditor.theme.field
            border.color: flag.activeFocus ? formatEditor.theme.accent : formatEditor.theme.border
            Text {
                anchors.centerIn: parent
                text: flag.checked ? "\u00d7" : ""
                color: formatEditor.theme.text
                font.pixelSize: 13
            }
        }
        background: Rectangle { color: flag.hovered ? formatEditor.theme.hover : "transparent"; radius: 2 }
    }
    component NumberCell: RowLayout {
        id: numberCell
        required property string editKey
        required property string label
        property bool integer: false
        // This cell's own gesture token: the preview, the commit and the cancel
        // only ever name the gesture it began.
        property string gestureToken: ""
        spacing: 4
        ParameterLabel { editKey: numberCell.editKey; labelText: numberCell.label }
        NumericField {
            objectName: "reformat_" + formatEditor.nodeId + "_" + numberCell.editKey
            interactionOwner: formatEditor.panel
            theme: formatEditor.theme
            value: Number(formatEditor.valueOf(numberCell.editKey, 1))
            label: numberCell.label
            integer: numberCell.integer
            hasMinimum: false
            hasMaximum: false
            hasSoftMinimum: false
            hasSoftMaximum: false
            fieldWidth: 62
            step: numberCell.integer ? 1 : 0.01
            dragThreshold: formatEditor.dragThreshold
            gestureLive: numberCell.gestureToken.length > 0
                         && formatEditor.panelActiveToken === numberCell.gestureToken
            enabled: formatEditor.editable
            onTextCommitted: function(text) { formatEditor.panel.gestureText(formatEditor.rowFor(numberCell.editKey), text); }
            onTextRejected: function(text) { formatEditor.panel.rejectText(formatEditor.rowFor(numberCell.editKey), text); }
            onStepped: function(value) { formatEditor.commitValue(numberCell.editKey, numberCell.integer ? Math.round(value) : value); }
            onScrubStarted: numberCell.gestureToken = formatEditor.panel.beginScrub(formatEditor.rowFor(numberCell.editKey))
            onScrubbed: function(value) {
                formatEditor.panel.updateScrub(numberCell.gestureToken, numberCell.integer ? Math.round(value) : value);
            }
            onScrubFinished: {
                var token = numberCell.gestureToken;
                numberCell.gestureToken = "";
                formatEditor.panel.finishScrub(token);
            }
            onScrubCancelled: {
                var token = numberCell.gestureToken;
                numberCell.gestureToken = "";
                formatEditor.panel.cancelScrub(token);
            }
            onKeyRequested: formatEditor.keyAtFrame(numberCell.editKey)
        }
    }
    component ActionButton: Button {
        id: action
        implicitHeight: 23
        padding: 5
        font.pixelSize: formatEditor.theme.fontSize
        contentItem: Text {
            text: action.text
            font: action.font
            color: action.enabled ? formatEditor.theme.text : formatEditor.theme.disabled
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: formatEditor.theme.smallRadius
            color: action.down ? formatEditor.theme.raised : action.hovered ? formatEditor.theme.hover : formatEditor.theme.field
            border.color: action.activeFocus ? formatEditor.theme.accent : formatEditor.theme.border
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        ParameterLabel { editKey: "type"; labelText: "type"; Layout.preferredWidth: 76 }
        ParameterChoice { editKey: "type"; labels: ["to format", "to box", "scale"]; Layout.preferredWidth: 100 }
        Item { Layout.fillWidth: true }
    }
    RowLayout {
        visible: formatEditor.formatType === "format"
        Layout.fillWidth: true
        spacing: 6
        ParameterLabel { editKey: "formatSource"; labelText: "output format"; Layout.preferredWidth: 76 }
        StudioComboBox {
            id: outputBox
            objectName: "reformatOutput_" + formatEditor.nodeId
            theme: formatEditor.theme
            model: formatEditor.outputChoices
            displayText: formatEditor.outputReadout
            currentIndex: formatEditor.outputIndex
            Layout.fillWidth: true
            implicitHeight: 23
            enabled: formatEditor.editable
            Accessible.name: "Output format"
            function restoreSelection() {
                currentIndex = Qt.binding(function() { return formatEditor.outputIndex; });
            }
            onModelChanged: Qt.callLater(restoreSelection)
            onActivated: {
                formatEditor.selectOutput(currentIndex);
                Qt.callLater(restoreSelection);
            }
        }
        ActionButton {
            objectName: "reformatPresetEdit_" + formatEditor.nodeId
            text: "="
            Layout.preferredWidth: 23
            enabled: formatEditor.editable
            Accessible.name: "Define or edit output format"
            ToolTip.visible: hovered
            ToolTip.text: "Custom dimensions and saved formats"
            onClicked: formatEditor.openFormatEditor()
        }
    }
    Flow {
        visible: formatEditor.formatType === "box"
        Layout.fillWidth: true
        spacing: 6
        NumberCell { editKey: "boxWidth"; label: "width"; integer: true }
        NumberCell { editKey: "boxHeight"; label: "height"; integer: true }
        NumberCell { editKey: "boxPixelAspect"; label: "pixel aspect" }
        Flag { editKey: "forceShape"; text: "force this shape" }
    }
    RowLayout {
        visible: formatEditor.formatType === "scale"
        Layout.fillWidth: true
        spacing: 6
        Text { text: "scale"; color: formatEditor.theme.text; font.pixelSize: formatEditor.theme.fontSize; Layout.preferredWidth: 76 }
        NumberCell { editKey: "scaleX"; label: "x" }
        NumberCell { editKey: "scaleY"; label: "y" }
        Item { Layout.fillWidth: true }
    }
    Rectangle { Layout.fillWidth: true; Layout.topMargin: 7; Layout.bottomMargin: 3; height: 1; color: formatEditor.theme.border }
    Flow {
        Layout.fillWidth: true
        spacing: 4
        RowLayout {
            width: 150; height: 23; spacing: 6
            ParameterLabel { editKey: "resize"; labelText: "resize type"; Layout.preferredWidth: 76 }
            ParameterChoice { editKey: "resize"; Layout.fillWidth: true }
        }
        Flag { editKey: "center"; text: "center" }
        Flag { editKey: "flip"; text: "flip" }
        Flag { editKey: "flop"; text: "flop" }
        Flag { editKey: "turn"; text: "turn" }
    }
    Flow {
        Layout.fillWidth: true
        spacing: 4
        RowLayout {
            width: 150; height: 23; spacing: 6
            ParameterLabel { editKey: "filter"; labelText: "filter"; Layout.preferredWidth: 76 }
            ParameterChoice { editKey: "filter"; Layout.fillWidth: true }
        }
        Flag { editKey: "clamp"; text: "clamp" }
        Flag { editKey: "blackOutside"; text: "black outside" }
        Flag { editKey: "preserveBoundingBox"; text: "preserve bounding box" }
    }
    Text {
        visible: text.length > 0
        Layout.fillWidth: true
        text: formatEditor.formatProblem || formatEditor.gestureProblem || formatEditor.schemaProblem
        color: formatEditor.theme.errorText
        font.pixelSize: formatEditor.theme.fontSize
        wrapMode: Text.WordWrap
    }

    property string presetProblem: ""
    property string presetDraftName: ""
    property real presetDraftWidth: 1920
    property real presetDraftHeight: 1080
    property real presetDraftPixelAspect: 1
    readonly property bool draftApplied: customSource && presetDraftWidth === displayedWidth
        && presetDraftHeight === displayedHeight && presetDraftPixelAspect === displayedPixelAspect
    function presetFor(name) {
        for (var i = 0; i < presets.length; ++i)
            if (String(presets[i].name) === String(name)) return presets[i];
        return null;
    }
    function openFormatEditor() {
        presetProblem = "";
        var match = matchingPreset();
        presetDraftName = match >= 0 ? String(presets[match].name) : "";
        presetDraftWidth = displayedWidth;
        presetDraftHeight = displayedHeight;
        presetDraftPixelAspect = displayedPixelAspect;
        formatPopup.open();
    }
    function applyDraft() {
        if (gestureFormat({ formatSource: "custom", width: Math.round(presetDraftWidth),
                            height: Math.round(presetDraftHeight), pixelAspect: presetDraftPixelAspect }))
            formatPopup.close();
        else
            presetProblem = formatProblem;
    }
    function savePreset() {
        var name = presetDraftName.trim();
        if (!name.length) {
            presetProblem = "Enter a name to save a format";
            return;
        }
        if (!controller.setNamedFormat(name, Math.round(presetDraftWidth), Math.round(presetDraftHeight), presetDraftPixelAspect))
            presetProblem = String(controller.error);
        else {
            presetProblem = "";
            refresh();
        }
    }
    function deletePreset() {
        if (!controller.removeNamedFormat(presetDraftName.trim()))
            presetProblem = String(controller.error);
        else {
            presetProblem = "";
            refresh();
        }
    }
    Popup {
        id: formatPopup
        objectName: "reformatPresetPopup_" + formatEditor.nodeId
        width: Math.min(360, Math.max(270, formatEditor.width))
        y: formatEditor.height
        margins: 8
        padding: 8
        focus: true
        Shortcut {
            sequence: "Escape"
            enabled: formatPopup.visible
            onActivated: formatPopup.close()
        }
        background: Rectangle { color: formatEditor.theme.panel; border.color: formatEditor.theme.border; radius: formatEditor.theme.smallRadius }
        contentItem: ColumnLayout {
            spacing: 6
            Text { text: "Output format"; color: formatEditor.theme.text; font.pixelSize: formatEditor.theme.fontSize }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: [{ key: "width", title: "width", draft: "presetDraftWidth", suffix: "Width" },
                            { key: "height", title: "height", draft: "presetDraftHeight", suffix: "Height" },
                            { key: "pixelAspect", title: "pixel aspect", draft: "presetDraftPixelAspect", suffix: "Aspect" }]
                    delegate: ColumnLayout {
                        id: draftCell
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2
                        ParameterLabel { editKey: draftCell.modelData.key; labelText: draftCell.modelData.title; keyingEnabled: formatEditor.draftApplied }
                        NumericField {
                            objectName: "reformatPreset" + draftCell.modelData.suffix + "_" + formatEditor.nodeId
                            theme: formatEditor.theme
                            value: formatEditor[draftCell.modelData.draft]
                            integer: draftCell.modelData.key !== "pixelAspect"
                            label: draftCell.modelData.title
                            hasMinimum: false
                            hasMaximum: false
                            hasSoftMinimum: false
                            hasSoftMaximum: false
                            fieldWidth: 64
                            Layout.fillWidth: true
                            onTextCommitted: function(text) { formatEditor[draftCell.modelData.draft] = Number(text); }
                            onStepped: function(value) { formatEditor[draftCell.modelData.draft] = value; }
                        }
                    }
                }
            }
            TextField {
                id: nameField
                objectName: "reformatPresetName_" + formatEditor.nodeId
                Layout.fillWidth: true
                implicitHeight: 23
                text: formatEditor.presetDraftName
                placeholderText: "Name (only for saving a preset)"
                color: formatEditor.theme.text
                font.pixelSize: formatEditor.theme.fontSize
                selectByMouse: true
                onTextEdited: formatEditor.presetDraftName = text
                background: Rectangle { color: formatEditor.theme.field; border.color: nameField.activeFocus ? formatEditor.theme.accent : formatEditor.theme.border; radius: formatEditor.theme.smallRadius }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 5
                ActionButton { objectName: "reformatApply_" + formatEditor.nodeId; text: "Apply"; onClicked: formatEditor.applyDraft() }
                ActionButton { objectName: "reformatPresetSave_" + formatEditor.nodeId; text: formatEditor.presetFor(formatEditor.presetDraftName.trim()) ? "Update preset" : "Save preset"; onClicked: formatEditor.savePreset() }
                ActionButton { objectName: "reformatPresetDelete_" + formatEditor.nodeId; text: "Delete"; enabled: !!formatEditor.presetFor(formatEditor.presetDraftName.trim()); onClicked: formatEditor.deletePreset() }
                ActionButton { objectName: "reformatPresetClose_" + formatEditor.nodeId; text: "Close"; onClicked: formatPopup.close() }
            }
            Text { visible: text.length > 0; text: formatEditor.presetProblem; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: formatEditor.theme.errorText; font.pixelSize: formatEditor.theme.fontSize }
        }
    }
}
