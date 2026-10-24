import QtQuick
import QtQuick.Controls.Basic
import "."

// Text input. Capsule for filters (search), small-radius rectangle for form entry.
TextField {
    id: root
    property string iconName: ""
    property bool clearable: true
    readonly property real _left: iconName !== "" ? 38 : 14

    implicitHeight: Theme.minTap
    leftPadding: _left
    rightPadding: clearable && text.length > 0 ? 36 : 14
    font.family: Theme.ui
    font.pixelSize: Theme.fsBody
    color: Theme.text
    placeholderTextColor: Theme.text3
    selectionColor: Qt.alpha(Theme.mark, 0.35)
    selectedTextColor: Theme.text
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    Accessible.role: Accessible.EditableText
    Accessible.name: placeholderText

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: 4
            color: root.activeFocus ? Theme.bg3 : Theme.field
            Behavior on color { ColorAnimation { duration: Theme.dFast } }
            Rectangle {   // focus = a 2px underline, not a box around the field
                visible: root.activeFocus
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: Theme.focus
            }
        }
    }
    Icon { visible: root.iconName !== ""; name: root.iconName; size: 16; color: root.activeFocus ? Theme.markText : Theme.text3; x: 14; anchors.verticalCenter: parent.verticalCenter }
    Item {
        visible: root.clearable && root.text.length > 0
        width: 28; height: 28
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        Accessible.role: Accessible.Button
        Accessible.name: "Clear"
        Icon { anchors.centerIn: parent; name: "x"; size: 14; color: ma.containsMouse ? Theme.text : Theme.text2 }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { root.clear(); root.forceActiveFocus() } }
    }
}
