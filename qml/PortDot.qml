import QtQuick
import "."

// One instrument per (skill, agent): is this skill visible to that agent, and how.
// Healthy = a plain lit disc. Only states that need a look get a ring: missing (cross), copy (diamond), foreign (dash), absent (grey dash).
Item {
    id: root
    property string link: "link"      // link | shared | missing | copy | foreign | absent
    property string agentLabel: ""
    readonly property bool ok: link === "link" || link === "shared"
    readonly property var words: ({ "link": "linked", "shared": "reads the shared folder", "missing": "missing, not linked",
                                    "copy": "separate copy, not the shared one", "foreign": "link points elsewhere", "absent": "agent not detected" })
    width: 22
    height: 22
    Accessible.role: Accessible.StaticText
    Accessible.name: agentLabel + ": " + (words[link] || link)

    Rectangle {   // lit disc
        visible: root.ok
        anchors.centerIn: parent
        width: 10; height: 10; radius: 5
        color: Theme.mark
    }
    Rectangle {   // ring, only where attention is needed
        visible: !root.ok
        anchors.centerIn: parent
        width: 18; height: 18; radius: 9
        color: "transparent"
        border.width: 1.5
        border.color: root.link === "missing" ? Theme.danger : root.link === "absent" ? Theme.lineStrong : Theme.notify
    }
    Icon { visible: root.link === "missing"; anchors.centerIn: parent; name: "x"; size: 10; weight: 2.4; color: Theme.danger }
    Rectangle {   // copy: diamond
        visible: root.link === "copy"
        anchors.centerIn: parent
        width: 7; height: 7; rotation: 45
        color: Theme.notify
    }
    Rectangle {   // foreign / absent: dash
        visible: root.link === "foreign" || root.link === "absent"
        anchors.centerIn: parent
        width: 8; height: 2; radius: 1
        color: root.link === "absent" ? Theme.lineStrong : Theme.notify
    }
}
