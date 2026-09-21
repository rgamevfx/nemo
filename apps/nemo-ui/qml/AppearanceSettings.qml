import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: appearanceSettings
    required property var controller
    required property var theme
    spacing: 12

    Text {
        text: "Appearance"
        color: appearanceSettings.theme.text
        font.pixelSize: 12
        font.weight: Font.Medium
    }

    RowLayout {
        Text {
            text: "Theme"
            color: appearanceSettings.theme.muted
            font.pixelSize: appearanceSettings.theme.fontSize
            Layout.fillWidth: true
        }
        StudioComboBox {
            id: themePresetMenu
            objectName: "themePresetMenu"
            theme: appearanceSettings.theme
            model: ["Graphite", "Slate", "Paper"]
            currentIndex: Math.max(0, model.indexOf(appearanceSettings.controller.appearancePreset))
            implicitWidth: 142
            implicitHeight: 28
            onActivated: appearanceSettings.controller.setAppearancePreset(currentText)
        }
    }

    RowLayout {
        Text {
            text: "Accent"
            color: appearanceSettings.theme.muted
            font.pixelSize: appearanceSettings.theme.fontSize
            Layout.fillWidth: true
        }
        Rectangle {
            width: 18
            height: 18
            radius: 9
            color: appearanceSettings.theme.accent
        }
        TextField {
            id: accentField
            objectName: "accentColorField"
            text: appearanceSettings.theme.accent.toString()
            implicitWidth: 112
            implicitHeight: 28
            selectByMouse: true
            Accessible.name: "Accent color"
            onEditingFinished: {
                appearanceSettings.controller.setAccentOverride(text)
                text = appearanceSettings.theme.accent.toString()
            }
            Connections {
                target: appearanceSettings.controller
                function onAppearanceChanged() {
                    accentField.text = appearanceSettings.theme.accent.toString()
                }
            }
        }
    }

    ChromeButton {
        objectName: "resetAccentButton"
        theme: appearanceSettings.theme
        text: "Reset accent"
        onClicked: {
            appearanceSettings.controller.setAccentOverride("")
            accentField.text = appearanceSettings.theme.accent.toString()
        }
    }

    Text {
        text: "Node category colors"
        color: appearanceSettings.theme.muted
        font.pixelSize: appearanceSettings.theme.fontSize
    }

    ScrollView {
        id: categoryScroll
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(300, categoryColumn.implicitHeight)
        clip: true

        ColumnLayout {
            id: categoryColumn
            width: categoryScroll.availableWidth
            spacing: 6

            Repeater {
                model: Object.keys(appearanceSettings.theme.nodeCategoryColors).sort()
                delegate: RowLayout {
                    id: categoryRow
                    required property string modelData
                    property string categoryId: modelData
                    Layout.fillWidth: true
                    spacing: 7

                    Text {
                        text: categoryRow.categoryId
                        color: appearanceSettings.theme.text
                        font.pixelSize: appearanceSettings.theme.fontSize
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        width: 18
                        height: 18
                        radius: appearanceSettings.theme.smallRadius
                        color: appearanceSettings.theme.nodeCategoryColor(categoryRow.categoryId)
                        border.color: appearanceSettings.theme.border
                    }
                    TextField {
                        id: categoryField
                        objectName: "categoryColorField_" + categoryRow.categoryId
                        text: appearanceSettings.theme.nodeCategoryColor(categoryRow.categoryId)
                        implicitWidth: 112
                        implicitHeight: 28
                        selectByMouse: true
                        Accessible.name: categoryRow.categoryId + " node category color"
                        onEditingFinished: {
                                        appearanceSettings.controller.setCategoryColor(categoryRow.categoryId, text)
                            text = appearanceSettings.theme.nodeCategoryColor(categoryRow.categoryId)
                        }
                        Connections {
                            target: appearanceSettings.controller
                            function onAppearanceChanged() {
                                categoryField.text = appearanceSettings.theme.nodeCategoryColor(categoryRow.categoryId)
                            }
                        }
                    }
                }
            }
        }
    }

    ChromeButton {
        objectName: "resetNodeCategoryColorsButton"
        theme: appearanceSettings.theme
        text: "Reset node category colors"
        Layout.fillWidth: true
        onClicked: appearanceSettings.controller.resetCategoryColors()
    }
}
