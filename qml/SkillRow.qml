import QtQuick
import "."

// One skill: name and description, where it comes from, whether it is current, and four ports (one per agent).
Item {
    id: row
    required property int index
    required property string name
    required property string description
    required property string source
    required property string status
    required property string note
    required property var links
    required property bool problem

    readonly property bool selected: ListView.isCurrentItem
    readonly property int shown: app.hiddenCount > 0 ? app.columnCount - 1 : app.columnCount
    readonly property real portsX: width - Theme.padR - app.columnCount * app.portW
    readonly property real statusX: portsX - Theme.statusW
    signal activated(int index)

    width: ListView.view ? ListView.view.width : 600
    height: Theme.rowH
    Accessible.role: Accessible.ListItem
    Accessible.name: name + ", " + status + (problem ? ", has link problems" : "")
    Accessible.selected: selected

    Rectangle {
        anchors.fill: parent
        color: row.selected ? Theme.bg4 : hover.hovered ? Theme.bg3 : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.dFast } }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    // column highlight while an agent is being inspected
    Rectangle {
        readonly property int idx: {
            for (var k = 0; k < app.agents.length; ++k) if (app.agents[k].id === app.focusAgent) return k
            return -1
        }
        visible: idx >= 0 && idx < row.shown
        x: row.portsX + idx * app.portW
        width: app.portW
        height: parent.height
        color: Qt.alpha(Theme.mark, 0.07)
    }

    // selection speaks through corner hooks, not a glow
    Item {
        visible: row.selected
        anchors.fill: parent
        Rectangle { x: 0; y: 0; width: 12; height: 2; color: Theme.mark }
        Rectangle { x: 0; y: 0; width: 2; height: 12; color: Theme.mark }
        Rectangle { x: parent.width - 12; y: parent.height - 3; width: 12; height: 2; color: Theme.mark }
        Rectangle { x: parent.width - 2; y: parent.height - 13; width: 2; height: 12; color: Theme.mark }
    }

    Column {
        x: 20
        width: row.statusX - 20 - 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
        Row {
            width: parent.width
            spacing: 8
            Text {
                id: nameText
                width: Math.min(implicitWidth, parent.width)
                text: row.name
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.ui
                font.pixelSize: Theme.fsBody
                font.weight: Font.DemiBold
            }
            Text {
                visible: row.source !== "" && parent.width - nameText.width > 70
                width: Math.max(0, parent.width - nameText.width - 8)
                text: row.source
                elide: Text.ElideRight
                color: Theme.text3
                font.family: Theme.ui
                font.pixelSize: Theme.fsSmall
                anchors.baseline: nameText.baseline
            }
        }
        Text {
            width: parent.width
            text: row.description
            elide: Text.ElideRight
            color: Theme.text2
            font.family: Theme.ui
            font.pixelSize: Theme.fsSmall
        }
    }
    Tag {
        x: row.statusX
        anchors.verticalCenter: parent.verticalCenter
        kind: row.status
    }
    Repeater {
        model: row.shown
        delegate: PortDot {
            required property int index
            x: row.portsX + index * app.portW + (app.portW - width) / 2
            anchors.verticalCenter: parent.verticalCenter
            agentLabel: app.agents[index] ? app.agents[index].label : ""
            link: index < row.links.length ? row.links[index] : "missing"
        }
    }
    PortDot {   // the agents folded into the overflow column: red cross if any of them cannot see this skill
        visible: app.hiddenCount > 0
        x: row.portsX + row.shown * app.portW + (app.portW - width) / 2
        anchors.verticalCenter: parent.verticalCenter
        agentLabel: app.hiddenCount + " more agents"
        link: {
            for (var k = row.shown; k < row.links.length; ++k) if (row.links[k] === "missing" || row.links[k] === "copy" || row.links[k] === "foreign") return "missing"
            return "link"
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor; enabled: !app.noHover }
    TapHandler { onTapped: row.activated(row.index) }
}
