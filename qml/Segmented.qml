import QtQuick
import QtQuick.Controls.Basic
import "."

// Small flat selector (appearance: auto / dark / light). Selection is a tone step, not an outline.
Item {
    id: root
    property var options: []          // [{ value, label, icon }]
    property string current: ""
    signal picked(string value)
    implicitHeight: 32
    implicitWidth: 200

    Rectangle { anchors.fill: parent; radius: 4; color: Theme.bg2 }
    Row {
        anchors.fill: parent
        anchors.margins: 2
        spacing: 2
        Repeater {
            model: root.options
            delegate: AbstractButton {
                id: seg
                required property var modelData
                readonly property bool on: modelData.value === root.current
                width: (root.width - 4 - (root.options.length - 1) * 2) / root.options.length
                height: parent.height
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                Accessible.checked: on
                background: Rectangle {
                    radius: 3
                    color: seg.on ? Theme.bg4 : seg.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.focus
                    border.width: seg.visualFocus ? 2 : 0
                    Behavior on color { ColorAnimation { duration: Theme.dFast } }
                }
                contentItem: Row {
                    spacing: 5
                    anchors.centerIn: parent
                    Icon { name: seg.modelData.icon; size: 14; color: seg.on ? Theme.markText : Theme.text2; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        text: seg.modelData.label
                        color: seg.on ? Theme.text : Theme.text2
                        font.family: Theme.ui
                        font.pixelSize: Theme.fsMicro
                        font.weight: Font.DemiBold
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                onClicked: root.picked(modelData.value)
                HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: !app.noHover }
            }
        }
    }
}
