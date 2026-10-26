import QtQuick
import "."

// The agents Skill Hub manages, as one column each, directly above that agent's column of ports in the list.
// Each column: identity tile, name, "visible / total", and a bar of that ratio. Everything is flat and measured.
Item {
    id: root
    implicitHeight: 128
    readonly property var agents: app.agents
    readonly property int cols: app.columnCount
    readonly property real colW: app.portW
    readonly property int shown: app.hiddenCount > 0 ? cols - 1 : cols
    function colX(i) { return width - Theme.padR - (cols - i) * colW }
    readonly property real hubW: cols > 0 ? Math.max(150, Math.min(300, colX(0) - 28 - 20)) : Math.min(300, width - 40)

    Rectangle { anchors.fill: parent; color: Theme.bg2 }
    DotGrid { anchors.fill: parent }

    // the shared folder
    Item {
        id: hub
        x: 20
        y: 34
        width: root.hubW
        height: 64
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: app.skillCount + " skills in " + app.centralPath + ". Open folder"
        Keys.onReturnPressed: app.openFolder()
        Keys.onSpacePressed: app.openFolder()
        Chamfer { anchors.fill: parent; fill: hubMouse.containsMouse ? Theme.bg4 : Theme.bg3; cutTL: Theme.cut; cutBR: Theme.cut }
        Chamfer { anchors.fill: parent; anchors.margins: -3; visible: hub.activeFocus; stroke: Theme.focus; strokeWidth: 2; cutTL: Theme.cut + 3; cutBR: Theme.cut + 3 }
        Icon { x: 14; anchors.verticalCenter: parent.verticalCenter; name: "folder"; size: 22; color: Theme.markText }
        Column {
            x: 46
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 56
            Row {
                spacing: 6
                NumText { text: app.skillCount; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead }
                Text { text: app.skillCount === 1 ? "skill" : "skills"; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.baseline: parent.children[0].baseline }
            }
            Text { text: app.centralPath; width: parent.width; elide: Text.ElideMiddle; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
        }
        MouseArea { id: hubMouse; anchors.fill: parent; hoverEnabled: !app.noHover; cursorShape: Qt.PointingHandCursor; onClicked: app.openFolder() }
    }

    // nothing detected: say so and offer the way out
    Column {
        visible: root.cols === 0
        anchors.right: parent.right
        anchors.rightMargin: Theme.padR
        y: 30
        width: Math.max(0, root.width - root.hubW - 20 - 20 - 28)
        spacing: 8
        Text { width: parent.width; text: "No coding agent found"; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsTitle; font.weight: Font.Bold; horizontalAlignment: Text.AlignRight }
        Text { width: parent.width; text: "Skill Hub looks for the agents it knows. Switch one on, or add your own."; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignRight }
        Btn { anchors.right: parent.right; kind: "secondary"; compact: true; iconName: "sliders"; text: "Manage agents"; onClicked: app.detailMode = "providers" }
    }

    // one column per managed agent (up to five when more are folded into the overflow column)
    Repeater {
        model: root.shown
        delegate: Item {
            id: col
            required property int index
            // columnCount and agents change together, but QML may see the new count a moment before the new list
            readonly property var a: root.agents[index] ?? ({ id: "", label: "", short: "", monogram: "", icon: ({}), mode: "", ok: 0, total: 0, problems: 0, detected: true, health: "ok" })
            readonly property bool focused: app.focusAgent === a.id
            x: root.colX(index)
            width: root.colW
            height: root.height
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: a.label + ": " + a.ok + " of " + a.total + " skills visible" + (a.problems > 0 ? ", " + a.problems + " problems" : "") + (a.detected ? "" : ", not found on this machine")
            Keys.onReturnPressed: app.focusAgent = a.id
            Keys.onSpacePressed: app.focusAgent = a.id

            Rectangle { anchors.fill: parent; anchors.margins: 2; visible: colMouse.containsMouse && !col.focused; color: Theme.bg3; opacity: 0.55 }
            Item {   // selection / focus hooks
                anchors.fill: parent
                visible: col.focused || col.activeFocus
                readonly property color c: col.activeFocus ? Theme.focus : Theme.mark
                Rectangle { x: 4; y: 8; width: 10; height: 2; color: parent.c }
                Rectangle { x: 4; y: 8; width: 2; height: 10; color: parent.c }
                Rectangle { x: parent.width - 14; y: parent.height - 10; width: 10; height: 2; color: parent.c }
                Rectangle { x: parent.width - 6; y: parent.height - 18; width: 2; height: 10; color: parent.c }
            }
            AgentTile { x: (parent.width - width) / 2; y: 22; text: col.a.monogram; icon: col.a.icon; tone: !col.a.detected ? "off" : col.a.health; selected: col.focused }
            Text {
                y: 64
                width: parent.width - 4
                x: 2
                horizontalAlignment: Text.AlignHCenter
                text: col.a.short
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.ui
                font.pixelSize: root.colW < 66 ? Theme.fsMicro : Theme.fsSmall
                font.weight: Font.DemiBold
            }
            NumText {
                y: 82
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: col.a.ok + " / " + col.a.total
                color: col.a.health === "issue" ? Theme.notifyText : Theme.text
                font.family: Theme.display
                font.pixelSize: Theme.fsLead
                font.weight: Font.DemiBold
            }
            Rectangle {   // ratio bar: visible share in mint, the rest in amber when something needs attention
                x: 12; y: 108
                width: parent.width - 24
                height: 3
                radius: 1.5
                color: Theme.line
                Rectangle { width: parent.width * (col.a.total > 0 ? col.a.ok / col.a.total : 1); height: parent.height; radius: 1.5; color: col.a.health === "issue" ? Theme.notify : Theme.mark }
            }
            MouseArea { id: colMouse; anchors.fill: parent; hoverEnabled: !app.noHover; cursorShape: Qt.PointingHandCursor; onClicked: { col.forceActiveFocus(); app.focusAgent = col.a.id } }
        }
    }

    // overflow: the agents that did not get their own column
    Item {
        id: more
        visible: app.hiddenCount > 0
        x: root.colX(root.cols - 1)
        width: root.colW
        height: root.height
        activeFocusOnTab: visible
        readonly property bool issue: app.overflow.health === "issue"
        Accessible.role: Accessible.Button
        Accessible.name: app.hiddenCount + " more agents: " + app.overflow.names
        Keys.onReturnPressed: app.detailMode = "providers"
        Keys.onSpacePressed: app.detailMode = "providers"
        Rectangle { anchors.fill: parent; anchors.margins: 2; visible: moreMouse.containsMouse; color: Theme.bg3; opacity: 0.55 }
        AgentTile { x: (parent.width - width) / 2; y: 22; text: "+" + app.hiddenCount; tone: more.issue ? "issue" : "ok" }
        Text { y: 64; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "more"; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
        Text {
            y: 82
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: app.overflow.problems > 0 ? app.overflow.problems + " to fix" : "all ok"
            color: more.issue ? Theme.notifyText : Theme.text2
            font.family: Theme.ui
            font.pixelSize: Theme.fsSmall
            font.weight: Font.DemiBold
        }
        MouseArea { id: moreMouse; anchors.fill: parent; hoverEnabled: !app.noHover; cursorShape: Qt.PointingHandCursor; onClicked: app.detailMode = "providers" }
    }
}
