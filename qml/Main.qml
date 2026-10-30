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
    property bool settingsOpen: false
    property bool dockChecked: false
    readonly property real centerMin: 140 + 36 + Theme.statusW + app.columnCount * app.portW + Theme.padR
    // docked as long as the center keeps its minimum; otherwise the panel floats over it
    readonly property bool overlayDetail: shell.width - Math.max(300, detail.SplitView.preferredWidth) - 1 < centerMin
    readonly property bool detailOpen: !overlayDetail || app.selected !== "" || app.detailMode !== "skill"
    readonly property bool bandCompact: Theme.setting("layout", "bandCompact", false)
    readonly property bool typing: activeFocusItem && activeFocusItem.hasOwnProperty("cursorPosition")

    // every panel resizes: Rail | (top bar over (Center | Detail) over the dock) | footer
    SplitView {
        id: outer
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: PaneHandle {}
        onResizingChanged: if (!resizing) win.saveLayout()

        Rail {
            id: rail
            SplitView.preferredWidth: Theme.railW
            SplitView.minimumWidth: 56
            SplitView.maximumWidth: 360
            onSettingsRequested: win.settingsOpen = true
        }

        Item {
            id: shell
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320

            TopBar {
                id: top
                width: parent.width
                height: 64
                onEscaped: list.forceActiveFocus()
            }

            SplitView {
                id: vertical
                anchors.top: top.bottom
                anchors.bottom: statusBar.top
                width: parent.width
                orientation: Qt.Vertical
                handle: PaneHandle {}
                onResizingChanged: if (!resizing) win.saveLayout()

                SplitView {
                    id: inner
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 200
                    orientation: Qt.Horizontal
                    handle: PaneHandle {}
                    onResizingChanged: if (!resizing) win.saveLayout()

                Item {
                    id: center
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: win.centerMin

                    Rectangle { anchors.fill: parent; color: Theme.bg2 }
                    ProviderBand { id: band; width: parent.width; height: win.bandCompact ? compactHeight : implicitHeight }
                    MouseArea {   // the band's lower edge: drag it and the band snaps between full height and tiles only
                        y: band.height - 3
                        width: parent.width
                        height: 7
                        z: 3
                        cursorShape: Qt.SizeVerCursor
                        property real from: 0
                        onPressed: (e) => from = e.y
                        onPositionChanged: (e) => { if (Math.abs(e.y - from) > 24) app.setSetting("layout", "bandCompact", e.y < from) }
                    }
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

                    DetailPanel {
                        id: detail
                        visible: !win.overlayDetail
                        SplitView.preferredWidth: Theme.detailW
                        SplitView.minimumWidth: 300
                        SplitView.maximumWidth: Math.max(300, inner.width / 2)
                    }
                }

                ConsoleFeed {
                    id: activity
                    visible: win.consoleOpen
                    SplitView.preferredHeight: 164
                    SplitView.minimumHeight: 96
                    SplitView.maximumHeight: Math.max(96, vertical.height - 200)
                }
            }

            Loader {   // too narrow to dock: the same panel, floating above the list
                active: win.overlayDetail && win.detailOpen
                anchors.right: parent.right
                anchors.top: top.bottom
                anchors.bottom: statusBar.top
                width: Math.min(430, parent.width - 48)
                z: 5
                sourceComponent: DetailPanel {
                    showClose: true
                    onCloseRequested: app.dismiss()
                }
            }

            ReviewPage {
                anchors.top: top.bottom
                anchors.bottom: statusBar.top
                width: parent.width
                visible: app.reviewOpen
                z: 10
            }

            SettingsPage {
                id: settings
                anchors.top: top.bottom
                anchors.bottom: statusBar.top
                width: parent.width
                visible: win.settingsOpen
                z: 11
                onCloseRequested: win.settingsOpen = false
                onResetLayout: win.resetLayout()
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
    }

    readonly property string look: Theme.material + (Theme.dark ? "/dark" : "/light")
    onLookChanged: app.applyMaterial(win, Theme.dark)

    function saveLayout() {
        app.saveLayout("outer", outer.saveState())
        app.saveLayout("vertical", vertical.saveState())
        app.saveLayout("inner", inner.saveState())
    }
    function resetLayout() {
        rail.SplitView.preferredWidth = Theme.railW
        detail.SplitView.preferredWidth = Theme.detailW
        activity.SplitView.preferredHeight = 164
        app.saveLayout("outer", undefined)
        app.saveLayout("vertical", undefined)
        app.saveLayout("inner", undefined)
    }
    function inDetail() {
        for (var i = win.activeFocusItem; i; i = i.parent) if (i === detail) return true
        return false
    }
    function nudgeSide(step) {
        if (inDetail()) detail.SplitView.preferredWidth = Math.max(300, Math.min(inner.width / 2, detail.SplitView.preferredWidth - step))
        else rail.SplitView.preferredWidth = Math.max(56, Math.min(360, rail.SplitView.preferredWidth + step))
        saveLayout()
    }
    function nudgeDock(step) {
        activity.SplitView.preferredHeight = Math.max(96, Math.min(vertical.height - 200, activity.SplitView.preferredHeight + step))
        saveLayout()
    }
    Component.onCompleted: {
        app.applyMaterial(win, Theme.dark)
        if (app.layoutState("outer")) outer.restoreState(app.layoutState("outer"))
        if (app.layoutState("vertical")) vertical.restoreState(app.layoutState("vertical"))
        if (app.layoutState("inner")) inner.restoreState(app.layoutState("inner"))
        if (typeof startLayout !== "undefined" && startLayout !== "") {   // screenshots: rail=64,detail=460,dock=260
            var sizes = startLayout.split(",")
            for (var i = 0; i < sizes.length; ++i) {
                var kv = sizes[i].split("=")
                if (kv[0] === "rail") rail.SplitView.preferredWidth = Number(kv[1])
                if (kv[0] === "detail") detail.SplitView.preferredWidth = Number(kv[1])
                if (kv[0] === "dock") { activity.SplitView.preferredHeight = Number(kv[1]); win.consoleOpen = true }
            }
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
        behind: outer
        x: rail.width + (shell.width - width) / 2
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 54 + (win.consoleOpen ? activity.height : 0)
        z: 20
        onActed: app.undoLast()
    }
    Connections {
        target: app
        function onShowSettings(section) { win.settingsOpen = true }
        function onSettingsChanged() {   // once, when the settings first arrive
            if (win.dockChecked) return
            win.dockChecked = true
            if (Theme.setting("layout", "dockAtStart", false)) win.consoleOpen = true
        }
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
    Shortcut { sequence: "Ctrl+,"; onActivated: win.settingsOpen = !win.settingsOpen }
    Shortcut { sequence: "Ctrl+Alt+Left"; onActivated: win.nudgeSide(-24) }
    Shortcut { sequence: "Ctrl+Alt+Right"; onActivated: win.nudgeSide(24) }
    Shortcut { sequence: "Ctrl+Alt+Up"; onActivated: win.nudgeDock(24) }
    Shortcut { sequence: "Ctrl+Alt+Down"; onActivated: win.nudgeDock(-24) }
    Shortcut { sequence: "Ctrl+Alt+0"; onActivated: win.resetLayout() }
    Shortcut { sequence: "Escape"; enabled: !win.typing; onActivated: { if (win.settingsOpen) win.settingsOpen = false; else if (app.reviewOpen) app.closeReview(); else if (app.detailMode === "add") app.closeAdd(); else app.dismiss() } }
}
