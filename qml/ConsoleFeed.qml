import QtQuick
import QtQuick.Controls.Basic
import "."

// Activity feed: the real log lines from Store (clones, links, updates, failures).
Rectangle {
    id: root
    property bool open: false
    color: Theme.bg1
    height: open ? 164 : 0
    clip: true
    Behavior on height { NumberAnimation { duration: Theme.dBase; easing.type: Easing.OutCubic } }
    Rectangle { width: parent.width; height: 1; color: Theme.line }

    Item {
        id: bar
        y: 1
        width: parent.width
        height: 32
        SectionLabel { x: 20; width: 160; anchors.verticalCenter: parent.verticalCenter; text: "Activity"; trailing: list.count > 0 ? list.count : "" }
        Btn { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; kind: "ghost"; compact: true; iconName: "trash"; text: "Clear"; onClicked: app.log.clear() }
    }
    ListView {
        id: list
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        model: app.log
        ScrollBar.vertical: Scroll {}
        onCountChanged: Qt.callLater(positionViewAtEnd)
        delegate: Item {
            id: line
            required property string time
            required property string text
            required property string level
            width: list.width
            height: 24
            Row {
                x: 20
                height: parent.height
                spacing: 10
                NumText { text: line.time; width: 62; color: Theme.text3; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; anchors.verticalCenter: parent.verticalCenter }
                Icon { visible: line.level !== "info"; name: "alert"; size: 14; color: line.level === "error" ? Theme.dangerText : Theme.notifyText; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: line.text
                    width: list.width - 130 - (line.level !== "info" ? 24 : 0)
                    elide: Text.ElideMiddle
                    color: line.level === "error" ? Theme.dangerText : line.level === "warn" ? Theme.notifyText : Theme.text2
                    font.family: Theme.ui
                    font.pixelSize: Theme.fsSmall
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        Text {
            visible: list.count === 0
            anchors.centerIn: parent
            text: "Nothing has run yet. Clones, links and updates will be listed here."
            color: Theme.text3
            font.family: Theme.ui
            font.pixelSize: Theme.fsSmall
        }
    }
}
