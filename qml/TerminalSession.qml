import QtQuick
import SkillHub.Terminal
import "."

// One shell in the dock. The banner is written into the screen before the shell starts, so it never lands in its history.
Rectangle {
    id: root
    color: term.background
    property bool started: false
    property string startIn: ""   // the folder the shell starts in, "" for the usual one
    property string typed: ""     // typed once the shell is up, without Enter: the user starts it
    property alias title: term.title
    property alias running: term.running
    objectName: "terminalSession"
    function typeText(text) { term.type(text) }
    function screenText() { return term.screenText() }
    function focusShell() { term.forceActiveFocus() }
    readonly property var look: { void app.settingsRev; return app.terminalLook() }

    onVisibleChanged: begin()
    onWidthChanged: begin()
    Component.onCompleted: begin()
    function begin() {
        if (!visible || started || width <= 0) return
        started = true
        // ConPTY clears the screen as it starts, so on Windows the shell itself prints the banner
        if (app.terminalGreeting() === "skillhub" && Qt.platform.os !== "windows") {
            app.requestBanner()
            late.start()
        } else {
            go()
        }
    }
    Timer { id: late; interval: 2500; onTriggered: if (!term.running) root.go() }   // no banner, then: the shell matters more
    function go() {
        const start = app.terminalStart()
        term.program = start.program
        term.arguments = start.arguments
        term.environment = start.environment
        term.workingDirectory = root.startIn !== "" ? root.startIn : app.terminalFolder()
        term.start()
        term.forceActiveFocus()
        if (root.typed !== "") typeLater.start()
    }
    Timer { id: typeLater; interval: 900; onTriggered: term.type(root.typed) }
    Connections {
        target: app
        function onBannerReady(text) {
            if (root.started && !term.running) {
                late.stop()
                term.feed(text)
                root.go()
            }
        }
    }

    VTermItem {
        id: term
        anchors.fill: parent
        anchors.margins: 6
        fontFamily: Theme.setting("terminal", "fontFamily", "") || root.look.fontFamily || ""
        fontSize: Theme.setting("terminal", "fontSize", 0) || root.look.fontSize || 14
        foreground: root.look.foreground || (Theme.dark ? "#E8ECF1" : "#11161C")
        background: root.look.background || (Theme.dark ? "#10141A" : "#F5F6F7")
        cursorColor: Theme.mark
        selectionColor: Theme.markLow
        cursorShape: Theme.setting("terminal", "cursor", "block")
        scrollback: Theme.setting("terminal", "scrollback", 10000)
        colorTable: root.look.palette || []
    }
}
