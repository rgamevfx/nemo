import QtQuick
import QtQuick.Controls

// One numeric editor for every built-in node (stories 1-14). It owns only the
// gesture and the text buffer; the host owns the one parameter gesture
// (begin/update/commit/cancel) and the catalog remains the only validation
// authority. Typing, scrubbing and stepping use legal-value bounds only;
// slider navigation never limits authoring or rounds to display precision.
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
    property real step: 0.01
    // -1 exposes enough significant digits for the stored value.
    property int decimals: -1
    property bool integer: false
    property string label: ""
    property string errorText: ""
    // Platform drag threshold; the panel passes the shared application value.
    property int dragThreshold: 4
    property real fieldWidth: 62

    // Inspector wiring (issue #102), all optional. With a panel and an identity
    // the field owns the shared value menu (key/reset actions) and states the
    // animation status; without them it is an ordinary numeric editor and its
    // right-click stays with its host. Typing, selection, cut/copy/paste,
    // Escape and the one-undo scrub are identical in both cases.
    property var panel: null
    property string networkId: ""
    property string nodeId: ""
    property string parameterKey: ""
    // The one session-scoped interaction owner this control retires before it
    // starts typing: a bound inspector field names its panel, which routes to
    // the shared session owner and so releases an armed viewport pick; a
    // consumer whose gesture belongs to another owner names that owner instead.
    // Null means there is nothing to hand off.
    property var interactionOwner: panel
    property string keyStatus: "none"
    property string scope: ""
    property int frame: 0
    property bool revealAvailable: false
    property bool modified: false
    // Inspector metrics. The defaults are this control's accepted appearance,
    // so an unrelated consumer is unchanged; an inspector row states its own
    // readable height, text size and whether it offers step buttons.
    property int controlHeight: 23
    property int textSize: 0
    property bool stepper: false

    readonly property bool bound: panel !== null && networkId.length > 0 && nodeId.length > 0 && parameterKey.length > 0

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
    implicitHeight: controlHeight

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
    // The host's live parameter gesture: false once the host no longer accepts
    // preview updates. The host binds it from the token IT began, so this
    // control never infers ownership from another control's gesture. A retired
    // gesture (Escape, a preview-only Undo, or another control taking the
    // interaction over) ends the local scrub at once - the authored value is
    // shown again without waiting for a document refresh - and the press that
    // produced the scrub may not start another one, so neither renewed motion
    // nor the release can publish the cancelled preview.
    property bool gestureLive: false
    property bool scrubAbandoned: false
    onGestureLiveChanged: if (!gestureLive)
        endScrub()
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

    function clampToBounds(candidate) {
        var result = candidate;
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
        stepped(clampToBounds(next));
    }

    function beginTextEdit() {
        if (!enabled || editing)
            return;
        // A text edit is a parameter interaction from the first keystroke even
        // though its value is only known at commit, so whatever the session was
        // doing is retired first through the owning presentation interface.
        // That is what releases an armed viewport pick without this control
        // knowing anything about picking.
        if (interactionOwner)
            interactionOwner.prepareParameterInteraction();
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

    // Ends a live scrub without a panel round-trip: the display returns to the
    // authored value the host last published, and the same press stops driving
    // the preview.
    function endScrub() {
        if (!scrubbing)
            return;
        scrubbing = false;
        scrubChanged = false;
        scrubPreview = root.value;
        scrubAbandoned = gesture.pressed;
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
        id: fieldFrame
        anchors.fill: parent
        color: root.enabled ? theme.field : theme.panel
        border.width: 1
        border.color: root.hasError ? theme.errorText : input.activeFocus ? theme.accent : root.activeFocus ? theme.accent : theme.border
        radius: theme.smallRadius
    }

    // The animation status of an inspector value, stated without a key-button
    // column: a keyed value shows a filled diamond, an animated one an outline.
    // An ordinary field (no host wiring, nothing keyed) shows nothing at all.
    Text {
        id: keyGlyph
        visible: root.keyStatus === "key" || root.keyStatus === "animated"
        anchors.left: parent.left
        anchors.leftMargin: 5
        anchors.verticalCenter: parent.verticalCenter
        text: root.keyStatus === "key" ? "\u25c6" : "\u25c7"
        color: theme ? theme.accent : "#3485f6"
        font.pixelSize: 10
        Accessible.name: root.keyStatus === "key" ? "Keyed at this frame" : "Animated"
    }

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: keyGlyph.visible ? 16 : 4
        anchors.rightMargin: root.stepper ? 16 : 4
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: TextInput.AlignRight
        font.pixelSize: root.textSize > 0 ? root.textSize : theme.fontSize
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
            root.scrubAbandoned = false;
        }
        onPositionChanged: function (mouse) {
            if (!pressed || altPress || !root.incrementSafe || root.scrubAbandoned)
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
            var candidate = root.quantize(root.clampToBounds(root.scrubOrigin + (mouse.x - pressX) * root.incrementFor(mouse.modifiers)));
            if (candidate !== root.scrubOrigin)
                root.scrubChanged = true;
            root.scrubPreview = candidate;
            root.scrubbed(candidate);
        }
        onReleased: {
            if (root.scrubAbandoned) {
                // The live gesture was cancelled under this press: the release
                // publishes nothing and does not fall through to a typed edit.
                root.scrubAbandoned = false;
                return;
            }
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
            root.scrubAbandoned = false;
            if (root.scrubbing) {
                root.scrubbing = false;
                root.scrubCancelled();
            }
        }
        Accessible.name: root.label.length > 0 ? root.label : "Numeric value"
    }

    // The up/down affordance of an inspector numeric cell: one click is one
    // discrete step, committed through the same one-undo gesture as an arrow
    // key. A value that cannot be stepped without losing precision (an exact
    // 64-bit integer) disables both buttons and keeps typed entry.
    Item {
        id: stepButtons
        visible: root.stepper
        width: 13
        height: 18
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        enabled: root.incrementSafe

        Text {
            id: stepUpGlyph
            anchors.horizontalCenter: parent.horizontalCenter
            y: -1
            text: "\u25b4"
            font.pixelSize: 8
            color: root.enabled ? theme.muted : theme.disabled
        }

        Text {
            id: stepDownGlyph
            anchors.horizontalCenter: parent.horizontalCenter
            y: 9
            text: "\u25be"
            font.pixelSize: 8
            color: root.enabled ? theme.muted : theme.disabled
        }

        MouseArea {
            anchors.top: parent.top
            width: parent.width
            height: parent.height / 2
            enabled: stepButtons.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: function (mouse) {
                root.adjustBy(1, mouse.modifiers);
            }
        }

        MouseArea {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height / 2
            enabled: stepButtons.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: function (mouse) {
                root.adjustBy(-1, mouse.modifiers);
            }
        }
    }

    // Right-click on an inspector value offers the shared value actions. The
    // menu belongs to KeyIndicator, the one owner of the key status and the
    // action wording, so no second copy of it exists; an ordinary field with no
    // host wiring keeps plain text editing and no menu at all.
    MouseArea {
        id: contextMenuArea
        anchors.fill: parent
        acceptedButtons: root.bound ? Qt.RightButton : Qt.NoButton
        onClicked: valueMenu.openMenu(fieldFrame)
    }

    KeyIndicator {
        id: valueMenu
        width: 0
        height: 0
        theme: root.theme
        networkId: root.networkId
        nodeId: root.nodeId
        parameterKey: root.parameterKey
        parameterLabel: root.label
        keyStatus: root.keyStatus
        scope: root.scope
        frame: root.frame
        revealAvailable: root.revealAvailable
        resettable: root.bound
        modified: root.modified
        onKeyRequested: if (root.bound)
            root.panel.keyParameterAtFrame(root.networkId, root.nodeId, root.parameterKey)
        onRemoveKeyRequested: if (root.bound)
            root.panel.removeParameterKeyAtFrame(root.networkId, root.nodeId, root.parameterKey)
        onRevealRequested: if (root.bound)
            root.panel.revealInAnimation(root.networkId, root.nodeId, root.parameterKey)
        onResetRequested: if (root.bound)
            root.panel.resetValue({
                "networkId": root.networkId,
                "nodeId": root.nodeId,
                "parameterKey": root.parameterKey,
                "label": root.label
            })
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
            var status = "";
            if (root.keyStatus === "key")
                status = " Keyed at frame " + root.frame + ".";
            else if (root.keyStatus === "animated")
                status = " Animated; no key at frame " + root.frame + ".";
            var actions = root.bound ? " Right-click for key and reset actions." : "";
            if (!root.incrementSafe)
                return "Exact 64-bit value: type the new value (scrubbing would lose precision)." + status + actions;
            return "Click to type or drag to scrub. Shift fine (" + root.displayText(root.effectiveStep * 0.1) + "), Ctrl coarse (" + root.displayText(root.effectiveStep * 10) + ")." + status + actions;
        }
    }
}
