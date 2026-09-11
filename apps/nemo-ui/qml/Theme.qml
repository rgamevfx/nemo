import QtQuick

// Presentation-only palette shared by the docking shell. WorkspaceController
// owns the persisted appearance values; this object only derives role colors.
QtObject {
    id: theme

    property var workspace
    readonly property string preset: workspace && workspace.appearancePreset ? workspace.appearancePreset : "Graphite"
    readonly property string accentOverride: workspace && workspace.accentOverride ? workspace.accentOverride : ""
    readonly property var categoryColors: workspace && workspace.categoryColors ? workspace.categoryColors : ({})

    readonly property color accent: accentOverride.length > 0 ? accentOverride : presetAccent(preset)
    readonly property color window: preset === "Paper" ? "#f2f1ee" : preset === "Slate" ? "#18212b" : "#1e1e1e"
    readonly property color surface: preset === "Paper" ? "#faf9f6" : preset === "Slate" ? "#202b38" : "#242424"
    readonly property color panel: preset === "Paper" ? "#ffffff" : preset === "Slate" ? "#253241" : "#2b2b2b"
    readonly property color header: preset === "Paper" ? "#e6e3de" : preset === "Slate" ? "#2d3b4b" : "#333333"
    readonly property color tabs: preset === "Paper" ? "#dedbd5" : preset === "Slate" ? "#202c39" : "#262626"
    readonly property color text: preset === "Paper" ? "#252525" : "#d0d0d0"
    readonly property color mutedText: preset === "Paper" ? "#66625d" : preset === "Slate" ? "#aebdca" : "#999999"
    readonly property color border: preset === "Paper" ? "#beb9b1" : preset === "Slate" ? "#536579" : "#4b4b4b"
    readonly property color accentText: preset === "Paper" ? "#ffffff" : "#ffffff"
    readonly property color errorSurface: preset === "Paper" ? "#f2d8d8" : "#5a2b2b"
    readonly property color errorText: preset === "Paper" ? "#6d2525" : "#f0d0d0"

    function presetAccent(name) {
        if (name === "Paper")
            return "#386b91"
        if (name === "Slate")
            return "#6d9cc7"
        return "#4a6fa5"
    }

    function categoryColor(category) {
        if (categoryColors && categoryColors[category])
            return categoryColors[category]
        if (category === "Merge")
            return "#60656b"
        if (category === "Filter")
            return "#a96832"
        if (category === "IO")
            return "#386b91"
        return border
    }
}
