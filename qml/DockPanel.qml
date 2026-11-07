import QtQuick
import QtQuick.Controls.Basic
import "."

// The bottom dock: the activity log, and terminals as tabs.
Rectangle {
    id: root
    color: Theme.bg1
    GlassEdge { anchors.fill: parent }   // over the system's Liquid Glass: a lit rim
    clip: true
    property string tab: "activity"
    property int nextSession: 1   // what tells sessions apart, never used twice; the number on a tab is another thing
    function showTerminal() {
        if (sessions.count === 0) addSession()
        tab = "t" + sessions.get(sessions.count - 1).sid
    }
    function freeNumber() {   // the lowest one no open terminal shows: tabs that are open keep theirs, and closing them all starts again at 1
        const taken = []
        for (let i = 0; i < sessions.count; i++) taken.push(sessions.get(i).num)
        let n = 1
        while (taken.includes(n)) n += 1
        return n
    }
    function addSession(folder, command) {   // both optional: a folder to start in, and a command typed but not run
        const num = freeNumber()
        sessions.append({ sid: nextSession, num: num, name: qsTr("Terminal %1").arg(num), folder: folder || "", command: command || "" })
        nextSession += 1
        tab = "t" + (nextSession - 1)
    }
    function closeSession(index) {
        sessions.remove(index)
        tab = sessions.count > 0 ? "t" + sessions.get(sessions.count - 1).sid : "activity"
    }
    function sessionNames() {
        const names = []
        for (let i = 0; i < sessions.count; i++) names.push(sessions.get(i).name)
        return names
    }
    readonly property bool onTerminal: tab !== "activity" && tab !== "log"
    function activeSession() {   // the terminal that is shown
        for (let i = 0; i < stack.count; i++) {
            const it = stack.itemAt(i)
            if (it && it.visible) return it
        }
        return null
    }
    function sessionItem() { return activeSession() || (stack.count > 0 ? stack.itemAt(stack.count - 1) : null) }
    // The keyboard goes to the shell whenever its tab is shown: switching tabs, opening the dock, or starting a terminal.
    function focusActive() { Qt.callLater(function () { const s = root.activeSession(); if (s) s.focusShell() }) }
    onTabChanged: if (onTerminal) focusActive()
    ListModel { id: sessions }

    Item {
        id: bar
        width: parent.width
        height: 34
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        component Tab: AbstractButton {
            id: t
            property bool on: false
            property bool closable: false
            signal closed()
            height: bar.height
            implicitWidth: label.implicitWidth + (closable ? 44 : 24)
            hoverEnabled: true
            focusPolicy: Qt.TabFocus   // a click must not take the keyboard from the terminal
            Accessible.role: Accessible.PageTab
            Accessible.name: text
            background: Rectangle { color: t.on ? Theme.bg3 : t.hovered ? Theme.bg2 : "transparent"; border.color: Theme.focus; border.width: t.visualFocus ? 2 : 0 }
            contentItem: Item {
                Text { textFormat: Text.PlainText; id: label; x: 12; anchors.verticalCenter: parent.verticalCenter; text: t.text; color: t.on ? Theme.text : Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
                Icon { visible: t.closable; x: label.x + label.width + 8; anchors.verticalCenter: parent.verticalCenter; name: "x"; size: 12; color: Theme.text2; TapHandler { onTapped: t.closed() } }
            }
        }
        Row {
            height: parent.height
            Tab { text: qsTr("Activity"); on: root.tab === "activity"; onClicked: root.tab = "activity" }
            Tab { text: qsTr("Log"); on: root.tab === "log"; onClicked: root.tab = "log" }
            Repeater {
                model: sessions
                delegate: Tab {
                    required property int sid
                    required property string name
                    required property int index
                    text: name
                    closable: true
                    on: root.tab === "t" + sid
                    onClicked: root.tab = "t" + sid
                    onClosed: root.closeSession(index)
                }
            }
            Btn { kind: "ghost"; compact: true; iconName: "plus"; text: ""; focusPolicy: Qt.TabFocus; Accessible.name: qsTr("New terminal"); anchors.verticalCenter: parent.verticalCenter; onClicked: root.addSession() }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Repeater {
                model: root.onTerminal ? app.externalTerminals.slice(0, 3) : []
                delegate: Btn { required property var modelData; kind: "ghost"; compact: true; text: qsTr("Open in %1").arg(modelData.label); onClicked: app.openExternalTerminal(modelData.id) }
            }
        }
    }
    ConsoleFeed {
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        visible: root.tab === "activity"
    }
    LogViewer {
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        visible: root.tab === "log"
    }
    Repeater {
        id: stack
        model: sessions
        delegate: TerminalSession {
            required property int sid
            required property string folder
            required property string command
            startIn: folder
            typed: command
            anchors.fill: parent
            anchors.topMargin: bar.height
            visible: root.tab === "t" + sid
        }
    }
}
