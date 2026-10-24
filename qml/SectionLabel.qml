import QtQuick
import "."

// Section head: a short mint bar and the label. This is the heading itself, not a kicker above another one.
Item {
    id: root
    property string text: ""
    property string trailing: ""
    implicitHeight: 28
    implicitWidth: 200
    Accessible.role: Accessible.Heading
    Accessible.name: text

    Rectangle { x: 0; anchors.verticalCenter: parent.verticalCenter; width: 3; height: 14; color: Theme.mark }
    Text {
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        color: Theme.text
        font.family: Theme.ui
        font.pixelSize: Theme.fsSmall
        font.weight: Font.DemiBold
        font.letterSpacing: 0.4
    }
    NumText {
        visible: root.trailing !== ""
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: root.trailing
        color: Theme.text2
        font.family: Theme.ui
        font.pixelSize: Theme.fsMicro
    }
}
