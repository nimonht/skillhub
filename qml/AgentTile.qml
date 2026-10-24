import QtQuick
import QtQuick.Shapes
import "."

// An agent's identity: a flat square with its brand mark (monogram when the catalog has no mark). Tone follows health.
Rectangle {
    id: root
    property string text: ""
    property var icon: null         // {vb: [x, y, w, h], paths: [{d, eo}]} from the icon catalog
    property string tone: "ok"      // ok | issue | off
    property bool selected: false
    property real size: 36
    readonly property bool hasIcon: !!icon && !!icon.paths && icon.paths.length > 0
    readonly property color ink: tone === "issue" ? Theme.notifyText : tone === "off" ? Theme.text3 : Theme.markText
    width: size
    height: size
    radius: 6
    color: selected ? Theme.bg4 : Theme.bg3
    Behavior on color { ColorAnimation { duration: Theme.dFast } }

    Item {
        id: mark
        visible: root.hasIcon
        readonly property real box: Math.round(root.size * 0.58)
        readonly property real vbW: root.hasIcon ? root.icon.vb[2] : 24
        readonly property real vbH: root.hasIcon ? root.icon.vb[3] : 24
        readonly property real k: box / Math.max(vbW, vbH)
        width: vbW * k
        height: vbH * k
        anchors.centerIn: parent
        Repeater {
            model: root.hasIcon ? root.icon.paths : []
            CurveShape {
                width: mark.vbW
                height: mark.vbH
                transform: Scale { xScale: mark.k; yScale: mark.k }
                ShapePath {
                    fillColor: root.ink
                    strokeColor: "transparent"
                    strokeWidth: 0
                    fillRule: modelData.eo ? ShapePath.OddEvenFill : ShapePath.WindingFill
                    PathSvg { path: modelData.d }
                }
            }
        }
    }

    Text {
        visible: !root.hasIcon
        anchors.centerIn: parent
        text: root.text
        color: root.ink
        font.family: Theme.display
        font.pixelSize: Math.round(root.size * 0.44)
        font.weight: Font.Bold
        font.letterSpacing: 0.5
    }
}
