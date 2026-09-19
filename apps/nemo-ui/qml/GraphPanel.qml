import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// The node graph panel is chrome, input plumbing, popups and shortcuts, and
// nothing else. Every pointer event is forwarded verbatim to the
// GraphInteraction core, which owns the scene, the view transform, the
// selection, the hover result and the active gesture session. The panel renders
// what the core publishes: the paint item, the enter-subnet chips, the marquee
// and the popups. It computes no graph geometry, resolves no pick and iterates
// no graph element; the view, the selection and the last-click record it
// exposes are read-only aliases of the core's own properties.
FocusScope {
    id: graphPanel
    objectName: "graphPanel"
    focus: true
    activeFocusOnTab: true
    clip: true

    property string panelId: ""
    property string panelGroup: "A"
    property var panelState: ({})
    property var contextRouter: null
    property var theme
    property var workspace
    property bool toolsOpen: false
    readonly property var controller: viewerController
    property var categories: []
    // A scope path is explicit occurrence ancestry. Definitions can be used by
    // multiple instances, so navigation never infers a parent from a definition
    // identity.
    property var scopePath: []
    property var scopeBreadcrumbs: []
    property bool stateReady: false
    // The network displayed here. Scope navigation is presentation state, never
    // document data: the core owns the scene the network id names.
    property string graphNetworkId: ""
    readonly property bool graphAvailable: interaction.available
    // The node the core reported under a right press. The core has already made
    // it the selection by the time the menu opens.
    property string contextNodeId: ""
    property string contextTarget: ""
    property var contextNodeInfo: ({})
    // The last pointer position the surface forwarded, in surface coordinates.
    // The search popup is placed relative to it; the core owns the pointer for
    // every gesture decision.
    property real pointerX: 0
    property real pointerY: 0
    property point searchAnchor: Qt.point(0, 0)

    // Everything about the graph the native UI tests read is the core's. The
    // panel holds no second copy of the view, the selection or the last click.
    readonly property real zoom: interaction.zoom
    readonly property real panX: interaction.panX
    readonly property real panY: interaction.panY
    readonly property bool lastClickValid: interaction.lastClickValid
    readonly property real lastClickX: interaction.lastClickX
    readonly property real lastClickY: interaction.lastClickY
    readonly property var selectedNodeIds: interaction.selectedNodeIds
    // Only a gesture that can publish an authored edit takes Undo away from the
    // document: node move, wire connect/rewire/disconnect, reroute and the pipe
    // pull. The core decides which sessions those are. Navigation (pan, subnet
    // entry), selection (marquee, toggle) and view changes stay presentation
    // state and never claim Undo.
    readonly property bool historyGestureActive: interaction.historyGestureActive
    onHistoryGestureActiveChanged: historyController.setGesture(graphPanel, historyGestureActive)
    // The shared history adapter routes a preview-only Undo to the registered
    // gesture owner by this name: the gesture is dropped, so the release that
    // follows finds nothing to commit and records no history entry.
    function cancelHistoryGesture() {
        interaction.cancelGesture();
    }

    property Component headerTools: Component {
        RowLayout {
            spacing: 4
            ChromeButton {
                objectName: "graphFrameAll"
                theme: graphPanel.theme
                text: "Frame all"
                implicitHeight: 24
                implicitWidth: 67
                padding: 5
                onClicked: interaction.frameAll()
            }
            ChromeButton {
                id: toolsButton
                objectName: "graphToolsButton"
                theme: graphPanel.theme
                text: "Tools"
                implicitHeight: 24
                implicitWidth: 49
                padding: 5
                onClicked: graphPanel.toolsOpen = !graphPanel.toolsOpen
                background: Rectangle {
                    radius: 4
                    color: toolsButton.down ? graphPanel.theme.hover : toolsButton.hovered ? graphPanel.theme.raised : graphPanel.theme.panel
                    border.color: graphPanel.toolsOpen ? graphPanel.theme.accent : graphPanel.theme.border
                }
            }
            Item {
                Layout.fillWidth: true
            }
            Text {
                objectName: "graphZoomLabel"
                text: Math.round(graphPanel.zoom * 100) + "%"
                color: graphPanel.theme.muted
                font.pixelSize: 11
                verticalAlignment: Text.AlignVCenter
                Layout.preferredWidth: 38
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // GraphItem consumes this plain map on the GUI thread; no Theme QObject is
    // handed to the render thread.
    readonly property var presentationStyle: ({
            "accent": graphPanel.theme ? graphPanel.theme.accent : "#3485f6",
            "border": graphPanel.theme ? graphPanel.theme.border : "#30343a",
            "muted": graphPanel.theme ? graphPanel.theme.muted : "#979ea8",
            "panel": graphPanel.theme ? graphPanel.theme.panel : "#1e2023",
            "node": graphPanel.theme ? graphPanel.theme.node : "#2a2e33",
            "fontSize": graphPanel.theme ? graphPanel.theme.fontSize : 11
        })

    // The graph panel always starts from the document's root network; scope
    // navigation below is presentation state, never document data.
    function baseNetworkId() {
        return controller && controller.rootNetworkId !== undefined ? String(controller.rootNetworkId || "") : "";
    }
    function viewerShortcutEnabled() {
        return graphPanel.activeFocus && selectedNodeIds.length === 1 && !interaction.gestureActive && !graphSearchPopup.opened && !graphContextMenu.opened;
    }
    function assignViewerShortcut(viewerIndex) {
        if (!viewerShortcutEnabled())
            return;
        interaction.assignViewer(Number(viewerIndex));
    }
    function targetNetworkId() {
        if (scopePath.length)
            return String(scopePath[scopePath.length - 1].networkId || "");
        return baseNetworkId();
    }
    function pathRootMatchesBase(path) {
        if (!path || !path.length)
            return true;
        return String(path[0].parentNetworkId || baseNetworkId()) === baseNetworkId();
    }
    function scopeKey(id) {
        return String(id || "");
    }
    function displayCategory(group) {
        if (group === "I/O")
            return "IO";
        if (group === "Compositing")
            return "Merge";
        return theme && theme.nodeCategoryColors && theme.nodeCategoryColors[group] !== undefined ? group : "Utility";
    }
    // A plain copy of the core's selection, so a panel-state record never aliases
    // the sequence the core publishes.
    function selectionIds() {
        var ids = [], source = selectedNodeIds;
        for (var i = 0; i < source.length; ++i)
            ids.push(String(source[i]));
        return ids;
    }
    // The node the context menu acts on: the node the core reported under the
    // pointer, or the sole selection when the menu was opened without one.
    function contextTargetId() {
        var candidate = contextNodeId;
        if (!candidate && selectedNodeIds.length === 1)
            candidate = selectedNodeIds[0];
        return String(candidate || "");
    }
    function contextSubnetId() {
        return contextNodeInfo.isSubnet === true ? contextTarget : "";
    }
    // The context target and its node facts. The menus read these records rather
    // than calling the core inside a binding: a binding is re-evaluated on the
    // notify signal of every property it read, and a Q_INVOKABLE call registers
    // no dependency at all, so a menu built from a bare call goes stale — a
    // duplicated subnet left "Make independent" disabled forever.
    function refreshContextTarget() {
        contextTarget = contextTargetId();
        contextNodeInfo = interaction.nodeInfo(contextTarget);
    }
    // Scope entry is explicit occurrence ancestry: definitions can be used by
    // multiple instances, so the path records the occurrence that was entered.
    function enterSubnet(id) {
        var info = interaction.nodeInfo(id);
        if (info.exists !== true || info.isSubnet !== true)
            return false;
        var next = scopePath.slice();
        next.push({
                "parentNetworkId": graphNetworkId,
                "networkId": String(info.definition),
                "instanceId": String(info.instance),
                "nodeId": String(id),
                "name": String(info.name || info.type || "Node")
            });
        scopePath = next;
        switchNetwork();
        return true;
    }
    function navigateBreadcrumb(index) {
        var target = Number(index);
        if (!isFinite(target) || target < 0)
            return;
        if (target === 0)
            scopePath = [];
        else
            scopePath = scopePath.slice(0, target);
        switchNetwork();
    }
    function collapseSelection() {
        if (!interaction.collapseSelection())
            return;
        savePanelState();
    }
    // The scene point of a surface point. The transform is the core's, so the
    // screenshot and test coordinate agrees with the pick by construction.
    function scenePoint(px, py) {
        return interaction.scenePoint(px, py);
    }

    function panelStateCopy() {
        var source = panelState || ({});
        if (workspace && panelId && workspace.panelState)
            source = workspace.panelState(panelId) || source;
        var copy = {};
        for (var key in source)
            copy[key] = source[key];
        return copy;
    }
    function savePanelState() {
        if (!workspace || !panelId || !workspace.setPanelState || !stateReady)
            return;
        var merged = panelStateCopy(), views = {}, selections = {};
        var savedViews = merged.graphViews || ({}), savedSelections = merged.graphSelections || ({});
        for (var key in savedViews)
            views[key] = savedViews[key];
        for (var selectedKey in savedSelections)
            selections[selectedKey] = savedSelections[selectedKey];
        views[scopeKey(graphNetworkId)] = {
            "zoom": Number(zoom),
            "panX": Number(panX),
            "panY": Number(panY),
            "lastClickValid": lastClickValid,
            "lastClickX": Number(lastClickX),
            "lastClickY": Number(lastClickY)
        };
        selections[scopeKey(graphNetworkId)] = selectionIds();
        merged.graphViews = views;
        merged.graphSelections = selections;
        merged.scopeRootNetworkId = baseNetworkId();
        merged.scopePath = scopePath.slice();
        // Migrate the former root-only view record into the scoped record.
        delete merged.zoom;
        delete merged.panX;
        delete merged.panY;
        delete merged.lastClickValid;
        delete merged.lastClickX;
        delete merged.lastClickY;
        if (JSON.stringify(merged) === JSON.stringify(panelStateCopy()))
            return;
        workspace.setPanelState(panelId, merged);
    }
    function restoreScopePath() {
        var state = panelStateCopy(), saved = state.scopePath || [];
        if (!Array.isArray(saved) || !pathRootMatchesBase(saved)) {
            scopePath = [];
            return;
        }
        var restored = [], parentNetwork = baseNetworkId();
        for (var i = 0; i < saved.length; ++i) {
            var entry = saved[i] || ({});
            if (!entry.networkId || !entry.instanceId || !entry.name || String(entry.parentNetworkId || parentNetwork) !== parentNetwork)
                break;
            restored.push({
                    "parentNetworkId": parentNetwork,
                    "networkId": String(entry.networkId),
                    "instanceId": String(entry.instanceId),
                    "nodeId": String(entry.nodeId || ""),
                    "name": String(entry.name)
                });
            parentNetwork = String(entry.networkId);
        }
        scopePath = restored;
    }
    function reconcileScopePath() {
        var scope = controller.graphScope(baseNetworkId(), scopePath);
        var valid = scope.path || [];
        if (JSON.stringify(valid) !== JSON.stringify(scopePath))
            scopePath = valid;
        scopeBreadcrumbs = scope.breadcrumbs || [];
    }
    // `autoFrameScope` is true only when a network scope is entered: a restored
    // view record is used as stored, and only a scope that has none is framed.
    // A panel-state write must never re-frame a view the artist is using, so the
    // request waits for the surface's size and a later adoption wins over it.
    function restoreScopeState(autoFrameScope) {
        var state = panelState || ({}), views = state.graphViews || ({}), selections = state.graphSelections || ({});
        var view = views[scopeKey(graphNetworkId)];
        if (!view && graphNetworkId === String(controller.rootNetworkId) && state.zoom !== undefined)
            view = state;
        if (view && isFinite(Number(view.zoom)) && isFinite(Number(view.panX)) && isFinite(Number(view.panY))) {
            interaction.adoptView(Number(view.zoom), Number(view.panX), Number(view.panY), view.lastClickValid === true,
                    Number(view.lastClickX) || 0, Number(view.lastClickY) || 0);
        } else {
            interaction.clearLastClick();
            if (autoFrameScope)
                interaction.requestFrame();
        }
        interaction.adoptSelection(selections[scopeKey(graphNetworkId)] || []);
    }
    function switchNetwork() {
        reconcileScopePath();
        var nextNetwork = targetNetworkId();
        if (stateReady && nextNetwork === graphNetworkId) {
            var refreshed = graphNetworkId.length ? controller.graphSnapshot(graphNetworkId) : ({});
            // Undo can remove the active child definition. Walk the explicit
            // occurrence path back to its surviving parent rather than falling
            // through to an unrelated definition or root graph.
            if (refreshed.available !== true && scopePath.length) {
                scopePath = scopePath.slice(0, scopePath.length - 1);
                switchNetwork();
                return;
            }
            interaction.setSnapshot(graphNetworkId, refreshed, controller.graphRevision());
            refreshSnapshots();
            return;
        }
        if (stateReady && graphNetworkId.length)
            savePanelState();
        interaction.cancelGesture();
        graphSearchPopup.close();
        graphContextMenu.close();
        graphNetworkId = nextNetwork;
        var snapshot = ({});
        if (graphNetworkId.length && controller && controller.graphSnapshot)
            snapshot = controller.graphSnapshot(graphNetworkId) || ({});
        while (snapshot.available !== true && scopePath.length) {
            scopePath = scopePath.slice(0, scopePath.length - 1);
            graphNetworkId = targetNetworkId();
            snapshot = controller.graphSnapshot(graphNetworkId) || ({});
        }
        stateReady = false;
        reconcileScopePath();
        interaction.setSnapshot(graphNetworkId, snapshot, controller.graphRevision());
        restoreScopeState(true);
        refreshSnapshots();
        stateReady = true;
    }
    // The core names the node, plans its position and the shift it owes the
    // downstream nodes, and publishes the new selection.
    function createFromDescriptor(descriptor) {
        interaction.createNode(descriptor);
        forceActiveFocus();
    }
    function filteredCatalog(needle) {
        var result = [], value = String(needle || "").toLowerCase(), catalog = controller.nodeCatalog || [];
        for (var i = 0; i < catalog.length; ++i)
            if (String(catalog[i].displayName || catalog[i].type).toLowerCase().indexOf(value) >= 0)
                result.push(catalog[i]);
        return result;
    }
    // The creation catalog is panel chrome: the core owns the document, the
    // panel only groups the descriptors its menus offer.
    function refreshSnapshots() {
        var catalog = controller.nodeCatalog || [], groups = {};
        for (var i = 0; i < catalog.length; ++i) {
            var descriptor = catalog[i];
            var label = displayCategory(descriptor.group);
            if (!groups[label])
                groups[label] = {
                    "label": label,
                    "nodes": []
                };
            groups[label].nodes.push(descriptor);
        }
        var ordered = ["Color", "Distort", "Filter", "Utility", "Merge", "IO"], nextGroups = [];
        for (var g = 0; g < ordered.length; ++g)
            if (groups[ordered[g]])
                nextGroups.push(groups[ordered[g]]);
        for (var key in groups)
            if (ordered.indexOf(key) < 0)
                nextGroups.push(groups[key]);
        categories = nextGroups;
    }

    // The interaction core: the panel forwards to it and renders what it
    // publishes. It is declared before the paint item so the item can bind to it.
    GraphInteraction {
        id: interaction
        objectName: "graphInteraction"
        controller: graphPanel.controller
        onViewSettled: graphPanel.savePanelState()
        onContextMenuRequested: function (x, y, nodeId) {
            graphPanel.contextNodeId = String(nodeId || "");
            graphContextMenu.x = Math.max(2, Math.min(graphSurface.width - 190, x));
            graphContextMenu.y = Math.max(2, Math.min(graphSurface.height - 120, y));
            graphContextMenu.open();
        }
        onInspectorRequested: function (nodeId) {
            if (contextRouter)
                contextRouter.requestInspector(graphPanel.panelGroup, graphPanel.graphNetworkId, String(nodeId));
        }
        onScopeEntryRequested: function (nodeId) {
            graphPanel.enterSubnet(String(nodeId));
        }
        onSceneChanged: graphPanel.refreshContextTarget()
    }

    Connections {
        target: controller
        function onGraphChanged() {
            graphPanel.switchNetwork();
        }
        function onCatalogChanged() {
            graphPanel.refreshSnapshots();
        }
    }

    // A panel-state write is a view/preference record, never a graph change: the
    // panel picks up what it displays (view, selection) and rebuilds nothing.
    // Another panel's write — a viewer pan, an inspector arrangement — must not
    // cost this panel its catalog model, and a write must never disturb a
    // gesture that started since.
    onContextNodeIdChanged: refreshContextTarget()
    onSelectedNodeIdsChanged: refreshContextTarget()
    onPanelStateChanged: {
        if (graphPanel.stateReady && !interaction.gestureActive)
            graphPanel.restoreScopeState(false);
    }
    Component.onCompleted: {
        restoreScopePath();
        switchNetwork();
        historyController.setGesture(graphPanel, historyGestureActive);
    }
    // Teardown drops the registration so reopening a panel never accumulates
    // competing owners.
    Component.onDestruction: historyController.setGesture(graphPanel, false)

    // A view still in motion when the application closes is still the view the
    // project should record: the close is a boundary, not a teardown of a
    // record that no longer belongs to this panel.
    Connections {
        target: graphPanel.Window.window
        function onClosing() { graphPanel.savePanelState(); }
    }

    Rectangle {
        anchors.fill: parent
        color: graphPanel.theme.background
        objectName: "graphCanvas"

        Rectangle {
            id: toolsBar
            objectName: "graphToolsBar"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: categoryFlow.implicitHeight + 8
            visible: graphPanel.toolsOpen
            z: 20
            color: Qt.rgba(graphPanel.theme.panel.r, graphPanel.theme.panel.g, graphPanel.theme.panel.b, 0.97)
            border.color: graphPanel.theme.border
            Flow {
                id: categoryFlow
                anchors.fill: parent
                anchors.margins: 4
                spacing: 3
                Repeater {
                    model: graphPanel.categories
                    delegate: ChromeButton {
                        required property var modelData
                        theme: graphPanel.theme
                        objectName: "toolCategory_" + modelData.label
                        text: modelData.label === "IO" ? "I/O" : modelData.label
                        enabled: modelData.nodes.length > 0
                        implicitHeight: 23
                        implicitWidth: Math.max(48, text.length * 7 + 18)
                        padding: 4
                        onClicked: {
                            interaction.beginPlacement();
                            categoryMenu.open();
                        }
                        Menu {
                            id: categoryMenu
                            y: parent.height
                            Repeater {
                                model: modelData.nodes
                                delegate: MenuItem {
                                    required property var modelData
                                    objectName: "toolNode_" + modelData.type
                                    text: modelData.displayName
                                    onTriggered: graphPanel.createFromDescriptor(modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            id: breadcrumb
            objectName: "graphBreadcrumbBar"
            anchors.top: toolsBar.visible ? toolsBar.bottom : parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 27
            color: Qt.rgba(graphPanel.theme.panel.r, graphPanel.theme.panel.g, graphPanel.theme.panel.b, 0.94)
            border.color: graphPanel.theme.border
            Row {
                anchors.fill: parent
                anchors.leftMargin: 6
                spacing: 2
                Repeater {
                    model: graphPanel.scopeBreadcrumbs
                    delegate: ChromeButton {
                        required property var modelData
                        required property int index
                        objectName: "graphBreadcrumb_" + String(modelData.networkId)
                        theme: graphPanel.theme
                        text: (index > 0 ? "›  " : "") + String(modelData.name || modelData.networkId)
                        flat: true
                        padding: 4
                        implicitHeight: 25
                        onClicked: graphPanel.navigateBreadcrumb(index)
                        background: Rectangle {
                            radius: 3
                            color: parent.hovered ? graphPanel.theme.hover : "transparent"
                        }
                    }
                }
            }
        }
        Item {
            id: graphSurface
            objectName: "graphSurface"
            anchors.top: breadcrumb.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            clip: true
            // The core owns the visible rectangle: it sizes its deferred framing
            // and its paint window from the surface it is given.
            onWidthChanged: interaction.setViewport(width, height)
            onHeightChanged: interaction.setViewport(width, height)
            Component.onCompleted: interaction.setViewport(width, height)
            // The periodic grid is painted off-period once and then translated
            // by the view, so a pan or zoom step moves this item instead of
            // re-rasterising the whole panel area. It is repainted only when
            // its size or the theme colour changes.
            Canvas {
                id: grid
                objectName: "graphGrid"
                renderTarget: Canvas.FramebufferObject
                readonly property int period: 24
                x: Math.round(((graphPanel.panX % period) + period) % period) - period
                y: Math.round(((graphPanel.panY % period) + period) % period) - period
                width: graphSurface.width + period
                height: graphSurface.height + period
                property color lineColor: Qt.rgba(graphPanel.theme.border.r, graphPanel.theme.border.g, graphPanel.theme.border.b, 0.24)
                onLineColorChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.clearRect(0, 0, width, height);
                    ctx.strokeStyle = lineColor;
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    for (var x = 0; x < width; x += period) {
                        ctx.moveTo(Math.round(x) + 0.5, 0);
                        ctx.lineTo(Math.round(x) + 0.5, height);
                    }
                    ;
                    for (var y = 0; y < height; y += period) {
                        ctx.moveTo(0, Math.round(y) + 0.5);
                        ctx.lineTo(width, Math.round(y) + 0.5);
                    }
                    ;
                    ctx.stroke();
                }
            }
            GraphItem {
                id: graphItem
                objectName: "graphItem"
                interaction: interaction
                x: interaction.panX
                y: interaction.panY
                scale: interaction.zoom
                transformOrigin: Item.TopLeft
                width: interaction.sceneWidth
                height: interaction.sceneHeight
                visibleRect: interaction.visibleRect
                categoryColors: graphPanel.theme.nodeCategoryColors
                presentationStyle: graphPanel.presentationStyle
            }
            Repeater {
                model: interaction.enterAffordances
                delegate: Rectangle {
                    required property var modelData
                    objectName: "graphEnterAffordance_" + String(modelData.id)
                    enabled: false
                    x: Number(modelData.x)
                    y: Number(modelData.y)
                    width: Number(modelData.size)
                    height: Number(modelData.size)
                    radius: 3
                    color: graphPanel.theme.raised
                    // Local subnets keep the neutral terminal border; a linked
                    // or shared definition is called out in the accent so the
                    // card distinguishes reuse at a glance.
                    border.color: String(modelData.linkState || "local") === "local" ? graphPanel.theme.border : graphPanel.theme.accent
                    border.width: 1
                    z: 6
                    Text {
                        anchors.centerIn: parent
                        text: ">"
                        color: graphPanel.theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }
                }
            }
            Rectangle {
                id: selectionBox
                objectName: "graphSelectionBox"
                visible: interaction.marqueeVisible
                x: interaction.marqueeRect.x
                y: interaction.marqueeRect.y
                width: interaction.marqueeRect.width
                height: interaction.marqueeRect.height
                color: Qt.rgba(graphPanel.theme.accent.r, graphPanel.theme.accent.g, graphPanel.theme.accent.b, 0.12)
                border.color: graphPanel.theme.accent
                border.width: 1
                z: 5
            }
            MouseArea {
                id: canvasMouse
                objectName: "graphCanvasSurface"
                anchors.fill: parent
                z: 10
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                hoverEnabled: true
                preventStealing: true
                onEntered: interaction.hover()
                onExited: interaction.leave()
                onPressed: function (mouse) {
                    if (contextRouter && panelId)
                        contextRouter.setActivePanel(panelId);
                    forceActiveFocus();
                    pointerX = mouse.x;
                    pointerY = mouse.y;
                    interaction.press(mouse.x, mouse.y, mouse.button, mouse.modifiers);
                    mouse.accepted = true;
                }
                onPositionChanged: function (mouse) {
                    pointerX = mouse.x;
                    pointerY = mouse.y;
                    interaction.move(mouse.x, mouse.y, mouse.modifiers);
                }
                onReleased: function (mouse) {
                    pointerX = mouse.x;
                    pointerY = mouse.y;
                    interaction.release(mouse.x, mouse.y, mouse.button, mouse.modifiers);
                }
                onCanceled: interaction.cancelGesture()
                onDoubleClicked: function (mouse) {
                    interaction.doubleClick(mouse.x, mouse.y);
                }
                onWheel: function (wheel) {
                    interaction.wheel(wheel.pixelDelta.y, wheel.angleDelta.y, wheel.x, wheel.y);
                    wheel.accepted = true;
                }
            }
        }
    }

    Shortcut {
        sequence: "Tab"
        context: Qt.WindowShortcut
        enabled: graphPanel.activeFocus && !interaction.gestureActive && !graphSearchPopup.opened
        onActivated: {
            interaction.beginPlacement();
            graphSearchPopup.open();
            graphSearchField.forceActiveFocus();
        }
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: interaction.gestureActive || graphSearchPopup.opened || graphContextMenu.opened
        onActivated: {
            if (graphSearchPopup.opened)
                graphSearchPopup.close();
            else if (graphContextMenu.opened)
                graphContextMenu.close();
            else
                interaction.cancelGesture();
        }
    }
    Shortcut {
        sequence: "Delete"
        context: Qt.WindowShortcut
        enabled: graphPanel.activeFocus && !interaction.gestureActive && !graphSearchPopup.opened && !graphContextMenu.opened
        onActivated: {
            if (interaction.deleteSelection())
                graphPanel.savePanelState();
        }
    }
    // Graphical copy/paste of a graph selection. The clipboard is panel
    // presentation state; each paste is one shared command and one undo step.
    Shortcut {
        sequences: [StandardKey.Copy]
        context: Qt.WindowShortcut
        enabled: graphPanel.activeFocus && graphPanel.selectedNodeIds.length > 0 && !graphSearchPopup.opened && !graphContextMenu.opened
        onActivated: interaction.copySelection()
    }
    Shortcut {
        sequences: [StandardKey.Paste]
        context: Qt.WindowShortcut
        enabled: graphPanel.activeFocus && !graphSearchPopup.opened && !graphContextMenu.opened
        onActivated: {
            if (interaction.pasteSelection())
                graphPanel.savePanelState();
        }
    }
    // Digits 1..9 attach the single selected node to viewer N. The core command
    // toggles, so pressing the same digit for the attached node detaches it.
    Shortcut {
        sequence: "1"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(0)
    }
    Shortcut {
        sequence: "2"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(1)
    }
    Shortcut {
        sequence: "3"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(2)
    }
    Shortcut {
        sequence: "4"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(3)
    }
    Shortcut {
        sequence: "5"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(4)
    }
    Shortcut {
        sequence: "6"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(5)
    }
    Shortcut {
        sequence: "7"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(6)
    }
    Shortcut {
        sequence: "8"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(7)
    }
    Shortcut {
        sequence: "9"
        context: Qt.WindowShortcut
        enabled: graphPanel.viewerShortcutEnabled()
        onActivated: graphPanel.assignViewerShortcut(8)
    }

    Popup {
        id: graphSearchPopup
        objectName: "graphSearchPopup"
        parent: Overlay.overlay
        width: 190
        height: 214
        padding: 6
        modal: false
        x: Math.max(4, Math.min(Overlay.overlay.width - width - 4, graphPanel.searchAnchor.x - width / 2))
        y: Math.max(4, Math.min(Overlay.overlay.height - height - 4, graphPanel.searchAnchor.y - height / 2))
        closePolicy: Popup.NoAutoClose
        background: Rectangle {
            radius: graphPanel.theme.radius
            color: graphPanel.theme.panel
            border.color: graphPanel.theme.border
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 5
            TextField {
                id: graphSearchField
                objectName: "graphSearchField"
                Layout.fillWidth: true
                implicitHeight: 28
                placeholderText: "Search nodes"
                color: graphPanel.theme.text
                placeholderTextColor: graphPanel.theme.muted
                font.pixelSize: graphPanel.theme.fontSize
                background: Rectangle {
                    radius: graphPanel.theme.smallRadius
                    color: graphPanel.theme.field
                    border.color: graphSearchField.activeFocus ? graphPanel.theme.accent : graphPanel.theme.border
                }
                onTextChanged: {
                    graphSearchList.currentIndex = 0;
                    graphSearchList.model = graphPanel.filteredCatalog(text);
                }
                Keys.onDownPressed: function (event) {
                    graphSearchList.incrementCurrentIndex();
                    event.accepted = true;
                }
                Keys.onUpPressed: function (event) {
                    graphSearchList.decrementCurrentIndex();
                    event.accepted = true;
                }
                Keys.onEscapePressed: function (event) {
                    event.accepted = true;
                    graphSearchPopup.close();
                    graphPanel.forceActiveFocus();
                }
                onAccepted: {
                    if (graphSearchList.currentItem) {
                        graphPanel.createFromDescriptor(graphSearchList.currentItem.modelData);
                        graphSearchPopup.close();
                    }
                }
            }
            ListView {
                id: graphSearchList
                objectName: "graphSearchList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                currentIndex: 0
                model: graphPanel.filteredCatalog("")
                delegate: ItemDelegate {
                    required property var modelData
                    width: graphSearchList.width
                    height: 29
                    text: modelData.displayName
                    highlighted: ListView.isCurrentItem
                    onClicked: {
                        graphPanel.createFromDescriptor(modelData);
                        graphSearchPopup.close();
                    }
                    contentItem: Text {
                        text: parent.text
                        color: graphPanel.theme.text
                        font.pixelSize: graphPanel.theme.fontSize
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: graphPanel.theme.smallRadius
                        color: parent.hovered || parent.highlighted ? graphPanel.theme.hover : "transparent"
                    }
                }
            }
        }
        onOpened: {
            graphPanel.searchAnchor = graphSurface.mapToItem(Overlay.overlay, graphPanel.pointerX, graphPanel.pointerY);
            graphSearchField.text = "";
            graphSearchList.currentIndex = 0;
            graphSearchList.model = graphPanel.filteredCatalog("");
        }
        onClosed: graphPanel.forceActiveFocus()
    }
    Menu {
        id: graphContextMenu
        objectName: "graphNodeContextMenu"
        MenuItem {
            objectName: "graphCollapseSelection"
            text: "Collapse to subnetwork"
            enabled: graphPanel.graphAvailable && graphPanel.selectedNodeIds.length > 0
            onTriggered: graphPanel.collapseSelection()
        }
        MenuItem {
            objectName: "graphEnterSelection"
            text: "Enter subnet"
            enabled: graphPanel.contextSubnetId().length > 0
            onTriggered: graphPanel.enterSubnet(graphPanel.contextTargetId())
        }
        MenuSeparator {
        }
        MenuItem {
            objectName: "graphEditExposedParameters"
            text: "Edit exposed parameters…"
            enabled: graphPanel.contextSubnetId().length > 0
            onTriggered: {
                var id = graphPanel.contextSubnetId();
                if (id.length) {
                    var path = graphPanel.scopeBreadcrumbs.map(function (crumb) {
                            return String(crumb.name || crumb.networkId);
                        }).join(" / ");
                    subnetParameters.openFor(graphPanel.graphNetworkId, id, path);
                }
            }
        }
        MenuItem {
            objectName: "graphMakeIndependent"
            text: "Make independent"
            enabled: graphPanel.contextSubnetId().length > 0 && String(graphPanel.contextNodeInfo.linkState) !== "local"
            onTriggered: {
                var info = interaction.nodeInfo(graphPanel.contextSubnetId());
                if (info.exists === true)
                    interaction.makeIndependent(String(info.instance));
            }
        }
        MenuItem {
            objectName: "graphDuplicateLinked"
            text: "Duplicate (linked)"
            enabled: graphPanel.contextSubnetId().length > 0
            onTriggered: {
                var id = graphPanel.contextSubnetId();
                if (id.length)
                    interaction.duplicateLinked(id);
            }
        }
        MenuSeparator {
        }
        MenuItem {
            objectName: "graphDeleteSelection"
            text: "Delete"
            enabled: graphPanel.selectedNodeIds.length > 0
            onTriggered: {
                if (interaction.deleteSelection())
                    graphPanel.savePanelState();
            }
        }
        MenuItem {
            objectName: "graphSetViewerSelection"
            text: "Set as Viewer"
            enabled: graphPanel.selectedNodeIds.length === 1
            onTriggered: {
                if (graphPanel.selectedNodeIds.length !== 1)
                    return;
                interaction.assignViewer(0);
            }
        }
    }
    SubnetParameters {
        id: subnetParameters
        controller: graphPanel.controller
        theme: graphPanel.theme
        transientParent: graphPanel.Window.window
        Connections {
            target: typeof projectFile !== "undefined" ? projectFile : null
            function onProjectOpened() {
                subnetParameters.close();
            }
        }
    }
    Text {
        objectName: "graphErrorLabel"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        visible: controller.error.length > 0
        text: controller.error
        color: graphPanel.theme.accent
        font.pixelSize: 11
        elide: Text.ElideRight
        z: 50
    }
}
