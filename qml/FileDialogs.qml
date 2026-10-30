import QtQuick
import QtQuick.Dialogs

// Native file dialogs, loaded on demand: if the module is missing the panels fall back to typing a path.
Item {
    id: root
    signal openChosen(string path)
    signal saveChosen(string path)
    signal folderChosen(string path)
    property string startFolder: ""
    function toPath(u) { return decodeURIComponent(u.toString().replace(/^file:\/\/\/([A-Za-z]:)/, "$1").replace(/^file:\/\//, "")) }
    function openFile() { openDlg.open() }
    function openFolder() { folderDlg.open() }
    function saveFile(name) { saveDlg.currentFile = "file://" + root.startFolder + "/" + name; saveDlg.open() }

    FileDialog {
        id: openDlg
        title: "Choose a Skill Hub export"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Skill Hub exports (*.json)", "All files (*)"]
        onAccepted: root.openChosen(root.toPath(selectedFile))
    }
    FileDialog {
        id: saveDlg
        title: "Export skills to"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["Skill Hub exports (*.json)"]
        onAccepted: root.saveChosen(root.toPath(selectedFile))
    }
    FolderDialog {
        id: folderDlg
        title: "Choose a folder of skills"
        onAccepted: root.folderChosen(root.toPath(selectedFolder))
    }
}
