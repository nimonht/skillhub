import QtQuick
import QtQuick.Controls.Basic
import "."

// A short name shown on hover. A popup, so it draws above every panel, nothing clips it, and it stays inside the window.
ToolTip {
    id: root
    property string side: "below"   // below | right
    x: !parent ? 0 : side === "right" ? parent.width + 8 : (parent.width - implicitWidth) / 2
    y: !parent ? 0 : side === "right" ? (parent.height - implicitHeight) / 2 : parent.height + 8
    margins: 8
    leftPadding: 9
    rightPadding: 9
    topPadding: 5
    bottomPadding: 5
    enter: null
    exit: null
    contentItem: Text { textFormat: Text.PlainText; text: root.text; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
    background: Rectangle { radius: 4; color: Theme.bg3; border.color: Theme.line; border.width: 1 }
}
