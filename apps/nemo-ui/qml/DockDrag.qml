import QtQuick

// Only transient gesture state lives here. A drop crosses the model seam once,
// after pointer delivery returns; hovering never edits the split tree.
Item {
    id: dockDrag
    objectName: "dockDrag"
    property var workspace
    property var theme
    property bool active: false
    property string sourcePanelId: ""
    property string sourceLeafId: ""
    property string sourceType: ""
    property string targetLeafId: ""
    property string placement: "tabs"
    property int tabIndex: 0
    property bool targetValid: false
    property bool tabInsertion: false
    property var leaves: ({})
    property real sceneX: 0
    property real sceneY: 0
    property int generation: 0
    readonly property int minPaneWidth: 120
    readonly property int minPaneHeight: 80
    readonly property int handleSize: 4

    Connections {
        target: dockDrag.workspace
        function onRootChanged() { dockDrag.cancelDrag() }
    }

    function registerLeaf(id, item) {
        if (id && item)
            leaves[id] = item
    }

    function unregisterLeaf(id, item) {
        // An old Loader may be destroyed after its replacement registered.
        if (leaves[id] === item)
            delete leaves[id]
    }

    function displayType(type) {
        var descriptor = workspace ? workspace.panelDescriptor(type) : ({})
        return descriptor && descriptor.title ? descriptor.title : type
    }

    function beginDrag(panelId, leafId, type) {
        if (!panelId || !leafId || active)
            return
        ++generation
        sourcePanelId = panelId
        sourceLeafId = leafId
        sourceType = type
        targetValid = false
        active = true
    }

    function updateDrag(x, y) {
        if (!active)
            return
        sceneX = x
        sceneY = y
        updateTarget()
    }

    function clearTransient() {
        active = false
        targetValid = false
        tabInsertion = false
        sourcePanelId = ""
        sourceLeafId = ""
        sourceType = ""
        targetLeafId = ""
    }

    function cancelDrag() {
        // Also invalidates a queued release when a grab is canceled, the
        // window deactivates, or the layout changes before the next event turn.
        ++generation
        clearTransient()
    }

    function endDrag() {
        if (!active)
            return
        var panel = sourcePanelId
        var leaf = targetLeafId
        var edge = placement
        var index = tabIndex
        var accepted = targetValid
        var serial = generation
        clearTransient()
        if (accepted) {
            Qt.callLater(function() {
                if (serial === dockDrag.generation && dockDrag.workspace)
                    dockDrag.workspace.movePanel(panel, leaf, edge, index)
            })
        }
    }

    function updateTarget() {
        targetValid = false
        tabInsertion = false
        var hit = null
        var local = null
        for (var id in leaves) {
            var candidate = leaves[id]
            if (!candidate || !candidate.visible)
                continue
            var point = candidate.mapFromItem(null, sceneX, sceneY)
            if (point.x >= 0 && point.x < candidate.width && point.y >= 0 && point.y < candidate.height) {
                hit = candidate
                targetLeafId = id
                local = point
                break
            }
        }
        if (!hit || (targetLeafId === sourceLeafId && hit.panelCount === 1))
            return

        var strip = hit.tabStrip
        if (strip) {
            var stripPoint = strip.mapFromItem(null, sceneX, sceneY)
            if (stripPoint.y >= 0 && stripPoint.y <= strip.height) {
                placement = "tabs"
                tabInsertion = true
                tabIndex = hit.tabButtons.count
                for (var i = 0; i < hit.tabButtons.count; ++i) {
                    var button = hit.tabButtons.itemAt(i)
                    if (stripPoint.x < button.x + button.width / 2) {
                        tabIndex = i
                        break
                    }
                }
            }
        }
        if (!tabInsertion) {
            var left = local.x, right = hit.width - local.x
            var top = local.y, bottom = hit.height - local.y
            var margin = Math.max(16, Math.min(48, Math.min(hit.width, hit.height) * 0.25))
            placement = "tabs"
            tabIndex = hit.panelCount
            if (left <= margin && left <= right && left <= top && left <= bottom) placement = "left"
            else if (right <= margin && right <= top && right <= bottom) placement = "right"
            else if (top <= margin && top <= bottom) placement = "top"
            else if (bottom <= margin) placement = "bottom"
            // Do not offer a split that cannot fit two usable panes.
            if ((placement === "left" || placement === "right") && hit.width < 2 * minPaneWidth + handleSize)
                return
            if ((placement === "top" || placement === "bottom") && hit.height < 2 * minPaneHeight + handleSize)
                return
        }
        targetValid = true
        var origin = hit.mapToItem(dockDrag, 0, 0)
        if (tabInsertion) {
            var stripOrigin = strip.mapToItem(dockDrag, 0, 0)
            var count = hit.tabButtons.count
            var tab = hit.tabButtons.itemAt(Math.min(tabIndex, count - 1))
            tabLine.x = stripOrigin.x + (tab ? tab.x + (tabIndex === count ? tab.width : 0) : 0)
            tabLine.y = stripOrigin.y
            tabLine.height = strip.height
        } else {
            var w = hit.width, h = hit.height
            dockPreview.x = origin.x
            dockPreview.y = origin.y
            if (placement === "left" || placement === "right") {
                w = (w - handleSize) / 2
                if (placement === "right") dockPreview.x += w + handleSize
            } else if (placement === "top" || placement === "bottom") {
                h = (h - handleSize) / 2
                if (placement === "bottom") dockPreview.y += h + handleSize
            }
            dockPreview.width = w
            dockPreview.height = h
        }
    }

    Rectangle {
        id: dockPreview
        objectName: "dockPreview"
        visible: dockDrag.active && dockDrag.targetValid && !dockDrag.tabInsertion
        color: theme ? theme.accent : "#304a6fa5"
        opacity: 0.38
        border.color: theme ? theme.accent : "#7a9cc9"
        border.width: 1
    }
    Rectangle {
        id: tabLine
        objectName: "tabLine"
        visible: dockDrag.active && dockDrag.targetValid && dockDrag.tabInsertion
        width: 2
        color: theme ? theme.accent : "#7a9cc9"
    }
    Rectangle {
        id: dockLabel
        objectName: "dockLabel"
        visible: dockDrag.active
        readonly property point pointer: dockDrag.mapFromItem(null, dockDrag.sceneX, dockDrag.sceneY)
        x: Math.max(4, Math.min(pointer.x + 16, dockDrag.width - width - 4))
        y: Math.max(4, Math.min(pointer.y + 18, dockDrag.height - height - 4))
        width: label.implicitWidth + 12
        height: 22
        color: theme ? theme.header : "#333333"
        border.color: theme ? theme.border : "#606060"
        radius: 3
        Text {
            id: label
            anchors.centerIn: parent
            text: dockDrag.displayType(dockDrag.sourceType)
            color: theme ? theme.text : "#e0e0e0"
            font.pixelSize: 11
        }
    }
}
