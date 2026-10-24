import QtQuick
import "."

// Transient confirmation on an inverted surface: no outline, readable over anything. Level = icon + words.
Rectangle {
    id: root
    property string level: "ok"
    property string message: ""
    function show(l, m) {
        if (m === "") return
        level = l
        message = m
        opacity = 1
        hideTimer.restart()
    }
    implicitWidth: row.implicitWidth + 32
    implicitHeight: 40
    radius: 4
    color: Theme.inkBg
    opacity: 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.dBase } }
    Accessible.role: Accessible.AlertMessage
    Accessible.name: message

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 9
        Icon { name: root.level === "ok" ? "check" : "alert"; size: 16; weight: 2.4; color: root.level === "error" ? Theme.inkDanger : root.level === "warn" ? Theme.inkWarn : Theme.inkOk; anchors.verticalCenter: parent.verticalCenter }
        Text { text: root.message; color: Theme.inkText; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.Medium; anchors.verticalCenter: parent.verticalCenter }
    }
    Timer { id: hideTimer; interval: 3800; onTriggered: root.opacity = 0 }
}
