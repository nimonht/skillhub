import QtQuick
import QtQuick.Shapes
import "."

// One icon family: 24 px grid, 1.8 px stroke, round caps and joins, drawn as paths (no icon font, no glyphs).
Item {
    id: root
    property string name: ""
    property color color: Theme.text
    property real size: 18
    property real weight: 1.8
    width: size
    height: size
    implicitWidth: size
    implicitHeight: size

    readonly property var paths: ({
        "refresh": "M20 11a8 8 0 0 0-14.9-3M4.5 4v4.5H9M4 13a8 8 0 0 0 14.9 3M19.5 20v-4.5H15",
        "download": "M12 4v11M7 11l5 5 5-5M5 20h14",
        "upgrade": "M12 19V5M6 11l6-6 6 6M5 20h14",
        "plus": "M12 5v14M5 12h14",
        "check": "M5 12.5l4.5 4.5L19 7.5",
        "x": "M6 6l12 12M18 6L6 18",
        "search": "M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-4-4",
        "link": "M10 14l4-4M8.5 11.5l-2 2a3.5 3.5 0 0 0 5 5l2-2M15.5 12.5l2-2a3.5 3.5 0 0 0-5-5l-2 2",
        "folder": "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z",
        "config": "M7 3h7l5 5v13H7zM14 3v5h5M10 13h6M10 17h6",
        "trash": "M4 7h16M10 11v6M14 11v6M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12M9 7V4h6v3",
        "alert": "M12 4l9 16H3zM12 10v4M12 17.2v.1",
        "branch": "M7 4v10M7 20a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM17 10a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM17 10c0 5-10 2-10 6",
        "terminal": "M4 6l5 6-5 6M12 18h8",
        "moon": "M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z",
        "sun": "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4",
        "auto": "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 3v18",
        "chevron": "M9 6l6 6-6 6",
        "up": "M7 14l5-5 5 5",
        "down": "M7 10l5 5 5-5",
        "clock": "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 7v5l3 2",
        "eye": "M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12zM12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6z",
        "copy": "M9 9h10a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2zM5 15H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v1",
        "all": "M4 6h16M4 12h16M4 18h16",
        "terminal": "M5 7l5 5-5 5M12 18h7",
        "tool": "M14.5 5.5a4 4 0 0 0-5 5L4 16l4 4 5.5-5.5a4 4 0 0 0 5-5l-2.5 2.5-3-1-1-3z",
        "sliders": "M4 7h9M17 7h3M4 17h3M11 17h9M15 5v4M9 15v4",
        "export": "M12 15V4M7 9l5-5 5 5M5 20h14",
        "import": "M12 4v11M7 11l5 5 5-5M5 20h14M5 4h14",
        "info": "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 11v6M12 7.2v.1"
    })

    CurveShape {
        width: 24
        height: 24
        transform: Scale { xScale: root.size / 24; yScale: root.size / 24 }
        ShapePath {
            fillColor: "transparent"
            strokeColor: root.color
            strokeWidth: root.weight
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.paths[root.name] || "" }
        }
    }
}
