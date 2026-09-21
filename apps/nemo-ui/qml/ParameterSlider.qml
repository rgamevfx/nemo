import QtQuick

// The shared inspector slider (issue #102). It owns the pointer gesture and its
// own preview only; the host owns the one parameter gesture, so a drag becomes
// exactly one history entry:
//
//   editStarted()   the press begins the host gesture (the host raises
//                   `gestureLive` while it still accepts previews)
//   valueEdited()   the snapped value under the pointer, previewed only
//   editFinished()  release after a real change: publish
//   editCancelled() release without a change, Escape, a preview-only Undo or a
//                   gesture the session handed to another control: publish
//                   nothing
//
// A press seeks the value under the pointer. Beginning the host gesture retires
// whatever interaction the session was running first, so a press always owns
// the next one. If the host does not grant it (`gestureLive` stays false) or
// retires it mid-drag, the handle returns to the authored value at once and
// neither renewed motion nor the release can publish the abandoned preview.
// `graduated` adds a linear ruler with tick labels and `markers` places the
// per-channel positions of an expanded tuple; both are linear divisions of
// [from, to] and imply no transfer function.
Item {
    id: slider

    property var theme: null
    property real from: 0
    property real to: 1
    property real value: 0
    property real stepSize: 0.01
    property bool graduated: false
    // [{ value: real, color: color }]
    property var markers: []
    property bool gestureLive: true
    property string label: ""

    signal editStarted()
    signal valueEdited(real value)
    signal editFinished()
    signal editCancelled()
    signal keyRequested()

    readonly property real travel: Math.max(0, to - from)
    readonly property int handleSize: 12
    readonly property int padding: 6
    readonly property real trackWidth: Math.max(1, width - 2 * padding)
    readonly property real handleTravel: Math.max(1, trackWidth - handleSize)
    readonly property real trackY: graduated ? 8 : height / 2
    readonly property real displayValue: scrubbing ? scrubPreview : value
    // Linear ruler ticks: a "nice" step that divides the travel into about
    // eight labels, formatted to the step's own precision.
    readonly property var ticks: tickValues()

    // The live drag. The preview is local to this control: the host defers its
    // refresh while a gesture is live, so the handle must not wait for a
    // document round-trip to follow the pointer.
    property bool scrubbing: false
    property real scrubOrigin: 0
    property real scrubPreview: 0
    property bool scrubAbandoned: false

    implicitWidth: 120
    implicitHeight: graduated ? 34 : 22
    Accessible.role: Accessible.Slider
    Accessible.name: label.length > 0 ? label + " slider" : "Slider"

    onGestureLiveChanged: if (!gestureLive)
        abandonScrub()

    function clampUnit(candidate) {
        return Math.min(1, Math.max(0, candidate));
    }

    function positionFor(candidate) {
        return travel <= 0 ? 0 : clampUnit((candidate - from) / travel);
    }

    // Center of the handle for a value; the one mapping ticks, markers, the
    // handle and the pointer share, so a click lands where it points.
    function centerXFor(candidate) {
        return padding + handleSize / 2 + positionFor(candidate) * handleTravel;
    }

    function snapped(candidate) {
        var bounded = Math.min(to, Math.max(from, candidate));
        if (!(stepSize > 0) || travel <= 0)
            return bounded;
        return Math.min(to, Math.max(from, from + Math.round((bounded - from) / stepSize) * stepSize));
    }

    function valueAt(x) {
        return snapped(from + clampUnit((x - padding - handleSize / 2) / handleTravel) * travel);
    }

    function seek(x) {
        if (!scrubbing || scrubAbandoned || !gestureLive)
            return;
        var candidate = valueAt(x);
        scrubPreview = candidate;
        valueEdited(candidate);
    }

    // Ends a live drag without publishing: the host cancelled the gesture or
    // the begin was refused, so the authored value is shown again.
    function abandonScrub() {
        if (!scrubbing)
            return;
        scrubbing = false;
        scrubPreview = value;
        scrubAbandoned = true;
    }

    function tickValues() {
        if (!graduated || travel <= 0)
            return [];
        var target = travel / Math.max(1, Math.min(8, Math.floor(width / 44)));
        var magnitude = Math.pow(10, Math.floor(Math.log(target) / Math.LN10));
        var normalized = target / magnitude;
        var multiplier = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
        var step = multiplier * magnitude;
        var decimals = Math.max(0, Math.ceil(-Math.log(step) / Math.LN10));
        var result = [];
        for (var tick = Math.ceil(from / step) * step; tick <= to + step * 1e-6 && result.length < 64; tick += step) {
            var rounded = Math.abs(tick) < step * 1e-6 ? 0 : tick;
            result.push({
                    "value": rounded,
                    "label": rounded.toFixed(decimals)
                });
        }
        return result;
    }

    Rectangle {
        id: groove
        x: slider.padding
        y: slider.trackY - height / 2
        width: slider.trackWidth
        height: slider.graduated ? 8 : 4
        radius: slider.graduated ? 1 : 2
        color: slider.graduated && slider.theme ? slider.theme.field : (slider.theme ? slider.theme.border : "#30343a")
        border.width: slider.graduated ? 1 : 0
        border.color: slider.theme ? slider.theme.border : "#30343a"
    }

    Rectangle {
        id: fill
        x: groove.x
        y: groove.y
        width: Math.max(0, slider.centerXFor(slider.displayValue) - slider.padding)
        height: groove.height
        radius: 2
        visible: !slider.graduated && width > 0
        color: slider.theme ? slider.theme.accent : "#3485f6"
    }

    Repeater {
        id: minorTicks
        model: slider.graduated ? Math.max(1, Math.floor(slider.handleTravel / 8)) : 0
        delegate: Rectangle {
            required property int index
            x: slider.centerXFor(slider.from) + (index + 0.5) * slider.handleTravel / minorTicks.count
            y: groove.y + 2
            width: 1
            height: 4
            color: slider.theme ? slider.theme.border : "#30343a"
        }
    }

    Repeater {
        model: slider.ticks
        delegate: Item {
            required property var modelData
            x: slider.centerXFor(Number(modelData.value)) - width / 2
            y: groove.y + groove.height
            width: Math.max(16, tickLabel.implicitWidth)
            height: Math.max(0, slider.height - y)
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 1
                height: 4
                color: slider.theme ? slider.theme.border : "#30343a"
            }
            Text {
                id: tickLabel
                anchors.top: parent.top
                anchors.topMargin: 5
                anchors.horizontalCenter: parent.horizontalCenter
                text: modelData.label
                color: slider.theme ? slider.theme.muted : "#979ea8"
                font.pixelSize: slider.theme ? Math.max(9, slider.theme.inspectorFontSize - 3) : 9
            }
        }
    }

    Repeater {
        model: slider.markers
        delegate: Rectangle {
            required property var modelData
            x: slider.centerXFor(Number(modelData.value)) - width / 2
            y: slider.trackY - height / 2
            width: 2
            height: 12
            radius: 1
            color: modelData.color !== undefined ? modelData.color : (slider.theme ? slider.theme.accent : "#3485f6")
        }
    }

    Rectangle {
        id: handle
        visible: !slider.graduated || slider.markers.length === 0
        x: slider.centerXFor(slider.displayValue) - width / 2
        y: slider.trackY - height / 2
        width: slider.graduated ? 3 : slider.handleSize
        height: slider.graduated ? 14 : slider.handleSize
        radius: slider.graduated ? 0 : width / 2
        border.width: slider.graduated ? 0 : 1
        border.color: slider.theme ? slider.theme.border : "#30343a"
        color: !slider.theme ? "#3485f6" : slider.graduated ? slider.theme.accent : slider.theme.text
    }

    MouseArea {
        id: gesture
        property bool altPress: false
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        enabled: slider.enabled
        onPressed: function(mouse) {
            altPress = !!(mouse.modifiers & Qt.AltModifier);
            if (altPress)
                return;
            slider.scrubAbandoned = false;
            slider.scrubOrigin = slider.value;
            slider.scrubPreview = slider.value;
            slider.scrubbing = true;
            slider.editStarted();
            slider.seek(mouse.x);
        }
        onPositionChanged: function(mouse) {
            if (pressed && !altPress)
                slider.seek(mouse.x);
        }
        onReleased: {
            if (altPress) {
                altPress = false;
                slider.keyRequested();
                return;
            }
            if (slider.scrubAbandoned) {
                slider.scrubAbandoned = false;
                return;
            }
            if (!slider.scrubbing)
                return;
            slider.scrubbing = false;
            if (slider.scrubPreview !== slider.scrubOrigin)
                slider.editFinished();
            else
                slider.editCancelled();
        }
        onCanceled: {
            altPress = false;
            slider.scrubAbandoned = false;
            if (!slider.scrubbing)
                return;
            slider.scrubbing = false;
            slider.editCancelled();
        }
    }
}
