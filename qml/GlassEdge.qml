import QtQuick
import "."

// The lit edge of a panel over the system's Liquid Glass: a faint line all round, and a brighter one along the top and the left,
// where a light from the top left would catch the rim of a pane of glass. Only drawn when the system makes the glass.
Item {
    id: root
    property real radius: 0
    readonly property real litAlpha: Theme.dark ? 0.30 : 0.90
    readonly property real faintAlpha: Theme.dark ? 0.07 : 0.50
    visible: Theme.nativeGlass
    z: 5
    Rectangle { anchors.fill: parent; radius: root.radius; color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, root.faintAlpha) }
    Rectangle {   // along the top, from the corner out
        width: parent.width; height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, root.litAlpha) }
            GradientStop { position: 0.7; color: Qt.rgba(1, 1, 1, root.faintAlpha) }
            GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0) }
        }
    }
    Rectangle {   // down the left
        width: 1; height: parent.height
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, root.litAlpha) }
            GradientStop { position: 0.7; color: Qt.rgba(1, 1, 1, root.faintAlpha) }
            GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0) }
        }
    }
}
