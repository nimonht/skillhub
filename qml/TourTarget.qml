import QtQuick
import "."

// Names its parent, so that the tour can point at it wherever the layout has put it.
Item {
    property string key
    Component.onCompleted: Theme.tourTargets[key] = parent
    Component.onDestruction: delete Theme.tourTargets[key]
}
