import QtQuick
import QtQuick.Controls.Basic
import "."

// What an update changes, before it happens: skills and their files on the left, the chosen file's diff on the right. Added
// and removed lines carry + and - as well as a colour. Only what is ticked here is applied, exactly as shown.
Rectangle {
    id: root
    color: Theme.bg1
    focus: visible

    property var leftOut: ({})   // skill name -> not to be applied
    property var open: ({})      // skill name -> its files are shown or hidden, overriding the default
    property int skill: -1
    property int file: -1
    readonly property var reviews: app.reviews
    readonly property bool many: reviews.length > 1
    readonly property bool openByDefault: reviews.length <= 6

    function applicable(r) { return r.stage !== "" && r.available }
    function isOpen(name) { return root.open[name] !== undefined ? root.open[name] : root.openByDefault }
    function toggleOpen(name) {
        var o = Object.assign({}, root.open)
        o[name] = !isOpen(name)
        root.open = o
    }
    function toggleChosen(name) {
        var o = Object.assign({}, root.leftOut)
        if (o[name]) delete o[name]; else o[name] = true
        root.leftOut = o
    }
    readonly property var chosen: {
        var names = []
        for (var i = 0; i < reviews.length; ++i)
            if (applicable(reviews[i]) && !leftOut[reviews[i].name]) names.push(reviews[i].name)
        return names
    }
    readonly property var rows: {
        var out = []
        for (var i = 0; i < reviews.length; ++i) {
            out.push({ s: i, f: -1 })
            if (!isOpen(reviews[i].name)) continue
            for (var k = 0; k < reviews[i].files.length; ++k) out.push({ s: i, f: k })
        }
        return out
    }
    readonly property var current: skill >= 0 && skill < reviews.length && file >= 0 && file < reviews[skill].files.length ? reviews[skill].files[file] : null
    readonly property var lines: {
        if (!current || current.patch === "") return []
        var l = current.patch.split("\n")
        if (l.length > 0 && l[l.length - 1] === "") l.pop()
        return l
    }
    readonly property var totals: {
        var t = { files: 0, add: 0, del: 0 }
        for (var i = 0; i < reviews.length; ++i) {
            t.files += reviews[i].files.length
            for (var k = 0; k < reviews[i].files.length; ++k) { t.add += reviews[i].files[k].add; t.del += reviews[i].files[k].del }
        }
        return t
    }
    function pickFirst() {
        skill = -1; file = -1
        for (var i = 0; i < reviews.length && skill < 0; ++i)
            if (reviews[i].files.length > 0) { skill = i; file = 0 }
    }
    onReviewsChanged: { leftOut = ({}); open = ({}); pickFirst() }
    Keys.onEscapePressed: app.closeReview()

    Item {
        id: head
        width: parent.width
        height: 72
        Column {
            x: 24
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - actions.width - 72
            spacing: 2
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: app.reviewLoading ? "Reading what changed" : root.many ? "Review " + root.reviews.length + " updates" : root.reviews.length === 1 ? "Review " + root.reviews[0].name : "Review"
                color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold
            }
            Text {
                visible: !app.reviewLoading && root.reviews.length > 0
                width: parent.width
                elide: Text.ElideRight
                text: root.totals.files + (root.totals.files === 1 ? " file" : " files") + "  ·  +" + root.totals.add + "  ·  −" + root.totals.del
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
            }
        }
        Row {
            id: actions
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Btn {
                kind: "primary"
                iconName: "upgrade"
                visible: !app.reviewLoading
                text: root.chosen.length === 0 ? "Nothing to apply" : "Apply " + root.chosen.length + (root.chosen.length === 1 ? " update" : " updates")
                enabled: root.chosen.length > 0 && !app.busy
                onClicked: app.applyReview(root.chosen)
            }
            Btn { kind: "ghost"; text: "Close"; onClicked: app.closeReview() }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
    }

    Column {
        visible: app.reviewLoading
        anchors.centerIn: parent
        spacing: 12
        Spinner { anchors.horizontalCenter: parent.horizontalCenter }
        Text { text: "Comparing upstream with the files on disk"; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
    }

    SplitView {
        visible: !app.reviewLoading
        anchors.top: head.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        orientation: Qt.Horizontal
        handle: PaneHandle {}

        ListView {
            id: fileList
            SplitView.preferredWidth: 340
            SplitView.minimumWidth: 220
            clip: true
            model: root.rows
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: Scroll {}
            Accessible.role: Accessible.List
            Accessible.name: "Skills and files in this review"
            delegate: Loader {
                required property var modelData
                width: ListView.view.width
                sourceComponent: modelData.f < 0 ? skillRow : fileRow
            }
        }

        Item {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320

            Column {
                id: notes
                x: 20
                width: parent.width - 40
                topPadding: 12
                spacing: 8
                visible: root.skill >= 0
                readonly property var r: root.skill >= 0 && root.skill < root.reviews.length ? root.reviews[root.skill] : null
                Note {
                    visible: notes.r !== null && notes.r.risk
                    icon: "alert"; tone: Theme.notifyText; fill: Theme.notifyLow
                    text: "Adds or changes scripts. Agents may run them, so read them before you apply."
                }
                Note {
                    visible: notes.r !== null && notes.r.available && notes.r.localEdits
                    icon: "alert"; tone: Theme.notifyText; fill: Theme.notifyLow
                    text: "Your edits in this skill will be replaced. A backup is kept."
                }
                Note {
                    visible: notes.r !== null && !notes.r.available && notes.r.localEdits
                    icon: "info"; tone: Theme.text2; fill: Theme.bg3
                    text: "No update here: these are your own edits against upstream."
                }
            }

            Text {
                id: pathLine
                visible: root.current !== null
                anchors.top: notes.bottom
                anchors.topMargin: 12
                x: 20
                width: parent.width - 40
                text: root.current ? root.current.path : ""
                wrapMode: Text.WrapAnywhere
                color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold
            }

            ListView {
                id: diff
                anchors.top: pathLine.bottom
                anchors.topMargin: 10
                anchors.bottom: parent.bottom
                x: 20
                width: parent.width - 40
                clip: true
                model: root.lines
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: Scroll {}
                Accessible.role: Accessible.List
                Accessible.name: "Changes in this file"
                delegate: Rectangle {
                    id: line
                    required property string modelData
                    readonly property string sign: modelData.charAt(0)
                    width: ListView.view.width - 12
                    height: code.implicitHeight + 2
                    color: sign === "+" ? Theme.gainLow : sign === "-" ? Theme.dangerLow : sign === "@" ? Theme.bg3 : "transparent"
                    Text {
                        id: glyph
                        x: 6
                        width: 14
                        text: line.sign === "+" ? "+" : line.sign === "-" ? "−" : ""
                        color: line.sign === "+" ? Theme.gainText : Theme.dangerText
                        font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.weight: Font.Bold
                    }
                    Text {
                        id: code
                        x: 22
                        width: parent.width - 26
                        text: line.sign === "@" ? line.modelData : line.modelData.substring(1)
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: line.sign === "@" ? Theme.text2 : Theme.text
                        font.family: Theme.mono; font.pixelSize: Theme.fsSmall
                    }
                }
                footer: Text {
                    visible: root.current !== null && root.current.cut
                    width: diff.width
                    height: visible ? implicitHeight + 16 : 0
                    topPadding: 8
                    wrapMode: Text.Wrap
                    text: "This file changes too much to show in full. Everything is applied, also what is not shown."
                    color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                }
            }

            Text {
                visible: root.current !== null && root.lines.length === 0
                anchors.top: pathLine.bottom
                anchors.topMargin: 12
                x: 20
                width: parent.width - 40
                wrapMode: Text.Wrap
                text: root.current === null ? "" : root.current.binary ? "A binary file. Its content cannot be shown." : root.current.cut ? "This file changes too much to show." : "Only the file's permissions change."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody
            }

            Text {
                visible: root.current === null && root.reviews.length > 0
                anchors.centerIn: parent
                text: "Nothing differs"
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody
            }
        }
    }

    component Note: Rectangle {
        id: note
        property string icon
        property string text
        property color tone
        property color fill
        width: parent.width
        height: visible ? noteText.implicitHeight + 16 : 0
        radius: 4
        color: fill
        Icon { x: 10; y: 8; name: note.icon; size: 16; color: note.tone }
        Text {
            id: noteText
            x: 34; y: 8
            width: parent.width - 44
            wrapMode: Text.Wrap
            text: note.text
            color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
        }
    }

    Component {
        id: skillRow
        AbstractButton {
            id: row
            readonly property var r: root.reviews[parent ? parent.modelData.s : 0]
            readonly property bool ok: root.applicable(r)
            readonly property bool on: ok && !root.leftOut[r.name]
            height: 52
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            onClicked: root.toggleOpen(r.name)
            Accessible.role: Accessible.Button
            Accessible.name: r.name + (r.error ? ", " + r.error : ", " + r.files.length + " files") + (r.risk ? ", adds or changes scripts" : "")
            background: Rectangle { color: row.hovered ? Theme.bg3 : "transparent"; border.color: Theme.focus; border.width: row.visualFocus ? 2 : 0 }
            contentItem: Item {
                AbstractButton {   // chosen, or left out of the update
                    id: tick
                    visible: root.many && row.ok
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22; height: 22
                    onClicked: root.toggleChosen(row.r.name)
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Apply " + row.r.name
                    Accessible.checked: row.on
                    contentItem: Item {}
                    background: Rectangle {
                        radius: 3
                        color: row.on ? Theme.action : "transparent"
                        border.color: row.on ? Theme.action : Theme.text3
                        border.width: 1
                        Icon { anchors.centerIn: parent; visible: row.on; name: "check"; size: 14; weight: 2.4; color: Theme.actionInk }
                    }
                }
                Column {
                    x: tick.visible ? 48 : 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - x - 16
                    spacing: 1
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: row.r.name
                        color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: row.r.error ? row.r.error : !row.r.available && row.r.files.length === 0 ? "current, nothing differs" : (row.r.files.length + (row.r.files.length === 1 ? " file" : " files") + (row.r.risk ? "  ·  scripts" : "") + (row.r.localEdits ? "  ·  local edits" : ""))
                        color: row.r.error ? Theme.dangerText : row.r.risk ? Theme.notifyText : Theme.text2
                        font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                    }
                }
            }
        }
    }

    Component {
        id: fileRow
        AbstractButton {
            id: row
            readonly property var at: parent ? parent.modelData : ({ s: 0, f: 0 })
            readonly property var f: root.reviews[at.s].files[at.f]
            readonly property bool active: root.skill === at.s && root.file === at.f
            height: 30
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            onClicked: { root.skill = at.s; root.file = at.f }
            Accessible.role: Accessible.Button
            Accessible.name: f.status + " " + f.path + (f.risk ? ", a script" : "")
            background: Rectangle { color: row.active ? Theme.bg4 : row.hovered ? Theme.bg3 : "transparent"; border.color: Theme.focus; border.width: row.visualFocus ? 2 : 0 }
            contentItem: Item {
                Text {
                    id: sign
                    x: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    text: row.f.status === "added" ? "+" : row.f.status === "removed" ? "−" : "~"
                    color: row.f.status === "added" ? Theme.gainText : row.f.status === "removed" ? Theme.dangerText : Theme.text2
                    font.family: Theme.mono; font.pixelSize: Theme.fsBody; font.weight: Font.Bold
                }
                Text {
                    x: 40
                    anchors.verticalCenter: parent.verticalCenter
                    width: counts.x - x - 8
                    elide: Text.ElideMiddle
                    text: row.f.path
                    color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                }
                Row {
                    id: counts
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Icon { visible: row.f.risk; name: "alert"; size: 14; color: Theme.notifyText; anchors.verticalCenter: parent.verticalCenter }
                    NumText {
                        text: row.f.binary ? "bin" : "+" + row.f.add + " −" + row.f.del
                        color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }
}
