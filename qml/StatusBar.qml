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

    readonly property alias summary: counts.text
    NumText {
        id: counts
        x: 20
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(80, parent.width - rightGroup.width - 20 - 24)
        elide: Text.ElideRight
        text: (app.skillCount === 1 ? qsTr("1 skill") : qsTr("%1 skills").arg(app.skillCount)) + "   ·   " + qsTr("%1 updates").arg(app.updateCount) + "   ·   " + qsTr("%1 link problems").arg(app.problemCount)
              + "   ·   " + qsTr("%1 local").arg(app.localCount) + (app.strayCount > 0 ? "   ·   " + qsTr("%1 stray").arg(app.strayCount) : "")
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
        Btn {   // a new version of Skill Hub itself: one click where it can replace itself, what to do where it cannot
            id: appUpdate
            readonly property bool can: app.appUpdate.canInstall === true
            readonly property bool back: app.appUpdate.downgrade === true   // a beta that follows stable: the newest final version is older
            visible: app.appUpdate.available === true
            enabled: !app.busy
            kind: "ghost"; compact: true; iconName: "upgrade"
            text: back ? (can ? qsTr("Go back to %1") : qsTr("Stable is %1")).arg(app.appUpdate.latest) : can ? qsTr("Update to %1").arg(app.appUpdate.latest) : qsTr("%1 is out").arg(app.appUpdate.latest)
            onClicked: can ? app.installAppUpdate() : app.showSettings("about")
            anchors.verticalCenter: parent.verticalCenter
            Accessible.description: !can ? qsTr("Shows how to get that version of Skill Hub") : back ? qsTr("Downloads the newest stable version of Skill Hub, replaces this beta and restarts") : qsTr("Downloads the new version of Skill Hub and restarts")
        }
        Btn {
            kind: "ghost"; compact: true; iconName: "terminal"
            text: qsTr("Activity")
            onClicked: root.toggleConsole()
            anchors.verticalCenter: parent.verticalCenter
            Accessible.description: root.consoleOpen ? qsTr("Hide the activity log") : qsTr("Show the activity log")
        }
    }
}
