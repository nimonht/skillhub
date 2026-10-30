import QtQuick
import QtQuick.Controls.Basic
import "."

// Add skills from a repository or a folder: read it, tick the skills, install. Inline, not a modal.
Item {
    id: root
    property var chosen: ({})
    property bool live: false
    readonly property int chosenCount: Object.keys(chosen).length
    readonly property string origin: app.candidateType === "local" ? "this machine" : app.candidateHost
    function toggle(name) {
        var c = Object.assign({}, chosen)
        if (c[name]) delete c[name]; else c[name] = true
        chosen = c
    }
    function reset() { chosen = ({}); live = false }
    Connections { target: app; function onCandidatesChanged() { root.reset() } }
    onVisibleChanged: if (visible) repo.forceActiveFocus()

    Loader {   // native dialogs when the module exists, otherwise a typed path
        id: dialogs
        source: "FileDialogs.qml"
        onLoaded: item.folderChosen.connect(function (p) { repo.text = p; app.discover(p) })
    }

    Column {
        id: head
        x: 24; y: 24
        width: root.width - 48
        spacing: 12
        Text { width: parent.width; text: "Add skills"; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Name a repository or a folder. Skill Hub lists every SKILL.md in it; you choose which ones to install into the shared folder."
            color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; lineHeight: 1.2
        }
        Field {
            id: repo
            width: parent.width
            iconName: "branch"
            placeholderText: "owner/repo, a GitLab or git URL, or a local folder"
            enabled: !app.busy
            onAccepted: if (text.trim() !== "") app.discover(text)
        }
        Row {
            spacing: 8
            Btn { kind: "primary"; iconName: "search"; text: "Read source"; enabled: repo.text.trim() !== "" && !app.busy; onClicked: app.discover(repo.text) }
            Btn { visible: dialogs.status === Loader.Ready; kind: "secondary"; iconName: "folder"; text: "Choose folder"; enabled: !app.busy; onClicked: dialogs.item.openFolder() }
            Btn { kind: "ghost"; text: "Close"; onClicked: app.closeAdd() }
        }
        Text {
            visible: app.candidateError !== ""
            width: parent.width
            wrapMode: Text.Wrap
            text: app.candidateError
            color: Theme.dangerText; font.family: Theme.ui; font.pixelSize: Theme.fsBody
        }
    }

    // reading: skeleton rows (layout stays stable, nothing pretends to be loaded)
    Column {
        visible: app.discovering
        anchors.top: head.bottom
        anchors.topMargin: 20
        x: 24
        width: root.width - 48
        spacing: 10
        Repeater {
            model: 4
            delegate: Rectangle {
                width: parent.width; height: 48; radius: 6
                color: Theme.bg2
                opacity: Theme.still ? 0.8 : 0.5
                SequentialAnimation on opacity {
                    running: app.discovering && !Theme.still
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.9; duration: 700 }
                    NumberAnimation { to: 0.45; duration: 700 }
                }
            }
        }
    }

    Item {
        visible: !app.discovering && app.candidates.length > 0
        anchors.top: head.bottom
        anchors.topMargin: 16
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        SectionLabel { id: lbl; x: 24; width: parent.width - 48; text: "Skills in " + app.candidateRepo + (root.origin !== "" ? " · " + root.origin : ""); trailing: app.candidates.length }
        Toggle {
            id: liveToggle
            visible: app.candidateType === "local" && !app.inProject
            anchors.top: lbl.bottom
            anchors.topMargin: 8
            x: 24
            width: parent.width - 48
            text: "Live link"
            hint: "Edits in the folder reach every agent at once. Off: a copy, updated when you choose"
            checked: root.live
            onToggled: root.live = checked
        }
        ListView {
            id: list
            anchors.top: liveToggle.visible ? liveToggle.bottom : lbl.bottom
            anchors.topMargin: 6
            anchors.bottom: foot.top
            anchors.left: parent.left
            anchors.right: parent.right
            clip: true
            model: app.candidates
            ScrollBar.vertical: Scroll {}
            delegate: AbstractButton {
                id: cand
                required property var modelData
                readonly property bool on: root.chosen[modelData.name] === true
                width: list.width
                height: 62
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                Accessible.role: Accessible.CheckBox
                Accessible.name: modelData.name
                Accessible.checked: on
                onClicked: root.toggle(modelData.name)
                background: Rectangle {
                    color: cand.on ? Theme.bg4 : cand.hovered ? Theme.bg3 : "transparent"
                    border.color: cand.visualFocus ? Theme.focus : "transparent"
                    border.width: 2
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
                }
                contentItem: Item {
                    Rectangle {   // checkbox: square, check glyph, not colour-only
                        x: 24; anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20; radius: 4
                        color: cand.on ? Theme.mark : "transparent"
                        border.color: cand.on ? Theme.mark : Theme.lineInput
                        border.width: 1.5
                        Icon { visible: cand.on; anchors.centerIn: parent; name: "check"; size: 14; weight: 2.6; color: Theme.inkOnMark }
                    }
                    Column {
                        x: 58
                        width: parent.width - 58 - 24 - (installed.visible ? 80 : 0)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text { width: parent.width; text: cand.modelData.name; elide: Text.ElideRight; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
                        Text { width: parent.width; text: cand.modelData.description; elide: Text.ElideRight; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    }
                    Tag { id: installed; visible: cand.modelData.installed; kind: "current"; label: "Installed"; anchors.right: parent.right; anchors.rightMargin: 24; anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
        Item {
            id: foot
            anchors.bottom: parent.bottom
            width: parent.width
            height: 64
            Rectangle { width: parent.width; height: 1; color: Theme.line }
            Row {
                x: 24
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                Btn {
                    kind: "primary"
                    iconName: "download"
                    text: root.chosenCount > 0 ? "Install " + root.chosenCount + (root.chosenCount === 1 ? " skill" : " skills") : "Install"
                    enabled: root.chosenCount > 0 && !app.busy
                    onClicked: app.installChosen(Object.keys(root.chosen), root.live)
                }
                Btn {
                    kind: "ghost"
                    text: root.chosenCount === app.candidates.length ? "Clear" : "Select all"
                    onClicked: {
                        var c = {}
                        if (root.chosenCount !== app.candidates.length) for (var i = 0; i < app.candidates.length; ++i) c[app.candidates[i].name] = true
                        root.chosen = c
                    }
                }
            }
        }
    }
}
