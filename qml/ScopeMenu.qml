import QtQuick
import QtQuick.Controls.Basic
import "."

// Whose skills the window shows: your own, or a project's. The list, the agents and the Add sheet all follow it.
AbstractButton {
    id: root
    property bool compact: false
    implicitHeight: 40
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    enabled: !app.busy
    onClicked: pop.open()
    Accessible.role: Accessible.ComboBox
    Accessible.name: "Skills of " + app.scopeLabel
    Connections { target: app; function onShowScopeMenu() { pop.open() } }
    Loader {   // native dialogs when the module exists; without it projects are added with skillhub projects add
        id: dialogs
        source: "FileDialogs.qml"
        onLoaded: item.folderChosen.connect(function (p) { app.addProject(p) })
    }

    background: Rectangle { radius: 4; color: root.hovered ? Theme.bg3 : Theme.bg2; border.color: Theme.focus; border.width: root.visualFocus ? 2 : 0 }
    contentItem: Item {
        Icon { x: root.compact ? (parent.width - width) / 2 : 10; anchors.verticalCenter: parent.verticalCenter; name: app.inProject ? "branch" : "all"; size: 16; color: Theme.markText }
        Text {
            visible: !root.compact
            x: 34
            width: parent.width - x - 28
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: app.scopeLabel
            color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold
        }
        Icon { visible: !root.compact; anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; name: "down"; size: 14; color: Theme.text2 }
    }

    Popup {
        id: pop
        y: root.height + 4
        width: Math.max(root.width, 280)
        padding: 6
        focus: true
        background: Rectangle { radius: 4; color: Theme.bg2; border.color: Theme.line; border.width: 1 }
        onOpened: app.loadProjects()

        component Choice: AbstractButton {
            id: choice
            property string detail: ""
            property string iconName: "branch"
            property bool current: false
            width: parent ? parent.width : 200
            height: detail !== "" ? 44 : 34
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.MenuItem
            Accessible.name: text
            background: Rectangle { radius: 3; color: choice.current ? Theme.bg4 : choice.hovered ? Theme.bg3 : "transparent"; border.color: Theme.focus; border.width: choice.visualFocus ? 2 : 0 }
            contentItem: Item {
                Icon { x: 8; anchors.verticalCenter: parent.verticalCenter; name: choice.iconName; size: 16; color: choice.current ? Theme.markText : Theme.text2 }
                Column {
                    x: 32
                    width: parent.width - x - 8
                    anchors.verticalCenter: parent.verticalCenter
                    Text { width: parent.width; elide: Text.ElideRight; text: choice.text; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                    Text { visible: choice.detail !== ""; width: parent.width; elide: Text.ElideMiddle; text: choice.detail; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                }
            }
        }

        contentItem: Column {
            spacing: 2
            Choice { text: "Your skills"; iconName: "all"; current: !app.inProject; onClicked: { pop.close(); app.setScope("") } }
            Repeater {
                model: app.projects
                delegate: Choice {
                    required property var modelData
                    text: modelData.name + "  ·  " + modelData.skills
                    detail: modelData.path
                    current: app.scope === modelData.path
                    onClicked: { pop.close(); app.setScope(modelData.path) }
                }
            }
            Repeater {
                model: app.suggestedProjects
                delegate: Choice {
                    required property string modelData
                    text: "Add " + modelData.split("/").pop()
                    detail: modelData
                    iconName: "plus"
                    onClicked: { pop.close(); app.addProject(modelData) }
                }
            }
            Choice {
                visible: dialogs.status === Loader.Ready
                text: "Add a project"
                iconName: "folder"
                onClicked: { pop.close(); dialogs.item.openFolder() }
            }
        }
    }
}
