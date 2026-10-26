import QtQuick
import QtQuick.Controls.Basic
import "."

// Every coding agent Skill Hub knows: the ones found on this machine, the rest of the catalog, and your own.
Flickable {
    id: root
    property bool showAll: false
    property bool adding: false
    readonly property string needle: search.text.trim().toLowerCase()
    readonly property var all: app.providerList
    function match(p) { return needle === "" || p.label.toLowerCase().indexOf(needle) >= 0 || p.id.indexOf(needle) >= 0 }
    readonly property var managed: all.filter(function (p) { return p.managed && match(p) })
    readonly property var foundOff: all.filter(function (p) { return !p.managed && p.detected && match(p) })
    readonly property var rest: all.filter(function (p) { return !p.managed && !p.detected && match(p) })
    contentWidth: width
    contentHeight: col.height + 40
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: Scroll {}

    component AgentRow: Item {
        id: row
        property var p
        width: parent ? parent.width : 300
        height: 52
        Accessible.role: Accessible.ListItem
        Accessible.name: p.label + (p.managed ? ", managed" : ", not managed")
        AgentTile { x: 0; anchors.verticalCenter: parent.verticalCenter; size: 32; text: p.monogram; icon: p.icon; tone: p.managed ? (p.detected ? "ok" : "issue") : "off" }
        Column {
            x: 44
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 44 - (p.custom ? 110 : 76)
            spacing: 1
            Text { width: parent.width; text: p.label + (p.custom ? "  (yours)" : ""); elide: Text.ElideRight; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: p.managed ? p.mechanism : p.detected ? "Found via " + p.detectedBy + ", switched off" : "Not found on this machine"
                color: Theme.text2
                font.family: Theme.ui
                font.pixelSize: Theme.fsSmall
            }
        }
        Btn {
            visible: p.custom
            anchors.right: sw.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            kind: "ghost"; compact: true; iconName: "trash"
            Accessible.name: "Remove " + p.label
            enabled: !app.busy
            onClicked: app.removeCustomProvider(p.id)
        }
        Toggle {
            id: sw
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 58
            text: ""
            checked: p.managed
            enabled: !app.busy
            onToggled: app.setProviderManaged(p.id, checked)
            Accessible.name: "Manage " + p.label
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
    }

    Column {
        id: col
        x: 24
        y: 24
        width: root.width - 48 - 6
        spacing: 14

        Column {
            width: parent.width
            spacing: 10
            Text { width: parent.width; text: "Agents"; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Skill Hub manages the coding agents it finds on this machine (" + app.agents.length + " now). Switch one on to link skills into it anyway, off to leave it alone, or add your own."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; lineHeight: 1.2
            }
            Row {
                spacing: 8
                Btn { iconName: "refresh"; text: "Look again"; enabled: !app.busy; onClicked: app.rescanProviders() }
                Btn { iconName: "plus"; text: "Add your own"; onClicked: root.adding = !root.adding }
                Btn { kind: "ghost"; text: "Close"; onClicked: app.dismiss() }
            }
        }

        Column {   // custom agent: inline, not a modal
            visible: root.adding
            width: parent.width
            spacing: 8
            Connections {
                target: app
                function onCustomProviderAdded(error) { if (error === "") { cName.clear(); cDir.clear(); root.adding = false } }
            }
            SectionLabel { width: parent.width; text: "Your agent" }
            Field { id: cName; width: parent.width; placeholderText: "Name, for example My Agent" }
            Field { id: cDir; width: parent.width; iconName: "folder"; placeholderText: "Its skills folder, for example ~/.myagent/skills" }
            Toggle { id: cShared; width: parent.width; text: "It reads the shared folder itself"; hint: "No links are made; its own folder is only scanned for strays" }
            Row {
                spacing: 8
                Btn {
                    kind: "primary"; text: "Add agent"
                    enabled: cName.text.trim() !== "" && cDir.text.trim() !== "" && !app.busy
                    onClicked: app.addCustomProvider(cName.text, cDir.text, cShared.checked)
                }
                Btn { kind: "ghost"; text: "Cancel"; onClicked: root.adding = false }
            }
        }

        Field { id: search; width: parent.width; iconName: "search"; placeholderText: "Search " + app.knownCount + " known agents" }

        Column {
            width: parent.width
            visible: root.managed.length > 0
            SectionLabel { width: parent.width; text: "Managed"; trailing: root.managed.length }
            Repeater { model: root.managed; delegate: AgentRow { required property var modelData; p: modelData } }
        }
        Column {
            width: parent.width
            visible: root.foundOff.length > 0
            SectionLabel { width: parent.width; text: "Found but off"; trailing: root.foundOff.length }
            Repeater { model: root.foundOff; delegate: AgentRow { required property var modelData; p: modelData } }
        }
        Column {
            width: parent.width
            visible: root.rest.length > 0
            SectionLabel { width: parent.width; text: "Not found on this machine"; trailing: root.rest.length }
            Btn {
                kind: "ghost"; compact: true
                iconName: root.showAll || root.needle !== "" ? "up" : "down"
                text: root.showAll || root.needle !== "" ? "Hide" : "Show " + root.rest.length + " more"
                onClicked: root.showAll = !root.showAll
            }
            Repeater { model: root.showAll || root.needle !== "" ? root.rest : []; delegate: AgentRow { required property var modelData; p: modelData } }
        }
        Text {
            visible: root.managed.length + root.foundOff.length + root.rest.length === 0
            width: parent.width
            text: "No agent matches that search."
            color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "The list of agents and where each keeps skills comes from the open-source registry of the skills CLI (vercel-labs/skills, MIT)."
            color: Theme.text3; font.family: Theme.ui; font.pixelSize: Theme.fsMicro
        }
    }
}
