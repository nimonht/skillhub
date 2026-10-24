import QtQuick
import QtQuick.Shapes

// Rectangle with independently cut corners. Angular cuts mark machinery and action; plain rectangles hold content.
CurveShape {
    id: root
    property color fill: "transparent"
    property color stroke: "transparent"
    property real strokeWidth: 1
    property real cutTL: 0
    property real cutTR: 0
    property real cutBR: 0
    property real cutBL: 0
    readonly property real _h: stroke.a > 0 ? strokeWidth / 2 : 0   // keep the stroke inside the item

    ShapePath {
        fillColor: root.fill
        strokeColor: root.stroke
        strokeWidth: root.stroke.a > 0 ? root.strokeWidth : 0
        joinStyle: ShapePath.MiterJoin
        startX: root._h + root.cutTL
        startY: root._h
        PathLine { x: root.width - root._h - root.cutTR; y: root._h }
        PathLine { x: root.width - root._h; y: root._h + root.cutTR }
        PathLine { x: root.width - root._h; y: root.height - root._h - root.cutBR }
        PathLine { x: root.width - root._h - root.cutBR; y: root.height - root._h }
        PathLine { x: root._h + root.cutBL; y: root.height - root._h }
        PathLine { x: root._h; y: root.height - root._h - root.cutBL }
        PathLine { x: root._h; y: root._h + root.cutTL }
        PathLine { x: root._h + root.cutTL; y: root._h }
    }
}
