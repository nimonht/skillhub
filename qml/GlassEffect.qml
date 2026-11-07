import QtQuick
import "."

// The shader of Glass.qml, kept apart so that a build or a renderer without it never loads it. The item is `pad` larger than
// the glass on every side: that margin is where the shadow falls (shaders/glass.frag).
ShaderEffect {
    id: fx
    property Item behind
    property real radius: 4
    property real strength: 1
    property color tint: "transparent"
    property real pad: 14
    property real bevel: 16
    property real spec: 1
    readonly property size size: Qt.size(width, height)
    readonly property point at: behind ? mapToItem(behind, 0, 0) : Qt.point(0, 0)
    property var source: ShaderEffectSource {
        sourceItem: fx.behind
        sourceRect: Qt.rect(fx.at.x, fx.at.y, fx.width, fx.height)
        live: true
        hideSource: false
    }
    fragmentShader: "qrc:/shaders/glass.frag.qsb"
}
