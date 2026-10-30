import QtQuick
import "."

// Nothing selected: say what this panel is for and how to drive the app.
Item {
    id: root
    component Key: Rectangle {
        property string label
        implicitWidth: kt.implicitWidth + 14
        implicitHeight: 24
        radius: 4
        color: Theme.bg3
        Text { id: kt; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsMicro; font.weight: Font.DemiBold }
    }
    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 28
        anchors.topMargin: 56
        spacing: 14
        Icon { name: "config"; size: 30; color: Theme.text3 }
        Text {
            width: parent.width
            text: "Pick a skill"
            color: Theme.text
            font.family: Theme.display
            font.pixelSize: Theme.fsHead
            font.weight: Font.Bold
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Its SKILL.md, where it came from and which agents can use it show up here. Click an agent above the list to inspect that agent instead."
            color: Theme.text2
            font.family: Theme.ui
            font.pixelSize: Theme.fsBody
            lineHeight: 1.25
        }
        Item { width: 1; height: 6 }
        Repeater {
            model: [ { k: "/", t: "Filter skills" }, { k: "Ctrl+N", t: "Add skills" }, { k: "Ctrl+U", t: "Check for updates" }, { k: "Ctrl+Shift+U", t: "Review every update" } ]
            delegate: Row {
                required property var modelData
                spacing: 10
                Key { label: modelData.k; anchors.verticalCenter: parent.verticalCenter }
                Text { text: modelData.t; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.verticalCenter: parent.verticalCenter }
            }
        }
    }
}
