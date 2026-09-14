import QtQuick
import QtQuick.Controls

// One numeric editor for every built-in node (stories 1-14). It owns only the
// gesture and the text buffer; the host owns the one parameter gesture
// (begin/update/commit/cancel) and the catalog remains the only validation
// authority. Typed values are never clamped to the soft slider travel or
// rounded to the display precision: the soft range is interaction only.
FocusScope {
    id: root

    property var theme: null
    property real value: 0
    property bool valueAvailable: true
    // Optional exact authored text. When present it is shown verbatim and is
    // the source for a typed edit, so an exact 64-bit integer never passes
    // through a JavaScript double (Read offset/step/range, Integer params).
    property string text: ""
    property bool hasMinimum: false
    property bool hasMaximum: false
    property real minimum: 0
    property real maximum: 0
    // Soft adjustment travel for scrub/slider. Never a legal-value bound.
    property bool hasSoftMinimum: false
    property bool hasSoftMaximum: false
    property real softMinimum: 0
    property real softMaximum: 0
    property real step: 0.01
    // -1 exposes enough significant digits for the stored value.
    property int decimals: -1
    property bool integer: false
    property string label: ""
    property string errorText: ""
    // Platform drag threshold; the panel passes the shared application value.
    property int dragThreshold: 4
    property real fieldWidth: 62

    // Discrete commit of an exactly typed value, carried as text so the
    // catalog parses it without a lossy double round-trip.
    signal textCommitted(string text)
    // Typed text that is not a number; the previous value is kept.
    signal textRejected(string text)
    // One arrow-key increment, committed as one discrete gesture.
    signal stepped(real value)
    // Alt-click keeps the supported keying shortcut on the numeric control.
    signal keyRequested
    // Continuous scrub: begin, preview, then exactly one commit or cancel.
    signal scrubStarted
    signal scrubbed(real value)
    signal scrubFinished
    signal scrubCancelled

    activeFocusOnTab: enabled
    implicitWidth: fieldWidth
    implicitHeight: 23

    readonly property real effectiveStep: integer ? 1 : (step > 0 ? step : 0.01)
    property bool editing: false
    property string buffer: ""
    // Seeded once when an edit starts. Keeping it out of the input's `text`
    // dependency set means typing never re-evaluates the binding, so the caret
    // and the in-progress buffer are never rewritten.
    property string editSeed: ""
    // The scrub preview is local to the control: the panel defers its refresh
    // while a gesture is live, so the editor must not depend on a document
    // round-trip to show the value under the pointer.
    property bool scrubbing: false
    property real scrubPreview: 0
    property bool scrubChanged: false
    property real scrubOrigin: 0
    property bool hasError: errorText.length > 0

    // An exact integer outside the safe double range cannot be scrubbed or
    // stepped without silently quantizing the stored value, so only typed
    // entry remains available for it.
    readonly property bool incrementSafe: valueAvailable && (!integer || text.length === 0 || Math.abs(Number(text)) <= 9007199254740991)
    readonly property string displayedText: editing ? editSeed : scrubbing ? displayText(scrubPreview) : !valueAvailable ? "\u2014" : (text.length > 0 ? text : displayText(value))

    function incrementFor(modifiers) {
        if (modifiers & Qt.ShiftModifier)
            return effectiveStep * 0.1;
        if (modifiers & Qt.ControlModifier)
            return effectiveStep * 10;
        return effectiveStep;
    }

    function clampToTravel(candidate) {
        var result = candidate;
        if (hasSoftMinimum && result < softMinimum)
            result = softMinimum;
        if (hasSoftMaximum && result > softMaximum)
            result = softMaximum;
        if (hasMinimum && result < minimum)
            result = minimum;
        if (hasMaximum && result > maximum)
            result = maximum;
        return result;
    }

    function quantize(candidate) {
        if (integer)
            return Math.round(candidate);
        return candidate;
    }

    function displayText(candidate) {
        if (integer)
            return String(Math.round(candidate));
        var number = Number(candidate);
        if (!Number.isFinite(number))
            return "0";
        if (decimals >= 0)
            return number.toFixed(decimals);
        // Expose the stored value without hiding precision behind a fixed
        // display rounding, and keep exponent notation usable for extremes.
        var precise = number.toPrecision(6);
        if (precise.indexOf("e") >= 0)
            return precise;
        return String(Number(precise));
    }

    // Numeric text without a lossy double conversion: signs, decimals and
    // exponent notation are accepted; integers additionally require digits.
    readonly property var decimalPattern: /^[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?$/
    readonly property var integerPattern: /^[+-]?\d+$/

    function validText(candidate) {
        if (candidate.length === 0)
            return false;
        return integer ? integerPattern.test(candidate) : decimalPattern.test(candidate);
    }

    function adjustBy(delta, modifiers) {
        if (!incrementSafe)
            return;
        var next = quantize(value + delta * incrementFor(modifiers));
        stepped(clampToTravel(next));
    }

    function beginTextEdit() {
        if (!enabled || editing)
            return;
        editing = true;
        editSeed = !valueAvailable ? "" : text.length > 0 ? text : displayText(value);
        buffer = editSeed;
        input.selectAll();
        input.forceActiveFocus();
    }

    function commitBuffer(restoreFocus) {
        if (!editing)
            return;
        var entered = buffer.trim();
        editing = false;
        buffer = "";
        if (restoreFocus)
            root.forceActiveFocus();
        if (!validText(entered)) {
            textRejected(entered);
            return;
        }
        textCommitted(entered);
    }

    function cancelBuffer() {
        editing = false;
        buffer = "";
        root.forceActiveFocus();
    }

    Keys.onUpPressed: function (event) {
        root.adjustBy(1, event.modifiers);
    }
    Keys.onDownPressed: function (event) {
        root.adjustBy(-1, event.modifiers);
    }

    Rectangle {
        anchors.fill: parent
        color: root.enabled ? theme.field : theme.panel
        border.width: 1
        border.color: root.hasError ? theme.errorText : input.activeFocus ? theme.accent : root.activeFocus ? theme.accent : theme.border
        radius: theme.smallRadius
    }

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: TextInput.AlignRight
        font.pixelSize: theme.fontSize
        color: root.enabled ? theme.text : theme.disabled
        selectByMouse: true
        readOnly: !root.editing
        activeFocusOnTab: false
        text: root.displayedText
        Accessible.name: root.label.length > 0 ? root.label : "Numeric value"
        Accessible.description: root.hasError && root.errorText.length > 0 ? root.errorText : tooltip.text
        onTextEdited: {
            if (root.editing)
                root.buffer = text;
        }
        Keys.onReturnPressed: function (event) {
            event.accepted = true;
            root.commitBuffer/*restoreFocus=*/(true);
        }
        Keys.onEnterPressed: function (event) {
            event.accepted = true;
            root.commitBuffer/*restoreFocus=*/(true);
        }
        Keys.onEscapePressed: function (event) {
            event.accepted = true;
            root.cancelBuffer();
        }
        onActiveFocusChanged: {
            if (!activeFocus && root.editing)
                root.commitBuffer/*restoreFocus=*/(false);
        }
    }

    // Click without movement edits the text; horizontal movement past the
    // platform drag threshold scrubs from the captured value. The gesture never
    // runs on the parameter label, so exposure dragging keeps its own handler.
    MouseArea {
        id: gesture
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        enabled: root.enabled && !root.editing
        hoverEnabled: true
        property real pressX: 0
        property bool altPress: false
        onPressed: function (mouse) {
            pressX = mouse.x;
            altPress = !!(mouse.modifiers & Qt.AltModifier);
        }
        onPositionChanged: function (mouse) {
            if (!pressed || altPress || !root.incrementSafe)
                return;
            if (!root.scrubbing) {
                if (Math.abs(mouse.x - pressX) < root.dragThreshold)
                    return;
                root.scrubbing = true;
                root.scrubOrigin = root.value;
                root.scrubPreview = root.value;
                root.scrubChanged = false;
                root.scrubStarted();
            }
            var candidate = root.quantize(root.clampToTravel(root.scrubOrigin + (mouse.x - pressX) * root.incrementFor(mouse.modifiers)));
            if (candidate !== root.scrubOrigin)
                root.scrubChanged = true;
            root.scrubPreview = candidate;
            root.scrubbed(candidate);
        }
        onReleased: {
            if (root.scrubbing) {
                root.scrubbing = false;
                if (root.scrubChanged)
                    root.scrubFinished();
                else
                    root.scrubCancelled();
            } else if (altPress) {
                altPress = false;
                root.keyRequested();
            } else {
                root.beginTextEdit();
            }
        }
        onCanceled: {
            if (root.scrubbing) {
                root.scrubbing = false;
                root.scrubCancelled();
            }
        }
        Accessible.name: root.label.length > 0 ? root.label : "Numeric value"
    }

    HoverHandler {
        id: hover
    }
    ToolTip {
        id: tooltip
        visible: hover.hovered && !root.editing
        text: {
            if (root.hasError && root.errorText.length > 0)
                return root.errorText;
            if (!root.valueAvailable)
                return "No representable value. Click to type a new value.";
            if (!root.incrementSafe)
                return "Exact 64-bit value: type the new value (scrubbing would lose precision).";
            return "Click to type or drag to scrub. Shift fine (" + root.displayText(root.effectiveStep * 0.1) + "), Ctrl coarse (" + root.displayText(root.effectiveStep * 10) + ").";
        }
    }
}
