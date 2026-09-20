import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Write delivery editor (issue #94, recomposed for the shared inspector
// foundation in issue #102), hosted by the generic inspector through
// ParameterEditorRegistry id "nemo.write.delivery" with presentation "section".
// The host mounts it on the `file` row and consumes the Write node's other
// delivery settings.
//
// Composition: one row per authored group in the owner's reference order —
// channels, file (path / browse / file type), frames (first / last / offset),
// format (the authored format's own settings), color (mode / transform) with
// the optional LUT beneath it, then the directory flags beside the explicit
// Render action, with the job's own cancel / progress / result / failure state
// beneath them. There are no tabs and no render-mode selector: the authored
// frame range is the only thing that decides which frames a job delivers.
//
// The authored state stays the typed node parameters: every cell is read
// through the shared inspector query (frame-evaluated) and written through the
// shared panel gesture, so a typed value, an arrow-key step and a scrub are all
// exactly one validated undo entry with the existing key-at-frame semantics.
// Nothing here is a second settings model, and nothing here re-implements a
// control: the numeric cells, the choices, the check boxes, the label cells and
// the value menu they host all come from the shared foundation, which owns the
// gesture, the validation, the animation actions and the appearance. The
// channels row is the node's own authored selection — `all`, `rgb`, `rgba` or
// `alpha` — which the delivery seam resolves against the evaluated frame's own
// channel names; this editor never maps or renames a channel itself.
//
// The format row states exactly what the AUTHORED format consumes — EXR
// precision and compression, MOV profile and frame rate, MP4 bitrate and frame
// rate — and the color rows state the output color mode, the transform that
// mode selects (discovered by the delivery adapter from the active project
// config) and the optional LUT path. A hidden setting is not an authored one:
// the value travels to the seam unchanged when its format returns.
//
// Delivery itself is not authored state. "Render" submits the node's resolved
// settings to the ONE delivery-job seam (issue #94 stories 74-86) as an
// external side effect: it never enters document history, so undo can never
// claim to reverse an export (story 86). Ordinary playback through a Write node
// writes nothing at all (story 73) — this editor is the only path that can
// create files, and only when the artist presses Render. Submitting performs no
// filesystem preflight: the seam's own worker resolves and refuses the request
// before touching a file, and that refusal arrives as the job's own failure
// naming the offending path.
ColumnLayout {
    id: writeEditor

    // Host-injected contract (ParametersPanel). A bare host may load this
    // editor without a controller or a delivery seam; every control then states
    // that it cannot act instead of pretending.
    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "file"
    property var parameter
    property var controller
    property var panel

    // --- inspector metrics --------------------------------------------------
    // The shared inspector tokens, with the same fallback chain the shared
    // controls use: a custom or older theme states the base metrics and the row
    // still reads correctly instead of collapsing.
    readonly property int inspectorFontSize: writeEditor.theme && writeEditor.theme.inspectorFontSize !== undefined
                                             ? Number(writeEditor.theme.inspectorFontSize)
                                             : writeEditor.fontSizeValue
    readonly property int controlHeight: writeEditor.theme && writeEditor.theme.inspectorControlHeight !== undefined
                                         ? Number(writeEditor.theme.inspectorControlHeight)
                                         : 24
    readonly property int labelWidth: writeEditor.theme && writeEditor.theme.inspectorLabelWidth !== undefined
                                      ? Number(writeEditor.theme.inspectorLabelWidth)
                                      : 86
    readonly property int rowSpacing: writeEditor.theme && writeEditor.theme.inspectorSpacing !== undefined
                                      ? Number(writeEditor.theme.inspectorSpacing)
                                      : 6
    readonly property int fontSizeValue: writeEditor.theme ? Number(writeEditor.theme.fontSize) : 11
    readonly property int smallFontSize: Math.max(9, writeEditor.inspectorFontSize - 2)

    readonly property color textColor: writeEditor.theme ? writeEditor.theme.text : "#dce0e6"
    readonly property color mutedColor: writeEditor.theme ? writeEditor.theme.muted : "#979ea8"
    readonly property color borderColor: writeEditor.theme ? writeEditor.theme.border : "#30343a"
    readonly property color fieldColor: writeEditor.theme ? writeEditor.theme.field : "#24272c"
    readonly property color raisedColor: writeEditor.theme ? writeEditor.theme.raised : "#282c31"
    readonly property color hoverColor: writeEditor.theme ? writeEditor.theme.hover : "#343940"
    readonly property color accentColor: writeEditor.theme ? writeEditor.theme.accent : "#3485f6"
    readonly property color disabledColor: writeEditor.theme ? writeEditor.theme.disabled : "#5f6670"
    readonly property color errorColor: writeEditor.theme ? writeEditor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: writeEditor.theme ? writeEditor.theme.smallRadius : 4

    // The delivery seam installed by main.cpp. It is a context property, so a
    // bare editor host (a UI test or a preview) simply has none.
    readonly property var delivery: (typeof deliveryController !== "undefined" && deliveryController !== null) ? deliveryController : null
    readonly property bool canDeliver: writeEditor.delivery !== null && writeEditor.panel !== null && writeEditor.controller !== null
    // The seam owns the ONE native chooser; a host without one (or without the
    // browse entry point) states that instead of offering a dead button.
    readonly property bool canBrowse: writeEditor.delivery !== null && writeEditor.delivery.chooseOutputFile !== undefined

    // --- authored format and output color -----------------------------------
    // The file type and color mode as the inspector resolves them at the current
    // frame (the same values the seam will read). Each flag below only decides
    // WHICH control is presented; no path, extension or transform is ever
    // rewritten from them, so a format/extension mismatch reaches the seam
    // verbatim and is refused there with a message naming the file.
    readonly property string outputFileType: {
        var type = writeEditor.textValue("fileType");
        return type.length > 0 ? type : "exr";
    }
    readonly property bool exrFormat: writeEditor.outputFileType === "exr"
    readonly property bool movFormat: writeEditor.outputFileType === "mov"
    readonly property bool mp4Format: writeEditor.outputFileType === "mp4"
    readonly property bool movieFormat: writeEditor.movFormat || writeEditor.mp4Format
    // The setting the leading format label exposes/keys first, so the group
    // label always belongs to a control that is really on screen.
    readonly property string formatLeadKey: writeEditor.exrFormat ? "precision" : (writeEditor.movFormat ? "profile" : "bitrateKbps")
    readonly property string outputColorMode: {
        var mode = writeEditor.textValue("colorMode");
        return mode.length > 0 ? mode : "raw";
    }
    // Only `colorspace` and `display` name an explicit transform: `raw` writes
    // the working pixels and `project` resolves the project's own delivery
    // transform, so neither has an entry to choose.
    readonly property bool transformSelectable: writeEditor.outputColorMode === "colorspace" || writeEditor.outputColorMode === "display"
    // The placeholder states what the authored format expects; it never edits the
    // authored path.
    readonly property string fileHint: writeEditor.exrFormat ? "/renders/shot.####.exr" : (writeEditor.movFormat ? "/renders/shot.mov" : "/renders/shot.mp4")

    // One compact capability statement for the AUTHORED format: a movie is
    // video-only and only ProRes 4444/4444 XQ keep alpha. It is stated as a
    // tooltip, never as an audio control, and it never bakes a viewer transform
    // into the delivered pixels.
    function capabilityHint() {
        if (writeEditor.mp4Format)
            return "H.264 MP4 is video-only: no alpha is written.";
        if (writeEditor.movFormat)
            return writeEditor.textValue("profile") === "422" ? "ProRes 422 is video-only: no alpha is written." : "ProRes 4444/4444 XQ retain alpha.";
        return "OpenEXR writes every delivered channel, alpha included.";
    }

    // The entries the delivery adapter discovered for the CURRENT mode in the
    // active project config (one cached discovery per generation/config/mode).
    property var transformEntries: []
    property string transformProblem: ""

    objectName: "writeDeliveryEditor_" + writeEditor.nodeId
    Layout.fillWidth: true
    spacing: writeEditor.rowSpacing

    // --- query coordinates -------------------------------------------------
    // The inspector query addresses the real node that owns the parameters (a
    // definition network for an occurrence exposure); gestures stay on the host
    // row, so an edit can never leave its own scope.
    readonly property string queryNetwork: writeEditor.parameter && writeEditor.parameter.targetNetwork !== undefined ? String(writeEditor.parameter.targetNetwork) : writeEditor.networkId
    readonly property string queryNode: writeEditor.parameter && writeEditor.parameter.targetNode !== undefined ? String(writeEditor.parameter.targetNode) : writeEditor.nodeId
    readonly property int revision: writeEditor.panel ? Number(writeEditor.panel.revision) : 0
    readonly property int frame: writeEditor.controller ? Number(writeEditor.controller.frame) : 0
    readonly property int dragThreshold: writeEditor.controller ? Number(writeEditor.controller.dragDistance) : 4

    // --- model -------------------------------------------------------------
    // Key -> the shared inspector row for that parameter. Re-queried whenever
    // the panel revision or the frame advances, so keyed settings state the
    // value at the current frame and a keyed edit is visible immediately.
    property var paramRows: ({})
    onRevisionChanged: writeEditor.refresh()
    onFrameChanged: writeEditor.refresh()
    onNodeIdChanged: writeEditor.refresh()
    onQueryNetworkChanged: writeEditor.refresh()
    onQueryNodeChanged: writeEditor.refresh()
    onControllerChanged: writeEditor.refresh()
    onPanelChanged: writeEditor.refresh()
    Component.onCompleted: writeEditor.refresh()

    function refresh() {
        var next = ({});
        if (writeEditor.controller && writeEditor.queryNetwork.length > 0 && writeEditor.queryNode.length > 0) {
            var inspector = writeEditor.controller.parameterInspector(writeEditor.queryNetwork, writeEditor.queryNode);
            if (inspector && inspector.available === true) {
                var sections = inspector.sections || [];
                for (var i = 0; i < sections.length; ++i) {
                    var parameters = sections[i].parameters || [];
                    for (var j = 0; j < parameters.length; ++j) {
                        var entry = parameters[j];
                        if (entry && entry.key !== undefined)
                            next[String(entry.key)] = entry;
                    }
                }
            }
        }
        writeEditor.paramRows = next;
        writeEditor.refreshTransform();
    }

    // The transform entries for the CURRENT mode, discovered by the delivery
    // adapter from the ACTIVE project config (the adapter caches one discovery
    // per project generation/config/mode, so an inspector revision never opens a
    // config). Entries and a discovery failure are both stated locally: an empty
    // list for a mode that needs one is shown with its reason, never silently.
    function refreshTransform() {
        var entries = [];
        var problem = "";
        if (writeEditor.delivery && writeEditor.transformSelectable) {
            var discovered = writeEditor.delivery.transformChoices(writeEditor.outputColorMode);
            if (discovered) {
                entries = discovered.choices !== undefined && discovered.choices !== null ? discovered.choices : [];
                problem = discovered.error !== undefined && discovered.error !== null ? String(discovered.error) : "";
            } else {
                problem = "The delivery adapter cannot enumerate this config's transforms.";
            }
        }
        var changed = entries.length !== writeEditor.transformEntries.length;
        for (var i = 0; !changed && i < entries.length; ++i)
            changed = String(entries[i]) !== String(writeEditor.transformEntries[i]);
        if (changed)
            writeEditor.transformEntries = entries;
        if (problem !== writeEditor.transformProblem)
            writeEditor.transformProblem = problem;
    }

    function paramRow(key) {
        var entry = writeEditor.paramRows[key];
        return entry !== undefined ? entry : null;
    }

    function numberValue(key) {
        var entry = writeEditor.paramRow(key);
        if (!entry || entry.value === undefined || entry.value === null || entry.value.length !== undefined)
            return 0;
        return Number(entry.value);
    }

    function textValue(key) {
        var entry = writeEditor.paramRow(key);
        if (!entry || entry.value === undefined || entry.value === null)
            return "";
        return String(entry.value);
    }

    function boolValue(key) {
        var entry = writeEditor.paramRow(key);
        return entry !== undefined && entry !== null && entry.value === true;
    }

    function integerParameter(key) {
        var entry = writeEditor.paramRow(key);
        return entry !== null && String(entry.type) === "integer";
    }

    function stepOf(key) {
        var entry = writeEditor.paramRow(key);
        if (entry === null)
            return 1;
        // A declared step wins (the shared numeric control scrubs by it); an
        // integer without one steps by 1 and any other number by 0.01.
        return entry.step !== undefined ? Number(entry.step) : (writeEditor.integerParameter(key) ? 1 : 0.01);
    }

    function valueTextOf(key) {
        var entry = writeEditor.paramRow(key);
        return entry !== null && entry.valueText !== undefined ? String(entry.valueText) : "";
    }

    function choiceIndex(key) {
        var entry = writeEditor.paramRow(key);
        if (entry === null || !entry.choices)
            return 0;
        var index = entry.choices.indexOf(entry.value);
        return index < 0 ? 0 : index;
    }

    function choicesOf(key) {
        var entry = writeEditor.paramRow(key);
        return entry !== null && entry.choices ? entry.choices : [];
    }

    function ownsKey(key) {
        return writeEditor.paramRows[key] !== undefined;
    }

    // The shared controls' optional wiring, read from the SAME queried row: the
    // key status, the edit scope and the modified-from-default marker travel to
    // the control that states them, and no second model is kept here.
    function keyStatusOf(key) {
        writeEditor.revision;
        writeEditor.frame;
        if (!writeEditor.panel || key.length === 0)
            return "none";
        return String(writeEditor.panel.parameterKeyStatusFor(writeEditor.networkId, writeEditor.nodeId, key));
    }

    function scopeOf(key) {
        var entry = writeEditor.paramRow(key);
        return entry !== null && entry.scope !== undefined ? String(entry.scope) : "";
    }

    function modifiedOf(key) {
        var entry = writeEditor.paramRow(key);
        return entry !== null && entry.modified === true;
    }

    function rowFor(key) {
        return {
            "networkId": writeEditor.networkId,
            "nodeId": writeEditor.nodeId,
            "parameterKey": key,
            "parameter": writeEditor.paramRow(key),
            "label": writeEditor.labelFor(key)
        };
    }

    function labelFor(key) {
        if (key === "channels")
            return "Channels";
        if (key === "file")
            return "File";
        if (key === "fileType")
            return "File Type";
        if (key === "createDirectories")
            return "Create Directories";
        if (key === "overwrite")
            return "Overwrite Existing Files";
        if (key === "frameFirst")
            return "First";
        if (key === "frameLast")
            return "Last";
        if (key === "frameOffset")
            return "Offset";
        if (key === "precision")
            return "Precision";
        if (key === "compression")
            return "Compression";
        if (key === "profile")
            return "Profile";
        if (key === "frameRate")
            return "Frame Rate";
        if (key === "bitrateKbps")
            return "Bitrate (kbps)";
        if (key === "colorMode")
            return "Color Mode";
        if (key === "outputTransform")
            return "Output Transform";
        if (key === "lutFile")
            return "LUT";
        return key;
    }

    // A cell's tooltip: its own label, plus the authored format's capability
    // where the choice itself carries it (the file type, and the MOV profile).
    function cellHint(key) {
        var label = writeEditor.labelFor(key);
        return key === "fileType" || key === "profile" ? label + " — " + writeEditor.capabilityHint() : label;
    }

    // --- shared key and exposure affordances --------------------------------
    // The label cells own the exposure drag and the Alt-click keying gesture and
    // the value cells own the right-click value menu, both through the SAME
    // shared controls the generic rows use. No key state, command or exposure
    // rule lives here.
    function keyAtFrame(key) {
        if (!writeEditor.panel || key.length === 0)
            return false;
        return writeEditor.panel.keyParameterAtFrame(writeEditor.networkId, writeEditor.nodeId, key);
    }

    function revealAvailable() {
        return writeEditor.panel && writeEditor.panel.groupHasAnimationPanel ? writeEditor.panel.groupHasAnimationPanel() : false;
    }

    // --- the shared gestures -----------------------------------------------
    function commitValue(key, value) {
        if (!writeEditor.panel)
            return false;
        return writeEditor.panel.gestureSingle(writeEditor.rowFor(key), value);
    }

    function commitText(key, text) {
        if (!writeEditor.panel)
            return false;
        return writeEditor.panel.gestureText(writeEditor.rowFor(key), text);
    }

    function rejectText(key, text) {
        if (!writeEditor.panel)
            return;
        writeEditor.panel.rejectText(writeEditor.rowFor(key), text);
    }

    function beginScrub(key) {
        if (!writeEditor.panel)
            return;
        writeEditor.panel.beginScrub(writeEditor.rowFor(key));
    }

    function updateScrub(key, value) {
        if (!writeEditor.panel)
            return false;
        return writeEditor.panel.updateScrub(value);
    }

    function finishScrub() {
        return writeEditor.panel ? writeEditor.panel.finishScrub() : false;
    }

    function cancelScrub() {
        return writeEditor.panel ? writeEditor.panel.cancelScrub() : false;
    }

    // The most recent rejected edit attributed to one of the consumed keys. The
    // message comes from the controller/catalog; this editor never re-validates.
    readonly property string gestureProblem: writeEditor.panel && writeEditor.ownsKey(String(writeEditor.panel.gestureErrorKey)) ? String(writeEditor.panel.gestureError) : ""

    // --- the native output-path chooser -------------------------------------
    // The ONE native chooser the application owns, asked through the delivery
    // adapter for THIS node's identity. The chosen path is committed through the
    // shared gesture, so a browse is one ordinary undo entry; a cancelled dialog
    // changes nothing, and an outcome for another node is ignored.
    function browseFile() {
        if (!writeEditor.canBrowse)
            return;
        writeEditor.delivery.chooseOutputFile(writeEditor.networkId, writeEditor.nodeId, writeEditor.outputFileType,
                                              writeEditor.textValue("file"));
    }

    Connections {
        target: writeEditor.delivery
        function onOutputFileChosen(chosenNetwork, chosenNode, path) {
            if (String(chosenNetwork) !== writeEditor.networkId || String(chosenNode) !== writeEditor.nodeId)
                return;
            if (String(path).length === 0)
                return;
            writeEditor.commitValue("file", String(path));
        }
    }

    // --- delivery job state -------------------------------------------------
    // The seam republishes every accepted job; `jobs` is a binding dependency so
    // the strip below re-evaluates whenever progress or a result arrives. Jobs
    // are newest first, so the node's most recent job is the first match.
    readonly property var jobs: writeEditor.delivery ? writeEditor.delivery.jobs : []
    readonly property var job: {
        var list = writeEditor.jobs || [];
        for (var i = 0; i < list.length; ++i) {
            var entry = list[i];
            if (String(entry.network) === writeEditor.networkId && String(entry.node) === writeEditor.nodeId)
                return entry;
        }
        return null;
    }
    readonly property string jobState: writeEditor.job ? String(writeEditor.job.state) : ""
    readonly property bool jobActive: writeEditor.jobState === "queued" || writeEditor.jobState === "running"
    readonly property string seamError: writeEditor.delivery ? String(writeEditor.delivery.error) : ""

    // The seam's own last refusal (a submission, cancellation, forget or browse
    // it could not answer). The editor raises none of its own: a request the
    // seam refuses before any write arrives as the JOB's own failure below,
    // naming the offending path, so nothing here repeats a preflight the seam
    // owns.
    readonly property string problemText: writeEditor.seamError

    function frameCount() {
        var first = writeEditor.numberValue("frameFirst");
        var last = writeEditor.numberValue("frameLast");
        return last >= first ? Math.round(last - first + 1) : 0;
    }

    function submit() {
        if (!writeEditor.canDeliver)
            return;
        // Submission states the request and nothing else (issue #94). The
        // authoritative preflight — settings, destination collisions, the
        // raster's own description — runs on the seam's worker before the first
        // byte is touched, so a refusal is reported as the job's own failure
        // with the offending path instead of a probe this panel would have to
        // repeat on every keystroke. The job accepted here freezes what it
        // resolved, so a later edit cannot change the frames it delivers.
        writeEditor.delivery.submit(writeEditor.networkId, writeEditor.nodeId, writeEditor.frame);
    }

    // What the last job delivers: its format, the MOV profile where the format
    // has one, the channels it wrote and the output color when a transform ran.
    // Compact by construction (the row elides) and it states only what the seam
    // reported.
    function deliverySummary() {
        var job = writeEditor.job;
        if (job === null)
            return "";
        var parts = [];
        var type = job.fileType !== undefined ? String(job.fileType) : "";
        if (type === "mov" && job.profile !== undefined && String(job.profile).length > 0)
            parts.push("MOV ProRes " + String(job.profile).toUpperCase());
        else if (type.length > 0)
            parts.push(type.toUpperCase());
        var channels = job.channels !== undefined && job.channels !== null ? job.channels : [];
        if (channels.length > 0)
            parts.push(channels.join(" "));
        var color = job.colorMode !== undefined ? String(job.colorMode) : "";
        if (color.length > 0 && color !== "raw")
            parts.push(color);
        return parts.join(" · ");
    }

    function statusText() {
        writeEditor.revision;
        var job = writeEditor.job;
        if (job === null)
            return writeEditor.paramRow("file") === null ? "" : String(writeEditor.frameCount()) + (writeEditor.frameCount() === 1 ? " frame" : " frames");
        var state = String(job.state);
        var total = Number(job.totalFrames);
        var summary = writeEditor.deliverySummary();
        var format = summary.length > 0 ? " · " + summary : "";
        if (state === "queued")
            return "queued · " + total + (total === 1 ? " frame" : " frames") + format;
        if (state === "running")
            return "rendering " + Number(job.writtenFrames) + "/" + total + format;
        if (state === "completed")
            return "delivered " + Number(job.writtenFrames) + (total === 1 ? " frame" : " frames") + format;
        if (state === "cancelled")
            return "cancelled · " + Number(job.writtenFrames) + "/" + total + " written" + format;
        var failed = Number(job.failedFrames);
        return "failed · " + Number(job.writtenFrames) + " written, " + failed + (failed === 1 ? " frame" : " frames") + " failed" + format;
    }

    // The first concrete failure the job reported, so a partial delivery always
    // names one real file instead of only a count (story 82).
    function failureText() {
        var job = writeEditor.job;
        if (job === null)
            return "";
        if (job.error !== undefined && String(job.error).length > 0)
            return String(job.error);
        var files = job.files || [];
        for (var i = 0; i < files.length; ++i) {
            if (files[i].written !== true && String(files[i].error).length > 0)
                return String(files[i].error);
        }
        return "";
    }

    // --- components ---------------------------------------------------------
    // The shared value menu for a cell that is neither a numeric field nor a
    // check box: KeyIndicator owns the wording, the enable rules and the object
    // names, and this zero-size host never adds a key-button column. A
    // right-click on the cell opens it at the pointer.
    component ValueMenu: KeyIndicator {
        id: valueMenu
        required property string cellKey
        width: 0
        height: 0
        theme: writeEditor.theme
        networkId: writeEditor.networkId
        nodeId: writeEditor.nodeId
        parameterKey: valueMenu.cellKey
        parameterLabel: writeEditor.labelFor(valueMenu.cellKey)
        keyStatus: writeEditor.keyStatusOf(valueMenu.cellKey)
        scope: writeEditor.scopeOf(valueMenu.cellKey)
        frame: writeEditor.frame
        revealAvailable: writeEditor.revealAvailable()
        resettable: writeEditor.panel !== null
        modified: writeEditor.modifiedOf(valueMenu.cellKey)
        onKeyRequested: writeEditor.keyAtFrame(valueMenu.cellKey)
        onRemoveKeyRequested: {
            if (writeEditor.panel)
                writeEditor.panel.removeParameterKeyAtFrame(writeEditor.networkId, writeEditor.nodeId, valueMenu.cellKey);
        }
        onRevealRequested: {
            if (writeEditor.panel)
                writeEditor.panel.revealInAnimation(writeEditor.networkId, writeEditor.nodeId, valueMenu.cellKey);
        }
        onResetRequested: {
            if (writeEditor.panel)
                writeEditor.panel.resetValue(writeEditor.rowFor(valueMenu.cellKey));
        }
    }

    // The group label cell: the shared label control, so exposure dragging and
    // the Alt-click keying shortcut stay exactly where they are on a consumed
    // parameter.
    component GroupLabel: ExposureLabel {
        id: groupLabel
        required property string editKey
        required property string caption
        Layout.preferredWidth: writeEditor.labelWidth
        Layout.minimumWidth: 44
        Layout.maximumWidth: writeEditor.labelWidth
        Layout.alignment: Qt.AlignVCenter
        theme: writeEditor.theme
        networkId: writeEditor.networkId
        instanceId: writeEditor.instanceId
        nodeId: writeEditor.nodeId
        parameterKey: groupLabel.editKey
        labelText: groupLabel.caption
        keyStatus: writeEditor.keyStatusOf(groupLabel.editKey)
        frame: writeEditor.frame
        textSize: writeEditor.inspectorFontSize
        controlHeight: writeEditor.controlHeight
        onKeyRequested: writeEditor.keyAtFrame(groupLabel.editKey)
        // The shared value menu for the group's own lead key: the label cell is
        // where the old key column's actions belong now, and a left press still
        // starts the exposure drag and Alt-click still keys.
        ValueMenu {
            id: groupMenu
            cellKey: groupLabel.editKey
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: groupMenu.openMenu(groupLabel)
        }
    }

    // A cell's own inline label ("first", "offset", "fps"), sized to its text.
    component InlineLabel: ExposureLabel {
        id: inlineLabel
        required property string editKey
        required property string caption
        implicitWidth: inlineMetrics.advanceWidth + 2
        Layout.alignment: Qt.AlignVCenter
        theme: writeEditor.theme
        networkId: writeEditor.networkId
        instanceId: writeEditor.instanceId
        nodeId: writeEditor.nodeId
        parameterKey: inlineLabel.editKey
        labelText: inlineLabel.caption
        keyStatus: writeEditor.keyStatusOf(inlineLabel.editKey)
        frame: writeEditor.frame
        textSize: writeEditor.inspectorFontSize
        controlHeight: writeEditor.controlHeight
        onKeyRequested: writeEditor.keyAtFrame(inlineLabel.editKey)
        TextMetrics {
            id: inlineMetrics
            text: inlineLabel.caption
            font.pixelSize: writeEditor.inspectorFontSize
        }
    }

    // One integer setting: an optional inline label beside the shared numeric
    // field, which owns typing, stepping, the scrub gesture, the exact-value
    // rules and the right-click value menu. The cell states its own natural
    // width, so a row of cells wraps instead of squeezing its fields unreadably
    // when the card is narrow.
    component NumberCell: RowLayout {
        id: numberCell
        required property string cellKey
        property string caption: ""
        spacing: 5
        InlineLabel {
            editKey: numberCell.cellKey
            caption: numberCell.caption
            visible: numberCell.caption.length > 0
        }
        NumericField {
            objectName: "writeNumber_" + writeEditor.nodeId + "_" + numberCell.cellKey
            theme: writeEditor.theme
            value: writeEditor.numberValue(numberCell.cellKey)
            text: writeEditor.valueTextOf(numberCell.cellKey)
            hasMinimum: false
            hasMaximum: false
            hasSoftMinimum: false
            hasSoftMaximum: false
            step: writeEditor.stepOf(numberCell.cellKey)
            integer: writeEditor.integerParameter(numberCell.cellKey)
            label: writeEditor.labelFor(numberCell.cellKey)
            errorText: writeEditor.panel && String(writeEditor.panel.gestureErrorKey) === numberCell.cellKey ? String(writeEditor.panel.gestureError) : ""
            dragThreshold: writeEditor.dragThreshold
            // The shared value menu and the animation status, owned by the field
            // itself when it is bound to the host.
            panel: writeEditor.panel
            networkId: writeEditor.networkId
            nodeId: writeEditor.nodeId
            parameterKey: numberCell.cellKey
            keyStatus: writeEditor.keyStatusOf(numberCell.cellKey)
            scope: writeEditor.scopeOf(numberCell.cellKey)
            frame: writeEditor.frame
            revealAvailable: writeEditor.revealAvailable()
            modified: writeEditor.modifiedOf(numberCell.cellKey)
            controlHeight: writeEditor.controlHeight
            stepper: true
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            enabled: writeEditor.controller !== null && writeEditor.paramRow(numberCell.cellKey) !== null
            gestureLive: writeEditor.panel ? writeEditor.panel.activeToken.length > 0 : false
            onTextCommitted: function(text) { writeEditor.commitText(numberCell.cellKey, text); }
            onTextRejected: function(text) { writeEditor.rejectText(numberCell.cellKey, text); }
            onStepped: function(value) { writeEditor.commitValue(numberCell.cellKey, value); }
            onScrubStarted: writeEditor.beginScrub(numberCell.cellKey)
            onScrubbed: function(value) { writeEditor.updateScrub(numberCell.cellKey, value); }
            onScrubFinished: writeEditor.finishScrub()
            onScrubCancelled: writeEditor.cancelScrub()
            onKeyRequested: writeEditor.keyAtFrame(numberCell.cellKey)
        }
    }

    // One choice setting, using the shared combo box the generic rows use.
    component ChoiceCell: StudioComboBox {
        id: choiceCell
        required property string cellKey
        // The panel revision this cell's selection was read at: assigning
        // `currentIndex` in syncSelection replaces its binding, so the shared
        // deferred readout pattern restores it whenever the authored value can
        // have changed (and after Qt resets a changed model).
        property int revision: writeEditor.revision
        theme: writeEditor.theme
        objectName: "writeChoice_" + writeEditor.nodeId + "_" + choiceCell.cellKey
        controlHeight: writeEditor.controlHeight
        textSize: writeEditor.inspectorFontSize
        Layout.fillWidth: true
        Layout.minimumWidth: 56
        enabled: writeEditor.paramRow(choiceCell.cellKey) !== null
        model: writeEditor.choicesOf(choiceCell.cellKey)
        currentIndex: writeEditor.choiceIndex(choiceCell.cellKey)
        Accessible.name: writeEditor.labelFor(choiceCell.cellKey)
        function syncSelection() {
            currentIndex = Qt.binding(function() { return writeEditor.choiceIndex(choiceCell.cellKey); });
        }
        onRevisionChanged: syncSelection()
        // Qt resets the selection after assigning a changed model.
        onModelChanged: Qt.callLater(syncSelection)
        onActivated: writeEditor.commitValue(choiceCell.cellKey, String(currentText))
        ToolTip.visible: hovered
        ToolTip.text: writeEditor.cellHint(choiceCell.cellKey)
        // The shared value menu and the supported Alt-click keying shortcut,
        // wired exactly as the generic choice row wires them: a plain click
        // still opens the combo, a right-click opens the menu at the pointer.
        ValueMenu {
            id: cellMenu
            cellKey: choiceCell.cellKey
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: function(mouse) {
                mouse.accepted = (mouse.button === Qt.RightButton) || !!(mouse.modifiers & Qt.AltModifier);
            }
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton)
                    cellMenu.openMenu(null);
                else
                    writeEditor.keyAtFrame(choiceCell.cellKey);
            }
        }
    }

    // One flag setting, using the shared check box: it reports the user's input
    // and never assigns `checked`, so the authored binding survives a refused or
    // deferred commit, and its own value menu states the key/reset actions.
    component Flag: InspectorCheckBox {
        id: flag
        required property string cellKey
        required property string caption
        objectName: "writeFlag_" + writeEditor.nodeId + "_" + flag.cellKey
        theme: writeEditor.theme
        text: flag.caption
        checked: writeEditor.boolValue(flag.cellKey)
        enabled: writeEditor.paramRow(flag.cellKey) !== null
        panel: writeEditor.panel
        networkId: writeEditor.networkId
        instanceId: writeEditor.instanceId
        nodeId: writeEditor.nodeId
        parameterKey: flag.cellKey
        keyStatus: writeEditor.keyStatusOf(flag.cellKey)
        scope: writeEditor.scopeOf(flag.cellKey)
        frame: writeEditor.frame
        revealAvailable: writeEditor.revealAvailable()
        modified: writeEditor.modifiedOf(flag.cellKey)
        onToggled: function(state) { writeEditor.commitValue(flag.cellKey, state); }
        onKeyRequested: writeEditor.keyAtFrame(flag.cellKey)
    }

    // The output transform: the shared TYPEABLE combo the Shuffle and Viewer
    // layers use, stating the active config's own entries beside the authored
    // value. A value the current config does not enumerate keeps its exact text
    // and stays committable, so a project switch never rewrites an authored
    // transform. Selection and typed text both go through the shared gesture,
    // which owns validation and the undo entry.
    component TransformCell: StudioComboBox {
        id: transformCell
        required property string cellKey
        property int revision: writeEditor.revision
        theme: writeEditor.theme
        objectName: "writeTransform_" + writeEditor.nodeId
        typeable: true
        readout: writeEditor.textValue(transformCell.cellKey)
        model: writeEditor.transformEntries
        controlHeight: writeEditor.controlHeight
        textSize: writeEditor.inspectorFontSize
        Layout.fillWidth: true
        Layout.minimumWidth: 70
        enabled: writeEditor.transformSelectable && writeEditor.paramRow(transformCell.cellKey) !== null
        Accessible.name: writeEditor.labelFor(transformCell.cellKey)
        // The panel revision is the authored-value watermark: re-state the
        // readout when it moves, exactly as the entry cells do.
        onRevisionChanged: syncReadout()
        onActivated: writeEditor.commitValue(transformCell.cellKey, String(currentText))
        onTextAccepted: function(text) { writeEditor.commitValue(transformCell.cellKey, text); }
        ToolTip.visible: hovered
        ToolTip.text: writeEditor.transformProblem.length > 0 ? writeEditor.transformProblem : writeEditor.labelFor(transformCell.cellKey) + " — applied to the delivered pixels before the LUT; color mode raw applies no transform at all."
        // The same shared value menu and keying shortcut as a choice cell: a
        // plain click still edits the entry, which stays typeable.
        ValueMenu {
            id: transformMenu
            cellKey: transformCell.cellKey
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: function(mouse) {
                mouse.accepted = (mouse.button === Qt.RightButton) || !!(mouse.modifiers & Qt.AltModifier);
            }
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton)
                    transformMenu.openMenu(null);
                else
                    writeEditor.keyAtFrame(transformCell.cellKey);
            }
        }
    }

    // A compact themed button for this editor's own actions. It is presentation
    // only: every parameter edit goes through the shared gesture.
    component Action: Button {
        id: action
        implicitHeight: writeEditor.controlHeight
        padding: 6
        contentItem: Text {
            text: action.text
            color: action.enabled ? writeEditor.textColor : writeEditor.disabledColor
            font.pixelSize: writeEditor.inspectorFontSize
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: action.down ? writeEditor.raisedColor : action.hovered ? writeEditor.hoverColor : writeEditor.fieldColor
            border.color: action.activeFocus ? writeEditor.accentColor : writeEditor.borderColor
            radius: writeEditor.smallRadiusValue
        }
    }

    // --- channels -----------------------------------------------------------
    // Which channels the delivery writes: the node's own authored choice, the
    // same value the seam resolves. `all` (the default) delivers every channel
    // the connected image carries.
    RowLayout {
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        GroupLabel {
            editKey: "channels"
            caption: "Channels"
        }
        ChoiceCell {
            cellKey: "channels"
        }
    }

    // One path cell: the authored path, the native chooser's own button and the
    // format it delivers.
    RowLayout {
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        GroupLabel {
            editKey: "file"
            caption: "File"
        }
        TextField {
            id: fileField
            objectName: "writeFile_" + writeEditor.nodeId
            property int revision: writeEditor.revision
            text: writeEditor.textValue("file")
            enabled: writeEditor.panel !== null
            Layout.fillWidth: true
            Layout.minimumWidth: 80
            implicitHeight: writeEditor.controlHeight
            font.pixelSize: writeEditor.inspectorFontSize
            color: writeEditor.textColor
            selectByMouse: true
            placeholderText: writeEditor.fileHint
            Accessible.name: "Output file path"
            ToolTip.visible: hovered && !activeFocus
            ToolTip.text: writeEditor.exrFormat ? "Output path or sequence pattern: each '#' is one zero-padded frame digit" : "One movie path: the whole frame range becomes this single file"
            onEditingFinished: writeEditor.commitValue("file", String(text))
            Keys.onEscapePressed: function(event) {
                event.accepted = true;
                fileField.text = writeEditor.textValue("file");
            }
            onRevisionChanged: {
                if (!activeFocus)
                    fileField.text = writeEditor.textValue("file");
            }
            background: Rectangle {
                color: writeEditor.fieldColor
                border.color: fileField.activeFocus ? writeEditor.accentColor : writeEditor.borderColor
                radius: writeEditor.smallRadiusValue
            }
        }
        // The native chooser's own affordance: a folder mark, never a second
        // text entry. A host without a chooser states that in its tooltip.
        Action {
            id: browseAction
            objectName: "writeBrowse_" + writeEditor.nodeId
            implicitWidth: 32
            enabled: writeEditor.canBrowse && writeEditor.panel !== null
            onClicked: writeEditor.browseFile()
            Accessible.name: "Browse for the output path"
            ToolTip.visible: hovered
            ToolTip.text: writeEditor.canBrowse ? "Browse for the output path" : "This host has no native file chooser; type the path instead"
            contentItem: Item {
                Rectangle {
                    x: (parent.width - 14) / 2
                    y: (parent.height - 11) / 2
                    width: 6
                    height: 3
                    radius: 1
                    color: browseAction.enabled ? writeEditor.mutedColor : writeEditor.disabledColor
                }
                Rectangle {
                    x: (parent.width - 14) / 2
                    y: (parent.height - 11) / 2 + 3
                    width: 14
                    height: 8
                    radius: 1.5
                    color: "transparent"
                    border.width: 1
                    border.color: browseAction.enabled ? writeEditor.mutedColor : writeEditor.disabledColor
                }
            }
        }
        ChoiceCell {
            cellKey: "fileType"
            Layout.fillWidth: false
            Layout.preferredWidth: 74
        }
    }

    // --- frames -------------------------------------------------------------
    // The authored range is the whole frame decision: there is no render-mode
    // selector, and a movie delivers the same authored frames as one container.
    // The cells wrap when the card is narrow instead of squeezing the fields.
    RowLayout {
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        GroupLabel {
            editKey: "frameFirst"
            caption: "Frames"
        }
        Flow {
            Layout.fillWidth: true
            spacing: writeEditor.rowSpacing
            NumberCell {
                cellKey: "frameFirst"
                caption: "first"
            }
            NumberCell {
                cellKey: "frameLast"
                caption: "last"
            }
            // File numbering is an EXR SEQUENCE setting: a movie is one container
            // named by the authored path, so the offset is absent rather than shown
            // with a value that would do nothing.
            NumberCell {
                cellKey: "frameOffset"
                caption: "offset"
                visible: writeEditor.exrFormat
            }
        }
    }

    // --- format -------------------------------------------------------------
    // ONE row for the authored format's own settings: EXR precision and
    // compression, MOV profile and frame rate, MP4 bitrate and frame rate. A
    // control whose format is not authored is absent rather than disabled with a
    // stale value, the group label keys/exposes the first control that IS
    // present, and the cells wrap when the card is narrow.
    RowLayout {
        id: formatRow
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        GroupLabel {
            editKey: writeEditor.formatLeadKey
            caption: "Format"
        }
        Flow {
            id: formatOptions
            readonly property real cellWidth: Math.max(100, (width - spacing) / 2)
            Layout.fillWidth: true
            spacing: writeEditor.rowSpacing
            ChoiceCell {
                cellKey: "precision"
                width: formatOptions.cellWidth
                visible: writeEditor.exrFormat
            }
            ChoiceCell {
                cellKey: "compression"
                width: formatOptions.cellWidth
                visible: writeEditor.exrFormat
            }
            ChoiceCell {
                cellKey: "profile"
                width: formatOptions.cellWidth
                visible: writeEditor.movFormat
            }
            NumberCell {
                cellKey: "frameRate"
                width: formatOptions.cellWidth
                caption: "fps"
                visible: writeEditor.movieFormat
            }
            NumberCell {
                cellKey: "bitrateKbps"
                width: formatOptions.cellWidth
                caption: "kbps"
                visible: writeEditor.mp4Format
            }
        }
    }

    // --- output color -------------------------------------------------------
    // The output color mode and the transform that mode selects, on one row. The
    // entry list is the ACTIVE project config's own enumeration (the delivery
    // adapter discovers it once per generation/config/mode) and the authored
    // value is stated verbatim even when the current config does not enumerate
    // it, so switching projects never rewrites a setting. A mode with no
    // explicit choice disables the entry cell instead of pretending to offer
    // one.
    RowLayout {
        id: colorRow
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        GroupLabel {
            editKey: "colorMode"
            caption: "Color"
        }
        ChoiceCell {
            cellKey: "colorMode"
            Layout.minimumWidth: 70
        }
        TransformCell {
            cellKey: "outputTransform"
        }
    }

    // A discovery failure for the transform entry list is stated locally, never
    // swallowed: an empty list under a mode that needs one must say why.
    Text {
        objectName: "writeTransformProblem_" + writeEditor.nodeId
        visible: writeEditor.transformProblem.length > 0
        Layout.fillWidth: true
        Layout.leftMargin: writeEditor.labelWidth + writeEditor.rowSpacing
        text: writeEditor.transformProblem
        color: writeEditor.errorColor
        font.pixelSize: writeEditor.smallFontSize
        wrapMode: Text.WordWrap
        Accessible.name: writeEditor.transformProblem
    }

    // The optional LUT is applied after the base transform, to primary RGB only.
    // It is a path the artist states (or pastes); the seam validates and loads it
    // when the job runs, never here.
    RowLayout {
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        GroupLabel {
            editKey: "lutFile"
            caption: "LUT"
        }
        TextField {
            id: lutField
            objectName: "writeLut_" + writeEditor.nodeId
            property int revision: writeEditor.revision
            text: writeEditor.textValue("lutFile")
            enabled: writeEditor.panel !== null
            Layout.fillWidth: true
            Layout.minimumWidth: 80
            implicitHeight: writeEditor.controlHeight
            font.pixelSize: writeEditor.inspectorFontSize
            color: writeEditor.textColor
            selectByMouse: true
            placeholderText: "Optional .cube LUT"
            Accessible.name: "LUT file"
            ToolTip.visible: hovered && !activeFocus
            ToolTip.text: "Optional .cube LUT applied after the selected output transform, to primary RGB only. Color mode raw writes the working pixels unchanged: no base conversion runs at all."
            onEditingFinished: writeEditor.commitValue("lutFile", String(text))
            Keys.onEscapePressed: function(event) {
                event.accepted = true;
                lutField.text = writeEditor.textValue("lutFile");
            }
            onRevisionChanged: {
                if (!activeFocus)
                    lutField.text = writeEditor.textValue("lutFile");
            }
            background: Rectangle {
                color: writeEditor.fieldColor
                border.color: lutField.activeFocus ? writeEditor.accentColor : writeEditor.borderColor
                radius: writeEditor.smallRadiusValue
            }
        }
    }

    // --- directory flags and the explicit render action ----------------------
    // The flags stay on the authored parameters and the Render action submits the
    // one delivery request. The flags wrap when the card is narrow; the action
    // stays on the trailing edge.
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 2
        spacing: writeEditor.rowSpacing
        Flow {
            Layout.fillWidth: true
            spacing: writeEditor.rowSpacing * 2
            Flag {
                cellKey: "createDirectories"
                caption: "create directories"
            }
            Flag {
                cellKey: "overwrite"
                caption: "overwrite existing files"
            }
        }
        Button {
            id: renderAction
            objectName: "writeDeliver_" + writeEditor.nodeId
            text: writeEditor.jobActive ? "Rendering" : "Render"
            implicitHeight: writeEditor.controlHeight
            padding: 8
            enabled: writeEditor.canDeliver && !writeEditor.jobActive && writeEditor.textValue("file").length > 0
            onClicked: writeEditor.submit()
            Accessible.name: renderAction.text
            ToolTip.visible: hovered
            ToolTip.text: writeEditor.canDeliver ? "Render this node's frames through the delivery job seam. Undo cannot reverse it." : "The delivery seam is unavailable in this host"
            contentItem: RowLayout {
                spacing: 7
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: "\u25b6"
                    color: renderAction.enabled ? writeEditor.textColor : writeEditor.disabledColor
                    font.pixelSize: Math.max(8, writeEditor.inspectorFontSize - 3)
                }
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: renderAction.text
                    color: renderAction.enabled ? writeEditor.textColor : writeEditor.disabledColor
                    font.pixelSize: writeEditor.inspectorFontSize
                    verticalAlignment: Text.AlignVCenter
                }
            }
            background: Rectangle {
                color: renderAction.down ? writeEditor.raisedColor : renderAction.hovered ? writeEditor.hoverColor : writeEditor.fieldColor
                border.color: renderAction.activeFocus ? writeEditor.accentColor : writeEditor.borderColor
                radius: writeEditor.smallRadiusValue
            }
        }
    }

    // --- job feedback -------------------------------------------------------
    // Cancellation, progress, the result and the failure, beneath the action that
    // started the job. Every message is the seam's own.
    RowLayout {
        Layout.fillWidth: true
        spacing: writeEditor.rowSpacing
        Action {
            id: cancelAction
            objectName: "writeCancel_" + writeEditor.nodeId
            visible: writeEditor.jobActive
            text: "Cancel"
            onClicked: {
                if (writeEditor.job)
                    writeEditor.delivery.cancel(Number(writeEditor.job.id));
            }
        }
        Text {
            objectName: "writeStatus_" + writeEditor.nodeId
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: writeEditor.statusText()
            color: writeEditor.jobActive ? writeEditor.textColor : writeEditor.mutedColor
            font.pixelSize: writeEditor.smallFontSize
            elide: Text.ElideRight
            Accessible.name: text
        }
        Action {
            objectName: "writeClear_" + writeEditor.nodeId
            visible: writeEditor.job !== null && !writeEditor.jobActive
            text: "\u00d7"
            implicitWidth: 28
            Accessible.name: "Clear the delivery result"
            onClicked: {
                if (writeEditor.job)
                    writeEditor.delivery.forget(Number(writeEditor.job.id));
            }
        }
    }

    Text {
        objectName: "writeProblem_" + writeEditor.nodeId
        visible: writeEditor.problemText.length > 0
        Layout.fillWidth: true
        text: writeEditor.problemText
        color: writeEditor.errorColor
        font.pixelSize: writeEditor.smallFontSize
        wrapMode: Text.WordWrap
        Accessible.name: writeEditor.problemText
    }

    Text {
        objectName: "writeFailure_" + writeEditor.nodeId
        visible: writeEditor.failureText().length > 0
        Layout.fillWidth: true
        text: writeEditor.failureText()
        color: writeEditor.errorColor
        font.pixelSize: writeEditor.smallFontSize
        elide: Text.ElideRight
        Accessible.name: writeEditor.failureText()
    }

    Text {
        objectName: "writeEditorError_" + writeEditor.nodeId
        visible: writeEditor.gestureProblem.length > 0
        Layout.fillWidth: true
        text: writeEditor.gestureProblem
        color: writeEditor.errorColor
        font.pixelSize: writeEditor.smallFontSize
        elide: Text.ElideRight
        wrapMode: Text.WordWrap
        Accessible.name: writeEditor.gestureProblem
    }
}
