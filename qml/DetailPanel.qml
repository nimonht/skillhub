import QtQuick
import "."

// Right panel: switches between the selected skill, an inspected agent, the add sheet and the empty state.
Rectangle {
    id: root
    color: Theme.bg1
    signal closeRequested()
    property bool showClose: false
    readonly property string view: app.detailMode === "add" ? "add"
                                 : app.detailMode === "providers" ? "providers"
                                 : app.detailMode === "transfer" ? "transfer"
                                 : app.detailMode === "agent" && app.focusAgent !== "" ? "agent"
                                 : app.selected !== "" ? "skill" : "empty"

    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.line }

    EmptyDetail { anchors.fill: parent; visible: root.view === "empty" }
    DetailSkill { anchors.fill: parent; anchors.leftMargin: 1; visible: root.view === "skill" }
    DetailAgent { anchors.fill: parent; anchors.leftMargin: 1; visible: root.view === "agent" }
    AddSheet { anchors.fill: parent; anchors.leftMargin: 1; visible: root.view === "add" }
    DetailProviders { anchors.fill: parent; anchors.leftMargin: 1; visible: root.view === "providers" }
    DetailTransfer { anchors.fill: parent; anchors.leftMargin: 1; visible: root.view === "transfer" }

    Btn {
        visible: root.showClose
        kind: "ghost"; compact: true; iconName: "x"
        anchors.top: parent.top; anchors.right: parent.right
        anchors.margins: 10
        onClicked: root.closeRequested()
        Accessible.name: "Close panel"
    }
}
