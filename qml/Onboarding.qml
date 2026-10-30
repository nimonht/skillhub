import QtQuick
import QtQuick.Controls.Basic
import "."

// The first-run tour, on real data: your agents, your skills, your colours, then the window itself with a spotlight. Esc leaves at
// any time, the arrows and Enter walk through it, and with reduced motion everything just cross-fades.
FocusScope {
    id: root
    anchors.fill: parent
    focus: true
    signal finished()
    signal openTerminal()
    property int step: 0
    property int coach: 0
    readonly property int steps: 6
    readonly property var marks: [
        { key: "filters", title: "Your skills, sorted by what they need", text: "All skills, updates waiting, skills of your own, and links that need a look.", place: "right" },
        { key: "band", title: "Every agent has a column", text: "Click one to see how it finds skills and which it can see.", place: "below", wait: "agent" },
        { key: "header", title: "One port per skill, per agent", text: "Lit means that agent sees the skill. A ring means something is worth a look.", place: "below" },
        { key: "detail", title: "Review before you update", text: "Pick a skill to read it, see what an update would change, and go back to any kept version.", place: "left" },
        { key: "top", title: "Search and act", text: "Filter with /, add skills from anywhere, and review every update at once.", place: "below" },
        { key: "status", title: "Activity and a terminal", text: "The dock below shows what ran. Ctrl+` opens a terminal right there.", place: "above" },
        { key: "settings", title: "Make it yours", text: "Panels resize, and everything else is in Settings.", place: "right" } ]
    readonly property var mark: step === 4 ? marks[coach] : null
    readonly property Item target: mark ? Theme.tourTargets[mark.key] || null : null
    readonly property rect hole: {
        if (!target || width <= 0) return Qt.rect(0, 0, 0, 0)
        void (target.width + target.height)
        const p = target.mapToItem(root, 0, 0)
        return Qt.rect(p.x - 6, p.y - 6, target.width + 12, target.height + 12)
    }
    readonly property bool coaching: step === 4 && hole.width > 0

    function next() {
        if (step === 4 && coach + 1 < marks.length) { coach += 1; return }
        if (step + 1 < steps) step += 1
        else finished()
    }
    function back() {
        if (step === 4 && coach > 0) { coach -= 1; return }
        if (step > 0) step -= 1
    }
    Keys.onEscapePressed: finished()
    Keys.onRightPressed: next()
    Keys.onLeftPressed: back()
    Keys.onReturnPressed: next()
    Keys.onEnterPressed: next()
    Accessible.role: Accessible.Dialog
    Accessible.name: "Welcome to Skill Hub"

    // a step that waits for the real thing: clicking an agent's column moves on by itself
    Connections {
        target: app
        function onFocusChanged() { if (root.mark && root.mark.wait === "agent" && app.focusAgent !== "") root.next() }
    }

    // the dim: whole for the pages, four pieces around the spotlight for the coach marks
    readonly property color dim: Qt.alpha(Theme.dark ? "#05070A" : "#0E1624", step === 3 ? 0.42 : step === 4 ? 0.62 : 0.95)
    Rectangle { visible: !root.coaching; anchors.fill: parent; color: root.dim }
    Rectangle { visible: root.coaching; width: parent.width; height: root.hole.y; color: root.dim }
    Rectangle { visible: root.coaching; y: root.hole.y + root.hole.height; width: parent.width; height: parent.height - y; color: root.dim }
    Rectangle { visible: root.coaching; y: root.hole.y; width: root.hole.x; height: root.hole.height; color: root.dim }
    Rectangle { visible: root.coaching; x: root.hole.x + root.hole.width; y: root.hole.y; width: parent.width - x; height: root.hole.height; color: root.dim }
    MouseArea {   // everything is covered, except what the spotlight is on
        anchors.fill: parent
        enabled: !root.coaching
    }
    Item {   // corner hooks around the spotlight, as on anything selected in the window
        visible: root.coaching
        x: root.hole.x; y: root.hole.y
        width: root.hole.width; height: root.hole.height
        Behavior on x { NumberAnimation { duration: Theme.dSlow; easing.type: Easing.OutCubic } }
        Behavior on y { NumberAnimation { duration: Theme.dSlow; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: Theme.dSlow; easing.type: Easing.OutCubic } }
        Behavior on height { NumberAnimation { duration: Theme.dSlow; easing.type: Easing.OutCubic } }
        Rectangle { width: 14; height: 2; color: Theme.mark }
        Rectangle { width: 2; height: 14; color: Theme.mark }
        Rectangle { x: parent.width - 14; y: parent.height - 2; width: 14; height: 2; color: Theme.mark }
        Rectangle { x: parent.width - 2; y: parent.height - 14; width: 2; height: 14; color: Theme.mark }
    }

    // ---- the pages
    Item {
        id: stage
        anchors.fill: parent
        opacity: root.step === 4 ? 0 : 1
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.dBase } }

        Canvas {   // a wave through the dot grid, for the welcome only
            id: wave
            anchors.fill: parent
            visible: root.step === 0
            property real phase: 0
            onPhaseChanged: requestPaint()
            onWidthChanged: requestPaint()
            renderStrategy: Canvas.Cooperative
            NumberAnimation on phase { from: 0; to: 6.283; duration: 4200; loops: Animation.Infinite; running: root.step === 0 && !Theme.still }
            onPaint: {
                var c = getContext("2d")
                c.reset()
                c.fillStyle = Theme.mark
                for (var y = 16; y < height; y += 26) {
                    for (var x = 16; x < width; x += 26) {
                        var d = Math.sin(x * 0.012 + y * 0.008 + phase)
                        c.globalAlpha = 0.05 + 0.13 * (d + 1) / 2
                        c.fillRect(x, y, 2 + d, 2 + d)
                    }
                }
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(root.width - 80, 680)
            spacing: 20

            // welcome: the mark puts itself together
            Item {
                visible: root.step === 0
                width: parent.width
                height: 150
                id: markBox
                readonly property real s: 128
                property bool shown: false
                Component.onCompleted: shown = true
                readonly property real k: s / 256
                x: 0
                Item {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: markBox.s; height: markBox.s
                    Rectangle {
                        x: 36 * markBox.k; width: 44 * markBox.k; height: 192 * markBox.k; radius: width / 2; color: "#FFFFFF"
                        y: (markBox.shown ? 32 : 90) * markBox.k
                        opacity: markBox.shown ? 1 : 0
                        Behavior on y { NumberAnimation { duration: Theme.dSlow * 2; easing.type: Easing.OutCubic } }
                        Behavior on opacity { NumberAnimation { duration: Theme.dSlow } }
                    }
                    Rectangle {
                        x: 176 * markBox.k; width: 44 * markBox.k; height: 192 * markBox.k; radius: width / 2; color: "#FFFFFF"
                        y: (markBox.shown ? 32 : 90) * markBox.k
                        opacity: markBox.shown ? 1 : 0
                        Behavior on y { NumberAnimation { duration: Theme.dSlow * 2.4; easing.type: Easing.OutCubic } }
                        Behavior on opacity { NumberAnimation { duration: Theme.dSlow } }
                    }
                    Rectangle {
                        id: bar
                        y: 106 * markBox.k; width: 64 * markBox.k; height: 40 * markBox.k; radius: height / 2; color: Theme.mark
                        x: (markBox.shown ? 96 : -40) * markBox.k
                        Behavior on x { SequentialAnimation { PauseAnimation { duration: Theme.dSlow * 2 } NumberAnimation { duration: Theme.dSlow * 1.5; easing.type: Easing.OutBack } } }
                    }
                    Rectangle {   // the port lights up as it locks
                        anchors.centerIn: bar
                        width: bar.width; height: width; radius: width / 2
                        color: "transparent"
                        border.color: Theme.mark
                        border.width: 2
                        opacity: 0
                        SequentialAnimation on scale {
                            running: markBox.shown && !Theme.still
                            PauseAnimation { duration: Theme.dSlow * 3 }
                            PropertyAction { target: bar; property: "opacity"; value: 1 }
                            NumberAnimation { from: 1; to: 2.6; duration: 700; easing.type: Easing.OutCubic }
                        }
                        SequentialAnimation on opacity {
                            running: markBox.shown && !Theme.still
                            PauseAnimation { duration: Theme.dSlow * 3 }
                            NumberAnimation { from: 0.9; to: 0; duration: 700 }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: ["One skills folder.\nEvery coding agent.", "Your agents", "Your skills", "Make it yours", "", "Ready"][root.step]
                color: "#FFFFFF"
                font.family: Theme.display
                font.pixelSize: root.step === 0 ? 44 : 34
                font.weight: Font.Bold
            }

            // your agents, wired to the shared folder
            Item {
                id: wires
                visible: root.step === 1
                width: parent.width
                height: 300
                readonly property var list: app.agents.slice(0, 7)
                Rectangle {
                    id: hub
                    width: 92; height: 92; radius: 4
                    x: 8; y: (wires.height - height) / 2
                    color: Theme.bg2
                    Icon { anchors.centerIn: parent; name: "folder"; size: 34; color: Theme.mark }
                }
                Canvas {
                    id: lines
                    anchors.fill: parent
                    property real dash: 0
                    onDashChanged: requestPaint()
                    onVisibleChanged: requestPaint()
                    NumberAnimation on dash { from: 0; to: 16; duration: 900; loops: Animation.Infinite; running: wires.visible && !Theme.still }
                    onPaint: {
                        var c = getContext("2d")
                        c.reset()
                        c.strokeStyle = Theme.mark
                        c.lineWidth = 1.5
                        c.setLineDash([6, 10])
                        c.lineDashOffset = -dash
                        for (var i = 0; i < wires.list.length; ++i) {
                            var y = wires.rowY(i) + 18
                            c.beginPath()
                            c.moveTo(100, wires.height / 2)
                            c.bezierCurveTo(170, wires.height / 2, 170, y, 250, y)
                            c.stroke()
                        }
                    }
                }
                function rowY(i) { return (height - list.length * 42) / 2 + i * 42 + 3 }
                Repeater {
                    model: wires.list
                    delegate: Item {
                        required property var modelData
                        required property int index
                        x: 250
                        y: wires.rowY(index)
                        width: wires.width - 250
                        height: 36
                        opacity: wires.visible ? 1 : 0
                        Behavior on opacity { SequentialAnimation { PauseAnimation { duration: Theme.still ? 0 : index * 90 } NumberAnimation { duration: Theme.dSlow } } }
                        AgentTile { size: 36; text: modelData.monogram || ""; icon: modelData.icon; tone: "ok" }
                        Column {
                            x: 48
                            anchors.verticalCenter: parent.verticalCenter
                            Text { text: modelData.label; color: "#FFFFFF"; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
                            Text { text: modelData.detectedBy ? "Found via " + modelData.detectedBy : ""; color: "#C6CEDA"; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        }
                    }
                }
                Text {
                    visible: wires.list.length === 0
                    x: 130; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 130
                    wrapMode: Text.Wrap
                    text: "No coding agent found yet. Install one and it shows up here by itself."
                    color: "#C6CEDA"; font.family: Theme.ui; font.pixelSize: Theme.fsBody
                }
            }

            // your skills, and what the ports say
            Column {
                visible: root.step === 2
                width: parent.width
                spacing: 18
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 40
                    Repeater {
                        model: [ { n: app.skillCount, t: "skills" }, { n: app.updateCount, t: "updates" }, { n: app.problemCount, t: "link problems" }, { n: app.strayCount, t: "strays" } ]
                        delegate: Column {
                            required property var modelData
                            NumText { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.n; color: "#FFFFFF"; font.family: Theme.display; font.pixelSize: 38; font.weight: Font.Bold }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.t; color: "#C6CEDA"; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        }
                    }
                }
                Grid {
                    anchors.horizontalCenter: parent.horizontalCenter
                    columns: 2
                    columnSpacing: 32
                    rowSpacing: 8
                    Repeater {
                        model: [ { l: "link", t: "linked to this agent" }, { l: "off", t: "turned off, on purpose" }, { l: "missing", t: "not linked yet" }, { l: "copy", t: "its own separate copy" } ]
                        delegate: Row {
                            required property var modelData
                            spacing: 10
                            PortDot { link: modelData.l; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: modelData.t; color: "#E8ECF1"; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }
                Column {
                    visible: app.strayCount > 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width
                    spacing: 8
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: "Some skills sit in an agent's own folder. Adopting moves them into the shared one; identical copies and conflicts are kept in a backup."
                        color: "#C6CEDA"; font.family: Theme.ui; font.pixelSize: Theme.fsBody
                    }
                    Btn { anchors.horizontalCenter: parent.horizontalCenter; iconName: "tool"; text: "Adopt strays now"; enabled: !app.busy; onClicked: app.adopt() }
                }
            }

            // theme, accent and material, on the real window behind
            Column {
                visible: root.step === 3
                width: Math.min(parent.width, 420)
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 14
                Rectangle {
                    width: parent.width
                    height: choices.height + 32
                    radius: 4
                    color: Theme.bg2
                    Column {
                        id: choices
                        x: 16; y: 16
                        width: parent.width - 32
                        spacing: 12
                        Segmented {
                            width: parent.width
                            options: [ { value: "system", label: "System", icon: "auto" }, { value: "dark", label: "Dark", icon: "moon" }, { value: "light", label: "Light", icon: "sun" } ]
                            current: app.themeMode
                            onPicked: (v) => app.themeMode = v
                        }
                        Flow {
                            width: parent.width
                            spacing: 8
                            Repeater {
                                model: ["#34E0A6", "#2EC4F1", "#5B8DEF", "#8B6CF6", "#D66BE3", "#F06292", "#F2734E", "#7A8796"]
                                delegate: AbstractButton {
                                    id: swatch
                                    required property string modelData
                                    width: 32; height: 32
                                    focusPolicy: Qt.StrongFocus
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: "Accent " + modelData
                                    onClicked: app.setSetting("appearance", "accent", { source: modelData === "#34E0A6" ? "default" : "custom", color: modelData })
                                    background: Rectangle { radius: 4; color: swatch.modelData; border.color: swatch.visualFocus ? Theme.focus : "transparent"; border.width: 2 }
                                }
                            }
                        }
                        Segmented {
                            width: parent.width
                            options: [ { value: "solid", label: "Solid", icon: "all" }, { value: "blur", label: "Blur", icon: "moon" }, { value: "glass", label: "Liquid Glass", icon: "auto" } ]
                            current: Theme.setting("appearance", "material", "solid")
                            onPicked: (v) => app.setSetting("appearance", "material", v)
                        }
                    }
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: "It all applies right away, and all of it stays in Settings."
                    color: "#C6CEDA"; font.family: Theme.ui; font.pixelSize: Theme.fsBody
                }
            }

            // ready
            Grid {
                visible: root.step === 5
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 2
                columnSpacing: 16
                rowSpacing: 8
                Repeater {
                    model: [ { k: "/", t: "Filter skills" }, { k: "Ctrl+N", t: "Add skills" }, { k: "Ctrl+U", t: "Check for updates" }, { k: "Ctrl+,", t: "Settings" }, { k: "Ctrl+`", t: "Terminal" }, { k: "F1", t: "This tour again" } ]
                    delegate: Row {
                        required property var modelData
                        spacing: 10
                        Rectangle {
                            width: 76; height: 26; radius: 3
                            color: Theme.bg3
                            Text { anchors.centerIn: parent; text: modelData.k; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
                        }
                        Text { text: modelData.t; color: "#E8ECF1"; font.family: Theme.ui; font.pixelSize: Theme.fsBody; anchors.verticalCenter: parent.verticalCenter }
                    }
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Btn { visible: root.step > 0; kind: "ghost"; text: "Back"; onClicked: root.back() }
                Btn { kind: "primary"; text: root.step === 0 ? "Start" : root.step === root.steps - 1 ? "Done" : "Next"; onClicked: root.next() }
                Btn { visible: root.step === root.steps - 1; iconName: "terminal"; text: "Open the terminal"; onClicked: { root.finished(); root.openTerminal() } }
                Btn { visible: root.step < root.steps - 1; kind: "ghost"; text: "Skip"; onClicked: root.finished() }
            }
        }
    }

    // ---- the coach marks
    Rectangle {
        id: card
        visible: root.step === 4
        width: 320
        height: cardCol.height + 28
        radius: 4
        color: Theme.bg3
        readonly property real gap: 16
        x: {
            const h = root.hole
            const want = !root.mark ? (root.width - width) / 2
                         : root.mark.place === "right" ? h.x + h.width + gap
                         : root.mark.place === "left" ? h.x - width - gap
                         : h.x + (h.width - width) / 2
            return Math.max(16, Math.min(root.width - width - 16, want))
        }
        y: {
            const h = root.hole
            const want = !root.mark ? (root.height - height) / 2
                         : root.mark.place === "below" ? h.y + h.height + gap
                         : root.mark.place === "above" ? h.y - height - gap
                         : h.y
            return Math.max(16, Math.min(root.height - height - 16, want))
        }
        Behavior on x { NumberAnimation { duration: Theme.dSlow; easing.type: Easing.OutCubic } }
        Behavior on y { NumberAnimation { duration: Theme.dSlow; easing.type: Easing.OutCubic } }
        Accessible.role: Accessible.StaticText
        Accessible.name: root.mark ? root.mark.title + ". " + root.mark.text : ""
        Column {
            id: cardCol
            x: 16; y: 14
            width: parent.width - 32
            spacing: 8
            Text { width: parent.width; wrapMode: Text.Wrap; text: root.mark ? root.mark.title : ""; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsLead + 2; font.weight: Font.Bold }
            Text { width: parent.width; wrapMode: Text.Wrap; text: root.mark ? root.mark.text : ""; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; lineHeight: 1.2 }
            Item {
                width: parent.width
                height: 34
                NumText { anchors.verticalCenter: parent.verticalCenter; text: (root.coach + 1) + " / " + root.marks.length; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                Row {
                    anchors.right: parent.right
                    spacing: 6
                    Btn { kind: "ghost"; compact: true; text: "Back"; onClicked: root.back() }
                    Btn { kind: "primary"; compact: true; text: "Next"; onClicked: root.next() }
                }
            }
        }
    }
}
