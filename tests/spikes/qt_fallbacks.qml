// Spike 4 of the 2.0 plan: what the window can rely on with Qt 6.4 and with the software renderer (the headless tests).
//   QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software /usr/lib/qt6/bin/qml --verbose tests/spikes/qt_fallbacks.qml
// Prints one line per probe, then quits.
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic

Window {
    id: win
    width: 600
    height: 300
    visible: true

    SplitView {
        id: split
        anchors.fill: parent
        orientation: Qt.Horizontal
        Rectangle { id: hidden; SplitView.preferredWidth: 120; color: "red"; visible: false }
        Rectangle { id: middle; SplitView.fillWidth: true; color: "green"; readonly property int api: GraphicsInfo.api }
        Rectangle { id: right; SplitView.preferredWidth: 150; color: "blue" }
    }

    // QtQuick.Effects (MultiEffect) only exists from Qt 6.5: load it through a Loader so an older Qt just reports an error
    Loader { id: effects; source: "qt_fallbacks_effects.qml" }

    // a ShaderEffect with no shaders is a pass-through; the software renderer cannot run shaders at all
    ShaderEffect { id: shader; width: 10; height: 10 }

    Timer {
        interval: 300
        running: true
        onTriggered: {
            console.log("probe qt", Qt.platform.os, "api", middle.api, "software", middle.api === GraphicsInfo.Software)
            console.log("probe splitview hidden-child-skipped", hidden.width === 0 || middle.x < 10, "middle", middle.x, middle.width, "right", right.width)
            console.log("probe multieffect-loaded", effects.status === Loader.Ready, "status", effects.status)
            console.log("probe shadereffect-status", shader.status, "log", JSON.stringify(shader.log))
            var saved = split.saveState()
            console.log("probe splitview-savestate-bytes", saved ? saved.byteLength !== undefined ? saved.byteLength : String(saved).length : 0)
            Qt.quit()
        }
    }
}
