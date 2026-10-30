import QtQuick
import "."

// The Skill Hub mark: two stems and a free-floating crossbar (the shared link). Geometry from branding/skillhub-logo.svg.
Item {
    id: root
    property real size: 32
    readonly property real k: size / 256
    width: size
    height: size
    Accessible.role: Accessible.Graphic
    Accessible.name: "Skill Hub"

    Rectangle { x: 36 * root.k; y: 32 * root.k; width: 44 * root.k; height: 192 * root.k; radius: width / 2; color: Theme.dark ? "#FFFFFF" : "#0E1624" }
    Rectangle { x: 176 * root.k; y: 32 * root.k; width: 44 * root.k; height: 192 * root.k; radius: width / 2; color: Theme.dark ? "#FFFFFF" : "#0E1624" }
    Rectangle { x: 96 * root.k; y: 106 * root.k; width: 64 * root.k; height: 40 * root.k; radius: height / 2; color: Theme.accent ? Theme.mark : (Theme.dark ? "#34E0A6" : "#0EA47A") }
}
