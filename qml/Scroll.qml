import QtQuick
import QtQuick.Controls.Basic
import "."

// Themed scrollbar: a thin bar that brightens on hover (the default one belongs to no design system).
ScrollBar {
    id: bar
    policy: ScrollBar.AsNeeded
    minimumSize: 0.08
    implicitWidth: 10
    contentItem: Rectangle {
        implicitWidth: 6
        radius: 3
        color: bar.pressed ? Theme.mark : bar.hovered ? Theme.text2 : Theme.lineStrong
        opacity: bar.active || bar.hovered ? 1 : 0.7
        Behavior on color { ColorAnimation { duration: Theme.dFast } }
    }
    background: Rectangle { color: "transparent" }
}
