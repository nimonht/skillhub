import QtQuick
import "."

// Status as icon + word, with no badge or outline around it. Colour is never the only signal.
Item {
    id: root
    property string kind: "current"   // available | current | updated | error | local | tracked | healthy | attention | absent
    property string label: ""
    property bool iconOnly: false

    readonly property var spec: ({
        "available": { text: qsTr("Update"),       icon: "upgrade", fg: Theme.notifyText, strong: true },
        "current":   { text: qsTr("Current"),      icon: "check",   fg: Theme.text2,      strong: false },
        "updated":   { text: qsTr("Updated"),      icon: "check",   fg: Theme.gainText,   strong: true },
        "error":     { text: qsTr("Error"),        icon: "alert",   fg: Theme.dangerText, strong: true },
        "local":     { text: qsTr("Local"),        icon: "folder",  fg: Theme.text2,      strong: false },
        "tracked":   { text: qsTr("Not checked"),  icon: "clock",   fg: Theme.text2,      strong: false },
        "healthy":   { text: qsTr("Healthy"),      icon: "check",   fg: Theme.markText,   strong: true },
        "attention": { text: qsTr("Needs repair"), icon: "alert",   fg: Theme.notifyText, strong: true },
        "absent":    { text: qsTr("Not found"),    icon: "info",    fg: Theme.text2,      strong: false }
    })
    readonly property var s: spec[kind] || spec["current"]

    implicitWidth: row.implicitWidth
    implicitHeight: 24
    Accessible.role: Accessible.StaticText
    Accessible.name: label !== "" ? label : s.text

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
        Icon { name: root.s.icon; size: 15; weight: 2; color: root.s.fg; anchors.verticalCenter: parent.verticalCenter }
        Text {
            textFormat: Text.PlainText
            visible: !root.iconOnly
            text: root.label !== "" ? root.label : root.s.text
            color: root.s.fg
            font.family: Theme.ui
            font.pixelSize: Theme.fsSmall
            font.weight: root.s.strong ? Font.Bold : Font.Medium
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
