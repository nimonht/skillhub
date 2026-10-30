pragma Singleton
import QtQuick

// Design tokens. Roles, not colours: components never name a hue directly.
// Dark = smoked charcoal, light = paper over concrete. Mint is the brand/selection colour;
// amber = update available, lime = just updated, red = broken. Cyan/blue/purple are unused on purpose.
QtObject {
    id: t

    readonly property string mode: typeof app !== "undefined" ? app.themeMode : "system"
    readonly property bool dark: mode === "dark" || (mode !== "light" && systemDark)
    // Qt 6.5+ reports the system colour scheme; before that the window colour of the platform palette tells
    readonly property bool systemDark: Application.styleHints && Application.styleHints.colorScheme !== undefined
                                       ? Application.styleHints.colorScheme !== Qt.Light : sysPalette.window.hslLightness < 0.5
    property SystemPalette sysPalette: SystemPalette { colorGroup: SystemPalette.Active }
    readonly property bool still: typeof app !== "undefined" ? app.stillMotion : false
    // a setting, in a way bindings notice when it changes
    function setting(section, key, fallback) { return app.settingsRev >= 0 ? app.setting(section, key, fallback) : fallback }

    // surfaces: window < rail/console < list < raised < selected
    readonly property color bg0Default: dark ? "#0B0E13" : "#DDE1E6"
    readonly property color bg1Default: dark ? "#10141A" : "#E8EBEE"
    readonly property color bg2Default: dark ? "#141920" : "#F5F6F7"
    // Blur and Liquid Glass: what is behind the window shows through the panels, and never more than these floors allow, so text
    // keeps 4.5:1 even over pure black or white (tests/contrast.py). The window itself is then clear: panels carry the tint.
    readonly property string material: typeof app !== "undefined" ? app.material : "solid"
    readonly property bool translucent: material !== "solid"
    readonly property real alphaFloor: dark ? 0.89 : 0.83
    readonly property real surfaceAlpha: translucent ? 1 - setting("appearance", "intensity", 0.8) * (1 - alphaFloor) : 1
    readonly property bool tinted: typeof app !== "undefined" && setting("appearance", "tintSurfaces", false)
    function surface(c) {
        const t = tinted ? Qt.tint(c, Qt.rgba(mark.r, mark.g, mark.b, 0.03)) : c
        return translucent ? Qt.rgba(t.r, t.g, t.b, surfaceAlpha) : t
    }
    readonly property color bg0: translucent ? "transparent" : surface(bg0Default)
    readonly property color bg1: surface(bg1Default)
    readonly property color bg2: surface(bg2Default)
    readonly property color bg3: dark ? "#1B212A" : "#FFFFFF"
    readonly property color bg4Default: dark ? "#222A35" : "#DCEFE8"
    readonly property color bg4: accent ? accent.selected : bg4Default
    readonly property color field: dark ? "#222A35" : "#E4E8EC"          // input fill: a tone step, no outline
    readonly property color trackOff: dark ? "#2C3542" : "#BFC7D0"       // switch track when off
    readonly property color line: dark ? "#262E39" : "#CBD1D8"
    readonly property color lineStrong: dark ? "#3A4452" : "#A3ADB9"
    readonly property color lineInput: dark ? "#667180" : "#6F7A87"        // control boundaries that identify a control: >= 3:1

    // text
    readonly property color text: dark ? "#E8ECF1" : "#11161C"
    readonly property color text2: dark ? "#A5B0BE" : "#465160"
    readonly property color text3Default: dark ? "#8995A4" : "#5E6977"   // tertiary: hints, disabled, still >= 4.5:1 on list surfaces
    readonly property color text3: translucent || tinted ? text2 : text3Default   // over a backdrop or a tint there is no contrast to spare

    // the accent: these defaults, or the roles derived from the user's colour (src/core/sh_palette.c)
    readonly property var accent: typeof app !== "undefined" && app.accentOn ? (dark ? app.paletteDark : app.paletteLight) : null
    readonly property color actionDefault: dark ? "#34E0A6" : "#11161C"        // primary action fill
    readonly property color actionInkDefault: dark ? "#04251A" : "#FFFFFF"     // text on the primary action
    readonly property color markDefault: dark ? "#34E0A6" : "#0A8F69"          // selection marks, lit wires, ports (graphics)
    readonly property color markTextDefault: dark ? "#34E0A6" : "#07765A"      // the accent used as text
    readonly property color markLowDefault: dark ? "#12362B" : "#D4EFE5"
    readonly property color focusDefault: dark ? "#34E0A6" : "#0B7F5F"
    readonly property color inkOkDefault: dark ? "#07765A" : "#34E0A6"
    readonly property color action: accent ? accent.action : actionDefault
    readonly property color actionInk: accent ? accent.actionInk : actionInkDefault
    readonly property color mark: accent ? accent.mark : markDefault
    readonly property color markTextSolid: accent ? accent.markText : markTextDefault
    readonly property color markText: strengthen(markTextSolid)
    function strengthen(c) { return translucent && !dark ? Qt.darker(c, 1.4) : c }   // light surfaces can go dark behind: see alphaFloor
    readonly property color markLow: accent ? accent.markLow : markLowDefault
    readonly property color focus: accent ? accent.focus : focusDefault
    readonly property color inkOk: accent ? accent.inkOk : inkOkDefault
    readonly property color inkOnMark: dark ? actionInk : "#FFFFFF"               // a knob or a tick on a lit fill
    // semantic roles
    readonly property color notify: dark ? "#FFB224" : "#B26A00"        // update available (graphics)
    readonly property color notifyTextDefault: dark ? "#FFB224" : "#8A5200"
    readonly property color notifyText: strengthen(notifyTextDefault)
    readonly property color notifyLow: dark ? "#3A2A0C" : "#FFEFCC"
    readonly property color gain: dark ? "#C8F23C" : "#5F8200"
    readonly property color gainTextDefault: dark ? "#C8F23C" : "#466100"
    readonly property color gainText: strengthen(gainTextDefault)
    readonly property color gainLow: dark ? "#2A3A0A" : "#EAF6C4"
    readonly property color danger: dark ? "#FF6A5E" : "#C8362B"
    readonly property color dangerTextDefault: dark ? "#FF6A5E" : "#B3241B"
    readonly property color dangerText: strengthen(dangerTextDefault)
    readonly property color dangerLow: dark ? "#3C1917" : "#FBE3E0"
    // inverted surface for transient things (toast, tooltip): borderless, readable over anything
    readonly property color inkBg: dark ? "#E8ECF1" : "#11161C"
    readonly property color inkText: dark ? "#0B0E13" : "#F5F6F7"
    readonly property color inkWarn: dark ? "#8A5200" : "#FFB224"
    readonly property color inkDanger: dark ? "#B3241B" : "#FF6A5E"

    // type: Barlow Semi Condensed for the interface, Barlow Condensed for numbers and titles (both SIL OFL)
    readonly property FontLoader _f1: FontLoader { source: "qrc:/fonts/BarlowSemiCondensed-Regular.ttf" }
    readonly property FontLoader _f2: FontLoader { source: "qrc:/fonts/BarlowSemiCondensed-Medium.ttf" }
    readonly property FontLoader _f3: FontLoader { source: "qrc:/fonts/BarlowSemiCondensed-SemiBold.ttf" }
    readonly property FontLoader _f4: FontLoader { source: "qrc:/fonts/BarlowCondensed-SemiBold.ttf" }
    readonly property FontLoader _f5: FontLoader { source: "qrc:/fonts/BarlowCondensed-Bold.ttf" }
    readonly property string ui: _f1.name !== "" ? _f1.name : "sans-serif"
    readonly property string display: _f5.name !== "" ? _f5.name : "sans-serif"
    readonly property string mono: Qt.platform.os === "windows" ? "Consolas" : Qt.platform.os === "osx" ? "Menlo" : "monospace"
    // fixed scale (product UI): 12 / 13 / 14 / 16 / 20 / 26 / 34
    readonly property int fsMicro: 12
    readonly property int fsSmall: 13
    readonly property int fsBody: 14
    readonly property int fsLead: 16
    readonly property int fsTitle: 20
    readonly property int fsHead: 26
    readonly property int fsBig: 34

    // spacing and geometry
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 24
    readonly property int s6: 32
    readonly property int railW: 216
    readonly property int detailW: 380
    readonly property int rowH: typeof app !== "undefined" && app.compactDensity ? 44 : 56
    readonly property int portW: 72          // one agent column: header tile and row port share it
    readonly property int padR: 20
    readonly property int statusW: 116
    readonly property int cut: 8             // chamfer used on action and structure shapes
    readonly property int minTap: 36

    // motion: state changes only, 120-220 ms, off when reduced
    readonly property int dFast: still ? 0 : 120
    readonly property int dBase: still ? 0 : 180
    readonly property int dSlow: still ? 0 : 320
}
