import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shuffle socket mapping editor (issue #90, stories 21-27), hosted by the
// generic inspector through ParameterEditorRegistry id "nemo.shuffle.mapping"
// with presentation "section". The host mounts it on the `input1` row and
// consumes every other Shuffle key, so this editor is the ONE control for the
// node's mapping parameters.
//
// It presents the Nuke 17 Shuffle reference layout: input groups on the left
// (which image and which named layer each group reads) and output groups on the
// right (the four mapping rows of each group), with coloured sockets, wires and
// per-output zero/one constants. A drag connects in either direction, a
// completed gesture is ONE shared parameter gesture (one undo entry), Escape or
// an invalid drop cancels it, and changing a group selector rewrites that
// group's authored rows inside the same atomic gesture so the rows always
// represent the mappings the artist sees.
//
// The typed schema is the authored state, so this editor owns no mapping, no
// channel storage and no second parameter model: rows are read through the
// shared inspector query and written through the shared panel gesture. Named
// channel availability comes from the target's ACTUAL described channels,
// queried on the worker (ViewerController::nodeInputChannels); nothing here
// probes media, and a channel that cannot be observed is never invented.
//
// Documented Nuke 17 shortcuts implemented here (swapping_channels.html):
//   drag/drop            connect one socket, from either end
//   double-click         auto-connect by order within the group
//   Ctrl/Cmd+double-click auto-connect by channel name within the group
//   Ctrl/Cmd+drag        connect the channel and all lower channels
//   Ctrl/Cmd+Shift+drag  connect the channel and all higher channels
//   Alt+drag             broadcast the input channel to every output channel
//   Ctrl/Cmd+click 0/1   set all output channels to black (0) / white (1)
// Layer reordering is the group handle: drop it on the sibling group to reorder,
// or drop an input group on an output group to connect all channels by order.
//
// Every consumed parameter stays reachable through the shared animation owners:
// each row's output channel is a shared label cell (exposure drag) plus a shared
// key cell (Set/Update/Remove Key, Show in Animation), the group selectors and
// each row's constants key through the same Alt-click hold-key gesture, and no
// numeric exposure control applies because the Shuffle schema declares no
// numeric parameter.
//
// Owner-approved deviation: Nemo preserves explicitly authored channel names;
// it does not impose Nuke's built-in reservations or automatic renaming.
ColumnLayout {
    id: shuffleEditor

    // Host-injected contract (ParametersPanel). A bare host may load this editor
    // without a controller; every control then states that it cannot act.
    property var theme
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "input1"
    property var parameter
    property var controller
    property var panel

    readonly property color textColor: shuffleEditor.theme ? shuffleEditor.theme.text : "#dce0e6"
    readonly property color mutedColor: shuffleEditor.theme ? shuffleEditor.theme.muted : "#979ea8"
    readonly property color borderColor: shuffleEditor.theme ? shuffleEditor.theme.border : "#30343a"
    readonly property color fieldColor: shuffleEditor.theme ? shuffleEditor.theme.field : "#24272c"
    readonly property color panelColor: shuffleEditor.theme ? shuffleEditor.theme.panel : "#1e2023"
    readonly property color raisedColor: shuffleEditor.theme ? shuffleEditor.theme.raised : "#282c31"
    readonly property color hoverColor: shuffleEditor.theme ? shuffleEditor.theme.hover : "#343940"
    readonly property color accentColor: shuffleEditor.theme ? shuffleEditor.theme.accent : "#3485f6"
    readonly property color disabledColor: shuffleEditor.theme ? shuffleEditor.theme.disabled : "#5f6670"
    readonly property color errorColor: shuffleEditor.theme ? shuffleEditor.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: shuffleEditor.theme ? shuffleEditor.theme.smallRadius : 4
    readonly property int fontSizeValue: shuffleEditor.theme ? shuffleEditor.theme.fontSize : 11
    readonly property int smallFontSize: Math.max(9, shuffleEditor.fontSizeValue - 1)
    readonly property int rowHeight: 19
    readonly property int socketSize: 13

    // The schema's own split: rows 0..3 are group 1 and rows 4..7 are group 2, so
    // a group's sockets ARE its rows and no row is ever renumbered here.
    readonly property int rowsPerGroup: 4
    readonly property int rowCount: 8
    // The channel-name convention's default layer, spelled exactly as the
    // schema's in1/out1 defaults spell it.
    readonly property string rootLayerName: "rgba"

    readonly property int inputColumnWidth: 172
    readonly property int outputColumnWidth: 264
    readonly property int wireGap: 26

    objectName: "shuffleEditor_" + shuffleEditor.nodeId
    Layout.fillWidth: true
    spacing: 3

    // --- query coordinates -------------------------------------------------
    // The inspector query addresses the real node that owns the parameters (a
    // definition network for an occurrence exposure); gestures stay on the host
    // row, so an edit can never leave its own scope.
    readonly property string queryNetwork: shuffleEditor.parameter && shuffleEditor.parameter.targetNetwork !== undefined ? String(shuffleEditor.parameter.targetNetwork) : shuffleEditor.networkId
    readonly property string queryNode: shuffleEditor.parameter && shuffleEditor.parameter.targetNode !== undefined ? String(shuffleEditor.parameter.targetNode) : shuffleEditor.nodeId
    readonly property int revision: shuffleEditor.panel ? Number(shuffleEditor.panel.revision) : 0
    readonly property int frame: shuffleEditor.controller ? Number(shuffleEditor.controller.frame) : 0

    // --- model -------------------------------------------------------------
    property var paramRows: ({})
    onParamRowsChanged: shuffleEditor.scheduleWirePaint()
    property string schemaProblem: ""
    property string gestureProblem: ""
    // Named channel availability of this node's image inputs, as answered by the
    // worker description query. Never a fabricated list.
    property var channelAnswer: ({})
    onChannelAnswerChanged: shuffleEditor.scheduleWirePaint()
    property int channelAnswerEpoch: 0
    // Transient (never persisted) source channels typed by the artist, so a
    // mapping stays authorable when a target cannot be described. This is
    // presentation state: the authored source channel is the row's own value.
    property var typedSources: []

    function paramRow(key) {
        return shuffleEditor.paramRows ? shuffleEditor.paramRows[key] : null;
    }

    function rowValueText(key) {
        var row = shuffleEditor.paramRow(key);
        if (!row)
            return "";
        if (row.valueText !== undefined && String(row.valueText).length > 0)
            return String(row.valueText);
        if (row.value !== undefined && row.value !== null)
            return String(row.value);
        return "";
    }

    function refresh() {
        if (!shuffleEditor.controller || shuffleEditor.nodeId.length === 0) {
            shuffleEditor.paramRows = ({});
            shuffleEditor.channelAnswer = ({});
            return;
        }
        var inspector = shuffleEditor.controller.parameterInspector(shuffleEditor.queryNetwork, shuffleEditor.queryNode);
        var rows = ({});
        var sections = inspector && inspector.sections ? inspector.sections : [];
        for (var index = 0; index < sections.length; ++index) {
            var parameters = sections[index].parameters || [];
            for (var row = 0; row < parameters.length; ++row)
                rows[String(parameters[row].key)] = parameters[row];
        }
        shuffleEditor.paramRows = rows;
        shuffleEditor.schemaProblem = inspector && inspector.available === false ? String(inspector.reason === undefined ? "" : inspector.reason) : "";
        shuffleEditor.refreshChannels();
    }

    function refreshChannels() {
        if (!shuffleEditor.controller || !shuffleEditor.controller.nodeInputChannels) {
            shuffleEditor.channelAnswer = ({});
            return;
        }
        shuffleEditor.channelAnswer = shuffleEditor.controller.nodeInputChannels(shuffleEditor.queryNetwork, shuffleEditor.queryNode);
    }

    onRevisionChanged: shuffleEditor.refresh()
    onFrameChanged: shuffleEditor.refresh()
    // The host injects identity and controller after the item exists (it supplies
    // them through the loader's sync), so a late binding of any of them must
    // refresh too, not only a revision change.
    onControllerChanged: shuffleEditor.refresh()
    onPanelChanged: shuffleEditor.refresh()
    onNodeIdChanged: shuffleEditor.refresh()
    onQueryNetworkChanged: shuffleEditor.refresh()
    onQueryNodeChanged: shuffleEditor.refresh()
    Component.onCompleted: shuffleEditor.refresh()

    Connections {
        target: shuffleEditor.controller
        ignoreUnknownSignals: true
        function onNodeChannelsChanged() {
            shuffleEditor.refreshChannels();
            shuffleEditor.channelAnswerEpoch = shuffleEditor.channelAnswerEpoch + 1;
        }
    }

    // --- the channel-name convention at the QML boundary --------------------
    // Mirrors core's nemo::channelLayer/channelLeaf (core/evaluation/Image.hpp):
    // a stored name's layer is the prefix before its last dot, and a root channel
    // has none. Root channels are addressed by the schema's default layer name,
    // so a file writing "R" and a file writing "rgba.R" are both addressed as
    // "rgba" without renaming either one.
    function layerOf(name) {
        var text = String(name);
        var dot = text.lastIndexOf(".");
        return dot <= 0 ? shuffleEditor.rootLayerName : text.substring(0, dot);
    }

    function leafOf(name) {
        var text = String(name);
        var dot = text.lastIndexOf(".");
        return dot < 0 ? text : text.substring(dot + 1);
    }

    function fullChannelName(layer, channel) {
        var leaf = String(channel);
        if (leaf.length === 0)
            return "";
        var target = String(layer);
        if (target.length === 0 || target === shuffleEditor.rootLayerName)
            return leaf;
        return target + "." + leaf;
    }

    // A socket's colour states its role. It is presentation only, so a data
    // channel simply takes the theme accent.
    function channelColor(name) {
        var leaf = shuffleEditor.leafOf(name).toLowerCase();
        if (leaf === "r" || leaf === "red")
            return "#e2564d";
        if (leaf === "g" || leaf === "green")
            return "#5cc45c";
        if (leaf === "b" || leaf === "blue")
            return "#4d8ee2";
        if (leaf === "a" || leaf === "alpha")
            return shuffleEditor.textColor;
        return shuffleEditor.accentColor;
    }

    // --- rows and groups ----------------------------------------------------
    function groupOf(row) {
        return Math.floor(Number(row) / shuffleEditor.rowsPerGroup);
    }

    function firstRowOf(group) {
        return Number(group) * shuffleEditor.rowsPerGroup;
    }

    function inputChoiceKey(group) {
        return Number(group) === 0 ? "input1" : "input2";
    }

    function inLayerKey(group) {
        return Number(group) === 0 ? "in1" : "in2";
    }

    function outLayerKey(group) {
        return Number(group) === 0 ? "out1" : "out2";
    }

    // The image a group reads: the schema's own Choice. The declared primary
    // input is the fallback only when the row itself is unavailable.
    function groupImage(group) {
        var value = shuffleEditor.rowValueText(shuffleEditor.inputChoiceKey(group));
        return value === "A" || value === "B" ? value : "B";
    }

    function inLayer(group) {
        return shuffleEditor.rowValueText(shuffleEditor.inLayerKey(group));
    }

    function outLayer(group) {
        return shuffleEditor.rowValueText(shuffleEditor.outLayerKey(group));
    }

    // The declared Choice is the authority for the input selector; the frozen
    // pair is only the fallback for a host whose query cannot see the row yet.
    function imageChoices(group) {
        var row = shuffleEditor.paramRow(shuffleEditor.inputChoiceKey(group));
        return row && row.choices ? row.choices : ["B", "A"];
    }

    function rowSourceKind(row) {
        var value = shuffleEditor.rowValueText("sourceKind" + row);
        return value.length > 0 ? value : shuffleEditor.inputChoiceKey(shuffleEditor.groupOf(row));
    }

    function rowInputGroup(row) {
        var kind = shuffleEditor.rowSourceKind(row);
        return kind === "input1" ? 0 : kind === "input2" ? 1 : -1;
    }

    function rowKind(row) {
        var group = shuffleEditor.rowInputGroup(row);
        return group >= 0 ? shuffleEditor.groupImage(group) : shuffleEditor.rowSourceKind(row);
    }

    function rowSource(row) {
        return shuffleEditor.rowValueText("sourceChannel" + row);
    }

    function rowOutput(row) {
        return shuffleEditor.rowValueText("outputChannel" + row);
    }

    function rowEnabled(row) {
        return shuffleEditor.rowOutput(row).length > 0;
    }

    // What one authored row actually resolves to, using the same exact-name
    // lookup the node performs on its input (never a UI-side layer filter):
    // "disabled" (no output channel), "constant", "ready", "missing" (the named
    // source channel is not carried) or "disconnected" (the row's input has
    // nothing connected to it). The frozen policy makes missing and disconnected
    // sources zero, so both are stated on the row instead of being inferred.
    function rowSourceState(row) {
        if (!shuffleEditor.rowEnabled(row))
            return "disabled";
        var kind = shuffleEditor.rowKind(row);
        if (kind === "zero" || kind === "one")
            return "constant";
        var port = shuffleEditor.portForImage(kind);
        if (port && port.connected !== true)
            return "disconnected";
        var source = shuffleEditor.rowSource(row);
        if (source.length === 0)
            return "missing";
        var channels = port && port.channels ? port.channels : [];
        return channels.indexOf(source) < 0 ? "missing" : "ready";
    }

    function rowSourceIssue(row) {
        var state = shuffleEditor.rowSourceState(row);
        if (state === "missing")
            return "Source channel '" + shuffleEditor.rowSource(row) + "' is not carried by " + shuffleEditor.rowKind(row) + "; the output is zero";
        if (state === "disconnected")
            return shuffleEditor.rowKind(row) + " is not connected; the output is zero";
        return "";
    }

    // --- described channels -------------------------------------------------
    // Port 0 is B (the required primary) and port 1 is A (optional); the answer
    // reports the ports by name, so the lookup follows the descriptor rather than
    // assuming an order.
    function portForImage(image) {
        var ports = shuffleEditor.channelAnswer && shuffleEditor.channelAnswer.ports ? shuffleEditor.channelAnswer.ports : [];
        for (var index = 0; index < ports.length; ++index) {
            if (String(ports[index].name).toUpperCase() === String(image).toUpperCase())
                return ports[index];
        }
        var fallback = image === "A" ? 1 : 0;
        return fallback < ports.length ? ports[fallback] : null;
    }

    function portReason(image) {
        var port = shuffleEditor.portForImage(image);
        if (!port)
            return "Channel availability is not known yet";
        if (port.connected !== true)
            return String(image) + " is not connected";
        var reason = port.reason !== undefined ? String(port.reason) : "";
        if (reason.length > 0)
            return reason;
        return String(port.state) === "pending" ? "Reading " + String(image) + " channels..." : "";
    }

    function portChannels(image) {
        var port = shuffleEditor.portForImage(image);
        return port && port.channels ? port.channels : [];
    }

    function portLayers(image) {
        var port = shuffleEditor.portForImage(image);
        return port && port.layers ? port.layers : [];
    }

    // Real channels a group can source from: the described channels of the image
    // the group reads, narrowed to the group's input layer, the channels this
    // group's rows already map, plus the transient channels the artist typed.
    function groupSourceChannels(group) {
        var image = shuffleEditor.groupImage(group);
        var layer = shuffleEditor.inLayer(group);
        var described = shuffleEditor.portChannels(image);
        var result = [];
        for (var index = 0; index < described.length; ++index) {
            var name = String(described[index]);
            if (shuffleEditor.layerOf(name) === layer && result.indexOf(name) < 0)
                result.push(name);
        }
        // Include authored sources even when unavailable, so their dotted wires
        // survive disconnection and can recover without rewriting the mapping.
        for (var row = 0; row < shuffleEditor.rowCount; ++row) {
            if (!shuffleEditor.rowEnabled(row) || shuffleEditor.rowInputGroup(row) !== group)
                continue;
            var source = shuffleEditor.rowSource(row);
            if (source.length > 0 && result.indexOf(source) < 0)
                result.push(source);
        }
        for (var extra = 0; extra < shuffleEditor.typedSources.length; ++extra) {
            var typed = shuffleEditor.typedSources[extra];
            if (Number(typed.group) !== Number(group) || String(typed.image) !== image)
                continue;
            if (result.indexOf(String(typed.channel)) < 0)
                result.push(String(typed.channel));
        }
        return result;
    }

    // Layers an output group can address: the layers this node's own primary
    // input describes (an untouched B keeps its channels), the layers the group's
    // rows already name, and the group's current selector. A brand-new layer is
    // typed into the selector or into the new-channel dialog, exactly like the
    // reference's "new" entry.
    function outputLayerNames(group) {
        var names = ["none"];
        var described = shuffleEditor.portLayers("B");
        for (var index = 0; index < described.length; ++index) {
            var name = String(described[index].name);
            if (names.indexOf(name) < 0)
                names.push(name);
        }
        var first = shuffleEditor.firstRowOf(group);
        for (var row = first; row < first + shuffleEditor.rowsPerGroup; ++row) {
            var output = shuffleEditor.rowOutput(row);
            if (output.length === 0)
                continue;
            var layer = shuffleEditor.layerOf(output);
            if (names.indexOf(layer) < 0)
                names.push(layer);
        }
        var current = shuffleEditor.outLayer(group);
        if (current.length > 0 && names.indexOf(current) < 0)
            names.push(current);
        names.push("new");
        return names;
    }

    function inputLayerNames(group) {
        var names = ["none"];
        var described = shuffleEditor.portLayers(shuffleEditor.groupImage(group));
        for (var index = 0; index < described.length; ++index) {
            var name = String(described[index].name);
            if (names.indexOf(name) < 0)
                names.push(name);
        }
        var current = shuffleEditor.inLayer(group);
        if (current.length > 0 && names.indexOf(current) < 0)
            names.push(current);
        return names;
    }

    // --- sockets ------------------------------------------------------------
    // Presentation geometry only: the registry lets the wires and the drop test
    // find the socket an item currently draws, without a second model.
    property var socketRegistry: []
    property int socketEpoch: 0

    function registerSocket(socket) {
        var next = shuffleEditor.socketRegistry.slice();
        next.push(socket);
        shuffleEditor.socketRegistry = next;
        shuffleEditor.socketEpoch = shuffleEditor.socketEpoch + 1;
        shuffleEditor.scheduleWirePaint();
    }

    function unregisterSocket(socket) {
        var next = [];
        for (var index = 0; index < shuffleEditor.socketRegistry.length; ++index) {
            if (shuffleEditor.socketRegistry[index] !== socket)
                next.push(shuffleEditor.socketRegistry[index]);
        }
        shuffleEditor.socketRegistry = next;
        shuffleEditor.socketEpoch = shuffleEditor.socketEpoch + 1;
        shuffleEditor.scheduleWirePaint();
    }

    function socketEntry(role, group, channel, row) {
        for (var index = 0; index < shuffleEditor.socketRegistry.length; ++index) {
            var socket = shuffleEditor.socketRegistry[index];
            if (!socket)
                continue;
            var entry = socket.entry;
            if (String(entry.role) !== String(role))
                continue;
            if (String(role) === "input") {
                if (Number(entry.group) === Number(group) && String(entry.channel) === String(channel))
                    return entry;
            } else if (Number(entry.row) === Number(row)) {
                return entry;
            }
        }
        return null;
    }

    function socketPointIn(canvas, entry) {
        if (!entry || !entry.item || !canvas)
            return null;
        return entry.item.mapToItem(canvas, entry.item.width / 2, entry.item.height / 2);
    }

    function socketAt(point) {
        if (!point)
            return null;
        for (var index = 0; index < shuffleEditor.socketRegistry.length; ++index) {
            var socket = shuffleEditor.socketRegistry[index];
            if (!socket)
                continue;
            var entry = socket.entry;
            if (!entry.item || entry.item.width <= 0)
                continue;
            var local = entry.item.mapFromItem(shuffleEditor, point.x, point.y);
            if (local.x >= -4 && local.y >= -4 && local.x <= entry.item.width + 4 && local.y <= entry.item.height + 4)
                return entry;
        }
        return null;
    }

    // A wire belongs to its authored source group, including duplicate groups.
    function sourceSocket(row) {
        var group = shuffleEditor.rowInputGroup(row);
        return group < 0 ? null : shuffleEditor.socketEntry("input", group, shuffleEditor.rowSource(row));
    }

    function scheduleWirePaint() {
        Qt.callLater(function () {
            wires.requestPaint();
        });
    }

    function strokeWire(context, from, to) {
        context.beginPath();
        if (from) {
            var mid = (from.x + to.x) / 2;
            context.moveTo(from.x, from.y);
            context.bezierCurveTo(mid, from.y, mid, to.y, to.x, to.y);
        } else {
            context.moveTo(shuffleEditor.inputColumnWidth + 2, to.y);
            context.lineTo(to.x - 3, to.y);
        }
        context.stroke();
    }

    function paintWires(canvas) {
        var context = canvas.getContext("2d");
        if (!context)
            return;
        context.reset();
        context.lineWidth = 1.4;
        for (var row = 0; row < shuffleEditor.rowCount; ++row) {
            if (!shuffleEditor.rowEnabled(row))
                continue;
            var output = shuffleEditor.socketEntry("output", 0, "", row);
            var to = shuffleEditor.socketPointIn(canvas, output);
            if (!to)
                continue;
            var kind = shuffleEditor.rowKind(row);
            if (kind === "zero" || kind === "one") {
                // A constant is the socket's own fill: it has no source wire.
                continue;
            }
            var source = shuffleEditor.rowSource(row);
            var from = null;
            var missing = shuffleEditor.rowSourceState(row) !== "ready";
            if (source.length === 0) {
                missing = true;
            } else {
                var entry = shuffleEditor.sourceSocket(row);
                if (entry)
                    from = shuffleEditor.socketPointIn(canvas, entry);
                else
                    missing = true;
            }
            context.strokeStyle = missing ? shuffleEditor.mutedColor : shuffleEditor.channelColor(source);
            context.setLineDash(missing ? [3, 3] : []);
            shuffleEditor.strokeWire(context, from, to);
            context.setLineDash([]);
        }
        if (!shuffleEditor.dragSource || !shuffleEditor.dragPoint)
            return;
        context.strokeStyle = "#ffffff";
        context.lineWidth = 1.8;
        if (shuffleEditor.dragValid) {
            var proposed = shuffleEditor.connectionValues(shuffleEditor.dragSource, shuffleEditor.dragTarget, shuffleEditor.dragMode());
            for (var proposedRow = 0; proposedRow < shuffleEditor.rowCount; ++proposedRow) {
                var proposedKind = proposed["sourceKind" + proposedRow];
                if (proposedKind === undefined)
                    continue;
                var proposedSource = shuffleEditor.socketEntry("input", proposedKind === "input1" ? 0 : 1,
                                                               proposed["sourceChannel" + proposedRow]);
                var proposedOutput = shuffleEditor.socketEntry("output", 0, "", proposedRow);
                var proposedFrom = shuffleEditor.socketPointIn(canvas, proposedSource);
                var proposedTo = shuffleEditor.socketPointIn(canvas, proposedOutput);
                if (proposedFrom && proposedTo)
                    shuffleEditor.strokeWire(context, proposedFrom, proposedTo);
            }
        } else {
            var origin = shuffleEditor.socketPointIn(canvas, shuffleEditor.dragSource);
            var point = canvas.mapFromItem(shuffleEditor, shuffleEditor.dragPoint.x, shuffleEditor.dragPoint.y);
            if (origin) {
                context.setLineDash([3, 3]);
                shuffleEditor.strokeWire(context, origin, point);
            }
        }
    }

    // --- socket drag --------------------------------------------------------
    // ONE live gesture at a time, owned by the editor. A press that never moved
    // starts nothing, and an invalid drop or Escape publishes nothing.
    property var dragSource: null
    property var dragTarget: null
    property bool dragValid: false
    property var dragPoint: null
    property string dragHint: ""
    property int lastModifiers: 0
    readonly property int dragThreshold: shuffleEditor.controller && shuffleEditor.controller.dragDistance !== undefined ? Number(shuffleEditor.controller.dragDistance) : 4

    function beginSocketDrag(entry, point) {
        shuffleEditor.dragSource = entry;
        shuffleEditor.dragTarget = null;
        shuffleEditor.dragValid = false;
        shuffleEditor.dragPoint = point;
        shuffleEditor.dragHint = "";
        wires.requestPaint();
    }

    function updateSocketDrag(point) {
        if (!shuffleEditor.dragSource)
            return;
        shuffleEditor.dragPoint = point;
        var hovered = shuffleEditor.socketAt(point);
        var other = hovered && String(hovered.role) !== String(shuffleEditor.dragSource.role) ? hovered : null;
        shuffleEditor.dragTarget = other;
        shuffleEditor.dragValid = other ? shuffleEditor.dropIsValid(shuffleEditor.dragSource, other) : false;
        if (!other)
            shuffleEditor.dragHint = "";
        else if (shuffleEditor.dragValid)
            shuffleEditor.dragHint = shuffleEditor.dropHint(shuffleEditor.dragSource, other);
        else
            shuffleEditor.dragHint = shuffleEditor.invalidDropReason(shuffleEditor.dragSource, other);
        wires.requestPaint();
    }

    function endSocketDrag(committed) {
        var source = shuffleEditor.dragSource;
        var target = shuffleEditor.dragTarget;
        var valid = shuffleEditor.dragValid;
        shuffleEditor.dragSource = null;
        shuffleEditor.dragTarget = null;
        shuffleEditor.dragValid = false;
        shuffleEditor.dragPoint = null;
        shuffleEditor.dragHint = "";
        wires.requestPaint();
        if (!source || !target)
            return;
        if (!committed || !valid)
            return;
        shuffleEditor.connectSockets(source, target, shuffleEditor.dragMode());
    }

    // The documented modifier set, read at release from the live keyboard state,
    // so a drag that changed modifiers still means what the artist held.
    function dragMode() {
        var modifiers = shuffleEditor.lastModifiers;
        if (modifiers & Qt.AltModifier)
            return "broadcast";
        if (modifiers & Qt.ControlModifier)
            return (modifiers & Qt.ShiftModifier) ? "higher" : "lower";
        return "single";
    }

    function dropIsValid(source, target) {
        var rows = shuffleEditor.targetRows(source, target, "single");
        if (!rows || rows.length === 0)
            return false;
        for (var index = 0; index < rows.length; ++index) {
            if (shuffleEditor.rowEnabled(rows[index]))
                return true;
        }
        return false;
    }

    function invalidDropReason(source, target) {
        if (String(source.role) === String(target.role))
            return "Drag between an input and an output socket";
        var row = String(target.role) === "output" ? Number(target.row) : Number(source.row);
        if (!shuffleEditor.rowEnabled(row))
            return "Name output channel " + (row + 1) + " before mapping into it";
        return "";
    }

    function dropHint(source, target) {
        var rows = shuffleEditor.targetRows(source, target, shuffleEditor.dragMode());
        if (!rows || rows.length === 0)
            return "";
        return rows.length === 1 ? "Connect " + shuffleEditor.rowOutput(rows[0]) : "Connect " + rows.length + " channels";
    }

    // Consecutive routing stays in the destination group; Alt broadcasts to all
    // authored outputs. Either end may initiate a connection.
    function targetRows(source, target, mode) {
        var input = String(source.role) === "input" ? source : target;
        var output = String(source.role) === "output" ? source : target;
        if (String(input.role) !== "input" || String(output.role) !== "output")
            return null;
        var group = shuffleEditor.groupOf(output.row);
        var first = shuffleEditor.firstRowOf(group);
        var position = Number(input.slot);
        var slot = Number(output.slot);
        var rows = [];
        if (mode === "broadcast") {
            for (var index = 0; index < shuffleEditor.rowCount; ++index)
                rows.push(index);
        } else if (mode === "lower") {
            var channelCount = shuffleEditor.groupSourceChannels(input.group).length;
            for (var down = 0; position + down < channelCount && slot + down < shuffleEditor.rowsPerGroup; ++down)
                rows.push(first + slot + down);
        } else if (mode === "higher") {
            for (var up = 0; position - up >= 0 && slot - up >= 0; ++up)
                rows.push(first + slot - up);
        } else {
            rows.push(Number(output.row));
        }
        return rows;
    }

    function connectionValues(source, target, mode) {
        var input = String(source.role) === "input" ? source : target;
        var output = String(source.role) === "output" ? source : target;
        if (String(input.role) !== "input" || String(output.role) !== "output")
            return {};
        var sourceKind = shuffleEditor.inputChoiceKey(Number(input.group));
        var rows = shuffleEditor.targetRows(source, target, mode);
        if (!rows || rows.length === 0)
            return {};
        var values = {};
        var channels = shuffleEditor.groupSourceChannels(Number(input.group));
        for (var index = 0; index < rows.length; ++index) {
            var row = rows[index];
            if (!shuffleEditor.rowEnabled(row))
                continue;
            var sourceSlot = Number(input.slot) + (mode === "lower" ? index : mode === "higher" ? -index : 0);
            values["sourceKind" + row] = sourceKind;
            values["sourceChannel" + row] = mode === "lower" || mode === "higher"
                                            ? String(channels[sourceSlot]) : String(input.channel);
        }
        return values;
    }

    function connectSockets(source, target, mode) {
        shuffleEditor.gestureProblem = "";
        var values = shuffleEditor.connectionValues(source, target, mode);
        if (Object.keys(values).length === 0) {
            shuffleEditor.gestureProblem = shuffleEditor.invalidDropReason(source, target);
            return false;
        }
        return shuffleEditor.gestureValues(values);
    }

    // Order follows the visible socket sequence; name matching spans both
    // output groups and compares full names, not ambiguous leaves such as u/v.
    function autoConnectByOrder(input) {
        var position = Number(input.slot);
        for (var group = 0; group < Number(input.group); ++group)
            position += shuffleEditor.groupSourceChannels(group).length;
        for (var row = 0; row < shuffleEditor.rowCount; ++row) {
            if (!shuffleEditor.rowEnabled(row))
                continue;
            if (position-- !== 0)
                continue;
            return shuffleEditor.connectSockets(input, shuffleEditor.socketEntry("output", 0, "", row), "single");
        }
        shuffleEditor.gestureProblem = "No output socket at this channel's position";
        return false;
    }

    function autoConnectByName(input) {
        for (var row = 0; row < shuffleEditor.rowCount; ++row) {
            if (!shuffleEditor.rowEnabled(row) || shuffleEditor.rowOutput(row) !== String(input.channel))
                continue;
            return shuffleEditor.connectSockets(input, shuffleEditor.socketEntry("output", 0, "", row), "single");
        }
        shuffleEditor.gestureProblem = "No output channel named '" + input.channel + "'";
        return false;
    }

    function doubleClickSocket(entry, modifiers) {
        if (String(entry.role) !== "input")
            return false;
        shuffleEditor.gestureProblem = "";
        if (modifiers & Qt.ControlModifier)
            return shuffleEditor.autoConnectByName(entry);
        return shuffleEditor.autoConnectByOrder(entry);
    }

    // Auto-connect all channels (drag and drop): each of the input group's
    // channels connects to the output row at the same position.
    function autoConnectAll(inputGroup, outputGroup) {
        shuffleEditor.gestureProblem = "";
        var channels = shuffleEditor.groupSourceChannels(inputGroup);
        var values = {};
        for (var index = 0; index < channels.length && index < shuffleEditor.rowsPerGroup; ++index) {
            var row = shuffleEditor.firstRowOf(outputGroup) + index;
            if (!shuffleEditor.rowEnabled(row))
                continue;
            values["sourceKind" + row] = shuffleEditor.inputChoiceKey(inputGroup);
            values["sourceChannel" + row] = channels[index];
        }
        if (Object.keys(values).length === 0) {
            shuffleEditor.gestureProblem = "No output channel of this group is available to connect";
            return false;
        }
        return shuffleEditor.gestureValues(values);
    }

    // --- constants ----------------------------------------------------------
    function setConstant(row, kind) {
        shuffleEditor.gestureProblem = "";
        var values = {};
        values["sourceKind" + row] = kind;
        return shuffleEditor.gestureValues(values);
    }

    // Ctrl/Cmd+click sets every authored output channel to the same constant.
    function setAllConstants(kind) {
        shuffleEditor.gestureProblem = "";
        var values = {};
        for (var row = 0; row < shuffleEditor.rowCount; ++row) {
            if (!shuffleEditor.rowEnabled(row))
                continue;
            values["sourceKind" + row] = kind;
        }
        if (Object.keys(values).length === 0)
            return false;
        return shuffleEditor.gestureValues(values);
    }

    // --- group selectors ----------------------------------------------------
    // Source group references follow B/A changes directly. Layer changes rewrite
    // the rows attached to that source group, independent of destination group.
    function groupSelectionValues(group, field, value) {
        var values = {};
        var first = shuffleEditor.firstRowOf(group);
        var last = first + shuffleEditor.rowsPerGroup;
        if (field === "image") {
            values[shuffleEditor.inputChoiceKey(group)] = value;
            return values;
        }
        if (field === "inLayer") {
            values[shuffleEditor.inLayerKey(group)] = value;
            var described = shuffleEditor.portChannels(shuffleEditor.groupImage(group));
            for (var source = 0; source < shuffleEditor.rowCount; ++source) {
                if (shuffleEditor.rowInputGroup(source) !== group)
                    continue;
                if (value.length === 0) {
                    values["sourceKind" + source] = "zero";
                    continue;
                }
                var authored = shuffleEditor.rowSource(source);
                if (authored.length === 0)
                    continue;
                var leaf = shuffleEditor.leafOf(authored);
                values["sourceChannel" + source] = shuffleEditor.fullChannelName(value, leaf);
                for (var index = 0; index < described.length; ++index) {
                    var candidate = String(described[index]);
                    if (shuffleEditor.layerOf(candidate) !== value || shuffleEditor.leafOf(candidate) !== leaf)
                        continue;
                    values["sourceChannel" + source] = candidate;
                    break;
                }
            }
            return values;
        }
        values[shuffleEditor.outLayerKey(group)] = value;
        var layerChannels = shuffleEditor.portChannels("B").filter(function (name) {
            return shuffleEditor.layerOf(String(name)) === value;
        });
        for (var target = first; target < last; ++target) {
            var output = shuffleEditor.rowOutput(target);
            if (value.length === 0) {
                values["outputChannel" + target] = "";
                continue;
            }
            var slot = target - first;
            if (layerChannels.length > 0) {
                values["outputChannel" + target] = slot < layerChannels.length ? String(layerChannels[slot]) : "";
                if (output.length === 0 && slot < layerChannels.length)
                    values["sourceKind" + target] = "zero";
            } else if (output.length > 0) {
                values["outputChannel" + target] = shuffleEditor.fullChannelName(value, shuffleEditor.leafOf(output));
            }
        }
        return values;
    }

    function currentGroupValue(group, field) {
        if (field === "image")
            return shuffleEditor.groupImage(group);
        return field === "inLayer" ? shuffleEditor.inLayer(group) : shuffleEditor.outLayer(group);
    }

    function commitGroupSelection(group, field, value) {
        shuffleEditor.gestureProblem = "";
        var text = String(value);
        if (field !== "image" && text === "none")
            text = "";
        if (field === "image" && text !== "A" && text !== "B")
            return false;
        if (shuffleEditor.currentGroupValue(group, field) === text)
            return true;
        return shuffleEditor.gestureValues(shuffleEditor.groupSelectionValues(group, field, text));
    }

    function outputsUnique(values) {
        var seen = ({});
        for (var row = 0; row < shuffleEditor.rowCount; ++row) {
            var output = values["outputChannel" + row] !== undefined ? String(values["outputChannel" + row]) : shuffleEditor.rowOutput(row);
            if (output.length === 0)
                continue;
            if (seen[output] === true) {
                shuffleEditor.gestureProblem = "Output channel '" + output + "' is already mapped by another row";
                return false;
            }
            seen[output] = true;
        }
        return true;
    }

    // --- layer reordering ---------------------------------------------------
    // Reorder one side without changing pixels or the other side's order.
    function swapGroups(side) {
        var values = {};
        var pairs = side === "input" ? [["input1", "input2"], ["in1", "in2"]] : [["out1", "out2"]];
        for (var index = 0; index < pairs.length; ++index) {
            values[pairs[index][0]] = shuffleEditor.rowValueText(pairs[index][1]);
            values[pairs[index][1]] = shuffleEditor.rowValueText(pairs[index][0]);
        }
        if (side === "input") {
            for (var row = 0; row < shuffleEditor.rowCount; ++row) {
                var kind = shuffleEditor.rowSourceKind(row);
                if (kind === "input1" || kind === "input2")
                    values["sourceKind" + row] = kind === "input1" ? "input2" : "input1";
            }
        } else {
            for (var slot = 0; slot < shuffleEditor.rowsPerGroup; ++slot) {
                var second = slot + shuffleEditor.rowsPerGroup;
                values["sourceKind" + slot] = shuffleEditor.rowSourceKind(second);
                values["sourceChannel" + slot] = shuffleEditor.rowSource(second);
                values["outputChannel" + slot] = shuffleEditor.rowOutput(second);
                values["sourceKind" + second] = shuffleEditor.rowSourceKind(slot);
                values["sourceChannel" + second] = shuffleEditor.rowSource(slot);
                values["outputChannel" + second] = shuffleEditor.rowOutput(slot);
            }
        }
        return shuffleEditor.gestureValues(values);
    }

    // --- shared key and exposure affordances --------------------------------
    // A consumed parameter has no generic row left, so the editor states those
    // same cells itself through the SAME shared owners: the label cell carries
    // the exposure drag and the Alt-click hold-key gesture, and the key cell
    // owns Set/Update/Remove Key and Show in Animation. No key state, command or
    // exposure rule lives here.
    readonly property var cellTheme: ({
            "text": shuffleEditor.textColor,
            "muted": shuffleEditor.mutedColor,
            "accent": shuffleEditor.accentColor,
            "border": shuffleEditor.borderColor,
            "hover": shuffleEditor.hoverColor,
            "field": shuffleEditor.fieldColor,
            "panel": shuffleEditor.panelColor,
            "raised": shuffleEditor.raisedColor,
            "disabled": shuffleEditor.disabledColor,
            "errorText": shuffleEditor.errorColor,
            "smallRadius": shuffleEditor.smallRadiusValue,
            "fontSize": shuffleEditor.fontSizeValue
        })

    function keyStatusOf(key) {
        shuffleEditor.revision;
        shuffleEditor.frame;
        if (!shuffleEditor.panel || key.length === 0)
            return "none";
        return String(shuffleEditor.panel.parameterKeyStatusFor(shuffleEditor.networkId, shuffleEditor.nodeId, key));
    }

    function keyAtFrame(key) {
        if (!shuffleEditor.panel || key.length === 0)
            return false;
        return shuffleEditor.panel.keyParameterAtFrame(shuffleEditor.networkId, shuffleEditor.nodeId, key);
    }

    function removeKeyAtFrame(key) {
        if (!shuffleEditor.panel || key.length === 0)
            return false;
        return shuffleEditor.panel.removeParameterKeyAtFrame(shuffleEditor.networkId, shuffleEditor.nodeId, key);
    }

    function revealInAnimation(key) {
        if (!shuffleEditor.panel || !shuffleEditor.panel.revealInAnimation)
            return;
        shuffleEditor.panel.revealInAnimation(shuffleEditor.networkId, shuffleEditor.nodeId, key);
    }

    function revealAvailable() {
        return shuffleEditor.panel && shuffleEditor.panel.groupHasAnimationPanel ? shuffleEditor.panel.groupHasAnimationPanel() : false;
    }

    // --- the one shared gesture --------------------------------------------
    function gestureValues(values) {
        if (!shuffleEditor.panel || !shuffleEditor.panel.beginEditForMany)
            return false;
        if (Object.keys(values).length === 0)
            return false;
        if (!shuffleEditor.outputsUnique(values))
            return false;
        var keys = Object.keys(values);
        var token = String(shuffleEditor.panel.beginEditForMany(shuffleEditor.networkId, shuffleEditor.nodeId, keys));
        if (token.length === 0) {
            shuffleEditor.gestureProblem = shuffleEditor.controller ? String(shuffleEditor.controller.error) : "";
            return false;
        }
        if (shuffleEditor.panel.updateEditMany(token, values) !== true) {
            var message = shuffleEditor.controller ? String(shuffleEditor.controller.error) : "";
            shuffleEditor.panel.cancelEdit(token);
            shuffleEditor.gestureProblem = message;
            shuffleEditor.refresh();
            return false;
        }
        if (shuffleEditor.panel.commitEdit(token) !== true) {
            shuffleEditor.gestureProblem = shuffleEditor.controller ? String(shuffleEditor.controller.error) : "";
            shuffleEditor.refresh();
            return false;
        }
        shuffleEditor.gestureProblem = "";
        shuffleEditor.refresh();
        return true;
    }

    // --- new layer / channel dialog ----------------------------------------
    // Creation belongs to Out -> new. Existing output labels open routing/key
    // controls without exposing per-row channel creation or deletion.
    property var newChannelRequest: null
    property string newChannelProblem: ""

    function openNewChannel(group) {
        shuffleEditor.newChannelProblem = "";
        shuffleEditor.newChannelRequest = ({
            "kind": "output",
            "group": Number(group)
        });
        newLayerField.text = shuffleEditor.outLayer(group) || shuffleEditor.rootLayerName;
        newChannelField.text = "";
    }

    function openRouting(row) {
        shuffleEditor.newChannelProblem = "";
        shuffleEditor.newChannelRequest = ({
            "kind": "routing",
            "row": Number(row)
        });
    }

    function openNewSourceChannel(group) {
        shuffleEditor.newChannelProblem = "";
        shuffleEditor.newChannelRequest = ({
            "kind": "source",
            "row": -1,
            "group": Number(group),
            "layer": shuffleEditor.inLayer(group).length > 0 ? shuffleEditor.inLayer(group) : shuffleEditor.rootLayerName,
            "channel": ""
        });
        newLayerField.text = shuffleEditor.newChannelRequest.layer;
        newChannelField.text = shuffleEditor.newChannelRequest.channel;
    }

    function cancelNewChannel() {
        // Cancelling publishes nothing: the request is dropped and no gesture
        // was ever started for it.
        shuffleEditor.newChannelRequest = null;
        shuffleEditor.newChannelProblem = "";
    }

    function confirmNewChannel() {
        var request = shuffleEditor.newChannelRequest;
        if (!request)
            return;
        var layer = String(newLayerField.text).trim();
        var channel = String(newChannelField.text).trim();
        if (layer.length === 0 || channel.length === 0) {
            shuffleEditor.newChannelProblem = "Enter both a layer name and a channel name";
            return;
        }
        var name = shuffleEditor.fullChannelName(layer, channel);
        if (String(request.kind) === "source") {
            // A typed source channel is presentation state: it adds a socket the
            // artist can drag from, and it never claims the channel exists until
            // the described channels say so.
            var next = shuffleEditor.typedSources.slice();
            next.push({
                "group": Number(request.group),
                "image": shuffleEditor.groupImage(request.group),
                "channel": name
            });
            shuffleEditor.typedSources = next;
            shuffleEditor.cancelNewChannel();
            return;
        }
        var group = Number(request.group);
        var first = shuffleEditor.firstRowOf(group);
        var last = first + shuffleEditor.rowsPerGroup;
        var sameLayer = layer === shuffleEditor.outLayer(group);
        var row = first;
        if (sameLayer) {
            while (row < last && shuffleEditor.rowEnabled(row))
                ++row;
            if (row === last) {
                shuffleEditor.newChannelProblem = "This output group has four channels. Use the other Out group for another layer.";
                return;
            }
        }
        var values = {};
        if (!sameLayer) {
            for (var other = first; other < last; ++other)
                values["outputChannel" + other] = "";
        }
        values["outputChannel" + row] = name;
        values["sourceKind" + row] = "zero";
        values[shuffleEditor.outLayerKey(group)] = layer;
        if (!shuffleEditor.outputsUnique(values))
            return;
        if (!shuffleEditor.gestureValues(values))
            return;
        shuffleEditor.cancelNewChannel();
    }

    // --- group handle drag (reorder / connect all) --------------------------
    property var groupDragSource: null

    // --- group geometry -----------------------------------------------------
    // Each group registers its own rectangle, so a group-handle drop resolves the
    // group under the pointer from the item that draws it rather than from a
    // guessed division of the column.
    property var groupRegistry: []

    function registerGroup(entry) {
        var next = shuffleEditor.groupRegistry.slice();
        next.push(entry);
        shuffleEditor.groupRegistry = next;
    }

    function unregisterGroup(entry) {
        var next = [];
        for (var index = 0; index < shuffleEditor.groupRegistry.length; ++index) {
            if (shuffleEditor.groupRegistry[index] !== entry)
                next.push(shuffleEditor.groupRegistry[index]);
        }
        shuffleEditor.groupRegistry = next;
    }

    function groupEntryAt(point) {
        if (!point)
            return null;
        for (var index = 0; index < shuffleEditor.groupRegistry.length; ++index) {
            var entry = shuffleEditor.groupRegistry[index];
            var local = entry.item.mapFromItem(shuffleEditor, point.x, point.y);
            if (local.x < -4 || local.y < -4 || local.x > entry.item.width + 4 || local.y > entry.item.height + 4)
                continue;
            return entry;
        }
        return null;
    }

    function beginGroupDrag(side, group, point) {
        shuffleEditor.groupDragSource = ({ "side": String(side), "group": Number(group) });
        shuffleEditor.dragPoint = point;
        shuffleEditor.dragHint = "Drop on the sibling group to reorder, or on an output group to connect all channels";
    }

    function updateGroupDrag(point) {
        if (!shuffleEditor.groupDragSource)
            return;
        shuffleEditor.dragPoint = point;
    }

    function endGroupDrag(committed, point) {
        var source = shuffleEditor.groupDragSource;
        shuffleEditor.groupDragSource = null;
        shuffleEditor.dragPoint = null;
        shuffleEditor.dragHint = "";
        if (!source || !committed)
            return;
        // A drop on a socket of the opposite side connects all channels into that
        // output group; a drop on the other group's own area reorders the layers.
        var hoveredSocket = shuffleEditor.socketAt(point);
        if (hoveredSocket && String(hoveredSocket.role) !== String(source.side)) {
            if (String(source.side) === "input")
                shuffleEditor.autoConnectAll(Number(source.group), shuffleEditor.groupOf(hoveredSocket.row));
            else
                shuffleEditor.autoConnectAll(Number(hoveredSocket.group), Number(source.group));
            return;
        }
        var hoveredGroup = shuffleEditor.groupEntryAt(point);
        if (!hoveredGroup)
            return;
        if (String(hoveredGroup.side) !== String(source.side)) {
            if (String(source.side) === "input")
                shuffleEditor.autoConnectAll(Number(source.group), Number(hoveredGroup.group));
            else
                shuffleEditor.autoConnectAll(Number(hoveredGroup.group), Number(source.group));
            return;
        }
        if (Number(hoveredGroup.group) === Number(source.group))
            return;
        shuffleEditor.swapGroups(String(source.side));
    }

    // --- new layer / channel dialog ----------------------------------------
    // Shared inline panel: layer-menu creation or an existing row's routing.
    // Cancelling a creation draft publishes nothing.
    Rectangle {
        id: newChannelPanel
        objectName: "shuffleNewChannel_" + shuffleEditor.nodeId
        Layout.fillWidth: true
        visible: shuffleEditor.newChannelRequest !== null
        implicitHeight: newChannelColumn.implicitHeight + 8
        color: shuffleEditor.raisedColor
        border.color: shuffleEditor.borderColor
        radius: shuffleEditor.smallRadiusValue

        ColumnLayout {
            id: newChannelColumn
            anchors.fill: parent
            anchors.margins: 4
            spacing: 4

            Text {
                Layout.fillWidth: true
                text: shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) === "routing"
                    ? "Routing: " + shuffleEditor.rowOutput(shuffleEditor.newChannelRequest.row)
                    : shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) === "source" ? "New source channel" : "New output channel"
                color: shuffleEditor.textColor
                font.pixelSize: shuffleEditor.fontSizeValue
            }

            RowLayout {
                Layout.fillWidth: true
                visible: shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) !== "routing"
                spacing: 4
                Text {
                    text: "Name"
                    color: shuffleEditor.mutedColor
                    font.pixelSize: shuffleEditor.smallFontSize
                    Layout.preferredWidth: 52
                }
                TextField {
                    id: newLayerField
                    objectName: "shuffleNewLayer_" + shuffleEditor.nodeId
                    Layout.fillWidth: true
                    implicitHeight: 22
                    font.pixelSize: shuffleEditor.smallFontSize
                    color: shuffleEditor.textColor
                    selectByMouse: true
                    Accessible.name: "Layer name"
                    Keys.onEscapePressed: function (event) {
                        event.accepted = true;
                        shuffleEditor.cancelNewChannel();
                    }
                    background: Rectangle {
                        color: shuffleEditor.fieldColor
                        border.color: newLayerField.activeFocus ? shuffleEditor.accentColor : shuffleEditor.borderColor
                        radius: shuffleEditor.smallRadiusValue
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) !== "routing"
                spacing: 4
                Text {
                    text: "Channels"
                    color: shuffleEditor.mutedColor
                    font.pixelSize: shuffleEditor.smallFontSize
                    Layout.preferredWidth: 52
                }
                TextField {
                    id: newChannelField
                    objectName: "shuffleNewChannelField_" + shuffleEditor.nodeId
                    Layout.fillWidth: true
                    implicitHeight: 22
                    font.pixelSize: shuffleEditor.smallFontSize
                    color: shuffleEditor.textColor
                    selectByMouse: true
                    Accessible.name: "Channel name"
                    onAccepted: shuffleEditor.confirmNewChannel()
                    Keys.onEscapePressed: function (event) {
                        event.accepted = true;
                        shuffleEditor.cancelNewChannel();
                    }
                    background: Rectangle {
                        color: shuffleEditor.fieldColor
                        border.color: newChannelField.activeFocus ? shuffleEditor.accentColor : shuffleEditor.borderColor
                        radius: shuffleEditor.smallRadiusValue
                    }
                }
            }

            Repeater {
                model: shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) === "routing"
                       ? ["sourceKind" + shuffleEditor.newChannelRequest.row,
                          "sourceChannel" + shuffleEditor.newChannelRequest.row] : []
                delegate: RowLayout {
                    id: routingField
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: 4
                    ExposureLabel {
                        objectName: "shuffleRoutingLabel_" + routingField.modelData + "_" + shuffleEditor.nodeId
                        Layout.preferredWidth: 100
                        theme: shuffleEditor.cellTheme
                        networkId: shuffleEditor.networkId
                        instanceId: shuffleEditor.instanceId
                        nodeId: shuffleEditor.nodeId
                        parameterKey: routingField.modelData
                        labelText: routingField.modelData.indexOf("sourceKind") === 0 ? "Source group" : "Source channel"
                        keyStatus: shuffleEditor.keyStatusOf(routingField.modelData)
                        frame: shuffleEditor.frame
                        onKeyRequested: shuffleEditor.keyAtFrame(routingField.modelData)
                    }
                    Text {
                        Layout.fillWidth: true
                        objectName: "shuffleRoutingValue_" + routingField.modelData + "_" + shuffleEditor.nodeId
                        text: shuffleEditor.rowValueText(routingField.modelData)
                        color: shuffleEditor.textColor
                        font.pixelSize: shuffleEditor.smallFontSize
                        elide: Text.ElideMiddle
                    }
                    KeyIndicator {
                        objectName: "shuffleRoutingKey_" + routingField.modelData + "_" + shuffleEditor.nodeId
                        theme: shuffleEditor.cellTheme
                        networkId: shuffleEditor.networkId
                        nodeId: shuffleEditor.nodeId
                        parameterKey: routingField.modelData
                        parameterLabel: routingField.modelData
                        keyStatus: shuffleEditor.keyStatusOf(routingField.modelData)
                        scope: ""
                        frame: shuffleEditor.frame
                        revealAvailable: shuffleEditor.revealAvailable()
                        onKeyRequested: shuffleEditor.keyAtFrame(routingField.modelData)
                        onRemoveKeyRequested: shuffleEditor.removeKeyAtFrame(routingField.modelData)
                        onRevealRequested: shuffleEditor.revealInAnimation(routingField.modelData)
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: shuffleEditor.newChannelProblem.length > 0
                text: shuffleEditor.newChannelProblem
                color: shuffleEditor.errorColor
                font.pixelSize: shuffleEditor.smallFontSize
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Item {
                    Layout.fillWidth: true
                }
                Button {
                    id: newChannelCancel
                    objectName: "shuffleNewCancel_" + shuffleEditor.nodeId
                    implicitWidth: 60
                    implicitHeight: 22
                    padding: 0
                    text: shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) === "routing" ? "Close" : "Cancel"
                    onClicked: shuffleEditor.cancelNewChannel()
                    contentItem: Text {
                        text: newChannelCancel.text
                        color: shuffleEditor.textColor
                        font.pixelSize: shuffleEditor.smallFontSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: newChannelCancel.hovered ? shuffleEditor.hoverColor : "transparent"
                        border.color: shuffleEditor.borderColor
                        radius: shuffleEditor.smallRadiusValue
                    }
                }
                Button {
                    id: newChannelConfirm
                    objectName: "shuffleNewConfirm_" + shuffleEditor.nodeId
                    visible: shuffleEditor.newChannelRequest && String(shuffleEditor.newChannelRequest.kind) !== "routing"
                    implicitWidth: 60
                    implicitHeight: 22
                    padding: 0
                    text: "OK"
                    onClicked: shuffleEditor.confirmNewChannel()
                    contentItem: Text {
                        text: newChannelConfirm.text
                        color: shuffleEditor.textColor
                        font.pixelSize: shuffleEditor.smallFontSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: newChannelConfirm.hovered ? shuffleEditor.hoverColor : "transparent"
                        border.color: shuffleEditor.accentColor
                        radius: shuffleEditor.smallRadiusValue
                    }
                }
            }
        }
    }

    // --- presentation -------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 0

        Text {
            text: "Input Layer"
            color: shuffleEditor.mutedColor
            font.pixelSize: shuffleEditor.fontSizeValue
            Layout.preferredWidth: shuffleEditor.inputColumnWidth
        }
        Text {
            text: "Output Layer"
            color: shuffleEditor.mutedColor
            font.pixelSize: shuffleEditor.fontSizeValue
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
    }

    // The socket area keeps the width its drop targets need instead of shrinking
    // them, and scrolls horizontally inside the card when the card is narrower
    // than that (the reference panel does the same). The flickable accepts only
    // horizontal drags, so the inspector body keeps owning vertical scrolling.
    Flickable {
        id: socketScroll
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(inputGroups.implicitHeight, outputGroups.implicitHeight) + 4
        contentWidth: socketArea.width
        contentHeight: height
        clip: true
        interactive: contentWidth > width
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        Item {
            id: socketArea
            width: Math.max(socketScroll.width, shuffleEditor.inputColumnWidth + shuffleEditor.wireGap + shuffleEditor.outputColumnWidth)
            height: socketScroll.height

            Canvas {
                id: wires
                anchors.fill: parent
                z: -1
                onPaint: shuffleEditor.paintWires(wires)
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Connections {
                    target: shuffleEditor
                    function onSocketEpochChanged() {
                        wires.requestPaint();
                    }
                    function onChannelAnswerEpochChanged() {
                        wires.requestPaint();
                    }
                }
            }

            ColumnLayout {
                id: inputGroups
                x: 0
                y: 0
                width: shuffleEditor.inputColumnWidth
                spacing: 6

                Repeater {
                    model: [0, 1]
                    delegate: Loader {
                        id: inputGroupLoader
                        required property int index
                        Layout.fillWidth: true
                        sourceComponent: inputGroupComponent
                        onLoaded: {
                            item.group = index;
                            item.side = "input";
                            item.register();
                        }
                    }
                }
            }

            ColumnLayout {
                id: outputGroups
                x: shuffleEditor.inputColumnWidth + shuffleEditor.wireGap
                y: 0
                width: shuffleEditor.outputColumnWidth
                spacing: 6

                Repeater {
                    model: [0, 1]
                    delegate: Loader {
                        id: outputGroupLoader
                        required property int index
                        Layout.fillWidth: true
                        sourceComponent: outputGroupComponent
                        onLoaded: {
                            item.group = index;
                            item.side = "output";
                            item.register();
                        }
                    }
                }
            }
        }
    }

    Text {
        objectName: "shuffleProblem_" + shuffleEditor.nodeId
        Layout.fillWidth: true
        visible: shuffleEditor.gestureProblem.length > 0 || shuffleEditor.schemaProblem.length > 0
        text: shuffleEditor.gestureProblem.length > 0 ? shuffleEditor.gestureProblem : shuffleEditor.schemaProblem
        color: shuffleEditor.errorColor
        font.pixelSize: shuffleEditor.smallFontSize
        wrapMode: Text.WordWrap
        Accessible.name: text
    }

    Text {
        objectName: "shuffleHint_" + shuffleEditor.nodeId
        Layout.fillWidth: true
        visible: shuffleEditor.dragHint.length > 0 && shuffleEditor.gestureProblem.length === 0
        text: shuffleEditor.dragHint
        color: shuffleEditor.mutedColor
        font.pixelSize: shuffleEditor.smallFontSize
        elide: Text.ElideRight
    }

    // --- one socket ---------------------------------------------------------
    // The socket owns geometry and the drag that authors a mapping; the model it
    // reports lives in the rows above, so nothing here can drift from them.
    Component {
        id: socketComponent
        Rectangle {
            id: socket
            property string socketRole: "input"
            property int socketGroup: 0
            property int socketSlot: 0
            property string socketChannel: ""
            property int socketRow: -1

            readonly property bool outputSide: socket.socketRole === "output"
            readonly property string kind: socket.outputSide && socket.socketRow >= 0 ? shuffleEditor.rowKind(socket.socketRow) : "B"
            readonly property string source: socket.outputSide && socket.socketRow >= 0 ? shuffleEditor.rowSource(socket.socketRow) : ""
            readonly property bool wired: socket.outputSide && socket.source.length > 0 && socket.kind !== "zero" && socket.kind !== "one" && shuffleEditor.sourceSocket(socket.socketRow) !== null

            implicitWidth: shuffleEditor.socketSize
            implicitHeight: shuffleEditor.socketSize
            width: shuffleEditor.socketSize
            height: shuffleEditor.socketSize
            radius: width / 2
            border.width: 1
            objectName: socket.outputSide
                       ? "shuffleOutSocket_" + socket.socketRow + "_" + shuffleEditor.nodeId
                       : "shuffleInSocket_" + socket.socketGroup + "_" + socket.socketSlot + "_" + shuffleEditor.nodeId
            color: {
                if (socket.kind === "zero")
                    return "#000000";
                if (socket.kind === "one")
                    return "#ffffff";
                if (socket.outputSide && !socket.wired)
                    return "transparent";
                return shuffleEditor.channelColor(socket.outputSide ? socket.source : socket.socketChannel);
            }
            border.color: socket.outputSide && !socket.wired ? shuffleEditor.mutedColor : shuffleEditor.borderColor
            onXChanged: shuffleEditor.scheduleWirePaint()
            onYChanged: shuffleEditor.scheduleWirePaint()

            property var entry: ({
                "id": socket.socketRole + ":" + socket.socketGroup + ":" + socket.socketSlot + ":" + socket.socketChannel,
                "role": socket.socketRole,
                "group": socket.socketGroup,
                "slot": socket.socketSlot,
                "channel": socket.socketChannel,
                "row": socket.socketRow,
                "item": socket
            })

            MouseArea {
                id: socketMouse
                anchors.fill: parent
                anchors.margins: -3
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                activeFocusOnTab: false
                preventStealing: true
                property bool dragging: false
                property point pressPoint: Qt.point(0, 0)

                function editorPoint(mouse) {
                    // The MouseArea is the item the event coordinates belong to
                    // (its margins make it larger than the socket).
                    return socketMouse.mapToItem(shuffleEditor, mouse.x, mouse.y);
                }

                onPressed: function (mouse) {
                    socketMouse.forceActiveFocus(Qt.MouseFocusReason);
                    shuffleEditor.lastModifiers = mouse.modifiers;
                    socketMouse.pressPoint = socketMouse.editorPoint(mouse);
                    socketMouse.dragging = false;
                }
                onPositionChanged: function (mouse) {
                    if (!(mouse.buttons & Qt.LeftButton))
                        return;
                    shuffleEditor.lastModifiers = mouse.modifiers;
                    var point = socketMouse.editorPoint(mouse);
                    if (!socketMouse.dragging) {
                        if (Math.abs(point.x - socketMouse.pressPoint.x) < shuffleEditor.dragThreshold &&
                            Math.abs(point.y - socketMouse.pressPoint.y) < shuffleEditor.dragThreshold)
                            return;
                        socketMouse.dragging = true;
                        shuffleEditor.beginSocketDrag(socket.entry, point);
                    }
                    shuffleEditor.updateSocketDrag(point);
                }
                onReleased: function (mouse) {
                    if (!socketMouse.dragging)
                        return;
                    socketMouse.dragging = false;
                    shuffleEditor.lastModifiers = mouse.modifiers;
                    shuffleEditor.updateSocketDrag(socketMouse.editorPoint(mouse));
                    shuffleEditor.endSocketDrag(true);
                }
                onCanceled: {
                    if (!socketMouse.dragging)
                        return;
                    socketMouse.dragging = false;
                    shuffleEditor.endSocketDrag(false);
                }
                onDoubleClicked: function (mouse) {
                    shuffleEditor.doubleClickSocket(socket.entry, mouse.modifiers);
                }
                // Escape cancels a live drag without publishing anything.
                Keys.onEscapePressed: function (event) {
                    event.accepted = true;
                    if (!socketMouse.dragging)
                        return;
                    socketMouse.dragging = false;
                    shuffleEditor.endSocketDrag(false);
                }
            }

            ToolTip.visible: socketMouse.containsMouse
            ToolTip.text: {
                if (!socket.outputSide)
                    return socket.socketChannel + " — drag to an output socket, double-click to auto-connect";
                if (socket.socketRow < 0 || !shuffleEditor.rowEnabled(socket.socketRow))
                    return "Empty output slot — name it to create the channel";
                if (socket.kind === "zero")
                    return shuffleEditor.rowOutput(socket.socketRow) + " = 0 (black / transparent / zero data)";
                if (socket.kind === "one")
                    return shuffleEditor.rowOutput(socket.socketRow) + " = 1 (white / opaque / one data)";
                if (!socket.wired)
                    return shuffleEditor.rowOutput(socket.socketRow) + " — source '" + socket.source + "' is not available; the output becomes zero";
                return shuffleEditor.rowOutput(socket.socketRow) + " ← " + socket.source;
            }

            // Registration happens after the loader has stated the socket's
            // identity (onLoaded), never at Component.onCompleted, which runs
            // before those properties exist.
            property bool registered: false

            function register() {
                if (socket.registered)
                    return;
                socket.registered = true;
                shuffleEditor.registerSocket(socket);
            }

            Component.onDestruction: if (socket.registered)
                shuffleEditor.unregisterSocket(socket)
        }
    }

    // --- input group (rows 0-3 / 4-7) --------------------------------------
    Component {
        id: inputGroupComponent
        Rectangle {
            id: inputGroup
            property int group: 0
            property string side: "input"

            readonly property string image: shuffleEditor.groupImage(inputGroup.group)
            readonly property string reason: shuffleEditor.portReason(inputGroup.image)

            Layout.fillWidth: true
            implicitHeight: inputColumn.implicitHeight + 6
            color: shuffleEditor.panelColor
            border.color: shuffleEditor.borderColor
            radius: shuffleEditor.smallRadiusValue
            property var groupEntry: null

            function register() {
                if (inputGroup.groupEntry)
                    return;
                inputGroup.groupEntry = ({
                    "side": "input",
                    "group": inputGroup.group,
                    "item": inputGroup
                });
                shuffleEditor.registerGroup(inputGroup.groupEntry);
            }

            Component.onDestruction: if (inputGroup.groupEntry)
                shuffleEditor.unregisterGroup(inputGroup.groupEntry)

            ColumnLayout {
                id: inputColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 3
                spacing: 1

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    // The group's own selector key ("in1"/"in2") keeps the shared
                    // label cell: exposure drag and the Alt-click hold-key gesture
                    // are the ones every other parameter row uses.
                    ExposureLabel {
                        id: inLabel
                        objectName: "shuffleInLabel_" + inputGroup.group + "_" + shuffleEditor.nodeId
                        Layout.preferredWidth: 22
                        Layout.alignment: Qt.AlignVCenter
                        theme: shuffleEditor.cellTheme
                        networkId: shuffleEditor.networkId
                        instanceId: shuffleEditor.instanceId
                        nodeId: shuffleEditor.nodeId
                        parameterKey: shuffleEditor.inLayerKey(inputGroup.group)
                        labelText: "In"
                        keyStatus: shuffleEditor.keyStatusOf(shuffleEditor.inLayerKey(inputGroup.group))
                        frame: shuffleEditor.frame
                        onKeyRequested: shuffleEditor.keyAtFrame(shuffleEditor.inLayerKey(inputGroup.group))
                    }

                    StudioComboBox {
                        id: imageBox
                        objectName: "shuffleInputChoice_" + inputGroup.group + "_" + shuffleEditor.nodeId
                        theme: shuffleEditor.theme
                        Layout.preferredWidth: 52
                        implicitHeight: 21
                        model: shuffleEditor.imageChoices(inputGroup.group)
                        currentIndex: Math.max(0, model.indexOf(inputGroup.image))
                        Accessible.name: "Input " + (inputGroup.group + 1) + " image"
                        onActivated: shuffleEditor.commitGroupSelection(inputGroup.group, "image", currentText)
                        // Alt-click is the shared hold-key gesture for this
                        // selector; the plain click stays the choice.
                        MouseArea {
                            anchors.fill: parent
                            onPressed: function (mouse) {
                                mouse.accepted = !!(mouse.modifiers & Qt.AltModifier);
                            }
                            onClicked: shuffleEditor.keyAtFrame(shuffleEditor.inputChoiceKey(inputGroup.group))
                        }
                    }

                    StudioComboBox {
                        id: inLayerBox
                        objectName: "shuffleInLayer_" + inputGroup.group + "_" + shuffleEditor.nodeId
                        theme: shuffleEditor.theme
                        Layout.fillWidth: true
                        implicitHeight: 21
                        typeable: true
                        model: shuffleEditor.inputLayerNames(inputGroup.group)
                        readout: shuffleEditor.inLayer(inputGroup.group) || "none"
                        Accessible.name: "Input " + (inputGroup.group + 1) + " layer"
                        onActivated: shuffleEditor.commitGroupSelection(inputGroup.group, "inLayer", currentText)
                        onTextAccepted: function (text) {
                            shuffleEditor.commitGroupSelection(inputGroup.group, "inLayer", text);
                        }
                    }

                    // Layer reordering: drop on the sibling group to reorder, or
                    // on an output group to connect all channels by order.
                    Rectangle {
                        id: inHandle
                        objectName: "shuffleGroupHandle_" + inputGroup.group + "_input_" + shuffleEditor.nodeId
                        Layout.preferredWidth: 12
                        Layout.preferredHeight: 18
                        color: inHandleMouse.containsMouse ? shuffleEditor.hoverColor : "transparent"
                        radius: shuffleEditor.smallRadiusValue
                        Accessible.name: "Reorder input group " + (inputGroup.group + 1)
                        Text {
                            anchors.centerIn: parent
                            text: "\u2630"
                            color: shuffleEditor.mutedColor
                            font.pixelSize: shuffleEditor.smallFontSize
                        }
                        MouseArea {
                            id: inHandleMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.SizeAllCursor
                            preventStealing: true
                            property bool dragging: false
                            property point pressPoint: Qt.point(0, 0)
                            onPressed: function (mouse) {
                                inHandleMouse.pressPoint = inHandle.mapToItem(shuffleEditor, mouse.x, mouse.y);
                                inHandleMouse.dragging = false;
                            }
                            onPositionChanged: function (mouse) {
                                if (!(mouse.buttons & Qt.LeftButton))
                                    return;
                                var point = inHandle.mapToItem(shuffleEditor, mouse.x, mouse.y);
                                if (!inHandleMouse.dragging) {
                                    if (Math.abs(point.x - inHandleMouse.pressPoint.x) < shuffleEditor.dragThreshold &&
                                        Math.abs(point.y - inHandleMouse.pressPoint.y) < shuffleEditor.dragThreshold)
                                        return;
                                    inHandleMouse.dragging = true;
                                    shuffleEditor.beginGroupDrag("input", inputGroup.group, point);
                                }
                                shuffleEditor.updateGroupDrag(point);
                            }
                            onReleased: function (mouse) {
                                if (!inHandleMouse.dragging)
                                    return;
                                inHandleMouse.dragging = false;
                                shuffleEditor.endGroupDrag(true, inHandle.mapToItem(shuffleEditor, mouse.x, mouse.y));
                            }
                            onCanceled: {
                                if (!inHandleMouse.dragging)
                                    return;
                                inHandleMouse.dragging = false;
                                shuffleEditor.endGroupDrag(false, null);
                            }
                            Keys.onEscapePressed: function (event) {
                                event.accepted = true;
                                if (!inHandleMouse.dragging)
                                    return;
                                inHandleMouse.dragging = false;
                                shuffleEditor.endGroupDrag(false, null);
                            }
                        }
                    }
                }

                Repeater {
                    model: shuffleEditor.groupSourceChannels(inputGroup.group)

                    delegate: RowLayout {
                        id: inputSocketRow
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true
                        spacing: 3

                        Text {
                            Layout.fillWidth: true
                            text: inputSocketRow.modelData
                            color: shuffleEditor.textColor
                            font.pixelSize: shuffleEditor.smallFontSize
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideLeft
                            Accessible.name: inputSocketRow.modelData
                        }
                        Loader {
                            id: inputSocketLoader
                            Layout.alignment: Qt.AlignVCenter
                            sourceComponent: socketComponent
                            onLoaded: {
                                item.socketRole = "input";
                                item.socketGroup = inputGroup.group;
                                item.socketSlot = inputSocketRow.index;
                                item.socketChannel = inputSocketRow.modelData;
                                item.socketRow = -1;
                                item.register();
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: shuffleEditor.groupSourceChannels(inputGroup.group).length === 0
                    text: shuffleEditor.inLayer(inputGroup.group).length === 0 ? "Choose an input layer for this group" : (inputGroup.reason.length > 0 ? inputGroup.reason : "No channel of layer '" + shuffleEditor.inLayer(inputGroup.group) + "' is available")
                    color: shuffleEditor.mutedColor
                    font.pixelSize: shuffleEditor.smallFontSize
                    wrapMode: Text.WordWrap
                }

                // Recovery: a channel typed by the artist becomes a draggable
                // socket even when nothing can be described.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Button {
                        id: addSourceButton
                        objectName: "shuffleAddSource_" + inputGroup.group + "_" + shuffleEditor.nodeId
                        Layout.fillWidth: true
                        implicitHeight: 18
                        padding: 0
                        flat: true
                        text: "+ channel"
                        Accessible.name: "Add a source channel to input group " + (inputGroup.group + 1)
                        onClicked: shuffleEditor.openNewSourceChannel(inputGroup.group)
                        contentItem: Text {
                            text: addSourceButton.text
                            color: shuffleEditor.mutedColor
                            font.pixelSize: shuffleEditor.smallFontSize
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: addSourceButton.hovered ? shuffleEditor.hoverColor : "transparent"
                            radius: shuffleEditor.smallRadiusValue
                        }
                    }
                }
            }
        }
    }

    // --- output group (rows 0-3 / 4-7) -------------------------------------
    Component {
        id: outputGroupComponent
        Rectangle {
            id: outputGroup
            property int group: 0
            property string side: "output"

            Layout.fillWidth: true
            implicitHeight: outputColumn.implicitHeight + 6
            color: shuffleEditor.panelColor
            border.color: shuffleEditor.borderColor
            radius: shuffleEditor.smallRadiusValue
            property var groupEntry: null

            function register() {
                if (outputGroup.groupEntry)
                    return;
                outputGroup.groupEntry = ({
                    "side": "output",
                    "group": outputGroup.group,
                    "item": outputGroup
                });
                shuffleEditor.registerGroup(outputGroup.groupEntry);
            }

            Component.onDestruction: if (outputGroup.groupEntry)
                shuffleEditor.unregisterGroup(outputGroup.groupEntry)

            ColumnLayout {
                id: outputColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 3
                spacing: 1

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    // The group's own selector key ("out1"/"out2") keeps the
                    // shared label cell, exactly like the input group's.
                    ExposureLabel {
                        id: outLabel
                        objectName: "shuffleOutLabel_" + outputGroup.group + "_" + shuffleEditor.nodeId
                        Layout.preferredWidth: 26
                        Layout.alignment: Qt.AlignVCenter
                        theme: shuffleEditor.cellTheme
                        networkId: shuffleEditor.networkId
                        instanceId: shuffleEditor.instanceId
                        nodeId: shuffleEditor.nodeId
                        parameterKey: shuffleEditor.outLayerKey(outputGroup.group)
                        labelText: "Out"
                        keyStatus: shuffleEditor.keyStatusOf(shuffleEditor.outLayerKey(outputGroup.group))
                        frame: shuffleEditor.frame
                        onKeyRequested: shuffleEditor.keyAtFrame(shuffleEditor.outLayerKey(outputGroup.group))
                    }

                    StudioComboBox {
                        id: outLayerBox
                        objectName: "shuffleOutLayer_" + outputGroup.group + "_" + shuffleEditor.nodeId
                        theme: shuffleEditor.theme
                        Layout.fillWidth: true
                        implicitHeight: 21
                        typeable: true
                        model: shuffleEditor.outputLayerNames(outputGroup.group)
                        readout: shuffleEditor.outLayer(outputGroup.group) || "none"
                        Accessible.name: "Output " + (outputGroup.group + 1) + " layer"
                        onActivated: function (index) {
                            if (index === model.length - 1) {
                                shuffleEditor.openNewChannel(outputGroup.group);
                                outLayerBox.syncReadout();
                            } else {
                                shuffleEditor.commitGroupSelection(outputGroup.group, "outLayer", currentText);
                            }
                        }
                        onTextAccepted: function (text) {
                            shuffleEditor.commitGroupSelection(outputGroup.group, "outLayer", text);
                        }
                    }

                    Rectangle {
                        id: outHandle
                        objectName: "shuffleGroupHandle_" + outputGroup.group + "_output_" + shuffleEditor.nodeId
                        Layout.preferredWidth: 12
                        Layout.preferredHeight: 18
                        color: outHandleMouse.containsMouse ? shuffleEditor.hoverColor : "transparent"
                        radius: shuffleEditor.smallRadiusValue
                        Accessible.name: "Reorder output group " + (outputGroup.group + 1)
                        Text {
                            anchors.centerIn: parent
                            text: "\u2630"
                            color: shuffleEditor.mutedColor
                            font.pixelSize: shuffleEditor.smallFontSize
                        }
                        MouseArea {
                            id: outHandleMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.SizeAllCursor
                            preventStealing: true
                            property bool dragging: false
                            property point pressPoint: Qt.point(0, 0)
                            onPressed: function (mouse) {
                                outHandleMouse.pressPoint = outHandle.mapToItem(shuffleEditor, mouse.x, mouse.y);
                                outHandleMouse.dragging = false;
                            }
                            onPositionChanged: function (mouse) {
                                if (!(mouse.buttons & Qt.LeftButton))
                                    return;
                                var point = outHandle.mapToItem(shuffleEditor, mouse.x, mouse.y);
                                if (!outHandleMouse.dragging) {
                                    if (Math.abs(point.x - outHandleMouse.pressPoint.x) < shuffleEditor.dragThreshold &&
                                        Math.abs(point.y - outHandleMouse.pressPoint.y) < shuffleEditor.dragThreshold)
                                        return;
                                    outHandleMouse.dragging = true;
                                    shuffleEditor.beginGroupDrag("output", outputGroup.group, point);
                                }
                                shuffleEditor.updateGroupDrag(point);
                            }
                            onReleased: function (mouse) {
                                if (!outHandleMouse.dragging)
                                    return;
                                outHandleMouse.dragging = false;
                                shuffleEditor.endGroupDrag(true, outHandle.mapToItem(shuffleEditor, mouse.x, mouse.y));
                            }
                            onCanceled: {
                                if (!outHandleMouse.dragging)
                                    return;
                                outHandleMouse.dragging = false;
                                shuffleEditor.endGroupDrag(false, null);
                            }
                            Keys.onEscapePressed: function (event) {
                                event.accepted = true;
                                if (!outHandleMouse.dragging)
                                    return;
                                outHandleMouse.dragging = false;
                                shuffleEditor.endGroupDrag(false, null);
                            }
                        }
                    }
                }

                Repeater {
                    model: shuffleEditor.rowsPerGroup

                    delegate: RowLayout {
                        id: outputRowItem
                        required property int index
                        readonly property int row: shuffleEditor.firstRowOf(outputGroup.group) + index
                        Layout.fillWidth: true
                        spacing: 2

                        Loader {
                            id: outputSocketLoader
                            Layout.alignment: Qt.AlignVCenter
                            sourceComponent: socketComponent
                            onLoaded: {
                                item.socketRole = "output";
                                item.socketGroup = Qt.binding(function () { return outputGroup.group; });
                                item.socketSlot = outputRowItem.index;
                                item.socketChannel = "";
                                item.socketRow = Qt.binding(function () { return outputRowItem.row; });
                                item.register();
                            }
                        }

                        // Per-output constants: black (0) and white (1). A
                        // Ctrl/Cmd+click applies the constant to every authored
                        // output channel, as documented.
                        Button {
                            id: zeroButton
                            objectName: "shuffleZero_" + outputRowItem.row + "_" + shuffleEditor.nodeId
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 17
                            padding: 0
                            enabled: shuffleEditor.rowEnabled(outputRowItem.row)
                            text: "0"
                            Accessible.name: shuffleEditor.rowOutput(outputRowItem.row) + " set to zero"
                            onClicked: shuffleEditor.setConstant(outputRowItem.row, "zero")
                            contentItem: Text {
                                text: zeroButton.text
                                color: zeroButton.enabled ? shuffleEditor.textColor : shuffleEditor.disabledColor
                                font.pixelSize: shuffleEditor.smallFontSize
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: shuffleEditor.rowKind(outputRowItem.row) === "zero" ? "#000000" : (zeroButton.hovered ? shuffleEditor.hoverColor : "transparent")
                                border.color: shuffleEditor.rowKind(outputRowItem.row) === "zero" ? shuffleEditor.textColor : shuffleEditor.borderColor
                                radius: shuffleEditor.smallRadiusValue
                            }
                            // Ctrl/Cmd+click applies the constant to every
                            // authored output channel; Alt-click is the shared
                            // hold-key gesture for this row's source kind. The
                            // press is taken only while a modifier is held, so a
                            // plain click stays the one-row constant.
                            MouseArea {
                                anchors.fill: parent
                                enabled: zeroButton.enabled
                                onPressed: function (mouse) {
                                    mouse.accepted = !!(mouse.modifiers & (Qt.ControlModifier | Qt.AltModifier));
                                }
                                onClicked: function (mouse) {
                                    if (mouse.modifiers & Qt.AltModifier)
                                        shuffleEditor.keyAtFrame("sourceKind" + outputRowItem.row);
                                    else
                                        shuffleEditor.setAllConstants("zero");
                                }
                            }
                        }

                        Button {
                            id: oneButton
                            objectName: "shuffleOne_" + outputRowItem.row + "_" + shuffleEditor.nodeId
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 17
                            padding: 0
                            enabled: shuffleEditor.rowEnabled(outputRowItem.row)
                            text: "1"
                            Accessible.name: shuffleEditor.rowOutput(outputRowItem.row) + " set to one"
                            onClicked: shuffleEditor.setConstant(outputRowItem.row, "one")
                            contentItem: Text {
                                text: oneButton.text
                                color: shuffleEditor.rowKind(outputRowItem.row) === "one" ? "#101010" : (oneButton.enabled ? shuffleEditor.textColor : shuffleEditor.disabledColor)
                                font.pixelSize: shuffleEditor.smallFontSize
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: shuffleEditor.rowKind(outputRowItem.row) === "one" ? "#ffffff" : (oneButton.hovered ? shuffleEditor.hoverColor : "transparent")
                                border.color: shuffleEditor.rowKind(outputRowItem.row) === "one" ? shuffleEditor.textColor : shuffleEditor.borderColor
                                radius: shuffleEditor.smallRadiusValue
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: oneButton.enabled
                                onPressed: function (mouse) {
                                    mouse.accepted = !!(mouse.modifiers & (Qt.ControlModifier | Qt.AltModifier));
                                }
                                onClicked: function (mouse) {
                                    if (mouse.modifiers & Qt.AltModifier)
                                        shuffleEditor.keyAtFrame("sourceKind" + outputRowItem.row);
                                    else
                                        shuffleEditor.setAllConstants("one");
                                }
                            }
                        }

                        // The row states a source the input does not carry (or a
                        // disconnected input) rather than leaving the zero-fill
                        // to be discovered on the output pixels.
                        Text {
                            id: missingMarker
                            objectName: "shuffleMissing_" + outputRowItem.row + "_" + shuffleEditor.nodeId
                            Layout.alignment: Qt.AlignVCenter
                            visible: shuffleEditor.rowSourceState(outputRowItem.row) === "missing" || shuffleEditor.rowSourceState(outputRowItem.row) === "disconnected"
                            text: "missing"
                            color: shuffleEditor.errorColor
                            font.pixelSize: shuffleEditor.smallFontSize
                            Accessible.name: shuffleEditor.rowSourceIssue(outputRowItem.row)
                            ToolTip.visible: missingHover.hovered
                            ToolTip.text: shuffleEditor.rowSourceIssue(outputRowItem.row)
                            HoverHandler {
                                id: missingHover
                            }
                        }

                        // Plain click opens routing/key controls; shared Alt-key
                        // and exposure-drag gestures retain their own ownership.
                        ExposureLabel {
                            id: outputName
                            objectName: "shuffleOutName_" + outputRowItem.row + "_" + shuffleEditor.nodeId
                            Layout.fillWidth: true
                            Layout.minimumWidth: 40
                            theme: shuffleEditor.cellTheme
                            networkId: shuffleEditor.networkId
                            instanceId: shuffleEditor.instanceId
                            nodeId: shuffleEditor.nodeId
                            parameterKey: "outputChannel" + outputRowItem.row
                            labelText: shuffleEditor.rowEnabled(outputRowItem.row) ? shuffleEditor.rowOutput(outputRowItem.row) : "\u2014"
                            keyStatus: shuffleEditor.keyStatusOf("outputChannel" + outputRowItem.row)
                            frame: shuffleEditor.frame
                            onKeyRequested: shuffleEditor.keyAtFrame("outputChannel" + outputRowItem.row)
                            ToolTip.text: "Click for routing and keys. " + outputName.tooltipText()
                            TapHandler {
                                acceptedModifiers: Qt.NoModifier
                                enabled: shuffleEditor.rowEnabled(outputRowItem.row)
                                onTapped: shuffleEditor.openRouting(outputRowItem.row)
                            }
                        }

                        KeyIndicator {
                            objectName: "shuffleKey_" + outputRowItem.row + "_" + shuffleEditor.nodeId
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth: 20
                            theme: shuffleEditor.cellTheme
                            networkId: shuffleEditor.networkId
                            nodeId: shuffleEditor.nodeId
                            parameterKey: "outputChannel" + outputRowItem.row
                            parameterLabel: shuffleEditor.rowEnabled(outputRowItem.row) ? shuffleEditor.rowOutput(outputRowItem.row) : "output channel " + (outputRowItem.row + 1)
                            keyStatus: shuffleEditor.keyStatusOf("outputChannel" + outputRowItem.row)
                            scope: ""
                            frame: shuffleEditor.frame
                            revealAvailable: shuffleEditor.revealAvailable()
                            onKeyRequested: shuffleEditor.keyAtFrame("outputChannel" + outputRowItem.row)
                            onRemoveKeyRequested: shuffleEditor.removeKeyAtFrame("outputChannel" + outputRowItem.row)
                            onRevealRequested: shuffleEditor.revealInAnimation("outputChannel" + outputRowItem.row)
                        }

                    }
                }
            }
        }
    }
}
