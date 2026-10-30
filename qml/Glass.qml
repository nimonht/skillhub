import QtQuick
import "."

// A floating surface. With Liquid Glass it refracts what lies behind it; everywhere else, and whenever the shader cannot run
// (no shader support in the build, the software renderer, screenshots), it is a plain tinted surface.
Item {
    id: root
    property Item behind
    property real radius: 4
    property color tint: Theme.bg2
    readonly property bool live: app.glassReady && Theme.material === "glass" && behind !== null
    Rectangle { anchors.fill: parent; visible: !root.live; radius: root.radius; color: root.tint }
    Loader {
        anchors.fill: parent
        active: root.live && root.visible
        source: "GlassEffect.qml"
        onLoaded: {
            item.behind = root.behind
            item.radius = root.radius
            item.tint = Qt.rgba(root.tint.r, root.tint.g, root.tint.b, 0.55)
            item.strength = Theme.setting("appearance", "intensity", 0.8)
        }
    }
}
