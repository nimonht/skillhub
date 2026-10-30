import QtQuick
import QtQuick.Controls.Basic
import "."

// A slider with a label and its value in words. Steps with the arrow keys; the value is always shown, so nothing rests on the knob's place alone.
Item {
    id: root
    property string label: ""
    property alias from: s.from
    property alias to: s.to
    property alias stepSize: s.stepSize
    property alias value: s.value
    property string shown: ""
    property alias track: trackFill.data     // something to draw along the track, such as a hue strip
    signal moved(real value)
    implicitWidth: 280
    implicitHeight: 48

    Text { text: root.label; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
    Text { anchors.right: parent.right; text: root.shown; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
    Slider {
        id: s
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        padding: 0
        live: false
        onMoved: root.moved(value)
        Accessible.name: root.label
        background: Rectangle {
            x: s.leftPadding
            y: (s.height - height) / 2
            width: s.availableWidth
            height: 6
            radius: 3
            color: Theme.trackOff
            clip: true
            Item { id: trackFill; anchors.fill: parent }
        }
        handle: Rectangle {
            x: s.leftPadding + s.visualPosition * (s.availableWidth - width)
            y: (s.height - height) / 2
            width: 16; height: 16; radius: 8
            color: s.pressed ? Theme.mark : Theme.text
            border.color: s.visualFocus ? Theme.focus : Theme.bg1
            border.width: 2
        }
    }
}
