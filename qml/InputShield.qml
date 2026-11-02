import QtQuick

// The first child of a panel that floats over others: whatever the panel's own controls do not take stops here, so hover,
// clicks and the wheel never reach what lies below.
MouseArea {
    anchors.fill: parent
    acceptedButtons: Qt.AllButtons
    hoverEnabled: true
    cursorShape: Qt.ArrowCursor
    onWheel: (wheel) => wheel.accepted = true
    HoverHandler { blocking: true }
}
