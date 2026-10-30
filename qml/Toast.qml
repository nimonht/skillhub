import QtQuick
import QtQuick.Controls.Basic
import "."

// Transient confirmation on an inverted surface: no outline, readable over anything. Level = icon + words.
Rectangle {
    id: root
    property string level: "ok"
    property string message: ""
    property string action: ""
    signal acted()
    function show(l, m, a) {
        if (m === "") return
        level = l
        message = m
        action = a === undefined ? "" : a
        opacity = 1
        hideTimer.interval = action !== "" ? 9000 : 3800
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
        AbstractButton {
            id: act
            visible: root.action !== ""
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: actText.implicitWidth + 16
            implicitHeight: 28
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.Button
            Accessible.name: root.action
            onClicked: { root.opacity = 0; root.acted() }
            contentItem: Item {}
            background: Rectangle {
                radius: 3
                color: act.hovered ? Qt.alpha(Theme.inkText, 0.14) : "transparent"
                border.color: Theme.inkOk
                border.width: act.visualFocus ? 2 : 0
                Text { id: actText; anchors.centerIn: parent; text: root.action; color: Theme.inkOk; font.family: Theme.ui; font.pixelSize: Theme.fsBody; font.weight: Font.Bold }
            }
        }
    }
    Timer { id: hideTimer; interval: 3800; onTriggered: root.opacity = 0 }
}
