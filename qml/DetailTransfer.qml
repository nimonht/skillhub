import QtQuick
import QtQuick.Controls.Basic
import "."

// Move your skills to another machine: export one file, import it there, and the app fetches everything.
Flickable {
    id: root
    property bool dialogsOk: dialogs.status === Loader.Ready
    property bool includeLocal: true
    contentWidth: width
    contentHeight: col.height + 40
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: Scroll {}

    Loader {   // native dialogs when the module exists, otherwise typed paths
        id: dialogs
        source: "FileDialogs.qml"
        onLoaded: { item.startFolder = app.documentsFolder; item.openChosen.connect(function (p) { importPath.text = p; app.importPreview(p) }); item.saveChosen.connect(function (p) { exportPath.text = p; app.exportTo(p, root.includeLocal) }) }
    }

    component Step: Row {
        property string label
        spacing: 8
        Text { text: label; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
    }

    Column {
        id: col
        x: 24
        y: 24
        width: root.width - 48 - 6
        spacing: 18

        Column {
            width: parent.width
            spacing: 10
            Text { width: parent.width; text: "Transfer"; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Take your whole skill set to another computer. Export writes one file; import on the other machine installs every skill from it: GitHub skills are downloaded again, local skills come out of the file."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; lineHeight: 1.2
            }
        }

        // ---- export
        Column {
            width: parent.width
            spacing: 10
            SectionLabel { width: parent.width; text: "Export"; trailing: app.exportInfo.tracked + app.exportInfo.local + " skills" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: app.exportInfo.tracked + " come from GitHub (only their source is saved) and " + app.exportInfo.local + " are local."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody
            }
            Toggle {
                width: parent.width
                text: "Include local skills"
                hint: "Their files are embedded in the export"
                checked: root.includeLocal
                onToggled: root.includeLocal = checked
            }
            Field { id: exportPath; width: parent.width; iconName: "export"; placeholderText: "Where to save"; text: app.defaultExportPath }
            Row {
                spacing: 8
                Btn { kind: "primary"; iconName: "export"; text: "Export"; enabled: exportPath.text.trim() !== "" && !app.busy; onClicked: app.exportTo(exportPath.text, root.includeLocal) }
                Btn { visible: root.dialogsOk; kind: "ghost"; iconName: "folder"; text: "Choose location"; onClicked: dialogs.item.saveFile("skillhub-skills.json") }
            }
            Text {
                visible: root.includeLocal && app.exportInfo.local > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: "Local skills are copied into the file as they are. Share it only with people you trust."
                color: Theme.notifyText; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
            }
        }

        // ---- import
        Column {
            width: parent.width
            spacing: 10
            SectionLabel { width: parent.width; text: "Import" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Choose an export file, or drop one on this window. You see what it contains before anything is installed."
                color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody
            }
            Field { id: importPath; width: parent.width; iconName: "import"; placeholderText: "Path of an export file"; onAccepted: app.importPreview(text) }
            Row {
                spacing: 8
                Btn { visible: root.dialogsOk; kind: "primary"; iconName: "folder"; text: "Choose file"; onClicked: dialogs.item.openFile() }
                Btn { kind: root.dialogsOk ? "secondary" : "primary"; iconName: "search"; text: "Read file"; enabled: importPath.text.trim() !== "" && !app.busy; onClicked: app.importPreview(importPath.text) }
            }
            Text { visible: app.importError !== ""; width: parent.width; wrapMode: Text.Wrap; text: app.importError; color: Theme.dangerText; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
        }

        // ---- preview
        Column {
            id: preview
            visible: app.importItems.length > 0
            width: parent.width
            spacing: 6
            property var chosen: ({})
            property int chosenCount: Object.keys(chosen).length
            function reset() {
                var c = {}
                for (var i = 0; i < app.importItems.length; ++i) if (app.importItems[i].installable) c[app.importItems[i].name] = true
                chosen = c
            }
            Connections { target: app; function onImportChanged() { preview.reset() } }
            SectionLabel { width: parent.width; text: "In this file"; trailing: app.importItems.length }
            Repeater {
                model: app.importItems
                delegate: AbstractButton {
                    id: it
                    required property var modelData
                    readonly property bool on: preview.chosen[modelData.name] === true
                    width: preview.width
                    height: 54
                    hoverEnabled: true
                    focusPolicy: modelData.installable ? Qt.StrongFocus : Qt.NoFocus
                    enabled: modelData.installable
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: modelData.name
                    Accessible.checked: on
                    onClicked: { var c = Object.assign({}, preview.chosen); if (c[modelData.name]) delete c[modelData.name]; else c[modelData.name] = true; preview.chosen = c }
                    background: Rectangle { color: it.on ? Theme.bg4 : it.hovered && it.enabled ? Theme.bg3 : "transparent"; border.color: Theme.focus; border.width: it.visualFocus ? 2 : 0 }
                    contentItem: Item {
                        Rectangle {
                            x: 8; anchors.verticalCenter: parent.verticalCenter
                            width: 20; height: 20; radius: 4
                            visible: modelData.installable
                            color: it.on ? Theme.mark : "transparent"
                            border.color: it.on ? Theme.mark : Theme.lineInput
                            border.width: 1.5
                            Icon { visible: it.on; anchors.centerIn: parent; name: "check"; size: 14; weight: 2.6; color: Theme.dark ? "#04251A" : "#FFFFFF" }
                        }
                        Icon { visible: !modelData.installable; x: 8; anchors.verticalCenter: parent.verticalCenter; size: 20; name: modelData.problem !== "" ? "alert" : "check"; color: modelData.problem !== "" ? Theme.dangerText : Theme.text3 }
                        Column {
                            x: 42
                            width: parent.width - 42 - 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text { width: parent.width; text: modelData.name; elide: Text.ElideRight; color: modelData.installable ? Theme.text : Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: modelData.problem !== "" ? "Rejected: " + modelData.problem
                                      : modelData.exists ? "Already installed, kept as it is"
                                      : modelData.local ? "Local skill, " + modelData.files + " files from the export"
                                      : "From " + modelData.source
                                color: modelData.problem !== "" ? Theme.dangerText : Theme.text2
                                font.family: Theme.ui; font.pixelSize: Theme.fsSmall
                            }
                        }
                    }
                    HoverHandler { cursorShape: it.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor; enabled: !app.noHover }
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Skills are instructions your agents follow with your permissions: install only files you trust. GitHub skills arrive at their latest version, not necessarily the one on the other machine."
                color: Theme.text3; font.family: Theme.ui; font.pixelSize: Theme.fsSmall
            }
            Row {
                spacing: 8
                Btn {
                    kind: "primary"; iconName: "download"
                    text: preview.chosenCount > 0 ? "Install " + preview.chosenCount + (preview.chosenCount === 1 ? " skill" : " skills") : "Install"
                    enabled: preview.chosenCount > 0 && !app.busy
                    onClicked: app.importInstall(Object.keys(preview.chosen))
                }
                Btn { kind: "ghost"; text: "Cancel"; onClicked: app.importClear() }
            }
        }
    }
}
