import QtQuick
import QtQuick.Controls

// Roto viewport overlay (issue #93), mounted under the image area of the panel
// that views a Roto node this context group has inspected.
//
// The overlay owns presentation and the pointer: the evaluated geometry the
// shared RotoController publishes, the hit tests, the marquee, the selection
// box and the live gesture. Authored state, undo and keying stay in the
// document/session; the tool, the selection and the topology draft live in the
// controller, so this overlay and the inspector's hierarchy share ONE selection
// for the same node. Nothing here reads the inspector, and no geometry is
// previewed locally: a live drag is visible through the controller's published
// preview, so what is drawn is exactly what will be committed.
//
// Gesture map (Select tool):
//   click a point .............. select it (replaces the selection)
//   Shift + click .............. toggle it in the selection (points or shapes)
//   drag a point ............... move every selected point (one undo entry)
//   drag inside a shape ........ select the shape, then move its points
//   drag empty space ........... marquee select the points inside (no modifier
//                                clears the selection)
//   drag a scale handle ........ scale the selection about the opposite handle
//                                (Shift: uniform)
//   drag the rotate handle ..... rotate about the pivot (view pixel aspect)
//   drag the pivot marker ...... move the transient pivot anywhere (it is never
//                                document state; the selection is untouched)
//   drag a tangent handle ...... edit that tangent; a smooth point keeps the
//                                mirrored link, Alt breaks it, Ctrl forces it
//   Alt + drag a B-spline point  tension
//   Ctrl + drag a point ........ feather from zero
//   Ctrl + Alt + click curve ... insert a point on that curve
//   Delete ..................... remove the selected points
//   Z / Shift+Z ................ smooth / cusp the selected points
//   Ctrl+A ..................... select every point of every editable shape
//   Q .......................... Select tool
//   Enter ...................... close the draft; Escape discards it
//
// The pointer stays with the panel's own pan gesture for everything the overlay
// does not own: this item installs no MouseArea, so middle drag and the wheel
// keep working and there is exactly ONE hit test per press.
Item {
    id: overlay

    property var theme: null
    // The viewer panel: the shared image mapping and the accepted pan/zoom
    // state. The panel calls the interaction functions below.
    property var panel: null
    // The shared per-node authoring adapter, or null when this panel does not
    // view an inspected Roto.
    property var roto: null
    // Panel identity, so a native test can address the controls.
    property string panelId: ""

    readonly property bool active: overlay.roto !== null && overlay.roto.available === true
    // The one live pointer edit (gesture token plus its frozen baseline) or null.
    property var drag: null
    // True while a live edit owns a session gesture: the shared history owner
    // must treat Escape / a preview-only Undo as a cancellation of this edit.
    readonly property bool gestureLive: (overlay.drag !== null && String(overlay.drag.token || "").length > 0)
                                         || (overlay.roto !== null && overlay.roto.draftActive === true)
    // True while a rectangle/ellipse drag owns the draft's second corner.
    property bool draftDragging: false
    // The transient gesture pivot in image coordinates, or null for the
    // selection box centre. It never becomes document state.
    property var pivot: null
    // The handle or marker under the pointer, for the hover highlight and the
    // cursor. {kind, element, point, handle}.
    property var hoverTarget: null
    // The live marquee in overlay screen coordinates, or null.
    property var marquee: null

    objectName: "rotoOverlay_" + overlay.panelId
    anchors.fill: parent
    visible: overlay.active

    readonly property real handleRadius: 6
    readonly property real pointRadius: 5
    readonly property real bboxHandleRadius: 5
    readonly property real pivotRadius: 6
    readonly property real pathTolerance: 6
    // The selection box sits this many screen pixels outside the outermost
    // selected point, so a control point never competes with a scale corner.
    readonly property int bboxPadding: 12
    readonly property int rotateOffset: 28
    readonly property var scaleHandles: ["left-top", "top", "right-top", "right",
                                         "right-bottom", "bottom", "left-bottom", "left"]
    readonly property string tool: overlay.roto ? String(overlay.roto.tool) : "select"
    readonly property var toolEntries: [{
        "key": "select",
        "glyph": "\u2196",
        "name": "Select",
        "hint": "Q"
    }, {
        "key": "bezier",
        "glyph": "\u2312",
        "name": "Bézier",
        "hint": ""
    }, {
        "key": "bspline",
        "glyph": "\u223F",
        "name": "B-spline",
        "hint": ""
    }, {
        "key": "rectangle",
        "glyph": "\u25AD",
        "name": "Rectangle",
        "hint": ""
    }, {
        "key": "ellipse",
        "glyph": "\u25EF",
        "name": "Ellipse",
        "hint": ""
    }]

    function themeColor(role, fallback) {
        return overlay.panel ? overlay.panel.themeColor(role, fallback) : fallback;
    }

    // --- image <-> screen --------------------------------------------------
    // ONE mapping serves drawing and the pointer, and a live gesture keeps the
    // mapping it started on, so a wheel zoom mid-drag cannot bend the edit.
    function mapping() {
        return overlay.panel ? overlay.panel.rotoViewMapping() : null;
    }

    function toScreen(view, x, y) {
        return {
            "x": view.originX + x * view.sx,
            "y": view.originY + y * view.scale
        };
    }

    function toImage(view, px, py) {
        return {
            "x": (px - view.originX) / view.sx,
            "y": (py - view.originY) / view.scale
        };
    }

    // Image pixels of one screen pixel along each axis: the rotation below works
    // in physical space (x = imageX * pixel aspect) while the affine it hands
    // the controller stays in image pixels.
    function pixelAspect(view) {
        return view.scale > 0 ? view.sx / view.scale : 1;
    }

    function placeVector(record, x, y) {
        return {
            "x": record.a * x + record.c * y,
            "y": record.b * x + record.d * y
        };
    }

    function unplaceVector(record, x, y) {
        var determinant = record.a * record.d - record.b * record.c;
        if (Math.abs(determinant) < 1e-9)
            return {
                "x": 0,
                "y": 0
            };
        return {
            "x": (record.d * x - record.c * y) / determinant,
            "y": (-record.b * x + record.a * y) / determinant
        };
    }

    function near(px, py, x, y, radius) {
        return Math.abs(px - x) <= radius && Math.abs(py - y) <= radius;
    }

    // --- published geometry ------------------------------------------------
    function shapes() {
        return overlay.roto ? (overlay.roto.geometry || []) : [];
    }

    function shapeById(id) {
        var list = overlay.shapes();
        for (var index = 0; index < list.length; ++index)
            if (String(list[index].id) === String(id))
                return list[index];
        return null;
    }

    function pointsOf(record) {
        return record && record.points ? record.points : [];
    }

    // A locked element locks its whole subtree: the renderer and the authoring
    // addresses agree, so neither a handle nor a marquee may touch it.
    function lockedInHierarchy(record) {
        var current = record;
        var guard = 0;
        while (current && guard < 64) {
            if (current.locked === true)
                return true;
            var parent = String(current.parent);
            if (parent.length === 0 || parent === "0")
                return false;
            current = overlay.shapeById(parent);
            ++guard;
        }
        return false;
    }

    function editableShapes() {
        var list = overlay.shapes();
        var result = [];
        for (var index = 0; index < list.length; ++index) {
            var record = list[index];
            if (record.group || !record.visible || overlay.lockedInHierarchy(record))
                continue;
            result.push(record);
        }
        return result;
    }

    // {record, point} for every selected point of every editable shape.
    function selectedPointEntries() {
        var list = overlay.editableShapes();
        var result = [];
        for (var index = 0; index < list.length; ++index) {
            var points = overlay.pointsOf(list[index]);
            for (var point = 0; point < points.length; ++point)
                if (points[point].selected === true)
                    result.push({
                        "record": list[index],
                        "point": points[point]
                    });
        }
        return result;
    }

    // The selection the inspector and the viewport share: the controller's own
    // primary text. The published geometry flags agree with it, but reading the
    // primary directly keeps the box correct on the same turn a selection was
    // just published.
    function primaryElement() {
        return overlay.roto ? String(overlay.roto.selectedElement || "") : "";
    }

    function withinPrimary(record) {
        var primary = overlay.primaryElement();
        if (primary.length === 0 || !record)
            return false;
        var current = record;
        var guard = 0;
        while (current && guard < 64) {
            if (String(current.id) === primary)
                return true;
            var parent = String(current.parent);
            if (parent.length === 0 || parent === "0")
                return false;
            current = overlay.shapeById(parent);
            ++guard;
        }
        return false;
    }

    // The points a selection transform would edit: the selected points, else
    // every point of the selected shapes and of the descendants of a selected
    // group. The controller resolves the same set; this is for drawing.
    function selectionEntries() {
        var points = overlay.selectedPointEntries();
        if (points.length > 0)
            return points;
        var list = overlay.editableShapes();
        var result = [];
        for (var index = 0; index < list.length; ++index) {
            var record = list[index];
            if (record.selected !== true && !overlay.withinPrimary(record))
                continue;
            var owned = overlay.pointsOf(record);
            for (var point = 0; point < owned.length; ++point)
                result.push({
                    "record": record,
                    "point": owned[point]
                });
        }
        return result;
    }

    function selectionTouches(record) {
        if (!record)
            return false;
        if (record.selected === true || overlay.withinPrimary(record))
            return true;
        var points = overlay.pointsOf(record);
        for (var index = 0; index < points.length; ++index)
            if (points[index].selected === true)
                return true;
        return false;
    }

    function hasSelection() {
        return overlay.selectionEntries().length > 0;
    }

    // --- selection box -----------------------------------------------------
    function boundsOf(entries) {
        if (entries.length === 0)
            return null;
        var bounds = {
            "minX": Infinity,
            "minY": Infinity,
            "maxX": -Infinity,
            "maxY": -Infinity
        };
        for (var index = 0; index < entries.length; ++index) {
            var point = entries[index].point;
            bounds.minX = Math.min(bounds.minX, point.x);
            bounds.minY = Math.min(bounds.minY, point.y);
            bounds.maxX = Math.max(bounds.maxX, point.x);
            bounds.maxY = Math.max(bounds.maxY, point.y);
        }
        return bounds;
    }

    // The scale/rotate/pivot handles in OVERLAY screen coordinates: the same
    // space press/move/release take, so a native test can press them directly.
    function selectionGeometry() {
        if (!overlay.active)
            return null;
        var view = overlay.mapping();
        if (!view)
            return null;
        var entries = overlay.selectionEntries();
        // A single point already has tangent and feather handles. A collapsed
        // transform box only crowds those direct editing targets.
        if (entries.length < 2)
            return null;
        var bounds = overlay.boundsOf(entries);
        if (!bounds)
            return null;
        var topLeft = overlay.toScreen(view, bounds.minX, bounds.minY);
        var bottomRight = overlay.toScreen(view, bounds.maxX, bounds.maxY);
        var left = topLeft.x - overlay.bboxPadding;
        var top = topLeft.y - overlay.bboxPadding;
        var right = bottomRight.x + overlay.bboxPadding;
        var bottom = bottomRight.y + overlay.bboxPadding;
        var centreX = (left + right) / 2;
        var centreY = (top + bottom) / 2;
        var pivot = overlay.pivot
                ? overlay.toScreen(view, overlay.pivot.x, overlay.pivot.y)
                : { "x": centreX, "y": centreY };
        return {
            "view": view,
            "bounds": bounds,
            // The padded box itself, apart from the handles named below: a
            // "left" key here would collide with the left scale handle.
            "area": {
                "left": left,
                "top": top,
                "right": right,
                "bottom": bottom
            },
            "left-top": { "x": left, "y": top },
            "top": { "x": centreX, "y": top },
            "right-top": { "x": right, "y": top },
            "right": { "x": right, "y": centreY },
            "right-bottom": { "x": right, "y": bottom },
            "bottom": { "x": centreX, "y": bottom },
            "left-bottom": { "x": left, "y": bottom },
            "left": { "x": left, "y": centreY },
            "pivot": pivot,
            "rotate": { "x": centreX, "y": top - overlay.rotateOffset }
        };
    }

    function selectionHandles() {
        var geometry = overlay.selectionGeometry();
        var result = {};
        if (!geometry)
            return result;
        var names = overlay.scaleHandles;
        for (var index = 0; index < names.length; ++index) {
            var point = geometry[names[index]];
            result[names[index]] = {
                "x": point.x,
                "y": point.y
            };
        }
        result["rotate"] = {
            "x": geometry.rotate.x,
            "y": geometry.rotate.y
        };
        result["pivot"] = {
            "x": geometry.pivot.x,
            "y": geometry.pivot.y
        };
        result["bounds"] = {
            "left": geometry.area.left,
            "top": geometry.area.top,
            "right": geometry.area.right,
            "bottom": geometry.area.bottom,
            "width": geometry.area.right - geometry.area.left,
            "height": geometry.area.bottom - geometry.area.top
        };
        return result;
    }

    function oppositeHandle(handle) {
        var name = String(handle);
        name = name.indexOf("left") >= 0 ? name.replace("left", "right")
                                         : name.replace("right", "left");
        return name.indexOf("top") >= 0 ? name.replace("top", "bottom")
                                        : name.replace("bottom", "top");
    }

    function pointHovered(record, point) {
        var hovered = overlay.hoverTarget;
        return hovered !== null && String(hovered.kind) === "point" && String(hovered.element) === String(record.id)
                && String(hovered.point) === String(point.id);
    }

    // --- hit testing -------------------------------------------------------
    // {kind, element, point, handle}: a tangent or feather handle of a selected
    // point, a selection-box handle, any point of an editable shape, the
    // transient pivot, a shape's own curve or area, or nothing — the overlay
    // then starts a marquee.
    //
    // The point-level handles are tested first: a short tangent or a small
    // feather can sit within a few pixels of a scale corner, and the handle the
    // artist aimed at is the one that must win.
    function hitTest(px, py) {
        if (!overlay.active)
            return {};
        var view = overlay.mapping();
        if (!view)
            return {};
        if (overlay.roto.draftActive)
            return { "kind": "draft" };
        // A handle is only addressable when it is visibly off its point: a zero
        // tangent or a zero feather sits exactly on the point, so it must never
        // shadow dragging that point.
        var entries = overlay.selectedPointEntries();
        for (var index = 0; index < entries.length; ++index) {
            var entry = entries[index];
            var point = entry.point;
            var origin = overlay.toScreen(view, point.x, point.y);
            if (String(entry.record.kind) !== "bspline") {
                var inHandle = overlay.toScreen(view, point.x + point.inX, point.y + point.inY);
                if (Math.hypot(inHandle.x - origin.x, inHandle.y - origin.y) > overlay.handleRadius
                        && overlay.near(px, py, inHandle.x, inHandle.y, overlay.handleRadius))
                    return {
                        "kind": "tangent",
                        "element": entry.record.id,
                        "point": point.id,
                        "key": "inTangent"
                    };
                var outHandle = overlay.toScreen(view, point.x + point.outX, point.y + point.outY);
                if (Math.hypot(outHandle.x - origin.x, outHandle.y - origin.y) > overlay.handleRadius
                        && overlay.near(px, py, outHandle.x, outHandle.y, overlay.handleRadius))
                    return {
                        "kind": "tangent",
                        "element": entry.record.id,
                        "point": point.id,
                        "key": "outTangent"
                    };
            }
            var tip = overlay.toScreen(view, point.tipX, point.tipY);
            if (Math.hypot(tip.x - origin.x, tip.y - origin.y) > overlay.handleRadius
                    && overlay.near(px, py, tip.x, tip.y, overlay.handleRadius))
                return {
                    "kind": "feather",
                    "element": entry.record.id,
                    "point": point.id
                };
        }
        var box = overlay.selectionGeometry();
        if (box) {
            if (overlay.near(px, py, box.rotate.x, box.rotate.y, overlay.bboxHandleRadius + 2))
                return { "kind": "rotate" };
            var names = overlay.scaleHandles;
            for (var handle = 0; handle < names.length; ++handle) {
                var at = box[names[handle]];
                if (overlay.near(px, py, at.x, at.y, overlay.bboxHandleRadius + 2))
                    return {
                        "kind": "scale",
                        "handle": names[handle]
                    };
            }
        }
        var list = overlay.editableShapes();
        for (var shape = list.length - 1; shape >= 0; --shape) {
            var points = overlay.pointsOf(list[shape]);
            for (var marker = 0; marker < points.length; ++marker) {
                var screen = overlay.toScreen(view, points[marker].x, points[marker].y);
                if (overlay.near(px, py, screen.x, screen.y, overlay.pointRadius + 2))
                    return {
                        "kind": "point",
                        "element": list[shape].id,
                        "point": points[marker].id
                    };
            }
        }
        if (box && overlay.near(px, py, box.pivot.x, box.pivot.y, overlay.pivotRadius))
            return { "kind": "pivot" };
        for (var curve = list.length - 1; curve >= 0; --curve) {
            if (overlay.nearPath(view, list[curve], px, py))
                return {
                    "kind": "element",
                    "element": list[curve].id
                };
        }
        for (var area = list.length - 1; area >= 0; --area) {
            if (overlay.insideShape(view, list[area], px, py))
                return {
                    "kind": "element",
                    "element": list[area].id
                };
        }
        return {};
    }

    // The screen bounds of a shape's control polygon. Both path kinds stay
    // inside it, so it is a cheap exact rejection before the sampled outline.
    function controlBounds(view, record) {
        var points = overlay.pointsOf(record);
        var bounds = null;
        for (var index = 0; index < points.length; ++index) {
            var point = points[index];
            var offsets = [[point.x, point.y], [point.x + point.inX, point.y + point.inY],
                           [point.x + point.outX, point.y + point.outY]];
            for (var offset = 0; offset < offsets.length; ++offset) {
                var screen = overlay.toScreen(view, offsets[offset][0], offsets[offset][1]);
                if (!bounds)
                    bounds = {
                        "minX": screen.x,
                        "minY": screen.y,
                        "maxX": screen.x,
                        "maxY": screen.y
                    };
                else {
                    bounds.minX = Math.min(bounds.minX, screen.x);
                    bounds.minY = Math.min(bounds.minY, screen.y);
                    bounds.maxX = Math.max(bounds.maxX, screen.x);
                    bounds.maxY = Math.max(bounds.maxY, screen.y);
                }
            }
        }
        return bounds;
    }

    function nearPath(view, record, px, py) {
        var bounds = overlay.controlBounds(view, record);
        if (!bounds)
            return false;
        var margin = overlay.pathTolerance;
        if (px < bounds.minX - margin || px > bounds.maxX + margin
                || py < bounds.minY - margin || py > bounds.maxY + margin)
            return false;
        return overlay.curveLocation(view, record, px, py) !== null;
    }

    function insideShape(view, record, px, py) {
        var points = overlay.pointsOf(record);
        if (points.length < 3 || !canvas.available)
            return false;
        var ctx = canvas.getContext("2d");
        if (!ctx)
            return false;
        ctx.save();
        overlay.traceShape(ctx, record, points, view);
        var inside = ctx.isPointInPath(px, py);
        ctx.restore();
        return inside;
    }

    // The curve parameter under the pointer: the segment whose sampled outline
    // passes closest, and where along it. Ctrl+Alt uses this to insert a point
    // exactly on the curve the artist clicked.
    function curveLocation(view, record, px, py) {
        var points = overlay.pointsOf(record);
        if (points.length < 2)
            return null;
        var count = points.length;
        var steps = 24;
        var bestDistance = Infinity;
        var bestSegment = 0;
        var bestTime = 0;
        for (var segment = 0; segment < count; ++segment) {
            var from = points[segment];
            var to = points[(segment + 1) % count];
            var start = overlay.cubicPoint(points, record, from, to, 0);
            var previous = overlay.toScreen(view, start.x, start.y);
            for (var step = 1; step <= steps; ++step) {
                var at = overlay.cubicPoint(points, record, from, to, step / steps);
                var screen = overlay.toScreen(view, at.x, at.y);
                var dx = screen.x - previous.x;
                var dy = screen.y - previous.y;
                var lengthSquared = dx * dx + dy * dy;
                // Hit the continuous sampled outline, not isolated vertices;
                // otherwise zooming creates unclickable gaps along a curve.
                var fraction = lengthSquared > 0
                        ? Math.max(0, Math.min(1, ((px - previous.x) * dx + (py - previous.y) * dy) / lengthSquared)) : 0;
                var offsetX = previous.x + fraction * dx - px;
                var offsetY = previous.y + fraction * dy - py;
                var distance = offsetX * offsetX + offsetY * offsetY;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestSegment = segment;
                    bestTime = (step - 1 + fraction) / steps;
                }
                previous = screen;
            }
        }
        if (bestDistance > overlay.pathTolerance * overlay.pathTolerance)
            return null;
        return { "segment": bestSegment, "t": bestTime };
    }

    // One point of a segment at `t`, in image coordinates: the same periodic
    // uniform cubic with tension blend the renderer uses for B-splines, and the
    // cubic Bézier the renderer uses for keyed tangents.
    function cubicPoint(points, record, from, to, t) {
        if (String(record.kind) === "bspline") {
            var count = points.length;
            var index = points.indexOf(from);
            var p0 = points[(index + count - 1) % count];
            var p2 = points[(index + 1) % count];
            var p3 = points[(index + 2) % count];
            var tension = (1 - t) * Number(from.tension) + t * Number(to.tension);
            var t2 = t * t;
            var t3 = t2 * t;
            var x = ((1 - t) * (1 - t) * (1 - t) * p0.x + (3 * t3 - 6 * t2 + 4) * from.x
                    + (-3 * t3 + 3 * t2 + 3 * t + 1) * to.x + t3 * p3.x) / 6;
            var y = ((1 - t) * (1 - t) * (1 - t) * p0.y + (3 * t3 - 6 * t2 + 4) * from.y
                    + (-3 * t3 + 3 * t2 + 3 * t + 1) * to.y + t3 * p3.y) / 6;
            return {
                "x": x + (from.x + (to.x - from.x) * t - x) * tension,
                "y": y + (from.y + (to.y - from.y) * t - y) * tension
            };
        }
        var inverse = 1 - t;
        var c1 = inverse * inverse * inverse;
        var c2 = 3 * inverse * inverse * t;
        var c3 = 3 * inverse * t * t;
        var c4 = t * t * t;
        return {
            "x": c1 * from.x + c2 * (from.x + from.outX) + c3 * (to.x + to.inX) + c4 * to.x,
            "y": c1 * from.y + c2 * (from.y + from.outY) + c3 * (to.y + to.inY) + c4 * to.y
        };
    }

    function cursorShape(px, py) {
        if (!overlay.active)
            return Qt.ArrowCursor;
        if (overlay.roto.draftActive || overlay.tool !== "select")
            return Qt.CrossCursor;
        var drag = overlay.drag;
        if (drag !== null) {
            if (String(drag.kind) === "transform") {
                if (String(drag.mode) === "rotate")
                    return Qt.CrossCursor;
                if (String(drag.mode) === "scale")
                    return overlay.scaleCursor(String(drag.handle));
                return Qt.SizeAllCursor;
            }
            if (String(drag.kind) === "marquee")
                return Qt.CrossCursor;
            return Qt.SizeAllCursor;
        }
        return overlay.targetCursor(overlay.hitTest(px, py));
    }

    function targetCursor(target) {
        if (target.kind === "scale")
            return overlay.scaleCursor(String(target.handle));
        if (target.kind === "rotate")
            return Qt.CrossCursor;
        if (target.kind === "pivot" || target.kind === "tangent" || target.kind === "feather"
                || target.kind === "point" || target.kind === "element")
            return Qt.SizeAllCursor;
        return Qt.ArrowCursor;
    }

    function scaleCursor(handle) {
        var name = String(handle);
        if (name === "left-top" || name === "right-bottom")
            return Qt.SizeFDiagCursor;
        if (name === "right-top" || name === "left-bottom")
            return Qt.SizeBDiagCursor;
        if (name === "left" || name === "right")
            return Qt.SizeHorCursor;
        if (name === "top" || name === "bottom")
            return Qt.SizeVerCursor;
        return Qt.SizeAllCursor;
    }

    function clearHover() {
        if (overlay.hoverTarget === null)
            return;
        overlay.hoverTarget = null;
        canvas.requestPaint();
    }

    function updateHover(px, py) {
        if (!overlay.active || overlay.drag !== null || overlay.draftDragging
                || overlay.roto.draftActive) {
            overlay.clearHover();
            return;
        }
        var target = overlay.hitTest(px, py);
        var previous = overlay.hoverTarget;
        var same = previous === null
                ? target.kind === undefined
                : (String(previous.kind) === String(target.kind)
                   && String(previous.element || "") === String(target.element || "")
                   && String(previous.point || "") === String(target.point || "")
                   && String(previous.handle || "") === String(target.handle || ""));
        if (same)
            return;
        overlay.hoverTarget = target.kind === undefined || target.kind === "draft" ? null : target;
        canvas.requestPaint();
    }

    // --- gestures ----------------------------------------------------------
    // One numeric edit = ONE session gesture over the element/point-scoped
    // addresses of the document, so undo, keying and validation are the shared
    // ones. A release publishes exactly one history entry; a press that never
    // moved cancels, so a click never writes a no-op entry.
    function freeze(drag, px, py) {
        drag.view = overlay.mapping();
        drag.pressX = px;
        drag.pressY = py;
        drag.moved = false;
        overlay.hoverTarget = null;
        overlay.drag = drag;
        return true;
    }

    function beginSelectionTransform(mode, handle, px, py, modifiers) {
        var token = String(overlay.roto.beginSelectionTransform());
        if (token.length === 0)
            return false;
        var view = overlay.mapping();
        var geometry = overlay.selectionGeometry();
        if (!view || (mode !== "move" && !geometry)) {
            overlay.roto.cancelGesture(token);
            return false;
        }
        var image = overlay.toImage(view, px, py);
        var drag = {
            "kind": "transform",
            "mode": mode,
            "handle": String(handle),
            "token": token,
            "pressImage": image,
            "uniform": !!(modifiers & Qt.ShiftModifier)
        };
        if (mode === "rotate") {
            drag.pivot = overlay.pivot ? {
                "x": overlay.pivot.x,
                "y": overlay.pivot.y
            } : overlay.toImage(view, geometry.pivot.x, geometry.pivot.y);
            drag.par = overlay.pixelAspect(view);
            var physicalX = (image.x - drag.pivot.x) * drag.par;
            var physicalY = image.y - drag.pivot.y;
            drag.startAngle = Math.atan2(physicalY, physicalX);
        } else if (mode === "scale") {
            var anchorScreen = geometry[overlay.oppositeHandle(handle)];
            drag.anchor = overlay.toImage(view, anchorScreen.x, anchorScreen.y);
            drag.handleImage = overlay.toImage(view, geometry[handle].x, geometry[handle].y);
            drag.horizontal = String(handle).indexOf("left") >= 0 || String(handle).indexOf("right") >= 0;
            drag.vertical = String(handle).indexOf("top") >= 0 || String(handle).indexOf("bottom") >= 0;
        }
        return overlay.freeze(drag, px, py);
    }

    function beginPivot(px, py, modifiers) {
        var view = overlay.mapping();
        var geometry = overlay.selectionGeometry();
        if (!view || !geometry)
            return false;
        var image = overlay.toImage(view, px, py);
        var centre = overlay.pivot ? overlay.pivot : overlay.toImage(view, geometry.pivot.x, geometry.pivot.y);
        // The transient pivot only: no session gesture and no history entry.
        return overlay.freeze({
            "kind": "pivot",
            "token": "",
            "offsetX": image.x - centre.x,
            "offsetY": image.y - centre.y
        }, px, py);
    }

    function beginMarquee(px, py, modifiers) {
        overlay.marquee = {
            "left": px,
            "top": py,
            "right": px,
            "bottom": py
        };
        return overlay.freeze({
            "kind": "marquee",
            "token": "",
            "x0": px,
            "y0": py,
            "x1": px,
            "y1": py,
            "additive": !!(modifiers & Qt.ShiftModifier)
        }, px, py);
    }

    // The selection a point or a shape press starts from: a modifier toggles
    // membership and publishes no geometry edit, a plain press replaces the
    // selection only when the target is not part of it already.
    function selectForTransform(target, additive) {
        var record = overlay.shapeById(target.element);
        if (!record)
            return false;
        if (target.kind === "point") {
            if (additive)
                return overlay.roto.selectPoint(String(target.point), true);
            var current = overlay.roto.selectedPoints || [];
            var inSelection = current.indexOf(String(target.point)) >= 0;
            if (!inSelection)
                return overlay.roto.selectPoint(String(target.point), false);
            return true;
        }
        if (additive)
            return overlay.roto.selectElement(String(record.id), true);
        if (!overlay.selectionTouches(record))
            return overlay.roto.selectElement(String(record.id), false);
        return true;
    }

    function beginPointTransform(target, px, py, additive) {
        if (!overlay.selectForTransform(target, additive))
            return true;
        if (additive)
            return true;
        overlay.beginSelectionTransform("move", "", px, py, additive ? Qt.ShiftModifier : 0);
        return true;
    }

    function beginElementTransform(target, px, py, additive) {
        if (!overlay.selectForTransform(target, additive))
            return true;
        if (additive)
            return true;
        overlay.beginSelectionTransform("move", "", px, py, 0);
        return true;
    }

    function beginTangent(target, px, py, modifiers) {
        var record = overlay.shapeById(target.element);
        var point = overlay.pointById(record, target.point);
        if (!record || !point || overlay.lockedInHierarchy(record))
            return false;
        var smooth = Math.abs(point.inX + point.outX) < 0.01 && Math.abs(point.inY + point.outY) < 0.01;
        var draggedKey = String(target.key);
        var mirrorKey = draggedKey === "inTangent" ? "outTangent" : "inTangent";
        // A smooth point mirrors: dragging one handle moves its reflection, in
        // the same one preview and one history entry. A broken point moves only
        // the handle under the pointer. Ctrl forces the link and Alt breaks it,
        // so the modifier means the same thing on a smooth and a broken point.
        var mirror = (modifiers & Qt.AltModifier) ? false
                                                  : ((modifiers & Qt.ControlModifier) ? true : smooth);
        var targets = [{
            "element": String(record.id),
            "point": String(point.id),
            "key": draggedKey
        }];
        if (mirror)
            targets.push({
                "element": String(record.id),
                "point": String(point.id),
                "key": mirrorKey
            });
        var token = String(overlay.roto.beginGesture(targets));
        if (token.length === 0)
            return false;
        var inLocal = overlay.unplaceVector(record, point.inX, point.inY);
        var outLocal = overlay.unplaceVector(record, point.outX, point.outY);
        var origin = {};
        origin[String(point.id)] = {
            "inX": inLocal.x,
            "inY": inLocal.y,
            "outX": outLocal.x,
            "outY": outLocal.y
        };
        return overlay.freeze({
            "kind": "tangent",
            "token": token,
            "record": record,
            "element": String(record.id),
            "point": String(point.id),
            "key": draggedKey,
            "mirror": mirror,
            "origin": origin
        }, px, py);
    }

    function beginFeather(target, px, py, modifiers) {
        var record = overlay.shapeById(target.element);
        var point = overlay.pointById(record, target.point);
        var zeroStart = !!(modifiers & Qt.ControlModifier);
        if (!record || !point || overlay.lockedInHierarchy(record) || !(record.featherScale > 0))
            return false;
        var token = String(overlay.roto.beginGesture([{
            "element": String(record.id),
            "point": String(point.id),
            "key": "feather"
        }]));
        if (token.length === 0)
            return false;
        return overlay.freeze({
            "kind": "feather",
            "token": token,
            "element": String(record.id),
            "point": String(point.id),
            "zeroStart": zeroStart,
            "base": zeroStart ? 0 : Number(point.effectiveFeather),
            "bias": Number(record.featherBias),
            "scale": Number(record.featherScale),
            "nx": Number(point.nx),
            "ny": Number(point.ny)
        }, px, py);
    }

    function beginTension(target, px, py, modifiers) {
        var record = overlay.shapeById(target.element);
        var point = overlay.pointById(record, target.point);
        if (!record || !point || overlay.lockedInHierarchy(record))
            return false;
        var token = String(overlay.roto.beginGesture([{
            "element": String(record.id),
            "point": String(point.id),
            "key": "tension"
        }]));
        if (token.length === 0)
            return false;
        return overlay.freeze({
            "kind": "tension",
            "token": token,
            "element": String(record.id),
            "point": String(point.id),
            "base": Number(point.tension)
        }, px, py);
    }

    function pointById(record, id) {
        var points = overlay.pointsOf(record);
        for (var index = 0; index < points.length; ++index)
            if (String(points[index].id) === String(id))
                return points[index];
        return null;
    }

    // The panel's pan gesture delegates here first; true means the press was
    // consumed by the overlay.
    function press(px, py, modifiers) {
        if (!overlay.active)
            return false;
        var view = overlay.mapping();
        if (!view)
            return false;
        var image = overlay.toImage(view, px, py);
        var additive = !!(modifiers & Qt.ShiftModifier);
        var control = !!(modifiers & Qt.ControlModifier);
        var alternate = !!(modifiers & Qt.AltModifier);
        if (overlay.roto.draftActive) {
            var kind = String(overlay.roto.draftKind);
            if (kind === "rectangle" || kind === "ellipse") {
                overlay.draftDragging = true;
                return true;
            }
            var draft = overlay.roto.draftPoints || [];
            if (draft.length >= 2) {
                var first = overlay.toScreen(view, draft[0][0], draft[0][1]);
                if (overlay.near(px, py, first.x, first.y, overlay.pointRadius + 3)) {
                    overlay.roto.commitDraft();
                    return true;
                }
            }
            overlay.roto.addDraftPoint(image.x, image.y);
            return true;
        }
        if (overlay.tool !== "select") {
            if (!overlay.roto.beginDraft(overlay.tool, image.x, image.y))
                return false;
            overlay.draftDragging = overlay.tool === "rectangle" || overlay.tool === "ellipse";
            return true;
        }
        var target = overlay.hitTest(px, py);
        target.pressX = px;
        target.pressY = py;
        overlay.hoverTarget = target.kind === undefined ? null : target;
        if (target.kind === "rotate") {
            overlay.beginSelectionTransform("rotate", "", px, py, modifiers);
            return true;
        }
        if (target.kind === "scale") {
            overlay.beginSelectionTransform("scale", String(target.handle), px, py, modifiers);
            return true;
        }
        if (target.kind === "pivot") {
            overlay.beginPivot(px, py, modifiers);
            return true;
        }
        if (target.kind === "tangent") {
            overlay.beginTangent(target, px, py, modifiers);
            return true;
        }
        if (target.kind === "feather") {
            overlay.beginFeather(target, px, py, modifiers);
            return true;
        }
        if (target.kind === "point") {
            if (control && !alternate) {
                if (!overlay.selectForTransform(target, additive) || additive)
                    return true;
                overlay.beginFeather(target, px, py, modifiers);
                return true;
            }
            if (alternate) {
                var pointRecord = overlay.shapeById(target.element);
                if (pointRecord && String(pointRecord.kind) === "bspline") {
                    if (!overlay.selectForTransform(target, additive) || additive)
                        return true;
                    overlay.beginTension(target, px, py, modifiers);
                }
                return true;
            }
            overlay.beginPointTransform(target, px, py, additive);
            return true;
        }
        if (target.kind === "element") {
            var elementRecord = overlay.shapeById(target.element);
            if (control && alternate && elementRecord) {
                var located = overlay.curveLocation(view, elementRecord, px, py);
                if (located) {
                    overlay.roto.selectElement(String(elementRecord.id), false);
                    overlay.roto.insertCurvePoint(String(elementRecord.id), located.segment, located.t);
                    canvas.requestPaint();
                    return true;
                }
            }
            overlay.beginElementTransform(target, px, py, additive);
            return true;
        }
        overlay.beginMarquee(px, py, modifiers);
        return true;
    }

    function move(px, py) {
        if (!overlay.active)
            return false;
        if (overlay.draftDragging) {
            var draftView = overlay.mapping();
            if (!draftView)
                return true;
            var draftImage = overlay.toImage(draftView, px, py);
            overlay.roto.updateDraft(draftImage.x, draftImage.y);
            return true;
        }
        // A live path draft follows the pointer as its rubber band, so a drag
        // between two clicks draws rather than panning. Middle drag and the
        // wheel stay the panel's, so the view is still navigable while drawing.
        if (overlay.roto.draftActive) {
            var pathView = overlay.mapping();
            if (!pathView)
                return true;
            var pathImage = overlay.toImage(pathView, px, py);
            overlay.roto.updateDraft(pathImage.x, pathImage.y);
            return true;
        }
        var drag = overlay.drag;
        if (drag === null) {
            overlay.updateHover(px, py);
            return false;
        }
        if (Math.abs(px - drag.pressX) > 2 || Math.abs(py - drag.pressY) > 2)
            drag.moved = true;
        var view = drag.view;
        if (!view)
            return true;
        var deltaX = (px - drag.pressX) / view.sx;
        var deltaY = (py - drag.pressY) / view.scale;
        if (String(drag.kind) === "marquee") {
            drag.x1 = px;
            drag.y1 = py;
            overlay.marquee = {
                "left": Math.min(drag.x0, px),
                "top": Math.min(drag.y0, py),
                "right": Math.max(drag.x0, px),
                "bottom": Math.max(drag.y0, py)
            };
            canvas.requestPaint();
            return true;
        }
        if (String(drag.kind) === "pivot") {
            var pivotImage = overlay.toImage(view, px, py);
            overlay.pivot = {
                "x": pivotImage.x - drag.offsetX,
                "y": pivotImage.y - drag.offsetY
            };
            canvas.requestPaint();
            return true;
        }
        if (String(drag.kind) === "transform") {
            overlay.moveTransform(drag, px, py);
            return true;
        }
        if (String(drag.kind) === "tangent") {
            var origin = drag.origin[drag.point];
            if (!origin)
                return true;
            var draggingIn = String(drag.key) === "inTangent";
            var dragged = draggingIn ? {
                "x": origin.inX,
                "y": origin.inY
            } : {
                "x": origin.outX,
                "y": origin.outY
            };
            var handle = overlay.placeVector(drag.record, dragged.x, dragged.y);
            var moved = overlay.unplaceVector(drag.record, handle.x + deltaX, handle.y + deltaY);
            var values = [[moved.x, moved.y]];
            if (drag.mirror)
                values.push([-moved.x, -moved.y]);
            overlay.applyGesture(drag, values);
            return true;
        }
        if (String(drag.kind) === "feather") {
            var distance = drag.base + deltaX * drag.nx + deltaY * drag.ny;
            overlay.applyGesture(drag, [(distance - drag.bias) / drag.scale]);
            return true;
        }
        if (String(drag.kind) === "tension") {
            var tension = drag.base + deltaX / 200;
            overlay.applyGesture(drag, [Math.max(0, Math.min(1, tension))]);
            return true;
        }
        return true;
    }

    function moveTransform(drag, px, py) {
        var view = drag.view;
        var image = overlay.toImage(view, px, py);
        var mode = String(drag.mode);
        if (mode === "rotate") {
            var physicalX = (image.x - drag.pivot.x) * drag.par;
            var physicalY = image.y - drag.pivot.y;
            if (physicalX * physicalX + physicalY * physicalY < 4)
                return true;
            var angle = Math.atan2(physicalY, physicalX) - drag.startAngle;
            var cosine = Math.cos(angle);
            var sine = Math.sin(angle);
            var a = cosine;
            var b = sine * drag.par;
            var c = -sine / drag.par;
            var d = cosine;
            overlay.applyTransform(drag, {
                "a": a,
                "b": b,
                "c": c,
                "d": d,
                "e": drag.pivot.x - (a * drag.pivot.x + c * drag.pivot.y),
                "f": drag.pivot.y - (b * drag.pivot.x + d * drag.pivot.y)
            });
            return true;
        }
        if (mode === "scale") {
            var factorX = 1;
            var factorY = 1;
            if (drag.horizontal) {
                var spanX = drag.handleImage.x - drag.anchor.x;
                factorX = Math.abs(spanX) > 1e-6 ? (image.x - drag.anchor.x) / spanX : 1;
            }
            if (drag.vertical) {
                var spanY = drag.handleImage.y - drag.anchor.y;
                factorY = Math.abs(spanY) > 1e-6 ? (image.y - drag.anchor.y) / spanY : 1;
            }
            if (drag.uniform) {
                var factor = Math.abs(1 - factorX) >= Math.abs(1 - factorY) ? factorX : factorY;
                factorX = factor;
                factorY = factor;
            }
            overlay.applyTransform(drag, {
                "a": factorX,
                "b": 0,
                "c": 0,
                "d": factorY,
                "e": drag.anchor.x * (1 - factorX),
                "f": drag.anchor.y * (1 - factorY)
            });
            return true;
        }
        // Every other mode is a translation of the press-time selection: the
        // affine is the pointer's image-space offset from the frozen press
        // sample, never an accumulation of per-move deltas.
        overlay.applyTransform(drag, {
            "a": 1,
            "b": 0,
            "c": 0,
            "d": 1,
            "e": image.x - drag.pressImage.x,
            "f": image.y - drag.pressImage.y
        });
        return true;
    }

    function applyTransform(drag, affine) {
        if (!overlay.roto.updateSelectionTransform(drag.token, affine.a, affine.b, affine.c, affine.d,
                                                   affine.e, affine.f)) {
            overlay.roto.cancelGesture(drag.token);
            overlay.drag = null;
            overlay.marquee = null;
            overlay.clearHover();
            canvas.requestPaint();
            return;
        }
        canvas.requestPaint();
    }

    function applyGesture(drag, values) {
        if (!overlay.roto.updateGesture(drag.token, values)) {
            overlay.roto.cancelGesture(drag.token);
            overlay.drag = null;
            overlay.clearHover();
            canvas.requestPaint();
        }
    }

    function release(px, py) {
        if (!overlay.active)
            return false;
        if (overlay.draftDragging) {
            overlay.draftDragging = false;
            var view = overlay.mapping();
            if (view) {
                var image = overlay.toImage(view, px, py);
                overlay.roto.updateDraft(image.x, image.y);
            }
            overlay.roto.commitDraft();
            canvas.requestPaint();
            return true;
        }
        if (overlay.roto.draftActive)
            return false;
        var drag = overlay.drag;
        if (drag === null)
            return false;
        overlay.drag = null;
        var kind = String(drag.kind);
        if (kind === "marquee") {
            overlay.marquee = null;
            overlay.finishMarquee(drag, px, py);
            canvas.requestPaint();
            return true;
        }
        if (kind === "pivot") {
            canvas.requestPaint();
            return true;
        }
        var token = String(drag.token || "");
        if (token.length === 0) {
            canvas.requestPaint();
            return true;
        }
        if (drag.moved)
            overlay.roto.commitGesture(token);
        else
            overlay.roto.cancelGesture(token);
        overlay.updateHover(px, py);
        canvas.requestPaint();
        return true;
    }

    // One marquee: every point of every editable shape inside it. A drag that
    // never moved is a click on empty space, which clears the selection unless
    // the modifier says the artist is adding to it.
    function finishMarquee(drag, px, py) {
        if (!overlay.roto)
            return;
        if (!drag.moved) {
            if (!drag.additive)
                overlay.roto.clearSelection();
            overlay.pivot = null;
            return;
        }
        var left = Math.min(drag.x0, px);
        var right = Math.max(drag.x0, px);
        var top = Math.min(drag.y0, py);
        var bottom = Math.max(drag.y0, py);
        var list = overlay.editableShapes();
        var ids = [];
        for (var index = 0; index < list.length; ++index) {
            var points = overlay.pointsOf(list[index]);
            for (var point = 0; point < points.length; ++point) {
                var screen = overlay.toScreen(drag.view, points[point].x, points[point].y);
                if (screen.x < left || screen.x > right || screen.y < top || screen.y > bottom)
                    continue;
                ids.push(String(points[point].id));
            }
        }
        overlay.roto.setPointSelection(ids, drag.additive);
        overlay.pivot = null;
    }

    // Escape, focus loss, a target/frame change, or an interruption from the
    // shared history owner: the preview is discarded and nothing is published.
    function cancelAll() {
        overlay.draftDragging = false;
        var drag = overlay.drag;
        overlay.drag = null;
        overlay.marquee = null;
        overlay.clearHover();
        var token = drag ? String(drag.token || "") : "";
        if (token.length > 0 && overlay.roto)
            overlay.roto.cancelGesture(token);
        if (overlay.roto)
            overlay.roto.cancelDraft();
        canvas.requestPaint();
    }

    // The viewer panel hands every key to the overlay first. Only the keys the
    // overlay truly owns are consumed, so transport, viewer switching and every
    // other shared shortcut keep working.
    function handleKey(key, modifiers) {
        if (!overlay.active)
            return false;
        var shift = !!(modifiers & Qt.ShiftModifier);
        var control = !!(modifiers & Qt.ControlModifier);
        if (key === Qt.Key_Escape) {
            if (overlay.drag === null && !overlay.draftDragging && overlay.marquee === null
                    && overlay.roto.draftActive !== true)
                return false;
            overlay.cancelAll();
            return true;
        }
        if (overlay.roto.draftActive) {
            if (key === Qt.Key_Return || key === Qt.Key_Enter) {
                overlay.draftDragging = false;
                overlay.roto.commitDraft();
                canvas.requestPaint();
                return true;
            }
            return false;
        }
        if (control && key === Qt.Key_A) {
            if (!overlay.hasSelection() && overlay.editableShapes().length === 0)
                return false;
            overlay.roto.selectAllPoints();
            overlay.pivot = null;
            canvas.requestPaint();
            return true;
        }
        if (key === Qt.Key_Q && !control && !shift) {
            overlay.roto.setTool("select");
            return true;
        }
        if (key === Qt.Key_Z && !control) {
            if (!overlay.hasSelection())
                return false;
            overlay.roto.smoothSelection(!shift);
            canvas.requestPaint();
            return true;
        }
        if (key === Qt.Key_Delete || key === Qt.Key_Backspace) {
            if (!overlay.hasSelection())
                return false;
            overlay.roto.deleteSelection();
            overlay.pivot = null;
            canvas.requestPaint();
            return true;
        }
        return false;
    }

    // --- drawing -----------------------------------------------------------
    function drawScene(ctx, width, height) {
        ctx.reset();
        if (!overlay.active || !overlay.panel)
            return;
        var view = overlay.mapping();
        if (!view)
            return;
        ctx.save();
        ctx.beginPath();
        ctx.rect(0, 0, width, height);
        ctx.clip();
        var accent = overlay.themeColor("accent", "#3485f6");
        var text = overlay.themeColor("text", "#dce0e6");
        var muted = overlay.themeColor("muted", "#979ea8");
        var list = overlay.shapes();
        for (var index = 0; index < list.length; ++index) {
            var record = list[index];
            if (record.group || !record.visible)
                continue;
            var selected = record.selected === true || String(record.id) === String(overlay.roto.selectedElement);
            var hovered = overlay.hoverTarget !== null
                    && String(overlay.hoverTarget.element || "") === String(record.id);
            overlay.strokeShape(ctx, record, view, selected ? accent : (hovered ? text : muted), selected ? 2 : 1);
        }
        // Every visible control point is drawn, so the contour can be edited by
        // its points instead of only by its fill.
        for (var marker = 0; marker < list.length; ++marker) {
            var shape = list[marker];
            if (shape.group || !shape.visible || overlay.lockedInHierarchy(shape))
                continue;
            overlay.drawPoints(ctx, shape, view, accent, text);
        }
        var entries = overlay.selectedPointEntries();
        for (var entry = 0; entry < entries.length; ++entry)
            overlay.drawPointHandles(ctx, entries[entry], view, accent, text);
        overlay.drawSelectionBox(ctx, accent, text);
        overlay.drawMarquee(ctx, accent);
        if (overlay.roto.draftActive)
            overlay.drawDraft(ctx, view, accent, text);
        ctx.restore();
    }

    function strokeShape(ctx, record, view, color, lineWidth) {
        var points = overlay.pointsOf(record);
        if (points.length < 2)
            return;
        ctx.save();
        ctx.strokeStyle = color;
        ctx.lineWidth = lineWidth;
        overlay.traceShape(ctx, record, points, view);
        ctx.stroke();
        ctx.restore();
    }

    function traceShape(ctx, record, points, view) {
        ctx.beginPath();
        ctx.fillRule = Qt.OddEvenFill;
        if (String(record.kind) === "bspline") {
            overlay.bsplinePath(ctx, points, view);
        } else {
            var start = overlay.toScreen(view, points[0].x, points[0].y);
            ctx.moveTo(start.x, start.y);
            for (var index = 0; index < points.length; ++index) {
                var from = points[index];
                var to = points[(index + 1) % points.length];
                var control1 = overlay.toScreen(view, from.x + from.outX, from.y + from.outY);
                var control2 = overlay.toScreen(view, to.x + to.inX, to.y + to.inY);
                var end = overlay.toScreen(view, to.x, to.y);
                ctx.bezierCurveTo(control1.x, control1.y, control2.x, control2.y, end.x, end.y);
            }
        }
        ctx.closePath();
    }

    // The renderer's periodic uniform cubic with the tension blend toward the
    // control polygon (0 smooth, 1 the polygon itself), sampled for display.
    function bsplinePath(ctx, points, view) {
        var count = points.length;
        for (var index = 0; index < count; ++index) {
            var p0 = points[(index + count - 1) % count];
            var p1 = points[index];
            var p2 = points[(index + 1) % count];
            var p3 = points[(index + 2) % count];
            for (var step = 0; step <= 32; ++step) {
                var t = step / 32;
                var tension = (1 - t) * Number(p1.tension) + t * Number(p2.tension);
                var t2 = t * t;
                var t3 = t2 * t;
                var x = ((1 - t) * (1 - t) * (1 - t) * p0.x + (3 * t3 - 6 * t2 + 4) * p1.x
                        + (-3 * t3 + 3 * t2 + 3 * t + 1) * p2.x + t3 * p3.x) / 6;
                var y = ((1 - t) * (1 - t) * (1 - t) * p0.y + (3 * t3 - 6 * t2 + 4) * p1.y
                        + (-3 * t3 + 3 * t2 + 3 * t + 1) * p2.y + t3 * p3.y) / 6;
                var chordX = p1.x + (p2.x - p1.x) * t;
                var chordY = p1.y + (p2.y - p1.y) * t;
                var screen = overlay.toScreen(view, x + (chordX - x) * tension, y + (chordY - y) * tension);
                if (index === 0 && step === 0)
                    ctx.moveTo(screen.x, screen.y);
                else
                    ctx.lineTo(screen.x, screen.y);
            }
        }
    }

    // Unselected points are small squares, a hovered point grows and takes the
    // accent, and selected points are the large filled markers: the three
    // states never read as each other.
    function drawPoints(ctx, record, view, accent, text) {
        var points = overlay.pointsOf(record);
        for (var index = 0; index < points.length; ++index) {
            var point = points[index];
            var screen = overlay.toScreen(view, point.x, point.y);
            ctx.save();
            ctx.lineWidth = 1;
            if (point.selected === true) {
                ctx.fillStyle = accent;
                ctx.strokeStyle = text;
                ctx.beginPath();
                ctx.rect(screen.x - 4, screen.y - 4, 8, 8);
                ctx.fill();
                ctx.stroke();
            } else if (overlay.pointHovered(record, point)) {
                ctx.fillStyle = accent;
                ctx.strokeStyle = text;
                ctx.beginPath();
                ctx.rect(screen.x - 3, screen.y - 3, 6, 6);
                ctx.fill();
                ctx.stroke();
            } else {
                ctx.fillStyle = text;
                ctx.strokeStyle = accent;
                ctx.beginPath();
                ctx.rect(screen.x - 2.5, screen.y - 2.5, 5, 5);
                ctx.fill();
                ctx.stroke();
            }
            ctx.restore();
        }
    }

    function drawPointHandles(ctx, entry, view, accent, text) {
        var record = entry.record;
        var point = entry.point;
        var origin = overlay.toScreen(view, point.x, point.y);
        ctx.save();
        ctx.strokeStyle = accent;
        ctx.lineWidth = 1;
        if (String(record.kind) !== "bspline") {
            var inHandle = overlay.toScreen(view, point.x + point.inX, point.y + point.inY);
            var outHandle = overlay.toScreen(view, point.x + point.outX, point.y + point.outY);
            ctx.beginPath();
            ctx.moveTo(inHandle.x, inHandle.y);
            ctx.lineTo(origin.x, origin.y);
            ctx.lineTo(outHandle.x, outHandle.y);
            ctx.stroke();
            overlay.drawMarker(ctx, inHandle.x, inHandle.y, accent, text, false);
            overlay.drawMarker(ctx, outHandle.x, outHandle.y, accent, text, false);
        }
        // Per-point feather: the signed outward distance the renderer ramps
        // over, with a grab handle at its end.
        var tip = overlay.toScreen(view, point.tipX, point.tipY);
        ctx.beginPath();
        ctx.moveTo(origin.x, origin.y);
        ctx.lineTo(tip.x, tip.y);
        ctx.stroke();
        overlay.drawMarker(ctx, tip.x, tip.y, accent, text, true);
        ctx.restore();
    }

    function drawSelectionBox(ctx, accent, text) {
        var box = overlay.selectionGeometry();
        if (!box)
            return;
        var area = box.area;
        ctx.save();
        ctx.lineWidth = 1;
        ctx.strokeStyle = accent;
        ctx.fillStyle = accent;
        // A faint tint states the area the transform owns without hiding the
        // image underneath it.
        ctx.globalAlpha = 0.07;
        ctx.fillRect(area.left, area.top, area.right - area.left, area.bottom - area.top);
        ctx.globalAlpha = 1;
        if (typeof ctx.setLineDash === "function")
            ctx.setLineDash([4, 3]);
        ctx.strokeRect(area.left, area.top, area.right - area.left, area.bottom - area.top);
        if (typeof ctx.setLineDash === "function")
            ctx.setLineDash([]);
        var names = overlay.scaleHandles;
        for (var index = 0; index < names.length; ++index) {
            var handle = box[names[index]];
            ctx.fillStyle = text;
            ctx.beginPath();
            ctx.rect(handle.x - 3, handle.y - 3, 6, 6);
            ctx.fill();
            ctx.stroke();
        }
        ctx.beginPath();
        ctx.moveTo(box.rotate.x, area.top);
        ctx.lineTo(box.rotate.x, box.rotate.y);
        ctx.stroke();
        ctx.fillStyle = text;
        ctx.beginPath();
        ctx.arc(box.rotate.x, box.rotate.y, 4, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
        // The transient pivot: a crosshair the artist can drag, never state.
        ctx.beginPath();
        ctx.moveTo(box.pivot.x - 6, box.pivot.y);
        ctx.lineTo(box.pivot.x + 6, box.pivot.y);
        ctx.moveTo(box.pivot.x, box.pivot.y - 6);
        ctx.lineTo(box.pivot.x, box.pivot.y + 6);
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(box.pivot.x, box.pivot.y, 2, 0, Math.PI * 2);
        ctx.stroke();
        ctx.restore();
    }

    function drawMarquee(ctx, accent) {
        var rect = overlay.marquee;
        if (rect === null)
            return;
        ctx.save();
        ctx.strokeStyle = accent;
        ctx.fillStyle = accent;
        ctx.lineWidth = 1;
        if (typeof ctx.setLineDash === "function")
            ctx.setLineDash([4, 3]);
        ctx.globalAlpha = 0.12;
        ctx.fillRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        ctx.globalAlpha = 1;
        ctx.strokeRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        if (typeof ctx.setLineDash === "function")
            ctx.setLineDash([]);
        ctx.restore();
    }

    function drawMarker(ctx, x, y, accent, text, diamond) {
        ctx.fillStyle = text;
        ctx.strokeStyle = accent;
        if (diamond) {
            ctx.beginPath();
            ctx.moveTo(x, y - 4);
            ctx.lineTo(x + 4, y);
            ctx.lineTo(x, y + 4);
            ctx.lineTo(x - 4, y);
            ctx.closePath();
            ctx.fill();
            ctx.stroke();
            return;
        }
        ctx.fillRect(x - 3, y - 3, 6, 6);
        ctx.strokeRect(x - 3.5, y - 3.5, 7, 7);
    }

    function drawDraft(ctx, view, accent, text) {
        var draft = overlay.roto.draftPoints || [];
        if (draft.length === 0)
            return;
        var kind = String(overlay.roto.draftKind);
        ctx.save();
        ctx.strokeStyle = accent;
        ctx.lineWidth = 1;
        ctx.beginPath();
        if ((kind === "rectangle" || kind === "ellipse") && draft.length >= 2) {
            var a = overlay.toScreen(view, draft[0][0], draft[0][1]);
            var b = overlay.toScreen(view, draft[draft.length - 1][0], draft[draft.length - 1][1]);
            if (kind === "rectangle") {
                ctx.rect(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.abs(b.x - a.x), Math.abs(b.y - a.y));
            } else {
                ctx.ellipse(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.abs(b.x - a.x), Math.abs(b.y - a.y));
            }
        } else {
            for (var index = 0; index < draft.length; ++index) {
                var screen = overlay.toScreen(view, draft[index][0], draft[index][1]);
                if (index === 0)
                    ctx.moveTo(screen.x, screen.y);
                else
                    ctx.lineTo(screen.x, screen.y);
            }
        }
        ctx.stroke();
        for (var marker = 0; marker < draft.length; ++marker) {
            var at = overlay.toScreen(view, draft[marker][0], draft[marker][1]);
            overlay.drawMarker(ctx, at.x, at.y, accent, text, false);
        }
        ctx.restore();
    }

    // --- presentation ------------------------------------------------------
    Canvas {
        id: canvas
        objectName: "rotoCanvas_" + overlay.panelId
        anchors.fill: parent
        visible: overlay.active
        onPaint: {
            if (available)
                overlay.drawScene(getContext("2d"), width, height)
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: requestPaint()
        onAvailableChanged: if (available) requestPaint()
        Component.onCompleted: requestPaint()
        Connections {
            target: overlay.panel
            function onViewZoomChanged() { canvas.requestPaint() }
            function onViewPanXChanged() { canvas.requestPaint() }
            function onViewPanYChanged() { canvas.requestPaint() }
            function onViewFittedChanged() { canvas.requestPaint() }
        }
        Connections {
            target: overlay.panel ? overlay.panel.controller : null
            function onFrameArrived() { canvas.requestPaint() }
        }
        Connections {
            target: overlay.roto
            function onDataChanged() { canvas.requestPaint() }
            function onSelectionChanged() {
                overlay.pivot = null
                canvas.requestPaint()
            }
            function onDraftChanged() { canvas.requestPaint() }
            function onToolChanged() { canvas.requestPaint() }
        }
        Connections {
            target: overlay.theme
            function onPresetChanged() { canvas.requestPaint() }
            function onAccentOverrideChanged() { canvas.requestPaint() }
        }
    }

    // The compact viewport tool strip: glyph buttons with an accessible name and
    // a tooltip that states the shortcut. The buttons never take focus, so
    // Escape and Enter still reach the panel that owns the live draft.
    Row {
        id: toolbar
        objectName: "rotoToolbar_" + overlay.panelId
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 6
        spacing: 2

        Repeater {
            model: overlay.toolEntries
            delegate: Button {
                id: toolButton
                required property var modelData
                objectName: "rotoTool_" + modelData.key + "_" + overlay.panelId
                width: 24
                height: 24
                padding: 0
                focusPolicy: Qt.NoFocus
                Accessible.name: modelData.name
                Accessible.role: Accessible.Button
                ToolTip.visible: hovered
                ToolTip.text: modelData.hint.length > 0 ? modelData.name + " (" + modelData.hint + ")" : modelData.name
                onClicked: overlay.roto.setTool(modelData.key)
                contentItem: Text {
                    text: toolButton.modelData.glyph
                    color: overlay.tool === toolButton.modelData.key ? overlay.themeColor("accent", "#3485f6")
                                                                     : overlay.themeColor("text", "#dce0e6")
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: overlay.theme ? overlay.theme.smallRadius : 4
                    color: toolButton.down ? overlay.themeColor("raised", "#282c31")
                                           : toolButton.hovered ? overlay.themeColor("hover", "#343940") : "transparent"
                    border.width: overlay.tool === toolButton.modelData.key ? 1 : 0
                    border.color: overlay.themeColor("accent", "#3485f6")
                }
            }
        }
    }
}
