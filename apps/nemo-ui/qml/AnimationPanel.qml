import QtQuick
import QtQuick.Controls

// Prototype-overhaul presentation port. AnimationViewModel queries the shared
// session and submits commands; this panel owns only view/selection/previews.
FocusScope {
    id: animationPanel
    objectName: "animationPanel"

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var panelContext: ({})
    property var contextRouter: null
    property var workspace: null
    property var theme: null
    readonly property var controller: viewerController
    property var model: null
    property string networkId: ""
    property string targetNodeId: ""
    property var selectedKeyIds: []
    property var selectedChannelIds: []
    readonly property real currentFrame: contextRouter && panelContext.timelineClock !== undefined ? Number(panelContext.timelineClock) : controller.frame
    property var groupStates: ({})
    property string loadedGroup: ""
    property string loadedPanel: ""
    property bool stateReady: false
    clip: true

    function viewState() {
        return {
            "network": networkId,
            "target": targetNodeId,
            "mode": viewMode,
            "start": viewStart,
            "end": viewEnd,
            "min": valueMin,
            "max": valueMax,
            "tree": treeWidth,
            "scroll": scrollY,
            "collapsed": collapsedNodes,
            "hidden": hiddenCurveIds,
            "keys": selectedKeyIds,
            "channels": selectedChannelIds
        };
    }
    function rememberGroup() {
        if (!stateReady || !loadedGroup)
            return;
        var copy = Object.assign({}, groupStates);
        copy[loadedGroup] = viewState();
        groupStates = copy;
    }
    function saveState() {
        if (!stateReady || !workspace || !panelId)
            return;
        rememberGroup();
        var saved = Object.assign({}, workspace.panelState(panelId));
        saved.animationGroups = groupStates;
        if (JSON.stringify(saved) !== JSON.stringify(workspace.panelState(panelId)))
            workspace.setPanelState(panelId, saved);
    }
    function restoreState() {
        if (!panelId || !model)
            return;
        var previous = stateReady ? viewState() : ({});
        if (loadedPanel !== panelId) {
            stateReady = false;
            loadedPanel = panelId;
            groupStates = panelState.animationGroups || ({});
        } else if (loadedGroup === panelGroup) {
            return;
        } else {
            rememberGroup();
        }
        stateReady = false;
        cancelPreview();
        keyEditPopup.close();
        loadedGroup = panelGroup;
        var saved = groupStates[loadedGroup] || previous;
        networkId = String(saved.network || controller.rootNetworkId);
        targetNodeId = String(saved.target || "");
        model.networkId = networkId;
        viewMode = saved.mode === "curves" ? "curves" : "track";
        viewStart = Number.isFinite(saved.start) ? saved.start : currentFrame - 8;
        viewEnd = Number.isFinite(saved.end) && saved.end > viewStart ? saved.end : viewStart + 72;
        valueMin = Number.isFinite(saved.min) ? saved.min : -1;
        valueMax = Number.isFinite(saved.max) && saved.max > valueMin ? saved.max : valueMin + 3;
        treeWidth = Number.isFinite(saved.tree) ? saved.tree : 154;
        collapsedNodes = saved.collapsed || ({});
        hiddenCurveIds = saved.hidden || ({});
        selectedKeyIds = saved.keys || [];
        selectedChannelIds = saved.channels || [];
        tree.contentY = Math.max(0, Math.min(Number(saved.scroll || 0), tree.contentHeight - tree.height));
        stateReady = true;
        animationSurface.requestPaint();
    }
    function queueSave() {
        if (stateReady)
            stateTimer.restart();
    }
    Timer {
        id: stateTimer
        interval: 0
        onTriggered: saveState()
    }
    onPanelIdChanged: Qt.callLater(restoreState)
    onPanelGroupChanged: Qt.callLater(restoreState)
    onSelectedKeyIdsChanged: {
        animationSurface.requestPaint();
        queueSave();
    }
    onSelectedChannelIdsChanged: {
        animationSurface.requestPaint();
        queueSave();
    }
    onCurrentFrameChanged: animationSurface.requestPaint()
    onViewStartChanged: {
        animationSurface.requestPaint();
        queueSave();
    }
    onViewEndChanged: {
        animationSurface.requestPaint();
        queueSave();
    }
    onValueMinChanged: {
        animationSurface.requestPaint();
        queueSave();
    }
    onValueMaxChanged: {
        animationSurface.requestPaint();
        queueSave();
    }
    onTreeWidthChanged: queueSave()
    onScrollYChanged: queueSave()
    onCollapsedNodesChanged: queueSave()
    onHiddenCurveIdsChanged: queueSave()
    Connections {
        target: animationPanel.contextRouter
        function onInspectorRequested(group, network, nodeId) {
            if (group !== animationPanel.panelGroup)
                return;
            animationPanel.cancelPreview();
            keyEditPopup.close();
            animationPanel.networkId = network;
            animationPanel.targetNodeId = nodeId;
            if (animationPanel.model)
                animationPanel.model.networkId = network;
            animationPanel.selectNode(nodeId);
            animationPanel.queueSave();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.panel
        z: -1
    }

    property string viewMode: "track"
    property real viewStart: currentFrame - 8
    property real viewEnd: currentFrame + 64
    readonly property real pixelsPerFrame: animationSurface.width / Math.max(1, viewEnd - viewStart)
    property real valueMin: -1
    property real valueMax: 2
    property real scrollY: 0
    property real panValueMin: 0
    property real panValueMax: 1
    property string hoverKey: ""
    property int rowHeight: 22
    property int rulerHeight: 28
    property real treeWidth: 154
    readonly property int labelWidth: Math.round(Math.max(118, Math.min(260, treeWidth)))
    property int gestureThreshold: 4
    property var collapsedNodes: ({})
    property string gestureState: "idle"
    property var previewIds: []
    property var previewKeyframes: ({})
    property real previewDeltaTime: 0
    property real previewDeltaValue: 0
    property string tangentPreviewId: ""
    property string tangentPreviewSide: ""
    property real tangentPreviewSlope: 0
    property bool boxSelecting: false
    property real boxStartX: 0
    property real boxStartY: 0
    property real boxEndX: 0
    property real boxEndY: 0
    property real panAnchorFrame: 0
    property string contextKeyId: ""
    property string transientError: ""
    property bool dragConstrained: false
    property string dragAxis: ""
    property string pendingToggleKey: ""
    property bool dragMoved: false
    property var hiddenCurveIds: ({})
    property var contextCurveIds: []

    function curveVisible(id) {
        return hiddenCurveIds[String(id)] !== true;
    }
    function setCurvesVisible(ids, visible) {
        var next = Object.assign({}, hiddenCurveIds);
        for (var i = 0; i < ids.length; ++i) {
            if (visible)
                delete next[ids[i]];
            else
                next[ids[i]] = true;
        }
        hiddenCurveIds = next;
        animationSurface.requestPaint();
    }
    function isolateCurves(ids) {
        var next = {};
        for (var i = 0; i < channels.length; ++i)
            if (ids.indexOf(channels[i].id) < 0)
                next[channels[i].id] = true;
        hiddenCurveIds = next;
        animationSurface.requestPaint();
    }
    function showAllCurves() {
        hiddenCurveIds = {};
        animationSurface.requestPaint();
    }
    function openChannelMenu(row, anchor) {
        forceActiveFocus();
        contextCurveIds = row.parent ? row.channels.map(function (c) {
                return c.id;
            }) : [row.channelId];
        var point = anchor.mapToItem(animationPanel, anchor.width, anchor.height / 2);
        channelMenu.x = Math.max(2, Math.min(width - channelMenu.width - 2, point.x));
        channelMenu.y = Math.max(2, Math.min(height - channelMenu.height - 2, point.y));
        channelMenu.open();
    }

    readonly property var channels: model ? model.channels : []
    readonly property var rows: buildRows(channels)
    readonly property real frameSpan: Math.max(1, viewEnd - viewStart)
    readonly property int selectedCount: selectedKeys().length
    readonly property real timelineWidth: Math.max(1, width - labelWidth)
    property alias headerTools: headerToolsComponent
    function channelColor(id) {
        var colors = ["#7da7c8", "#c99b6d", "#8fb58a", "#aa98bf", "#c28c9e", "#82b6b0", "#b5ad7b", "#9ca6bc"];
        var hash = 2166136261, name = String(id);
        for (var i = 0; i < name.length; ++i)
            hash = Math.imul(hash ^ name.charCodeAt(i), 16777619);
        return colors[(hash >>> 0) % colors.length];
    }

    function channelId(channel) {
        return channel ? String(channel.id) : "";
    }

    function buildRows(list) {
        var grouped = ({});
        var order = [];
        for (var i = 0; i < list.length; ++i) {
            var channel = list[i];
            if (!channel)
                continue;
            var nodeId = String(channel.nodeId);
            if (!grouped[nodeId]) {
                grouped[nodeId] = [];
                order.push(nodeId);
            }
            grouped[nodeId].push(channel);
        }
        var result = [];
        for (var n = 0; n < order.length; ++n) {
            var id = order[n];
            var name = grouped[id][0].nodeName || id;
            result.push({
                    "nodeId": id,
                    "channelId": "",
                    "label": name,
                    "parent": true,
                    "collapsed": collapsedNodes[id] === true,
                    "channels": grouped[id]
                });
            if (collapsedNodes[id] === true)
                continue;
            for (var c = 0; c < grouped[id].length; ++c) {
                var child = grouped[id][c];
                result.push({
                        "nodeId": id,
                        "channelId": channelId(child),
                        "parameterKey": String(child.parameterKey),
                        "label": child.label || child.parameterKey,
                        "kind": child.kind || "number",
                        "parent": false,
                        "channel": child
                    });
            }
        }
        return result;
    }

    function channelById(id) {
        for (var i = 0; i < channels.length; ++i)
            if (channelId(channels[i]) === String(id))
                return channels[i];
        return null;
    }

    function channelsForNode(nodeId) {
        var result = [];
        for (var i = 0; i < channels.length; ++i)
            if (String(channels[i].nodeId) === String(nodeId))
                result.push(channels[i]);
        return result;
    }

    function keysForChannel(channel) {
        return channel && channel.keys ? channel.keys : [];
    }

    function idsForChannels(list) {
        var result = [];
        for (var i = 0; i < list.length; ++i) {
            var keys = keysForChannel(list[i]);
            for (var k = 0; k < keys.length; ++k)
                if (result.indexOf(String(keys[k].id)) < 0)
                    result.push(String(keys[k].id));
        }
        return result;
    }

    function idsForNode(nodeId) {
        return idsForChannels(channelsForNode(nodeId));
    }

    function selectedKeys() {
        return selectedKeyIds;
    }

    function selectedChannels() {
        return selectedChannelIds;
    }

    function isSelectedKey(id) {
        return selectedKeys().indexOf(String(id)) >= 0;
    }

    function isSelectedChannel(id) {
        return selectedChannels().indexOf(String(id)) >= 0;
    }

    function setSelectedKeys(ids) {
        selectedKeyIds = ids;
    }

    function setSelectedChannels(ids) {
        selectedChannelIds = ids;
    }

    function unique(values) {
        var result = [];
        for (var i = 0; i < values.length; ++i) {
            var value = String(values[i]);
            if (result.indexOf(value) < 0)
                result.push(value);
        }
        return result;
    }

    function selectKey(id, additive, toggle) {
        var next = additive ? selectedKeys().slice() : [];
        var value = String(id);
        var index = next.indexOf(value);
        if (toggle && index >= 0)
            next.splice(index, 1);
        else if (index < 0)
            next.push(value);
        setSelectedKeys(next);
        var channel = keyChannel(value);
        if (channel) {
            var channelIds = additive ? selectedChannels().slice() : [];
            if (channelIds.indexOf(channelId(channel)) < 0)
                channelIds.push(channelId(channel));
            setSelectedChannels(unique(channelIds));
        }
    }

    function selectChannel(channel, additive, toggle) {
        if (!channel)
            return;
        var next = additive ? selectedChannels().slice() : [];
        var id = channelId(channel);
        var index = next.indexOf(id);
        if (toggle && index >= 0)
            next.splice(index, 1);
        else if (index < 0)
            next.push(id);
        setSelectedChannels(unique(next));
    }

    function selectNode(nodeId) {
        var list = channelsForNode(nodeId);
        var ids = [];
        for (var i = 0; i < list.length; ++i)
            ids.push(channelId(list[i]));
        setSelectedChannels(ids);
    }

    function keyChannel(id) {
        for (var i = 0; i < channels.length; ++i) {
            var keys = keysForChannel(channels[i]);
            for (var k = 0; k < keys.length; ++k)
                if (String(keys[k].id) === String(id))
                    return channels[i];
        }
        return null;
    }

    function rowIndex(nodeId, channelIdValue) {
        for (var i = 0; i < rows.length; ++i)
            if (String(rows[i].nodeId) === String(nodeId) && String(rows[i].channelId) === String(channelIdValue))
                return i;
        for (var j = 0; j < rows.length; ++j)
            if (String(rows[j].nodeId) === String(nodeId) && rows[j].parent)
                return j;
        return -1;
    }

    function toggleNode(nodeId) {
        var copy = {};
        for (var key in collapsedNodes)
            copy[key] = collapsedNodes[key];
        copy[String(nodeId)] = copy[String(nodeId)] !== true;
        collapsedNodes = copy;
        animationSurface.requestPaint();
    }

    function allKeyRecords() {
        var result = [];
        if (viewMode === "curves") {
            for (var c = 0; c < channels.length; ++c) {
                var channel = channels[c];
                if (!isNumericChannel(channel) || !curveVisible(channel.id))
                    continue;
                for (var k = 0; k < channel.keys.length; ++k)
                    result.push({
                            "key": channel.keys[k],
                            "row": {
                                "channel": channel
                            },
                            "rowIndex": -1
                        });
            }
            return result;
        }
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].parent)
                continue;
            var keys = keysForChannel(rows[i].channel);
            for (var j = 0; j < keys.length; ++j)
                result.push({
                        "key": keys[j],
                        "row": rows[i],
                        "rowIndex": i
                    });
        }
        return result;
    }

    function keyRecord(id) {
        for (var i = 0; i < channels.length; ++i) {
            var channel = channels[i];
            var keys = keysForChannel(channel);
            for (var k = 0; k < keys.length; ++k) {
                if (String(keys[k].id) === String(id))
                    return {
                        "key": keys[k],
                        "channel": channel,
                        "rowIndex": rowIndex(channel.nodeId, channelId(channel))
                    };
            }
        }
        return null;
    }

    function numericValue(value) {
        if (typeof value === "number" && isFinite(value))
            return value;
        if (value === true)
            return 1;
        if (value === false)
            return 0;
        var parsed = Number(value);
        return isFinite(parsed) ? parsed : 0;
    }

    function isNumericChannel(channel) {
        return channel && channel.kind !== "choice" && channel.kind !== "toggle";
    }

    function rowY(index) {
        return rulerHeight + Number(index) * rowHeight + rowHeight * 0.5 - scrollY;
    }

    function valueToY(value) {
        return rulerHeight + (valueMax - Number(value)) / Math.max(0.000001, valueMax - valueMin) * Math.max(1, animationSurface.height - rulerHeight);
    }

    function yToValue(y) {
        return valueMax - (Number(y) - rulerHeight) / Math.max(1, animationSurface.height - rulerHeight) * (valueMax - valueMin);
    }

    function timeToX(frame) {
        return (Number(frame) - viewStart) * pixelsPerFrame;
    }

    function xToTime(x) {
        return viewStart + Number(x) / Math.max(0.001, pixelsPerFrame);
    }

    function displayedTime(key) {
        if (!key)
            return 0;
        return Number(key.time) + (previewKeyframes[key.keyframeId] ? previewDeltaTime : 0);
    }

    function displayedValue(key) {
        if (!key)
            return 0;
        return numericValue(key.value) + (previewIds.indexOf(String(key.id)) >= 0 ? previewDeltaValue : 0);
    }

    function displayedSlope(key, side) {
        if (tangentPreviewId === String(key.id) && (tangentPreviewSide === side || key.tangentMode === "smooth"))
            return tangentPreviewSlope;
        return Number(side === "in" ? key.inSlope : key.outSlope) || 0;
    }

    function currentAlt(modifiers) {
        return (modifiers & Qt.AltModifier) !== 0;
    }

    function showTransientError(message) {
        transientError = String(message || "Animation edit rejected.");
        errorToastTimer.restart();
    }

    function keyForContext() {
        return contextKeyId.length > 0 ? keyRecord(contextKeyId) : null;
    }

    function showKeyContext(record, x, y) {
        if (!record)
            return;
        if (!isSelectedKey(record.key.id))
            selectKey(record.key.id, false, false);
        contextKeyId = String(record.key.id);
        keyContextMenu.x = Math.max(2, Math.min(width - keyContextMenu.width - 2, x));
        keyContextMenu.y = Math.max(2, Math.min(height - keyContextMenu.height - 2, y));
        keyContextMenu.open();
    }

    function openEditorMenu(anchorItem) {
        forceActiveFocus();
        if (anchorItem && anchorItem.mapToItem) {
            var point = anchorItem.mapToItem(animationPanel, 0, anchorItem.height);
            editorMenu.x = Math.max(2, Math.min(width - editorMenu.width - 2, point.x));
            editorMenu.y = Math.max(2, Math.min(height - editorMenu.height - 2, point.y));
        } else {
            editorMenu.x = Math.max(2, width - editorMenu.width - 8);
            editorMenu.y = 4;
        }
        editorMenu.open();
    }
    function applyInterpolation(mode) {
        if (selectedKeys().length > 0 && !model.setInterpolation(selectedKeys(), mode))
            showTransientError(model.error);
    }

    function applyTangentMode(mode) {
        if (selectedKeys().length > 0 && !model.setTangentMode(selectedKeys(), mode))
            showTransientError(model.error);
    }

    function openExactEditor() {
        var record = keyForContext();
        if (!record)
            return;
        keyEditPopup.errorMessage = "";
        exactTime.text = Number(record.key.time).toString();
        exactValue.text = numericValue(record.key.value).toString();
        exactInSlope.text = Number(record.key.inSlope || 0).toString();
        exactOutSlope.text = Number(record.key.outSlope || 0).toString();
        if (!model.beginGesture()) {
            showTransientError(model.error);
            return;
        }
        keyEditPopup.open();
    }

    function applyExactEditor() {
        var record = keyForContext();
        if (!record)
            return;
        if (!exactTime.acceptableInput || !exactValue.acceptableInput || (keyEditPopup.showSlopes && (!exactInSlope.acceptableInput || (!keyEditPopup.smooth && !exactOutSlope.acceptableInput)))) {
            keyEditPopup.errorMessage = "Enter a valid number in each field.";
            return;
        }
        var time = Number(exactTime.text), value = Number(exactValue.text);
        var inSlope = Number(exactInSlope.text), outSlope = keyEditPopup.smooth ? inSlope : Number(exactOutSlope.text);
        if (![time, value, inSlope, outSlope].every(function (item) {
                return isFinite(item);
            })) {
            keyEditPopup.errorMessage = "Animation values must be finite numbers.";
            return;
        }
        var accepted = model.editKey(record.key.id, time, value, inSlope, outSlope);
        if (!accepted) {
            keyEditPopup.errorMessage = model.error;
            return;
        }
        keyEditPopup.close();
        forceActiveFocus();
    }

    function insertCurveKey(curve) {
        if (!curve)
            return;
        var insertedId = String(model.insertKey(curve.channelId, Math.round(curve.time)));
        if (insertedId.length > 0)
            selectKey(insertedId, false, false);
        else
            showTransientError(model.error);
        animationSurface.requestPaint();
    }

    function beginPreview(ids, state) {
        if (!model || !model.beginGesture())
            return;
        previewIds = unique(ids);
        var frames = {};
        for (var i = 0; i < previewIds.length; ++i) {
            var record = keyRecord(previewIds[i]);
            if (record)
                frames[record.key.keyframeId] = true;
        }
        previewKeyframes = frames;
        previewDeltaTime = 0;
        previewDeltaValue = 0;
        gestureState = state;
        dragMoved = false;
        dragAxis = "";
        animationSurface.requestPaint();
    }

    function cancelPreview() {
        if (model && !keyEditPopup.visible)
            model.cancelGesture();
        gestureState = "idle";
        previewIds = [];
        previewKeyframes = ({});
        previewDeltaTime = 0;
        previewDeltaValue = 0;
        tangentPreviewId = "";
        tangentPreviewSide = "";
        boxSelecting = false;
        dragMoved = false;
        dragAxis = "";
        pendingToggleKey = "";
        animationSurface.requestPaint();
    }

    function commitPreview() {
        var ids = previewIds.slice();
        var dt = previewDeltaTime;
        var dv = previewDeltaValue;
        var ok = true;
        if (ids.length > 0 && (Math.abs(dt) > 0.000001 || Math.abs(dv) > 0.000001))
            ok = model.moveKeys(ids, dt, dv);
        if (tangentPreviewId.length > 0)
            ok = model.setTangent(tangentPreviewId, tangentPreviewSide, tangentPreviewSlope) && ok;
        if (!ok)
            showTransientError(model.error || "Animation edit rejected.");
        cancelPreview();
        return ok;
    }

    function hitKey(x, y) {
        var best = null, distance = 100000;
        function consider(key, channel, rowIndexValue) {
            var px = timeToX(displayedTime(key));
            var py = viewMode === "curves" ? valueToY(displayedValue(key)) : rowY(rowIndexValue);
            var d = Math.hypot(x - px, y - py);
            if (d <= 10 && d < distance) {
                distance = d;
                best = {
                    "key": key,
                    "row": {
                        "channel": channel
                    },
                    "rowIndex": rowIndexValue,
                    "distance": d
                };
            }
        }
        if (viewMode === "curves") {
            for (var c = 0; c < channels.length; ++c) {
                if (!isNumericChannel(channels[c]) || !curveVisible(channels[c].id))
                    continue;
                var curveKeys = keysForChannel(channels[c]);
                for (var k = 0; k < curveKeys.length; ++k)
                    consider(curveKeys[k], channels[c], -1);
            }
        } else {
            for (var r = 0; r < rows.length; ++r) {
                if (rows[r].parent)
                    continue;
                var trackKeys = keysForChannel(rows[r].channel);
                for (var t = 0; t < trackKeys.length; ++t)
                    consider(trackKeys[t], rows[r].channel, r);
            }
        }
        return best;
    }

    function curveValueAt(channel, a, b, time) {
        var at = Number(time), ta = displayedTime(a), tb = displayedTime(b);
        var dt = tb - ta;
        if (!isFinite(dt) || dt <= 0)
            return displayedValue(a);
        if (at <= ta)
            return displayedValue(a);
        if (at >= tb)
            return displayedValue(b);
        var u = (at - ta) / dt;
        var mode = a.interpolation || "linear";
        if (mode === "hold")
            return displayedValue(a);
        if (mode !== "bezier")
            return displayedValue(a) + (displayedValue(b) - displayedValue(a)) * u;
        var m0 = displayedSlope(a, "out");
        var m1 = displayedSlope(b, "in");
        var u2 = u * u, u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * displayedValue(a) + (u3 - 2 * u2 + u) * dt * m0 + (-2 * u3 + 3 * u2) * displayedValue(b) + (u3 - u2) * dt * m1;
    }

    function curveHit(x, y) {
        if (viewMode !== "curves")
            return null;
        var best = null, distance = 12;
        function consider(channel, time, value, px, py) {
            var d = Math.hypot(x - px, y - py);
            if (d < distance) {
                distance = d;
                best = {
                    "channelId": String(channel.id),
                    "time": time,
                    "value": value,
                    "x": px,
                    "y": py
                };
            }
        }
        function considerLine(channel, x0, y0, x1, y1, t0, t1, v0, v1, evaluator) {
            var dx = x1 - x0, dy = y1 - y0;
            var denom = dx * dx + dy * dy;
            var u = denom > 0 ? ((x - x0) * dx + (y - y0) * dy) / denom : 0;
            u = Math.max(0, Math.min(1, u));
            var time = t0 + (t1 - t0) * u;
            var value = evaluator ? evaluator(time) : v0 + (v1 - v0) * u;
            consider(channel, time, value, x0 + dx * u, y0 + dy * u);
        }
        for (var c = 0; c < channels.length; ++c) {
            var channel = channels[c];
            if (!isNumericChannel(channel) || !curveVisible(channel.id))
                continue;
            var ordered = keysForChannel(channel);
            for (var i = 0; i < ordered.length - 1; ++i) {
                var a = ordered[i], b = ordered[i + 1];
                var ta = displayedTime(a), tb = displayedTime(b);
                if (!isFinite(ta) || !isFinite(tb) || tb <= ta)
                    continue;
                var ax = timeToX(ta), bx = timeToX(tb);
                var ay = valueToY(displayedValue(a)), by = valueToY(displayedValue(b));
                var mode = a.interpolation || "linear";
                if (mode === "hold") {
                    considerLine(channel, ax, ay, bx, ay, ta, tb, displayedValue(a), displayedValue(a));
                    considerLine(channel, bx, ay, bx, by, tb, tb, displayedValue(a), displayedValue(b));
                    continue;
                }
                var samples = Math.max(16, Math.ceil(Math.abs(bx - ax) / 2));
                var px = ax, py = ay, pt = ta, pv = displayedValue(a);
                for (var s = 1; s <= samples; ++s) {
                    var qt = ta + (tb - ta) * s / samples;
                    var qx = timeToX(qt), qv = curveValueAt(channel, a, b, qt), qy = valueToY(qv);
                    considerLine(channel, px, py, qx, qy, pt, qt, pv, qv, function (time) {
                            return curveValueAt(channel, a, b, time);
                        });
                    px = qx;
                    py = qy;
                    pt = qt;
                    pv = qv;
                }
            }
        }
        return best;
    }

    function tangentPositions() {
        var result = [];
        if (viewMode !== "curves")
            return result;
        var records = allKeyRecords();
        for (var i = 0; i < records.length; ++i) {
            var key = records[i].key, channel = records[i].row.channel;
            if (!isSelectedKey(key.id))
                continue;
            var index = channel.keys.indexOf(key);
            for (var sideIndex = 0; sideIndex < 2; ++sideIndex) {
                var side = sideIndex === 0 ? "in" : "out";
                if (side === "out" && (key.interpolation !== "bezier" || index === channel.keys.length - 1))
                    continue;
                if (side === "in" && (index <= 0 || channel.keys[index - 1].interpolation !== "bezier"))
                    continue;
                var slope = displayedSlope(key, side);
                var neighbor = channel.keys[index + (side === "in" ? -1 : 1)];
                // These unweighted slope handles need a usable screen-space length,
                // even when adjacent keys are only a frame apart.
                var length = Math.max(18, Math.min(42, Math.abs(displayedTime(neighbor) - displayedTime(key)) * pixelsPerFrame / 3));
                var valueScale = Math.max(1, animationSurface.height - rulerHeight) / Math.max(0.000001, valueMax - valueMin);
                var dt = length / Math.max(0.001, Math.hypot(pixelsPerFrame, slope * valueScale));
                dt *= side === "in" ? -1 : 1;
                result.push({
                        "id": String(key.id),
                        "key": key,
                        "side": side,
                        "x": timeToX(displayedTime(key) + dt),
                        "y": valueToY(displayedValue(key) + slope * dt)
                    });
            }
        }
        return result;
    }

    function tangentHit(x, y) {
        var handles = tangentPositions(), best = null, distance = 100000;
        for (var i = 0; i < handles.length; ++i) {
            var d = Math.hypot(x - handles[i].x, y - handles[i].y);
            if (d <= 10 && d < distance) {
                best = handles[i];
                distance = d;
            }
        }
        if (best)
            best.distance = distance;
        return best;
    }

    function keyPositions() {
        var result = [];
        var records = allKeyRecords();
        for (var i = 0; i < records.length; ++i) {
            var record = records[i];
            var key = record.key;
            var py = viewMode === "curves" && isNumericChannel(record.row.channel) ? valueToY(displayedValue(key)) : rowY(record.rowIndex);
            result.push({
                    "id": String(key.id),
                    "x": timeToX(displayedTime(key)),
                    "y": py
                });
        }
        return result;
    }

    function frameAll() {
        var minFrame = Number.POSITIVE_INFINITY, maxFrame = Number.NEGATIVE_INFINITY;
        for (var i = 0; i < channels.length; ++i) {
            if (viewMode === "curves" && !curveVisible(channels[i].id))
                continue;
            var keys = keysForChannel(channels[i]);
            for (var k = 0; k < keys.length; ++k) {
                minFrame = Math.min(minFrame, Number(keys[k].time));
                maxFrame = Math.max(maxFrame, Number(keys[k].time));
            }
        }
        if (!isFinite(minFrame)) {
            viewStart = currentFrame - 8;
            viewEnd = currentFrame + 64;
            return;
        }
        var padding = Math.max(2, (maxFrame - minFrame) * 0.08);
        viewStart = minFrame - padding;
        viewEnd = Math.max(viewStart + 10, maxFrame + padding);
        fitValues([]);
        animationSurface.requestPaint();
    }

    function frameSelected() {
        var ids = selectedKeys().slice();
        if (ids.length === 0) {
            var channelsToFrame = selectedChannels();
            for (var c = 0; c < channelsToFrame.length; ++c) {
                var selectedChannel = channelById(channelsToFrame[c]);
                var keys = keysForChannel(selectedChannel);
                for (var q = 0; q < keys.length; ++q)
                    ids.push(String(keys[q].id));
            }
        }
        var minFrame = Number.POSITIVE_INFINITY, maxFrame = Number.NEGATIVE_INFINITY;
        for (var i = 0; i < ids.length; ++i) {
            var record = keyRecord(ids[i]);
            if (!record)
                continue;
            minFrame = Math.min(minFrame, Number(record.key.time));
            maxFrame = Math.max(maxFrame, Number(record.key.time));
        }
        if (!isFinite(minFrame))
            return frameAll();
        var padding = Math.max(2, (maxFrame - minFrame) * 0.12);
        viewStart = minFrame - padding;
        viewEnd = Math.max(viewStart + 10, maxFrame + padding);
        fitValues(ids);
        animationSurface.requestPaint();
    }

    function fitValues(ids) {
        var lo = Infinity, hi = -Infinity;
        for (var c = 0; c < channels.length; ++c) {
            var channel = channels[c];
            if (!isNumericChannel(channel) || (viewMode === "curves" && !curveVisible(channel.id)))
                continue;
            for (var k = 0; k < channel.keys.length; ++k) {
                var key = channel.keys[k];
                if (ids.length && ids.indexOf(String(key.id)) < 0)
                    continue;
                lo = Math.min(lo, Number(key.value));
                hi = Math.max(hi, Number(key.value));
                // Include cubic control values so overshooting tangents remain reachable.
                if (!ids.length && k + 1 < channel.keys.length && key.interpolation === "bezier") {
                    var next = channel.keys[k + 1], dt = (next.time - key.time) / 3;
                    lo = Math.min(lo, key.value + key.outSlope * dt, next.value - next.inSlope * dt);
                    hi = Math.max(hi, key.value + key.outSlope * dt, next.value - next.inSlope * dt);
                }
            }
        }
        if (!isFinite(lo)) {
            lo = 0;
            hi = 1;
        }
        var padding = Math.max(0.1, (hi - lo) * 0.22);
        valueMin = lo - padding;
        valueMax = hi + padding;
    }

    function setFrame(frameValue) {
        var frame = Math.round(frameValue);
        if (contextRouter)
            contextRouter.setGroupContext(panelGroup, {
                    "timelineClock": frame
                });
        controller.setFrame(frame);
    }

    function selectBox() {
        var left = Math.min(boxStartX, boxEndX), right = Math.max(boxStartX, boxEndX);
        var top = Math.min(boxStartY, boxEndY), bottom = Math.max(boxStartY, boxEndY);
        var ids = [];
        var records = allKeyRecords();
        for (var i = 0; i < records.length; ++i) {
            var record = records[i];
            var x = timeToX(displayedTime(record.key));
            var y = viewMode === "curves" && isNumericChannel(record.row.channel) ? valueToY(displayedValue(record.key)) : rowY(record.rowIndex);
            if (x >= left && x <= right && y >= top && y <= bottom)
                ids.push(String(record.key.id));
        }
        setSelectedKeys(ids);
        var channelIds = [];
        for (var k = 0; k < ids.length; ++k) {
            var channel = keyChannel(ids[k]);
            if (channel && channelIds.indexOf(channelId(channel)) < 0)
                channelIds.push(channelId(channel));
        }
        setSelectedChannels(channelIds);
    }

    function drawKey(ctx, record, x, y, selected) {
        var size = selected || hoverKey === String(record.key.id) ? 5 : viewMode === "track" ? 4 : 3;
        var color = record.row.channel ? channelColor(record.row.channel.id) : theme.muted;
        ctx.beginPath();
        ctx.moveTo(x, y - size);
        ctx.lineTo(x + size, y);
        ctx.lineTo(x, y + size);
        ctx.lineTo(x - size, y);
        ctx.closePath();
        ctx.fillStyle = selected ? theme.accent : color;
        ctx.globalAlpha = selected ? 1 : 0.9;
        ctx.fill();
        ctx.globalAlpha = 1;
        ctx.strokeStyle = selected || hoverKey === String(record.key.id) ? theme.text : theme.panel;
        ctx.lineWidth = selected ? 1.2 : 0.8;
        ctx.stroke();
    }

    function timeGridStep() {
        var raw = Math.max(1, 64 / Math.max(0.001, pixelsPerFrame));
        var magnitude = Math.pow(10, Math.floor(Math.log10(raw)));
        var scaled = raw / magnitude;
        return (scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 5 ? 5 : 10) * magnitude;
    }

    function drawRuler(ctx) {
        ctx.fillStyle = theme.header;
        ctx.fillRect(0, 0, animationSurface.width, rulerHeight);
        ctx.strokeStyle = theme.border;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, rulerHeight - 0.5);
        ctx.lineTo(animationSurface.width, rulerHeight - 0.5);
        ctx.stroke();
        var spacing = timeGridStep();
        var first = Math.floor(viewStart / spacing) * spacing;
        ctx.font = "10px sans-serif";
        for (var frame = first; frame <= viewEnd + spacing; frame += spacing) {
            var x = timeToX(frame);
            if (x < 0 || x > animationSurface.width)
                continue;
            ctx.strokeStyle = theme.muted;
            ctx.globalAlpha = 0.7;
            ctx.beginPath();
            ctx.moveTo(x, rulerHeight - 8);
            ctx.lineTo(x, rulerHeight);
            ctx.stroke();
            ctx.globalAlpha = 1;
            ctx.fillStyle = theme.muted;
            var label = String(Math.round(frame));
            if (x + 3 + ctx.measureText(label).width <= animationSurface.width - 4)
                ctx.fillText(label, x + 3, 11);
        }
        var playheadX = timeToX(currentFrame);
        if (playheadX >= 0 && playheadX <= animationSurface.width) {
            ctx.strokeStyle = theme.accent;
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.moveTo(playheadX, 0);
            ctx.lineTo(playheadX, animationSurface.height);
            ctx.stroke();
        }
    }

    function drawExtent(ctx, minTime, maxTime, y, parent) {
        var left = Math.max(0, timeToX(minTime));
        var right = Math.min(animationSurface.width, timeToX(maxTime));
        if (right < 0 || left > animationSurface.width)
            return;
        left = Math.max(0, Math.min(animationSurface.width, left));
        right = Math.max(0, Math.min(animationSurface.width, right));
        if (right - left < 3)
            right = left + 3;
        ctx.strokeStyle = parent ? theme.muted : theme.border;
        ctx.globalAlpha = parent ? 0.75 : 0.5;
        ctx.lineWidth = parent ? 1.2 : 1;
        ctx.beginPath();
        ctx.moveTo(left, y);
        ctx.lineTo(right, y);
        ctx.moveTo(left, y - 4);
        ctx.lineTo(left, y + 4);
        ctx.moveTo(right, y - 4);
        ctx.lineTo(right, y + 4);
        ctx.stroke();
        ctx.globalAlpha = 1;
    }

    function drawTrack(ctx) {
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i];
            var y0 = rulerHeight + i * rowHeight - scrollY;
            if (y0 + rowHeight < rulerHeight || y0 > animationSurface.height)
                continue;
            var selectedRow = row.parent ? false : isSelectedChannel(row.channelId);
            if (selectedRow) {
                ctx.fillStyle = theme.nodeSelected;
                ctx.globalAlpha = 0.32;
                ctx.fillRect(0, y0, animationSurface.width, rowHeight);
                ctx.globalAlpha = 1;
            }
            ctx.strokeStyle = row.parent ? theme.border : theme.field;
            ctx.lineWidth = row.parent ? 1.2 : 0.7;
            ctx.beginPath();
            ctx.moveTo(0, y0 + rowHeight - 0.5);
            ctx.lineTo(animationSurface.width, y0 + rowHeight - 0.5);
            ctx.stroke();
            var list = row.parent ? row.channels : [row.channel];
            var minTime = Number.POSITIVE_INFINITY, maxTime = Number.NEGATIVE_INFINITY;
            for (var c = 0; c < list.length; ++c) {
                var keys = keysForChannel(list[c]);
                for (var k = 0; k < keys.length; ++k) {
                    var displayTime = displayedTime(keys[k]);
                    minTime = Math.min(minTime, displayTime);
                    maxTime = Math.max(maxTime, displayTime);
                }
            }
            if (!isFinite(minTime))
                continue;
            var bandY = rowY(i);
            drawExtent(ctx, minTime, maxTime, bandY, row.parent);
            if (row.parent) {
                var seen = {};
                ctx.strokeStyle = theme.muted;
                ctx.globalAlpha = 0.65;
                ctx.lineWidth = 1;
                ctx.beginPath();
                for (var source = 0; source < list.length; ++source) {
                    var summaryKeys = keysForChannel(list[source]);
                    for (var tick = 0; tick < summaryKeys.length; ++tick) {
                        var time = displayedTime(summaryKeys[tick]);
                        if (seen[time])
                            continue;
                        seen[time] = true;
                        var x = timeToX(time);
                        if (x < 0 || x > animationSurface.width)
                            continue;
                        ctx.moveTo(x, bandY - 3);
                        ctx.lineTo(x, bandY + 3);
                    }
                }
                ctx.stroke();
                ctx.globalAlpha = 1;
            } else {
                var channelKeys = keysForChannel(row.channel);
                for (var keyIndex = 0; keyIndex < channelKeys.length; ++keyIndex)
                    drawKey(ctx, {
                            "key": channelKeys[keyIndex],
                            "row": row
                        }, timeToX(displayedTime(channelKeys[keyIndex])), bandY, isSelectedKey(channelKeys[keyIndex].id));
            }
        }
    }

    function drawCurve(ctx, channel) {
        var keys = keysForChannel(channel);
        if (keys.length === 0)
            return;
        ctx.strokeStyle = channelColor(channel.id);
        ctx.lineWidth = isSelectedChannel(channelId(channel)) ? 2 : 1.2;
        ctx.globalAlpha = isSelectedChannel(channelId(channel)) ? 1 : 0.82;
        ctx.beginPath();
        var firstX = timeToX(displayedTime(keys[0]));
        var firstY = valueToY(displayedValue(keys[0]));
        ctx.moveTo(firstX, firstY);
        for (var i = 0; i < keys.length - 1; ++i) {
            var a = keys[i], b = keys[i + 1];
            var ax = timeToX(displayedTime(a)), ay = valueToY(displayedValue(a));
            var bx = timeToX(displayedTime(b)), by = valueToY(displayedValue(b));
            var dt = Math.max(0.000001, displayedTime(b) - displayedTime(a));
            var mode = a.interpolation || "linear";
            if (mode === "hold") {
                ctx.lineTo(bx, ay);
                ctx.lineTo(bx, by);
            } else if (mode === "bezier") {
                var outSlope = displayedSlope(a, "out");
                var inSlope = displayedSlope(b, "in");
                ctx.bezierCurveTo(ax + (bx - ax) / 3, valueToY(displayedValue(a) + outSlope * dt / 3), bx - (bx - ax) / 3, valueToY(displayedValue(b) - inSlope * dt / 3), bx, by);
            } else {
                ctx.lineTo(bx, by);
            }
        }
        ctx.stroke();
        ctx.globalAlpha = 1;
        for (var k = 0; k < keys.length; ++k) {
            var key = keys[k];
            drawKey(ctx, {
                    "key": key,
                    "row": {
                        "channel": channel
                    }
                }, timeToX(displayedTime(key)), valueToY(displayedValue(key)), isSelectedKey(key.id));
        }
    }

    function formatAxisValue(value, step) {
        var decimals = Math.max(0, Math.min(6, Math.ceil(-Math.log10(Math.max(0.000001, step)))));
        return Number(value.toFixed(decimals)).toString();
    }

    function drawCurves(ctx) {
        ctx.font = "10px sans-serif";
        var targetTicks = Math.max(2, (animationSurface.height - rulerHeight) / 34);
        var rawStep = (valueMax - valueMin) / targetTicks;
        var magnitude = Math.pow(10, Math.floor(Math.log10(Math.max(0.00001, rawStep))));
        var normalized = rawStep / magnitude;
        var step = (normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10) * magnitude;
        var majorTime = timeGridStep();
        var firstTime = Math.floor(viewStart / majorTime) * majorTime;
        for (var frame = firstTime; frame <= viewEnd + majorTime; frame += majorTime) {
            var x = timeToX(frame);
            if (x < 0 || x > animationSurface.width)
                continue;
            ctx.strokeStyle = theme.border;
            ctx.globalAlpha = 0.34;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(x, rulerHeight);
            ctx.lineTo(x, animationSurface.height);
            ctx.stroke();
            if (majorTime * pixelsPerFrame > 30) {
                for (var minor = 1; minor < 4; ++minor) {
                    var minorX = timeToX(frame + majorTime * minor / 4);
                    if (minorX < 0 || minorX > animationSurface.width)
                        continue;
                    ctx.globalAlpha = 0.15;
                    ctx.beginPath();
                    ctx.moveTo(minorX, rulerHeight);
                    ctx.lineTo(minorX, animationSurface.height);
                    ctx.stroke();
                }
            }
        }
        for (var v = Math.ceil(valueMin / step) * step; v <= valueMax + step * 0.01; v += step) {
            var y = valueToY(v);
            if (y < rulerHeight || y > animationSurface.height)
                continue;
            ctx.strokeStyle = Math.abs(v) < step * 0.001 ? theme.muted : theme.border;
            ctx.globalAlpha = Math.abs(v) < step * 0.001 ? 0.85 : 0.38;
            ctx.lineWidth = Math.abs(v) < step * 0.001 ? 1.4 : 0.7;
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(animationSurface.width, y);
            ctx.stroke();
            ctx.fillStyle = theme.muted;
            ctx.globalAlpha = 0.95;
            if (y >= rulerHeight + 12)
                ctx.fillText(formatAxisValue(v, step), 6, y - 4);
        }
        ctx.globalAlpha = 1;
        for (var c = 0; c < channels.length; ++c)
            if (isNumericChannel(channels[c]) && curveVisible(channels[c].id))
                drawCurve(ctx, channels[c]);
        var handles = tangentPositions();
        for (var h = 0; h < handles.length; ++h) {
            var handle = handles[h];
            ctx.strokeStyle = theme.accent;
            ctx.globalAlpha = 0.9;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(timeToX(displayedTime(handle.key)), valueToY(displayedValue(handle.key)));
            ctx.lineTo(handle.x, handle.y);
            ctx.stroke();
            ctx.fillStyle = theme.accent;
            ctx.fillRect(handle.x - 4, handle.y - 4, 8, 8);
        }
        ctx.globalAlpha = 1;
    }

    Item {
        id: editor
        anchors.fill: parent
        clip: true

        Flickable {
            id: tree
            objectName: "animationScroll"
            x: 0
            y: rulerHeight
            width: labelWidth
            height: Math.max(1, parent.height - rulerHeight)
            contentWidth: width
            contentHeight: rows.length * rowHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            onContentYChanged: {
                scrollY = contentY;
                animationSurface.requestPaint();
            }
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
            Repeater {
                model: animationPanel.rows
                delegate: Rectangle {
                    id: channelRow
                    required property var modelData
                    required property int index
                    x: 0
                    y: index * rowHeight
                    width: labelWidth
                    height: rowHeight
                    color: modelData.parent ? theme.raised : isSelectedChannel(modelData.channelId) ? theme.nodeSelected : theme.panel
                    objectName: modelData.parent ? "animationNodeRow_" + modelData.nodeId : "animationParameterRow_" + modelData.nodeId + "_" + modelData.parameterKey
                    Rectangle {
                        visible: !modelData.parent
                        x: 9
                        y: (parent.height - height) / 2
                        width: 5
                        height: 8
                        radius: 2
                        color: channelColor(modelData.channelId)
                        opacity: viewMode !== "curves" || curveVisible(modelData.channelId) ? 0.9 : 0.25
                    }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: modelData.parent ? 9 : 22
                        anchors.rightMargin: modelData.parent ? 8 : 25
                        text: modelData.parent ? ((modelData.collapsed ? "›  " : "⌄  ") + modelData.label) : modelData.label
                        color: modelData.parent ? theme.text : viewMode === "curves" && !curveVisible(modelData.channelId) ? theme.disabled : theme.muted
                        font.pixelSize: modelData.parent ? 11 : 10
                        font.weight: modelData.parent ? Font.Medium : Font.Normal
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: modelData.parent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: theme.border
                        opacity: 0.75
                    }
                    MouseArea {
                        id: channelMouse
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        anchors.fill: parent
                        onClicked: function (mouse) {
                            if (mouse.button === Qt.RightButton) {
                                openChannelMenu(modelData, channelRow);
                                return;
                            }
                            animationPanel.forceActiveFocus();
                            if (modelData.parent) {
                                if (mouse.x < 24)
                                    toggleNode(modelData.nodeId);
                                else
                                    selectNode(modelData.nodeId);
                            } else {
                                selectChannel(modelData.channel, !!(mouse.modifiers & Qt.ShiftModifier), !!(mouse.modifiers & Qt.ShiftModifier));
                            }
                        }
                        onDoubleClicked: if (modelData.parent)
                            contextRouter.requestInspector(panelGroup, networkId, modelData.nodeId)
                    }
                    Item {
                        visible: !modelData.parent && viewMode === "curves" && (channelMouse.containsMouse || visibilityMouse.containsMouse || !curveVisible(modelData.channelId))
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 20
                        Canvas {
                            id: visibilityGlyph
                            anchors.centerIn: parent
                            width: 14
                            height: 12
                            property bool shown: curveVisible(modelData.channelId)
                            readonly property color tint: theme.muted
                            onTintChanged: requestPaint()
                            onShownChanged: requestPaint()
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.reset();
                                ctx.strokeStyle = tint;
                                ctx.lineWidth = 1;
                                ctx.beginPath();
                                ctx.moveTo(1, 6);
                                ctx.quadraticCurveTo(7, -1, 13, 6);
                                ctx.quadraticCurveTo(7, 13, 1, 6);
                                ctx.stroke();
                                ctx.beginPath();
                                ctx.arc(7, 6, 2, 0, Math.PI * 2);
                                ctx.stroke();
                                if (!shown) {
                                    ctx.beginPath();
                                    ctx.moveTo(1, 11);
                                    ctx.lineTo(13, 1);
                                    ctx.stroke();
                                }
                            }
                        }
                        MouseArea {
                            id: visibilityMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: setCurvesVisible([modelData.channelId], !curveVisible(modelData.channelId))
                        }
                        ToolTip.visible: visibilityMouse.containsMouse
                        ToolTip.delay: 450
                        ToolTip.text: curveVisible(modelData.channelId) ? "Hide curve" : "Show curve"
                    }
                }
            }
        }

        Canvas {
            id: animationSurface
            objectName: "animationSurface"
            x: labelWidth
            y: 0
            width: animationPanel.timelineWidth
            height: editor.height
            antialiasing: true
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                ctx.fillStyle = theme.panel;
                ctx.fillRect(0, 0, width, height);
                ctx.save();
                ctx.beginPath();
                ctx.rect(0, rulerHeight, width, Math.max(0, height - rulerHeight));
                ctx.clip();
                if (viewMode === "curves")
                    drawCurves(ctx);
                else
                    drawTrack(ctx);
                if (boxSelecting) {
                    var left = Math.min(boxStartX, boxEndX), top = Math.min(boxStartY, boxEndY);
                    ctx.fillStyle = theme.accent;
                    ctx.globalAlpha = 0.13;
                    ctx.fillRect(left, top, Math.abs(boxEndX - boxStartX), Math.abs(boxEndY - boxStartY));
                    ctx.globalAlpha = 1;
                    ctx.strokeStyle = theme.accent;
                    ctx.strokeRect(left, top, Math.abs(boxEndX - boxStartX), Math.abs(boxEndY - boxStartY));
                }
                ctx.restore();
                drawRuler(ctx);
            }
            MouseArea {
                id: surfaceMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                hoverEnabled: true
                preventStealing: true
                property real pressX: 0
                property real pressY: 0
                property real panSpan: 0
                property real pressKeyTime: 0
                onPressed: function (mouse) {
                    animationPanel.forceActiveFocus();
                    pressX = mouse.x;
                    pressY = mouse.y;
                    if (mouse.button === Qt.RightButton) {
                        cancelPreview();
                        var contextHit = hitKey(mouse.x, mouse.y);
                        if (contextHit)
                            showKeyContext(contextHit, animationSurface.x + mouse.x, mouse.y);
                        else
                            openEditorMenu(animationSurface);
                        return;
                    }
                    if (mouse.button === Qt.MiddleButton) {
                        gestureState = "panning";
                        panAnchorFrame = viewStart;
                        panSpan = frameSpan;
                        panValueMin = valueMin;
                        panValueMax = valueMax;
                        return;
                    }
                    if (mouse.y < rulerHeight) {
                        gestureState = "scrubbing";
                        setFrame(xToTime(mouse.x));
                        return;
                    }
                    var hit = hitKey(mouse.x, mouse.y);
                    var tangent = tangentHit(mouse.x, mouse.y);
                    if (tangent && (!hit || tangent.distance < hit.distance)) {
                        if (!model.beginGesture())
                            return;
                        tangentPreviewId = tangent.id;
                        tangentPreviewSide = tangent.side;
                        tangentPreviewSlope = Number(tangent.side === "out" ? tangent.key.outSlope : tangent.key.inSlope);
                        gestureState = "tangent";
                        animationSurface.requestPaint();
                        return;
                    }
                    if (hit) {
                        var wasSelected = isSelectedKey(hit.key.id);
                        pendingToggleKey = "";
                        if (mouse.modifiers & Qt.ShiftModifier) {
                            if (wasSelected)
                                pendingToggleKey = String(hit.key.id);
                            else
                                selectKey(hit.key.id, true, false);
                        } else if (!wasSelected) {
                            selectKey(hit.key.id, false, false);
                        }
                        pressKeyTime = Number(hit.key.time);
                        dragConstrained = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                        beginPreview(selectedKeys(), "key");
                        return;
                    }
                    if (viewMode === "curves" && currentAlt(mouse.modifiers)) {
                        insertCurveKey(curveHit(mouse.x, mouse.y));
                        return;
                    }
                    if (viewMode === "track") {
                        var index = Math.floor((mouse.y - rulerHeight + scrollY) / rowHeight);
                        if (index >= 0 && index < rows.length) {
                            var row = rows[index], ids = row.parent ? idsForNode(row.nodeId) : idsForChannels([row.channel]);
                            var lo = Infinity, hi = -Infinity;
                            for (var i = 0; i < ids.length; ++i) {
                                var record = keyRecord(ids[i]);
                                if (!record)
                                    continue;
                                lo = Math.min(lo, Number(record.key.time));
                                hi = Math.max(hi, Number(record.key.time));
                            }
                            if (isFinite(lo) && mouse.x >= timeToX(lo) - 8 && mouse.x <= timeToX(hi) + 8 && Math.abs(mouse.y - rowY(index)) <= 11) {
                                if (row.parent)
                                    selectNode(row.nodeId);
                                else
                                    selectChannel(row.channel, false, false);
                                dragConstrained = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                                beginPreview(ids, row.parent ? "node band" : "parameter span");
                                return;
                            }
                        }
                    }
                    boxSelecting = true;
                    gestureState = "box selecting";
                    boxStartX = boxEndX = mouse.x;
                    boxStartY = boxEndY = mouse.y;
                    animationSurface.requestPaint();
                }
                onPositionChanged: function (mouse) {
                    if (!pressed) {
                        var hoverHit = hitKey(mouse.x, mouse.y);
                        var nextHover = hoverHit ? String(hoverHit.key.id) : "";
                        if (hoverKey !== nextHover) {
                            hoverKey = nextHover;
                            animationSurface.requestPaint();
                        }
                        return;
                    }
                    if (gestureState === "panning") {
                        var delta = (mouse.x - pressX) / pixelsPerFrame;
                        viewStart = panAnchorFrame - delta;
                        viewEnd = viewStart + panSpan;
                        if (viewMode === "curves") {
                            var dv = (mouse.y - pressY) / Math.max(1, height - rulerHeight) * (panValueMax - panValueMin);
                            valueMin = panValueMin + dv;
                            valueMax = panValueMax + dv;
                        }
                    } else if (gestureState === "scrubbing") {
                        setFrame(xToTime(mouse.x));
                    } else if (gestureState === "tangent") {
                        var rec = keyRecord(tangentPreviewId);
                        if (rec) {
                            var dtTangent = xToTime(mouse.x) - displayedTime(rec.key);
                            if ((tangentPreviewSide === "in" && dtTangent < -0.01) || (tangentPreviewSide === "out" && dtTangent > 0.01))
                                tangentPreviewSlope = (yToValue(mouse.y) - displayedValue(rec.key)) / dtTangent;
                        }
                    } else if (previewIds.length) {
                        var distance = Math.hypot(mouse.x - pressX, mouse.y - pressY);
                        if (distance < gestureThreshold)
                            return;
                        dragMoved = true;
                        if (dragConstrained && dragAxis.length === 0)
                            dragAxis = viewMode === "curves" ? (Math.abs(mouse.x - pressX) >= Math.abs(mouse.y - pressY) ? "time" : "value") : "time";
                        var rawDt = (mouse.x - pressX) / pixelsPerFrame;
                        var dtMove = currentAlt(mouse.modifiers) ? rawDt : gestureState === "key" ? Math.round(pressKeyTime + rawDt) - pressKeyTime : Math.round(rawDt);
                        var dvMove = viewMode === "curves" && gestureState === "key" ? yToValue(mouse.y) - yToValue(pressY) : 0;
                        if (dragAxis === "time")
                            dvMove = 0;
                        else if (dragAxis === "value")
                            dtMove = 0;
                        previewDeltaTime = dtMove;
                        previewDeltaValue = dvMove;
                    } else if (boxSelecting) {
                        boxEndX = mouse.x;
                        boxEndY = mouse.y;
                    }
                    animationSurface.requestPaint();
                }
                onReleased: function (mouse) {
                    if (pendingToggleKey.length && !dragMoved)
                        selectKey(pendingToggleKey, true, true);
                    if (boxSelecting) {
                        selectBox();
                        cancelPreview();
                    } else if (gestureState === "panning" || gestureState === "scrubbing") {
                        cancelPreview();
                    } else {
                        commitPreview();
                    }
                }
                onCanceled: cancelPreview()
                onWheel: function (wheel) {
                    var anchor = xToTime(wheel.x);
                    var delta = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y / 120 * 53;
                    var span = Math.max(2, Math.min(100000, frameSpan / Math.exp(delta * 0.002)));
                    var fraction = wheel.x / Math.max(1, width);
                    viewStart = anchor - span * fraction;
                    viewEnd = viewStart + span;
                    animationSurface.requestPaint();
                    wheel.accepted = true;
                }
            }
        }

        Rectangle {
            id: treeDivider
            objectName: "animationTreeDivider"
            x: labelWidth - 2
            y: 0
            width: 5
            height: parent.height
            color: dividerMouse.containsMouse ? theme.accent : theme.border
            opacity: dividerMouse.pressed ? 0.9 : 0.65
            MouseArea {
                id: dividerMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SplitHCursor
                onPositionChanged: function (mouse) {
                    if (pressed)
                        treeWidth = Math.max(118, Math.min(Math.max(118, editor.width - 180), mouse.x + treeDivider.x + 2));
                }
            }
        }

        Rectangle {
            id: errorToast
            objectName: "animationErrorToast"
            visible: transientError.length > 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            width: Math.min(280, parent.width - 16)
            height: 28
            radius: 3
            color: "#603a38"
            border.color: "#9b625d"
            z: 4
            Text {
                anchors.fill: parent
                anchors.margins: 6
                text: transientError
                color: "#f4d4d1"
                elide: Text.ElideRight
                font.pixelSize: 10
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: model && !model.available
        text: "Animation network unavailable"
        color: theme.muted
        font.pixelSize: theme.fontSize
    }

    Timer {
        id: errorToastTimer
        interval: 2600
        onTriggered: transientError = ""
    }

    Menu {
        id: keyContextMenu
        objectName: "animationKeyContextMenu"
        MenuItem {
            objectName: "animationEditKey"
            text: "Edit key…"
            enabled: !!keyForContext() && isNumericChannel(keyForContext().channel)
            onTriggered: openExactEditor()
        }
        Menu {
            objectName: "animationInterpolationMenu"
            title: "Interpolation"
            MenuItem {
                objectName: "animationLinear"
                text: "Linear"
                onTriggered: applyInterpolation("linear")
            }
            MenuItem {
                objectName: "animationHold"
                text: "Hold"
                onTriggered: applyInterpolation("hold")
            }
            MenuItem {
                objectName: "animationBezier"
                text: "Bézier"
                onTriggered: applyInterpolation("bezier")
            }
        }
        MenuSeparator {
        }
        MenuItem {
            objectName: "animationSmooth"
            text: "Smooth tangents"
            enabled: !!keyForContext() && isNumericChannel(keyForContext().channel)
            onTriggered: applyTangentMode("smooth")
        }
        MenuItem {
            objectName: "animationBroken"
            text: "Broken tangents"
            enabled: !!keyForContext() && isNumericChannel(keyForContext().channel)
            onTriggered: applyTangentMode("broken")
        }
    }

    Popup {
        id: keyEditPopup
        objectName: "animationEditKeyPopup"
        property string errorMessage: ""
        readonly property bool smooth: {
            var record = keyForContext();
            return record && record.key.tangentMode === "smooth";
        }
        readonly property bool showSlopes: {
            var record = keyForContext();
            if (!record)
                return false;
            var index = record.channel.keys.indexOf(record.key);
            return record.key.interpolation === "bezier" || (index > 0 && record.channel.keys[index - 1].interpolation === "bezier");
        }
        width: 238
        height: topPadding + bottomPadding + exactFields.implicitHeight + 36
        padding: 12
        modal: false
        onClosed: if (model)
            model.cancelGesture()
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Keep the reference placement when it fits; a short dock must not put
        // Apply/Cancel below the native window's edge.
        parent: Overlay.overlay
        readonly property point dockPosition: animationPanel.mapToItem(parent, Math.max(4, animationPanel.width - width - 12), Math.max(4, (animationPanel.height - height) / 2))
        x: Math.max(4, Math.min(parent.width - width - 4, dockPosition.x))
        y: Math.max(4, Math.min(parent.height - height - 4, dockPosition.y))
        background: Rectangle {
            color: theme.header
            border.color: theme.border
            radius: theme.smallRadius
        }
        Column {
            id: exactFields
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            spacing: 5
            Text {
                text: "Edit key"
                color: theme.text
                font.pixelSize: 11
                font.weight: Font.Medium
            }
            Row {
                spacing: 6
                Text {
                    text: "Time"
                    color: theme.muted
                    width: 56
                    height: 24
                    font.pixelSize: 10
                    verticalAlignment: Text.AlignVCenter
                }
                TextField {
                    id: exactTime
                    objectName: "animationKeyTime"
                    width: 145
                    implicitHeight: 24
                    validator: DoubleValidator {
                    }
                }
            }
            Row {
                spacing: 6
                Text {
                    text: "Value"
                    color: theme.muted
                    width: 56
                    height: 24
                    font.pixelSize: 10
                    verticalAlignment: Text.AlignVCenter
                }
                TextField {
                    id: exactValue
                    objectName: "animationKeyValue"
                    width: 145
                    implicitHeight: 24
                    validator: DoubleValidator {
                    }
                }
            }
            Row {
                visible: keyEditPopup.showSlopes
                spacing: 6
                Text {
                    text: keyEditPopup.smooth ? "Slope" : "In slope"
                    color: theme.muted
                    width: 56
                    height: 24
                    font.pixelSize: 10
                    verticalAlignment: Text.AlignVCenter
                }
                TextField {
                    id: exactInSlope
                    objectName: "animationKeyInSlope"
                    width: 145
                    implicitHeight: 24
                    validator: DoubleValidator {
                    }
                }
            }
            Row {
                visible: keyEditPopup.showSlopes && !keyEditPopup.smooth
                spacing: 6
                Text {
                    text: "Out slope"
                    color: theme.muted
                    width: 56
                    height: 24
                    font.pixelSize: 10
                    verticalAlignment: Text.AlignVCenter
                }
                TextField {
                    id: exactOutSlope
                    objectName: "animationKeyOutSlope"
                    width: 145
                    implicitHeight: 24
                    validator: DoubleValidator {
                    }
                }
            }
            Text {
                width: parent.width
                visible: keyEditPopup.errorMessage.length > 0
                text: keyEditPopup.errorMessage
                color: "#e9a29b"
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
        }
        Row {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: 6
            Button {
                objectName: "animationKeyApply"
                text: "Apply"
                implicitWidth: 64
                implicitHeight: 24
                onClicked: applyExactEditor()
            }
            Button {
                objectName: "animationKeyCancel"
                text: "Cancel"
                implicitWidth: 64
                implicitHeight: 24
                onClicked: keyEditPopup.close()
            }
        }
    }

    Menu {
        id: channelMenu
        objectName: "animationChannelContextMenu"
        MenuItem {
            objectName: "animationHideCurves"
            text: "Hide curves"
            onTriggered: setCurvesVisible(contextCurveIds, false)
        }
        MenuItem {
            objectName: "animationShowCurves"
            text: "Show curves"
            onTriggered: setCurvesVisible(contextCurveIds, true)
        }
        MenuItem {
            objectName: "animationIsolateCurves"
            text: "Isolate curves"
            onTriggered: isolateCurves(contextCurveIds)
        }
        MenuSeparator {
        }
        MenuItem {
            objectName: "animationShowAllCurves"
            text: "Show all curves"
            onTriggered: showAllCurves()
        }
    }

    Menu {
        id: editorMenu
        objectName: "animationEditorContextMenu"
        MenuItem {
            objectName: "animationFrameSelected"
            text: "Frame selected"
            onTriggered: frameSelected()
        }
        MenuItem {
            objectName: "animationFrameAllAction"
            text: "Frame all"
            onTriggered: frameAll()
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Edit selected key…"
            enabled: selectedKeys().length === 1 && !!keyRecord(selectedKeys()[0]) && isNumericChannel(keyRecord(selectedKeys()[0]).channel)
            onTriggered: {
                contextKeyId = String(selectedKeys()[0]);
                openExactEditor();
            }
        }
        MenuItem {
            text: "Isolate selected curves"
            enabled: viewMode === "curves" && selectedChannels().length > 0
            onTriggered: isolateCurves(selectedChannels())
        }
        MenuItem {
            text: "Show all curves"
            onTriggered: showAllCurves()
        }
        MenuSeparator {
        }
        Menu {
            title: "Interpolation"
            MenuItem {
                text: "Linear"
                onTriggered: applyInterpolation("linear")
            }
            MenuItem {
                text: "Hold"
                onTriggered: applyInterpolation("hold")
            }
            MenuItem {
                text: "Bézier"
                onTriggered: applyInterpolation("bezier")
            }
        }
        MenuItem {
            text: "Smooth tangents"
            onTriggered: applyTangentMode("smooth")
        }
        MenuItem {
            text: "Broken tangents"
            onTriggered: applyTangentMode("broken")
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Undo"
            enabled: model && model.canUndo
            onTriggered: model.undo()
        }
        MenuItem {
            text: "Redo"
            enabled: model && model.canRedo
            onTriggered: model.redo()
        }
    }

    Component {
        id: headerToolsComponent
        AnimationHeaderTools {
            theme: animationPanel.theme
            editor: animationPanel
        }
    }

    Keys.onPressed: function (event) {
        var modifiers = event.modifiers;
        var control = (modifiers & Qt.ControlModifier) !== 0 || (modifiers & Qt.MetaModifier) !== 0;
        if (event.key === Qt.Key_Escape) {
            if (keyEditPopup.visible)
                keyEditPopup.close();
            keyContextMenu.close();
            editorMenu.close();
            cancelPreview();
            event.accepted = true;
        } else if (control && event.key === Qt.Key_Z) {
            if ((modifiers & Qt.ShiftModifier) !== 0)
                model.redo();
            else
                model.undo();
            event.accepted = true;
        } else if (event.key === Qt.Key_F) {
            frameSelected();
            event.accepted = true;
        } else if (event.key === Qt.Key_Home) {
            frameAll();
            event.accepted = true;
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            if (selectedKeys().length > 0 && !model.removeKeys(selectedKeys()))
                showTransientError(model.error);
            event.accepted = true;
        }
    }

    Connections {
        target: theme
        function onPresetChanged() {
            animationSurface.requestPaint();
        }
        function onAccentChanged() {
            animationSurface.requestPaint();
        }
    }

    Connections {
        target: model
        function onProjectChanged() {
            animationPanel.cancelPreview();
            keyEditPopup.close();
            animationPanel.selectedKeyIds = [];
            animationPanel.selectedChannelIds = [];
            animationPanel.targetNodeId = "";
            animationPanel.hiddenCurveIds = ({});
            animationPanel.collapsedNodes = ({});
            animationPanel.groupStates = ({});
            animationPanel.networkId = controller.rootNetworkId;
            model.networkId = animationPanel.networkId;
        }
        function onChannelsChanged() {
            animationSurface.requestPaint();
        }
    }

    onViewModeChanged: {
        queueSave();
        cancelPreview();
        if (viewMode === "curves")
            fitValues([]);
        animationSurface.requestPaint();
    }
    onRowsChanged: animationSurface.requestPaint()
    onChannelsChanged: {
        selectedKeyIds = selectedKeyIds.filter(function (id) {
                return keyRecord(id) !== null;
            });
        selectedChannelIds = selectedChannelIds.filter(function (id) {
                return channelById(id) !== null;
            });
        animationSurface.requestPaint();
    }
    Component.onCompleted: {
        model = controller.createAnimationModel(animationPanel);
        networkId = controller.rootNetworkId;
        Qt.callLater(restoreState);
    }
    Component.onDestruction: saveState()
}
