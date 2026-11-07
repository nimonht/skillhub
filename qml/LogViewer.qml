import QtQuick
import QtQuick.Controls.Basic
import "."

// The log viewer: every line of the log file's current session and the one before (app.logView, src/gui/app_log.h). What the
// window, Qt and the background service said, filtered by level and side, searched, copied or saved for a bug report.
Rectangle {
    id: root
    color: Theme.bg1
    clip: true

    property bool follow: true      // the list stays at the newest line
    property bool wrap: false       // long lines break instead of being cut
    property var chosen: ({})       // rows selected: { row: true }
    property int chosenRev: 0
    property int anchorRow: -1
    readonly property var view: app.logView

    function isChosen(row) { void chosenRev; return chosen[row] === true }
    function chosenRows() {
        void chosenRev
        const rows = []
        for (const k in chosen) rows.push(parseInt(k))
        return rows.sort((a, b) => a - b)
    }
    function choose(row, ctrl, shift) {
        const next = ctrl ? Object.assign({}, chosen) : {}
        if (shift && anchorRow >= 0) {
            for (let r = Math.min(anchorRow, row); r <= Math.max(anchorRow, row); ++r) next[r] = true
        } else if (ctrl && next[row]) {
            delete next[row]
            anchorRow = row
        } else {
            next[row] = true
            anchorRow = row
        }
        chosen = next
        chosenRev++
    }
    function chooseAll() {
        const next = {}
        for (let r = 0; r < list.count; ++r) next[r] = true
        chosen = next
        chosenRev++
    }
    function clearChosen() { chosen = ({}); chosenRev++; anchorRow = -1 }
    function levelColor(level) { return level === "error" ? Theme.dangerText : level === "warn" ? Theme.notifyText : Theme.text2 }

    // a selection names rows of the list as it was: it ends when the filter changes, the list is cleared, or the oldest line goes
    Connections {
        target: root.view
        function onChanged() { root.clearChosen() }
        function onRowsRemoved() { root.clearChosen() }
        function onModelReset() { root.clearChosen() }
    }

    Loader {
        id: dialogs
        source: "FileDialogs.qml"
        onLoaded: { item.startFolder = app.documentsFolder; item.textChosen.connect(function (p) { app.saveLog(p) }) }
    }

    Item {
        id: bar
        y: 1
        width: parent.width
        height: 34
        SectionLabel {
            id: heading
            x: 20
            width: 150
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Log")
            trailing: list.count !== root.view.total ? list.count + " / " + root.view.total : (list.count > 0 ? list.count : "")
        }
        Segmented {
            id: levels
            x: heading.x + heading.width + 8
            anchors.verticalCenter: parent.verticalCenter
            width: 190
            options: [{ value: "all", label: qsTr("All") }, { value: "warn", label: qsTr("Warnings") }, { value: "error", label: qsTr("Errors") }]
            current: root.view.level
            onPicked: (value) => root.view.level = value
        }
        Segmented {
            id: sides
            x: levels.x + levels.width + 8
            anchors.verticalCenter: parent.verticalCenter
            width: 190
            options: [{ value: "all", label: qsTr("Everything") }, { value: "window", label: qsTr("Window") }, { value: "service", label: qsTr("Service") }]
            current: root.view.side
            onPicked: (value) => root.view.side = value
        }
        Row {
            id: actions
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Btn { kind: root.follow ? "secondary" : "ghost"; compact: true; iconName: "down"; text: ""; Accessible.name: qsTr("Follow new lines"); onClicked: { root.follow = !root.follow; if (root.follow) list.positionViewAtEnd() } }
            Btn { kind: root.wrap ? "secondary" : "ghost"; compact: true; iconName: "all"; text: ""; Accessible.name: qsTr("Wrap long lines"); onClicked: root.wrap = !root.wrap }
            Btn { kind: "ghost"; compact: true; iconName: "copy"; text: ""; Accessible.name: qsTr("Copy the selected lines, or all shown"); onClicked: app.copyLog(root.chosenRows()) }
            Btn { kind: "ghost"; compact: true; iconName: "download"; text: ""; enabled: dialogs.status === Loader.Ready; Accessible.name: qsTr("Save the shown lines as a file"); onClicked: dialogs.item.saveText("skillhub.log") }
            Btn { kind: "ghost"; compact: true; iconName: "folder"; text: ""; Accessible.name: qsTr("Open the folder of the log files"); onClicked: root.view.openFolder() }
            Btn { kind: "ghost"; compact: true; iconName: "trash"; text: ""; Accessible.name: qsTr("Clear the list (the files stay)"); onClicked: root.view.clearView() }
        }
        Field {
            id: search
            anchors.left: sides.right
            anchors.leftMargin: 8
            anchors.right: actions.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            height: 28
            iconName: "search"
            placeholderText: qsTr("Search the log")
            Accessible.name: qsTr("Search the log")
            visible: width >= 90
            text: root.view.text
            onTextChanged: root.view.text = text
        }
    }

    ListView {
        id: list
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        model: root.view
        activeFocusOnTab: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: Scroll {}
        onCountChanged: if (root.follow) Qt.callLater(positionViewAtEnd)
        onMovementEnded: root.follow = atYEnd
        Component.onCompleted: positionViewAtEnd()
        Keys.onPressed: (event) => {
            if (event.matches(StandardKey.SelectAll)) { root.chooseAll(); event.accepted = true }
            else if (event.matches(StandardKey.Copy)) { app.copyLog(root.chosenRows()); event.accepted = true }
        }
        delegate: Item {
            id: line
            required property int index
            required property string time
            required property string when
            required property string level
            required property string source
            required property string text
            readonly property bool open: root.wrap || opened
            property bool opened: false
            width: list.width
            height: Math.max(24, body.implicitHeight + 8)

            Rectangle { anchors.fill: parent; color: root.isChosen(line.index) ? Theme.bg4 : "transparent" }
            Row {
                x: 20
                y: 4
                spacing: 10
                NumText { id: stamp; text: line.time; width: 62; color: Theme.text3; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                Icon { visible: line.level === "warn" || line.level === "error"; name: "alert"; size: 14; color: root.levelColor(line.level); y: 1 }
                Text {
                    textFormat: Text.PlainText
                    text: line.source
                    width: 50
                    color: Theme.text3
                    font.family: Theme.ui
                    font.pixelSize: Theme.fsMicro
                    topPadding: 1
                    elide: Text.ElideRight
                }
                Text {
                    id: body
                    textFormat: Text.PlainText
                    text: line.text
                    width: list.width - 20 - 62 - 50 - 40 - (line.level === "info" || line.level === "debug" ? 0 : 24) - 16
                    wrapMode: line.open ? Text.WrapAnywhere : Text.NoWrap
                    elide: line.open ? Text.ElideNone : Text.ElideRight
                    maximumLineCount: line.open ? 1000 : 1
                    color: root.levelColor(line.level)
                    font.family: Theme.mono
                    font.pixelSize: Theme.fsSmall
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line; opacity: 0.5 }
            TapHandler {
                id: tap
                // the handler's point knows the modifiers; the event point that tapped() passes does not
                onTapped: { list.forceActiveFocus(); root.choose(line.index, tap.point.modifiers & Qt.ControlModifier, tap.point.modifiers & Qt.ShiftModifier) }
                onDoubleTapped: line.opened = !line.opened
            }
        }
        Text {
            textFormat: Text.PlainText
            visible: list.count === 0
            anchors.centerIn: parent
            text: root.view.total > 0 ? qsTr("No line matches. Change the level, the side or the search.") : qsTr("Nothing has been logged yet.")
            color: Theme.text3
            font.family: Theme.ui
            font.pixelSize: Theme.fsSmall
        }
    }
}
