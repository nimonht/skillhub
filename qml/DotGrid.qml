import QtQuick
import "."

// Texture 1 of 2 (the other is the hatch on idle progress): a faint dot grid behind the topology band.
Canvas {
    id: root
    property real step: 20
    property color dot: Qt.alpha(Theme.text, Theme.dark ? 0.07 : 0.09)
    renderStrategy: Canvas.Cooperative
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onDotChanged: requestPaint()
    onPaint: {
        var c = getContext("2d")
        c.reset()
        c.fillStyle = dot
        for (var y = step / 2; y < height; y += step)
            for (var x = step / 2; x < width; x += step)
                c.fillRect(x - 0.75, y - 0.75, 1.5, 1.5)
    }
}
