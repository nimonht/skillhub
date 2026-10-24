import QtQuick
import QtQuick.Shapes

// A Shape drawn by the curve renderer (Qt 6.6+: exact curves, anti-aliased without multisampling). Older Qt versions have no
// such renderer; the shape then falls back to the default one, anti-aliased by the multisampling set up in main.cpp.
Shape {
    Component.onCompleted: {
        if (Shape.CurveRenderer !== undefined) preferredRendererType = Shape.CurveRenderer
        else antialiasing = true
    }
}
