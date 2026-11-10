import QtQuick
import QtQuick.Controls.Basic
import "."

// Everything that can be set, on one page with its own sections. Changes apply at once, and reach settings.json right away.
Rectangle {
    id: root
    color: Theme.bg1
    focus: visible
    signal closeRequested()
    signal resetLayout()
    property string section: "appearance"
    readonly property var sections: [
        { id: "appearance", label: qsTr("Appearance"), icon: "sun" }, { id: "layout", label: qsTr("Layout"), icon: "all" },
        { id: "terminal", label: qsTr("Terminal"), icon: "terminal" }, { id: "updates", label: qsTr("Updates and sources"), icon: "upgrade" }, { id: "safety", label: qsTr("Safety"), icon: "alert" }, { id: "projects", label: qsTr("Projects"), icon: "branch" },
        { id: "storage", label: qsTr("Storage"), icon: "folder" }, { id: "advanced", label: qsTr("Advanced"), icon: "tool" }, { id: "about", label: qsTr("About"), icon: "info" } ]

    function look(key, fallback) { return Theme.setting("appearance", key, fallback) }
    function setLook(key, value) { app.setSetting("appearance", key, value) }
    readonly property var accent: { const a = look("accent", null); return a && a.source ? a : { source: "default", color: "#34E0A6" } }
    readonly property string seed: accent.source === "system" ? app.systemAccent : (accent.color || "#34E0A6")
    readonly property var lch: app.oklch(seed) || { l: 0.8, c: 0.14, h: 165 }
    function setAccent(source, color) { setLook("accent", { source: source, color: color === undefined ? accent.color : color }) }
    readonly property string clash: { var p = app.derivePalette(seed, true); return p && p.clash ? p.clash : "" }

    component Group: Column {
        property string title
        property string note: ""
        default property alias content: inner.data
        width: parent ? parent.width : 400
        spacing: 8
        Text { textFormat: Text.PlainText; text: parent.title; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
        Text { textFormat: Text.PlainText; visible: parent.note !== ""; width: parent.width; wrapMode: Text.Wrap; text: parent.note; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2 }
        Column { id: inner; width: parent.width; spacing: 8 }
    }
    component Line: Text { textFormat: Text.PlainText; width: parent ? parent.width : 400; wrapMode: Text.Wrap; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2 }

    InputShield {}

    Item {
        id: nav
        width: 220
        height: parent.height
        Text { textFormat: Text.PlainText; x: 24; y: 22; text: qsTr("Settings"); color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
        Column {
            x: 12; y: 68
            width: parent.width - 24
            spacing: 2
            Repeater {
                model: root.sections
                delegate: AbstractButton {
                    id: entry
                    required property var modelData
                    readonly property bool on: root.section === modelData.id
                    width: parent.width
                    height: 38
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.label
                    onClicked: root.section = modelData.id
                    background: Rectangle { radius: 4; color: entry.on ? Theme.bg4 : entry.hovered ? Theme.bg3 : "transparent"; border.color: Theme.focus; border.width: entry.visualFocus ? 2 : 0 }
                    contentItem: Item {
                        Icon { x: 12; anchors.verticalCenter: parent.verticalCenter; name: entry.modelData.icon; size: 16; color: entry.on ? Theme.markText : Theme.text2 }
                        Text { textFormat: Text.PlainText; x: 40; anchors.verticalCenter: parent.verticalCenter; text: entry.modelData.label; color: entry.on ? Theme.text : Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: entry.on ? Font.DemiBold : Font.Medium }
                    }
                }
            }
        }
        Btn { x: 16; anchors.bottom: parent.bottom; anchors.bottomMargin: 16; kind: "ghost"; iconName: "x"; text: qsTr("Close"); onClicked: root.closeRequested() }
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
    }

    Flickable {
        id: pane
        anchors.left: nav.right
        anchors.right: parent.right
        height: parent.height
        contentWidth: width
        contentHeight: page.height + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: Scroll {}

        Column {
            id: page
            x: 32; y: 24
            width: Math.min(pane.width - 64, 640)
            spacing: 24

            // ---- appearance
            Column {
                visible: root.section === "appearance"
                width: parent.width
                spacing: 24
                Group {
                    title: qsTr("Language")
                    note: qsTr("The window's own text. What the background service and the command line say stays in English for now.")
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: app.languages
                        current: app.language
                        onPicked: (v) => app.language = v
                    }
                }
                Group {
                    title: qsTr("Theme")
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "system", label: qsTr("System"), icon: "auto" }, { value: "dark", label: qsTr("Dark"), icon: "moon" }, { value: "light", label: qsTr("Light"), icon: "sun" } ]
                        current: app.themeMode
                        onPicked: (v) => app.themeMode = v
                    }
                }
                Group {
                    title: qsTr("Accent")
                    note: app.systemAccent === "" ? qsTr("This system does not tell an accent colour, so Match system is off.") : ""
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "default", label: qsTr("Mint"), icon: "check" }, { value: "system", label: qsTr("Match system"), icon: "auto" }, { value: "custom", label: qsTr("Custom"), icon: "sliders" } ]
                        current: root.accent.source
                        onPicked: (v) => { if (v !== "system" || app.systemAccent !== "") root.setAccent(v) }
                    }
                    Flow {
                        visible: root.accent.source === "custom"
                        width: parent.width
                        spacing: 8
                        Repeater {
                            model: ["#34E0A6", "#2EC4F1", "#5B8DEF", "#8B6CF6", "#D66BE3", "#F06292", "#F2734E", "#7A8796"]
                            delegate: AbstractButton {
                                id: swatch
                                required property string modelData
                                width: 32; height: 32
                                hoverEnabled: true
                                focusPolicy: Qt.StrongFocus
                                Accessible.role: Accessible.RadioButton
                                Accessible.name: qsTr("Accent %1").arg(modelData)
                                onClicked: root.setAccent("custom", modelData)
                                background: Rectangle {
                                    radius: 4
                                    color: swatch.modelData
                                    border.color: root.seed.toUpperCase() === swatch.modelData ? Theme.text : swatch.visualFocus ? Theme.focus : "transparent"
                                    border.width: 2
                                }
                            }
                        }
                    }
                    Range {
                        visible: root.accent.source === "custom"
                        width: parent.width
                        label: qsTr("Hue")
                        from: 0; to: 359; stepSize: 1
                        value: root.lch.h
                        shown: Math.round(value) + "°"
                        onMoved: (v) => root.setAccent("custom", app.hexOf(root.lch.l, root.lch.c, v))
                        track: Row {
                            anchors.fill: parent
                            Repeater {
                                model: 36
                                delegate: Rectangle { required property int index; width: parent.width / 36; height: parent.height; color: app.hexOf(0.75, 0.14, index * 10) }
                            }
                        }
                    }
                    Range {
                        visible: root.accent.source === "custom"
                        width: parent.width
                        label: qsTr("Colourfulness")
                        from: 0; to: 0.25; stepSize: 0.005
                        value: root.lch.c
                        shown: Math.round(value * 400) + " %"
                        onMoved: (v) => root.setAccent("custom", app.hexOf(root.lch.l, v, root.lch.h))
                    }
                    Range {
                        visible: root.accent.source === "custom"
                        width: parent.width
                        label: qsTr("Lightness")
                        from: 0.3; to: 0.95; stepSize: 0.01
                        value: root.lch.l
                        shown: Math.round(value * 100) + " %"
                        onMoved: (v) => root.setAccent("custom", app.hexOf(v, root.lch.c, root.lch.h))
                    }
                    Field {
                        visible: root.accent.source === "custom"
                        width: 160
                        text: root.seed
                        placeholderText: "#RRGGBB"
                        onAccepted: if (/^#[0-9A-Fa-f]{6}$/.test(text.trim())) root.setAccent("custom", text.trim().toUpperCase())
                    }
                    Line {
                        visible: root.clash !== ""
                        text: root.clash === "amber" ? qsTr("This hue sits close to the amber that means an update is waiting, so the two may be hard to tell apart.")
                            : root.clash === "red" ? qsTr("This hue sits close to the red that means something is broken, so the two may be hard to tell apart.")
                            : qsTr("This hue sits close to the lime that means just updated, so the two may be hard to tell apart.")
                        color: Theme.notifyText
                    }
                    Row {   // what the accent does, in both modes, with the ratios that matter
                        spacing: 12
                        Repeater {
                            model: [ { dark: true, label: qsTr("Dark"), bg: "#141920", text: "#E8ECF1" }, { dark: false, label: qsTr("Light"), bg: "#F5F6F7", text: "#11161C" } ]
                            delegate: Rectangle {
                                required property var modelData
                                readonly property var p: root.accent.source === "default" ? null : app.derivePalette(root.seed, modelData.dark)
                                readonly property color fill: p ? p.action : (modelData.dark ? "#34E0A6" : "#11161C")
                                readonly property color ink: p ? p.actionInk : (modelData.dark ? "#04251A" : "#FFFFFF")
                                readonly property color mark: p ? p.mark : (modelData.dark ? "#34E0A6" : "#0A8F69")
                                readonly property color markText: p ? p.markText : (modelData.dark ? "#34E0A6" : "#07765A")
                                width: Math.min(220, (page.width - 12) / 2)
                                height: 118
                                radius: 4
                                color: modelData.bg
                                border.color: Theme.line
                                Column {
                                    x: 12; y: 12
                                    spacing: 8
                                    Rectangle {
                                        width: 92; height: 30; radius: 3
                                        color: parent.parent.fill
                                        Text { textFormat: Text.PlainText; anchors.centerIn: parent; text: qsTr("Primary"); color: parent.parent.parent.ink; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
                                    }
                                    Row {
                                        spacing: 8
                                        Rectangle { width: 10; height: 10; radius: 5; color: parent.parent.parent.mark; anchors.verticalCenter: parent.verticalCenter }
                                        Text { textFormat: Text.PlainText; text: qsTr("Selected"); color: parent.parent.parent.markText; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                                    }
                                    Text {
                                        textFormat: Text.PlainText
                                        text: qsTr("%1  ·  %2:1 text  ·  %3:1 button").arg(modelData.label).arg(app.contrast(parent.parent.markText, modelData.bg).toFixed(1)).arg(app.contrast(parent.parent.fill, parent.parent.ink).toFixed(1))
                                        color: modelData.text
                                        font.family: Theme.ui
                                        font.pixelSize: Theme.fsMicro
                                    }
                                }
                            }
                        }
                    }
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Tint surfaces with the accent"); checked: root.look("tintSurfaces", false); onToggled: root.setLook("tintSurfaces", checked) }
                }
                Group {
                    title: qsTr("Material")
                    note: app.materialNote
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "solid", label: qsTr("Solid"), icon: "all" }, { value: "blur", label: qsTr("Blur"), icon: "moon" }, { value: "glass", label: qsTr("Liquid Glass"), icon: "auto" } ]
                        current: root.look("material", "solid")
                        onPicked: (v) => root.setLook("material", v)
                    }
                    Range {
                        visible: root.look("material", "solid") !== "solid"
                        width: Math.min(parent.width, 340)
                        label: qsTr("Intensity")
                        from: 0; to: 1; stepSize: 0.05
                        value: root.look("intensity", 0.8)
                        shown: Math.round(value * 100) + " %"
                        onMoved: (v) => root.setLook("intensity", v)
                    }
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Reduce transparency"); checked: root.look("reduceTransparency", false); onToggled: root.setLook("reduceTransparency", checked) }
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Reduce motion"); checked: app.reduceMotion; onToggled: app.reduceMotion = checked }
                }
                Group {
                    title: qsTr("Size")
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "0.9", label: "90 %" }, { value: "1", label: "100 %" }, { value: "1.1", label: "110 %" }, { value: "1.25", label: "125 %" } ]
                        current: String(root.look("uiScale", 1))
                        onPicked: (v) => root.setLook("uiScale", Number(v))
                    }
                    Line { text: qsTr("The scale applies the next time Skill Hub starts.") }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "comfortable", label: qsTr("Comfortable") }, { value: "compact", label: qsTr("Compact") } ]
                        current: root.look("density", "comfortable")
                        onPicked: (v) => root.setLook("density", v)
                    }
                }
            }

            // ---- layout
            Column {
                visible: root.section === "layout"
                width: parent.width
                spacing: 16
                Group {
                    title: qsTr("Panels")
                    note: qsTr("Drag any edge between panels. Ctrl+Alt with the arrow keys does the same from the keyboard, and Ctrl+Alt+0 puts everything back.")
                    Btn { iconName: "refresh"; text: qsTr("Reset sizes"); onClicked: root.resetLayout() }
                }
                Toggle { width: Math.min(parent.width, 420); text: qsTr("Fold the rail to icons in narrow windows"); checked: Theme.setting("layout", "railAutoCollapse", false); onToggled: app.setSetting("layout", "railAutoCollapse", checked) }
                Toggle { width: Math.min(parent.width, 420); text: qsTr("Open the activity dock at start"); checked: Theme.setting("layout", "dockAtStart", false); onToggled: app.setSetting("layout", "dockAtStart", checked) }
                Toggle { width: Math.min(parent.width, 420); text: qsTr("Tiles only in the agent band"); checked: Theme.setting("layout", "bandCompact", false); onToggled: app.setSetting("layout", "bandCompact", checked) }
            }

            // ---- terminal
            Column {
                visible: root.section === "terminal"
                width: parent.width
                spacing: 16
                Group {
                    title: qsTr("Shell")
                    Flow {
                        width: parent.width
                        spacing: 6
                        Repeater {
                            model: app.shells
                            delegate: Btn {
                                required property var modelData
                                kind: Theme.setting("terminal", "shell", "") === modelData.path ? "secondary" : "ghost"
                                compact: true
                                text: modelData.name
                                onClicked: app.setSetting("terminal", "shell", modelData.path)
                            }
                        }
                    }
                }
                Group {
                    title: qsTr("Look")
                    note: qsTr("Take the font and colours of a terminal you already set up.")
                    Segmented {
                        width: Math.min(parent.width, 420)
                        options: [ { value: "", label: qsTr("Own") }, { value: "kitty", label: "kitty" }, { value: "ghostty", label: "Ghostty" }, { value: "alacritty", label: "Alacritty" } ]   // i18n-ok
                        current: Theme.setting("terminal", "importLook", "")
                        onPicked: (v) => app.setSetting("terminal", "importLook", v)
                    }
                    Field { width: Math.min(parent.width, 420); text: Theme.setting("terminal", "fontFamily", ""); placeholderText: qsTr("Font family"); onAccepted: app.setSetting("terminal", "fontFamily", text.trim()) }
                    Range { width: Math.min(parent.width, 420); label: qsTr("Font size"); from: 9; to: 28; stepSize: 1; value: Theme.setting("terminal", "fontSize", 0) || 14; shown: qsTr("%1 px").arg(Math.round(value)); onMoved: (v) => app.setSetting("terminal", "fontSize", Math.round(v)) }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "block", label: qsTr("Block") }, { value: "bar", label: qsTr("Bar") }, { value: "underline", label: qsTr("Underline") } ]
                        current: Theme.setting("terminal", "cursor", "block")
                        onPicked: (v) => app.setSetting("terminal", "cursor", v)
                    }
                    Range { width: Math.min(parent.width, 420); label: qsTr("Lines kept above the screen"); from: 1000; to: 100000; stepSize: 1000; value: Theme.setting("terminal", "scrollback", 10000); shown: Math.round(value); onMoved: (v) => app.setSetting("terminal", "scrollback", Math.round(v)) }
                }
                Group {
                    title: qsTr("Greeting")
                    note: { void app.settingsRev; return app.terminalGreetingNote() }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "auto", label: qsTr("Automatic") }, { value: "skillhub", label: "Skill Hub" }, { value: "shell", label: qsTr("My shell") } ]   // i18n-ok
                        current: Theme.setting("terminal", "greeting", Theme.setting("terminal", "banner", true) ? "auto" : "shell")
                        onPicked: (v) => app.setSetting("terminal", "greeting", v)
                    }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "unicode", label: qsTr("Plain") }, { value: "nerd", label: qsTr("Nerd icons") }, { value: "none", label: qsTr("No dots") } ]
                        current: Theme.setting("terminal", "icons", "unicode")
                        onPicked: (v) => app.setSetting("terminal", "icons", v)
                    }
                    Field { width: Math.min(parent.width, 420); text: Theme.setting("terminal", "logo", "builtin"); placeholderText: qsTr("builtin, none, or the path of an image"); onAccepted: app.setSetting("terminal", "logo", text.trim()) }
                }
                Group {
                    title: qsTr("Other terminals")
                    note: qsTr("Found on this machine: %1. {dir} and {shell} stand for the folder and the shell in a command of your own.").arg(app.externalTerminals.length ? app.externalTerminals.map(function (t) { return t.label }).join(", ") : qsTr("none"))
                    Field { width: parent.width; text: Theme.setting("terminal", "externalTemplate", ""); placeholderText: "kitty --directory {dir} {shell}"; onAccepted: app.setSetting("terminal", "externalTemplate", text.trim()) }   // i18n-ok
                }
            }

            // ---- safety
            Column {
                visible: root.section === "safety"
                width: parent.width
                spacing: 16
                Group {
                    title: qsTr("Checks")
                    note: qsTr("Every skill is read with fixed rules for things worth a look: downloads piped into a shell, commands that wipe files, hidden text, drop services for data, instructions to hide things from you. Nothing is sent anywhere.")
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Leave out updates that add a warning"); hint: qsTr("Automatic runs only. A new high finding, or a report that got worse, waits for you in Review"); checked: Theme.setting("updates", "autoSkipFindings", true); onToggled: app.setSetting("updates", "autoSkipFindings", checked) }
                }
                Group {
                    title: qsTr("Reports from skills.sh")
                    note: qsTr("What three outside scanners (Gen Agent Trust Hub, Socket, Snyk) found, shown for each skill. Only skills from public GitHub repositories are looked up, and their repository and name are sent to skills.sh; a private repository is never sent. Off while DO_NOT_TRACK is set.")
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Look up security reports"); checked: Theme.setting("security", "reports", false); onToggled: { app.setSetting("security", "reports", checked); if (app.selected !== "") app.readSafety(app.selected) } }
                }
            }

            // ---- updates and sources
            Column {
                visible: root.section === "updates"
                width: parent.width
                spacing: 16
                Group {
                    title: qsTr("Automation")
                    Toggle { visible: app.automationSupported; width: Math.min(parent.width, 420); text: qsTr("Daily update"); hint: app.automationMechanism; checked: app.timerState === "on"; onToggled: app.setTimer(checked) }
                    Toggle { visible: app.automationSupported; width: Math.min(parent.width, 420); text: qsTr("Adopt strays"); hint: qsTr("watch agent folders"); checked: app.watcherState === "on"; onToggled: app.setWatcher(checked) }
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Leave out updates that add or change scripts"); hint: qsTr("Automatic runs only. You review those yourself"); checked: Theme.setting("updates", "autoSkipScripts", true); onToggled: app.setSetting("updates", "autoSkipScripts", checked) }
                }
                Group {
                    title: qsTr("Updating")
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Review before updating"); hint: qsTr("Update all opens the review first"); checked: Theme.setting("updates", "reviewBeforeUpdate", true); onToggled: app.setSetting("updates", "reviewBeforeUpdate", checked) }
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Look for new versions of Skill Hub"); hint: qsTr("Reads the list of versions from GitHub at launch and once a day. Nothing is installed until you ask."); checked: Theme.setting("updates", "appCheck", true); onToggled: app.setSetting("updates", "appCheck", checked) }
                    Toggle { width: Math.min(parent.width, 420); text: qsTr("Check at launch"); checked: Theme.setting("updates", "checkAtLaunch", true); onToggled: app.setSetting("updates", "checkAtLaunch", checked) }
                    Range {
                        width: Math.min(parent.width, 420)
                        label: qsTr("Versions kept of each skill")
                        from: 1; to: 20; stepSize: 1
                        value: Theme.setting("updates", "backupsKept", 3)
                        shown: Math.round(value)
                        onMoved: (v) => app.setSetting("updates", "backupsKept", Math.round(v))
                    }
                }
                Group {
                    title: qsTr("Downloads at once")
                    note: qsTr("More is faster with many repositories, and heavier on the machine and the server.")
                    Range {
                        width: Math.min(parent.width, 420)
                        label: qsTr("In total")
                        from: 1; to: 32; stepSize: 1
                        value: Theme.setting("updates", "parallel", 16)
                        shown: Math.round(value)
                        onMoved: (v) => app.setSetting("updates", "parallel", Math.round(v))
                    }
                    Range {
                        width: Math.min(parent.width, 420)
                        label: qsTr("Per host")
                        from: 1; to: 16; stepSize: 1
                        value: Theme.setting("updates", "parallelPerHost", 8)
                        shown: Math.round(value)
                        onMoved: (v) => app.setSetting("updates", "parallelPerHost", Math.round(v))
                    }
                }
            }

            // ---- projects
            Column {
                visible: root.section === "projects"
                width: parent.width
                spacing: 12
                Line { text: qsTr("Projects keep their own skills in .agents/skills and share them through skills-lock.json. Switch between them at the top of the rail.") }
                Text { textFormat: Text.PlainText; visible: app.projects.length === 0; text: qsTr("No projects yet"); color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                Repeater {
                    model: app.projects
                    delegate: Item {
                        required property var modelData
                        width: parent.width
                        height: 52
                        Column {
                            width: parent.width - autoToggle.width - forget.width - 16
                            anchors.verticalCenter: parent.verticalCenter
                            Text { textFormat: Text.PlainText; width: parent.width; elide: Text.ElideRight; text: modelData.name + "  ·  " + (modelData.skills === 1 ? qsTr("1 skill") : qsTr("%1 skills").arg(modelData.skills)); color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                            Text { textFormat: Text.PlainText; width: parent.width; elide: Text.ElideMiddle; text: modelData.path; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        }
                        Toggle { id: autoToggle; anchors.right: forget.left; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; implicitWidth: 150; text: qsTr("Daily update"); checked: modelData.autoUpdate; onToggled: app.setProjectAuto(modelData.path, checked) }
                        Btn { id: forget; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; kind: "ghost"; compact: true; text: qsTr("Remove"); onClicked: app.removeProject(modelData.path) }
                    }
                }
                Repeater {
                    model: app.suggestedProjects
                    delegate: Btn { required property string modelData; kind: "ghost"; compact: true; iconName: "plus"; text: qsTr("Add %1").arg(modelData); onClicked: app.addProject(modelData) }
                }
                Group {
                    title: qsTr("Where to look")
                    note: qsTr("Folders below these that hold skills are suggested here. One path per line.")
                    Field {
                        width: parent.width
                        text: Theme.setting("projects", "scanRoots", ["~/Projects"]).join("  ")
                        onAccepted: app.setSetting("projects", "scanRoots", text.trim().split(/\s+/))
                    }
                }
            }

            // ---- storage
            Column {
                id: storage
                visible: root.section === "storage"
                width: parent.width
                spacing: 18
                property var pick: ({ cache: true, versions: true, removed: true, conflicts: false })
                readonly property var notes: ({
                    cache: qsTr("Downloaded again when needed."),
                    versions: qsTr("The newest backup of each skill stays, for rollback."),
                    removed: qsTr("Removed skills, duplicates and replaced copies. Restore stops working for these."),
                    conflicts: qsTr("Your own edits. Nothing removes these unless you do.")
                })
                onVisibleChanged: if (visible) app.measurePaths()
                Group {
                    title: qsTr("Space in use")
                    note: qsTr("Skills themselves are never touched here.")
                    Repeater {
                        model: app.pathSizes
                        delegate: Item {
                            required property var modelData
                            readonly property bool choice: storage.notes[modelData.key] !== undefined
                            width: Math.min(parent ? parent.width : 420, 520)
                            height: choice ? pickRow.implicitHeight : 22
                            Toggle {
                                id: pickRow
                                visible: parent.choice
                                width: parent.width
                                text: Words.storagePart(modelData.key, modelData.label) + "  ·  " + modelData.size
                                hint: storage.notes[modelData.key] || ""
                                checked: storage.pick[modelData.key] === true
                                onToggled: { const p = Object.assign({}, storage.pick); p[modelData.key] = checked; storage.pick = p }
                            }
                            Text { textFormat: Text.PlainText; visible: !parent.choice; text: Words.storagePart(modelData.key, modelData.label) + "  ·  " + modelData.size; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        }
                    }
                }
                Row {
                    spacing: 8
                    Btn {
                        kind: "primary"; compact: true; iconName: "trash"; text: qsTr("Free up space")
                        enabled: !app.busy && (storage.pick.cache || storage.pick.versions || storage.pick.removed || storage.pick.conflicts)
                        onClicked: app.cleanStorage(storage.pick.cache, storage.pick.versions, storage.pick.removed, storage.pick.conflicts)
                    }
                    Btn { kind: "ghost"; compact: true; iconName: "refresh"; text: qsTr("Measure"); enabled: !app.busy; onClicked: app.measurePaths() }
                }
                Group {
                    title: qsTr("Automatically")
                    note: qsTr("Checked after each update, install and import.")
                    Range {
                        width: Math.min(parent.width, 420)
                        label: qsTr("Cache limit: the clones used longest ago go first")
                        from: 0; to: 8192; stepSize: 256
                        value: Theme.setting("storage", "cacheMaxMB", 1024)
                        shown: value < 1 ? qsTr("No limit") : value >= 1024 ? qsTr("%1 GB").arg((value / 1024).toFixed(1)) : qsTr("%1 MB").arg(Math.round(value))
                        onMoved: (v) => app.setSetting("storage", "cacheMaxMB", Math.round(v))
                    }
                    Range {
                        width: Math.min(parent.width, 420)
                        label: qsTr("Keep removed skills for")
                        from: 0; to: 360; stepSize: 30
                        value: Theme.setting("storage", "backupDays", 90)
                        shown: value < 1 ? qsTr("Forever") : qsTr("%1 days").arg(Math.round(value))
                        onMoved: (v) => app.setSetting("storage", "backupDays", Math.round(v))
                    }
                }
            }

            // ---- advanced
            Column {
                visible: root.section === "advanced"
                width: parent.width
                spacing: 12
                Row {
                    spacing: 8
                    Btn { kind: "ghost"; compact: true; iconName: "refresh"; text: qsTr("Restart background service"); enabled: !app.busy; onClicked: app.restartBackend() }
                }
                Line { text: qsTr("Backend %1").arg(app.backendInfo) }
                Group {
                    title: qsTr("Diagnostics")
                    note: qsTr("For a bug report: a text file with versions, system, folders, settings, memory use and the end of the log. Nothing is sent anywhere; the home folder is written as ~ and user and host names are left out.")
                    Flow {
                        width: parent.width
                        spacing: 8
                        Btn { kind: "secondary"; compact: true; iconName: "download"; text: qsTr("Save a report"); enabled: reportDialogs.status === Loader.Ready; onClicked: reportDialogs.item.saveText("skillhub-report.txt", qsTr("Save the report")) }
                        Btn { kind: "secondary"; compact: true; iconName: "copy"; text: qsTr("Copy the report"); onClicked: app.createReport("copy") }
                        Btn { kind: "ghost"; compact: true; iconName: "terminal"; text: qsTr("Show the log"); onClicked: { root.closeRequested(); app.showLog() } }
                        Btn { kind: "ghost"; compact: true; iconName: "folder"; text: qsTr("Open the log folder"); onClicked: app.logView.openFolder() }
                    }
                }
                Group {
                    title: qsTr("Memory dump")
                    note: qsTr("A dump holds everything the program had in memory, which can include tokens and clipboard text: share it only with someone you trust. Skill Hub does not write one itself: your system's own tool does it, with steps below.")
                    Btn { kind: "secondary"; compact: true; iconName: "tool"; text: qsTr("How to make a memory dump"); onClicked: dumpHelp.open() }
                }
                Loader {
                    id: reportDialogs
                    source: "FileDialogs.qml"
                    onLoaded: { item.startFolder = app.documentsFolder; item.textChosen.connect(function (p) { app.createReport("save", p) }) }
                }
            }

            // ---- about
            Column {
                visible: root.section === "about"
                width: parent.width
                spacing: 12
                Logo { size: 48 }
                Text { textFormat: Text.PlainText; text: "Skill Hub " + app.version; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }   // i18n-ok
                Line { text: qsTr("One skills folder for every coding agent. MIT licence. Third-party licences are listed in NOTICE.md, next to the program.") }
                Group {
                    id: channel
                    // nobody chose: a beta follows betas, a final version follows final versions (sh_update_channel_of)
                    readonly property string saved: Theme.setting("updates", "channel", "")
                    readonly property string current: saved === "stable" || saved === "beta" ? saved : app.version.indexOf("-") > 0 ? "beta" : "stable"
                    title: qsTr("Update channel")
                    note: current === "beta" ? qsTr("Beta: test versions arrive before they are final, and may have rough edges. Final versions arrive too.")
                                             : qsTr("Stable: final versions only.")
                    Segmented {
                        width: Math.min(parent.width, 240)
                        options: [ { value: "stable", label: qsTr("Stable") }, { value: "beta", label: qsTr("Beta") } ]
                        current: channel.current
                        onPicked: (v) => { if (v !== channel.current) app.setUpdateChannel(v) }
                    }
                }
                Line {
                    visible: app.appUpdate.available === true
                    text: (app.appUpdate.downgrade === true ? qsTr("Skill Hub %1 is the newest stable version. Going back to it replaces this beta; your skills and settings stay.") : qsTr("Skill Hub %1 is out.")).arg(app.appUpdate.latest || "")
                          + " " + (app.appUpdate.advice || "")
                }
                Row {
                    spacing: 8
                    Btn {
                        visible: app.appUpdate.canInstall === true; enabled: !app.busy; iconName: "upgrade"
                        text: (app.appUpdate.downgrade === true ? qsTr("Go back to %1") : qsTr("Update to %1")).arg(app.appUpdate.latest || "")
                        onClicked: app.installAppUpdate()
                    }
                    Btn { visible: app.appUpdate.available === true; kind: "ghost"; text: qsTr("What is new"); onClicked: Qt.openUrlExternally(app.appUpdate.page) }
                    Btn { visible: app.appUpdate.available !== true; kind: "secondary"; iconName: "refresh"; text: qsTr("Look for a new version"); onClicked: app.checkAppUpdate(true) }
                }
                Btn { iconName: "refresh"; text: qsTr("Take the tour again"); onClicked: { root.closeRequested(); app.startTour(0, 0) } }
            }
        }
    }
    // how to make a memory dump with the system's own tool
    Popup {
        id: dumpHelp
        property var ids: ({ window: 0, backend: 0 })
        readonly property bool windows: Qt.platform.os === "windows"
        readonly property bool mac: Qt.platform.os === "osx"
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: parent   // the page; Qt 6.4's QtQuick.Controls.Basic has no Overlay
        width: Math.min(540, root.width - 48)
        padding: 22
        onAboutToShow: ids = app.processIds()
        background: Rectangle { radius: 6; color: Theme.bg3; border.color: Theme.line; border.width: 1 }
        contentItem: Column {
            spacing: 12
            Text { textFormat: Text.PlainText; width: parent.width; text: qsTr("How to make a memory dump"); color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsTitle; font.weight: Font.Bold }
            Line { text: qsTr("The window is process %1 and the background service is process %2.").arg(dumpHelp.ids.window).arg(dumpHelp.ids.backend) }
            Line { visible: dumpHelp.windows; text: qsTr("1. Open Task Manager and go to the Details tab.\n2. Find skillhub.exe (the window) or skillhubw.exe (the background service) by the number above.\n3. Right-click it and choose Create memory dump file. Windows shows where the .DMP file is saved.") }
            Line { visible: dumpHelp.mac; text: qsTr("In a terminal, start the debugger on the process, and at its prompt save the core file:\n   sudo lldb -p %1\n   (lldb) process save-core skillhub.core").arg(dumpHelp.ids.window) }
            Line { visible: !dumpHelp.windows && !dumpHelp.mac; text: qsTr("In a terminal, with gdb installed:\n   gcore %1\nAfter a crash, coredumpctl dump may already hold one.").arg(dumpHelp.ids.window) }
            Line { text: qsTr("A dump can hold tokens and clipboard text. Share it only with someone you trust.") }
            Row {
                spacing: 8
                Btn { visible: dumpHelp.windows; kind: "secondary"; iconName: "tool"; text: qsTr("Open Task Manager"); onClicked: app.openTaskManager() }
                Btn { kind: "ghost"; text: qsTr("Close"); onClicked: dumpHelp.close() }
            }
        }
    }
    Connections { target: app; function onShowSettings(s) { root.section = s } }
    onVisibleChanged: if (visible) app.loadProjects()
}
