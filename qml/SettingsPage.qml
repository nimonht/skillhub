import QtQuick
import QtQuick.Controls.Basic
import "."

// Everything that can be set, on one page with its own sections. Changes apply at once, and reach settings.json right away.
Rectangle {
    id: root
    color: Theme.bg1
    signal closeRequested()
    signal resetLayout()
    property string section: "appearance"
    readonly property var sections: [
        { id: "appearance", label: "Appearance", icon: "sun" }, { id: "layout", label: "Layout", icon: "all" },
        { id: "terminal", label: "Terminal", icon: "terminal" }, { id: "updates", label: "Updates and sources", icon: "upgrade" }, { id: "projects", label: "Projects", icon: "branch" },
        { id: "advanced", label: "Advanced", icon: "tool" }, { id: "about", label: "About", icon: "info" } ]

    function look(key, fallback) { return Theme.setting("appearance", key, fallback) }
    function setLook(key, value) { app.setSetting("appearance", key, value) }
    readonly property var accent: look("accent", { source: "default", color: "#34E0A6" })
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
        Text { text: parent.title; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold }
        Text { visible: parent.note !== ""; width: parent.width; wrapMode: Text.Wrap; text: parent.note; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2 }
        Column { id: inner; width: parent.width; spacing: 8 }
    }
    component Line: Text { width: parent ? parent.width : 400; wrapMode: Text.Wrap; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; lineHeight: 1.2 }

    Item {
        id: nav
        width: 220
        height: parent.height
        Text { x: 24; y: 22; text: "Settings"; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
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
                        Text { x: 40; anchors.verticalCenter: parent.verticalCenter; text: entry.modelData.label; color: entry.on ? Theme.text : Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: entry.on ? Font.DemiBold : Font.Medium }
                    }
                }
            }
        }
        Btn { x: 16; anchors.bottom: parent.bottom; anchors.bottomMargin: 16; kind: "ghost"; iconName: "x"; text: "Close"; onClicked: root.closeRequested() }
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
                    title: "Theme"
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "system", label: "System", icon: "auto" }, { value: "dark", label: "Dark", icon: "moon" }, { value: "light", label: "Light", icon: "sun" } ]
                        current: app.themeMode
                        onPicked: (v) => app.themeMode = v
                    }
                }
                Group {
                    title: "Accent"
                    note: app.systemAccent === "" ? "This system does not tell an accent colour, so Match system is off." : ""
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "default", label: "Mint", icon: "check" }, { value: "system", label: "Match system", icon: "auto" }, { value: "custom", label: "Custom", icon: "sliders" } ]
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
                                Accessible.name: "Accent " + modelData
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
                        label: "Hue"
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
                        label: "Colourfulness"
                        from: 0; to: 0.25; stepSize: 0.005
                        value: root.lch.c
                        shown: Math.round(value * 400) + " %"
                        onMoved: (v) => root.setAccent("custom", app.hexOf(root.lch.l, v, root.lch.h))
                    }
                    Range {
                        visible: root.accent.source === "custom"
                        width: parent.width
                        label: "Lightness"
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
                        text: "This hue sits close to the " + root.clash + " that means " + (root.clash === "amber" ? "an update is waiting" : root.clash === "red" ? "something is broken" : "just updated") + ", so the two may be hard to tell apart."
                        color: Theme.notifyText
                    }
                    Row {   // what the accent does, in both modes, with the ratios that matter
                        spacing: 12
                        Repeater {
                            model: [ { dark: true, label: "Dark", bg: "#141920", text: "#E8ECF1" }, { dark: false, label: "Light", bg: "#F5F6F7", text: "#11161C" } ]
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
                                        Text { anchors.centerIn: parent; text: "Primary"; color: parent.parent.parent.ink; font.family: Theme.ui; font.pixelSize: Theme.fsSmall; font.weight: Font.DemiBold }
                                    }
                                    Row {
                                        spacing: 8
                                        Rectangle { width: 10; height: 10; radius: 5; color: parent.parent.parent.mark; anchors.verticalCenter: parent.verticalCenter }
                                        Text { text: "Selected"; color: parent.parent.parent.markText; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                                    }
                                    Text {
                                        text: modelData.label + "  ·  " + app.contrast(parent.parent.markText, modelData.bg).toFixed(1) + ":1 text  ·  " + app.contrast(parent.parent.fill, parent.parent.ink).toFixed(1) + ":1 button"
                                        color: modelData.text
                                        font.family: Theme.ui
                                        font.pixelSize: Theme.fsMicro
                                    }
                                }
                            }
                        }
                    }
                    Toggle { width: Math.min(parent.width, 420); text: "Tint surfaces with the accent"; checked: root.look("tintSurfaces", false); onToggled: root.setLook("tintSurfaces", checked) }
                }
                Group {
                    title: "Material"
                    note: app.materialNote
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "solid", label: "Solid", icon: "all" }, { value: "blur", label: "Blur", icon: "moon" }, { value: "glass", label: "Liquid Glass", icon: "auto" } ]
                        current: root.look("material", "solid")
                        onPicked: (v) => root.setLook("material", v)
                    }
                    Range {
                        visible: root.look("material", "solid") !== "solid"
                        width: Math.min(parent.width, 340)
                        label: "Intensity"
                        from: 0; to: 1; stepSize: 0.05
                        value: root.look("intensity", 0.8)
                        shown: Math.round(value * 100) + " %"
                        onMoved: (v) => root.setLook("intensity", v)
                    }
                    Toggle { width: Math.min(parent.width, 420); text: "Reduce transparency"; checked: root.look("reduceTransparency", false); onToggled: root.setLook("reduceTransparency", checked) }
                    Toggle { width: Math.min(parent.width, 420); text: "Reduce motion"; checked: app.reduceMotion; onToggled: app.reduceMotion = checked }
                }
                Group {
                    title: "Size"
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "0.9", label: "90 %" }, { value: "1", label: "100 %" }, { value: "1.1", label: "110 %" }, { value: "1.25", label: "125 %" } ]
                        current: String(root.look("uiScale", 1))
                        onPicked: (v) => root.setLook("uiScale", Number(v))
                    }
                    Line { text: "The scale applies the next time Skill Hub starts." }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "comfortable", label: "Comfortable" }, { value: "compact", label: "Compact" } ]
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
                    title: "Panels"
                    note: "Drag any edge between panels. Ctrl+Alt with the arrow keys does the same from the keyboard, and Ctrl+Alt+0 puts everything back."
                    Btn { iconName: "refresh"; text: "Reset sizes"; onClicked: root.resetLayout() }
                }
                Toggle { width: Math.min(parent.width, 420); text: "Fold the rail to icons in narrow windows"; checked: Theme.setting("layout", "railAutoCollapse", false); onToggled: app.setSetting("layout", "railAutoCollapse", checked) }
                Toggle { width: Math.min(parent.width, 420); text: "Open the activity dock at start"; checked: Theme.setting("layout", "dockAtStart", false); onToggled: app.setSetting("layout", "dockAtStart", checked) }
                Toggle { width: Math.min(parent.width, 420); text: "Tiles only in the agent band"; checked: Theme.setting("layout", "bandCompact", false); onToggled: app.setSetting("layout", "bandCompact", checked) }
            }

            // ---- terminal
            Column {
                visible: root.section === "terminal"
                width: parent.width
                spacing: 16
                Group {
                    title: "Shell"
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
                    title: "Look"
                    note: "Take the font and colours of a terminal you already set up."
                    Segmented {
                        width: Math.min(parent.width, 420)
                        options: [ { value: "", label: "Own" }, { value: "kitty", label: "kitty" }, { value: "ghostty", label: "Ghostty" }, { value: "alacritty", label: "Alacritty" } ]
                        current: Theme.setting("terminal", "importLook", "")
                        onPicked: (v) => app.setSetting("terminal", "importLook", v)
                    }
                    Field { width: Math.min(parent.width, 420); text: Theme.setting("terminal", "fontFamily", ""); placeholderText: "Font family"; onAccepted: app.setSetting("terminal", "fontFamily", text.trim()) }
                    Range { width: Math.min(parent.width, 420); label: "Font size"; from: 9; to: 28; stepSize: 1; value: Theme.setting("terminal", "fontSize", 0) || 14; shown: Math.round(value) + " px"; onMoved: (v) => app.setSetting("terminal", "fontSize", Math.round(v)) }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "block", label: "Block" }, { value: "bar", label: "Bar" }, { value: "underline", label: "Underline" } ]
                        current: Theme.setting("terminal", "cursor", "block")
                        onPicked: (v) => app.setSetting("terminal", "cursor", v)
                    }
                    Range { width: Math.min(parent.width, 420); label: "Lines kept above the screen"; from: 1000; to: 100000; stepSize: 1000; value: Theme.setting("terminal", "scrollback", 10000); shown: Math.round(value); onMoved: (v) => app.setSetting("terminal", "scrollback", Math.round(v)) }
                }
                Group {
                    title: "Banner"
                    Toggle { width: Math.min(parent.width, 420); text: "Show the banner in new terminals"; checked: Theme.setting("terminal", "banner", true); onToggled: app.setSetting("terminal", "banner", checked) }
                    Segmented {
                        width: Math.min(parent.width, 340)
                        options: [ { value: "unicode", label: "Plain" }, { value: "nerd", label: "Nerd icons" }, { value: "none", label: "No dots" } ]
                        current: Theme.setting("terminal", "icons", "unicode")
                        onPicked: (v) => app.setSetting("terminal", "icons", v)
                    }
                    Field { width: Math.min(parent.width, 420); text: Theme.setting("terminal", "logo", "builtin"); placeholderText: "builtin, none, or the path of an image"; onAccepted: app.setSetting("terminal", "logo", text.trim()) }
                }
                Group {
                    title: "Other terminals"
                    note: "Found on this machine: " + (app.externalTerminals.length ? app.externalTerminals.map(function (t) { return t.label }).join(", ") : "none") + ". {dir} and {shell} stand for the folder and the shell in a command of your own."
                    Field { width: parent.width; text: Theme.setting("terminal", "externalTemplate", ""); placeholderText: "kitty --directory {dir} {shell}"; onAccepted: app.setSetting("terminal", "externalTemplate", text.trim()) }
                }
            }

            // ---- updates and sources
            Column {
                visible: root.section === "updates"
                width: parent.width
                spacing: 16
                Group {
                    title: "Automation"
                    Toggle { visible: app.automationSupported; width: Math.min(parent.width, 420); text: "Daily update"; hint: app.automationMechanism; checked: app.timerState === "on"; onToggled: app.setTimer(checked) }
                    Toggle { visible: app.automationSupported; width: Math.min(parent.width, 420); text: "Adopt strays"; hint: "watch agent folders"; checked: app.watcherState === "on"; onToggled: app.setWatcher(checked) }
                    Toggle { width: Math.min(parent.width, 420); text: "Leave out updates that add or change scripts"; hint: "Automatic runs only. You review those yourself"; checked: Theme.setting("updates", "autoSkipScripts", true); onToggled: app.setSetting("updates", "autoSkipScripts", checked) }
                }
                Group {
                    title: "Updating"
                    Toggle { width: Math.min(parent.width, 420); text: "Review before updating"; hint: "Update all opens the review first"; checked: Theme.setting("updates", "reviewBeforeUpdate", true); onToggled: app.setSetting("updates", "reviewBeforeUpdate", checked) }
                    Toggle { width: Math.min(parent.width, 420); text: "Check at launch"; checked: Theme.setting("updates", "checkAtLaunch", true); onToggled: app.setSetting("updates", "checkAtLaunch", checked) }
                    Range {
                        width: Math.min(parent.width, 420)
                        label: "Versions kept of each skill"
                        from: 1; to: 20; stepSize: 1
                        value: Theme.setting("updates", "backupsKept", 3)
                        shown: Math.round(value)
                        onMoved: (v) => app.setSetting("updates", "backupsKept", Math.round(v))
                    }
                }
                Group {
                    title: "Downloads at once"
                    note: "More is faster with many repositories, and heavier on the machine and the server."
                    Range {
                        width: Math.min(parent.width, 420)
                        label: "In total"
                        from: 1; to: 32; stepSize: 1
                        value: Theme.setting("updates", "parallel", 16)
                        shown: Math.round(value)
                        onMoved: (v) => app.setSetting("updates", "parallel", Math.round(v))
                    }
                    Range {
                        width: Math.min(parent.width, 420)
                        label: "Per host"
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
                Line { text: "Projects keep their own skills in .agents/skills and share them through skills-lock.json. Switch between them at the top of the rail." }
                Text { visible: app.projects.length === 0; text: "No projects yet"; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                Repeater {
                    model: app.projects
                    delegate: Item {
                        required property var modelData
                        width: parent.width
                        height: 52
                        Column {
                            width: parent.width - autoToggle.width - forget.width - 16
                            anchors.verticalCenter: parent.verticalCenter
                            Text { width: parent.width; elide: Text.ElideRight; text: modelData.name + "  ·  " + modelData.skills + (modelData.skills === 1 ? " skill" : " skills"); color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsBody }
                            Text { width: parent.width; elide: Text.ElideMiddle; text: modelData.path; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        }
                        Toggle { id: autoToggle; anchors.right: forget.left; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; implicitWidth: 150; text: "Daily update"; checked: modelData.autoUpdate; onToggled: app.setProjectAuto(modelData.path, checked) }
                        Btn { id: forget; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; kind: "ghost"; compact: true; text: "Remove"; onClicked: app.removeProject(modelData.path) }
                    }
                }
                Repeater {
                    model: app.suggestedProjects
                    delegate: Btn { required property string modelData; kind: "ghost"; compact: true; iconName: "plus"; text: "Add " + modelData; onClicked: app.addProject(modelData) }
                }
                Group {
                    title: "Where to look"
                    note: "Folders below these that hold skills are suggested here. One path per line."
                    Field {
                        width: parent.width
                        text: Theme.setting("projects", "scanRoots", ["~/Projects"]).join("  ")
                        onAccepted: app.setSetting("projects", "scanRoots", text.trim().split(/\s+/))
                    }
                }
            }

            // ---- advanced
            Column {
                visible: root.section === "advanced"
                width: parent.width
                spacing: 12
                Repeater {
                    model: app.pathSizes
                    delegate: Row {
                        required property var modelData
                        spacing: 12
                        Text { width: 110; text: modelData.label; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        NumText { width: 80; text: modelData.size; color: Theme.text; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                        Text { width: page.width - 202; elide: Text.ElideMiddle; text: modelData.path; color: Theme.text2; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
                    }
                }
                Row {
                    spacing: 8
                    Btn { kind: "ghost"; compact: true; iconName: "refresh"; text: "Measure"; enabled: !app.busy; onClicked: app.measurePaths() }
                    Btn { kind: "ghost"; compact: true; iconName: "trash"; text: "Clear cache"; enabled: !app.busy; onClicked: app.clearCache() }
                    Btn { kind: "ghost"; compact: true; iconName: "refresh"; text: "Restart background service"; enabled: !app.busy; onClicked: app.restartBackend() }
                }
                Line { text: "Backend " + app.backendInfo }
            }

            // ---- about
            Column {
                visible: root.section === "about"
                width: parent.width
                spacing: 12
                Logo { size: 48 }
                Text { text: "Skill Hub " + app.version; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.fsHead; font.weight: Font.Bold }
                Line { text: "One skills folder for every coding agent. MIT licence. Third-party licences are listed in NOTICE.md, next to the program." }
                Btn { iconName: "refresh"; text: "Take the tour again"; onClicked: { root.closeRequested(); app.startTour(0, 0) } }
            }
        }
    }
    Connections { target: app; function onShowSettings(s) { root.section = s } }
    onVisibleChanged: if (visible) app.loadProjects()
}
