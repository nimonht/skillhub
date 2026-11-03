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
        if (d.source === "") return qsTr("none, local skill")
        if (d.live) return qsTr("%1 (live link)").arg(d.source)
        return ["github", "gitlab", "git", "local"].indexOf(d.sourceType) < 0 ? qsTr("%1 (managed by npx)").arg(d.source) : d.source
    }
    property bool confirmRemove: false
    property bool editSource: false
    property bool pickProject: false
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
        Text { textFormat: Text.PlainText; text: parent.label; width: 76; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; topPadding: 1 }
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
                textFormat: Text.PlainText
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
                    textFormat: Text.PlainText
                    visible: root.d.state === "error" && root.d.note !== ""
                    text: root.d.note
                    color: Theme.dangerText
                    font.family: Theme.ui
                    font.pixelSize: Theme.fsSmall
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Text {
                textFormat: Text.PlainText
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
                text: root.moreText ? qsTr("Show less") : qsTr("Show more")
                onClicked: root.moreText = !root.moreText
            }
        }

        // actions: only what makes sense for this state
        Flow {
            width: parent.width
            spacing: 8
            Btn { visible: root.d.state === "available"; kind: "primary"; iconName: "search"; text: qsTr("Review changes"); enabled: !app.busy; onClicked: app.openReview([root.d.name]) }
            Btn { visible: root.d.state === "available"; iconName: "upgrade"; text: qsTr("Update now"); enabled: !app.busy; onClicked: app.updateSkill(root.d.name) }
            Btn { visible: root.d.state === "tracked" || root.d.state === "error"; iconName: "refresh"; text: root.d.state === "error" ? qsTr("Retry") : qsTr("Check and update"); enabled: !app.busy; onClicked: app.updateSkill(root.d.name) }
            Btn { visible: !root.d.tracked; iconName: "branch"; text: qsTr("Set update source"); onClicked: root.editSource = !root.editSource }
            Btn { visible: !app.inProject && app.projects.length > 0; iconName: "branch"; text: qsTr("Copy to project"); enabled: !app.busy; onClicked: root.pickProject = !root.pickProject }
            Btn { kind: "danger"; iconName: "trash"; text: qsTr("Remove"); enabled: !app.busy; onClicked: root.confirmRemove = !root.confirmRemove }
        }
        Column {
            visible: root.pickProject && !app.inProject
            width: parent.width
            spacing: 4
            Repeater {
                model: app.projects
                delegate: Btn {
                    required property var modelData
                    kind: "ghost"; compact: true; iconName: "branch"
                    text: modelData.name
                    enabled: !app.busy
                    onClicked: { root.pickProject = false; app.copyToProject(root.d.name, modelData.path) }
                }
            }
        }
        Column {   // inline instead of a modal
            visible: root.editSource
            width: parent.width
            spacing: 8
            Field {
                id: srcField
                width: parent.width
                iconName: "branch"
                placeholderText: qsTr("owner/repo or GitHub URL")
                onAccepted: if (text !== "") { app.trackSkill(root.d.name, text); root.editSource = false }
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("The skill folder is matched by name inside that repository. Nothing changes until you update.")
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
            }
            Row {
                spacing: 8
                Btn { kind: "primary"; text: qsTr("Link source"); enabled: srcField.text !== "" && !app.busy; onClicked: { app.trackSkill(root.d.name, srcField.text); root.editSource = false } }
                Btn { kind: "ghost"; text: qsTr("Cancel"); onClicked: root.editSource = false }
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
                    textFormat: Text.PlainText
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Move %1 to the backup folder and unlink it from every agent? You can restore it from ~/.agents/.backup.").arg(root.d.name)
                    color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody
                }
                Row {
                    spacing: 8
                    Btn { kind: "danger"; text: qsTr("Remove skill"); onClicked: { app.removeSkill(root.d.name); root.confirmRemove = false } }
                    Btn { kind: "ghost"; text: qsTr("Cancel"); onClicked: root.confirmRemove = false }
                }
            }
        }

        // who sees it
        Column {
            width: parent.width
            spacing: 4
            SectionLabel { width: parent.width; text: qsTr("Visible to") }
            Repeater {
                model: root.d.links.filter(function (l) { return l.mode === "link" })
                delegate: Item {
                    required property var modelData
                    width: parent.width
                    height: 32
                    PortDot { id: dot; link: modelData.state; agentLabel: modelData.label; anchors.verticalCenter: parent.verticalCenter }
                    Text { textFormat: Text.PlainText; x: 32; text: modelData.label; width: 110; elide: Text.ElideRight; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        textFormat: Text.PlainText
                        x: 152
                        width: parent.width - x - agentSwitch.width
                        elide: Text.ElideRight
                        text: modelData.state === "link" ? qsTr("symlink to the shared folder")
                              : modelData.state === "off" ? qsTr("turned off for this agent")
                              : modelData.state === "synced" ? qsTr("a copy kept identical")
                              : modelData.state === "missing" ? qsTr("no link, repair needed")
                              : modelData.state === "copy" ? qsTr("own copy, not the shared one")
                              : qsTr("link points elsewhere")
                        color: (modelData.state === "link" || modelData.state === "off" || modelData.state === "synced") ? Theme.text2 : Theme.notifyText
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
                        visible: !app.inProject
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
                    agentLabel: qsTr("Shared-folder readers")
                    anchors.verticalCenter: parent.verticalCenter
                }
                Column {
                    x: 32
                    width: parent.width - x - sharedSwitch.width
                    anchors.verticalCenter: parent.verticalCenter
                    Text { textFormat: Text.PlainText; width: parent.width; elide: Text.ElideRight; text: qsTr("Shared-folder readers (%1)").arg(parent.parent.readers.length); color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        elide: Text.ElideRight
                        text: root.d.parked ? qsTr("turned off, the skill is parked") : parent.parent.readers.map(function (l) { return l.label }).join(", ")
                        color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                    }
                }
                Toggle {
                    id: sharedSwitch
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 56
                    text: qsTr("Shared-folder readers")
                    showLabel: false
                    visible: !app.inProject
                    checked: !root.d.parked
                    enabled: !app.busy
                    onToggled: app.setAgentEnabled([root.d.name], "@shared", checked)
                }
            }
        }

        Text {
            textFormat: Text.PlainText
            visible: app.inProject
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("A project's skills are shared with its team, so they have no personal switches.")
            color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2
        }

        // earlier versions, and what keeps automatic updates away
        Column {
            visible: app.versions.length > 0 || app.onHold || root.d.tracked
            width: parent.width
            spacing: 6
            SectionLabel { width: parent.width; text: qsTr("Versions"); trailing: app.versions.length }
            Row {
                visible: app.onHold
                width: parent.width
                spacing: 8
                Icon { name: "clock"; size: 16; color: Theme.notifyText; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width - 24 - holdOff.width
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: qsTr("On hold: automatic updates leave this skill alone")
                    color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                }
                Btn { id: holdOff; kind: "ghost"; compact: true; text: qsTr("Release"); enabled: !app.busy; onClicked: app.setHold(root.d.name, false) }
            }
            Btn {
                visible: root.d.tracked && !app.onHold
                kind: "ghost"; compact: true; iconName: "clock"; text: qsTr("Hold this version"); enabled: !app.busy
                onClicked: app.setHold(root.d.name, true)
                Accessible.name: qsTr("Hold this version: automatic updates leave it alone")
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
                        Text { textFormat: Text.PlainText; width: parent.width; text: modelData.label; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    }
                    Btn { id: back; kind: "ghost"; compact: true; iconName: "refresh"; text: qsTr("Roll back"); enabled: !app.busy; onClicked: app.rollback(root.d.name, modelData.stamp) }
                }
            }
        }

        // the offline check, what outside scanners said, and a second opinion: findings to read, never a verdict
        Column {
            id: checks
            readonly property var s: Object.assign({ loaded: false, findings: [], high: 0, medium: 0, truncated: false,
                                                     reports: { state: "off", source: "", scanners: [] } }, app.safety)
            readonly property var rep: checks.s.reports
            width: parent.width
            spacing: 6
            SectionLabel { width: parent.width; text: qsTr("Checks"); trailing: checks.s.loaded && checks.s.findings.length > 0 ? String(checks.s.findings.length) : "" }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.Wrap
                text: !checks.s.loaded ? qsTr("Reading the skill...")
                    : checks.s.findings.length === 0 ? qsTr("Nothing to note. The check reads the text with fixed rules, so no finding is no proof of safety.")
                    : qsTr("%1 high, %2 medium. Read these lines before an agent follows the skill.").arg(checks.s.high).arg(checks.s.medium)
                color: checks.s.high > 0 ? Theme.dangerText : Theme.text2
                font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2
            }
            Repeater {
                model: checks.s.findings
                delegate: Column {
                    required property var modelData
                    width: checks.width
                    spacing: 2
                    Row {
                        spacing: 8
                        Text { textFormat: Text.PlainText; text: modelData.level === "high" ? qsTr("HIGH") : qsTr("MEDIUM"); color: modelData.level === "high" ? Theme.dangerText : Theme.notifyText; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
                        Text { textFormat: Text.PlainText; text: modelData.title; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    }
                    NumText { width: parent.width; elide: Text.ElideMiddle; text: modelData.file + ":" + modelData.line; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    Text { textFormat: Text.PlainText; width: parent.width; wrapMode: Text.WrapAnywhere; text: modelData.excerpt; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                }
            }
            Text { textFormat: Text.PlainText; visible: checks.s.truncated; text: qsTr("... and more: run skillhub scan %1").arg(root.d.name); color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }

            SectionLabel { width: parent.width; text: qsTr("Reports from skills.sh") }
            Text {
                textFormat: Text.PlainText
                visible: checks.rep.state !== "ok"
                width: parent.width
                wrapMode: Text.Wrap
                text: checks.rep.state === "off" ? qsTr("Off. When on, Skill Hub asks skills.sh what three outside scanners found. Only public GitHub skills are looked up, and the repository and the skill name are sent.")
                    : checks.rep.state === "not-github" ? qsTr("Only skills from public GitHub repositories have reports.")
                    : checks.rep.state === "private" ? qsTr("%1 is not public, so nothing was sent.").arg(checks.rep.source)
                    : checks.rep.state === "failed" ? qsTr("skills.sh could not be reached.")
                    : qsTr("skills.sh has no report for this skill.")
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2
            }
            Btn {
                visible: checks.rep.state === "off"
                kind: "ghost"; compact: true; text: qsTr("Turn reports on")
                onClicked: { app.setSetting("security", "reports", true); app.readSafety(root.d.name) }
            }
            Repeater {
                model: checks.rep.state === "ok" ? checks.rep.scanners : []
                delegate: Row {
                    required property var modelData
                    width: checks.width
                    spacing: 10
                    Text { textFormat: Text.PlainText; width: 150; elide: Text.ElideRight; text: modelData.label; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    Text {
                        textFormat: Text.PlainText
                        width: 130
                        text: modelData.risk + (modelData.alerts > 0 ? ", " + (modelData.alerts === 1 ? qsTr("1 alert") : qsTr("%1 alerts").arg(modelData.alerts)) : "")
                        color: modelData.risk === "high" || modelData.risk === "critical" ? Theme.dangerText : modelData.risk === "medium" ? Theme.notifyText : Theme.text2
                        font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold
                    }
                    NumText { text: modelData.analyzedAt.substring(0, 10); color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                }
            }
            Text {
                textFormat: Text.PlainText
                visible: checks.rep.state === "ok"
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("Each scanner on its own: they often disagree. A report describes the skill upstream on its date, which may not be the version you have.")
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2
            }

            SectionLabel { width: parent.width; text: qsTr("Second opinion"); visible: app.reviewAgents.length > 0 }
            Text {
                textFormat: Text.PlainText
                visible: app.reviewAgents.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("Opens a terminal in a copy of this skill with the agent's command typed. Press Enter to start it; it asks before it does anything. Its answer is not saved.")
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2
            }
            Flow {
                width: parent.width
                spacing: 6
                Repeater {
                    model: app.reviewAgents
                    delegate: Btn { required property var modelData; kind: "ghost"; compact: true; iconName: "terminal"; text: qsTr("Ask %1").arg(modelData.label); enabled: checks.s.loaded; onClicked: app.askSecondOpinion(modelData.id) }
                }
            }
        }

        // facts
        Column {
            width: parent.width
            spacing: 6
            SectionLabel { width: parent.width; text: qsTr("Source") }
            Meta { label: root.d.sourceType === "local" ? qsTr("Folder source") : qsTr("Repository"); value: root.sourceText }
            Meta { label: qsTr("Upstream"); value: root.d.upstreamPath }
            Meta { label: qsTr("Version"); value: root.d.tracked ? (root.d.hash !== "" ? root.d.hash : qsTr("unknown until the first update")) : "" }
            Meta { label: qsTr("Installed"); value: root.d.installed }
            Meta { label: qsTr("Updated"); value: root.d.updated }
            Meta { label: qsTr("Folder"); value: root.d.path }
            Row {
                spacing: 6
                Btn { kind: "ghost"; compact: true; iconName: "copy"; text: qsTr("Copy path"); onClicked: app.copyText(root.d.path.replace(/^~/, "")) }
                Btn { kind: "ghost"; compact: true; iconName: "folder"; text: qsTr("Open shared folder"); onClicked: app.openFolder() }
            }
        }

        // SKILL.md
        Column {
            width: parent.width
            spacing: 10
            SectionLabel { width: parent.width; text: "SKILL.md" }   // i18n-ok
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
                    text: { var m = app.preview; return m !== "" ? app.renderMarkdown(m, Theme.dark) : "<p>" + qsTr("This skill has no readable SKILL.md body.") + "</p>" }
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    selectionColor: Qt.alpha(Theme.mark, 0.35)
                    selectedTextColor: Theme.text
                    font.family: Theme.ui
                    font.pixelSize: Theme.fsBody
                    onLinkActivated: (l) => { if (/^(https?|mailto):/i.test(l)) Qt.openUrlExternally(l) }  // a repository wrote this: no file:, smb: or other handlers
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("SKILL.md of %1").arg(app.selected)
                }
            }
        }
    }
}
