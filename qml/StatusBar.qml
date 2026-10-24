import QtQuick
import "."

// Footer: honest counts on the left, running work (with real progress when known) and the activity toggle on the right.
Rectangle {
    id: root
    property bool consoleOpen: false
    signal toggleConsole()
    color: Theme.bg1
    implicitHeight: 34
    Rectangle { width: parent.width; height: 1; color: Theme.line }
    Rectangle {   // determinate progress only: a 2px line along the top edge, clipped to the bar
        visible: app.busy && app.progress >= 0
        width: parent.width * Math.max(0, Math.min(1, app.progress))
        height: 2
        color: Theme.mark
        Behavior on width { NumberAnimation { duration: Theme.dBase; easing.type: Easing.OutCubic } }
    }

    NumText {
        x: 20
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(80, parent.width - rightGroup.width - 20 - 24)
        elide: Text.ElideRight
        text: app.skillCount + (app.skillCount === 1 ? " skill" : " skills") + "   ·   " + app.updateCount + " updates   ·   " + app.problemCount + " link problems"
              + "   ·   " + app.localCount + " local" + (app.strayCount > 0 ? "   ·   " + app.strayCount + " stray" : "")
        color: Theme.text2
        font.family: Theme.ui
        font.pixelSize: Theme.fsSmall
        Accessible.role: Accessible.StaticText
    }
    Row {
        id: rightGroup
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14
        Row {
            visible: app.busy
            spacing: 9
            anchors.verticalCenter: parent.verticalCenter
            Spinner { size: 16; anchors.verticalCenter: parent.verticalCenter }
            NumText {
                text: app.busyText + (app.progress >= 0 ? "   " + Math.round(app.progress * 100) + "%" : "")
                color: Theme.text
                font.family: Theme.ui
                font.pixelSize: Theme.fsSmall
                font.weight: Font.Medium
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Btn {
            kind: "ghost"; compact: true; iconName: "terminal"
            text: "Activity"
            onClicked: root.toggleConsole()
            anchors.verticalCenter: parent.verticalCenter
            Accessible.description: root.consoleOpen ? "Hide the activity log" : "Show the activity log"
        }
    }
}
