import QtQuick
import QtQuick.Controls.Basic
import "."

// One agent inspected: how it finds skills, how healthy that is, and what is wrong.
Flickable {
    id: root
    readonly property var a: Object.assign({ id: "", label: "", monogram: "", mode: "", mechanism: "", folder: "", detected: true, detectedBy: "",
                                             wired: true, ok: 0, problems: 0, total: 0, health: "ok" }, app.agentDetail)
    readonly property var issues: a.issues ? a.issues : []
    readonly property var skills: a.skills ? a.skills : []
    readonly property bool readsShared: a.mode !== "link"
    readonly property string switchKey: readsShared ? "@shared" : (a.id || "")
    function names() { return skills.map(function (s) { return s.name }) }
    contentWidth: width
    contentHeight: col.height + 40
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: Scroll {}

    Column {
        id: col
        x: 24
        y: 24
        width: root.width - 48 - 6
        spacing: 16

        Column {
            width: parent.width
            spacing: 10
            Row {
                spacing: 12
                AgentTile { size: 40; text: root.a.monogram || ""; icon: root.a.icon; tone: root.a.detected === false ? "off" : root.a.health || "ok" }
                Text { text: root.a.label || ""; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold; anchors.verticalCenter: parent.verticalCenter }
            }
            Tag {
                kind: root.a.detected === false ? "absent" : root.a.health === "ok" ? "healthy" : "attention"
                label: root.a.detected === false ? "Not found, managed because you switched it on"
                       : root.a.health === "ok" ? (root.a.ok === root.a.total ? "All skills visible" : (root.a.total - root.a.ok) + " turned off")
                       : (root.a.problems || 0) > 0 ? root.a.problems + " need repair" : "Not wired up"
            }
            Text { width: parent.width; wrapMode: Text.Wrap; text: root.a.mechanism || ""; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
            Text { width: parent.width; wrapMode: Text.Wrap; visible: (root.a.detectedBy || "") !== ""; text: "Found via " + (root.a.detectedBy || ""); color: Theme.text3; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
        }

        Row {
            spacing: 6
            NumText { text: root.a.ok; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsBig; font.weight: Font.Bold }
            Text { text: "of " + root.a.total + " skills visible"; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.baseline: parent.children[0].baseline }
        }

        Text {
            visible: root.a.detected === false
            width: parent.width
            wrapMode: Text.Wrap
            text: "Neither its command nor its folder was found, so nothing proves it is installed. Skills are still linked for it. Stop managing it if that is not what you want."
            color: Theme.text2
            font.family: Theme.ui
            font.pixelSize: Theme.fsBody
            lineHeight: 1.2
        }
        Text {
            visible: root.a.wired === false
            width: parent.width
            wrapMode: Text.Wrap
            text: "The shared folder is not listed in " + (root.a.folder || "its config file") + " yet. Repair adds it and keeps the entries that are already there."
            color: Theme.notifyText
            font.family: Theme.ui
            font.pixelSize: Theme.fsBody
            lineHeight: 1.2
        }

        Btn {
            visible: root.issues.length > 0 || root.a.wired === false
            kind: "primary"; iconName: "tool"; text: "Repair links"; enabled: !app.busy
            onClicked: app.adopt()
        }

        Column {
            visible: root.issues.length > 0
            width: parent.width
            spacing: 2
            SectionLabel { width: parent.width; text: "Cannot see these skills"; trailing: root.issues.length }
            Repeater {
                model: root.issues.slice(0, 40)
                delegate: AbstractButton {
                    required property string modelData
                    width: parent.width
                    height: 34
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData
                    onClicked: app.select(modelData)
                    background: Rectangle { radius: 4; color: parent.hovered ? Theme.bg3 : "transparent"; border.color: Theme.focus; border.width: parent.visualFocus ? 2 : 0 }
                    contentItem: Row {
                        spacing: 10
                        leftPadding: 8
                        PortDot { link: "missing"; agentLabel: root.a.label; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: modelData; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.verticalCenter: parent.verticalCenter }
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: !app.noHover }
                }
            }
        }

        Column {
            visible: root.skills.length > 0
            width: parent.width
            spacing: 6
            SectionLabel { width: parent.width; text: "Skills"; trailing: root.skills.length }
            Text {
                visible: root.readsShared
                width: parent.width
                wrapMode: Text.Wrap
                text: "This agent reads the shared folder, so a switch here applies to every agent that does. A skill turned off moves to the parked folder."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2
            }
            Row {
                spacing: 6
                Btn { kind: "ghost"; compact: true; text: "Enable all"; enabled: !app.busy; onClicked: app.setAgentEnabled(root.names(), root.switchKey, true) }
                Btn { kind: "ghost"; compact: true; text: "Disable all"; enabled: !app.busy; onClicked: app.setAgentEnabled(root.names(), root.switchKey, false) }
            }
            ListView {
                width: parent.width
                height: Math.min(contentHeight, 340)
                clip: true
                model: root.skills
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: Scroll {}
                Accessible.role: Accessible.List
                Accessible.name: "Skills of this agent"
                delegate: Item {
                    required property var modelData
                    width: ListView.view.width - 8
                    height: 34
                    PortDot { link: modelData.state; agentLabel: root.a.label || ""; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        x: 32
                        width: parent.width - x - toggle.width
                        elide: Text.ElideRight
                        text: modelData.name
                        color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Toggle {
                        id: toggle
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 56
                        text: modelData.name
                        showLabel: false
                        checked: modelData.state !== "off"
                        enabled: !app.busy
                        onToggled: app.setAgentEnabled([modelData.name], root.switchKey, checked)
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: 4
            SectionLabel { width: parent.width; text: "Folder" }
            Text { width: parent.width; text: root.a.folder || ""; wrapMode: Text.WrapAnywhere; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
            Row {
                spacing: 6
                Btn { kind: "ghost"; compact: true; iconName: "sliders"; text: "Manage agents"; onClicked: app.detailMode = "providers" }
                Btn { kind: "ghost"; compact: true; iconName: "x"; text: "Stop managing"; enabled: !app.busy; onClicked: app.setProviderManaged(root.a.id, false) }
            }
        }
    }
}
