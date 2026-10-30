import QtQuick
import QtQuick.Controls.Basic
import "."

// Selected skill: identity, what you can do about its state, which agents see it, its metadata and its SKILL.md.
Flickable {
    id: root
    // defaults for the moment before a skill is selected (older Qt versions warn when undefined is assigned to a string)
    readonly property var d: Object.assign({ name: "", description: "", state: "", note: "", path: "", source: "", sourceType: "", live: false, parked: false, upstreamPath: "", hash: "",
                                             installed: "", updated: "", tracked: false, links: [] }, app.detail)
    readonly property string sourceText: {
        if (d.source === "") return "none, local skill"
        if (d.live) return d.source + " (live link)"
        return ["github", "gitlab", "git", "local"].indexOf(d.sourceType) < 0 ? d.source + " (managed by npx)" : d.source
    }
    property bool confirmRemove: false
    property bool editSource: false
    property bool moreText: false
    contentWidth: width
    contentHeight: col.height + 40
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: Scroll {}
    onDChanged: { confirmRemove = false; editSource = false; moreText = false; contentY = 0 }

    component Meta: Row {
        property string label
        property string value
        property bool mono: false
        visible: value !== ""
        spacing: 10
        width: parent ? parent.width : 300
        Text { text: parent.label; width: 76; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; topPadding: 1 }
        NumText {
            text: parent.value
            width: parent.width - 86
            wrapMode: Text.WrapAnywhere
            color: Theme.text
            font.family: Theme.ui
            font.pixelSize: Theme.fsSmall
        }
    }

    Column {
        id: col
        x: 24
        width: root.width - 48 - 6
        y: 24
        spacing: 16

        // identity
        Column {
            width: parent.width
            spacing: 8
            Text {
                width: parent.width
                text: root.d.name
                wrapMode: Text.WrapAnywhere
                color: Theme.text
                font.family: Theme.display
                font.pixelSize: Theme.fsHead
                font.weight: Font.Bold
            }
            Row {
                spacing: 8
                Tag { kind: root.d.state }
                Text {
                    visible: root.d.state === "error" && root.d.note !== ""
                    text: root.d.note
                    color: Theme.dangerText
                    font.family: Theme.ui
                    font.pixelSize: Theme.fsSmall
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Text {
                id: desc
                width: parent.width
                text: root.d.description
                wrapMode: Text.Wrap
                maximumLineCount: root.moreText ? 99 : 4
                elide: Text.ElideRight
                color: Theme.text2
                font.family: Theme.ui
                font.pixelSize: Theme.fsBody
                lineHeight: 1.2
            }
            Btn {
                visible: desc.truncated || root.moreText
                kind: "ghost"; compact: true
                text: root.moreText ? "Show less" : "Show more"
                onClicked: root.moreText = !root.moreText
            }
        }

        // actions: only what makes sense for this state
        Flow {
            width: parent.width
            spacing: 8
            Btn { visible: root.d.state === "available"; kind: "primary"; iconName: "search"; text: "Review changes"; enabled: !app.busy; onClicked: app.openReview([root.d.name]) }
            Btn { visible: root.d.state === "available"; iconName: "upgrade"; text: "Update now"; enabled: !app.busy; onClicked: app.updateSkill(root.d.name) }
            Btn { visible: root.d.state === "tracked" || root.d.state === "error"; iconName: "refresh"; text: root.d.state === "error" ? "Retry" : "Check and update"; enabled: !app.busy; onClicked: app.updateSkill(root.d.name) }
            Btn { visible: !root.d.tracked; iconName: "branch"; text: "Set update source"; onClicked: root.editSource = !root.editSource }
            Btn { kind: "danger"; iconName: "trash"; text: "Remove"; enabled: !app.busy; onClicked: root.confirmRemove = !root.confirmRemove }
        }
        Column {   // inline instead of a modal
            visible: root.editSource
            width: parent.width
            spacing: 8
            Field {
                id: srcField
                width: parent.width
                iconName: "branch"
                placeholderText: "owner/repo or GitHub URL"
                onAccepted: if (text !== "") { app.trackSkill(root.d.name, text); root.editSource = false }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "The skill folder is matched by name inside that repository. Nothing changes until you update."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
            }
            Row {
                spacing: 8
                Btn { kind: "primary"; text: "Link source"; enabled: srcField.text !== "" && !app.busy; onClicked: { app.trackSkill(root.d.name, srcField.text); root.editSource = false } }
                Btn { kind: "ghost"; text: "Cancel"; onClicked: root.editSource = false }
            }
        }
        Rectangle {
            visible: root.confirmRemove
            width: parent.width
            height: rm.height + 28
            radius: 4
            color: Theme.dangerLow
            Column {
                id: rm
                x: 14; y: 14
                width: parent.width - 28
                spacing: 10
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: "Move " + root.d.name + " to the backup folder and unlink it from every agent? You can restore it from ~/.agents/.backup."
                    color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody
                }
                Row {
                    spacing: 8
                    Btn { kind: "danger"; text: "Remove skill"; onClicked: { app.removeSkill(root.d.name); root.confirmRemove = false } }
                    Btn { kind: "ghost"; text: "Cancel"; onClicked: root.confirmRemove = false }
                }
            }
        }

        // who sees it
        Column {
            width: parent.width
            spacing: 4
            SectionLabel { width: parent.width; text: "Visible to" }
            Repeater {
                model: root.d.links.filter(function (l) { return l.mode === "link" })
                delegate: Item {
                    required property var modelData
                    width: parent.width
                    height: 32
                    PortDot { id: dot; link: modelData.state; agentLabel: modelData.label; anchors.verticalCenter: parent.verticalCenter }
                    Text { x: 32; text: modelData.label; width: 110; elide: Text.ElideRight; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        x: 152
                        width: parent.width - x - agentSwitch.width
                        elide: Text.ElideRight
                        text: modelData.state === "link" ? "symlink to the shared folder"
                              : modelData.state === "off" ? "turned off for this agent"
                              : modelData.state === "missing" ? "no link, repair needed"
                              : modelData.state === "copy" ? "own copy, not the shared one"
                              : "link points elsewhere"
                        color: (modelData.state === "link" || modelData.state === "off") ? Theme.text2 : Theme.notifyText
                        font.family: Theme.ui
                        font.pixelSize: Theme.fsSmall
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Toggle {
                        id: agentSwitch
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 56
                        text: modelData.label
                        showLabel: false
                        checked: modelData.state !== "off"
                        enabled: !app.busy
                        onToggled: app.setAgentEnabled([root.d.name], modelData.id, checked)
                    }
                }
            }
            Item {   // the agents that read the shared folder see one thing, so they switch together
                readonly property var readers: root.d.links.filter(function (l) { return l.mode !== "link" })
                visible: readers.length > 0
                width: parent.width
                height: 44
                PortDot {
                    link: root.d.parked ? "off" : parent.readers.length > 0 ? parent.readers[0].state : "shared"
                    agentLabel: "Shared-folder readers"
                    anchors.verticalCenter: parent.verticalCenter
                }
                Column {
                    x: 32
                    width: parent.width - x - sharedSwitch.width
                    anchors.verticalCenter: parent.verticalCenter
                    Text { width: parent.width; elide: Text.ElideRight; text: "Shared-folder readers (" + parent.parent.readers.length + ")"; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: root.d.parked ? "turned off, the skill is parked" : parent.parent.readers.map(function (l) { return l.label }).join(", ")
                        color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                    }
                }
                Toggle {
                    id: sharedSwitch
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 56
                    text: "Shared-folder readers"
                    showLabel: false
                    checked: !root.d.parked
                    enabled: !app.busy
                    onToggled: app.setAgentEnabled([root.d.name], "@shared", checked)
                }
            }
        }

        // earlier versions, and what keeps automatic updates away
        Column {
            visible: app.versions.length > 0 || app.onHold
            width: parent.width
            spacing: 6
            SectionLabel { width: parent.width; text: "Versions"; trailing: app.versions.length }
            Row {
                visible: app.onHold
                width: parent.width
                spacing: 8
                Icon { name: "clock"; size: 16; color: Theme.notifyText; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    width: parent.width - 24 - holdOff.width
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: "On hold: automatic updates leave this skill alone"
                    color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                }
                Btn { id: holdOff; kind: "ghost"; compact: true; text: "Release"; enabled: !app.busy; onClicked: app.setHold(root.d.name, false) }
            }
            Repeater {
                model: app.versions
                delegate: Row {
                    required property var modelData
                    width: parent.width
                    spacing: 8
                    Column {
                        width: parent.width - back.width - 8
                        anchors.verticalCenter: parent.verticalCenter
                        NumText { width: parent.width; text: modelData.when; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        Text { width: parent.width; text: modelData.label; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    }
                    Btn { id: back; kind: "ghost"; compact: true; iconName: "refresh"; text: "Roll back"; enabled: !app.busy; onClicked: app.rollback(root.d.name, modelData.stamp) }
                }
            }
        }

        // facts
        Column {
            width: parent.width
            spacing: 6
            SectionLabel { width: parent.width; text: "Source" }
            Meta { label: root.d.sourceType === "local" ? "Folder source" : "Repository"; value: root.sourceText }
            Meta { label: "Upstream"; value: root.d.upstreamPath }
            Meta { label: "Version"; value: root.d.tracked ? (root.d.hash !== "" ? root.d.hash : "unknown until the first update") : "" }
            Meta { label: "Installed"; value: root.d.installed }
            Meta { label: "Updated"; value: root.d.updated }
            Meta { label: "Folder"; value: root.d.path }
            Row {
                spacing: 6
                Btn { kind: "ghost"; compact: true; iconName: "copy"; text: "Copy path"; onClicked: app.copyText(root.d.path.replace(/^~/, "")) }
                Btn { kind: "ghost"; compact: true; iconName: "folder"; text: "Open shared folder"; onClicked: app.openFolder() }
            }
        }

        // SKILL.md
        Column {
            width: parent.width
            spacing: 10
            SectionLabel { width: parent.width; text: "SKILL.md" }
            Rectangle {
                width: parent.width
                height: md.height + 28
                color: Theme.bg2
                radius: 4
                TextEdit {
                    id: md
                    x: 14; y: 14
                    width: parent.width - 28
                    readOnly: true
                    selectByMouse: true
                    textFormat: TextEdit.RichText
                    text: { var m = app.preview; return m !== "" ? app.renderMarkdown(m, Theme.dark) : "<p>This skill has no readable SKILL.md body.</p>" }
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    selectionColor: Qt.alpha(Theme.mark, 0.35)
                    selectedTextColor: Theme.text
                    font.family: Theme.ui
                    font.pixelSize: Theme.fsBody
                    onLinkActivated: (l) => Qt.openUrlExternally(l)
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "SKILL.md of " + app.selected
                }
            }
        }
    }
}
