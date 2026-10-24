import QtQuick
import QtQuick.Controls.Basic
import "."

// Column header band: sortable Skill / Source / Status, and the span that the agent columns above belong to.
Item {
    id: root
    implicitHeight: 36
    readonly property real portsX: width - Theme.padR - app.columnCount * app.portW
    readonly property real statusX: portsX - Theme.statusW

    Rectangle { anchors.fill: parent; color: Theme.bg1 }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }

    component Cell: AbstractButton {
        id: cell
        property string sortKey: ""
        readonly property bool active: app.filter.sortKey === sortKey
        height: root.height
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.Button
        Accessible.name: "Sort by " + text
        onClicked: {
            if (app.filter.sortKey === sortKey) app.filter.ascending = !app.filter.ascending
            else { app.filter.sortKey = sortKey; app.filter.ascending = true }
        }
        background: Rectangle { color: cell.hovered ? Theme.bg3 : "transparent"; border.color: cell.visualFocus ? Theme.focus : "transparent"; border.width: 2 }
        contentItem: Row {
            spacing: 4
            leftPadding: 2
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: cell.text
                color: cell.active ? Theme.text : Theme.text2
                font.family: Theme.ui
                font.pixelSize: Theme.fsMicro
                font.weight: Font.DemiBold
                font.letterSpacing: 0.6
                font.capitalization: Font.AllUppercase
                anchors.verticalCenter: parent.verticalCenter
            }
            Icon {
                visible: cell.active
                name: app.filter.ascending ? "up" : "down"
                size: 14
                color: Theme.markText
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: !app.noHover }
    }

    Cell { text: "Skill"; sortKey: "name"; x: 12; width: Math.max(80, root.statusX - 12 - 8) }
    Rectangle { x: root.statusX - 1; y: 8; width: 1; height: root.height - 16; color: Theme.line }
    Cell { text: "Status"; sortKey: "status"; x: root.statusX; width: Theme.statusW }
    Rectangle { visible: app.columnCount > 0; x: root.portsX - 1; y: 8; width: 1; height: root.height - 16; color: Theme.line }
    Text {
        visible: app.columnCount > 0
        x: root.portsX
        width: app.columnCount * app.portW
        height: root.height
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
        text: "Visible to"
        color: Theme.text2
        font.family: Theme.ui
        font.pixelSize: Theme.fsMicro
        font.weight: Font.DemiBold
        font.letterSpacing: 0.6
        font.capitalization: Font.AllUppercase
    }
}
