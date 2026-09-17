import QtQuick
import QtQuick.Controls

// Roto viewport overlay (issue #93), mounted under the image area of the panel
// that DIRECTLY views a Roto node the same group's inspector has open.
//
// The overlay owns presentation only: it draws the evaluated geometry the
// shared RotoController publishes and translates the pointer into that
// controller's existing parameter gestures. Authored state, undo and keying
// stay in the document/session; the tool, the selection and the in-progress
// topology draft live in the controller, so this overlay and the inspector's
// shape list share one selection for the same node.
//
// The pointer stays with the panel's own pan gesture: this item installs no
// MouseArea, so a blank press still pans, the middle drag and the wheel keep
// working, and there is exactly ONE hit test per press. A live drag draws from
// its own frozen mapping and its own local preview, because a session gesture
// publishes nothing until it is committed.
Item {
    id: overlay

    property var theme: null
    // The viewer panel: the shared image mapping and the accepted pan/zoom
    // state. The panel calls the interaction functions below.
    property var panel: null
    // The shared per-node authoring adapter, or null when this panel does not
    // directly view an inspected Roto.
    property var roto: null
    // Panel identity, so a native test can address the controls.
    property string panelId: ""

    readonly property bool active: overlay.roto !== null && overlay.roto.available === true
    // The one live pointer edit (gesture token plus its local preview) or null.
    property var drag: null
    readonly property bool gestureLive: overlay.drag !== null || (overlay.roto !== null && overlay.roto.draftActive)
    // True while a rectangle/ellipse drag owns the draft's second corner.
    property bool draftDragging: false

    objectName: "rotoOverlay_" + overlay.panelId
    anchors.fill: parent
    visible: overlay.active

    readonly property real handleRadius: 6
    readonly property real pointRadius: 5
    readonly property string tool: overlay.roto ? String(overlay.roto.tool) : "select"
    readonly property var toolEntries: [{
        "key": "select",
        "glyph": "\u2196",
        "name": "Select"
    }, {
        "key": "bezier",
        "glyph": "\u2312",
        "name": "Bézier"
    }, {
        "key": "bspline",
        "glyph": "\u223F",
        "name": "B-spline"
    }, {
        "key": "rectangle",
        "glyph": "\u25AD",
        "name": "Rectangle"
    }, {
        "key": "ellipse",
        "glyph": "\u25EF",
        "name": "Ellipse"
    }]

    function themeColor(role, fallback) {
        return overlay.panel ? overlay.panel.themeColor(role, fallback) : fallback;
    }

    // --- image <-> screen --------------------------------------------------
    // ONE mapping serves drawing and the pointer, exactly like the accepted
    // crop overlay, and a live gesture keeps the mapping it started on.
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

    // The element's own local -> image affine, published with the geometry.
    function place(record, x, y) {
        return {
            "x": record.a * x + record.c * y + record.e,
            "y": record.b * x + record.d * y + record.f
        };
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

    function unplace(record, x, y) {
        return overlay.unplaceVector(record, x - record.e, y - record.f);
    }

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

    function selectedShape() {
        return overlay.roto ? overlay.shapeById(overlay.roto.selectedElement) : null;
    }

    function pointById(record, id) {
        if (!record)
            return null;
        var points = record.points || [];
        for (var index = 0; index < points.length; ++index)
            if (String(points[index].id) === String(id))
                return points[index];
        return null;
    }

    function near(px, py, x, y, radius) {
        return Math.abs(px - x) <= radius && Math.abs(py - y) <= radius;
    }

    // The previewed geometry of one point: a live drag replaces the authored
    // values locally, because the session preview is not published until the
    // gesture commits. The handle therefore always sits under the pointer.
    function previewPoint(record, point) {
        if (!overlay.drag || String(overlay.drag.element) !== String(record.id))
            return point;
        var edited = overlay.drag.local[String(point.id)];
        if (!edited)
            return point;
        var world = overlay.drag.kind === "position"
                ? overlay.place(record, edited.x, edited.y) : { "x": point.x, "y": point.y };
        var inHandle = overlay.drag.kind === "tangent"
                ? overlay.placeVector(record, edited.inX, edited.inY) : { "x": point.inX, "y": point.inY };
        var outHandle = overlay.drag.kind === "tangent"
                ? overlay.placeVector(record, edited.outX, edited.outY) : { "x": point.outX, "y": point.outY };
        var effective = overlay.drag.kind === "feather" && overlay.drag.distance[String(point.id)] !== undefined
                ? overlay.drag.distance[String(point.id)] : point.effectiveFeather;
        // Effective feather and the normal are already in image coordinates.
        return {
            "id": point.id,
            "index": point.index,
            "x": world.x,
            "y": world.y,
            "inX": inHandle.x,
            "inY": inHandle.y,
            "outX": outHandle.x,
            "outY": outHandle.y,
            "nx": point.nx,
            "ny": point.ny,
            "feather": point.feather,
            "effectiveFeather": effective,
            "tipX": world.x + point.nx * effective,
            "tipY": world.y + point.ny * effective,
            "tension": point.tension,
            "selected": point.selected
        };
    }

    function previewPoints(record) {
        var points = record.points || [];
        var result = [];
        for (var index = 0; index < points.length; ++index)
            result.push(overlay.previewPoint(record, points[index]));
        return result;
    }

    // --- hit testing -------------------------------------------------------
    // {kind, element, point, key}: a feather tip or tangent handle of a
    // selected point, any point of a visible shape, a shape's own area, or
    // nothing — the panel then pans.
    function hitTest(px, py) {
        if (!overlay.active)
            return {};
        var view = overlay.mapping();
        if (!view)
            return {};
        if (overlay.roto.draftActive)
            return {
                "kind": "draft"
            };
        // A handle is only addressable when it is visibly off its point: a
        // zero tangent or a zero feather sits exactly on the point, so it must
        // never shadow dragging that point.
        var selected = overlay.selectedShape();
        if (selected && !selected.locked && String(selected.kind) !== "bspline") {
            var handles = overlay.previewPoints(selected);
            for (var index = 0; index < handles.length; ++index) {
                var point = handles[index];
                if (!point.selected)
                    continue;
                var origin = overlay.toScreen(view, point.x, point.y);
                var inHandle = overlay.toScreen(view, point.x + point.inX, point.y + point.inY);
                if (Math.hypot(inHandle.x - origin.x, inHandle.y - origin.y) > overlay.handleRadius
                        && overlay.near(px, py, inHandle.x, inHandle.y, overlay.handleRadius))
                    return {
                        "kind": "tangent",
                        "element": selected.id,
                        "point": point.id,
                        "key": "inTangent"
                    };
                var outHandle = overlay.toScreen(view, point.x + point.outX, point.y + point.outY);
                if (Math.hypot(outHandle.x - origin.x, outHandle.y - origin.y) > overlay.handleRadius
                        && overlay.near(px, py, outHandle.x, outHandle.y, overlay.handleRadius))
                    return {
                        "kind": "tangent",
                        "element": selected.id,
                        "point": point.id,
                        "key": "outTangent"
                    };
            }
        }
        if (selected && !selected.locked) {
            var featherPoints = overlay.previewPoints(selected);
            for (var featherIndex = 0; featherIndex < featherPoints.length; ++featherIndex) {
                var featherPoint = featherPoints[featherIndex];
                if (!featherPoint.selected)
                    continue;
                var featherOrigin = overlay.toScreen(view, featherPoint.x, featherPoint.y);
                var tip = overlay.toScreen(view, featherPoint.tipX, featherPoint.tipY);
                if (Math.hypot(tip.x - featherOrigin.x, tip.y - featherOrigin.y) > overlay.handleRadius
                        && overlay.near(px, py, tip.x, tip.y, overlay.handleRadius))
                    return {
                        "kind": "feather",
                        "element": selected.id,
                        "point": featherPoint.id
                    };
            }
        }
        var list = overlay.shapes();
        for (var shape = list.length - 1; shape >= 0; --shape) {
            var record = list[shape];
            if (!record.visible || record.group)
                continue;
            var points = overlay.previewPoints(record);
            for (var pointIndex = 0; pointIndex < points.length; ++pointIndex) {
                var screen = overlay.toScreen(view, points[pointIndex].x, points[pointIndex].y);
                if (overlay.near(px, py, screen.x, screen.y, overlay.pointRadius + 2))
                    return {
                        "kind": "point",
                        "element": record.id,
                        "point": points[pointIndex].id
                    };
            }
        }
        for (var area = list.length - 1; area >= 0; --area) {
            var candidate = list[area];
            if (!candidate.visible || candidate.group)
                continue;
            if (overlay.insideShape(view, candidate, px, py))
                return {
                    "kind": "element",
                    "element": candidate.id
                };
        }
        return {};
    }

    function insideShape(view, record, px, py) {
        var points = overlay.previewPoints(record);
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

    function cursorShape(px, py) {
        if (!overlay.active)
            return Qt.ArrowCursor;
        if (overlay.roto.draftActive || overlay.tool !== "select")
            return Qt.CrossCursor;
        var target = overlay.hitTest(px, py);
        if (target.kind === "feather" || target.kind === "tangent" || target.kind === "point"
                || target.kind === "element")
            return Qt.SizeAllCursor;
        return Qt.ArrowCursor;
    }

    // --- gestures ----------------------------------------------------------
    // One numeric edit = one gesture over the element/point-scoped addresses of
    // the session, so undo, keying and validation are the shared ones. The
    // release publishes exactly one history entry; a press that never moved
    // cancels, so a click never writes a no-op entry.
    function beginPosition(target, additive) {
        var record = overlay.shapeById(target.element);
        var point = overlay.pointById(record, target.point);
        if (!record || !point || record.locked)
            return false;
        var selectedShape = String(overlay.roto.selectedElement || "");
        var current = overlay.roto.selectedPoints || [];
        var inSelection = selectedShape === String(record.id) && current.indexOf(String(point.id)) >= 0;
        if (!inSelection) {
            if (selectedShape !== String(record.id) && !overlay.roto.selectElement(String(record.id), additive))
                return false;
            if (!overlay.roto.selectPoint(String(point.id), additive))
                return false;
        }
        var ids = overlay.roto.selectedPoints || [];
        var targets = [];
        var origin = {};
        for (var index = 0; index < ids.length; ++index) {
            var entry = overlay.pointById(record, ids[index]);
            if (!entry)
                continue;
            var local = overlay.unplace(record, entry.x, entry.y);
            origin[String(entry.id)] = {
                "x": local.x,
                "y": local.y,
                "inX": overlay.unplaceVector(record, entry.inX, entry.inY).x,
                "inY": overlay.unplaceVector(record, entry.inX, entry.inY).y,
                "outX": overlay.unplaceVector(record, entry.outX, entry.outY).x,
                "outY": overlay.unplaceVector(record, entry.outX, entry.outY).y
            };
            targets.push({
                "element": String(record.id),
                "point": String(entry.id),
                "key": "position"
            });
        }
        if (targets.length === 0)
            return false;
        var token = String(overlay.roto.beginGesture(targets));
        if (token.length === 0)
            return false;
        overlay.drag = {
            "kind": "position",
            "token": token,
            "element": String(record.id),
            "key": "position",
            "ids": Object.keys(origin),
            "origin": origin,
            "local": ({}),
            "distance": ({}),
            "view": overlay.mapping(),
            "pressX": 0,
            "pressY": 0,
            "moved": false
        };
        return true;
    }

    function beginTangent(target) {
        var record = overlay.shapeById(target.element);
        var point = overlay.pointById(record, target.point);
        if (!record || !point || record.locked)
            return false;
        var smooth = Math.abs(point.inX + point.outX) < 0.01 && Math.abs(point.inY + point.outY) < 0.01;
        var draggedKey = String(target.key);
        var mirrorKey = draggedKey === "inTangent" ? "outTangent" : "inTangent";
        var targets = [{
            "element": String(record.id),
            "point": String(point.id),
            "key": draggedKey
        }];
        // A smooth point mirrors: dragging one handle moves its reflection, in
        // the same one preview and one history entry. A broken point moves only
        // the handle under the pointer.
        if (smooth)
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
            "x": 0,
            "y": 0,
            "inX": inLocal.x,
            "inY": inLocal.y,
            "outX": outLocal.x,
            "outY": outLocal.y
        };
        overlay.drag = {
            "kind": "tangent",
            "token": token,
            "element": String(record.id),
            "key": draggedKey,
            "mirror": smooth,
            "ids": [String(point.id)],
            "origin": origin,
            "local": ({}),
            "distance": ({}),
            "view": overlay.mapping(),
            "pressX": 0,
            "pressY": 0,
            "moved": false
        };
        return true;
    }

    function beginFeather(target) {
        var record = overlay.shapeById(target.element);
        var point = overlay.pointById(record, target.point);
        if (!record || !point || record.locked || !(record.featherScale > 0))
            return false;
        var token = String(overlay.roto.beginGesture([{
            "element": String(record.id),
            "point": String(point.id),
            "key": "feather"
        }]));
        if (token.length === 0)
            return false;
        var origin = {};
        origin[String(point.id)] = {
            "x": 0,
            "y": 0,
            "inX": 0,
            "inY": 0,
            "outX": 0,
            "outY": 0
        };
        overlay.drag = {
            "kind": "feather",
            "token": token,
            "element": String(record.id),
            "key": "feather",
            "ids": [String(point.id)],
            "origin": origin,
            "local": ({}),
            "distance": ({}),
            "base": Number(point.effectiveFeather),
            "bias": Number(record.featherBias),
            "scale": Number(record.featherScale),
            "nx": Number(point.nx),
            "ny": Number(point.ny),
            "view": overlay.mapping(),
            "pressX": 0,
            "pressY": 0,
            "moved": false
        };
        return true;
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
        if (target.kind === "feather")
            overlay.beginFeather(target);
        else if (target.kind === "tangent")
            overlay.beginTangent(target);
        else if (target.kind === "point")
            overlay.beginPosition(target, additive);
        else if (target.kind === "element")
            overlay.roto.selectElement(String(target.element), additive);
        else
            return false;
        if (overlay.drag) {
            overlay.drag.pressX = px;
            overlay.drag.pressY = py;
            overlay.drag.view = view;
        }
        canvas.requestPaint();
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
        // between two clicks draws rather than panning. The middle drag and the
        // wheel stay the panel's, so the view is still navigable while drawing.
        if (overlay.roto.draftActive) {
            var pathView = overlay.mapping();
            if (!pathView)
                return true;
            var pathImage = overlay.toImage(pathView, px, py);
            overlay.roto.updateDraft(pathImage.x, pathImage.y);
            return true;
        }
        if (!overlay.drag)
            return false;
        var view = overlay.drag.view;
        var deltaX = (px - overlay.drag.pressX) / view.sx;
        var deltaY = (py - overlay.drag.pressY) / view.scale;
        var record = overlay.shapeById(overlay.drag.element);
        if (!record)
            return true;
        var values = [];
        if (overlay.drag.kind === "position") {
            for (var index = 0; index < overlay.drag.ids.length; ++index) {
                var id = overlay.drag.ids[index];
                var base = overlay.drag.origin[id];
                if (!base)
                    continue;
                var start = overlay.place(record, base.x, base.y);
                var next = overlay.unplace(record, start.x + deltaX, start.y + deltaY);
                overlay.drag.local[id] = {
                    "x": next.x,
                    "y": next.y,
                    "inX": base.inX,
                    "inY": base.inY,
                    "outX": base.outX,
                    "outY": base.outY
                };
                values.push([next.x, next.y]);
            }
        } else if (overlay.drag.kind === "tangent") {
            var pointId = overlay.drag.ids[0];
            var tangentBase = overlay.drag.origin[pointId];
            var draggingIn = overlay.drag.key === "inTangent";
            var dragged = draggingIn ? {
                "x": tangentBase.inX,
                "y": tangentBase.inY
            } : {
                "x": tangentBase.outX,
                "y": tangentBase.outY
            };
            var handle = overlay.placeVector(record, dragged.x, dragged.y);
            var moved = overlay.unplaceVector(record, handle.x + deltaX, handle.y + deltaY);
            var preview = {
                "x": 0,
                "y": 0,
                "inX": draggingIn ? moved.x : (overlay.drag.mirror ? -moved.x : tangentBase.inX),
                "inY": draggingIn ? moved.y : (overlay.drag.mirror ? -moved.y : tangentBase.inY),
                "outX": draggingIn ? (overlay.drag.mirror ? -moved.x : tangentBase.outX) : moved.x,
                "outY": draggingIn ? (overlay.drag.mirror ? -moved.y : tangentBase.outY) : moved.y
            };
            overlay.drag.local[pointId] = preview;
            values.push([moved.x, moved.y]);
            if (overlay.drag.mirror)
                values.push([-moved.x, -moved.y]);
        } else if (overlay.drag.kind === "feather") {
            var distance = overlay.drag.base + deltaX * overlay.drag.nx + deltaY * overlay.drag.ny;
            overlay.drag.distance[overlay.drag.ids[0]] = distance;
            values.push((distance - overlay.drag.bias) / overlay.drag.scale);
            overlay.drag.local[overlay.drag.ids[0]] = overlay.drag.origin[overlay.drag.ids[0]];
        }
        if (values.length === 0)
            return true;
        if (!overlay.roto.updateGesture(overlay.drag.token, values)) {
            overlay.roto.cancelGesture(overlay.drag.token);
            overlay.drag = null;
            canvas.requestPaint();
            return true;
        }
        overlay.drag.moved = true;
        canvas.requestPaint();
        return true;
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
            return true;
        }
        if (!overlay.drag)
            return false;
        var token = overlay.drag.token;
        var moved = overlay.drag.moved;
        overlay.drag = null;
        if (moved)
            overlay.roto.commitGesture(token);
        else
            overlay.roto.cancelGesture(token);
        canvas.requestPaint();
        return true;
    }

    // Escape, focus loss, a target/frame change, or an interruption from the
    // shared history owner: the preview is discarded and nothing is published.
    function cancelAll() {
        overlay.draftDragging = false;
        var token = overlay.drag ? overlay.drag.token : "";
        overlay.drag = null;
        if (token.length > 0 && overlay.roto)
            overlay.roto.cancelGesture(token);
        if (overlay.roto)
            overlay.roto.cancelDraft();
        canvas.requestPaint();
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
            var selected = String(record.id) === String(overlay.roto.selectedElement);
            overlay.strokeShape(ctx, record, view, selected ? accent : muted, selected ? 2 : 1);
            if (selected && !record.locked)
                overlay.drawHandles(ctx, record, view, accent, text);
        }
        if (overlay.roto.draftActive)
            overlay.drawDraft(ctx, view, accent, text);
        ctx.restore();
    }

    function strokeShape(ctx, record, view, color, lineWidth) {
        var points = overlay.previewPoints(record);
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

    function drawHandles(ctx, record, view, accent, text) {
        var points = overlay.previewPoints(record);
        var bspline = String(record.kind) === "bspline";
        for (var index = 0; index < points.length; ++index) {
            var point = points[index];
            var origin = overlay.toScreen(view, point.x, point.y);
            if (point.selected && !bspline) {
                // Tangent handles, drawn exactly where the renderer's control
                // points sit for the segment on either side.
                ctx.save();
                ctx.strokeStyle = accent;
                ctx.lineWidth = 1;
                var inHandle = overlay.toScreen(view, point.x + point.inX, point.y + point.inY);
                var outHandle = overlay.toScreen(view, point.x + point.outX, point.y + point.outY);
                ctx.beginPath();
                ctx.moveTo(inHandle.x, inHandle.y);
                ctx.lineTo(origin.x, origin.y);
                ctx.lineTo(outHandle.x, outHandle.y);
                ctx.stroke();
                overlay.drawMarker(ctx, inHandle.x, inHandle.y, accent, text, false);
                overlay.drawMarker(ctx, outHandle.x, outHandle.y, accent, text, false);
                ctx.restore();
            }
            if (point.selected || Math.abs(Number(point.effectiveFeather)) > 0.01) {
                // Per-point feather: the signed outward distance the renderer
                // ramps over, with a grab handle at its end.
                var tip = overlay.toScreen(view, point.tipX, point.tipY);
                ctx.save();
                ctx.strokeStyle = accent;
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.moveTo(origin.x, origin.y);
                ctx.lineTo(tip.x, tip.y);
                ctx.stroke();
                if (point.selected)
                    overlay.drawMarker(ctx, tip.x, tip.y, accent, text, true);
                ctx.restore();
            }
        }
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
            function onSelectionChanged() { canvas.requestPaint() }
            function onDraftChanged() { canvas.requestPaint() }
            function onToolChanged() { canvas.requestPaint() }
        }
        Connections {
            target: overlay.theme
            function onPresetChanged() { canvas.requestPaint() }
            function onAccentOverrideChanged() { canvas.requestPaint() }
        }
    }

    // The compact viewport tool strip: glyph buttons with an accessible name
    // and a tooltip. The buttons never take focus, so Escape and Enter still
    // reach the panel that owns the live draft.
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
                ToolTip.text: modelData.name
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
