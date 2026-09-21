import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "ColorWarp Extension Example" panel (issue #37). It is declared by the
// installed package manifest as panel "org.nemo.colorwarp.example" and loaded by
// the shared Panel.qml shell exactly like a built-in: the host owns the header,
// full-header dragging, docking/splitting/tabbing, resize and minimum sizes,
// focus/shortcuts, theme/scaling, the A-E context and workspace persistence.
// Nothing here builds chrome, a window, a dock or a second panel registry, and
// this panel edits nothing: it opens and closes without touching the graph.
//
// Its content is deliberately STATIC labelled text, which the contract allows
// for the demonstration panel only. ColorWarp execution, discovery, lifecycle
// and its mesh editor are real; this page states what the package supplies and
// where the real authoring surface lives, so the panel is identifiable without
// inventing production behaviour it does not have.
Rectangle {
    id: colorWarpExample

    property var theme: null

    readonly property color textColor: colorWarpExample.theme ? colorWarpExample.theme.text : "#dce0e6"
    readonly property color mutedColor: colorWarpExample.theme ? colorWarpExample.theme.muted : "#979ea8"
    readonly property color headingColor: colorWarpExample.theme ? colorWarpExample.theme.accent : "#3485f6"
    readonly property int fontSizeValue: colorWarpExample.theme ? colorWarpExample.theme.fontSize : 11

    // Static package facts, stated exactly as the package's manifest declares
    // them. They are text, not a second registry: nothing resolves or activates
    // a package from here.
    readonly property var facts: [{
        "heading": "Package",
        "lines": ["id org.nemo.colorwarp, format 1, package version 1",
                  "state version 1, processing version 1, API 1..1, no dependencies",
                  "capabilities nemo.effect.pointwise.v1, nemo.ui.qml.v1",
                  "native library installed outside the built-in catalog"]
    }, {
        "heading": "Node",
        "lines": ["type org.nemo.colorwarp, display name ColorWarp, group Color",
                  "pointwise execution over scene-linear Rec.709 RGB with alpha preserved",
                  "negative and above-one values are never clamped, neutral is fixed",
                  "identity is an exact bypass; the GPU payload is 592 bytes"]
    }, {
        "heading": "Mesh",
        "lines": ["12 spoke hue cells (u periodic over 12) x 4 radial cells (v over 4)",
                  "3 editable interior rings at 1/4, 1/2 and 3/4 radius: 36 control points",
                  "the centre and the outer boundary are fixed at zero displacement",
                  "hue and saturation are displacements; strength scales them from identity",
                  "curves are sampled from the same smoothstep tensor mapping the node evaluates"]
    }, {
        "heading": "Authoring",
        "lines": ["select a ColorWarp node: its Parameters card hosts the wheel editor",
                  "drag a control point, or type its hue and saturation in the selected rows",
                  "Pin holds a point; Reset Selected clears one, Reset All clears every one",
                  "one drag is one undo entry; Escape cancels and restores the previous shape",
                  "a fold-producing shape is refused by the validator and the last valid shape stays"]
    }, {
        "heading": "This panel",
        "lines": ["static demonstration content supplied by the installed package",
                  "it opens, docks, tabs and persists through the shared workspace like any built-in",
                  "it edits nothing and processes nothing: close and reopen it freely"]
    }]

    objectName: "colorWarpExamplePanel"
    color: colorWarpExample.theme ? colorWarpExample.theme.background : "#181a1d"

    Flickable {
        id: page
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: width
        contentHeight: body.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: body
            width: page.width
            spacing: 10

            Text {
                objectName: "colorWarpExampleTitle"
                Layout.fillWidth: true
                text: "ColorWarp Extension Example"
                color: colorWarpExample.textColor
                font.pixelSize: colorWarpExample.fontSizeValue + 4
                font.bold: true
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: "This panel is contributed by a separately installed extension package. Everything it states is supplied by that package; the shared workspace owns this panel's shell, docking and persistence."
                color: colorWarpExample.mutedColor
                font.pixelSize: colorWarpExample.fontSizeValue
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: colorWarpExample.facts
                delegate: ColumnLayout {
                    id: fact
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 3

                    Text {
                        Layout.fillWidth: true
                        text: fact.modelData.heading
                        color: colorWarpExample.headingColor
                        font.pixelSize: colorWarpExample.fontSizeValue + 1
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Repeater {
                        model: fact.modelData.lines
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            text: "\u00b7 " + modelData
                            color: colorWarpExample.textColor
                            font.pixelSize: colorWarpExample.fontSizeValue
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}
