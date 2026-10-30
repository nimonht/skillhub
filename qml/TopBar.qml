import QtQuick
import "."

// Search plus the four actions that matter. Labels drop to icons when the window gets narrow.
Item {
    id: root
    implicitHeight: 64
    readonly property bool wide: width > 1000
    readonly property int attention: app.problemCount + app.strayCount + (app.wiringOk ? 0 : 1)
    property alias search: searchField
    signal escaped()
    function focusSearch() { searchField.forceActiveFocus(); searchField.selectAll() }

    Rectangle { anchors.fill: parent; color: Theme.bg2 }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    Field {
        id: searchField
        x: 20
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(160, Math.min(380, root.width - 20 - actions.width - 40))
        iconName: "search"
        placeholderText: "Filter skills   /"
        text: app.filter.text
        onTextChanged: app.filter.text = text
        Keys.onEscapePressed: { text = ""; root.escaped() }
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        Btn {
            visible: root.attention > 0
            iconName: "tool"
            text: root.wide ? "Repair  " + root.attention : ""
            Accessible.name: "Repair " + root.attention + " link problems"
            enabled: !app.busy
            onClicked: { app.focusAgent = ""; app.adopt() }
        }
        Btn { iconName: "plus"; text: root.wide ? "Add skill" : ""; Accessible.name: "Add skill from GitHub"; onClicked: app.detailMode = "add" }
        Btn { iconName: "refresh"; text: root.wide ? "Check" : ""; Accessible.name: "Check for updates"; enabled: !app.busy; onClicked: app.checkUpdates() }
        Btn {
            kind: "primary"
            iconName: app.updateCount > 0 ? "upgrade" : "check"
            text: root.wide ? (app.updateCount > 0 ? "Update all  " + app.updateCount : "Up to date") : (app.updateCount > 0 ? "" + app.updateCount : "")
            Accessible.name: app.updateCount > 0 ? "Update all " + app.updateCount + " skills" : "Everything is up to date"
            enabled: app.updateCount > 0 && !app.busy
            onClicked: app.setting("updates", "reviewBeforeUpdate", true) ? app.reviewAll() : app.updateAll()
        }
    }
}
