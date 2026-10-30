import QtQuick
import QtQuick.Controls.Basic
import "."

ApplicationWindow {
    id: win
    visible: true
    width: typeof startSize !== "undefined" ? startSize.width : 1280
    height: typeof startSize !== "undefined" ? startSize.height : 800
    minimumWidth: 820
    minimumHeight: 560
    title: "Skill Hub"
    color: Theme.bg0
    font.family: Theme.ui
    font.pixelSize: Theme.fsBody

    property bool consoleOpen: false
    readonly property bool overlayDetail: width < 1140
    readonly property bool detailOpen: !overlayDetail || app.selected !== "" || app.detailMode !== "skill"
    readonly property bool typing: activeFocusItem && activeFocusItem.hasOwnProperty("cursorPosition")

    Rail {
        id: rail
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.railW
    }

    Item {
        id: shell
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        TopBar {
            id: top
            width: parent.width
            height: 64
            onEscaped: list.forceActiveFocus()
        }

        Item {
            id: work
            anchors.top: top.bottom
            anchors.bottom: activity.top
            width: parent.width
            clip: true

            Item {
                id: center
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width - (win.overlayDetail ? 0 : Theme.detailW)

                Rectangle { anchors.fill: parent; color: Theme.bg2 }
                ProviderBand { id: band; width: parent.width; height: implicitHeight }
                ListHeader { id: head; anchors.top: band.bottom; width: parent.width; height: 36 }

                ListView {
                    id: list
                    anchors.top: head.bottom
                    anchors.bottom: parent.bottom
                    width: parent.width
                    clip: true
                    model: app.skills
                    currentIndex: -1
                    keyNavigationEnabled: true
                    activeFocusOnTab: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: Scroll {}
                    Accessible.role: Accessible.List
                    Accessible.name: "Skills"
                    delegate: SkillRow {
                        onActivated: (i) => { list.currentIndex = i; list.forceActiveFocus(); app.select(app.filter.nameAt(i)) }
                    }
                    onCurrentIndexChanged: if (currentIndex >= 0 && activeFocus) app.select(app.filter.nameAt(currentIndex))
                    Keys.onReturnPressed: if (currentIndex >= 0) app.select(app.filter.nameAt(currentIndex))
                    function sync() {
                        var r = app.selected !== "" ? app.filter.rowOf(app.selected) : -1
                        if (r !== currentIndex) currentIndex = r
                        if (r >= 0) positionViewAtIndex(r, ListView.Contain)
                    }
                    Connections { target: app; function onSelectedChanged() { list.sync() } }
                    Connections {
                        target: app.skills
                        function onModelReset() { Qt.callLater(list.sync) }
                        function onLayoutChanged() { Qt.callLater(list.sync) }
                        function onRowsInserted() { Qt.callLater(list.sync) }
                    }
                    Rectangle {   // focus ring for the list as a whole
                        anchors.fill: parent
                        visible: list.activeFocus && list.currentIndex < 0
                        color: "transparent"
                        border.color: Theme.focus
                        border.width: 2
                    }

                    // empty states that say why and what next
                    Column {
                        visible: list.count === 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 56
                        width: Math.min(420, parent.width - 48)
                        spacing: 12
                        readonly property bool none: app.skillCount === 0
                        readonly property bool good: !none && app.filter.mode === "updates" && app.filter.text === ""
                        Icon { anchors.horizontalCenter: parent.horizontalCenter; name: parent.good ? "check" : parent.none ? "folder" : "search"; size: 30; color: parent.good ? Theme.markText : Theme.text3 }
                        Text {
                            width: parent.width; horizontalAlignment: Text.AlignHCenter
                            text: parent.none ? "The shared folder is empty" : parent.good ? "Everything is current" : "No skill matches"
                            color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold
                        }
                        Text {
                            width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                            text: parent.none ? "Install skills from a repository or a folder, or drop a folder with a SKILL.md into " + app.centralPath + "."
                                : parent.good ? "Every tracked skill matches its upstream. Check again any time."
                                : "Nothing in this view matches the current filter."
                            color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; lineHeight: 1.2
                        }
                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 8
                            Btn { visible: parent.parent.none; kind: "primary"; iconName: "plus"; text: "Add skills"; onClicked: app.detailMode = "add" }
                            Btn { visible: !parent.parent.none && (app.filter.mode !== "all" || app.filter.text !== ""); iconName: "x"; text: "Clear filter"; onClicked: { app.filter.mode = "all"; app.filter.text = "" } }
                        }
                    }
                }
            }

            ReviewPage {
                anchors.fill: parent
                visible: app.reviewOpen
                z: 10
            }

            DetailPanel {
                id: detail
                visible: win.detailOpen
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: win.overlayDetail ? Math.min(430, parent.width - 72) : Theme.detailW
                z: 5
                showClose: win.overlayDetail
                onCloseRequested: app.dismiss()
            }
        }

        ConsoleFeed {
            id: activity
            anchors.bottom: statusBar.top
            width: parent.width
            open: win.consoleOpen
        }
        StatusBar {
            id: statusBar
            anchors.bottom: parent.bottom
            width: parent.width
            height: 34
            consoleOpen: win.consoleOpen
            onToggleConsole: win.consoleOpen = !win.consoleOpen
        }
    }

    // drop a Skill Hub export anywhere on the window to start an import
    DropArea {
        id: drop
        anchors.fill: parent
        z: 60
        onDropped: (d) => { if (d.hasUrls && d.urls.length > 0) app.importPreview(d.urls[0].toString()) }
        Rectangle {
            anchors.fill: parent
            visible: drop.containsDrag
            color: Qt.alpha(Theme.bg0, 0.86)
            Column {
                anchors.centerIn: parent
                spacing: 10
                Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "import"; size: 36; weight: 1.8; color: Theme.markText }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Drop a Skill Hub export to import it"; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "You see what is inside before anything is installed"; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
            }
        }
    }

    // agents can be installed or removed while the window is open: look again when it comes back to the front
    Timer { id: refocus; interval: 600; onTriggered: if (!app.busy) app.refresh() }
    Connections { target: Qt.application; function onStateChanged() { if (Qt.application.state === Qt.ApplicationActive) refocus.restart() } }

    Toast {
        id: toast
        anchors.horizontalCenter: shell.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 54 + (win.consoleOpen ? 164 : 0)
        z: 20
        onActed: app.undoLast()
    }
    Connections {
        target: app
        function onToast(level, text) { toast.show(level, text) }
        function onUndoOffered(text) { toast.show("ok", text, "Undo") }
    }

    Shortcut { sequence: "/"; enabled: !win.typing; onActivated: top.focusSearch() }
    Shortcut { sequence: "Ctrl+F"; onActivated: top.focusSearch() }
    Shortcut { sequence: "Ctrl+N"; onActivated: app.detailMode = "add" }
    Shortcut { sequence: "Ctrl+Shift+E"; onActivated: app.detailMode = "transfer" }
    Shortcut { sequence: "Ctrl+Shift+A"; onActivated: app.detailMode = "providers" }
    Shortcut { sequence: "Ctrl+U"; onActivated: if (!app.busy) app.checkUpdates() }
    Shortcut { sequence: "Ctrl+Shift+U"; onActivated: if (!app.busy && app.updateCount > 0) app.reviewAll() }
    Shortcut { sequences: ["F5", "Ctrl+R"]; onActivated: app.refresh() }
    Shortcut { sequence: "Ctrl+J"; onActivated: win.consoleOpen = !win.consoleOpen }
    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "Escape"; enabled: !win.typing; onActivated: { if (app.reviewOpen) app.closeReview(); else if (app.detailMode === "add") app.closeAdd(); else app.dismiss() } }
}
