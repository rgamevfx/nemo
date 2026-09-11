import QtQuick

// Read-only presentation tokens. WorkspaceController owns appearance defaults,
// validation and persistence; shared controls receive this object explicitly.
QtObject {

    required property var workspace

    readonly property string preset: workspace.appearancePreset
    readonly property string accentOverride: workspace.accentOverride
    readonly property bool light: preset === "Paper"
    readonly property bool cool: preset === "Slate"
    readonly property color background: light ? "#e8eaed" : cool ? "#171d27" : "#181a1d"
    readonly property color panel: light ? "#f8f9fa" : cool ? "#202837" : "#1e2023"
    readonly property color header: light ? "#f0f2f4" : cool ? "#242d3c" : "#212428"
    readonly property color raised: light ? "#ffffff" : cool ? "#2b3547" : "#282c31"
    readonly property color hover: light ? "#dce2e9" : cool ? "#364258" : "#343940"
    readonly property color border: light ? "#ccd1d7" : cool ? "#354052" : "#30343a"
    readonly property color text: light ? "#252a32" : "#dce0e6"
    readonly property color muted: light ? "#616b78" : "#979ea8"
    readonly property color disabled: light ? "#929aa5" : "#5f6670"
    readonly property color accent: accentOverride.length ? accentOverride : "#3485f6"
    readonly property color field: light ? "#e9edf1" : cool ? "#1c2432" : "#24272c"
    readonly property color imageSurround: "#17191b"
    readonly property color node: light ? "#e1e6ee" : "#2a2e33"
    readonly property color nodeSelected: light ? "#c7dbf7" : "#293e57"
    readonly property int radius: 7
    readonly property int smallRadius: 4
    readonly property int fontSize: 11
    readonly property int minimumPaneWidth: 280
    readonly property int minimumPaneHeight: 140
    readonly property int splitHandleSize: 8
    readonly property var nodeCategoryColors: workspace.categoryColors

    // Production retains the existing error surface contract while adopting
    // the prototype's compact visual treatment.
    readonly property color errorSurface: light ? "#f2d8d8" : "#5a2b2b"
    readonly property color errorText: light ? "#6d2525" : "#f0d0d0"

    function nodeCategoryColor(category) {
        return nodeCategoryColors[String(category)] || nodeCategoryColors.Utility
    }

}
