import QtQuick
import QtQuick.Shapes
import "."

// Indeterminate work: a three-quarter arc turning at a steady rate. Static when motion is reduced.
Item {
    id: root
    property real size: 16
    property color color: Theme.markText
    property real weight: 2.2
    width: size
    height: size
    Accessible.role: Accessible.Indicator
    Accessible.name: "Working"

    CurveShape {
        id: arc
        anchors.fill: parent
        ShapePath {
            fillColor: "transparent"
            strokeColor: root.color
            strokeWidth: root.weight
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.size / 2
                centerY: root.size / 2
                radiusX: root.size / 2 - root.weight
                radiusY: root.size / 2 - root.weight
                startAngle: -90
                sweepAngle: 270
            }
        }
        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
            running: root.visible && !Theme.still
        }
    }
}
