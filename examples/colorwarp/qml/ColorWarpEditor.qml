import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Nemo

// ColorWarp mesh editor (issue #37, resolved from #28). The installed package
// declares it as editor "org.nemo.colorwarp.mesh" with presentation "section",
// so the generic inspector mounts it on the `strength` row. It CONSUMES the
// node's point keys (hue{i}, saturation{i}, pin{i}), so this ONE control
// presents the whole bounded wheel instead of 109 generic rows. It is ordinary
// packaged QML: `import Nemo` gives it the shared host facilities, and it
// queries only public inspector data and writes only through the shared panel
// gesture.
//
// Authored state is the typed parameters. Nothing here owns a mesh, a document,
// a history entry or an animation channel: every value is read through the
// shared `controller.parameterInspector` query and written through the shared
// panel gesture (begin/update/commit/cancel), so a drag is exactly one undo
// entry and a cancellation restores the values the drag started from. Pin is a
// protection flag ON the authored point; it is serialized and undoable like
// every other consumed key and never reaches the renderer. Strength scales the
// whole deformation from identity (all displacements zero, or strength zero, is
// exactly the identity mapping) and is presented through the host's own shared
// numeric control.
//
// Display contract: the wheel draws the ACTUAL deformation, never decoration.
// A control point sits at the mapping of its own knot position, and every drawn
// edge samples the same tensor-smoothstep mapping the node evaluates, so the
// drawn mesh IS the field being authored. The wheel's background is a
// display-only opponent-hue palette (the node's own opponent plane at a fixed
// display luminance); it never reaches the renderer.
//
// Coordinates, verbatim from the contract: u is periodic over [0,12) with one
// cell = 1/12 turn, v spans [0,4] with one cell = 1/4 radius; knots are (h,r)
// for spokes h=0..11 and rings r=0..4; the three editable interior rings r=1..3
// carry index i=(r-1)*12+h; the centre r=0 and the outer boundary r=4 are fixed
// at zero displacement. hue{i}/saturation{i} are DISPLACEMENTS in those cell
// coordinates, so the mapping is (u,v) + strength*(hue,saturation) with cubic
// smoothstep (t*t*(3-2t)) tensor weights, which is C1 across every cell edge.
//
// Refusal: the host's validator is the final authority. When a preview is
// refused (a fold-producing shape, or a value the catalog rejects) the refused
// shape is never shown: the editor drops the poisoned gesture and replaces it
// with a fresh one over the last ACCEPTED values, so the drag continues from the
// last valid mesh and the release still publishes one undoable edit.
ColumnLayout {
    id: colorWarp

    // Host-injected contract (ParametersPanel). A bare host may load this editor
    // without a controller; every control then states it cannot act.
    property var theme: null
    property string networkId: ""
    property string instanceId: ""
    property string nodeId: ""
    property string parameterKey: "strength"
    property var parameter
    property var controller
    property var panel

    readonly property color textColor: colorWarp.theme ? colorWarp.theme.text : "#dce0e6"
    readonly property color mutedColor: colorWarp.theme ? colorWarp.theme.muted : "#979ea8"
    readonly property color borderColor: colorWarp.theme ? colorWarp.theme.border : "#30343a"
    readonly property color fieldColor: colorWarp.theme ? colorWarp.theme.field : "#24272c"
    readonly property color panelColor: colorWarp.theme ? colorWarp.theme.panel : "#1e2023"
    readonly property color raisedColor: colorWarp.theme ? colorWarp.theme.raised : "#282c31"
    readonly property color hoverColor: colorWarp.theme ? colorWarp.theme.hover : "#343940"
    readonly property color accentColor: colorWarp.theme ? colorWarp.theme.accent : "#3485f6"
    readonly property color disabledColor: colorWarp.theme ? colorWarp.theme.disabled : "#5f6670"
    readonly property color errorColor: colorWarp.theme ? colorWarp.theme.errorText : "#f0d0d0"
    readonly property int smallRadiusValue: colorWarp.theme ? colorWarp.theme.smallRadius : 4
    readonly property int fontSizeValue: colorWarp.theme ? colorWarp.theme.fontSize : 11

    // --- fixed topology (approved design: 12 spokes x 3 interior rings) -----
    readonly property int spokeCount: 12
    readonly property int interiorRings: 3
    readonly property int boundaryRing: 4
    readonly property int pointCount: colorWarp.spokeCount * colorWarp.interiorRings
    // Display-only palette resolution: finer than the mesh so the background is
    // a smooth hue sweep, while the mesh itself keeps the authored 12 spokes.
    readonly property int paletteSectors: colorWarp.spokeCount * 6
    // The disc radius at which the palette takes its fully saturated hue color:
    // the node's radius-to-chroma inversion is out of gamut well before this, so
    // the wedges state the hue direction and the radial overlay carries the ramp.
    readonly property real paletteHueRadius: 0.6
    // The opponent plane's luma weights and the display luminance of the
    // palette. Neither is used by the renderer.
    readonly property real displayLuma: 0.5
    readonly property int wheelMaxSide: 340
    readonly property int wheelHitRadius: 9

    objectName: "colorWarpEditor_" + colorWarp.nodeId
    Layout.fillWidth: true
    spacing: 3

    // --- query coordinates -------------------------------------------------
    // The inspector query addresses the real node that owns the parameters (a
    // definition network for an occurrence exposure); gestures stay on the host
    // row, so an edit can never leave its own scope.
    readonly property string queryNetwork: colorWarp.parameter && colorWarp.parameter.targetNetwork !== undefined ? String(colorWarp.parameter.targetNetwork) : colorWarp.networkId
    readonly property string queryNode: colorWarp.parameter && colorWarp.parameter.targetNode !== undefined ? String(colorWarp.parameter.targetNode) : colorWarp.nodeId
    readonly property int revision: colorWarp.panel ? Number(colorWarp.panel.revision) : 0
    readonly property int frame: colorWarp.controller ? Number(colorWarp.controller.frame) : 0
    readonly property int dragThreshold: colorWarp.controller ? Number(colorWarp.controller.dragDistance) : 4

    // --- keys ---------------------------------------------------------------
    function pointIndexOf(spoke, ring) {
        return (ring - 1) * colorWarp.spokeCount + spoke;
    }
    function spokeOf(index) {
        return index % colorWarp.spokeCount;
    }
    function ringOf(index) {
        return 1 + Math.floor(index / colorWarp.spokeCount);
    }
    function hueKeyOf(index) {
        return "hue" + index;
    }
    function saturationKeyOf(index) {
        return "saturation" + index;
    }
    function pinKeyOf(index) {
        return "pin" + index;
    }

    // --- model --------------------------------------------------------------
    // Key -> the shared inspector row for that parameter. Re-queried whenever the
    // panel revision or the frame advances, so an animated coordinate states the
    // value at the current frame.
    property var paramRows: ({})
    // Accepted values during the one live gesture. The host defers structural
    // refresh, so numeric controls publish previews without rebuilding rows.
    property var previewValues: ({})
    property string numericPreviewToken: ""

    onRevisionChanged: colorWarp.refresh()
    onFrameChanged: colorWarp.refresh()
    onNodeIdChanged: {
        colorWarp.selectedIndex = -1;
        colorWarp.refresh();
    }
    onQueryNetworkChanged: colorWarp.refresh()
    onQueryNodeChanged: colorWarp.refresh()
    onControllerChanged: colorWarp.refresh()
    onPanelChanged: colorWarp.refresh()
    onParamRowsChanged: colorWarp.repaint()
    onPreviewValuesChanged: colorWarp.repaint()
    onSelectedIndexChanged: colorWarp.repaint()
    onHoverIndexChanged: colorWarp.repaint()
    Component.onCompleted: colorWarp.refresh()

    Connections {
        target: colorWarp.panel ? colorWarp.panel : null
        function onEditPreviewed(token, values) {
            var row = colorWarp.panel ? colorWarp.panel.activeRow : null;
            if (colorWarp.pointDrag === null && row
                    && String(token) === colorWarp.panelActiveToken
                    && String(row.networkId) === colorWarp.networkId
                    && String(row.nodeId) === colorWarp.nodeId) {
                colorWarp.numericPreviewToken = String(token);
                colorWarp.previewValues = values;
            }
        }
    }

    function refresh() {
        var next = ({});
        if (colorWarp.controller && colorWarp.queryNetwork.length > 0 && colorWarp.queryNode.length > 0) {
            var inspector = colorWarp.controller.parameterInspector(colorWarp.queryNetwork, colorWarp.queryNode);
            if (inspector && inspector.available === true) {
                var sections = inspector.sections || [];
                for (var i = 0; i < sections.length; ++i) {
                    var parameters = sections[i].parameters || [];
                    for (var j = 0; j < parameters.length; ++j) {
                        var entry = parameters[j];
                        if (entry && entry.key !== undefined)
                            next[String(entry.key)] = entry;
                    }
                }
            }
        }
        colorWarp.paramRows = next;
        // A frame refresh can run while a numeric preview is still unpublished.
        // Only its own live token may retain it; retirement or a successor clears it.
        if (colorWarp.pointDrag === null
                && (colorWarp.numericPreviewToken.length === 0
                    || colorWarp.numericPreviewToken !== colorWarp.panelActiveToken)) {
            colorWarp.numericPreviewToken = "";
            colorWarp.previewValues = ({});
        }
    }

    function paramRow(key) {
        var entry = colorWarp.paramRows[key];
        return entry !== undefined ? entry : null;
    }

    function numberValue(key) {
        var entry = colorWarp.paramRow(key);
        if (!entry || entry.value === undefined || entry.value === null || entry.value.length !== undefined)
            return 0;
        return Number(entry.value);
    }

    function boolValue(key) {
        var entry = colorWarp.paramRow(key);
        return entry !== undefined && entry !== null && entry.value === true;
    }

    // The value the display states: the live preview while a drag owns the point,
    // otherwise the authored value from the shared query.
    function effectiveNumber(key) {
        if (colorWarp.previewValues[key] !== undefined)
            return Number(colorWarp.previewValues[key]);
        return colorWarp.numberValue(key);
    }

    function rowFor(key) {
        return {
            "networkId": colorWarp.networkId,
            "nodeId": colorWarp.nodeId,
            "parameterKey": key,
            "parameter": colorWarp.paramRow(key),
            "label": key
        };
    }

    function keyStatusOf(key) {
        colorWarp.revision;
        colorWarp.frame;
        if (!colorWarp.panel || key.length === 0)
            return "none";
        return String(colorWarp.panel.parameterKeyStatusFor(colorWarp.networkId, colorWarp.nodeId, key));
    }

    function keyAtFrame(key) {
        if (!colorWarp.panel || key.length === 0)
            return false;
        return colorWarp.panel.keyParameterAtFrame(colorWarp.networkId, colorWarp.nodeId, key);
    }

    function removeKeyAtFrame(key) {
        if (!colorWarp.panel || key.length === 0)
            return false;
        return colorWarp.panel.removeParameterKeyAtFrame(colorWarp.networkId, colorWarp.nodeId, key);
    }

    function revealInAnimation(key) {
        if (!colorWarp.panel || !colorWarp.panel.revealInAnimation)
            return;
        colorWarp.panel.revealInAnimation(colorWarp.networkId, colorWarp.nodeId, key);
    }

    function revealAvailable() {
        return colorWarp.panel && colorWarp.panel.groupHasAnimationPanel ? colorWarp.panel.groupHasAnimationPanel() : false;
    }

    // --- shared key and exposure affordances --------------------------------
    // A consumed parameter has no generic row left, so the editor states those
    // same cells itself through the SAME shared owners: the label cell carries
    // the exposure drag and the Alt-click keying gesture, and the key cell owns
    // Set/Update/Remove Key and Show in Animation. No key state, command or
    // exposure rule lives here.
    readonly property var cellTheme: ({
            "text": colorWarp.textColor,
            "muted": colorWarp.mutedColor,
            "accent": colorWarp.accentColor,
            "border": colorWarp.borderColor,
            "hover": colorWarp.hoverColor,
            "field": colorWarp.fieldColor,
            "panel": colorWarp.panelColor,
            "raised": colorWarp.raisedColor,
            "disabled": colorWarp.disabledColor,
            "errorText": colorWarp.errorColor,
            "smallRadius": colorWarp.smallRadiusValue,
            "fontSize": colorWarp.fontSizeValue
        })

    // --- the field's own math ----------------------------------------------
    // The one display mapping. Every drawn curve, every control point and the
    // pointer's own inverse use these functions, so what the artist sees and
    // what the node evaluates cannot drift apart.
    function smoothstep(t) {
        var c = t < 0 ? 0 : t > 1 ? 1 : t;
        return c * c * (3 - 2 * c);
    }

    // Authored displacement of one knot, in cell coordinates. The centre and the
    // outer boundary are fixed at zero; every other knot reads its own stable
    // index (r-1)*12+h.
    function knotDisplacement(spoke, ring) {
        if (ring <= 0 || ring >= colorWarp.boundaryRing)
            return {
                "u": 0,
                "v": 0
            };
        var wrapped = ((spoke % colorWarp.spokeCount) + colorWarp.spokeCount) % colorWarp.spokeCount;
        var index = colorWarp.pointIndexOf(wrapped, ring);
        return {
            "u": colorWarp.effectiveNumber(colorWarp.hueKeyOf(index)),
            "v": colorWarp.effectiveNumber(colorWarp.saturationKeyOf(index))
        };
    }

    // The displacement field: bilinear interpolation of one cell's four knots
    // with cubic smoothstep weights, so the field is C1 across every edge and
    // periodic in u.
    function displacementAt(u, v) {
        var boundedV = Math.min(colorWarp.boundaryRing, Math.max(0, v));
        var cellU = Math.floor(u);
        var cellV = Math.floor(boundedV);
        var tu = u - cellU;
        var tv = boundedV - cellV;
        var su = colorWarp.smoothstep(tu);
        var sv = colorWarp.smoothstep(tv);
        var bottomLeft = colorWarp.knotDisplacement(cellU, cellV);
        var bottomRight = colorWarp.knotDisplacement(cellU + 1, cellV);
        var topLeft = colorWarp.knotDisplacement(cellU, cellV + 1);
        var topRight = colorWarp.knotDisplacement(cellU + 1, cellV + 1);
        var w0 = (1 - su) * (1 - sv);
        var w1 = su * (1 - sv);
        var w2 = (1 - su) * sv;
        var w3 = su * sv;
        return {
            "u": w0 * bottomLeft.u + w1 * bottomRight.u + w2 * topLeft.u + w3 * topRight.u,
            "v": w0 * bottomLeft.v + w1 * bottomRight.v + w2 * topLeft.v + w3 * topRight.v
        };
    }

    // The mapping the node applies: (u,v) + strength*displacement.
    function mappedCell(u, v) {
        var displacement = colorWarp.displacementAt(u, v);
        return {
            "u": u + colorWarp.strength * displacement.u,
            "v": v + colorWarp.strength * displacement.v
        };
    }

    // Cell coordinates -> wheel pixels. One u cell is 1/12 turn; v is the radius
    // fraction of the disc (v/4). Screen y grows downwards, so the hue angle is
    // negated to keep one turn counter-clockwise from the +x axis.
    function cellToPoint(u, v) {
        var angle = (u / colorWarp.spokeCount) * Math.PI * 2;
        var radius = (v / colorWarp.boundaryRing) * colorWarp.discRadius;
        return {
            "x": colorWarp.discCenterX + radius * Math.cos(angle),
            "y": colorWarp.discCenterY - radius * Math.sin(angle)
        };
    }

    function mappedPoint(u, v) {
        var cell = colorWarp.mappedCell(u, v);
        return colorWarp.cellToPoint(cell.u, cell.v);
    }

    // The drawn position of one control point: the mapping of its own knot.
    function knotPoint(index) {
        return colorWarp.mappedPoint(colorWarp.spokeOf(index), colorWarp.ringOf(index));
    }

    // The pointer's inverse of cellToPoint. u is returned unwrapped; the caller
    // resolves it to the nearest turn of the spoke it is dragging.
    function pointToCell(x, y) {
        var dx = x - colorWarp.discCenterX;
        var dy = colorWarp.discCenterY - y;
        var radius = Math.sqrt(dx * dx + dy * dy);
        return {
            "u": colorWarp.spokeCount * Math.atan2(dy, dx) / (Math.PI * 2),
            "v": Math.max(0, colorWarp.boundaryRing * radius / Math.max(1, colorWarp.discRadius))
        };
    }

    // The turn nearest a reference spoke, so dragging across the periodic seam
    // states a small displacement instead of a whole wheel rotation.
    function unwrapNear(u, reference) {
        var delta = u - reference;
        delta -= Math.round(delta / colorWarp.spokeCount) * colorWarp.spokeCount;
        return reference + delta;
    }

    // The displacement a pointer position states for one point: the mode is
    // inverted exactly ((pointer - base) / strength), so the point follows the
    // pointer. The wheel's own domain limits v to the displayed disc.
    function dragDisplacement(index, x, y) {
        if (!(colorWarp.strength > 0))
            return null;
        var cell = colorWarp.pointToCell(x, y);
        var spoke = colorWarp.spokeOf(index);
        return {
            "u": (colorWarp.unwrapNear(cell.u, spoke) - spoke) / colorWarp.strength,
            "v": (Math.min(colorWarp.boundaryRing, cell.v) - colorWarp.ringOf(index)) / colorWarp.strength
        };
    }

    // --- geometry ----------------------------------------------------------
    readonly property real discCenterX: wheelCanvas.width / 2
    readonly property real discCenterY: wheelCanvas.height / 2
    readonly property real discRadius: Math.max(16, Math.min(wheelCanvas.width, wheelCanvas.height) / 2 - 14)
    readonly property real strength: colorWarp.effectiveNumber("strength")
    onStrengthChanged: colorWarp.setLocalNotice("", false)

    // --- selection ----------------------------------------------------------
    // Presentation state only: the selected point is addressed by its stable
    // index, never by a position, so a refresh or an animated coordinate cannot
    // move the selection to a different point.
    property int selectedIndex: -1
    property int hoverIndex: -1
    readonly property bool hasSelection: colorWarp.selectedIndex >= 0 && colorWarp.selectedIndex < colorWarp.pointCount
    readonly property string selectedHueKey: colorWarp.hasSelection ? colorWarp.hueKeyOf(colorWarp.selectedIndex) : ""
    readonly property string selectedSaturationKey: colorWarp.hasSelection ? colorWarp.saturationKeyOf(colorWarp.selectedIndex) : ""
    readonly property string selectedPinKey: colorWarp.hasSelection ? colorWarp.pinKeyOf(colorWarp.selectedIndex) : ""
    readonly property bool selectedPinned: colorWarp.hasSelection && colorWarp.boolValue(colorWarp.selectedPinKey)

    function selectPoint(index) {
        if (index === colorWarp.selectedIndex)
            return;
        colorWarp.selectedIndex = index;
        colorWarp.setLocalNotice("", false);
    }

    function pointPinned(index) {
        return colorWarp.boolValue(colorWarp.pinKeyOf(index));
    }

    function pointAt(x, y) {
        var best = -1;
        var bestDistance = colorWarp.wheelHitRadius;
        for (var index = 0; index < colorWarp.pointCount; ++index) {
            var point = colorWarp.knotPoint(index);
            var distance = Math.sqrt((point.x - x) * (point.x - x) + (point.y - y) * (point.y - y));
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = index;
            }
        }
        return best;
    }

    // The reason the wheel states for itself: a hint this editor knows (a pinned
    // point, an identity mapping) or the host's own message for a refused shape,
    // which is preserved here because the recovery gesture clears the panel's
    // error on its way in.
    property string localNotice: ""
    property bool localNoticeIsRefusal: false

    function setLocalNotice(text, refusal) {
        colorWarp.localNotice = text;
        colorWarp.localNoticeIsRefusal = refusal;
    }

    function hostProblem(key) {
        if (!colorWarp.panel || String(colorWarp.panel.gestureErrorKey) !== String(key))
            return "";
        return String(colorWarp.panel.gestureError);
    }

    readonly property string wheelNotice: {
        colorWarp.revision;
        colorWarp.previewValues;
        var keys = [colorWarp.selectedHueKey, colorWarp.selectedSaturationKey, "strength"];
        for (var i = 0; i < keys.length; ++i) {
            var problem = colorWarp.hostProblem(keys[i]);
            if (problem.length > 0)
                return problem;
        }
        return colorWarp.localNotice;
    }

    // --- the live point drag -----------------------------------------------
    // ONE live gesture at a time, owned by this editor; the session gesture
    // belongs to the host. The drag begins it, every pointer move previews into
    // it, and the release either commits it as ONE undo entry or cancels it,
    // which restores the values the drag started from.
    property var pointDrag: null
    readonly property bool dragging: colorWarp.pointDrag !== null
    readonly property string panelActiveToken: colorWarp.panel ? String(colorWarp.panel.activeToken) : ""
    onPanelActiveTokenChanged: {
        // Retirement and successors must not retain an earlier numeric preview.
        if (colorWarp.pointDrag === null)
            colorWarp.refresh();
        Qt.callLater(function () {
            // Token replacement during refusal recovery completes synchronously.
            // A genuinely retired host gesture also retires its pointer preview.
            if (colorWarp.pointDrag !== null && !colorWarp.gestureLive(colorWarp.pointDrag)) {
                colorWarp.pointDrag = null;
                colorWarp.previewValues = ({});
                wheelGesture.dragging = false;
                wheelGesture.dragIndex = -1;
            }
        });
    }

    function gestureLive(drag) {
        return drag !== null && colorWarp.panel !== null
                && String(colorWarp.panel.activeToken) === String(drag.token);
    }

    function beginPointDrag(index) {
        if (!colorWarp.panel || colorWarp.pointPinned(index))
            return false;
        if (!(colorWarp.strength > 0)) {
            // The mapping is the identity, so a pointer offset states no finite
            // displacement. The authored values stay editable through the
            // selected coordinates below and through strength.
            colorWarp.setLocalNotice("Strength is 0: raise it to drag a control point", false);
            return false;
        }
        var hueKey = colorWarp.hueKeyOf(index);
        var saturationKey = colorWarp.saturationKeyOf(index);
        var token = String(colorWarp.panel.beginEditForMany(colorWarp.networkId, colorWarp.nodeId, [hueKey, saturationKey]));
        if (token.length === 0)
            return false;
        var originHue = colorWarp.numberValue(hueKey);
        var originSaturation = colorWarp.numberValue(saturationKey);
        colorWarp.setLocalNotice("", false);
        colorWarp.pointDrag = {
            "index": index,
            "token": token,
            "originHue": originHue,
            "originSaturation": originSaturation,
            // The last ACCEPTED shape. A refused preview never enters it.
            "hue": originHue,
            "saturation": originSaturation
        };
        colorWarp.previewValues = colorWarp.pointValues(index, originHue, originSaturation);
        return true;
    }

    function pointValues(index, hue, saturation) {
        var values = ({});
        values[colorWarp.hueKeyOf(index)] = hue;
        values[colorWarp.saturationKeyOf(index)] = saturation;
        return values;
    }

    function updatePointDrag(index, x, y) {
        var drag = colorWarp.pointDrag;
        if (drag === null || drag.index !== index)
            return;
        if (!colorWarp.gestureLive(drag)) {
            // The session retired this gesture (Escape, a preview-only Undo, or
            // another control taking the interaction over): the preview stops
            // here and the authored values stand.
            colorWarp.pointDrag = null;
            colorWarp.previewValues = ({});
            return;
        }
        var displacement = colorWarp.dragDisplacement(index, x, y);
        if (displacement === null)
            return;
        var values = colorWarp.pointValues(index, displacement.u, displacement.v);
        if (colorWarp.panel.updateEditMany(drag.token, values) === true) {
            drag.hue = displacement.u;
            drag.saturation = displacement.v;
        } else {
            // The host refused this shape. The refused preview is never drawn:
            // the poisoned gesture is cancelled (which restores the values the
            // drag started from) and replaced by a fresh one over the last
            // accepted shape, so the edit stays committable and the drag
            // continues from the last valid mesh. The reason is kept on screen
            // because the recovery gesture clears the panel's own error.
            var reason = colorWarp.panel && colorWarp.panel.gestureError ? String(colorWarp.panel.gestureError) : "";
            colorWarp.setLocalNotice(reason.length > 0 ? reason : "That shape was refused; the last valid mesh is kept", true);
            colorWarp.replaceDragGesture(drag);
        }
        // A recovery that could not begin again has already ended the drag.
        if (colorWarp.pointDrag === null)
            return;
        colorWarp.previewValues = colorWarp.pointValues(index, drag.hue, drag.saturation);
    }

    function replaceDragGesture(drag) {
        colorWarp.panel.cancelEdit(drag.token);
        var token = String(colorWarp.panel.beginEditForMany(colorWarp.networkId, colorWarp.nodeId, [colorWarp.hueKeyOf(drag.index), colorWarp.saturationKeyOf(drag.index)]));
        if (token.length === 0) {
            colorWarp.pointDrag = null;
            colorWarp.previewValues = ({});
            return;
        }
        drag.token = token;
        // The accepted shape was valid a moment ago and the document is back at
        // the values this drag started from, so re-stating it is accepted again.
        colorWarp.panel.updateEditMany(token, colorWarp.pointValues(drag.index, drag.hue, drag.saturation));
    }

    function finishPointDrag(commit) {
        var drag = colorWarp.pointDrag;
        colorWarp.pointDrag = null;
        if (drag === null)
            return;
        var changed = drag.hue !== drag.originHue || drag.saturation !== drag.originSaturation;
        if (!colorWarp.gestureLive(drag)) {
            // The session retired the gesture under this drag: nothing may be
            // published and the authored values stand.
            colorWarp.previewValues = ({});
            return;
        }
        if (commit && changed) {
            if (colorWarp.panel.commitEdit(drag.token) !== true)
                colorWarp.previewValues = ({});
            return;
        }
        colorWarp.panel.cancelEdit(drag.token);
        colorWarp.previewValues = ({});
    }

    // A destruction or a hidden editor must not keep a session gesture alive.
    function abandonDrag() {
        if (colorWarp.pointDrag === null)
            return;
        var drag = colorWarp.pointDrag;
        colorWarp.pointDrag = null;
        colorWarp.previewValues = ({});
        if (colorWarp.gestureLive(drag))
            colorWarp.panel.cancelEdit(drag.token);
    }

    onVisibleChanged: if (!colorWarp.visible)
        colorWarp.abandonDrag()
    Component.onDestruction: colorWarp.abandonDrag()

    // --- the one shared gesture for a single value --------------------------
    function commitValue(key, value) {
        if (!colorWarp.panel)
            return false;
        return colorWarp.panel.gestureSingle(colorWarp.rowFor(key), value);
    }

    function commitText(key, text) {
        if (!colorWarp.panel)
            return false;
        return colorWarp.panel.gestureText(colorWarp.rowFor(key), text);
    }

    function togglePin() {
        if (!colorWarp.hasSelection)
            return;
        colorWarp.commitValue(colorWarp.selectedPinKey, !colorWarp.selectedPinned);
    }

    // Reset Selected clears the selected point's displacement and leaves its
    // protection flag authored, as the contract states.
    function resetSelected() {
        if (!colorWarp.hasSelection || !colorWarp.panel)
            return;
        colorWarp.gestureValues(colorWarp.pointValues(colorWarp.selectedIndex, 0, 0));
    }

    // Reset All clears every coordinate and every pin. Only the keys that
    // actually change are named, so an already-reset mesh publishes no history
    // entry at all; one changed mesh publishes exactly one.
    function resetAll() {
        if (!colorWarp.panel)
            return;
        var values = ({});
        for (var index = 0; index < colorWarp.pointCount; ++index) {
            var hueKey = colorWarp.hueKeyOf(index);
            var saturationKey = colorWarp.saturationKeyOf(index);
            var pinKey = colorWarp.pinKeyOf(index);
            if (colorWarp.numberValue(hueKey) !== 0)
                values[hueKey] = 0;
            if (colorWarp.numberValue(saturationKey) !== 0)
                values[saturationKey] = 0;
            if (colorWarp.boolValue(pinKey))
                values[pinKey] = false;
        }
        colorWarp.gestureValues(values);
    }

    // An atomic multi-key edit of ONE node: one begin, one commit, one undo
    // entry, and a cancellation that publishes nothing.
    function gestureValues(values) {
        if (!colorWarp.panel || !colorWarp.panel.beginEditForMany)
            return false;
        var keys = Object.keys(values);
        if (keys.length === 0)
            return false;
        var token = String(colorWarp.panel.beginEditForMany(colorWarp.networkId, colorWarp.nodeId, keys));
        if (token.length === 0)
            return false;
        if (colorWarp.panel.updateEditMany(token, values) !== true) {
            colorWarp.panel.cancelEdit(token);
            colorWarp.refresh();
            return false;
        }
        if (colorWarp.panel.commitEdit(token) !== true) {
            colorWarp.refresh();
            return false;
        }
        return true;
    }

    // --- painting -----------------------------------------------------------
    function repaint() {
        wheelCanvas.requestPaint();
    }

    function clampUnit(candidate) {
        return candidate < 0 ? 0 : candidate > 1 ? 1 : candidate;
    }

    // The palette's own zero: a zero opponent plane is exactly this neutral at
    // the display luminance.
    function neutralChannels() {
        var y = colorWarp.displayLuma;
        return [y, y, y];
    }

    function channelString(channels, alpha) {
        return "rgba(" + Math.round(channels[0] * 255) + "," + Math.round(channels[1] * 255) + "," + Math.round(channels[2] * 255) + "," + alpha + ")";
    }

    // The display-only palette color: the node's own opponent plane at the
    // palette's fixed display luminance, clamped to a displayable RGB triple.
    // Nothing here reaches the renderer.
    function wheelChannels(turn, radius) {
        var y = colorWarp.displayLuma;
        var bounded = Math.min(0.97, Math.max(0, radius));
        var chroma = bounded * (1 + Math.abs(y)) / (1 - bounded);
        var angle = turn * Math.PI * 2;
        var u = chroma * Math.cos(angle);
        var v = chroma * Math.sin(angle);
        var red = Math.sqrt(2 / 3) * u;
        var green = -u / Math.sqrt(6) + v / Math.sqrt(2);
        var blue = -u / Math.sqrt(6) - v / Math.sqrt(2);
        var offset = y - (0.2126 * red + 0.7152 * green + 0.0722 * blue);
        return [colorWarp.clampUnit(red + offset), colorWarp.clampUnit(green + offset), colorWarp.clampUnit(blue + offset)];
    }

    function wheelColor(turn, radius) {
        return colorWarp.channelString(colorWarp.wheelChannels(turn, radius), 1);
    }

    function paintPalette(canvas) {
        var context = canvas.getContext("2d");
        if (!context)
            return;
        context.reset();
        var radius = colorWarp.discRadius;
        var centreX = colorWarp.discCenterX;
        var centreY = colorWarp.discCenterY;
        // The palette's own zero: a zero opponent plane is exactly this neutral
        // at the display luminance.
        var neutral = colorWarp.channelString(colorWarp.neutralChannels(), 1);
        context.beginPath();
        context.arc(centreX, centreY, radius, 0, Math.PI * 2);
        context.fillStyle = neutral;
        context.fill();
        // ONE solid wedge per hue step, at the palette's saturated hue color. A
        // gradient per wedge would be prettier but Qt's Canvas drops fills once
        // a paint creates too many gradient resources, so the radial ramp below
        // is the ONE gradient this paint creates.
        var sectors = colorWarp.paletteSectors;
        for (var sector = 0; sector < sectors; ++sector) {
            // Canvas angles grow clockwise from +x while the hue angle grows
            // counter-clockwise, so one sector is that interval negated.
            var start = -Math.PI * 2 * (sector + 1) / sectors;
            var end = -Math.PI * 2 * sector / sectors;
            context.beginPath();
            context.moveTo(centreX, centreY);
            context.arc(centreX, centreY, radius, start, end);
            context.closePath();
            context.fillStyle = colorWarp.wheelColor((sector + 0.5) / sectors, colorWarp.paletteHueRadius);
            context.fill();
        }
        // The chroma ramp: one radial overlay from the neutral centre to the
        // saturated rim blends the wedges to a neutral middle. It is a display
        // ramp only; the node's own radius-to-chroma inversion saturates inside
        // the outer fifth of the disc and would read as an almost grey wheel.
        var ramp = context.createRadialGradient(centreX, centreY, 0, centreX, centreY, radius);
        ramp.addColorStop(0, colorWarp.channelString(colorWarp.neutralChannels(), 1));
        ramp.addColorStop(1, colorWarp.channelString(colorWarp.neutralChannels(), 0));
        context.beginPath();
        context.arc(centreX, centreY, radius, 0, Math.PI * 2);
        context.fillStyle = ramp;
        context.fill();
        // The fixed outer boundary of the field, drawn once: rings r=0 and r=4
        // carry zero displacement by contract.
        context.beginPath();
        context.arc(centreX, centreY, radius, 0, Math.PI * 2);
        context.lineWidth = 1;
        context.strokeStyle = colorWarp.borderColor;
        context.stroke();
    }

    // The undeformed grid: where the knots would sit at zero displacement. It
    // is what the deformation is read against; it is not a stand-in for it.
    function paintBaseGrid(context) {
        context.save();
        context.globalAlpha = 0.28;
        context.strokeStyle = colorWarp.borderColor;
        context.lineWidth = 1;
        for (var spoke = 0; spoke < colorWarp.spokeCount; ++spoke) {
            var origin = colorWarp.cellToPoint(spoke, 0);
            var end = colorWarp.cellToPoint(spoke, colorWarp.boundaryRing);
            context.beginPath();
            context.moveTo(origin.x, origin.y);
            context.lineTo(end.x, end.y);
            context.stroke();
        }
        for (var ring = 1; ring < colorWarp.boundaryRing; ++ring) {
            context.beginPath();
            context.arc(colorWarp.discCenterX, colorWarp.discCenterY, (ring / colorWarp.boundaryRing) * colorWarp.discRadius, 0, Math.PI * 2);
            context.stroke();
        }
        context.restore();
    }

    // The deformation itself: every spoke and every ring is SAMPLED through the
    // mapping, so the drawn curve is the field the node evaluates and not a
    // decorative approximation of it.
    function paintField(context) {
        var samples = 32;
        context.lineWidth = 1.1;
        context.lineJoin = "round";
        context.strokeStyle = colorWarp.mutedColor;
        for (var spoke = 0; spoke < colorWarp.spokeCount; ++spoke) {
            context.beginPath();
            for (var step = 0; step <= samples; ++step) {
                var point = colorWarp.mappedPoint(spoke, colorWarp.boundaryRing * step / samples);
                if (step === 0)
                    context.moveTo(point.x, point.y);
                else
                    context.lineTo(point.x, point.y);
            }
            context.stroke();
        }
        for (var ring = 1; ring <= colorWarp.boundaryRing; ++ring) {
            context.beginPath();
            for (var around = 0; around <= samples * 1.5; ++around) {
                var cellU = colorWarp.spokeCount * around / (samples * 1.5);
                var ringPoint = colorWarp.mappedPoint(cellU, ring);
                if (around === 0)
                    context.moveTo(ringPoint.x, ringPoint.y);
                else
                    context.lineTo(ringPoint.x, ringPoint.y);
            }
            context.stroke();
        }
        // The selected point's own hue line, so the coordinates it authors are
        // readable against the field.
        if (colorWarp.hasSelection) {
            context.lineWidth = 1.8;
            context.strokeStyle = colorWarp.accentColor;
            context.beginPath();
            for (var selected = 0; selected <= samples; ++selected) {
                var selectedPoint = colorWarp.mappedPoint(colorWarp.spokeOf(colorWarp.selectedIndex), colorWarp.boundaryRing * selected / samples);
                if (selected === 0)
                    context.moveTo(selectedPoint.x, selectedPoint.y);
                else
                    context.lineTo(selectedPoint.x, selectedPoint.y);
            }
            context.stroke();
        }
    }

    function paintPoints(context) {
        var marker = 4.5;
        for (var index = 0; index < colorWarp.pointCount; ++index) {
            var point = colorWarp.knotPoint(index);
            var pinned = colorWarp.pointPinned(index);
            var selected = index === colorWarp.selectedIndex;
            var hovered = index === colorWarp.hoverIndex;
            context.beginPath();
            if (selected) {
                context.arc(point.x, point.y, marker + 3.5, 0, Math.PI * 2);
                context.lineWidth = 1;
                context.strokeStyle = colorWarp.accentColor;
                context.stroke();
                context.beginPath();
            }
            if (pinned) {
                // A pinned point is a square: it states that interactive moving
                // is refused, without hiding the authored position.
                context.rect(point.x - marker, point.y - marker, marker * 2, marker * 2);
                context.fillStyle = selected ? colorWarp.accentColor : colorWarp.mutedColor;
            } else {
                context.arc(point.x, point.y, selected ? marker + 1 : marker, 0, Math.PI * 2);
                context.fillStyle = selected ? colorWarp.accentColor : colorWarp.textColor;
            }
            context.fill();
            context.lineWidth = 1;
            context.strokeStyle = hovered || selected ? colorWarp.accentColor : colorWarp.borderColor;
            context.stroke();
        }
    }

    function paintWheel(canvas) {
        var context = canvas.getContext("2d");
        if (!context)
            return;
        context.reset();
        colorWarp.paintBaseGrid(context);
        colorWarp.paintField(context);
        colorWarp.paintPoints(context);
    }

    // --- shared cells -------------------------------------------------------
    component Caption: ExposureLabel {
        id: caption
        required property string editKey
        theme: colorWarp.theme
        networkId: colorWarp.networkId
        instanceId: colorWarp.instanceId
        nodeId: colorWarp.nodeId
        parameterKey: caption.editKey
        frame: colorWarp.frame
        keyStatus: colorWarp.keyStatusOf(caption.editKey)
        implicitWidth: metrics.advanceWidth
        implicitHeight: 23
        onKeyRequested: colorWarp.keyAtFrame(caption.editKey)
        TextMetrics {
            id: metrics
            text: caption.labelText
            font.pixelSize: colorWarp.fontSizeValue
        }
        KeyIndicator {
            id: keyActions
            visible: false
            theme: colorWarp.cellTheme
            networkId: colorWarp.networkId
            nodeId: colorWarp.nodeId
            parameterKey: caption.editKey
            parameterLabel: caption.labelText
            keyStatus: caption.keyStatus
            frame: colorWarp.frame
            revealAvailable: colorWarp.revealAvailable()
            onKeyRequested: colorWarp.keyAtFrame(caption.editKey)
            onRemoveKeyRequested: colorWarp.removeKeyAtFrame(caption.editKey)
            onRevealRequested: colorWarp.revealInAnimation(caption.editKey)
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: keyActions.openMenu(caption)
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            visible: caption.keyStatus !== "none"
            color: caption.keyStatus === "key" ? colorWarp.accentColor : colorWarp.mutedColor
        }
    }

    component Action: Button {
        id: action
        implicitHeight: 23
        padding: 4
        contentItem: Text {
            text: action.text
            color: action.enabled ? colorWarp.textColor : colorWarp.disabledColor
            font.pixelSize: colorWarp.fontSizeValue
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: !action.enabled ? colorWarp.panelColor : action.down ? colorWarp.raisedColor : action.hovered ? colorWarp.hoverColor : colorWarp.fieldColor
            border.color: colorWarp.borderColor
            radius: colorWarp.smallRadiusValue
        }
    }

    // --- strength -----------------------------------------------------------
    // The mounting row's own parameter, presented through the host's shared
    // numeric bundle so it keeps the shared typed field, scrub, arrow keys and
    // value menu: the editor owns no numeric control of its own.
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Caption {
            editKey: "strength"
            labelText: "strength"
            Layout.preferredWidth: 62
        }
        Loader {
            Layout.fillWidth: true
            sourceComponent: colorWarp.panel ? colorWarp.panel.numericEditorComponent : null
            onLoaded: {
                item.theme = colorWarp.theme;
                item.panel = colorWarp.panel;
                item.row = strengthRow;
                item.compact = true;
                item.fieldFirst = true;
            }
        }
        QtObject {
            id: strengthRow
            readonly property string nodeId: colorWarp.nodeId
            readonly property string parameterKey: "strength"
            readonly property string rowLabel: "Strength"
            readonly property real numberValue: colorWarp.numberValue("strength")
            readonly property var metadata: colorWarp.paramRow("strength")
            readonly property bool hasMinimum: metadata !== null && metadata.minimum !== undefined
            readonly property bool hasMaximum: metadata !== null && metadata.maximum !== undefined
            readonly property real minimum: strengthRow.hasMinimum ? Number(metadata.minimum) : 0
            readonly property real maximum: strengthRow.hasMaximum ? Number(metadata.maximum) : 0
            readonly property bool hasSoftMinimum: metadata !== null && metadata.softMinimum !== undefined
            readonly property bool hasSoftMaximum: metadata !== null && metadata.softMaximum !== undefined
            readonly property real softMinimum: strengthRow.hasSoftMinimum ? Number(metadata.softMinimum) : 0
            readonly property real softMaximum: strengthRow.hasSoftMaximum ? Number(metadata.softMaximum) : 0
            readonly property real numberStep: metadata !== null && metadata.step !== undefined ? Number(metadata.step) : 0.01
            readonly property int decimals: metadata !== null && metadata.displayDecimals !== undefined ? Number(metadata.displayDecimals) : -1
            readonly property bool integerParameter: false
            readonly property int dragThreshold: colorWarp.dragThreshold
            readonly property string rowError: colorWarp.hostProblem("strength")
            readonly property string keyStatus: colorWarp.keyStatusOf("strength")
            readonly property string exactText: metadata !== null && metadata.valueText !== undefined ? String(metadata.valueText) : ""
            function rowRef() {
                return colorWarp.rowFor("strength");
            }
            function commitText(text) {
                return colorWarp.commitText("strength", text);
            }
            function commitDiscrete(value) {
                return colorWarp.commitValue("strength", value);
            }
            function keyAtFrame() {
                return colorWarp.keyAtFrame("strength");
            }
        }
    }

    // --- the wheel ----------------------------------------------------------
    Rectangle {
        id: wheelFrame
        Layout.fillWidth: true
        // Responsive: the wheel is square and never wider than the card. The
        // height follows the card width, which the column lays out independently
        // of this item's height, so no layout cycle is possible.
        readonly property real side: Math.max(180, Math.min(colorWarp.wheelMaxSide, wheelFrame.width))
        Layout.preferredHeight: wheelFrame.side
        color: colorWarp.panelColor
        radius: colorWarp.smallRadiusValue
        border.color: colorWarp.borderColor

        Canvas {
            id: paletteCanvas
            objectName: "colorWarpPalette_" + colorWarp.nodeId
            anchors.fill: parent
            antialiasing: true
            onPaint: colorWarp.paintPalette(paletteCanvas)
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }

        Canvas {
            id: wheelCanvas
            objectName: "colorWarpWheel_" + colorWarp.nodeId
            anchors.fill: parent
            antialiasing: true
            onPaint: colorWarp.paintWheel(wheelCanvas)
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }

        MouseArea {
            id: wheelGesture
            objectName: "colorWarpWheelGesture_" + colorWarp.nodeId
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            activeFocusOnTab: false
            preventStealing: true
            cursorShape: wheelGesture.dragging ? Qt.ClosedHandCursor : colorWarp.hoverIndex >= 0 && !colorWarp.pointPinned(colorWarp.hoverIndex) ? Qt.OpenHandCursor : Qt.ArrowCursor
            property bool dragging: false
            property int dragIndex: -1
            property point pressPoint: Qt.point(0, 0)

            Accessible.name: "ColorWarp mesh"
            Accessible.description: colorWarp.hasSelection
                                 ? "Control point " + colorWarp.selectedIndex + " selected, spoke " + colorWarp.spokeOf(colorWarp.selectedIndex) + ", ring " + colorWarp.ringOf(colorWarp.selectedIndex) + ". Drag it to warp, or edit its coordinates below."
                                 : "Click a control point to select it, then drag it to warp the mesh."

            onPressed: function (mouse) {
                wheelGesture.forceActiveFocus(Qt.MouseFocusReason);
                wheelGesture.pressPoint = Qt.point(mouse.x, mouse.y);
                wheelGesture.dragging = false;
                wheelGesture.dragIndex = colorWarp.pointAt(mouse.x, mouse.y);
                colorWarp.selectPoint(wheelGesture.dragIndex);
            }
            onPositionChanged: function (mouse) {
                var point = Qt.point(mouse.x, mouse.y);
                if (!(mouse.buttons & Qt.LeftButton)) {
                    colorWarp.hoverIndex = colorWarp.pointAt(point.x, point.y);
                    return;
                }
                if (!wheelGesture.dragging) {
                    if (Math.abs(point.x - wheelGesture.pressPoint.x) < colorWarp.dragThreshold &&
                        Math.abs(point.y - wheelGesture.pressPoint.y) < colorWarp.dragThreshold)
                        return;
                    if (wheelGesture.dragIndex < 0)
                        return;
                    if (colorWarp.pointPinned(wheelGesture.dragIndex)) {
                        colorWarp.setLocalNotice("Point " + wheelGesture.dragIndex + " is pinned: unpin it to move it", false);
                        return;
                    }
                    if (!colorWarp.beginPointDrag(wheelGesture.dragIndex))
                        return;
                    wheelGesture.dragging = true;
                }
                colorWarp.updatePointDrag(wheelGesture.dragIndex, point.x, point.y);
            }
            onReleased: {
                if (!wheelGesture.dragging) {
                    wheelGesture.dragging = false;
                    return;
                }
                wheelGesture.dragging = false;
                colorWarp.finishPointDrag(true);
            }
            onCanceled: {
                if (!wheelGesture.dragging)
                    return;
                wheelGesture.dragging = false;
                colorWarp.finishPointDrag(false);
            }
            onExited: colorWarp.hoverIndex = -1
            // Focus loss (another control taking focus, a dialog, a torn-down
            // parent) must never leave the session gesture live: cancel it,
            // publishing nothing.
            onActiveFocusChanged: {
                if (!activeFocus && colorWarp.pointDrag !== null) {
                    wheelGesture.dragging = false;
                    colorWarp.finishPointDrag(false);
                }
            }
            // Escape cancels a live drag without publishing anything; with no
            // drag it clears the selection, like a click on empty space.
            Keys.onEscapePressed: function (event) {
                event.accepted = true;
                if (wheelGesture.dragging) {
                    wheelGesture.dragging = false;
                    wheelGesture.dragIndex = -1;
                    colorWarp.finishPointDrag(false);
                    return;
                }
                colorWarp.selectPoint(-1);
            }
        }
    }

    // --- the refusal / refusal reason ---------------------------------------
    Text {
        objectName: "colorWarpNotice_" + colorWarp.nodeId
        Layout.fillWidth: true
        visible: colorWarp.wheelNotice.length > 0
        text: colorWarp.wheelNotice
        color: colorWarp.localNoticeIsRefusal || colorWarp.hostProblem(colorWarp.selectedHueKey).length > 0 || colorWarp.hostProblem(colorWarp.selectedSaturationKey).length > 0 ? colorWarp.errorColor : colorWarp.mutedColor
        font.pixelSize: Math.max(9, colorWarp.fontSizeValue - 1)
        wrapMode: Text.WordWrap
        elide: Text.ElideRight
        maximumLineCount: 2
    }

    // --- selection actions --------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Text {
            objectName: "colorWarpSelection_" + colorWarp.nodeId
            text: colorWarp.hasSelection
                  ? "point " + colorWarp.selectedIndex + " · spoke " + colorWarp.spokeOf(colorWarp.selectedIndex) + " · ring " + colorWarp.ringOf(colorWarp.selectedIndex)
                  : "no point selected"
            color: colorWarp.hasSelection ? colorWarp.textColor : colorWarp.mutedColor
            font.pixelSize: colorWarp.fontSizeValue
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        PinButton {
            objectName: "colorWarpPin_" + colorWarp.nodeId
            theme: colorWarp.cellTheme
            pinned: colorWarp.selectedPinned
            enabled: colorWarp.hasSelection && colorWarp.panel !== null
            Accessible.name: colorWarp.selectedPinned ? "Unpin the selected control point" : "Pin the selected control point"
            onClicked: colorWarp.togglePin()
            ToolTip.visible: hovered
            ToolTip.text: colorWarp.selectedPinned ? "Unpin: allow dragging this point again" : "Pin: keep this point fixed while editing"
        }
        Action {
            objectName: "colorWarpResetSelected_" + colorWarp.nodeId
            text: "Reset Selected"
            enabled: colorWarp.hasSelection && colorWarp.panel !== null
            onClicked: colorWarp.resetSelected()
            ToolTip.visible: hovered
            ToolTip.text: "Return the selected point's hue and saturation displacement to zero; leave its pin authored"
        }
        Action {
            objectName: "colorWarpResetAll_" + colorWarp.nodeId
            text: "Reset All"
            enabled: colorWarp.panel !== null
            onClicked: colorWarp.resetAll()
            ToolTip.visible: hovered
            ToolTip.text: "Return every coordinate to zero and clear every pin, in one undo entry"
        }
    }

    // --- the selected point's coordinates -----------------------------------
    // The selected hue/saturation displacement, stated through the host's own
    // numeric bundle and the shared label/key cells, so a typed value, an
    // arrow-key step and a scrub are all exactly one validated undo entry with
    // the shared key-at-frame semantics.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        enabled: colorWarp.hasSelection
        Repeater {
            model: ["hue", "saturation"]
            delegate: RowLayout {
                id: coordinateRow
                required property string modelData
                readonly property string coordinateKey: modelData === "hue" ? colorWarp.selectedHueKey : colorWarp.selectedSaturationKey
                Layout.fillWidth: true
                spacing: 4
                Caption {
                    editKey: coordinateRow.coordinateKey
                    labelText: coordinateRow.modelData
                    Layout.preferredWidth: 62
                }
                Loader {
                    Layout.fillWidth: true
                    sourceComponent: colorWarp.panel ? colorWarp.panel.numericEditorComponent : null
                    onLoaded: {
                        item.theme = colorWarp.theme;
                        item.panel = colorWarp.panel;
                        item.row = coordinateAdapter;
                        item.compact = true;
                        item.fieldFirst = true;
                    }
                }
                QtObject {
                    id: coordinateAdapter
                    readonly property string nodeId: colorWarp.nodeId
                    readonly property string parameterKey: coordinateRow.coordinateKey
                    readonly property string rowLabel: coordinateRow.modelData === "hue" ? "Hue displacement" : "Saturation displacement"
                    readonly property real numberValue: colorWarp.effectiveNumber(coordinateRow.coordinateKey)
                    readonly property var metadata: colorWarp.paramRow(coordinateRow.coordinateKey)
                    readonly property bool hasMinimum: metadata !== null && metadata.minimum !== undefined
                    readonly property bool hasMaximum: metadata !== null && metadata.maximum !== undefined
                    readonly property real minimum: coordinateAdapter.hasMinimum ? Number(metadata.minimum) : 0
                    readonly property real maximum: coordinateAdapter.hasMaximum ? Number(metadata.maximum) : 0
                    readonly property bool hasSoftMinimum: metadata !== null && metadata.softMinimum !== undefined
                    readonly property bool hasSoftMaximum: metadata !== null && metadata.softMaximum !== undefined
                    readonly property real softMinimum: coordinateAdapter.hasSoftMinimum ? Number(metadata.softMinimum) : 0
                    readonly property real softMaximum: coordinateAdapter.hasSoftMaximum ? Number(metadata.softMaximum) : 0
                    readonly property real numberStep: metadata !== null && metadata.step !== undefined ? Number(metadata.step) : 0.001
                    readonly property int decimals: metadata !== null && metadata.displayDecimals !== undefined ? Number(metadata.displayDecimals) : -1
                    readonly property bool integerParameter: false
                    readonly property int dragThreshold: colorWarp.dragThreshold
                    readonly property string rowError: colorWarp.hostProblem(coordinateRow.coordinateKey)
                    readonly property string keyStatus: colorWarp.keyStatusOf(coordinateRow.coordinateKey)
                    readonly property string exactText: metadata !== null && metadata.valueText !== undefined ? String(metadata.valueText) : ""
                    function rowRef() {
                        return colorWarp.rowFor(coordinateRow.coordinateKey);
                    }
                    function commitText(text) {
                        return colorWarp.commitText(coordinateRow.coordinateKey, text);
                    }
                    function commitDiscrete(value) {
                        return colorWarp.commitValue(coordinateRow.coordinateKey, value);
                    }
                    function keyAtFrame() {
                        return colorWarp.keyAtFrame(coordinateRow.coordinateKey);
                    }
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        visible: !colorWarp.hasSelection
        text: "Click a control point on the wheel to author its hue and saturation displacement."
        color: colorWarp.mutedColor
        font.pixelSize: Math.max(9, colorWarp.fontSizeValue - 1)
        wrapMode: Text.WordWrap
    }

    Text {
        Layout.fillWidth: true
        text: colorWarp.pointCount + " control points · 12 spokes x 3 rings · centre and outer boundary fixed"
        color: colorWarp.mutedColor
        font.pixelSize: Math.max(9, colorWarp.fontSizeValue - 1)
        elide: Text.ElideRight
    }
}
