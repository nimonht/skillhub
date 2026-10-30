import QtQuick
import QtQuick.Controls.Basic
import "."

// The bottom dock: the activity log, and terminals as tabs.
Rectangle {
    id: root
    color: Theme.bg1
    clip: true
    property string tab: "activity"
    property int nextSession: 1
    function showTerminal() {
        if (sessions.count === 0) addSession()
        tab = "t" + sessions.get(sessions.count - 1).sid
    }
    function addSession() {
        sessions.append({ sid: nextSession, name: "Terminal " + nextSession })
        nextSession += 1
        tab = "t" + (nextSession - 1)
    }
    function sessionItem() { return stack.count > 0 ? stack.itemAt(stack.count - 1) : null }
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
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.PageTab
            Accessible.name: text
            background: Rectangle { color: t.on ? Theme.bg3 : t.hovered ? Theme.bg2 : "transparent"; border.color: Theme.focus; border.width: t.visualFocus ? 2 : 0 }
            contentItem: Item {
                Text { id: label; x: 12; anchors.verticalCenter: parent.verticalCenter; text: t.text; color: t.on ? Theme.text : Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
                Icon { visible: t.closable; x: label.x + label.width + 8; anchors.verticalCenter: parent.verticalCenter; name: "x"; size: 12; color: Theme.text2; TapHandler { onTapped: t.closed() } }
            }
        }
        Row {
            height: parent.height
            Tab { text: "Activity"; on: root.tab === "activity"; onClicked: root.tab = "activity" }
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
                    onClosed: { sessions.remove(index); root.tab = sessions.count > 0 ? "t" + sessions.get(sessions.count - 1).sid : "activity" }
                }
            }
            Btn { kind: "ghost"; compact: true; iconName: "plus"; text: ""; Accessible.name: "New terminal"; anchors.verticalCenter: parent.verticalCenter; onClicked: root.addSession() }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Repeater {
                model: root.tab !== "activity" ? app.externalTerminals.slice(0, 3) : []
                delegate: Btn { required property var modelData; kind: "ghost"; compact: true; text: "Open in " + modelData.label; onClicked: app.openExternalTerminal(modelData.id) }
            }
        }
    }
    ConsoleFeed {
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        visible: root.tab === "activity"
    }
    Repeater {
        id: stack
        model: sessions
        delegate: TerminalSession {
            required property int sid
            anchors.fill: parent
            anchors.topMargin: bar.height
            visible: root.tab === "t" + sid
        }
    }
}
