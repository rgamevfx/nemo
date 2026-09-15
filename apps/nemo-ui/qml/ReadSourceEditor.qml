import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Node-local media control for a Read node (issues #61/#82), hosted by the
// generic inspector through ParameterEditorRegistry id "nemo.read.source".
// It presents the shared media reference the node names — file, validated
// summary, Original Range — and the Read's OWN choices (range mode, custom
// range, Offset/Start At mapping, step, boundary/missing policies, Input
// Transform and alpha). Media selection uses ReadSourceController; value edits
// use the shared panel gesture. One completed gesture is one validated,
// undoable command, and a rejected edit changes nothing.
// This control owns no scanning, no timing/color arithmetic and no persistent
// fact: discovery, the effective mapping and the resolved interpretation come
// from the core/media owners. Numeric editing is the shared NumericField.
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
    // Shared platform drag threshold (ViewerController owns the application
    // value); a bare host falls back to the conventional 4 px.
    readonly property int dragThreshold: (typeof viewerController !== "undefined" && viewerController && viewerController.dragDistance !== undefined) ? viewerController.dragDistance : 4

    // Theme with fallbacks so the editor also renders in a bare test host; the
    // shared numeric control receives a complete map.
    readonly property color borderColor: readEditor.theme ? readEditor.theme.border : "#30343a"
    readonly property color fieldColor: readEditor.theme ? readEditor.theme.field : "#24272c"
    readonly property color accentColor: readEditor.theme ? readEditor.theme.accent : "#3485f6"
    readonly property color textColor: readEditor.theme ? readEditor.theme.text : "#dce0e6"
    readonly property color mutedColor: readEditor.theme ? readEditor.theme.muted : "#979ea8"
    readonly property color errorColor: readEditor.theme ? readEditor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: readEditor.theme ? readEditor.theme.smallRadius : 4
    readonly property int fontSizeValue: readEditor.theme ? readEditor.theme.fontSize : 11
    readonly property int smallFontSize: Math.max(9, readEditor.fontSizeValue - 1)
    readonly property var numericTheme: ({
            "field": readEditor.fieldColor,
            "panel": readEditor.theme && readEditor.theme.panel ? readEditor.theme.panel : "#1d2024",
            "text": readEditor.textColor,
            "muted": readEditor.mutedColor,
            "accent": readEditor.accentColor,
            "border": readEditor.borderColor,
            "errorText": readEditor.errorColor,
            "disabled": readEditor.mutedColor,
            "smallRadius": readEditor.smallRadiusValue,
            "fontSize": readEditor.fontSizeValue
        })

    property var info: ({})
    // The Input Transform list from the ACTIVE project config.
    property var inputSpaces: []
    // ONE effective-value query per refresh: the current-frame parameter rows
    // (value/valueText/choices) every control derives from. The authored view
    // stays in info(); the generic inspector query is the effective-value owner.
    property var paramRows: ({})
    // The single query result this refresh is built from.
    property var paramInspector: ({})
    // Hint choices come from the CURRENT inspector schema (assigned only when
    // they change); empty when no schema is available, in which case the hint
    // rows are not shown and no local vocabulary is invented.
    property var transferChoices: []
    property var primariesChoices: []
    property var matrixChoices: []
    property var hintRangeChoices: []
    property var chromaChoices: []
    // Why the schema-derived hint choices are unavailable, when the inspector
    // query reports it: shown, never silently dropped.
    property string hintsProblem: ""
    readonly property bool hintsAvailable: readEditor.transferChoices.length > 0 || readEditor.primariesChoices.length > 0 || readEditor.matrixChoices.length > 0 || readEditor.hintRangeChoices.length > 0 || readEditor.chromaChoices.length > 0
    // One local rejection message, shown next to the field that produced it.
    property string fieldError: ""
    property bool advancedExpanded: false

    readonly property string state: readEditor.info && readEditor.info.state !== undefined ? String(readEditor.info.state) : "unresolved"
    readonly property bool pending: readEditor.info && readEditor.info.pending === true
    readonly property string problem: readEditor.info && readEditor.info.error !== undefined ? String(readEditor.info.error) : ""
    readonly property string resolvedPath: readEditor.info && readEditor.info.resolvedPath !== undefined ? String(readEditor.info.resolvedPath) : ""
    readonly property bool choiceRequired: readEditor.info && readEditor.info.choiceRequired === true
    readonly property string rangeMode: readEditor.rowValueText("rangeMode").length > 0 ? readEditor.rowValueText("rangeMode") : "auto"
    readonly property bool customRange: readEditor.rangeMode === "custom"
    readonly property bool hasMedia: readEditor.state === "ready" || readEditor.state === "offline"
    // Rejected media must still allow the missing interpretation to be supplied.
    readonly property bool interpretationAvailable: readEditor.hasMedia || readEditor.problem.length > 0
    readonly property string inputMode: readEditor.rowValueText("inputTransform").length > 0 ? readEditor.rowValueText("inputTransform") : "auto"
    readonly property string alphaMode: readEditor.rowValueText("alphaMode").length > 0 ? readEditor.rowValueText("alphaMode") : "auto"

    objectName: "readSource_" + readEditor.nodeId
    // The host loads a registered editor as a full-width SECTION body: no outer
    // label/key column is rendered, so this root owns its own labels and fills
    // the card width.
    Layout.fillWidth: true
    spacing: 3

    function refresh() {
        if (!readEditor.readSource || readEditor.nodeId.length === 0) {
            readEditor.info = ({});
            return;
        }
        readEditor.info = readEditor.readSource.info(readEditor.queryNetwork, readEditor.queryNode, readEditor.frameOf(), readEditor.instanceId);
        readEditor.inputSpaces = readEditor.readSource.inputTransformChoices();
        readEditor.setInputEntries();
        // The effective current-frame rows: one query, reused by every control
        // and by the schema-derived choice lists below.
        readEditor.paramInspector = readEditor.inspectorQuery();
        readEditor.paramRows = readEditor.inspectorRows();
        readEditor.hintsProblem = readEditor.paramInspector && readEditor.paramInspector.available === false ? String(readEditor.paramInspector.reason === undefined ? "" : readEditor.paramInspector.reason) : "";
        readEditor.assignIfChanged("transferChoices", readEditor.rowChoices("sourceTransfer"));
        readEditor.assignIfChanged("primariesChoices", readEditor.rowChoices("sourcePrimaries"));
        readEditor.assignIfChanged("matrixChoices", readEditor.rowChoices("sourceMatrix"));
        readEditor.assignIfChanged("hintRangeChoices", readEditor.rowChoices("sourceRange"));
        readEditor.assignIfChanged("chromaChoices", readEditor.rowChoices("sourceChromaLocation"));
        if (!inputTransformBox.activeFocus)
            inputTransformBox.editText = readEditor.authoredInputEntry();
        if (!alphaBox.activeFocus)
            alphaBox.currentIndex = Math.max(0, alphaBox.model.indexOf(readEditor.alphaMode));
        if (!beforeBox.activeFocus)
            beforeBox.currentIndex = Math.max(0, beforeBox.model.indexOf(readEditor.rowValueText("beforePolicy")));
        if (!afterBox.activeFocus)
            afterBox.currentIndex = Math.max(0, afterBox.model.indexOf(readEditor.rowValueText("afterPolicy")));
        if (!missingBox.activeFocus)
            missingBox.currentIndex = Math.max(0, missingBox.model.indexOf(readEditor.rowValueText("missingPolicy")));
        if (!transferBox.activeFocus)
            transferBox.currentIndex = Math.max(0, transferBox.model.indexOf(readEditor.rowValueText("sourceTransfer")));
        if (!primariesBox.activeFocus)
            primariesBox.currentIndex = Math.max(0, primariesBox.model.indexOf(readEditor.rowValueText("sourcePrimaries")));
        if (!matrixBox.activeFocus)
            matrixBox.currentIndex = Math.max(0, matrixBox.model.indexOf(readEditor.rowValueText("sourceMatrix")));
        if (!hintRangeBox.activeFocus)
            hintRangeBox.currentIndex = Math.max(0, hintRangeBox.model.indexOf(readEditor.rowValueText("sourceRange")));
        if (!chromaBox.activeFocus)
            chromaBox.currentIndex = Math.max(0, chromaBox.model.indexOf(readEditor.rowValueText("sourceChromaLocation")));
    }

    function fieldText(name) {
        return readEditor.info && readEditor.info[name] !== undefined ? String(readEditor.info[name]) : "";
    }

    function numberValue(name, fallback) {
        const text = readEditor.fieldText(name);
        const value = Number(text);
        return text.length > 0 && Number.isFinite(value) ? value : fallback;
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

    // The authored Input Transform as one displayable entry of the control.
    // The Input Transform entries in one stored list; the visible list is
    // assigned only when it really changes (no per-evaluation array allocation,
    // so no delegate-model churn).
    function choiceIsAmbiguous() {
        return readEditor.info && readEditor.info.choiceAmbiguous === true;
    }

    // Modified-from-default for the groups this editor owns, compared against
    // the schema defaults of the EFFECTIVE current-frame values.
    function timingModified() {
        return readEditor.rangeMode !== "auto" || readEditor.controlText("frameOffset") !== "0" || readEditor.controlText("frameStep") !== "1" || readEditor.controlText("beforePolicy") !== "error" || readEditor.controlText("afterPolicy") !== "error" || readEditor.controlText("missingPolicy") !== "error";
    }

    function colorModified() {
        return readEditor.inputMode !== "auto" || readEditor.alphaMode !== "auto" || readEditor.controlText("inputColorSpace").length > 0;
    }

    // The File control is modified once a source key is bound (its default is
    // empty), and it carries the host row's own gesture handle.
    // The File control's own host handle: the row's parameterKey (the exposed
    // token on an occurrence card, `source` on a direct card).
    readonly property string fileKey: readEditor.parameterKey
    function fileModified() {
        return readEditor.info && readEditor.info.sourceKey !== undefined && String(readEditor.info.sourceKey).length > 0;
    }

    function setInputEntries() {
        inputTransformBox.allEntries = ["Auto", "Raw / Data"].concat(readEditor.inputSpaces);
        readEditor.applyInputFilter(inputTransformBox.activeFocus ? readEditor.inputSearchText() : "");
    }

    function inputSearchText() {
        const input = inputTransformBox.contentItem;
        // Qt selects an autocompleted suffix backwards; filter the artist's
        // typed prefix, not the suggested completion.
        return input.selectedText.length > 0 && input.cursorPosition === input.selectionStart && input.selectionEnd === input.text.length ? input.text.substring(0, input.selectionStart) : input.text;
    }

    function filterInput() {
        readEditor.applyInputFilter(readEditor.inputSearchText());
    }

    function applyInputFilter(text) {
        const needle = String(text === undefined ? "" : text).toLowerCase();
        const next = needle.length === 0 ? inputTransformBox.allEntries : inputTransformBox.allEntries.filter(function (entry) {
                return entry.toLowerCase().indexOf(needle) >= 0;
            });
        if (next.length === inputTransformBox.entries.length) {
            let same = true;
            for (let index = 0; index < next.length; ++index) {
                if (next[index] !== inputTransformBox.entries[index]) {
                    same = false;
                    break;
                }
            }
            if (same)
                return;  // identical list: do not rebuild the delegate model
        }
        const input = inputTransformBox.contentItem;
        const editText = input.text;
        const cursor = input.cursorPosition;
        const start = input.selectionStart;
        const end = input.selectionEnd;
        inputTransformBox.entries = next;
        inputTransformBox.editText = editText;
        // A newly available completion owns its new suffix selection.
        if (input.text !== editText)
            return;
        // Rebuilding the ComboBox model also changes its current text. Keep the
        // edit and selection intact so subsequent typing replaces completion.
        if (start === end)
            input.cursorPosition = cursor;
        else if (cursor === start)
            input.select(end, start);
        else
            input.select(start, end);
    }

    function authoredInputEntry() {
        if (readEditor.inputMode === "raw")
            return "Raw / Data";
        if (readEditor.inputMode === "explicit")
            return readEditor.fieldText("inputColorSpace");
        return "Auto";
    }

    // --- shared parameter gesture ------------------------------------------
    // Timing/color parameters are consumed by this editor, so its own rows must
    // provide the same keying affordances as the generic host rows: when ANY
    // channel exists for a parameter, an edit authors the CURRENT-FRAME typed
    // key through the shared gesture (never a static override shadowed by the
    // animation); otherwise the Read command stays the validator/owner of the
    // static value. No private key mode, no second history path.
    // The current document-local frame from the host (0 when no host is
    // attached, e.g. a bare test engine).
    function frameOf() {
        return readEditor.panel && readEditor.panel.controller ? readEditor.panel.controller.frame : 0;
    }

    function parameterForField(field) {
        return field === "startAt" ? "frameOffset" : field;
    }

    function keyAtFrame(key) {
        key = readEditor.parameterForField(key);
        if (readEditor.panel && readEditor.panel.keyParameterAtFrame)
            readEditor.panel.keyParameterAtFrame(readEditor.networkId, readEditor.nodeId, key);
        readEditor.refresh();
    }

    function removeKeyAtFrame(key) {
        key = readEditor.parameterForField(key);
        if (readEditor.panel && readEditor.panel.removeParameterKeyAtFrame)
            readEditor.panel.removeParameterKeyAtFrame(readEditor.networkId, readEditor.nodeId, key);
        readEditor.refresh();
    }

    function revealInAnimation(key) {
        key = readEditor.parameterForField(key);
        if (readEditor.panel && readEditor.panel.revealInAnimation)
            readEditor.panel.revealInAnimation(readEditor.networkId, readEditor.nodeId, key);
    }

    function revealAvailable() {
        return readEditor.panel && readEditor.panel.groupHasAnimationPanel ? readEditor.panel.groupHasAnimationPanel() : false;
    }

    function resetValue(key) {
        key = readEditor.parameterForField(key);
        readEditor.fieldError = "";
        if (readEditor.panel && readEditor.panel.resetValue) {
            if (!readEditor.panel.resetValue({
                    "networkId": readEditor.networkId,
                    "nodeId": readEditor.nodeId,
                    "parameterKey": handle
                }))
                readEditor.fieldError = readEditor.gestureError();
        }
        readEditor.refresh();
    }

    function keyStatusOf(key) {
        key = readEditor.parameterForField(key);
        if (!readEditor.panel || !readEditor.panel.parameterKeyStatusFor)
            return "none";
        return String(readEditor.panel.parameterKeyStatusFor(readEditor.networkId, readEditor.nodeId, key));
    }

    // --- shared parameter gesture (single owner for every value edit) --------
    // All value edits go through the shared batch gesture: each address follows
    // the core rule (an existing channel authors the CURRENT-FRAME key, an
    // unanimated parameter takes the static value), so an animated choice and
    // its static companion change in ONE atomic gesture, one preview and one
    // history entry. This editor never decides keyed-vs-static itself.
    readonly property var paramController: readEditor.panel && readEditor.panel.controller ? readEditor.panel.controller : null

    function gestureError() {
        return readEditor.paramController ? String(readEditor.paramController.error) : "";
    }

    // The PANEL wrappers are the entry point: they own the live token, defer a
    // document/frame refresh while a gesture is live (so nothing rebuilds this
    // editor mid-scrub) and route Escape to cancel the active gesture. They are
    // the same controller owner underneath.
    function gestureBegin(keys) {
        if (!readEditor.panel || !readEditor.panel.beginEditForMany)
            return "";
        return String(readEditor.panel.beginEditForMany(readEditor.networkId, readEditor.nodeId, keys));
    }

    function gestureUpdate(values) {
        if (!readEditor.panel || !readEditor.panel.updateEditMany)
            return false;
        return readEditor.panel.updateEditMany(values) === true;
    }

    function gestureCommit() {
        if (!readEditor.panel || !readEditor.panel.commitEdit)
            return false;
        return readEditor.panel.commitEdit() === true;
    }

    function gestureCancel() {
        if (readEditor.panel && readEditor.panel.cancelEdit)
            readEditor.panel.cancelEdit();
    }

    // One atomic value gesture over the given keys. Values are exact text for
    // integer parameters (the core parser owns the conversion) and the chosen
    // string for choices; a refused or stale gesture changes nothing.
    function commitValues(values) {
        readEditor.fieldError = "";
        // Aggregate value edits exist only on the DIRECT card, where the child
        // keys ARE the host handles (an occurrence exposure renders File/source
        // actions only, and the generic inspector owns its exposed rows).
        const mapped = values;
        const keys = Object.keys(mapped);
        if (keys.length === 0)
            return false;
        const token = readEditor.gestureBegin(keys);
        if (token.length === 0) {
            readEditor.fieldError = readEditor.gestureError();
            return false;
        }
        if (!readEditor.gestureUpdate(mapped)) {
            const message = readEditor.gestureError();
            readEditor.gestureCancel();
            readEditor.fieldError = message;
            readEditor.refresh();
            return false;
        }
        if (!readEditor.gestureCommit()) {
            readEditor.fieldError = readEditor.gestureError();
            readEditor.refresh();
            return false;
        }
        readEditor.refresh();
        return true;
    }

    // --- scrub: the SAME gesture, live ---------------------------------------
    property string scrubKey: ""
    property string scrubToken: ""
    property int scrubFrame: 0

    function scrubBegin(key) {
        readEditor.fieldError = "";
        readEditor.scrubCancel();
        readEditor.scrubKey = key;
        readEditor.scrubFrame = readEditor.frameOf();
        readEditor.scrubToken = readEditor.gestureBegin([readEditor.parameterForField(key)]);
        if (readEditor.scrubToken.length === 0)
            readEditor.fieldError = readEditor.gestureError();
    }

    function scrubUpdate(key, valueText) {
        if (readEditor.scrubToken.length === 0 || key !== readEditor.scrubKey)
            return;
        const values = ({});
        const text = key === "startAt" ? readEditor.startAtOffsetText(valueText, readEditor.scrubFrame) : valueText;
        if (text.length === 0)
            return;
        values[readEditor.parameterForField(key)] = text;
        if (!readEditor.gestureUpdate(values))
            readEditor.fieldError = readEditor.gestureError();  // the preview stays valid either way
    }

    function scrubFinish(key) {
        if (readEditor.scrubToken.length === 0 || key !== readEditor.scrubKey)
            return;
        readEditor.scrubToken = "";
        readEditor.scrubKey = "";
        if (!readEditor.gestureCommit())
            readEditor.fieldError = readEditor.gestureError();
        readEditor.refresh();
    }

    // Escape / lost target / a superseded gesture cancels the live gesture:
    // nothing is published.
    function scrubCancel() {
        if (readEditor.scrubToken.length === 0)
            return;
        readEditor.scrubToken = "";
        readEditor.scrubKey = "";
        readEditor.gestureCancel();
        readEditor.refresh();
    }

    // Exact text for an integer parameter: the core parser converts it, so a
    // 64-bit frame number is never rounded through a JavaScript double.
    function exactText(value) {
        return String(value);
    }

    // The current inspector query, once per refresh.
    function inspectorQuery() {
        if (!readEditor.paramController || !readEditor.paramController.parameterInspector)
            return ({});
        return readEditor.paramController.parameterInspector(readEditor.networkId, readEditor.nodeId);
    }

    // key -> { value, valueText, choices } from the query's real section rows.
    // A consumed key is excluded from the rendered card but is still part of the
    // inspector data, so a control reads the EFFECTIVE (current-frame) value.
    function inspectorRows() {
        const rows = ({});
        const inspector = readEditor.paramInspector;
        const sections = inspector && inspector.sections ? inspector.sections : [];
        for (let index = 0; index < sections.length; ++index) {
            const parameters = sections[index].parameters || [];
            for (let row = 0; row < parameters.length; ++row) {
                const entry = parameters[row];
                rows[String(entry.key)] = entry;
            }
        }
        return rows;
    }

    // The value a control shows: the effective current-frame row when the query
    // exposes that key, otherwise the authored value from info(). A missing row
    // (an occurrence card exposes only its definition's `exposed:` controls) is
    // therefore never turned into a fabricated default.
    function controlText(key) {
        const row = readEditor.paramRows ? readEditor.paramRows[key] : null;
        if (row) {
            if (row.valueText !== undefined && String(row.valueText).length > 0)
                return String(row.valueText);
            if (row.value !== undefined)
                return String(row.value);
        }
        return readEditor.fieldText(key);
    }

    // QUERY coordinates: the real node that owns the parameters, published by the
    // host row as targetNetwork/targetNode (a definition network for an
    // occurrence exposure, the inspected pair for a definition-hosted Read).
    // GESTURE coordinates stay the host row (networkId/nodeId + the row's key),
    // which is what keeps an edit inside its own scope.
    readonly property string queryNetwork: readEditor.parameter && readEditor.parameter.targetNetwork !== undefined && String(readEditor.parameter.targetNetwork).length > 0 ? String(readEditor.parameter.targetNetwork) : readEditor.networkId
    readonly property string queryNode: readEditor.parameter && readEditor.parameter.targetNode !== undefined && String(readEditor.parameter.targetNode).length > 0 ? String(readEditor.parameter.targetNode) : readEditor.nodeId

    // True when this editor is hosted for a Network occurrence.
    readonly property bool occurrenceScope: readEditor.instanceId.length > 0 && readEditor.instanceId !== "0"

    function rowValueText(key) {
        return readEditor.controlText(key);
    }

    function rowNumber(key, fallback) {
        const value = Number(readEditor.rowValueText(key));
        return Number.isFinite(value) ? value : fallback;
    }

    function rowChoices(key) {
        const row = readEditor.paramRows ? readEditor.paramRows[key] : null;
        return row && row.choices ? row.choices : [];
    }

    function assignIfChanged(name, next) {
        const current = readEditor[name];
        if (current && current.length === next.length) {
            let same = true;
            for (let index = 0; index < next.length; ++index) {
                if (current[index] !== next[index]) {
                    same = false;
                    break;
                }
            }
            if (same)
                return;  // identical list: do not rebuild a delegate model
        }
        readEditor[name] = next;
    }

    function commitPath(text) {
        if (!readEditor.readSource)
            return;
        readEditor.readSource.setSourcePath(readEditor.queryNetwork, readEditor.queryNode, text, readEditor.instanceId, readEditor.frameOf());
        readEditor.refresh();
    }

    // --- value edits (each an atomic shared gesture) -------------------------

    // Range endpoints are ONE edit: the mode and both endpoints travel in a
    // single gesture (an animated endpoint authors its current-frame key).
    function commitRange() {
        readEditor.commitValues({
                "rangeMode": "custom",
                "rangeFirst": firstField.exactText(),
                "rangeLast": lastField.exactText()
            });
    }

    function commitOffset(value) {
        readEditor.commitValues({
                "frameOffset": readEditor.exactText(value)
            });
    }

    function commitStep(value) {
        readEditor.commitValues({
                "frameStep": readEditor.exactText(value)
            });
    }

    // Both typed and scrubbed Start At edit the one underlying Offset through
    // core's checked mapping; presentation never derives timing arithmetic.
    function startAtOffsetText(value, frame) {
        if (!readEditor.readSource)
            return "";
        const offset = readEditor.readSource.startAtOffsetValue(readEditor.queryNetwork, readEditor.queryNode, readEditor.exactText(value), frame, readEditor.instanceId);
        if (offset.length === 0)
            readEditor.fieldError = "Start At could not be resolved for the current range.";
        return offset;
    }

    function commitStartAt(value) {
        const offset = readEditor.startAtOffsetText(value, readEditor.frameOf());
        if (offset.length > 0)
            readEditor.commitValues({
                    "frameOffset": offset
                });
    }

    function commitRangeMode(mode) {
        readEditor.commitValues({
                "rangeMode": mode
            });
    }

    function commitPolicies() {
        readEditor.commitValues({
                "beforePolicy": beforeBox.currentText,
                "afterPolicy": afterBox.currentText,
                "missingPolicy": missingBox.currentText
            });
    }

    function commitInputTransform(text) {
        readEditor.fieldError = "";
        const entry = text.trim();
        let mode = "";
        let space = "";
        if (entry === "Auto") {
            mode = "auto";
        } else if (entry === "Raw / Data") {
            mode = "raw";
        } else if (readEditor.inputSpaces.indexOf(entry) >= 0) {
            mode = "explicit";
            space = entry;
        } else {
            readEditor.fieldError = "Unknown input transform '" + entry + "' for the project config.";
            inputTransformBox.editText = readEditor.authoredInputEntry();
            return;
        }
        // The mode and the color space are ONE edit (atomic), whether either of
        // them is animated.
        readEditor.commitValues({
                "inputTransform": mode,
                "inputColorSpace": space
            });
    }

    function commitAlpha(mode) {
        readEditor.commitValues({
                "alphaMode": mode
            });
    }

    // One schema-declared hint key, edited through the shared single-key gesture.
    function commitHint(key, value) {
        const values = ({});
        values[key] = String(value);
        readEditor.commitValues(values);
    }

    // Dispatch one committed numeric value to the gesture that owns it.
    function commitFor(field, text) {
        if (field === "frameOffset")
            readEditor.commitOffset(text);
        else if (field === "frameStep")
            readEditor.commitStep(text);
        else if (field === "startAt")
            readEditor.commitStartAt(text);
        else
            readEditor.commitRangeFrom(field, text);
    }

    function commitRangeFrom(field, text) {
        const first = field === "rangeFirst" ? text : firstField.exactText();
        const last = field === "rangeLast" ? text : lastField.exactText();
        readEditor.commitValues({
                "rangeMode": "custom",
                "rangeFirst": first,
                "rangeLast": last
            });
    }

    component FrameLabel: Text {
        color: readEditor.mutedColor
        font.pixelSize: readEditor.smallFontSize
    }

    // Exposure drag for one consumed numeric parameter: the same MIME gesture
    // the generic rows use, so exposure/exposed-key routing is unchanged.
    component TimingExposure: ExposureLabel {
        required property string keyName
        Layout.preferredWidth: 72
        Layout.minimumWidth: 72
        Layout.maximumWidth: 72
        Layout.alignment: Qt.AlignVCenter
        theme: readEditor.numericTheme
        networkId: readEditor.networkId
        instanceId: readEditor.instanceId
        nodeId: readEditor.nodeId
        parameterKey: keyName
        labelText: readEditor.paramRows[keyName] ? readEditor.paramRows[keyName].label : keyName
        keyStatus: readEditor.keyStatusOf(keyName)
        frame: readEditor.frameOf()
        onKeyRequested: readEditor.keyAtFrame(keyName)
    }

    // Aligned key column for one consumed numeric parameter: current-frame
    // key state, Set/Update Key, Remove Key and Show in Animation.
    component TimingKey: KeyIndicator {
        required property string keyName
        objectName: "readSourceKey_" + keyName + "_" + readEditor.nodeId
        Layout.alignment: Qt.AlignVCenter
        theme: readEditor.numericTheme
        networkId: readEditor.networkId
        nodeId: readEditor.nodeId
        parameterKey: keyName
        parameterLabel: readEditor.paramRows[keyName] ? readEditor.paramRows[keyName].label : keyName
        keyStatus: readEditor.keyStatusOf(keyName)
        scope: "Value"
        frame: readEditor.frameOf()
        revealAvailable: readEditor.revealAvailable()
        onKeyRequested: readEditor.keyAtFrame(keyName)
        onRemoveKeyRequested: readEditor.removeKeyAtFrame(keyName)
        onRevealRequested: readEditor.revealInAnimation(keyName)
    }

    // One whole-number field on the shared numeric editor: click to type, drag
    // to scrub, Shift fine / Ctrl coarse, Enter commits, Escape cancels.
    component ReadIntegerField: NumericField {
        theme: readEditor.numericTheme
        integer: true
        decimals: 0
        step: 1
        dragThreshold: readEditor.dragThreshold
        fieldWidth: 60
        property string fieldName: ""
        // The authored value as EXACT text (never through a double), so a large
        // frame number is submitted to the core parser unchanged.
        function exactText() {
            return readEditor.rowValueText(fieldName);
        }
        // Scrub drives the SAME shared gesture: begin on press, one preview
        // update per move (the panel previews it), commit on release, cancel on
        // Escape or a lost target.
        onScrubStarted: readEditor.scrubBegin(fieldName)
        onScrubbed: function (candidate) {
            readEditor.scrubUpdate(fieldName, readEditor.exactText(candidate));
        }
        onScrubFinished: readEditor.scrubFinish(fieldName)
        onScrubCancelled: readEditor.scrubCancel()
        // A cancelled gesture (Escape or a preview-only Undo) returns the field
        // to the authored value at once instead of holding the cancelled
        // preview until release.
        gestureLive: readEditor.panel ? readEditor.panel.activeToken.length > 0 : false
        onTextCommitted: function (committed) {
            readEditor.commitFor(fieldName, String(committed));
        }
        onStepped: function (committed) {
            readEditor.commitFor(fieldName, readEditor.exactText(committed));
        }
        onTextRejected: readEditor.fieldError = "Enter a whole number."
        onKeyRequested: readEditor.keyAtFrame(fieldName)

        // Key/reset affordances for a consumed parameter, reachable without the
        // generic row: the same gesture the aligned key column exposes.
        Menu {
            id: fieldMenu
            MenuItem {
                text: readEditor.keyStatusOf(fieldName) === "key" ? "Update Key" : "Set Key"
                enabled: readEditor.panel !== undefined && readEditor.panel !== null
                onTriggered: readEditor.keyAtFrame(fieldName)
            }
            MenuItem {
                text: "Remove Key"
                enabled: readEditor.keyStatusOf(fieldName) === "key"
                onTriggered: readEditor.removeKeyAtFrame(fieldName)
            }
            MenuItem {
                text: "Show in Animation"
                enabled: readEditor.revealAvailable()
                onTriggered: readEditor.revealInAnimation(fieldName)
            }
            MenuSeparator {
            }
            MenuItem {
                text: "Reset Value"
                onTriggered: readEditor.resetValue(fieldName)
            }
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: function (mouse) {
                fieldMenu.popup();
                mouse.accepted = true;
            }
        }
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
        spacing: 6

        Rectangle {
            objectName: "readSourceFileModified_" + readEditor.nodeId
            Layout.preferredWidth: 6
            Layout.maximumWidth: 6
            Layout.preferredHeight: 6
            Layout.alignment: Qt.AlignVCenter
            radius: 3
            color: readEditor.fileModified() ? readEditor.accentColor : "transparent"
            Accessible.name: readEditor.fileModified() ? "File is modified from its default" : "File is at its default"
        }

        ExposureLabel {
            Layout.preferredWidth: 72
            Layout.minimumWidth: 44
            Layout.maximumWidth: 72
            Layout.alignment: Qt.AlignVCenter
            theme: readEditor.numericTheme
            networkId: readEditor.networkId
            instanceId: readEditor.instanceId
            nodeId: readEditor.nodeId
            parameterKey: readEditor.fileKey
            labelText: "File"
            keyStatus: readEditor.keyStatusOf(readEditor.fileKey)
            frame: readEditor.frameOf()
            onKeyRequested: readEditor.keyAtFrame(readEditor.fileKey)
        }

        KeyIndicator {
            objectName: "readSourceFileKey_" + readEditor.nodeId
            Layout.preferredWidth: 24
            Layout.maximumWidth: 24
            Layout.alignment: Qt.AlignVCenter
            theme: readEditor.numericTheme
            networkId: readEditor.networkId
            nodeId: readEditor.nodeId
            parameterKey: readEditor.fileKey
            parameterLabel: "File"
            keyStatus: readEditor.keyStatusOf(readEditor.fileKey)
            scope: "Value"
            frame: readEditor.frameOf()
            revealAvailable: readEditor.revealAvailable()
            onKeyRequested: readEditor.keyAtFrame(readEditor.fileKey)
            onRemoveKeyRequested: readEditor.removeKeyAtFrame(readEditor.fileKey)
            onRevealRequested: readEditor.revealInAnimation(readEditor.fileKey)
        }

        TextField {
            id: pathField
            property int revision: readEditor.panel ? readEditor.panel.revision : 0
            property bool edited: false
            objectName: "readSourcePath_" + readEditor.nodeId
            Layout.fillWidth: true
            implicitHeight: 23
            font.pixelSize: readEditor.fontSizeValue
            color: readEditor.textColor
            placeholderText: "File or sequence pattern (#, @)"
            selectByMouse: true
            text: readEditor.fieldText("path")
            onTextEdited: edited = true
            onEditingFinished: {
                // Enter and later focus departure both emit editingFinished.
                // Only the first ends this edit; a second probe would discard
                // the pending Sequence/Single choice before its click lands.
                if (!edited)
                    return;
                edited = false;
                readEditor.commitPath(text);
            }
            Keys.onEscapePressed: function (event) {
                event.accepted = true;
                pathField.edited = false;
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
                readEditor.readSource.chooseSource(readEditor.queryNetwork, readEditor.queryNode, readEditor.instanceId, readEditor.frameOf());
                readEditor.refresh();
            }
        }
    }

    // Validated summary near the file control: kind, dimensions, pixel aspect,
    // rate, span, precision, channels and coverage quality. Guessed values are
    // never presented as probed facts.
    Text {
        objectName: "readSourceSummary_" + readEditor.nodeId
        Layout.fillWidth: true
        visible: readEditor.hasMedia
        text: {
            if (!readEditor.hasMedia)
                return "";
            const kind = readEditor.fieldText("kind");
            const width = readEditor.info.width !== undefined ? Number(readEditor.info.width) : 0;
            const height = readEditor.info.height !== undefined ? Number(readEditor.info.height) : 0;
            const aspect = readEditor.info.pixelAspect !== undefined ? Number(readEditor.info.pixelAspect) : 0;
            let text = kind.length > 0 ? kind : "media";
            if (width > 0 && height > 0)
                text += "  " + width + "\u00d7" + height;
            if (aspect > 0 && Math.abs(aspect - 1.0) > 1e-6)
                text += "  par " + aspect.toFixed(3);
            const rate = readEditor.fieldText("rate");
            if (rate.length > 0)
                text += "  " + rate + " fps";
            const span = readEditor.fieldText("frameSpan");
            if (span.length > 0)
                text += "  span " + span;
            const precision = readEditor.fieldText("precision");
            if (precision.length > 0)
                text += "  " + precision;
            const channels = readEditor.fieldText("channels");
            if (channels.length > 0)
                text += "  " + channels;
            const quality = readEditor.fieldText("coverageQuality");
            if (quality.length > 0 && quality !== "unknown")
                text += "  coverage " + quality;
            if (readEditor.info.shared === true)
                text += "  (shared)";
            return text;
        }
        color: readEditor.mutedColor
        font.pixelSize: readEditor.smallFontSize
        wrapMode: Text.WordWrap
    }

    // The explicit numbered Sequence vs Single Image choice (story 45): a
    // numbered selection whose run matches several files is never silently
    // reinterpreted.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        visible: readEditor.choiceRequired

        // An ambiguous numbering names its candidates and asks for an explicit
        // pattern: the Sequence button is only offered when one run is certain.
        readonly property bool ambiguous: readEditor.info && readEditor.info.choiceAmbiguous === true

        Text {
            objectName: "readSourceChoice_" + readEditor.nodeId
            Layout.fillWidth: true
            text: {
                if (readEditor.choiceIsAmbiguous())
                    return readEditor.fieldText("choiceDetail").length > 0 ? readEditor.fieldText("choiceDetail") : "This file name has more than one numbered run. Type an explicit '#' or '@' pattern to " + "load a sequence, or choose Single Image.";
                return "Numbered sequence: frames " + readEditor.fieldText("choiceFirst") + "\u2013" + readEditor.fieldText("choiceLast") + (Number(readEditor.fieldText("missingCount")) > 0 ? " (" + readEditor.fieldText("missingCount") + " missing)" : "") + ". Load as a sequence or as a single image?";
            }
            color: readEditor.choiceIsAmbiguous() ? readEditor.errorColor : readEditor.textColor
            font.pixelSize: readEditor.smallFontSize
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            ChromeButton {
                objectName: "readSourceChoiceSequence_" + readEditor.nodeId
                theme: readEditor.theme
                text: "Sequence"
                implicitHeight: 21
                enabled: readEditor.readSource !== null && !readEditor.choiceIsAmbiguous()
                onClicked: {
                    readEditor.readSource.confirmSourceChoice(readEditor.queryNetwork, readEditor.queryNode, true, readEditor.instanceId);
                    readEditor.refresh();
                }
            }

            ChromeButton {
                objectName: "readSourceChoiceSingle_" + readEditor.nodeId
                theme: readEditor.theme
                text: "Single Image"
                implicitHeight: 21
                enabled: readEditor.readSource !== null
                onClicked: {
                    readEditor.readSource.confirmSourceChoice(readEditor.queryNetwork, readEditor.queryNode, false, readEditor.instanceId);
                    readEditor.refresh();
                }
            }

            Item {
                Layout.fillWidth: true
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
                readEditor.readSource.chooseRelinkSource(readEditor.queryNetwork, readEditor.queryNode, readEditor.instanceId, readEditor.frameOf());
                readEditor.refresh();
            }
        }
    }

    // ---------------------------------------------------------------------
    // Timing
    // ---------------------------------------------------------------------
    FrameLabel {
        objectName: "readSourceTimingGroup_" + readEditor.nodeId
        Layout.fillWidth: true
        visible: readEditor.hasMedia && !readEditor.occurrenceScope
        text: readEditor.timingModified() ? "Timing \u2022" : "Timing"
        color: readEditor.timingModified() ? readEditor.accentColor : readEditor.textColor
        Accessible.name: readEditor.timingModified() ? "Timing, modified from default" : "Timing"
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.hasMedia && !readEditor.occurrenceScope

        FrameLabel {
            text: "Range"
        }

        ChromeButton {
            objectName: "readSourceRangeAuto_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Auto"
            implicitHeight: 21
            enabled: readEditor.customRange
            onClicked: readEditor.commitRangeMode("auto")
        }

        ChromeButton {
            objectName: "readSourceRangeCustom_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Custom"
            implicitHeight: 21
            enabled: !readEditor.customRange
            onClicked: readEditor.commitRangeMode("custom")
        }

        ChromeButton {
            objectName: "readSourceResetToSource_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Reset to Source"
            implicitHeight: 21
            enabled: readEditor.customRange
            onClicked: readEditor.commitRangeMode("auto")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.hasMedia

        FrameLabel {
            text: "Original"
        }
        Text {
            objectName: "readSourceOriginal_" + readEditor.nodeId
            Layout.fillWidth: true
            color: readEditor.textColor
            font.pixelSize: readEditor.smallFontSize
            elide: Text.ElideRight
            text: {
                const kind = readEditor.fieldText("kind");
                const first = readEditor.fieldText("originalFirst");
                const last = readEditor.fieldText("originalLast");
                // A still has one image and no interval.
                if (kind === "image" && first.length === 0)
                    return "still (one image)";
                if (first.length === 0 || last.length === 0)
                    return "coverage unknown";
                if (first === last)
                    return "single frame " + first;
                const count = readEditor.fieldText("originalCount");
                const missing = readEditor.fieldText("missingCount");
                let text = first + " \u2013 " + last;
                if (count.length > 0)
                    text += "  (" + count + " frames";
                if (Number(missing) > 0)
                    text += count.length > 0 ? ", " + missing + " missing)" : "(" + missing + " missing)";
                else if (count.length > 0)
                    text += ")";
                return text;
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.hasMedia && readEditor.customRange && !readEditor.occurrenceScope

        TimingExposure {
            keyName: "rangeFirst"
        }
        TimingKey {
            keyName: "rangeFirst"
        }
        ReadIntegerField {
            id: firstField
            objectName: "readSourceFirst_" + readEditor.nodeId
            fieldName: "rangeFirst"
            label: "First frame"
            value: readEditor.rowNumber("rangeFirst", 0)
            text: readEditor.rowValueText("rangeFirst")
        }

        TimingExposure {
            keyName: "rangeLast"
        }
        TimingKey {
            keyName: "rangeLast"
        }
        ReadIntegerField {
            id: lastField
            objectName: "readSourceLast_" + readEditor.nodeId
            fieldName: "rangeLast"
            label: "Last frame"
            value: readEditor.rowNumber("rangeLast", 0)
            text: readEditor.rowValueText("rangeLast")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.hasMedia && !readEditor.occurrenceScope

        TimingExposure {
            keyName: "frameOffset"
        }
        TimingKey {
            keyName: "frameOffset"
        }
        ReadIntegerField {
            id: offsetField
            objectName: "readSourceOffset_" + readEditor.nodeId
            fieldName: "frameOffset"
            label: "Frame offset"
            value: readEditor.rowNumber("frameOffset", 0)
            text: readEditor.rowValueText("frameOffset")
        }

        TimingExposure {
            keyName: "frameOffset"
            labelText: "Start At"
        }
        TimingKey {
            keyName: "frameOffset"
        }
        ReadIntegerField {
            id: startAtField
            objectName: "readSourceStartAt_" + readEditor.nodeId
            fieldName: "startAt"
            label: "Start At (local frame of the selected forward or reverse boundary)"
            value: readEditor.rowNumber("startAt", 0)
            text: readEditor.rowValueText("startAt")
            valueAvailable: text.length > 0
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.interpretationAvailable && !readEditor.occurrenceScope

        ChromeButton {
            objectName: "readSourceAdvanced_" + readEditor.nodeId
            theme: readEditor.theme
            text: readEditor.advancedExpanded ? "Advanced \u25be" : "Advanced \u25b8"
            implicitHeight: 21
            onClicked: readEditor.advancedExpanded = !readEditor.advancedExpanded
        }

        Item {
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: readEditor.interpretationAvailable && readEditor.advancedExpanded && !readEditor.occurrenceScope

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            TimingExposure {
                keyName: "frameStep"
            }
            TimingKey {
                keyName: "frameStep"
            }
            ReadIntegerField {
                id: stepField
                objectName: "readSourceStep_" + readEditor.nodeId
                fieldName: "frameStep"
                label: "Frame step (signed, nonzero)"
                value: readEditor.rowNumber("frameStep", 1)
                text: readEditor.rowValueText("frameStep")
            }
            Item {
                Layout.fillWidth: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            FrameLabel {
                text: "Before"
            }
            StudioComboBox {
                id: beforeBox
                theme: readEditor.theme
                objectName: "readSourceBeforePolicy_" + readEditor.nodeId
                Accessible.name: "Before-range policy"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: ["error", "hold", "black"]
                onActivated: readEditor.commitPolicies()
            }

            FrameLabel {
                text: "After"
            }
            StudioComboBox {
                id: afterBox
                theme: readEditor.theme
                objectName: "readSourceAfterPolicy_" + readEditor.nodeId
                Accessible.name: "After-range policy"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: ["error", "hold", "black"]
                onActivated: readEditor.commitPolicies()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            FrameLabel {
                text: "Missing"
            }
            StudioComboBox {
                id: missingBox
                theme: readEditor.theme
                objectName: "readSourceMissingPolicy_" + readEditor.nodeId
                Accessible.name: "Missing-frame policy"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: ["error", "black"]
                onActivated: readEditor.commitPolicies()
            }
            Item {
                Layout.fillWidth: true
            }
        }

        // Media-interpretation hints (advanced, collapsed with the rest of this
        // group): fill-only choices the media owner honors only where the file
        // declares nothing, so a tagged source still wins. Shown here rather
        // than as a default-visible section.
        FrameLabel {
            objectName: "readSourceHintsLabel_" + readEditor.nodeId
            Layout.fillWidth: true
            visible: readEditor.hintsAvailable
            text: "Interpretation hints"
        }

        // The schema query failed: report the real reason rather than silently
        // dropping controls the artist is entitled to see.
        Text {
            objectName: "readSourceHintsProblem_" + readEditor.nodeId
            Layout.fillWidth: true
            visible: !readEditor.hintsAvailable && readEditor.hintsProblem.length > 0
            text: "Interpretation hints unavailable: " + readEditor.hintsProblem
            color: readEditor.errorColor
            font.pixelSize: readEditor.smallFontSize
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: readEditor.hintsAvailable

            FrameLabel {
                text: "Transfer"
            }
            StudioComboBox {
                id: transferBox
                theme: readEditor.theme
                objectName: "readSourceHintTransfer_" + readEditor.nodeId
                Accessible.name: "Source transfer hint"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: readEditor.transferChoices
                onActivated: readEditor.commitHint("sourceTransfer", currentText)
            }

            FrameLabel {
                text: "Primaries"
            }
            StudioComboBox {
                id: primariesBox
                theme: readEditor.theme
                objectName: "readSourceHintPrimaries_" + readEditor.nodeId
                Accessible.name: "Source primaries hint"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: readEditor.primariesChoices
                onActivated: readEditor.commitHint("sourcePrimaries", currentText)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: readEditor.hintsAvailable

            FrameLabel {
                text: "Matrix"
            }
            StudioComboBox {
                id: matrixBox
                theme: readEditor.theme
                objectName: "readSourceHintMatrix_" + readEditor.nodeId
                Accessible.name: "Source matrix hint"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: readEditor.matrixChoices
                onActivated: readEditor.commitHint("sourceMatrix", currentText)
            }

            FrameLabel {
                text: "Range"
            }
            StudioComboBox {
                id: hintRangeBox
                theme: readEditor.theme
                objectName: "readSourceHintRange_" + readEditor.nodeId
                Accessible.name: "Source range hint"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: readEditor.hintRangeChoices
                onActivated: readEditor.commitHint("sourceRange", currentText)
            }

            FrameLabel {
                text: "Chroma"
            }
            StudioComboBox {
                id: chromaBox
                theme: readEditor.theme
                objectName: "readSourceHintChroma_" + readEditor.nodeId
                Accessible.name: "Source chroma-location hint"
                Layout.fillWidth: true
                implicitHeight: 23
                font.pixelSize: readEditor.fontSizeValue
                model: readEditor.chromaChoices
                onActivated: readEditor.commitHint("sourceChromaLocation", currentText)
            }
        }
    }

    // ---------------------------------------------------------------------
    // Color
    // ---------------------------------------------------------------------
    FrameLabel {
        objectName: "readSourceColorGroup_" + readEditor.nodeId
        Layout.fillWidth: true
        visible: readEditor.interpretationAvailable && !readEditor.occurrenceScope
        text: readEditor.colorModified() ? "Color \u2022" : "Color"
        color: readEditor.colorModified() ? readEditor.accentColor : readEditor.textColor
        Accessible.name: readEditor.colorModified() ? "Color, modified from default" : "Color"
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.interpretationAvailable && !readEditor.occurrenceScope

        FrameLabel {
            text: "Input"
        }

        StudioComboBox {
            id: inputTransformBox
            theme: readEditor.theme
            objectName: "readSourceInputTransform_" + readEditor.nodeId
            Accessible.name: "Input Transform"
            Layout.fillWidth: true
            implicitHeight: 23
            font.pixelSize: readEditor.fontSizeValue
            editable: true
            contentItem: TextInput {
                Accessible.name: "Input Transform search"
                text: inputTransformBox.editText
                font: inputTransformBox.font
                color: readEditor.textColor
                selectionColor: readEditor.accentColor
                selectByMouse: true
                clip: true
                verticalAlignment: TextInput.AlignVCenter
            }
            // Searchable over the project config's own color spaces; "Auto" and
            // "Raw / Data" are the two non-config interpretations.
            // The entry list is STORED and assigned (never a dependency-driven
            // array binding): a ComboBox rebuilds its delegate model whenever
            // `model` becomes a new array, so a binding that allocates on every
            // evaluation would churn delegate models for no reason. The filter
            // updates it only when the search text actually changes.
            property var entries: ["Auto", "Raw / Data"]
            property var allEntries: ["Auto", "Raw / Data"]
            model: inputTransformBox.entries
            Connections {
                target: inputTransformBox.contentItem
                function onTextEdited() {
                    // Qt finishes autocomplete and caret updates after this
                    // signal. Rebuild delegates only once that edit is complete.
                    Qt.callLater(readEditor.filterInput);
                }
            }
            Connections {
                target: inputTransformBox.popup
                function onAboutToShow() {
                    readEditor.applyInputFilter("");
                }
                function onOpened() {
                    inputTransformBox.contentItem.forceActiveFocus(Qt.PopupFocusReason);
                    inputTransformBox.selectAll();
                }
                function onClosed() {
                    readEditor.applyInputFilter("");
                    const entry = readEditor.authoredInputEntry();
                    inputTransformBox.currentIndex = inputTransformBox.allEntries.indexOf(entry);
                    inputTransformBox.editText = entry;
                }
            }
            onActivated: readEditor.commitInputTransform(currentText)
            onAccepted: readEditor.commitInputTransform(editText)
        }
    }

    Text {
        objectName: "readSourceResolution_" + readEditor.nodeId
        Layout.fillWidth: true
        visible: readEditor.hasMedia && !readEditor.occurrenceScope
        color: readEditor.mutedColor
        font.pixelSize: readEditor.smallFontSize
        wrapMode: Text.WordWrap
        text: {
            if (!readEditor.hasMedia)
                return "";
            const resolved = readEditor.fieldText("resolvedInputColorSpace");
            const origin = readEditor.fieldText("inputTransformOrigin");
            let text = "";
            if (readEditor.inputMode === "auto")
                text = resolved.length > 0 ? "Auto \u2192 " + resolved : "Auto (resolved from media)";
            else if (readEditor.inputMode === "raw")
                text = "Raw / Data \u2014 no color conversion";
            else
                text = readEditor.rowValueText("inputColorSpace");
            if (origin.length > 0)
                text += "  \u00b7 " + origin;
            const working = readEditor.fieldText("workingSpace");
            if (working.length > 0)
                text += "  \u00b7 working " + working;
            return text;
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.interpretationAvailable && !readEditor.occurrenceScope

        FrameLabel {
            text: "Alpha"
        }
        StudioComboBox {
            id: alphaBox
            theme: readEditor.theme
            objectName: "readSourceAlpha_" + readEditor.nodeId
            Accessible.name: "Alpha interpretation"
            Layout.fillWidth: true
            implicitHeight: 23
            font.pixelSize: readEditor.fontSizeValue
            model: ["auto", "straight", "premultiplied"]
            onActivated: readEditor.commitAlpha(currentText)
        }
        Item {
            Layout.fillWidth: true
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: readEditor.hasMedia

        ChromeButton {
            objectName: "readSourceReload_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Reload"
            implicitHeight: 21
            enabled: !readEditor.pending
            onClicked: {
                readEditor.readSource.reloadSource(readEditor.queryNetwork, readEditor.queryNode, readEditor.instanceId, readEditor.frameOf());
                readEditor.refresh();
            }
        }

        Text {
            Layout.fillWidth: true
            text: readEditor.info && readEditor.info.shared === true ? "Reload affects every Read sharing this media." : "Reload re-reads this media."
            color: readEditor.mutedColor
            font.pixelSize: readEditor.smallFontSize
            elide: Text.ElideRight
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        Text {
            Layout.fillWidth: true
            visible: readEditor.fieldError.length > 0
            text: readEditor.fieldError
            color: readEditor.errorColor
            font.pixelSize: readEditor.smallFontSize
            wrapMode: Text.WordWrap
        }

        ChromeButton {
            objectName: "readSourceClear_" + readEditor.nodeId
            theme: readEditor.theme
            text: "Clear"
            implicitHeight: 21
            enabled: readEditor.readSource !== null && readEditor.state !== "empty"
            onClicked: {
                // Clearing is an ordinary value edit of the File control, so it
                // goes through the shared gesture (keyed when the binding is
                // animated) like every other value change.
                const cleared = ({});
                cleared[readEditor.fileKey] = "";
                readEditor.commitValues(cleared);
            }
        }
    }
}
