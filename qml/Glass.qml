import QtQuick
import "."

// A floating surface. With Liquid Glass it is a lens over what lies behind it (shaders/glass.frag): the content bends at the
// rim, a light draws bright edges, a soft shadow falls around it. Everywhere else, and whenever the shader cannot run (no shader
// support in the build, the software renderer, screenshots), it is a plain tinted surface.
// `behind` is the item whose picture the glass looks at: it must not contain the glass itself.
Item {
    id: root
    property Item behind: Theme.glassBehind
    property real radius: 4
    property color tint: Theme.bg2
    property real tintAlpha: Theme.glassTintAlpha   // how much of the tint covers the picture
    property real shadow: 14                        // the margin the shadow takes around the surface, outside it
    readonly property bool live: app.glassReady && Theme.material === "glass" && behind !== null
    Rectangle { anchors.fill: parent; visible: !root.live; radius: root.radius; color: root.tint }
    Loader {
        anchors.fill: parent
        anchors.margins: -root.shadow
        active: root.live && root.visible
        source: "GlassEffect.qml"
        onLoaded: {
            item.behind = Qt.binding(() => root.behind)
            item.radius = Qt.binding(() => root.radius)
            item.pad = Qt.binding(() => root.shadow)
            item.bevel = Qt.binding(() => Math.max(4, Math.min(18, Math.min(root.width, root.height) / 2.2)))
            item.tint = Qt.binding(() => Qt.rgba(root.tint.r, root.tint.g, root.tint.b, root.tintAlpha))   // bound: the theme can change while it is shown
            item.strength = Qt.binding(() => Theme.setting("appearance", "intensity", 0.8))
            item.spec = Qt.binding(() => Theme.dark ? 1.0 : 0.85)
        }
    }
}
