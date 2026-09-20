import QtQuick

// A parameter-group boundary inside an inspector card (issue #102). The
// mockup-led design states a schema section as a rule, not a titled box: the
// section name orders and separates the groups and adds no chrome of its own,
// so the inspector keeps one explicit collapse/close/pin header per card.
Rectangle {
    id: separator

    property var theme: null

    implicitHeight: 9
    color: "transparent"

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 1
        color: separator.theme ? separator.theme.border : "#30343a"
    }
}
