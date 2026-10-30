import QtQuick
import QtQuick.Controls.Basic
import "."

// Switch with a text label. On = lit track plus a check, so state is never colour-only.
AbstractButton {
    id: root
    property string hint: ""
    property bool showLabel: true   // the switch alone; `text` still names it for screen readers
    checkable: true
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    implicitHeight: hint !== "" ? 44 : 32
    implicitWidth: 200
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checked: checked

    background: Rectangle {
        radius: 4
        color: root.hovered ? Theme.bg3 : "transparent"
        border.color: Theme.focus
        border.width: root.visualFocus ? 2 : 0
        Behavior on color { ColorAnimation { duration: Theme.dFast } }
    }
    contentItem: Item {
        Rectangle {
            id: track
            width: 38; height: 20; radius: 4
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            color: root.checked ? Theme.mark : Theme.trackOff
            Behavior on color { ColorAnimation { duration: Theme.dBase } }
            Rectangle {
                width: 14; height: 14; radius: 2
                y: 3
                x: root.checked ? parent.width - width - 3 : 3
                color: root.checked ? (Theme.dark ? "#04251A" : "#FFFFFF") : Theme.text2
                Behavior on x { NumberAnimation { duration: Theme.dBase; easing.type: Easing.OutCubic } }
                Icon { visible: root.checked; anchors.centerIn: parent; name: "check"; size: 10; weight: 3; color: Theme.mark }
            }
        }
        Column {
            visible: root.showLabel
            anchors.left: track.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: root.text
                width: parent.width
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.ui
                font.pixelSize: Theme.fsBody
                font.weight: Font.Medium
            }
            Text {
                visible: root.hint !== ""
                text: root.hint
                width: parent.width
                elide: Text.ElideRight
                color: Theme.text2
                font.family: Theme.ui
                font.pixelSize: Theme.fsMicro
            }
        }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: !app.noHover }
}
