import QtQuick

// The source never moves. Buttons keep ordinary clicks until Qt's platform
// drag threshold is crossed and this handler takes the exclusive grab.
DragHandler {
    id: root
    property string panelId: ""
    property string leafId: ""
    property string panelType: ""
    property var drag
    property bool ownsGesture: false
    target: null
    acceptedButtons: Qt.LeftButton
    grabPermissions: PointerHandler.CanTakeOverFromItems | PointerHandler.TakeOverForbidden

    onActiveChanged: {
        if (active && root.drag) {
            ownsGesture = true
            root.drag.beginDrag(panelId, leafId, panelType)
            root.drag.updateDrag(centroid.scenePosition.x, centroid.scenePosition.y)
        } else if (ownsGesture && root.drag) {
            root.drag.updateDrag(centroid.scenePosition.x, centroid.scenePosition.y)
        }
    }
    onCentroidChanged: {
        if (active && root.drag)
            root.drag.updateDrag(centroid.scenePosition.x, centroid.scenePosition.y)
    }
    onGrabChanged: function(transition, point) {
        if (transition === PointerDevice.UngrabExclusive && ownsGesture) {
            // Qt 6.4 may emit cancellation during cleanup after a normal
            // ungrab. Finish once; that later cleanup must not cancel the drop.
            ownsGesture = false
            if (root.drag)
                root.drag.endDrag()
        }
    }
    onCanceled: {
        if (ownsGesture) {
            ownsGesture = false
            if (root.drag)
                root.drag.cancelDrag()
        }
    }
}
