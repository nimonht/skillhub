import QtQuick
import QtQuick.Controls.Basic
import "."

// The handle of a SplitView: a hairline with a wider grip that lights up while it is used. Only looks: the SplitView does the
// dragging, and event handling here would break it.
Rectangle {
    id: handle
    implicitWidth: 1
    implicitHeight: 1
    color: SplitHandle.pressed || SplitHandle.hovered ? Theme.mark : Theme.line
    containmentMask: Item {
        x: handle.width > handle.height ? 0 : -4
        y: handle.width > handle.height ? -4 : 0
        width: handle.width > handle.height ? handle.width : 9
        height: handle.width > handle.height ? 9 : handle.height
    }
}
